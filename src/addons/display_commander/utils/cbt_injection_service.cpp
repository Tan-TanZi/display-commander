// Source Code <Display Commander> // follow this order for includes in all files + add this comment at the top
#include "cbt_injection_service.hpp"

#include "general_utils.hpp"
#include "srwlock_wrapper.hpp"

// Libraries <standard C++>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// Libraries <Windows.h>
#include <Windows.h>

namespace display_commander::cbt_service {

namespace {

#if defined(_WIN64)
constexpr wchar_t kServicePidLeaf[] = L"service_64.pid";
#else
constexpr wchar_t kServicePidLeaf[] = L"service_32.pid";
#endif

constexpr wchar_t kInjectionListRelative[] = L"injection_list.txt";
constexpr wchar_t kInjectionSuccessRelative[] = L"injection_list_success.txt";

std::atomic<void*> g_cbt_installed_hook{nullptr};
std::atomic<void*> g_cbt_loaded_hook_duplicate{nullptr};

constexpr uint64_t kPrefixReloadMs = 2000;

SRWLOCK g_prefix_reload_lock = SRWLOCK_INIT;
std::vector<std::wstring> g_cached_prefix_lowercase;
FILETIME g_cached_list_mtime{};
bool g_cached_list_has_mtime = false;
uint64_t g_last_reload_tick = 0;

std::wstring ServicePidAbsolutePathWritable() {
    std::filesystem::path folder = GetDisplayCommanderAppDataFolder();
    if (folder.empty()) return {};
    return (folder / kServicePidLeaf).wstring();
}

std::wstring ServicePidAbsolutePathNoCreate() {
    std::filesystem::path folder = GetDisplayCommanderAppDataRootPathNoCreate();
    if (folder.empty()) return {};
    return (folder / kServicePidLeaf).wstring();
}

static std::string TrimAsciiWhitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (static_cast<unsigned char>(s[start]) <= 32)) ++start;
    size_t end = s.size();
    while (end > start && (static_cast<unsigned char>(s[end - 1]) <= 32)) --end;
    return s.substr(start, end - start);
}

static bool TryUtf8ToWideBytes(const std::string& utf8_bytes, std::wstring* out_wide) {
    if (!out_wide) return false;
    out_wide->clear();
    const int cw = MultiByteToWideChar(CP_UTF8, 0, utf8_bytes.data(), static_cast<int>(utf8_bytes.size()), nullptr, 0);
    if (cw <= 0) return false;
    out_wide->resize(static_cast<size_t>(cw));
    return MultiByteToWideChar(CP_UTF8, 0, utf8_bytes.data(), static_cast<int>(utf8_bytes.size()),
                               out_wide->data(), cw) == cw;
}

static void WCStringAsciiLower_inplace(std::wstring* s) {
    if (!s) return;
    for (wchar_t& c : *s) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c + 32);
    }
}

[[nodiscard]] static bool TryReadWholeFileAscii(const std::wstring& path_wide, std::string* body) {
    if (!body) return false;
    body->clear();
    HANDLE fh = CreateFileW(path_wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(fh, nullptr);
    constexpr DWORD kCap = 1u << 20;
    if (size == INVALID_FILE_SIZE || size > kCap) {
        CloseHandle(fh);
        return false;
    }
    if (size == 0) {
        CloseHandle(fh);
        return true;
    }
    body->resize(size);
    DWORD read_actual = 0;
    BOOL ok_read = ReadFile(fh, body->data(), size, &read_actual, nullptr);
    CloseHandle(fh);
    if (!ok_read || read_actual != size) return false;
    return true;
}

static bool TryGetInjectionListTimes(const std::wstring& wide_path, FILETIME* out_ft) {
    if (!out_ft) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(wide_path.c_str(), GetFileExInfoStandard, &fad)) return false;
    *out_ft = fad.ftLastWriteTime;
    return true;
}

[[nodiscard]] static bool TryParsePidAscii(const std::string& ascii, DWORD* out_pid) {
    if (!out_pid || ascii.empty()) return false;
    char* end_parse = nullptr;
    const unsigned long parsed = std::strtoul(ascii.c_str(), &end_parse, 10);
    if (end_parse == ascii.c_str() || parsed == 0 || parsed != static_cast<unsigned long>(static_cast<DWORD>(parsed)))
        return false;
    for (; *end_parse != '\0'; ++end_parse) {
        if (!std::isspace(static_cast<unsigned char>(*end_parse)))
            return false;
    }
    *out_pid = static_cast<DWORD>(parsed);
    return true;
}

static bool TryReadPidFromPath(const std::wstring& wide_path, DWORD* out_pid) {
    std::string body;
    if (!TryReadWholeFileAscii(wide_path, &body)) return false;
    std::string trimmed = TrimAsciiWhitespace(body);
    return TryParsePidAscii(trimmed, out_pid);
}

static bool TryWritePidOverwrite(const DWORD pid) {
    const std::wstring path = ServicePidAbsolutePathWritable();
    if (path.empty()) return false;
    char ascii[32]{};
#if defined(_MSC_VER)
    snprintf(ascii, sizeof(ascii), "%lu", pid);
#else
    std::snprintf(ascii, sizeof(ascii), "%lu", static_cast<unsigned long>(pid));
#endif

    HANDLE fh =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD ignored = 0;
    BOOL ok_write = WriteFile(fh, ascii, static_cast<DWORD>(std::strlen(ascii)), &ignored, nullptr);
    FlushFileBuffers(fh);
    CloseHandle(fh);
    return ok_write == TRUE && ignored > 0;
}

static bool TryDeletePidBestEffort() {
    std::wstring path = ServicePidAbsolutePathWritable();
    if (path.empty())
        path = ServicePidAbsolutePathNoCreate();
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

static bool IsProcessAliveLimited(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return false;
    HANDLE h_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (h_proc == nullptr) return false;
    DWORD exit_code = STILL_ACTIVE;
    const BOOL ec_ok = GetExitCodeProcess(h_proc, &exit_code);
    CloseHandle(h_proc);
    if (!ec_ok) return false;
    return exit_code == STILL_ACTIVE;
}

static bool IsPrefixBoundaryMatchIc(const wchar_t* abs_path, const wchar_t* prefix_lc, size_t prefix_len) {
    if (!abs_path || !prefix_lc || prefix_len == 0) return false;
    for (size_t i = 0; i < prefix_len; ++i) {
        const wchar_t ca = abs_path[i];
        const wchar_t cb = prefix_lc[i];
        if ((ca >= L'A' && ca <= L'Z' ? ca + 32 : ca) != cb) return false;
    }
    const wchar_t next_char = abs_path[prefix_len];
    return next_char == L'\0' || next_char == L'\\';
}

static void ReloadInjectionPrefixesCached() {
    const std::wstring list_path_wide =
        (GetDisplayCommanderAppDataRootPathNoCreate() / kInjectionListRelative).wstring();

    utils::SRWLockExclusive lk(g_prefix_reload_lock);

    FILETIME mt{};
    if (!TryGetInjectionListTimes(list_path_wide, &mt)) {
        g_cached_prefix_lowercase.clear();
        g_cached_list_has_mtime = false;
        g_last_reload_tick = GetTickCount64();
        return;
    }

    if (g_cached_list_has_mtime &&
        CompareFileTime(&g_cached_list_mtime, &mt) == 0 &&
        GetTickCount64() - g_last_reload_tick < kPrefixReloadMs) {
        return;
    }

    g_cached_list_has_mtime = true;
    g_cached_list_mtime = mt;
    g_last_reload_tick = GetTickCount64();

    std::vector<std::wstring> next{};
    std::string body_utf8;
    if (!TryReadWholeFileAscii(list_path_wide, &body_utf8)) {
        g_cached_prefix_lowercase = std::move(next);
        return;
    }

    std::istringstream stream(body_utf8);
    std::string line{};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string trimmed = TrimAsciiWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        std::wstring wide{};
        if (!TryUtf8ToWideBytes(trimmed, &wide))
            continue;

        while (!wide.empty() && (wide.back() == L'/' || wide.back() == L'\\')) wide.pop_back();

        if (wide.empty())
            continue;

        WCStringAsciiLower_inplace(&wide);

        wchar_t canon[MAX_PATH * 4]{};
        const DWORD got = ExpandEnvironmentStringsW(wide.c_str(), canon, MAX_PATH * 4);
        if (got == 0 || got > MAX_PATH * 4)
            continue;

        std::wstring expanded_lowercase{canon};
        WCStringAsciiLower_inplace(&expanded_lowercase);

        next.push_back(expanded_lowercase);
    }

    g_cached_prefix_lowercase = std::move(next);
}

static bool ExecutableMatchesPrefixes(const wchar_t* exe_lc) {
    if (!exe_lc || exe_lc[0] == L'\0') return false;
    ReloadInjectionPrefixesCached();
    utils::SRWLockShared lk(g_prefix_reload_lock);
    for (const std::wstring& pref : g_cached_prefix_lowercase) {
        if (IsPrefixBoundaryMatchIc(exe_lc, pref.c_str(), pref.size())) return true;
    }
    return false;
}

[[nodiscard]] static bool MatchesInjectionWhitelistCurrentProcessExe() {
    WCHAR exe_caps[MAX_PATH * 4]{};
    if (GetModuleFileNameW(nullptr, exe_caps, MAX_PATH * 4) <= 1) return false;
    std::wstring exe_lc{exe_caps};
    WCStringAsciiLower_inplace(&exe_lc);
    return ExecutableMatchesPrefixes(exe_lc.c_str());
}

static bool AppendInjectionSuccessUtf8TabLine(const wchar_t* exe_path_w, DWORD pid_this) {
    const std::filesystem::path dc = GetDisplayCommanderAppDataFolder();
    if (dc.empty())
        return false;

    SYSTEMTIME lt{};
    GetLocalTime(&lt);

    char exe_utf8[MAX_PATH * 4]{};
    const int narrowed_ex =
        WideCharToMultiByte(CP_UTF8, 0, exe_path_w, -1, exe_utf8, static_cast<int>(sizeof(exe_utf8)), nullptr, nullptr);
    if (narrowed_ex <= 0)
        return false;

    char line[MAX_PATH * 5]{};
    const int line_len =
        snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u\t%lu\t%s\r\n", static_cast<unsigned>(lt.wYear),
                 static_cast<unsigned>(lt.wMonth), static_cast<unsigned>(lt.wDay), static_cast<unsigned>(lt.wHour),
                 static_cast<unsigned>(lt.wMinute), static_cast<unsigned>(lt.wSecond),
                 static_cast<unsigned long>(pid_this), exe_utf8);
    if (line_len <= 0 || static_cast<size_t>(line_len) >= sizeof(line)) return false;

    const std::wstring path_wide = (dc / kInjectionSuccessRelative).wstring();
    HANDLE fh = CreateFileW(path_wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD ignored = 0;
    const BOOL wrote = WriteFile(fh, line, static_cast<DWORD>(line_len), &ignored, nullptr);
    FlushFileBuffers(fh);
    CloseHandle(fh);
    return wrote == TRUE && ignored == static_cast<DWORD>(line_len);
}

extern "C" LRESULT CALLBACK CBTProc(_In_ int nCode, _In_ WPARAM wParam, _In_ LPARAM lParam) {
    if (nCode == HCBT_CREATEWND || nCode == HCBT_ACTIVATE) {
        WCHAR exe_caps[MAX_PATH * 4]{};
        if (GetModuleFileNameW(nullptr, exe_caps, MAX_PATH * 4) > 1) {
            std::wstring exe_lc{exe_caps};
            WCStringAsciiLower_inplace(&exe_lc);
            if (ExecutableMatchesPrefixes(exe_lc.c_str())) {
                static volatile LONG s_reported_once = 0;
                if (InterlockedCompareExchange(&s_reported_once, 1, 0) == 0) {
                    const DWORD pid = GetCurrentProcessId();
                    (void)AppendInjectionSuccessUtf8TabLine(exe_caps, pid);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static void CbtLogWin32FailA(const char* context, DWORD gle) {
    char sys_msg[448]{};
    DWORD n =
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, gle,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), sys_msg, sizeof(sys_msg), nullptr);
    while (n > 0 && (sys_msg[n - 1] == '\n' || sys_msg[n - 1] == '\r')) {
        sys_msg[--n] = '\0';
    }
    char buf[928]{};
    if (sys_msg[0] != '\0') {
#if defined(_MSC_VER)
        snprintf(buf, sizeof(buf), "[CBT_service] %s failed: Win32 error %lu - %s\r\n", context,
                 static_cast<unsigned long>(gle), sys_msg);
#else
        std::snprintf(buf, sizeof(buf), "[CBT_service] %s failed: Win32 error %lu - %s\r\n", context,
                      static_cast<unsigned long>(gle), sys_msg);
#endif
    } else {
#if defined(_MSC_VER)
        snprintf(buf, sizeof(buf), "[CBT_service] %s failed: Win32 error %lu\r\n", context,
                 static_cast<unsigned long>(gle));
#else
        std::snprintf(buf, sizeof(buf), "[CBT_service] %s failed: Win32 error %lu\r\n", context,
                      static_cast<unsigned long>(gle));
#endif
    }
    OutputDebugStringA(buf);
}

[[nodiscard]] static bool TryGetModulePathFromExportAddress(const void* export_addr, std::wstring* out_path) {
    if (!export_addr || !out_path) return false;
    HMODULE hm_self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(export_addr), &hm_self) == FALSE ||
        hm_self == nullptr) {
        return false;
    }
    WCHAR buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(hm_self, buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return false;
    *out_path = buf;
    return true;
}

static void RemoveHookCopyTreeBestEffort(const std::filesystem::path& hook_copy_dll_path) {
    std::error_code ec{};
    std::filesystem::remove(hook_copy_dll_path, ec);
    std::filesystem::remove_all(hook_copy_dll_path.parent_path(), ec);
}

// Copy the addon DLL to %TEMP%/DisplayCommander/CbtHook/<random-hex>/dcbthook.dll and LoadLibrary so the desktop hook
// does not associate the hooked module with the install path image (games keep the temp copy mapped, not the original).
[[nodiscard]] static bool TryCreateTempCbtHookCopyAndLoad(std::filesystem::path* out_copy_dll_path,
                                                         HMODULE* out_loaded_copy, FARPROC* out_cbt_proc) {
    if (!out_copy_dll_path || !out_loaded_copy || !out_cbt_proc) return false;
    out_copy_dll_path->clear();
    *out_loaded_copy = nullptr;
    *out_cbt_proc = nullptr;

    std::wstring src_path_wide;
    if (!TryGetModulePathFromExportAddress(reinterpret_cast<const void*>(&CBTProc), &src_path_wide)) return false;

    std::error_code fs_ec{};
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path(fs_ec);
    if (fs_ec || temp_root.empty()) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t salt =
            GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentProcessId()) * UINT64_C(0xD6E8FEB86327A591)) ^
            (static_cast<uint64_t>(attempt + 1) * UINT64_C(0x2545F4914F6CDD01));
        wchar_t uniq[24]{};
        if (swprintf_s(uniq, L"%016llx", static_cast<unsigned long long>(salt)) < 0) continue;

        const std::filesystem::path dir = temp_root / L"DisplayCommander" / L"CbtHook" / uniq;
        std::error_code mk_ec{};
        std::filesystem::create_directories(dir, mk_ec);
        if (mk_ec) continue;

        const std::filesystem::path dst = dir / L"dcbthook.dll";

        const BOOL copied = CopyFileW(src_path_wide.c_str(), dst.wstring().c_str(), FALSE);
        if (copied == FALSE) {
            RemoveHookCopyTreeBestEffort(dst);
            continue;
        }

        HMODULE loaded = LoadLibraryW(dst.wstring().c_str());
        if (loaded == nullptr) {
            CbtLogWin32FailA("LoadLibrary WH_CBT hook copy", GetLastError());
            RemoveHookCopyTreeBestEffort(dst);
            continue;
        }

        FARPROC proc = GetProcAddress(loaded, "CBTProc");
        if (proc == nullptr) {
            FreeLibrary(loaded);
            RemoveHookCopyTreeBestEffort(dst);
            continue;
        }

        *out_copy_dll_path = dst;
        *out_loaded_copy = loaded;
        *out_cbt_proc = proc;
        return true;
    }

    OutputDebugStringA("[CBT_service] Failed after retries to prepare temp WH_CBT hook DLL copy\r\n");
    return false;
}

}  // namespace

bool CurrentProcessExeMatchesInjectionWhitelistPrefixes() { return MatchesInjectionWhitelistCurrentProcessExe(); }

bool ShouldEnterCbtInjecteeMinimalGuest() {
    const std::wstring path = ServicePidAbsolutePathNoCreate();
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) return false;

    DWORD file_pid = 0;
    if (!TryReadPidFromPath(path, &file_pid)) return false;
    if (file_pid == GetCurrentProcessId()) return false;
    return IsProcessAliveLimited(file_pid);
}

void DllDetachCbtCleanup() {
    void* got = g_cbt_installed_hook.exchange(nullptr, std::memory_order_acq_rel);
    if (got != nullptr) {
        // Unhook removes the desktop hook install; Windows does NOT guarantee unloading this module copy from
        // every process where the hook once ran—those unrelated processes may retain a mapped addon image until exit.
        UnhookWindowsHookEx(reinterpret_cast<HHOOK>(got));
    }
    void* dup = g_cbt_loaded_hook_duplicate.exchange(nullptr, std::memory_order_acq_rel);
    if (dup != nullptr) {
        FreeLibrary(reinterpret_cast<HMODULE>(dup));
    }
}

#if !defined(DISPLAY_COMMANDER_BUILD_EXE)
extern "C" __declspec(dllexport) void CALLBACK start_service(HWND /*hwnd*/, HINSTANCE /*hinst*/, LPSTR /*lpszCmdLine*/,
                                                             int /*nCmdShow*/) {
    const DWORD owner_pid = GetCurrentProcessId();
    if (!TryWritePidOverwrite(owner_pid)) {
        OutputDebugStringA("[CBT_service] Failed to write service PID sentinel file\r\n");
        return;
    }

    std::filesystem::path copy_dll_path;
    HMODULE hmod_hook = nullptr;
    FARPROC cbt_proc = nullptr;
    if (!TryCreateTempCbtHookCopyAndLoad(&copy_dll_path, &hmod_hook, &cbt_proc)) {
        OutputDebugStringA("[CBT_service] Could not create temp hook DLL copy or resolve CBTProc export\r\n");
        (void)TryDeletePidBestEffort();
        return;
    }

    HHOOK hook = SetWindowsHookExA(WH_CBT, reinterpret_cast<HOOKPROC>(cbt_proc), hmod_hook, 0);
    if (hook == nullptr) {
        const DWORD gle = GetLastError();
        CbtLogWin32FailA("SetWindowsHookEx WH_CBT", gle);
        FreeLibrary(hmod_hook);
        RemoveHookCopyTreeBestEffort(copy_dll_path);
        (void)TryDeletePidBestEffort();
        return;
    }

    g_cbt_loaded_hook_duplicate.store(reinterpret_cast<void*>(hmod_hook), std::memory_order_release);
    g_cbt_installed_hook.store(reinterpret_cast<void*>(hook), std::memory_order_release);

    for (;;) {
        Sleep(1000);
        const std::wstring path = ServicePidAbsolutePathWritable();
        if (path.empty()) break;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) break;

        DWORD read_back = 0;
        if (!TryReadPidFromPath(path, &read_back)) break;
        if (read_back != owner_pid) break;
    }

    DllDetachCbtCleanup();
    (void)TryDeletePidBestEffort();

    ExitProcess(0);
}
#endif  // !DISPLAY_COMMANDER_BUILD_EXE

}  // namespace display_commander::cbt_service

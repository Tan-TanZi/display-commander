// Source Code <Display Commander> // follow this order for includes in all files + add this comment at the top
#pragma once

// Libraries <Windows.h>
#include <Windows.h>

namespace display_commander::cbt_service {

// Whitelist prefixes (same file as WH_CBT path): caller uses this during ProcessAttach before full/minimal branching.
[[nodiscard]] bool CurrentProcessExeMatchesInjectionWhitelistPrefixes();

[[nodiscard]] bool ShouldEnterCbtInjecteeMinimalGuest();

// Unhook if this module instance owns the hook (service host rundll only); safe before ReShade detour path.
void DllDetachCbtCleanup();

}  // namespace display_commander::cbt_service

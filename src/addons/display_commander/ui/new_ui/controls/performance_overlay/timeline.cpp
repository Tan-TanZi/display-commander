// Source Code <Display Commander> // follow this order for includes in all files + add this comment at the top
// Headers <Display Commander>
#include "performance_overlay_internal.hpp"
#include "utils.hpp"

namespace ui::new_ui {

namespace {

// Cached data for frame timeline; updated at most once per second to reduce flicker.
struct CachedTimelinePhase {
    const char* label;
    double start_ms;
    double end_ms;
    ImVec4 color;
};

static std::vector<CachedTimelinePhase> s_timeline_phases;
static bool s_nvidiaProfileChangeRestartNeeded = false;
static double s_timeline_t_min = 0.0;
static double s_timeline_t_max = 1.0;
static double s_timeline_time_range = 1.0;
static LONGLONG s_timeline_last_update_ns = 0;

}  // namespace

namespace performance_overlay {

void UpdateFrameTimelineCache() {
    CALL_GUARD_NO_TS();
    const uint64_t last_completed_frame_id = (g_global_frame_id.load() > 0) ? (g_global_frame_id.load() - 1) : 0;
    if (last_completed_frame_id == 0) {
        s_timeline_phases.clear();
        return;
    }
    const size_t slot = static_cast<size_t>(last_completed_frame_id % kFrameDataBufferSize);
    const FrameData& fd = g_frame_data[slot];
    if (fd.frame_id.load() != last_completed_frame_id || fd.sim_start_ns.load() <= 0
        || fd.present_end_time_ns.load() <= 0) {
        s_timeline_phases.clear();
        return;
    }

    const LONGLONG now_ns = utils::get_now_ns();
    const bool should_update =
        (s_timeline_phases.empty()) || (now_ns - s_timeline_last_update_ns >= static_cast<LONGLONG>(utils::SEC_TO_NS));
    if (!should_update) {
        return;
    }
    s_timeline_last_update_ns = now_ns;

    const LONGLONG base_ns = fd.sim_start_ns.load();
    const double to_ms = 1.0 / static_cast<double>(utils::NS_TO_MS);

    const double sim_start_ms = 0.0;
    const double sim_end_ms = (fd.submit_start_time_ns.load() > base_ns)
                                  ? (static_cast<double>(fd.submit_start_time_ns.load() - base_ns) * to_ms)
                                  : sim_start_ms;
    const double render_end_ms = (fd.render_submit_end_time_ns.load() > base_ns)
                                     ? (static_cast<double>(fd.render_submit_end_time_ns.load() - base_ns) * to_ms)
                                     : sim_end_ms;
    const double present_start_ms = (fd.present_start_time_ns.load() > base_ns)
                                        ? (static_cast<double>(fd.present_start_time_ns.load() - base_ns) * to_ms)
                                        : render_end_ms;
    const double present_end_ms = (fd.present_end_time_ns.load() > base_ns)
                                      ? (static_cast<double>(fd.present_end_time_ns.load() - base_ns) * to_ms)
                                      : present_start_ms;
    const double sleep_pre_start_ms =
        (fd.sleep_pre_present_start_time_ns.load() > base_ns)
            ? (static_cast<double>(fd.sleep_pre_present_start_time_ns.load() - base_ns) * to_ms)
            : render_end_ms;
    const double sleep_pre_end_ms =
        (fd.sleep_pre_present_end_time_ns.load() > base_ns)
            ? (static_cast<double>(fd.sleep_pre_present_end_time_ns.load() - base_ns) * to_ms)
            : present_start_ms;
    const double sleep_post_start_ms =
        (fd.sleep_post_present_start_time_ns.load() > base_ns)
            ? (static_cast<double>(fd.sleep_post_present_start_time_ns.load() - base_ns) * to_ms)
            : present_end_ms;
    const double sleep_post_end_ms =
        (fd.sleep_post_present_end_time_ns.load() > base_ns)
            ? (static_cast<double>(fd.sleep_post_present_end_time_ns.load() - base_ns) * to_ms)
            : present_end_ms;
    const bool has_gpu =
        (settings::g_mainTabSettings.gpu_measurement_enabled.GetValue() != 0 && fd.gpu_completion_time_ns.load() > 0);
    const double gpu_end_ms = has_gpu && fd.gpu_completion_time_ns.load() > base_ns
                                  ? (static_cast<double>(fd.gpu_completion_time_ns.load() - base_ns) * to_ms)
                                  : present_end_ms;

    const ImVec4 col_sim(0.2f, 0.75f, 0.35f, 1.0f);
    const ImVec4 col_render(0.35f, 0.55f, 1.0f, 1.0f);
    const ImVec4 col_reshade(0.75f, 0.4f, 1.0f, 1.0f);
    const ImVec4 col_sleep(0.5f, 0.5f, 0.55f, 1.0f);
    const ImVec4 col_present(1.0f, 0.55f, 0.2f, 1.0f);
    const ImVec4 col_gpu(0.95f, 0.35f, 0.35f, 1.0f);

    s_timeline_phases.clear();
    if (sim_end_ms > sim_start_ms) {
        s_timeline_phases.push_back({"模拟", sim_start_ms, sim_end_ms, col_sim});
    }
    if (render_end_ms > sim_end_ms) {
        s_timeline_phases.push_back({"渲染提交", sim_end_ms, render_end_ms, col_render});
    }
    const double reshade_end_ms =
        (fd.sleep_pre_present_start_time_ns.load() > 0) ? sleep_pre_start_ms : present_start_ms;
    if (reshade_end_ms > render_end_ms) {
        s_timeline_phases.push_back({"ReShade", render_end_ms, reshade_end_ms, col_reshade});
    }
    if (sleep_pre_end_ms > sleep_pre_start_ms) {
        s_timeline_phases.push_back({"FPS 睡眠（之前）", sleep_pre_start_ms, sleep_pre_end_ms, col_sleep});
    }
    if (present_end_ms > present_start_ms) {
        s_timeline_phases.push_back({"Present", present_start_ms, present_end_ms, col_present});
    }
    if (sleep_post_end_ms > sleep_post_start_ms) {
        s_timeline_phases.push_back({"FPS（每秒帧数）休眠（之后）", sleep_post_start_ms, sleep_post_end_ms, col_sleep});
    }
    if (has_gpu && gpu_end_ms > present_start_ms) {
        s_timeline_phases.push_back({"GPU", present_start_ms, gpu_end_ms, col_gpu});
    }

    const double frame_ms = (sleep_post_end_ms > present_end_ms) ? sleep_post_end_ms : present_end_ms;
    s_timeline_t_min = 0.0;
    s_timeline_t_max = frame_ms;
    for (const auto& p : s_timeline_phases) {
        if (p.end_ms > s_timeline_t_max) {
            s_timeline_t_max = p.end_ms;
        }
    }
    if (s_timeline_t_max <= s_timeline_t_min) {
        s_timeline_t_max = s_timeline_t_min + 1.0;
    }
    s_timeline_time_range = s_timeline_t_max - s_timeline_t_min;
}

}  // namespace performance_overlay

void DrawFrameTimelineBar(display_commander::ui::IImGuiWrapper& imgui) {
    CALL_GUARD_NO_TS();
    if (IsNativeReflexActive()) {
        imgui.TextColored(ui::colors::TEXT_DIMMED, "Frame timeline: not implemented yet for Reflex path.");
        return;
    }
    performance_overlay::UpdateFrameTimelineCache();
    if (s_timeline_phases.empty()) {
        imgui.TextColored(ui::colors::TEXT_DIMMED, "Frame timeline: no frame time data yet.");
        return;
    }

    const std::vector<CachedTimelinePhase>& phases = s_timeline_phases;
    const double t_min = s_timeline_t_min;
    const double t_max = s_timeline_t_max;
    const double time_range = s_timeline_time_range;

    imgui.Text("Frame timeline (start to end, relative to sim start, updates every 1 s)");
    if (imgui.IsItemHovered()) {
        imgui.SetTooltipEx(
            "Each row = one phase. Bar shows when it started and ended (0 = sim start). "
            "Times from last completed frame (g_frame_data).");
    }
    imgui.Spacing();

    const float row_height = 18.0f;
    const float bar_rounding = 2.0f;
    const float label_width = 150.0f;

    if (!imgui.BeginTable("##FrameTimeline", 2, ImGuiTableFlags_None, ImVec2(-1.0f, 0.0f))) {
        return;
    }
    imgui.TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, label_width);
    imgui.TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthStretch);

    auto* draw_list = imgui.GetWindowDrawList();
    if (draw_list == nullptr) {
        imgui.EndTable();
        return;
    }

    for (const auto& p : phases) {
        const double duration = p.end_ms - p.start_ms;
        if (duration <= 0.0) {
            continue;
        }

        imgui.TableNextColumn();
        imgui.TextUnformatted(p.label);

        imgui.TableNextColumn();
        const ImVec2 bar_pos = imgui.GetCursorScreenPos();
        const float bar_width = imgui.GetContentRegionAvail().x;
        const ImVec2 bar_size(bar_width, row_height);

        const double frac_start = (p.start_ms - t_min) / time_range;
        const double frac_end = (p.end_ms - t_min) / time_range;
        float x0 = bar_pos.x + static_cast<float>(frac_start * static_cast<double>(bar_width));
        float x1 = bar_pos.x + static_cast<float>(frac_end * static_cast<double>(bar_width));
        if (x1 - x0 < 1.0f) {
            x1 = x0 + 1.0f;
        }
        if (x1 > bar_pos.x + bar_width) {
            x1 = bar_pos.x + bar_width;
        }
        if (x0 < bar_pos.x) {
            x0 = bar_pos.x;
        }

        draw_list->AddRectFilled(ImVec2(bar_pos.x, bar_pos.y), ImVec2(bar_pos.x + bar_width, bar_pos.y + bar_size.y),
                                 imgui.GetColorU32(ImGuiCol_FrameBg), bar_rounding);
        draw_list->AddRectFilled(ImVec2(x0, bar_pos.y), ImVec2(x1, bar_pos.y + bar_size.y),
                                 imgui.ColorConvertFloat4ToU32(p.color), bar_rounding);

        imgui.Dummy(bar_size);

        if (imgui.IsItemHovered()) {
            imgui.SetTooltipEx("%s: %.2f ms - %.2f ms (%.2f ms)", p.label, p.start_ms, p.end_ms, duration);
        }
    }

    imgui.TableNextColumn();
    imgui.TextUnformatted("");
    imgui.TableNextColumn();
    const float axis_bar_width = imgui.GetContentRegionAvail().x;
    const float axis_cell_x = imgui.GetCursorPosX();
    imgui.TextColored(ui::colors::TEXT_DIMMED, "0 ms");
    imgui.SameLine(axis_cell_x + axis_bar_width - 50.0f);
    imgui.TextColored(ui::colors::TEXT_DIMMED, "%.1f ms", t_max);

    imgui.EndTable();
}

void DrawFrameTimelineBarOverlay(display_commander::ui::IImGuiWrapper& imgui, bool show_tooltips) {
    CALL_GUARD_NO_TS();
    performance_overlay::UpdateFrameTimelineCache();
    if (s_timeline_phases.empty()) {
        return;
    }
    const std::vector<CachedTimelinePhase>& phases = s_timeline_phases;
    const double t_min = s_timeline_t_min;
    const double t_max = s_timeline_t_max;
    const double time_range = s_timeline_time_range;

    const float row_height = 10.0f;
    const float bar_rounding = 1.0f;
    const float label_width = 88.0f;
    const float graph_scale = settings::g_mainTabSettings.overlay_graph_scale.GetValue();
    const float total_width = 280.0f * graph_scale;

    if (!imgui.BeginTable("##FrameTimelineOverlay", 2, ImGuiTableFlags_None, ImVec2(total_width, 0.0f))) {
        return;
    }
    imgui.TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, label_width);
    imgui.TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthStretch);
    auto* draw_list = imgui.GetWindowDrawList();
    if (draw_list == nullptr) {
        imgui.EndTable();
        return;
    }

    for (const auto& p : phases) {
        const double duration = p.end_ms - p.start_ms;
        if (duration <= 0.0) {
            continue;
        }

        imgui.TableNextColumn();
        imgui.TextUnformatted(p.label);
        imgui.TableNextColumn();
        const ImVec2 bar_pos = imgui.GetCursorScreenPos();
        const float bar_width = imgui.GetContentRegionAvail().x;
        const ImVec2 bar_size(bar_width, row_height);

        const double frac_start = (p.start_ms - t_min) / time_range;
        const double frac_end = (p.end_ms - t_min) / time_range;
        float x0 = bar_pos.x + static_cast<float>(frac_start * static_cast<double>(bar_width));
        float x1 = bar_pos.x + static_cast<float>(frac_end * static_cast<double>(bar_width));
        if (x1 - x0 < 1.0f) {
            x1 = x0 + 1.0f;
        }
        if (x1 > bar_pos.x + bar_width) {
            x1 = bar_pos.x + bar_width;
        }
        if (x0 < bar_pos.x) {
            x0 = bar_pos.x;
        }
        draw_list->AddRectFilled(ImVec2(bar_pos.x, bar_pos.y), ImVec2(bar_pos.x + bar_width, bar_pos.y + bar_size.y),
                                 imgui.GetColorU32(ImGuiCol_FrameBg), bar_rounding);
        draw_list->AddRectFilled(ImVec2(x0, bar_pos.y), ImVec2(x1, bar_pos.y + bar_size.y),
                                 imgui.ColorConvertFloat4ToU32(p.color), bar_rounding);
        imgui.Dummy(bar_size);
        if (show_tooltips && imgui.IsItemHovered()) {
            imgui.SetTooltipEx("%s: %.2f - %.2f ms", p.label, p.start_ms, p.end_ms);
        }
    }
    imgui.TableNextColumn();
    imgui.TextUnformatted("");
    imgui.TableNextColumn();
    const float axis_bar_width = imgui.GetContentRegionAvail().x;
    const float axis_cell_x = imgui.GetCursorPosX();
    imgui.TextColored(ui::colors::TEXT_DIMMED, "0");
    imgui.SameLine(axis_cell_x + axis_bar_width - 28.0f);
    imgui.TextColored(ui::colors::TEXT_DIMMED, "%.0f ms", t_max);
    imgui.EndTable();
}

}  // namespace ui::new_ui

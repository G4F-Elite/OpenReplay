#pragma once

#include <windows.h>

namespace openreplay::ui {

struct MonitorArea {
    HMONITOR monitor{nullptr};
    RECT bounds{};
    bool fullscreen{false};
    bool taskbar_visible{false};
};

[[nodiscard]] MonitorArea ForegroundMonitorArea(HWND fallback_window = nullptr) noexcept;
[[nodiscard]] UINT WindowDpi(HWND window) noexcept;
[[nodiscard]] UINT MonitorDpi(HMONITOR monitor) noexcept;
[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept;

}  // namespace openreplay::ui

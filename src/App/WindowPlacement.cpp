#include "pch.h"

#include "WindowPlacement.h"

#include <array>
#include <cwchar>

namespace openreplay::ui {
namespace {

bool CoversMonitor(HWND window, const RECT& monitor_bounds) noexcept {
    if (!window || window == GetShellWindow() || !IsWindowVisible(window) || IsIconic(window)) return false;
    const auto root = GetAncestor(window, GA_ROOT);
    RECT bounds{};
    if (!GetWindowRect(root ? root : window, &bounds)) return false;

    constexpr LONG tolerance = 3;
    return bounds.left <= monitor_bounds.left + tolerance &&
           bounds.top <= monitor_bounds.top + tolerance &&
           bounds.right >= monitor_bounds.right - tolerance &&
           bounds.bottom >= monitor_bounds.bottom - tolerance;
}

struct TaskbarSearch {
    HMONITOR monitor{};
    RECT monitor_bounds{};
    RECT visible_bounds{};
    bool visible{false};
};

BOOL CALLBACK FindVisibleTaskbar(HWND window, LPARAM context) {
    auto* search = reinterpret_cast<TaskbarSearch*>(context);
    if (!IsWindowVisible(window) || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != search->monitor) return TRUE;

    wchar_t class_name[64]{};
    if (!GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)))) return TRUE;
    if (std::wcscmp(class_name, L"Shell_TrayWnd") != 0 &&
        std::wcscmp(class_name, L"Shell_SecondaryTrayWnd") != 0) {
        return TRUE;
    }

    RECT taskbar{};
    if (!GetWindowRect(window, &taskbar)) return TRUE;
    RECT intersection{};
    if (!IntersectRect(&intersection, &taskbar, &search->monitor_bounds)) return TRUE;

    const auto width = intersection.right - intersection.left;
    const auto height = intersection.bottom - intersection.top;
    const auto monitor_width = search->monitor_bounds.right - search->monitor_bounds.left;
    const auto monitor_height = search->monitor_bounds.bottom - search->monitor_bounds.top;
    const bool horizontal = width >= monitor_width / 2 && height > 4;
    const bool vertical = height >= monitor_height / 2 && width > 4;
    search->visible = horizontal || vertical;
    if (search->visible) search->visible_bounds = intersection;
    return search->visible ? FALSE : TRUE;
}

RECT AvailableBounds(const RECT& monitor_bounds, const TaskbarSearch& taskbar) noexcept {
    if (!taskbar.visible) return monitor_bounds;
    auto result = monitor_bounds;
    const auto width = taskbar.visible_bounds.right - taskbar.visible_bounds.left;
    const auto height = taskbar.visible_bounds.bottom - taskbar.visible_bounds.top;
    if (width >= height) {
        if (taskbar.visible_bounds.top <= monitor_bounds.top + 2) result.top = taskbar.visible_bounds.bottom;
        else result.bottom = taskbar.visible_bounds.top;
    } else {
        if (taskbar.visible_bounds.left <= monitor_bounds.left + 2) result.left = taskbar.visible_bounds.right;
        else result.right = taskbar.visible_bounds.left;
    }
    return result;
}

}  // namespace

MonitorArea ForegroundMonitorArea(HWND fallback_window) noexcept {
    auto foreground = GetForegroundWindow();
    if (!foreground) foreground = fallback_window;
    auto monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return {};

    MonitorArea result;
    result.monitor = monitor;
    result.fullscreen = CoversMonitor(foreground, info.rcMonitor);
    TaskbarSearch taskbar{monitor, info.rcMonitor};
    EnumWindows(FindVisibleTaskbar, reinterpret_cast<LPARAM>(&taskbar));
    result.taskbar_visible = taskbar.visible;
    result.bounds = result.fullscreen ? info.rcMonitor : AvailableBounds(info.rcMonitor, taskbar);
    return result;
}

UINT WindowDpi(HWND window) noexcept {
    const auto dpi = window ? GetDpiForWindow(window) : 0U;
    return dpi ? dpi : 96U;
}

UINT MonitorDpi(HMONITOR monitor) noexcept {
    using GetDpiForMonitorFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    struct ShcoreApi {
        HMODULE module{LoadLibraryW(L"Shcore.dll")};
        GetDpiForMonitorFunction get_dpi_for_monitor{module
            ? reinterpret_cast<GetDpiForMonitorFunction>(GetProcAddress(module, "GetDpiForMonitor"))
            : nullptr};
        ~ShcoreApi() {
            if (module) FreeLibrary(module);
        }
    };
    static const ShcoreApi api;
    UINT horizontal = 96;
    UINT vertical = 96;
    if (monitor && api.get_dpi_for_monitor &&
        SUCCEEDED(api.get_dpi_for_monitor(monitor, 0, &horizontal, &vertical)) &&
        horizontal) {
        return horizontal;
    }
    return 96;
}

int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96U), 96);
}

}  // namespace openreplay::ui

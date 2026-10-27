#include "FilterPreview.h"

#include <future>

namespace openreplay::host {

bool FilterPreview::Start(ObsApi& api, obs_source_t* source, const RECT& area, std::string& error) {
    if (thread_.joinable()) return true;
    api_ = &api;
    source_ = source;
    area_ = area;
    bypass_ = false;
    std::promise<std::string> started;
    auto result = started.get_future();
    thread_ = std::thread([this, started = std::move(started)]() mutable {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"OpenReplay.FilterPreview";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            started.set_value("Unable to register the game filter window");
            return;
        }
        const auto window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            type.lpszClassName, L"OpenReplay Game Filters", WS_POPUP,
            area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top,
            nullptr, nullptr, type.hInstance, this);
        // Exclusion must succeed before this window can ever be shown. WGC then
        // captures the original desktop underneath it, avoiding recursive capture.
        if (!window || !SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) ||
            !SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) {
            if (window) DestroyWindow(window);
            started.set_value("Unable to exclude the game filter window from capture");
            return;
        }
        ObsDisplayInfo info{};
        info.window = window;
        info.width = static_cast<std::uint32_t>(area_.right - area_.left);
        info.height = static_cast<std::uint32_t>(area_.bottom - area_.top);
        info.backbuffers = 2;
        info.format = 5; // GS_BGRA
        info.depth_format = 0; // GS_ZS_NONE
        auto* display = api_->obs_display_create(&info, 0);
        if (!display || !RegisterHotKey(window, 1, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F3)) {
            if (display) api_->obs_display_destroy(display);
            DestroyWindow(window);
            started.set_value("Unable to start game filters or reserve Alt+Shift+F3 for preview bypass");
            return;
        }
        api_->obs_display_add_draw_callback(display, Draw, this);
        SetTimer(window, 1, 100, nullptr);
        window_ = window;
        // Decide visibility before the first display callback. This avoids
        // rendering a hidden full-frame overlay during startup.
        Refresh(window);
        started.set_value({});
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        window_ = nullptr;
        visible_.store(false, std::memory_order_release);
        ShowWindow(window, SW_HIDE);
        KillTimer(window, 1);
        UnregisterHotKey(window, 1);
        api_->obs_display_remove_draw_callback(display, Draw, this);
        api_->obs_display_destroy(display);
        DestroyWindow(window);
    });
    error = result.get();
    if (!error.empty()) {
        thread_.join();
        return false;
    }
    return true;
}

void FilterPreview::Stop() noexcept {
    visible_.store(false, std::memory_order_release);
    if (const auto window = window_.load()) PostMessageW(window, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void FilterPreview::Draw(void* context, std::uint32_t, std::uint32_t) {
    auto& self = *static_cast<FilterPreview*>(context);
    if (!self.visible_.load(std::memory_order_acquire)) return;
    if (!self.api_->obs_source_get_width(self.source_)) return;
    self.api_->gs_ortho(0.0F, static_cast<float>((self.area_.right - self.area_.left) & ~1),
                       0.0F, static_cast<float>((self.area_.bottom - self.area_.top) & ~1), -100.0F, 100.0F);
    self.api_->obs_render_main_texture();
}

void FilterPreview::Refresh(HWND window) {
    const auto foreground = GetForegroundWindow();
    RECT client{};
    POINT origin{};
    // Only cover a borderless foreground application on this monitor. Alt+Tab,
    // the desktop, and the settings panel immediately reveal the original screen.
    const bool fullscreen = foreground && foreground != GetShellWindow() &&
        foreground != GetDesktopWindow() && GetClientRect(foreground, &client) &&
        ClientToScreen(foreground, &origin) && origin.x == area_.left && origin.y == area_.top &&
        client.right == area_.right - area_.left && client.bottom == area_.bottom - area_.top;
    const bool show = fullscreen && !bypass_;
    visible_.store(show, std::memory_order_release);
    if (show) {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (IsWindowVisible(window)) {
        ShowWindow(window, SW_HIDE);
    }
}

LRESULT CALLBACK FilterPreview::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FilterPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<FilterPreview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        switch (message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_HOTKEY: self->bypass_ = !self->bypass_; self->Refresh(window); return 0;
        case WM_TIMER: self->Refresh(window); return 0;
        case WM_DISPLAYCHANGE: self->bypass_ = true; self->visible_.store(false, std::memory_order_release);
            ShowWindow(window, SW_HIDE); return 0;
        case WM_CLOSE: self->visible_.store(false, std::memory_order_release); PostQuitMessage(0); return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace openreplay::host

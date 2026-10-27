#pragma once

#include "ObsApi.h"

#include <atomic>
#include <thread>

namespace openreplay::host {

// Displays the already-filtered OBS canvas above a borderless foreground window.
class FilterPreview {
public:
    ~FilterPreview() { Stop(); }
    bool Start(ObsApi& api, obs_source_t* source, const RECT& area, std::string& error);
    void Stop() noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static void Draw(void* context, std::uint32_t width, std::uint32_t height);
    void Refresh(HWND window);

    ObsApi* api_{};
    obs_source_t* source_{};
    RECT area_{};
    std::thread thread_;
    std::atomic<HWND> window_{};
    std::atomic_bool visible_{false};
    bool bypass_{false}; // Accessed only by the window thread.
};

} // namespace openreplay::host

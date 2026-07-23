#include "pch.h"

#include "PerformanceOverlay.h"
#include "WindowPlacement.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace openreplay::ui {
namespace {

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;

std::uint64_t FileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

std::wstring MemoryText(std::uint64_t used, std::uint64_t total, bool english) {
    constexpr double gibibyte = 1024.0 * 1024.0 * 1024.0;
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f / %.1f %s", static_cast<double>(used) / gibibyte,
               static_cast<double>(total) / gibibyte, english ? L"GB" : L"ГБ");
    return text;
}

}  // namespace

PerformanceOverlay::~PerformanceOverlay() {
    Shutdown();
}

bool PerformanceOverlay::Initialize(HWND owner) {
    if (window_) return true;
    owner_ = owner;

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"OpenReplay.PerformanceOverlay";
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        window_class.lpszClassName, L"OpenReplay Performance", WS_POPUP,
        0, 0, 340, 260, nullptr, nullptr, window_class.hInstance, this);
    if (!window_) return false;
    InitializeNvml();
    return true;
}

void PerformanceOverlay::Shutdown() noexcept {
    if (window_) {
        KillTimer(window_, kRefreshTimer);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    visible_ = false;
    ShutdownNvml();
}

void PerformanceOverlay::Toggle(const Settings& settings, bool english) {
    ApplySettings(settings, english);
    if (!window_ && !Initialize(owner_)) return;
    if (visible_) Hide();
    else Show();
}

void PerformanceOverlay::ApplySettings(const Settings& settings, bool english) {
    settings_ = settings;
    settings_.Normalize();
    english_ = english;
    if (!window_) return;
    const auto alpha = static_cast<BYTE>(std::lround(settings_.performance_overlay_opacity * 255.0 / 100.0));
    SetLayeredWindowAttributes(window_, 0, alpha, LWA_ALPHA);
    if (visible_) {
        PositionWindow();
        InvalidateRect(window_, nullptr, TRUE);
    }
}

std::optional<RECT> PerformanceOverlay::Bounds() const noexcept {
    RECT bounds{};
    if (!visible_ || !window_ || !GetWindowRect(window_, &bounds)) return std::nullopt;
    return bounds;
}

bool PerformanceOverlay::InitializeNvml() {
    if (nvml_module_) return nvml_device_ != nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (next_nvml_retry_ != std::chrono::steady_clock::time_point{} && now < next_nvml_retry_) return false;
    next_nvml_retry_ = now + std::chrono::seconds{10};
    nvml_module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvml_module_) {
        std::array<wchar_t, 32768> program_files{};
        const auto length = GetEnvironmentVariableW(L"ProgramW6432", program_files.data(),
                                                     static_cast<DWORD>(program_files.size()));
        if (length && length < program_files.size()) {
            const auto path = std::filesystem::path{std::wstring{program_files.data(), length}} /
                              L"NVIDIA Corporation" / L"NVSMI" / L"nvml.dll";
            nvml_module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        }
    }
    if (!nvml_module_) return false;

    const auto resolve = [this](const char* name) { return GetProcAddress(nvml_module_, name); };
    nvml_init_ = reinterpret_cast<NvmlInit>(resolve("nvmlInit_v2"));
    nvml_shutdown_ = reinterpret_cast<NvmlShutdown>(resolve("nvmlShutdown"));
    nvml_device_get_handle_ = reinterpret_cast<NvmlDeviceGetHandleByIndex>(
        resolve("nvmlDeviceGetHandleByIndex_v2"));
    nvml_get_utilization_ = reinterpret_cast<NvmlDeviceGetUtilizationRates>(
        resolve("nvmlDeviceGetUtilizationRates"));
    nvml_get_temperature_ = reinterpret_cast<NvmlDeviceGetTemperature>(resolve("nvmlDeviceGetTemperature"));
    nvml_get_clock_ = reinterpret_cast<NvmlDeviceGetClockInfo>(resolve("nvmlDeviceGetClockInfo"));
    nvml_get_memory_ = reinterpret_cast<NvmlDeviceGetMemoryInfo>(resolve("nvmlDeviceGetMemoryInfo"));
    if (!nvml_init_ || !nvml_shutdown_ || !nvml_device_get_handle_ || !nvml_get_utilization_ ||
        !nvml_get_temperature_ || !nvml_get_clock_ || !nvml_get_memory_ || nvml_init_() != kNvmlSuccess) {
        ShutdownNvml();
        return false;
    }
    nvml_initialized_ = true;
    if (nvml_device_get_handle_(0, &nvml_device_) != kNvmlSuccess) {
        ShutdownNvml();
        return false;
    }
    next_nvml_retry_ = {};
    return true;
}

void PerformanceOverlay::ShutdownNvml() noexcept {
    if (nvml_module_) {
        if (nvml_shutdown_ && nvml_initialized_) nvml_shutdown_();
        FreeLibrary(nvml_module_);
    }
    nvml_module_ = nullptr;
    nvml_device_ = nullptr;
    nvml_initialized_ = false;
    nvml_init_ = nullptr;
    nvml_shutdown_ = nullptr;
    nvml_device_get_handle_ = nullptr;
    nvml_get_utilization_ = nullptr;
    nvml_get_temperature_ = nullptr;
    nvml_get_clock_ = nullptr;
    nvml_get_memory_ = nullptr;
}

void PerformanceOverlay::SampleMetrics() {
    sample_ = {};
    SampleNvml();
    SampleCpuAndMemory();
    if (!nvml_device_) SampleDxgiMemory();
}

void PerformanceOverlay::SampleNvml() {
    if (!nvml_device_ && !InitializeNvml()) return;

    NvmlUtilization utilization{};
    if (nvml_get_utilization_(nvml_device_, &utilization) == kNvmlSuccess) {
        sample_.gpu_percent = std::min(utilization.gpu, 100U);
    }
    unsigned int value = 0;
    if (nvml_get_temperature_(nvml_device_, kNvmlTemperatureGpu, &value) == kNvmlSuccess) {
        sample_.gpu_temperature_c = value;
    }
    if (nvml_get_clock_(nvml_device_, kNvmlClockGraphics, &value) == kNvmlSuccess) {
        sample_.gpu_clock_mhz = value;
    }
    NvmlMemory memory{};
    if (nvml_get_memory_(nvml_device_, &memory) == kNvmlSuccess) {
        sample_.gpu_memory_used = memory.used;
        sample_.gpu_memory_total = memory.total;
    }
}

void PerformanceOverlay::SampleCpuAndMemory() {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto current_idle = FileTimeValue(idle);
        const auto current_kernel = FileTimeValue(kernel);
        const auto current_user = FileTimeValue(user);
        if (previous_kernel_ || previous_user_) {
            const auto idle_delta = current_idle - previous_idle_;
            const auto total_delta = current_kernel - previous_kernel_ + current_user - previous_user_;
            if (total_delta) {
                const auto busy = total_delta > idle_delta ? total_delta - idle_delta : 0;
                sample_.cpu_percent = static_cast<unsigned int>(std::min<std::uint64_t>(
                    100, (busy * 100 + total_delta / 2) / total_delta));
            }
        }
        previous_idle_ = current_idle;
        previous_kernel_ = current_kernel;
        previous_user_ = current_user;
    }

    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        sample_.memory_percent = memory.dwMemoryLoad;
        sample_.memory_total = memory.ullTotalPhys;
        sample_.memory_used = memory.ullTotalPhys - memory.ullAvailPhys;
    }
}

void PerformanceOverlay::SampleDxgiMemory() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

    const auto area = ForegroundMonitorArea(owner_);
    IDXGIAdapter1* fallback_adapter = nullptr;
    IDXGIAdapter1* selected_adapter = nullptr;
    for (UINT adapter_index = 0;; ++adapter_index) {
        IDXGIAdapter1* adapter = nullptr;
        const auto adapter_result = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapter_result) || !adapter) break;
        if (!fallback_adapter) {
            fallback_adapter = adapter;
            fallback_adapter->AddRef();
        }
        bool matches = false;
        for (UINT output_index = 0;; ++output_index) {
            IDXGIOutput* output = nullptr;
            const auto output_result = adapter->EnumOutputs(output_index, &output);
            if (output_result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(output_result) || !output) break;
            DXGI_OUTPUT_DESC description{};
            matches = SUCCEEDED(output->GetDesc(&description)) && description.Monitor == area.monitor;
            output->Release();
            if (matches) break;
        }
        if (matches) selected_adapter = adapter;
        else adapter->Release();
        if (selected_adapter) break;
    }
    if (!selected_adapter) selected_adapter = fallback_adapter;
    else if (fallback_adapter) fallback_adapter->Release();

    IDXGIAdapter3* adapter3 = nullptr;
    if (selected_adapter && SUCCEEDED(selected_adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            sample_.gpu_memory_used = memory.CurrentUsage;
            sample_.gpu_memory_total = memory.Budget;
        }
        adapter3->Release();
    }
    if (selected_adapter) selected_adapter->Release();
    factory->Release();
}

void PerformanceOverlay::Show() {
    if (!window_ || visible_) return;
    SampleMetrics();
    ApplySettings(settings_, english_);
    PositionWindow();
    InvalidateRect(window_, nullptr, TRUE);
    UpdateWindow(window_);
    if (!AnimateWindow(window_, 170, AW_SLIDE | AW_HOR_NEGATIVE)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
    SetTimer(window_, kRefreshTimer, 500, nullptr);
    visible_ = true;
}

void PerformanceOverlay::Hide() {
    if (!window_ || !visible_) return;
    KillTimer(window_, kRefreshTimer);
    if (!AnimateWindow(window_, 130, AW_HIDE | AW_SLIDE | AW_HOR_POSITIVE)) ShowWindow(window_, SW_HIDE);
    visible_ = false;
}

int PerformanceOverlay::EnabledMetricCount() const noexcept {
    return static_cast<int>(settings_.performance_show_gpu_usage) +
           static_cast<int>(settings_.performance_show_gpu_temperature) +
           static_cast<int>(settings_.performance_show_gpu_clock) +
           static_cast<int>(settings_.performance_show_gpu_memory) +
           static_cast<int>(settings_.performance_show_cpu_usage) +
           static_cast<int>(settings_.performance_show_memory);
}

void PerformanceOverlay::PositionWindow() {
    if (!window_) return;
    const auto area = ForegroundMonitorArea(owner_);
    if (!area.monitor) return;
    const auto dpi = MonitorDpi(area.monitor);
    const int width = ScaleForDpi(340, dpi);
    const int height = ScaleForDpi(64 + EnabledMetricCount() * 28, dpi);
    const int margin = ScaleForDpi(18, dpi);
    const int x = area.bounds.right - width - margin;
    const int y = settings_.performance_overlay_position == PerformanceOverlayPosition::BottomRight
                      ? area.bounds.bottom - height - margin
                      : area.bounds.top + margin;
    if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                               ScaleForDpi(16, dpi), ScaleForDpi(16, dpi))) {
        if (!SetWindowRgn(window_, region, TRUE)) DeleteObject(region);
    }
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible_ ? SWP_SHOWWINDOW : 0));
}

void PerformanceOverlay::Paint(HDC dc, const RECT& bounds) {
    const auto dpi = WindowDpi(window_);
    const auto scale = [dpi](int value) { return ScaleForDpi(value, dpi); };
    const auto background = CreateSolidBrush(RGB(24, 24, 27));
    const auto accent = CreateSolidBrush(RGB(0, 193, 106));
    if (background) {
        FillRect(dc, &bounds, background);
        DeleteObject(background);
    }
    if (accent) {
        RECT indicator{0, 0, scale(5), bounds.bottom};
        FillRect(dc, &indicator, accent);
        DeleteObject(accent);
    }

    SetBkMode(dc, TRANSPARENT);
    const auto title_font = CreateFontW(-scale(16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Public Sans");
    const auto label_font = CreateFontW(-scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Public Sans");
    const auto value_font = CreateFontW(-scale(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Public Sans");
    const auto old_font = SelectObject(dc, title_font ? title_font : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT title{scale(20), scale(13), bounds.right - scale(18), scale(39)};
    DrawTextW(dc, english_ ? L"PERFORMANCE" : L"ПРОИЗВОДИТЕЛЬНОСТЬ", -1, &title,
              DT_SINGLELINE | DT_VCENTER);
    SetTextColor(dc, RGB(113, 113, 122));
    RECT shortcut{bounds.right - scale(90), scale(13), bounds.right - scale(18), scale(39)};
    DrawTextW(dc, L"ALT + R", -1, &shortcut, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

    int y = scale(47);
    const auto row = [&](std::wstring_view label, const std::wstring& value) {
        SelectObject(dc, label_font ? label_font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(161, 161, 170));
        RECT label_rect{scale(20), y, bounds.right - scale(125), y + scale(24)};
        DrawTextW(dc, label.data(), static_cast<int>(label.size()), &label_rect, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, value_font ? value_font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(0, 220, 130));
        RECT value_rect{bounds.right - scale(170), y, bounds.right - scale(18), y + scale(24)};
        DrawTextW(dc, value.c_str(), -1, &value_rect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
        y += scale(28);
    };
    const auto percent = [](const std::optional<unsigned int>& value) {
        return value ? std::to_wstring(*value) + L"%" : std::wstring{L"--"};
    };
    if (settings_.performance_show_gpu_usage) row(english_ ? L"GPU usage" : L"Загрузка GPU", percent(sample_.gpu_percent));
    if (settings_.performance_show_gpu_temperature) {
        row(english_ ? L"GPU temperature" : L"Температура GPU",
            sample_.gpu_temperature_c ? std::to_wstring(*sample_.gpu_temperature_c) + L" °C" : L"--");
    }
    if (settings_.performance_show_gpu_clock) {
        row(english_ ? L"GPU clock" : L"Частота GPU",
            sample_.gpu_clock_mhz ? std::to_wstring(*sample_.gpu_clock_mhz) + L" MHz" : L"--");
    }
    if (settings_.performance_show_gpu_memory) {
        row(L"VRAM", sample_.gpu_memory_used && sample_.gpu_memory_total
                         ? MemoryText(*sample_.gpu_memory_used, *sample_.gpu_memory_total, english_) : L"--");
    }
    if (settings_.performance_show_cpu_usage) row(english_ ? L"CPU usage" : L"Загрузка CPU", percent(sample_.cpu_percent));
    if (settings_.performance_show_memory) {
        row(english_ ? L"System memory" : L"Оперативная память",
            sample_.memory_used && sample_.memory_total
                ? MemoryText(*sample_.memory_used, *sample_.memory_total, english_) : percent(sample_.memory_percent));
    }

    SelectObject(dc, old_font);
    if (title_font) DeleteObject(title_font);
    if (label_font) DeleteObject(label_font);
    if (value_font) DeleteObject(value_font);
}

LRESULT CALLBACK PerformanceOverlay::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PerformanceOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<PerformanceOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_TIMER && wparam == kRefreshTimer) {
        self->SampleMetrics();
        self->PositionWindow();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (dc) {
            RECT bounds{};
            GetClientRect(window, &bounds);
            self->Paint(dc, bounds);
            EndPaint(window, &paint);
        }
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace openreplay::ui

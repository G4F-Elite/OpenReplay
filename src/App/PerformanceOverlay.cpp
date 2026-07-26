#include "pch.h"

#include "PerformanceOverlay.h"
#include "WindowPlacement.h"

#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <numeric>
#include <cwctype>
#include <vector>

namespace openreplay::ui {
namespace {

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
constexpr UINT kFrameRefreshMilliseconds = 100;
constexpr unsigned int kSystemRefreshTicks = 5;

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

std::optional<double> Number(std::string_view value) {
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result)
               ? std::optional<double>{result}
               : std::nullopt;
}

std::map<std::string, std::string> ParseResponse(std::string_view response) {
    std::map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start < response.size()) {
        const auto end = response.find('\n', start);
        const auto line = response.substr(start, end == std::string_view::npos ? response.size() - start : end - start);
        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            fields.emplace(std::string{line.substr(0, separator)}, std::string{line.substr(separator + 1)});
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

std::wstring Decimal(double value, std::wstring_view suffix = {}) {
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f%.*s", value, static_cast<int>(suffix.size()), suffix.data());
    return text;
}

std::wstring PreciseDecimal(double value, std::wstring_view suffix = {}) {
    wchar_t text[64]{};
    swprintf_s(text, L"%.2f%.*s", value, static_cast<int>(suffix.size()), suffix.data());
    return text;
}

bool FrameTargetProcess(DWORD process_id) {
    if (!process_id || process_id == GetCurrentProcessId()) return false;
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return false;
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const auto queried = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!queried) return false;
    std::wstring name = std::filesystem::path{std::wstring{path.data(), length}}.filename().wstring();
    std::ranges::transform(name, name.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return name != L"lockapp.exe" && name != L"consent.exe" && name != L"explorer.exe" &&
           name != L"shellexperiencehost.exe" && name != L"searchhost.exe" &&
           name != L"startmenuexperiencehost.exe";
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
    StopTelemetry();
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
    nvml_get_power_ = reinterpret_cast<NvmlDeviceGetPowerUsage>(resolve("nvmlDeviceGetPowerUsage"));
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
    nvml_get_power_ = nullptr;
}

void PerformanceOverlay::SampleMetrics() {
    SampleFrameMetrics();
    SampleSystemMetrics();
}

void PerformanceOverlay::SampleSystemMetrics() {
    sample_.gpu_percent.reset();
    sample_.gpu_temperature_c.reset();
    sample_.gpu_clock_mhz.reset();
    sample_.gpu_power_mw.reset();
    sample_.gpu_memory_used.reset();
    sample_.gpu_memory_total.reset();
    sample_.cpu_percent.reset();
    sample_.memory_percent.reset();
    sample_.memory_used.reset();
    sample_.memory_total.reset();
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
    if (nvml_get_power_ && nvml_get_power_(nvml_device_, &value) == kNvmlSuccess) {
        sample_.gpu_power_mw = value;
    }
    NvmlMemory memory{};
    if (nvml_get_memory_(nvml_device_, &memory) == kNvmlSuccess) {
        sample_.gpu_memory_used = memory.used;
        sample_.gpu_memory_total = memory.total;
    }
}

void PerformanceOverlay::StartTelemetry() {
    if (telemetry_process_ || telemetry_launch_attempted_) return;
    telemetry_launch_attempted_ = true;
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) {
        sample_.frame_error = "Unable to resolve telemetry helper";
        return;
    }
    const auto helper = std::filesystem::path{std::wstring{executable.data(), length}}.parent_path() /
                        L"OpenReplay.Telemetry.exe";
    if (!std::filesystem::exists(helper)) {
        sample_.frame_error = "Telemetry helper is missing";
        return;
    }
    const auto arguments = std::format(L"--parent={}", GetCurrentProcessId());
    const auto helper_directory = helper.parent_path().wstring();
    auto command = std::format(L"\"{}\" {}", helper.wstring(), arguments);
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, helper_directory.c_str(), &startup, &process)) {
        sample_.frame_error = "Unable to start frame telemetry";
        return;
    }
    CloseHandle(process.hThread);
    telemetry_process_ = process.hProcess;
    sample_.frame_error = "Starting frame telemetry";
}

void PerformanceOverlay::StopTelemetry() noexcept {
    if (!telemetry_process_) return;
    static_cast<void>(TelemetryCommand("shutdown"));
    WaitForSingleObject(telemetry_process_, 1500);
    CloseHandle(telemetry_process_);
    telemetry_process_ = nullptr;
    telemetry_target_process_ = 0;
    telemetry_pending_process_ = 0;
}

std::string PerformanceOverlay::TelemetryCommand(std::string_view command) const {
    const auto pipe_name = std::format(L"\\\\.\\pipe\\OpenReplay.Telemetry.{}", GetCurrentProcessId());
    std::array<char, 8192> response{};
    DWORD response_size = 0;
    if (!CallNamedPipeW(pipe_name.c_str(), const_cast<char*>(command.data()), static_cast<DWORD>(command.size()),
                        response.data(), static_cast<DWORD>(response.size() - 1), &response_size, 200)) return {};
    return {response.data(), response_size};
}

void PerformanceOverlay::SampleFrameMetrics() {
    const bool requested = settings_.performance_show_fps || settings_.performance_show_fps_lows ||
                           settings_.performance_show_frametime || settings_.performance_show_frametime_graph;
    if (!requested) return;
    DWORD foreground_process = 0;
    if (const auto foreground = GetForegroundWindow()) {
        GetWindowThreadProcessId(foreground, &foreground_process);
    }
    if (!telemetry_process_ && !telemetry_launch_attempted_ && FrameTargetProcess(foreground_process)) {
        telemetry_pending_process_ = foreground_process;
    }
    if (telemetry_process_ && WaitForSingleObject(telemetry_process_, 0) == WAIT_OBJECT_0) {
        CloseHandle(telemetry_process_);
        telemetry_process_ = nullptr;
        telemetry_target_process_ = 0;
        telemetry_launch_attempted_ = false;
    }
    StartTelemetry();
    if (!telemetry_process_) return;

    auto process_id = FrameTargetProcess(foreground_process) ? foreground_process : telemetry_target_process_;
    if (!process_id && telemetry_pending_process_) process_id = telemetry_pending_process_;
    const auto command = process_id && process_id != telemetry_target_process_
                              ? std::format("target:{}", process_id)
                              : std::string{"sample"};
    const auto response = TelemetryCommand(command);
    if (response.empty()) return;
    if (process_id) {
        telemetry_target_process_ = process_id;
        telemetry_pending_process_ = 0;
    }
    const auto fields = ParseResponse(response);
    const auto metric = [&](std::string_view name) -> std::optional<double> {
        const auto found = fields.find(std::string{name});
        return found == fields.end() ? std::nullopt : Number(found->second);
    };
    sample_.fps = metric("fps");
    sample_.fps_low_1 = metric("low1");
    sample_.fps_low_01 = metric("low01");
    sample_.frame_time_ms = metric("frametime");
    sample_.frame_times.clear();
    if (const auto history = fields.find("history"); history != fields.end()) {
        std::string_view values{history->second};
        while (!values.empty()) {
            const auto separator = values.find(',');
            const auto value = Number(values.substr(0, separator));
            if (value) sample_.frame_times.push_back(*value);
            if (separator == std::string_view::npos) break;
            values.remove_prefix(separator + 1);
        }
    }
    if (const auto error = fields.find("error"); error != fields.end()) sample_.frame_error = error->second;
    if (sample_.fps && *sample_.fps > 0.0) sample_.frame_error.clear();
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
    refresh_tick_ = 0;
    ApplySettings(settings_, english_);
    PositionWindow();
    InvalidateRect(window_, nullptr, TRUE);
    UpdateWindow(window_);
    if (!AnimateWindow(window_, 170, AW_SLIDE | AW_HOR_NEGATIVE)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
    SetTimer(window_, kRefreshTimer, kFrameRefreshMilliseconds, nullptr);
    visible_ = true;
}

void PerformanceOverlay::Hide() {
    if (!window_ || !visible_) return;
    KillTimer(window_, kRefreshTimer);
    static_cast<void>(TelemetryCommand("pause"));
    telemetry_target_process_ = 0;
    telemetry_pending_process_ = 0;
    sample_.frame_times.clear();
    if (!telemetry_process_) telemetry_launch_attempted_ = false;
    if (!AnimateWindow(window_, 130, AW_HIDE | AW_SLIDE | AW_HOR_POSITIVE)) ShowWindow(window_, SW_HIDE);
    visible_ = false;
}

int PerformanceOverlay::EnabledMetricCount() const noexcept {
    return static_cast<int>(settings_.performance_show_fps) +
           static_cast<int>(settings_.performance_show_fps_lows) +
           static_cast<int>(settings_.performance_show_frametime) +
           static_cast<int>(settings_.performance_show_gpu_usage) +
           static_cast<int>(settings_.performance_show_gpu_temperature) +
           static_cast<int>(settings_.performance_show_gpu_clock) +
           static_cast<int>(settings_.performance_show_gpu_power) +
           static_cast<int>(settings_.performance_show_gpu_memory) +
           static_cast<int>(settings_.performance_show_cpu_usage) +
           static_cast<int>(settings_.performance_show_memory);
}

void PerformanceOverlay::PositionWindow() {
    if (!window_) return;
    const auto area = ForegroundMonitorArea(owner_);
    if (!area.monitor) return;
    const auto dpi = MonitorDpi(area.monitor);
    const int width = ScaleForDpi(400, dpi);
    const int graph_height = settings_.performance_show_frametime_graph ? 112 : 0;
    const int height = ScaleForDpi(68 + EnabledMetricCount() * 27 + graph_height, dpi);
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
    const auto frame_metric = [](const std::optional<double>& value, std::wstring_view suffix) {
        return value && *value > 0.0 ? Decimal(*value, suffix) : std::wstring{L"--"};
    };
    if (settings_.performance_show_fps) {
        row(L"FPS", frame_metric(sample_.fps, L""));
    }
    if (settings_.performance_show_fps_lows) {
        const auto lows = sample_.fps_low_1 && sample_.fps_low_01 && *sample_.fps_low_1 > 0.0 &&
                                  *sample_.fps_low_01 > 0.0
                              ? Decimal(*sample_.fps_low_1) + L" / " + Decimal(*sample_.fps_low_01)
                              : std::wstring{L"--"};
        row(L"1% / 0.1% low", lows);
    }
    if (settings_.performance_show_frametime) {
        const auto frame_time = sample_.frame_time_ms && *sample_.frame_time_ms > 0.0
                                    ? PreciseDecimal(*sample_.frame_time_ms, L" ms")
                                    : std::wstring{L"--"};
        row(english_ ? L"Frame time" : L"Время кадра", frame_time);
    }
    if (settings_.performance_show_frametime_graph) {
        const int graph_left = scale(20);
        const int graph_right = bounds.right - scale(18);
        const int graph_top = y + scale(5);
        const int graph_bottom = graph_top + scale(82);
        const int plot_top = graph_top + scale(24);
        const int plot_bottom = graph_bottom - scale(5);
        const auto graph_background = CreateSolidBrush(RGB(31, 31, 35));
        RECT graph_bounds{graph_left, graph_top, graph_right, graph_bottom};
        if (graph_background) {
            FillRect(dc, &graph_bounds, graph_background);
            DeleteObject(graph_background);
        }

        double graph_min_ms = 0.0;
        double graph_max_ms = 50.0;
        if (sample_.frame_times.size() >= 2) {
            auto sorted = sample_.frame_times;
            std::ranges::sort(sorted);
            const auto percentile = [&](double value) {
                const auto position = value * static_cast<double>(sorted.size() - 1);
                const auto lower = static_cast<std::size_t>(position);
                const auto upper = std::min(lower + 1, sorted.size() - 1);
                const auto fraction = position - static_cast<double>(lower);
                return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
            };
            const auto low = percentile(0.05);
            const auto high = percentile(0.95);
            const auto padding = std::max(0.5, (high - low) * 0.25);
            graph_min_ms = std::max(0.0, low - padding);
            graph_max_ms = high + padding;
            if (graph_max_ms - graph_min_ms < 2.0) {
                const auto center = (graph_min_ms + graph_max_ms) * 0.5;
                graph_min_ms = std::max(0.0, center - 1.0);
                graph_max_ms = center + 1.0;
            }
        }
        const auto graph_y = [&](double value) {
            const auto normalized = std::clamp(
                (value - graph_min_ms) / (graph_max_ms - graph_min_ms), 0.0, 1.0);
            return plot_bottom - static_cast<int>(std::lround(normalized * (plot_bottom - plot_top)));
        };

        const auto guide_pen = CreatePen(PS_DOT, 1, RGB(63, 63, 70));
        const auto old_pen = SelectObject(dc, guide_pen ? guide_pen : GetStockObject(NULL_PEN));
        for (const auto guide : {graph_min_ms, (graph_min_ms + graph_max_ms) * 0.5, graph_max_ms}) {
            const auto guide_y = graph_y(guide);
            MoveToEx(dc, graph_left, guide_y, nullptr);
            LineTo(dc, graph_right, guide_y);
        }
        SelectObject(dc, old_pen);
        if (guide_pen) DeleteObject(guide_pen);

        if (sample_.frame_times.size() >= 2) {
            std::vector<POINT> trend_points;
            trend_points.reserve(sample_.frame_times.size());
            const auto width = graph_right - graph_left - scale(3);
            for (std::size_t index = 0; index < sample_.frame_times.size(); ++index) {
                const auto x = graph_left + scale(1) + static_cast<int>(
                    index * static_cast<std::size_t>(width) / (sample_.frame_times.size() - 1));
                double weighted_total = 0.0;
                double weight_total = 0.0;
                for (int offset = -2; offset <= 2; ++offset) {
                    const auto neighbor = static_cast<std::ptrdiff_t>(index) + offset;
                    if (neighbor < 0 || neighbor >= static_cast<std::ptrdiff_t>(sample_.frame_times.size())) continue;
                    const auto weight = static_cast<double>(3 - std::abs(offset));
                    weighted_total += sample_.frame_times[static_cast<std::size_t>(neighbor)] * weight;
                    weight_total += weight;
                }
                trend_points.push_back({x, graph_y(weighted_total / weight_total)});
            }

            const auto raw_brush = CreateSolidBrush(RGB(63, 120, 92));
            const auto previous_brush = SelectObject(dc, raw_brush ? raw_brush : GetStockObject(NULL_BRUSH));
            const auto previous_raw_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            for (std::size_t index = 0; index < sample_.frame_times.size(); ++index) {
                const auto x = graph_left + scale(1) + static_cast<int>(
                    index * static_cast<std::size_t>(width) / (sample_.frame_times.size() - 1));
                const auto point_y = graph_y(sample_.frame_times[index]);
                const auto radius = std::max(1, scale(1));
                Ellipse(dc, x - radius, point_y - radius, x + radius + 1, point_y + radius + 1);
            }
            SelectObject(dc, previous_raw_pen);
            SelectObject(dc, previous_brush);
            if (raw_brush) DeleteObject(raw_brush);

            const auto graph_pen = CreatePen(PS_SOLID, scale(2), RGB(0, 220, 130));
            const auto previous_pen = SelectObject(dc, graph_pen ? graph_pen : GetStockObject(WHITE_PEN));
            Polyline(dc, trend_points.data(), static_cast<int>(trend_points.size()));
            SelectObject(dc, previous_pen);
            if (graph_pen) DeleteObject(graph_pen);
        }
        SelectObject(dc, label_font ? label_font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(113, 113, 122));
        RECT graph_label{graph_left + scale(7), graph_top + scale(4), graph_right - scale(7), graph_top + scale(22)};
        const auto graph_text = english_
                                    ? std::format(L"FRAME TIME · {:.1f}–{:.1f} ms · TREND", graph_min_ms, graph_max_ms)
                                    : std::format(L"ВРЕМЯ КАДРА · {:.1f}–{:.1f} мс · ТРЕНД", graph_min_ms, graph_max_ms);
        DrawTextW(dc, graph_text.c_str(), -1, &graph_label, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        if (sample_.frame_times.empty() && !sample_.frame_error.empty()) {
            const std::wstring status{sample_.frame_error.begin(), sample_.frame_error.end()};
            RECT status_rect{graph_left + scale(7), graph_top + scale(28), graph_right - scale(7), graph_bottom - scale(6)};
            DrawTextW(dc, status.c_str(), -1, &status_rect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
        y = graph_bottom + scale(15);
    }
    if (settings_.performance_show_gpu_usage) row(english_ ? L"GPU usage" : L"Загрузка GPU", percent(sample_.gpu_percent));
    if (settings_.performance_show_gpu_temperature) {
        row(english_ ? L"GPU temperature" : L"Температура GPU",
            sample_.gpu_temperature_c ? std::to_wstring(*sample_.gpu_temperature_c) + L" °C" : L"--");
    }
    if (settings_.performance_show_gpu_clock) {
        row(english_ ? L"GPU clock" : L"Частота GPU",
            sample_.gpu_clock_mhz ? std::to_wstring(*sample_.gpu_clock_mhz) + L" MHz" : L"--");
    }
    if (settings_.performance_show_gpu_power) {
        row(english_ ? L"GPU power" : L"Потребление GPU",
            sample_.gpu_power_mw ? Decimal(static_cast<double>(*sample_.gpu_power_mw) / 1000.0, L" W") : L"--");
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
        self->SampleFrameMetrics();
        if (++self->refresh_tick_ >= kSystemRefreshTicks) {
            self->refresh_tick_ = 0;
            self->SampleSystemMetrics();
            self->PositionWindow();
        }
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

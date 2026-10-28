#include "pch.h"

#include "PerformanceOverlay.h"
#include "GdiFonts.h"
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
constexpr int kMaxTopmostRaises = 3;
// Overlay layout in 96-DPI pixels; Paint and ContentHeight must agree.
constexpr int kOverlayWidth = 360;
constexpr int kOverlayPadding = 16;
constexpr int kOverlayHeader = 22;
constexpr int kStatusGap = 16;
constexpr int kStatusHeight = 44;
constexpr int kGraphGap = 14;
constexpr int kGraphHeight = 30;
constexpr int kCaptionGap = 5;
constexpr int kCaptionHeight = 14;
constexpr int kSectionHeight = 30;
constexpr int kRowHeight = 38;
// The frame-time graph is a fixed time window, so it scrolls at the same speed at any frame rate.
// 1.2 s keeps a 60 FPS frame several pixels wide, since bar width is the frame's duration.
constexpr auto kGraphWindow = std::chrono::milliseconds{1200};
// 60 FPS is the target: slower frames turn yellow, below 30 FPS red. The slack absorbs timing jitter at 60.
constexpr double kWarnFrameMs = 1000.0 / 57.0;
constexpr double kBadFrameMs = 1000.0 / 30.0;
// The right edge trails real time by more than the sampling interval, so new frames slide in instead of popping.
constexpr auto kGraphDelay = std::chrono::milliseconds{160};
constexpr UINT kAnimationMilliseconds = 15;

std::uint64_t FileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

std::wstring MemoryText(std::uint64_t used, std::uint64_t total) {
    constexpr double gibibyte = 1024.0 * 1024.0 * 1024.0;
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f / %.1f", static_cast<double>(used) / gibibyte,
               static_cast<double>(total) / gibibyte);
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
        KillTimer(window_, kAnimationTimer);
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
        InvalidateRect(window_, nullptr, FALSE);
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
    if (const auto followed = game_window_ ? game_window_ : GetForegroundWindow()) {
        GetWindowThreadProcessId(followed, &foreground_process);
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

    // Rebuild absolute end times from the newest frame backwards and keep only frames not seen yet.
    // The newest frame's present is a system-wide QPC tick, so every sample lands on the same timeline.
    using SteadyDuration = std::chrono::steady_clock::duration;
    const auto to_duration = [](double milliseconds) {
        return std::chrono::duration_cast<SteadyDuration>(std::chrono::duration<double, std::milli>{milliseconds});
    };
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const auto now = std::chrono::steady_clock::now();
    const auto present = metric("present").value_or(0.0);
    const auto age_ms = present > 0.0 && frequency.QuadPart > 0
        ? std::max(0.0, (static_cast<double>(counter.QuadPart) - present) * 1000.0 /
                            static_cast<double>(frequency.QuadPart))
        : 0.0;
    const auto newest = frame_history_.empty() ? std::chrono::steady_clock::time_point{} : frame_history_.back().end;
    auto end = now - to_duration(age_ms);
    std::vector<FrameMark> fresh;
    for (auto frame = sample_.frame_times.rbegin(); frame != sample_.frame_times.rend(); ++frame) {
        const auto duration = to_duration(*frame);
        if (end <= newest + duration / 2) break;
        fresh.push_back({end, *frame});
        end -= duration;
    }
    frame_history_.insert(frame_history_.end(), fresh.rbegin(), fresh.rend());
    const auto cutoff = now - kGraphWindow - std::chrono::seconds{1};
    std::erase_if(frame_history_, [cutoff](const FrameMark& frame) { return frame.end < cutoff; });
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

    const auto area = FollowedArea();
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
    TrackGameWindow();
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
    SetTimer(window_, kAnimationTimer, kAnimationMilliseconds, nullptr);
    visible_ = true;
}

void PerformanceOverlay::Hide() {
    if (!window_ || !visible_) return;
    KillTimer(window_, kRefreshTimer);
    KillTimer(window_, kAnimationTimer);
    static_cast<void>(TelemetryCommand("pause"));
    telemetry_target_process_ = 0;
    telemetry_pending_process_ = 0;
    game_window_ = nullptr;
    sample_.frame_times.clear();
    frame_history_.clear();
    graph_scale_ms_ = 0.0;
    if (!telemetry_process_) telemetry_launch_attempted_ = false;
    if (!AnimateWindow(window_, 130, AW_HIDE | AW_SLIDE | AW_HOR_POSITIVE)) ShowWindow(window_, SW_HIDE);
    visible_ = false;
}

int PerformanceOverlay::ContentHeight() const noexcept {
    const auto& s = settings_;
    const int gpu_rows = s.performance_show_gpu_usage + s.performance_show_gpu_temperature +
                         s.performance_show_gpu_clock + s.performance_show_gpu_power + s.performance_show_gpu_memory;
    const int system_rows = s.performance_show_cpu_usage + s.performance_show_memory;
    int height = kOverlayPadding + kOverlayHeader;
    if (s.performance_show_fps || s.performance_show_fps_lows) height += kStatusGap + kStatusHeight;
    if (s.performance_show_frametime_graph) height += kGraphGap + kGraphHeight;
    if (s.performance_show_frametime_graph || s.performance_show_frametime) height += kCaptionGap + kCaptionHeight;
    if (gpu_rows) height += kSectionHeight + gpu_rows * kRowHeight;
    if (system_rows) height += kSectionHeight + system_rows * kRowHeight;
    return height + kOverlayPadding;
}

// The overlay belongs to the last fullscreen game, so alt-tabbing to another window or monitor leaves it,
// and its FPS, on that game until another fullscreen app comes to the front or the game closes.
void PerformanceOverlay::TrackGameWindow() {
    const auto foreground = GetForegroundWindow();
    DWORD process_id = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &process_id);
    if (foreground && IsFullscreenWindow(foreground) && FrameTargetProcess(process_id)) game_window_ = foreground;
    else if (game_window_ && !IsWindow(game_window_)) game_window_ = nullptr;
}

MonitorArea PerformanceOverlay::FollowedArea() const noexcept {
    return game_window_ ? WindowMonitorArea(game_window_) : ForegroundMonitorArea(owner_);
}

void PerformanceOverlay::PositionWindow() {
    if (!window_) return;
    const auto area = FollowedArea();
    if (!area.monitor) return;
    const auto dpi = MonitorDpi(area.monitor);
    const int width = ScaleForDpi(kOverlayWidth, dpi);
    const int height = ScaleForDpi(ContentHeight(), dpi);
    const int margin = ScaleForDpi(18, dpi);
    const int x = area.bounds.right - width - margin;
    const int y = settings_.performance_overlay_position == PerformanceOverlayPosition::BottomRight
                      ? area.bounds.bottom - height - margin
                      : area.bounds.top + margin;

    RECT desired{x, y, x + width, y + height};
    RECT current{};
    const bool has_current_bounds = GetWindowRect(window_, &current) != FALSE;
    const bool same_bounds = has_current_bounds && EqualRect(&current, &desired);
    const bool same_size = has_current_bounds &&
                           current.right - current.left == width &&
                           current.bottom - current.top == height;
    if (same_bounds) return;

    if (!same_size) {
        if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                                   ScaleForDpi(24, dpi), ScaleForDpi(24, dpi))) {
            if (!SetWindowRgn(window_, region, TRUE)) DeleteObject(region);
        }
    }
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible_ ? SWP_SHOWWINDOW : 0));
}

void PerformanceOverlay::Paint(HDC dc, const RECT& bounds) {
    const auto dpi = WindowDpi(window_);
    const auto scale = [dpi](int value) { return ScaleForDpi(value, dpi); };
    const auto fill = [dc](RECT rect, COLORREF color) {
        if (const auto brush = CreateSolidBrush(color)) {
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        }
    };
    const GdiFont title_font{scale(13), FW_BOLD};
    const GdiFont big_font{scale(40), FW_BOLD};
    const GdiFont big_light_font{scale(40), FW_MEDIUM};
    const GdiFont small_font{scale(12), FW_MEDIUM};
    const GdiFont caption_font{scale(10), FW_MEDIUM};
    const GdiFont label_font{scale(13), FW_MEDIUM};
    const GdiFont value_font{scale(13), FW_SEMIBOLD};
    const GdiFont unit_font{scale(11), FW_MEDIUM};
    const auto key_font = CreateFontW(-scale(10), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      L"Cascadia Mono");
    const auto old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    const auto text = [&](const GdiFont& font, COLORREF color, std::wstring_view value, RECT rect, UINT format) {
        SelectObject(dc, font.For(value));
        SetTextColor(dc, color);
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect, format | DT_SINGLELINE | DT_NOPREFIX);
    };
    const auto text_width = [&](const GdiFont& font, std::wstring_view value) {
        SelectObject(dc, font.For(value));
        SIZE size{};
        GetTextExtentPoint32W(dc, value.data(), static_cast<int>(value.size()), &size);
        return static_cast<int>(size.cx);
    };
    const auto descent = [&](const GdiFont& font) {
        SelectObject(dc, font.For({}));
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        return static_cast<int>(metrics.tmDescent);
    };

    constexpr COLORREF background = RGB(14, 14, 15);
    constexpr COLORREF line = RGB(31, 31, 34);
    constexpr COLORREF primary = RGB(237, 237, 237);
    constexpr COLORREF secondary = RGB(161, 161, 170);
    constexpr COLORREF muted = RGB(124, 124, 130);
    constexpr COLORREF faint = RGB(85, 85, 91);
    constexpr COLORREF accent = RGB(118, 185, 0);
    constexpr COLORREF value_green = RGB(139, 212, 0);
    fill(bounds, background);
    SetBkMode(dc, TRANSPARENT);
    const int left = scale(18);
    const int right = bounds.right - scale(18);

    const auto border_pen = CreatePen(PS_SOLID, 1, line);
    const auto previous_pen = SelectObject(dc, border_pen ? border_pen : GetStockObject(NULL_PEN));
    const auto previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, bounds.right, bounds.bottom, scale(24), scale(24));

    int y = scale(kOverlayPadding);
    const int header_bottom = y + scale(kOverlayHeader);
    const int mark_top = y + (scale(kOverlayHeader) - scale(7)) / 2;
    fill({left, mark_top, left + scale(7), mark_top + scale(7)}, accent);
    text(title_font, primary, english_ ? L"Performance" : L"Производительность",
         {left + scale(15), y, right - scale(70), header_bottom}, DT_LEFT | DT_VCENTER);

    // Keycaps for the fixed toggle shortcut, laid out right to left.
    const auto key_pen = CreatePen(PS_SOLID, 1, RGB(46, 46, 51));
    SelectObject(dc, key_pen ? key_pen : GetStockObject(NULL_PEN));
    SelectObject(dc, key_font ? key_font : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(196, 196, 200));
    int key_right = right;
    for (const std::wstring_view key : {std::wstring_view{L"R"}, std::wstring_view{L"Alt"}}) {
        SIZE key_size{};
        GetTextExtentPoint32W(dc, key.data(), static_cast<int>(key.size()), &key_size);
        const int key_width = std::max(scale(22), static_cast<int>(key_size.cx) + scale(10));
        RECT cap{key_right - key_width, y + scale(2), key_right, header_bottom - scale(1)};
        RoundRect(dc, cap.left, cap.top, cap.right, cap.bottom, scale(10), scale(10));
        DrawTextW(dc, key.data(), static_cast<int>(key.size()), &cap,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        key_right -= key_width + scale(3);
    }
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
    if (key_pen) DeleteObject(key_pen);
    if (border_pen) DeleteObject(border_pen);
    y = header_bottom;

    if (settings_.performance_show_fps || settings_.performance_show_fps_lows) {
        y += scale(kStatusGap);
        const int baseline = y + scale(kStatusHeight) - descent(big_font);
        int x = left;
        if (settings_.performance_show_fps) {
            const auto fps = sample_.fps && *sample_.fps > 0.0 ? std::to_wstring(std::lround(*sample_.fps))
                                                                 : std::wstring{L"--"};
            text(big_font, primary, fps, {x, y, right, y + scale(kStatusHeight)}, DT_LEFT | DT_BOTTOM);
            x += text_width(big_font, fps);
            text(big_light_font, primary, L" FPS", {x, y, right, y + scale(kStatusHeight)}, DT_LEFT | DT_BOTTOM);
            x += text_width(big_light_font, L" FPS") + scale(10);
        }
        if (settings_.performance_show_fps_lows) {
            const auto low = [](const std::optional<double>& value) {
                return value && *value > 0.0 ? Decimal(*value) : std::wstring{L"--"};
            };
            const auto lows = L"1% " + low(sample_.fps_low_1) + L" · 0.1% " + low(sample_.fps_low_01);
            text(small_font, muted, lows, {x, y, right, baseline + descent(small_font)}, DT_LEFT | DT_BOTTOM);
        }
        y += scale(kStatusHeight);
    }

    double graph_top_ms = 40.0;
    if (settings_.performance_show_frametime_graph) {
        y += scale(kGraphGap);
        const RECT graph{left, y, right, y + scale(kGraphHeight)};
        using SteadyClock = std::chrono::steady_clock;
        const auto to_duration = [](double milliseconds) {
            return std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double, std::milli>{milliseconds});
        };
        const auto right_time = SteadyClock::now() - kGraphDelay;
        const auto left_time = right_time - kGraphWindow;
        // Positions snap to a pixel grid pinned to absolute time, so a frame keeps its width while it scrolls.
        const auto ms_per_pixel = std::chrono::duration<double, std::milli>(kGraphWindow).count() /
                                  static_cast<double>(std::max(1L, graph.right - graph.left));
        const auto pixel = [ms_per_pixel](SteadyClock::time_point time) {
            return static_cast<long long>(std::floor(
                std::chrono::duration<double, std::milli>(time.time_since_epoch()).count() / ms_per_pixel));
        };
        const auto left_pixel = pixel(left_time);
        const auto x_at = [&](SteadyClock::time_point time) {
            return static_cast<int>(graph.left + pixel(time) - left_pixel);
        };

        if (settings_.performance_frametime_line) {
            double target_top = 10.0;
            for (const auto& frame : frame_history_) {
                if (frame.end >= left_time) target_top = std::max(target_top, std::ceil(frame.milliseconds * 1.15 / 10.0) * 10.0);
            }
            // Eased so a spike entering or leaving the window does not make the whole line jump.
            graph_scale_ms_ = graph_scale_ms_ <= 0.0 ? target_top : graph_scale_ms_ + (target_top - graph_scale_ms_) * 0.12;
            graph_top_ms = graph_scale_ms_;
        }
        const auto bar_height = [&](double ms) {
            return std::max(scale(2), static_cast<int>(std::lround(
                std::clamp(ms / graph_top_ms, 0.0, 1.0) * (graph.bottom - graph.top))));
        };

        if (settings_.performance_frametime_line) fill(graph, RGB(20, 20, 22));
        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, graph.left, graph.top, graph.right, graph.bottom);
        if (!settings_.performance_frametime_line) {
            // One bar per frame: its width is the frame's duration, its height grows only slightly with it.
            const int graph_height = graph.bottom - graph.top;
            for (const auto& frame : frame_history_) {
                const auto start = frame.end - to_duration(frame.milliseconds);
                if (frame.end < left_time || start > right_time) continue;
                const int bar_left = x_at(start);
                const int bar_right = std::max(bar_left + 1, x_at(frame.end) - scale(1));
                const auto height = static_cast<int>(std::lround(
                    graph_height * std::clamp(0.3 + frame.milliseconds / 120.0, 0.3, 1.0)));
                const auto color = frame.milliseconds > kBadFrameMs ? RGB(255, 95, 99)
                                 : frame.milliseconds > kWarnFrameMs ? RGB(224, 179, 65)
                                                                     : accent;
                fill({bar_left, graph.bottom - height, bar_right, graph.bottom}, color);
            }
        } else {
            std::vector<POINT> points;
            for (const auto& frame : frame_history_) {
                if (frame.end < left_time - std::chrono::milliseconds{200}) continue;
                points.push_back({x_at(frame.end), graph.bottom - bar_height(frame.milliseconds)});
            }
            if (points.size() >= 2) {
                auto area = points;
                area.push_back({points.back().x, graph.bottom});
                area.push_back({points.front().x, graph.bottom});
                // Accent at 12% over the overlay background; GDI has no alpha fill.
                const auto area_brush = CreateSolidBrush(RGB(27, 35, 13));
                SelectObject(dc, area_brush ? area_brush : GetStockObject(NULL_BRUSH));
                SelectObject(dc, GetStockObject(NULL_PEN));
                Polygon(dc, area.data(), static_cast<int>(area.size()));
                const auto graph_pen = CreatePen(PS_SOLID, scale(2), value_green);
                SelectObject(dc, graph_pen ? graph_pen : GetStockObject(WHITE_PEN));
                Polyline(dc, points.data(), static_cast<int>(points.size()));
                SelectObject(dc, GetStockObject(NULL_BRUSH));
                SelectObject(dc, GetStockObject(NULL_PEN));
                if (area_brush) DeleteObject(area_brush);
                if (graph_pen) DeleteObject(graph_pen);
            }
        }
        RestoreDC(dc, saved);
        if (frame_history_.empty() && !sample_.frame_error.empty()) {
            const std::wstring status{sample_.frame_error.begin(), sample_.frame_error.end()};
            text(caption_font, faint, status, graph, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
        y = graph.bottom;
    }

    if (settings_.performance_show_frametime_graph || settings_.performance_show_frametime) {
        y += scale(kCaptionGap);
        const RECT caption{left, y, right, y + scale(kCaptionHeight)};
        std::wstring frame_time = L"frametime";
        if (settings_.performance_show_frametime) {
            frame_time += L" · " + (sample_.frame_time_ms && *sample_.frame_time_ms > 0.0
                                        ? PreciseDecimal(*sample_.frame_time_ms)
                                        : std::wstring{L"--"}) + (english_ ? L" ms" : L" мс");
        }
        text(caption_font, faint, frame_time, caption, DT_LEFT | DT_VCENTER);
        if (settings_.performance_show_frametime_graph && settings_.performance_frametime_line) {
            text(caption_font, faint, std::format(L"0–{:.0f} {}", graph_top_ms, english_ ? L"ms" : L"мс"),
                 caption, DT_RIGHT | DT_VCENTER);
        }
        y = caption.bottom;
    }

    struct Row {
        std::wstring_view label;
        std::wstring value;
        std::wstring_view unit;
    };
    const auto percent = [](const std::optional<unsigned int>& value) {
        return value ? std::to_wstring(*value) : std::wstring{L"--"};
    };
    std::vector<Row> gpu_rows;
    if (settings_.performance_show_gpu_usage) {
        gpu_rows.push_back({english_ ? L"Usage" : L"Загрузка", percent(sample_.gpu_percent), L"%"});
    }
    if (settings_.performance_show_gpu_temperature) {
        gpu_rows.push_back({english_ ? L"Temperature" : L"Температура",
                            sample_.gpu_temperature_c ? std::to_wstring(*sample_.gpu_temperature_c) : L"--", L"°C"});
    }
    if (settings_.performance_show_gpu_clock) {
        gpu_rows.push_back({english_ ? L"Clock" : L"Частота",
                            sample_.gpu_clock_mhz ? std::to_wstring(*sample_.gpu_clock_mhz) : L"--",
                            english_ ? L"MHz" : L"МГц"});
    }
    if (settings_.performance_show_gpu_power) {
        gpu_rows.push_back({english_ ? L"Power" : L"Питание",
                            sample_.gpu_power_mw ? Decimal(static_cast<double>(*sample_.gpu_power_mw) / 1000.0) : L"--",
                            english_ ? L"W" : L"Вт"});
    }
    if (settings_.performance_show_gpu_memory) {
        gpu_rows.push_back({L"VRAM", sample_.gpu_memory_used && sample_.gpu_memory_total
                                         ? MemoryText(*sample_.gpu_memory_used, *sample_.gpu_memory_total) : L"--",
                            english_ ? L"GB" : L"ГБ"});
    }
    std::vector<Row> system_rows;
    if (settings_.performance_show_cpu_usage) system_rows.push_back({L"CPU", percent(sample_.cpu_percent), L"%"});
    if (settings_.performance_show_memory) {
        const bool totals = sample_.memory_used && sample_.memory_total;
        system_rows.push_back({english_ ? L"RAM" : L"ОЗУ",
                               totals ? MemoryText(*sample_.memory_used, *sample_.memory_total)
                                      : percent(sample_.memory_percent),
                               totals ? (english_ ? L"GB" : L"ГБ") : L"%"});
    }

    const auto last_section = system_rows.empty() ? &gpu_rows : &system_rows;
    const auto section = [&](std::wstring_view title, const std::vector<Row>& rows) {
        if (rows.empty()) return;
        SetTextCharacterExtra(dc, scale(1));
        text(caption_font, faint, title, {left, y + scale(12), right, y + scale(kSectionHeight)}, DT_LEFT | DT_VCENTER);
        SetTextCharacterExtra(dc, 0);
        y += scale(kSectionHeight);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            const RECT rect{left, y, right, y + scale(kRowHeight)};
            text(label_font, secondary, row.label, rect, DT_LEFT | DT_VCENTER);
            const int unit_width = text_width(unit_font, row.unit);
            text(unit_font, muted, row.unit, rect, DT_RIGHT | DT_VCENTER);
            text(value_font, value_green, row.value, {rect.left, rect.top, rect.right - unit_width - scale(4), rect.bottom},
                 DT_RIGHT | DT_VCENTER);
            y = rect.bottom;
            if (&rows != last_section || index + 1 < rows.size()) fill({0, y - 1, bounds.right, y}, line);
        }
    };
    section(L"GPU", gpu_rows);
    section(english_ ? L"SYSTEM" : L"СИСТЕМА", system_rows);

    SelectObject(dc, old_font);
    if (key_font) DeleteObject(key_font);
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
        // Fullscreen games often make themselves topmost when they get focus; take the top back from them,
        // but give up on a window that keeps grabbing it back instead of fighting it ten times a second.
        const auto foreground = GetForegroundWindow();
        if (foreground != self->topmost_rival_) {
            self->topmost_rival_ = foreground;
            self->topmost_raises_ = 0;
        }
        for (auto above = GetWindow(window, GW_HWNDPREV);
             foreground && above && self->topmost_raises_ < kMaxTopmostRaises; above = GetWindow(above, GW_HWNDPREV)) {
            if (above != foreground) continue;
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            ++self->topmost_raises_;
            break;
        }
        self->TrackGameWindow();
        self->SampleFrameMetrics();
        if (++self->refresh_tick_ >= kSystemRefreshTicks) {
            self->refresh_tick_ = 0;
            self->SampleSystemMetrics();
            self->PositionWindow();
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_TIMER && wparam == kAnimationTimer) {
        if (self->settings_.performance_show_frametime_graph) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (dc) {
            RECT bounds{};
            GetClientRect(window, &bounds);
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            const auto buffer = CreateCompatibleDC(dc);
            const auto bitmap = buffer && width > 0 && height > 0
                                    ? CreateCompatibleBitmap(dc, width, height)
                                    : nullptr;
            if (buffer && bitmap) {
                const auto previous_bitmap = SelectObject(buffer, bitmap);
                self->Paint(buffer, bounds);
                BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, previous_bitmap);
                DeleteObject(bitmap);
                DeleteDC(buffer);
            } else {
                if (bitmap) DeleteObject(bitmap);
                if (buffer) DeleteDC(buffer);
                self->Paint(dc, bounds);
            }
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

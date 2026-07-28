#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <shellapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
struct Handle {
    HANDLE value{nullptr};
    ~Handle() { Reset(); }
    Handle() = default;
    explicit Handle(HANDLE input) : value(input) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    void Reset(HANDLE input = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = input;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value && value != INVALID_HANDLE_VALUE;
    }
};

struct FramePoint {
    Clock::time_point received;
    double milliseconds{};
};

std::optional<DWORD> ParentProcessId() {
    int count = 0;
    const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return std::nullopt;
    std::optional<DWORD> result;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument{arguments[index]};
        constexpr std::wstring_view prefix{L"--parent="};
        if (!argument.starts_with(prefix)) continue;
        std::wstring text{argument.substr(prefix.size())};
        wchar_t* end = nullptr;
        const auto value = std::wcstoul(text.c_str(), &end, 10);
        if (end == text.c_str() + text.size() && value <= MAXDWORD) result = static_cast<DWORD>(value);
    }
    LocalFree(arguments);
    return result;
}

struct WindowSearch {
    DWORD process_id{};
    HWND window{};
    std::uint64_t area{};
};

HWND TargetWindow(DWORD process_id) {
    if (const auto foreground = GetForegroundWindow()) {
        DWORD foreground_process = 0;
        GetWindowThreadProcessId(foreground, &foreground_process);
        if (foreground_process == process_id) return foreground;
    }

    WindowSearch search{process_id};
    EnumWindows([](HWND window, LPARAM state) {
        auto& search = *reinterpret_cast<WindowSearch*>(state);
        DWORD owner_process = 0;
        GetWindowThreadProcessId(window, &owner_process);
        if (owner_process != search.process_id || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return TRUE;
        const auto width = std::max<LONG>(0, bounds.right - bounds.left);
        const auto height = std::max<LONG>(0, bounds.bottom - bounds.top);
        const auto area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        if (area > search.area) {
            search.window = window;
            search.area = area;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

class FrameCapture final {
public:
    ~FrameCapture() { Stop(); }

    bool Target(DWORD process_id) {
        if (!process_id) {
            Stop();
            return true;
        }
        {
            std::scoped_lock lock{mutex_};
            if (process_id_ == process_id && capture_running_) return true;
        }
        Stop();

        const auto window = TargetWindow(process_id);
        if (!window) {
            SetError("Target window is unavailable");
            return false;
        }
        const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        auto duplication = CreateDuplication(monitor);
        if (!duplication) return false;

        {
            std::scoped_lock lock{mutex_};
            process_id_ = process_id;
            frame_times_.clear();
            error_.clear();
        }
        stop_capture_ = false;
        capture_running_ = true;
        capture_thread_ = std::thread{
            [this, monitor, duplication = std::move(*duplication)]() mutable {
                ReadFrames(monitor, std::move(duplication));
            }};
        return true;
    }

    void Stop() noexcept {
        stop_capture_ = true;
        if (capture_thread_.joinable()) capture_thread_.join();
        capture_running_ = false;
        std::scoped_lock lock{mutex_};
        process_id_ = 0;
        frame_times_.clear();
    }

    std::string Snapshot() {
        std::vector<FramePoint> frames;
        DWORD process_id = 0;
        std::string error;
        {
            std::scoped_lock lock{mutex_};
            const auto cutoff = Clock::now() - std::chrono::seconds{60};
            std::erase_if(frame_times_, [cutoff](const FramePoint& frame) { return frame.received < cutoff; });
            frames = frame_times_;
            process_id = process_id_;
            error = error_;
        }
        if (!frames.empty() && Clock::now() - frames.back().received > std::chrono::seconds{2}) frames.clear();
        if (frames.empty() && process_id && error.empty()) error = "Waiting for captured frames";

        const auto average = [](const auto& values) {
            double total = 0.0;
            for (const auto value : values) total += value;
            return values.empty() ? 0.0 : total / static_cast<double>(values.size());
        };
        std::vector<double> recent;
        double recent_duration = 0.0;
        for (auto frame = frames.rbegin(); frame != frames.rend() && recent_duration < 1000.0; ++frame) {
            recent.push_back(frame->milliseconds);
            recent_duration += frame->milliseconds;
        }
        const auto fps = recent_duration > 0.0
                             ? static_cast<double>(recent.size()) * 1000.0 / recent_duration
                             : 0.0;
        const auto frame_count = std::min<std::size_t>(recent.size(), 8);
        const std::vector<double> latest_frames{recent.begin(), recent.begin() + static_cast<std::ptrdiff_t>(frame_count)};
        const auto frame_time = average(latest_frames);

        std::vector<double> sorted;
        sorted.reserve(frames.size());
        for (const auto& frame : frames) sorted.push_back(frame.milliseconds);
        std::ranges::sort(sorted, std::greater{});
        const auto low = [&](double fraction) {
            if (sorted.empty()) return 0.0;
            const auto count = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sorted.size() * fraction)));
            const std::vector<double> slowest{sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count)};
            return 1000.0 / average(slowest);
        };

        std::string history;
        const auto first = frames.size() > 120 ? frames.size() - 120 : 0;
        for (std::size_t index = first; index < frames.size(); ++index) {
            if (!history.empty()) history += ',';
            history += std::format("{:.4f}", frames[index].milliseconds);
        }
        return std::format("ok\npid={}\nfps={:.2f}\nlow1={:.2f}\nlow01={:.2f}\nframetime={:.2f}\n"
                           "history={}\nerror={}\nsource=DXGI Desktop Duplication",
                           process_id, fps, low(0.01), low(0.001), frame_time, history, error);
    }

private:
    struct Duplication {
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<IDXGIOutputDuplication> output;
    };

    std::optional<Duplication> CreateDuplication(HMONITOR monitor) noexcept {
        winrt::com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
            SetError("Unable to create DXGI factory");
            return std::nullopt;
        }

        for (UINT adapter_index = 0;; ++adapter_index) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            const auto adapter_result = factory->EnumAdapters1(adapter_index, adapter.put());
            if (adapter_result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(adapter_result)) continue;
            for (UINT output_index = 0;; ++output_index) {
                winrt::com_ptr<IDXGIOutput> output;
                const auto output_result = adapter->EnumOutputs(output_index, output.put());
                if (output_result == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(output_result)) continue;
                DXGI_OUTPUT_DESC description{};
                if (FAILED(output->GetDesc(&description)) || description.Monitor != monitor) continue;

                Duplication result;
                if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, result.device.put(), nullptr, nullptr))) {
                    SetError("Unable to create display telemetry device");
                    return std::nullopt;
                }
                const auto output1 = output.as<IDXGIOutput1>();
                const auto duplicate_result = output1->DuplicateOutput(result.device.get(), result.output.put());
                if (FAILED(duplicate_result)) {
                    SetError(std::format("Unable to duplicate display output: 0x{:08X}",
                                         static_cast<unsigned int>(duplicate_result)));
                    return std::nullopt;
                }
                return result;
            }
        }
        SetError("Target display is unavailable");
        return std::nullopt;
    }

    void ReadFrames(HMONITOR monitor, Duplication duplication) noexcept {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::optional<std::int64_t> last_timestamp;
        while (!stop_capture_) {
            DXGI_OUTDUPL_FRAME_INFO frame{};
            winrt::com_ptr<IDXGIResource> resource;
            const auto acquired = duplication.output->AcquireNextFrame(100, &frame, resource.put());
            if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (acquired == DXGI_ERROR_ACCESS_LOST) {
                duplication = {};
                while (!stop_capture_) {
                    auto replacement = CreateDuplication(monitor);
                    if (replacement) {
                        duplication = std::move(*replacement);
                        last_timestamp.reset();
                        SetError({});
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{250});
                }
                continue;
            }
            if (FAILED(acquired)) {
                SetError(std::format("Display telemetry stopped: 0x{:08X}",
                                     static_cast<unsigned int>(acquired)));
                break;
            }

            if (frame.LastPresentTime.QuadPart > 0 && frequency.QuadPart > 0) {
                const auto timestamp = frame.LastPresentTime.QuadPart;
                if (last_timestamp && timestamp > *last_timestamp) {
                    const auto accumulated = std::max(1U, frame.AccumulatedFrames);
                    const auto milliseconds = static_cast<double>(timestamp - *last_timestamp) * 1000.0 /
                                              static_cast<double>(frequency.QuadPart) / accumulated;
                    if (milliseconds > 0.05 && milliseconds <= 1000.0) {
                        std::scoped_lock lock{mutex_};
                        for (UINT index = 0; index < accumulated; ++index) {
                            frame_times_.push_back({Clock::now(), milliseconds});
                        }
                        if (frame_times_.size() > 7200) {
                            frame_times_.erase(frame_times_.begin(), frame_times_.begin() + 1200);
                        }
                    }
                }
                last_timestamp = timestamp;
                SetError({});
            }
            duplication.output->ReleaseFrame();
        }
        capture_running_ = false;
    }

    void SetError(std::string value) noexcept {
        std::scoped_lock lock{mutex_};
        error_ = std::move(value);
    }

    std::mutex mutex_;
    std::thread capture_thread_;
    std::atomic_bool stop_capture_{false};
    std::atomic_bool capture_running_{false};
    DWORD process_id_{};
    std::vector<FramePoint> frame_times_;
    std::string error_;
};

std::wstring PipeName(DWORD parent_id) {
    return std::format(L"\\\\.\\pipe\\OpenReplay.Telemetry.{}", parent_id);
}

bool WriteResponse(HANDLE pipe, std::string_view response) {
    Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!completed) return false;
    OVERLAPPED operation{};
    operation.hEvent = completed.value;
    DWORD written = 0;
    const auto immediate = WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written,
                                     &operation) != FALSE;
    if (!immediate && GetLastError() != ERROR_IO_PENDING) return false;
    if (!immediate && WaitForSingleObject(completed.value, 1000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        return false;
    }
    if (!immediate && !GetOverlappedResult(pipe, &operation, &written, FALSE)) return false;
    if (written != response.size()) return false;
    return FlushFileBuffers(pipe) != FALSE;
}

int Run(DWORD parent_id) {
    Handle parent{OpenProcess(SYNCHRONIZE, FALSE, parent_id)};
    if (!parent) return 2;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &descriptor, nullptr)) return 3;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    FrameCapture capture;
    bool shutdown = false;
    const auto pipe_name = PipeName(parent_id);
    while (!shutdown) {
        Handle pipe{CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                     1, 64 * 1024, 4096, 0, &security)};
        if (!pipe) break;
        Handle connected_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED connected{};
        connected.hEvent = connected_event.value;
        const auto connected_now = ConnectNamedPipe(pipe.value, &connected) != FALSE;
        const auto connect_error = connected_now ? ERROR_SUCCESS : GetLastError();
        if (connected_now || connect_error == ERROR_PIPE_CONNECTED) SetEvent(connected_event.value);
        else if (connect_error != ERROR_IO_PENDING) break;
        HANDLE waits[]{parent.value, connected_event.value};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
            CancelIoEx(pipe.value, &connected);
            break;
        }

        std::array<char, 4096> request_buffer{};
        Handle request_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED request_operation{};
        request_operation.hEvent = request_event.value;
        DWORD request_size = 0;
        const auto read_now = ReadFile(pipe.value, request_buffer.data(), static_cast<DWORD>(request_buffer.size() - 1),
                                       &request_size, &request_operation) != FALSE;
        const auto read_error = read_now ? ERROR_SUCCESS : GetLastError();
        if (read_now || read_error == ERROR_MORE_DATA) SetEvent(request_event.value);
        else if (read_error != ERROR_IO_PENDING) continue;
        HANDLE read_waits[]{parent.value, request_event.value};
        if (WaitForMultipleObjects(2, read_waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
            CancelIoEx(pipe.value, &request_operation);
            shutdown = true;
            continue;
        }
        if (!read_now && read_error != ERROR_MORE_DATA &&
            !GetOverlappedResult(pipe.value, &request_operation, &request_size, FALSE)) continue;
        if (request_size) {
            const std::string_view request{request_buffer.data(), request_size};
            if (request.starts_with("target:")) {
                DWORD target = 0;
                const auto value = request.substr(7);
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), target);
                if (parsed.ec == std::errc{}) capture.Target(target);
                WriteResponse(pipe.value, capture.Snapshot());
            } else if (request == "sample") {
                WriteResponse(pipe.value, capture.Snapshot());
            } else if (request == "pause") {
                capture.Stop();
                WriteResponse(pipe.value, "ok");
            } else if (request == "shutdown") {
                shutdown = true;
                capture.Stop();
                WriteResponse(pipe.value, "ok");
            } else {
                WriteResponse(pipe.value, "error\nerror=Unknown command");
            }
        }
        DisconnectNamedPipe(pipe.value);
    }
    capture.Stop();
    LocalFree(descriptor);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto parent = ParentProcessId();
    return parent ? Run(*parent) : 1;
}

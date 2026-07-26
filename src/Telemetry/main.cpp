#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <sddl.h>
#include <shellapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
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
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

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
            if (process_id_ == process_id && session_) return true;
        }
        Stop();

        try {
            if (!GraphicsCaptureSession::IsSupported()) {
                SetError("Windows Graphics Capture is unavailable");
                return false;
            }
            const auto window = TargetWindow(process_id);
            if (!window) {
                SetError("Target window is unavailable");
                return false;
            }

            winrt::com_ptr<ID3D11Device> d3d_device;
            auto device_result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                   D3D11_SDK_VERSION, d3d_device.put(), nullptr, nullptr);
            if (FAILED(device_result)) {
                device_result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                  D3D11_SDK_VERSION, d3d_device.put(), nullptr, nullptr);
            }
            winrt::check_hresult(device_result);
            const auto dxgi_device = d3d_device.as<IDXGIDevice>();
            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
            device_ = inspectable.as<IDirect3DDevice>();

            const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(),
                                                          winrt::put_abi(item_)));
            const auto size = item_.Size();
            if (size.Width <= 0 || size.Height <= 0) {
                SetError("Target window has no capture area");
                Stop();
                return false;
            }

            frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            const auto generation = generation_.fetch_add(1) + 1;
            frame_arrived_ = frame_pool_.FrameArrived(
                [this, generation](const Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) noexcept {
                    OnFrame(sender, generation);
                });
            session_ = frame_pool_.CreateCaptureSession(item_);
            {
                std::scoped_lock lock{mutex_};
                process_id_ = process_id;
                frame_times_.clear();
                last_timestamp_.reset();
                error_.clear();
            }
            session_.StartCapture();
            return true;
        } catch (const winrt::hresult_error& error) {
            SetError(winrt::to_string(error.message()));
        } catch (...) {
            SetError("Unable to start window frame capture");
        }
        StopCaptureObjects();
        return false;
    }

    void Stop() noexcept {
        StopCaptureObjects();
        std::scoped_lock lock{mutex_};
        process_id_ = 0;
        frame_times_.clear();
        last_timestamp_.reset();
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
                           "history={}\nerror={}\nsource=Windows Graphics Capture",
                           process_id, fps, low(0.01), low(0.001), frame_time, history, error);
    }

private:
    void OnFrame(const Direct3D11CaptureFramePool& sender, std::uint64_t generation) noexcept {
        try {
            for (;;) {
                const auto frame = sender.TryGetNextFrame();
                if (!frame) break;
                const auto timestamp = frame.SystemRelativeTime().count();
                std::scoped_lock lock{mutex_};
                if (generation != generation_.load()) return;
                if (last_timestamp_ && timestamp > *last_timestamp_) {
                    const auto milliseconds = static_cast<double>(timestamp - *last_timestamp_) / 10000.0;
                    if (milliseconds > 0.05 && milliseconds <= 1000.0) {
                        frame_times_.push_back({Clock::now(), milliseconds});
                        if (frame_times_.size() > 7200) frame_times_.erase(frame_times_.begin(), frame_times_.begin() + 1200);
                    }
                }
                last_timestamp_ = timestamp;
            }
        } catch (...) {
            SetError("Window frame capture stopped");
        }
    }

    void StopCaptureObjects() noexcept {
        generation_.fetch_add(1);
        try {
            if (frame_pool_ && frame_arrived_.value) frame_pool_.FrameArrived(frame_arrived_);
            frame_arrived_ = {};
            if (session_) session_.Close();
            if (frame_pool_) frame_pool_.Close();
        } catch (...) {
        }
        session_ = nullptr;
        frame_pool_ = nullptr;
        item_ = nullptr;
        device_ = nullptr;
    }

    void SetError(std::string value) noexcept {
        std::scoped_lock lock{mutex_};
        error_ = std::move(value);
    }

    std::mutex mutex_;
    std::atomic_uint64_t generation_{};
    IDirect3DDevice device_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool frame_pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_arrived_{};
    DWORD process_id_{};
    std::optional<std::int64_t> last_timestamp_;
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

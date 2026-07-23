#include "openreplay/Ipc.h"

#include "openreplay/SettingsStore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

namespace openreplay {
namespace {

DWORD RemainingTimeout(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(remaining, MAXDWORD - 1ULL));
}

bool CompleteOverlapped(HANDLE pipe, OVERLAPPED& operation,
                        std::chrono::steady_clock::time_point deadline, DWORD& transferred) noexcept {
    const auto timeout = RemainingTimeout(deadline);
    if (WaitForSingleObject(operation.hEvent, timeout) == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    }
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(operation.hEvent, INFINITE);
    return false;
}

bool OverlappedSucceeded(BOOL immediate, HANDLE pipe, OVERLAPPED& operation,
                         std::chrono::steady_clock::time_point deadline, DWORD& transferred) noexcept {
    if (immediate) return true;
    return GetLastError() == ERROR_IO_PENDING && CompleteOverlapped(pipe, operation, deadline, transferred);
}

}  // namespace

Command ParseCommand(std::string_view value) {
    if (value == "status") return {CommandType::QueryStatus};
    if (value == "save_replay") return {CommandType::SaveReplay};
    constexpr std::string_view replay_prefix{"save_replay:"};
    if (value.starts_with(replay_prefix)) {
        std::uint32_t seconds = 0;
        const auto duration = value.substr(replay_prefix.size());
        const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
        if (parsed.ec == std::errc{} && parsed.ptr == duration.data() + duration.size() && seconds > 0) {
            return {CommandType::SaveReplay, false, seconds};
        }
        return {};
    }
    if (value == "toggle_recording") return {CommandType::ToggleRecording};
    if (value == "screenshot") return {CommandType::Screenshot};
    if (value == "reload_settings") return {CommandType::ReloadSettings};
    if (value == "reload_audio_sources") return {CommandType::ReloadAudioSources};
    if (value == "reload_replay_outputs") return {CommandType::ReloadReplayOutputs};
    if (value == "shutdown") return {CommandType::Shutdown};
    if (value == "replay:on") return {CommandType::SetReplayEnabled, true};
    if (value == "replay:off") return {CommandType::SetReplayEnabled, false};
    return {};
}

std::string SerializeResponse(const Response& response) {
    std::ostringstream output;
    output << (response.ok ? "ok" : "error") << '\n';
    for (const auto& [key, value] : response.fields) output << key << '=' << PercentEncode(value) << '\n';
    return output.str();
}

Response ParseResponse(std::string_view value) {
    Response response;
    std::istringstream input{std::string{value}};
    std::string line;
    if (std::getline(input, line)) response.ok = line == "ok";
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        response.fields.emplace(line.substr(0, separator), PercentDecode(line.substr(separator + 1)));
    }
    return response;
}

Response StatusResponse(const CaptureStatus& status) {
    Response response{true};
    response.fields["replay_state"] = std::string{ToString(status.replay_state)};
    response.fields["replay_requested"] = status.replay_requested ? "true" : "false";
    response.fields["recording"] = status.recording ? "true" : "false";
    response.fields["recording_seconds"] = std::to_string(status.recording_duration.count());
    response.fields["protected_content_possible"] = status.protected_content_possible ? "true" : "false";
    response.fields["active_encoder"] = status.active_encoder;
    response.fields["last_error"] = status.last_error;
    response.fields["last_output"] = ToUtf8(status.last_output.wstring());
    response.fields["replay_save_in_progress"] = status.replay_save.in_progress ? "true" : "false";
    response.fields["replay_save_revision"] = std::to_string(status.replay_save.revision);
    response.fields["replay_save_output"] = ToUtf8(status.replay_save.output.wstring());
    response.fields["replay_save_error"] = status.replay_save.error;
    response.fields["audio_level_count"] = std::to_string(status.audio_levels.size());
    for (std::size_t index = 0; index < status.audio_levels.size(); ++index) {
        const auto& level = status.audio_levels[index];
        const auto prefix = "audio_level_" + std::to_string(index);
        response.fields[prefix + "_kind"] = level.microphone ? "input" : "output";
        response.fields[prefix + "_device"] = level.device_id;
        response.fields[prefix + "_peak"] = std::to_string(level.peak);
    }
    return response;
}

Response PipeClient::Request(std::string_view command, std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto connection_timeout = RemainingTimeout(deadline);
    if (connection_timeout == 0 || !WaitNamedPipeW(kPipeName, connection_timeout)) {
        return {false, {{"message", "OpenReplay Host is not available"}}};
    }

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return {false, {{"message", "Unable to connect to OpenReplay Host"}}};

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    OVERLAPPED write_operation{};
    write_operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    const auto write_started = write_operation.hEvent
        ? WriteFile(pipe, command.data(), static_cast<DWORD>(command.size()), &written, &write_operation)
        : FALSE;
    bool write_ok = write_operation.hEvent &&
                    OverlappedSucceeded(write_started, pipe, write_operation, deadline, written);
    if (write_operation.hEvent) CloseHandle(write_operation.hEvent);
    write_ok = write_ok && written == command.size();
    if (!write_ok) {
        CloseHandle(pipe);
        return {false, {{"message", RemainingTimeout(deadline) == 0
            ? "OpenReplay Host command timed out" : "Unable to send command to OpenReplay Host"}}};
    }

    std::array<char, 8192> buffer{};
    OVERLAPPED read_operation{};
    read_operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD read = 0;
    const auto read_started = read_operation.hEvent
        ? ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, &read_operation)
        : FALSE;
    const bool read_ok = read_operation.hEvent &&
                         OverlappedSucceeded(read_started, pipe, read_operation, deadline, read);
    if (read_operation.hEvent) CloseHandle(read_operation.hEvent);
    CloseHandle(pipe);
    if (!read_ok) return {false, {{"message", RemainingTimeout(deadline) == 0
        ? "OpenReplay Host command timed out" : "OpenReplay Host did not return a response"}}};
    return ParseResponse(std::string_view{buffer.data(), read});
}

PipeServer::PipeServer(Handler handler) : handler_(std::move(handler)) {}
PipeServer::~PipeServer() { Stop(); }

void PipeServer::Start() {
    if (worker_.joinable()) return;
    stopping_ = false;
    worker_ = std::thread([this] { Run(); });
}

void PipeServer::Stop() {
    stopping_ = true;
    if (worker_.joinable()) {
        while (worker_running_) {
            CancelSynchronousIo(worker_.native_handle());
            const auto wake_response = PipeClient{}.Request("status", std::chrono::milliseconds{20});
            (void)wake_response;
            if (worker_running_) std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        worker_.join();
    }
}

void PipeServer::Run() {
    worker_running_ = true;
    while (!stopping_) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       1, 8192, 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) break;

        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            std::array<char, 1024> request{};
            DWORD read = 0;
            if (ReadFile(pipe, request.data(), static_cast<DWORD>(request.size() - 1), &read, nullptr)) {
                const auto response = SerializeResponse(handler_(ParseCommand(std::string_view{request.data(), read})));
                DWORD written = 0;
                WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    worker_running_ = false;
}

}  // namespace openreplay

#pragma once

#include "openreplay/Types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>

namespace openreplay {

inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\OpenReplay.Host.v1)";

enum class CommandType {
    QueryStatus,
    SetReplayEnabled,
    SaveReplay,
    ToggleRecording,
    Screenshot,
    ReloadSettings,
    ReloadAudioSources,
    ReloadReplayOutputs,
    Shutdown,
    Unknown,
};

struct Command {
    CommandType type{CommandType::Unknown};
    bool enabled{false};
    std::uint32_t replay_seconds{0};
};

struct Response {
    bool ok{false};
    std::map<std::string, std::string> fields;
};

Command ParseCommand(std::string_view value);
std::string SerializeResponse(const Response& response);
Response ParseResponse(std::string_view value);
Response StatusResponse(const CaptureStatus& status);

class PipeClient {
public:
    [[nodiscard]] Response Request(std::string_view command,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) const;
};

class PipeServer {
public:
    using Handler = std::function<Response(const Command&)>;

    explicit PipeServer(Handler handler);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void Start();
    void Stop();

private:
    void Run();

    Handler handler_;
    std::atomic_bool stopping_{false};
    std::atomic_bool worker_running_{false};
    std::thread worker_;
};

}  // namespace openreplay

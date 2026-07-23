#pragma once

#include "ObsCaptureEngine.h"

#include "openreplay/CaptureStateMachine.h"
#include "openreplay/Ipc.h"
#include "openreplay/SettingsStore.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace openreplay::host {

class CaptureController {
public:
    CaptureController() = default;
    ~CaptureController();
    CaptureController(const CaptureController&) = delete;
    CaptureController& operator=(const CaptureController&) = delete;

    void Start();
    void Stop();
    Response Handle(const Command& command);
    [[nodiscard]] bool stop_requested() const noexcept { return stop_requested_; }

private:
    bool InitializePipeline(std::string& error);
    void ScheduleRecovery(std::string error);
    void RecoveryLoop();
    CaptureStatus StatusLocked();
    Response ErrorResponse(std::string message) const;

    mutable std::mutex mutex_;
    SettingsStore settings_store_;
    Settings settings_;
    CaptureStateMachine replay_state_;
    ObsCaptureEngine engine_;
    std::string pipeline_error_;
    std::filesystem::path last_output_;
    std::chrono::steady_clock::time_point recording_started_{};
    std::chrono::steady_clock::time_point retry_at_{};
    std::atomic_bool running_{false};
    std::atomic_bool stop_requested_{false};
    std::thread recovery_thread_;
    bool pipeline_ready_{false};
};

}  // namespace openreplay::host

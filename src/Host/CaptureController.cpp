#include "CaptureController.h"

#include <exception>
#include <thread>
#include <utility>

namespace openreplay::host {

CaptureController::~CaptureController() { Stop(); }

void CaptureController::Start() {
    std::scoped_lock lock{mutex_};
    if (running_) return;

    settings_ = settings_store_.Load();
    replay_state_.SetDesired(settings_.instant_replay_enabled);
    std::string error;
    if (!InitializePipeline(error) && replay_state_.desired()) ScheduleRecovery(std::move(error));

    stop_requested_ = false;
    running_ = true;
    recovery_thread_ = std::thread([this] { RecoveryLoop(); });
}

void CaptureController::Stop() {
    running_ = false;
    if (recovery_thread_.joinable()) recovery_thread_.join();

    std::scoped_lock lock{mutex_};
    try {
        engine_.Shutdown();
    } catch (const std::exception& exception) {
        WriteHostLog(std::string{"Capture shutdown failed: "} + exception.what());
    } catch (...) {
        WriteHostLog("Capture shutdown failed with an unknown exception");
    }
    pipeline_ready_ = false;
}

Response CaptureController::Handle(const Command& command) {
    std::scoped_lock lock{mutex_};
    try {
        switch (command.type) {
        case CommandType::QueryStatus:
            return StatusResponse(StatusLocked());

        case CommandType::SetReplayEnabled: {
            auto persisted = settings_store_.Load();
            persisted.instant_replay_enabled = command.enabled;
            settings_store_.Save(persisted);
            settings_.instant_replay_enabled = command.enabled;
            replay_state_.SetDesired(command.enabled);
            if (!command.enabled) {
                engine_.StopReplay();
                pipeline_error_.clear();
                return StatusResponse(StatusLocked());
            }

            std::string error;
            if (!pipeline_ready_ && !InitializePipeline(error)) {
                ScheduleRecovery(error);
                return ErrorResponse(std::move(error));
            }
            if (!engine_.StartReplay(error)) {
                ScheduleRecovery(error);
                return ErrorResponse(std::move(error));
            }
            replay_state_.MarkRunning();
            pipeline_error_.clear();
            return StatusResponse(StatusLocked());
        }

        case CommandType::SaveReplay: {
            std::string error;
            std::filesystem::path output;
            if (!engine_.SaveReplay(command.replay_seconds, output, error)) return ErrorResponse(std::move(error));
            last_output_ = std::move(output);
            auto response = StatusResponse(StatusLocked());
            response.fields["message"] = "Replay save started";
            return response;
        }

        case CommandType::ToggleRecording: {
            std::string error;
            std::filesystem::path output;
            if (engine_.RecordingActive()) {
                if (!engine_.StopRecording(output, error)) return ErrorResponse(std::move(error));
                recording_started_ = {};
            } else {
                if (!pipeline_ready_ && !InitializePipeline(error)) return ErrorResponse(std::move(error));
                if (!engine_.StartRecording(output, error)) return ErrorResponse(std::move(error));
                recording_started_ = std::chrono::steady_clock::now();
            }
            last_output_ = std::move(output);
            return StatusResponse(StatusLocked());
        }

        case CommandType::Screenshot: {
            std::string error;
            std::filesystem::path output;
            if (!pipeline_ready_ && !InitializePipeline(error)) return ErrorResponse(std::move(error));
            if (!engine_.Screenshot(output, error)) return ErrorResponse(std::move(error));
            last_output_ = std::move(output);
            auto response = StatusResponse(StatusLocked());
            response.fields["message"] = "Screenshot saved";
            return response;
        }

        case CommandType::ReloadSettings: {
            if (engine_.RecordingActive()) return ErrorResponse("Stop recording before reloading capture settings");
            if (engine_.ReplaySaveState().in_progress) {
                return ErrorResponse("Wait for the current replay save to finish before changing capture settings");
            }
            settings_ = settings_store_.Load();
            WriteHostLog("Applying a full capture pipeline settings reload");
            replay_state_.SetDesired(settings_.instant_replay_enabled);
            std::string error;
            if (!InitializePipeline(error)) {
                if (replay_state_.desired()) ScheduleRecovery(error);
                return ErrorResponse(std::move(error));
            }
            return StatusResponse(StatusLocked());
        }

        case CommandType::ReloadAudioSources: {
            if (engine_.RecordingActive()) return ErrorResponse("Stop recording before changing audio devices");
            if (engine_.ReplaySaveState().in_progress) {
                return ErrorResponse("Wait for the current replay save to finish before changing audio devices");
            }
            auto updated = settings_store_.Load();
            WriteHostLog("Applying an audio-source-only settings reload");
            if (!CapturePipelineCoreSettingsEqual(settings_, updated) ||
                !ReplayOutputSettingsEqual(settings_, updated)) {
                return ErrorResponse("Other capture settings require a full pipeline reload");
            }
            std::string error;
            if (!pipeline_ready_) {
                settings_ = std::move(updated);
                if (!InitializePipeline(error)) {
                    if (replay_state_.desired()) ScheduleRecovery(error);
                    return ErrorResponse(std::move(error));
                }
                return StatusResponse(StatusLocked());
            }
            if (!engine_.ReloadAudioSources(updated, error)) return ErrorResponse(std::move(error));
            settings_ = std::move(updated);
            pipeline_error_.clear();
            return StatusResponse(StatusLocked());
        }

        case CommandType::ReloadReplayOutputs: {
            if (engine_.RecordingActive()) {
                return ErrorResponse("Stop recording before changing replay settings");
            }
            auto updated = settings_store_.Load();
            WriteHostLog("Applying a replay-output-only settings reload");
            if (!CapturePipelineSettingsEqual(settings_, updated)) {
                return ErrorResponse("Capture settings require a full pipeline reload");
            }
            replay_state_.SetDesired(updated.instant_replay_enabled);
            std::string error;
            if (!pipeline_ready_) {
                settings_ = std::move(updated);
                if (!InitializePipeline(error)) {
                    if (replay_state_.desired()) ScheduleRecovery(error);
                    return ErrorResponse(std::move(error));
                }
                return StatusResponse(StatusLocked());
            }
            if (!engine_.ReloadReplayOutputs(updated, replay_state_.desired(), error)) {
                if (replay_state_.desired() && !engine_.ReplayActive()) ScheduleRecovery(error);
                return ErrorResponse(std::move(error));
            }
            settings_ = std::move(updated);
            pipeline_error_.clear();
            if (replay_state_.desired()) replay_state_.MarkRunning();
            return StatusResponse(StatusLocked());
        }

        case CommandType::Shutdown:
            stop_requested_ = true;
            return {true, {{"message", "OpenReplay Host is shutting down"}}};

        case CommandType::Unknown:
            return ErrorResponse("Unknown IPC command");
        }
    } catch (const std::exception& exception) {
        WriteHostLog(std::string{"IPC command failed with an exception: "} + exception.what());
        return ErrorResponse(exception.what());
    } catch (...) {
        WriteHostLog("IPC command failed with an unknown exception");
        return ErrorResponse("Unknown Host command failure");
    }
    return ErrorResponse("Unhandled IPC command");
}

bool CaptureController::InitializePipeline(std::string& error) {
    pipeline_ready_ = false;
    if (!engine_.Initialize(settings_, error)) {
        pipeline_error_ = error;
        return false;
    }
    pipeline_ready_ = true;
    pipeline_error_.clear();

    if (!replay_state_.desired()) return true;
    replay_state_.MarkAttemptStarted();
    if (!engine_.StartReplay(error)) {
        pipeline_error_ = error;
        pipeline_ready_ = false;
        engine_.Shutdown();
        return false;
    }
    replay_state_.MarkRunning();
    return true;
}

void CaptureController::ScheduleRecovery(std::string error) {
    WriteHostLog("Capture recovery scheduled: " + error);
    pipeline_error_ = error;
    replay_state_.MarkTransientFailure(std::move(error));
    retry_at_ = std::chrono::steady_clock::now() + replay_state_.retry_delay();
}

void CaptureController::RecoveryLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        try {
            std::scoped_lock lock{mutex_};
            if (!running_ || !replay_state_.desired()) continue;

            if (replay_state_.state() == CaptureState::Running && !engine_.ReplayActive()) {
                if (engine_.RecordingActive() || engine_.ReplaySaveState().in_progress) continue;
                std::string error;
                if (engine_.ReloadReplayOutputs(settings_, true, error)) {
                    pipeline_error_.clear();
                    replay_state_.MarkRunning();
                    WriteHostLog("Replay outputs recovered without restarting the capture pipeline");
                    continue;
                }
                pipeline_ready_ = false;
                ScheduleRecovery("The replay output stopped unexpectedly and could not be restored: " + error);
                continue;
            }
            if (replay_state_.state() != CaptureState::Recovering ||
                std::chrono::steady_clock::now() < retry_at_) {
                continue;
            }

            replay_state_.MarkRetryDue();
            std::string error;
            if (!InitializePipeline(error)) ScheduleRecovery(std::move(error));
        } catch (const std::exception& exception) {
            WriteHostLog(std::string{"Capture recovery failed with an exception: "} + exception.what());
            std::scoped_lock lock{mutex_};
            if (running_ && replay_state_.desired()) ScheduleRecovery(exception.what());
        } catch (...) {
            WriteHostLog("Capture recovery failed with an unknown exception");
            std::scoped_lock lock{mutex_};
            if (running_ && replay_state_.desired()) ScheduleRecovery("Unknown capture recovery failure");
        }
    }
}

CaptureStatus CaptureController::StatusLocked() {
    CaptureStatus status;
    status.replay_state = replay_state_.state();
    status.replay_requested = replay_state_.desired();
    status.recording = engine_.RecordingActive();
    if (status.recording && recording_started_ != std::chrono::steady_clock::time_point{}) {
        status.recording_duration =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - recording_started_);
    }
    status.active_encoder = engine_.ActiveEncoder();
    status.last_error = pipeline_error_;
    status.last_output = last_output_;
    status.replay_save = engine_.ReplaySaveState();
    status.audio_levels = engine_.AudioLevels();
    return status;
}

Response CaptureController::ErrorResponse(std::string message) const {
    return {false, {{"message", std::move(message)},
                    {"replay_requested", replay_state_.desired() ? "true" : "false"},
                    {"replay_state", std::string{ToString(replay_state_.state())}}}};
}

}  // namespace openreplay::host

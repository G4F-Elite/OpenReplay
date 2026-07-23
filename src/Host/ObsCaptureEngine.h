#pragma once

#include "ICaptureEngine.h"
#include "ObsApi.h"
#include "ScreenshotService.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openreplay::host {

class ObsCaptureEngine final : public ICaptureEngine {
public:
    struct Monitor {
        std::string id;
        RECT area{};
        bool primary{false};
    };

    ~ObsCaptureEngine() override;

    bool Initialize(const Settings& settings, std::string& error) override;
    bool ReloadAudioSources(const Settings& settings, std::string& error) override;
    bool ReloadReplayOutputs(const Settings& settings, bool start_replay, std::string& error) override;
    void Shutdown() noexcept override;
    bool StartReplay(std::string& error) override;
    void StopReplay() noexcept override;
    bool ReplayActive() const noexcept override;
    bool SaveReplay(std::uint32_t replay_seconds, std::filesystem::path& output, std::string& error) override;
    ReplaySaveStatus ReplaySaveState() override;
    bool StartRecording(std::filesystem::path& output, std::string& error) override;
    bool StopRecording(std::filesystem::path& output, std::string& error) override;
    bool RecordingActive() const noexcept override;
    bool Screenshot(std::filesystem::path& output, std::string& error) override;
    std::string ActiveEncoder() const override { return active_encoder_; }
    std::vector<AudioLevel> AudioLevels() const override;

private:
    bool LoadModule(std::wstring_view name, bool required, std::string& error);
    bool CreateSources(std::string& error);
    bool CreateAudioSources(std::string& error);
    bool CreateEncoders(std::string& error);
    bool CreateReplayOutputs(std::string& error);
    bool TryVideoEncoder(std::string_view id, std::string& error);
    obs_data_t* CreateVideoSettings(std::string_view encoder_id);
    void AttachEncoders(obs_output_t* output);
    void StopOutput(obs_output_t* output) noexcept;
    void ReleasePipeline() noexcept;
    static std::vector<Monitor> EnumerateMonitors();
    static std::filesystem::path RecordingPath(const std::filesystem::path& directory, OutputFormat format);
    std::string OutputError(obs_output_t* output, std::string_view fallback) const;
    static void ReplaySaved(void* context, calldata_t* data) noexcept;
    static void AudioLevelUpdated(void* context, const float* magnitude, const float* peak,
                                  const float* input_peak) noexcept;
    struct AudioSource {
        obs_source_t* source{};
        obs_volmeter_t* meter{};
        bool microphone{false};
        std::string device_id;
        std::atomic<float> peak{0.0F};
    };
    struct ReplayOutput {
        ObsCaptureEngine* owner{};
        std::uint32_t replay_seconds{};
        obs_output_t* output{};
        signal_handler_t* signal_handler{};
    };

    void DisconnectReplaySignal(ReplayOutput& replay) noexcept;
    void ReleaseReplayOutputs(bool mark_interrupted) noexcept;
    void ReleaseAudioSources() noexcept;
    bool AttachAudioMeter(AudioSource& source, std::string& error);
    void ReleaseAudioSource(AudioSource& source) noexcept;

    ObsApi api_;
    Settings settings_;
    Monitor monitor_;
    obs_source_t* display_{};
    std::vector<std::unique_ptr<AudioSource>> desktop_audio_sources_;
    std::vector<std::unique_ptr<AudioSource>> microphone_sources_;
    obs_encoder_t* video_encoder_{};
    std::array<obs_encoder_t*, 3> audio_encoders_{};
    std::vector<std::unique_ptr<ReplayOutput>> replay_outputs_;
    std::vector<std::unique_ptr<ReplayOutput>> retired_replay_outputs_;
    obs_output_t* recording_output_{};
    std::filesystem::path recording_path_;
    std::string active_encoder_;
    bool obs_started_{false};
    ScreenshotService screenshot_service_;
    std::mutex replay_save_mutex_;
    ReplaySaveStatus replay_save_status_;
    std::chrono::steady_clock::time_point replay_save_started_{};
    std::uint32_t replay_save_seconds_{};
};

}  // namespace openreplay::host

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openreplay {

enum class CaptureState { Disabled, Starting, Running, Degraded, Recovering, WaitingForDisplay, Failed };
enum class EncoderVendor { Auto, Nvidia, Amd, Intel, Software };
enum class VideoCodec { H264, Hevc, Av1 };
enum class QualityPreset { Performance, Balanced, HighQuality, Custom };
enum class OutputFormat { Mkv, Mp4 };
enum class PerformanceOverlayPosition { TopRight, BottomRight };

struct ReplayHotkey {
    bool enabled{true};
    std::string chord;
    std::uint32_t replay_seconds{15};

    bool operator==(const ReplayHotkey&) const = default;
};

struct AudioDeviceConfig {
    std::string id;
    std::string name;
    std::uint32_t volume_percent{100};

    bool operator==(const AudioDeviceConfig&) const = default;
};

struct Settings {
    bool instant_replay_enabled{true};
    bool start_with_windows{false};
    bool automatic_updates{true};
    bool capture_cursor{true};
    bool microphone_enabled{true};
    std::uint32_t replay_seconds{60};
    std::uint32_t replay_max_megabytes{2048};
    std::uint32_t fps{60};
    std::uint32_t bitrate_kbps{24000};
    std::uint32_t quality_level{22};
    std::string language{"en-US"};
    std::string monitor_id;
    std::vector<AudioDeviceConfig> desktop_audio_devices{{"default", {}, 100}};
    std::vector<AudioDeviceConfig> microphone_devices{{"default", {}, 100}};
    std::vector<ReplayHotkey> replay_hotkeys{
        {true, "Ctrl+F10", 15},
        {true, "Ctrl+F11", 30},
    };
    bool screenshot_hotkey_enabled{true};
    std::string screenshot_hotkey_chord{"Ctrl+F12"};
    EncoderVendor encoder{EncoderVendor::Auto};
    VideoCodec codec{VideoCodec::H264};
    QualityPreset quality_preset{QualityPreset::Balanced};
    OutputFormat output_format{OutputFormat::Mp4};
    PerformanceOverlayPosition performance_overlay_position{PerformanceOverlayPosition::TopRight};
    bool performance_show_gpu_usage{true};
    bool performance_show_gpu_temperature{true};
    bool performance_show_gpu_clock{true};
    bool performance_show_gpu_memory{true};
    bool performance_show_cpu_usage{true};
    bool performance_show_memory{true};
    std::uint32_t performance_overlay_opacity{92};
    std::filesystem::path output_directory;

    void Normalize();
    bool operator==(const Settings&) const = default;
};

struct AudioLevel {
    bool microphone{false};
    std::string device_id;
    float peak{0.0F};
};

struct ReplaySaveStatus {
    bool in_progress{false};
    std::uint64_t revision{0};
    std::filesystem::path output;
    std::string error;
};

struct CaptureStatus {
    CaptureState replay_state{CaptureState::Disabled};
    bool replay_requested{false};
    bool recording{false};
    bool protected_content_possible{false};
    std::chrono::seconds recording_duration{0};
    std::string active_encoder;
    std::string last_error;
    std::filesystem::path last_output;
    ReplaySaveStatus replay_save;
    std::vector<AudioLevel> audio_levels;
};

std::string_view ToString(CaptureState value) noexcept;
std::string_view ToString(EncoderVendor value) noexcept;
std::string_view ToString(VideoCodec value) noexcept;
std::string_view ToString(QualityPreset value) noexcept;
std::string_view ToString(OutputFormat value) noexcept;
std::string_view ToString(PerformanceOverlayPosition value) noexcept;
EncoderVendor ParseEncoderVendor(std::string_view value);
VideoCodec ParseVideoCodec(std::string_view value);
QualityPreset ParseQualityPreset(std::string_view value);
OutputFormat ParseOutputFormat(std::string_view value);
PerformanceOverlayPosition ParsePerformanceOverlayPosition(std::string_view value);
bool ReconcileAudioDevices(std::vector<AudioDeviceConfig>& selected,
                            const std::vector<AudioDeviceConfig>& available,
                            bool fallback_to_default = true);
bool CapturePipelineCoreSettingsEqual(const Settings& left, const Settings& right) noexcept;
bool AudioSourceSettingsEqual(const Settings& left, const Settings& right) noexcept;
bool CapturePipelineSettingsEqual(const Settings& left, const Settings& right) noexcept;
bool ReplayOutputSettingsEqual(const Settings& left, const Settings& right);
std::uint64_t EstimateReplaySizeBytes(const Settings& settings) noexcept;

}  // namespace openreplay

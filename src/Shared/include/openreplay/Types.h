#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <algorithm>
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
// Keep the first four values stable: they are persisted in settings.conf.
enum class FilterPreset { Custom, Crisp, VividHdr, Cinema, Default };

struct VideoFilters {
    bool enabled{false};
    // Full-screen preview adds another compositing pass. Keep it opt-in so
    // recording filters do not unexpectedly reduce game FPS.
    bool show_in_game{false};
    FilterPreset preset{FilterPreset::Default};
    int sharpness{0};
    int hdr{0};
    int contrast{0};
    int saturation{0};
    int brightness{0};
    int warmth{0};

    void Normalize() noexcept;
    void ApplyPreset(FilterPreset value) noexcept;
    bool operator==(const VideoFilters&) const = default;
};

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
    bool developer_updates{false};
    bool discord_auto_send{false};
    bool capture_cursor{true};
    VideoFilters filters;
    bool microphone_enabled{true};
    std::uint32_t replay_seconds{60};
    std::uint32_t replay_max_megabytes{2048};
    std::uint32_t fps{60};
    std::uint32_t bitrate_kbps{24000};
    std::uint32_t quality_level{22};
    std::uint32_t discord_target_megabytes{8};
    std::string language{"en-US"};
    std::string monitor_id;
    std::vector<AudioDeviceConfig> desktop_audio_devices{{"default", {}, 100}};
    std::vector<AudioDeviceConfig> microphone_devices{{"default", {}, 100}};
    std::vector<ReplayHotkey> replay_hotkeys{
        {true, "Alt+F10", 15},
        {true, "Alt+F11", 30},
    };
    bool screenshot_hotkey_enabled{true};
    std::string screenshot_hotkey_chord{"Alt+F12"};
    bool recording_hotkey_enabled{true};
    std::string recording_hotkey_chord{"Alt+F9"};
    EncoderVendor encoder{EncoderVendor::Auto};
    VideoCodec codec{VideoCodec::H264};
    QualityPreset quality_preset{QualityPreset::Balanced};
    OutputFormat output_format{OutputFormat::Mp4};
    PerformanceOverlayPosition performance_overlay_position{PerformanceOverlayPosition::TopRight};
    bool performance_show_fps{true};
    bool performance_show_fps_lows{true};
    bool performance_show_frametime{true};
    bool performance_show_frametime_graph{true};
    bool performance_frametime_line{false};
    bool performance_show_gpu_usage{true};
    bool performance_show_gpu_temperature{true};
    bool performance_show_gpu_clock{true};
    bool performance_show_gpu_power{true};
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

    [[nodiscard]] bool HasAudioSignal() const noexcept {
        return std::ranges::any_of(audio_levels, [](const AudioLevel& level) {
            return level.peak > 0.01F;
        });
    }
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
std::uint32_t DiscordVideoBitrate(std::chrono::milliseconds duration,
                                  std::uint32_t target_megabytes,
                                  std::uint32_t audio_bitrate = 128000) noexcept;
// A window title made safe for a file name: no reserved characters or trademark signs, at most 60 characters.
std::wstring GameFileName(std::wstring_view title);
// The game a clip was saved from, read back from its file name; empty when it has none.
std::wstring ClipGameName(const std::filesystem::path& path);

}  // namespace openreplay

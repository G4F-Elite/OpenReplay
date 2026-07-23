#include "openreplay/Types.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace openreplay {
namespace {

constexpr std::size_t kMaximumAudioSources = 5;
constexpr std::size_t kMaximumReplayHotkeys = 8;

std::string Lowercase(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

void NormalizeDevices(std::vector<AudioDeviceConfig>& devices) {
    std::unordered_set<std::string> seen;
    std::erase_if(devices, [&](AudioDeviceConfig& device) {
        device.volume_percent = std::min(device.volume_percent, 100U);
        return device.id.empty() || !seen.emplace(device.id).second;
    });
    if (devices.empty()) devices.push_back({"default", {}, 100});
    if (devices.size() > 1) {
        std::erase_if(devices, [](const AudioDeviceConfig& device) { return device.id == "default"; });
    }
}

std::vector<std::uint32_t> ReplayDurations(const Settings& settings) {
    std::vector<std::uint32_t> durations{settings.replay_seconds};
    for (const auto& hotkey : settings.replay_hotkeys) {
        if (hotkey.enabled) durations.push_back(hotkey.replay_seconds);
    }
    std::ranges::sort(durations);
    durations.erase(std::unique(durations.begin(), durations.end()), durations.end());
    return durations;
}

bool AudioDevicesEqual(const std::vector<AudioDeviceConfig>& left,
                       const std::vector<AudioDeviceConfig>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id ||
            left[index].volume_percent != right[index].volume_percent) return false;
    }
    return true;
}

}  // namespace

void Settings::Normalize() {
    replay_seconds = std::clamp(replay_seconds, 15u, 1200u);
    replay_max_megabytes = std::clamp(replay_max_megabytes, 256u, 16384u);
    fps = fps >= 60 ? 60u : 30u;
    bitrate_kbps = std::clamp(bitrate_kbps, 2500u, 200000u);
    quality_level = std::clamp(quality_level, 1u, 51u);
    switch (quality_preset) {
    case QualityPreset::Performance: quality_level = 25; break;
    case QualityPreset::Balanced: quality_level = 22; break;
    case QualityPreset::HighQuality: quality_level = 18; break;
    case QualityPreset::Custom: break;
    }
    if (language != "ru-RU" && language != "en-US") language = "en-US";

    NormalizeDevices(desktop_audio_devices);
    NormalizeDevices(microphone_devices);
    const auto desktop_limit = microphone_enabled ? kMaximumAudioSources - 1 : kMaximumAudioSources;
    desktop_audio_devices.resize(std::min(desktop_audio_devices.size(), desktop_limit));
    if (microphone_enabled) {
        const auto microphone_limit = kMaximumAudioSources - desktop_audio_devices.size();
        microphone_devices.resize(std::min(microphone_devices.size(), microphone_limit));
    } else {
        microphone_devices.resize(std::min(microphone_devices.size(), kMaximumAudioSources));
    }
    replay_hotkeys.resize(std::min(replay_hotkeys.size(), kMaximumReplayHotkeys));
    for (auto& hotkey : replay_hotkeys) {
        hotkey.replay_seconds = std::clamp(hotkey.replay_seconds, 15U, 1200U);
        if (hotkey.chord.empty()) hotkey.enabled = false;
    }
    if (screenshot_hotkey_chord.empty()) screenshot_hotkey_enabled = false;
    if (recording_hotkey_chord.empty()) recording_hotkey_enabled = false;
    performance_overlay_opacity = std::clamp(performance_overlay_opacity, 55U, 100U);
    if (!performance_show_gpu_usage && !performance_show_gpu_temperature && !performance_show_gpu_clock &&
        !performance_show_gpu_memory && !performance_show_cpu_usage && !performance_show_memory) {
        performance_show_gpu_usage = true;
    }
}

std::string_view ToString(CaptureState value) noexcept {
    switch (value) {
    case CaptureState::Disabled: return "disabled";
    case CaptureState::Starting: return "starting";
    case CaptureState::Running: return "running";
    case CaptureState::Degraded: return "degraded";
    case CaptureState::Recovering: return "recovering";
    case CaptureState::WaitingForDisplay: return "waiting_for_display";
    case CaptureState::Failed: return "failed";
    }
    return "failed";
}

std::string_view ToString(EncoderVendor value) noexcept {
    switch (value) {
    case EncoderVendor::Auto: return "auto";
    case EncoderVendor::Nvidia: return "nvidia";
    case EncoderVendor::Amd: return "amd";
    case EncoderVendor::Intel: return "intel";
    case EncoderVendor::Software: return "software";
    }
    return "auto";
}

std::string_view ToString(VideoCodec value) noexcept {
    switch (value) {
    case VideoCodec::H264: return "h264";
    case VideoCodec::Hevc: return "hevc";
    case VideoCodec::Av1: return "av1";
    }
    return "h264";
}

std::string_view ToString(QualityPreset value) noexcept {
    switch (value) {
    case QualityPreset::Performance: return "performance";
    case QualityPreset::Balanced: return "balanced";
    case QualityPreset::HighQuality: return "high_quality";
    case QualityPreset::Custom: return "custom";
    }
    return "balanced";
}

std::string_view ToString(OutputFormat value) noexcept {
    return value == OutputFormat::Mp4 ? "mp4" : "mkv";
}

std::string_view ToString(PerformanceOverlayPosition value) noexcept {
    return value == PerformanceOverlayPosition::BottomRight ? "bottom_right" : "top_right";
}

EncoderVendor ParseEncoderVendor(std::string_view value) {
    if (value == "nvidia") return EncoderVendor::Nvidia;
    if (value == "amd") return EncoderVendor::Amd;
    if (value == "intel") return EncoderVendor::Intel;
    if (value == "software") return EncoderVendor::Software;
    return EncoderVendor::Auto;
}

VideoCodec ParseVideoCodec(std::string_view value) {
    if (value == "hevc") return VideoCodec::Hevc;
    if (value == "av1") return VideoCodec::Av1;
    return VideoCodec::H264;
}

QualityPreset ParseQualityPreset(std::string_view value) {
    if (value == "performance") return QualityPreset::Performance;
    if (value == "high_quality") return QualityPreset::HighQuality;
    if (value == "custom") return QualityPreset::Custom;
    return QualityPreset::Balanced;
}

OutputFormat ParseOutputFormat(std::string_view value) {
    return value == "mkv" ? OutputFormat::Mkv : OutputFormat::Mp4;
}

PerformanceOverlayPosition ParsePerformanceOverlayPosition(std::string_view value) {
    return value == "bottom_right" ? PerformanceOverlayPosition::BottomRight
                                    : PerformanceOverlayPosition::TopRight;
}

bool ReconcileAudioDevices(std::vector<AudioDeviceConfig>& selected,
                            const std::vector<AudioDeviceConfig>& available,
                            bool fallback_to_default) {
    bool changed = false;
    std::unordered_set<std::string> claimed;
    for (auto& device : selected) {
        if (device.id == "default") {
            const auto remembered_name = Lowercase(device.name);
            const auto replacement = std::ranges::find_if(available, [&](const AudioDeviceConfig& candidate) {
                return !remembered_name.empty() && Lowercase(candidate.name) == remembered_name &&
                       !claimed.contains(candidate.id);
            });
            if (replacement != available.end()) {
                const auto volume = device.volume_percent;
                device = *replacement;
                device.volume_percent = volume;
                claimed.insert(device.id);
                changed = true;
            }
            continue;
        }

        const auto exact = std::ranges::find_if(available, [&](const AudioDeviceConfig& candidate) {
            return candidate.id == device.id;
        });
        if (exact != available.end()) {
            claimed.insert(exact->id);
            if (device.name != exact->name) {
                device.name = exact->name;
                changed = true;
            }
            continue;
        }

        const auto old_name = Lowercase(device.name);
        const auto replacement = std::ranges::find_if(available, [&](const AudioDeviceConfig& candidate) {
            return !old_name.empty() && Lowercase(candidate.name) == old_name && !claimed.contains(candidate.id);
        });
        if (replacement != available.end()) {
            device.id = replacement->id;
            device.name = replacement->name;
            claimed.insert(replacement->id);
        } else if (fallback_to_default) {
            if (claimed.empty()) {
                device.id = "default";
            } else {
                device.id.clear();
            }
        } else {
            continue;
        }
        changed = true;
    }

    const auto before = selected;
    NormalizeDevices(selected);
    return changed || selected != before;
}

bool CapturePipelineCoreSettingsEqual(const Settings& left, const Settings& right) noexcept {
    return left.capture_cursor == right.capture_cursor &&
           left.microphone_enabled == right.microphone_enabled &&
           left.fps == right.fps && left.bitrate_kbps == right.bitrate_kbps &&
           left.quality_level == right.quality_level && left.monitor_id == right.monitor_id &&
           left.encoder == right.encoder && left.codec == right.codec &&
           left.quality_preset == right.quality_preset;
}

bool AudioSourceSettingsEqual(const Settings& left, const Settings& right) noexcept {
    return AudioDevicesEqual(left.desktop_audio_devices, right.desktop_audio_devices) &&
           AudioDevicesEqual(left.microphone_devices, right.microphone_devices);
}

bool CapturePipelineSettingsEqual(const Settings& left, const Settings& right) noexcept {
    return CapturePipelineCoreSettingsEqual(left, right) && AudioSourceSettingsEqual(left, right);
}

bool ReplayOutputSettingsEqual(const Settings& left, const Settings& right) {
    return left.replay_seconds == right.replay_seconds &&
           left.replay_max_megabytes == right.replay_max_megabytes &&
           left.output_format == right.output_format && left.output_directory == right.output_directory &&
           ReplayDurations(left) == ReplayDurations(right);
}

std::uint64_t EstimateReplaySizeBytes(const Settings& settings) noexcept {
    // Every saved file contains the mixed and isolated AAC tracks.
    const std::uint64_t audio_kbps = settings.microphone_enabled ? 704U : 512U;
    const auto payload = (static_cast<std::uint64_t>(settings.bitrate_kbps) + audio_kbps) *
                         settings.replay_seconds * 1000U / 8U;
    return payload + payload / 100U;
}

}  // namespace openreplay

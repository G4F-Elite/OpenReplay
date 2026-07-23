#include "openreplay/SettingsStore.h"

#include <Windows.h>
#include <ShlObj.h>

#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace openreplay {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id, const std::filesystem::path& fallback) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        const std::filesystem::path result{raw};
        CoTaskMemFree(raw);
        return result;
    }
    return fallback;
}

bool ParseBool(const std::map<std::string, std::string>& values, std::string_view key, bool fallback) {
    const auto it = values.find(std::string{key});
    return it == values.end() ? fallback : it->second == "true";
}

std::uint32_t ParseUInt(const std::map<std::string, std::string>& values, std::string_view key,
                        std::uint32_t fallback) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) return fallback;
    std::uint32_t result = fallback;
    const auto parsed = std::from_chars(it->second.data(), it->second.data() + it->second.size(), result);
    return parsed.ec == std::errc{} ? result : fallback;
}

bool HasKey(const std::map<std::string, std::string>& values, std::string_view key) {
    return values.contains(std::string{key});
}

std::string ParseString(const std::map<std::string, std::string>& values, std::string_view key,
                         std::string fallback = {}) {
    const auto it = values.find(std::string{key});
    return it == values.end() ? std::move(fallback) : PercentDecode(it->second);
}

std::vector<AudioDeviceConfig> ParseDeviceList(const std::map<std::string, std::string>& values,
                                                std::string_view prefix, std::string_view legacy_key) {
    std::vector<AudioDeviceConfig> result;
    const auto count = std::min(ParseUInt(values, std::string{prefix} + "_count", 0), 5U);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto item_prefix = std::string{prefix} + '_' + std::to_string(index);
        auto id = ParseString(values, item_prefix + "_id");
        if (id.empty()) id = ParseString(values, item_prefix);
        if (!id.empty()) {
            result.push_back({std::move(id), ParseString(values, item_prefix + "_name"),
                              ParseUInt(values, item_prefix + "_volume", 100)});
        }
    }
    if (result.empty()) result.push_back({ParseString(values, legacy_key, "default"), {}, 100});
    return result;
}

}  // namespace

SettingsStore::SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}

Settings SettingsStore::Load() const {
    Settings result;
    result.output_directory = DefaultOutputDirectory();

    std::ifstream input(path_);
    if (!input) {
        result.Normalize();
        return result;
    }

    std::map<std::string, std::string> values;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values.emplace(line.substr(0, separator), line.substr(separator + 1));
    }

    result.instant_replay_enabled = ParseBool(values, "instant_replay_enabled", result.instant_replay_enabled);
    result.start_with_windows = ParseBool(values, "start_with_windows", result.start_with_windows);
    result.automatic_updates = ParseBool(values, "automatic_updates", result.automatic_updates);
    result.capture_cursor = ParseBool(values, "capture_cursor", result.capture_cursor);
    result.microphone_enabled = ParseBool(values, "microphone_enabled", result.microphone_enabled);
    result.replay_seconds = ParseUInt(values, "replay_seconds", result.replay_seconds);
    result.replay_max_megabytes = ParseUInt(values, "replay_max_megabytes", result.replay_max_megabytes);
    result.fps = ParseUInt(values, "fps", result.fps);
    result.bitrate_kbps = ParseUInt(values, "bitrate_kbps", result.bitrate_kbps);
    result.quality_level = ParseUInt(values, "quality_level", result.quality_level);
    result.language = ParseString(values, "language", result.language);
    result.monitor_id = ParseString(values, "monitor_id");
    result.desktop_audio_devices = ParseDeviceList(values, "desktop_audio_device", "desktop_audio_device");
    result.microphone_devices = ParseDeviceList(values, "microphone_device", "microphone_device");
    constexpr std::string_view hotkey_count_key{"replay_hotkey_count"};
    if (HasKey(values, hotkey_count_key)) {
        result.replay_hotkeys.resize(std::min(ParseUInt(values, hotkey_count_key, 0), 8U));
    }
    for (std::size_t index = 0; index < result.replay_hotkeys.size(); ++index) {
        const auto prefix = "replay_hotkey_" + std::to_string(index);
        auto& hotkey = result.replay_hotkeys[index];
        hotkey.enabled = ParseBool(values, prefix + "_enabled", hotkey.enabled);
        hotkey.chord = ParseString(values, prefix + "_chord", hotkey.chord);
        hotkey.replay_seconds = ParseUInt(values, prefix + "_seconds", hotkey.replay_seconds);
    }
    result.screenshot_hotkey_enabled = ParseBool(
        values, "screenshot_hotkey_enabled", result.screenshot_hotkey_enabled);
    result.screenshot_hotkey_chord = ParseString(
        values, "screenshot_hotkey_chord", result.screenshot_hotkey_chord);
    result.recording_hotkey_enabled = ParseBool(
        values, "recording_hotkey_enabled", result.recording_hotkey_enabled);
    result.recording_hotkey_chord = ParseString(
        values, "recording_hotkey_chord", result.recording_hotkey_chord);
    result.encoder = ParseEncoderVendor(ParseString(values, "encoder", "auto"));
    result.codec = ParseVideoCodec(ParseString(values, "codec", "h264"));
    result.quality_preset = ParseQualityPreset(ParseString(values, "quality_preset", "balanced"));
    result.output_format = ParseOutputFormat(ParseString(values, "output_format", "mp4"));
    result.performance_overlay_position = ParsePerformanceOverlayPosition(
        ParseString(values, "performance_overlay_position", "top_right"));
    result.performance_show_gpu_usage = ParseBool(
        values, "performance_show_gpu_usage", result.performance_show_gpu_usage);
    result.performance_show_gpu_temperature = ParseBool(
        values, "performance_show_gpu_temperature", result.performance_show_gpu_temperature);
    result.performance_show_gpu_clock = ParseBool(
        values, "performance_show_gpu_clock", result.performance_show_gpu_clock);
    result.performance_show_gpu_memory = ParseBool(
        values, "performance_show_gpu_memory", result.performance_show_gpu_memory);
    result.performance_show_cpu_usage = ParseBool(
        values, "performance_show_cpu_usage", result.performance_show_cpu_usage);
    result.performance_show_memory = ParseBool(
        values, "performance_show_memory", result.performance_show_memory);
    result.performance_overlay_opacity = ParseUInt(
        values, "performance_overlay_opacity", result.performance_overlay_opacity);
    const auto output = ParseString(values, "output_directory");
    if (!output.empty()) result.output_directory = FromUtf8(output);
    result.Normalize();
    return result;
}

void SettingsStore::Save(const Settings& input) const {
    Settings settings = input;
    settings.Normalize();
    std::filesystem::create_directories(path_.parent_path());

    auto temporary = path_;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to open temporary settings file");
        output << "version=8\n"
               << "instant_replay_enabled=" << (settings.instant_replay_enabled ? "true" : "false") << '\n'
               << "start_with_windows=" << (settings.start_with_windows ? "true" : "false") << '\n'
               << "automatic_updates=" << (settings.automatic_updates ? "true" : "false") << '\n'
               << "capture_cursor=" << (settings.capture_cursor ? "true" : "false") << '\n'
               << "microphone_enabled=" << (settings.microphone_enabled ? "true" : "false") << '\n'
               << "replay_seconds=" << settings.replay_seconds << '\n'
               << "replay_max_megabytes=" << settings.replay_max_megabytes << '\n'
               << "fps=" << settings.fps << '\n'
               << "bitrate_kbps=" << settings.bitrate_kbps << '\n'
               << "quality_level=" << settings.quality_level << '\n'
               << "language=" << PercentEncode(settings.language) << '\n'
               << "monitor_id=" << PercentEncode(settings.monitor_id) << '\n'
               << "desktop_audio_device_count=" << settings.desktop_audio_devices.size() << '\n';
        for (std::size_t index = 0; index < settings.desktop_audio_devices.size(); ++index) {
            const auto& device = settings.desktop_audio_devices[index];
            output << "desktop_audio_device_" << index << "_id=" << PercentEncode(device.id) << '\n'
                   << "desktop_audio_device_" << index << "_name=" << PercentEncode(device.name) << '\n'
                   << "desktop_audio_device_" << index << "_volume=" << device.volume_percent << '\n';
        }
        output << "microphone_device_count=" << settings.microphone_devices.size() << '\n';
        for (std::size_t index = 0; index < settings.microphone_devices.size(); ++index) {
            const auto& device = settings.microphone_devices[index];
            output << "microphone_device_" << index << "_id=" << PercentEncode(device.id) << '\n'
                   << "microphone_device_" << index << "_name=" << PercentEncode(device.name) << '\n'
                   << "microphone_device_" << index << "_volume=" << device.volume_percent << '\n';
        }
        output << "replay_hotkey_count=" << settings.replay_hotkeys.size() << '\n';
        for (std::size_t index = 0; index < settings.replay_hotkeys.size(); ++index) {
            const auto& hotkey = settings.replay_hotkeys[index];
            output << "replay_hotkey_" << index << "_enabled=" << (hotkey.enabled ? "true" : "false") << '\n'
                   << "replay_hotkey_" << index << "_chord=" << PercentEncode(hotkey.chord) << '\n'
                    << "replay_hotkey_" << index << "_seconds=" << hotkey.replay_seconds << '\n';
        }
        output << "screenshot_hotkey_enabled=" << (settings.screenshot_hotkey_enabled ? "true" : "false") << '\n'
               << "screenshot_hotkey_chord=" << PercentEncode(settings.screenshot_hotkey_chord) << '\n'
               << "recording_hotkey_enabled=" << (settings.recording_hotkey_enabled ? "true" : "false") << '\n'
               << "recording_hotkey_chord=" << PercentEncode(settings.recording_hotkey_chord) << '\n'
               << "encoder=" << ToString(settings.encoder) << '\n'
               << "codec=" << ToString(settings.codec) << '\n'
               << "quality_preset=" << ToString(settings.quality_preset) << '\n'
               << "output_format=" << ToString(settings.output_format) << '\n'
               << "performance_overlay_position=" << ToString(settings.performance_overlay_position) << '\n'
               << "performance_show_gpu_usage=" << (settings.performance_show_gpu_usage ? "true" : "false") << '\n'
               << "performance_show_gpu_temperature=" << (settings.performance_show_gpu_temperature ? "true" : "false") << '\n'
               << "performance_show_gpu_clock=" << (settings.performance_show_gpu_clock ? "true" : "false") << '\n'
               << "performance_show_gpu_memory=" << (settings.performance_show_gpu_memory ? "true" : "false") << '\n'
               << "performance_show_cpu_usage=" << (settings.performance_show_cpu_usage ? "true" : "false") << '\n'
               << "performance_show_memory=" << (settings.performance_show_memory ? "true" : "false") << '\n'
               << "performance_overlay_opacity=" << settings.performance_overlay_opacity << '\n'
               << "output_directory=" << PercentEncode(ToUtf8(settings.output_directory.wstring())) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Unable to write settings file");
    }

    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to publish settings file");
    }
}

std::filesystem::path SettingsStore::DefaultPath() {
    return KnownFolder(FOLDERID_LocalAppData, std::filesystem::temp_directory_path()) /
           L"OpenReplay" / L"settings.conf";
}

std::filesystem::path SettingsStore::DefaultOutputDirectory() {
    return KnownFolder(FOLDERID_Videos, std::filesystem::current_path()) / L"OpenReplay";
}

std::string PercentEncode(std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '\\' || character == '/' || character == ':') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(digits[character >> 4]);
            result.push_back(digits[character & 0x0F]);
        }
    }
    return result;
}

std::string PercentDecode(std::string_view value) {
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("Unable to encode UTF-8 string");
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("Unable to decode UTF-8 string");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

}  // namespace openreplay

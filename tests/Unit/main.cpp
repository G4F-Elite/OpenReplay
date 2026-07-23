#include "openreplay/CaptureStateMachine.h"
#include "openreplay/Ipc.h"
#include "openreplay/SettingsStore.h"
#include "openreplay/UpdateTypes.h"
#include "openreplay/Version.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void StateMachinePreservesIntent() {
    openreplay::CaptureStateMachine state;
    state.SetDesired(true);
    Check(state.desired(), "replay intent was not enabled");
    Check(state.state() == openreplay::CaptureState::Starting, "replay did not enter starting state");
    state.MarkRunning();
    state.MarkTransientFailure("protected frame");
    Check(state.desired(), "transient failure disabled replay intent");
    Check(state.state() == openreplay::CaptureState::Recovering, "transient failure did not start recovery");
    state.MarkRetryDue();
    Check(state.state() == openreplay::CaptureState::Starting, "retry did not restart capture");
    state.SetDesired(false);
    Check(state.state() == openreplay::CaptureState::Disabled, "explicit disable was ignored");
}

void SettingsRoundTrip() {
    const auto path = std::filesystem::temp_directory_path() / L"OpenReplay.UnitTests.settings";
    openreplay::SettingsStore store{path};
    openreplay::Settings settings;
    settings.instant_replay_enabled = false;
    settings.start_with_windows = true;
    settings.automatic_updates = false;
    settings.language = "ru-RU";
    settings.monitor_id = "display:1=value";
    settings.output_directory = std::filesystem::temp_directory_path() / L"Open Replay";
    settings.replay_seconds = 90;
    settings.bitrate_kbps = 42000;
    settings.output_format = openreplay::OutputFormat::Mp4;
    settings.performance_overlay_position = openreplay::PerformanceOverlayPosition::BottomRight;
    settings.performance_show_gpu_temperature = false;
    settings.performance_show_gpu_clock = false;
    settings.performance_show_memory = false;
    settings.performance_overlay_opacity = 78;
    settings.desktop_audio_devices = {{"output-one", "Speakers", 75}, {"output-two", "Headset", 20}};
    settings.microphone_devices = {{"input-one", "Desk mic", 90}, {"input-two", "Webcam mic", 40}};
    settings.replay_hotkeys = {{true, "Ctrl+F8", 45}, {false, "Alt+F9", 75}, {true, "Ctrl+F12", 120}};
    settings.screenshot_hotkey_enabled = true;
    settings.screenshot_hotkey_chord = "Ctrl+Shift+F12";
    store.Save(settings);
    const auto loaded = store.Load();
    Check(!loaded.instant_replay_enabled, "boolean setting did not round-trip");
    Check(loaded.start_with_windows, "start with Windows setting did not round-trip");
    Check(!loaded.automatic_updates, "automatic updates setting did not round-trip");
    Check(loaded.monitor_id == settings.monitor_id, "escaped setting did not round-trip");
    Check(loaded.output_directory == settings.output_directory, "path setting did not round-trip");
    Check(loaded.replay_seconds == 90, "numeric setting did not round-trip");
    Check(loaded.bitrate_kbps == 42000, "bitrate setting did not round-trip");
    Check(loaded.output_format == openreplay::OutputFormat::Mp4, "output format did not round-trip");
    Check(loaded.performance_overlay_position == openreplay::PerformanceOverlayPosition::BottomRight,
          "performance overlay position did not round-trip");
    Check(!loaded.performance_show_gpu_temperature && !loaded.performance_show_gpu_clock &&
              !loaded.performance_show_memory && loaded.performance_overlay_opacity == 78,
          "performance overlay settings did not round-trip");
    Check(loaded.desktop_audio_devices == settings.desktop_audio_devices, "desktop audio devices did not round-trip");
    Check(loaded.microphone_devices == settings.microphone_devices, "microphone devices did not round-trip");
    Check(loaded.replay_hotkeys[0].chord == "Ctrl+F8" && loaded.replay_hotkeys[0].replay_seconds == 45,
           "replay hotkey did not round-trip");
    Check(!loaded.replay_hotkeys[1].enabled, "disabled replay hotkey did not round-trip");
    Check(loaded.replay_hotkeys.size() == 3 && loaded.replay_hotkeys[2].replay_seconds == 120,
          "dynamic replay hotkey count did not round-trip");
    Check(loaded.screenshot_hotkey_enabled && loaded.screenshot_hotkey_chord == "Ctrl+Shift+F12",
          "screenshot hotkey did not round-trip");
    std::filesystem::remove(path);
}

void DefaultSettingsUseEnglishAndMp4() {
    openreplay::Settings settings;
    Check(settings.language == "en-US", "English was not the default language");
    Check(settings.output_format == openreplay::OutputFormat::Mp4, "MP4 was not the default output format");
    Check(settings.screenshot_hotkey_enabled && settings.screenshot_hotkey_chord == "Ctrl+F12",
           "screenshot hotkey defaults were not applied");
    Check(settings.automatic_updates, "automatic updates were not enabled by default");
}

void UpdateMetadataParsesAndComparesVersions() {
    const auto current = openreplay::ParseSemanticVersion(openreplay::kVersion);
    const auto newer = openreplay::ParseSemanticVersion("v0.2.0");
    Check(current && newer && *newer > *current, "semantic versions were not compared correctly");
    Check(!openreplay::ParseSemanticVersion("0.2"), "incomplete semantic version was accepted");
    Check(!openreplay::ParseSemanticVersion("0.2.0-beta"), "prerelease version was accepted as stable");

    const auto manifest = openreplay::ParseUpdateManifest(R"({
        "schema": 1,
        "product": "OpenReplay",
        "channel": "stable",
        "version": "0.2.0",
        "tag": "v0.2.0",
        "release_notes_url": "https://github.com/G4F-Elite/OpenReplay/releases/tag/v0.2.0",
        "asset_url": "https://github.com/G4F-Elite/OpenReplay/releases/download/v0.2.0/OpenReplay-0.2.0-win-x64.zip",
        "asset_name": "OpenReplay-0.2.0-win-x64.zip",
        "architecture": "x64",
        "size_bytes": 123456,
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    })");
    Check(manifest.has_value(), "update manifest was not parsed");
    Check(openreplay::IsValidStableUpdate(*manifest, openreplay::kVersion),
          "valid stable update was rejected");
    auto invalid = *manifest;
    invalid.asset_url = "https://example.com/update.zip";
    Check(!openreplay::IsValidStableUpdate(invalid, openreplay::kVersion),
          "untrusted update URL was accepted");
}

void UpdateSignatureFixtureVerifies() {
    const std::string content{"OpenReplay update signature fixture v1"};
    const std::string signature_base64{
        "H7sRp28ws+Tjppu9K3B+5QhlaKDfQV492sp25TCC3u7L69GwrfVdLJqvjTuba/fqnfPDhFwoM1KzxKIvvQWGfYCd0ByQS+APDl+FLg7UHt1p+lKSo4ql6FiL1thAuvB9vy/psiqdOLqDN5OhzdkoElyWCGgsSfpRglOikfPlix5MiVLLvwjcIAX9sAHZS1h5uhxKnO+b0URNoBTMj/p+l8Hx8Mkm3RwgzY7p/gn554BDdqe6ZYtxTsh3/CsN3Sm8JeqeWDxnn1xVSKqLaoFKJNxJbcdw8Fv/FuexAPO9JDXSPAsuzrdMrZBg8GlHoRotWPFBAkkf8oBAAR4Y7T0VQzwlImiYT8WMlncu/VKPIuOTWFCGLKTN/Fuh13ctqErFDSkRrso2ETTX329t65HuzbCnkt89lI22C2Y22yBpQCOUjtCMi/5Pe6Mut2Bk+WRXwLj+lOInmowqS6MLujQRtMbolvGQw4dAqqmadlCS+DaDjfSOZ2dwXh5S6QQN34jl"};
    const auto decode = [](std::string_view value) {
        const auto digit = [](char character) -> int {
            if (character >= 'A' && character <= 'Z') return character - 'A';
            if (character >= 'a' && character <= 'z') return character - 'a' + 26;
            if (character >= '0' && character <= '9') return character - '0' + 52;
            if (character == '+') return 62;
            if (character == '/') return 63;
            return -1;
        };
        std::vector<std::uint8_t> result;
        std::uint32_t accumulator = 0;
        int bits = -8;
        for (const char character : value) {
            if (character == '=') break;
            const int value_digit = digit(character);
            if (value_digit < 0) continue;
            accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value_digit);
            bits += 6;
            if (bits >= 0) {
                result.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU));
                bits -= 8;
            }
        }
        return result;
    };
    const auto signature = decode(signature_base64);
    Check(openreplay::VerifyUpdateSignature(
              {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}, signature),
          "embedded update public key did not verify the release signature fixture");
    auto changed = content;
    changed.back() = '2';
    Check(!openreplay::VerifyUpdateSignature(
              {reinterpret_cast<const std::uint8_t*>(changed.data()), changed.size()}, signature),
          "update signature accepted modified content");
}

void SettingsChangeClassification() {
    openreplay::Settings current;
    auto changed = current;
    changed.language = "ru-RU";
    changed.replay_hotkeys.front().chord = "Ctrl+F8";
    changed.screenshot_hotkey_chord = "Ctrl+Shift+F12";
    Check(openreplay::CapturePipelineSettingsEqual(current, changed),
          "app-only or chord-only changes required a capture pipeline reload");
    Check(openreplay::ReplayOutputSettingsEqual(current, changed),
          "a chord-only change recreated replay outputs");

    changed.replay_hotkeys.front().replay_seconds = 45;
    Check(!openreplay::ReplayOutputSettingsEqual(current, changed),
          "a replay duration change did not recreate replay outputs");
    changed = current;
    changed.bitrate_kbps += 1000;
    Check(!openreplay::CapturePipelineSettingsEqual(current, changed),
          "an encoder bitrate change did not require a capture pipeline reload");

    changed = current;
    changed.microphone_devices.front().volume_percent = 75;
    Check(openreplay::CapturePipelineCoreSettingsEqual(current, changed),
          "an audio-only change modified core capture settings");
    Check(!openreplay::AudioSourceSettingsEqual(current, changed),
          "an audio-only change was not classified for source reload");
}

void LegacyAudioSettingsMigrate() {
    const auto path = std::filesystem::temp_directory_path() / L"OpenReplay.UnitTests.legacy.settings";
    {
        std::ofstream output(path);
        output << "desktop_audio_device=legacy-output\n"
               << "microphone_device=legacy-input\n";
    }
    const auto loaded = openreplay::SettingsStore{path}.Load();
    Check(loaded.desktop_audio_devices == std::vector<openreplay::AudioDeviceConfig>{{"legacy-output", {}, 100}},
          "legacy desktop audio device was not migrated");
    Check(loaded.microphone_devices == std::vector<openreplay::AudioDeviceConfig>{{"legacy-input", {}, 100}},
          "legacy microphone device was not migrated");
    std::filesystem::remove(path);
}

void ProtocolRoundTrip() {
    openreplay::CaptureStatus status;
    status.replay_requested = true;
    status.replay_state = openreplay::CaptureState::Recovering;
    status.last_error = "capture=device unavailable";
    status.replay_save.in_progress = true;
    status.replay_save.revision = 7;
    status.replay_save.output = L"C:\\Captures\\Replay.mkv";
    status.audio_levels = {{false, "output-one", 0.75F}, {true, "input-one", 0.5F}};
    const auto parsed = openreplay::ParseResponse(openreplay::SerializeResponse(openreplay::StatusResponse(status)));
    Check(parsed.ok, "status response was not successful");
    Check(parsed.fields.at("replay_requested") == "true", "status intent was lost");
    Check(parsed.fields.at("last_error") == status.last_error, "status escaping failed");
    Check(parsed.fields.at("replay_save_in_progress") == "true", "replay save state was lost");
    Check(parsed.fields.at("replay_save_revision") == "7", "replay save revision was lost");
    Check(parsed.fields.at("replay_save_output") == "C:\\Captures\\Replay.mkv", "replay path was lost");
    Check(parsed.fields.at("audio_level_count") == "2", "audio level count was lost");
    Check(parsed.fields.at("audio_level_1_device") == "input-one", "audio level device was lost");
}

void ReplayDurationCommandParses() {
    const auto command = openreplay::ParseCommand("save_replay:30");
    Check(command.type == openreplay::CommandType::SaveReplay, "duration replay command type was not parsed");
    Check(command.replay_seconds == 30, "duration replay command value was not parsed");
    Check(openreplay::ParseCommand("save_replay:nope").type == openreplay::CommandType::Unknown,
          "invalid replay duration command was accepted");
    Check(openreplay::ParseCommand("reload_replay_outputs").type == openreplay::CommandType::ReloadReplayOutputs,
          "lightweight replay output reload command was not parsed");
    Check(openreplay::ParseCommand("reload_audio_sources").type == openreplay::CommandType::ReloadAudioSources,
          "lightweight audio source reload command was not parsed");
}

void AudioDevicesNormalize() {
    openreplay::Settings settings;
    settings.desktop_audio_devices = {{"default", {}, 100}, {"output", "Speakers", 120},
                                      {"output", "Duplicate", 50}, {"", {}, 100}};
    settings.microphone_devices = {{"default", {}, 100}, {"input", "Mic", 80}};
    settings.Normalize();
    Check(settings.desktop_audio_devices == std::vector<openreplay::AudioDeviceConfig>{{"output", "Speakers", 100}},
          "desktop audio devices were not normalized");
    Check(settings.microphone_devices == std::vector<openreplay::AudioDeviceConfig>{{"input", "Mic", 80}},
          "microphone devices were not normalized");
}

void AudioDevicesReconcileByNameAndDefault() {
    std::vector<openreplay::AudioDeviceConfig> selected{
        {"old-speakers", "Speakers", 55}, {"missing-mic", "Missing microphone", 35}};
    const std::vector<openreplay::AudioDeviceConfig> available{{"new-speakers", "Speakers", 100}};
    Check(openreplay::ReconcileAudioDevices(selected, available), "audio device reconciliation reported no change");
    Check(selected == std::vector<openreplay::AudioDeviceConfig>{{"new-speakers", "Speakers", 55}},
          "audio device was not rematched by friendly name or fallback was not normalized");

    std::vector<openreplay::AudioDeviceConfig> fallback{{"missing", "Gone", 25}};
    Check(openreplay::ReconcileAudioDevices(fallback, {}), "missing device did not fall back");
    Check(fallback == std::vector<openreplay::AudioDeviceConfig>{{"default", "Gone", 25}},
          "missing audio device did not preserve its name while falling back to Windows default");

    std::vector<openreplay::AudioDeviceConfig> transient{{"missing", "USB microphone", 65}};
    Check(!openreplay::ReconcileAudioDevices(transient, {}, false),
          "temporary endpoint loss changed the saved audio device");
    Check(transient == std::vector<openreplay::AudioDeviceConfig>{{"missing", "USB microphone", 65}},
          "temporary endpoint loss discarded the remembered audio device");

    std::vector<openreplay::AudioDeviceConfig> remembered_default{{"default", "USB microphone", 70}};
    Check(openreplay::ReconcileAudioDevices(
              remembered_default, {{"new-id", "USB microphone", 100}}, false),
          "remembered default device was not restored by friendly name");
    Check(remembered_default == std::vector<openreplay::AudioDeviceConfig>{{"new-id", "USB microphone", 70}},
          "remembered default device did not preserve its volume");
}

void ReplayHotkeysNormalizeToLimit() {
    openreplay::Settings settings;
    settings.replay_hotkeys.assign(10, {true, "Ctrl+F10", 5000});
    settings.Normalize();
    Check(settings.replay_hotkeys.size() == 8, "replay hotkeys were not limited");
    Check(settings.replay_hotkeys.front().replay_seconds == 1200, "replay hotkey duration was not clamped");
}

void ScreenshotHotkeyNormalizes() {
    openreplay::Settings settings;
    settings.screenshot_hotkey_enabled = true;
    settings.screenshot_hotkey_chord.clear();
    settings.Normalize();
    Check(!settings.screenshot_hotkey_enabled, "empty screenshot hotkey remained enabled");
}

void PerformanceOverlaySettingsNormalize() {
    openreplay::Settings settings;
    settings.performance_show_gpu_usage = false;
    settings.performance_show_gpu_temperature = false;
    settings.performance_show_gpu_clock = false;
    settings.performance_show_gpu_memory = false;
    settings.performance_show_cpu_usage = false;
    settings.performance_show_memory = false;
    settings.performance_overlay_opacity = 20;
    settings.Normalize();
    Check(settings.performance_show_gpu_usage, "performance overlay allowed an empty metric list");
    Check(settings.performance_overlay_opacity == 55, "performance overlay opacity was not clamped");
}

void QualityPresetsApplyEncoderLevels() {
    openreplay::Settings settings;
    settings.quality_level = 1;
    settings.quality_preset = openreplay::QualityPreset::Performance;
    settings.Normalize();
    Check(settings.quality_level == 25, "performance preset quality was not applied");
    settings.quality_preset = openreplay::QualityPreset::Balanced;
    settings.Normalize();
    Check(settings.quality_level == 22, "balanced preset quality was not applied");
    settings.quality_preset = openreplay::QualityPreset::HighQuality;
    settings.Normalize();
    Check(settings.quality_level == 18, "high quality preset was not applied");
}

void ReplaySizeEstimateIncludesEncodedAudioAndOverhead() {
    openreplay::Settings settings;
    settings.replay_seconds = 60;
    settings.bitrate_kbps = 24000;
    settings.microphone_enabled = true;
    Check(openreplay::EstimateReplaySizeBytes(settings) == 187132800,
          "replay size estimate did not include all encoded tracks and overhead");
}

}  // namespace

int wmain() {
    try {
        StateMachinePreservesIntent();
        SettingsRoundTrip();
        DefaultSettingsUseEnglishAndMp4();
        UpdateMetadataParsesAndComparesVersions();
        UpdateSignatureFixtureVerifies();
        SettingsChangeClassification();
        LegacyAudioSettingsMigrate();
        ProtocolRoundTrip();
        ReplayDurationCommandParses();
        AudioDevicesNormalize();
        AudioDevicesReconcileByNameAndDefault();
        ReplayHotkeysNormalizeToLimit();
        ScreenshotHotkeyNormalizes();
        PerformanceOverlaySettingsNormalize();
        QualityPresetsApplyEncoderLevels();
        ReplaySizeEstimateIncludesEncodedAudioAndOverhead();
        std::cout << "OpenReplay unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OpenReplay unit test failed: " << error.what() << '\n';
        return 1;
    }
}

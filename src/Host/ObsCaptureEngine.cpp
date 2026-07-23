#include "ObsCaptureEngine.h"

#include "openreplay/SettingsStore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <thread>

namespace openreplay::host {
namespace {

constexpr int kModuleSuccess = 0;
constexpr int kVideoSuccess = 0;
constexpr long long kCaptureMethodWgc = 2;

struct ObsDataReleaser {
    ObsApi* api{};
    void operator()(obs_data_t* value) const noexcept {
        if (value) api->obs_data_release(value);
    }
};
using ObsData = std::unique_ptr<obs_data_t, ObsDataReleaser>;

std::wstring Timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    return std::format(L"{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local.wYear, local.wMonth,
                       local.wDay, local.wHour, local.wMinute, local.wSecond);
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& monitors = *reinterpret_cast<std::vector<ObsCaptureEngine::Monitor>*>(parameter);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) return TRUE;

    DISPLAY_DEVICEA device{};
    device.cb = sizeof(device);
    ObsCaptureEngine::Monitor result;
    result.area = info.rcMonitor;
    result.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (EnumDisplayDevicesA(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
        result.id = device.DeviceID;
    } else {
        result.id = info.szDevice;
    }
    monitors.push_back(std::move(result));
    return TRUE;
}

std::vector<std::string_view> EncoderCandidates(const Settings& settings) {
    using enum EncoderVendor;
    using enum VideoCodec;
    if (settings.codec == H264) {
        switch (settings.encoder) {
        case Nvidia: return {"obs_nvenc_h264_tex"};
        case Amd: return {"h264_texture_amf"};
        case Intel: return {"obs_qsv11_v2"};
        case Software: return {"obs_x264"};
        case Auto: return {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
        }
    }
    if (settings.codec == Hevc) {
        switch (settings.encoder) {
        case Nvidia: return {"obs_nvenc_hevc_tex"};
        case Amd: return {"h265_texture_amf"};
        case Intel: return {"obs_qsv11_hevc"};
        case Software: return {};
        case Auto: return {"obs_nvenc_hevc_tex", "h265_texture_amf", "obs_qsv11_hevc"};
        }
    }
    switch (settings.encoder) {
    case Nvidia: return {"obs_nvenc_av1_tex"};
    case Amd: return {"av1_texture_amf"};
    case Intel: return {"obs_qsv11_av1"};
    case Software: return {};
    case Auto: return {"obs_nvenc_av1_tex", "av1_texture_amf", "obs_qsv11_av1"};
    }
    return {};
}

}  // namespace

ObsCaptureEngine::~ObsCaptureEngine() { Shutdown(); }

bool ObsCaptureEngine::Initialize(const Settings& settings, std::string& error) {
    Shutdown();
    settings_ = settings;
    settings_.Normalize();
    if (!api_.Load(error)) return false;
    if (!api_.Version().starts_with("32.")) {
        error = "Unsupported OBS runtime " + api_.Version() + "; OpenReplay MVP requires OBS 32.x";
        api_.Unload();
        return false;
    }

    std::filesystem::create_directories(SettingsStore::DefaultPath().parent_path());
    const auto config = ToUtf8(SettingsStore::DefaultPath().parent_path().wstring());
    if (!api_.obs_startup("en-US", config.c_str(), nullptr)) {
        error = "libobs initialization failed";
        return false;
    }
    obs_started_ = true;

    auto installation = api_.runtime_root();
    if (!std::filesystem::exists(installation / L"data")) {
        installation = installation.parent_path().parent_path();
    }
    const auto libobs_data = ToUtf8((installation / L"data" / L"libobs").wstring() + L"\\");
    api_.obs_add_data_path(libobs_data.c_str());

    const auto monitors = EnumerateMonitors();
    if (monitors.empty()) {
        error = "Windows did not report any active displays";
        Shutdown();
        return false;
    }
    auto selected = std::find_if(monitors.begin(), monitors.end(), [this](const Monitor& monitor) {
        return !settings_.monitor_id.empty() && monitor.id == settings_.monitor_id;
    });
    if (selected == monitors.end()) {
        selected = std::find_if(monitors.begin(), monitors.end(), [](const Monitor& monitor) { return monitor.primary; });
    }
    monitor_ = selected == monitors.end() ? monitors.front() : *selected;

    const auto width = static_cast<std::uint32_t>(monitor_.area.right - monitor_.area.left) & ~1u;
    const auto height = static_cast<std::uint32_t>(monitor_.area.bottom - monitor_.area.top) & ~1u;
    const ObsAudioInfo audio{48000, ObsSpeakerLayout::Stereo};
    if (!api_.obs_reset_audio(&audio)) {
        error = "Unable to initialize the 48 kHz stereo audio pipeline";
        Shutdown();
        return false;
    }
    ObsVideoInfo video{"libobs-d3d11", settings_.fps, 1, width, height, width, height,
                       ObsVideoFormat::Nv12, 0, true, ObsVideoColorSpace::Cs709,
                       ObsVideoRange::Partial, ObsScaleType::Lanczos};
    const int video_result = api_.obs_reset_video(&video);
    if (video_result != kVideoSuccess) {
        error = "Unable to initialize the Direct3D 11 video pipeline (libobs error " +
                std::to_string(video_result) + ')';
        Shutdown();
        return false;
    }

    if (!LoadModule(L"win-capture", true, error) || !LoadModule(L"win-wasapi", true, error) ||
        !LoadModule(L"obs-ffmpeg", true, error)) {
        Shutdown();
        return false;
    }
    LoadModule(L"obs-nvenc", false, error);
    LoadModule(L"obs-qsv11", false, error);
    LoadModule(L"obs-x264", false, error);
    api_.obs_post_load_modules();

    if (!CreateSources(error) || !CreateEncoders(error) || !CreateReplayOutputs(error)) {
        Shutdown();
        return false;
    }
    return true;
}

bool ObsCaptureEngine::ReloadAudioSources(const Settings& settings, std::string& error) {
    auto previous = settings_;
    WriteHostLog("Reloading audio sources without restarting video capture or encoders");
    ReleaseAudioSources();
    settings_ = settings;
    settings_.Normalize();
    if (CreateAudioSources(error)) {
        WriteHostLog("Audio sources reloaded successfully");
        return true;
    }

    const auto apply_error = error;
    ReleaseAudioSources();
    settings_ = std::move(previous);
    std::string restore_error;
    if (!CreateAudioSources(restore_error)) {
        error = apply_error + "; previous audio sources could not be restored: " + restore_error;
    } else {
        error = apply_error;
    }
    return false;
}

bool ObsCaptureEngine::ReloadReplayOutputs(const Settings& settings, bool start_replay, std::string& error) {
    {
        std::scoped_lock lock{replay_save_mutex_};
        if (replay_save_status_.in_progress) {
            error = "Wait for the current replay save to finish before changing replay settings";
            return false;
        }
    }

    auto previous = settings_;
    ReleaseReplayOutputs(false);
    settings_ = settings;
    settings_.Normalize();
    if (CreateReplayOutputs(error) && (!start_replay || StartReplay(error))) return true;

    const auto apply_error = error;
    ReleaseReplayOutputs(false);
    settings_ = std::move(previous);
    std::string restore_error;
    if (!CreateReplayOutputs(restore_error) || (start_replay && !StartReplay(restore_error))) {
        error = apply_error + "; previous replay buffers could not be restored: " + restore_error;
    } else {
        error = apply_error;
    }
    return false;
}

void ObsCaptureEngine::Shutdown() noexcept {
    ReleasePipeline();
    if (obs_started_) api_.obs_shutdown();
    obs_started_ = false;
    retired_replay_outputs_.clear();
    api_.Unload();
    active_encoder_.clear();
}

bool ObsCaptureEngine::LoadModule(std::wstring_view name, bool required, std::string& error) {
    auto installation = api_.runtime_root();
    if (!std::filesystem::exists(installation / L"obs-plugins")) {
        installation = installation.parent_path().parent_path();
    }
    const auto binary = installation / L"obs-plugins" / L"64bit" / (std::wstring{name} + L".dll");
    const auto data = installation / L"data" / L"obs-plugins" / std::wstring{name};
    if (!std::filesystem::exists(binary)) {
        if (required) error = "Required OBS module is missing: " + ToUtf8(binary.wstring());
        return !required;
    }

    obs_module_t* module = nullptr;
    const auto binary_utf8 = ToUtf8(binary.wstring());
    const auto data_utf8 = ToUtf8(data.wstring());
    if (api_.obs_open_module(&module, binary_utf8.c_str(), data_utf8.c_str()) != kModuleSuccess ||
        !api_.obs_init_module(module)) {
        if (required) error = "Unable to initialize OBS module: " + ToUtf8(std::wstring{name});
        return !required;
    }
    return true;
}

bool ObsCaptureEngine::CreateSources(std::string& error) {
    ObsData display_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
    api_.obs_data_set_string(display_settings.get(), "monitor_id", monitor_.id.c_str());
    // DXGI duplication can repeatedly return DXGI_ERROR_UNSUPPORTED while still
    // producing a valid, black encoded stream. WGC is the resilient monitor
    // capture path on the supported Windows 10/11 versions.
    api_.obs_data_set_int(display_settings.get(), "method", kCaptureMethodWgc);
    api_.obs_data_set_bool(display_settings.get(), "capture_cursor", settings_.capture_cursor);
    api_.obs_data_set_bool(display_settings.get(), "force_sdr", true);
    display_ = api_.obs_source_create("monitor_capture", "OpenReplay Display", display_settings.get(), nullptr);

    if (!display_) {
        error = "Unable to create monitor capture source";
        return false;
    }
    api_.obs_set_output_source(0, display_);
    return CreateAudioSources(error);
}

bool ObsCaptureEngine::CreateAudioSources(std::string& error) {
    for (std::size_t index = 0; index < settings_.desktop_audio_devices.size(); ++index) {
        const auto& device = settings_.desktop_audio_devices[index];
        ObsData desktop_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
        api_.obs_data_set_string(desktop_settings.get(), "device_id", device.id.c_str());
        api_.obs_data_set_bool(desktop_settings.get(), "use_device_timing", true);
        const auto name = std::string{"OpenReplay Desktop Audio "} + std::to_string(index + 1);
        WriteHostLog("Configuring desktop audio source " + std::to_string(index + 1) +
                     ": id=" + device.id + ", name=" + device.name);
        auto* source = api_.obs_source_create("wasapi_output_capture", name.c_str(), desktop_settings.get(), nullptr);
        if (!source) {
            error = "Unable to create desktop WASAPI capture source " + std::to_string(index + 1);
            return false;
        }
        api_.obs_source_set_audio_mixers(source, (1u << 0) | (1u << 1));
        api_.obs_source_set_volume(source, static_cast<float>(device.volume_percent) / 100.0F);
        auto audio_source = std::make_unique<AudioSource>();
        audio_source->source = source;
        audio_source->device_id = device.id;
        if (!AttachAudioMeter(*audio_source, error)) {
            api_.obs_source_release(source);
            return false;
        }
        desktop_audio_sources_.push_back(std::move(audio_source));
    }

    if (settings_.microphone_enabled) {
        for (std::size_t index = 0; index < settings_.microphone_devices.size(); ++index) {
            const auto& device = settings_.microphone_devices[index];
            ObsData mic_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
            api_.obs_data_set_string(mic_settings.get(), "device_id", device.id.c_str());
            api_.obs_data_set_bool(mic_settings.get(), "use_device_timing", false);
            const auto name = std::string{"OpenReplay Microphone "} + std::to_string(index + 1);
            WriteHostLog("Configuring microphone source " + std::to_string(index + 1) +
                         ": id=" + device.id + ", name=" + device.name);
            auto* source = api_.obs_source_create("wasapi_input_capture", name.c_str(), mic_settings.get(), nullptr);
            if (!source) {
                error = "Unable to create microphone WASAPI capture source " + std::to_string(index + 1);
                return false;
            }
            api_.obs_source_set_audio_mixers(source, (1u << 0) | (1u << 2));
            api_.obs_source_set_volume(source, static_cast<float>(device.volume_percent) / 100.0F);
            auto audio_source = std::make_unique<AudioSource>();
            audio_source->source = source;
            audio_source->microphone = true;
            audio_source->device_id = device.id;
            if (!AttachAudioMeter(*audio_source, error)) {
                api_.obs_source_release(source);
                return false;
            }
            microphone_sources_.push_back(std::move(audio_source));
        }
    }
    std::uint32_t channel = 1;
    for (const auto& source : desktop_audio_sources_) api_.obs_set_output_source(channel++, source->source);
    for (const auto& source : microphone_sources_) api_.obs_set_output_source(channel++, source->source);
    return true;
}

bool ObsCaptureEngine::AttachAudioMeter(AudioSource& source, std::string& error) {
    source.meter = api_.obs_volmeter_create(ObsFaderType::Log);
    if (!source.meter || !api_.obs_volmeter_attach_source(source.meter, source.source)) {
        if (source.meter) api_.obs_volmeter_destroy(source.meter);
        source.meter = nullptr;
        error = "Unable to create an audio level meter";
        return false;
    }
    api_.obs_volmeter_add_callback(source.meter, AudioLevelUpdated, &source);
    return true;
}

void ObsCaptureEngine::AudioLevelUpdated(void* context, const float*, const float* peak, const float*) noexcept {
    auto* source = static_cast<AudioSource*>(context);
    if (!source || !peak) return;

    float strongest_db = -60.0F;
    for (std::size_t channel = 0; channel < 8; ++channel) {
        if (std::isfinite(peak[channel])) strongest_db = std::max(strongest_db, peak[channel]);
    }
    source->peak.store(std::clamp((strongest_db + 60.0F) / 60.0F, 0.0F, 1.0F),
                       std::memory_order_relaxed);
}

std::vector<AudioLevel> ObsCaptureEngine::AudioLevels() const {
    std::vector<AudioLevel> result;
    result.reserve(desktop_audio_sources_.size() + microphone_sources_.size());
    const auto append = [&result](const auto& sources) {
        for (const auto& source : sources) {
            result.push_back({source->microphone, source->device_id,
                              source->peak.load(std::memory_order_relaxed)});
        }
    };
    append(desktop_audio_sources_);
    append(microphone_sources_);
    return result;
}

bool ObsCaptureEngine::CreateEncoders(std::string& error) {
    for (const auto candidate : EncoderCandidates(settings_)) {
        std::string candidate_error;
        if (TryVideoEncoder(candidate, candidate_error)) break;
        error = candidate_error;
    }
    if (!video_encoder_) {
        if (error.empty()) error = "The selected codec has no compatible encoder on this system";
        return false;
    }

    const std::size_t track_count = settings_.microphone_enabled ? 3u : 2u;
    for (std::size_t index = 0; index < track_count; ++index) {
        ObsData audio_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
        api_.obs_data_set_int(audio_settings.get(), "bitrate", index == 0 ? 320 : 192);
        const auto name = std::string{"OpenReplay AAC "} + std::to_string(index + 1);
        audio_encoders_[index] = api_.obs_audio_encoder_create("ffmpeg_aac", name.c_str(), audio_settings.get(),
                                                               index, nullptr);
        if (!audio_encoders_[index]) {
            error = "Unable to create AAC audio encoder";
            return false;
        }
        api_.obs_encoder_set_audio(audio_encoders_[index], api_.obs_get_audio());
    }
    return true;
}

bool ObsCaptureEngine::TryVideoEncoder(std::string_view id, std::string& error) {
    if (!api_.obs_get_encoder_codec(std::string{id}.c_str())) return false;
    ObsData settings{CreateVideoSettings(id), ObsDataReleaser{&api_}};
    const std::string id_string{id};
    auto* encoder = api_.obs_video_encoder_create(id_string.c_str(), "OpenReplay Video", settings.get(), nullptr);
    if (!encoder) {
        error = "Encoder " + id_string + " is registered but could not create an instance";
        return false;
    }
    api_.obs_encoder_set_video(encoder, api_.obs_get_video());
    video_encoder_ = encoder;
    active_encoder_ = id_string;
    return true;
}

obs_data_t* ObsCaptureEngine::CreateVideoSettings(std::string_view encoder_id) {
    auto* settings = api_.obs_data_create();
    const auto preset = settings_.quality_preset == QualityPreset::Custom
                            ? QualityPreset::Balanced
                            : settings_.quality_preset;
    const auto maximum_bitrate = settings_.bitrate_kbps * 3U / 2U;
    api_.obs_data_set_int(settings, "keyint_sec", 2);
    api_.obs_data_set_int(settings, "bitrate", settings_.bitrate_kbps);
    if (encoder_id.starts_with("obs_nvenc")) {
        api_.obs_data_set_string(settings, "rate_control", "VBR");
        api_.obs_data_set_int(settings, "max_bitrate", maximum_bitrate);
        api_.obs_data_set_string(settings, "preset", preset == QualityPreset::Performance
                                                            ? "p3"
                                                            : preset == QualityPreset::HighQuality ? "p7" : "p5");
        api_.obs_data_set_string(settings, "tune", "hq");
        api_.obs_data_set_string(settings, "multipass", preset == QualityPreset::Performance
                                                               ? "disabled"
                                                               : preset == QualityPreset::HighQuality ? "fullres" : "qres");
        api_.obs_data_set_string(settings, "profile", settings_.codec == VideoCodec::H264 ? "high" : "main");
        api_.obs_data_set_int(settings, "bf", 2);
        api_.obs_data_set_bool(settings, "psycho_aq", preset != QualityPreset::Performance);
    } else if (encoder_id.find("_amf") != std::string_view::npos) {
        api_.obs_data_set_string(settings, "rate_control", "VBR");
        api_.obs_data_set_string(settings, "preset", preset == QualityPreset::Performance
                                                            ? "speed"
                                                            : preset == QualityPreset::HighQuality ? "highQuality" : "balanced");
        api_.obs_data_set_string(settings, "profile", settings_.codec == VideoCodec::H264 ? "high" : "main");
    } else if (encoder_id.starts_with("obs_qsv")) {
        api_.obs_data_set_string(settings, "rate_control", "VBR");
        api_.obs_data_set_int(settings, "max_bitrate", maximum_bitrate);
        api_.obs_data_set_string(settings, "target_usage", preset == QualityPreset::Performance
                                                                  ? "TU6"
                                                                  : preset == QualityPreset::HighQuality ? "TU2" : "TU4");
        api_.obs_data_set_string(settings, "profile", settings_.codec == VideoCodec::H264 ? "high" : "main");
    } else {
        api_.obs_data_set_string(settings, "preset", preset == QualityPreset::Performance
                                                            ? "ultrafast"
                                                            : preset == QualityPreset::HighQuality ? "medium" : "veryfast");
        api_.obs_data_set_string(settings, "profile", "high");
    }
    return settings;
}

bool ObsCaptureEngine::CreateReplayOutputs(std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(settings_.output_directory, filesystem_error);
    if (filesystem_error) {
        error = "Unable to create output directory: " + filesystem_error.message();
        return false;
    }
    std::vector<std::uint32_t> durations{settings_.replay_seconds};
    for (const auto& hotkey : settings_.replay_hotkeys) {
        if (hotkey.enabled) durations.push_back(hotkey.replay_seconds);
    }
    std::ranges::sort(durations);
    durations.erase(std::unique(durations.begin(), durations.end()), durations.end());

    replay_outputs_.reserve(durations.size());
    const auto directory = ToUtf8(settings_.output_directory.wstring());
    for (const auto seconds : durations) {
        ObsData output_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
        api_.obs_data_set_string(output_settings.get(), "directory", directory.c_str());
        const auto format = std::string{"OpenReplay_%CCYY-%MM-%DD_%hh-%mm-%ss_Replay_"} +
                            std::to_string(seconds) + "s";
        api_.obs_data_set_string(output_settings.get(), "format", format.c_str());
        api_.obs_data_set_string(output_settings.get(), "extension", ToString(settings_.output_format).data());
        api_.obs_data_set_bool(output_settings.get(), "allow_spaces", false);
        api_.obs_data_set_int(output_settings.get(), "max_time_sec", seconds);
        api_.obs_data_set_int(output_settings.get(), "max_size_mb", settings_.replay_max_megabytes);

        auto replay = std::make_unique<ReplayOutput>();
        replay->owner = this;
        replay->replay_seconds = seconds;
        const auto name = std::string{"OpenReplay Replay "} + std::to_string(seconds) + "s";
        replay->output = api_.obs_output_create("replay_buffer", name.c_str(), output_settings.get(), nullptr);
        if (!replay->output) {
            error = "Unable to create the " + std::to_string(seconds) + " second libobs replay buffer";
            return false;
        }
        replay->signal_handler = api_.obs_output_get_signal_handler(replay->output);
        if (replay->signal_handler) {
            api_.signal_handler_connect(replay->signal_handler, "saved", ReplaySaved, replay.get());
        }
        AttachEncoders(replay->output);
        replay_outputs_.push_back(std::move(replay));
    }
    return true;
}

void ObsCaptureEngine::AttachEncoders(obs_output_t* output) {
    api_.obs_output_set_video_encoder(output, video_encoder_);
    for (std::size_t index = 0; index < audio_encoders_.size(); ++index) {
        if (audio_encoders_[index]) api_.obs_output_set_audio_encoder(output, audio_encoders_[index], index);
    }
}

bool ObsCaptureEngine::StartReplay(std::string& error) {
    if (replay_outputs_.empty()) {
        error = "Replay output is not initialized";
        return false;
    }
    if (ReplayActive()) return true;
    for (const auto& replay : replay_outputs_) {
        if (api_.obs_output_active(replay->output)) continue;
        if (!api_.obs_output_start(replay->output)) {
            error = OutputError(replay->output, "Unable to start replay buffer");
            for (const auto& started : replay_outputs_) StopOutput(started->output);
            return false;
        }
    }
    return true;
}

void ObsCaptureEngine::StopReplay() noexcept {
    for (const auto& replay : replay_outputs_) StopOutput(replay->output);
}

bool ObsCaptureEngine::ReplayActive() const noexcept {
    return !replay_outputs_.empty() && std::ranges::all_of(replay_outputs_, [this](const auto& replay) {
        return replay->output && api_.obs_output_active(replay->output);
    });
}

bool ObsCaptureEngine::SaveReplay(std::uint32_t replay_seconds, std::filesystem::path& output, std::string& error) {
    if (!ReplayActive()) {
        error = "Instant replay is not running";
        return false;
    }
    const auto requested_seconds = replay_seconds == 0 ? settings_.replay_seconds : replay_seconds;
    const auto selected = std::ranges::find_if(replay_outputs_, [requested_seconds](const auto& replay) {
        return replay->replay_seconds == requested_seconds;
    });
    if (selected == replay_outputs_.end()) {
        error = "No replay buffer is configured for " + std::to_string(requested_seconds) + " seconds";
        return false;
    }
    {
        std::scoped_lock lock{replay_save_mutex_};
        if (replay_save_status_.in_progress) {
            error = "A replay save is already in progress";
            return false;
        }
        replay_save_status_.in_progress = true;
        replay_save_status_.output.clear();
        replay_save_status_.error.clear();
        replay_save_started_ = std::chrono::steady_clock::now();
        replay_save_seconds_ = requested_seconds;
    }
    calldata_t call{};
    if (!api_.proc_handler_call(api_.obs_output_get_proc_handler((*selected)->output), "save", &call)) {
        std::scoped_lock lock{replay_save_mutex_};
        replay_save_status_.in_progress = false;
        replay_save_status_.error = "libobs rejected the replay save command";
        ++replay_save_status_.revision;
        error = "libobs rejected the replay save command";
        WriteHostLog("Replay save command was rejected by libobs");
        return false;
    }
    WriteHostLog("Replay save requested: " + std::to_string(requested_seconds) + " seconds");
    output = settings_.output_directory;
    return true;
}

ReplaySaveStatus ObsCaptureEngine::ReplaySaveState() {
    bool timed_out = false;
    ReplaySaveStatus result;
    {
        std::scoped_lock lock{replay_save_mutex_};
        const auto timeout_seconds = std::clamp(replay_save_seconds_ / 2U, 30U, 120U);
        if (replay_save_status_.in_progress &&
            std::chrono::steady_clock::now() - replay_save_started_ > std::chrono::seconds{timeout_seconds}) {
            replay_save_status_.in_progress = false;
            replay_save_status_.error = "Replay save timed out after " + std::to_string(timeout_seconds) + " seconds";
            ++replay_save_status_.revision;
            timed_out = true;
        }
        result = replay_save_status_;
    }
    if (timed_out) WriteHostLog(result.error);
    return result;
}

void ObsCaptureEngine::ReplaySaved(void* context, calldata_t*) noexcept {
    auto* replay = static_cast<ReplayOutput*>(context);
    auto* self = replay ? replay->owner : nullptr;
    if (!self || !replay->output) return;
    calldata_t call{};
    try {
        const auto called = self->api_.proc_handler_call(
            self->api_.obs_output_get_proc_handler(replay->output), "get_last_replay", &call);
        const char* path = nullptr;
        if (!called || !self->api_.calldata_get_string(&call, "path", &path) || !path || !*path) {
            throw std::runtime_error{"Replay was saved, but its output path is unavailable"};
        }

        const std::string saved_path{path};
        {
            std::scoped_lock lock{self->replay_save_mutex_};
            self->replay_save_status_.in_progress = false;
            self->replay_save_status_.output = FromUtf8(saved_path);
            self->replay_save_status_.error.clear();
            ++self->replay_save_status_.revision;
        }
        WriteHostLog("Replay saved: " + saved_path);
    } catch (const std::exception& exception) {
        try {
            std::scoped_lock lock{self->replay_save_mutex_};
            self->replay_save_status_.in_progress = false;
            self->replay_save_status_.error = exception.what();
            ++self->replay_save_status_.revision;
        } catch (...) {
        }
        WriteHostLog(std::string{"Replay save callback failed: "} + exception.what());
    } catch (...) {
        try {
            std::scoped_lock lock{self->replay_save_mutex_};
            self->replay_save_status_.in_progress = false;
            self->replay_save_status_.error = "Unknown replay save callback failure";
            ++self->replay_save_status_.revision;
        } catch (...) {
        }
        WriteHostLog("Replay save callback failed with an unknown exception");
    }
    if (call.stack) self->api_.bfree(call.stack);
}

void ObsCaptureEngine::DisconnectReplaySignal(ReplayOutput& replay) noexcept {
    if (!replay.signal_handler) return;
    api_.signal_handler_disconnect(replay.signal_handler, "saved", ReplaySaved, &replay);
    replay.signal_handler = nullptr;
}

bool ObsCaptureEngine::StartRecording(std::filesystem::path& output, std::string& error) {
    if (RecordingActive()) {
        error = "Recording is already active";
        return false;
    }
    recording_path_ = RecordingPath(settings_.output_directory, settings_.output_format);
    ObsData output_settings{api_.obs_data_create(), ObsDataReleaser{&api_}};
    const auto path = ToUtf8(recording_path_.wstring());
    api_.obs_data_set_string(output_settings.get(), "path", path.c_str());
    api_.obs_data_set_bool(output_settings.get(), "allow_overwrite", false);
    recording_output_ = api_.obs_output_create("ffmpeg_muxer", "OpenReplay Recording", output_settings.get(), nullptr);
    if (!recording_output_) {
        error = "Unable to create recording output";
        return false;
    }
    AttachEncoders(recording_output_);
    if (!api_.obs_output_start(recording_output_)) {
        error = OutputError(recording_output_, "Unable to start recording");
        api_.obs_output_release(recording_output_);
        recording_output_ = nullptr;
        return false;
    }
    output = recording_path_;
    return true;
}

bool ObsCaptureEngine::StopRecording(std::filesystem::path& output, std::string&) {
    if (!recording_output_) return true;
    StopOutput(recording_output_);
    api_.obs_output_release(recording_output_);
    recording_output_ = nullptr;
    output = recording_path_;
    return true;
}

bool ObsCaptureEngine::RecordingActive() const noexcept {
    return recording_output_ && api_.obs_output_active(recording_output_);
}

bool ObsCaptureEngine::Screenshot(std::filesystem::path& output, std::string& error) {
    return screenshot_service_.CaptureMonitor(settings_.output_directory, monitor_.area, output, error);
}

void ObsCaptureEngine::StopOutput(obs_output_t* output) noexcept {
    if (!output || !api_.obs_output_active(output)) return;
    api_.obs_output_stop(output);
    for (int attempt = 0; attempt < 30 && api_.obs_output_active(output); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (api_.obs_output_active(output)) api_.obs_output_force_stop(output);
}

void ObsCaptureEngine::ReleasePipeline() noexcept {
    if (!api_.loaded()) return;
    if (recording_output_) {
        StopOutput(recording_output_);
        api_.obs_output_release(recording_output_);
        recording_output_ = nullptr;
    }
    ReleaseReplayOutputs(true);
    for (auto*& encoder : audio_encoders_) {
        if (encoder) api_.obs_encoder_release(encoder);
        encoder = nullptr;
    }
    if (video_encoder_) api_.obs_encoder_release(video_encoder_);
    video_encoder_ = nullptr;

    ReleaseAudioSources();
    api_.obs_set_output_source(0, nullptr);
    if (display_) api_.obs_source_release(display_);
    display_ = nullptr;
}

void ObsCaptureEngine::ReleaseReplayOutputs(bool mark_interrupted) noexcept {
    if (!replay_outputs_.empty()) {
        bool save_interrupted = false;
        {
            std::scoped_lock lock{replay_save_mutex_};
            if (mark_interrupted && replay_save_status_.in_progress) {
                replay_save_status_.in_progress = false;
                replay_save_status_.error = "Replay save was interrupted by capture pipeline shutdown";
                ++replay_save_status_.revision;
                save_interrupted = true;
            }
        }
        if (save_interrupted) WriteHostLog("Replay save interrupted by capture pipeline shutdown");
        for (auto& replay : replay_outputs_) {
            DisconnectReplaySignal(*replay);
            StopOutput(replay->output);
            api_.obs_output_release(replay->output);
            replay->output = nullptr;
            retired_replay_outputs_.push_back(std::move(replay));
        }
        replay_outputs_.clear();
    }
}

void ObsCaptureEngine::ReleaseAudioSources() noexcept {
    constexpr std::uint32_t maximum_audio_sources = 5;
    for (std::uint32_t index = 0; index < maximum_audio_sources; ++index) {
        api_.obs_set_output_source(index + 1, nullptr);
    }
    for (const auto& source : microphone_sources_) ReleaseAudioSource(*source);
    for (const auto& source : desktop_audio_sources_) ReleaseAudioSource(*source);
    microphone_sources_.clear();
    desktop_audio_sources_.clear();
}

void ObsCaptureEngine::ReleaseAudioSource(AudioSource& source) noexcept {
    if (source.meter) {
        api_.obs_volmeter_remove_callback(source.meter, AudioLevelUpdated, &source);
        api_.obs_volmeter_detach_source(source.meter);
        api_.obs_volmeter_destroy(source.meter);
        source.meter = nullptr;
    }
    if (source.source) api_.obs_source_release(source.source);
    source.source = nullptr;
}

std::vector<ObsCaptureEngine::Monitor> ObsCaptureEngine::EnumerateMonitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

std::filesystem::path ObsCaptureEngine::RecordingPath(const std::filesystem::path& directory, OutputFormat format) {
    const auto extension = format == OutputFormat::Mp4 ? L".mp4" : L".mkv";
    return directory / (L"OpenReplay_" + Timestamp() + L"_Recording" + extension);
}

std::string ObsCaptureEngine::OutputError(obs_output_t* output, std::string_view fallback) const {
    const char* detail = output ? api_.obs_output_get_last_error(output) : nullptr;
    return detail && *detail ? detail : std::string{fallback};
}

}  // namespace openreplay::host

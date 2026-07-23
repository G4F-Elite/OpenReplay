#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openreplay::host {

void WriteHostLog(std::string_view message) noexcept;

struct obs_data;
struct obs_source;
struct obs_encoder;
struct obs_output;
struct obs_module;
struct proc_handler;
struct signal_handler;
struct video_output;
struct audio_output;
struct profiler_name_store;
struct obs_volmeter;
struct calldata {
    std::uint8_t* stack;
    std::size_t size;
    std::size_t capacity;
    bool fixed;
};

using obs_data_t = obs_data;
using obs_source_t = obs_source;
using obs_encoder_t = obs_encoder;
using obs_output_t = obs_output;
using obs_module_t = obs_module;
using proc_handler_t = proc_handler;
using signal_handler_t = signal_handler;
using video_t = video_output;
using audio_t = audio_output;
using profiler_name_store_t = profiler_name_store;
using obs_volmeter_t = obs_volmeter;
using calldata_t = calldata;
using ObsLogHandler = void (*)(int, const char*, ::va_list, void*);
using SignalCallback = void (*)(void*, calldata_t*);
using VolmeterCallback = void (*)(void*, const float*, const float*, const float*);

enum class ObsVideoFormat : int { None, I420, Nv12 };
enum class ObsVideoColorSpace : int { Default, Cs601, Cs709, Srgb, Cs2100Pq, Cs2100Hlg };
enum class ObsVideoRange : int { Default, Partial, Full };
enum class ObsScaleType : int { Disable, Point, Bicubic, Bilinear, Lanczos, Area };
enum class ObsSpeakerLayout : int { Unknown, Mono, Stereo };
enum class ObsFaderType : int { Cubic, Iec, Log };

struct ObsVideoInfo {
    const char* graphics_module;
    std::uint32_t fps_num;
    std::uint32_t fps_den;
    std::uint32_t base_width;
    std::uint32_t base_height;
    std::uint32_t output_width;
    std::uint32_t output_height;
    ObsVideoFormat output_format;
    std::uint32_t adapter;
    bool gpu_conversion;
    ObsVideoColorSpace colorspace;
    ObsVideoRange range;
    ObsScaleType scale_type;
};

struct ObsAudioInfo {
    std::uint32_t samples_per_sec;
    ObsSpeakerLayout speakers;
};

class ObsApi {
public:
    ObsApi() = default;
    ~ObsApi();
    ObsApi(const ObsApi&) = delete;
    ObsApi& operator=(const ObsApi&) = delete;

    bool Load(std::string& error);
    void Unload() noexcept;
    [[nodiscard]] bool loaded() const noexcept { return module_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& runtime_root() const noexcept { return runtime_root_; }
    [[nodiscard]] std::string Version() const;

    bool (*obs_startup)(const char*, const char*, profiler_name_store_t*){};
    void (*obs_shutdown)(){};
    const char* (*obs_get_version_string)(){};
    void (*base_set_log_handler)(ObsLogHandler, void*){};
    void (*obs_add_data_path)(const char*){};
    int (*obs_open_module)(obs_module_t**, const char*, const char*){};
    bool (*obs_init_module)(obs_module_t*){};
    void (*obs_post_load_modules)(){};
    int (*obs_reset_video)(ObsVideoInfo*){};
    bool (*obs_reset_audio)(const ObsAudioInfo*){};
    video_t* (*obs_get_video)(){};
    audio_t* (*obs_get_audio)(){};
    void (*obs_set_output_source)(std::uint32_t, obs_source_t*){};

    obs_data_t* (*obs_data_create)(){};
    void (*obs_data_release)(obs_data_t*){};
    void (*obs_data_set_string)(obs_data_t*, const char*, const char*){};
    void (*obs_data_set_int)(obs_data_t*, const char*, long long){};
    void (*obs_data_set_bool)(obs_data_t*, const char*, bool){};

    obs_source_t* (*obs_source_create)(const char*, const char*, obs_data_t*, obs_data_t*){};
    void (*obs_source_release)(obs_source_t*){};
    void (*obs_source_set_audio_mixers)(obs_source_t*, std::uint32_t){};
    void (*obs_source_set_volume)(obs_source_t*, float){};

    obs_volmeter_t* (*obs_volmeter_create)(ObsFaderType){};
    void (*obs_volmeter_destroy)(obs_volmeter_t*){};
    bool (*obs_volmeter_attach_source)(obs_volmeter_t*, obs_source_t*){};
    void (*obs_volmeter_detach_source)(obs_volmeter_t*){};
    void (*obs_volmeter_add_callback)(obs_volmeter_t*, VolmeterCallback, void*){};
    void (*obs_volmeter_remove_callback)(obs_volmeter_t*, VolmeterCallback, void*){};

    const char* (*obs_get_encoder_codec)(const char*){};
    obs_encoder_t* (*obs_video_encoder_create)(const char*, const char*, obs_data_t*, obs_data_t*){};
    obs_encoder_t* (*obs_audio_encoder_create)(const char*, const char*, obs_data_t*, std::size_t, obs_data_t*){};
    void (*obs_encoder_release)(obs_encoder_t*){};
    void (*obs_encoder_set_video)(obs_encoder_t*, video_t*){};
    void (*obs_encoder_set_audio)(obs_encoder_t*, audio_t*){};

    obs_output_t* (*obs_output_create)(const char*, const char*, obs_data_t*, obs_data_t*){};
    void (*obs_output_release)(obs_output_t*){};
    bool (*obs_output_start)(obs_output_t*){};
    void (*obs_output_stop)(obs_output_t*){};
    void (*obs_output_force_stop)(obs_output_t*){};
    bool (*obs_output_active)(const obs_output_t*){};
    const char* (*obs_output_get_last_error)(obs_output_t*){};
    void (*obs_output_set_video_encoder)(obs_output_t*, obs_encoder_t*){};
    void (*obs_output_set_audio_encoder)(obs_output_t*, obs_encoder_t*, std::size_t){};
    proc_handler_t* (*obs_output_get_proc_handler)(const obs_output_t*){};
    signal_handler_t* (*obs_output_get_signal_handler)(const obs_output_t*){};
    bool (*proc_handler_call)(proc_handler_t*, const char*, calldata_t*){};
    bool (*calldata_get_string)(const calldata_t*, const char*, const char**){};
    void (*signal_handler_connect)(signal_handler_t*, const char*, SignalCallback, void*){};
    void (*signal_handler_disconnect)(signal_handler_t*, const char*, SignalCallback, void*){};
    void (*bfree)(void*){};

private:
    template <typename T>
    bool Resolve(T& target, const char* name, std::string& error);
    bool ResolveAll(std::string& error);

    HMODULE module_{};
    std::vector<HMODULE> runtime_modules_;
    std::filesystem::path runtime_root_;
};

}  // namespace openreplay::host

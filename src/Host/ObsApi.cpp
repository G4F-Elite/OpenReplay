#include "ObsApi.h"

#include "openreplay/SettingsStore.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace openreplay::host {
namespace {

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length == 0 || length == path.size()
               ? std::filesystem::current_path()
               : std::filesystem::path{std::wstring{path.data(), static_cast<std::size_t>(length)}}.parent_path();
}

std::filesystem::path FindRuntimeRoot() {
    const auto local = ExecutableDirectory();
    const auto bundled = local / L"obs-runtime" / L"bin" / L"64bit";
    if (std::filesystem::exists(bundled / L"obs.dll")) return bundled;
    if (std::filesystem::exists(local / L"obs.dll")) return local;
    return {};
}

void ObsLog(int level, const char* format, ::va_list arguments, void*) noexcept {
    try {
        std::array<char, 4096> buffer{};
        ::va_list copy;
        va_copy(copy, arguments);
        const int length = std::vsnprintf(buffer.data(), buffer.size(), format, copy);
        va_end(copy);
        if (length <= 0) return;
        WriteHostLog("[OBS " + std::to_string(level) + "] " + buffer.data());
    } catch (...) {
    }
}

}  // namespace

void WriteHostLog(std::string_view message) noexcept {
    try {
        static std::mutex log_mutex;
        std::scoped_lock lock{log_mutex};
        const auto path = openreplay::SettingsStore::DefaultPath().parent_path() / L"host.log";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        char prefix[32]{};
        sprintf_s(prefix, "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond,
                  time.wMilliseconds);
        std::ofstream output(path, std::ios::app);
        output << prefix << message << '\n';
    } catch (...) {
    }
}

ObsApi::~ObsApi() { Unload(); }

bool ObsApi::Load(std::string& error) {
    if (loaded()) return true;
    runtime_root_ = FindRuntimeRoot();
    if (runtime_root_.empty()) {
        error = "OBS runtime was not found beside Host. Run scripts\\deploy-obs-runtime.ps1.";
        return false;
    }

    SetDllDirectoryW(runtime_root_.c_str());
    SetCurrentDirectoryW(runtime_root_.c_str());
    constexpr std::array runtime_dependencies{
        L"libcurl.dll", L"avdevice-61.dll", L"librist.dll", L"srt.dll"};
    for (const auto* dependency : runtime_dependencies) {
        HMODULE loaded = LoadLibraryExW((runtime_root_ / dependency).c_str(), nullptr,
                                        LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!loaded) {
            error = "Unable to load OBS runtime dependency: " + ToUtf8(dependency);
            Unload();
            return false;
        }
        runtime_modules_.push_back(loaded);
    }
    module_ = LoadLibraryExW((runtime_root_ / L"obs.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) {
        std::ostringstream message;
        message << "Unable to load obs.dll (Windows error " << GetLastError() << ')';
        error = message.str();
        return false;
    }
    if (!ResolveAll(error)) {
        Unload();
        return false;
    }
    base_set_log_handler(ObsLog, nullptr);
    return true;
}

void ObsApi::Unload() noexcept {
    if (module_) FreeLibrary(module_);
    for (auto it = runtime_modules_.rbegin(); it != runtime_modules_.rend(); ++it) FreeLibrary(*it);
    module_ = nullptr;
    runtime_modules_.clear();
    runtime_root_.clear();
}

std::string ObsApi::Version() const {
    return obs_get_version_string ? obs_get_version_string() : std::string{};
}

template <typename T>
bool ObsApi::Resolve(T& target, const char* name, std::string& error) {
    target = reinterpret_cast<T>(GetProcAddress(module_, name));
    if (target) return true;
    error = std::string{"OBS runtime is missing required export: "} + name;
    return false;
}

bool ObsApi::ResolveAll(std::string& error) {
#define OPENREPLAY_OBS_RESOLVE(name) if (!Resolve(name, #name, error)) return false
    OPENREPLAY_OBS_RESOLVE(obs_startup);
    OPENREPLAY_OBS_RESOLVE(obs_shutdown);
    OPENREPLAY_OBS_RESOLVE(obs_get_version_string);
    OPENREPLAY_OBS_RESOLVE(base_set_log_handler);
    OPENREPLAY_OBS_RESOLVE(obs_add_data_path);
    OPENREPLAY_OBS_RESOLVE(obs_open_module);
    OPENREPLAY_OBS_RESOLVE(obs_init_module);
    OPENREPLAY_OBS_RESOLVE(obs_post_load_modules);
    OPENREPLAY_OBS_RESOLVE(obs_reset_video);
    OPENREPLAY_OBS_RESOLVE(obs_reset_audio);
    OPENREPLAY_OBS_RESOLVE(obs_get_video);
    OPENREPLAY_OBS_RESOLVE(obs_get_audio);
    OPENREPLAY_OBS_RESOLVE(obs_set_output_source);
    OPENREPLAY_OBS_RESOLVE(obs_data_create);
    OPENREPLAY_OBS_RESOLVE(obs_data_release);
    OPENREPLAY_OBS_RESOLVE(obs_data_set_string);
    OPENREPLAY_OBS_RESOLVE(obs_data_set_int);
    OPENREPLAY_OBS_RESOLVE(obs_data_set_bool);
    OPENREPLAY_OBS_RESOLVE(obs_source_create);
    OPENREPLAY_OBS_RESOLVE(obs_source_release);
    OPENREPLAY_OBS_RESOLVE(obs_source_set_audio_mixers);
    OPENREPLAY_OBS_RESOLVE(obs_source_set_volume);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_create);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_destroy);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_attach_source);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_detach_source);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_add_callback);
    OPENREPLAY_OBS_RESOLVE(obs_volmeter_remove_callback);
    OPENREPLAY_OBS_RESOLVE(obs_get_encoder_codec);
    OPENREPLAY_OBS_RESOLVE(obs_video_encoder_create);
    OPENREPLAY_OBS_RESOLVE(obs_audio_encoder_create);
    OPENREPLAY_OBS_RESOLVE(obs_encoder_release);
    OPENREPLAY_OBS_RESOLVE(obs_encoder_set_video);
    OPENREPLAY_OBS_RESOLVE(obs_encoder_set_audio);
    OPENREPLAY_OBS_RESOLVE(obs_output_create);
    OPENREPLAY_OBS_RESOLVE(obs_output_release);
    OPENREPLAY_OBS_RESOLVE(obs_output_start);
    OPENREPLAY_OBS_RESOLVE(obs_output_stop);
    OPENREPLAY_OBS_RESOLVE(obs_output_force_stop);
    OPENREPLAY_OBS_RESOLVE(obs_output_active);
    OPENREPLAY_OBS_RESOLVE(obs_output_get_last_error);
    OPENREPLAY_OBS_RESOLVE(obs_output_set_video_encoder);
    OPENREPLAY_OBS_RESOLVE(obs_output_set_audio_encoder);
    OPENREPLAY_OBS_RESOLVE(obs_output_get_proc_handler);
    OPENREPLAY_OBS_RESOLVE(obs_output_get_signal_handler);
    OPENREPLAY_OBS_RESOLVE(proc_handler_call);
    OPENREPLAY_OBS_RESOLVE(calldata_get_string);
    OPENREPLAY_OBS_RESOLVE(signal_handler_connect);
    OPENREPLAY_OBS_RESOLVE(signal_handler_disconnect);
    OPENREPLAY_OBS_RESOLVE(bfree);
#undef OPENREPLAY_OBS_RESOLVE
    return true;
}

}  // namespace openreplay::host

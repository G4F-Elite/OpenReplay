#pragma once

#include "openreplay/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace openreplay::host {

class ICaptureEngine {
public:
    virtual ~ICaptureEngine() = default;
    virtual bool Initialize(const Settings& settings, std::string& error) = 0;
    virtual bool ReloadAudioSources(const Settings& settings, std::string& error) = 0;
    virtual bool ReloadReplayOutputs(const Settings& settings, bool start_replay, std::string& error) = 0;
    virtual void Shutdown() noexcept = 0;
    virtual bool StartReplay(std::string& error) = 0;
    virtual void StopReplay() noexcept = 0;
    virtual bool ReplayActive() const noexcept = 0;
    virtual bool SaveReplay(std::uint32_t replay_seconds, std::filesystem::path& output, std::string& error) = 0;
    virtual ReplaySaveStatus ReplaySaveState() = 0;
    virtual bool StartRecording(std::filesystem::path& output, std::string& error) = 0;
    virtual bool StopRecording(std::filesystem::path& output, std::string& error) = 0;
    virtual bool RecordingActive() const noexcept = 0;
    virtual bool Screenshot(std::filesystem::path& output, std::string& error) = 0;
    virtual std::string ActiveEncoder() const = 0;
    virtual std::vector<AudioLevel> AudioLevels() const = 0;
};

}  // namespace openreplay::host

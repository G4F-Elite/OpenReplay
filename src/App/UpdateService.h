#pragma once

#include "openreplay/UpdateTypes.h"

#include <filesystem>
#include <string>

namespace openreplay::ui {

struct UpdateCheckResult {
    bool ok{false};
    bool update_available{false};
    UpdateManifest manifest;
    std::wstring error;
};

struct UpdateDownloadResult {
    bool ok{false};
    std::filesystem::path archive;
    std::wstring error;
};

class UpdateService {
public:
    UpdateCheckResult Check() const;
    UpdateDownloadResult Download(const UpdateManifest& manifest) const;
    bool LaunchUpdater(const UpdateManifest& manifest, const std::filesystem::path& archive,
                       DWORD app_pid, const std::filesystem::path& install_root,
                       const std::filesystem::path& health_file, std::wstring& error) const;

private:
    static std::filesystem::path UpdateRoot();
};

}  // namespace openreplay::ui

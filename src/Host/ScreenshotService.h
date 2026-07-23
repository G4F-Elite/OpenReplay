#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

namespace openreplay::host {

class ScreenshotService {
public:
    bool CaptureMonitor(const std::filesystem::path& directory, const RECT& area,
                        std::filesystem::path& output, std::string& error) const;
};

}  // namespace openreplay::host

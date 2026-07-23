#pragma once

#include "openreplay/Types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace openreplay {

class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path path = DefaultPath());

    [[nodiscard]] Settings Load() const;
    void Save(const Settings& settings) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path DefaultOutputDirectory();

private:
    std::filesystem::path path_;
};

std::string PercentEncode(std::string_view value);
std::string PercentDecode(std::string_view value);
std::string ToUtf8(const std::wstring& value);
std::wstring FromUtf8(std::string_view value);

}  // namespace openreplay

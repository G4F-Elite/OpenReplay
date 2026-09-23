#pragma once

#include <cstdint>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace openreplay {

struct SemanticVersion {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};
    std::string pre_release;

    std::strong_ordering operator<=>(const SemanticVersion& other) const noexcept;
    bool operator==(const SemanticVersion&) const = default;
};

struct UpdateManifest {
    std::uint32_t schema{};
    std::string product;
    std::string channel;
    std::string version;
    std::string tag;
    std::string release_notes_url;
    std::string asset_url;
    std::string asset_name;
    std::string architecture;
    std::uint64_t asset_size{};
    std::string asset_sha256;
};

std::optional<SemanticVersion> ParseSemanticVersion(std::string_view value) noexcept;
bool IsStableReleasePreferred(std::string_view stable_version,
                              std::string_view developer_version) noexcept;
std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json);
bool IsValidUpdate(const UpdateManifest& manifest, std::string_view current_version,
                   std::string_view channel) noexcept;
bool VerifyUpdateSignature(std::span<const std::uint8_t> content,
                           std::span<const std::uint8_t> signature) noexcept;

}  // namespace openreplay

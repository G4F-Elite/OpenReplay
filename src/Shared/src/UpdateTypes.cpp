#include "openreplay/UpdateTypes.h"

#include "openreplay/Version.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <vector>

namespace openreplay {
namespace {

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
    const auto marker = '"' + std::string{key} + '"';
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string_view::npos || json[position] != '"') return std::nullopt;

    std::string result;
    for (++position; position < json.size(); ++position) {
        const char character = json[position];
        if (character == '"') return result;
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (++position >= json.size()) return std::nullopt;
        switch (json[position]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

template <typename Integer>
std::optional<Integer> JsonInteger(std::string_view json, std::string_view key) {
    const auto marker = '"' + std::string{key} + '"';
    auto position = json.find(marker);
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return std::nullopt;
    position = json.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string_view::npos) return std::nullopt;
    auto end = position;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == position) return std::nullopt;
    Integer result{};
    const auto parsed = std::from_chars(json.data() + position, json.data() + end, result);
    return parsed.ec == std::errc{} && parsed.ptr == json.data() + end
               ? std::optional<Integer>{result}
               : std::nullopt;
}

bool IsLowerHex(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!std::isdigit(static_cast<unsigned char>(character)) &&
            (character < 'a' || character > 'f')) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<SemanticVersion> ParseSemanticVersion(std::string_view value) noexcept {
    if (value.starts_with('v')) value.remove_prefix(1);
    SemanticVersion result;
    std::uint32_t* parts[] = {&result.major, &result.minor, &result.patch};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto separator = value.find('.');
        const auto part = separator == std::string_view::npos ? value : value.substr(0, separator);
        if (part.empty()) return std::nullopt;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), *parts[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) return std::nullopt;
        if (index < 2) {
            if (separator == std::string_view::npos) return std::nullopt;
            value.remove_prefix(separator + 1);
        } else if (separator != std::string_view::npos) {
            return std::nullopt;
        }
    }
    return result;
}

std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json) {
    UpdateManifest result;
    const auto schema = JsonInteger<std::uint32_t>(json, "schema");
    const auto product = JsonString(json, "product");
    const auto channel = JsonString(json, "channel");
    const auto version = JsonString(json, "version");
    const auto tag = JsonString(json, "tag");
    const auto release_notes = JsonString(json, "release_notes_url");
    const auto asset_url = JsonString(json, "asset_url");
    const auto asset_name = JsonString(json, "asset_name");
    const auto architecture = JsonString(json, "architecture");
    const auto size = JsonInteger<std::uint64_t>(json, "size_bytes");
    const auto hash = JsonString(json, "sha256");
    if (!schema || !product || !channel || !version || !tag || !release_notes || !asset_url ||
        !asset_name || !architecture || !size || !hash) {
        return std::nullopt;
    }
    result.schema = *schema;
    result.product = *product;
    result.channel = *channel;
    result.version = *version;
    result.tag = *tag;
    result.release_notes_url = *release_notes;
    result.asset_url = *asset_url;
    result.asset_name = *asset_name;
    result.architecture = *architecture;
    result.asset_size = *size;
    result.asset_sha256 = *hash;
    return result;
}

bool IsValidStableUpdate(const UpdateManifest& manifest, std::string_view current_version) noexcept {
    const auto current = ParseSemanticVersion(current_version);
    const auto available = ParseSemanticVersion(manifest.version);
    return current && available && manifest.schema == 1 && manifest.product == "OpenReplay" &&
           manifest.channel == "stable" && manifest.architecture == "x64" && manifest.asset_size > 0 &&
           IsLowerHex(manifest.asset_sha256) && manifest.asset_url.starts_with(
               "https://github.com/G4F-Elite/OpenReplay/releases/download/") &&
           *available > *current;
}

bool VerifyUpdateSignature(std::span<const std::uint8_t> content,
                           std::span<const std::uint8_t> signature) noexcept {
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

    const auto public_key = decode(kUpdatePublicKeyBase64);
    if (public_key.empty() || content.empty() || signature.empty()) return false;
    BCRYPT_ALG_HANDLE sha_algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    BCRYPT_ALG_HANDLE rsa_algorithm{};
    BCRYPT_KEY_HANDLE key{};
    DWORD object_size = 0;
    DWORD actual = 0;
    std::array<std::uint8_t, 32> digest{};
    bool verified = false;
    if (BCryptOpenAlgorithmProvider(&sha_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(sha_algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &actual, 0) >= 0) {
        std::vector<UCHAR> object(object_size);
        if (BCryptCreateHash(sha_algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(content.data()), static_cast<ULONG>(content.size()), 0) >= 0 &&
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
            BCryptOpenAlgorithmProvider(&rsa_algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptImportKeyPair(rsa_algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                                const_cast<PUCHAR>(public_key.data()), static_cast<ULONG>(public_key.size()), 0) >= 0) {
            BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
            verified = BCryptVerifySignature(key, &padding, digest.data(), static_cast<ULONG>(digest.size()),
                                              const_cast<PUCHAR>(signature.data()),
                                              static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
        }
    }
    if (key) BCryptDestroyKey(key);
    if (rsa_algorithm) BCryptCloseAlgorithmProvider(rsa_algorithm, 0);
    if (hash) BCryptDestroyHash(hash);
    if (sha_algorithm) BCryptCloseAlgorithmProvider(sha_algorithm, 0);
    return verified;
}

}  // namespace openreplay

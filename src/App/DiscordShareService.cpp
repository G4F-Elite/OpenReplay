#include "pch.h"

#include "DiscordShareService.h"

#include "openreplay/SettingsStore.h"
#include "openreplay/Types.h"
#include "openreplay/Version.h"

#include <bcrypt.h>
#include <wincred.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <format>
#include <limits>

namespace openreplay::ui {
namespace {

constexpr wchar_t kCredentialTarget[] = L"OpenReplay/DiscordWebhook";
constexpr std::uint32_t kDiscordAudioBitrate = 128000;
constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

struct FileHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};

struct WipeString {
    std::wstring& value;
    ~WipeString() { if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
};

std::wstring SystemMessage(DWORD error) {
    wchar_t* raw = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    std::wstring result = length && raw ? std::wstring{raw, length} : L"Windows error " + std::to_wstring(error);
    if (raw) LocalFree(raw);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

std::string JsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 16);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                result += "\\u00";
                result.push_back(hex[character >> 4]);
                result.push_back(hex[character & 0x0F]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    return result;
}

bool AllowedDiscordHost(std::wstring_view host) {
    std::wstring normalized{host};
    std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) { return ::towlower(value); });
    return normalized == L"discord.com" || normalized == L"canary.discord.com" ||
           normalized == L"ptb.discord.com" || normalized == L"discordapp.com";
}

struct WebhookParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{};
};

bool ParseWebhook(std::wstring_view url, WebhookParts& result, std::wstring& error) {
    if (url.find(L'#') != std::wstring_view::npos) {
        error = L"Discord webhook URL must not contain a fragment";
        return false;
    }
    std::wstring mutable_url{url};
    WipeString wipe_url{mutable_url};
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(mutable_url.data(), static_cast<DWORD>(mutable_url.size()), 0, &components)) {
        error = L"Invalid Discord webhook URL";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS || components.nPort != INTERNET_DEFAULT_HTTPS_PORT) {
        error = L"Discord webhook must use HTTPS";
        return false;
    }
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    if (!AllowedDiscordHost(result.host)) {
        error = L"Only official Discord webhook domains are allowed";
        return false;
    }
    result.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) result.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    const std::wstring_view path{result.path};
    const auto query = path.find(L'?');
    const auto route = path.substr(0, query);
    const bool route_ok = route.starts_with(L"/api/webhooks/") || route.starts_with(L"/api/v10/webhooks/");
    if (!route_ok) {
        error = L"Discord webhook URL is incomplete";
        return false;
    }
    const auto prefix_length = route.starts_with(L"/api/v10/webhooks/") ? 18U : 14U;
    const auto webhook = route.substr(prefix_length);
    const auto separator = webhook.find(L'/');
    if (separator == std::wstring::npos || separator == 0 || separator + 1 >= webhook.size() ||
        webhook.find(L'/', separator + 1) != std::wstring::npos) {
        error = L"Discord webhook URL is incomplete";
        return false;
    }
    result.port = components.nPort;
    return true;
}

std::wstring WaitPath(std::wstring path) {
    path += path.find(L'?') == std::wstring::npos ? L"?wait=true" : L"&wait=true";
    return path;
}

bool WriteHttp(HINTERNET request, std::span<const std::uint8_t> data, std::wstring& error) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 64U * 1024U));
        DWORD written = 0;
        if (!WinHttpWriteData(request, data.data() + offset, chunk, &written) || written != chunk) {
            error = SystemMessage(GetLastError());
            return false;
        }
        offset += written;
    }
    return true;
}

bool WriteHttp(HINTERNET request, std::string_view data, std::wstring& error) {
    return WriteHttp(request, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, error);
}

DiscordUploadResult CompleteRequest(HINTERNET request) {
    if (!WinHttpReceiveResponse(request, nullptr)) return {.error = SystemMessage(GetLastError())};
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return {.error = SystemMessage(GetLastError())};
    }
    if (status >= 200 && status < 300) return {.ok = true};
    if (status == 401 || status == 403 || status == 404) {
        return {.webhook_invalid = true, .error = L"Discord rejected this webhook"};
    }
    if (status == 413) return {.error = L"Discord rejected the clip because it is too large"};
    if (status == 429) {
        wchar_t retry[64]{};
        DWORD size = sizeof(retry);
        std::uint32_t delay = 1000;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            try {
                delay = static_cast<std::uint32_t>(std::clamp(std::stod(retry) * 1000.0, 1000.0, 30000.0));
            } catch (...) {
            }
        }
        return {.retry_after_ms = delay, .error = L"Discord rate limit reached"};
    }
    return {.error = L"Discord returned HTTP " + std::to_wstring(status)};
}

bool OpenWebhookRequest(const WebhookParts& webhook, std::wstring_view method,
                        InternetHandle& session, InternetHandle& connection,
                        InternetHandle& request, std::wstring& error) {
    const auto user_agent = L"OpenReplay (https://github.com/G4F-Elite/OpenReplay, " +
                            openreplay::FromUtf8(openreplay::kVersion) + L")";
    session.value = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        error = SystemMessage(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.value, 15000, 15000, 120000, 120000);
    connection.value = WinHttpConnect(session.value, webhook.host.c_str(), webhook.port, 0);
    if (!connection.value) {
        error = SystemMessage(GetLastError());
        return false;
    }
    auto path = WaitPath(webhook.path);
    WipeString wipe_path{path};
    request.value = WinHttpOpenRequest(connection.value, std::wstring{method}.c_str(), path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.value) {
        error = SystemMessage(GetLastError());
        return false;
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy, sizeof(redirect_policy))) {
        error = SystemMessage(GetLastError());
        return false;
    }
    return true;
}

std::pair<std::uint32_t, std::uint32_t> FitDimensions(std::uint32_t width, std::uint32_t height,
                                                      std::uint32_t bitrate) {
    if (!width || !height) return {1920, 1080};
    const auto max_width = bitrate < 5000000 ? 1280U : 1920U;
    const auto max_height = bitrate < 5000000 ? 720U : 1080U;
    const auto scale = std::min({1.0, static_cast<double>(max_width) / width,
                                static_cast<double>(max_height) / height});
    const auto even = [](double value) {
        return std::max(2U, static_cast<std::uint32_t>(value) & ~1U);
    };
    return {even(width * scale), even(height * scale)};
}

std::string MultipartJson(std::string_view filename, std::uint64_t size) {
    const auto size_text = std::format("{:.1f} MiB", static_cast<double>(size) / kMebibyte);
    return "{\"username\":\"OpenReplay\",\"allowed_mentions\":{\"parse\":[]},"
           "\"embeds\":[{\"title\":\"Replay captured\","
           "\"description\":\"Compressed automatically for Discord.\",\"color\":49418,"
           "\"fields\":[{\"name\":\"File\",\"value\":\"`" + JsonEscape(filename) +
           "`\",\"inline\":true},{\"name\":\"Size\",\"value\":\"" + JsonEscape(size_text) +
           "\",\"inline\":true}],\"footer\":{\"text\":\"OpenReplay " +
           JsonEscape(openreplay::kVersion) + "\"}}],\"attachments\":[{\"id\":0,\"filename\":\"" +
           JsonEscape(filename) + "\",\"description\":\"OpenReplay instant replay\"}]}";
}

std::optional<std::string> MultipartBoundary() {
    std::array<std::uint8_t, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return std::nullopt;
    constexpr char digits[] = "0123456789abcdef";
    std::string result{"----OpenReplay"};
    for (const auto value : random) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0F]);
    }
    return result;
}

DiscordUploadResult UploadOnce(const WebhookParts& parts, const std::filesystem::path& file) {
    FileHandle input;
    input.value = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) return {.error = L"Unable to open the Discord clip"};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.value, &size) || size.QuadPart < 0) {
        return {.error = SystemMessage(GetLastError())};
    }
    const auto file_size = static_cast<std::uint64_t>(size.QuadPart);
    const auto boundary = MultipartBoundary();
    if (!boundary) return {.error = L"Unable to create a secure upload boundary"};
    const auto filename = openreplay::ToUtf8(file.filename().wstring());
    const auto payload = MultipartJson(filename, file_size);
    const std::string prefix = std::string{"--"} + *boundary +
        "\r\nContent-Disposition: form-data; name=\"payload_json\"\r\n"
        "Content-Type: application/json\r\n\r\n" + payload + "\r\n--" + *boundary +
        "\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"" +
        JsonEscape(filename) + "\"\r\nContent-Type: video/mp4\r\n\r\n";
    const std::string suffix = std::string{"\r\n--"} + *boundary + "--\r\n";
    const auto total = static_cast<std::uint64_t>(prefix.size()) + file_size + suffix.size();
    if (total > std::numeric_limits<DWORD>::max()) return {.error = L"Discord clip is too large to upload"};

    std::wstring error;
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    if (!OpenWebhookRequest(parts, L"POST", session, connection, request, error)) {
        return {.error = std::move(error)};
    }
    const auto headers = L"Content-Type: multipart/form-data; boundary=" +
                         openreplay::FromUtf8(*boundary) + L"\r\n";
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(total), 0)) {
        return {.error = SystemMessage(GetLastError())};
    }
    if (!WriteHttp(request.value, prefix, error)) return {.error = std::move(error)};
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t remaining = file_size;
    while (remaining > 0) {
        const auto requested = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        if (!ReadFile(input.value, buffer.data(), requested, &read, nullptr) || read == 0) {
            return {.error = L"Unable to read the Discord clip"};
        }
        if (!WriteHttp(request.value, {buffer.data(), read}, error)) return {.error = std::move(error)};
        remaining -= read;
    }
    if (!WriteHttp(request.value, suffix, error)) return {.error = std::move(error)};
    return CompleteRequest(request.value);
}

}  // namespace

bool DiscordShareService::ValidateWebhook(std::wstring_view url, std::wstring& error) {
    WebhookParts ignored;
    WipeString wipe_path{ignored.path};
    return ParseWebhook(url, ignored, error);
}

bool DiscordShareService::HasWebhook(std::wstring* error) const {
    PCREDENTIALW credential = nullptr;
    const bool result = CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) != FALSE;
    const auto failure = result ? ERROR_SUCCESS : GetLastError();
    if (credential) {
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
    } else if (error && failure != ERROR_NOT_FOUND) {
        *error = SystemMessage(failure);
    }
    return result;
}

std::wstring DiscordShareService::LoadWebhook(std::wstring& error, bool* invalid) const {
    if (invalid) *invalid = false;
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) {
        const auto failure = GetLastError();
        if (failure == ERROR_NOT_FOUND) {
            if (invalid) *invalid = true;
        } else {
            error = SystemMessage(failure);
        }
        return {};
    }
    std::wstring result;
    if (credential->CredentialBlob && credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
        result.assign(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
                      credential->CredentialBlobSize / sizeof(wchar_t));
    }
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    if (!ValidateWebhook(result, error)) {
        if (invalid) *invalid = true;
        if (!result.empty()) SecureZeroMemory(result.data(), result.size() * sizeof(wchar_t));
        return {};
    }
    return result;
}

bool DiscordShareService::StoreWebhook(std::wstring_view url, std::wstring& error) const {
    if (!ValidateWebhook(url, error)) return false;
    if (url.size() * sizeof(wchar_t) > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        error = L"Discord webhook URL is too long";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(url.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(url.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        error = SystemMessage(GetLastError());
        return false;
    }
    return true;
}

bool DiscordShareService::RemoveWebhook(std::wstring& error) const {
    if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
    error = SystemMessage(GetLastError());
    return false;
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFile>
DiscordShareService::ExportAsync(const std::filesystem::path& input, std::uint32_t target_megabytes,
                                 std::uint32_t source_fps) const {
    using namespace winrt::Windows::Media::MediaProperties;
    using namespace winrt::Windows::Media::Editing;
    using namespace winrt::Windows::Media::Transcoding;
    using namespace winrt::Windows::Storage;

    const auto source = co_await StorageFile::GetFileFromPathAsync(input.wstring());
    const auto target_bytes = static_cast<std::uint64_t>(std::clamp(target_megabytes, 5U, 500U)) * kMebibyte;
    const auto source_size = co_await source.GetBasicPropertiesAsync();
    if (source_size.Size() <= target_bytes) co_return source;
    const auto properties = co_await source.Properties().GetVideoPropertiesAsync();
    const auto clip = co_await MediaClip::CreateFromFileAsync(source);
    clip.SelectedEmbeddedAudioTrackIndex(0);
    MediaComposition composition;
    composition.Clips().Append(clip);
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(properties.Duration());
    auto bitrate = openreplay::DiscordVideoBitrate(duration, target_megabytes, kDiscordAudioBitrate);
    if (!bitrate) throw winrt::hresult_error(E_INVALIDARG, L"Clip is too long for the selected Discord size limit");

    const auto directory = input.parent_path() / L"Discord";
    std::filesystem::create_directories(directory);
    const auto folder = co_await StorageFolder::GetFolderFromPathAsync(directory.wstring());
    const auto desired_name = input.stem().wstring() + L"-discord.mp4";
    auto destination = co_await folder.CreateFileAsync(desired_name, CreationCollisionOption::GenerateUniqueName);
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::exception_ptr failure;
        try {
            auto profile = MediaEncodingProfile::CreateMp4(VideoEncodingQuality::Auto);
            const auto dimensions = FitDimensions(properties.Width(), properties.Height(), bitrate);
            profile.Video().Width(dimensions.first);
            profile.Video().Height(dimensions.second);
            profile.Video().Bitrate(bitrate);
            profile.Video().FrameRate().Numerator(std::min(source_fps, 60U));
            profile.Video().FrameRate().Denominator(1);
            profile.Audio().Bitrate(kDiscordAudioBitrate);
            profile.Audio().ChannelCount(2);
            profile.Audio().SampleRate(48000);

            const auto transcode_result = co_await composition.RenderToFileAsync(
                destination, MediaTrimmingPreference::Precise, profile);
            if (transcode_result != TranscodeFailureReason::None) {
                const auto reason = transcode_result == TranscodeFailureReason::InvalidProfile
                    ? L"invalid encoding profile"
                    : transcode_result == TranscodeFailureReason::CodecNotFound
                        ? L"H.264/AAC codec unavailable"
                        : L"unknown Windows media error";
                throw winrt::hresult_error(E_FAIL, std::wstring{L"Windows could not encode the Discord clip: "} + reason);
            }
            const auto output_path = std::filesystem::path{destination.Path().c_str()};
            const auto size = std::filesystem::file_size(output_path);
            if (size <= target_bytes) co_return destination;
            if (attempt == 2) {
                throw winrt::hresult_error(E_FAIL, L"Unable to fit the replay under the selected Discord limit");
            }
            bitrate = static_cast<std::uint32_t>(static_cast<std::uint64_t>(bitrate) * target_bytes * 95ULL /
                                                 (size * 100ULL));
            if (bitrate < 250000) {
                throw winrt::hresult_error(E_FAIL, L"The selected Discord limit is too small for this clip");
            }
        } catch (...) { failure = std::current_exception(); }
        try {
            co_await destination.DeleteAsync(StorageDeleteOption::PermanentDelete);
        } catch (...) {
            if (!failure) throw;
        }
        if (failure) std::rethrow_exception(failure);
        destination = co_await folder.CreateFileAsync(desired_name, CreationCollisionOption::GenerateUniqueName);
    }
    throw winrt::hresult_error(E_FAIL, L"Discord export failed");
}

DiscordUploadResult DiscordShareService::Upload(const std::wstring& webhook,
                                                 const std::filesystem::path& file) const {
    std::wstring error;
    WebhookParts parts;
    WipeString wipe_path{parts.path};
    if (!ParseWebhook(webhook, parts, error)) return {.error = std::move(error)};
    auto result = UploadOnce(parts, file);
    if (result.retry_after_ms) {
        Sleep(result.retry_after_ms);
        result = UploadOnce(parts, file);
    }
    return result;
}

DiscordUploadResult DiscordShareService::Test(const std::wstring& webhook) const {
    std::wstring error;
    WebhookParts parts;
    WipeString wipe_path{parts.path};
    if (!ParseWebhook(webhook, parts, error)) return {.error = std::move(error)};
    const auto payload = std::string{
        "{\"username\":\"OpenReplay\",\"allowed_mentions\":{\"parse\":[]},\"embeds\":[{"
        "\"title\":\"OpenReplay webhook connected\",\"description\":\"Automatic replay sharing is ready.\","
        "\"color\":49418}]}"};
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    if (!OpenWebhookRequest(parts, L"POST", session, connection, request, error)) {
        return {.error = std::move(error)};
    }
    constexpr wchar_t headers[] = L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1),
                            const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
                            static_cast<DWORD>(payload.size()), 0)) {
        return {.error = SystemMessage(GetLastError())};
    }
    return CompleteRequest(request.value);
}

bool DiscordShareService::CopyFileToClipboard(HWND owner, const std::filesystem::path& file) noexcept {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const auto path = file.wstring();
    const auto bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    bool copied = false;
    if (memory) {
        if (auto* data = static_cast<std::uint8_t*>(GlobalLock(memory))) {
            auto* drop = reinterpret_cast<DROPFILES*>(data);
            drop->pFiles = sizeof(DROPFILES);
            drop->fWide = TRUE;
            memcpy(data + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_HDROP, memory) != nullptr;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

}  // namespace openreplay::ui

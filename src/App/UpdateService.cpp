#include "pch.h"

#include "UpdateService.h"

#include "openreplay/SettingsStore.h"
#include "openreplay/Version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace openreplay::ui {
namespace {

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

struct KernelHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~KernelHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    KernelHandle() = default;
    explicit KernelHandle(HANDLE handle) : value(handle) {}
    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;
    explicit operator bool() const noexcept { return value != INVALID_HANDLE_VALUE; }
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

bool OpenRequest(std::wstring_view url, InternetHandle& session, InternetHandle& connection,
                 InternetHandle& request, std::wstring& error) {
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    std::wstring mutable_url{url};
    if (!WinHttpCrackUrl(mutable_url.data(), static_cast<DWORD>(mutable_url.size()), 0, &components)) {
        error = SystemMessage(GetLastError());
        return false;
    }
    const std::wstring host{components.lpszHostName, components.dwHostNameLength};
    std::wstring path{components.lpszUrlPath, components.dwUrlPathLength};
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    session.value = WinHttpOpen((L"OpenReplay/" + openreplay::FromUtf8(openreplay::kVersion)).c_str(),
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        error = SystemMessage(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.value, 10000, 10000, 30000, 30000);
    connection.value = WinHttpConnect(session.value, host.c_str(), components.nPort, 0);
    if (!connection.value) {
        error = SystemMessage(GetLastError());
        return false;
    }
    request.value = WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.value || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        error = SystemMessage(GetLastError());
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        error = L"Update server returned HTTP " + std::to_wstring(status);
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> DownloadMemory(std::wstring_view url, std::size_t maximum,
                                                        std::wstring& error) {
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    if (!OpenRequest(url, session, connection, request, error)) return std::nullopt;
    std::vector<std::uint8_t> result;
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            error = SystemMessage(GetLastError());
            return std::nullopt;
        }
        if (!read) break;
        if (result.size() + read > maximum) {
            error = L"Update metadata is too large";
            return std::nullopt;
        }
        result.insert(result.end(), buffer.begin(), buffer.begin() + read);
    }
    return result;
}

std::wstring FileHash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size = 0;
    DWORD actual = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &actual, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(object_size);
    std::array<UCHAR, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                      static_cast<ULONG>(read), 0) < 0) {
            input.setstate(std::ios::badbit);
        }
    }
    const bool ok = input.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

std::wstring Quote(const std::filesystem::path& value) {
    return L'"' + value.wstring() + L'"';
}

bool RunProcess(const std::filesystem::path& executable, std::wstring arguments,
                DWORD timeout, std::wstring& error) {
    std::wstring command = Quote(executable) + L" " + std::move(arguments);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, executable.parent_path().c_str(), &startup, &process)) {
        error = SystemMessage(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, timeout);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    if (wait == WAIT_OBJECT_0 && exit_code == 0) return true;
    error = wait == WAIT_TIMEOUT ? L"Updater extraction timed out" : L"Unable to extract the updater";
    return false;
}

bool ExtractUpdater(const UpdateManifest& manifest, const std::filesystem::path& archive,
                    const std::filesystem::path& directory, std::filesystem::path& helper,
                    std::wstring& error) {
    KernelHandle archive_lock{CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!archive_lock) {
        error = SystemMessage(GetLastError());
        return false;
    }

    std::error_code file_error;
    if (!std::filesystem::is_regular_file(archive, file_error) || file_error ||
        std::filesystem::file_size(archive, file_error) != manifest.asset_size || file_error ||
        _wcsicmp(FileHash(archive).c_str(), openreplay::FromUtf8(manifest.asset_sha256).c_str()) != 0) {
        error = L"Update archive verification failed before updater extraction";
        return false;
    }

    std::array<wchar_t, 32768> windows{};
    const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size()) {
        error = SystemMessage(GetLastError());
        return false;
    }
    const auto tar = std::filesystem::path{std::wstring{windows.data(), length}} / L"System32" / L"tar.exe";
    if (!RunProcess(tar, L"-xf " + Quote(archive) + L" -C " + Quote(directory) +
                              L" OpenReplay.Updater.exe", 60000, error)) {
        return false;
    }
    helper = directory / L"OpenReplay.Updater.exe";
    if (!std::filesystem::is_regular_file(helper, file_error) || file_error) {
        error = L"The update archive does not contain OpenReplay.Updater.exe";
        return false;
    }
    return true;
}

}  // namespace

std::filesystem::path UpdateService::UpdateRoot() {
    return openreplay::SettingsStore::DefaultPath().parent_path() / L"updates";
}

UpdateCheckResult UpdateService::Check(bool developer_channel) const {
    const auto check_channel = [](bool developer) {
        UpdateCheckResult result;
        std::wstring error;
        const auto manifest_url = developer ? openreplay::kDevUpdateManifestUrl
                                            : openreplay::kUpdateManifestUrl;
        const auto signature_url = developer ? openreplay::kDevUpdateSignatureUrl
                                             : openreplay::kUpdateSignatureUrl;
        const auto manifest = DownloadMemory(openreplay::FromUtf8(manifest_url), 1024 * 1024, error);
        if (!manifest) {
            result.error = std::move(error);
            return result;
        }
        const auto signature = DownloadMemory(openreplay::FromUtf8(signature_url), 4096, error);
        if (!signature || !openreplay::VerifyUpdateSignature(*manifest, *signature)) {
            result.error = signature ? L"Release metadata signature is invalid" : std::move(error);
            return result;
        }
        const std::string json(reinterpret_cast<const char*>(manifest->data()), manifest->size());
        auto parsed = openreplay::ParseUpdateManifest(json);
        if (!parsed) {
            result.error = L"Release metadata is invalid";
            return result;
        }
        result.ok = true;
        result.manifest = std::move(*parsed);
        result.update_available = openreplay::IsValidUpdate(
            result.manifest, openreplay::kVersion, developer ? "dev" : "stable");
        return result;
    };

    auto stable = check_channel(false);
    if (!developer_channel) return stable;
    auto developer = check_channel(true);
    if (stable.update_available &&
        (!developer.update_available || openreplay::IsStableReleasePreferred(
                                            stable.manifest.version, developer.manifest.version))) {
        return stable;
    }
    if (developer.update_available) return developer;
    if (stable.ok) return stable;
    return developer;
}

UpdateDownloadResult UpdateService::Download(const UpdateManifest& manifest, bool developer_channel) const {
    UpdateDownloadResult result;
    if (!openreplay::IsValidUpdate(manifest, openreplay::kVersion,
                                   developer_channel ? "dev" : "stable")) {
        result.error = L"Release metadata is not trusted";
        return result;
    }
    std::error_code file_error;
    const auto directory = UpdateRoot() / L"downloads";
    std::filesystem::create_directories(directory, file_error);
    if (file_error) {
        result.error = L"Unable to create the update download directory";
        return result;
    }
    const auto destination = directory / openreplay::FromUtf8(manifest.asset_name);
    auto partial = destination;
    partial += L".part";
    std::filesystem::remove(partial, file_error);

    std::wstring error;
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    if (!OpenRequest(openreplay::FromUtf8(manifest.asset_url), session, connection, request, error)) {
        result.error = std::move(error);
        return result;
    }
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = L"Unable to create the update archive";
        return result;
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    std::uint64_t total = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            result.error = SystemMessage(GetLastError());
            break;
        }
        if (!read) break;
        total += read;
        if (total > manifest.asset_size) {
            result.error = L"Downloaded update is larger than signed metadata";
            break;
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), read);
        if (!output) {
            result.error = L"Unable to write the update archive";
            break;
        }
    }
    output.close();
    if (!result.error.empty() || total != manifest.asset_size ||
        FileHash(partial) != openreplay::FromUtf8(manifest.asset_sha256)) {
        if (result.error.empty()) result.error = L"Downloaded update failed SHA-256 verification";
        std::filesystem::remove(partial, file_error);
        return result;
    }
    std::filesystem::remove(destination, file_error);
    std::filesystem::rename(partial, destination, file_error);
    if (file_error) {
        result.error = L"Unable to publish the verified update archive";
        return result;
    }
    result.ok = true;
    result.archive = destination;
    return result;
}

bool UpdateService::LaunchUpdater(const UpdateManifest& manifest, const std::filesystem::path& archive,
                                   DWORD app_pid, const std::filesystem::path& install_root,
                                   const std::filesystem::path& health_file, std::wstring& error) const {
    std::error_code file_error;
    const auto helper_directory = UpdateRoot() / L"helper" / openreplay::FromUtf8(manifest.version) /
                                  (std::to_wstring(app_pid) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(helper_directory, file_error);
    if (file_error) {
        error = L"Unable to create the updater directory";
        return false;
    }
    std::filesystem::path helper;
    if (!ExtractUpdater(manifest, archive, helper_directory, helper, error)) return false;
    const std::wstring arguments = L"--archive=" + Quote(archive) + L" --target=" + Quote(install_root) +
        L" --health=" + Quote(health_file) + L" --sha256=" + openreplay::FromUtf8(manifest.asset_sha256) +
        L" --version=" + openreplay::FromUtf8(manifest.version) + L" --pid=" + std::to_wstring(app_pid) +
        L" --size=" + std::to_wstring(manifest.asset_size);
    std::wstring command = Quote(helper) + L" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        helper_directory.c_str(), &startup, &process)) {
        error = SystemMessage(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace openreplay::ui

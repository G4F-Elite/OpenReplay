#include "Updater.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace openreplay::updater {
namespace {

using namespace std::chrono_literals;

struct Arguments {
    std::filesystem::path archive;
    std::filesystem::path target;
    std::filesystem::path health;
    std::wstring expected_hash;
    std::wstring version;
    DWORD app_pid{};
    std::uint64_t expected_size{};
};

struct ScopedHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~ScopedHandle() { Reset(); }
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : value(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    explicit operator bool() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value);
        value = handle;
    }
};

std::filesystem::path LocalAppData() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    return length && length < buffer.size() ? std::filesystem::path{std::wstring{buffer.data(), length}}
                                             : std::filesystem::temp_directory_path();
}

void Log(std::wstring_view message) noexcept {
    try {
        const auto directory = LocalAppData() / L"OpenReplay" / L"updates";
        std::filesystem::create_directories(directory);
        std::wofstream output(directory / L"updater.log", std::ios::app);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        output << L'[' << now.wYear << L'-' << now.wMonth << L'-' << now.wDay << L' '
               << now.wHour << L':' << now.wMinute << L':' << now.wSecond << L"] " << message << L'\n';
    } catch (...) {
    }
}

std::map<std::wstring, std::wstring> CommandLineArguments() {
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::map<std::wstring, std::wstring> result;
    if (!raw) return result;
    for (int index = 1; index < count; ++index) {
        std::wstring argument{raw[index]};
        if (!argument.starts_with(L"--")) continue;
        const auto separator = argument.find(L'=');
        result.emplace(argument.substr(2, separator == std::wstring::npos ? separator : separator - 2),
                       separator == std::wstring::npos ? std::wstring{} : argument.substr(separator + 1));
    }
    LocalFree(raw);
    return result;
}

std::optional<Arguments> ParseArguments() {
    const auto values = CommandLineArguments();
    const auto value = [&](std::wstring_view key) -> std::optional<std::wstring> {
        const auto found = values.find(std::wstring{key});
        return found == values.end() ? std::nullopt : std::optional<std::wstring>{found->second};
    };
    const auto archive = value(L"archive");
    const auto target = value(L"target");
    const auto health = value(L"health");
    const auto hash = value(L"sha256");
    const auto version = value(L"version");
    const auto pid = value(L"pid");
    const auto size = value(L"size");
    if (!archive || !target || !health || !hash || !version || !pid || !size) return std::nullopt;
    try {
        return Arguments{*archive, *target, *health, *hash, *version,
                         static_cast<DWORD>(std::stoul(*pid)), std::stoull(*size)};
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring HashFile(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &result_size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                          sizeof(hash_size), &result_size, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                      static_cast<ULONG>(read), 0) < 0) {
            input.setstate(std::ios::badbit);
        }
    }
    const bool ok = input.eof() && BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

bool ArchiveMatches(const Arguments& update) {
    std::error_code error;
    return std::filesystem::is_regular_file(update.archive, error) && !error &&
           std::filesystem::file_size(update.archive, error) == update.expected_size && !error &&
           HashFile(update.archive) == update.expected_hash;
}

std::wstring Quote(const std::filesystem::path& value) {
    return L'"' + value.wstring() + L'"';
}

bool RunProcess(const std::filesystem::path& executable, std::wstring arguments, DWORD timeout,
                const std::filesystem::path& output = {}) {
    HANDLE output_handle = INVALID_HANDLE_VALUE;
    STARTUPINFOW startup{sizeof(startup)};
    if (!output.empty()) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        output_handle = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output_handle == INVALID_HANDLE_VALUE) return false;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output_handle;
        startup.hStdError = output_handle;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::wstring command = Quote(executable) + (arguments.empty() ? L"" : L" " + arguments);
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                        output_handle != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                                        executable.parent_path().c_str(), &startup, &process) != FALSE;
    if (output_handle != INVALID_HANDLE_VALUE) CloseHandle(output_handle);
    if (!created) return false;
    const auto wait = WaitForSingleObject(process.hProcess, timeout);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
    else TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exit_code == 0;
}

bool ValidateArchiveEntries(const std::filesystem::path& archive, const std::filesystem::path& list_path) {
    std::array<wchar_t, 32768> windows{};
    const auto windows_length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!windows_length || windows_length >= windows.size()) return false;
    const auto tar = std::filesystem::path{std::wstring{windows.data(), windows_length}} / L"System32" / L"tar.exe";
    if (!RunProcess(tar, L"-tf " + Quote(archive), 60000, list_path)) return false;
    auto verbose_path = list_path;
    verbose_path += L".verbose";
    if (!RunProcess(tar, L"-tvf " + Quote(archive), 60000, verbose_path)) return false;
    {
        std::wifstream verbose(verbose_path);
        for (std::wstring line; std::getline(verbose, line);) {
            if (!line.empty() && line.front() != L'-' && line.front() != L'd') return false;
        }
    }
    std::wifstream input(list_path);
    std::set<std::wstring> entries;
    for (std::wstring entry; std::getline(input, entry);) {
        std::ranges::replace(entry, L'\\', L'/');
        while (entry.starts_with(L"./")) entry.erase(0, 2);
        while (entry.ends_with(L'/')) entry.pop_back();
        if (entry.empty()) continue;
        if (entry.starts_with(L'/') || entry.find(L':') != std::wstring::npos) return false;
        std::filesystem::path relative{entry};
        for (const auto& component : relative) {
            const auto text = component.wstring();
            if (text == L".." || text == L"." || text.empty() || text.ends_with(L'.') || text.ends_with(L' ')) {
                return false;
            }
        }
        std::ranges::transform(entry, entry.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
        if (!entries.insert(entry).second) return false;
    }
    return !entries.empty();
}

bool ValidateExtractedTree(const std::filesystem::path& staging) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(staging, error);
    if (error) return false;
    for (std::filesystem::recursive_directory_iterator iterator(staging, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
        const auto canonical = std::filesystem::weakly_canonical(iterator->path(), error);
        if (error) return false;
        const auto root_text = root.native();
        const auto path_text = canonical.native();
        if (path_text.size() <= root_text.size() || _wcsnicmp(path_text.c_str(), root_text.c_str(), root_text.size()) != 0 ||
            (path_text[root_text.size()] != L'\\' && path_text[root_text.size()] != L'/')) {
            return false;
        }
    }
    return !error;
}

bool ExtractArchive(const std::filesystem::path& archive, const std::filesystem::path& staging) {
    std::array<wchar_t, 32768> windows{};
    const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size()) return false;
    const auto tar = std::filesystem::path{std::wstring{windows.data(), length}} / L"System32" / L"tar.exe";
    return RunProcess(tar, L"-xf " + Quote(archive) + L" -C " + Quote(staging), 300000);
}

bool HostRunning() noexcept {
    const auto mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\OpenReplay.Host.Singleton.v1");
    if (!mutex) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(mutex);
    return true;
}

void RequestHostShutdown() noexcept {
    if (!WaitNamedPipeW(L"\\\\.\\pipe\\OpenReplay.Host.v1", 500)) return;
    const auto pipe = CreateFileW(L"\\\\.\\pipe\\OpenReplay.Host.v1", GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    constexpr char command[] = "shutdown";
    DWORD written = 0;
    WriteFile(pipe, command, sizeof(command) - 1, &written, nullptr);
    CloseHandle(pipe);
}

void TerminateHostInDirectory(const std::filesystem::path& directory) noexcept {
    std::error_code expected_error;
    const auto expected = std::filesystem::weakly_canonical(directory, expected_error);
    if (expected_error) return;

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"OpenReplay.Host.exe") != 0) continue;
            const auto process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::array<wchar_t, 32768> path{};
            DWORD length = static_cast<DWORD>(path.size());
            if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
                CloseHandle(process);
                continue;
            }
            std::error_code process_error;
            const auto process_directory = std::filesystem::weakly_canonical(
                std::filesystem::path{std::wstring{path.data(), length}}.parent_path(), process_error);
            if (process_error || _wcsicmp(process_directory.c_str(), expected.c_str()) != 0) {
                CloseHandle(process);
                continue;
            }
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool WaitForHostExit(const std::filesystem::path& directory, std::chrono::seconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (HostRunning() && std::chrono::steady_clock::now() < deadline) Sleep(100);
    if (!HostRunning()) return true;

    Log(L"Capture Host did not stop gracefully; terminating the process from the installation directory");
    TerminateHostInDirectory(directory);
    return !HostRunning();
}

bool LaunchApp(const std::filesystem::path& target, std::wstring_view arguments,
               PROCESS_INFORMATION& process) {
    const auto executable = target / L"OpenReplay.App.exe";
    std::wstring command = Quote(executable) + L" " + std::wstring{arguments};
    STARTUPINFOW startup{sizeof(startup)};
    return CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          target.c_str(), &startup, &process) != FALSE;
}

bool WaitForHealth(const std::filesystem::path& health, std::wstring_view expected_version, HANDLE process) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::wifstream input(health);
        if (input) {
            std::wstring actual;
            std::getline(input, actual);
            if (actual == expected_version) return true;
        }
        if (WaitForSingleObject(process, 250) == WAIT_OBJECT_0) return false;
    }
    return false;
}

}  // namespace

int Run(HINSTANCE) {
    const auto arguments = ParseArguments();
    if (!arguments) {
        Log(L"Invalid updater command line");
        return 2;
    }
    const auto& update = *arguments;
    Log(L"Starting update to " + update.version);

    ScopedHandle archive_lock{CreateFileW(update.archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    std::error_code error;
    if (!archive_lock || !ArchiveMatches(update)) {
        Log(L"Update archive verification failed");
        if (const auto app = OpenProcess(SYNCHRONIZE, FALSE, update.app_pid)) {
            if (WaitForSingleObject(app, 30000) == WAIT_OBJECT_0) {
                PROCESS_INFORMATION restored{};
                if (LaunchApp(update.target, L"--background", restored)) {
                    CloseHandle(restored.hThread);
                    CloseHandle(restored.hProcess);
                }
            }
            CloseHandle(app);
        }
        return 4;
    }
    if (const auto app = OpenProcess(SYNCHRONIZE, FALSE, update.app_pid)) {
        WaitForSingleObject(app, 30000);
        CloseHandle(app);
    }
    RequestHostShutdown();
    if (!WaitForHostExit(update.target, 10s)) {
        Log(L"Capture Host did not stop");
        PROCESS_INFORMATION restored{};
        if (LaunchApp(update.target, L"--background", restored)) {
            CloseHandle(restored.hThread);
            CloseHandle(restored.hProcess);
        }
        return 3;
    }

    const auto parent = update.target.parent_path();
    const auto transaction = std::to_wstring(GetCurrentProcessId());
    const auto staging = parent / (update.target.filename().wstring() + L".staging." + transaction);
    const auto backup = parent / (update.target.filename().wstring() + L".backup." + transaction);
    const auto failed = parent / (update.target.filename().wstring() + L".failed." + transaction);
    const auto list_path = LocalAppData() / L"OpenReplay" / L"updates" / (L"archive-" + transaction + L".txt");
    std::filesystem::remove_all(staging, error);
    std::filesystem::remove_all(backup, error);
    std::filesystem::remove_all(failed, error);
    std::filesystem::create_directories(staging, error);
    if (error || !ValidateArchiveEntries(update.archive, list_path) || !ArchiveMatches(update) ||
        !ExtractArchive(update.archive, staging) || !ValidateExtractedTree(staging) || !ArchiveMatches(update) ||
        !std::filesystem::is_regular_file(staging / L"OpenReplay.App.exe") ||
        !std::filesystem::is_regular_file(staging / L"OpenReplay.Host.exe") ||
        !std::filesystem::is_regular_file(staging / L"OpenReplay.Updater.exe") ||
        !std::filesystem::is_regular_file(staging / L"obs-runtime" / L"bin" / L"64bit" / L"obs.dll")) {
        Log(L"Update archive extraction or content validation failed");
        std::filesystem::remove_all(staging, error);
        return 5;
    }
    archive_lock.Reset();

    error.clear();
    std::filesystem::remove(update.health, error);
    if (error) {
        Log(L"Unable to clear the previous update health check");
        std::filesystem::remove_all(staging, error);
        return 5;
    }
    std::filesystem::rename(update.target, backup, error);
    if (error) {
        Log(L"Unable to move the current installation to backup");
        std::filesystem::remove_all(staging, error);
        return 6;
    }
    std::filesystem::rename(staging, update.target, error);
    if (error) {
        Log(L"Unable to publish staged installation; restoring backup");
        std::filesystem::rename(backup, update.target, error);
        return 7;
    }

    PROCESS_INFORMATION process{};
    const auto launch_arguments = L"--background --post-update=" + update.version +
                                  L" --update-health=" + Quote(update.health);
    if (LaunchApp(update.target, launch_arguments, process) &&
        WaitForHealth(update.health, update.version, process.hProcess)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::filesystem::remove_all(backup, error);
        std::filesystem::remove(update.archive, error);
        std::filesystem::remove(list_path, error);
        Log(L"Update completed successfully");
        return 0;
    }

    Log(L"Updated application failed its health check; rolling back");
    if (process.hProcess) {
        RequestHostShutdown();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (!WaitForHostExit(update.target, 5s)) {
        Log(L"Rollback could not stop the updated Capture Host; backup was retained at " + backup.wstring());
        return 8;
    }
    error.clear();
    std::filesystem::rename(update.target, failed, error);
    if (error) {
        Log(L"Rollback could not move the failed installation; backup was retained at " + backup.wstring());
        return 8;
    }
    std::filesystem::rename(backup, update.target, error);
    if (error) {
        Log(L"Rollback failed; backup was retained at " + backup.wstring());
        std::error_code restore_error;
        std::filesystem::rename(failed, update.target, restore_error);
        return 8;
    }
    PROCESS_INFORMATION restored{};
    if (LaunchApp(update.target, L"--background", restored)) {
        CloseHandle(restored.hThread);
        CloseHandle(restored.hProcess);
    }
    std::filesystem::remove_all(failed, error);
    return 9;
}

}  // namespace openreplay::updater

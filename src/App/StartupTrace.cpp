#include "pch.h"

#include "StartupTrace.h"

namespace openreplay::ui {

void TraceStartup(std::wstring_view message) noexcept {
    wchar_t local_app_data[MAX_PATH]{};
    const DWORD path_size = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (path_size == 0 || path_size >= MAX_PATH) return;

    const std::filesystem::path directory = std::filesystem::path{local_app_data} / L"OpenReplay";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;

    const HANDLE file = CreateFileW((directory / L"app.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    const std::wstring line = std::wstring{prefix} + std::wstring{message} + L"\r\n";
    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                               nullptr, 0, nullptr, nullptr);
    if (utf8_size > 0) {
        std::string utf8(static_cast<size_t>(utf8_size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(),
                            utf8_size, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

}  // namespace openreplay::ui

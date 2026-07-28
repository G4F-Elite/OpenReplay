#pragma once

#include "pch.h"

namespace openreplay::ui {

struct DiscordUploadResult {
    bool ok{false};
    bool webhook_invalid{false};
    std::uint32_t retry_after_ms{0};
    std::wstring error;
};

class DiscordShareService {
public:
    [[nodiscard]] bool HasWebhook(std::wstring* error = nullptr) const;
    [[nodiscard]] std::wstring LoadWebhook(std::wstring& error, bool* invalid = nullptr) const;
    bool StoreWebhook(std::wstring_view url, std::wstring& error) const;
    bool RemoveWebhook(std::wstring& error) const;

    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFile> ExportAsync(
        const std::filesystem::path& input, std::uint32_t target_megabytes, std::uint32_t source_fps) const;
    [[nodiscard]] DiscordUploadResult Upload(const std::wstring& webhook,
                                             const std::filesystem::path& file) const;
    [[nodiscard]] DiscordUploadResult Test(const std::wstring& webhook) const;
    static bool CopyFileToClipboard(HWND owner, const std::filesystem::path& file) noexcept;

private:
    static bool ValidateWebhook(std::wstring_view url, std::wstring& error);
};

}  // namespace openreplay::ui

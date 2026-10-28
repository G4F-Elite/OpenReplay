#include "pch.h"

#include "GdiFonts.h"

#include <algorithm>

namespace openreplay::ui {
namespace {

constexpr std::array kFontFiles{L"SpaceGrotesk-Variable.ttf", L"Rubik-Variable.ttf"};

std::filesystem::path AssetsFolder() {
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return {};
    return std::filesystem::path{std::wstring{executable.data(), length}}.parent_path() / L"Assets";
}

HFONT MakeFont(int pixel_height, int weight, const wchar_t* face) noexcept {
    return CreateFontW(-pixel_height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

}  // namespace

void LoadGdiFonts() noexcept {
    const auto folder = AssetsFolder();
    if (folder.empty()) return;
    for (const auto file : kFontFiles) AddFontResourceExW((folder / file).c_str(), FR_PRIVATE, nullptr);
}

void UnloadGdiFonts() noexcept {
    const auto folder = AssetsFolder();
    if (folder.empty()) return;
    for (const auto file : kFontFiles) RemoveFontResourceExW((folder / file).c_str(), FR_PRIVATE, nullptr);
}

// GDI reaches a variable font's weights only through its named instances, each under its own face name, and Space
// Grotesk has no SemiBold instance.
GdiFont::GdiFont(int pixel_height, int weight) noexcept {
    if (weight <= 350) latin_ = MakeFont(pixel_height, FW_LIGHT, L"Space Grotesk Light");
    else if (weight <= 450) latin_ = MakeFont(pixel_height, FW_NORMAL, L"Space Grotesk");
    else if (weight <= 550) latin_ = MakeFont(pixel_height, FW_MEDIUM, L"Space Grotesk Medium");
    else latin_ = MakeFont(pixel_height, FW_BOLD, L"Space Grotesk");

    if (weight <= 350) cyrillic_ = MakeFont(pixel_height, FW_LIGHT, L"Rubik Light");
    else if (weight <= 450) cyrillic_ = MakeFont(pixel_height, FW_NORMAL, L"Rubik");
    else if (weight <= 550) cyrillic_ = MakeFont(pixel_height, FW_MEDIUM, L"Rubik Medium");
    else if (weight <= 650) cyrillic_ = MakeFont(pixel_height, FW_SEMIBOLD, L"Rubik SemiBold");
    else cyrillic_ = MakeFont(pixel_height, FW_BOLD, L"Rubik");
}

GdiFont::~GdiFont() {
    if (latin_) DeleteObject(latin_);
    if (cyrillic_) DeleteObject(cyrillic_);
}

HFONT GdiFont::For(std::wstring_view text) const noexcept {
    const bool cyrillic = std::ranges::any_of(text, [](wchar_t character) {
        return character >= 0x0400 && character <= 0x04FF;
    });
    const auto font = cyrillic ? cyrillic_ : latin_;
    return font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

}  // namespace openreplay::ui

#pragma once

#include <windows.h>

#include <string_view>

namespace openreplay::ui {

// GDI windows (notifications, the performance overlay) use the XAML UI's faces, loaded from Assets for this process.
void LoadGdiFonts() noexcept;
void UnloadGdiFonts() noexcept;

// Space Grotesk has no Cyrillic and GDI does not fall back to another face by itself,
// so each font is a Space Grotesk and Rubik pair and text with Cyrillic letters is drawn in Rubik.
class GdiFont {
public:
    GdiFont(int pixel_height, int weight) noexcept;
    ~GdiFont();
    GdiFont(const GdiFont&) = delete;
    GdiFont& operator=(const GdiFont&) = delete;

    [[nodiscard]] HFONT For(std::wstring_view text) const noexcept;

private:
    HFONT latin_{};
    HFONT cyrillic_{};
};

}  // namespace openreplay::ui

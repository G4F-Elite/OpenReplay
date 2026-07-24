#pragma once

#include "pch.h"

namespace openreplay::ui {

using BrushType = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;

struct UiTheme {
    BrushType root{nullptr};
    BrushType panel{nullptr};
    BrushType card{nullptr};
    BrushType card_hover{nullptr};
    BrushType inset{nullptr};
    BrushType border{nullptr};
    BrushType accent{nullptr};
    BrushType accent_hover{nullptr};
    BrushType accent_pressed{nullptr};
    BrushType status_border{nullptr};
    BrushType accent_text{nullptr};
    BrushType primary_text{nullptr};
    BrushType secondary_text{nullptr};
    BrushType danger{nullptr};
    BrushType quiet{nullptr};
    BrushType muted{nullptr};
    BrushType track{nullptr};
    BrushType track_disabled{nullptr};
};

enum class ButtonKind {
    Quiet,
    Accent,
};

struct SliderControl {
    winrt::Microsoft::UI::Xaml::Controls::Slider input{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid view{nullptr};
};

[[nodiscard]] BrushType Brush(std::uint32_t argb);
[[nodiscard]] UiTheme DarkTheme();
[[nodiscard]] winrt::Microsoft::UI::Xaml::Media::FontFamily PublicSans();
[[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::TextBlock Text(
    std::wstring_view value, double size,
    winrt::Microsoft::UI::Xaml::Media::Brush const& foreground);
void AddRow(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid, double value,
            winrt::Microsoft::UI::Xaml::GridUnitType type);
void AddColumn(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid, double value,
               winrt::Microsoft::UI::Xaml::GridUnitType type);

class ControlFactory {
public:
    explicit ControlFactory(UiTheme theme);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Button ActionButton(
        std::wstring_view label, ButtonKind kind = ButtonKind::Quiet) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch Toggle() const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::ComboBox Select() const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem Option(
        std::wstring_view label) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::TextBox HotkeyInput(
        std::wstring_view placeholder = {}) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::NumberBox NumberInput(
        double minimum, double maximum, double step) const;
    [[nodiscard]] SliderControl RangeSlider(
        double minimum, double maximum, double step, double value) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::CheckBox Check(
        std::wstring_view label, bool checked = true, double font_size = 12) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Border ShortcutSurface() const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Border AudioDeviceSurface() const;

private:
    void StyleInput(winrt::Microsoft::UI::Xaml::Controls::Control const& control,
                    double height) const;
    void StyleSlider(winrt::Microsoft::UI::Xaml::Controls::Slider const& slider) const;

    UiTheme theme_;
};

}  // namespace openreplay::ui

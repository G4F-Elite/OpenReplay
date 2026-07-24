#include "pch.h"

#include "UiControls.h"

namespace openreplay::ui {
namespace {

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

void SetResource(FrameworkElement const& element, std::wstring_view key,
                 winrt::Windows::Foundation::IInspectable const& value) {
    (void)element.Resources().Insert(winrt::box_value(winrt::hstring{key}), value);
}

void CenterTextContentHosts(DependencyObject const& root) {
    if (const auto control = root.try_as<Control>()) control.ApplyTemplate();
    if (const auto element = root.try_as<FrameworkElement>()) {
        const auto name = element.Name();
        if (name == L"ContentElement" || name == L"PlaceholderTextContentPresenter") {
            element.VerticalAlignment(VerticalAlignment::Center);
        }
    }

    const auto child_count = VisualTreeHelper::GetChildrenCount(root);
    for (int32_t index = 0; index < child_count; ++index) {
        CenterTextContentHosts(VisualTreeHelper::GetChild(root, index));
    }
}

void CenterSingleLineInput(Control const& control) {
    control.VerticalContentAlignment(VerticalAlignment::Center);
    control.Loaded([](auto const& sender, auto const&) {
        CenterTextContentHosts(sender.template as<DependencyObject>());
    });
}

void UpdateSliderTrack(Slider const& slider, ColumnDefinition const& filled,
                       ColumnDefinition const& remaining) {
    const auto range = slider.Maximum() - slider.Minimum();
    const auto ratio = range > 0 ? (slider.Value() - slider.Minimum()) / range : 0.0;
    filled.Width(GridLength{ratio, GridUnitType::Star});
    remaining.Width(GridLength{1.0 - ratio, GridUnitType::Star});
}

}  // namespace

BrushType Brush(std::uint32_t argb) {
    winrt::Windows::UI::Color color{};
    color.A = static_cast<std::uint8_t>(argb >> 24);
    color.R = static_cast<std::uint8_t>(argb >> 16);
    color.G = static_cast<std::uint8_t>(argb >> 8);
    color.B = static_cast<std::uint8_t>(argb);
    return BrushType{color};
}

UiTheme DarkTheme() {
    return {
        .root = Brush(0xFF1B1718),
        .panel = Brush(0xFF18181B),
        .card = Brush(0xFF27272A),
        .card_hover = Brush(0xFF303034),
        .inset = Brush(0xFF202023),
        .border = Brush(0xFF3F3F46),
        .accent = Brush(0xFF00C16A),
        .accent_hover = Brush(0xFF00DC82),
        .accent_pressed = Brush(0xFF007F45),
        .status_border = Brush(0xFF016538),
        .accent_text = Brush(0xFF052E16),
        .primary_text = Brush(0xFFFFFFFF),
        .secondary_text = Brush(0xFFA1A1AA),
        .danger = Brush(0xFFFF5C67),
        .quiet = Brush(0xFF3F3F46),
        .muted = Brush(0xFF71717A),
        .track = Brush(0xFF52525B),
        .track_disabled = Brush(0xFF2A2A2E),
    };
}

FontFamily PublicSans() {
    return FontFamily{L"ms-appx:///Assets/PublicSans-Variable.ttf#Public Sans"};
}

TextBlock Text(std::wstring_view value, double size,
               winrt::Microsoft::UI::Xaml::Media::Brush const& foreground) {
    TextBlock text;
    text.Text(winrt::hstring{value});
    text.FontSize(size);
    text.FontFamily(PublicSans());
    text.Foreground(foreground);
    return text;
}

void AddRow(Grid const& grid, double value, GridUnitType type) {
    RowDefinition row;
    row.Height(GridLength{value, type});
    grid.RowDefinitions().Append(row);
}

void AddColumn(Grid const& grid, double value, GridUnitType type) {
    ColumnDefinition column;
    column.Width(GridLength{value, type});
    grid.ColumnDefinitions().Append(column);
}

ControlFactory::ControlFactory(UiTheme theme) : theme_(std::move(theme)) {}

Button ControlFactory::ActionButton(std::wstring_view label, ButtonKind kind) const {
    Button button;
    button.Content(winrt::box_value(winrt::hstring{label}));
    button.Background(kind == ButtonKind::Accent ? theme_.accent : theme_.quiet);
    button.Foreground(kind == ButtonKind::Accent ? theme_.accent_text : theme_.primary_text);
    button.FontFamily(PublicSans());
    button.FontSize(13);
    button.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    button.MinHeight(42);
    button.Padding(Thickness{16, 9, 16, 9});
    button.CornerRadius(CornerRadius{12, 12, 12, 12});
    button.HorizontalContentAlignment(HorizontalAlignment::Center);
    button.VerticalContentAlignment(VerticalAlignment::Center);
    return button;
}

ToggleSwitch ControlFactory::Toggle() const {
    ToggleSwitch toggle;
    toggle.FontFamily(PublicSans());
    toggle.Width(40);
    toggle.MinWidth(40);
    toggle.Height(20);
    toggle.MinHeight(20);
    toggle.OnContent(nullptr);
    toggle.OffContent(nullptr);
    SetResource(toggle, L"ToggleSwitchPreContentMargin", winrt::box_value(0.0));
    SetResource(toggle, L"ToggleSwitchPostContentMargin", winrt::box_value(0.0));
    SetResource(toggle, L"ToggleSwitchFillOn", theme_.accent);
    SetResource(toggle, L"ToggleSwitchFillOnPointerOver", theme_.accent_hover);
    SetResource(toggle, L"ToggleSwitchFillOnPressed", theme_.accent_pressed);
    SetResource(toggle, L"ToggleSwitchStrokeOn", theme_.accent);
    SetResource(toggle, L"ToggleSwitchStrokeOnPointerOver", theme_.accent_hover);
    SetResource(toggle, L"ToggleSwitchStrokeOnPressed", theme_.accent_pressed);
    SetResource(toggle, L"ToggleSwitchKnobFillOn", theme_.primary_text);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPointerOver", theme_.primary_text);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPressed", theme_.primary_text);
    SetResource(toggle, L"ToggleSwitchFillOff", theme_.quiet);
    SetResource(toggle, L"ToggleSwitchFillOffPointerOver", theme_.muted);
    SetResource(toggle, L"ToggleSwitchStrokeOff", theme_.muted);
    SetResource(toggle, L"ToggleSwitchKnobFillOff", theme_.primary_text);
    return toggle;
}

void ControlFactory::StyleInput(Control const& control, double height) const {
    control.FontFamily(PublicSans());
    control.FontSize(13);
    control.Height(height);
    control.MinHeight(height);
    control.CornerRadius(CornerRadius{8, 8, 8, 8});
    control.VerticalContentAlignment(VerticalAlignment::Center);
}

ComboBox ControlFactory::Select() const {
    ComboBox select;
    StyleInput(select, 38);
    select.Padding(Thickness{12, 0, 0, 0});
    select.HorizontalAlignment(HorizontalAlignment::Stretch);
    select.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    return select;
}

ComboBoxItem ControlFactory::Option(std::wstring_view label) const {
    ComboBoxItem option;
    option.Content(winrt::box_value(winrt::hstring{label}));
    option.FontFamily(PublicSans());
    option.FontSize(13);
    option.MinHeight(36);
    option.Padding(Thickness{12, 0, 12, 0});
    option.VerticalContentAlignment(VerticalAlignment::Center);
    return option;
}

TextBox ControlFactory::HotkeyInput(std::wstring_view placeholder) const {
    TextBox input;
    StyleInput(input, 36);
    input.Padding(Thickness{12, 0, 12, 0});
    input.IsReadOnly(true);
    if (!placeholder.empty()) input.PlaceholderText(winrt::hstring{placeholder});
    CenterSingleLineInput(input);
    return input;
}

NumberBox ControlFactory::NumberInput(double minimum, double maximum, double step) const {
    NumberBox input;
    StyleInput(input, 36);
    input.Padding(Thickness{10, 0, 4, 0});
    input.Minimum(minimum);
    input.Maximum(maximum);
    input.SmallChange(step);
    input.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
    CenterSingleLineInput(input);
    return input;
}

void ControlFactory::StyleSlider(Slider const& slider) const {
    const auto transparent = Brush(0x00000000);
    slider.Background(transparent);
    slider.Foreground(theme_.accent);
    slider.HorizontalAlignment(HorizontalAlignment::Stretch);
    SetResource(slider, L"SliderContainerBackground", transparent);
    SetResource(slider, L"SliderContainerBackgroundPointerOver", transparent);
    SetResource(slider, L"SliderContainerBackgroundPressed", transparent);
    SetResource(slider, L"SliderContainerBackgroundDisabled", transparent);
    SetResource(slider, L"SliderTrackFill", theme_.border);
    SetResource(slider, L"SliderTrackFillPointerOver", theme_.track);
    SetResource(slider, L"SliderTrackFillPressed", theme_.track);
    SetResource(slider, L"SliderTrackFillDisabled", theme_.track_disabled);
    SetResource(slider, L"SliderTrackValueFill", theme_.accent);
    SetResource(slider, L"SliderTrackValueFillPointerOver", theme_.accent_hover);
    SetResource(slider, L"SliderTrackValueFillPressed", theme_.accent_pressed);
    SetResource(slider, L"SliderThumbBackground", theme_.accent);
    SetResource(slider, L"SliderThumbBackgroundPointerOver", theme_.accent_hover);
    SetResource(slider, L"SliderThumbBackgroundPressed", theme_.accent_pressed);
    SetResource(slider, L"SliderOuterThumbBackground", transparent);
    SetResource(slider, L"SliderThumbBorderBrush", transparent);
}

SliderControl ControlFactory::RangeSlider(double minimum, double maximum, double step,
                                          double value) const {
    Slider slider;
    slider.Minimum(minimum);
    slider.Maximum(maximum);
    slider.StepFrequency(step);
    slider.Value(value);
    StyleSlider(slider);

    Grid view;
    view.HorizontalAlignment(HorizontalAlignment::Stretch);
    view.Children().Append(slider);

    Grid track;
    track.HorizontalAlignment(HorizontalAlignment::Stretch);
    track.IsHitTestVisible(false);
    Canvas::SetZIndex(track, 1);
    ColumnDefinition filled;
    ColumnDefinition thumb;
    thumb.Width(GridLength{18, GridUnitType::Pixel});
    ColumnDefinition remaining;
    track.ColumnDefinitions().Append(filled);
    track.ColumnDefinitions().Append(thumb);
    track.ColumnDefinitions().Append(remaining);

    Border filled_track;
    filled_track.Height(4);
    filled_track.Background(theme_.accent);
    filled_track.CornerRadius(CornerRadius{2, 2, 2, 2});
    filled_track.VerticalAlignment(VerticalAlignment::Center);
    track.Children().Append(filled_track);

    Border remaining_track;
    remaining_track.Height(4);
    remaining_track.Background(theme_.track);
    remaining_track.CornerRadius(CornerRadius{2, 2, 2, 2});
    remaining_track.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(remaining_track, 2);
    track.Children().Append(remaining_track);
    view.Children().Append(track);

    UpdateSliderTrack(slider, filled, remaining);
    slider.ValueChanged([filled, remaining](auto const& sender, auto const&) {
        UpdateSliderTrack(sender.template as<Slider>(), filled, remaining);
    });
    return {slider, view};
}

CheckBox ControlFactory::Check(std::wstring_view label, bool checked, double font_size) const {
    CheckBox check;
    check.Content(winrt::box_value(winrt::hstring{label}));
    check.FontFamily(PublicSans());
    check.FontSize(font_size);
    check.IsChecked(checked);
    check.VerticalContentAlignment(VerticalAlignment::Center);
    return check;
}

Border ControlFactory::ShortcutSurface() const {
    Border surface;
    surface.Background(theme_.inset);
    surface.BorderBrush(theme_.border);
    surface.BorderThickness(Thickness{1, 1, 1, 1});
    surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    surface.Padding(Thickness{12, 12, 12, 12});
    return surface;
}

Border ControlFactory::AudioDeviceSurface() const {
    Border surface;
    surface.Background(theme_.inset);
    surface.CornerRadius(CornerRadius{7, 7, 7, 7});
    surface.Padding(Thickness{9, 7, 9, 7});
    return surface;
}

}  // namespace openreplay::ui

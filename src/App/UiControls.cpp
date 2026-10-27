#include "pch.h"

#include "UiControls.h"

#include "openreplay/Types.h"

#include <array>
#include <ctime>
#include <format>

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
        .root = Brush(0xFF0E0E0F),
        .panel = Brush(0xFF0E0E0F),
        .card = Brush(0xFF141416),
        .card_hover = Brush(0xFF1A1A1D),
        .inset = Brush(0xFF111113),
        .border = Brush(0xFF1F1F22),
        .control_border = Brush(0xFF2E2E33),
        .accent = Brush(0xFF76B900),
        .accent_hover = Brush(0xFF8BD400),
        .accent_pressed = Brush(0xFF4D7900),
        .status_border = Brush(0xFF527F00),
        .accent_text = Brush(0xFF0B1200),
        .primary_text = Brush(0xFFEDEDED),
        .secondary_text = Brush(0xFFA1A1AA),
        .danger = Brush(0xFFFF6B6E),
        .quiet = Brush(0xFF161618),
        .muted = Brush(0xFF7C7C82),
        .faint = Brush(0xFF55555B),
        .track = Brush(0xFF1F1F22),
        .track_disabled = Brush(0xFF17171A),
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
    button.BorderBrush(kind == ButtonKind::Accent ? theme_.accent : theme_.control_border);
    button.BorderThickness(Thickness{1, 1, 1, 1});
    button.FontFamily(PublicSans());
    button.FontSize(12.5);
    button.FontWeight(kind == ButtonKind::Accent ? winrt::Windows::UI::Text::FontWeights::SemiBold()
                                                 : winrt::Windows::UI::Text::FontWeights::Normal());
    button.MinHeight(34);
    button.Padding(Thickness{14, 6, 14, 6});
    button.CornerRadius(CornerRadius{8, 8, 8, 8});
    button.HorizontalContentAlignment(HorizontalAlignment::Center);
    button.VerticalContentAlignment(VerticalAlignment::Center);
    return button;
}

Button ControlFactory::IconButton(std::wstring_view glyph, std::wstring_view tooltip, ButtonKind kind) const {
    auto button = ActionButton(L"", kind);
    button.Width(36);
    button.Height(36);
    button.MinWidth(36);
    button.Padding(Thickness{0, 0, 0, 0});
    FontIcon icon;
    icon.Glyph(winrt::hstring{glyph});
    icon.FontSize(15);
    button.Content(icon);
    ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring{tooltip}));
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
    SetResource(toggle, L"ToggleSwitchKnobFillOn", theme_.root);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPointerOver", theme_.root);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPressed", theme_.root);
    SetResource(toggle, L"ToggleSwitchFillOff", theme_.control_border);
    SetResource(toggle, L"ToggleSwitchFillOffPointerOver", theme_.control_border);
    SetResource(toggle, L"ToggleSwitchStrokeOff", theme_.control_border);
    SetResource(toggle, L"ToggleSwitchStrokeOffPointerOver", theme_.muted);
    SetResource(toggle, L"ToggleSwitchKnobFillOff", theme_.muted);
    SetResource(toggle, L"ToggleSwitchKnobFillOffPointerOver", theme_.primary_text);
    return toggle;
}

void ControlFactory::StyleInput(Control const& control, double height) const {
    control.FontFamily(PublicSans());
    control.FontSize(13);
    control.Height(height);
    control.MinHeight(height);
    control.CornerRadius(CornerRadius{8, 8, 8, 8});
    control.Background(theme_.card);
    control.BorderBrush(theme_.control_border);
    control.VerticalContentAlignment(VerticalAlignment::Center);
    // Without these, hover and focus fall back to WinUI defaults, including the system accent underline.
    SetResource(control, L"TextControlBackground", theme_.card);
    SetResource(control, L"TextControlBackgroundPointerOver", theme_.card_hover);
    SetResource(control, L"TextControlBackgroundFocused", theme_.card);
    SetResource(control, L"TextControlBorderBrush", theme_.control_border);
    SetResource(control, L"TextControlBorderBrushPointerOver", theme_.control_border);
    SetResource(control, L"TextControlBorderBrushFocused", theme_.accent);
    SetResource(control, L"TextControlForeground", theme_.primary_text);
    SetResource(control, L"TextControlForegroundPointerOver", theme_.primary_text);
    SetResource(control, L"TextControlForegroundFocused", theme_.primary_text);
    SetResource(control, L"TextControlPlaceholderForeground", theme_.muted);
    SetResource(control, L"TextControlPlaceholderForegroundPointerOver", theme_.muted);
    SetResource(control, L"TextControlPlaceholderForegroundFocused", theme_.muted);
    SetResource(control, L"TextControlSelectionHighlightColor", theme_.accent_pressed);
}

ComboBox ControlFactory::Select() const {
    ComboBox select;
    StyleInput(select, 34);
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
    StyleInput(input, 34);
    input.Padding(Thickness{12, 0, 12, 0});
    input.IsReadOnly(true);
    if (!placeholder.empty()) input.PlaceholderText(winrt::hstring{placeholder});
    CenterSingleLineInput(input);
    return input;
}

PasswordBox ControlFactory::SecretInput(std::wstring_view placeholder) const {
    PasswordBox input;
    StyleInput(input, 34);
    input.Padding(Thickness{12, 0, 12, 0});
    input.PasswordRevealMode(PasswordRevealMode::Peek);
    if (!placeholder.empty()) input.PlaceholderText(winrt::hstring{placeholder});
    CenterSingleLineInput(input);
    return input;
}

NumberBox ControlFactory::NumberInput(double minimum, double maximum, double step) const {
    NumberBox input;
    StyleInput(input, 34);
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
    SetResource(slider, L"SliderTrackFill", transparent);
    SetResource(slider, L"SliderTrackFillPointerOver", transparent);
    SetResource(slider, L"SliderTrackFillPressed", transparent);
    SetResource(slider, L"SliderTrackFillDisabled", transparent);
    SetResource(slider, L"SliderTrackValueFill", transparent);
    SetResource(slider, L"SliderTrackValueFillPointerOver", transparent);
    SetResource(slider, L"SliderTrackValueFillPressed", transparent);
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
    surface.Padding(Thickness{12, 10, 12, 10});
    return surface;
}

Border ControlFactory::AudioDeviceSurface() const {
    Border surface;
    surface.Background(theme_.inset);
    surface.CornerRadius(CornerRadius{7, 7, 7, 7});
    surface.Padding(Thickness{9, 7, 9, 7});
    return surface;
}

bool IsVideoClip(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    if (!entry.is_regular_file(error)) return false;
    const auto extension = entry.path().extension().wstring();
    return _wcsicmp(extension.c_str(), L".mkv") == 0 || _wcsicmp(extension.c_str(), L".mp4") == 0;
}

std::vector<std::filesystem::directory_entry> ListClips(const std::filesystem::path& directory) {
    std::vector<std::filesystem::directory_entry> clips;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it{
             directory, std::filesystem::directory_options::skip_permission_denied, error}, end;
         !error && it != end; it.increment(error)) {
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            if (it.depth() > 0 || it->path().filename() == L"Discord") it.disable_recursion_pending();
            continue;
        }
        if (IsVideoClip(*it)) clips.push_back(*it);
    }
    return clips;
}

std::wstring ClipTitle(const std::filesystem::path& path, bool english) {
    if (auto game = openreplay::ClipGameName(path); !game.empty()) return game;
    const auto name = path.filename().wstring();
    if (name.find(L"_Replay") != std::wstring::npos) return english ? L"Replay" : L"Повтор";
    if (name.find(L"_Recording") != std::wstring::npos) return english ? L"Recording" : L"Запись";
    return english ? L"Clip" : L"Клип";
}

namespace {

std::tm LocalTime(std::chrono::system_clock::time_point time) {
    const auto value = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &value);
    return local;
}

std::tm LocalTime(std::filesystem::file_time_type written) {
    return LocalTime(std::chrono::clock_cast<std::chrono::system_clock>(written));
}

}  // namespace

std::wstring ClipDay(std::filesystem::file_time_type written, bool english) {
    const auto clip = LocalTime(written);
    const auto now = std::chrono::system_clock::now();
    const auto same_day = [&](const std::tm& day) { return clip.tm_year == day.tm_year && clip.tm_yday == day.tm_yday; };
    if (same_day(LocalTime(now))) return english ? L"Today" : L"Сегодня";
    if (same_day(LocalTime(now - std::chrono::hours{24}))) return english ? L"Yesterday" : L"Вчера";
    constexpr std::array<const wchar_t*, 12> months_en{L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    constexpr std::array<const wchar_t*, 12> months_ru{L"янв", L"фев", L"мар", L"апр", L"мая", L"июн",
                                                       L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек"};
    return std::to_wstring(clip.tm_mday) + L" " + (english ? months_en : months_ru)[clip.tm_mon];
}

std::wstring ClipClock(std::filesystem::file_time_type written) {
    const auto clip = LocalTime(written);
    return std::format(L"{:02}:{:02}", clip.tm_hour, clip.tm_min);
}

}  // namespace openreplay::ui

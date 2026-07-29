#include "pch.h"

#include "ClipLibraryWindow.xaml.h"

#include "DiscordShareService.h"
#include "UiControls.h"

#if __has_include("ClipLibraryWindow.g.cpp")
#include "ClipLibraryWindow.g.cpp"
#endif

#include "microsoft.ui.xaml.window.h"

#include <algorithm>
#include <format>

namespace {

using openreplay::ui::AddColumn;
using openreplay::ui::AddRow;
using openreplay::ui::Brush;
using openreplay::ui::ButtonKind;
using openreplay::ui::ControlFactory;
using openreplay::ui::DarkTheme;
using openreplay::ui::Text;
using Border = winrt::Microsoft::UI::Xaml::Controls::Border;
using FontIcon = winrt::Microsoft::UI::Xaml::Controls::FontIcon;
using Grid = winrt::Microsoft::UI::Xaml::Controls::Grid;
using Image = winrt::Microsoft::UI::Xaml::Controls::Image;
using MediaPlayerElement = winrt::Microsoft::UI::Xaml::Controls::MediaPlayerElement;
using ScrollViewer = winrt::Microsoft::UI::Xaml::Controls::ScrollViewer;
using StackPanel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
using TextBlock = winrt::Microsoft::UI::Xaml::Controls::TextBlock;
using CornerRadius = winrt::Microsoft::UI::Xaml::CornerRadius;
using GridUnitType = winrt::Microsoft::UI::Xaml::GridUnitType;
using Stretch = winrt::Microsoft::UI::Xaml::Media::Stretch;
using TextTrimming = winrt::Microsoft::UI::Xaml::TextTrimming;
using TextWrapping = winrt::Microsoft::UI::Xaml::TextWrapping;
using Thickness = winrt::Microsoft::UI::Xaml::Thickness;
using Visibility = winrt::Microsoft::UI::Xaml::Visibility;
using ScrollBarVisibility = winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility;

bool IsClip(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    if (!entry.is_regular_file(error)) return false;
    const auto extension = entry.path().extension().wstring();
    return extension == L".mkv" || extension == L".mp4";
}

std::wstring SizeText(std::uintmax_t bytes) {
    const auto mebibytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mebibytes < 1.0) return std::to_wstring(bytes / 1024U) + L" KiB";
    return std::format(L"{:.1f} MiB", mebibytes);
}

std::wstring DurationText(std::chrono::milliseconds duration) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (seconds >= 3600) {
        return std::format(L"{:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    }
    return std::format(L"{:02}:{:02}", seconds / 60, seconds % 60);
}

}  // namespace

namespace winrt::OpenReplay::implementation {

ClipLibraryWindow::ClipLibraryWindow() {
    BuildUi();
    const Microsoft::UI::Xaml::Window window = *this;
    window.Title(L"OpenReplay Clips");
    window.ExtendsContentIntoTitleBar(true);
    window.AppWindow().Resize(Windows::Graphics::SizeInt32{1180, 760});
    if (const auto presenter = window.AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
        presenter.SetBorderAndTitleBar(true, false);
    }
    window.AppWindow().Closing({this, &ClipLibraryWindow::Window_Closing});
    HWND handle{};
    if (SUCCEEDED(window.as<::IWindowNative>()->get_WindowHandle(&handle))) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            const auto x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - 1180) / 2;
            const auto y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - 760) / 2;
            SetWindowPos(handle, nullptr, x, y, 1180, 760, SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
    playback_timer_ = DispatcherQueue().CreateTimer();
    playback_timer_.Interval(std::chrono::milliseconds{200});
    playback_timer_.Tick([this](auto&&, auto&&) { UpdatePlaybackUi(); });
}

void ClipLibraryWindow::Configure(std::filesystem::path output_directory, bool english, bool webhook_available,
                                  HWND overlay_window,
                                  std::function<void(std::filesystem::path, bool)> share_callback,
                                  std::function<void(bool)> fullscreen_callback) {
    output_directory_ = std::move(output_directory);
    english_ = english;
    webhook_available_ = webhook_available;
    overlay_window_ = overlay_window;
    share_callback_ = std::move(share_callback);
    fullscreen_callback_ = std::move(fullscreen_callback);
    ApplyLanguage();
    RefreshClips();
}

void ClipLibraryWindow::ActivateWindow() {
    if (expanded_) PositionBesideOverlay();
    Activate();
    if (playback_timer_) playback_timer_.Start();
    const Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().MoveInZOrderAtTop();
}

void ClipLibraryWindow::HideWindow() {
    if (fullscreen_) {
        ToggleFullscreen();
    }
    if (playback_timer_) playback_timer_.Stop();
    if (media_player_) media_player_.Pause();
    const Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().Hide();
}

void ClipLibraryWindow::Shutdown() {
    closing_for_exit_ = true;
    if (playback_timer_) playback_timer_.Stop();
    if (player_ && player_.MediaPlayer()) player_.MediaPlayer().Pause();
    Close();
}

void ClipLibraryWindow::Window_Closing(
    Microsoft::UI::Windowing::AppWindow const&,
    Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
    if (closing_for_exit_) return;
    args.Cancel(true);
    HideWindow();
}

void ClipLibraryWindow::CloseButton_Click() {
    HideWindow();
}

void ClipLibraryWindow::MaximizeButton_Click() {
    if (fullscreen_) return;
    const Microsoft::UI::Xaml::Window window = *this;
    HWND handle{};
    if (FAILED(window.as<::IWindowNative>()->get_WindowHandle(&handle))) return;
    if (expanded_) {
        if (has_restored_bounds_) {
            SetWindowPos(handle, nullptr, restored_bounds_.left, restored_bounds_.top,
                         restored_bounds_.right - restored_bounds_.left,
                         restored_bounds_.bottom - restored_bounds_.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
        expanded_ = false;
        maximize_icon_.Glyph(L"\xE922");
    } else {
        has_restored_bounds_ = GetWindowRect(handle, &restored_bounds_) != FALSE;
        expanded_ = true;
        maximize_icon_.Glyph(L"\xE923");
        PositionBesideOverlay();
    }
}

void ClipLibraryWindow::PositionBesideOverlay() {
    if (!overlay_window_ || !IsWindow(overlay_window_)) return;
    const Microsoft::UI::Xaml::Window window = *this;
    HWND handle{};
    if (FAILED(window.as<::IWindowNative>()->get_WindowHandle(&handle))) return;

    RECT overlay{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(overlay_window_, &overlay) ||
        !GetMonitorInfoW(MonitorFromWindow(overlay_window_, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        return;
    }
    const bool overlay_uses_monitor = overlay.top < monitor.rcWork.top || overlay.bottom > monitor.rcWork.bottom;
    const auto right = overlay_uses_monitor ? monitor.rcMonitor.right : monitor.rcWork.right;
    SetWindowPos(handle, nullptr, overlay.right, overlay.top, std::max(1L, right - overlay.right),
                 std::max(1L, overlay.bottom - overlay.top), SWP_NOACTIVATE | SWP_NOZORDER);
}

void ClipLibraryWindow::SetFullscreenLayout(bool fullscreen) {
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    header_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    root_.RowDefinitions().GetAt(0).Height(GridLength{fullscreen ? 0.0 : 64.0, GridUnitType::Pixel});
    content_.Padding(fullscreen ? Thickness{} : Thickness{18, 18, 18, 18});
    content_.ColumnDefinitions().GetAt(0).Width(GridLength{fullscreen ? 0.0 : 360.0, GridUnitType::Pixel});
    list_scroll_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    player_column_.Margin(fullscreen ? Thickness{} : Thickness{18, 0, 0, 0});
    selected_title_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    selected_metadata_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    actions_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    player_surface_.BorderThickness(fullscreen ? Thickness{} : Thickness{1, 1, 1, 1});
    player_surface_.CornerRadius(fullscreen ? CornerRadius{} : CornerRadius{10, 10, 10, 10});
    Grid::SetRow(transport_surface_, fullscreen ? 0 : 1);
    transport_surface_.HorizontalAlignment(HorizontalAlignment::Stretch);
    transport_surface_.VerticalAlignment(fullscreen ? VerticalAlignment::Bottom : VerticalAlignment::Stretch);
    transport_surface_.Margin(fullscreen ? Thickness{24, 0, 24, 24} : Thickness{0, 10, 0, 0});
    transport_surface_.CornerRadius(CornerRadius{8, 8, 8, 8});
    fullscreen_icon_.Glyph(fullscreen ? L"\xE73F" : L"\xE740");
    Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        fullscreen_button_, winrt::box_value(fullscreen
            ? (english_ ? L"Exit full screen" : L"Выйти из полноэкранного режима")
            : (english_ ? L"Full screen" : L"Полноэкранный режим")));
}

void ClipLibraryWindow::Root_KeyDown(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    if (fullscreen_ && args.Key() == Windows::System::VirtualKey::Escape) {
        ToggleFullscreen();
        args.Handled(true);
    }
}

void ClipLibraryWindow::BuildUi() {
    const auto theme = DarkTheme();
    const ControlFactory controls{theme};
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;

    root_ = Grid{};
    root_.Background(theme.root);
    root_.KeyDown([this](auto&&, auto const& args) { Root_KeyDown(args); });
    AddRow(root_, 64, GridUnitType::Pixel);
    AddRow(root_, 1, GridUnitType::Star);

    header_ = Grid{};
    header_.Padding(Thickness{20, 6, 8, 6});
    header_.Background(theme.panel);
    AddColumn(header_, 1, GridUnitType::Star);
    AddColumn(header_, 0, GridUnitType::Auto);
    AddColumn(header_, 0, GridUnitType::Auto);
    AddColumn(header_, 0, GridUnitType::Auto);
    const Microsoft::UI::Xaml::Window window = *this;
    window.SetTitleBar(header_);
    StackPanel heading;
    heading.VerticalAlignment(VerticalAlignment::Center);
    heading.Spacing(2);
    title_ = Text(L"Clips", 23, theme.primary_text);
    title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    subtitle_ = Text(L"Replay library", 11.5, theme.secondary_text);
    heading.Children().Append(title_);
    heading.Children().Append(subtitle_);
    header_.Children().Append(heading);
    refresh_button_ = controls.ActionButton(L"Refresh");
    refresh_button_.VerticalAlignment(VerticalAlignment::Center);
    refresh_button_.Margin(Thickness{0, 0, 8, 0});
    refresh_button_.Click([this](auto&&, auto&&) { RefreshClips(); });
    Grid::SetColumn(refresh_button_, 1);
    header_.Children().Append(refresh_button_);
    maximize_button_ = controls.IconButton(L"\xE922", L"Maximize");
    maximize_icon_ = maximize_button_.Content().as<FontIcon>();
    maximize_button_.VerticalAlignment(VerticalAlignment::Center);
    maximize_button_.Margin(Thickness{0, 0, 8, 0});
    maximize_button_.Click([this](auto&&, auto&&) { MaximizeButton_Click(); });
    Grid::SetColumn(maximize_button_, 2);
    header_.Children().Append(maximize_button_);
    auto close_button = controls.IconButton(L"\xE8BB", L"Close");
    close_button.VerticalAlignment(VerticalAlignment::Center);
    close_button.Click([this](auto&&, auto&&) { CloseButton_Click(); });
    Grid::SetColumn(close_button, 3);
    header_.Children().Append(close_button);
    Grid::SetRow(header_, 0);
    root_.Children().Append(header_);

    content_ = Grid{};
    content_.Padding(Thickness{18, 18, 18, 18});
    AddColumn(content_, 360, GridUnitType::Pixel);
    AddColumn(content_, 1, GridUnitType::Star);
    Grid::SetRow(content_, 1);

    list_scroll_ = ScrollViewer{};
    list_scroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    list_scroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    clip_list_ = StackPanel{};
    clip_list_.Spacing(10);
    empty_text_ = Text(L"No clips found", 13, theme.secondary_text);
    empty_text_.TextWrapping(TextWrapping::Wrap);
    empty_text_.Margin(Thickness{8, 8, 8, 8});
    clip_list_.Children().Append(empty_text_);
    list_scroll_.Content(clip_list_);
    Grid::SetColumn(list_scroll_, 0);
    content_.Children().Append(list_scroll_);

    player_column_ = Grid{};
    player_column_.Margin(Thickness{18, 0, 0, 0});
    AddRow(player_column_, 1, GridUnitType::Star);
    AddRow(player_column_, 0, GridUnitType::Auto);
    AddRow(player_column_, 0, GridUnitType::Auto);
    AddRow(player_column_, 0, GridUnitType::Auto);
    AddRow(player_column_, 0, GridUnitType::Auto);

    player_surface_ = Border{};
    player_surface_.Background(theme.inset);
    player_surface_.BorderBrush(theme.border);
    player_surface_.BorderThickness(Thickness{1, 1, 1, 1});
    player_surface_.CornerRadius(CornerRadius{10, 10, 10, 10});
    player_surface_.Padding(Thickness{1, 1, 1, 1});
    player_ = MediaPlayerElement{};
    media_player_ = Windows::Media::Playback::MediaPlayer{};
    media_player_.AutoPlay(false);
    player_.SetMediaPlayer(media_player_);
    player_.AreTransportControlsEnabled(false);
    player_.AutoPlay(false);
    player_.Stretch(Stretch::Uniform);
    player_.HorizontalAlignment(HorizontalAlignment::Stretch);
    player_.VerticalAlignment(VerticalAlignment::Stretch);
    player_.PointerPressed([this](auto&&, auto&&) { TogglePlayback(); });
    player_surface_.Child(player_);
    Grid::SetRow(player_surface_, 0);
    player_column_.Children().Append(player_surface_);

    transport_surface_ = Border{};
    transport_surface_.Background(theme.inset);
    transport_surface_.BorderBrush(theme.border);
    transport_surface_.BorderThickness(Thickness{1, 1, 1, 1});
    transport_surface_.CornerRadius(CornerRadius{8, 8, 8, 8});
    transport_surface_.Padding(Thickness{10, 6, 10, 8});
    transport_surface_.Margin(Thickness{0, 10, 0, 0});
    StackPanel playback_controls;
    auto timeline_view = controls.RangeSlider(0, 1, 0.05, 0);
    timeline_ = timeline_view.input;
    timeline_.ValueChanged([this](auto&&, auto&&) {
        if (updating_timeline_ || !media_player_ || !player_source_) return;
        const auto position = std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
            std::chrono::duration<double>{timeline_.Value()});
        media_player_.PlaybackSession().Position(position);
    });
    timeline_view.view.Margin(Thickness{0, 0, 0, 2});
    playback_controls.Children().Append(timeline_view.view);

    Grid transport_row;
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 1, GridUnitType::Star);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 110, GridUnitType::Pixel);
    AddColumn(transport_row, 96, GridUnitType::Pixel);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    play_button_ = controls.IconButton(L"\xE768", L"Play / Pause");
    play_icon_ = play_button_.Content().as<FontIcon>();
    play_button_.Click([this](auto&&, auto&&) { TogglePlayback(); });
    transport_row.Children().Append(play_button_);
    playback_time_ = Text(L"00:00 / 00:00", 11.5, theme.secondary_text);
    playback_time_.VerticalAlignment(VerticalAlignment::Center);
    playback_time_.Margin(Thickness{12, 0, 0, 0});
    Grid::SetColumn(playback_time_, 1);
    transport_row.Children().Append(playback_time_);
    auto volume_label = Text(L"Volume", 11, theme.secondary_text);
    volume_label.VerticalAlignment(VerticalAlignment::Center);
    volume_label.Margin(Thickness{12, 0, 6, 0});
    Grid::SetColumn(volume_label, 2);
    transport_row.Children().Append(volume_label);
    Slider volume_slider;
    const auto transparent = Brush(0x00000000);
    volume_slider.Minimum(0);
    volume_slider.Maximum(100);
    volume_slider.StepFrequency(1);
    volume_slider.Value(100);
    volume_slider.Background(transparent);
    volume_slider.Foreground(theme.accent);
    volume_slider.Height(32);
    volume_slider.HorizontalAlignment(HorizontalAlignment::Stretch);
    volume_slider.VerticalAlignment(VerticalAlignment::Center);
    const auto set_volume_resource = [&volume_slider](std::wstring_view key, auto const& value) {
        (void)volume_slider.Resources().Insert(winrt::box_value(winrt::hstring{key}), value);
    };
    set_volume_resource(L"SliderContainerBackground", transparent);
    set_volume_resource(L"SliderContainerBackgroundPointerOver", transparent);
    set_volume_resource(L"SliderContainerBackgroundPressed", transparent);
    set_volume_resource(L"SliderContainerBackgroundDisabled", transparent);
    set_volume_resource(L"SliderTrackFill", theme.border);
    set_volume_resource(L"SliderTrackFillPointerOver", theme.track);
    set_volume_resource(L"SliderTrackFillPressed", theme.track);
    set_volume_resource(L"SliderTrackFillDisabled", theme.track_disabled);
    set_volume_resource(L"SliderTrackValueFill", theme.accent);
    set_volume_resource(L"SliderTrackValueFillPointerOver", theme.accent_hover);
    set_volume_resource(L"SliderTrackValueFillPressed", theme.accent_pressed);
    set_volume_resource(L"SliderThumbBackground", theme.accent);
    set_volume_resource(L"SliderThumbBackgroundPointerOver", theme.accent_hover);
    set_volume_resource(L"SliderThumbBackgroundPressed", theme.accent_pressed);
    set_volume_resource(L"SliderOuterThumbBackground", transparent);
    set_volume_resource(L"SliderThumbBorderBrush", transparent);
    volume_ = volume_slider;
    volume_.ValueChanged([this](auto&&, auto&&) {
        if (media_player_) media_player_.Volume(volume_.Value() / 100.0);
    });
    Grid::SetColumn(volume_, 3);
    transport_row.Children().Append(volume_);
    speed_selector_ = controls.Select();
    speed_selector_.Items().Append(controls.Option(L"0.25x"));
    speed_selector_.Items().Append(controls.Option(L"0.5x"));
    speed_selector_.Items().Append(controls.Option(L"0.75x"));
    speed_selector_.Items().Append(controls.Option(L"1x"));
    speed_selector_.Items().Append(controls.Option(L"1.25x"));
    speed_selector_.Items().Append(controls.Option(L"1.5x"));
    speed_selector_.Items().Append(controls.Option(L"2x"));
    speed_selector_.SelectedIndex(3);
    speed_selector_.VerticalAlignment(VerticalAlignment::Center);
    speed_selector_.Margin(Thickness{8, 0, 0, 0});
    speed_selector_.SelectionChanged([this](auto&&, auto&&) {
        if (!media_player_ || speed_selector_.SelectedIndex() < 0) return;
        constexpr std::array<double, 7> rates{0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
        const auto index = static_cast<std::size_t>(speed_selector_.SelectedIndex());
        if (index >= rates.size()) return;
        try {
            media_player_.PlaybackSession().PlaybackRate(rates[index]);
        } catch (...) {
            speed_selector_.SelectedIndex(3);
        }
    });
    Grid::SetColumn(speed_selector_, 4);
    transport_row.Children().Append(speed_selector_);
    fullscreen_button_ = controls.IconButton(L"\xE740", L"Full screen");
    fullscreen_button_.Margin(Thickness{8, 0, 0, 0});
    fullscreen_icon_ = fullscreen_button_.Content().as<FontIcon>();
    fullscreen_button_.Click([this](auto&&, auto&&) { ToggleFullscreen(); });
    Grid::SetColumn(fullscreen_button_, 5);
    transport_row.Children().Append(fullscreen_button_);
    playback_controls.Children().Append(transport_row);
    transport_surface_.Child(playback_controls);
    Grid::SetRow(transport_surface_, 1);
    player_column_.Children().Append(transport_surface_);

    selected_title_ = Text(L"Select a clip", 18, theme.primary_text);
    selected_title_.Margin(Thickness{0, 12, 0, 2});
    selected_title_.TextTrimming(TextTrimming::CharacterEllipsis);
    Grid::SetRow(selected_title_, 2);
    player_column_.Children().Append(selected_title_);
    selected_metadata_ = Text(L"", 12, theme.secondary_text);
    selected_metadata_.Margin(Thickness{0, 0, 0, 12});
    Grid::SetRow(selected_metadata_, 3);
    player_column_.Children().Append(selected_metadata_);

    actions_ = StackPanel{};
    actions_.Orientation(Orientation::Horizontal);
    actions_.Spacing(8);
    actions_.HorizontalAlignment(HorizontalAlignment::Right);
    open_button_ = controls.ActionButton(L"Open");
    open_button_.IsEnabled(false);
    open_button_.Click([this](auto&&, auto&&) { OpenSelectedClip(); });
    actions_.Children().Append(open_button_);
    discord_copy_button_ = controls.ActionButton(L"Discord copy");
    discord_copy_button_.IsEnabled(false);
    discord_copy_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(false); });
    actions_.Children().Append(discord_copy_button_);
    webhook_button_ = controls.ActionButton(L"Send webhook", ButtonKind::Accent);
    webhook_button_.IsEnabled(false);
    webhook_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(true); });
    actions_.Children().Append(webhook_button_);
    Grid::SetRow(actions_, 4);
    player_column_.Children().Append(actions_);
    Grid::SetColumn(player_column_, 1);
    content_.Children().Append(player_column_);
    root_.Children().Append(content_);
    Content(root_);
}

void ClipLibraryWindow::ApplyLanguage() {
    if (!title_) return;
    title_.Text(english_ ? L"Clips" : L"Клипы");
    subtitle_.Text(english_ ? L"Replay library" : L"Библиотека повторов");
    refresh_button_.Content(winrt::box_value(english_ ? L"Refresh" : L"Обновить"));
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        maximize_button_, winrt::box_value(english_ ? L"Fill beside overlay / restore"
                                                     : L"Заполнить рядом с оверлеем / восстановить"));
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        fullscreen_button_, winrt::box_value(fullscreen_
            ? (english_ ? L"Exit full screen" : L"Выйти из полноэкранного режима")
            : (english_ ? L"Full screen" : L"Полноэкранный режим")));
    winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        speed_selector_, winrt::box_value(english_ ? L"Playback speed" : L"Скорость воспроизведения"));
    open_button_.Content(winrt::box_value(english_ ? L"Open" : L"Открыть"));
    discord_copy_button_.Content(winrt::box_value(english_ ? L"Discord copy" : L"Копия Discord"));
    webhook_button_.Content(winrt::box_value(english_ ? L"Send webhook" : L"Webhook"));
    if (selected_clip_.empty()) selected_title_.Text(english_ ? L"Select a clip" : L"Выберите клип");
}

void ClipLibraryWindow::RefreshClips() {
    if (!clip_list_) return;
    while (clip_list_.Children().Size() > 1) clip_list_.Children().RemoveAtEnd();
    clip_cards_.clear();

    std::vector<std::filesystem::path> clips;
    std::error_code error;
    if (std::filesystem::exists(output_directory_, error)) {
        for (const std::filesystem::directory_entry entry : std::filesystem::directory_iterator(
                 output_directory_, std::filesystem::directory_options::skip_permission_denied, error)) {
            if (!error && IsClip(entry)) clips.push_back(entry.path());
            error.clear();
        }
    }
    std::ranges::sort(clips, [](const auto& left, const auto& right) {
        std::error_code left_error;
        std::error_code right_error;
        const auto left_time = std::filesystem::last_write_time(left, left_error);
        const auto right_time = std::filesystem::last_write_time(right, right_error);
        return left_error || right_error ? left.filename() > right.filename() : left_time > right_time;
    });

    empty_text_.Visibility(clips.empty() ? Visibility::Visible : Visibility::Collapsed);
    if (clips.empty()) {
        subtitle_.Text(english_ ? L"Replay library" : L"Библиотека повторов");
    } else if (english_) {
        subtitle_.Text(std::format(L"{} clips · newest first", clips.size()));
    } else {
        subtitle_.Text(std::format(L"Клипов: {} · новые сверху", clips.size()));
    }
    empty_text_.Text(clips.empty()
        ? (english_ ? L"No MKV or MP4 clips found in the output folder"
                    : L"В папке сохранения нет клипов MKV или MP4")
        : L"");
    if (clips.empty()) return;

    const auto theme = DarkTheme();
    for (const auto& path : clips) {
        Border card;
        card.Background(theme.card);
        card.BorderBrush(theme.border);
        card.BorderThickness(Thickness{1, 1, 1, 1});
        card.CornerRadius(CornerRadius{8, 8, 8, 8});
        card.Shadow(Microsoft::UI::Xaml::Media::ThemeShadow{});
        card.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 6.0f});
        card.PointerEntered([theme](auto const& sender, auto&&) {
            const auto hovered = sender.template as<Border>();
            hovered.Background(theme.card_hover);
            hovered.BorderBrush(theme.accent_pressed);
            hovered.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 10.0f});
        });
        card.PointerExited([this, theme, path](auto const& sender, auto&&) {
            const auto hovered = sender.template as<Border>();
            const bool selected = path == selected_clip_;
            hovered.Background(selected ? theme.inset : theme.card);
            hovered.BorderBrush(selected ? theme.accent : theme.border);
            hovered.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 6.0f});
        });
        card.Padding(Thickness{8, 8, 8, 8});

        Grid card_grid;
        AddColumn(card_grid, 132, GridUnitType::Pixel);
        AddColumn(card_grid, 1, GridUnitType::Star);
        Image thumbnail;
        thumbnail.Width(132);
        thumbnail.Height(74);
        thumbnail.Stretch(Stretch::UniformToFill);
        Grid::SetColumn(thumbnail, 0);
        card_grid.Children().Append(thumbnail);

        StackPanel details;
        details.Margin(Thickness{12, 0, 4, 0});
        auto name = Text(path.filename().wstring(), 12.5, theme.primary_text);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        name.TextWrapping(TextWrapping::NoWrap);
        details.Children().Append(name);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        auto metadata = Text(size_error ? L"Video" : SizeText(size), 11, theme.secondary_text);
        metadata.Margin(Thickness{0, 5, 0, 0});
        details.Children().Append(metadata);
        Grid::SetColumn(details, 1);
        card_grid.Children().Append(details);
        card.Child(card_grid);
        card.PointerPressed([this, path](auto&&, auto&&) { SelectClip(path); });
        clip_list_.Children().Append(card);
        clip_cards_.emplace_back(path, card);
        LoadClipVisualsAsync(path, thumbnail, metadata);
    }
    UpdateSelectionStyles();
}

void ClipLibraryWindow::UpdateSelectionStyles() {
    const auto theme = DarkTheme();
    for (const auto& [path, card] : clip_cards_) {
        const bool selected = path == selected_clip_;
        card.Background(selected ? theme.inset : theme.card);
        card.BorderBrush(selected ? theme.accent : theme.border);
        card.BorderThickness(selected ? Thickness{2, 2, 2, 2} : Thickness{1, 1, 1, 1});
    }
}

void ClipLibraryWindow::SelectClip(const std::filesystem::path& path) {
    selected_clip_ = path;
    open_button_.IsEnabled(true);
    discord_copy_button_.IsEnabled(true);
    webhook_button_.IsEnabled(webhook_available_ && share_callback_ != nullptr);
    selected_title_.Text(path.filename().wstring());
    selected_metadata_.Text(english_ ? L"Loading media..." : L"Загрузка медиа...");
    UpdateSelectionStyles();
    OpenClipAsync(path);
}

winrt::fire_and_forget ClipLibraryWindow::OpenClipAsync(std::filesystem::path path) {
    const auto weak = get_weak();
    try {
        const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path.wstring());
        const auto source = winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
        std::wstring details;
        try {
            const auto video = co_await file.Properties().GetVideoPropertiesAsync();
            const auto basic = co_await file.GetBasicPropertiesAsync();
            details = SizeText(basic.Size()) + L"  ·  " +
                      DurationText(std::chrono::duration_cast<std::chrono::milliseconds>(video.Duration()));
        } catch (...) {
        }
        if (const auto self = weak.get()) {
            if (self->selected_clip_ != path) co_return;
            self->player_source_ = source;
            self->media_player_.Source(source);
            constexpr std::array<double, 7> rates{0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
            const auto speed_index = self->speed_selector_.SelectedIndex();
            if (speed_index >= 0 && static_cast<std::size_t>(speed_index) < rates.size()) {
                try {
                    self->media_player_.PlaybackSession().PlaybackRate(rates[static_cast<std::size_t>(speed_index)]);
                } catch (...) {
                    self->speed_selector_.SelectedIndex(3);
                }
            }
            self->timeline_.Value(0);
            self->selected_metadata_.Text(details.empty()
                ? (self->english_ ? L"Ready to play" : L"Готово к воспроизведению")
                : details);
        }
    } catch (...) {
        if (const auto self = weak.get()) {
            if (self->selected_clip_ == path) {
                self->selected_metadata_.Text(self->english_ ? L"Unable to open this clip"
                                                              : L"Не удалось открыть этот клип");
            }
        }
    }
}

void ClipLibraryWindow::TogglePlayback() {
    if (!media_player_ || !player_source_) return;
    const auto session = media_player_.PlaybackSession();
    const bool playing = session.PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing;
    if (playing) {
        media_player_.Pause();
    } else {
        media_player_.Play();
    }
    play_icon_.Glyph(playing ? L"\xE768" : L"\xE769");
    UpdatePlaybackUi();
}

void ClipLibraryWindow::UpdatePlaybackUi() {
    if (!media_player_ || !player_source_ || !timeline_ || !playback_time_) return;
    try {
        const auto session = media_player_.PlaybackSession();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(session.NaturalDuration());
        const auto position = std::chrono::duration_cast<std::chrono::milliseconds>(session.Position());
        const auto duration_seconds = std::max(0.0, std::chrono::duration<double>(duration).count());
        const auto position_seconds = std::clamp(std::chrono::duration<double>(position).count(), 0.0,
                                                 std::max(0.0, duration_seconds));
        updating_timeline_ = true;
        timeline_.Maximum(std::max(1.0, duration_seconds));
        timeline_.Value(position_seconds);
        updating_timeline_ = false;
        playback_time_.Text(DurationText(position) + L" / " + DurationText(duration));
        const bool playing = session.PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing;
        play_icon_.Glyph(playing ? L"\xE769" : L"\xE768");
    } catch (...) {
        updating_timeline_ = false;
    }
}

void ClipLibraryWindow::ToggleFullscreen() {
    if (!player_ || !player_source_) return;
    const Microsoft::UI::Xaml::Window window = *this;
    HWND handle{};
    if (FAILED(window.as<::IWindowNative>()->get_WindowHandle(&handle))) return;
    if (fullscreen_) {
        const auto presenter = Microsoft::UI::Windowing::OverlappedPresenter::Create();
        presenter.SetBorderAndTitleBar(true, false);
        window.AppWindow().SetPresenter(presenter);
        fullscreen_ = false;
        SetFullscreenLayout(false);
        if (has_pre_fullscreen_bounds_) {
            SetWindowPos(handle, nullptr, pre_fullscreen_bounds_.left, pre_fullscreen_bounds_.top,
                         pre_fullscreen_bounds_.right - pre_fullscreen_bounds_.left,
                         pre_fullscreen_bounds_.bottom - pre_fullscreen_bounds_.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
        if (fullscreen_callback_) fullscreen_callback_(false);
    } else {
        has_pre_fullscreen_bounds_ = GetWindowRect(handle, &pre_fullscreen_bounds_) != FALSE;
        window.AppWindow().SetPresenter(Microsoft::UI::Windowing::FullScreenPresenter::Create());
        fullscreen_ = true;
        SetFullscreenLayout(true);
        if (fullscreen_callback_) fullscreen_callback_(true);
    }
}

void ClipLibraryWindow::OpenSelectedClip() {
    if (selected_clip_.empty()) return;
    ShellExecuteW(nullptr, L"open", selected_clip_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ClipLibraryWindow::ShareSelectedClip(bool webhook) {
    if (selected_clip_.empty() || !share_callback_) return;
    share_callback_(selected_clip_, webhook);
}

winrt::fire_and_forget ClipLibraryWindow::LoadClipVisualsAsync(
    std::filesystem::path path, Image image, TextBlock metadata) {
    const auto weak = get_weak();
    try {
        const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path.wstring());
        const auto thumbnail = co_await file.GetThumbnailAsync(
            winrt::Windows::Storage::FileProperties::ThumbnailMode::VideosView, 320,
            winrt::Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(thumbnail);
        const auto video = co_await file.Properties().GetVideoPropertiesAsync();
        const auto details = SizeText((co_await file.GetBasicPropertiesAsync()).Size()) + L"  ·  " +
                             DurationText(std::chrono::duration_cast<std::chrono::milliseconds>(video.Duration()));
        if (const auto self = weak.get()) {
            image.Source(bitmap);
            metadata.Text(details);
        }
    } catch (...) {
        if (const auto self = weak.get()) metadata.Text(self->english_ ? L"Video" : L"Видео");
    }
}

}  // namespace winrt::OpenReplay::implementation

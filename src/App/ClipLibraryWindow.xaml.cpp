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

FontIcon Icon(std::wstring_view glyph) {
    FontIcon icon;
    icon.Glyph(winrt::hstring{glyph});
    icon.FontSize(15);
    return icon;
}

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
    window.AppWindow().Resize(Windows::Graphics::SizeInt32{1180, 760});
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
                                  std::function<void(std::filesystem::path, bool)> share_callback) {
    output_directory_ = std::move(output_directory);
    english_ = english;
    webhook_available_ = webhook_available;
    share_callback_ = std::move(share_callback);
    ApplyLanguage();
    RefreshClips();
}

void ClipLibraryWindow::ActivateWindow() {
    Activate();
    if (playback_timer_) playback_timer_.Start();
    const Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().MoveInZOrderAtTop();
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
    if (playback_timer_) playback_timer_.Stop();
    if (player_ && player_.MediaPlayer()) player_.MediaPlayer().Pause();
    const Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().Hide();
}

void ClipLibraryWindow::BuildUi() {
    const auto theme = DarkTheme();
    const ControlFactory controls{theme};
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;

    root_ = Grid{};
    root_.Background(theme.root);
    AddRow(root_, 80, GridUnitType::Pixel);
    AddRow(root_, 1, GridUnitType::Star);

    Grid header;
    header.Padding(Thickness{20, 0, 20, 0});
    header.Background(theme.panel);
    AddColumn(header, 1, GridUnitType::Star);
    AddColumn(header, 0, GridUnitType::Auto);
    StackPanel heading;
    title_ = Text(L"Clips", 23, theme.primary_text);
    title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    subtitle_ = Text(L"Replay library", 11.5, theme.secondary_text);
    subtitle_.Margin(Thickness{0, 2, 0, 0});
    heading.Children().Append(title_);
    heading.Children().Append(subtitle_);
    header.Children().Append(heading);
    refresh_button_ = controls.ActionButton(L"Refresh");
    refresh_button_.Click([this](auto&&, auto&&) { RefreshClips(); });
    Grid::SetColumn(refresh_button_, 1);
    header.Children().Append(refresh_button_);
    Grid::SetRow(header, 0);
    root_.Children().Append(header);

    Grid content;
    content.Padding(Thickness{18, 18, 18, 18});
    AddColumn(content, 360, GridUnitType::Pixel);
    AddColumn(content, 1, GridUnitType::Star);
    Grid::SetRow(content, 1);

    ScrollViewer list_scroll;
    list_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    list_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    clip_list_ = StackPanel{};
    clip_list_.Spacing(10);
    empty_text_ = Text(L"No clips found", 13, theme.secondary_text);
    empty_text_.TextWrapping(TextWrapping::Wrap);
    empty_text_.Margin(Thickness{8, 8, 8, 8});
    clip_list_.Children().Append(empty_text_);
    list_scroll.Content(clip_list_);
    Grid::SetColumn(list_scroll, 0);
    content.Children().Append(list_scroll);

    Grid player_column;
    player_column.Margin(Thickness{18, 0, 0, 0});
    AddRow(player_column, 1, GridUnitType::Star);
    AddRow(player_column, 0, GridUnitType::Auto);
    AddRow(player_column, 0, GridUnitType::Auto);
    AddRow(player_column, 0, GridUnitType::Auto);
    AddRow(player_column, 0, GridUnitType::Auto);

    Border player_surface;
    player_surface.Background(theme.inset);
    player_surface.BorderBrush(theme.border);
    player_surface.BorderThickness(Thickness{1, 1, 1, 1});
    player_surface.CornerRadius(CornerRadius{10, 10, 10, 10});
    player_surface.Padding(Thickness{1, 1, 1, 1});
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
    player_surface.Child(player_);
    Grid::SetRow(player_surface, 0);
    player_column.Children().Append(player_surface);

    Border transport_surface;
    transport_surface.Background(theme.inset);
    transport_surface.BorderBrush(theme.border);
    transport_surface.BorderThickness(Thickness{1, 1, 1, 1});
    transport_surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    transport_surface.Padding(Thickness{10, 6, 10, 8});
    transport_surface.Margin(Thickness{0, 10, 0, 0});
    StackPanel playback_controls;
    auto timeline_view = controls.RangeSlider(0, 1, 1, 0);
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
    AddColumn(transport_row, 0, GridUnitType::Auto);
    play_button_ = controls.ActionButton(L"Play");
    play_button_.Width(42);
    play_button_.Height(42);
    play_button_.MinWidth(42);
    play_button_.Padding(Thickness{0, 0, 0, 0});
    play_icon_ = Icon(L"\xE768");
    play_button_.Content(play_icon_);
    ToolTipService::SetToolTip(play_button_, winrt::box_value(L"Play / Pause"));
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
    fullscreen_button_ = controls.ActionButton(L"Full screen");
    fullscreen_button_.Width(42);
    fullscreen_button_.Height(42);
    fullscreen_button_.MinWidth(42);
    fullscreen_button_.Margin(Thickness{8, 0, 0, 0});
    fullscreen_button_.Padding(Thickness{0, 0, 0, 0});
    fullscreen_icon_ = Icon(L"\xE740");
    fullscreen_button_.Content(fullscreen_icon_);
    ToolTipService::SetToolTip(fullscreen_button_, winrt::box_value(L"Full screen"));
    fullscreen_button_.Click([this](auto&&, auto&&) { ToggleFullscreen(); });
    Grid::SetColumn(fullscreen_button_, 4);
    transport_row.Children().Append(fullscreen_button_);
    playback_controls.Children().Append(transport_row);
    transport_surface.Child(playback_controls);
    Grid::SetRow(transport_surface, 1);
    player_column.Children().Append(transport_surface);

    selected_title_ = Text(L"Select a clip", 18, theme.primary_text);
    selected_title_.Margin(Thickness{0, 12, 0, 2});
    selected_title_.TextTrimming(TextTrimming::CharacterEllipsis);
    Grid::SetRow(selected_title_, 2);
    player_column.Children().Append(selected_title_);
    selected_metadata_ = Text(L"", 12, theme.secondary_text);
    selected_metadata_.Margin(Thickness{0, 0, 0, 12});
    Grid::SetRow(selected_metadata_, 3);
    player_column.Children().Append(selected_metadata_);

    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(8);
    actions.HorizontalAlignment(HorizontalAlignment::Right);
    open_button_ = controls.ActionButton(L"Open");
    open_button_.IsEnabled(false);
    open_button_.Click([this](auto&&, auto&&) { OpenSelectedClip(); });
    actions.Children().Append(open_button_);
    discord_copy_button_ = controls.ActionButton(L"Discord copy");
    discord_copy_button_.IsEnabled(false);
    discord_copy_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(false); });
    actions.Children().Append(discord_copy_button_);
    webhook_button_ = controls.ActionButton(L"Send webhook", ButtonKind::Accent);
    webhook_button_.IsEnabled(false);
    webhook_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(true); });
    actions.Children().Append(webhook_button_);
    Grid::SetRow(actions, 4);
    player_column.Children().Append(actions);
    Grid::SetColumn(player_column, 1);
    content.Children().Append(player_column);
    root_.Children().Append(content);
    Content(root_);
}

void ClipLibraryWindow::ApplyLanguage() {
    if (!title_) return;
    title_.Text(english_ ? L"Clips" : L"Клипы");
    subtitle_.Text(english_ ? L"Replay library" : L"Библиотека повторов");
    refresh_button_.Content(winrt::box_value(english_ ? L"Refresh" : L"Обновить"));
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
    player_.IsFullWindow(!player_.IsFullWindow());
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

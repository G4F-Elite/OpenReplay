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
using openreplay::ui::ClipClock;
using openreplay::ui::ClipDay;
using openreplay::ui::ClipTitle;
using openreplay::ui::ControlFactory;
using openreplay::ui::DarkTheme;
using openreplay::ui::DurationText;
using openreplay::ui::JoinDetails;
using openreplay::ui::ListClips;
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

constexpr auto kSpaceHoldDelay = std::chrono::milliseconds{350};

constexpr double kListWidth = 300;

std::wstring SizeText(std::uintmax_t bytes, bool english) {
    const auto mebibytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mebibytes < 1.0) return std::to_wstring(bytes / 1024U) + (english ? L" KB" : L" КБ");
    if (mebibytes >= 1024.0) return std::format(L"{:.1f} {}", mebibytes / 1024.0, english ? L"GB" : L"ГБ");
    return std::format(L"{:.1f} {}", mebibytes, english ? L"MB" : L"МБ");
}

std::wstring BitrateText(std::uint64_t bitrate, bool english) {
    if (!bitrate) return {};
    return bitrate >= 1000000
        ? std::format(L"{:.1f} {}", static_cast<double>(bitrate) / 1000000.0, english ? L"Mbps" : L"Мбит/с")
        : std::format(L"{} {}", bitrate / 1000, english ? L"Kbps" : L"Кбит/с");
}

std::wstring ResolutionText(std::uint32_t width, std::uint32_t height) {
    return width && height ? std::format(L"{}×{}", width, height) : std::wstring{};
}

// Parts joined with " · ", skipping empty ones.
void MoveToRecycleBin(const std::filesystem::path& path) {
    winrt::com_ptr<IFileOperation> operation;
    winrt::check_hresult(CoCreateInstance(
        CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation.put())));

    constexpr DWORD flags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI |
                            FOFX_EARLYFAILURE | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD;
    winrt::check_hresult(operation->SetOperationFlags(flags));

    winrt::com_ptr<IShellItem> item;
    winrt::check_hresult(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())));
    winrt::check_hresult(operation->DeleteItem(item.get(), nullptr));

    const auto operation_result = operation->PerformOperations();
    BOOL aborted = FALSE;
    const auto aborted_result = operation->GetAnyOperationsAborted(&aborted);
    winrt::check_hresult(operation_result);
    winrt::check_hresult(aborted_result);
    if (aborted) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
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
    window.Activated([this](auto&&, auto const& args) {
        if (args.WindowActivationState() == Microsoft::UI::Xaml::WindowActivationState::Deactivated) {
            CancelSpaceHold();
        }
    });
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
    playback_feedback_timer_ = DispatcherQueue().CreateTimer();
    playback_feedback_timer_.Interval(std::chrono::milliseconds{16});
    playback_feedback_timer_.Tick([this](auto&&, auto&&) { UpdatePlaybackFeedback(); });
    space_hold_timer_ = DispatcherQueue().CreateTimer();
    space_hold_timer_.Interval(kSpaceHoldDelay);
    space_hold_timer_.IsRepeating(false);
    space_hold_timer_.Tick([this](auto&&, auto&&) { StartSpaceBoost(); });
}

void ClipLibraryWindow::Configure(std::filesystem::path output_directory, bool english, bool webhook_available,
                                  std::uint32_t share_limit_megabytes, HWND overlay_window,
                                  std::function<void(std::filesystem::path, bool)> share_callback,
                                  std::function<void(bool)> fullscreen_callback) {
    output_directory_ = std::move(output_directory);
    english_ = english;
    webhook_available_ = webhook_available;
    share_limit_megabytes_ = share_limit_megabytes;
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

bool ClipLibraryWindow::IsShown() {
    const Microsoft::UI::Xaml::Window window = *this;
    return window.AppWindow().IsVisible();
}

void ClipLibraryWindow::HideWindow() {
    CancelSpaceHold();
    if (fullscreen_) {
        ToggleFullscreen();
    }
    if (playback_timer_) playback_timer_.Stop();
    if (playback_feedback_timer_) playback_feedback_timer_.Stop();
    if (playback_feedback_) playback_feedback_.Opacity(0);
    if (media_player_) media_player_.Pause();
    const Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().Hide();
}

void ClipLibraryWindow::Shutdown() {
    closing_for_exit_ = true;
    CancelSpaceHold();
    if (playback_timer_) playback_timer_.Stop();
    if (playback_feedback_timer_) playback_feedback_timer_.Stop();
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
    root_.RowDefinitions().GetAt(0).Height(GridLength{fullscreen ? 0.0 : 60.0, GridUnitType::Pixel});
    content_.ColumnDefinitions().GetAt(0).Width(GridLength{fullscreen ? 0.0 : kListWidth, GridUnitType::Pixel});
    list_scroll_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    player_column_.Padding(fullscreen ? Thickness{} : Thickness{20, 18, 20, 18});
    info_row_.Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
    player_surface_.CornerRadius(fullscreen ? CornerRadius{} : CornerRadius{8, 8, 8, 8});
    fullscreen_icon_.Glyph(fullscreen ? L"\xE73F" : L"\xE740");
    Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        fullscreen_button_, winrt::box_value(fullscreen
            ? (english_ ? L"Exit full screen" : L"Выйти из полноэкранного режима")
            : (english_ ? L"Full screen" : L"Полноэкранный режим")));
}

void ClipLibraryWindow::Root_KeyDown(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    if (args.Key() == Windows::System::VirtualKey::Space && space_down_) {
        args.Handled(true);
        return;
    }
    if (delete_in_progress_ || speed_selector_.IsDropDownOpen()) return;
    if (fullscreen_ && args.Key() == Windows::System::VirtualKey::Escape) {
        ToggleFullscreen();
        args.Handled(true);
        return;
    }
    if (args.Key() != Windows::System::VirtualKey::Space || !player_source_) return;
    if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_SHIFT) |
         GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) return;
    args.Handled(true);
    // A repeat after cancellation must not start a new gesture on another clip.
    if (args.KeyStatus().WasKeyDown) return;
    space_down_ = true;
    space_pressed_at_ = std::chrono::steady_clock::now();
    space_hold_timer_.Start();
}

void ClipLibraryWindow::Root_KeyUp(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    if (args.Key() != Windows::System::VirtualKey::Space || !space_down_) return;
    args.Handled(true);
    const bool tap = std::chrono::steady_clock::now() - space_pressed_at_ < kSpaceHoldDelay;
    CancelSpaceHold();
    if (tap) TogglePlayback();
}

void ClipLibraryWindow::StartSpaceBoost() {
    if (!space_down_ || !player_source_) return;
    const auto session = media_player_.PlaybackSession();
    const auto state = session.PlaybackState();
    if (state != Windows::Media::Playback::MediaPlaybackState::Playing &&
        state != Windows::Media::Playback::MediaPlaybackState::Paused) return;
    playback_rate_before_hold_ = session.PlaybackRate();
    playing_before_hold_ = state == Windows::Media::Playback::MediaPlaybackState::Playing;
    session.PlaybackRate(2.0);
    space_boost_active_ = true;
    if (!playing_before_hold_) TogglePlayback();
}

void ClipLibraryWindow::CancelSpaceHold() {
    if (space_hold_timer_) space_hold_timer_.Stop();
    space_down_ = false;
    if (!space_boost_active_) return;
    space_boost_active_ = false;
    media_player_.PlaybackSession().PlaybackRate(playback_rate_before_hold_);
    if (!playing_before_hold_) media_player_.Pause();
    UpdatePlaybackUi();
}

void ClipLibraryWindow::BuildUi() {
    const auto theme = DarkTheme();
    const ControlFactory controls{theme};
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;

    root_ = Grid{};
    root_.Background(theme.root);
    root_.PreviewKeyDown([this](auto&&, auto const& args) { Root_KeyDown(args); });
    root_.PreviewKeyUp([this](auto&&, auto const& args) { Root_KeyUp(args); });
    AddRow(root_, 60, GridUnitType::Pixel);
    AddRow(root_, 1, GridUnitType::Star);

    header_ = Grid{};
    header_.Padding(Thickness{24, 0, 12, 0});
    header_.Background(theme.root);
    header_.BorderBrush(theme.border);
    header_.BorderThickness(Thickness{0, 0, 0, 1});
    header_.ColumnSpacing(6);
    AddColumn(header_, 1, GridUnitType::Star);
    AddColumn(header_, 0, GridUnitType::Auto);
    AddColumn(header_, 0, GridUnitType::Auto);
    AddColumn(header_, 0, GridUnitType::Auto);
    const Microsoft::UI::Xaml::Window window = *this;
    window.SetTitleBar(header_);
    StackPanel heading;
    heading.Orientation(Orientation::Horizontal);
    heading.Spacing(8);
    heading.VerticalAlignment(VerticalAlignment::Center);
    Border mark;
    mark.Width(8);
    mark.Height(8);
    mark.CornerRadius(CornerRadius{2, 2, 2, 2});
    mark.Background(theme.accent);
    mark.VerticalAlignment(VerticalAlignment::Center);
    heading.Children().Append(mark);
    title_ = Text(L"Clips", 14, theme.primary_text);
    title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    heading.Children().Append(title_);
    subtitle_ = Text(L"", 12.5, theme.faint);
    subtitle_.Margin(Thickness{6, 0, 0, 0});
    subtitle_.VerticalAlignment(VerticalAlignment::Center);
    heading.Children().Append(subtitle_);
    header_.Children().Append(heading);
    refresh_button_ = controls.IconButton(L"\xE72C", L"Refresh");
    refresh_button_.VerticalAlignment(VerticalAlignment::Center);
    refresh_button_.Click([this](auto&&, auto&&) { RefreshClips(); });
    Grid::SetColumn(refresh_button_, 1);
    header_.Children().Append(refresh_button_);
    maximize_button_ = controls.IconButton(L"\xE922", L"Maximize");
    maximize_icon_ = maximize_button_.Content().as<FontIcon>();
    maximize_button_.VerticalAlignment(VerticalAlignment::Center);
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
    AddColumn(content_, kListWidth, GridUnitType::Pixel);
    AddColumn(content_, 1, GridUnitType::Star);
    Grid::SetRow(content_, 1);

    list_scroll_ = ScrollViewer{};
    list_scroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    list_scroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    list_scroll_.BorderBrush(theme.border);
    list_scroll_.BorderThickness(Thickness{0, 0, 1, 0});
    clip_list_ = StackPanel{};
    clip_list_.Padding(Thickness{0, 0, 0, 12});
    empty_text_ = Text(L"No clips found", 13, theme.secondary_text);
    empty_text_.TextWrapping(TextWrapping::Wrap);
    empty_text_.Margin(Thickness{20, 16, 20, 16});
    clip_list_.Children().Append(empty_text_);
    list_scroll_.Content(clip_list_);
    Grid::SetColumn(list_scroll_, 0);
    content_.Children().Append(list_scroll_);

    player_column_ = Grid{};
    player_column_.Padding(Thickness{20, 18, 20, 18});
    player_column_.RowSpacing(14);
    AddRow(player_column_, 1, GridUnitType::Star);
    AddRow(player_column_, 0, GridUnitType::Auto);

    player_surface_ = Border{};
    player_surface_.Background(Brush(0xFF000000));
    player_surface_.CornerRadius(CornerRadius{8, 8, 8, 8});
    Grid video_viewport;
    player_ = MediaPlayerElement{};
    media_player_ = Windows::Media::Playback::MediaPlayer{};
    media_player_.AutoPlay(false);
    player_.SetMediaPlayer(media_player_);
    player_.AreTransportControlsEnabled(false);
    player_.AutoPlay(false);
    player_.Stretch(Stretch::Uniform);
    player_.HorizontalAlignment(HorizontalAlignment::Stretch);
    player_.VerticalAlignment(VerticalAlignment::Stretch);
    player_.PointerPressed([this](auto&&, auto const& args) {
        play_button_.Focus(FocusState::Programmatic);
        TogglePlayback();
        args.Handled(true);
    });
    video_viewport.Children().Append(player_);

    playback_feedback_ = Border{};
    playback_feedback_.Width(76);
    playback_feedback_.Height(76);
    playback_feedback_.Background(Brush(0xB81B1718));
    playback_feedback_.CornerRadius(CornerRadius{38, 38, 38, 38});
    playback_feedback_.HorizontalAlignment(HorizontalAlignment::Center);
    playback_feedback_.VerticalAlignment(VerticalAlignment::Center);
    playback_feedback_.IsHitTestVisible(false);
    playback_feedback_.Opacity(0);
    playback_feedback_.CenterPoint(Windows::Foundation::Numerics::float3{38.0f, 38.0f, 0.0f});
    playback_feedback_.Scale(Windows::Foundation::Numerics::float3{0.82f, 0.82f, 1.0f});
    playback_feedback_icon_ = FontIcon{};
    playback_feedback_icon_.Glyph(L"\xE768");
    playback_feedback_icon_.FontSize(30);
    playback_feedback_icon_.Foreground(theme.primary_text);
    playback_feedback_.Child(playback_feedback_icon_);
    video_viewport.Children().Append(playback_feedback_);

    // Transport sits over the bottom of the video on a fade so the picture keeps the full height.
    transport_surface_ = Border{};
    transport_surface_.VerticalAlignment(VerticalAlignment::Bottom);
    transport_surface_.Padding(Thickness{16, 40, 16, 12});
    Media::LinearGradientBrush fade;
    fade.StartPoint(Windows::Foundation::Point{0, 0});
    fade.EndPoint(Windows::Foundation::Point{0, 1});
    Media::GradientStop fade_top;
    fade_top.Color(Windows::UI::Color{0, 0, 0, 0});
    fade_top.Offset(0);
    Media::GradientStop fade_bottom;
    fade_bottom.Color(Windows::UI::Color{0xCC, 0, 0, 0});
    fade_bottom.Offset(1);
    fade.GradientStops().Append(fade_top);
    fade.GradientStops().Append(fade_bottom);
    transport_surface_.Background(fade);

    Grid transport_row;
    transport_row.ColumnSpacing(12);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 1, GridUnitType::Star);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 84, GridUnitType::Pixel);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    AddColumn(transport_row, 0, GridUnitType::Auto);
    play_button_ = controls.IconButton(L"\xE768", L"Play / Pause", ButtonKind::Accent);
    play_button_.Width(34);
    play_button_.Height(34);
    play_button_.MinWidth(34);
    play_button_.CornerRadius(CornerRadius{17, 17, 17, 17});
    play_icon_ = play_button_.Content().as<FontIcon>();
    play_icon_.FontSize(13);
    play_button_.Click([this](auto&&, auto&&) { TogglePlayback(); });
    transport_row.Children().Append(play_button_);
    playback_time_ = Text(L"00:00 / 00:00", 12, Brush(0xFFD4D4D8));
    playback_time_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(playback_time_, 1);
    transport_row.Children().Append(playback_time_);
    auto timeline_view = controls.RangeSlider(0, 1, 0.05, 0);
    timeline_ = timeline_view.input;
    timeline_.ValueChanged([this](auto&&, auto&&) {
        if (updating_timeline_ || !media_player_ || !player_source_) return;
        const auto position = std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
            std::chrono::duration<double>{timeline_.Value()});
        media_player_.PlaybackSession().Position(position);
    });
    timeline_view.view.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(timeline_view.view, 2);
    transport_row.Children().Append(timeline_view.view);
    FontIcon volume_icon;
    volume_icon.Glyph(L"\xE767");
    volume_icon.FontSize(14);
    volume_icon.Foreground(Brush(0xFFD4D4D8));
    volume_icon.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(volume_icon, 3);
    transport_row.Children().Append(volume_icon);
    auto volume_view = controls.RangeSlider(0, 100, 1, 100);
    volume_ = volume_view.input;
    volume_.ValueChanged([this](auto&&, auto&&) {
        if (media_player_) media_player_.Volume(volume_.Value() / 100.0);
    });
    volume_view.view.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(volume_view.view, 4);
    transport_row.Children().Append(volume_view.view);
    speed_selector_ = controls.Select();
    speed_selector_.Items().Append(controls.Option(L"0.25x"));
    speed_selector_.Items().Append(controls.Option(L"0.5x"));
    speed_selector_.Items().Append(controls.Option(L"0.75x"));
    speed_selector_.Items().Append(controls.Option(L"1x"));
    speed_selector_.Items().Append(controls.Option(L"1.25x"));
    speed_selector_.Items().Append(controls.Option(L"1.5x"));
    speed_selector_.Items().Append(controls.Option(L"2x"));
    speed_selector_.SelectedIndex(3);
    speed_selector_.MinWidth(76);
    speed_selector_.VerticalAlignment(VerticalAlignment::Center);
    speed_selector_.SelectionChanged([this](auto&&, auto&&) {
        if (!media_player_ || speed_selector_.SelectedIndex() < 0) return;
        CancelSpaceHold();
        constexpr std::array<double, 7> rates{0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
        const auto index = static_cast<std::size_t>(speed_selector_.SelectedIndex());
        if (index >= rates.size()) return;
        try {
            media_player_.PlaybackSession().PlaybackRate(rates[index]);
        } catch (...) {
            speed_selector_.SelectedIndex(3);
        }
    });
    Grid::SetColumn(speed_selector_, 5);
    transport_row.Children().Append(speed_selector_);
    fullscreen_button_ = controls.IconButton(L"\xE740", L"Full screen");
    fullscreen_icon_ = fullscreen_button_.Content().as<FontIcon>();
    fullscreen_button_.Click([this](auto&&, auto&&) { ToggleFullscreen(); });
    Grid::SetColumn(fullscreen_button_, 6);
    transport_row.Children().Append(fullscreen_button_);
    transport_surface_.Child(transport_row);
    video_viewport.Children().Append(transport_surface_);
    player_surface_.Child(video_viewport);
    Grid::SetRow(player_surface_, 0);
    player_column_.Children().Append(player_surface_);

    info_row_ = Grid{};
    info_row_.ColumnSpacing(8);
    AddColumn(info_row_, 1, GridUnitType::Star);
    AddColumn(info_row_, 0, GridUnitType::Auto);
    AddColumn(info_row_, 0, GridUnitType::Auto);
    StackPanel details;
    details.Margin(Thickness{0, 0, 8, 0});
    selected_title_ = Text(L"Select a clip", 18, theme.primary_text);
    selected_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    selected_title_.TextTrimming(TextTrimming::CharacterEllipsis);
    details.Children().Append(selected_title_);
    selected_metadata_ = Text(L"", 12, theme.muted);
    selected_metadata_.Margin(Thickness{0, 3, 0, 0});
    selected_metadata_.TextTrimming(TextTrimming::CharacterEllipsis);
    details.Children().Append(selected_metadata_);
    StackPanel file_row;
    file_row.Orientation(Orientation::Horizontal);
    file_row.Spacing(6);
    file_row.Margin(Thickness{0, 4, 0, 0});
    FontIcon file_icon;
    file_icon.Glyph(L"\xE8A5");
    file_icon.FontSize(11);
    file_icon.Foreground(theme.muted);
    file_row.Children().Append(file_icon);
    selected_file_ = Text(L"", 11.5, theme.faint);
    selected_file_.TextTrimming(TextTrimming::CharacterEllipsis);
    file_row.Children().Append(selected_file_);
    details.Children().Append(file_row);
    info_row_.Children().Append(details);

    actions_ = StackPanel{};
    actions_.Orientation(Orientation::Horizontal);
    actions_.Spacing(8);
    actions_.VerticalAlignment(VerticalAlignment::Top);
    const auto action_icon = [&](std::wstring_view glyph, Media::Brush const& color) {
        auto button = controls.IconButton(glyph, L"");
        button.Width(44);
        button.Height(44);
        button.MinWidth(44);
        button.IsEnabled(false);
        if (color) button.Content().as<FontIcon>().Foreground(color);
        actions_.Children().Append(button);
        return button;
    };
    open_button_ = action_icon(L"\xE8B7", nullptr);
    open_button_.Click([this](auto&&, auto&&) { OpenSelectedClip(); });
    file_copy_button_ = action_icon(L"\xE8C8", nullptr);
    file_copy_icon_ = file_copy_button_.Content().as<FontIcon>();
    file_copy_button_.Click([this](auto&&, auto&&) { CopySelectedFile(); });
    delete_button_ = action_icon(L"\xE74D", theme.danger);
    delete_button_.Click([this](auto&&, auto&&) { DeleteSelectedClipAsync(); });
    webhook_button_ = action_icon(L"\xE724", Brush(0xFF8A93FF));
    webhook_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(true); });
    Grid::SetColumn(actions_, 1);
    info_row_.Children().Append(actions_);

    discord_copy_button_ = controls.ActionButton(L"", ButtonKind::Accent);
    discord_copy_button_.Height(44);
    discord_copy_button_.Padding(Thickness{16, 0, 16, 0});
    discord_copy_button_.CornerRadius(CornerRadius{9, 9, 9, 9});
    discord_copy_button_.VerticalAlignment(VerticalAlignment::Top);
    discord_copy_button_.IsEnabled(false);
    // The labels set their own color, so the disabled state would leave dark text on the dark disabled fill.
    discord_copy_button_.IsEnabledChanged([this](auto&&, auto&&) {
        const auto palette = DarkTheme();
        const auto foreground = discord_copy_button_.IsEnabled() ? palette.accent_text : palette.primary_text;
        copy_label_.Foreground(foreground);
        copy_hint_.Foreground(foreground);
    });
    StackPanel copy_content;
    copy_content.VerticalAlignment(VerticalAlignment::Center);
    copy_label_ = Text(L"Copy", 13.5, theme.primary_text);
    copy_label_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    copy_content.Children().Append(copy_label_);
    copy_hint_ = Text(L"", 11, theme.primary_text);
    copy_hint_.Opacity(0.7);
    copy_hint_.Visibility(Visibility::Collapsed);
    copy_content.Children().Append(copy_hint_);
    discord_copy_button_.Content(copy_content);
    discord_copy_button_.Click([this](auto&&, auto&&) { ShareSelectedClip(false); });
    Grid::SetColumn(discord_copy_button_, 2);
    info_row_.Children().Append(discord_copy_button_);
    Grid::SetRow(info_row_, 1);
    player_column_.Children().Append(info_row_);

    Grid::SetColumn(player_column_, 1);
    content_.Children().Append(player_column_);
    root_.Children().Append(content_);
    Content(root_);
}

void ClipLibraryWindow::ApplyLanguage() {
    if (!title_) return;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    const auto tip = [](auto const& target, std::wstring_view text) {
        ToolTipService::SetToolTip(target, winrt::box_value(winrt::hstring{text}));
    };
    title_.Text(english_ ? L"Clips" : L"Клипы");
    tip(refresh_button_, english_ ? L"Refresh" : L"Обновить");
    tip(maximize_button_, english_ ? L"Fill beside overlay / restore" : L"Заполнить рядом с оверлеем / восстановить");
    tip(fullscreen_button_, fullscreen_ ? (english_ ? L"Exit full screen" : L"Выйти из полноэкранного режима")
                                        : (english_ ? L"Full screen" : L"Полноэкранный режим"));
    tip(play_button_, english_ ? L"Play / Pause (Space); hold Space for 2x"
                               : L"Пауза / продолжить (Пробел); удерживайте Пробел для 2x");
    tip(speed_selector_, english_ ? L"Playback speed" : L"Скорость воспроизведения");
    tip(open_button_, english_ ? L"Show in folder" : L"Показать в папке");
    tip(file_copy_button_, english_ ? L"Copy file as is" : L"Скопировать файл без изменений");
    tip(delete_button_, english_ ? L"Delete" : L"Удалить");
    tip(webhook_button_, english_ ? L"Send to Discord channel (webhook)" : L"Отправить в канал Discord (webhook)");
    copy_label_.Text(english_ ? L"Copy" : L"Скопировать");
    webhook_button_.Visibility(webhook_available_ ? Visibility::Visible : Visibility::Collapsed);
    if (selected_clip_.empty()) {
        selected_title_.Text(english_ ? L"Select a clip" : L"Выберите клип");
        UpdateCopyHint();
    } else {
        UpdateCopyHint();
    }
}

void ClipLibraryWindow::RefreshClips() {
    if (!clip_list_) return;
    const auto generation = ++clip_list_generation_;
    while (clip_list_.Children().Size() > 1) clip_list_.Children().RemoveAtEnd();
    clip_cards_.clear();

    struct Clip {
        std::filesystem::path path;
        std::filesystem::file_time_type written;
    };
    std::vector<Clip> clips;
    for (const auto& entry : ListClips(output_directory_)) {
        std::error_code time_error;
        clips.push_back({entry.path(), entry.last_write_time(time_error)});
    }
    std::ranges::sort(clips, [](const Clip& left, const Clip& right) {
        return left.written != right.written ? left.written > right.written
                                             : left.path.filename() > right.path.filename();
    });
    const auto copy_of = [this](const std::filesystem::path& clip) {
        auto copy = clip.parent_path() / L"Discord" / (clip.stem().wstring() + L"-discord.mp4");
        std::error_code copy_error;
        return std::filesystem::is_regular_file(copy, copy_error) ? copy : std::filesystem::path{};
    };
    if (!selected_clip_.empty()) {
        const bool listed = std::ranges::any_of(clips, [&](const Clip& clip) {
            return clip.path == selected_clip_ || copy_of(clip.path) == selected_clip_;
        });
        if (!listed) ResetSelection();
    }

    subtitle_.Text(clips.empty() ? L"" : std::to_wstring(clips.size()));
    empty_text_.Visibility(clips.empty() ? Visibility::Visible : Visibility::Collapsed);
    empty_text_.Text(english_ ? L"No MKV or MP4 clips found in the output folder"
                              : L"В папке сохранения нет клипов MKV или MP4");
    if (clips.empty()) return;

    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    const auto theme = DarkTheme();
    std::vector<ClipVisual> visuals;
    const auto add_row = [&](const std::filesystem::path& path, bool compact) {
        Border row;
        row.Background(Brush(0x00000000));
        row.BorderBrush(Brush(0x00000000));
        row.BorderThickness(Thickness{2, 0, 0, 0});
        row.Padding(compact ? Thickness{0, 0, 16, 0} : Thickness{14, 9, 16, 9});
        row.PointerEntered([this, path, theme](auto const& sender, auto&&) {
            if (path != selected_clip_) sender.template as<Border>().Background(theme.card);
        });
        row.PointerExited([this, path](auto const& sender, auto&&) {
            if (path != selected_clip_) sender.template as<Border>().Background(Brush(0x00000000));
        });
        row.PointerPressed([this, path](auto&&, auto&&) { SelectClip(path); });

        Grid content;
        content.ColumnSpacing(compact ? 10 : 12);
        AddColumn(content, compact ? 64 : 96, GridUnitType::Pixel);
        AddColumn(content, 1, GridUnitType::Star);
        Border frame;
        frame.Height(compact ? 36 : 54);
        frame.CornerRadius(CornerRadius{5, 5, 5, 5});
        frame.Background(theme.card);
        Grid frame_content;
        Image thumbnail;
        thumbnail.Stretch(Stretch::UniformToFill);
        frame_content.Children().Append(thumbnail);
        TextBlock badge{nullptr};
        if (!compact) {
            Border badge_surface;
            badge_surface.Background(Brush(0xBB000000));
            badge_surface.CornerRadius(CornerRadius{3, 3, 3, 3});
            badge_surface.Padding(Thickness{5, 1, 5, 1});
            badge_surface.Margin(Thickness{0, 0, 4, 4});
            badge_surface.HorizontalAlignment(HorizontalAlignment::Right);
            badge_surface.VerticalAlignment(VerticalAlignment::Bottom);
            badge = Text(L"", 10, Brush(0xFFD4D4D8));
            badge_surface.Child(badge);
            frame_content.Children().Append(badge_surface);
        }
        frame.Child(frame_content);
        content.Children().Append(frame);

        StackPanel copy;
        copy.VerticalAlignment(VerticalAlignment::Center);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        std::error_code time_error;
        const auto written = std::filesystem::last_write_time(path, time_error);
        auto title = Text(compact ? (english_ ? L"For Discord" : L"Для Discord") + (size_error ? L"" : L" · " + SizeText(size, english_))
                                  : ClipTitle(path, english_) + (time_error ? L"" : L" · " + ClipClock(written)),
                          compact ? 12.5 : 13.5, theme.primary_text);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        copy.Children().Append(title);
        auto detail = Text(compact || size_error ? L"" : SizeText(size, english_), 11.5, theme.muted);
        detail.Margin(Thickness{0, 3, 0, 0});
        detail.TextTrimming(TextTrimming::CharacterEllipsis);
        copy.Children().Append(detail);
        Grid::SetColumn(copy, 1);
        content.Children().Append(copy);
        if (compact) {
            // Branch from the parent clip down to this copy; it runs from the row top to its middle.
            Grid branch_row;
            AddColumn(branch_row, 50, GridUnitType::Pixel);
            AddColumn(branch_row, 1, GridUnitType::Star);
            Grid branch_column;
            AddRow(branch_column, 1, GridUnitType::Star);
            AddRow(branch_column, 1, GridUnitType::Star);
            Border branch;
            branch.Width(10);
            branch.Margin(Thickness{34, 0, 0, 0});
            branch.HorizontalAlignment(HorizontalAlignment::Left);
            branch.BorderBrush(theme.control_border);
            branch.BorderThickness(Thickness{1, 0, 0, 1});
            branch.CornerRadius(CornerRadius{0, 0, 0, 5});
            branch_column.Children().Append(branch);
            branch_row.Children().Append(branch_column);
            content.Margin(Thickness{0, 4, 0, 8});
            Grid::SetColumn(content, 1);
            branch_row.Children().Append(content);
            row.Child(branch_row);
        } else {
            row.Child(content);
        }
        clip_list_.Children().Append(row);
        clip_cards_.emplace_back(path, row);
        visuals.push_back({path, thumbnail, badge, detail, compact});
    };

    std::wstring current_day;
    for (const auto& clip : clips) {
        auto day = ClipDay(clip.written, english_);
        CharUpperBuffW(day.data(), static_cast<DWORD>(day.size()));
        if (day != current_day) {
            current_day = day;
            auto group = Text(day, 10.5, theme.faint);
            group.CharacterSpacing(120);
            group.Margin(Thickness{20, 16, 20, 8});
            clip_list_.Children().Append(group);
        }
        add_row(clip.path, false);
        if (const auto copy = copy_of(clip.path); !copy.empty()) add_row(copy, true);
    }
    UpdateSelectionStyles();
    // Start on the newest clip, so the library never opens with its actions disabled.
    if (selected_clip_.empty()) SelectClip(clips.front().path);
    LoadClipVisualsAsync(std::move(visuals), generation);
}

void ClipLibraryWindow::ResetSelection() {
    CancelSpaceHold();
    ++source_generation_;
    selected_clip_.clear();
    if (media_player_) {
        media_player_.Pause();
        media_player_.Source(nullptr);
    }
    player_source_ = nullptr;
    open_button_.IsEnabled(false);
    file_copy_button_.IsEnabled(false);
    delete_button_.IsEnabled(false);
    discord_copy_button_.IsEnabled(false);
    webhook_button_.IsEnabled(false);
    selected_title_.Text(english_ ? L"Select a clip" : L"Выберите клип");
    selected_metadata_.Text(L"");
    selected_file_.Text(L"");
    UpdateCopyHint();
    timeline_.Value(0);
    playback_time_.Text(L"00:00 / 00:00");
    play_icon_.Glyph(L"\xE768");
    if (playback_feedback_timer_) playback_feedback_timer_.Stop();
    if (playback_feedback_) playback_feedback_.Opacity(0);
}

void ClipLibraryWindow::UpdateSelectionStyles() {
    const auto theme = DarkTheme();
    for (const auto& [path, row] : clip_cards_) {
        const bool selected = path == selected_clip_;
        row.Background(selected ? Brush(0x1476B900) : Brush(0x00000000));
        row.BorderBrush(selected ? theme.accent : Brush(0x00000000));
    }
}

void ClipLibraryWindow::UpdateCopyHint() {
    std::error_code error;
    const auto size = std::filesystem::file_size(selected_clip_, error);
    const auto limit = static_cast<std::uintmax_t>(std::clamp(share_limit_megabytes_, 5U, 500U)) * 1024U * 1024U;
    if (error) copy_hint_.Text(L"");
    else if (size > limit) {
        copy_hint_.Text(english_ ? std::format(L"compresses to {} MB", share_limit_megabytes_)
                                 : std::format(L"сожмётся до {} МБ", share_limit_megabytes_));
    } else {
        copy_hint_.Text((english_ ? L"no compression · " : L"без сжатия · ") + SizeText(size, english_));
    }
    // An empty hint still takes a line and pushes the label off center.
    copy_hint_.Visibility(copy_hint_.Text().empty() ? Microsoft::UI::Xaml::Visibility::Collapsed
                                                    : Microsoft::UI::Xaml::Visibility::Visible);
}

void ClipLibraryWindow::SelectClip(const std::filesystem::path& path) {
    if (path == selected_clip_ && player_source_) return;
    CancelSpaceHold();
    play_button_.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
    ++source_generation_;
    selected_clip_ = path;
    open_button_.IsEnabled(true);
    file_copy_button_.IsEnabled(true);
    file_copy_icon_.Glyph(L"\xE8C8");
    delete_button_.IsEnabled(true);
    discord_copy_button_.IsEnabled(share_callback_ != nullptr);
    webhook_button_.IsEnabled(webhook_available_ && share_callback_ != nullptr);
    const bool discord_copy = path.parent_path().filename() == L"Discord";
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    selected_title_.Text(ClipTitle(path, english_) +
                         (error ? L"" : L" · " + ClipDay(written, english_) + L", " + ClipClock(written)) +
                         (discord_copy ? (english_ ? L" · for Discord" : L" · для Discord") : L""));
    const auto relative = path.lexically_relative(output_directory_);
    selected_file_.Text(relative.empty() || *relative.begin() == L".." ? path.filename().wstring()
                                                                       : relative.generic_wstring());
    selected_metadata_.Text(english_ ? L"Loading media..." : L"Загрузка медиа...");
    UpdateCopyHint();
    UpdateSelectionStyles();
    OpenClipAsync(path);
}

void ClipLibraryWindow::ShowSharedCopy(const std::filesystem::path& path) {
    RefreshClips();
    if (std::ranges::any_of(clip_cards_, [&](const auto& card) { return card.first == path; })) SelectClip(path);
}

void ClipLibraryWindow::CopySelectedFile() {
    if (selected_clip_.empty()) return;
    const Microsoft::UI::Xaml::Window window = *this;
    HWND handle{};
    if (FAILED(window.as<::IWindowNative>()->get_WindowHandle(&handle))) return;
    const bool copied = openreplay::ui::DiscordShareService::CopyFileToClipboard(handle, selected_clip_);
    file_copy_icon_.Glyph(copied ? L"\xE73E" : L"\xE783");
}

winrt::fire_and_forget ClipLibraryWindow::OpenClipAsync(std::filesystem::path path) {
    const auto weak = get_weak();
    const auto generation = source_generation_;
    try {
        const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path.wstring());
        std::wstring details;
        try {
            const auto video = co_await file.Properties().GetVideoPropertiesAsync();
            const auto basic = co_await file.GetBasicPropertiesAsync();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(video.Duration());
            auto bitrate = static_cast<std::uint64_t>(video.Bitrate());
            if (!bitrate && duration.count() > 0) {
                bitrate = static_cast<std::uint64_t>(static_cast<long double>(basic.Size()) * 8000.0L / duration.count());
            }
            const auto self = weak.get();
            const bool english = !self || self->english_;
            details = JoinDetails({DurationText(duration), SizeText(basic.Size(), english),
                                   ResolutionText(video.Width(), video.Height()), BitrateText(bitrate, english)});
        } catch (...) {
        }
        if (const auto self = weak.get()) {
            if (self->selected_clip_ != path || self->source_generation_ != generation) co_return;
            self->CancelSpaceHold();
            const auto source = winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
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
            if (self->selected_clip_ == path && self->source_generation_ == generation) {
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
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(session.NaturalDuration());
        const auto position = std::chrono::duration_cast<std::chrono::milliseconds>(session.Position());
        if (duration.count() > 0 && position + std::chrono::milliseconds{100} >= duration) {
            session.Position(Windows::Foundation::TimeSpan{});
        }
        media_player_.Play();
    }
    play_icon_.Glyph(playing ? L"\xE768" : L"\xE769");
    ShowPlaybackFeedback(!playing);
    UpdatePlaybackUi();
}

void ClipLibraryWindow::ShowPlaybackFeedback(bool playing) {
    if (!playback_feedback_ || !playback_feedback_icon_ || !playback_feedback_timer_) return;
    playback_feedback_icon_.Glyph(playing ? L"\xE768" : L"\xE769");
    playback_feedback_.Opacity(1);
    playback_feedback_.Scale(Windows::Foundation::Numerics::float3{0.82f, 0.82f, 1.0f});
    playback_feedback_started_ = std::chrono::steady_clock::now();
    playback_feedback_timer_.Start();
}

void ClipLibraryWindow::UpdatePlaybackFeedback() {
    if (!playback_feedback_ || !playback_feedback_timer_) return;
    constexpr auto duration = std::chrono::milliseconds{420};
    const auto elapsed = std::chrono::steady_clock::now() - playback_feedback_started_;
    const auto progress = std::clamp(
        static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) /
            duration.count(),
        0.0, 1.0);
    const auto eased = 1.0 - std::pow(1.0 - progress, 3.0);
    const auto scale = static_cast<float>(0.82 + 0.18 * eased);
    playback_feedback_.Scale(Windows::Foundation::Numerics::float3{scale, scale, 1.0f});
    playback_feedback_.Opacity(progress < 0.2 ? 1.0 : std::max(0.0, (1.0 - progress) / 0.8));
    if (progress >= 1.0) {
        playback_feedback_.Opacity(0);
        playback_feedback_timer_.Stop();
    }
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
    if (!player_ || (!fullscreen_ && !player_source_)) return;
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
    const auto arguments = L"/select,\"" + selected_clip_.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

winrt::fire_and_forget ClipLibraryWindow::DeleteSelectedClipAsync() {
    const auto path = selected_clip_;
    if (path.empty() || delete_in_progress_) co_return;
    CancelSpaceHold();
    delete_in_progress_ = true;
    delete_button_.IsEnabled(false);
    const auto weak = get_weak();
    try {
        Microsoft::UI::Xaml::Controls::ContentDialog confirmation;
        confirmation.XamlRoot(root_.XamlRoot());
        confirmation.Title(winrt::box_value(english_ ? L"Delete clip?" : L"Удалить клип?"));
        confirmation.Content(winrt::box_value(path.filename().wstring()));
        confirmation.PrimaryButtonText(english_ ? L"Move to Recycle Bin" : L"Переместить в Корзину");
        confirmation.CloseButtonText(english_ ? L"Cancel" : L"Отмена");
        confirmation.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
        const auto result = co_await confirmation.ShowAsync();

        const auto self = weak.get();
        if (!self) co_return;
        if (result != Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary ||
            self->selected_clip_ != path) {
            self->delete_in_progress_ = false;
            self->delete_button_.IsEnabled(!self->selected_clip_.empty());
            co_return;
        }

        ++self->source_generation_;
        ++self->clip_list_generation_;
        self->media_player_.Pause();
        self->media_player_.Source(nullptr);
        self->player_source_ = nullptr;
        MoveToRecycleBin(path);
        self->delete_in_progress_ = false;
        if (self->selected_clip_ == path) {
            self->ResetSelection();
        }
        self->RefreshClips();
    } catch (...) {
        if (const auto self = weak.get()) {
            self->delete_in_progress_ = false;
            self->delete_button_.IsEnabled(!self->selected_clip_.empty());
            if (self->selected_clip_ == path) {
                self->selected_metadata_.Text(self->english_ ? L"Unable to move this clip to the Recycle Bin"
                                                              : L"Не удалось переместить клип в Корзину");
            }
        }
    }
}

void ClipLibraryWindow::ShareSelectedClip(bool webhook) {
    if (selected_clip_.empty() || !share_callback_) return;
    share_callback_(selected_clip_, webhook);
}

// One clip at a time, top of the list first: dozens of parallel thumbnail decodes stall the UI thread.
winrt::fire_and_forget ClipLibraryWindow::LoadClipVisualsAsync(std::vector<ClipVisual> visuals,
                                                               std::uint64_t generation) {
    const auto weak = get_weak();
    for (const auto& visual : visuals) {
        try {
            const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(visual.path.wstring());
            if (const auto self = weak.get(); !self || self->clip_list_generation_ != generation) co_return;
            const auto thumbnail = co_await file.GetThumbnailAsync(
                winrt::Windows::Storage::FileProperties::ThumbnailMode::VideosView, 192,
                winrt::Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);
            winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(thumbnail);
            thumbnail.Close();
            if (const auto self = weak.get(); !self || self->clip_list_generation_ != generation) co_return;
            const auto video = co_await file.Properties().GetVideoPropertiesAsync();
            const auto basic = co_await file.GetBasicPropertiesAsync();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(video.Duration());
            auto bitrate = static_cast<std::uint64_t>(video.Bitrate());
            if (!bitrate && duration.count() > 0) {
                bitrate = static_cast<std::uint64_t>(
                    static_cast<long double>(basic.Size()) * 8000.0L / duration.count());
            }
            const auto self = weak.get();
            if (!self || self->clip_list_generation_ != generation) co_return;
            visual.image.Source(bitmap);
            if (visual.badge) visual.badge.Text(DurationText(duration));
            const auto bitrate_text = BitrateText(bitrate, self->english_);
            visual.detail.Text(visual.compact
                                   ? JoinDetails({ResolutionText(video.Width(), video.Height()), bitrate_text})
                                   : JoinDetails({SizeText(basic.Size(), self->english_), bitrate_text}));
        } catch (...) {
        }
    }
}

}  // namespace winrt::OpenReplay::implementation

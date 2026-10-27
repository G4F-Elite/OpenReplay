#include "pch.h"

#include "MainWindow.xaml.h"
#include "StartupTrace.h"
#include "UiControls.h"
#include "WindowPlacement.h"

#include "openreplay/Version.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "microsoft.ui.xaml.window.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

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
using openreplay::ui::ListClips;
using openreplay::ui::PublicSans;
using openreplay::ui::Text;

constexpr UINT_PTR kNotificationAnimationTimer = 1;
constexpr UINT_PTR kNotificationHideTimer = 2;
constexpr int kBufferSegments = 30;
constexpr int kNotificationWidth = 360;

enum class NotificationAction { Copy, Webhook, OpenFolder, CopyPath };
using NotificationActions = std::vector<std::pair<NotificationAction, std::wstring>>;

NotificationActions NotificationActionsFor(bool has_output, bool shareable, bool webhook, bool english) {
    NotificationActions actions;
    if (has_output && shareable) {
        actions.emplace_back(NotificationAction::Copy, english ? L"Copy" : L"Скопировать");
        if (webhook) actions.emplace_back(NotificationAction::Webhook, english ? L"Send webhook" : L"Webhook");
    }
    if (has_output) {
        actions.emplace_back(NotificationAction::OpenFolder, english ? L"Open folder" : L"Открыть папку");
        actions.emplace_back(NotificationAction::CopyPath, english ? L"Copy path" : L"Копировать путь");
    }
    return actions;
}

struct NotificationFonts {
    HFONT title{};
    HFONT body{};
    HFONT action{};

    explicit NotificationFonts(UINT dpi) {
        const auto make = [dpi](int size, int weight) {
            return CreateFontW(-openreplay::ui::ScaleForDpi(size, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Public Sans");
        };
        title = make(14, FW_SEMIBOLD);
        body = make(12, FW_NORMAL);
        action = make(12, FW_SEMIBOLD);
    }
    ~NotificationFonts() {
        for (const auto font : {title, body, action}) {
            if (font) DeleteObject(font);
        }
    }
    NotificationFonts(const NotificationFonts&) = delete;
    NotificationFonts& operator=(const NotificationFonts&) = delete;
};

struct NotificationLayout {
    int height{};
    RECT marker{};
    RECT title{};
    RECT message{};
    std::vector<std::pair<NotificationAction, RECT>> actions;
};

// The only source of notification geometry: window size, painting and hit testing all use it.
NotificationLayout MeasureNotification(HDC dc, UINT dpi, const NotificationFonts& fonts, std::wstring_view message,
                                       const NotificationActions& actions) {
    const auto scale = [dpi](int value) { return openreplay::ui::ScaleForDpi(value, dpi); };
    const auto old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    const auto measure = [dc](HFONT font, std::wstring_view text, int width, UINT format) {
        SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        RECT rect{0, 0, width, 0};
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, format | DT_CALCRECT | DT_NOPREFIX);
        return rect;
    };
    NotificationLayout layout;
    const int left = scale(34);
    const int right = scale(kNotificationWidth) - scale(16);
    int y = scale(14);
    const int title_height = measure(fonts.title, L"Ag", right - left, DT_SINGLELINE).bottom;
    layout.title = {left, y, right, y + title_height};
    const int marker_top = y + (title_height - scale(8)) / 2;
    layout.marker = {scale(16), marker_top, scale(24), marker_top + scale(8)};
    y = layout.title.bottom;
    if (!message.empty()) {
        y += scale(3);
        const int line = measure(fonts.body, L"Ag", right - left, DT_SINGLELINE).bottom;
        const int height = measure(fonts.body, message, right - left, DT_WORDBREAK | DT_EDITCONTROL).bottom;
        layout.message = {left, y, right, y + std::min(height, line * 3)};
        y = layout.message.bottom;
    }
    if (!actions.empty()) {
        y += scale(10);
        const int row_height = measure(fonts.action, L"Ag", right - left, DT_SINGLELINE).bottom;
        int x = left;
        for (const auto& [action, label] : actions) {
            const int width = measure(fonts.action, label, right - left, DT_SINGLELINE).right;
            if (x > left && x + width > right) {
                x = left;
                y += row_height + scale(6);
            }
            layout.actions.emplace_back(action, RECT{x, y, x + width, y + row_height});
            x += width + scale(16);
        }
        y += row_height;
    }
    layout.height = y + scale(14);
    SelectObject(dc, old_font);
    return layout;
}
constexpr std::size_t kRecentClipCount = 3;

// One keycap per chord part: "Alt+F10" -> [Alt][F10].
// The game a replay comes from: the title of a fullscreen window. Empty for anything else, so a browser or
// desktop session never leaks a page title into a file name.
std::wstring GameNameOf(HWND window) {
    if (!window || !IsWindow(window) || !openreplay::ui::IsFullscreenWindow(window)) return {};
    std::array<wchar_t, 256> title{};
    GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    return openreplay::GameFileName(title.data());
}

void FillKeycaps(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& panel, std::string_view chord,
                 bool on_accent) {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    panel.Children().Clear();
    const auto theme = DarkTheme();
    std::size_t start = 0;
    while (start < chord.size()) {
        const auto end = std::min(chord.find('+', start), chord.size());
        const auto part = chord.substr(start, end - start);
        start = end + 1;
        if (part.empty()) continue;
        Border cap;
        cap.MinWidth(24);
        cap.Padding(Thickness{6, 2, 6, 3});
        cap.CornerRadius(CornerRadius{5, 5, 5, 5});
        cap.BorderThickness(Thickness{1, 1, 1, 2});
        cap.BorderBrush(on_accent ? Brush(0x400B1200) : theme.control_border);
        cap.Background(on_accent ? Brush(0x140B1200) : Brush(0x00000000));
        auto label = Text(openreplay::FromUtf8(std::string{part}), 11,
                          on_accent ? theme.accent_text : Brush(0xFFC4C4C8));
        label.FontFamily(Media::FontFamily{L"Cascadia Mono, Consolas"});
        label.HorizontalAlignment(HorizontalAlignment::Center);
        cap.Child(label);
        panel.Children().Append(cap);
    }
}

struct WipeString {
    std::wstring& value;
    ~WipeString() { if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
};

std::string Field(const openreplay::Response& response, std::string_view name) {
    const auto found = response.fields.find(std::string{name});
    return found == response.fields.end() ? std::string{} : found->second;
}

bool FieldIsTrue(const openreplay::Response& response, std::string_view name) {
    return Field(response, name) == "true";
}

std::wstring DurationText(long long total_seconds) {
    const auto hours = total_seconds / 3600;
    const auto minutes = total_seconds % 3600 / 60;
    const auto seconds = total_seconds % 60;
    wchar_t value[32]{};
    swprintf_s(value, L"%02lld:%02lld:%02lld", hours, minutes, seconds);
    return value;
}

std::wstring FriendlyMonitorName(std::wstring_view display_name) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return {};

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return {};
    }

    for (UINT32 index = 0; index < path_count; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            display_name != source.viewGdiDeviceName) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[index].targetInfo.adapterId;
        target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0]) {
            return target.monitorFriendlyDeviceName;
        }
    }
    return {};
}

struct AudioDeviceInfo {
    std::string id;
    std::wstring name;
    std::string utf8_name;
    bool is_default{false};
};

std::vector<AudioDeviceInfo> AudioDevices(EDataFlow flow) {
    std::vector<AudioDeviceInfo> result;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return result;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
            IMMDevice* device = nullptr;
            LPWSTR id = nullptr;
            IPropertyStore* properties = nullptr;
            PROPVARIANT friendly_name;
            PropVariantInit(&friendly_name);
            if (SUCCEEDED(collection->Item(index, &device)) &&
                SUCCEEDED(device->GetId(&id)) &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
                SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &friendly_name)) &&
                friendly_name.vt == VT_LPWSTR && friendly_name.pwszVal) {
                result.push_back({openreplay::ToUtf8(id), friendly_name.pwszVal,
                                  openreplay::ToUtf8(friendly_name.pwszVal), false});
            }
            PropVariantClear(&friendly_name);
            if (properties) properties->Release();
            if (id) CoTaskMemFree(id);
            if (device) device->Release();
        }
        IMMDevice* default_device = nullptr;
        LPWSTR default_id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device)) &&
            SUCCEEDED(default_device->GetId(&default_id))) {
            const auto id = openreplay::ToUtf8(default_id);
            for (auto& device : result) device.is_default = device.id == id;
        }
        if (default_id) CoTaskMemFree(default_id);
        if (default_device) default_device->Release();
    }
    if (collection) collection->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

const openreplay::AudioDeviceConfig* FindDevice(
    const std::vector<openreplay::AudioDeviceConfig>& devices, std::string_view id) {
    const auto found = std::ranges::find_if(devices, [id](const auto& device) { return device.id == id; });
    return found == devices.end() ? nullptr : &*found;
}

std::wstring ByteSize(std::uint64_t bytes, bool english) {
    constexpr std::uint64_t megabyte = 1000ULL * 1000ULL;
    constexpr std::uint64_t gigabyte = 1000ULL * megabyte;
    if (bytes >= gigabyte) {
        const auto tenths = (bytes * 10ULL + gigabyte / 2ULL) / gigabyte;
        return std::to_wstring(tenths / 10ULL) + (english ? L"." : L",") +
               std::to_wstring(tenths % 10ULL) + (english ? L" GB" : L" ГБ");
    }
    return std::to_wstring((bytes + megabyte / 2ULL) / megabyte) + (english ? L" MB" : L" МБ");
}

struct NativeHotkey {
    UINT modifiers{};
    UINT key{};
};

NativeHotkey ParseHotkey(std::string_view chord) {
    NativeHotkey result;
    while (!chord.empty()) {
        const auto separator = chord.find('+');
        auto token = chord.substr(0, separator);
        std::string normalized{token};
        for (auto& character : normalized) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        if (normalized == "CTRL" || normalized == "CONTROL") result.modifiers |= MOD_CONTROL;
        else if (normalized == "ALT") result.modifiers |= MOD_ALT;
        else if (normalized == "SHIFT") result.modifiers |= MOD_SHIFT;
        else if (normalized == "WIN" || normalized == "WINDOWS") result.modifiers |= MOD_WIN;
        else if (normalized.size() == 1 && std::isalnum(static_cast<unsigned char>(normalized.front()))) {
            result.key = static_cast<UINT>(normalized.front());
        } else if (normalized.starts_with('F')) {
            try {
                const auto function = std::stoi(normalized.substr(1));
                if (function >= 1 && function <= 24) result.key = VK_F1 + function - 1;
            } catch (...) {
            }
        }
        if (separator == std::string_view::npos) break;
        chord.remove_prefix(separator + 1);
    }
    return result;
}

std::string HotkeyText(UINT key) {
    std::string result;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) result += "Ctrl+";
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) result += "Alt+";
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) result += "Shift+";
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) result += "Win+";
    if (key >= VK_F1 && key <= VK_F24) return result + 'F' + std::to_string(key - VK_F1 + 1);
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        result.push_back(static_cast<char>(key));
        return result;
    }
    return {};
}

bool HostProcessRunning() noexcept {
    const auto mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\OpenReplay.Host.Singleton.v1");
    if (!mutex) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(mutex);
    return true;
}

}  // namespace

namespace winrt::OpenReplay::implementation {

using Microsoft::UI::Xaml::Controls::ComboBoxItem;
using Microsoft::UI::Xaml::Controls::CheckBox;
using Microsoft::UI::Xaml::Controls::StackPanel;
using Microsoft::UI::Xaml::FocusState;
using Microsoft::UI::Xaml::Visibility;

MainWindow::MainWindow() {
    openreplay::ui::TraceStartup(L"MainWindow constructor: BuildUi");
    BuildUi();
    openreplay::ui::TraceStartup(L"MainWindow constructor: InitializeWindow");
    InitializeWindow();
    settings_apply_timer_ = DispatcherQueue().CreateTimer();
    settings_apply_timer_.Interval(std::chrono::milliseconds{650});
    settings_apply_timer_.IsRepeating(false);
    settings_apply_timer_.Tick([this](auto&&, auto&&) { ApplySettingsFromControls(false); });
    openreplay::ui::TraceStartup(L"MainWindow constructor: LoadSettingsIntoControls");
    LoadSettingsIntoControls();
    openreplay::ui::TraceStartup(L"MainWindow constructor: ApplyLanguage");
    ApplyLanguage();
    openreplay::ui::TraceStartup(L"MainWindow constructor: EnsureHostRunning");
    EnsureHostRunning();
    StartPendingHostSettingsApply();

    status_timer_ = DispatcherQueue().CreateTimer();
    status_timer_.Interval(std::chrono::milliseconds{200});
    status_timer_.Tick([this](auto&&, auto&&) { PollStatus(); });
    status_timer_.Start();
    PollStatus();
    openreplay::ui::TraceStartup(L"MainWindow constructor: complete");
}

MainWindow::~MainWindow() {
    if (clip_library_window_owner_) clip_library_window_owner_->Shutdown();
    if (status_timer_) status_timer_.Stop();
    if (settings_apply_timer_) settings_apply_timer_.Stop();
    RemoveTrayIcon();
    UnregisterConfigurableHotkeys();
    if (hotkey_registered_) UnregisterHotKey(window_handle_, kOverlayHotkey);
    if (performance_hotkey_registered_) UnregisterHotKey(window_handle_, kPerformanceOverlayHotkey);
    performance_overlay_.Shutdown();
    if (window_handle_) RemoveWindowSubclass(window_handle_, WindowSubclass, 1);
    for (const auto& notification : notifications_) {
        if (notification->window) DestroyWindow(notification->window);
    }
    notifications_.clear();
    if (private_font_loaded_) {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length && length < executable.size()) {
            const auto font = std::filesystem::path{std::wstring{executable.data(), length}}.parent_path() /
                              L"Assets" / L"PublicSans-Variable.ttf";
            RemoveFontResourceExW(font.c_str(), FR_PRIVATE, nullptr);
        }
    }
}

void MainWindow::ConfigureUpdateLaunch(std::wstring version, std::filesystem::path health_file) {
    post_update_version_ = std::move(version);
    update_health_file_ = std::move(health_file);
}

void MainWindow::BeginUpdateChecks() {
    if (settings_.automatic_updates) CheckForUpdatesAsync(false, true);
}

void MainWindow::BuildUi() {
    openreplay::ui::TraceStartup(L"BuildUi: begin");
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;

    const auto theme = DarkTheme();
    const ControlFactory controls{theme};
    const auto root_brush = theme.root;
    const auto panel_brush = theme.panel;
    const auto card_border_brush = theme.border;
    accent_brush_ = theme.accent;
    success_brush_ = theme.accent_hover;
    const auto primary_text_brush = theme.primary_text;
    secondary_text_brush_ = theme.secondary_text;
    danger_brush_ = theme.danger;
    const auto muted_brush = theme.muted;

    root_ = Grid{};
    root_.Background(root_brush);
    root_.KeyDown({this, &MainWindow::Root_KeyDown});

    Border shell;
    shell.Background(panel_brush);
    shell.BorderBrush(card_border_brush);
    shell.BorderThickness(Thickness{0, 0, 1, 0});
    shell.CornerRadius(CornerRadius{0, 0, 0, 0});
    shell.Margin(Thickness{0, 0, 0, 0});
    shell.HorizontalAlignment(HorizontalAlignment::Stretch);
    shell.VerticalAlignment(VerticalAlignment::Stretch);

    Grid layout;
    AddRow(layout, 0, GridUnitType::Auto);
    AddRow(layout, 0, GridUnitType::Auto);
    AddRow(layout, 1, GridUnitType::Star);
    AddRow(layout, 48, GridUnitType::Pixel);
    shell.Child(layout);

    const auto faint_brush = theme.faint;
    const auto line_brush = theme.border;
    const auto transparent_brush = Brush(0x00000000);
    const auto semibold = Windows::UI::Text::FontWeights::SemiBold();

    const auto make_row = [&](UIElement const& child, Thickness padding = Thickness{24, 13, 24, 13}) {
        Border row;
        row.BorderBrush(line_brush);
        row.BorderThickness(Thickness{0, 0, 0, 1});
        row.Padding(padding);
        row.Child(child);
        return row;
    };
    const auto make_icon = [&](std::wstring_view glyph, double size, Media::Brush const& brush) {
        FontIcon icon;
        icon.Glyph(winrt::hstring{glyph});
        icon.FontSize(size);
        icon.Foreground(brush);
        return icon;
    };
    const auto plain_button = [&](Button const& button) {
        button.Background(transparent_brush);
        button.BorderThickness(Thickness{0, 0, 0, 0});
        button.CornerRadius(CornerRadius{0, 0, 0, 0});
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        button.VerticalContentAlignment(VerticalAlignment::Center);
    };

    Grid header;
    header.Padding(Thickness{24, 16, 12, 0});
    AddColumn(header, 1, GridUnitType::Star);
    AddColumn(header, 0, GridUnitType::Auto);
    Grid::SetRow(header, 0);

    StackPanel brand;
    brand.Orientation(Orientation::Horizontal);
    brand.Spacing(8);
    brand.VerticalAlignment(VerticalAlignment::Center);
    Border brand_mark;
    brand_mark.Width(8);
    brand_mark.Height(8);
    brand_mark.CornerRadius(CornerRadius{2, 2, 2, 2});
    brand_mark.Background(accent_brush_);
    brand_mark.VerticalAlignment(VerticalAlignment::Center);
    brand.Children().Append(brand_mark);
    auto brand_text = Text(L"OpenReplay", 14, primary_text_brush);
    brand_text.FontWeight(semibold);
    brand.Children().Append(brand_text);
    header.Children().Append(brand);

    StackPanel nav;
    nav.Orientation(Orientation::Horizontal);
    nav.VerticalAlignment(VerticalAlignment::Center);
    const auto nav_tab = [&] {
        Button tab;
        tab.FontFamily(openreplay::ui::PublicSans());
        tab.FontSize(13);
        tab.Foreground(muted_brush);
        tab.Background(transparent_brush);
        tab.BorderThickness(Thickness{0, 0, 0, 0});
        tab.CornerRadius(CornerRadius{6, 6, 6, 6});
        tab.Padding(Thickness{9, 6, 9, 6});
        tab.MinHeight(30);
        nav.Children().Append(tab);
        return tab;
    };
    back_button_ = nav_tab();
    back_button_.Click({this, &MainWindow::BackButton_Click});
    gallery_button_ = nav_tab();
    gallery_button_.Click({this, &MainWindow::GalleryButton_Click});
    settings_button_ = nav_tab();
    settings_button_text_ = Text(L"Настройки", 13, muted_brush);
    settings_button_.Content(settings_button_text_);
    settings_button_.Click({this, &MainWindow::SettingsButton_Click});
    auto close_button = nav_tab();
    close_button.Content(make_icon(L"\xE711", 11, muted_brush));
    close_button.Margin(Thickness{4, 0, 0, 0});
    close_button.Click({this, &MainWindow::CloseButton_Click});
    Grid::SetColumn(nav, 1);
    header.Children().Append(nav);
    layout.Children().Append(header);

    dashboard_panel_ = Grid{};
    Grid::SetRow(dashboard_panel_, 2);
    ScrollViewer dashboard_scroll;
    dashboard_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    dashboard_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    StackPanel dashboard;
    dashboard_scroll.Content(dashboard);
    dashboard_panel_.Children().Append(dashboard_scroll);

    StackPanel buffer_heading;
    buffer_heading.Orientation(Orientation::Horizontal);
    buffer_heading.Spacing(10);
    buffer_heading.Margin(Thickness{24, 26, 24, 0});
    buffer_value_text_ = Text(L"", 44, primary_text_brush);
    buffer_value_text_.FontWeight(Windows::UI::Text::FontWeights::Light());
    buffer_value_text_.CharacterSpacing(-20);
    buffer_filled_run_ = Documents::Run{};
    buffer_filled_run_.FontWeight(semibold);
    buffer_filled_run_.Text(L"0");
    buffer_total_run_ = Documents::Run{};
    buffer_value_text_.Inlines().Append(buffer_filled_run_);
    buffer_value_text_.Inlines().Append(buffer_total_run_);
    buffer_heading.Children().Append(buffer_value_text_);
    replay_state_text_ = Text(L"Подключение...", 13, muted_brush);
    replay_state_text_.VerticalAlignment(VerticalAlignment::Bottom);
    replay_state_text_.Margin(Thickness{0, 0, 0, 8});
    buffer_heading.Children().Append(replay_state_text_);
    dashboard.Children().Append(buffer_heading);

    buffer_segments_ = Grid{};
    buffer_segments_.Height(22);
    buffer_segments_.ColumnSpacing(3);
    buffer_segments_.Margin(Thickness{24, 16, 24, 0});
    buffer_segment_cells_.clear();
    track_brush_ = theme.track;
    for (int index = 0; index < kBufferSegments; ++index) {
        AddColumn(buffer_segments_, 1, GridUnitType::Star);
        Border cell;
        cell.CornerRadius(CornerRadius{1, 1, 1, 1});
        cell.Background(theme.track);
        Grid::SetColumn(cell, index);
        buffer_segments_.Children().Append(cell);
        buffer_segment_cells_.push_back(cell);
    }
    dashboard.Children().Append(buffer_segments_);

    Grid buffer_scale;
    buffer_scale.Margin(Thickness{24, 6, 24, 0});
    AddColumn(buffer_scale, 1, GridUnitType::Star);
    AddColumn(buffer_scale, 1, GridUnitType::Star);
    AddColumn(buffer_scale, 1, GridUnitType::Star);
    buffer_start_label_ = Text(L"", 10.5, faint_brush);
    buffer_scale.Children().Append(buffer_start_label_);
    buffer_middle_label_ = Text(L"", 10.5, faint_brush);
    buffer_middle_label_.HorizontalAlignment(HorizontalAlignment::Center);
    Grid::SetColumn(buffer_middle_label_, 1);
    buffer_scale.Children().Append(buffer_middle_label_);
    buffer_now_label_ = Text(L"", 10.5, faint_brush);
    buffer_now_label_.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(buffer_now_label_, 2);
    buffer_scale.Children().Append(buffer_now_label_);
    dashboard.Children().Append(buffer_scale);

    StackPanel meta;
    meta.Orientation(Orientation::Horizontal);
    meta.Spacing(14);
    meta.Margin(Thickness{24, 12, 24, 0});
    encoder_text_ = Text(L"Capture pipeline is starting", 12, secondary_text_brush_);
    encoder_text_.TextTrimming(TextTrimming::CharacterEllipsis);
    encoder_text_.MaxWidth(200);
    meta.Children().Append(encoder_text_);
    dashboard_meta_text_ = Text(L"", 12, muted_brush);
    dashboard_meta_text_.TextTrimming(TextTrimming::CharacterEllipsis);
    meta.Children().Append(dashboard_meta_text_);
    dashboard.Children().Append(meta);

    save_replay_button_ = controls.ActionButton(L"", ButtonKind::Accent);
    save_replay_button_.Height(52);
    save_replay_button_.CornerRadius(CornerRadius{10, 10, 10, 10});
    save_replay_button_.Margin(Thickness{24, 24, 24, 0});
    save_replay_button_.Padding(Thickness{18, 0, 14, 0});
    save_replay_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    save_replay_button_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    Grid save_content;
    AddColumn(save_content, 1, GridUnitType::Star);
    AddColumn(save_content, 0, GridUnitType::Auto);
    save_replay_text_ = Text(L"Сохранить повтор", 15, theme.accent_text);
    save_replay_text_.FontWeight(semibold);
    save_replay_text_.VerticalAlignment(VerticalAlignment::Center);
    save_content.Children().Append(save_replay_text_);
    save_replay_keys_ = StackPanel{};
    save_replay_keys_.Orientation(Orientation::Horizontal);
    save_replay_keys_.Spacing(4);
    save_replay_keys_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(save_replay_keys_, 1);
    save_content.Children().Append(save_replay_keys_);
    save_replay_button_.Content(save_content);
    save_replay_button_.Click({this, &MainWindow::SaveReplay_Click});
    dashboard.Children().Append(save_replay_button_);

    StackPanel actions;
    actions.Margin(Thickness{0, 20, 0, 0});
    actions.BorderBrush(line_brush);
    actions.BorderThickness(Thickness{0, 1, 0, 0});
    const auto action_content = [&](std::wstring_view glyph, TextBlock const& title, TextBlock const& subtitle,
                                    FrameworkElement const& trailing) {
        Grid content;
        content.ColumnSpacing(14);
        AddColumn(content, 32, GridUnitType::Pixel);
        AddColumn(content, 1, GridUnitType::Star);
        AddColumn(content, 0, GridUnitType::Auto);
        auto icon = make_icon(glyph, 16, secondary_text_brush_);
        icon.VerticalAlignment(VerticalAlignment::Center);
        content.Children().Append(icon);
        StackPanel copy;
        copy.VerticalAlignment(VerticalAlignment::Center);
        title.FontWeight(Windows::UI::Text::FontWeights::Medium());
        copy.Children().Append(title);
        subtitle.Margin(Thickness{0, 3, 0, 0});
        subtitle.TextTrimming(TextTrimming::CharacterEllipsis);
        copy.Children().Append(subtitle);
        Grid::SetColumn(copy, 1);
        content.Children().Append(copy);
        trailing.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(trailing, 2);
        content.Children().Append(trailing);
        return content;
    };
    const auto keycaps_panel = [] {
        StackPanel panel;
        panel.Orientation(Orientation::Horizontal);
        panel.Spacing(4);
        return panel;
    };

    replay_title_ = Text(L"Мгновенный повтор", 15, primary_text_brush);
    replay_description_ = Text(L"", 12, muted_brush);
    replay_toggle_ = controls.Toggle();
    replay_toggle_.Toggled({this, &MainWindow::ReplayToggle_Toggled});
    actions.Children().Append(make_row(action_content(L"\xE81C", replay_title_, replay_description_, replay_toggle_),
                                       Thickness{24, 14, 24, 14}));

    recording_title_ = Text(L"Начать запись", 15, primary_text_brush);
    recording_state_text_ = Text(L"Готово к записи", 12, muted_brush);
    recording_keys_ = keycaps_panel();
    recording_button_ = Button{};
    plain_button(recording_button_);
    recording_button_.Padding(Thickness{24, 14, 24, 14});
    recording_button_.Content(action_content(L"\xE7C8", recording_title_, recording_state_text_, recording_keys_));
    recording_button_.Click({this, &MainWindow::RecordingButton_Click});
    actions.Children().Append(make_row(recording_button_, Thickness{}));

    screenshot_title_ = Text(L"Снимок экрана", 15, primary_text_brush);
    screenshot_state_text_ = Text(L"PNG", 12, muted_brush);
    screenshot_keys_ = keycaps_panel();
    screenshot_button_ = Button{};
    plain_button(screenshot_button_);
    screenshot_button_.Padding(Thickness{24, 14, 24, 14});
    screenshot_button_.Content(action_content(L"\xE722", screenshot_title_, screenshot_state_text_, screenshot_keys_));
    screenshot_button_.Click({this, &MainWindow::ScreenshotButton_Click});
    actions.Children().Append(make_row(screenshot_button_, Thickness{}));
    dashboard.Children().Append(actions);

    Grid recent_header;
    recent_header.Margin(Thickness{24, 28, 14, 8});
    AddColumn(recent_header, 1, GridUnitType::Star);
    AddColumn(recent_header, 0, GridUnitType::Auto);
    recent_title_ = Text(L"НЕДАВНИЕ", 11, faint_brush);
    recent_title_.CharacterSpacing(120);
    recent_title_.VerticalAlignment(VerticalAlignment::Center);
    recent_header.Children().Append(recent_title_);
    recent_all_button_ = Button{};
    recent_all_button_.Background(transparent_brush);
    recent_all_button_.BorderThickness(Thickness{0, 0, 0, 0});
    recent_all_button_.Padding(Thickness{10, 4, 10, 4});
    recent_all_button_.FontFamily(openreplay::ui::PublicSans());
    recent_all_button_.FontSize(12);
    recent_all_button_.Foreground(muted_brush);
    recent_all_button_.Click({this, &MainWindow::GalleryButton_Click});
    Grid::SetColumn(recent_all_button_, 1);
    recent_header.Children().Append(recent_all_button_);
    dashboard.Children().Append(recent_header);
    recent_clips_panel_ = StackPanel{};
    recent_clips_panel_.Margin(Thickness{0, 0, 0, 24});
    dashboard.Children().Append(recent_clips_panel_);
    layout.Children().Append(dashboard_panel_);

    settings_panel_ = Grid{};
    settings_panel_.Visibility(Visibility::Collapsed);
    Grid::SetRow(settings_panel_, 2);

    ScrollViewer settings_scroll;
    settings_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    settings_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    StackPanel form;
    form.Padding(Thickness{0, 0, 0, 24});

    Grid settings_header;
    settings_header.Margin(Thickness{24, 24, 24, 0});
    AddColumn(settings_header, 1, GridUnitType::Star);
    AddColumn(settings_header, 0, GridUnitType::Auto);
    settings_title_ = Text(L"Настройки", 30, primary_text_brush);
    settings_title_.FontWeight(semibold);
    settings_header.Children().Append(settings_title_);
    StackPanel save_status;
    save_status.Orientation(Orientation::Horizontal);
    save_status.Spacing(6);
    save_status.VerticalAlignment(VerticalAlignment::Center);
    Border save_status_dot;
    save_status_dot.Width(6);
    save_status_dot.Height(6);
    save_status_dot.CornerRadius(CornerRadius{3, 3, 3, 3});
    save_status_dot.Background(accent_brush_);
    save_status_dot.VerticalAlignment(VerticalAlignment::Center);
    save_status.Children().Append(save_status_dot);
    settings_save_status_ = Text(L"Changes save automatically", 12, muted_brush);
    settings_save_status_.TextTrimming(TextTrimming::CharacterEllipsis);
    settings_save_status_.MaxWidth(260);
    save_status.Children().Append(settings_save_status_);
    Grid::SetColumn(save_status, 1);
    settings_header.Children().Append(save_status);
    form.Children().Append(settings_header);

    const auto create_section = [&](TextBlock& heading, std::wstring_view title) {
        heading = Text(title, 11, faint_brush);
        heading.CharacterSpacing(120);
        heading.Margin(Thickness{28, 28, 28, 8});
        form.Children().Append(heading);
        StackPanel content;
        content.Background(theme.card);
        content.CornerRadius(CornerRadius{12, 12, 12, 12});
        content.Margin(Thickness{16, 0, 16, 0});
        content.Padding(Thickness{0, 4, 0, 4});
        form.Children().Append(content);
        return content;
    };

    // The group surface does the grouping, so rows inside it carry no dividers.
    const auto group_row = [](UIElement const& child, Thickness padding = Thickness{16, 12, 16, 12}) {
        Border row;
        row.Padding(padding);
        row.Child(child);
        return row;
    };

    // Label (and optional hint) on the left, control on the right; wide controls go under the label.
    const auto add_field = [&](StackPanel const& section, TextBlock& label, std::wstring_view text,
                               FrameworkElement const& control, TextBlock const& hint = nullptr,
                               bool wide = false) {
        Grid row;
        row.ColumnSpacing(14);
        AddColumn(row, 1, GridUnitType::Star);
        AddColumn(row, 0, GridUnitType::Auto);
        StackPanel copy;
        copy.VerticalAlignment(VerticalAlignment::Center);
        label = Text(text, 14, primary_text_brush);
        copy.Children().Append(label);
        if (hint) {
            hint.FontSize(12);
            hint.Foreground(muted_brush);
            hint.TextWrapping(TextWrapping::Wrap);
            hint.Margin(Thickness{0, 3, 0, 0});
            copy.Children().Append(hint);
        }
        row.Children().Append(copy);
        if (wide) {
            AddRow(row, 0, GridUnitType::Auto);
            AddRow(row, 0, GridUnitType::Auto);
            control.Margin(Thickness{0, 10, 0, 0});
            Grid::SetRow(control, 1);
            Grid::SetColumnSpan(control, 2);
        } else {
            control.MinWidth(150);
            control.MaxWidth(260);
            control.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(control, 1);
        }
        row.Children().Append(control);
        section.Children().Append(group_row(row));
    };

    const auto add_toggle_row = [&](StackPanel const& section, TextBlock& label, std::wstring_view text,
                                    ToggleSwitch const& toggle) {
        Grid row;
        AddColumn(row, 1, GridUnitType::Star);
        AddColumn(row, 0, GridUnitType::Auto);
        label = Text(text, 14, primary_text_brush);
        label.TextWrapping(TextWrapping::Wrap);
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);
        toggle.HorizontalAlignment(HorizontalAlignment::Right);
        toggle.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(toggle, 1);
        row.Children().Append(toggle);
        section.Children().Append(group_row(row, Thickness{16, 12, 16, 12}));
    };

    const auto add_slider = [&](StackPanel const& section, TextBlock& label, std::wstring_view text,
                                TextBlock& value, std::wstring_view initial, FrameworkElement const& slider) {
        StackPanel block;
        Grid top;
        AddColumn(top, 1, GridUnitType::Star);
        AddColumn(top, 0, GridUnitType::Auto);
        label = Text(text, 14, primary_text_brush);
        top.Children().Append(label);
        value = Text(initial, 13.5, success_brush_);
        value.FontWeight(Windows::UI::Text::FontWeights::Medium());
        Grid::SetColumn(value, 1);
        top.Children().Append(value);
        block.Children().Append(top);
        slider.Margin(Thickness{0, 2, 0, 0});
        block.Children().Append(slider);
        section.Children().Append(group_row(block, Thickness{16, 12, 16, 6}));
    };

    const auto style_expander = [&](Expander const& expander, Grid const& header_content) {
        const auto resources = expander.Resources();
        const auto set = [&](std::wstring_view key, IInspectable const& value) {
            resources.Insert(winrt::box_value(winrt::hstring{key}), value);
        };
        set(L"ExpanderHeaderBackground", transparent_brush);
        set(L"ExpanderHeaderBorderBrush", transparent_brush);
        set(L"ExpanderHeaderBorderPointerOverBrush", transparent_brush);
        set(L"ExpanderHeaderBorderPressedBrush", transparent_brush);
        set(L"ExpanderHeaderBackgroundPointerOver", transparent_brush);
        set(L"ExpanderHeaderBackgroundPressed", transparent_brush);
        set(L"ExpanderContentBackground", transparent_brush);
        set(L"ExpanderContentBorderBrush", transparent_brush);
        set(L"ExpanderHeaderPadding", winrt::box_value(Thickness{16, 0, 10, 0}));
        set(L"ExpanderContentPadding", winrt::box_value(Thickness{16, 4, 16, 14}));
        set(L"ExpanderChevronForeground", faint_brush);
        expander.Header(header_content);
        expander.IsExpanded(false);
        expander.MinHeight(58);
        expander.CornerRadius(CornerRadius{0, 0, 0, 0});
        expander.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    };
    const auto expander_header = [&](TextBlock& label, std::wstring_view text, TextBlock& summary,
                                     std::wstring_view summary_text) {
        Grid content;
        AddColumn(content, 1, GridUnitType::Star);
        AddColumn(content, 0, GridUnitType::Auto);
        label = Text(text, 14, primary_text_brush);
        label.VerticalAlignment(VerticalAlignment::Center);
        content.Children().Append(label);
        summary = Text(summary_text, 12.5, muted_brush);
        summary.VerticalAlignment(VerticalAlignment::Center);
        summary.Margin(Thickness{10, 0, 4, 0});
        Grid::SetColumn(summary, 1);
        content.Children().Append(summary);
        return content;
    };
    const auto add_expander = [&](StackPanel const& section, Expander const& expander) {
        section.Children().Append(group_row(expander, Thickness{}));
    };

    const auto source_section = create_section(source_section_title_, L"ИСТОЧНИК");

    monitor_selector_ = controls.Select();
    monitor_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(source_section, monitor_label_, L"Монитор", monitor_selector_);

    const auto add_audio_devices = [&](Expander& expander, TextBlock& label, std::wstring_view text,
                                        TextBlock& summary, StackPanel& devices) {
        devices = StackPanel{};
        devices.Spacing(8);
        devices.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander = Expander{};
        style_expander(expander, expander_header(label, text, summary, L"1 выбрано"));
        expander.Content(devices);
        add_expander(source_section, expander);
    };
    add_audio_devices(desktop_audio_devices_expander_, desktop_audio_label_, L"Системный звук",
                      desktop_audio_summary_, desktop_audio_devices_panel_);
    add_audio_devices(microphone_devices_expander_, microphone_devices_label_, L"Микрофоны и входы",
                      microphone_audio_summary_, microphone_devices_panel_);
    audio_devices_hint_ = Text(L"Можно выбрать несколько устройств, суммарно до 5 активных источников.", 12,
                               muted_brush);
    audio_devices_hint_.TextWrapping(TextWrapping::Wrap);
    audio_devices_hint_.Margin(Thickness{28, 8, 28, 0});
    form.Children().Append(audio_devices_hint_);

    const auto video_section = create_section(video_section_title_, L"ВИДЕО");

    auto replay_duration_control = controls.RangeSlider(15, 1200, 15, 60);
    replay_duration_slider_ = replay_duration_control.input;
    replay_duration_slider_.ValueChanged({this, &MainWindow::ReplayDurationSlider_ValueChanged});
    add_slider(video_section, replay_duration_label_, L"Длительность повтора", replay_duration_value_, L"60 сек",
               replay_duration_control.view);

    quality_selector_ = controls.Select();
    quality_selector_.Items().Append(controls.Option(L"Производительность"));
    quality_selector_.Items().Append(controls.Option(L"Сбалансированный"));
    quality_selector_.Items().Append(controls.Option(L"Высокое качество"));
    quality_selector_.SelectionChanged({this, &MainWindow::QualitySelector_SelectionChanged});
    quality_description_ = Text(L"Баланс качества сжатия и нагрузки на GPU. Битрейт задаётся отдельно.", 12,
                                muted_brush);
    add_field(video_section, quality_label_, L"Пресет качества", quality_selector_, quality_description_);

    auto bitrate_control = controls.RangeSlider(4, 120, 1, 24);
    bitrate_slider_ = bitrate_control.input;
    bitrate_slider_.ValueChanged({this, &MainWindow::BitrateSlider_ValueChanged});
    add_slider(video_section, bitrate_label_, L"Целевой битрейт", bitrate_value_, L"24 Мбит/с",
               bitrate_control.view);

    Border replay_estimate;
    replay_estimate.Background(theme.inset);
    replay_estimate.BorderBrush(theme.border);
    replay_estimate.BorderThickness(Thickness{1, 1, 1, 1});
    replay_estimate.CornerRadius(CornerRadius{10, 10, 10, 10});
    replay_estimate.Padding(Thickness{16, 12, 16, 12});
    replay_estimate.Margin(Thickness{16, 6, 16, 10});
    Grid estimate_content;
    estimate_content.ColumnSpacing(14);
    AddColumn(estimate_content, 0, GridUnitType::Auto);
    AddColumn(estimate_content, 1, GridUnitType::Star);
    replay_size_value_ = Text(L"≈ 181 МБ", 22, primary_text_brush);
    replay_size_value_.FontWeight(semibold);
    replay_size_value_.VerticalAlignment(VerticalAlignment::Center);
    estimate_content.Children().Append(replay_size_value_);
    StackPanel estimate_copy;
    estimate_copy.VerticalAlignment(VerticalAlignment::Center);
    replay_size_label_ = Text(L"на один повтор", 12, muted_brush);
    estimate_copy.Children().Append(replay_size_label_);
    replay_size_detail_ = Text(L"60 сек · VBR 24 Мбит/с · 3 аудиодорожки · MKV", 12, secondary_text_brush_);
    replay_size_detail_.TextWrapping(TextWrapping::Wrap);
    estimate_copy.Children().Append(replay_size_detail_);
    Grid::SetColumn(estimate_copy, 1);
    estimate_content.Children().Append(estimate_copy);
    replay_estimate.Child(estimate_content);
    video_section.Children().Append(group_row(replay_estimate, Thickness{}));

    codec_selector_ = controls.Select();
    codec_selector_.Items().Append(controls.Option(L"H.264"));
    codec_selector_.Items().Append(controls.Option(L"HEVC"));
    codec_selector_.Items().Append(controls.Option(L"AV1"));
    codec_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(video_section, codec_label_, L"Кодек", codec_selector_);

    fps_selector_ = controls.Select();
    fps_selector_.Items().Append(controls.Option(L"30 FPS"));
    fps_selector_.Items().Append(controls.Option(L"60 FPS"));
    fps_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(video_section, fps_label_, L"Частота кадров", fps_selector_);

    const auto input_section = create_section(input_section_title_, L"ИНТЕРФЕЙС И ВВОД");

    language_selector_ = controls.Select();
    language_selector_.Items().Append(controls.Option(L"Русский"));
    language_selector_.Items().Append(controls.Option(L"English"));
    language_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(input_section, language_label_, L"Язык", language_selector_);

    microphone_toggle_ = controls.Toggle();
    microphone_toggle_.Toggled({this, &MainWindow::EstimateInput_Toggled});
    add_toggle_row(input_section, microphone_label_, L"Микрофон", microphone_toggle_);
    cursor_toggle_ = controls.Toggle();
    add_toggle_row(input_section, cursor_label_, L"Захватывать курсор", cursor_toggle_);
    cursor_toggle_.Toggled([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    start_with_windows_toggle_ = controls.Toggle();
    add_toggle_row(input_section, start_with_windows_label_, L"Запускать вместе с Windows",
                   start_with_windows_toggle_);
    start_with_windows_toggle_.Toggled([this](auto&&, auto&&) { ScheduleSettingsApply(); });

    StackPanel shortcuts_content;
    shortcuts_content.Spacing(8);
    shortcuts_content.HorizontalAlignment(HorizontalAlignment::Stretch);
    replay_hotkeys_panel_ = StackPanel{};
    replay_hotkeys_panel_.Spacing(8);
    shortcuts_content.Children().Append(replay_hotkeys_panel_);
    replay_hotkey_add_button_ = controls.ActionButton(L"+ Добавить хоткей");
    replay_hotkey_add_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    replay_hotkey_add_button_.Click({this, &MainWindow::ReplayHotkeyAdd_Click});
    shortcuts_content.Children().Append(replay_hotkey_add_button_);

    const auto add_shortcut_block = [&](TextBlock& label, std::wstring_view text, ToggleSwitch& toggle,
                                        TextBox& input, TextBlock& validation) {
        auto surface = controls.ShortcutSurface();
        StackPanel block;
        block.Spacing(10);
        Grid block_header;
        block_header.Height(20);
        AddColumn(block_header, 1, GridUnitType::Star);
        AddColumn(block_header, 0, GridUnitType::Auto);
        label = Text(text, 13, primary_text_brush);
        label.VerticalAlignment(VerticalAlignment::Center);
        block_header.Children().Append(label);
        toggle = controls.Toggle();
        toggle.VerticalAlignment(VerticalAlignment::Center);
        toggle.Toggled([this](auto&&, auto&&) {
            if (updating_ui_) return;
            ValidateHotkeys(true, false);
            ScheduleSettingsApply();
        });
        Grid::SetColumn(toggle, 1);
        block_header.Children().Append(toggle);
        block.Children().Append(block_header);
        input = controls.HotkeyInput(L"Нажмите сочетание");
        input.LostFocus([this](auto&&, auto&&) {
            ValidateHotkeys(true, false);
            ScheduleSettingsApply();
        });
        block.Children().Append(input);
        validation = Text(L"", 10.5, danger_brush_);
        validation.TextWrapping(TextWrapping::Wrap);
        validation.Visibility(Visibility::Collapsed);
        block.Children().Append(validation);
        surface.Child(block);
        shortcuts_content.Children().Append(surface);
    };
    add_shortcut_block(recording_hotkey_label_, L"Запись сессии", recording_hotkey_toggle_,
                       recording_hotkey_input_, recording_hotkey_validation_);
    recording_hotkey_input_.KeyDown({this, &MainWindow::RecordingHotkey_KeyDown});
    add_shortcut_block(screenshot_hotkey_label_, L"Снимок экрана", screenshot_hotkey_toggle_,
                       screenshot_hotkey_input_, screenshot_hotkey_validation_);
    screenshot_hotkey_input_.KeyDown({this, &MainWindow::ScreenshotHotkey_KeyDown});

    shortcuts_expander_ = Expander{};
    style_expander(shortcuts_expander_, expander_header(shortcuts_label_, L"Хоткеи", shortcuts_summary_, L"3 активны"));
    shortcuts_expander_.Content(shortcuts_content);
    add_expander(input_section, shortcuts_expander_);

    StackPanel performance_content;
    performance_content.Spacing(10);
    performance_content.HorizontalAlignment(HorizontalAlignment::Stretch);
    const auto performance_option = [&](TextBlock& label, std::wstring_view text, ComboBox& selector) {
        Grid row;
        AddColumn(row, 1, GridUnitType::Star);
        AddColumn(row, 170, GridUnitType::Pixel);
        label = Text(text, 13, primary_text_brush);
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);
        selector = controls.Select();
        selector.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
        Grid::SetColumn(selector, 1);
        row.Children().Append(selector);
        performance_content.Children().Append(row);
    };
    performance_option(performance_position_label_, L"Положение", performance_position_selector_);
    performance_position_selector_.Items().Append(controls.Option(L"Справа сверху"));
    performance_position_selector_.Items().Append(controls.Option(L"Справа снизу"));
    performance_position_selector_.SelectedIndex(0);
    performance_option(performance_graph_style_label_, L"График времени кадра", performance_graph_style_selector_);
    performance_graph_style_selector_.Items().Append(controls.Option(L"Столбики"));
    performance_graph_style_selector_.Items().Append(controls.Option(L"Линия"));
    performance_graph_style_selector_.SelectedIndex(0);

    Grid opacity_header;
    AddColumn(opacity_header, 1, GridUnitType::Star);
    AddColumn(opacity_header, 0, GridUnitType::Auto);
    performance_opacity_label_ = Text(L"Прозрачность фона", 13, primary_text_brush);
    opacity_header.Children().Append(performance_opacity_label_);
    performance_opacity_value_ = Text(L"92%", 12.5, success_brush_);
    Grid::SetColumn(performance_opacity_value_, 1);
    opacity_header.Children().Append(performance_opacity_value_);
    performance_content.Children().Append(opacity_header);
    auto performance_opacity_control = controls.RangeSlider(55, 100, 1, 92);
    performance_opacity_slider_ = performance_opacity_control.input;
    performance_opacity_control.view.Margin(Thickness{0, -6, 0, 2});
    performance_opacity_slider_.ValueChanged([this](auto&&, auto const& args) {
        if (performance_opacity_value_) {
            performance_opacity_value_.Text(std::to_wstring(static_cast<unsigned int>(std::lround(args.NewValue()))) + L"%");
        }
        ScheduleSettingsApply();
    });
    performance_content.Children().Append(performance_opacity_control.view);

    const auto metric_check = [&](const StackPanel& parent, std::wstring_view label, CheckBox& target) {
        target = controls.Check(label, true, 12.5);
        target.Click([this](auto&&, auto&&) {
            UpdatePerformanceOverlaySummary();
            ScheduleSettingsApply();
        });
        parent.Children().Append(target);
    };
    Grid performance_metrics;
    AddColumn(performance_metrics, 1, GridUnitType::Star);
    AddColumn(performance_metrics, 1, GridUnitType::Star);
    StackPanel gpu_metrics;
    StackPanel system_metrics;
    metric_check(gpu_metrics, L"FPS", performance_fps_);
    metric_check(gpu_metrics, L"1% / 0.1% FPS", performance_fps_lows_);
    metric_check(gpu_metrics, L"Время кадра", performance_frametime_);
    metric_check(gpu_metrics, L"График времени кадра", performance_frametime_graph_);
    metric_check(gpu_metrics, L"Загрузка GPU", performance_gpu_usage_);
    metric_check(gpu_metrics, L"Температура GPU", performance_gpu_temperature_);
    metric_check(gpu_metrics, L"Частота GPU", performance_gpu_clock_);
    metric_check(system_metrics, L"Потребление GPU", performance_gpu_power_);
    metric_check(gpu_metrics, L"VRAM", performance_gpu_memory_);
    metric_check(system_metrics, L"Загрузка CPU", performance_cpu_usage_);
    metric_check(system_metrics, L"Оперативная память", performance_memory_);
    Grid::SetColumn(system_metrics, 1);
    performance_metrics.Children().Append(gpu_metrics);
    performance_metrics.Children().Append(system_metrics);
    performance_content.Children().Append(performance_metrics);

    performance_overlay_expander_ = Expander{};
    style_expander(performance_overlay_expander_,
                   expander_header(performance_overlay_label_, L"Оверлей производительности",
                                   performance_overlay_summary_, L"Alt+R · 11 метрик"));
    performance_overlay_expander_.Content(performance_content);
    add_expander(input_section, performance_overlay_expander_);

    const auto storage_section = create_section(storage_section_title_, L"ХРАНИЛИЩЕ");

    output_format_selector_ = controls.Select();
    output_format_selector_.Items().Append(controls.Option(L"MKV"));
    output_format_selector_.Items().Append(controls.Option(L"MP4"));
    output_format_selector_.SelectionChanged({this, &MainWindow::OutputFormatSelector_SelectionChanged});
    output_format_description_ = Text(L"Широко совместим с приложениями и видеоредакторами.", 12, muted_brush);
    add_field(storage_section, output_format_label_, L"Формат видео", output_format_selector_,
              output_format_description_);

    Grid output_row;
    output_row.ColumnSpacing(8);
    AddColumn(output_row, 1, GridUnitType::Star);
    AddColumn(output_row, 0, GridUnitType::Auto);
    AddColumn(output_row, 0, GridUnitType::Auto);
    StackPanel output_copy;
    output_copy.VerticalAlignment(VerticalAlignment::Center);
    output_label_ = Text(L"Папка сохранения", 14, primary_text_brush);
    output_copy.Children().Append(output_label_);
    output_directory_text_ = Text(L"", 12, muted_brush);
    output_directory_text_.Margin(Thickness{0, 3, 0, 0});
    output_directory_text_.TextTrimming(TextTrimming::CharacterEllipsis);
    output_directory_text_.TextWrapping(TextWrapping::NoWrap);
    output_copy.Children().Append(output_directory_text_);
    storage_space_text_ = Text(L"Свободно: проверка...", 12, muted_brush);
    storage_space_text_.Margin(Thickness{0, 2, 0, 0});
    output_copy.Children().Append(storage_space_text_);
    output_row.Children().Append(output_copy);
    open_folder_button_ = controls.ActionButton(L"Открыть папку");
    open_folder_button_.VerticalAlignment(VerticalAlignment::Center);
    open_folder_button_.Click({this, &MainWindow::OpenFolder_Click});
    Grid::SetColumn(open_folder_button_, 1);
    output_row.Children().Append(open_folder_button_);
    open_logs_button_ = controls.ActionButton(L"Открыть логи");
    open_logs_button_.VerticalAlignment(VerticalAlignment::Center);
    open_logs_button_.Click({this, &MainWindow::OpenLogs_Click});
    Grid::SetColumn(open_logs_button_, 2);
    output_row.Children().Append(open_logs_button_);
    storage_section.Children().Append(group_row(output_row));

    const auto discord_section = create_section(discord_section_title_, L"DISCORD SHARE");
    discord_webhook_input_ = controls.SecretInput(L"https://discord.com/api/webhooks/...");
    discord_webhook_input_.PasswordChanged([this](auto&&, auto&&) { UpdateDiscordUi(); });
    discord_status_text_ = Text(L"Webhook is not configured", 12, muted_brush);
    add_field(discord_section, discord_webhook_label_, L"Webhook URL", discord_webhook_input_,
              discord_status_text_, true);

    StackPanel discord_actions;
    discord_actions.Orientation(Orientation::Horizontal);
    discord_actions.Spacing(8);
    discord_actions.HorizontalAlignment(HorizontalAlignment::Right);
    discord_clear_button_ = controls.ActionButton(L"Remove webhook");
    discord_clear_button_.Click({this, &MainWindow::ClearDiscordWebhook_Click});
    discord_actions.Children().Append(discord_clear_button_);
    discord_test_button_ = controls.ActionButton(L"Test");
    discord_test_button_.Click({this, &MainWindow::TestDiscordWebhook_Click});
    discord_actions.Children().Append(discord_test_button_);
    discord_save_button_ = controls.ActionButton(L"Save webhook", ButtonKind::Accent);
    discord_save_button_.Click({this, &MainWindow::SaveDiscordWebhook_Click});
    discord_actions.Children().Append(discord_save_button_);
    discord_section.Children().Append(group_row(discord_actions, Thickness{16, 6, 16, 12}));

    discord_size_input_ = controls.NumberInput(5, 500, 1);
    discord_size_input_.ValueChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(discord_section, discord_size_label_, L"Maximum upload size, MiB", discord_size_input_);

    discord_auto_send_toggle_ = controls.Toggle();
    discord_auto_send_toggle_.Toggled([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_toggle_row(discord_section, discord_auto_send_label_, L"Send automatically after saving",
                   discord_auto_send_toggle_);

    const auto updates_section = create_section(updates_section_title_, L"UPDATES");

    automatic_updates_toggle_ = controls.Toggle();
    automatic_updates_toggle_.Toggled([this](auto&&, auto&&) {
        if (updating_ui_) return;
        ScheduleSettingsApply();
        if (automatic_updates_toggle_.IsOn()) CheckForUpdatesAsync(false, true);
    });
    add_toggle_row(updates_section, automatic_updates_label_, L"Check and download automatically",
                   automatic_updates_toggle_);

    developer_updates_toggle_ = controls.Toggle();
    developer_updates_toggle_.Toggled([this](auto&&, auto&&) {
        if (updating_ui_) return;
        available_update_ = {};
        downloaded_update_.clear();
        ScheduleSettingsApply();
        UpdateUpdateUi();
        if (automatic_updates_toggle_.IsOn()) CheckForUpdatesAsync(false, true);
    });
    add_toggle_row(updates_section, developer_updates_label_, L"Receive developer builds",
                   developer_updates_toggle_);

    Grid update_row;
    update_row.ColumnSpacing(8);
    AddColumn(update_row, 1, GridUnitType::Star);
    AddColumn(update_row, 0, GridUnitType::Auto);
    StackPanel update_copy;
    update_copy.VerticalAlignment(VerticalAlignment::Center);
    update_version_text_ = Text(L"OpenReplay " + openreplay::FromUtf8(openreplay::kVersion) +
                                    (openreplay::kReleaseChannel == "dev" ? L" · Dev" : L" · Stable"),
                                14, primary_text_brush);
    update_copy.Children().Append(update_version_text_);
    update_status_text_ = Text(L"Updates are checked in the background.", 12, muted_brush);
    update_status_text_.TextWrapping(TextWrapping::Wrap);
    update_status_text_.Margin(Thickness{0, 3, 0, 0});
    update_copy.Children().Append(update_status_text_);
    update_row.Children().Append(update_copy);
    StackPanel update_actions;
    update_actions.Spacing(6);
    update_actions.VerticalAlignment(VerticalAlignment::Center);
    check_update_button_ = controls.ActionButton(L"Check now");
    check_update_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    check_update_button_.Click({this, &MainWindow::CheckUpdate_Click});
    update_actions.Children().Append(check_update_button_);
    update_action_button_ = controls.ActionButton(L"Download update", ButtonKind::Accent);
    update_action_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    update_action_button_.Visibility(Visibility::Collapsed);
    update_action_button_.Click({this, &MainWindow::UpdateAction_Click});
    update_actions.Children().Append(update_action_button_);
    release_notes_button_ = controls.ActionButton(L"Release notes");
    release_notes_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    release_notes_button_.Visibility(Visibility::Collapsed);
    release_notes_button_.Click({this, &MainWindow::OpenReleaseNotes_Click});
    update_actions.Children().Append(release_notes_button_);
    Grid::SetColumn(update_actions, 1);
    update_row.Children().Append(update_actions);
    updates_section.Children().Append(group_row(update_row, Thickness{16, 12, 16, 12}));

    settings_scroll.Content(form);
    settings_panel_.Children().Append(settings_scroll);
    layout.Children().Append(settings_panel_);

    Grid footer;
    footer.Padding(Thickness{24, 0, 24, 0});
    footer.BorderBrush(line_brush);
    footer.BorderThickness(Thickness{0, 1, 0, 0});
    Grid::SetRow(footer, 3);
    hint_text_ = Text(L"Alt+Z — скрыть панель", 11.5, faint_brush);
    hint_text_.VerticalAlignment(VerticalAlignment::Center);
    footer.Children().Append(hint_text_);
    layout.Children().Append(footer);

    root_.Children().Append(shell);
    Content(root_);
    RebuildReplayHotkeyRows();
    openreplay::ui::TraceStartup(L"BuildUi: complete");
}

HWND MainWindow::GetWindowHandle() {
    if (!window_handle_) {
        Microsoft::UI::Xaml::Window window = *this;
        winrt::check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&window_handle_));
    }
    return window_handle_;
}

void MainWindow::InitializeWindow() {
    const auto window = GetWindowHandle();
    Title(L"OpenReplay");
    ExtendsContentIntoTitleBar(true);
    SetWindowDisplayAffinity(window, WDA_NONE);
    SetWindowSubclass(window, WindowSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
    hotkey_registered_ = RegisterHotKey(window, kOverlayHotkey, MOD_ALT | MOD_NOREPEAT, 'Z') != FALSE;
    performance_hotkey_registered_ =
        RegisterHotKey(window, kPerformanceOverlayHotkey, MOD_ALT | MOD_NOREPEAT, 'R') != FALSE;
    performance_overlay_.Initialize(window);
    AddTrayIcon();
    InitializeNotificationWindow();
    ShowWindow(window, SW_HIDE);
    Microsoft::UI::Xaml::Window xaml_window = *this;
    xaml_window.AppWindow().Hide();

    if (!hotkey_registered_) {
        ShowNotification(L"Alt+Z уже используется другим приложением. Закройте NVIDIA Overlay или измените его сочетание.", true);
    }
    if (!performance_hotkey_registered_) {
        ShowNotification(L"Alt+R уже используется другим приложением. Отключите конфликтующий performance overlay.", true);
    }
}

void MainWindow::HideOnLaunch() {
    visible_ = false;
    const auto window_handle = GetWindowHandle();
    ShowWindow(window_handle, SW_HIDE);
    Microsoft::UI::Xaml::Window window = *this;
    window.AppWindow().Hide();
}

void MainWindow::UnregisterConfigurableHotkeys() noexcept {
    if (!window_handle_) return;
    for (std::size_t index = 0; index < replay_hotkey_registered_.size(); ++index) {
        if (replay_hotkey_registered_[index]) {
            UnregisterHotKey(window_handle_, kReplayHotkeyBase + static_cast<int>(index));
            replay_hotkey_registered_[index] = false;
        }
    }
    if (screenshot_hotkey_registered_) {
        UnregisterHotKey(window_handle_, kScreenshotHotkey);
        screenshot_hotkey_registered_ = false;
    }
    if (recording_hotkey_registered_) {
        UnregisterHotKey(window_handle_, kRecordingHotkey);
        recording_hotkey_registered_ = false;
    }
}

void MainWindow::RegisterConfigurableHotkeys() {
    UnregisterConfigurableHotkeys();
    replay_hotkey_registered_.assign(settings_.replay_hotkeys.size(), false);
    for (std::size_t index = 0; index < settings_.replay_hotkeys.size(); ++index) {
        const auto& binding = settings_.replay_hotkeys[index];
        if (!binding.enabled) continue;
        const auto hotkey = ParseHotkey(binding.chord);
        if (!hotkey.key) continue;
        replay_hotkey_registered_[index] = RegisterHotKey(
            GetWindowHandle(), kReplayHotkeyBase + static_cast<int>(index),
            hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
        if (!replay_hotkey_registered_[index]) {
            const auto chord = openreplay::FromUtf8(binding.chord);
            ShowNotification((english_ ? std::wstring{L"Shortcut is already in use: "}
                                       : std::wstring{L"Сочетание уже используется: "}) + chord, true);
        }
    }
    if (settings_.screenshot_hotkey_enabled) {
        const auto hotkey = ParseHotkey(settings_.screenshot_hotkey_chord);
        if (hotkey.key) {
            screenshot_hotkey_registered_ = RegisterHotKey(
                GetWindowHandle(), kScreenshotHotkey, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
            if (!screenshot_hotkey_registered_) {
                ShowNotification((english_ ? std::wstring{L"Screenshot shortcut is already in use: "}
                                           : std::wstring{L"Хоткей снимка уже используется: "}) +
                                     openreplay::FromUtf8(settings_.screenshot_hotkey_chord),
                                 true);
            }
        }
    }
    if (settings_.recording_hotkey_enabled) {
        const auto hotkey = ParseHotkey(settings_.recording_hotkey_chord);
        if (hotkey.key) {
            recording_hotkey_registered_ = RegisterHotKey(
                GetWindowHandle(), kRecordingHotkey, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
            if (!recording_hotkey_registered_) {
                ShowNotification((english_ ? std::wstring{L"Recording shortcut is already in use: "}
                                           : std::wstring{L"Хоткей записи уже используется: "}) +
                                     openreplay::FromUtf8(settings_.recording_hotkey_chord),
                                 true);
            }
        }
    }
    if (shortcuts_summary_) {
        const auto active = std::ranges::count_if(settings_.replay_hotkeys, [](const auto& hotkey) {
            return hotkey.enabled;
        }) + (settings_.screenshot_hotkey_enabled ? 1 : 0) +
             (settings_.recording_hotkey_enabled ? 1 : 0);
        shortcuts_summary_.Text(std::to_wstring(active) +
            (english_ ? L" active" : (active == 1 ? L" активно" : L" активны")));
    }
}

std::vector<openreplay::ReplayHotkey> MainWindow::ReplayHotkeysFromControls() const {
    std::vector<openreplay::ReplayHotkey> result;
    result.reserve(replay_hotkey_inputs_.size());
    for (std::size_t index = 0; index < replay_hotkey_inputs_.size(); ++index) {
        const auto seconds = replay_hotkey_durations_[index].Value();
        result.push_back({replay_hotkey_toggles_[index].IsOn(),
                          openreplay::ToUtf8(replay_hotkey_inputs_[index].Text().c_str()),
                          std::isnan(seconds) ? 15U : static_cast<std::uint32_t>(std::lround(seconds))});
    }
    return result;
}

void MainWindow::RebuildReplayHotkeyRows() {
    if (!replay_hotkeys_panel_) return;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;

    replay_hotkeys_panel_.Children().Clear();
    replay_hotkey_labels_.clear();
    replay_hotkey_units_.clear();
    replay_hotkey_validation_.clear();
    replay_hotkey_inputs_.clear();
    replay_hotkey_durations_.clear();
    replay_hotkey_toggles_.clear();
    replay_hotkey_remove_buttons_.clear();

    const auto theme = DarkTheme();
    const ControlFactory controls{theme};
    const auto primary = theme.primary_text;
    if (replay_hotkey_draft_.empty()) replay_hotkey_draft_.push_back({false, {}, 15});

    for (std::size_t index = 0; index < replay_hotkey_draft_.size(); ++index) {
        const auto& binding = replay_hotkey_draft_[index];
        auto surface = controls.ShortcutSurface();

        StackPanel block;
        block.Spacing(10);
        Grid header;
        header.Height(20);
        AddColumn(header, 1, GridUnitType::Star);
        AddColumn(header, 0, GridUnitType::Auto);
        AddColumn(header, 8, GridUnitType::Pixel);
        AddColumn(header, 20, GridUnitType::Pixel);
        auto label = Text((english_ ? L"Replay " : L"Повтор ") + std::to_wstring(index + 1), 13, primary);
        label.VerticalAlignment(VerticalAlignment::Center);
        header.Children().Append(label);
        replay_hotkey_labels_.push_back(label);

        auto toggle = controls.Toggle();
        toggle.VerticalAlignment(VerticalAlignment::Center);
        toggle.IsOn(binding.enabled);
        toggle.Toggled([this](auto&&, auto&&) {
            ValidateHotkeys(true, false);
            ScheduleSettingsApply();
        });
        Grid::SetColumn(toggle, 1);
        header.Children().Append(toggle);
        replay_hotkey_toggles_.push_back(toggle);

        auto remove = controls.ActionButton(L"×");
        remove.Width(20);
        remove.Height(20);
        remove.MinWidth(20);
        remove.MinHeight(20);
        remove.Padding(Thickness{});
        remove.CornerRadius(CornerRadius{10, 10, 10, 10});
        FontIcon remove_icon;
        remove_icon.Glyph(L"\xE711");
        remove_icon.FontSize(10);
        remove.Content(remove_icon);
        remove.VerticalAlignment(VerticalAlignment::Center);
        remove.VerticalContentAlignment(VerticalAlignment::Center);
        remove.Click({this, &MainWindow::ReplayHotkeyRemove_Click});
        remove.IsEnabled(replay_hotkey_draft_.size() > 1);
        Grid::SetColumn(remove, 3);
        header.Children().Append(remove);
        replay_hotkey_remove_buttons_.push_back(remove);
        block.Children().Append(header);

        Grid row;
        AddColumn(row, 1, GridUnitType::Star);
        AddColumn(row, 8, GridUnitType::Pixel);
        AddColumn(row, 82, GridUnitType::Pixel);
        AddColumn(row, 26, GridUnitType::Pixel);
        auto input = controls.HotkeyInput(english_ ? L"Press shortcut" : L"Нажмите сочетание");
        input.Text(openreplay::FromUtf8(binding.chord));
        input.KeyDown({this, &MainWindow::ReplayHotkey_KeyDown});
        row.Children().Append(input);
        replay_hotkey_inputs_.push_back(input);

        auto duration = controls.NumberInput(15, 1200, 15);
        duration.Value(binding.replay_seconds);
        duration.ValueChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
        Grid::SetColumn(duration, 2);
        row.Children().Append(duration);
        replay_hotkey_durations_.push_back(duration);

        auto unit = Text(english_ ? L"sec" : L"сек", 11, secondary_text_brush_);
        unit.VerticalAlignment(VerticalAlignment::Center);
        unit.HorizontalAlignment(HorizontalAlignment::Right);
        Grid::SetColumn(unit, 3);
        row.Children().Append(unit);
        replay_hotkey_units_.push_back(unit);
        block.Children().Append(row);

        auto validation = Text(L"", 10.5, danger_brush_);
        validation.TextWrapping(TextWrapping::Wrap);
        validation.Visibility(Visibility::Collapsed);
        block.Children().Append(validation);
        replay_hotkey_validation_.push_back(validation);
        input.LostFocus([this](auto&&, auto&&) {
            ValidateHotkeys(true, false);
            ScheduleSettingsApply();
        });
        surface.Child(block);
        replay_hotkeys_panel_.Children().Append(surface);
    }
    if (replay_hotkey_add_button_) replay_hotkey_add_button_.IsEnabled(replay_hotkey_draft_.size() < 8);
    ValidateHotkeys(settings_initialized_, false);
}

bool MainWindow::ValidateHotkeys(bool check_system_conflicts, bool show_notification) {
    const auto normal_border = DarkTheme().control_border;
    const auto bindings = ReplayHotkeysFromControls();
    std::unordered_set<std::string> chords;
    bool valid = true;
    const auto fixed_conflict = [](const NativeHotkey& hotkey) {
        return hotkey.modifiers == MOD_ALT && (hotkey.key == 'Z' || hotkey.key == 'R');
    };
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        std::wstring error;
        const auto& binding = bindings[index];
        const auto native = ParseHotkey(binding.chord);
        if (binding.enabled && native.key == 0) {
            error = english_ ? L"Choose a valid shortcut" : L"Задайте корректное сочетание";
        } else if (binding.enabled && fixed_conflict(native)) {
            error = english_ ? L"Conflicts with an OpenReplay overlay shortcut"
                             : L"Конфликтует с хоткеем оверлея OpenReplay";
        } else if (binding.enabled) {
            std::string normalized = binding.chord;
            for (auto& character : normalized) {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
            if (!chords.emplace(std::move(normalized)).second) {
                error = english_ ? L"This shortcut is duplicated" : L"Это сочетание уже повторяется";
            }
        }
        replay_hotkey_validation_[index].Text(error);
        replay_hotkey_validation_[index].Visibility(error.empty() ? Visibility::Collapsed : Visibility::Visible);
        replay_hotkey_inputs_[index].BorderBrush(error.empty() ? normal_border : danger_brush_);
        valid = valid && error.empty();
    }

    std::wstring screenshot_error;
    const auto screenshot_enabled = screenshot_hotkey_toggle_ && screenshot_hotkey_toggle_.IsOn();
    const auto screenshot_chord = screenshot_hotkey_input_
        ? openreplay::ToUtf8(screenshot_hotkey_input_.Text().c_str()) : std::string{};
    if (screenshot_enabled && ParseHotkey(screenshot_chord).key == 0) {
        screenshot_error = english_ ? L"Choose a valid shortcut" : L"Задайте корректное сочетание";
    } else if (screenshot_enabled) {
        std::string normalized = screenshot_chord;
        for (auto& character : normalized) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        if (!chords.emplace(std::move(normalized)).second) {
            screenshot_error = english_ ? L"Already assigned to replay" : L"Уже назначено для повтора";
        }
        const auto native = ParseHotkey(screenshot_chord);
        if (screenshot_error.empty() && fixed_conflict(native)) {
            screenshot_error = english_ ? L"Conflicts with an OpenReplay overlay shortcut"
                                        : L"Конфликтует с хоткеем оверлея OpenReplay";
        }
    }
    if (screenshot_hotkey_validation_) {
        screenshot_hotkey_validation_.Text(screenshot_error);
        screenshot_hotkey_validation_.Visibility(
            screenshot_error.empty() ? Visibility::Collapsed : Visibility::Visible);
        screenshot_hotkey_input_.BorderBrush(screenshot_error.empty() ? normal_border : danger_brush_);
    }
    valid = valid && screenshot_error.empty();

    std::wstring recording_error;
    const auto recording_enabled = recording_hotkey_toggle_ && recording_hotkey_toggle_.IsOn();
    const auto recording_chord = recording_hotkey_input_
        ? openreplay::ToUtf8(recording_hotkey_input_.Text().c_str()) : std::string{};
    if (recording_enabled && ParseHotkey(recording_chord).key == 0) {
        recording_error = english_ ? L"Choose a valid shortcut" : L"Задайте корректное сочетание";
    } else if (recording_enabled) {
        std::string normalized = recording_chord;
        for (auto& character : normalized) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        if (!chords.emplace(std::move(normalized)).second) {
            recording_error = english_ ? L"Already assigned to another shortcut"
                                       : L"Уже назначено другому хоткею";
        }
        const auto native = ParseHotkey(recording_chord);
        if (recording_error.empty() && fixed_conflict(native)) {
            recording_error = english_ ? L"Conflicts with an OpenReplay overlay shortcut"
                                       : L"Конфликтует с хоткеем оверлея OpenReplay";
        }
    }
    if (recording_hotkey_validation_) {
        recording_hotkey_validation_.Text(recording_error);
        recording_hotkey_validation_.Visibility(
            recording_error.empty() ? Visibility::Collapsed : Visibility::Visible);
        recording_hotkey_input_.BorderBrush(recording_error.empty() ? normal_border : danger_brush_);
    }
    valid = valid && recording_error.empty();

    const auto registrations_changed = bindings != settings_.replay_hotkeys ||
        screenshot_enabled != settings_.screenshot_hotkey_enabled ||
        screenshot_chord != settings_.screenshot_hotkey_chord ||
        recording_enabled != settings_.recording_hotkey_enabled ||
        recording_chord != settings_.recording_hotkey_chord;
    if (valid && check_system_conflicts && registrations_changed) {
        UnregisterConfigurableHotkeys();
        std::vector<int> temporary_ids;
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            if (!bindings[index].enabled) continue;
            const auto native = ParseHotkey(bindings[index].chord);
            const auto id = kReplayHotkeyBase + 0x100 + static_cast<int>(index);
            if (!RegisterHotKey(GetWindowHandle(), id, native.modifiers | MOD_NOREPEAT, native.key)) {
                replay_hotkey_validation_[index].Text(english_ ? L"Used by Windows or another app"
                                                               : L"Занято Windows или другой программой");
                replay_hotkey_validation_[index].Visibility(Visibility::Visible);
                replay_hotkey_inputs_[index].BorderBrush(danger_brush_);
                valid = false;
            } else {
                temporary_ids.push_back(id);
            }
        }
        if (screenshot_enabled) {
            const auto native = ParseHotkey(screenshot_chord);
            const auto id = kScreenshotHotkey + 0x100;
            if (!RegisterHotKey(GetWindowHandle(), id, native.modifiers | MOD_NOREPEAT, native.key)) {
                screenshot_hotkey_validation_.Text(english_ ? L"Used by Windows or another app"
                                                             : L"Занято Windows или другой программой");
                screenshot_hotkey_validation_.Visibility(Visibility::Visible);
                screenshot_hotkey_input_.BorderBrush(danger_brush_);
                valid = false;
            } else {
                temporary_ids.push_back(id);
            }
        }
        if (recording_enabled) {
            const auto native = ParseHotkey(recording_chord);
            const auto id = kRecordingHotkey + 0x100;
            if (!RegisterHotKey(GetWindowHandle(), id, native.modifiers | MOD_NOREPEAT, native.key)) {
                recording_hotkey_validation_.Text(english_ ? L"Used by Windows or another app"
                                                           : L"Занято Windows или другой программой");
                recording_hotkey_validation_.Visibility(Visibility::Visible);
                recording_hotkey_input_.BorderBrush(danger_brush_);
                valid = false;
            } else {
                temporary_ids.push_back(id);
            }
        }
        for (const auto id : temporary_ids) UnregisterHotKey(GetWindowHandle(), id);
        RegisterConfigurableHotkeys();
    }

    if (!valid && show_notification) {
            ShowNotification(english_ ? L"Fix the highlighted shortcuts"
                                      : L"Исправьте подсвеченные хоткеи", true);
    }
    return valid;
}

void MainWindow::RequestReplaySave(std::uint32_t replay_seconds) {
    replay_save_pending_ = true;
    DWORD process_id = 0;
    const auto foreground = GetForegroundWindow();
    GetWindowThreadProcessId(foreground, &process_id);
    replay_game_ = GameNameOf(process_id == GetCurrentProcessId() ? last_external_window_ : foreground);
    const auto duration = std::to_wstring(replay_seconds);
    ShowToast(english_ ? L"Saving replay" : L"Сохраняем повтор",
              english_ ? L"Finalizing the last " + duration + L" seconds..."
                       : L"Собираем последние " + duration + L" секунд...",
              false, true, false, {}, NotificationChannel::ReplaySave);
    const auto command = replay_seconds == 0 ? std::string{"save_replay"}
                                             : std::string{"save_replay:"} + std::to_string(replay_seconds);
    if (!RunCommand(command)) {
        replay_save_pending_ = false;
        DismissNotificationChannel(NotificationChannel::ReplaySave);
    }
}

void MainWindow::EnsureHostRunning() {
    if (pipe_client_.Request("status", std::chrono::milliseconds{50}).ok) {
        host_connected_ = true;
        host_ever_connected_ = true;
        return;
    }
    if (HostProcessRunning()) return;
    last_host_launch_attempt_ = std::chrono::steady_clock::now();

    std::array<wchar_t, 32768> executable_path{};
    const DWORD length = GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (!length || length == executable_path.size()) {
        ShowNotification(english_ ? L"Unable to locate the application directory"
                                  : L"Не удалось определить папку приложения", true);
        return;
    }
    const auto directory = std::filesystem::path{std::wstring{executable_path.data(), length}}.parent_path();
    const auto host_path = directory / L"OpenReplay.Host.exe";
    if (!std::filesystem::exists(host_path)) {
        const auto message = english_ ? L"OpenReplay.Host.exe is missing" : L"Файл OpenReplay.Host.exe не найден";
        openreplay::ui::TraceStartup(message);
        ShowNotification(message, true);
        return;
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(host_path.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, directory.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        openreplay::ui::TraceStartup(L"Host process launch requested");
    } else {
        const auto message = (english_ ? std::wstring{L"Unable to launch Host. Windows error "}
                                       : std::wstring{L"Не удалось запустить Host. Ошибка Windows "}) +
                             std::to_wstring(GetLastError());
        openreplay::ui::TraceStartup(message);
        ShowNotification(message, true);
    }
}

bool MainWindow::PositionOverlayWindow() {
    const auto window = GetWindowHandle();
    const auto area = openreplay::ui::ForegroundMonitorArea(window);
    if (!area.monitor) return false;
    const auto monitor_width = area.bounds.right - area.bounds.left;
    const auto dpi = openreplay::ui::WindowDpi(window);
    auto panel_width = openreplay::ui::ScaleForDpi(520, dpi);
    if (panel_width > monitor_width) panel_width = monitor_width;
    const RECT bounds{area.bounds.left, area.bounds.top, area.bounds.left + panel_width, area.bounds.bottom};
    if (visible_ && EqualRect(&bounds, &overlay_bounds_)) return true;
    overlay_bounds_ = bounds;
    auto style = GetWindowLongPtrW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(window, GWL_STYLE, style);
    auto extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    extended_style &= ~WS_EX_APPWINDOW;
    extended_style |= WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(window, GWL_EXSTYLE, extended_style);
    SetWindowPos(window, HWND_TOPMOST, area.bounds.left, area.bounds.top,
                 panel_width, area.bounds.bottom - area.bounds.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return true;
}

void MainWindow::ShowOnLaunch() {
    const auto window_handle = GetWindowHandle();
    const bool desktop = openreplay::ui::ForegroundMonitorArea(window_handle).desktop;
    if (visible_ || !PositionOverlayWindow()) return;
    visible_ = true;
    Microsoft::UI::Xaml::Window window = *this;
    if (desktop) {
        window.AppWindow().Show(false);
    } else {
        window.Activate();
        ShowWindow(window_handle, SW_SHOW);
        SetForegroundWindow(window_handle);
        // Pointer, not Programmatic: after a hotkey, Programmatic focus draws the Tab focus ring.
        SettingsButton().Focus(FocusState::Pointer);
    }
    PollStatus();
}

void MainWindow::ShowOverlay() {
    if (visible_) return;
    const auto window = GetWindowHandle();
    const bool desktop = openreplay::ui::ForegroundMonitorArea(window).desktop;
    if (!PositionOverlayWindow()) return;
    visible_ = true;
    RefreshRecentClips();
    if (desktop) {
        Microsoft::UI::Xaml::Window xaml_window = *this;
        xaml_window.AppWindow().Show(false);
        PollStatus();
        return;
    }
    if (!AnimateWindow(window, 170, AW_ACTIVATE | AW_SLIDE | AW_HOR_POSITIVE)) {
        ShowWindow(window, SW_SHOW);
    }
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SettingsButton().Focus(FocusState::Pointer);
    PollStatus();
}

void MainWindow::HideOverlay() {
    if (clip_library_window_owner_) clip_library_window_owner_->HideWindow();
    const auto window = GetWindowHandle();
    const bool showing = IsWindowVisible(window) != FALSE;
    visible_ = false;
    if (!showing || !AnimateWindow(window, 130, AW_HIDE | AW_SLIDE | AW_HOR_NEGATIVE)) {
        ShowWindow(window, SW_HIDE);
    }
}

void MainWindow::SetClipFullscreen(bool fullscreen) {
    if (fullscreen) {
        clip_fullscreen_hid_overlay_ = visible_;
        clip_fullscreen_hid_performance_ = performance_overlay_.Visible();
        if (visible_) {
            visible_ = false;
            ShowWindow(GetWindowHandle(), SW_HIDE);
        }
        if (clip_fullscreen_hid_performance_) performance_overlay_.Toggle(settings_, english_);
        return;
    }

    if (clip_fullscreen_hid_performance_ && !performance_overlay_.Visible()) {
        performance_overlay_.Toggle(settings_, english_);
    }
    if (clip_fullscreen_hid_overlay_ && !visible_ && PositionOverlayWindow()) {
        visible_ = true;
        ShowWindow(GetWindowHandle(), SW_SHOWNOACTIVATE);
        PollStatus();
    }
    clip_fullscreen_hid_overlay_ = false;
    clip_fullscreen_hid_performance_ = false;
}

void MainWindow::ToggleOverlay() {
    if (visible_) HideOverlay();
    else ShowOverlay();
}

void MainWindow::ShowSettings(bool show) {
    if (show == settings_visible_) return;
    if (!show && settings_apply_pending_) {
        settings_apply_timer_.Stop();
        ApplySettingsFromControls(false);
    }
    settings_visible_ = show;
    DashboardPanel().Visibility(show ? Visibility::Collapsed : Visibility::Visible);
    SettingsPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    UpdateNavTabs();
    if (show) {
        LoadSettingsIntoControls();
        MonitorSelector().Focus(FocusState::Pointer);
    } else {
        SettingsButton().Focus(FocusState::Pointer);
    }
}

void MainWindow::UpdateNavTabs() {
    if (!back_button_) return;
    const auto active = DarkTheme().primary_text;
    const auto idle = DarkTheme().muted;
    back_button_.Foreground(settings_visible_ ? idle : active);
    gallery_button_.Foreground(idle);
    settings_button_text_.Foreground(settings_visible_ ? active : idle);
}

void MainWindow::UpdateBufferUi(const std::string& state) {
    if (!buffer_filled_run_) return;
    last_replay_state_ = state;
    const auto total = std::max<long long>(settings_.replay_seconds, 1);
    // shortcut: fill is estimated from when the host reported "running", switch to a host field if one is added.
    const auto now = std::chrono::steady_clock::now();
    if (state != "running") replay_running_since_ = {};
    else if (replay_running_since_ == std::chrono::steady_clock::time_point{}) replay_running_since_ = now;
    const auto elapsed = replay_running_since_ == std::chrono::steady_clock::time_point{}
        ? 0LL : std::chrono::duration_cast<std::chrono::seconds>(now - replay_running_since_).count();
    const auto filled = std::min(elapsed, total);
    buffer_filled_run_.Text(std::to_wstring(filled));
    buffer_total_run_.Text(L" / " + std::to_wstring(total) + (english_ ? L" sec" : L" сек"));
    const auto lit = static_cast<std::size_t>(filled * kBufferSegments / total);
    for (std::size_t index = 0; index < buffer_segment_cells_.size(); ++index) {
        buffer_segment_cells_[index].Background(index < lit ? accent_brush_ : track_brush_);
    }
    if (state == "running") {
        ReplayStateText().Text(filled < total ? (english_ ? L"filling" : L"заполняется")
                                              : (english_ ? L"buffered" : L"в буфере"));
    }

    const auto span = [this](long long seconds) {
        return seconds >= 120 ? L"\x2212" + std::to_wstring(seconds / 60) + (english_ ? L"m" : L" мин")
                              : L"\x2212" + std::to_wstring(seconds) + (english_ ? L"s" : L" с");
    };
    buffer_start_label_.Text(span(total));
    buffer_middle_label_.Text(span(total / 2));
    buffer_now_label_.Text(english_ ? L"now" : L"сейчас");
}

void MainWindow::UpdateDashboardMeta() {
    if (!dashboard_meta_text_) return;
    const auto codec = settings_.codec == openreplay::VideoCodec::Hevc ? L"HEVC"
                     : settings_.codec == openreplay::VideoCodec::Av1  ? L"AV1"
                                                                       : L"H.264";
    std::wstring text = std::wstring{codec} + L" · " + std::to_wstring(settings_.fps) + L" FPS · " +
                        std::to_wstring(settings_.bitrate_kbps / 1000) + (english_ ? L" Mbps" : L" Мбит/с");
    if (!free_space_summary_.empty()) text += L" · " + free_space_summary_;
    dashboard_meta_text_.Text(text);
}

void MainWindow::UpdateDashboardShortcuts() {
    if (!save_replay_keys_) return;
    const auto replay = std::ranges::find_if(settings_.replay_hotkeys, [](const auto& hotkey) { return hotkey.enabled; });
    FillKeycaps(save_replay_keys_, replay == settings_.replay_hotkeys.end() ? std::string_view{} : replay->chord, true);
    FillKeycaps(recording_keys_, settings_.recording_hotkey_enabled ? settings_.recording_hotkey_chord : "", false);
    FillKeycaps(screenshot_keys_, settings_.screenshot_hotkey_enabled ? settings_.screenshot_hotkey_chord : "", false);
}

void MainWindow::RefreshRecentClips() {
    if (!recent_clips_panel_) return;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    const auto generation = ++recent_clips_generation_;
    recent_clips_panel_.Children().Clear();

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::directory_entry>> clips;
    for (const auto& entry : ListClips(settings_.output_directory)) {
        std::error_code time_error;
        const auto written = entry.last_write_time(time_error);
        if (!time_error) clips.emplace_back(written, entry);
    }
    const auto count = std::min(clips.size(), kRecentClipCount);
    std::partial_sort(clips.begin(), clips.begin() + static_cast<std::ptrdiff_t>(count), clips.end(),
                      [](const auto& left, const auto& right) { return left.first > right.first; });

    const auto theme = DarkTheme();
    if (count == 0) {
        auto empty = Text(english_ ? L"No clips yet. Save a replay to see it here."
                                   : L"Пока нет клипов. Сохраните повтор, и он появится здесь.", 12.5, theme.muted);
        empty.TextWrapping(TextWrapping::Wrap);
        empty.Margin(Thickness{24, 0, 24, 0});
        recent_clips_panel_.Children().Append(empty);
        return;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& [written, entry] = clips[index];
        // The button spans the whole panel so its hover fill reaches the edges; the inset divider lives inside.
        Button row;
        row.Background(Brush(0x00000000));
        row.BorderThickness(Thickness{0, 0, 0, 0});
        row.CornerRadius(CornerRadius{0, 0, 0, 0});
        row.Padding(Thickness{0, 0, 0, 0});
        row.HorizontalAlignment(HorizontalAlignment::Stretch);
        row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        row.Click([this](auto&&, auto&&) { ShowClipLibrary(); });

        Grid content;
        content.ColumnSpacing(14);
        AddColumn(content, 96, GridUnitType::Pixel);
        AddColumn(content, 1, GridUnitType::Star);
        Border thumbnail;
        thumbnail.Height(54);
        thumbnail.CornerRadius(CornerRadius{6, 6, 6, 6});
        thumbnail.Background(theme.card);
        Image image;
        image.Stretch(Media::Stretch::UniformToFill);
        thumbnail.Child(image);
        content.Children().Append(thumbnail);

        StackPanel copy;
        copy.VerticalAlignment(VerticalAlignment::Center);
        auto title = Text(ClipTitle(entry.path(), english_), 13.5, theme.primary_text);
        title.FontWeight(Windows::UI::Text::FontWeights::Medium());
        copy.Children().Append(title);
        std::error_code size_error;
        const auto size = entry.file_size(size_error);
        auto detail = Text(ClipDay(written, english_) + L", " + ClipClock(written) + (size_error ? L"" : L" · " + ByteSize(size, english_)),
                           11.5, theme.muted);
        detail.Margin(Thickness{0, 3, 0, 0});
        copy.Children().Append(detail);
        Grid::SetColumn(copy, 1);
        content.Children().Append(copy);
        Border inset;
        inset.Margin(Thickness{24, 0, 24, 0});
        inset.Padding(Thickness{0, 10, 0, 10});
        inset.BorderBrush(theme.border);
        inset.BorderThickness(Thickness{0, 0, 0, 1});
        inset.Child(content);
        row.Content(inset);
        recent_clips_panel_.Children().Append(row);
        LoadRecentThumbnailAsync(entry.path(), image, generation);
    }
}

winrt::fire_and_forget MainWindow::LoadRecentThumbnailAsync(std::filesystem::path path,
                                                            Microsoft::UI::Xaml::Controls::Image image,
                                                            std::uint64_t generation) {
    const auto weak = get_weak();
    try {
        const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path.wstring());
        const auto thumbnail = co_await file.GetThumbnailAsync(
            Windows::Storage::FileProperties::ThumbnailMode::VideosView, 192,
            Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);
        Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(thumbnail);
        thumbnail.Close();
        if (const auto self = weak.get(); self && self->recent_clips_generation_ == generation) image.Source(bitmap);
    } catch (...) {
    }
}

void MainWindow::ShowClipLibrary() {
    // Repeated clicks must not rebuild the list and reload every thumbnail.
    if (clip_library_window_owner_ && clip_library_window_owner_->IsShown()) {
        clip_library_window_owner_->ActivateWindow();
        return;
    }
    if (!clip_library_window_owner_) {
        clip_library_window_owner_ = winrt::make_self<ClipLibraryWindow>();
        const auto weak = get_weak();
        clip_library_window_owner_->Configure(
            settings_.output_directory, english_, discord_webhook_configured_,
            settings_.discord_target_megabytes, GetWindowHandle(),
            [weak](std::filesystem::path path, bool webhook) {
                if (const auto self = weak.get()) self->ShareReplay(path, webhook);
            },
            [weak](bool fullscreen) {
                if (const auto self = weak.get()) self->SetClipFullscreen(fullscreen);
            });
    } else {
        clip_library_window_owner_->Configure(settings_.output_directory, english_, discord_webhook_configured_,
                                               settings_.discord_target_megabytes, GetWindowHandle(),
                                               [weak = get_weak()](std::filesystem::path path, bool webhook) {
                                                   if (const auto self = weak.get()) {
                                                       self->ShareReplay(path, webhook);
                                                   }
                                               },
                                               [weak = get_weak()](bool fullscreen) {
                                                   if (const auto self = weak.get()) {
                                                       self->SetClipFullscreen(fullscreen);
                                                   }
                                               });
    }
    clip_library_window_owner_->ActivateWindow();
}

void MainWindow::LoadSettingsIntoControls() {
    updating_ui_ = true;
    try {
        settings_ = settings_store_.Load();
    } catch (...) {
        settings_ = {};
        settings_.Normalize();
    }

    english_ = settings_.language == "en-US";
    if (!settings_initialized_) host_settings_ = settings_;
    ReplayDurationSlider().Value(settings_.replay_seconds);
    ReplayDurationValue().Text(std::to_wstring(settings_.replay_seconds) + (english_ ? L" sec" : L" сек"));
    BitrateSlider().Value(static_cast<double>(settings_.bitrate_kbps) / 1000.0);
    BitrateValue().Text(std::to_wstring(settings_.bitrate_kbps / 1000U) +
                        (english_ ? L" Mbps" : L" Мбит/с"));
    FpsSelector().SelectedIndex(settings_.fps <= 30 ? 0 : 1);
    CodecSelector().SelectedIndex(static_cast<int32_t>(settings_.codec));
    const auto quality_index = settings_.quality_preset == openreplay::QualityPreset::Custom
                                   ? 1
                                   : static_cast<int32_t>(settings_.quality_preset);
    QualitySelector().SelectedIndex(quality_index);
    OutputFormatSelector().SelectedIndex(settings_.output_format == openreplay::OutputFormat::Mp4 ? 1 : 0);
    performance_position_selector_.SelectedIndex(
        settings_.performance_overlay_position == openreplay::PerformanceOverlayPosition::BottomRight ? 1 : 0);
    performance_graph_style_selector_.SelectedIndex(settings_.performance_frametime_line ? 1 : 0);
    performance_opacity_slider_.Value(settings_.performance_overlay_opacity);
    performance_fps_.IsChecked(settings_.performance_show_fps);
    performance_fps_lows_.IsChecked(settings_.performance_show_fps_lows);
    performance_frametime_.IsChecked(settings_.performance_show_frametime);
    performance_frametime_graph_.IsChecked(settings_.performance_show_frametime_graph);
    performance_gpu_usage_.IsChecked(settings_.performance_show_gpu_usage);
    performance_gpu_temperature_.IsChecked(settings_.performance_show_gpu_temperature);
    performance_gpu_clock_.IsChecked(settings_.performance_show_gpu_clock);
    performance_gpu_power_.IsChecked(settings_.performance_show_gpu_power);
    performance_gpu_memory_.IsChecked(settings_.performance_show_gpu_memory);
    performance_cpu_usage_.IsChecked(settings_.performance_show_cpu_usage);
    performance_memory_.IsChecked(settings_.performance_show_memory);
    ReplayToggle().IsOn(settings_.instant_replay_enabled);
    MicrophoneToggle().IsOn(settings_.microphone_enabled);
    CursorToggle().IsOn(settings_.capture_cursor);
    start_with_windows_toggle_.IsOn(settings_.start_with_windows);
    automatic_updates_toggle_.IsOn(settings_.automatic_updates);
    developer_updates_toggle_.IsOn(settings_.developer_updates);
    discord_credential_error_.clear();
    discord_webhook_configured_ = discord_share_service_.HasWebhook(&discord_credential_error_);
    const bool discord_settings_reconciled = !discord_webhook_configured_ && discord_credential_error_.empty() &&
                                              settings_.discord_auto_send;
    if (discord_settings_reconciled) settings_.discord_auto_send = false;
    discord_webhook_input_.Password(L"");
    discord_size_input_.Value(settings_.discord_target_megabytes);
    discord_auto_send_toggle_.IsOn(settings_.discord_auto_send && discord_webhook_configured_);
    LanguageSelector().SelectedIndex(english_ ? 1 : 0);
    OutputDirectoryText().Text(settings_.output_directory.wstring());
    EnumerateMonitors();
    EnumerateAudioDevices();
    screenshot_hotkey_toggle_.IsOn(settings_.screenshot_hotkey_enabled);
    screenshot_hotkey_input_.Text(openreplay::FromUtf8(settings_.screenshot_hotkey_chord));
    recording_hotkey_toggle_.IsOn(settings_.recording_hotkey_enabled);
    recording_hotkey_input_.Text(openreplay::FromUtf8(settings_.recording_hotkey_chord));
    replay_hotkey_draft_ = settings_.replay_hotkeys;
    RebuildReplayHotkeyRows();
    if (audio_settings_reconciled_ || discord_settings_reconciled) {
        try {
            settings_store_.Save(settings_);
            if (audio_settings_reconciled_) {
                audio_restore_notification_pending_ = true;
                QueueHostSettingsApply(true);
            }
        } catch (...) {
        }
        audio_settings_reconciled_ = false;
    }
    if (openreplay::ReplayOutputSettingsEqual(host_settings_, settings_)) RegisterConfigurableHotkeys();
    UpdateReplaySizeEstimate();
    UpdateStorageSpace();
    UpdatePerformanceOverlaySummary();
    UpdateDiscordUi();
    UpdateUpdateUi();
    performance_overlay_.ApplySettings(settings_, english_);
    LayoutNotifications();
    if (!ApplyStartWithWindows(settings_.start_with_windows)) {
        ShowNotification(english_ ? L"Unable to synchronize Windows startup"
                                  : L"Не удалось синхронизировать автозапуск Windows", true);
    }
    updating_ui_ = false;
    settings_initialized_ = true;
    UpdateSettingsSaveStatus(english_ ? L"Changes save automatically" : L"Изменения сохраняются автоматически");
}

void MainWindow::EnumerateMonitors() {
    monitor_ids_.clear();
    MonitorSelector().Items().Clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorCallback, reinterpret_cast<LPARAM>(this));

    int32_t selected = 0;
    for (size_t index = 0; index < monitor_ids_.size(); ++index) {
        if (monitor_ids_[index] == settings_.monitor_id) selected = static_cast<int32_t>(index);
    }
    if (!monitor_ids_.empty()) MonitorSelector().SelectedIndex(selected);
}

void MainWindow::EnumerateAudioDevices() {
    auto outputs = AudioDevices(eRender);
    auto inputs = AudioDevices(eCapture);
    const auto configs = [](const std::vector<AudioDeviceInfo>& devices) {
        std::vector<openreplay::AudioDeviceConfig> result;
        result.reserve(devices.size());
        for (const auto& device : devices) result.push_back({device.id, device.utf8_name, 100});
        return result;
    };
    audio_settings_reconciled_ =
        openreplay::ReconcileAudioDevices(settings_.desktop_audio_devices, configs(outputs), false) ||
        audio_settings_reconciled_;
    audio_settings_reconciled_ =
        openreplay::ReconcileAudioDevices(settings_.microphone_devices, configs(inputs), false) ||
        audio_settings_reconciled_;

    const auto populate = [this](StackPanel const& panel, std::vector<std::string>& ids,
                                   std::vector<std::string>& names, std::vector<CheckBox>& checks,
                                   std::vector<Microsoft::UI::Xaml::Controls::Slider>& volumes,
                                   std::vector<AudioLevelIndicator>& levels,
                                   std::vector<AudioDeviceInfo> devices,
                                   const std::vector<openreplay::AudioDeviceConfig>& selected) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const auto theme = DarkTheme();
        const ControlFactory factory{theme};
        panel.Children().Clear();
        ids.clear();
        names.clear();
        checks.clear();
        volumes.clear();
        levels.clear();
        for (const auto& configured : selected) {
            if (configured.id == "default" || std::ranges::any_of(devices, [&](const auto& device) {
                    return device.id == configured.id;
                })) {
                continue;
            }
            const auto base_name = configured.name.empty()
                ? (english_ ? L"Unavailable audio device" : L"Недоступное аудиоустройство")
                : openreplay::FromUtf8(configured.name);
            devices.push_back({configured.id,
                               base_name + (english_ ? L"  ·  Unavailable" : L"  ·  Недоступно"),
                               configured.name, false});
        }
        const auto current_default = std::ranges::find_if(devices, [](const auto& device) { return device.is_default; });
        const auto default_name = current_default == devices.end() ? std::wstring{} : current_default->name;
        const auto default_label = (english_ ? std::wstring{L"Default Windows device"}
                                             : std::wstring{L"Устройство Windows по умолчанию"}) +
            (default_name.empty() ? std::wstring{} : L"  ·  " + default_name);
        const auto configured_default = FindDevice(selected, "default");
        devices.insert(devices.begin(), {"default", default_label,
            configured_default ? configured_default->name : std::string{}, true});
        for (const auto& device : devices) {
            auto surface = factory.AudioDeviceSurface();
            StackPanel content;
            content.Spacing(4);
            const auto configured = FindDevice(selected, device.id);
            auto check = factory.Check(device.name, configured != nullptr, 12.5);
            check.Checked({this, &MainWindow::AudioDeviceSelection_Changed});
            check.Unchecked({this, &MainWindow::AudioDeviceSelection_Changed});
            content.Children().Append(check);

            Grid controls;
            AddColumn(controls, 1, GridUnitType::Star);
            AddColumn(controls, 8, GridUnitType::Pixel);
            AddColumn(controls, 116, GridUnitType::Pixel);
            AddColumn(controls, 38, GridUnitType::Pixel);
            Border level_track;
            level_track.Height(4);
            level_track.Background(theme.track);
            level_track.CornerRadius(CornerRadius{2, 2, 2, 2});
            level_track.VerticalAlignment(VerticalAlignment::Center);
            Grid level_grid;
            ColumnDefinition filled;
            filled.Width(GridLength{0, GridUnitType::Star});
            ColumnDefinition remaining;
            remaining.Width(GridLength{1, GridUnitType::Star});
            level_grid.ColumnDefinitions().Append(filled);
            level_grid.ColumnDefinitions().Append(remaining);
            Border level_fill;
            level_fill.Background(success_brush_);
            level_fill.CornerRadius(CornerRadius{2, 2, 2, 2});
            level_grid.Children().Append(level_fill);
            level_track.Child(level_grid);
            controls.Children().Append(level_track);
            auto volume_control = factory.RangeSlider(
                0, 100, 1, configured ? configured->volume_percent : 100);
            auto volume = volume_control.input;
            volume_control.view.MinHeight(32);
            volume_control.view.VerticalAlignment(VerticalAlignment::Center);
            volume.IsEnabled(configured != nullptr);
            Grid::SetColumn(volume_control.view, 2);
            controls.Children().Append(volume_control.view);
            auto value = Text(std::to_wstring(static_cast<std::uint32_t>(std::lround(volume.Value()))) + L"%",
                              10.5, secondary_text_brush_);
            value.VerticalAlignment(VerticalAlignment::Center);
            value.HorizontalAlignment(HorizontalAlignment::Right);
            Grid::SetColumn(value, 3);
            controls.Children().Append(value);
            volume.ValueChanged([this, value](auto const&, auto const& args) {
                value.Text(std::to_wstring(static_cast<std::uint32_t>(std::lround(args.NewValue()))) + L"%");
                ScheduleSettingsApply();
            });
            content.Children().Append(controls);
            surface.Child(content);
            panel.Children().Append(surface);
            ids.push_back(device.id);
            names.push_back(device.utf8_name);
            checks.push_back(check);
            volumes.push_back(volume);
            levels.push_back({filled, remaining});
        }
    };

    populate(desktop_audio_devices_panel_, desktop_audio_device_ids_, desktop_audio_device_names_,
             desktop_audio_device_checks_, desktop_audio_device_volumes_, desktop_audio_device_levels_,
             std::move(outputs), settings_.desktop_audio_devices);
    populate(microphone_devices_panel_, microphone_device_ids_, microphone_device_names_,
             microphone_device_checks_, microphone_device_volumes_, microphone_device_levels_,
             std::move(inputs), settings_.microphone_devices);
    UpdateAudioDeviceSummaries();
}

void MainWindow::AudioDeviceSelection_Changed(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto update = [](const std::vector<CheckBox>& checks,
                           const std::vector<Microsoft::UI::Xaml::Controls::Slider>& volumes) {
        for (std::size_t index = 0; index < checks.size() && index < volumes.size(); ++index) {
            const auto checked = checks[index].IsChecked();
            volumes[index].IsEnabled(checked && checked.Value());
        }
    };
    update(desktop_audio_device_checks_, desktop_audio_device_volumes_);
    update(microphone_device_checks_, microphone_device_volumes_);
    UpdateAudioDeviceSummaries();
    ScheduleSettingsApply();
}

void MainWindow::UpdateAudioDeviceSummaries() {
    const auto summary = [this](const std::vector<CheckBox>& checks) {
        std::vector<std::wstring> selected;
        for (const auto& check : checks) {
            const auto checked = check.IsChecked();
            if (!checked || !checked.Value()) continue;
            const auto content = winrt::unbox_value_or<winrt::hstring>(check.Content(), {});
            selected.push_back(content.c_str());
        }
        if (selected.empty()) return english_ ? std::wstring{L"None"} : std::wstring{L"Не выбрано"};
        if (selected.size() == 1) {
            auto name = selected.front();
            if (name.size() > 28) name = name.substr(0, 27) + L"…";
            return name;
        }
        return std::to_wstring(selected.size()) + (english_ ? L" selected" : L" выбрано");
    };
    if (desktop_audio_summary_) desktop_audio_summary_.Text(summary(desktop_audio_device_checks_));
    if (microphone_audio_summary_) microphone_audio_summary_.Text(summary(microphone_device_checks_));
}

BOOL CALLBACK MainWindow::MonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* self = reinterpret_cast<MainWindow*>(context);
    MONITORINFOEXW monitor_info{sizeof(monitor_info)};
    if (!GetMonitorInfoW(monitor, &monitor_info)) return TRUE;

    DISPLAY_DEVICEW display_device{sizeof(display_device)};
    const bool has_device = EnumDisplayDevicesW(monitor_info.szDevice, 0, &display_device,
                                                EDD_GET_DEVICE_INTERFACE_NAME) != FALSE;
    const std::wstring id = has_device && display_device.DeviceID[0] ? display_device.DeviceID : monitor_info.szDevice;
    const auto width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const auto height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    std::wstring label = FriendlyMonitorName(monitor_info.szDevice);
    if (label.empty() && has_device && display_device.DeviceString[0]) label = display_device.DeviceString;
    if (label.empty()) {
        label = self->english_ ? L"Monitor " : L"Монитор ";
        label += std::to_wstring(self->monitor_ids_.size() + 1U);
    }
    label += L"  ·  " + std::to_wstring(width) + L" × " + std::to_wstring(height);
    if ((monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0) label += self->english_ ? L"  (Primary)" : L"  (Основной)";

    const ControlFactory controls{DarkTheme()};
    auto item = controls.Option(label);
    self->MonitorSelector().Items().Append(item);
    self->monitor_ids_.push_back(openreplay::ToUtf8(id));
    return TRUE;
}

void MainWindow::ApplyLanguage() {
    const auto set = [](auto control, std::wstring_view text) { control.Text(winrt::hstring{text}); };
    set(SettingsButtonText(), english_ ? L"Settings" : L"Настройки");
    set(ReplayTitle(), english_ ? L"Instant replay" : L"Мгновенный повтор");
    set(ReplayDescription(), english_ ? L"Background buffer of the last " + std::to_wstring(settings_.replay_seconds) + L" sec"
                                      : L"Фоновый буфер последних " + std::to_wstring(settings_.replay_seconds) + L" сек");
    set(ScreenshotTitle(), english_ ? L"Screenshot" : L"Снимок экрана");
    set(ScreenshotStateText(), english_ ? L"PNG · native resolution" : L"PNG · исходное разрешение");
    set(SettingsTitle(), english_ ? L"Settings" : L"Настройки");
    set(save_replay_text_, english_ ? L"Save replay" : L"Сохранить повтор");
    set(recent_title_, english_ ? L"RECENT" : L"НЕДАВНИЕ");
    recent_all_button_.Content(winrt::box_value(english_ ? L"All clips →" : L"Все клипы →"));
    set(source_section_title_, english_ ? L"SOURCE" : L"ИСТОЧНИК");
    set(video_section_title_, english_ ? L"VIDEO" : L"ВИДЕО");
    set(input_section_title_, english_ ? L"INTERFACE AND INPUT" : L"ИНТЕРФЕЙС И ВВОД");
    set(storage_section_title_, english_ ? L"STORAGE" : L"ХРАНИЛИЩЕ");
    set(discord_section_title_, english_ ? L"DISCORD SHARE" : L"DISCORD");
    set(updates_section_title_, english_ ? L"UPDATES" : L"ОБНОВЛЕНИЯ");
    set(MonitorLabel(), english_ ? L"Monitor" : L"Монитор");
    set(DesktopAudioLabel(), english_ ? L"Desktop audio outputs" : L"Системный звук");
    set(MicrophoneDevicesLabel(), english_ ? L"Microphones and inputs" : L"Микрофоны и входы");
    set(audio_devices_hint_, english_ ? L"Select multiple devices, up to 5 active audio sources in total."
                                      : L"Можно выбрать несколько устройств, суммарно до 5 активных источников.");
    set(ReplayDurationLabel(), english_ ? L"Replay duration" : L"Длительность повтора");
    set(QualityLabel(), english_ ? L"Quality preset" : L"Пресет качества");
    set(BitrateLabel(), english_ ? L"Target bitrate" : L"Целевой битрейт");
    set(replay_size_label_, english_ ? L"per replay" : L"на один повтор");
    set(CodecLabel(), english_ ? L"Codec" : L"Кодек");
    set(FpsLabel(), english_ ? L"Frame rate" : L"Частота кадров");
    set(LanguageLabel(), english_ ? L"Language" : L"Язык");
    set(OutputLabel(), english_ ? L"Output folder" : L"Папка сохранения");
    set(OutputFormatLabel(), english_ ? L"Video format" : L"Формат видео");
    set(discord_webhook_label_, english_ ? L"Webhook URL" : L"Webhook URL");
    set(discord_size_label_, english_ ? L"Maximum upload size, MiB" : L"Максимальный размер, МиБ");
    set(discord_auto_send_label_, english_ ? L"Send automatically after saving"
                                            : L"Отправлять автоматически после сохранения");
    set(microphone_label_, english_ ? L"Microphone" : L"Микрофон");
    set(cursor_label_, english_ ? L"Capture cursor" : L"Захватывать курсор");
    set(start_with_windows_label_, english_ ? L"Start with Windows" : L"Запускать вместе с Windows");
    set(automatic_updates_label_, english_ ? L"Check and download automatically"
                                            : L"Проверять и скачивать автоматически");
    set(developer_updates_label_, english_ ? L"Receive developer builds"
                                           : L"Получать dev-сборки");
    set(shortcuts_label_, english_ ? L"Shortcuts" : L"Хоткеи");
    set(recording_hotkey_label_, english_ ? L"Session recording" : L"Запись сессии");
    recording_hotkey_input_.PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
    set(screenshot_hotkey_label_, english_ ? L"Screenshot" : L"Снимок экрана");
    screenshot_hotkey_input_.PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
    set(performance_overlay_label_, english_ ? L"Performance overlay" : L"Оверлей производительности");
    set(performance_position_label_, english_ ? L"Position" : L"Положение");
    set(performance_graph_style_label_, english_ ? L"Frame-time graph" : L"График времени кадра");
    const auto graph_styles = performance_graph_style_selector_.Items();
    graph_styles.GetAt(0).as<ComboBoxItem>().Content(winrt::box_value(english_ ? L"Bars" : L"Столбики"));
    graph_styles.GetAt(1).as<ComboBoxItem>().Content(winrt::box_value(english_ ? L"Line" : L"Линия"));
    set(performance_opacity_label_, english_ ? L"Background opacity" : L"Прозрачность фона");
    performance_fps_.Content(winrt::box_value(L"FPS"));
    performance_fps_lows_.Content(winrt::box_value(L"1% / 0.1% FPS"));
    performance_frametime_.Content(winrt::box_value(english_ ? L"Frame time" : L"Время кадра"));
    performance_frametime_graph_.Content(winrt::box_value(english_ ? L"Frame-time graph" : L"График времени кадра"));
    performance_gpu_usage_.Content(winrt::box_value(english_ ? L"GPU usage" : L"Загрузка GPU"));
    performance_gpu_temperature_.Content(winrt::box_value(english_ ? L"GPU temperature" : L"Температура GPU"));
    performance_gpu_clock_.Content(winrt::box_value(english_ ? L"GPU clock" : L"Частота GPU"));
    performance_gpu_power_.Content(winrt::box_value(english_ ? L"GPU power" : L"Потребление GPU"));
    performance_gpu_memory_.Content(winrt::box_value(L"VRAM"));
    performance_cpu_usage_.Content(winrt::box_value(english_ ? L"CPU usage" : L"Загрузка CPU"));
    performance_memory_.Content(winrt::box_value(english_ ? L"System memory" : L"Оперативная память"));
    const auto performance_positions = performance_position_selector_.Items();
    performance_positions.GetAt(0).as<ComboBoxItem>().Content(
        winrt::box_value(english_ ? L"Top right" : L"Справа сверху"));
    performance_positions.GetAt(1).as<ComboBoxItem>().Content(
        winrt::box_value(english_ ? L"Bottom right" : L"Справа снизу"));
    for (std::size_t index = 0; index < replay_hotkey_labels_.size(); ++index) {
        set(replay_hotkey_labels_[index], english_ ? L"Replay " + std::to_wstring(index + 1)
                                                   : L"Повтор " + std::to_wstring(index + 1));
        set(replay_hotkey_units_[index], english_ ? L"sec" : L"сек");
        replay_hotkey_inputs_[index].PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
    }
    replay_hotkey_add_button_.Content(winrt::box_value(english_ ? L"+ Add replay shortcut" : L"+ Добавить хоткей"));
    const auto shortcut_count = std::ranges::count_if(replay_hotkey_toggles_, [](const auto& toggle) {
        return toggle.IsOn();
    }) + (screenshot_hotkey_toggle_.IsOn() ? 1 : 0) + (recording_hotkey_toggle_.IsOn() ? 1 : 0);
    set(shortcuts_summary_, std::to_wstring(shortcut_count) +
        (english_ ? L" active" : (shortcut_count == 1 ? L" активно" : L" активны")));
    set(HintText(), english_ ? L"Alt+Z — hide panel" : L"Alt+Z — скрыть панель");

    ReplayToggle().OnContent(nullptr);
    ReplayToggle().OffContent(nullptr);
    MicrophoneToggle().OnContent(nullptr);
    MicrophoneToggle().OffContent(nullptr);
    CursorToggle().OnContent(nullptr);
    CursorToggle().OffContent(nullptr);
    start_with_windows_toggle_.OnContent(nullptr);
    start_with_windows_toggle_.OffContent(nullptr);
    automatic_updates_toggle_.OnContent(nullptr);
    automatic_updates_toggle_.OffContent(nullptr);
    developer_updates_toggle_.OnContent(nullptr);
    developer_updates_toggle_.OffContent(nullptr);
    discord_auto_send_toggle_.OnContent(nullptr);
    discord_auto_send_toggle_.OffContent(nullptr);
    if (!settings_apply_pending_ && !settings_reload_in_flight_ && !settings_reload_requested_) {
        UpdateSettingsSaveStatus(english_ ? L"Changes save automatically"
                                           : L"Изменения сохраняются автоматически");
    }
    back_button_.Content(winrt::box_value(english_ ? L"Home" : L"Главная"));
    gallery_button_.Content(winrt::box_value(english_ ? L"Clips" : L"Клипы"));
    open_folder_button_.Content(winrt::box_value(english_ ? L"Recordings" : L"Записи"));
    open_logs_button_.Content(winrt::box_value(english_ ? L"Logs" : L"Логи"));
    discord_save_button_.Content(winrt::box_value(english_ ? L"Save webhook" : L"Сохранить webhook"));
    discord_clear_button_.Content(winrt::box_value(english_ ? L"Remove webhook" : L"Удалить webhook"));
    discord_test_button_.Content(winrt::box_value(english_ ? L"Test" : L"Проверить"));
    check_update_button_.Content(winrt::box_value(english_ ? L"Check now" : L"Проверить"));
    release_notes_button_.Content(winrt::box_value(english_ ? L"Release notes" : L"Что нового"));
    RecordingTitle().Text(recording_ ? (english_ ? L"Stop recording" : L"Остановить запись")
                                     : (english_ ? L"Start recording" : L"Начать запись"));

    const auto quality_items = QualitySelector().Items();
    quality_items.GetAt(0).as<ComboBoxItem>().Content(winrt::box_value(english_ ? L"Performance" : L"Производительность"));
    quality_items.GetAt(1).as<ComboBoxItem>().Content(winrt::box_value(english_ ? L"Balanced" : L"Сбалансированный"));
    quality_items.GetAt(2).as<ComboBoxItem>().Content(winrt::box_value(english_ ? L"High quality" : L"Высокое качество"));
    const auto language_items = LanguageSelector().Items();
    language_items.GetAt(0).as<ComboBoxItem>().Content(winrt::box_value(L"Русский"));
    language_items.GetAt(1).as<ComboBoxItem>().Content(winrt::box_value(L"English"));
    BitrateValue().Text(std::to_wstring(static_cast<uint32_t>(std::lround(BitrateSlider().Value()))) +
                        (english_ ? L" Mbps" : L" Мбит/с"));
    UpdateQualityDescription();
    UpdateOutputFormatDescription();
    UpdateReplaySizeEstimate();
    UpdateAudioDeviceSummaries();
    UpdatePerformanceOverlaySummary();
    UpdateNavTabs();
    UpdateDashboardShortcuts();
    UpdateDashboardMeta();
    UpdateBufferUi(last_replay_state_);
    UpdateDiscordUi();
    last_storage_update_ = {};
    UpdateStorageSpace();
    UpdateUpdateUi();
}

void MainWindow::UpdateQualityDescription() {
    const auto selection = QualitySelector().SelectedIndex();
    if (selection == 0) {
        quality_description_.Text(english_
                                      ? L"Lower GPU load and faster encoding. Bitrate is configured separately."
                                      : L"Ниже нагрузка на GPU и быстрее кодирование. Битрейт задаётся отдельно.");
    } else if (selection == 2) {
        quality_description_.Text(english_
                                      ? L"Better compression at the same bitrate, with higher GPU load."
                                      : L"Лучшее сжатие при том же битрейте, но выше нагрузка на GPU.");
    } else {
        quality_description_.Text(english_
                                      ? L"Balanced compression quality and GPU load. Bitrate is configured separately."
                                      : L"Баланс качества сжатия и нагрузки на GPU. Битрейт задаётся отдельно.");
    }
}

void MainWindow::UpdateOutputFormatDescription() {
    if (!output_format_description_ || !OutputFormatSelector()) return;
    const bool mp4 = OutputFormatSelector().SelectedIndex() == 1;
    if (mp4) {
        output_format_description_.Text(
            english_ ? L"Broad compatibility with players, editors, and sharing apps."
                     : L"Широкая совместимость с плеерами, редакторами и приложениями для публикации.");
    } else {
        output_format_description_.Text(
            english_ ? L"Flexible container for recording and post-production workflows."
                     : L"Гибкий контейнер для записи и последующей обработки.");
    }
    output_format_description_.Foreground(secondary_text_brush_);
}

void MainWindow::UpdateReplaySizeEstimate() {
    if (!replay_size_value_ || !replay_size_detail_ || !ReplayDurationSlider() || !BitrateSlider()) return;

    auto estimate = settings_;
    estimate.replay_seconds = static_cast<uint32_t>(std::lround(ReplayDurationSlider().Value()));
    estimate.bitrate_kbps = static_cast<uint32_t>(std::lround(BitrateSlider().Value())) * 1000U;
    estimate.microphone_enabled = MicrophoneToggle() && MicrophoneToggle().IsOn();
    const auto estimated_bytes = openreplay::EstimateReplaySizeBytes(estimate);
    const auto cap_bytes = static_cast<uint64_t>(estimate.replay_max_megabytes) * 1024U * 1024U;
    const bool capped = estimated_bytes > cap_bytes;
    const auto shown_bytes = std::min(estimated_bytes, cap_bytes);
    constexpr uint64_t mebibyte = 1024U * 1024U;
    constexpr uint64_t gibibyte = 1024U * mebibyte;

    std::wstring size = english_ ? (capped ? L"≈ up to " : L"≈ ") : (capped ? L"≈ до " : L"≈ ");
    if (shown_bytes >= gibibyte) {
        const auto tenths = (shown_bytes * 10U + gibibyte / 2U) / gibibyte;
        size += std::to_wstring(tenths / 10U) + (english_ ? L"." : L",") +
                std::to_wstring(tenths % 10U) + (english_ ? L" GB" : L" ГБ");
    } else {
        size += std::to_wstring((shown_bytes + mebibyte / 2U) / mebibyte) +
                (english_ ? L" MB" : L" МБ");
    }
    replay_size_value_.Text(size);

    if (capped) {
        replay_size_detail_.Text(
            english_ ? L"The estimate exceeds the " + std::to_wstring(estimate.replay_max_megabytes) +
                           L" MB buffer limit; the saved clip will be shorter."
                     : L"Расчёт превышает лимит буфера " + std::to_wstring(estimate.replay_max_megabytes) +
                           L" МБ; сохранится более короткий фрагмент.");
        replay_size_detail_.Foreground(danger_brush_);
        return;
    }

    const auto format = OutputFormatSelector() && OutputFormatSelector().SelectedIndex() == 1 ? L"MP4" : L"MKV";
    const auto tracks = estimate.microphone_enabled ? 3U : 2U;
    replay_size_detail_.Text(
        std::to_wstring(estimate.replay_seconds) + (english_ ? L" sec · VBR " : L" сек · VBR ") +
        std::to_wstring(estimate.bitrate_kbps / 1000U) + (english_ ? L" Mbps · " : L" Мбит/с · ") +
        std::to_wstring(tracks) + (english_ ? L" audio tracks · " : L" аудиодорожки · ") + format);
    replay_size_detail_.Foreground(secondary_text_brush_);
}

void MainWindow::PollStatus() {
    // Follow the active window on this monitor: beside the taskbar over a normal window, at the edge over a
    // fullscreen game. While one of our own windows is active the panel stays put, since it still covers the game.
    const auto foreground = GetForegroundWindow();
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    const bool external = foreground && process_id != GetCurrentProcessId();
    // Remembered so a replay saved from the panel is still named after the game under it.
    if (external) last_external_window_ = foreground;
    if (visible_ && external && MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) ==
                                    MonitorFromWindow(GetWindowHandle(), MONITOR_DEFAULTTONULL)) {
        PositionOverlayWindow();
    }
    LayoutNotifications();
    ReconcileAudioDevices();
    UpdateStorageSpace();
    if (settings_reload_in_flight_) return;
    const auto response = pipe_client_.Request("status", std::chrono::milliseconds{250});
    if (!response.ok) {
        host_connected_ = false;
        ++host_poll_failures_;
        const auto error_threshold = host_ever_connected_ ? 20 : 40;
        const auto host_process_running = HostProcessRunning();
        if (host_ever_connected_ && host_process_running && host_poll_failures_ < error_threshold) return;
        const bool unavailable = host_poll_failures_ >= error_threshold && !host_process_running;
        const bool busy = host_poll_failures_ >= error_threshold && host_process_running;
        std::wstring encoder_status;
        std::wstring replay_status;
        if (unavailable) {
            encoder_status = english_ ? L"Host unavailable • retrying" : L"Host недоступен • перезапуск";
            replay_status = english_ ? L"Connection error" : L"Ошибка подключения";
        } else if (busy) {
            encoder_status = english_ ? L"Host is busy • waiting" : L"Host занят • ожидание";
            replay_status = english_ ? L"Applying capture changes..." : L"Применение настроек захвата...";
        } else {
            encoder_status = english_ ? L"Host is starting..." : L"Host запускается...";
            replay_status = english_ ? L"Connecting..." : L"Подключение...";
        }
        EncoderText().Text(encoder_status);
        ReplayStateText().Text(replay_status);
        ReplayStateText().Foreground(unavailable ? danger_brush_ : secondary_text_brush_);
        UpdateBufferUi({});
        SaveReplayButton().IsEnabled(false);
        RecordingButton().IsEnabled(false);
        ScreenshotButton().IsEnabled(false);

        if (replay_save_pending_ && !host_process_running && host_poll_failures_ >= error_threshold) {
            replay_save_pending_ = false;
            const auto message = english_ ? L"Host stopped before the replay was saved. Open Logs in Settings."
                                          : L"Host остановился до сохранения повтора. Откройте «Логи» в настройках.";
            ShowToast(english_ ? L"Replay save interrupted" : L"Сохранение повтора прервано",
                      message, true, false, true, {}, NotificationChannel::ReplaySave);
            openreplay::ui::TraceStartup(message);
        }

        const auto now = std::chrono::steady_clock::now();
        if (!host_process_running && host_poll_failures_ >= error_threshold &&
            (last_host_launch_attempt_ == std::chrono::steady_clock::time_point{} ||
             now - last_host_launch_attempt_ >= std::chrono::seconds{5})) {
            EnsureHostRunning();
        }

        if (!host_process_running && host_poll_failures_ >= error_threshold && !host_error_shown_) {
            host_error_shown_ = true;
            const auto message = english_
                                     ? L"The capture Host stopped. Automatic restart is in progress; details are in Logs."
                                     : L"Host захвата остановился. Выполняется автоперезапуск; подробности — в «Логах».";
            ShowToast(english_ ? L"Capture engine error" : L"Ошибка движка захвата",
                      message, true, false, true);
            openreplay::ui::TraceStartup(message);
        }
        return;
    }
    host_connected_ = true;
    host_ever_connected_ = true;
    host_poll_failures_ = 0;
    host_error_shown_ = false;
    ApplyResponse(response);
    PublishUpdateHealth();
    ApplyPendingReplayToggle();
    StartPendingHostSettingsApply();
}

void MainWindow::UpdateUpdateUi() {
    if (!update_version_text_ || !update_status_text_ || !update_action_button_) return;
    update_version_text_.Text(L"OpenReplay " + openreplay::FromUtf8(openreplay::kVersion) +
                               (openreplay::kReleaseChannel == "dev"
                                    ? (english_ ? L" · Dev" : L" · Dev-канал")
                                    : (english_ ? L" · Stable" : L" · Стабильный канал")));
    check_update_button_.IsEnabled(!update_check_in_flight_ && !update_download_in_flight_);
    developer_updates_toggle_.IsEnabled(!update_check_in_flight_ && !update_download_in_flight_);
    if (update_download_in_flight_) {
        update_status_text_.Text(english_ ? L"Downloading and verifying update..."
                                          : L"Загрузка и проверка обновления...");
        update_action_button_.Visibility(Visibility::Collapsed);
        release_notes_button_.Visibility(Visibility::Visible);
        return;
    }
    if (update_check_in_flight_) {
        update_status_text_.Text(english_ ? L"Checking signed release metadata..."
                                          : L"Проверка подписанных данных релиза...");
        update_action_button_.Visibility(Visibility::Collapsed);
        return;
    }
    if (!downloaded_update_.empty()) {
        update_status_text_.Text(english_ ? L"Update " + openreplay::FromUtf8(available_update_.version) +
                                               L" is ready. Restart when capture is idle."
                                          : L"Обновление " + openreplay::FromUtf8(available_update_.version) +
                                               L" готово. Перезапустите после завершения захвата.");
        update_action_button_.Content(winrt::box_value(english_ ? L"Restart to update" : L"Перезапустить"));
        update_action_button_.Visibility(Visibility::Visible);
        release_notes_button_.Visibility(Visibility::Visible);
        return;
    }
    if (!available_update_.version.empty()) {
        update_status_text_.Text(english_ ? L"Version " + openreplay::FromUtf8(available_update_.version) +
                                               L" is available."
                                          : L"Доступна версия " + openreplay::FromUtf8(available_update_.version) + L".");
        update_action_button_.Content(winrt::box_value(english_ ? L"Download update" : L"Скачать"));
        update_action_button_.Visibility(Visibility::Visible);
        release_notes_button_.Visibility(Visibility::Visible);
        return;
    }
    const auto developer_channel = developer_updates_toggle_.IsOn();
    update_status_text_.Text(developer_channel
        ? (english_ ? L"Stable and developer updates are checked in the background."
                    : L"Стабильные и dev-обновления проверяются в фоне.")
        : (english_ ? L"Stable updates are checked in the background."
                    : L"Стабильные обновления проверяются в фоне."));
    update_action_button_.Visibility(Visibility::Collapsed);
    release_notes_button_.Visibility(Visibility::Collapsed);
}

winrt::fire_and_forget MainWindow::CheckForUpdatesAsync(bool manual, bool automatic_download) {
    if (update_check_in_flight_ || update_download_in_flight_ || exiting_) co_return;
    update_check_in_flight_ = true;
    UpdateUpdateUi();
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    const auto update_service = update_service_;
    const bool developer_channel = developer_updates_toggle_.IsOn();
    co_await winrt::resume_background();
    auto result = update_service.Check(developer_channel);
    dispatcher.TryEnqueue([weak, result = std::move(result), manual, automatic_download,
                           developer_channel]() mutable {
        if (const auto self = weak.get()) {
            self->CompleteUpdateCheck(std::move(result), manual, automatic_download, developer_channel);
        }
    });
}

void MainWindow::CompleteUpdateCheck(openreplay::ui::UpdateCheckResult result, bool manual,
                                      bool automatic_download, bool developer_channel) {
    update_check_in_flight_ = false;
    if (developer_updates_toggle_.IsOn() != developer_channel) {
        UpdateUpdateUi();
        return;
    }
    if (!result.ok) {
        UpdateUpdateUi();
        if (manual) {
            update_status_text_.Text(result.error.empty() ? (english_ ? L"Unable to check for updates"
                                                                   : L"Не удалось проверить обновления")
                                                          : result.error);
            ShowToast(english_ ? L"Update check failed" : L"Ошибка проверки обновлений",
                      update_status_text_.Text().c_str(), true, false, true, {}, NotificationChannel::Update);
        }
        return;
    }
    if (!result.update_available) {
        available_update_ = {};
        UpdateUpdateUi();
        if (manual) {
            update_status_text_.Text(developer_channel
                ? (english_ ? L"You are using the latest available version."
                            : L"Установлена последняя доступная версия.")
                : (english_ ? L"You are using the latest stable version."
                            : L"Установлена последняя стабильная версия."));
        }
        return;
    }
    available_update_ = std::move(result.manifest);
    UpdateUpdateUi();
    ShowToast(english_ ? L"OpenReplay update available" : L"Доступно обновление OpenReplay",
              english_ ? L"Version " + openreplay::FromUtf8(available_update_.version) + L" can be installed."
                       : L"Можно установить версию " + openreplay::FromUtf8(available_update_.version) + L".",
              false, false, true, {}, NotificationChannel::Update);
    if (automatic_download && settings_.automatic_updates) DownloadUpdateAsync();
}

winrt::fire_and_forget MainWindow::DownloadUpdateAsync() {
    if (update_download_in_flight_ || available_update_.version.empty() || exiting_) co_return;
    update_download_in_flight_ = true;
    UpdateUpdateUi();
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    const auto manifest = available_update_;
    const auto update_service = update_service_;
    const bool developer_channel = manifest.channel == "dev";
    co_await winrt::resume_background();
    auto result = update_service.Download(manifest, developer_channel);
    dispatcher.TryEnqueue([weak, result = std::move(result)]() mutable {
        if (const auto self = weak.get()) self->CompleteUpdateDownload(std::move(result));
    });
}

void MainWindow::CompleteUpdateDownload(openreplay::ui::UpdateDownloadResult result) {
    update_download_in_flight_ = false;
    if (!result.ok) {
        UpdateUpdateUi();
        update_status_text_.Text(result.error.empty() ? (english_ ? L"Unable to download the update"
                                                                 : L"Не удалось скачать обновление")
                                                      : result.error);
        ShowToast(english_ ? L"Update download failed" : L"Ошибка загрузки обновления",
                  update_status_text_.Text().c_str(), true, false, true, {}, NotificationChannel::Update);
        return;
    }
    downloaded_update_ = std::move(result.archive);
    UpdateUpdateUi();
    ShowToast(english_ ? L"Update ready" : L"Обновление готово",
              english_ ? L"Open Settings and choose Restart to update."
                       : L"Откройте настройки и нажмите «Перезапустить».",
              false, false, true, {}, NotificationChannel::Update);
}

void MainWindow::ApplyDownloadedUpdate() {
    if (downloaded_update_.empty() || available_update_.version.empty()) return;
    if (recording_ || replay_save_pending_ || settings_reload_in_flight_) {
        const auto message = english_ ? L"Finish recording, replay saving, and settings updates before restarting."
                                      : L"Завершите запись, сохранение повтора и применение настроек перед перезапуском.";
        update_status_text_.Text(message);
        ShowToast(english_ ? L"Update is waiting" : L"Обновление ожидает", message,
                  false, false, true, {}, NotificationChannel::Update);
        return;
    }
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return;
    const auto install_root = std::filesystem::path{std::wstring{executable.data(), length}}.parent_path();
    const auto health = openreplay::SettingsStore::DefaultPath().parent_path() / L"updates" /
                        (L"health-" + openreplay::FromUtf8(available_update_.version) + L".ok");
    std::wstring error;
    if (!update_service_.LaunchUpdater(available_update_, downloaded_update_, GetCurrentProcessId(),
                                       install_root, health, error)) {
        ShowToast(english_ ? L"Unable to start updater" : L"Не удалось запустить обновление",
                  error, true, false, true, {}, NotificationChannel::Update);
        return;
    }
    ExitApplication();
}

void MainWindow::PublishUpdateHealth() {
    if (update_health_published_ || update_health_file_.empty() || !host_connected_) return;
    std::error_code error;
    std::filesystem::create_directories(update_health_file_.parent_path(), error);
    std::ofstream health(update_health_file_, std::ios::trunc);
    health << openreplay::kVersion;
    health.close();
    if (!health) return;
    update_health_published_ = true;
    if (!post_update_version_.empty() && visible_) {
        ShowToast(english_ ? L"OpenReplay updated" : L"OpenReplay обновлён",
                  english_ ? L"Version " + post_update_version_ + L" is running."
                           : L"Запущена версия " + post_update_version_ + L".",
                  false, false, true, {}, NotificationChannel::Update);
    }
}

void MainWindow::ReconcileAudioDevices() {
    if (settings_visible_ || recording_) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_audio_device_scan_ != std::chrono::steady_clock::time_point{} &&
        now - last_audio_device_scan_ < std::chrono::seconds{5}) return;
    last_audio_device_scan_ = now;

    const auto configs = [](EDataFlow flow) {
        std::vector<openreplay::AudioDeviceConfig> result;
        for (const auto& device : AudioDevices(flow)) result.push_back({device.id, device.utf8_name, 100});
        return result;
    };
    const auto outputs = configs(eRender);
    const auto inputs = configs(eCapture);
    auto desktop = settings_.desktop_audio_devices;
    auto microphones = settings_.microphone_devices;
    bool changed = openreplay::ReconcileAudioDevices(desktop, outputs, false) |
                   openreplay::ReconcileAudioDevices(microphones, inputs, false);
    const auto unavailable = [](const auto& selected, const auto& available) {
        return std::ranges::any_of(selected, [&](const auto& device) {
            return device.id != "default" && !std::ranges::any_of(available, [&](const auto& candidate) {
                return candidate.id == device.id;
            });
        });
    };
    const bool missing = unavailable(desktop, outputs) || unavailable(microphones, inputs);
    if (missing) {
        if (audio_device_missing_since_ == std::chrono::steady_clock::time_point{}) {
            audio_device_missing_since_ = now;
            openreplay::ui::TraceStartup(L"An audio endpoint is temporarily unavailable; waiting before fallback");
            return;
        }
        if (now - audio_device_missing_since_ < std::chrono::seconds{15}) return;
        changed = openreplay::ReconcileAudioDevices(desktop, outputs, true) |
                  openreplay::ReconcileAudioDevices(microphones, inputs, true) | changed;
        openreplay::ui::TraceStartup(L"An audio endpoint remained unavailable; applying Windows default fallback");
    } else {
        audio_device_missing_since_ = {};
    }
    try {
        if (changed) {
            settings_.desktop_audio_devices = std::move(desktop);
            settings_.microphone_devices = std::move(microphones);
            settings_store_.Save(settings_);
            audio_restore_notification_pending_ = true;
            QueueHostSettingsApply(true);
            audio_device_missing_since_ = {};
        }
    } catch (...) {
    }
}

void MainWindow::ApplyResponse(const openreplay::Response& response) {
    if (!response.ok) return;
    const auto encoder = Field(response, "active_encoder");
    const auto pipeline_error = Field(response, "last_error");
    if (!pipeline_error.empty()) {
        EncoderText().Text(openreplay::FromUtf8(pipeline_error));
        if (pipeline_error != last_pipeline_error_) {
            last_pipeline_error_ = pipeline_error;
            const auto message = openreplay::FromUtf8(pipeline_error);
            ShowToast(english_ ? L"Capture pipeline error" : L"Ошибка захвата",
                      message, true, false, true);
            openreplay::ui::TraceStartup(L"Host pipeline error: " + message);
        }
    } else {
        last_pipeline_error_.clear();
        EncoderText().Text(encoder.empty() ? (english_ ? L"Capture pipeline is starting" : L"Запуск захвата")
                                           : openreplay::FromUtf8(encoder));
    }

    updating_ui_ = true;
    ReplayToggle().IsOn(FieldIsTrue(response, "replay_requested"));
    updating_ui_ = false;

    const auto state = Field(response, "replay_state");
    std::wstring state_text;
    if (state == "running") state_text = english_ ? L"Running" : L"Работает";
    else if (state == "disabled") state_text = english_ ? L"Disabled" : L"Выключен";
    else if (state == "recovering") state_text = english_ ? L"Recovering..." : L"Восстановление...";
    else if (state == "waiting_for_display") state_text = english_ ? L"Waiting for display" : L"Ожидание монитора";
    else if (state == "degraded") state_text = english_ ? L"Protected frames may be omitted" : L"Защищённые кадры могут пропускаться";
    else state_text = english_ ? L"Starting..." : L"Запуск...";
    ReplayStateText().Text(state_text);
    ReplayStateText().Foreground(!pipeline_error.empty() ? danger_brush_
                                 : state == "running"     ? success_brush_
                                                          : secondary_text_brush_);

    recording_ = FieldIsTrue(response, "recording");
    const bool audio_signal = FieldIsTrue(response, "audio_signal");
    const bool capture_ready = pipeline_error.empty() && (state == "running" || state == "disabled");
    long long recording_seconds = 0;
    try { recording_seconds = std::stoll(Field(response, "recording_seconds")); } catch (...) {}
    const auto audio_text = audio_signal ? (english_ ? L"audio OK" : L"звук есть")
                                         : (english_ ? L"no audio signal" : L"нет сигнала звука");
    const auto ready_text = (english_ ? std::wstring{L"Ready to record  •  "} : std::wstring{L"Готово к записи  •  "}) + audio_text;
    RecordingStateText().Text(recording_ ? DurationText(recording_seconds) + L"  •  " + audio_text : ready_text);
    RecordingStateText().Foreground(recording_ ? danger_brush_ : (audio_signal ? secondary_text_brush_ : danger_brush_));
    RecordingTitle().Text(recording_ ? (english_ ? L"Stop recording" : L"Остановить запись")
                                     : (english_ ? L"Start recording" : L"Начать запись"));
    UpdateBufferUi(state);
    SaveReplayButton().IsEnabled(FieldIsTrue(response, "replay_requested") && state == "running");
    RecordingButton().IsEnabled(capture_ready);
    ScreenshotButton().IsEnabled(capture_ready);
    UpdateAudioLevels(response);
    ApplyReplaySaveStatus(response);
}

void MainWindow::UpdateAudioLevels(const openreplay::Response& response) {
    const auto set_level = [](AudioLevelIndicator& level, double value) {
        const auto peak = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
        level.filled.Width(Microsoft::UI::Xaml::GridLength{
            peak, Microsoft::UI::Xaml::GridUnitType::Star});
        level.remaining.Width(Microsoft::UI::Xaml::GridLength{
            1.0 - peak, Microsoft::UI::Xaml::GridUnitType::Star});
    };
    for (auto& level : desktop_audio_device_levels_) set_level(level, 0);
    for (auto& level : microphone_device_levels_) set_level(level, 0);
    std::size_t count = 0;
    try { count = std::stoull(Field(response, "audio_level_count")); } catch (...) {}
    for (std::size_t item = 0; item < count; ++item) {
        const auto prefix = "audio_level_" + std::to_string(item);
        double peak = 0;
        try { peak = std::stod(Field(response, prefix + "_peak")); } catch (...) { continue; }
        const auto microphone = Field(response, prefix + "_kind") == "input";
        const auto& ids = microphone ? microphone_device_ids_ : desktop_audio_device_ids_;
        auto& levels = microphone ? microphone_device_levels_ : desktop_audio_device_levels_;
        const auto device = Field(response, prefix + "_device");
        const auto found = std::ranges::find(ids, device);
        if (found != ids.end()) {
            const auto index = static_cast<std::size_t>(std::distance(ids.begin(), found));
            if (index < levels.size()) set_level(levels[index], peak);
        }
    }
}

void MainWindow::UpdateStorageSpace() {
    if (!storage_space_text_) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_storage_update_ != std::chrono::steady_clock::time_point{} &&
        now - last_storage_update_ < std::chrono::seconds{5}) return;
    last_storage_update_ = now;

    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    if (!GetDiskFreeSpaceExW(settings_.output_directory.c_str(), &available, &total, nullptr)) {
        storage_space_text_.Text(english_ ? L"Free space unavailable" : L"Свободное место недоступно");
        storage_space_text_.Foreground(secondary_text_brush_);
        free_space_summary_.clear();
        UpdateDashboardMeta();
        return;
    }
    constexpr std::uint64_t warning_bytes = 5ULL * 1000ULL * 1000ULL * 1000ULL;
    const bool low = available.QuadPart < warning_bytes;
    storage_space_text_.Text((english_ ? L"Free: " : L"Свободно: ") +
                             ByteSize(available.QuadPart, english_) + (english_ ? L" of " : L" из ") +
                             ByteSize(total.QuadPart, english_) +
                             (low ? (english_ ? L"  •  Low space" : L"  •  Мало места") : L""));
    storage_space_text_.Foreground(low ? danger_brush_ : secondary_text_brush_);
    free_space_summary_ = ByteSize(available.QuadPart, english_) + (english_ ? L" free" : L" свободно");
    UpdateDashboardMeta();
    if (low && !low_space_warning_shown_) {
        low_space_warning_shown_ = true;
        ShowToast(english_ ? L"Low disk space" : L"Мало места на диске",
                  english_ ? L"Less than 5 GB remains in the recording folder"
                           : L"В папке записей осталось меньше 5 ГБ",
                  true, false, true);
    } else if (!low) {
        low_space_warning_shown_ = false;
    }
}

void MainWindow::UpdatePerformanceOverlaySummary() {
    if (!performance_overlay_summary_) return;
    const auto checked = [](const Microsoft::UI::Xaml::Controls::CheckBox& box) {
        const auto value = box.IsChecked();
        return value && value.Value();
    };
    const auto count = static_cast<unsigned int>(checked(performance_fps_) +
                                                   checked(performance_fps_lows_) +
                                                   checked(performance_frametime_) +
                                                   checked(performance_frametime_graph_) +
                                                   checked(performance_gpu_usage_) +
                                                   checked(performance_gpu_temperature_) +
                                                   checked(performance_gpu_clock_) +
                                                   checked(performance_gpu_power_) +
                                                  checked(performance_gpu_memory_) +
                                                  checked(performance_cpu_usage_) +
                                                  checked(performance_memory_));
    std::wstring unit;
    if (english_) unit = count == 1 ? L" metric" : L" metrics";
    else if (count % 10 == 1 && count % 100 != 11) unit = L" метрика";
    else if (count % 10 >= 2 && count % 10 <= 4 && (count % 100 < 12 || count % 100 > 14)) unit = L" метрики";
    else unit = L" метрик";
    performance_overlay_summary_.Text(L"Alt+R · " + std::to_wstring(count) + unit);
}

bool MainWindow::ApplyStartWithWindows(bool enabled) {
    constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t value_name[] = L"OpenReplay";
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0,
                                        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) return false;

    LSTATUS result = ERROR_SUCCESS;
    if (enabled) {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) {
            result = ERROR_FILE_NOT_FOUND;
        } else {
            const std::wstring command = L"\"" + std::wstring{executable.data(), length} + L"\" --background";
            result = RegSetValueExW(key, value_name, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(command.c_str()),
                                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        result = RegDeleteValueW(key, value_name);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void MainWindow::ApplyReplaySaveStatus(const openreplay::Response& response) {
    std::uint64_t revision = 0;
    try { revision = std::stoull(Field(response, "replay_save_revision")); } catch (...) {}
    const auto in_progress = FieldIsTrue(response, "replay_save_in_progress");

    if (!replay_save_revision_initialized_) {
        replay_save_revision_initialized_ = true;
        replay_save_revision_ = revision;
    }

    if (in_progress && !replay_save_pending_) {
        replay_save_pending_ = true;
        ShowToast(english_ ? L"Saving replay" : L"Сохраняем повтор",
                  english_ ? L"Finalizing video and audio tracks..." : L"Собираем видео и аудиодорожки...",
                  false, true, false, {}, NotificationChannel::ReplaySave);
    }

    if (revision == replay_save_revision_) return;
    replay_save_revision_ = revision;
    replay_save_pending_ = false;
    const auto game = std::exchange(replay_game_, {});
    StartPendingHostSettingsApply();

    const auto error = Field(response, "replay_save_error");
    if (!error.empty()) {
        const auto message = openreplay::FromUtf8(error);
        ShowToast(english_ ? L"Replay was not saved" : L"Повтор не сохранён", message, true, false, true,
                  {}, NotificationChannel::ReplaySave);
        openreplay::ui::TraceStartup(L"Replay save failed: " + message);
        return;
    }

    const auto output = Field(response, "replay_save_output");
    if (output.empty()) return;
    auto path = std::filesystem::path{openreplay::FromUtf8(output)};
    // OBS names every replay OpenReplay_<time>; move it into the game's folder, named after the game.
    constexpr std::wstring_view obs_prefix = L"OpenReplay";
    if (const auto original = path.filename().wstring(); !game.empty() && original.starts_with(obs_prefix)) {
        const auto folder = path.parent_path() / game;
        const auto renamed = folder / (game + original.substr(obs_prefix.size()));
        std::error_code rename_error;
        std::filesystem::create_directories(folder, rename_error);
        if (!rename_error && !std::filesystem::exists(renamed, rename_error)) {
            std::filesystem::rename(path, renamed, rename_error);
            if (!rename_error) path = renamed;
        }
    }
    const auto filename = path.filename().wstring();
    ShowToast(english_ ? L"Replay saved" : L"Повтор сохранён",
              filename.empty() ? path.wstring() : filename, false, false, true, path,
              NotificationChannel::ReplaySave, true);
    openreplay::ui::TraceStartup(L"Replay saved: " + path.wstring());
    RefreshRecentClips();
    if (settings_.discord_auto_send && discord_webhook_configured_) ShareReplay(path, true);
}

void MainWindow::ShareReplay(const std::filesystem::path& source, bool webhook) {
    if (discord_share_in_progress_) {
        if (webhook) {
            pending_discord_uploads_.push_back(source);
            ShowNotification(english_ ? L"Discord upload queued"
                                       : L"Отправка в Discord добавлена в очередь");
            return;
        }
        ShowNotification(english_ ? L"Another Discord export is already running"
                                   : L"Другая подготовка для Discord уже выполняется", true);
        return;
    }
    if (webhook && !discord_webhook_configured_) {
        ShowNotification(english_ ? L"Configure a Discord webhook in Settings first"
                                   : L"Сначала настройте Discord webhook", true);
        return;
    }
    discord_share_in_progress_ = true;
    UpdateDiscordUi();
    std::error_code size_error;
    const auto target_bytes = static_cast<std::uintmax_t>(settings_.discord_target_megabytes) * 1024U * 1024U;
    const auto source_size = std::filesystem::file_size(source, size_error);
    const bool needs_encoding = size_error || source_size > target_bytes;
    ShowToast(webhook ? (english_ ? L"Sending to Discord" : L"Отправка в Discord")
                      : (english_ ? L"Preparing Discord clip" : L"Подготовка для Discord"),
              needs_encoding
                  ? (english_ ? L"Encoding H.264/AAC under the configured size limit..."
                              : L"Кодируем H.264/AAC под заданный лимит размера...")
                  : (english_ ? L"File already fits; using the original without re-encoding..."
                              : L"Файл уже подходит; отправляем оригинал без перекодирования..."),
              false, true, false, {}, NotificationChannel::DiscordShare);
    ShareReplayAsync(source, webhook);
}

winrt::fire_and_forget MainWindow::ShareReplayAsync(std::filesystem::path source, bool webhook) {
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    const auto target_megabytes = settings_.discord_target_megabytes;
    const auto source_fps = settings_.fps;
    const auto fallback_error = english_ ? std::wstring{L"Discord export failed"}
                                         : std::wstring{L"Не удалось подготовить файл для Discord"};
    openreplay::ui::DiscordShareService service;
    std::filesystem::path output;
    openreplay::ui::DiscordUploadResult result;
    try {
        const auto file = co_await service.ExportAsync(source, target_megabytes, source_fps);
        output = std::filesystem::path{file.Path().c_str()};
        if (webhook) {
            co_await winrt::resume_background();
            std::wstring error;
            bool webhook_invalid = false;
            auto webhook_url = service.LoadWebhook(error, &webhook_invalid);
            WipeString wipe_webhook{webhook_url};
            result = webhook_url.empty()
                ? openreplay::ui::DiscordUploadResult{
                      .webhook_invalid = webhook_invalid,
                      .error = error.empty() ? L"Discord webhook is not configured" : std::move(error)}
                : service.Upload(webhook_url, output);
        } else {
            result.ok = true;
        }
    } catch (const winrt::hresult_error& error) {
        result.error = error.message();
    } catch (const std::exception& error) {
        result.error = openreplay::FromUtf8(error.what());
    } catch (...) {
        result.error = fallback_error;
    }
    dispatcher.TryEnqueue([weak, output = std::move(output), webhook, result = std::move(result)]() mutable {
        if (const auto self = weak.get()) {
            self->CompleteReplayShare(std::move(output), webhook, std::move(result));
        }
    });
}

void MainWindow::CompleteReplayShare(std::filesystem::path output, bool webhook,
                                     openreplay::ui::DiscordUploadResult result) {
    discord_share_in_progress_ = false;
    if (result.webhook_invalid) ForgetInvalidDiscordWebhook();
    UpdateDiscordUi();
    if (!result.ok) {
        const auto message = result.error.empty()
            ? (english_ ? L"Discord operation failed" : L"Операция Discord не выполнена")
            : result.error;
        ShowToast(webhook ? (english_ ? L"Discord upload failed" : L"Ошибка отправки в Discord")
                          : (english_ ? L"Discord export failed" : L"Ошибка подготовки для Discord"),
                  message, true, false, true, output, NotificationChannel::DiscordShare);
    } else if (webhook) {
        ShowToast(english_ ? L"Sent to Discord" : L"Отправлено в Discord",
                  output.filename().wstring(), false, false, true, output,
                  NotificationChannel::DiscordShare);
    } else if (!openreplay::ui::DiscordShareService::CopyFileToClipboard(GetWindowHandle(), output)) {
        ShowToast(english_ ? L"Discord clip is ready" : L"Клип для Discord готов",
                  english_ ? L"Unable to copy the file; open its folder instead"
                           : L"Не удалось скопировать файл; откройте его папку",
                  true, false, true, output, NotificationChannel::DiscordShare);
    } else {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(output, size_error);
        const bool compressed = output.parent_path().filename() == L"Discord" && !size_error;
        ShowToast(compressed ? (english_ ? L"Compressed to " : L"Сжато до ") + ByteSize(size, english_) +
                                   (english_ ? L" and copied" : L" и скопировано")
                             : (english_ ? L"Copied" : L"Скопировано"),
                  english_ ? L"Paste into Discord with Ctrl+V" : L"Вставьте в Discord — Ctrl+V",
                  false, false, true, {}, NotificationChannel::DiscordShare);
    }
    if (result.ok && clip_library_window_owner_) clip_library_window_owner_->ShowSharedCopy(output);
    if (discord_webhook_configured_ && !pending_discord_uploads_.empty()) {
        auto next = std::move(pending_discord_uploads_.front());
        pending_discord_uploads_.pop_front();
        ShareReplay(next, true);
    }
}

void MainWindow::UpdateDiscordUi() {
    if (!discord_status_text_ || !discord_webhook_input_ || !discord_auto_send_toggle_) return;
    const bool has_draft = !discord_webhook_input_.Password().empty();
    discord_save_button_.IsEnabled(has_draft && !discord_test_in_progress_ && !discord_share_in_progress_);
    discord_test_button_.IsEnabled(discord_webhook_configured_ && !discord_test_in_progress_ &&
                                   !discord_share_in_progress_);
    discord_clear_button_.IsEnabled(discord_webhook_configured_ && !discord_test_in_progress_ &&
                                    !discord_share_in_progress_);
    discord_auto_send_toggle_.IsEnabled(discord_webhook_configured_ && !discord_share_in_progress_);
    if (!discord_webhook_configured_ && discord_auto_send_toggle_.IsOn()) {
        updating_ui_ = true;
        discord_auto_send_toggle_.IsOn(false);
        updating_ui_ = false;
    }
    if (discord_test_in_progress_) {
        discord_status_text_.Text(english_ ? L"Sending a test message..." : L"Отправка тестового сообщения...");
    } else if (discord_share_in_progress_) {
        discord_status_text_.Text(english_ ? L"Preparing or uploading a replay..."
                                           : L"Подготовка или отправка повтора...");
    } else if (has_draft) {
        discord_status_text_.Text(english_ ? L"Webhook URL is ready to save securely"
                                           : L"Webhook URL готов к безопасному сохранению");
    } else if (!discord_credential_error_.empty()) {
        discord_status_text_.Text(english_ ? L"Windows Credential Manager is unavailable"
                                           : L"Диспетчер учётных данных Windows недоступен");
    } else if (discord_webhook_configured_) {
        discord_status_text_.Text(english_ ? L"Webhook is stored in Windows Credential Manager"
                                           : L"Webhook хранится в диспетчере учётных данных Windows");
    } else {
        discord_status_text_.Text(english_ ? L"Webhook is not configured"
                                           : L"Webhook не настроен");
    }
}

void MainWindow::SaveDiscordWebhook_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    std::wstring value{discord_webhook_input_.Password()};
    WipeString wipe_value{value};
    std::wstring error;
    if (value.empty() || !discord_share_service_.StoreWebhook(value, error)) {
        ShowNotification(error.empty() ? (english_ ? L"Enter a Discord webhook URL"
                                                    : L"Введите Discord webhook URL")
                                       : error,
                         true);
        return;
    }
    discord_webhook_input_.Password(L"");
    discord_webhook_configured_ = true;
    discord_credential_error_.clear();
    UpdateDiscordUi();
    ShowNotification(english_ ? L"Discord webhook saved securely"
                               : L"Discord webhook безопасно сохранён");
}

void MainWindow::ClearDiscordWebhook_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    std::wstring error;
    if (!discord_share_service_.RemoveWebhook(error)) {
        ShowNotification(error.empty() ? (english_ ? L"Unable to remove the Discord webhook"
                                                    : L"Не удалось удалить Discord webhook")
                                       : error,
                         true);
        return;
    }
    discord_webhook_configured_ = false;
    discord_credential_error_.clear();
    pending_discord_uploads_.clear();
    discord_auto_send_toggle_.IsOn(false);
    ScheduleSettingsApply();
    UpdateDiscordUi();
    ShowNotification(english_ ? L"Discord webhook removed" : L"Discord webhook удалён");
}

void MainWindow::TestDiscordWebhook_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!discord_webhook_configured_ || discord_test_in_progress_) return;
    discord_test_in_progress_ = true;
    UpdateDiscordUi();
    TestDiscordWebhookAsync();
}

winrt::fire_and_forget MainWindow::TestDiscordWebhookAsync() {
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    openreplay::ui::DiscordShareService service;
    co_await winrt::resume_background();
    openreplay::ui::DiscordUploadResult result;
    try {
        std::wstring error;
        bool webhook_invalid = false;
        auto webhook = service.LoadWebhook(error, &webhook_invalid);
        WipeString wipe_webhook{webhook};
        result = webhook.empty()
            ? openreplay::ui::DiscordUploadResult{
                  .webhook_invalid = webhook_invalid,
                  .error = error.empty() ? L"Discord webhook is not configured" : std::move(error)}
            : service.Test(webhook);
    } catch (const winrt::hresult_error& error) {
        result.error = error.message();
    } catch (const std::exception& error) {
        result.error = openreplay::FromUtf8(error.what());
    } catch (...) {
        result.error = L"Discord webhook test failed";
    }
    dispatcher.TryEnqueue([weak, result = std::move(result)]() mutable {
        if (const auto self = weak.get()) self->CompleteDiscordWebhookTest(std::move(result));
    });
}

void MainWindow::CompleteDiscordWebhookTest(openreplay::ui::DiscordUploadResult result) {
    discord_test_in_progress_ = false;
    if (result.webhook_invalid) ForgetInvalidDiscordWebhook();
    UpdateDiscordUi();
    ShowToast(result.ok ? (english_ ? L"Discord webhook works" : L"Discord webhook работает")
                        : (english_ ? L"Discord webhook failed" : L"Ошибка Discord webhook"),
              result.ok ? (english_ ? L"Test message sent" : L"Тестовое сообщение отправлено")
                        : result.error,
              !result.ok, false, true, {}, NotificationChannel::DiscordShare);
}

void MainWindow::ForgetInvalidDiscordWebhook() {
    std::wstring error;
    if (!discord_share_service_.RemoveWebhook(error)) discord_credential_error_ = std::move(error);
    else discord_credential_error_.clear();
    discord_webhook_configured_ = false;
    settings_.discord_auto_send = false;
    pending_discord_uploads_.clear();
    updating_ui_ = true;
    discord_auto_send_toggle_.IsOn(false);
    updating_ui_ = false;
    try {
        settings_store_.Save(settings_);
    } catch (...) {
    }
}

bool MainWindow::RunCommand(std::string_view command, std::wstring_view success_message) {
    const auto response = pipe_client_.Request(command, std::chrono::milliseconds{500});
    if (!response.ok) {
        const auto message = Field(response, "message");
        const auto detail = message.empty() ? (english_ ? std::wstring{L"Command failed"}
                                                       : std::wstring{L"Команда не выполнена"})
                                            : openreplay::FromUtf8(message);
        ShowNotification(detail, true);
        openreplay::ui::TraceStartup(L"Command failed: " + detail);
        return false;
    }
    ApplyResponse(response);
    if (!success_message.empty()) ShowNotification(success_message);
    return true;
}

void MainWindow::ShowNotification(std::wstring_view message, bool error) {
    ShowToast(error ? (english_ ? L"Something went wrong" : L"Что-то пошло не так")
                    : (english_ ? L"Ready" : L"Готово"),
              message, error, false, true);
}

void MainWindow::ShowToast(std::wstring_view title, std::wstring_view message, bool error, bool progress,
                            bool auto_hide, const std::filesystem::path& output, NotificationChannel channel,
                            bool shareable) {
    ShowDesktopNotification(title, message, error, progress, auto_hide, output, channel, shareable);
}

void MainWindow::InitializeNotificationWindow() {
    if (notification_class_registered_) return;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = NotificationWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"OpenReplay.Notification";
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    notification_class_registered_ = true;
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length && length < executable.size()) {
        const auto font = std::filesystem::path{std::wstring{executable.data(), length}}.parent_path() /
                          L"Assets" / L"PublicSans-Variable.ttf";
        private_font_loaded_ = AddFontResourceExW(font.c_str(), FR_PRIVATE, nullptr) != 0;
    }
}

void MainWindow::ShowDesktopNotification(std::wstring_view title, std::wstring_view message, bool error,
                                           bool progress, bool auto_hide, const std::filesystem::path& output,
                                           NotificationChannel channel, bool shareable) {
    InitializeNotificationWindow();
    if (!notification_class_registered_) return;

    DesktopNotification* notification = nullptr;
    if (channel != NotificationChannel::General) {
        const auto existing = std::ranges::find_if(notifications_, [channel](const auto& item) {
            return item->channel == channel && item->progress && !item->hiding;
        });
        if (existing != notifications_.end()) notification = existing->get();
    }

    if (!notification) {
        auto item = std::make_unique<DesktopNotification>();
        item->owner = this;
        item->channel = channel;
        item->window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"OpenReplay.Notification", L"OpenReplay",
            WS_POPUP, 0, 0, kNotificationWidth, 80, nullptr, nullptr, GetModuleHandleW(nullptr), item.get());
        if (!item->window) return;
        // Windows 11 draws smooth corners and the border; older systems fall back to a window region.
        constexpr DWORD kCornerAttribute = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
        constexpr DWORD kBorderAttribute = 34;  // DWMWA_BORDER_COLOR
        constexpr DWORD round_corners = 2;      // DWMWCP_ROUND
        item->rounded_by_dwm = SUCCEEDED(DwmSetWindowAttribute(item->window, kCornerAttribute, &round_corners,
                                                               sizeof(round_corners)));
        if (item->rounded_by_dwm) {
            constexpr COLORREF border = RGB(46, 46, 51);
            DwmSetWindowAttribute(item->window, kBorderAttribute, &border, sizeof(border));
        }
        notification = item.get();
        notifications_.insert(notifications_.begin(), std::move(item));
    }

    KillTimer(notification->window, kNotificationAnimationTimer);
    KillTimer(notification->window, kNotificationHideTimer);
    if (notification->animating) {
        notification->animating = false;
        notification->entering = !IsWindowVisible(notification->window);
    }
    notification->title = title;
    notification->message = message;
    notification->output = output;
    notification->error = error;
    notification->progress = progress;
    notification->shareable = shareable;
    notification->display_ms = auto_hide
        ? (error ? 6500U : (progress ? 10000U : (shareable ? 12000U : 4500U))) : 0U;
    notification->hiding = false;
    notification->hide_timer_started = false;
    notification->width = 0;
    notification->height = 0;
    InvalidateRect(notification->window, nullptr, TRUE);
    LayoutNotifications();
    UpdateWindow(notification->window);
    if (!notification->animating && notification->display_ms) {
        SetTimer(notification->window, kNotificationHideTimer, notification->display_ms, nullptr);
    }
}

void MainWindow::LayoutNotifications() {
    if (notifications_.empty()) return;
    const auto area = openreplay::ui::ForegroundMonitorArea(GetWindowHandle());
    if (!area.monitor) return;
    const auto dpi = openreplay::ui::MonitorDpi(area.monitor);
    const auto margin = openreplay::ui::ScaleForDpi(18, dpi);
    const auto gap = openreplay::ui::ScaleForDpi(10, dpi);
    auto y = area.bounds.top + openreplay::ui::ScaleForDpi(24, dpi);
    auto available_bottom = area.bounds.bottom - margin;
    if (const auto performance_bounds = performance_overlay_.Bounds(); performance_bounds &&
        MonitorFromRect(&*performance_bounds, MONITOR_DEFAULTTONULL) == area.monitor) {
        if (performance_bounds->top <= y + openreplay::ui::ScaleForDpi(12, dpi)) {
            y = performance_bounds->bottom + openreplay::ui::ScaleForDpi(12, dpi);
        } else {
            available_bottom = std::min(available_bottom, performance_bounds->top - gap);
        }
    }

    std::vector<DesktopNotification*> overflow;
    for (const auto& item : notifications_) {
        auto& notification = *item;
        if (!notification.window || notification.hiding) continue;
        const auto width = openreplay::ui::ScaleForDpi(kNotificationWidth, dpi);
        auto height = openreplay::ui::ScaleForDpi(80, dpi);
        if (const auto dc = GetDC(notification.window)) {
            const NotificationFonts fonts{dpi};
            height = MeasureNotification(dc, dpi, fonts, notification.message,
                                         NotificationActionsFor(!notification.output.empty(), notification.shareable,
                                                                discord_webhook_configured_, english_)).height;
            ReleaseDC(notification.window, dc);
        }
        if (notification.dpi != dpi || notification.height != height) {
            InvalidateRect(notification.window, nullptr, FALSE);
        }
        notification.dpi = dpi;
        notification.width = width;
        notification.height = height;
        notification.hidden_x = area.bounds.right + openreplay::ui::ScaleForDpi(8, dpi);
        if (y + height > available_bottom) {
            overflow.push_back(&notification);
            continue;
        }
        if (!notification.rounded_by_dwm) {
            if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                                       openreplay::ui::ScaleForDpi(20, dpi),
                                                       openreplay::ui::ScaleForDpi(20, dpi))) {
                if (!SetWindowRgn(notification.window, region, TRUE)) DeleteObject(region);
            }
        }
        StartNotificationAnimation(notification, area.bounds.right - width - margin, y, false, 160);
        y += height + gap;
    }
    for (auto* notification : overflow) DismissNotification(*notification);
}

void MainWindow::StartNotificationAnimation(DesktopNotification& notification, int target_x, int target_y,
                                            bool hiding, UINT duration_ms) {
    if (!notification.window) return;
    RECT current{};
    GetWindowRect(notification.window, &current);
    const auto size_matches = current.right - current.left == notification.width &&
                              current.bottom - current.top == notification.height;
    if (notification.animating && notification.target_x == target_x &&
        notification.target_y == target_y && notification.hiding == hiding && size_matches) return;
    if (!notification.animating && !notification.entering && !hiding &&
        notification.target_x == target_x && notification.target_y == target_y && size_matches) return;

    if (notification.entering || !IsWindowVisible(notification.window)) {
        notification.start_x = notification.hidden_x;
        notification.start_y = target_y;
        SetWindowPos(notification.window, HWND_TOPMOST, notification.start_x, notification.start_y,
                     notification.width, notification.height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        notification.start_x = current.left;
        notification.start_y = current.top;
        SetWindowPos(notification.window, HWND_TOPMOST, current.left, current.top,
                     notification.width, notification.height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    notification.target_x = target_x;
    notification.target_y = target_y;
    notification.hiding = hiding;
    notification.animating = true;
    notification.animation_duration_ms = duration_ms;
    notification.animation_started = std::chrono::steady_clock::now();
    SetTimer(notification.window, kNotificationAnimationTimer, 15, nullptr);
}

void MainWindow::DismissNotification(DesktopNotification& notification) {
    if (!notification.window || notification.hiding) return;
    KillTimer(notification.window, kNotificationHideTimer);
    StartNotificationAnimation(notification, notification.hidden_x, notification.target_y, true, 140);
}

void MainWindow::DismissNotificationChannel(NotificationChannel channel) {
    for (const auto& notification : notifications_) {
        if (notification->channel == channel) DismissNotification(*notification);
    }
}

void MainWindow::RemoveNotification(HWND window) {
    const auto found = std::ranges::find_if(notifications_, [window](const auto& notification) {
        return notification->window == window;
    });
    if (found != notifications_.end()) notifications_.erase(found);
    LayoutNotifications();
}

void MainWindow::OpenNotificationOutput(const DesktopNotification& notification) {
    if (notification.output.empty()) return;
    std::error_code error;
    const auto target = std::filesystem::is_directory(notification.output, error)
                            ? notification.output : notification.output.parent_path();
    ShellExecuteW(GetWindowHandle(), L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::CopyNotificationOutput(const DesktopNotification& notification) {
    if (notification.output.empty() || !OpenClipboard(notification.window)) return;
    EmptyClipboard();
    const auto text = notification.output.wstring();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        if (auto* destination = static_cast<wchar_t*>(GlobalLock(memory))) {
            memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void MainWindow::ShareNotificationOutput(const DesktopNotification& notification, bool webhook) {
    if (notification.output.empty()) return;
    ShareReplay(notification.output, webhook);
}

void MainWindow::TogglePerformanceOverlay() {
    performance_overlay_.Toggle(settings_, english_);
    LayoutNotifications();
}

LRESULT CALLBACK MainWindow::NotificationWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* notification = reinterpret_cast<DesktopNotification*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        notification = static_cast<DesktopNotification*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(notification));
    }
    if (!notification || !notification->owner) return DefWindowProcW(window, message, wparam, lparam);
    auto* self = notification->owner;
    if (message == WM_TIMER && wparam == kNotificationHideTimer) {
        KillTimer(window, kNotificationHideTimer);
        notification->hide_timer_started = false;
        self->DismissNotification(*notification);
        return 0;
    }
    if (message == WM_TIMER && wparam == kNotificationAnimationTimer) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - notification->animation_started).count();
        const auto progress = std::clamp(static_cast<double>(elapsed) /
                                             static_cast<double>(notification->animation_duration_ms),
                                         0.0, 1.0);
        const auto eased = notification->hiding
                               ? progress * progress
                               : 1.0 - std::pow(1.0 - progress, 3.0);
        const auto x = notification->start_x + static_cast<int>(std::lround(
            (notification->target_x - notification->start_x) * eased));
        const auto y = notification->start_y + static_cast<int>(std::lround(
            (notification->target_y - notification->start_y) * eased));
        SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0,
                      SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (progress >= 1.0) {
            KillTimer(window, kNotificationAnimationTimer);
            notification->animating = false;
            notification->entering = false;
            if (notification->hiding) {
                DestroyWindow(window);
                self->RemoveNotification(window);
            } else if (notification->display_ms && !notification->hide_timer_started) {
                SetTimer(window, kNotificationHideTimer, notification->display_ms, nullptr);
                notification->hide_timer_started = true;
            }
        }
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    const auto dpi = notification->dpi ? notification->dpi : openreplay::ui::WindowDpi(window);
    const auto actions = NotificationActionsFor(!notification->output.empty(), notification->shareable,
                                                self->discord_webhook_configured_, self->english_);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (!dc) return 0;
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto scale = [dpi](int value) { return openreplay::ui::ScaleForDpi(value, dpi); };
        const auto fill = [dc](const RECT& rect, COLORREF color) {
            if (const auto brush = CreateSolidBrush(color)) {
                FillRect(dc, &rect, brush);
                DeleteObject(brush);
            }
        };
        const NotificationFonts fonts{dpi};
        const auto layout = MeasureNotification(dc, dpi, fonts, notification->message, actions);
        fill(bounds, RGB(23, 23, 26));
        if (!notification->rounded_by_dwm) {
            const auto pen = CreatePen(PS_SOLID, 1, RGB(46, 46, 51));
            const auto old_pen = SelectObject(dc, pen ? pen : GetStockObject(NULL_PEN));
            const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, 0, 0, bounds.right, bounds.bottom, scale(20), scale(20));
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            if (pen) DeleteObject(pen);
        }
        fill(layout.marker, notification->error ? RGB(255, 107, 110)
                            : notification->progress ? RGB(224, 179, 65)
                                                     : RGB(118, 185, 0));
        SetBkMode(dc, TRANSPARENT);
        const auto old_font = SelectObject(dc, fonts.title ? fonts.title : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(237, 237, 237));
        auto title_rect = layout.title;
        DrawTextW(dc, notification->title.c_str(), -1, &title_rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!notification->message.empty()) {
            SelectObject(dc, fonts.body ? fonts.body : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, RGB(161, 161, 170));
            auto message_rect = layout.message;
            DrawTextW(dc, notification->message.c_str(), -1, &message_rect,
                      DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, fonts.action ? fonts.action : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(139, 212, 0));
        for (std::size_t index = 0; index < layout.actions.size(); ++index) {
            auto rect = layout.actions[index].second;
            DrawTextW(dc, actions[index].second.c_str(), -1, &rect, DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, old_font);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        std::optional<NotificationAction> clicked;
        if (const auto dc = GetDC(window)) {
            const NotificationFonts fonts{dpi};
            const auto layout = MeasureNotification(dc, dpi, fonts, notification->message, actions);
            ReleaseDC(window, dc);
            for (const auto& [action, rect] : layout.actions) {
                if (PtInRect(&rect, point)) clicked = action;
            }
        }
        if (!clicked) {
            if (notification->output.empty()) self->ToggleOverlay();
        } else if (*clicked == NotificationAction::Copy) {
            self->ShareNotificationOutput(*notification, false);
        } else if (*clicked == NotificationAction::Webhook) {
            self->ShareNotificationOutput(*notification, true);
        } else if (*clicked == NotificationAction::OpenFolder) {
            self->OpenNotificationOutput(*notification);
        } else {
            self->CopyNotificationOutput(*notification);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void MainWindow::Root_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    if (args.Key() == Windows::System::VirtualKey::Escape) {
        if (settings_visible_) ShowSettings(false);
        else HideOverlay();
        args.Handled(true);
    }
}

void MainWindow::SettingsButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { ShowSettings(true); }
void MainWindow::GalleryButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ShowClipLibrary();
}
void MainWindow::CloseButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { HideOverlay(); }
void MainWindow::BackButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) { ShowSettings(false); }

void MainWindow::SaveReplay_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    openreplay::ui::TraceStartup(L"Replay save requested from overlay");
    RequestReplaySave(settings_.replay_seconds);
}

void MainWindow::RecordingButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ToggleRecording();
}

void MainWindow::ToggleRecording() {
    const bool was_recording = recording_;
    const auto response = pipe_client_.Request("toggle_recording", std::chrono::milliseconds{500});
    if (!response.ok) {
        const auto error = Field(response, "message");
        ShowNotification(error.empty() ? (english_ ? L"Recording command failed" : L"Команда записи не выполнена")
                                       : openreplay::FromUtf8(error), true);
        return;
    }
    ApplyResponse(response);
    const auto output = Field(response, "last_output");
    const auto path = output.empty() ? std::filesystem::path{} : std::filesystem::path{openreplay::FromUtf8(output)};
    ShowToast(was_recording ? (english_ ? L"Recording saved" : L"Запись сохранена")
                            : (english_ ? L"Recording started" : L"Запись начата"),
              was_recording && !path.empty() ? path.filename().wstring()
                                             : (english_ ? L"Capture is running" : L"Захват запущен"),
              false, false, true, was_recording ? path : std::filesystem::path{});
}

void MainWindow::ScreenshotButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    TakeScreenshot();
}

void MainWindow::TakeScreenshot() {
    HideOverlay();
    TakeScreenshotAsync();
}

winrt::fire_and_forget MainWindow::TakeScreenshotAsync() {
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    co_await winrt::resume_after(std::chrono::milliseconds{80});
    openreplay::Response response;
    try {
        response = openreplay::PipeClient{}.Request("screenshot", std::chrono::seconds{10});
    } catch (const std::exception& error) {
        response = {false, {{"message", error.what()}}};
    } catch (...) {
        response = {false, {{"message", "Unable to capture screenshot"}}};
    }
    dispatcher.TryEnqueue([weak, response = std::move(response)]() mutable {
        if (const auto self = weak.get()) self->CompleteScreenshot(std::move(response));
    });
}

void MainWindow::CompleteScreenshot(openreplay::Response response) {
    if (!response.ok) {
        const auto error = Field(response, "message");
        ShowToast(english_ ? L"Screenshot failed" : L"Снимок не сохранён",
                  error.empty() ? (english_ ? L"Capture command failed" : L"Команда захвата не выполнена")
                                : openreplay::FromUtf8(error),
                  true, false, true);
        return;
    }
    const auto output = Field(response, "last_output");
    const auto path = output.empty() ? std::filesystem::path{} : std::filesystem::path{openreplay::FromUtf8(output)};
    ShowToast(english_ ? L"Screenshot saved" : L"Снимок сохранён",
              path.empty() ? (english_ ? L"PNG image saved" : L"PNG сохранён") : path.filename().wstring(),
              false, false, true, path);
}

void MainWindow::ReplayToggle_Toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (updating_ui_) return;
    const auto enabled = ReplayToggle().IsOn();
    if (settings_reload_in_flight_) {
        const auto previous = settings_.instant_replay_enabled;
        settings_.instant_replay_enabled = enabled;
        try {
            settings_store_.Save(settings_);
            replay_toggle_desired_ = enabled;
            replay_toggle_apply_pending_ = true;
            UpdateSettingsSaveStatus(english_ ? L"Saved · replay state is queued"
                                               : L"Сохранено · состояние повтора в очереди");
        } catch (...) {
            settings_.instant_replay_enabled = previous;
            updating_ui_ = true;
            ReplayToggle().IsOn(previous);
            updating_ui_ = false;
            ShowNotification(english_ ? L"Unable to save instant replay state"
                                      : L"Не удалось сохранить состояние мгновенного повтора", true);
        }
        return;
    }
    if (RunCommand(enabled ? "replay:on" : "replay:off")) {
        settings_.instant_replay_enabled = enabled;
        host_settings_.instant_replay_enabled = enabled;
    } else {
        updating_ui_ = true;
        ReplayToggle().IsOn(!enabled);
        updating_ui_ = false;
    }
}

void MainWindow::ReplayDurationSlider_ValueChanged(
    IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
    const auto seconds = static_cast<uint32_t>(std::lround(args.NewValue()));
    ReplayDurationValue().Text(std::to_wstring(seconds) + (english_ ? L" sec" : L" сек"));
    UpdateReplaySizeEstimate();
    ScheduleSettingsApply();
}

void MainWindow::BitrateSlider_ValueChanged(
    IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
    const auto megabits = static_cast<uint32_t>(std::lround(args.NewValue()));
    BitrateValue().Text(std::to_wstring(megabits) + (english_ ? L" Mbps" : L" Мбит/с"));
    UpdateReplaySizeEstimate();
    ScheduleSettingsApply();
}

void MainWindow::QualitySelector_SelectionChanged(
    IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    UpdateQualityDescription();
    ScheduleSettingsApply();
}

void MainWindow::OutputFormatSelector_SelectionChanged(
    IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    UpdateOutputFormatDescription();
    UpdateReplaySizeEstimate();
    ScheduleSettingsApply();
}

void MainWindow::EstimateInput_Toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    UpdateReplaySizeEstimate();
    ScheduleSettingsApply();
}

void MainWindow::ReplayHotkey_KeyDown(
    IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    const auto chord = HotkeyText(static_cast<UINT>(args.Key()));
    if (chord.empty()) return;
    sender.as<Microsoft::UI::Xaml::Controls::TextBox>().Text(openreplay::FromUtf8(chord));
    ValidateHotkeys(true, false);
    ScheduleSettingsApply();
    args.Handled(true);
}

void MainWindow::ScreenshotHotkey_KeyDown(
    IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    const auto chord = HotkeyText(static_cast<UINT>(args.Key()));
    if (chord.empty()) return;
    sender.as<Microsoft::UI::Xaml::Controls::TextBox>().Text(openreplay::FromUtf8(chord));
    ValidateHotkeys(true, false);
    ScheduleSettingsApply();
    args.Handled(true);
}

void MainWindow::RecordingHotkey_KeyDown(
    IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
    const auto chord = HotkeyText(static_cast<UINT>(args.Key()));
    if (chord.empty()) return;
    sender.as<Microsoft::UI::Xaml::Controls::TextBox>().Text(openreplay::FromUtf8(chord));
    ValidateHotkeys(true, false);
    ScheduleSettingsApply();
    args.Handled(true);
}

void MainWindow::ReplayHotkeyAdd_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    replay_hotkey_draft_ = ReplayHotkeysFromControls();
    if (replay_hotkey_draft_.size() >= 8) return;
    replay_hotkey_draft_.push_back({false, {}, 60});
    RebuildReplayHotkeyRows();
    ScheduleSettingsApply();
}

void MainWindow::ReplayHotkeyRemove_Click(
    IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (replay_hotkey_remove_buttons_.size() <= 1) return;
    const auto button = sender.as<Microsoft::UI::Xaml::Controls::Button>();
    const auto found = std::ranges::find(replay_hotkey_remove_buttons_, button);
    if (found == replay_hotkey_remove_buttons_.end()) return;
    const auto index = static_cast<std::size_t>(std::distance(replay_hotkey_remove_buttons_.begin(), found));
    replay_hotkey_draft_ = ReplayHotkeysFromControls();
    replay_hotkey_draft_.erase(replay_hotkey_draft_.begin() + index);
    RebuildReplayHotkeyRows();
    ScheduleSettingsApply();
}

void MainWindow::ScheduleSettingsApply() {
    if (!settings_initialized_ || updating_ui_ || !settings_apply_timer_) return;
    settings_apply_pending_ = true;
    settings_apply_timer_.Stop();
    settings_apply_timer_.Start();
    if (settings_save_status_) {
        settings_save_status_.Text(english_ ? L"Saving changes..." : L"Сохранение изменений...");
        settings_save_status_.Foreground(secondary_text_brush_);
    }
}

bool MainWindow::ReadSettingsFromControls(openreplay::Settings& draft, bool show_errors) {
    draft = settings_;
    draft.replay_seconds = static_cast<uint32_t>(std::lround(ReplayDurationSlider().Value()));
    draft.bitrate_kbps = static_cast<uint32_t>(std::lround(BitrateSlider().Value())) * 1000U;
    draft.fps = FpsSelector().SelectedIndex() == 0 ? 30U : 60U;
    draft.codec = static_cast<openreplay::VideoCodec>(CodecSelector().SelectedIndex());
    draft.quality_preset = static_cast<openreplay::QualityPreset>(QualitySelector().SelectedIndex());
    draft.output_format = OutputFormatSelector().SelectedIndex() == 1 ? openreplay::OutputFormat::Mp4
                                                                      : openreplay::OutputFormat::Mkv;
    draft.performance_overlay_position = performance_position_selector_.SelectedIndex() == 1
        ? openreplay::PerformanceOverlayPosition::BottomRight
        : openreplay::PerformanceOverlayPosition::TopRight;
    draft.performance_frametime_line = performance_graph_style_selector_.SelectedIndex() == 1;
    draft.performance_overlay_opacity =
        static_cast<std::uint32_t>(std::lround(performance_opacity_slider_.Value()));
    const auto checked = [](const Microsoft::UI::Xaml::Controls::CheckBox& box) {
        const auto value = box.IsChecked();
        return value && value.Value();
    };
    draft.performance_show_fps = checked(performance_fps_);
    draft.performance_show_fps_lows = checked(performance_fps_lows_);
    draft.performance_show_frametime = checked(performance_frametime_);
    draft.performance_show_frametime_graph = checked(performance_frametime_graph_);
    draft.performance_show_gpu_usage = checked(performance_gpu_usage_);
    draft.performance_show_gpu_temperature = checked(performance_gpu_temperature_);
    draft.performance_show_gpu_clock = checked(performance_gpu_clock_);
    draft.performance_show_gpu_power = checked(performance_gpu_power_);
    draft.performance_show_gpu_memory = checked(performance_gpu_memory_);
    draft.performance_show_cpu_usage = checked(performance_cpu_usage_);
    draft.performance_show_memory = checked(performance_memory_);
    if (!draft.performance_show_fps && !draft.performance_show_fps_lows &&
        !draft.performance_show_frametime && !draft.performance_show_frametime_graph &&
        !draft.performance_show_gpu_usage && !draft.performance_show_gpu_temperature &&
        !draft.performance_show_gpu_clock && !draft.performance_show_gpu_power && !draft.performance_show_gpu_memory &&
        !draft.performance_show_cpu_usage && !draft.performance_show_memory) {
        updating_ui_ = true;
        performance_fps_.IsChecked(true);
        updating_ui_ = false;
        draft.performance_show_fps = true;
        UpdatePerformanceOverlaySummary();
    }
    draft.microphone_enabled = MicrophoneToggle().IsOn();
    draft.capture_cursor = CursorToggle().IsOn();
    draft.start_with_windows = start_with_windows_toggle_.IsOn();
    draft.automatic_updates = automatic_updates_toggle_.IsOn();
    draft.developer_updates = developer_updates_toggle_.IsOn();
    draft.discord_auto_send = discord_webhook_configured_ && discord_auto_send_toggle_.IsOn();
    const auto discord_size = discord_size_input_.Value();
    if (!std::isnan(discord_size)) {
        draft.discord_target_megabytes = static_cast<std::uint32_t>(std::lround(discord_size));
    }
    draft.language = LanguageSelector().SelectedIndex() == 0 ? "ru-RU" : "en-US";
    const auto monitor_index = MonitorSelector().SelectedIndex();
    if (monitor_index >= 0 && static_cast<size_t>(monitor_index) < monitor_ids_.size()) {
        draft.monitor_id = monitor_ids_[static_cast<size_t>(monitor_index)];
    }
    const auto selected_devices = [](const std::vector<std::string>& ids, const std::vector<std::string>& names,
                                     const std::vector<CheckBox>& checks,
                                     const std::vector<Microsoft::UI::Xaml::Controls::Slider>& volumes) {
        std::vector<openreplay::AudioDeviceConfig> result;
        for (std::size_t index = 0; index < ids.size() && index < checks.size() &&
                                    index < names.size() && index < volumes.size(); ++index) {
            const auto selected = checks[index].IsChecked();
            if (selected && selected.Value()) {
                result.push_back({ids[index], names[index],
                                  static_cast<std::uint32_t>(std::lround(volumes[index].Value()))});
            }
        }
        if (result.size() > 1) {
            std::erase_if(result, [](const auto& device) { return device.id == "default"; });
        }
        return result;
    };
    draft.desktop_audio_devices = selected_devices(
        desktop_audio_device_ids_, desktop_audio_device_names_, desktop_audio_device_checks_,
        desktop_audio_device_volumes_);
    draft.microphone_devices = selected_devices(
        microphone_device_ids_, microphone_device_names_, microphone_device_checks_,
        microphone_device_volumes_);
    if (draft.desktop_audio_devices.empty() ||
        (draft.microphone_enabled && draft.microphone_devices.empty())) {
        UpdateSettingsSaveStatus(english_ ? L"Select an output and an input while the microphone is enabled"
                                           : L"Выберите выход и вход при включённом микрофоне", true);
        if (show_errors) {
            ShowNotification(english_ ? L"Select at least one desktop output and one input when the microphone is enabled"
                                      : L"Выберите хотя бы один системный выход и один вход при включённом микрофоне", true);
        }
        return false;
    }
    const auto active_audio_sources = draft.desktop_audio_devices.size() +
                                      (draft.microphone_enabled ? draft.microphone_devices.size() : 0U);
    if (active_audio_sources > 5) {
        UpdateSettingsSaveStatus(english_ ? L"Select no more than 5 active audio devices"
                                           : L"Выберите не больше 5 активных аудиоустройств", true);
        if (show_errors) {
            ShowNotification(english_ ? L"Select no more than 5 active audio devices in total"
                                      : L"Выберите суммарно не больше 5 активных аудиоустройств", true);
        }
        return false;
    }
    if (!ValidateHotkeys(true, show_errors)) {
        UpdateSettingsSaveStatus(english_ ? L"Fix the highlighted shortcuts"
                                           : L"Исправьте подсвеченные хоткеи", true);
        return false;
    }
    draft.replay_hotkeys = ReplayHotkeysFromControls();
    draft.screenshot_hotkey_enabled = screenshot_hotkey_toggle_.IsOn();
    draft.screenshot_hotkey_chord = openreplay::ToUtf8(screenshot_hotkey_input_.Text().c_str());
    draft.recording_hotkey_enabled = recording_hotkey_toggle_.IsOn();
    draft.recording_hotkey_chord = openreplay::ToUtf8(recording_hotkey_input_.Text().c_str());
    draft.Normalize();
    return true;
}

void MainWindow::ApplySettingsFromControls(bool show_errors) {
    settings_apply_pending_ = false;
    openreplay::Settings draft;
    if (!ReadSettingsFromControls(draft, show_errors)) return;
    if (draft == settings_) {
        UpdateSettingsSaveStatus(english_ ? L"All changes saved" : L"Все изменения сохранены");
        return;
    }

    const auto previous = settings_;
    if (draft.start_with_windows != previous.start_with_windows &&
        !ApplyStartWithWindows(draft.start_with_windows)) {
        updating_ui_ = true;
        start_with_windows_toggle_.IsOn(previous.start_with_windows);
        updating_ui_ = false;
        UpdateSettingsSaveStatus(english_ ? L"Unable to update Windows startup"
                                           : L"Не удалось изменить автозапуск Windows", true);
        if (show_errors) {
            ShowNotification(english_ ? L"Unable to update Windows startup"
                                      : L"Не удалось изменить автозапуск Windows", true);
        }
        return;
    }

    try {
        settings_store_.Save(draft);
    } catch (...) {
        const auto startup_restored = draft.start_with_windows == previous.start_with_windows ||
                                      ApplyStartWithWindows(previous.start_with_windows);
        updating_ui_ = true;
        start_with_windows_toggle_.IsOn(previous.start_with_windows);
        updating_ui_ = false;
        UpdateSettingsSaveStatus(startup_restored
                                     ? (english_ ? L"Unable to save settings" : L"Не удалось сохранить настройки")
                                     : (english_ ? L"Unable to save settings or restore Windows startup"
                                                 : L"Не удалось сохранить настройки и восстановить автозапуск Windows"),
                                 true);
        return;
    }

    settings_ = std::move(draft);
    replay_hotkey_draft_ = settings_.replay_hotkeys;
    english_ = settings_.language == "en-US";
    performance_overlay_.ApplySettings(settings_, english_);
    ApplyLanguage();
    LayoutNotifications();
    QueueHostSettingsApply();
}

void MainWindow::UpdateSettingsSaveStatus(std::wstring_view text, bool error) {
    if (!settings_save_status_) return;
    settings_save_status_.Text(winrt::hstring{text});
    settings_save_status_.Foreground(error ? danger_brush_ : success_brush_);
}

void MainWindow::QueueHostSettingsApply(bool audio_device_restored) {
    audio_restore_notification_pending_ = audio_restore_notification_pending_ || audio_device_restored;
    const bool capture_changed = !openreplay::CapturePipelineSettingsEqual(host_settings_, settings_);
    const bool replay_changed = !openreplay::ReplayOutputSettingsEqual(host_settings_, settings_);
    if (!capture_changed && !replay_changed) {
        host_settings_ = settings_;
        RegisterConfigurableHotkeys();
        audio_restore_notification_pending_ = false;
        UpdateSettingsSaveStatus(english_ ? L"All changes saved" : L"Все изменения сохранены");
        return;
    }
    settings_reload_requested_ = true;
    StartPendingHostSettingsApply();
}

void MainWindow::StartPendingHostSettingsApply() {
    if (exiting_ || !settings_reload_requested_ || settings_reload_in_flight_) return;
    if (recording_) {
        UpdateSettingsSaveStatus(english_ ? L"Saved · capture changes apply after recording"
                                           : L"Сохранено · захват обновится после записи");
        return;
    }
    if (replay_save_pending_) {
        UpdateSettingsSaveStatus(english_ ? L"Saved · replay settings apply after the current save"
                                           : L"Сохранено · настройки повтора применятся после сохранения");
        return;
    }
    if (!host_connected_) {
        UpdateSettingsSaveStatus(english_ ? L"Saved · waiting for capture engine"
                                           : L"Сохранено · ожидание движка захвата");
        return;
    }

    const bool capture_changed = !openreplay::CapturePipelineSettingsEqual(host_settings_, settings_);
    const bool replay_changed = !openreplay::ReplayOutputSettingsEqual(host_settings_, settings_);
    if (!capture_changed && !replay_changed) {
        settings_reload_requested_ = false;
        host_settings_ = settings_;
        RegisterConfigurableHotkeys();
        UpdateSettingsSaveStatus(english_ ? L"All changes saved" : L"Все изменения сохранены");
        return;
    }

    settings_reload_requested_ = false;
    settings_reload_in_flight_ = true;
    if (settings_apply_timer_) settings_apply_timer_.Stop();
    settings_save_status_.Text(english_ ? L"Saved · applying capture changes in background..."
                                        : L"Сохранено · обновление захвата в фоне...");
    settings_save_status_.Foreground(secondary_text_brush_);
    const auto command = capture_changed
        ? (!replay_changed && openreplay::CapturePipelineCoreSettingsEqual(host_settings_, settings_) &&
           !openreplay::AudioSourceSettingsEqual(host_settings_, settings_)
               ? "reload_audio_sources" : "reload_settings")
        : "reload_replay_outputs";
    ReloadHostSettingsAsync(command, settings_);
}

void MainWindow::ApplyPendingReplayToggle() {
    if (!replay_toggle_apply_pending_ || settings_reload_in_flight_ || !host_connected_) return;
    const auto desired = replay_toggle_desired_;
    const auto response = pipe_client_.Request(desired ? "replay:on" : "replay:off",
                                               std::chrono::milliseconds{500});
    if (!response.ok) {
        const auto message = Field(response, "message");
        if (message.find("Host is not available") != std::string::npos ||
            message.find("connect to OpenReplay Host") != std::string::npos ||
            message.find("timed out") != std::string::npos) {
            host_connected_ = false;
            return;
        }
        replay_toggle_apply_pending_ = false;
        ShowNotification(message.empty() ? (english_ ? L"Unable to update instant replay state"
                                                     : L"Не удалось изменить состояние мгновенного повтора")
                                         : openreplay::FromUtf8(message),
                         true);
        return;
    }
    replay_toggle_apply_pending_ = false;
    settings_.instant_replay_enabled = desired;
    host_settings_.instant_replay_enabled = desired;
    ApplyResponse(response);
}

winrt::fire_and_forget MainWindow::ReloadHostSettingsAsync(std::string command, openreplay::Settings snapshot) {
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    co_await winrt::resume_background();
    openreplay::Response response;
    try {
        response = openreplay::PipeClient{}.Request(command, std::chrono::seconds{30});
    } catch (const std::exception& error) {
        response = {false, {{"message", error.what()}}};
    } catch (...) {
        response = {false, {{"message", "Unable to apply capture settings"}}};
    }
    dispatcher.TryEnqueue([weak, response = std::move(response), snapshot = std::move(snapshot)]() mutable {
        if (const auto self = weak.get()) {
            self->CompleteHostSettingsApply(std::move(response), std::move(snapshot));
        }
    });
}

void MainWindow::CompleteHostSettingsApply(openreplay::Response response, openreplay::Settings snapshot) {
    settings_reload_in_flight_ = false;
    if (response.ok) {
        host_settings_ = std::move(snapshot);
        ApplyResponse(response);
        if (!settings_reload_requested_ &&
            openreplay::CapturePipelineSettingsEqual(host_settings_, settings_) &&
            openreplay::ReplayOutputSettingsEqual(host_settings_, settings_)) {
            RegisterConfigurableHotkeys();
            UpdateSettingsSaveStatus(english_ ? L"All changes saved" : L"Все изменения сохранены");
            if (audio_restore_notification_pending_) {
                audio_restore_notification_pending_ = false;
                ShowToast(english_ ? L"Audio device restored" : L"Аудиоустройство восстановлено",
                          english_ ? L"A changed endpoint was matched by name or replaced with the Windows default"
                                   : L"Изменившийся endpoint найден по имени или заменён устройством Windows по умолчанию",
                          false, false, true);
            }
        }
    } else {
        const auto message = Field(response, "message");
        if (message.find("Host is not available") != std::string::npos ||
            message.find("connect to OpenReplay Host") != std::string::npos ||
            message.find("timed out") != std::string::npos) {
            host_connected_ = false;
            settings_reload_requested_ = true;
            UpdateSettingsSaveStatus(english_ ? L"Saved · waiting for capture engine"
                                               : L"Сохранено · ожидание движка захвата");
        } else if (message.find("Stop recording") != std::string::npos ||
                   message.find("current replay save") != std::string::npos) {
            settings_reload_requested_ = true;
            UpdateSettingsSaveStatus(english_ ? L"Saved · capture changes are queued"
                                               : L"Сохранено · изменения захвата в очереди");
        } else {
            settings_reload_requested_ = false;
            UpdateSettingsSaveStatus(message.empty()
                                         ? (english_ ? L"Capture settings could not be applied"
                                                     : L"Не удалось применить настройки захвата")
                                         : openreplay::FromUtf8(message),
                                     true);
        }
    }
    ApplyPendingReplayToggle();
    if (!recording_ && !replay_save_pending_) StartPendingHostSettingsApply();
}

void MainWindow::OpenFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    std::error_code error;
    std::filesystem::create_directories(settings_.output_directory, error);
    ShellExecuteW(GetWindowHandle(), L"open", settings_.output_directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::OpenLogs_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto directory = openreplay::SettingsStore::DefaultPath().parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto result = ShellExecuteW(GetWindowHandle(), L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowNotification(english_ ? L"Unable to open the logs folder" : L"Не удалось открыть папку логов", true);
    }
}

void MainWindow::CheckUpdate_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    CheckForUpdatesAsync(true, false);
}

void MainWindow::UpdateAction_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (downloaded_update_.empty()) DownloadUpdateAsync();
    else ApplyDownloadedUpdate();
}

void MainWindow::OpenReleaseNotes_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (available_update_.release_notes_url.empty()) return;
    const auto target = openreplay::FromUtf8(available_update_.release_notes_url);
    ShellExecuteW(GetWindowHandle(), L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::AddTrayIcon() {
    tray_icon_.cbSize = sizeof(tray_icon_);
    tray_icon_.hWnd = GetWindowHandle();
    tray_icon_.uID = 1;
    tray_icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_icon_.uCallbackMessage = kTrayMessage;
    tray_icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray_icon_.szTip, L"OpenReplay");
    Shell_NotifyIconW(NIM_ADD, &tray_icon_);
}

void MainWindow::RemoveTrayIcon() {
    if (tray_icon_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &tray_icon_);
        tray_icon_.hWnd = nullptr;
    }
}

void MainWindow::ShowTrayMenu() {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayOpen, english_ ? L"Open overlay\tAlt+Z" : L"Открыть оверлей\tAlt+Z");
    AppendMenuW(menu, MF_STRING, kTraySaveReplay, english_ ? L"Save replay" : L"Сохранить повтор");
    AppendMenuW(menu, MF_STRING, kTrayRecording,
                 recording_ ? (english_ ? L"Stop recording" : L"Остановить запись")
                             : (english_ ? L"Start recording" : L"Начать запись"));
    AppendMenuW(menu, MF_STRING, kTrayScreenshot,
                english_ ? L"Take screenshot" : L"Сделать снимок");
    AppendMenuW(menu, MF_STRING, kTrayPerformance,
                english_ ? L"Performance overlay\tAlt+R" : L"Производительность\tAlt+R");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, english_ ? L"Exit" : L"Выход");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(GetWindowHandle());
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0,
                   GetWindowHandle(), nullptr);
    DestroyMenu(menu);
}

void MainWindow::ExitApplication() {
    if (exiting_) return;
    exiting_ = true;
    if (settings_apply_pending_) {
        settings_apply_timer_.Stop();
        ApplySettingsFromControls(false);
    }
    if (status_timer_) status_timer_.Stop();
    if (settings_apply_timer_) settings_apply_timer_.Stop();
    if (clip_library_window_owner_) {
        clip_library_window_owner_->Shutdown();
        clip_library_window_owner_ = nullptr;
    }
    const auto response = pipe_client_.Request("shutdown", std::chrono::milliseconds{200});
    (void)response;
    RemoveTrayIcon();
    UnregisterConfigurableHotkeys();
    if (hotkey_registered_) {
        UnregisterHotKey(GetWindowHandle(), kOverlayHotkey);
        hotkey_registered_ = false;
    }
    if (performance_hotkey_registered_) {
        UnregisterHotKey(GetWindowHandle(), kPerformanceOverlayHotkey);
        performance_hotkey_registered_ = false;
    }
    performance_overlay_.Shutdown();
    Close();
}

LRESULT CALLBACK MainWindow::WindowSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                             UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<MainWindow*>(context);
    // WinUI can re-show the main window after a background launch hid it.
    if (message == WM_SHOWWINDOW && wparam && !self->visible_ && !self->exiting_) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    // Once the panel takes focus from a fullscreen game, the system pushes it into the work area,
    // beside a taskbar the game still covers. Keep it where PositionOverlayWindow put it.
    if (message == WM_WINDOWPOSCHANGING && self->visible_) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
        const auto& bounds = self->overlay_bounds_;
        if (!(position->flags & SWP_NOMOVE)) {
            position->x = bounds.left;
            position->y = bounds.top;
        }
        if (!(position->flags & SWP_NOSIZE)) {
            position->cx = bounds.right - bounds.left;
            position->cy = bounds.bottom - bounds.top;
        }
    }
    if (message == WM_HOTKEY && wparam == kOverlayHotkey) {
        self->ToggleOverlay();
        return 0;
    }
    if (message == WM_HOTKEY && wparam == kPerformanceOverlayHotkey) {
        self->TogglePerformanceOverlay();
        return 0;
    }
    if (message == WM_HOTKEY && wparam == kScreenshotHotkey) {
        self->TakeScreenshot();
        return 0;
    }
    if (message == WM_HOTKEY && wparam == kRecordingHotkey) {
        self->ToggleRecording();
        return 0;
    }
    if (message == WM_HOTKEY && wparam >= kReplayHotkeyBase &&
        wparam < kReplayHotkeyBase + static_cast<WPARAM>(self->settings_.replay_hotkeys.size())) {
        const auto index = static_cast<std::size_t>(wparam - kReplayHotkeyBase);
        self->RequestReplaySave(self->settings_.replay_hotkeys[index].replay_seconds);
        return 0;
    }
    if (message == kTrayMessage) {
        if (lparam == WM_LBUTTONUP) self->ToggleOverlay();
        else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) self->ShowTrayMenu();
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case kTrayOpen: self->ShowOverlay(); return 0;
        case kTraySaveReplay: self->RequestReplaySave(self->settings_.replay_seconds); return 0;
        case kTrayRecording: self->ToggleRecording(); return 0;
        case kTrayScreenshot: self->TakeScreenshot(); return 0;
        case kTrayPerformance: self->TogglePerformanceOverlay(); return 0;
        case kTrayExit: self->ExitApplication(); return 0;
        default: break;
        }
    }
    if (message == WM_CLOSE && !self->exiting_) {
        self->HideOverlay();
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace winrt::OpenReplay::implementation

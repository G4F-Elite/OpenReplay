#include "pch.h"

#include "MainWindow.xaml.h"
#include "StartupTrace.h"
#include "WindowPlacement.h"

#include "openreplay/Version.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "microsoft.ui.xaml.window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace {

constexpr UINT_PTR kNotificationAnimationTimer = 1;
constexpr UINT_PTR kNotificationHideTimer = 2;

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

winrt::Microsoft::UI::Xaml::Media::SolidColorBrush Brush(uint32_t argb) {
    winrt::Windows::UI::Color color{};
    color.A = static_cast<uint8_t>(argb >> 24);
    color.R = static_cast<uint8_t>(argb >> 16);
    color.G = static_cast<uint8_t>(argb >> 8);
    color.B = static_cast<uint8_t>(argb);
    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{color};
}

winrt::Microsoft::UI::Xaml::Media::FontFamily PublicSans() {
    return winrt::Microsoft::UI::Xaml::Media::FontFamily{
        L"ms-appx:///Assets/PublicSans-Variable.ttf#Public Sans"};
}

void SetResource(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, std::wstring_view key,
                 winrt::Windows::Foundation::IInspectable const& value) {
    (void)element.Resources().Insert(winrt::box_value(winrt::hstring{key}), value);
}

winrt::Microsoft::UI::Xaml::Controls::TextBlock Text(
    std::wstring_view value, double size,
    winrt::Microsoft::UI::Xaml::Media::Brush const& foreground) {
    winrt::Microsoft::UI::Xaml::Controls::TextBlock text;
    text.Text(winrt::hstring{value});
    text.FontSize(size);
    text.FontFamily(PublicSans());
    text.Foreground(foreground);
    return text;
}

void AddRow(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid, double value,
            winrt::Microsoft::UI::Xaml::GridUnitType type) {
    winrt::Microsoft::UI::Xaml::Controls::RowDefinition row;
    row.Height(winrt::Microsoft::UI::Xaml::GridLength{value, type});
    grid.RowDefinitions().Append(row);
}

void AddColumn(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid, double value,
               winrt::Microsoft::UI::Xaml::GridUnitType type) {
    winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition column;
    column.Width(winrt::Microsoft::UI::Xaml::GridLength{value, type});
    grid.ColumnDefinitions().Append(column);
}

winrt::Microsoft::UI::Xaml::Controls::Button ActionButton(
    std::wstring_view label,
    winrt::Microsoft::UI::Xaml::Media::Brush const& background,
    winrt::Microsoft::UI::Xaml::Media::Brush const& foreground) {
    winrt::Microsoft::UI::Xaml::Controls::Button button;
    button.Content(winrt::box_value(winrt::hstring{label}));
    button.Background(background);
    button.Foreground(foreground);
    button.FontFamily(PublicSans());
    button.FontSize(13);
    button.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    button.MinHeight(42);
    button.Padding(winrt::Microsoft::UI::Xaml::Thickness{16, 9, 16, 9});
    button.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{12, 12, 12, 12});
    button.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
    return button;
}

void StyleInput(winrt::Microsoft::UI::Xaml::Controls::Control const& control) {
    control.FontFamily(PublicSans());
    control.FontSize(13);
    control.MinHeight(38);
    control.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8, 8, 8, 8});
}

void StyleSlider(
    winrt::Microsoft::UI::Xaml::Controls::Slider const& slider,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent_hover,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent_pressed) {
    const auto transparent = Brush(0x00000000);
    SetResource(slider, L"SliderTrackFill", transparent);
    SetResource(slider, L"SliderTrackFillPointerOver", transparent);
    SetResource(slider, L"SliderTrackFillPressed", transparent);
    SetResource(slider, L"SliderTrackFillDisabled", transparent);
    SetResource(slider, L"SliderTrackValueFill", accent);
    SetResource(slider, L"SliderTrackValueFillPointerOver", accent_hover);
    SetResource(slider, L"SliderTrackValueFillPressed", accent_pressed);
    SetResource(slider, L"SliderThumbBackground", accent);
    SetResource(slider, L"SliderThumbBackgroundPointerOver", accent_hover);
    SetResource(slider, L"SliderThumbBackgroundPressed", accent_pressed);
}

void StyleToggle(
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch const& toggle,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent_hover,
    winrt::Microsoft::UI::Xaml::Media::Brush const& accent_pressed,
    winrt::Microsoft::UI::Xaml::Media::Brush const& knob,
    winrt::Microsoft::UI::Xaml::Media::Brush const& off_fill,
    winrt::Microsoft::UI::Xaml::Media::Brush const& off_stroke) {
    toggle.FontFamily(PublicSans());
    toggle.Width(44);
    toggle.MinWidth(44);
    toggle.OnContent(nullptr);
    toggle.OffContent(nullptr);
    SetResource(toggle, L"ToggleSwitchFillOn", accent);
    SetResource(toggle, L"ToggleSwitchFillOnPointerOver", accent_hover);
    SetResource(toggle, L"ToggleSwitchFillOnPressed", accent_pressed);
    SetResource(toggle, L"ToggleSwitchStrokeOn", accent);
    SetResource(toggle, L"ToggleSwitchStrokeOnPointerOver", accent_hover);
    SetResource(toggle, L"ToggleSwitchStrokeOnPressed", accent_pressed);
    SetResource(toggle, L"ToggleSwitchKnobFillOn", knob);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPointerOver", knob);
    SetResource(toggle, L"ToggleSwitchKnobFillOnPressed", knob);
    SetResource(toggle, L"ToggleSwitchFillOff", off_fill);
    SetResource(toggle, L"ToggleSwitchFillOffPointerOver", off_stroke);
    SetResource(toggle, L"ToggleSwitchStrokeOff", off_stroke);
    SetResource(toggle, L"ToggleSwitchKnobFillOff", knob);
}

winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem ComboItem(std::wstring_view label) {
    winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem item;
    item.FontFamily(PublicSans());
    item.Content(winrt::box_value(winrt::hstring{label}));
    return item;
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

    const auto root_brush = Brush(0xFF1B1718);
    const auto panel_brush = Brush(0xFF18181B);
    const auto card_brush = Brush(0xFF27272A);
    const auto card_hover_brush = Brush(0xFF303034);
    const auto card_border_brush = Brush(0xFF3F3F46);
    accent_brush_ = Brush(0xFF00C16A);
    success_brush_ = Brush(0xFF00DC82);
    const auto accent_dark_brush = Brush(0xFF007F45);
    const auto deep_green_brush = Brush(0xFF016538);
    const auto near_black_green_brush = Brush(0xFF052E16);
    const auto primary_text_brush = Brush(0xFFFFFFFF);
    secondary_text_brush_ = Brush(0xFFA1A1AA);
    danger_brush_ = Brush(0xFFFF5C67);
    const auto quiet_button_brush = Brush(0xFF3F3F46);
    const auto muted_brush = Brush(0xFF71717A);

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
    AddRow(layout, 80, GridUnitType::Pixel);
    AddRow(layout, 0, GridUnitType::Auto);
    AddRow(layout, 1, GridUnitType::Star);
    AddRow(layout, 44, GridUnitType::Pixel);
    shell.Child(layout);

    Grid header;
    header.Padding(Thickness{20, 0, 16, 0});
    header.Background(panel_brush);
    AddColumn(header, 1, GridUnitType::Star);
    AddColumn(header, 0, GridUnitType::Auto);
    AddColumn(header, 0, GridUnitType::Auto);
    Grid::SetRow(header, 0);

    StackPanel brand;
    brand.Orientation(Orientation::Horizontal);
    brand.VerticalAlignment(VerticalAlignment::Center);
    Border brand_mark;
    brand_mark.Width(5);
    brand_mark.Height(40);
    brand_mark.CornerRadius(CornerRadius{2, 2, 2, 2});
    brand_mark.Background(accent_brush_);
    brand_mark.Margin(Thickness{0, 0, 12, 0});
    brand.Children().Append(brand_mark);
    StackPanel brand_copy;
    auto brand_text = Text(L"OPENREPLAY", 19, primary_text_brush);
    brand_text.FontWeight(Windows::UI::Text::FontWeights::Bold());
    brand_text.CharacterSpacing(35);
    brand_copy.Children().Append(brand_text);
    encoder_text_ = Text(L"Capture pipeline is starting", 11, muted_brush);
    encoder_text_.TextTrimming(TextTrimming::CharacterEllipsis);
    encoder_text_.MaxWidth(230);
    encoder_text_.Margin(Thickness{0, 2, 0, 0});
    brand_copy.Children().Append(encoder_text_);
    brand.Children().Append(brand_copy);
    header.Children().Append(brand);

    settings_button_ = ActionButton(L"", quiet_button_brush, primary_text_brush);
    settings_button_.Margin(Thickness{0, 0, 8, 0});
    settings_button_.MinWidth(102);
    settings_button_.VerticalAlignment(VerticalAlignment::Center);
    settings_button_.Click({this, &MainWindow::SettingsButton_Click});
    settings_button_text_ = Text(L"Настройки", 12, primary_text_brush);
    settings_button_text_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    settings_button_.Content(settings_button_text_);
    Grid::SetColumn(settings_button_, 1);
    header.Children().Append(settings_button_);

    auto close_button = ActionButton(L"×", quiet_button_brush, primary_text_brush);
    close_button.Width(42);
    close_button.Height(42);
    close_button.Padding(Thickness{0, 0, 0, 0});
    close_button.FontSize(20);
    close_button.VerticalAlignment(VerticalAlignment::Center);
    close_button.Click({this, &MainWindow::CloseButton_Click});
    Grid::SetColumn(close_button, 2);
    header.Children().Append(close_button);
    layout.Children().Append(header);

    dashboard_panel_ = Grid{};
    dashboard_panel_.Padding(Thickness{20, 18, 20, 16});
    AddRow(dashboard_panel_, 0, GridUnitType::Auto);
    AddRow(dashboard_panel_, 0, GridUnitType::Auto);
    AddRow(dashboard_panel_, 1, GridUnitType::Star);
    Grid::SetRow(dashboard_panel_, 2);

    dashboard_title_ = Text(L"Центр захвата", 27, primary_text_brush);
    dashboard_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    dashboard_panel_.Children().Append(dashboard_title_);
    dashboard_subtitle_ = Text(L"Состояние и быстрые действия", 12.5, secondary_text_brush_);
    dashboard_subtitle_.Margin(Thickness{0, 5, 0, 0});
    Grid::SetRow(dashboard_subtitle_, 1);
    dashboard_panel_.Children().Append(dashboard_subtitle_);

    ScrollViewer cards_scroll;
    cards_scroll.Margin(Thickness{0, 18, 0, 0});
    cards_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    cards_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    Grid::SetRow(cards_scroll, 2);
    StackPanel cards;
    cards.Spacing(10);
    cards_scroll.Content(cards);

    const auto create_card = [&] {
        Border card;
        card.Background(card_brush);
        card.BorderBrush(card_border_brush);
        card.BorderThickness(Thickness{1, 1, 1, 1});
        card.CornerRadius(CornerRadius{8, 8, 8, 8});
        card.Shadow(Microsoft::UI::Xaml::Media::ThemeShadow{});
        card.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 6.0f});
        card.PointerEntered([card_hover_brush, accent_dark_brush](auto const& sender, auto const&) {
            const auto hovered = sender.template as<Border>();
            hovered.Background(card_hover_brush);
            hovered.BorderBrush(accent_dark_brush);
            hovered.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 10.0f});
        });
        card.PointerExited([card_brush, card_border_brush](auto const& sender, auto const&) {
            const auto hovered = sender.template as<Border>();
            hovered.Background(card_brush);
            hovered.BorderBrush(card_border_brush);
            hovered.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 6.0f});
        });
        StackPanel content;
        content.Padding(Thickness{18, 16, 18, 16});
        card.Child(content);
        cards.Children().Append(card);
        return content;
    };

    auto replay_card = create_card();
    Grid replay_header;
    AddColumn(replay_header, 1, GridUnitType::Star);
    AddColumn(replay_header, 0, GridUnitType::Auto);
    replay_title_ = Text(L"Мгновенный повтор", 18, primary_text_brush);
    replay_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    replay_title_.VerticalAlignment(VerticalAlignment::Center);
    replay_header.Children().Append(replay_title_);
    replay_toggle_ = ToggleSwitch{};
    StyleToggle(replay_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    replay_toggle_.HorizontalAlignment(HorizontalAlignment::Right);
    replay_toggle_.VerticalAlignment(VerticalAlignment::Center);
    replay_toggle_.Toggled({this, &MainWindow::ReplayToggle_Toggled});
    Grid::SetColumn(replay_toggle_, 1);
    replay_header.Children().Append(replay_toggle_);
    replay_card.Children().Append(replay_header);
    Border replay_status;
    replay_status.Background(near_black_green_brush);
    replay_status.BorderBrush(deep_green_brush);
    replay_status.BorderThickness(Thickness{1, 1, 1, 1});
    replay_status.CornerRadius(CornerRadius{6, 6, 6, 6});
    replay_status.Padding(Thickness{9, 4, 9, 4});
    replay_status.HorizontalAlignment(HorizontalAlignment::Left);
    replay_status.Margin(Thickness{0, 12, 0, 0});
    replay_state_text_ = Text(L"Подключение...", 12, success_brush_);
    replay_state_text_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    replay_status.Child(replay_state_text_);
    replay_card.Children().Append(replay_status);
    replay_description_ = Text(L"Сохранит последние 60 секунд без остановки игры.", 13, secondary_text_brush_);
    replay_description_.TextWrapping(TextWrapping::Wrap);
    replay_description_.LineHeight(18);
    replay_description_.Margin(Thickness{0, 12, 0, 14});
    replay_card.Children().Append(replay_description_);
    save_replay_button_ = ActionButton(L"Сохранить последние 60 сек", accent_brush_, near_black_green_brush);
    save_replay_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    save_replay_button_.Click({this, &MainWindow::SaveReplay_Click});
    replay_card.Children().Append(save_replay_button_);

    auto recording_card = create_card();
    Grid recording_header;
    AddColumn(recording_header, 1, GridUnitType::Star);
    AddColumn(recording_header, 0, GridUnitType::Auto);
    recording_title_ = Text(L"Запись сессии", 18, primary_text_brush);
    recording_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    recording_header.Children().Append(recording_title_);
    recording_state_text_ = Text(L"Готово к записи", 12, secondary_text_brush_);
    recording_state_text_.VerticalAlignment(VerticalAlignment::Center);
    recording_state_text_.Margin(Thickness{12, 0, 0, 0});
    Grid::SetColumn(recording_state_text_, 1);
    recording_header.Children().Append(recording_state_text_);
    recording_card.Children().Append(recording_header);
    recording_description_ = Text(L"MKV  •  Игра, система и микрофон на отдельных дорожках", 13,
                                   secondary_text_brush_);
    recording_description_.TextWrapping(TextWrapping::Wrap);
    recording_description_.LineHeight(18);
    recording_description_.Margin(Thickness{0, 13, 0, 14});
    recording_card.Children().Append(recording_description_);
    recording_button_ = ActionButton(L"Начать запись", quiet_button_brush, primary_text_brush);
    recording_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    recording_button_.Click({this, &MainWindow::RecordingButton_Click});
    recording_card.Children().Append(recording_button_);

    auto screenshot_card = create_card();
    Grid screenshot_header;
    AddColumn(screenshot_header, 1, GridUnitType::Star);
    AddColumn(screenshot_header, 0, GridUnitType::Auto);
    screenshot_title_ = Text(L"Снимок экрана", 18, primary_text_brush);
    screenshot_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    screenshot_header.Children().Append(screenshot_title_);
    screenshot_state_text_ = Text(L"PNG", 12, success_brush_);
    screenshot_state_text_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    screenshot_state_text_.VerticalAlignment(VerticalAlignment::Center);
    screenshot_state_text_.Margin(Thickness{12, 0, 0, 0});
    Grid::SetColumn(screenshot_state_text_, 1);
    screenshot_header.Children().Append(screenshot_state_text_);
    screenshot_card.Children().Append(screenshot_header);
    screenshot_description_ = Text(L"Сохранение в исходном разрешении выбранного монитора.", 13,
                                     secondary_text_brush_);
    screenshot_description_.TextWrapping(TextWrapping::Wrap);
    screenshot_description_.LineHeight(18);
    screenshot_description_.Margin(Thickness{0, 13, 0, 14});
    screenshot_card.Children().Append(screenshot_description_);
    screenshot_button_ = ActionButton(L"Сделать снимок", quiet_button_brush, primary_text_brush);
    screenshot_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    screenshot_button_.Click({this, &MainWindow::ScreenshotButton_Click});
    screenshot_card.Children().Append(screenshot_button_);
    dashboard_panel_.Children().Append(cards_scroll);
    layout.Children().Append(dashboard_panel_);

    settings_panel_ = Grid{};
    settings_panel_.Padding(Thickness{20, 18, 20, 16});
    settings_panel_.Visibility(Visibility::Collapsed);
    AddRow(settings_panel_, 0, GridUnitType::Auto);
    AddRow(settings_panel_, 1, GridUnitType::Star);
    AddRow(settings_panel_, 0, GridUnitType::Auto);
    Grid::SetRow(settings_panel_, 2);

    Grid settings_header;
    AddColumn(settings_header, 1, GridUnitType::Star);
    AddColumn(settings_header, 0, GridUnitType::Auto);
    StackPanel settings_heading;
    settings_title_ = Text(L"Настройки", 27, primary_text_brush);
    settings_title_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    settings_subtitle_ = Text(L"Источник, качество и сохранение", 12.5, secondary_text_brush_);
    settings_subtitle_.TextWrapping(TextWrapping::Wrap);
    settings_subtitle_.Margin(Thickness{0, 5, 0, 0});
    settings_heading.Children().Append(settings_title_);
    settings_heading.Children().Append(settings_subtitle_);
    settings_header.Children().Append(settings_heading);
    back_button_ = ActionButton(L"← Назад", quiet_button_brush, primary_text_brush);
    back_button_.Click({this, &MainWindow::BackButton_Click});
    Grid::SetColumn(back_button_, 1);
    settings_header.Children().Append(back_button_);
    settings_panel_.Children().Append(settings_header);

    ScrollViewer settings_scroll;
    settings_scroll.Margin(Thickness{0, 18, 0, 12});
    settings_scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    settings_scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    Grid::SetRow(settings_scroll, 1);
    StackPanel form;
    form.Spacing(12);

    const auto create_section = [&](TextBlock& heading, std::wstring_view title) {
        Border surface;
        surface.Background(card_brush);
        surface.BorderBrush(card_border_brush);
        surface.BorderThickness(Thickness{1, 1, 1, 1});
        surface.CornerRadius(CornerRadius{8, 8, 8, 8});
        surface.Padding(Thickness{16, 15, 16, 16});
        surface.Shadow(Microsoft::UI::Xaml::Media::ThemeShadow{});
        surface.Translation(Windows::Foundation::Numerics::float3{0.0f, 0.0f, 4.0f});

        StackPanel content;
        heading = Text(title, 12, accent_brush_);
        heading.FontWeight(Windows::UI::Text::FontWeights::Bold());
        heading.CharacterSpacing(70);
        heading.Margin(Thickness{0, 0, 0, 13});
        content.Children().Append(heading);
        surface.Child(content);
        form.Children().Append(surface);
        return content;
    };

    const auto add_field = [&](StackPanel const& column, TextBlock& label, std::wstring_view text,
                                 FrameworkElement const& control) {
        label = Text(text, 12.5, secondary_text_brush_);
        label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        label.Margin(Thickness{0, 0, 0, 6});
        column.Children().Append(label);
        control.Margin(Thickness{0, 0, 0, 14});
        column.Children().Append(control);
    };

    const auto source_section = create_section(source_section_title_, L"ИСТОЧНИК");

    monitor_selector_ = ComboBox{};
    StyleInput(monitor_selector_);
    monitor_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    monitor_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(source_section, monitor_label_, L"Монитор", monitor_selector_);

    const auto add_audio_devices = [&](Expander& expander, TextBlock& label, std::wstring_view text,
                                        TextBlock& summary, StackPanel& devices) {
        label = Text(text, 12.5, secondary_text_brush_);
        label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Grid header;
        AddColumn(header, 1, GridUnitType::Star);
        AddColumn(header, 0, GridUnitType::Auto);
        header.Children().Append(label);
        summary = Text(L"1 выбрано", 11, success_brush_);
        summary.VerticalAlignment(VerticalAlignment::Center);
        summary.Margin(Thickness{10, 0, 4, 0});
        Grid::SetColumn(summary, 1);
        header.Children().Append(summary);
        devices = StackPanel{};
        devices.Spacing(8);
        devices.Padding(Thickness{4, 6, 0, 8});
        devices.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander = Expander{};
        expander.Header(header);
        expander.Content(devices);
        expander.IsExpanded(false);
        expander.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        expander.Margin(Thickness{0, 0, 0, 8});
        source_section.Children().Append(expander);
    };
    add_audio_devices(desktop_audio_devices_expander_, desktop_audio_label_, L"Системный звук",
                      desktop_audio_summary_, desktop_audio_devices_panel_);
    add_audio_devices(microphone_devices_expander_, microphone_devices_label_, L"Микрофоны и входы",
                      microphone_audio_summary_, microphone_devices_panel_);
    audio_devices_hint_ = Text(L"Можно выбрать несколько устройств, суммарно до 5 активных источников.", 11.5,
                               secondary_text_brush_);
    audio_devices_hint_.TextWrapping(TextWrapping::Wrap);
    source_section.Children().Append(audio_devices_hint_);

    const auto video_section = create_section(video_section_title_, L"ВИДЕО");

    Grid duration_header;
    AddColumn(duration_header, 1, GridUnitType::Star);
    AddColumn(duration_header, 0, GridUnitType::Auto);
    replay_duration_label_ = Text(L"Длительность повтора", 12, secondary_text_brush_);
    replay_duration_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    duration_header.Children().Append(replay_duration_label_);
    replay_duration_value_ = Text(L"60 сек", 12, success_brush_);
    replay_duration_value_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    Grid::SetColumn(replay_duration_value_, 1);
    duration_header.Children().Append(replay_duration_value_);
    video_section.Children().Append(duration_header);
    replay_duration_slider_ = Slider{};
    StyleSlider(replay_duration_slider_, accent_brush_, success_brush_, accent_dark_brush);
    replay_duration_slider_.Minimum(15);
    replay_duration_slider_.Maximum(1200);
    replay_duration_slider_.StepFrequency(15);
    replay_duration_slider_.Margin(Thickness{0, 4, 0, 16});
    replay_duration_slider_.ValueChanged({this, &MainWindow::ReplayDurationSlider_ValueChanged});
    video_section.Children().Append(replay_duration_slider_);

    quality_selector_ = ComboBox{};
    StyleInput(quality_selector_);
    quality_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    quality_selector_.Items().Append(ComboItem(L"Производительность"));
    quality_selector_.Items().Append(ComboItem(L"Сбалансированный"));
    quality_selector_.Items().Append(ComboItem(L"Высокое качество"));
    quality_selector_.SelectionChanged({this, &MainWindow::QualitySelector_SelectionChanged});
    add_field(video_section, quality_label_, L"Пресет качества", quality_selector_);
    quality_description_ = Text(L"Баланс качества сжатия и нагрузки на GPU. Битрейт задаётся отдельно.", 11.5,
                                secondary_text_brush_);
    quality_description_.TextWrapping(TextWrapping::Wrap);
    quality_description_.Margin(Thickness{0, -8, 0, 14});
    video_section.Children().Append(quality_description_);

    Grid bitrate_header;
    AddColumn(bitrate_header, 1, GridUnitType::Star);
    AddColumn(bitrate_header, 0, GridUnitType::Auto);
    bitrate_label_ = Text(L"Целевой битрейт", 12.5, secondary_text_brush_);
    bitrate_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    bitrate_header.Children().Append(bitrate_label_);
    bitrate_value_ = Text(L"24 Мбит/с", 12, success_brush_);
    bitrate_value_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    Grid::SetColumn(bitrate_value_, 1);
    bitrate_header.Children().Append(bitrate_value_);
    video_section.Children().Append(bitrate_header);

    bitrate_slider_ = Slider{};
    StyleSlider(bitrate_slider_, accent_brush_, success_brush_, accent_dark_brush);
    bitrate_slider_.Minimum(4);
    bitrate_slider_.Maximum(120);
    bitrate_slider_.StepFrequency(1);
    bitrate_slider_.Margin(Thickness{0, 4, 0, 16});
    bitrate_slider_.ValueChanged({this, &MainWindow::BitrateSlider_ValueChanged});
    video_section.Children().Append(bitrate_slider_);

    Border replay_estimate;
    replay_estimate.Background(near_black_green_brush);
    replay_estimate.BorderBrush(accent_dark_brush);
    replay_estimate.BorderThickness(Thickness{1, 1, 1, 1});
    replay_estimate.CornerRadius(CornerRadius{8, 8, 8, 8});
    replay_estimate.Padding(Thickness{14, 12, 14, 12});
    replay_estimate.Margin(Thickness{0, 0, 0, 16});
    StackPanel estimate_content;
    replay_size_label_ = Text(L"ОЦЕНКА ПОВТОРА", 11, success_brush_);
    replay_size_label_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    replay_size_label_.CharacterSpacing(80);
    estimate_content.Children().Append(replay_size_label_);
    replay_size_value_ = Text(L"≈ 181 МБ", 22, primary_text_brush);
    replay_size_value_.FontWeight(Windows::UI::Text::FontWeights::Bold());
    replay_size_value_.Margin(Thickness{0, 5, 0, 2});
    estimate_content.Children().Append(replay_size_value_);
    replay_size_detail_ = Text(L"60 сек · VBR 24 Мбит/с · 3 аудиодорожки · MKV", 11.5,
                               secondary_text_brush_);
    replay_size_detail_.TextWrapping(TextWrapping::Wrap);
    estimate_content.Children().Append(replay_size_detail_);
    replay_estimate.Child(estimate_content);
    video_section.Children().Append(replay_estimate);

    codec_selector_ = ComboBox{};
    StyleInput(codec_selector_);
    codec_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    codec_selector_.Items().Append(ComboItem(L"H.264"));
    codec_selector_.Items().Append(ComboItem(L"HEVC"));
    codec_selector_.Items().Append(ComboItem(L"AV1"));
    codec_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(video_section, codec_label_, L"Кодек", codec_selector_);

    fps_selector_ = ComboBox{};
    StyleInput(fps_selector_);
    fps_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    fps_selector_.Items().Append(ComboItem(L"30 FPS"));
    fps_selector_.Items().Append(ComboItem(L"60 FPS"));
    fps_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(video_section, fps_label_, L"Частота кадров", fps_selector_);

    const auto input_section = create_section(input_section_title_, L"ИНТЕРФЕЙС И ВВОД");

    language_selector_ = ComboBox{};
    StyleInput(language_selector_);
    language_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    language_selector_.Items().Append(ComboItem(L"Русский"));
    language_selector_.Items().Append(ComboItem(L"English"));
    language_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    add_field(input_section, language_label_, L"Язык", language_selector_);

    const auto add_toggle_row = [&](TextBlock& label, std::wstring_view text, ToggleSwitch const& toggle) {
        Grid row;
        AddColumn(row, 1, GridUnitType::Star);
        AddColumn(row, 0, GridUnitType::Auto);
        row.Margin(Thickness{0, 1, 0, 13});
        label = Text(text, 13, primary_text_brush);
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);
        toggle.OnContent(winrt::box_value(L""));
        toggle.OffContent(winrt::box_value(L""));
        toggle.HorizontalAlignment(HorizontalAlignment::Right);
        toggle.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(toggle, 1);
        row.Children().Append(toggle);
        input_section.Children().Append(row);
    };

    microphone_toggle_ = ToggleSwitch{};
    StyleToggle(microphone_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    microphone_toggle_.Toggled({this, &MainWindow::EstimateInput_Toggled});
    add_toggle_row(microphone_label_, L"Микрофон", microphone_toggle_);
    cursor_toggle_ = ToggleSwitch{};
    StyleToggle(cursor_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    add_toggle_row(cursor_label_, L"Захватывать курсор", cursor_toggle_);
    cursor_toggle_.Toggled([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    start_with_windows_toggle_ = ToggleSwitch{};
    StyleToggle(start_with_windows_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    add_toggle_row(start_with_windows_label_, L"Запускать вместе с Windows", start_with_windows_toggle_);
    start_with_windows_toggle_.Toggled([this](auto&&, auto&&) { ScheduleSettingsApply(); });

    Grid shortcuts_header;
    AddColumn(shortcuts_header, 1, GridUnitType::Star);
    AddColumn(shortcuts_header, 0, GridUnitType::Auto);
    shortcuts_label_ = Text(L"Хоткеи", 12.5, secondary_text_brush_);
    shortcuts_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    shortcuts_header.Children().Append(shortcuts_label_);
    shortcuts_summary_ = Text(L"3 активны", 11, success_brush_);
    shortcuts_summary_.Margin(Thickness{10, 0, 4, 0});
    shortcuts_summary_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(shortcuts_summary_, 1);
    shortcuts_header.Children().Append(shortcuts_summary_);

    StackPanel shortcuts_content;
    shortcuts_content.Spacing(8);
    shortcuts_content.Padding(Thickness{4, 6, 0, 8});
    shortcuts_content.HorizontalAlignment(HorizontalAlignment::Stretch);
    replay_hotkeys_panel_ = StackPanel{};
    replay_hotkeys_panel_.Spacing(8);
    shortcuts_content.Children().Append(replay_hotkeys_panel_);
    replay_hotkey_add_button_ = ActionButton(L"+ Добавить хоткей", quiet_button_brush, primary_text_brush);
    replay_hotkey_add_button_.MinHeight(36);
    replay_hotkey_add_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    replay_hotkey_add_button_.Click({this, &MainWindow::ReplayHotkeyAdd_Click});
    shortcuts_content.Children().Append(replay_hotkey_add_button_);

    Border recording_shortcut_surface;
    recording_shortcut_surface.Background(Brush(0xFF202023));
    recording_shortcut_surface.BorderBrush(card_border_brush);
    recording_shortcut_surface.BorderThickness(Thickness{1, 1, 1, 1});
    recording_shortcut_surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    recording_shortcut_surface.Padding(Thickness{10, 9, 10, 9});
    StackPanel recording_shortcut_block;
    recording_shortcut_block.Spacing(6);
    Grid recording_shortcut_header;
    AddColumn(recording_shortcut_header, 1, GridUnitType::Star);
    AddColumn(recording_shortcut_header, 0, GridUnitType::Auto);
    recording_hotkey_label_ = Text(L"Запись сессии", 12.5, primary_text_brush);
    recording_hotkey_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    recording_hotkey_label_.VerticalAlignment(VerticalAlignment::Center);
    recording_shortcut_header.Children().Append(recording_hotkey_label_);
    recording_hotkey_toggle_ = ToggleSwitch{};
    StyleToggle(recording_hotkey_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    recording_hotkey_toggle_.Toggled([this](auto&&, auto&&) {
        if (updating_ui_) return;
        ValidateHotkeys(true, false);
        ScheduleSettingsApply();
    });
    Grid::SetColumn(recording_hotkey_toggle_, 1);
    recording_shortcut_header.Children().Append(recording_hotkey_toggle_);
    recording_shortcut_block.Children().Append(recording_shortcut_header);
    recording_hotkey_input_ = TextBox{};
    StyleInput(recording_hotkey_input_);
    recording_hotkey_input_.Height(36);
    recording_hotkey_input_.IsReadOnly(true);
    recording_hotkey_input_.PlaceholderText(L"Нажмите сочетание");
    recording_hotkey_input_.KeyDown({this, &MainWindow::RecordingHotkey_KeyDown});
    recording_hotkey_input_.LostFocus([this](auto&&, auto&&) {
        ValidateHotkeys(true, false);
        ScheduleSettingsApply();
    });
    recording_shortcut_block.Children().Append(recording_hotkey_input_);
    recording_hotkey_validation_ = Text(L"", 10.5, danger_brush_);
    recording_hotkey_validation_.TextWrapping(TextWrapping::Wrap);
    recording_hotkey_validation_.Visibility(Visibility::Collapsed);
    recording_shortcut_block.Children().Append(recording_hotkey_validation_);
    recording_shortcut_surface.Child(recording_shortcut_block);
    shortcuts_content.Children().Append(recording_shortcut_surface);

    Border screenshot_surface;
    screenshot_surface.Background(Brush(0xFF202023));
    screenshot_surface.BorderBrush(card_border_brush);
    screenshot_surface.BorderThickness(Thickness{1, 1, 1, 1});
    screenshot_surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    screenshot_surface.Padding(Thickness{10, 9, 10, 9});
    StackPanel screenshot_block;
    screenshot_block.Spacing(6);
    Grid screenshot_shortcut_header;
    AddColumn(screenshot_shortcut_header, 1, GridUnitType::Star);
    AddColumn(screenshot_shortcut_header, 0, GridUnitType::Auto);
    screenshot_hotkey_label_ = Text(L"Снимок экрана", 12.5, primary_text_brush);
    screenshot_hotkey_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    screenshot_hotkey_label_.VerticalAlignment(VerticalAlignment::Center);
    screenshot_shortcut_header.Children().Append(screenshot_hotkey_label_);
    screenshot_hotkey_toggle_ = ToggleSwitch{};
    StyleToggle(screenshot_hotkey_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    screenshot_hotkey_toggle_.Toggled([this](auto&&, auto&&) {
        if (updating_ui_) return;
        ValidateHotkeys(true, false);
        ScheduleSettingsApply();
    });
    Grid::SetColumn(screenshot_hotkey_toggle_, 1);
    screenshot_shortcut_header.Children().Append(screenshot_hotkey_toggle_);
    screenshot_block.Children().Append(screenshot_shortcut_header);
    screenshot_hotkey_input_ = TextBox{};
    StyleInput(screenshot_hotkey_input_);
    screenshot_hotkey_input_.Height(36);
    screenshot_hotkey_input_.IsReadOnly(true);
    screenshot_hotkey_input_.PlaceholderText(L"Нажмите сочетание");
    screenshot_hotkey_input_.KeyDown({this, &MainWindow::ScreenshotHotkey_KeyDown});
    screenshot_hotkey_input_.LostFocus([this](auto&&, auto&&) {
        ValidateHotkeys(true, false);
        ScheduleSettingsApply();
    });
    screenshot_block.Children().Append(screenshot_hotkey_input_);
    screenshot_hotkey_validation_ = Text(L"", 10.5, danger_brush_);
    screenshot_hotkey_validation_.TextWrapping(TextWrapping::Wrap);
    screenshot_hotkey_validation_.Visibility(Visibility::Collapsed);
    screenshot_block.Children().Append(screenshot_hotkey_validation_);
    screenshot_surface.Child(screenshot_block);
    shortcuts_content.Children().Append(screenshot_surface);

    shortcuts_expander_ = Expander{};
    shortcuts_expander_.Header(shortcuts_header);
    shortcuts_expander_.Content(shortcuts_content);
    shortcuts_expander_.IsExpanded(false);
    shortcuts_expander_.HorizontalAlignment(HorizontalAlignment::Stretch);
    shortcuts_expander_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    shortcuts_expander_.Margin(Thickness{0, 4, 0, 0});
    input_section.Children().Append(shortcuts_expander_);

    Grid performance_header;
    AddColumn(performance_header, 1, GridUnitType::Star);
    AddColumn(performance_header, 0, GridUnitType::Auto);
    performance_overlay_label_ = Text(L"Оверлей производительности", 12.5, secondary_text_brush_);
    performance_overlay_label_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    performance_header.Children().Append(performance_overlay_label_);
    performance_overlay_summary_ = Text(L"Alt+R · 6 метрик", 11, success_brush_);
    performance_overlay_summary_.Margin(Thickness{10, 0, 4, 0});
    performance_overlay_summary_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(performance_overlay_summary_, 1);
    performance_header.Children().Append(performance_overlay_summary_);

    StackPanel performance_content;
    performance_content.Spacing(8);
    performance_content.Padding(Thickness{4, 6, 0, 8});
    performance_content.HorizontalAlignment(HorizontalAlignment::Stretch);
    Grid position_row;
    AddColumn(position_row, 1, GridUnitType::Star);
    AddColumn(position_row, 160, GridUnitType::Pixel);
    performance_position_label_ = Text(L"Положение", 12, primary_text_brush);
    performance_position_label_.VerticalAlignment(VerticalAlignment::Center);
    position_row.Children().Append(performance_position_label_);
    performance_position_selector_ = ComboBox{};
    StyleInput(performance_position_selector_);
    performance_position_selector_.Items().Append(ComboItem(L"Справа сверху"));
    performance_position_selector_.Items().Append(ComboItem(L"Справа снизу"));
    performance_position_selector_.SelectedIndex(0);
    performance_position_selector_.SelectionChanged([this](auto&&, auto&&) { ScheduleSettingsApply(); });
    Grid::SetColumn(performance_position_selector_, 1);
    position_row.Children().Append(performance_position_selector_);
    performance_content.Children().Append(position_row);

    Grid opacity_header;
    AddColumn(opacity_header, 1, GridUnitType::Star);
    AddColumn(opacity_header, 0, GridUnitType::Auto);
    performance_opacity_label_ = Text(L"Прозрачность фона", 12, primary_text_brush);
    opacity_header.Children().Append(performance_opacity_label_);
    performance_opacity_value_ = Text(L"92%", 11.5, success_brush_);
    Grid::SetColumn(performance_opacity_value_, 1);
    opacity_header.Children().Append(performance_opacity_value_);
    performance_content.Children().Append(opacity_header);
    performance_opacity_slider_ = Slider{};
    performance_opacity_slider_.Minimum(55);
    performance_opacity_slider_.Maximum(100);
    performance_opacity_slider_.StepFrequency(1);
    performance_opacity_slider_.Value(92);
    performance_opacity_slider_.Margin(Thickness{0, 0, 0, 2});
    StyleSlider(performance_opacity_slider_, accent_brush_, success_brush_, accent_dark_brush);
    performance_opacity_slider_.ValueChanged([this](auto&&, auto const& args) {
        if (performance_opacity_value_) {
            performance_opacity_value_.Text(std::to_wstring(static_cast<unsigned int>(std::lround(args.NewValue()))) + L"%");
        }
        ScheduleSettingsApply();
    });
    performance_content.Children().Append(performance_opacity_slider_);

    const auto metric_check = [&](const StackPanel& parent, std::wstring_view label, CheckBox& target) {
        target = CheckBox{};
        target.Content(winrt::box_value(winrt::hstring{label}));
        target.FontFamily(PublicSans());
        target.FontSize(12);
        target.IsChecked(true);
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
    gpu_metrics.Spacing(2);
    StackPanel system_metrics;
    system_metrics.Spacing(2);
    metric_check(gpu_metrics, L"Загрузка GPU", performance_gpu_usage_);
    metric_check(gpu_metrics, L"Температура GPU", performance_gpu_temperature_);
    metric_check(gpu_metrics, L"Частота GPU", performance_gpu_clock_);
    metric_check(gpu_metrics, L"VRAM", performance_gpu_memory_);
    metric_check(system_metrics, L"Загрузка CPU", performance_cpu_usage_);
    metric_check(system_metrics, L"Оперативная память", performance_memory_);
    Grid::SetColumn(system_metrics, 1);
    performance_metrics.Children().Append(gpu_metrics);
    performance_metrics.Children().Append(system_metrics);
    performance_content.Children().Append(performance_metrics);

    performance_overlay_expander_ = Expander{};
    performance_overlay_expander_.Header(performance_header);
    performance_overlay_expander_.Content(performance_content);
    performance_overlay_expander_.IsExpanded(false);
    performance_overlay_expander_.HorizontalAlignment(HorizontalAlignment::Stretch);
    performance_overlay_expander_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    performance_overlay_expander_.Margin(Thickness{0, 8, 0, 0});
    input_section.Children().Append(performance_overlay_expander_);

    const auto storage_section = create_section(storage_section_title_, L"ХРАНИЛИЩЕ");

    output_format_selector_ = ComboBox{};
    StyleInput(output_format_selector_);
    output_format_selector_.HorizontalAlignment(HorizontalAlignment::Stretch);
    output_format_selector_.Items().Append(ComboItem(L"MKV"));
    output_format_selector_.Items().Append(ComboItem(L"MP4"));
    output_format_selector_.SelectionChanged({this, &MainWindow::OutputFormatSelector_SelectionChanged});
    add_field(storage_section, output_format_label_, L"Формат видео", output_format_selector_);
    output_format_description_ = Text(L"Широко совместим с приложениями и видеоредакторами.", 11.5,
                                       secondary_text_brush_);
    output_format_description_.TextWrapping(TextWrapping::Wrap);
    output_format_description_.Margin(Thickness{0, -8, 0, 14});
    storage_section.Children().Append(output_format_description_);

    Border output_surface;
    output_surface.Height(40);
    output_surface.Background(card_hover_brush);
    output_surface.BorderBrush(card_border_brush);
    output_surface.BorderThickness(Thickness{1, 1, 1, 1});
    output_surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    output_surface.Padding(Thickness{12, 0, 12, 0});
    output_directory_text_ = Text(L"", 12, primary_text_brush);
    output_directory_text_.VerticalAlignment(VerticalAlignment::Center);
    output_directory_text_.TextTrimming(TextTrimming::CharacterEllipsis);
    output_directory_text_.TextWrapping(TextWrapping::NoWrap);
    output_surface.Child(output_directory_text_);
    add_field(storage_section, output_label_, L"Папка сохранения", output_surface);
    storage_space_text_ = Text(L"Свободно: проверка...", 11.5, secondary_text_brush_);
    storage_space_text_.Margin(Thickness{0, -8, 0, 14});
    storage_section.Children().Append(storage_space_text_);
    open_folder_button_ = ActionButton(L"Открыть папку", quiet_button_brush, primary_text_brush);
    open_folder_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    open_folder_button_.Click({this, &MainWindow::OpenFolder_Click});
    open_logs_button_ = ActionButton(L"Открыть логи", quiet_button_brush, primary_text_brush);
    open_logs_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    open_logs_button_.Click({this, &MainWindow::OpenLogs_Click});
    Grid storage_actions;
    AddColumn(storage_actions, 1, GridUnitType::Star);
    AddColumn(storage_actions, 1, GridUnitType::Star);
    open_folder_button_.Margin(Thickness{0, 0, 6, 0});
    open_logs_button_.Margin(Thickness{6, 0, 0, 0});
    storage_actions.Children().Append(open_folder_button_);
    Grid::SetColumn(open_logs_button_, 1);
    storage_actions.Children().Append(open_logs_button_);
    storage_section.Children().Append(storage_actions);

    const auto updates_section = create_section(updates_section_title_, L"UPDATES");
    update_version_text_ = Text(L"OpenReplay " + openreplay::FromUtf8(openreplay::kVersion) +
                                    (openreplay::kReleaseChannel == "dev" ? L" · Dev" : L" · Stable"),
                                13, primary_text_brush);
    update_version_text_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    updates_section.Children().Append(update_version_text_);
    update_status_text_ = Text(L"Updates are checked in the background.", 11.5, secondary_text_brush_);
    update_status_text_.TextWrapping(TextWrapping::Wrap);
    update_status_text_.Margin(Thickness{0, 5, 0, 12});
    updates_section.Children().Append(update_status_text_);

    Grid automatic_updates_row;
    AddColumn(automatic_updates_row, 1, GridUnitType::Star);
    AddColumn(automatic_updates_row, 0, GridUnitType::Auto);
    automatic_updates_row.Margin(Thickness{0, 0, 0, 12});
    automatic_updates_label_ = Text(L"Check and download automatically", 12.5, primary_text_brush);
    automatic_updates_label_.VerticalAlignment(VerticalAlignment::Center);
    automatic_updates_row.Children().Append(automatic_updates_label_);
    automatic_updates_toggle_ = ToggleSwitch{};
    StyleToggle(automatic_updates_toggle_, accent_brush_, success_brush_, accent_dark_brush,
                primary_text_brush, quiet_button_brush, muted_brush);
    automatic_updates_toggle_.HorizontalAlignment(HorizontalAlignment::Right);
    automatic_updates_toggle_.Toggled([this](auto&&, auto&&) {
        if (updating_ui_) return;
        ScheduleSettingsApply();
        if (automatic_updates_toggle_.IsOn()) CheckForUpdatesAsync(false, true);
    });
    Grid::SetColumn(automatic_updates_toggle_, 1);
    automatic_updates_row.Children().Append(automatic_updates_toggle_);
    updates_section.Children().Append(automatic_updates_row);

    Grid update_actions;
    AddColumn(update_actions, 1, GridUnitType::Star);
    AddColumn(update_actions, 1, GridUnitType::Star);
    check_update_button_ = ActionButton(L"Check now", quiet_button_brush, primary_text_brush);
    check_update_button_.Margin(Thickness{0, 0, 6, 0});
    check_update_button_.Click({this, &MainWindow::CheckUpdate_Click});
    update_actions.Children().Append(check_update_button_);
    update_action_button_ = ActionButton(L"Download update", accent_brush_, near_black_green_brush);
    update_action_button_.Margin(Thickness{6, 0, 0, 0});
    update_action_button_.Visibility(Visibility::Collapsed);
    update_action_button_.Click({this, &MainWindow::UpdateAction_Click});
    Grid::SetColumn(update_action_button_, 1);
    update_actions.Children().Append(update_action_button_);
    updates_section.Children().Append(update_actions);
    release_notes_button_ = ActionButton(L"Release notes", quiet_button_brush, primary_text_brush);
    release_notes_button_.HorizontalAlignment(HorizontalAlignment::Stretch);
    release_notes_button_.Margin(Thickness{0, 8, 0, 0});
    release_notes_button_.Visibility(Visibility::Collapsed);
    release_notes_button_.Click({this, &MainWindow::OpenReleaseNotes_Click});
    updates_section.Children().Append(release_notes_button_);

    settings_scroll.Content(form);
    settings_panel_.Children().Append(settings_scroll);

    Border save_status_surface;
    save_status_surface.Background(near_black_green_brush);
    save_status_surface.BorderBrush(accent_dark_brush);
    save_status_surface.BorderThickness(Thickness{1, 1, 1, 1});
    save_status_surface.CornerRadius(CornerRadius{8, 8, 8, 8});
    save_status_surface.Padding(Thickness{12, 9, 12, 9});
    settings_save_status_ = Text(L"Changes save automatically", 11.5, success_brush_);
    settings_save_status_.HorizontalAlignment(HorizontalAlignment::Center);
    save_status_surface.Child(settings_save_status_);
    Grid::SetRow(save_status_surface, 2);
    settings_panel_.Children().Append(save_status_surface);
    layout.Children().Append(settings_panel_);

    Grid footer;
    footer.Padding(Thickness{20, 0, 20, 0});
    footer.Background(panel_brush);
    Grid::SetRow(footer, 3);
    hint_text_ = Text(L"ALT + Z   ЗАКРЫТЬ ПАНЕЛЬ", 11, muted_brush);
    hint_text_.CharacterSpacing(45);
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

    if (!hotkey_registered_) {
        ShowNotification(L"Alt+Z уже используется другим приложением. Закройте NVIDIA Overlay или измените его сочетание.", true);
    }
    if (!performance_hotkey_registered_) {
        ShowNotification(L"Alt+R уже используется другим приложением. Отключите конфликтующий performance overlay.", true);
    }
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

    const auto primary = Brush(0xFFFFFFFF);
    const auto quiet = Brush(0xFF3F3F46);
    const auto muted = Brush(0xFF71717A);
    const auto accent_dark = Brush(0xFF007F45);
    if (replay_hotkey_draft_.empty()) replay_hotkey_draft_.push_back({false, {}, 15});

    for (std::size_t index = 0; index < replay_hotkey_draft_.size(); ++index) {
        const auto& binding = replay_hotkey_draft_[index];
        Border surface;
        surface.Background(Brush(0xFF202023));
        surface.BorderBrush(Brush(0xFF3F3F46));
        surface.BorderThickness(Thickness{1, 1, 1, 1});
        surface.CornerRadius(CornerRadius{8, 8, 8, 8});
        surface.Padding(Thickness{10, 9, 10, 9});

        StackPanel block;
        block.Spacing(6);
        Grid header;
        AddColumn(header, 1, GridUnitType::Star);
        AddColumn(header, 0, GridUnitType::Auto);
        AddColumn(header, 6, GridUnitType::Pixel);
        AddColumn(header, 30, GridUnitType::Pixel);
        auto label = Text((english_ ? L"Replay " : L"Повтор ") + std::to_wstring(index + 1), 12.5, primary);
        label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        label.VerticalAlignment(VerticalAlignment::Center);
        header.Children().Append(label);
        replay_hotkey_labels_.push_back(label);

        ToggleSwitch toggle;
        StyleToggle(toggle, accent_brush_, success_brush_, accent_dark, primary, quiet, muted);
        toggle.IsOn(binding.enabled);
        toggle.Toggled([this](auto&&, auto&&) {
            ValidateHotkeys(true, false);
            ScheduleSettingsApply();
        });
        Grid::SetColumn(toggle, 1);
        header.Children().Append(toggle);
        replay_hotkey_toggles_.push_back(toggle);

        auto remove = ActionButton(L"×", quiet, primary);
        remove.Width(30);
        remove.Height(30);
        remove.MinHeight(30);
        remove.Padding(Thickness{});
        remove.FontSize(17);
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
        TextBox input;
        StyleInput(input);
        input.Height(36);
        input.IsReadOnly(true);
        input.Text(openreplay::FromUtf8(binding.chord));
        input.PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
        input.KeyDown({this, &MainWindow::ReplayHotkey_KeyDown});
        row.Children().Append(input);
        replay_hotkey_inputs_.push_back(input);

        NumberBox duration;
        StyleInput(duration);
        duration.Height(36);
        duration.Minimum(15);
        duration.Maximum(1200);
        duration.SmallChange(15);
        duration.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
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
        replay_hotkey_inputs_[index].BorderBrush(error.empty() ? Brush(0xFF3F3F46) : danger_brush_);
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
        screenshot_hotkey_input_.BorderBrush(screenshot_error.empty() ? Brush(0xFF3F3F46) : danger_brush_);
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
        recording_hotkey_input_.BorderBrush(recording_error.empty() ? Brush(0xFF3F3F46) : danger_brush_);
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
    if (CreateProcessW(host_path.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process)) {
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

void MainWindow::ShowOverlay() {
    if (visible_) return;
    const auto window = GetWindowHandle();
    const auto area = openreplay::ui::ForegroundMonitorArea(window);
    if (!area.monitor) return;
    auto style = GetWindowLongPtrW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(window, GWL_STYLE, style);
    const auto monitor_width = area.bounds.right - area.bounds.left;
    const auto dpi = openreplay::ui::WindowDpi(window);
    auto panel_width = openreplay::ui::ScaleForDpi(520, dpi);
    if (panel_width > monitor_width) panel_width = monitor_width;
    SetWindowPos(window, HWND_TOPMOST, area.bounds.left, area.bounds.top,
                 panel_width, area.bounds.bottom - area.bounds.top, SWP_FRAMECHANGED);
    if (!AnimateWindow(window, 170, AW_ACTIVATE | AW_SLIDE | AW_HOR_POSITIVE)) {
        ShowWindow(window, SW_SHOW);
    }
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SettingsButton().Focus(FocusState::Programmatic);
    visible_ = true;
    PollStatus();
}

void MainWindow::HideOverlay() {
    if (!visible_) return;
    if (!AnimateWindow(GetWindowHandle(), 130, AW_HIDE | AW_SLIDE | AW_HOR_NEGATIVE)) {
        ShowWindow(GetWindowHandle(), SW_HIDE);
    }
    visible_ = false;
}

void MainWindow::ToggleOverlay() {
    if (visible_) HideOverlay();
    else ShowOverlay();
}

void MainWindow::ShowSettings(bool show) {
    if (!show && settings_apply_pending_) {
        settings_apply_timer_.Stop();
        ApplySettingsFromControls(false);
    }
    settings_visible_ = show;
    DashboardPanel().Visibility(show ? Visibility::Collapsed : Visibility::Visible);
    SettingsPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    if (show) {
        LoadSettingsIntoControls();
        MonitorSelector().Focus(FocusState::Programmatic);
    } else {
        SettingsButton().Focus(FocusState::Programmatic);
    }
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
    performance_opacity_slider_.Value(settings_.performance_overlay_opacity);
    performance_gpu_usage_.IsChecked(settings_.performance_show_gpu_usage);
    performance_gpu_temperature_.IsChecked(settings_.performance_show_gpu_temperature);
    performance_gpu_clock_.IsChecked(settings_.performance_show_gpu_clock);
    performance_gpu_memory_.IsChecked(settings_.performance_show_gpu_memory);
    performance_cpu_usage_.IsChecked(settings_.performance_show_cpu_usage);
    performance_memory_.IsChecked(settings_.performance_show_memory);
    MicrophoneToggle().IsOn(settings_.microphone_enabled);
    CursorToggle().IsOn(settings_.capture_cursor);
    start_with_windows_toggle_.IsOn(settings_.start_with_windows);
    automatic_updates_toggle_.IsOn(settings_.automatic_updates);
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
    if (audio_settings_reconciled_) {
        try {
            settings_store_.Save(settings_);
            audio_restore_notification_pending_ = true;
            QueueHostSettingsApply(true);
        } catch (...) {
        }
        audio_settings_reconciled_ = false;
    }
    if (openreplay::ReplayOutputSettingsEqual(host_settings_, settings_)) RegisterConfigurableHotkeys();
    UpdateReplaySizeEstimate();
    UpdateStorageSpace();
    UpdatePerformanceOverlaySummary();
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
            Border surface;
            surface.Background(Brush(0xFF202023));
            surface.CornerRadius(CornerRadius{7, 7, 7, 7});
            surface.Padding(Thickness{9, 7, 9, 7});
            StackPanel content;
            content.Spacing(4);
            CheckBox check;
            check.FontFamily(PublicSans());
            check.FontSize(12.5);
            check.Content(winrt::box_value(winrt::hstring{device.name}));
            const auto configured = FindDevice(selected, device.id);
            check.IsChecked(configured != nullptr);
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
            level_track.Background(Brush(0xFF3F3F46));
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
            Slider volume;
            volume.Minimum(0);
            volume.Maximum(100);
            volume.StepFrequency(1);
            volume.Value(configured ? configured->volume_percent : 100);
            volume.MinHeight(32);
            volume.VerticalAlignment(VerticalAlignment::Center);
            volume.IsEnabled(configured != nullptr);
            StyleSlider(volume, accent_brush_, success_brush_, Brush(0xFF007F45));
            Grid::SetColumn(volume, 2);
            controls.Children().Append(volume);
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

    auto item = ComboBoxItem{};
    item.Content(winrt::box_value(winrt::hstring{label}));
    self->MonitorSelector().Items().Append(item);
    self->monitor_ids_.push_back(openreplay::ToUtf8(id));
    return TRUE;
}

void MainWindow::ApplyLanguage() {
    const auto set = [](auto control, std::wstring_view text) { control.Text(winrt::hstring{text}); };
    set(SettingsButtonText(), english_ ? L"Settings" : L"Настройки");
    set(DashboardTitle(), english_ ? L"Capture center" : L"Центр захвата");
    set(DashboardSubtitle(), english_ ? L"Status and quick actions"
                                       : L"Состояние и быстрые действия");
    set(ReplayTitle(), english_ ? L"Instant replay" : L"Мгновенный повтор");
    set(ReplayDescription(), english_ ? L"Save the last moments without interrupting your game."
                                      : L"Сохранит последние моменты без остановки игры.");
    set(RecordingTitle(), english_ ? L"Session recording" : L"Запись сессии");
    set(ScreenshotTitle(), english_ ? L"Screenshot" : L"Снимок экрана");
    set(ScreenshotStateText(), settings_.screenshot_hotkey_enabled
        ? L"PNG · " + openreplay::FromUtf8(settings_.screenshot_hotkey_chord) : L"PNG");
    set(ScreenshotDescription(), english_ ? L"Saved at the selected monitor's native resolution."
                                           : L"Сохранение в исходном разрешении выбранного монитора.");
    set(SettingsTitle(), english_ ? L"Settings" : L"Настройки");
    set(SettingsSubtitle(), english_ ? L"Changes save automatically"
                                       : L"Изменения сохраняются автоматически");
    set(source_section_title_, english_ ? L"SOURCE" : L"ИСТОЧНИК");
    set(video_section_title_, english_ ? L"VIDEO" : L"ВИДЕО");
    set(input_section_title_, english_ ? L"INTERFACE AND INPUT" : L"ИНТЕРФЕЙС И ВВОД");
    set(storage_section_title_, english_ ? L"STORAGE" : L"ХРАНИЛИЩЕ");
    set(updates_section_title_, english_ ? L"UPDATES" : L"ОБНОВЛЕНИЯ");
    set(MonitorLabel(), english_ ? L"Monitor" : L"Монитор");
    set(DesktopAudioLabel(), english_ ? L"Desktop audio outputs" : L"Системный звук");
    set(MicrophoneDevicesLabel(), english_ ? L"Microphones and inputs" : L"Микрофоны и входы");
    set(audio_devices_hint_, english_ ? L"Select multiple devices, up to 5 active audio sources in total."
                                      : L"Можно выбрать несколько устройств, суммарно до 5 активных источников.");
    set(ReplayDurationLabel(), english_ ? L"Replay duration" : L"Длительность повтора");
    set(QualityLabel(), english_ ? L"Quality preset" : L"Пресет качества");
    set(BitrateLabel(), english_ ? L"Target bitrate" : L"Целевой битрейт");
    set(replay_size_label_, english_ ? L"REPLAY ESTIMATE" : L"ОЦЕНКА ПОВТОРА");
    set(CodecLabel(), english_ ? L"Codec" : L"Кодек");
    set(FpsLabel(), english_ ? L"Frame rate" : L"Частота кадров");
    set(LanguageLabel(), english_ ? L"Language" : L"Язык");
    set(OutputLabel(), english_ ? L"Output folder" : L"Папка сохранения");
    set(OutputFormatLabel(), english_ ? L"Video format" : L"Формат видео");
    set(microphone_label_, english_ ? L"Microphone" : L"Микрофон");
    set(cursor_label_, english_ ? L"Capture cursor" : L"Захватывать курсор");
    set(start_with_windows_label_, english_ ? L"Start with Windows" : L"Запускать вместе с Windows");
    set(automatic_updates_label_, english_ ? L"Check and download automatically"
                                           : L"Проверять и скачивать автоматически");
    set(shortcuts_label_, english_ ? L"Shortcuts" : L"Хоткеи");
    set(recording_hotkey_label_, english_ ? L"Session recording" : L"Запись сессии");
    recording_hotkey_input_.PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
    set(screenshot_hotkey_label_, english_ ? L"Screenshot" : L"Снимок экрана");
    screenshot_hotkey_input_.PlaceholderText(english_ ? L"Press shortcut" : L"Нажмите сочетание");
    set(performance_overlay_label_, english_ ? L"Performance overlay" : L"Оверлей производительности");
    set(performance_position_label_, english_ ? L"Position" : L"Положение");
    set(performance_opacity_label_, english_ ? L"Background opacity" : L"Прозрачность фона");
    performance_gpu_usage_.Content(winrt::box_value(english_ ? L"GPU usage" : L"Загрузка GPU"));
    performance_gpu_temperature_.Content(winrt::box_value(english_ ? L"GPU temperature" : L"Температура GPU"));
    performance_gpu_clock_.Content(winrt::box_value(english_ ? L"GPU clock" : L"Частота GPU"));
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
    set(HintText(), english_ ? L"ALT + Z   CLOSE PANEL" : L"ALT + Z   ЗАКРЫТЬ ПАНЕЛЬ");

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
    const auto replay_action = english_ ? L"Save last " + std::to_wstring(settings_.replay_seconds) + L" sec"
                                        : L"Сохранить последние " + std::to_wstring(settings_.replay_seconds) + L" сек";
    SaveReplayButton().Content(winrt::box_value(replay_action));
    ScreenshotButton().Content(winrt::box_value(english_ ? L"Take screenshot" : L"Сделать снимок"));
    if (!settings_apply_pending_ && !settings_reload_in_flight_ && !settings_reload_requested_) {
        UpdateSettingsSaveStatus(english_ ? L"Changes save automatically"
                                           : L"Изменения сохраняются автоматически");
    }
    back_button_.Content(winrt::box_value(english_ ? L"← Back" : L"← Назад"));
    open_folder_button_.Content(winrt::box_value(english_ ? L"Recordings" : L"Записи"));
    open_logs_button_.Content(winrt::box_value(english_ ? L"Logs" : L"Логи"));
    check_update_button_.Content(winrt::box_value(english_ ? L"Check now" : L"Проверить"));
    release_notes_button_.Content(winrt::box_value(english_ ? L"Release notes" : L"Что нового"));
    RecordingButton().Content(winrt::box_value(recording_ ? (english_ ? L"Stop recording" : L"Остановить запись")
                                                          : (english_ ? L"Start recording" : L"Начать запись")));

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

    const auto format = mp4 ? L"MP4" : L"MKV";
    RecordingDescription().Text(
        english_ ? std::wstring{format} + L"  •  Game, desktop, and microphone on separate tracks"
                 : std::wstring{format} + L"  •  Игра, система и микрофон на отдельных дорожках");
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
    LayoutNotifications();
    ReconcileAudioDevices();
    UpdateStorageSpace();
    if (settings_reload_in_flight_) return;
    const auto response = pipe_client_.Request("status", std::chrono::milliseconds{80});
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
    update_status_text_.Text(english_ ? L"Updates are checked in the background."
                                      : L"Обновления проверяются в фоне.");
    update_action_button_.Visibility(Visibility::Collapsed);
    release_notes_button_.Visibility(Visibility::Collapsed);
}

winrt::fire_and_forget MainWindow::CheckForUpdatesAsync(bool manual, bool automatic_download) {
    if (update_check_in_flight_ || update_download_in_flight_ || exiting_) co_return;
    update_check_in_flight_ = true;
    UpdateUpdateUi();
    const auto dispatcher = DispatcherQueue();
    const auto weak = get_weak();
    co_await winrt::resume_background();
    auto result = update_service_.Check();
    dispatcher.TryEnqueue([weak, result = std::move(result), manual, automatic_download]() mutable {
        if (const auto self = weak.get()) {
            self->CompleteUpdateCheck(std::move(result), manual, automatic_download);
        }
    });
}

void MainWindow::CompleteUpdateCheck(openreplay::ui::UpdateCheckResult result, bool manual,
                                     bool automatic_download) {
    update_check_in_flight_ = false;
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
            update_status_text_.Text(english_ ? L"You are using the latest stable version."
                                              : L"Установлена последняя стабильная версия.");
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
    co_await winrt::resume_background();
    auto result = update_service_.Download(manifest);
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
    if (!post_update_version_.empty()) {
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
    long long recording_seconds = 0;
    try { recording_seconds = std::stoll(Field(response, "recording_seconds")); } catch (...) {}
    RecordingStateText().Text(recording_ ? DurationText(recording_seconds)
                                         : (english_ ? L"Ready to record" : L"Готово к записи"));
    RecordingStateText().Foreground(recording_ ? danger_brush_ : secondary_text_brush_);
    RecordingButton().Content(winrt::box_value(recording_ ? (english_ ? L"Stop recording" : L"Остановить запись")
                                                          : (english_ ? L"Start recording" : L"Начать запись")));
    SaveReplayButton().IsEnabled(FieldIsTrue(response, "replay_requested") && state == "running");
    RecordingButton().IsEnabled(true);
    ScreenshotButton().IsEnabled(true);
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
        return;
    }
    constexpr std::uint64_t warning_bytes = 5ULL * 1000ULL * 1000ULL * 1000ULL;
    const bool low = available.QuadPart < warning_bytes;
    storage_space_text_.Text((english_ ? L"Free: " : L"Свободно: ") +
                             ByteSize(available.QuadPart, english_) + (english_ ? L" of " : L" из ") +
                             ByteSize(total.QuadPart, english_) +
                             (low ? (english_ ? L"  •  Low space" : L"  •  Мало места") : L""));
    storage_space_text_.Foreground(low ? danger_brush_ : secondary_text_brush_);
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
    const auto count = static_cast<unsigned int>(checked(performance_gpu_usage_) +
                                                  checked(performance_gpu_temperature_) +
                                                  checked(performance_gpu_clock_) +
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
    const auto path = std::filesystem::path{openreplay::FromUtf8(output)};
    const auto filename = path.filename().wstring();
    ShowToast(english_ ? L"Replay saved" : L"Повтор сохранён",
              filename.empty() ? path.wstring() : filename, false, false, true, path,
              NotificationChannel::ReplaySave);
    openreplay::ui::TraceStartup(L"Replay saved: " + path.wstring());
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
                            bool auto_hide, const std::filesystem::path& output, NotificationChannel channel) {
    ShowDesktopNotification(title, message, error, progress, auto_hide, output, channel);
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
                                          NotificationChannel channel) {
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
            WS_POPUP, 0, 0, 390, 118, nullptr, nullptr, GetModuleHandleW(nullptr), item.get());
        if (!item->window) return;
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
    notification->display_ms = auto_hide ? (error ? 6500U : (progress ? 10000U : 4500U)) : 0U;
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
        const auto width = openreplay::ui::ScaleForDpi(390, dpi);
        const auto height = openreplay::ui::ScaleForDpi(notification.output.empty() ? 96 : 126, dpi);
        notification.width = width;
        notification.height = height;
        notification.hidden_x = area.bounds.right + openreplay::ui::ScaleForDpi(8, dpi);
        if (y + height > available_bottom) {
            overflow.push_back(&notification);
            continue;
        }
        if (const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                                   openreplay::ui::ScaleForDpi(18, dpi),
                                                   openreplay::ui::ScaleForDpi(18, dpi))) {
            if (!SetWindowRgn(notification.window, region, TRUE)) DeleteObject(region);
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
    if (message == WM_LBUTTONUP && notification->output.empty()) {
        self->ToggleOverlay();
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (!dc) return 0;
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto dpi = openreplay::ui::WindowDpi(window);
        const auto scale = [dpi](int value) { return openreplay::ui::ScaleForDpi(value, dpi); };
        const auto background = CreateSolidBrush(RGB(24, 24, 27));
        if (background) {
            FillRect(dc, &bounds, background);
            DeleteObject(background);
        }
        const auto accent = CreateSolidBrush(notification->error ? RGB(255, 92, 103)
                                                                 : notification->progress ? RGB(0, 193, 106)
                                                                                          : RGB(0, 220, 130));
        RECT indicator{0, 0, scale(5), bounds.bottom};
        if (accent) {
            FillRect(dc, &indicator, accent);
            DeleteObject(accent);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        auto title_font = CreateFontW(-scale(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Public Sans");
        auto body_font = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Public Sans");
        const auto old_font = SelectObject(dc, title_font ? title_font : GetStockObject(DEFAULT_GUI_FONT));
        RECT title_rect{scale(20), scale(14), bounds.right - scale(18), scale(38)};
        DrawTextW(dc, notification->title.c_str(), -1, &title_rect, DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, body_font ? body_font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(161, 161, 170));
        RECT message_rect{scale(20), scale(42), bounds.right - scale(18),
                          notification->output.empty() ? bounds.bottom - scale(12) : scale(82)};
        DrawTextW(dc, notification->message.c_str(), -1, &message_rect, DT_WORDBREAK | DT_END_ELLIPSIS);
        if (!notification->output.empty()) {
            SetTextColor(dc, RGB(0, 220, 130));
            RECT open_rect{scale(20), scale(92), scale(145), scale(116)};
            RECT copy_rect{scale(160), scale(92), scale(300), scale(116)};
            DrawTextW(dc, self->english_ ? L"Open folder" : L"Открыть папку", -1, &open_rect, DT_SINGLELINE);
            DrawTextW(dc, self->english_ ? L"Copy path" : L"Копировать путь", -1, &copy_rect, DT_SINGLELINE);
        }
        SelectObject(dc, old_font);
        if (title_font) DeleteObject(title_font);
        if (body_font) DeleteObject(body_font);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_LBUTTONUP && !notification->output.empty()) {
        const auto x = GET_X_LPARAM(lparam);
        const auto y = GET_Y_LPARAM(lparam);
        const auto dpi = openreplay::ui::WindowDpi(window);
        if (y >= openreplay::ui::ScaleForDpi(88, dpi) && x < openreplay::ui::ScaleForDpi(150, dpi)) {
            self->OpenNotificationOutput(*notification);
        } else if (y >= openreplay::ui::ScaleForDpi(88, dpi) && x < openreplay::ui::ScaleForDpi(315, dpi)) {
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
    draft.performance_overlay_opacity =
        static_cast<std::uint32_t>(std::lround(performance_opacity_slider_.Value()));
    const auto checked = [](const Microsoft::UI::Xaml::Controls::CheckBox& box) {
        const auto value = box.IsChecked();
        return value && value.Value();
    };
    draft.performance_show_gpu_usage = checked(performance_gpu_usage_);
    draft.performance_show_gpu_temperature = checked(performance_gpu_temperature_);
    draft.performance_show_gpu_clock = checked(performance_gpu_clock_);
    draft.performance_show_gpu_memory = checked(performance_gpu_memory_);
    draft.performance_show_cpu_usage = checked(performance_cpu_usage_);
    draft.performance_show_memory = checked(performance_memory_);
    if (!draft.performance_show_gpu_usage && !draft.performance_show_gpu_temperature &&
        !draft.performance_show_gpu_clock && !draft.performance_show_gpu_memory &&
        !draft.performance_show_cpu_usage && !draft.performance_show_memory) {
        updating_ui_ = true;
        performance_gpu_usage_.IsChecked(true);
        updating_ui_ = false;
        draft.performance_show_gpu_usage = true;
        UpdatePerformanceOverlaySummary();
    }
    draft.microphone_enabled = MicrophoneToggle().IsOn();
    draft.capture_cursor = CursorToggle().IsOn();
    draft.start_with_windows = start_with_windows_toggle_.IsOn();
    draft.automatic_updates = automatic_updates_toggle_.IsOn();
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
    if (!settings_reload_requested_ || settings_reload_in_flight_) return;
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
    exiting_ = true;
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

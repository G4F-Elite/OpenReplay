#pragma once

#include "MainWindow.g.h"
#include "pch.h"

#include "PerformanceOverlay.h"
#include "UpdateService.h"

#include "openreplay/Ipc.h"
#include "openreplay/SettingsStore.h"

namespace winrt::OpenReplay::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    [[nodiscard]] bool HotkeyRegistered() const noexcept { return hotkey_registered_; }
    void ConfigureUpdateLaunch(std::wstring version, std::filesystem::path health_file);
    void BeginUpdateChecks();

    void Root_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void SettingsButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CloseButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void BackButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SaveReplay_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RecordingButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ScreenshotButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ReplayToggle_Toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ReplayDurationSlider_ValueChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void BitrateSlider_ValueChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void QualitySelector_SelectionChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OutputFormatSelector_SelectionChanged(
        IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void EstimateInput_Toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void AudioDeviceSelection_Changed(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ReplayHotkey_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void ScreenshotHotkey_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void RecordingHotkey_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void ReplayHotkeyAdd_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ReplayHotkeyRemove_Click(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenFolder_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenLogs_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void CheckUpdate_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void UpdateAction_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenReleaseNotes_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    enum class NotificationChannel {
        General,
        ReplaySave,
        Update,
    };

    struct DesktopNotification {
        MainWindow* owner{};
        HWND window{};
        NotificationChannel channel{NotificationChannel::General};
        std::wstring title;
        std::wstring message;
        std::filesystem::path output;
        bool error{false};
        bool progress{false};
        bool entering{true};
        bool animating{false};
        bool hiding{false};
        bool hide_timer_started{false};
        int start_x{};
        int start_y{};
        int target_x{};
        int target_y{};
        int hidden_x{};
        int width{};
        int height{};
        UINT display_ms{};
        UINT animation_duration_ms{180};
        std::chrono::steady_clock::time_point animation_started{};
    };

    struct AudioLevelIndicator {
        Microsoft::UI::Xaml::Controls::ColumnDefinition filled{nullptr};
        Microsoft::UI::Xaml::Controls::ColumnDefinition remaining{nullptr};
    };

    static constexpr int kOverlayHotkey = 0x4F52;
    static constexpr int kPerformanceOverlayHotkey = 0x4F53;
    static constexpr int kScreenshotHotkey = 0x4F54;
    static constexpr int kRecordingHotkey = 0x4F55;
    static constexpr int kReplayHotkeyBase = 0x4F60;
    static constexpr UINT kTrayMessage = WM_APP + 42;
    static constexpr UINT kTrayOpen = 2001;
    static constexpr UINT kTraySaveReplay = 2002;
    static constexpr UINT kTrayRecording = 2003;
    static constexpr UINT kTrayPerformance = 2004;
    static constexpr UINT kTrayExit = 2005;
    static constexpr UINT kTrayScreenshot = 2006;

    HWND GetWindowHandle();
    void BuildUi();
    void InitializeWindow();
    void EnsureHostRunning();
    void ShowOverlay();
    void HideOverlay();
    void ToggleOverlay();
    void ShowSettings(bool show);
    void LoadSettingsIntoControls();
    void EnumerateMonitors();
    void EnumerateAudioDevices();
    void ReconcileAudioDevices();
    void UpdateAudioDeviceSummaries();
    void UpdateAudioLevels(const openreplay::Response& response);
    void RebuildReplayHotkeyRows();
    std::vector<openreplay::ReplayHotkey> ReplayHotkeysFromControls() const;
    bool ValidateHotkeys(bool check_system_conflicts, bool show_notification);
    void RegisterConfigurableHotkeys();
    void UnregisterConfigurableHotkeys() noexcept;
    void RequestReplaySave(std::uint32_t replay_seconds);
    void ToggleRecording();
    void TakeScreenshot();
    winrt::fire_and_forget TakeScreenshotAsync();
    void CompleteScreenshot(openreplay::Response response);
    void ApplyLanguage();
    void UpdateQualityDescription();
    void UpdateOutputFormatDescription();
    void UpdateReplaySizeEstimate();
    void PollStatus();
    void ApplyResponse(const openreplay::Response& response);
    void ApplyReplaySaveStatus(const openreplay::Response& response);
    void UpdateStorageSpace();
    void UpdatePerformanceOverlaySummary();
    void UpdateUpdateUi();
    winrt::fire_and_forget CheckForUpdatesAsync(bool manual, bool automatic_download);
    void CompleteUpdateCheck(openreplay::ui::UpdateCheckResult result, bool manual, bool automatic_download);
    winrt::fire_and_forget DownloadUpdateAsync();
    void CompleteUpdateDownload(openreplay::ui::UpdateDownloadResult result);
    void ApplyDownloadedUpdate();
    void PublishUpdateHealth();
    void ScheduleSettingsApply();
    void ApplySettingsFromControls(bool show_errors);
    bool ReadSettingsFromControls(openreplay::Settings& draft, bool show_errors);
    void UpdateSettingsSaveStatus(std::wstring_view text, bool error = false);
    void QueueHostSettingsApply(bool audio_device_restored = false);
    void StartPendingHostSettingsApply();
    void ApplyPendingReplayToggle();
    winrt::fire_and_forget ReloadHostSettingsAsync(std::string command, openreplay::Settings snapshot);
    void CompleteHostSettingsApply(openreplay::Response response, openreplay::Settings snapshot);
    bool ApplyStartWithWindows(bool enabled);
    bool RunCommand(std::string_view command, std::wstring_view success_message = {});
    void ShowNotification(std::wstring_view message, bool error = false);
    void ShowToast(std::wstring_view title, std::wstring_view message, bool error, bool progress,
                   bool auto_hide, const std::filesystem::path& output = {},
                   NotificationChannel channel = NotificationChannel::General);
    void InitializeNotificationWindow();
    void ShowDesktopNotification(std::wstring_view title, std::wstring_view message, bool error,
                                  bool progress, bool auto_hide, const std::filesystem::path& output,
                                  NotificationChannel channel);
    void LayoutNotifications();
    void StartNotificationAnimation(DesktopNotification& notification, int target_x, int target_y,
                                    bool hiding, UINT duration_ms);
    void DismissNotification(DesktopNotification& notification);
    void DismissNotificationChannel(NotificationChannel channel);
    void RemoveNotification(HWND window);
    void OpenNotificationOutput(const DesktopNotification& notification);
    void CopyNotificationOutput(const DesktopNotification& notification);
    void TogglePerformanceOverlay();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void ExitApplication();

    static BOOL CALLBACK MonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM context);
    static LRESULT CALLBACK NotificationWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK WindowSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR subclass_id, DWORD_PTR context);

    Microsoft::UI::Xaml::Controls::Button SettingsButton() const { return settings_button_; }
    Microsoft::UI::Xaml::Controls::TextBlock SettingsButtonText() const { return settings_button_text_; }
    Microsoft::UI::Xaml::Controls::Grid DashboardPanel() const { return dashboard_panel_; }
    Microsoft::UI::Xaml::Controls::Grid SettingsPanel() const { return settings_panel_; }
    Microsoft::UI::Xaml::Controls::TextBlock DashboardTitle() const { return dashboard_title_; }
    Microsoft::UI::Xaml::Controls::TextBlock DashboardSubtitle() const { return dashboard_subtitle_; }
    Microsoft::UI::Xaml::Controls::TextBlock ReplayTitle() const { return replay_title_; }
    Microsoft::UI::Xaml::Controls::TextBlock ReplayDescription() const { return replay_description_; }
    Microsoft::UI::Xaml::Controls::TextBlock ReplayStateText() const { return replay_state_text_; }
    Microsoft::UI::Xaml::Controls::TextBlock RecordingTitle() const { return recording_title_; }
    Microsoft::UI::Xaml::Controls::TextBlock RecordingDescription() const { return recording_description_; }
    Microsoft::UI::Xaml::Controls::TextBlock RecordingStateText() const { return recording_state_text_; }
    Microsoft::UI::Xaml::Controls::TextBlock ScreenshotTitle() const { return screenshot_title_; }
    Microsoft::UI::Xaml::Controls::TextBlock ScreenshotDescription() const { return screenshot_description_; }
    Microsoft::UI::Xaml::Controls::TextBlock ScreenshotStateText() const { return screenshot_state_text_; }
    Microsoft::UI::Xaml::Controls::TextBlock SettingsTitle() const { return settings_title_; }
    Microsoft::UI::Xaml::Controls::TextBlock SettingsSubtitle() const { return settings_subtitle_; }
    Microsoft::UI::Xaml::Controls::TextBlock MonitorLabel() const { return monitor_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock DesktopAudioLabel() const { return desktop_audio_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock MicrophoneDevicesLabel() const { return microphone_devices_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock ReplayDurationLabel() const { return replay_duration_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock ReplayDurationValue() const { return replay_duration_value_; }
    Microsoft::UI::Xaml::Controls::TextBlock QualityLabel() const { return quality_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock BitrateLabel() const { return bitrate_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock BitrateValue() const { return bitrate_value_; }
    Microsoft::UI::Xaml::Controls::TextBlock OutputFormatLabel() const { return output_format_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock CodecLabel() const { return codec_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock FpsLabel() const { return fps_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock LanguageLabel() const { return language_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock OutputLabel() const { return output_label_; }
    Microsoft::UI::Xaml::Controls::TextBlock HintText() const { return hint_text_; }
    Microsoft::UI::Xaml::Controls::TextBlock EncoderText() const { return encoder_text_; }
    Microsoft::UI::Xaml::Controls::ToggleSwitch ReplayToggle() const { return replay_toggle_; }
    Microsoft::UI::Xaml::Controls::ToggleSwitch MicrophoneToggle() const { return microphone_toggle_; }
    Microsoft::UI::Xaml::Controls::ToggleSwitch CursorToggle() const { return cursor_toggle_; }
    Microsoft::UI::Xaml::Controls::ComboBox MonitorSelector() const { return monitor_selector_; }
    Microsoft::UI::Xaml::Controls::ComboBox QualitySelector() const { return quality_selector_; }
    Microsoft::UI::Xaml::Controls::ComboBox CodecSelector() const { return codec_selector_; }
    Microsoft::UI::Xaml::Controls::ComboBox FpsSelector() const { return fps_selector_; }
    Microsoft::UI::Xaml::Controls::ComboBox LanguageSelector() const { return language_selector_; }
    Microsoft::UI::Xaml::Controls::ComboBox OutputFormatSelector() const { return output_format_selector_; }
    Microsoft::UI::Xaml::Controls::Slider ReplayDurationSlider() const { return replay_duration_slider_; }
    Microsoft::UI::Xaml::Controls::Slider BitrateSlider() const { return bitrate_slider_; }
    Microsoft::UI::Xaml::Controls::TextBlock OutputDirectoryText() const { return output_directory_text_; }
    Microsoft::UI::Xaml::Controls::Button SaveReplayButton() const { return save_replay_button_; }
    Microsoft::UI::Xaml::Controls::Button RecordingButton() const { return recording_button_; }
    Microsoft::UI::Xaml::Controls::Button ScreenshotButton() const { return screenshot_button_; }
    HWND window_handle_{nullptr};
    bool hotkey_registered_{false};
    bool performance_hotkey_registered_{false};
    bool screenshot_hotkey_registered_{false};
    bool recording_hotkey_registered_{false};
    bool visible_{false};
    bool settings_visible_{false};
    bool updating_ui_{false};
    bool english_{true};
    bool exiting_{false};
    bool recording_{false};
    bool replay_save_pending_{false};
    bool replay_save_revision_initialized_{false};
    bool host_connected_{false};
    bool host_ever_connected_{false};
    bool host_error_shown_{false};
    bool audio_settings_reconciled_{false};
    bool settings_initialized_{false};
    bool settings_reload_in_flight_{false};
    bool settings_reload_requested_{false};
    bool settings_apply_pending_{false};
    bool replay_toggle_apply_pending_{false};
    bool replay_toggle_desired_{true};
    bool audio_restore_notification_pending_{false};
    bool low_space_warning_shown_{false};
    bool update_check_in_flight_{false};
    bool update_download_in_flight_{false};
    bool update_health_published_{false};
    int host_poll_failures_{0};
    std::uint64_t replay_save_revision_{0};
    std::chrono::steady_clock::time_point last_host_launch_attempt_{};
    std::chrono::steady_clock::time_point last_storage_update_{};
    std::chrono::steady_clock::time_point last_audio_device_scan_{};
    std::chrono::steady_clock::time_point audio_device_missing_since_{};
    std::string last_pipeline_error_;
    NOTIFYICONDATAW tray_icon_{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer status_timer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer settings_apply_timer_{nullptr};
    openreplay::SettingsStore settings_store_;
    openreplay::Settings settings_;
    openreplay::Settings host_settings_;
    openreplay::PipeClient pipe_client_;
    openreplay::ui::PerformanceOverlay performance_overlay_;
    openreplay::ui::UpdateService update_service_;
    openreplay::UpdateManifest available_update_;
    std::filesystem::path downloaded_update_;
    std::filesystem::path update_health_file_;
    std::wstring post_update_version_;
    std::vector<std::string> monitor_ids_;
    std::vector<std::string> desktop_audio_device_ids_;
    std::vector<std::string> microphone_device_ids_;
    std::vector<std::string> desktop_audio_device_names_;
    std::vector<std::string> microphone_device_names_;
    std::vector<Microsoft::UI::Xaml::Controls::CheckBox> desktop_audio_device_checks_;
    std::vector<Microsoft::UI::Xaml::Controls::CheckBox> microphone_device_checks_;
    std::vector<Microsoft::UI::Xaml::Controls::Slider> desktop_audio_device_volumes_;
    std::vector<Microsoft::UI::Xaml::Controls::Slider> microphone_device_volumes_;
    std::vector<AudioLevelIndicator> desktop_audio_device_levels_;
    std::vector<AudioLevelIndicator> microphone_device_levels_;
    std::vector<bool> replay_hotkey_registered_;
    std::vector<openreplay::ReplayHotkey> replay_hotkey_draft_;

    Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid dashboard_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid settings_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::Button settings_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button back_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button open_folder_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button open_logs_button_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock settings_button_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock dashboard_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock dashboard_subtitle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_description_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_state_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recording_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recording_description_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recording_state_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock screenshot_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock screenshot_description_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock screenshot_state_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock settings_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock settings_subtitle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock source_section_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock video_section_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock input_section_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock storage_section_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock monitor_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock desktop_audio_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock microphone_devices_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock audio_devices_hint_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock desktop_audio_summary_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock microphone_audio_summary_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_duration_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_duration_value_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock quality_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock quality_description_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock bitrate_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock bitrate_value_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_size_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_size_value_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock replay_size_detail_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock codec_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock fps_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock language_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock output_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock output_format_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock output_format_description_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock microphone_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock cursor_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock hint_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock encoder_text_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch replay_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch microphone_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch cursor_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch start_with_windows_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock start_with_windows_label_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox monitor_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox quality_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox codec_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox fps_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox language_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox output_format_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::Expander desktop_audio_devices_expander_{nullptr};
    Microsoft::UI::Xaml::Controls::Expander microphone_devices_expander_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel desktop_audio_devices_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel microphone_devices_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel replay_hotkeys_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::Button replay_hotkey_add_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Expander shortcuts_expander_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock shortcuts_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock shortcuts_summary_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock screenshot_hotkey_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock screenshot_hotkey_validation_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox screenshot_hotkey_input_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch screenshot_hotkey_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recording_hotkey_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock recording_hotkey_validation_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox recording_hotkey_input_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch recording_hotkey_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::Expander performance_overlay_expander_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock performance_overlay_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock performance_overlay_summary_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock performance_position_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock performance_opacity_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock performance_opacity_value_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox performance_position_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider performance_opacity_slider_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_gpu_usage_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_gpu_temperature_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_gpu_clock_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_gpu_memory_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_cpu_usage_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox performance_memory_{nullptr};
    std::vector<Microsoft::UI::Xaml::Controls::TextBlock> replay_hotkey_labels_;
    std::vector<Microsoft::UI::Xaml::Controls::TextBlock> replay_hotkey_units_;
    std::vector<Microsoft::UI::Xaml::Controls::TextBlock> replay_hotkey_validation_;
    std::vector<Microsoft::UI::Xaml::Controls::TextBox> replay_hotkey_inputs_;
    std::vector<Microsoft::UI::Xaml::Controls::NumberBox> replay_hotkey_durations_;
    std::vector<Microsoft::UI::Xaml::Controls::ToggleSwitch> replay_hotkey_toggles_;
    std::vector<Microsoft::UI::Xaml::Controls::Button> replay_hotkey_remove_buttons_;
    Microsoft::UI::Xaml::Controls::Slider replay_duration_slider_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider bitrate_slider_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock output_directory_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock storage_space_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock updates_section_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock automatic_updates_label_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock update_version_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock update_status_text_{nullptr};
    Microsoft::UI::Xaml::Controls::ToggleSwitch automatic_updates_toggle_{nullptr};
    Microsoft::UI::Xaml::Controls::Button check_update_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button update_action_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button release_notes_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button save_replay_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button recording_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button screenshot_button_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock settings_save_status_{nullptr};
    Microsoft::UI::Xaml::Media::Brush accent_brush_{nullptr};
    Microsoft::UI::Xaml::Media::Brush success_brush_{nullptr};
    Microsoft::UI::Xaml::Media::Brush secondary_text_brush_{nullptr};
    Microsoft::UI::Xaml::Media::Brush danger_brush_{nullptr};
    std::vector<std::unique_ptr<DesktopNotification>> notifications_;
    bool notification_class_registered_{false};
    bool private_font_loaded_{false};
};

}  // namespace winrt::OpenReplay::implementation

namespace winrt::OpenReplay::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::OpenReplay::factory_implementation

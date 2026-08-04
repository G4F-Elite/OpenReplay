#pragma once

#include "ClipLibraryWindow.g.h"
#include "pch.h"

namespace winrt::OpenReplay::implementation {

struct ClipLibraryWindow : ClipLibraryWindowT<ClipLibraryWindow> {
    ClipLibraryWindow();

    void Configure(std::filesystem::path output_directory, bool english, bool webhook_available,
                   HWND overlay_window,
                   std::function<void(std::filesystem::path, bool)> share_callback,
                   std::function<void(bool)> fullscreen_callback);
    void ActivateWindow();
    void HideWindow();
    void Shutdown();

private:
    void BuildUi();
    void Window_Closing(Microsoft::UI::Windowing::AppWindow const&,
                        Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args);
    void CloseButton_Click();
    void MaximizeButton_Click();
    void PositionBesideOverlay();
    void SetFullscreenLayout(bool fullscreen);
    void Root_KeyDown(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void ApplyLanguage();
    void RefreshClips();
    void ResetSelection();
    void UpdateSelectionStyles();
    void SelectClip(const std::filesystem::path& path);
    winrt::fire_and_forget OpenClipAsync(std::filesystem::path path);
    void TogglePlayback();
    void ShowPlaybackFeedback(bool playing);
    void UpdatePlaybackFeedback();
    void UpdatePlaybackUi();
    void ToggleFullscreen();
    void OpenSelectedClip();
    winrt::fire_and_forget DeleteSelectedClipAsync();
    void ShareSelectedClip(bool webhook);
    winrt::fire_and_forget LoadClipVisualsAsync(
        std::filesystem::path path,
        Microsoft::UI::Xaml::Controls::Image image,
        Microsoft::UI::Xaml::Controls::TextBlock metadata,
        Microsoft::UI::Xaml::Controls::TextBlock quality,
        std::uint64_t generation);

    bool english_{true};
    bool webhook_available_{false};
    bool closing_for_exit_{false};
    bool expanded_{false};
    std::filesystem::path output_directory_;
    std::filesystem::path selected_clip_;
    HWND overlay_window_{nullptr};
    std::function<void(std::filesystem::path, bool)> share_callback_;
    std::function<void(bool)> fullscreen_callback_;
    RECT restored_bounds_{};
    bool has_restored_bounds_{false};
    RECT pre_fullscreen_bounds_{};
    bool has_pre_fullscreen_bounds_{false};
    std::vector<std::pair<std::filesystem::path, Microsoft::UI::Xaml::Controls::Border>> clip_cards_;
    Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel clip_list_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock subtitle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock empty_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selected_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selected_metadata_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid header_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid content_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid player_column_{nullptr};
    Microsoft::UI::Xaml::Controls::ScrollViewer list_scroll_{nullptr};
    Microsoft::UI::Xaml::Controls::Border player_surface_{nullptr};
    Microsoft::UI::Xaml::Controls::Border playback_feedback_{nullptr};
    Microsoft::UI::Xaml::Controls::Border transport_surface_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel actions_{nullptr};
    Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button maximize_button_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon maximize_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::Button open_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button delete_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button discord_copy_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button webhook_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button play_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button fullscreen_button_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon play_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon playback_feedback_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon fullscreen_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider timeline_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider volume_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock playback_time_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox speed_selector_{nullptr};
    Microsoft::UI::Xaml::Controls::MediaPlayerElement player_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer playback_timer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer playback_feedback_timer_{nullptr};
    winrt::Windows::Media::Playback::MediaPlayer media_player_{nullptr};
    winrt::Windows::Media::Core::MediaSource player_source_{nullptr};
    std::chrono::steady_clock::time_point playback_feedback_started_{};
    std::uint64_t source_generation_{0};
    std::uint64_t clip_list_generation_{0};
    bool updating_timeline_{false};
    bool delete_in_progress_{false};
    bool fullscreen_{false};
};

}  // namespace winrt::OpenReplay::implementation

namespace winrt::OpenReplay::factory_implementation {

struct ClipLibraryWindow : ClipLibraryWindowT<ClipLibraryWindow, implementation::ClipLibraryWindow> {};

}  // namespace winrt::OpenReplay::factory_implementation

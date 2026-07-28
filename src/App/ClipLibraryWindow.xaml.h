#pragma once

#include "ClipLibraryWindow.g.h"
#include "pch.h"

namespace winrt::OpenReplay::implementation {

struct ClipLibraryWindow : ClipLibraryWindowT<ClipLibraryWindow> {
    ClipLibraryWindow();

    void Configure(std::filesystem::path output_directory, bool english, bool webhook_available,
                   std::function<void(std::filesystem::path, bool)> share_callback);
    void ActivateWindow();
    void Shutdown();

private:
    void BuildUi();
    void Window_Closing(Microsoft::UI::Windowing::AppWindow const&,
                       Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args);
    void ApplyLanguage();
    void RefreshClips();
    void UpdateSelectionStyles();
    void SelectClip(const std::filesystem::path& path);
    winrt::fire_and_forget OpenClipAsync(std::filesystem::path path);
    void TogglePlayback();
    void UpdatePlaybackUi();
    void ToggleFullscreen();
    void OpenSelectedClip();
    void ShareSelectedClip(bool webhook);
    winrt::fire_and_forget LoadClipVisualsAsync(
        std::filesystem::path path,
        Microsoft::UI::Xaml::Controls::Image image,
        Microsoft::UI::Xaml::Controls::TextBlock metadata);

    bool english_{true};
    bool webhook_available_{false};
    bool closing_for_exit_{false};
    std::filesystem::path output_directory_;
    std::filesystem::path selected_clip_;
    std::function<void(std::filesystem::path, bool)> share_callback_;
    std::vector<std::pair<std::filesystem::path, Microsoft::UI::Xaml::Controls::Border>> clip_cards_;
    Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel clip_list_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock subtitle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock empty_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selected_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selected_metadata_{nullptr};
    Microsoft::UI::Xaml::Controls::Button refresh_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button open_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button discord_copy_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button webhook_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button play_button_{nullptr};
    Microsoft::UI::Xaml::Controls::Button fullscreen_button_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon play_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon fullscreen_icon_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider timeline_{nullptr};
    Microsoft::UI::Xaml::Controls::Slider volume_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock playback_time_{nullptr};
    Microsoft::UI::Xaml::Controls::MediaPlayerElement player_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer playback_timer_{nullptr};
    winrt::Windows::Media::Playback::MediaPlayer media_player_{nullptr};
    winrt::Windows::Media::Core::MediaSource player_source_{nullptr};
    bool updating_timeline_{false};
};

}  // namespace winrt::OpenReplay::implementation

namespace winrt::OpenReplay::factory_implementation {

struct ClipLibraryWindow : ClipLibraryWindowT<ClipLibraryWindow, implementation::ClipLibraryWindow> {};

}  // namespace winrt::OpenReplay::factory_implementation

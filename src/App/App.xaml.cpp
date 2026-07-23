#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "StartupTrace.h"
#include "XamlMetaDataProvider.h"
#include "microsoft.ui.xaml.window.h"

namespace winrt::OpenReplay::implementation {

namespace {

struct LaunchOptions {
    bool background{false};
    std::wstring post_update_version;
    std::filesystem::path update_health;
};

LaunchOptions ParseLaunchOptions() {
    LaunchOptions result;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return result;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument{arguments[index]};
        if (argument == L"--background") result.background = true;
        else if (argument.starts_with(L"--post-update=")) {
            result.post_update_version = argument.substr(std::wstring_view{L"--post-update="}.size());
        } else if (argument.starts_with(L"--update-health=")) {
            result.update_health = argument.substr(std::wstring_view{L"--update-health="}.size());
        }
    }
    LocalFree(arguments);
    return result;
}

}  // namespace

App::App() {
    openreplay::ui::TraceStartup(L"Application constructor: begin");
    InitializeComponent();
    openreplay::ui::TraceStartup(L"Application constructor: XAML initialized");
    UnhandledException([](Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args) {
        openreplay::ui::TraceStartup(std::wstring{L"Unhandled XAML exception: "} + args.Message().c_str());
    });
    DebugSettings().XamlResourceReferenceFailed(
        [](Microsoft::UI::Xaml::DebugSettings const&,
           Microsoft::UI::Xaml::XamlResourceReferenceFailedEventArgs const& args) {
            openreplay::ui::TraceStartup(std::wstring{L"XAML resource failure: "} + args.Message().c_str());
        });
    openreplay::ui::TraceStartup(L"Application constructor: complete");
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args) {
    openreplay::ui::TraceStartup(L"OnLaunched: begin");
    auto main_window = winrt::make_self<MainWindow>();
    const auto options = ParseLaunchOptions();
    main_window->ConfigureUpdateLaunch(options.post_update_version, options.update_health);
    openreplay::ui::TraceStartup(L"OnLaunched: MainWindow created");
    window_ = *main_window;
    window_.Activate();
    (void)args;
    if (options.background || main_window->HotkeyRegistered()) {
        HWND window_handle{};
        winrt::check_hresult(window_.as<::IWindowNative>()->get_WindowHandle(&window_handle));
        ShowWindow(window_handle, SW_HIDE);
    }
    main_window->BeginUpdateChecks();
    openreplay::ui::TraceStartup(L"OnLaunched: complete");
}

}  // namespace winrt::OpenReplay::implementation

#include "XamlMetaDataProvider.g.cpp"

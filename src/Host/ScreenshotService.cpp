#include "ScreenshotService.h"

#include "openreplay/SettingsStore.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <format>
#include <memory>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace openreplay::host {
namespace {

std::wstring Timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    return std::format(L"{:04}-{:02}-{:02}_{:02}-{:02}-{:02}-{:03}", local.wYear, local.wMonth,
                       local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
}

struct DeleteDc {
    void operator()(HDC value) const noexcept { if (value) DeleteDC(value); }
};
struct DeleteBitmap {
    void operator()(HBITMAP value) const noexcept { if (value) DeleteObject(value); }
};

}  // namespace

bool ScreenshotService::CaptureMonitor(const std::filesystem::path& directory, const RECT& area,
                                       std::filesystem::path& output, std::string& error) const {
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        error = "Unable to create screenshot directory: " + filesystem_error.message();
        return false;
    }

    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    HDC screen = GetDC(nullptr);
    if (!screen) {
        error = "Unable to acquire the monitor device context";
        return false;
    }
    std::unique_ptr<std::remove_pointer_t<HDC>, DeleteDc> memory{CreateCompatibleDC(screen)};
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteBitmap> bitmap{CreateCompatibleBitmap(screen, width, height)};
    if (!memory || !bitmap) {
        ReleaseDC(nullptr, screen);
        error = "Unable to allocate screenshot surface";
        return false;
    }

    const auto previous = SelectObject(memory.get(), bitmap.get());
    const bool copied = BitBlt(memory.get(), 0, 0, width, height, screen, area.left, area.top,
                               SRCCOPY | CAPTUREBLT) != FALSE;
    SelectObject(memory.get(), previous);
    ReleaseDC(nullptr, screen);
    if (!copied) {
        error = "Windows refused the screenshot request";
        return false;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        if (uninitialize_com) CoUninitialize();
        error = "Unable to initialize the Windows image encoder";
        return false;
    }
    ComPtr<IWICBitmap> source;
    result = factory->CreateBitmapFromHBITMAP(bitmap.get(), nullptr, WICBitmapUseAlpha, &source);
    if (FAILED(result)) {
        if (uninitialize_com) CoUninitialize();
        error = "Unable to copy screenshot pixels";
        return false;
    }

    output = directory / (L"OpenReplay_" + Timestamp() + L"_Screenshot.png");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        std::filesystem::remove(output, filesystem_error);
        if (uninitialize_com) CoUninitialize();
        error = "Unable to encode screenshot as PNG";
        return false;
    }
    if (uninitialize_com) CoUninitialize();
    return true;
}

}  // namespace openreplay::host

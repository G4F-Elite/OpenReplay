#include "CaptureController.h"

#include "openreplay/Ipc.h"

#include <Windows.h>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <thread>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::set_terminate([] {
        openreplay::host::WriteHostLog("Fatal Host error: std::terminate was called");
        std::abort();
    });
    openreplay::host::WriteHostLog("Host process started");

    try {
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\OpenReplay.Host.Singleton.v1");
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instance) CloseHandle(instance);
        return 0;
    }

    openreplay::host::CaptureController controller;
    controller.Start();
    openreplay::PipeServer server{[&controller](const openreplay::Command& command) {
        return controller.Handle(command);
    }};
    server.Start();

    while (!controller.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds{200});

    server.Stop();
    controller.Stop();
    ReleaseMutex(instance);
    CloseHandle(instance);
    openreplay::host::WriteHostLog("Host process stopped normally");
    return 0;
    } catch (const std::exception& exception) {
        openreplay::host::WriteHostLog(std::string{"Fatal Host exception: "} + exception.what());
        return 1;
    } catch (...) {
        openreplay::host::WriteHostLog("Fatal Host exception: unknown error");
        return 1;
    }
}

#pragma once

#include "openreplay/Types.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace openreplay::ui {

class PerformanceOverlay final {
public:
    PerformanceOverlay() = default;
    ~PerformanceOverlay();

    PerformanceOverlay(const PerformanceOverlay&) = delete;
    PerformanceOverlay& operator=(const PerformanceOverlay&) = delete;

    bool Initialize(HWND owner);
    void Shutdown() noexcept;
    void Toggle(const Settings& settings, bool english);
    void ApplySettings(const Settings& settings, bool english);
    [[nodiscard]] bool Visible() const noexcept { return visible_; }
    [[nodiscard]] std::optional<RECT> Bounds() const noexcept;

private:
    struct Sample {
        std::optional<unsigned int> gpu_percent;
        std::optional<unsigned int> gpu_temperature_c;
        std::optional<unsigned int> gpu_clock_mhz;
        std::optional<std::uint64_t> gpu_memory_used;
        std::optional<std::uint64_t> gpu_memory_total;
        std::optional<unsigned int> cpu_percent;
        std::optional<unsigned int> memory_percent;
        std::optional<std::uint64_t> memory_used;
        std::optional<std::uint64_t> memory_total;
    };

    struct NvmlDevice;
    using NvmlDeviceHandle = NvmlDevice*;
    struct NvmlUtilization {
        unsigned int gpu;
        unsigned int memory;
    };
    struct NvmlMemory {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };

    using NvmlInit = int(__cdecl*)();
    using NvmlShutdown = int(__cdecl*)();
    using NvmlDeviceGetHandleByIndex = int(__cdecl*)(unsigned int, NvmlDeviceHandle*);
    using NvmlDeviceGetUtilizationRates = int(__cdecl*)(NvmlDeviceHandle, NvmlUtilization*);
    using NvmlDeviceGetTemperature = int(__cdecl*)(NvmlDeviceHandle, int, unsigned int*);
    using NvmlDeviceGetClockInfo = int(__cdecl*)(NvmlDeviceHandle, int, unsigned int*);
    using NvmlDeviceGetMemoryInfo = int(__cdecl*)(NvmlDeviceHandle, NvmlMemory*);

    static constexpr UINT_PTR kRefreshTimer = 1;
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    bool InitializeNvml();
    void ShutdownNvml() noexcept;
    void SampleMetrics();
    void SampleNvml();
    void SampleCpuAndMemory();
    void SampleDxgiMemory();
    void Show();
    void Hide();
    void PositionWindow();
    void Paint(HDC dc, const RECT& bounds);
    [[nodiscard]] int EnabledMetricCount() const noexcept;

    HWND owner_{nullptr};
    HWND window_{nullptr};
    bool visible_{false};
    bool english_{false};
    Settings settings_{};
    Sample sample_{};
    HMODULE nvml_module_{nullptr};
    NvmlDeviceHandle nvml_device_{nullptr};
    bool nvml_initialized_{false};
    NvmlInit nvml_init_{nullptr};
    NvmlShutdown nvml_shutdown_{nullptr};
    NvmlDeviceGetHandleByIndex nvml_device_get_handle_{nullptr};
    NvmlDeviceGetUtilizationRates nvml_get_utilization_{nullptr};
    NvmlDeviceGetTemperature nvml_get_temperature_{nullptr};
    NvmlDeviceGetClockInfo nvml_get_clock_{nullptr};
    NvmlDeviceGetMemoryInfo nvml_get_memory_{nullptr};
    std::chrono::steady_clock::time_point next_nvml_retry_{};
    std::uint64_t previous_idle_{0};
    std::uint64_t previous_kernel_{0};
    std::uint64_t previous_user_{0};
};

}  // namespace openreplay::ui

#include "runtime/gs/gs_backend_factory.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_thread_backend.h"
#if defined(PS2X_HAS_GS_GPU)
#include "runtime/gs/gs_gpu_backend.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace
{
    std::string EnvLower(const char *name)
    {
        const char *value = std::getenv(name);
        std::string out = value ? value : "";
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    bool EnvFlag(const char *name)
    {
        const std::string value = EnvLower(name);
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    std::unique_ptr<GSRasterBackend> MakeThreadedCpuBackend()
    {
        GSThreadedBackend::Options options{};
        const std::string queueKb = EnvLower("PS2X_GS_QUEUE_KB");
        if (!queueKb.empty())
        {
            const unsigned long kb = std::strtoul(queueKb.c_str(), nullptr, 10);
            if (kb != 0ul)
                options.queueCapacityBytes = static_cast<size_t>(kb) << 10;
        }
        options.debugStats = EnvFlag("PS2X_GS_DEBUG");
        std::cout << "[gs-thread] PS2X_GS=cpu-thread: GSCpuBackend on a dedicated GS thread, queue_kb="
                  << (options.queueCapacityBytes >> 10) << " debug=" << (options.debugStats ? 1 : 0) << std::endl;
        return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>(), options);
    }

#if defined(PS2X_HAS_GS_GPU)
    std::unique_ptr<GSRasterBackend> MakeD3D11Backend()
    {
        GSGpuBackend::Options options{};
        const std::string draw = EnvLower("PS2X_GS_GPU_DRAW");
        options.drawOnGpu = !(draw == "0" || draw == "false" || draw == "off" || draw == "no");
        const std::string window = EnvLower("PS2X_GS_WINDOW");
        options.showWindow = !(window == "0" || window == "false" || window == "off" || window == "no");
        // Debug knobs for bisecting differences against the CPU reference.
        options.maxBatch = static_cast<uint32_t>(std::strtoul(EnvLower("PS2X_GS_GPU_MAXBATCH").c_str(), nullptr, 10));
        options.verify = static_cast<uint32_t>(std::strtoul(EnvLower("PS2X_GS_GPU_VERIFY").c_str(), nullptr, 10));
        options.doubleZ = EnvFlag("PS2X_GS_GPU_DOUBLE_Z");
        options.hazardLog = static_cast<int>(std::strtol(EnvLower("PS2X_GS_GPU_HAZARD_LOG").c_str(), nullptr, 10));
        const std::string fma = EnvLower("PS2X_GS_GPU_FMA");
        if (!fma.empty())
            options.fmaMode = static_cast<uint32_t>(std::strtoul(fma.c_str(), nullptr, 10));
        const std::string mask = EnvLower("PS2X_GS_GPU_MASK");
        if (!mask.empty())
            options.gpuClassMask = static_cast<uint32_t>(std::strtoul(mask.c_str(), nullptr, 0));

        gs_rhi::DeviceConfig config{};
        config.windowTitle = options.drawOnGpu ? "PS2 GS - D3D11 (GPU rasterizer)" : "PS2 GS - D3D11 (stage 1: CPU frame on GPU)";
        config.vsync = EnvFlag("PS2X_GS_VSYNC");
        config.debugLayer = EnvFlag("PS2X_GS_DEBUG");
        config.filter = EnvLower("PS2X_GS_FILTER") == "linear" ? gs_rhi::Filter::Linear : gs_rhi::Filter::Point;
        std::cout << "[gs-gpu] PS2X_GS=d3d11: GSGpuBackend draw_on_gpu=" << (options.drawOnGpu ? 1 : 0)
                  << " window=" << (options.showWindow ? 1 : 0) << std::endl;
        return std::make_unique<GSGpuBackend>(std::move(config), options);
    }
#endif
}

std::unique_ptr<GSRasterBackend> CreateGSRasterBackendFromEnv()
{
    const std::string choice = EnvLower("PS2X_GS");
    if (choice == "cpu-thread" || choice == "cpu-mt")
        return MakeThreadedCpuBackend();
    if (choice == "d3d11")
    {
#if defined(PS2X_HAS_GS_GPU)
        return MakeD3D11Backend();
#else
        std::cerr << "[gs-gpu] PS2X_GS=d3d11 not built in (PS2X_ENABLE_GS_D3D11=OFF or not Windows); using cpu"
                  << std::endl;
#endif
    }
    else if (!choice.empty() && choice != "cpu")
    {
        std::cerr << "[gs-gpu] unknown PS2X_GS=" << choice << "; using cpu" << std::endl;
    }
    return std::make_unique<GSCpuBackend>();
}

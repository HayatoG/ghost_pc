#pragma once

// GPU GS backend, stage 1 (docs/gs-gpu-plan.md):
//  - all drawing, transfers and VRAM access are delegated to a GSCpuBackend (composition), so the
//    guest-visible behaviour and the PresentationFrame returned to the runtime are identical;
//  - Present() additionally uploads the frame to a gs_rhi texture and shows it in the RHI's own
//    window. This proves the RHI path end to end without drawing anything on the GPU yet.
// Selected with PS2X_GS=d3d11 (see CreateGSRasterBackendFromEnv). Default stays "cpu".

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_rhi.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class GSGpuBackend final : public GSRasterBackend
{
public:
    explicit GSGpuBackend(gs_rhi::DeviceConfig config);
    ~GSGpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    using Clock = std::chrono::steady_clock;

    // Created lazily on the first Present(), i.e. on the host presentation thread, because the
    // D3D11 immediate context and the window must stay on one thread.
    bool EnsureDevice();
    void PresentOnGpu(const PresentationFrame &frame);
    void DisableGpu(const std::string &reason);
    void LogStatsIfDue(Clock::time_point now);

    GSCpuBackend m_reference;
    gs_rhi::DeviceConfig m_config;
    std::unique_ptr<gs_rhi::Device> m_device;
    std::unique_ptr<gs_rhi::Texture> m_frameTexture;
    bool m_gpuDisabled = false;

    // Present-path timing (CPU time spent in upload + blit + swapchain present).
    uint64_t m_presentCount = 0;
    double m_presentMsSum = 0.0;
    double m_presentMsMax = 0.0;
    Clock::time_point m_statsStart{};
};

// CreateGSRasterBackendFromEnv() (PS2X_GS=cpu|cpu-thread|d3d11) is declared here for existing
// includes; it is defined in gs_backend_factory.cpp.
#include "runtime/gs/gs_backend_factory.h"

#include "runtime/gs/gs_gpu_backend.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <utility>

namespace
{
    // Must match the host frame buffer of GSCpuBackend (gs_cpu_backend.cpp).
    constexpr uint32_t kHostFrameWidth = 640u;
    constexpr uint32_t kHostFrameHeight = 512u;

    constexpr double kStatsPeriodSeconds = 10.0;
}

GSGpuBackend::GSGpuBackend(gs_rhi::DeviceConfig config)
    : m_config(std::move(config))
{
}

GSGpuBackend::~GSGpuBackend()
{
    if (m_presentCount > 0)
    {
        std::cout << "[gs-gpu] summary presents=" << m_presentCount << std::fixed << std::setprecision(3)
                  << " avg_ms=" << (m_presentMsSum / static_cast<double>(m_presentCount))
                  << " max_ms=" << m_presentMsMax << std::endl;
    }
}

// ---- Delegation: the CPU backend is the reference and does all the GS work in stage 1. ----

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize) { m_reference.Initialize(vram, vramSize); }
void GSGpuBackend::Reset() { m_reference.Reset(); }
void GSGpuBackend::Submit(const GSPrimitiveBatch &batch) { m_reference.Submit(batch); }
void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) { m_reference.LoadClut(tex0, texclut); }
void GSGpuBackend::BeginTransfer(const GSTransferCommand &command) { m_reference.BeginTransfer(command); }
void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) { m_reference.UploadImage(data, sizeBytes); }
void GSGpuBackend::Flush() { m_reference.Flush(); }
void GSGpuBackend::TextureFlush() { m_reference.TextureFlush(); }
void GSGpuBackend::Sync(GSSyncReason reason) { m_reference.Sync(reason); }

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    return m_reference.ClearFramebuffer(context, rgba);
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    return m_reference.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_reference.ReadVram(psm, base, bw, x, y);
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_reference.WriteVram(psm, base, bw, x, y, value);
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const { m_reference.SnapshotVram(out); }
GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const { return m_reference.GetTransferSnapshot(); }

// ---- Presentation ----

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame = m_reference.Present(request);
    if (frame && !m_gpuDisabled)
        PresentOnGpu(frame);
    return frame; // unchanged: the runtime's own window keeps working exactly as with PS2X_GS=cpu
}

bool GSGpuBackend::EnsureDevice()
{
    if (m_device)
        return true;
    std::string error;
    m_device = gs_rhi::CreateD3D11Device(m_config, error);
    if (!m_device)
    {
        DisableGpu("device creation failed: " + error);
        return false;
    }
    const gs_rhi::AdapterInfo &info = m_device->Info();
    std::cout << "[gs-gpu] api=" << info.api << " adapter=\"" << info.description << "\""
              << std::hex << " vendor=0x" << info.vendorId << " device=0x" << info.deviceId << std::dec
              << " vram_mb=" << (info.dedicatedVideoMemory >> 20) << " feature_level=" << info.featureLevel
              << " software=" << (info.software ? 1 : 0) << " vsync=" << (m_config.vsync ? 1 : 0) << std::endl;
    m_statsStart = Clock::now();
    return true;
}

void GSGpuBackend::PresentOnGpu(const PresentationFrame &frame)
{
    if (!EnsureDevice())
        return;
    if (!m_device->PumpEvents())
    {
        DisableGpu("output window closed");
        return;
    }

    // The reference backend returns a fixed host buffer (kHostFrameWidth x kHostFrameHeight, row stride
    // kHostFrameWidth); frame.width/height only describe the valid top-left rectangle inside it.
    // Upload the whole buffer with its real pitch and let PresentTexture crop the valid region.
    const Clock::time_point start = Clock::now();
    if (!m_frameTexture || m_frameTexture->Width() != kHostFrameWidth || m_frameTexture->Height() != kHostFrameHeight)
    {
        gs_rhi::TextureDesc desc{};
        desc.width = kHostFrameWidth;
        desc.height = kHostFrameHeight;
        m_frameTexture = m_device->CreateTexture(desc);
        if (!m_frameTexture)
        {
            DisableGpu("cannot create the frame texture");
            return;
        }
    }

    const size_t needed = static_cast<size_t>(kHostFrameWidth) * kHostFrameHeight * 4u;
    if (frame.pixels.size() < needed || frame.width > kHostFrameWidth || frame.height > kHostFrameHeight ||
        !m_device->UpdateTexture(*m_frameTexture, frame.pixels.data(), kHostFrameWidth * 4u) ||
        !m_device->PresentTexture(*m_frameTexture, frame.width, frame.height))
    {
        // A failed present (e.g. minimized window) is not fatal; just skip this frame.
        return;
    }

    const Clock::time_point end = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    ++m_presentCount;
    m_presentMsSum += ms;
    m_presentMsMax = std::max(m_presentMsMax, ms);
    LogStatsIfDue(end);
}

void GSGpuBackend::DisableGpu(const std::string &reason)
{
    if (!m_gpuDisabled)
        std::cerr << "[gs-gpu] GPU presentation disabled (" << reason << "); CPU output continues" << std::endl;
    m_gpuDisabled = true;
    m_frameTexture.reset();
    m_device.reset();
}

void GSGpuBackend::LogStatsIfDue(Clock::time_point now)
{
    if (std::chrono::duration<double>(now - m_statsStart).count() < kStatsPeriodSeconds)
        return;
    m_statsStart = now;
    if (m_presentCount == 0)
        return;
    std::cout << "[gs-gpu] presents=" << m_presentCount << std::fixed << std::setprecision(3)
              << " avg_ms=" << (m_presentMsSum / static_cast<double>(m_presentCount))
              << " max_ms=" << m_presentMsMax << std::endl;
}

// The PS2X_GS factory lives in gs_backend_factory.cpp (built even without D3D11).

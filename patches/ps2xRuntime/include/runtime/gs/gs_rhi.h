#pragma once

// Thin rendering hardware interface (RHI) for the GPU GS backend.
//
// Scope today (roadmap stage 1 in docs/gs-gpu-plan.md): an output window, RGBA8 textures,
// full-texture upload, readback, letterboxed blit to the window and present. Draw/pipeline
// objects (stage 2) will be added here when they are needed, not before.
//
// Rules for implementations:
//  - every call happens on the thread that created the Device (D3D11 immediate context is not
//    thread-safe; the window belongs to that thread);
//  - failures are reported through return values and a one-line log, never by exceptions;
//  - nothing in here knows about a specific game.

#include <cstdint>
#include <memory>
#include <string>

namespace gs_rhi
{
    enum class Format : uint8_t
    {
        RGBA8, // byte order R, G, B, A (same as PresentationFrame::pixels)
    };

    enum class Filter : uint8_t
    {
        Point,  // matches raylib's default texture filter used by the CPU path
        Linear,
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        Format format = Format::RGBA8;
        bool cpuWritable = true;   // updated from the CPU every frame (dynamic upload)
        bool renderTarget = false; // stage 2: GS frame targets
    };

    class Texture
    {
    public:
        virtual ~Texture() = default;
        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual Format GetFormat() const = 0;
    };

    struct AdapterInfo
    {
        std::string api;          // "d3d11"
        std::string description;  // adapter name reported by DXGI
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        uint64_t dedicatedVideoMemory = 0;
        std::string featureLevel; // e.g. "11_1"
        bool software = false;    // WARP / basic render driver
    };

    struct DeviceConfig
    {
        std::string windowTitle = "PS2 GS (GPU)";
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 896;
        bool vsync = false;       // host pacing is done by the runtime; default: do not block
        bool debugLayer = false;  // D3D11 debug layer (needs the Graphics Tools optional feature)
        Filter filter = Filter::Point;
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        virtual const AdapterInfo &Info() const = 0;

        virtual std::unique_ptr<Texture> CreateTexture(const TextureDesc &desc) = 0;

        // Uploads a whole texture. `rowPitch` is the source stride in bytes.
        virtual bool UpdateTexture(Texture &texture, const void *data, uint32_t rowPitch) = 0;

        // Copies a whole texture back to the CPU (tightly packed rows). Blocks until the GPU is done.
        virtual bool ReadbackTexture(Texture &texture, void *dst, uint32_t dstRowPitch) = 0;

        // Draws the top-left srcWidth x srcHeight region of `texture` letterboxed into the window
        // (aspect preserved, black bars) and presents the swapchain.
        virtual bool PresentTexture(Texture &texture, uint32_t srcWidth, uint32_t srcHeight) = 0;

        // Waits until all submitted GPU work has finished (used for timing and readback).
        virtual void WaitIdle() = 0;

        // Processes window messages. Returns false once the output window was closed.
        virtual bool PumpEvents() = 0;
    };

    // Creates a D3D11 device with its own output window. Returns nullptr and fills `error` on
    // failure (no D3D11, no hardware adapter, shader compile error...). Windows only.
    std::unique_ptr<Device> CreateD3D11Device(const DeviceConfig &config, std::string &error);

    const char *FormatName(Format format);
}

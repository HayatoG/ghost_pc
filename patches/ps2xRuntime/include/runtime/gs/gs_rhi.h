#pragma once

// Thin rendering hardware interface (RHI) for the GPU GS backend.
//
// Stage 1 (docs/gs-gpu-plan.md): output window, RGBA8 textures, upload, readback, letterboxed blit.
// Stage 2 (docs/gs-gpu-step2-design.md, revised in ghost_pc/docs/gs-gpu-stage2.md): raw/structured
// buffers, buffer copies and ranged readback, runtime-compiled programs (VS+PS or CS) and a
// "procedural" draw with no vertex buffer whose pixel shader writes unordered-access views
// (optionally rasterizer-ordered). Everything here maps to D3D12/Vulkan concepts (raw buffers,
// fragment-shader interlock, compute) and knows nothing about the PS2.
//
// Rules for implementations:
//  - calls are not thread-safe: the owner serialises them (the GS GPU backend uses one mutex);
//    PumpEvents() must be called from the thread that created the Device (it owns the window);
//  - failures are reported through return values and a one-line log, never by exceptions;
//  - nothing in here knows about a specific game.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gs_rhi
{
    enum class Format : uint8_t
    {
        RGBA8, // byte order R, G, B, A (same as PresentationFrame::pixels)
        R32U,  // one 32-bit unsigned integer per texel (UAV surfaces)
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
        bool cpuWritable = true;      // updated from the CPU every frame (dynamic upload)
        bool renderTarget = false;
        bool unorderedAccess = false; // writable from shaders (UAV); not CPU-writable
    };

    class Texture
    {
    public:
        virtual ~Texture() = default;
        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual Format GetFormat() const = 0;
    };

    // Buffer usage flags (combine with |).
    enum BufferUsage : uint32_t
    {
        BufferShaderRaw = 1u << 0,        // read in shaders as a raw byte-address buffer
        BufferShaderStructured = 1u << 1, // read in shaders as a structured buffer (needs structureStride)
        BufferUnorderedRaw = 1u << 2,     // written in shaders as a raw byte-address buffer
        BufferDynamic = 1u << 3,          // CPU-written through WriteDynamicBuffer (no GPU writes)
    };

    struct BufferDesc
    {
        uint32_t size = 0;            // bytes; multiple of 4 (and of structureStride if structured)
        uint32_t structureStride = 0; // bytes per element for BufferShaderStructured
        uint32_t usage = 0;           // BufferUsage flags
    };

    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        virtual uint32_t Size() const = 0;
    };

    struct ByteRange
    {
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    // HLSL source compiled at runtime. Leave an entry empty when the program has no such stage.
    struct ProgramDesc
    {
        const char *source = nullptr;
        std::string name;     // for error messages / debuggers
        std::string vsEntry;  // vertex stage (procedural draws)
        std::string psEntry;  // pixel stage (procedural draws)
        std::string csEntry;  // compute stage (Dispatch)
        std::vector<std::pair<std::string, std::string>> defines;
    };

    class Program
    {
    public:
        virtual ~Program() = default;
    };

    // Resources bound for one draw/dispatch. Shader-resource slots map to t0..t3 (vertex, pixel and
    // compute stages), unordered-access slots to u0..u3 (pixel or compute stage). Null = unbound.
    struct Bindings
    {
        static constexpr size_t kSlots = 4;
        Buffer *shaderBuffers[kSlots]{};
        Buffer *unorderedBuffers[kSlots]{};
        Texture *unorderedTextures[kSlots]{}; // a slot holds either a buffer or a texture
    };

    struct Capabilities
    {
        bool rasterOrderedViews = false; // pixel-shader ordering of UAV accesses (ROV / interlock)
        bool doubles = false;            // double precision in shaders, incl. fma and double<->int conversion
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
        bool showWindow = true;   // false: create the output window hidden (offscreen work only)
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        virtual const AdapterInfo &Info() const = 0;
        virtual const Capabilities &Caps() const = 0;

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

        // ---- Stage 2: buffers, programs, procedural draws ----

        virtual std::unique_ptr<Buffer> CreateBuffer(const BufferDesc &desc, const void *initialData = nullptr) = 0;

        // GPU-side buffer (not dynamic): writes `size` bytes at `offset`, ordered after earlier commands.
        virtual bool UpdateBuffer(Buffer &buffer, uint32_t offset, const void *data, uint32_t size) = 0;

        // Dynamic buffer: writes at `offset`. `discard` = previous contents may be thrown away (start of a
        // new ring cycle); otherwise the caller guarantees it does not overwrite data still in use.
        virtual bool WriteDynamicBuffer(Buffer &buffer, uint32_t offset, const void *data, uint32_t size, bool discard) = 0;

        // GPU copy between two different buffers.
        virtual bool CopyBuffer(Buffer &dst, uint32_t dstOffset, Buffer &src, uint32_t srcOffset, uint32_t size) = 0;

        // Copies the given ranges of `src` back to the CPU (each range lands at dstBase + range.offset).
        // Blocks until the GPU has finished the work that writes them.
        virtual bool ReadBuffer(Buffer &src, const ByteRange *ranges, size_t rangeCount, void *dstBase) = 0;

        virtual std::unique_ptr<Program> CreateProgram(const ProgramDesc &desc, std::string &error) = 0;

        // Draws `vertexCount` vertices as a triangle list with no vertex buffer (SV_VertexID starts at
        // firstVertex) and no render target: the pixel stage only writes the bound unordered views.
        // The viewport is (0,0)-(viewportWidth, viewportHeight).
        virtual bool DrawProcedural(Program &program, const Bindings &bindings, uint32_t viewportWidth,
                                    uint32_t viewportHeight, uint32_t vertexCount, uint32_t firstVertex) = 0;

        virtual bool Dispatch(Program &program, const Bindings &bindings, uint32_t groupsX, uint32_t groupsY,
                              uint32_t groupsZ) = 0;

        // Submits queued commands to the GPU without waiting.
        virtual void Submit() = 0;
    };

    // Creates a D3D11 device with its own output window. Returns nullptr and fills `error` on
    // failure (no D3D11, no hardware adapter, shader compile error...). Windows only.
    std::unique_ptr<Device> CreateD3D11Device(const DeviceConfig &config, std::string &error);

    const char *FormatName(Format format);
}

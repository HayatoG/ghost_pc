#pragma once

// Graphics RHI for the hardware GS renderer (PS2X_GS=hw, gs_hw_backend.*).
//
// The older interface in gs_rhi.h serves the ROV backend (raw buffers + procedural draws, no render
// targets). This one adds what a "real" hardware renderer needs: textures that are render targets,
// depth buffers, sampled images and storage images; immutable pipeline objects (shaders + blend +
// depth + colour mask); render passes; per-draw constants and a vertex ring; compute dispatches;
// surface copies and readback; presentation of a surface to a window.
//
// It is shaped after D3D12/Vulkan so that both can implement it without emulating D3D11:
//  - Pipeline = PSO / VkPipeline, created up front from a PipelineDesc and cached by the caller.
//    Render-target formats are fixed by the desc (colour RGBA8 and/or depth D32F).
//  - BeginPass/EndPass = render pass / dynamic rendering. Inside a pass only draw-state calls are
//    legal (SetPipeline, SetConstants, SetTextures, SetBlendConstant, SetScissor, Draw).
//    Copies, dispatches, readbacks and uploads happen outside passes.
//  - Resource state transitions are implicit: the implementation tracks the last use of every
//    Surface/Buffer (render target, depth, sampled, storage, copy) and inserts barriers when a
//    pass/dispatch/copy needs another state. D3D11 does this itself.
//  - SetConstants copies up to kMaxConstantBytes into a per-frame upload ring and binds it at a
//    dynamic offset (D3D12 root CBV, Vulkan dynamic uniform buffer). Draw() copies vertices into a
//    ring as well; nothing passed in by pointer is referenced after the call returns.
//  - Shaders are HLSL (D3D11/D3D12: D3DCompile/DXC; Vulkan: DXC -spirv). Register convention:
//    b0 constants (all stages), t0..t3 textures/buffers, u0..u1 storage (compute), s0 point clamp.
//  - Every call is made from one thread at a time (the owner serialises them).
//  - Failures return false / nullptr and log one line; no exceptions.
// Nothing here knows about the PS2.

#include "runtime/gs/gs_rhi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gs_rhi
{
    enum class SurfaceFormat : uint8_t
    {
        RGBA8,    // UNORM, R G B A byte order
        Depth32F, // depth buffer; when Sampled, read as R32_FLOAT
        R32U,
    };

    enum SurfaceUsage : uint32_t
    {
        SurfaceSampled = 1u << 0,      // read in shaders (Texture2D)
        SurfaceRenderTarget = 1u << 1, // colour attachment
        SurfaceDepth = 1u << 2,        // depth attachment (Depth32F only)
        SurfaceStorage = 1u << 3,      // written by compute (RWTexture2D); not with SurfaceDepth
    };

    struct SurfaceDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        SurfaceFormat format = SurfaceFormat::RGBA8;
        uint32_t usage = SurfaceSampled;
    };

    class Surface
    {
    public:
        virtual ~Surface() = default;
        virtual const SurfaceDesc &Desc() const = 0;
    };

    enum class BlendFactor : uint8_t
    {
        Zero,
        One,
        Src1Alpha,    // dual-source: alpha of the pixel shader's second output
        InvSrc1Alpha,
        DstAlpha,
        InvDstAlpha,
        Constant,     // SetBlendConstant
        InvConstant,
    };

    enum class BlendOp : uint8_t
    {
        Add,         // src*fs + dst*fd
        Subtract,    // src*fs - dst*fd
        RevSubtract, // dst*fd - src*fs
    };

    enum class CompareOp : uint8_t
    {
        Never,
        Always,
        GreaterEqual,
        Greater,
    };

    enum ColorMask : uint8_t
    {
        MaskR = 1,
        MaskG = 2,
        MaskB = 4,
        MaskA = 8,
        MaskRGBA = 15,
    };

    struct BlendDesc
    {
        bool enable = false;                // colour channels only; alpha is always written as-is
        BlendFactor src = BlendFactor::One;
        BlendFactor dst = BlendFactor::Zero;
        BlendOp op = BlendOp::Add;
        uint8_t writeMask = MaskRGBA;
    };

    struct DepthDesc
    {
        bool test = false;
        CompareOp compare = CompareOp::Always;
        bool write = false;
    };

    enum class VertexFormat : uint8_t
    {
        Float2,
        Float3,
        Float4,
        UInt1,
        UInt2,
    };

    struct VertexAttribute
    {
        const char *semantic = "TEXCOORD"; // HLSL semantic name
        uint32_t semanticIndex = 0;
        VertexFormat format = VertexFormat::Float4;
        uint32_t offset = 0;
    };

    // Shaders of a graphics pipeline (vsEntry + psEntry) or a compute program (csEntry), from the
    // ProgramDesc of gs_rhi.h. The vertex layout is part of the graphics program.
    class GfxProgram
    {
    public:
        virtual ~GfxProgram() = default;
    };

    struct PipelineDesc
    {
        GfxProgram *program = nullptr;
        bool hasColor = true;  // RGBA8 colour attachment
        bool hasDepth = false; // Depth32F attachment
        BlendDesc blend{};
        DepthDesc depth{};
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
    };

    // A shader-visible resource for t0..t3: a surface, or a raw/structured Buffer from gs_rhi.h.
    struct ShaderResource
    {
        Surface *surface = nullptr;
        Buffer *buffer = nullptr;
    };

    struct ComputeBindings
    {
        static constexpr size_t kSlots = 4;
        ShaderResource resources[kSlots]{};
        Surface *storageSurfaces[2]{}; // u0, u1 (a slot holds a surface or a buffer)
        Buffer *storageBuffers[2]{};
    };

    struct Rect
    {
        uint32_t x = 0, y = 0, width = 0, height = 0;
    };

    struct GfxStats
    {
        uint64_t draws = 0, passes = 0, dispatches = 0, copies = 0, readbacks = 0, pipelineSwitches = 0;
        uint64_t vertexBytes = 0, constantBytes = 0;
        double readbackMs = 0.0;
    };

    class GfxDevice
    {
    public:
        static constexpr uint32_t kMaxConstantBytes = 4096;

        virtual ~GfxDevice() = default;

        virtual const AdapterInfo &Info() const = 0;
        virtual GfxStats &Stats() = 0;

        // ---- resources ----
        virtual std::unique_ptr<Surface> CreateSurface(const SurfaceDesc &desc) = 0;
        virtual std::unique_ptr<Buffer> CreateBuffer(const BufferDesc &desc, const void *initialData = nullptr) = 0;
        virtual std::unique_ptr<GfxProgram> CreateProgram(const ProgramDesc &desc, const std::vector<VertexAttribute> &layout,
                                                          uint32_t vertexStride, std::string &error) = 0;
        virtual std::unique_ptr<Pipeline> CreatePipeline(const PipelineDesc &desc) = 0;

        // ---- transfers (outside passes) ----
        virtual bool UpdateBuffer(Buffer &buffer, uint32_t offset, const void *data, uint32_t size) = 0;
        // Copies `ranges` of `src` to dstBase + range.offset. Blocks until the GPU has written them.
        virtual bool ReadBuffer(Buffer &src, const ByteRange *ranges, size_t rangeCount, void *dstBase) = 0;
        virtual bool UploadSurface(Surface &dst, const Rect &rect, const void *data, uint32_t rowPitch) = 0;
        // Blocks. RGBA8: 4 bytes per texel, Depth32F: float.
        virtual bool ReadbackSurface(Surface &src, const Rect &rect, void *dst, uint32_t rowPitch) = 0;
        // Same-format copy; rectangles must not overlap when src == dst (not allowed: use a temp).
        virtual bool CopySurface(Surface &dst, uint32_t dstX, uint32_t dstY, Surface &src, const Rect &srcRect) = 0;
        virtual bool ClearColor(Surface &target, const float rgba[4]) = 0;
        virtual bool ClearDepth(Surface &target, float depth) = 0;

        // ---- compute (outside passes) ----
        virtual bool Dispatch(GfxProgram &program, const ComputeBindings &bindings, const void *constants,
                              uint32_t constantBytes, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

        // ---- render passes ----
        // Viewport = (0,0)-(viewportWidth, viewportHeight) of the attachments (0 = attachment size).
        virtual bool BeginPass(Surface *color, Surface *depth, uint32_t viewportWidth = 0, uint32_t viewportHeight = 0) = 0;
        virtual void SetPipeline(Pipeline &pipeline) = 0;
        virtual void SetConstants(const void *data, uint32_t size) = 0;
        virtual void SetTextures(const ShaderResource *resources, uint32_t count) = 0; // t0..t(count-1)
        virtual void SetBlendConstant(float value) = 0;
        virtual void SetScissor(const Rect &rect) = 0;
        virtual bool Draw(const void *vertices, uint32_t vertexCount) = 0; // triangle list
        virtual void EndPass() = 0;

        // ---- presentation ----
        // Draws `rect` of `surface` letterboxed into the window and presents. False when there is no
        // window (headless) or it was closed. With a present filter set, `filterConstants` is called with
        // the size of the letterboxed output rectangle and fills the filter's constant buffer (b0).
        using FilterConstantsFn = std::function<void(uint32_t outWidth, uint32_t outHeight, std::vector<uint8_t> &constants)>;
        virtual bool PresentSurface(Surface &surface, const Rect &rect, const FilterConstantsFn &filterConstants = {}) = 0;
        // Optional final full-screen pass of the presentation only (e.g. CRT). `pixelShaderSource` is HLSL that
        // defines `float4 PresentFilterMain(float4 pos : SV_Position, float2 uv01 : TEXCOORD1) : SV_Target`
        // reading t0/s0 (linear clamp); uv01 spans the visible rectangle. nullptr removes the filter.
        virtual bool SetPresentFilter(const char *pixelShaderSource, std::string &error) = 0;
        virtual bool PumpEvents() = 0;
        virtual void Flush() = 0;    // submit queued work
        virtual void WaitIdle() = 0; // block until the GPU is idle
    };

    struct GfxDeviceConfig
    {
        bool headless = false;     // no window / swapchain (tools, tests)
        bool showWindow = true;
        bool vsync = false;
        bool debugLayer = false;
        bool linearPresent = false;
        std::string windowTitle = "PS2 GS (hardware)";
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 1024;
    };

    std::unique_ptr<GfxDevice> CreateD3D11GfxDevice(const GfxDeviceConfig &config, std::string &error);
}

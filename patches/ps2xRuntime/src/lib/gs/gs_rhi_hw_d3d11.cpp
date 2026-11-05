// D3D11 implementation of the hardware-renderer RHI (include/runtime/gs/gs_rhi_hw.h).
// Optional Win32 window + flip-model swapchain (headless mode has neither). HLSL compiled at
// runtime with D3DCompile. Libraries: d3d11, dxgi, d3dcompiler.

#include "runtime/gs/gs_rhi_hw.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>

#if defined(_MSC_VER)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace gs_rhi
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr wchar_t kWindowClass[] = L"PS2X_GS_HW_D3D11";
        constexpr uint32_t kVertexRingBytes = 16u << 20;
        constexpr uint32_t kConstantRingBytes = 4u << 20;

        constexpr char kBlitHlsl[] = R"(
Texture2D gSource : register(t0);
SamplerState gSampler : register(s0);
cbuffer C : register(b1) { float4 uvRect; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 uv01 : TEXCOORD1; };
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uvRect.xy + t * uvRect.zw;
    o.uv01 = t;
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return float4(gSource.Sample(gSampler, i.uv).rgb, 1.0); }
)";

        std::string HrText(HRESULT hr)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
            return buf;
        }

        std::wstring Widen(const std::string &text)
        {
            if (text.empty())
                return {};
            const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring out(static_cast<size_t>(std::max(len - 1, 0)), L'\0');
            if (len > 1)
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
            return out;
        }

        std::string Narrow(const wchar_t *text)
        {
            const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(std::max(len - 1, 0)), '\0');
            if (len > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
            return out;
        }

        bool Compile(const char *source, const char *name, const char *entry, const char *target,
                     const std::vector<std::pair<std::string, std::string>> &defines, ComPtr<ID3DBlob> &out, std::string &error)
        {
            std::vector<D3D_SHADER_MACRO> macros;
            for (const auto &d : defines)
                macros.push_back({d.first.c_str(), d.second.c_str()});
            macros.push_back({nullptr, nullptr});
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(source, std::strlen(source), name, macros.data(), nullptr, entry, target,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(hr))
            {
                error = std::string("D3DCompile ") + name + ":" + entry + " failed " + HrText(hr);
                if (errors)
                    error += ": " + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
                return false;
            }
            return true;
        }

        class HwSurface final : public Surface
        {
        public:
            const SurfaceDesc &Desc() const override { return desc; }
            SurfaceDesc desc{};
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            ComPtr<ID3D11UnorderedAccessView> uav;
        };

        class HwBuffer final : public Buffer
        {
        public:
            uint32_t Size() const override { return desc.size; }
            BufferDesc desc{};
            ComPtr<ID3D11Buffer> buffer;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11UnorderedAccessView> uav;
        };

        class HwProgram final : public GfxProgram
        {
        public:
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
            ComPtr<ID3D11ComputeShader> cs;
            ComPtr<ID3D11InputLayout> layout;
            uint32_t stride = 0;
        };

        class HwPipeline final : public Pipeline
        {
        public:
            HwProgram *program = nullptr;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth;
        };

        D3D11_BLEND ToBlend(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return D3D11_BLEND_ZERO;
            case BlendFactor::One: return D3D11_BLEND_ONE;
            case BlendFactor::Src1Alpha: return D3D11_BLEND_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha: return D3D11_BLEND_INV_SRC1_ALPHA;
            case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
            case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
            case BlendFactor::Constant: return D3D11_BLEND_BLEND_FACTOR;
            case BlendFactor::InvConstant: return D3D11_BLEND_INV_BLEND_FACTOR;
            }
            return D3D11_BLEND_ONE;
        }

        D3D11_BLEND_OP ToBlendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return D3D11_BLEND_OP_ADD;
            case BlendOp::Subtract: return D3D11_BLEND_OP_SUBTRACT;
            case BlendOp::RevSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
            }
            return D3D11_BLEND_OP_ADD;
        }

        D3D11_COMPARISON_FUNC ToCompare(CompareOp op)
        {
            switch (op)
            {
            case CompareOp::Never: return D3D11_COMPARISON_NEVER;
            case CompareOp::Always: return D3D11_COMPARISON_ALWAYS;
            case CompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
            case CompareOp::Greater: return D3D11_COMPARISON_GREATER;
            }
            return D3D11_COMPARISON_ALWAYS;
        }

        DXGI_FORMAT ToVertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
            case VertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case VertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case VertexFormat::UInt1: return DXGI_FORMAT_R32_UINT;
            case VertexFormat::UInt2: return DXGI_FORMAT_R32G32_UINT;
            case VertexFormat::UInt4: return DXGI_FORMAT_R32G32B32A32_UINT;
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        class D3D11GfxDevice final : public GfxDevice
        {
        public:
            ~D3D11GfxDevice() override
            {
                if (m_context)
                {
                    m_context->ClearState();
                    m_context->Flush();
                }
                m_backbufferRtv.Reset();
                m_swapchain.Reset();
                if (m_hwnd)
                {
                    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
                    DestroyWindow(m_hwnd);
                }
            }

            bool Create(const GfxDeviceConfig &config, std::string &error);

            const AdapterInfo &Info() const override { return m_info; }
            GfxStats &Stats() override { return m_stats; }

            std::unique_ptr<Surface> CreateSurface(const SurfaceDesc &desc) override;
            std::unique_ptr<Buffer> CreateBuffer(const BufferDesc &desc, const void *initialData) override;
            std::unique_ptr<GfxProgram> CreateProgram(const ProgramDesc &desc, const std::vector<VertexAttribute> &layout,
                                                      uint32_t vertexStride, std::string &error) override;
            std::unique_ptr<Pipeline> CreatePipeline(const PipelineDesc &desc) override;

            bool UpdateBuffer(Buffer &buffer, uint32_t offset, const void *data, uint32_t size) override;
            bool ReadBuffer(Buffer &src, const ByteRange *ranges, size_t rangeCount, void *dstBase) override;
            bool UploadSurface(Surface &dst, const Rect &rect, const void *data, uint32_t rowPitch) override;
            bool ReadbackSurface(Surface &src, const Rect &rect, void *dst, uint32_t rowPitch) override;
            bool CopySurface(Surface &dst, uint32_t dstX, uint32_t dstY, Surface &src, const Rect &srcRect) override;
            bool ClearColor(Surface &target, const float rgba[4]) override;
            bool ClearDepth(Surface &target, float depth) override;

            bool Dispatch(GfxProgram &program, const ComputeBindings &bindings, const void *constants, uint32_t constantBytes,
                          uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;

            bool BeginPass(Surface *color, Surface *depth, uint32_t viewportWidth, uint32_t viewportHeight) override;
            void SetPipeline(Pipeline &pipeline) override;
            void SetConstants(const void *data, uint32_t size) override;
            void SetTextures(const ShaderResource *resources, uint32_t count) override;
            void SetBlendConstant(float value) override { m_blendConstant = value; m_blendDirty = true; }
            void SetScissor(const Rect &rect) override;
            bool Draw(const void *vertices, uint32_t vertexCount) override;
            void EndPass() override;

            bool PresentSurface(Surface &surface, const Rect &rect, const FilterConstantsFn &filterConstants) override;
            bool SetPresentFilter(const char *pixelShaderSource, std::string &error) override;
            bool PumpEvents() override;
            void Flush() override { m_context->Flush(); }
            void WaitIdle() override;

        private:
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
            bool CreateOutputWindow(const GfxDeviceConfig &config, std::string &error);
            bool CreateSwapchain(std::string &error);
            bool CreateBackbufferView();
            bool WriteConstants(const void *data, uint32_t size, ID3D11Buffer **outBuffer, UINT &firstConstant, UINT &numConstants);
            bool EnsureStaging(uint32_t width, uint32_t height, DXGI_FORMAT format);
            bool EnsureBufferStaging(uint32_t size);
            void UnbindAll();
            ID3D11ShaderResourceView *Srv(const ShaderResource &r) const;

            GfxDeviceConfig m_config;
            AdapterInfo m_info{};
            GfxStats m_stats{};
            HWND m_hwnd = nullptr;
            bool m_windowClosed = false;
            bool m_resizePending = false;

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            ComPtr<ID3D11DeviceContext1> m_context1;
            ComPtr<IDXGISwapChain1> m_swapchain;
            ComPtr<ID3D11RenderTargetView> m_backbufferRtv;
            UINT m_backbufferWidth = 0, m_backbufferHeight = 0;

            ComPtr<ID3D11Buffer> m_vertexRing;
            uint32_t m_vertexCursor = 0;
            ComPtr<ID3D11Buffer> m_constantRing; // needs D3D11.1 constant buffer offsets
            uint32_t m_constantCursor = 0;
            ComPtr<ID3D11Buffer> m_constantFallback; // when offsets are not supported
            // Redundant-state filters for the draw path (each skipped call is a driver call less per batch)
            ID3D11ShaderResourceView *m_psSrv[4] = {};
            bool m_cbValid = false;
            uint32_t m_cbLastSize = 0;
            uint8_t m_cbLast[kMaxConstantBytes] = {};
            bool m_scissorValid = false;
            D3D11_RECT m_scissor{};
            ComPtr<ID3D11RasterizerState> m_rasterizer;
            ComPtr<ID3D11RasterizerState> m_rasterizerNoScissor;
            ComPtr<ID3D11SamplerState> m_pointClamp;
            ComPtr<ID3D11SamplerState> m_linearClamp;
            ComPtr<ID3D11VertexShader> m_blitVs;
            ComPtr<ID3D11PixelShader> m_blitPs;
            ComPtr<ID3D11PixelShader> m_filterPs;
            std::vector<uint8_t> m_filterConstants;
            ComPtr<ID3D11Query> m_idleQuery;

            ComPtr<ID3D11Texture2D> m_staging;
            uint32_t m_stagingW = 0, m_stagingH = 0;
            DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;
            ComPtr<ID3D11Buffer> m_bufferStaging;
            uint32_t m_bufferStagingSize = 0;

            bool m_inPass = false;
            HwPipeline *m_pipeline = nullptr;
            HwProgram *m_boundProgram = nullptr;
            float m_blendConstant = 1.0f;
            bool m_blendDirty = true;
        };

        LRESULT CALLBACK D3D11GfxDevice::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            auto *self = reinterpret_cast<D3D11GfxDevice *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_SIZE:
                if (self)
                    self->m_resizePending = true;
                return 0;
            case WM_CLOSE:
                if (self)
                    self->m_windowClosed = true;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        }

        bool D3D11GfxDevice::CreateOutputWindow(const GfxDeviceConfig &config, std::string &error)
        {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &D3D11GfxDevice::WndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.lpszClassName = kWindowClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                error = "RegisterClassExW failed";
                return false;
            }
            RECT rect{0, 0, static_cast<LONG>(config.windowWidth), static_cast<LONG>(config.windowHeight)};
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            const std::wstring title = Widen(config.windowTitle);
            m_hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                     rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
            if (!m_hwnd)
            {
                error = "CreateWindowExW failed";
                return false;
            }
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            if (config.showWindow)
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            return true;
        }

        bool D3D11GfxDevice::CreateBackbufferView()
        {
            ComPtr<ID3D11Texture2D> back;
            if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()))) ||
                FAILED(m_device->CreateRenderTargetView(back.Get(), nullptr, m_backbufferRtv.ReleaseAndGetAddressOf())))
                return false;
            D3D11_TEXTURE2D_DESC d{};
            back->GetDesc(&d);
            m_backbufferWidth = d.Width;
            m_backbufferHeight = d.Height;
            return true;
        }

        bool D3D11GfxDevice::CreateSwapchain(std::string &error)
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
                FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
            {
                error = "cannot reach the DXGI factory";
                return false;
            }
            DXGI_SWAP_CHAIN_DESC1 sc{};
            sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sc.SampleDesc.Count = 1;
            sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc.BufferCount = 2;
            sc.Scaling = DXGI_SCALING_STRETCH;
            sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            const HRESULT hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &sc, nullptr, nullptr, m_swapchain.GetAddressOf());
            if (FAILED(hr))
            {
                error = "CreateSwapChainForHwnd failed " + HrText(hr);
                return false;
            }
            factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
            return CreateBackbufferView();
        }

        bool D3D11GfxDevice::Create(const GfxDeviceConfig &config, std::string &error)
        {
            m_config = config;
            if (!config.headless && !CreateOutputWindow(config, error))
                return false;
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            if (config.debugLayer)
                flags |= D3D11_CREATE_DEVICE_DEBUG;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL obtained{};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                           m_device.GetAddressOf(), &obtained, m_context.GetAddressOf());
            if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
            {
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                       m_device.GetAddressOf(), &obtained, m_context.GetAddressOf());
            }
            if (FAILED(hr))
            {
                error = "D3D11CreateDevice failed " + HrText(hr);
                return false;
            }
            m_context.As(&m_context1);
            {
                ComPtr<IDXGIDevice> dxgiDevice;
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
                {
                    DXGI_ADAPTER_DESC ad{};
                    adapter->GetDesc(&ad);
                    m_info.description = Narrow(ad.Description);
                    m_info.vendorId = ad.VendorId;
                    m_info.deviceId = ad.DeviceId;
                    m_info.dedicatedVideoMemory = ad.DedicatedVideoMemory;
                    m_info.software = ad.VendorId == 0x1414u;
                }
            }
            m_info.api = "d3d11";
            m_info.featureLevel = obtained == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0";
            if (!config.headless && !CreateSwapchain(error))
                return false;

            D3D11_BUFFER_DESC vb{};
            vb.ByteWidth = kVertexRingBytes;
            vb.Usage = D3D11_USAGE_DYNAMIC;
            vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = kConstantRingBytes;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_BUFFER_DESC cbf = cb;
            cbf.ByteWidth = kMaxConstantBytes;
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = FALSE;
            rd.ScissorEnable = TRUE;
            D3D11_RASTERIZER_DESC rd2 = rd;
            rd2.ScissorEnable = FALSE;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            D3D11_SAMPLER_DESC sl = sd;
            sl.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_EVENT;
            if (FAILED(m_device->CreateBuffer(&vb, nullptr, m_vertexRing.GetAddressOf())) ||
                FAILED(m_device->CreateBuffer(&cb, nullptr, m_constantRing.GetAddressOf())) ||
                FAILED(m_device->CreateBuffer(&cbf, nullptr, m_constantFallback.GetAddressOf())) ||
                FAILED(m_device->CreateRasterizerState(&rd, m_rasterizer.GetAddressOf())) ||
                FAILED(m_device->CreateRasterizerState(&rd2, m_rasterizerNoScissor.GetAddressOf())) ||
                FAILED(m_device->CreateSamplerState(&sd, m_pointClamp.GetAddressOf())) ||
                FAILED(m_device->CreateSamplerState(&sl, m_linearClamp.GetAddressOf())) ||
                FAILED(m_device->CreateQuery(&qd, m_idleQuery.GetAddressOf())))
            {
                error = "fixed D3D11 objects could not be created";
                return false;
            }
            ComPtr<ID3DBlob> vsb, psb;
            if (!Compile(kBlitHlsl, "gs_hw_blit", "VSMain", "vs_5_0", {}, vsb, error) ||
                !Compile(kBlitHlsl, "gs_hw_blit", "PSMain", "ps_5_0", {}, psb, error) ||
                FAILED(m_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, m_blitVs.GetAddressOf())) ||
                FAILED(m_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, m_blitPs.GetAddressOf())))
            {
                if (error.empty())
                    error = "blit shader creation failed";
                return false;
            }
            return true;
        }

        std::unique_ptr<Surface> D3D11GfxDevice::CreateSurface(const SurfaceDesc &desc)
        {
            auto s = std::make_unique<HwSurface>();
            s->desc = desc;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = desc.width;
            td.Height = desc.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            const bool depth = desc.format == SurfaceFormat::Depth32F;
            DXGI_FORMAT fmt = desc.format == SurfaceFormat::RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_UINT;
            if (depth)
                fmt = (desc.usage & SurfaceSampled) ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
            td.Format = fmt;
            if (desc.usage & SurfaceSampled) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (desc.usage & SurfaceRenderTarget) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
            if (desc.usage & SurfaceDepth) td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
            if (desc.usage & SurfaceStorage) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, s->texture.GetAddressOf())))
            {
                std::cerr << "[gs-hw-rhi] CreateTexture2D " << desc.width << "x" << desc.height << " failed" << std::endl;
                return nullptr;
            }
            if (desc.usage & SurfaceSampled)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = depth ? DXGI_FORMAT_R32_FLOAT : fmt;
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                if (FAILED(m_device->CreateShaderResourceView(s->texture.Get(), &sv, s->srv.GetAddressOf())))
                    return nullptr;
            }
            if ((desc.usage & SurfaceRenderTarget) &&
                FAILED(m_device->CreateRenderTargetView(s->texture.Get(), nullptr, s->rtv.GetAddressOf())))
                return nullptr;
            if (desc.usage & SurfaceDepth)
            {
                D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
                dv.Format = DXGI_FORMAT_D32_FLOAT;
                dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                if (FAILED(m_device->CreateDepthStencilView(s->texture.Get(), &dv, s->dsv.GetAddressOf())))
                    return nullptr;
            }
            if ((desc.usage & SurfaceStorage) &&
                FAILED(m_device->CreateUnorderedAccessView(s->texture.Get(), nullptr, s->uav.GetAddressOf())))
                return nullptr;
            return s;
        }

        std::unique_ptr<Buffer> D3D11GfxDevice::CreateBuffer(const BufferDesc &desc, const void *initialData)
        {
            auto b = std::make_unique<HwBuffer>();
            b->desc = desc;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = desc.size;
            bd.Usage = D3D11_USAGE_DEFAULT;
            const bool raw = (desc.usage & (BufferShaderRaw | BufferUnorderedRaw)) != 0u;
            if (desc.usage & (BufferShaderRaw | BufferShaderStructured)) bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (desc.usage & BufferUnorderedRaw) bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            if (raw) bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            if (desc.usage & BufferShaderStructured)
            {
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = desc.structureStride;
            }
            D3D11_SUBRESOURCE_DATA init{initialData, 0, 0};
            if (FAILED(m_device->CreateBuffer(&bd, initialData ? &init : nullptr, b->buffer.GetAddressOf())))
                return nullptr;
            if (bd.BindFlags & D3D11_BIND_SHADER_RESOURCE)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
                if (raw)
                {
                    sv.Format = DXGI_FORMAT_R32_TYPELESS;
                    sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                    sv.BufferEx.NumElements = desc.size / 4u;
                }
                else
                {
                    sv.Format = DXGI_FORMAT_UNKNOWN;
                    sv.BufferEx.NumElements = desc.size / desc.structureStride;
                }
                if (FAILED(m_device->CreateShaderResourceView(b->buffer.Get(), &sv, b->srv.GetAddressOf())))
                    return nullptr;
            }
            if (bd.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
            {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
                uv.Format = DXGI_FORMAT_R32_TYPELESS;
                uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                uv.Buffer.NumElements = desc.size / 4u;
                uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                if (FAILED(m_device->CreateUnorderedAccessView(b->buffer.Get(), &uv, b->uav.GetAddressOf())))
                    return nullptr;
            }
            return b;
        }

        std::unique_ptr<GfxProgram> D3D11GfxDevice::CreateProgram(const ProgramDesc &desc, const std::vector<VertexAttribute> &layout,
                                                                  uint32_t vertexStride, std::string &error)
        {
            auto p = std::make_unique<HwProgram>();
            p->stride = vertexStride;
            ComPtr<ID3DBlob> blob;
            const char *name = desc.name.empty() ? "gs_hw" : desc.name.c_str();
            if (!desc.vsEntry.empty())
            {
                if (!Compile(desc.source, name, desc.vsEntry.c_str(), "vs_5_0", desc.defines, blob, error) ||
                    FAILED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, p->vs.GetAddressOf())))
                    return nullptr;
                if (!layout.empty())
                {
                    std::vector<D3D11_INPUT_ELEMENT_DESC> el;
                    for (const VertexAttribute &a : layout)
                        el.push_back({a.semantic, a.semanticIndex, ToVertexFormat(a.format), 0, a.offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
                    if (FAILED(m_device->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), blob->GetBufferPointer(),
                                                           blob->GetBufferSize(), p->layout.GetAddressOf())))
                    {
                        error = std::string("CreateInputLayout failed for ") + name;
                        return nullptr;
                    }
                }
            }
            if (!desc.psEntry.empty())
            {
                blob.Reset();
                if (!Compile(desc.source, name, desc.psEntry.c_str(), "ps_5_0", desc.defines, blob, error) ||
                    FAILED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, p->ps.GetAddressOf())))
                    return nullptr;
            }
            if (!desc.csEntry.empty())
            {
                blob.Reset();
                if (!Compile(desc.source, name, desc.csEntry.c_str(), "cs_5_0", desc.defines, blob, error) ||
                    FAILED(m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, p->cs.GetAddressOf())))
                    return nullptr;
            }
            return p;
        }

        std::unique_ptr<Pipeline> D3D11GfxDevice::CreatePipeline(const PipelineDesc &desc)
        {
            auto p = std::make_unique<HwPipeline>();
            p->program = static_cast<HwProgram *>(desc.program);
            D3D11_BLEND_DESC bd{};
            auto &rt = bd.RenderTarget[0];
            rt.BlendEnable = desc.blend.enable ? TRUE : FALSE;
            rt.SrcBlend = ToBlend(desc.blend.src);
            rt.DestBlend = ToBlend(desc.blend.dst);
            rt.BlendOp = ToBlendOp(desc.blend.op);
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = desc.hasColor ? desc.blend.writeMask : 0u;
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = (desc.hasDepth && (desc.depth.test || desc.depth.write)) ? TRUE : FALSE;
            dd.DepthWriteMask = desc.depth.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = desc.depth.test ? ToCompare(desc.depth.compare) : D3D11_COMPARISON_ALWAYS;
            if (FAILED(m_device->CreateBlendState(&bd, p->blend.GetAddressOf())) ||
                FAILED(m_device->CreateDepthStencilState(&dd, p->depth.GetAddressOf())))
                return nullptr;
            return p;
        }

        bool D3D11GfxDevice::UpdateBuffer(Buffer &buffer, uint32_t offset, const void *data, uint32_t size)
        {
            auto &b = static_cast<HwBuffer &>(buffer);
            if (offset + size > b.desc.size)
                return false;
            D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
            m_context->UpdateSubresource(b.buffer.Get(), 0, &box, data, 0, 0);
            return true;
        }

        bool D3D11GfxDevice::EnsureBufferStaging(uint32_t size)
        {
            if (m_bufferStaging && m_bufferStagingSize >= size)
                return true;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = size;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            m_bufferStaging.Reset();
            if (FAILED(m_device->CreateBuffer(&bd, nullptr, m_bufferStaging.GetAddressOf())))
                return false;
            m_bufferStagingSize = size;
            return true;
        }

        bool D3D11GfxDevice::ReadBuffer(Buffer &src, const ByteRange *ranges, size_t rangeCount, void *dstBase)
        {
            auto &b = static_cast<HwBuffer &>(src);
            if (!EnsureBufferStaging(b.desc.size))
                return false;
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < rangeCount; ++i)
            {
                D3D11_BOX box{ranges[i].offset, 0, 0, ranges[i].offset + ranges[i].size, 1, 1};
                m_context->CopySubresourceRegion(m_bufferStaging.Get(), 0, ranges[i].offset, 0, 0, b.buffer.Get(), 0, &box);
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_context->Map(m_bufferStaging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return false;
            for (size_t i = 0; i < rangeCount; ++i)
                std::memcpy(static_cast<uint8_t *>(dstBase) + ranges[i].offset, static_cast<const uint8_t *>(m.pData) + ranges[i].offset,
                            ranges[i].size);
            m_context->Unmap(m_bufferStaging.Get(), 0);
            ++m_stats.readbacks;
            m_stats.readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return true;
        }

        bool D3D11GfxDevice::UploadSurface(Surface &dst, const Rect &rect, const void *data, uint32_t rowPitch)
        {
            auto &s = static_cast<HwSurface &>(dst);
            D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
            m_context->UpdateSubresource(s.texture.Get(), 0, &box, data, rowPitch, 0);
            return true;
        }

        bool D3D11GfxDevice::EnsureStaging(uint32_t width, uint32_t height, DXGI_FORMAT format)
        {
            if (m_staging && m_stagingW >= width && m_stagingH >= height && m_stagingFormat == format)
                return true;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = std::max(width, m_stagingFormat == format ? m_stagingW : 0u);
            td.Height = std::max(height, m_stagingFormat == format ? m_stagingH : 0u);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            m_staging.Reset();
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_staging.GetAddressOf())))
                return false;
            m_stagingW = td.Width;
            m_stagingH = td.Height;
            m_stagingFormat = format;
            return true;
        }

        bool D3D11GfxDevice::ReadbackSurface(Surface &src, const Rect &rect, void *dst, uint32_t rowPitch)
        {
            auto &s = static_cast<HwSurface &>(src);
            D3D11_TEXTURE2D_DESC td{};
            s.texture->GetDesc(&td);
            if (!EnsureStaging(rect.width, rect.height, td.Format))
                return false;
            const auto t0 = std::chrono::steady_clock::now();
            D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
            m_context->CopySubresourceRegion(m_staging.Get(), 0, 0, 0, 0, s.texture.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return false;
            for (uint32_t y = 0; y < rect.height; ++y)
                std::memcpy(static_cast<uint8_t *>(dst) + static_cast<size_t>(y) * rowPitch,
                            static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch, rect.width * 4u);
            m_context->Unmap(m_staging.Get(), 0);
            ++m_stats.readbacks;
            m_stats.readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return true;
        }

        bool D3D11GfxDevice::CopySurface(Surface &dst, uint32_t dstX, uint32_t dstY, Surface &src, const Rect &r)
        {
            auto &d = static_cast<HwSurface &>(dst);
            auto &s = static_cast<HwSurface &>(src);
            D3D11_BOX box{r.x, r.y, 0, r.x + r.width, r.y + r.height, 1};
            m_context->CopySubresourceRegion(d.texture.Get(), 0, dstX, dstY, 0, s.texture.Get(), 0, &box);
            ++m_stats.copies;
            return true;
        }

        bool D3D11GfxDevice::ClearColor(Surface &target, const float rgba[4])
        {
            auto &s = static_cast<HwSurface &>(target);
            if (!s.rtv)
                return false;
            m_context->ClearRenderTargetView(s.rtv.Get(), rgba);
            return true;
        }

        bool D3D11GfxDevice::ClearDepth(Surface &target, float depth)
        {
            auto &s = static_cast<HwSurface &>(target);
            if (!s.dsv)
                return false;
            m_context->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH, depth, 0);
            return true;
        }

        ID3D11ShaderResourceView *D3D11GfxDevice::Srv(const ShaderResource &r) const
        {
            if (r.surface)
                return static_cast<HwSurface *>(r.surface)->srv.Get();
            if (r.buffer)
                return static_cast<HwBuffer *>(r.buffer)->srv.Get();
            return nullptr;
        }

        void D3D11GfxDevice::UnbindAll()
        {
            ID3D11ShaderResourceView *nullSrv[4] = {};
            ID3D11UnorderedAccessView *nullUav[2] = {};
            m_context->PSSetShaderResources(0, 4, nullSrv);
            std::memset(m_psSrv, 0, sizeof(m_psSrv));
            m_context->VSSetShaderResources(0, 4, nullSrv);
            m_context->CSSetShaderResources(0, 4, nullSrv);
            m_context->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }

        bool D3D11GfxDevice::WriteConstants(const void *data, uint32_t size, ID3D11Buffer **outBuffer, UINT &first, UINT &num)
        {
            const uint32_t aligned = (std::max(size, 16u) + 255u) & ~255u;
            if (aligned > kMaxConstantBytes)
                return false;
            m_stats.constantBytes += aligned;
            if (m_context1)
            {
                D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
                if (m_constantCursor + aligned > kConstantRingBytes)
                {
                    m_constantCursor = 0;
                    mode = D3D11_MAP_WRITE_DISCARD;
                    m_cbValid = false; // the ring was renamed: the bound draw constants are gone
                }
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(m_context->Map(m_constantRing.Get(), 0, mode, 0, &m)))
                    return false;
                std::memcpy(static_cast<uint8_t *>(m.pData) + m_constantCursor, data, size);
                m_context->Unmap(m_constantRing.Get(), 0);
                *outBuffer = m_constantRing.Get();
                first = m_constantCursor / 16u;
                num = aligned / 16u;
                m_constantCursor += aligned;
                return true;
            }
            m_cbValid = false; // single shared buffer: any write replaces the bound draw constants
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_context->Map(m_constantFallback.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return false;
            std::memcpy(m.pData, data, size);
            m_context->Unmap(m_constantFallback.Get(), 0);
            *outBuffer = m_constantFallback.Get();
            first = 0;
            num = kMaxConstantBytes / 16u;
            return true;
        }

        bool D3D11GfxDevice::Dispatch(GfxProgram &program, const ComputeBindings &bindings, const void *constants,
                                      uint32_t constantBytes, uint32_t gx, uint32_t gy, uint32_t gz)
        {
            if (m_inPass)
                return false;
            auto &p = static_cast<HwProgram &>(program);
            UnbindAll();
            ID3D11ShaderResourceView *srvs[4] = {};
            for (size_t i = 0; i < 4; ++i)
                srvs[i] = Srv(bindings.resources[i]);
            ID3D11UnorderedAccessView *uavs[2] = {};
            for (size_t i = 0; i < 2; ++i)
            {
                if (bindings.storageSurfaces[i])
                    uavs[i] = static_cast<HwSurface *>(bindings.storageSurfaces[i])->uav.Get();
                else if (bindings.storageBuffers[i])
                    uavs[i] = static_cast<HwBuffer *>(bindings.storageBuffers[i])->uav.Get();
            }
            if (constants && constantBytes)
            {
                ID3D11Buffer *cb = nullptr;
                UINT first = 0, num = 0;
                if (!WriteConstants(constants, constantBytes, &cb, first, num))
                    return false;
                if (m_context1)
                    m_context1->CSSetConstantBuffers1(0, 1, &cb, &first, &num);
                else
                    m_context->CSSetConstantBuffers(0, 1, &cb);
            }
            m_context->CSSetShader(p.cs.Get(), nullptr, 0);
            m_context->CSSetShaderResources(0, 4, srvs);
            m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            m_context->Dispatch(gx, gy, gz);
            ID3D11UnorderedAccessView *nullUav[2] = {};
            ID3D11ShaderResourceView *nullSrv[4] = {};
            m_context->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
            m_context->CSSetShaderResources(0, 4, nullSrv);
            ++m_stats.dispatches;
            return true;
        }

        bool D3D11GfxDevice::BeginPass(Surface *color, Surface *depth, uint32_t vw, uint32_t vh)
        {
            if (m_inPass)
                EndPass();
            ID3D11ShaderResourceView *nullSrv[4] = {};
            m_context->PSSetShaderResources(0, 4, nullSrv);
            std::memset(m_psSrv, 0, sizeof(m_psSrv));
            m_context->VSSetShaderResources(0, 4, nullSrv);
            ID3D11RenderTargetView *rtv = color ? static_cast<HwSurface *>(color)->rtv.Get() : nullptr;
            ID3D11DepthStencilView *dsv = depth ? static_cast<HwSurface *>(depth)->dsv.Get() : nullptr;
            m_context->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
            const SurfaceDesc &d = color ? color->Desc() : depth->Desc();
            D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(vw ? vw : d.width), static_cast<float>(vh ? vh : d.height), 0.0f, 1.0f};
            m_context->RSSetViewports(1, &vp);
            m_context->RSSetState(m_rasterizer.Get());
            D3D11_RECT sr{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
            m_context->RSSetScissorRects(1, &sr);
            m_scissor = sr;
            m_scissorValid = true;
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11SamplerState *samplers[2] = {m_pointClamp.Get(), m_linearClamp.Get()};
            m_context->PSSetSamplers(0, 2, samplers);
            m_pipeline = nullptr;
            m_boundProgram = nullptr;
            m_blendDirty = true;
            m_inPass = true;
            ++m_stats.passes;
            return true;
        }

        void D3D11GfxDevice::SetPipeline(Pipeline &pipeline)
        {
            auto *p = static_cast<HwPipeline *>(&pipeline);
            if (p == m_pipeline && !m_blendDirty)
                return;
            if (p->program != m_boundProgram)
            {
                m_context->VSSetShader(p->program->vs.Get(), nullptr, 0);
                m_context->PSSetShader(p->program->ps.Get(), nullptr, 0);
                m_context->IASetInputLayout(p->program->layout.Get());
                m_boundProgram = p->program;
            }
            const float bf[4] = {m_blendConstant, m_blendConstant, m_blendConstant, m_blendConstant};
            m_context->OMSetBlendState(p->blend.Get(), bf, 0xFFFFFFFFu);
            m_context->OMSetDepthStencilState(p->depth.Get(), 0);
            m_pipeline = p;
            m_blendDirty = false;
            ++m_stats.pipelineSwitches;
        }

        void D3D11GfxDevice::SetConstants(const void *data, uint32_t size)
        {
            if (m_cbValid && size == m_cbLastSize && std::memcmp(m_cbLast, data, size) == 0)
                return; // same bytes still bound
            ID3D11Buffer *cb = nullptr;
            UINT first = 0, num = 0;
            if (!WriteConstants(data, size, &cb, first, num))
                return;
            if (size <= sizeof(m_cbLast))
            {
                std::memcpy(m_cbLast, data, size);
                m_cbLastSize = size;
                m_cbValid = true;
            }
            if (m_context1)
            {
                m_context1->VSSetConstantBuffers1(0, 1, &cb, &first, &num);
                m_context1->PSSetConstantBuffers1(0, 1, &cb, &first, &num);
            }
            else
            {
                m_context->VSSetConstantBuffers(0, 1, &cb);
                m_context->PSSetConstantBuffers(0, 1, &cb);
            }
        }

        void D3D11GfxDevice::SetTextures(const ShaderResource *resources, uint32_t count)
        {
            ID3D11ShaderResourceView *srvs[4] = {};
            for (uint32_t i = 0; i < count && i < 4; ++i)
                srvs[i] = Srv(resources[i]);
            if (std::memcmp(srvs, m_psSrv, sizeof(srvs)) == 0)
                return;
            m_context->PSSetShaderResources(0, 4, srvs);
            std::memcpy(m_psSrv, srvs, sizeof(srvs));
        }

        void D3D11GfxDevice::SetScissor(const Rect &r)
        {
            D3D11_RECT sr{static_cast<LONG>(r.x), static_cast<LONG>(r.y), static_cast<LONG>(r.x + r.width),
                          static_cast<LONG>(r.y + r.height)};
            if (m_scissorValid && std::memcmp(&sr, &m_scissor, sizeof(sr)) == 0)
                return;
            m_context->RSSetScissorRects(1, &sr);
            m_scissor = sr;
            m_scissorValid = true;
        }

        bool D3D11GfxDevice::Draw(const void *vertices, uint32_t vertexCount)
        {
            if (!m_inPass || !m_pipeline || vertexCount == 0)
                return false;
            if (m_blendDirty)
                SetPipeline(*m_pipeline);
            const uint32_t stride = m_pipeline->program->stride;
            const uint8_t *src = static_cast<const uint8_t *>(vertices);
            while (vertexCount > 0)
            {
                const uint32_t maxVerts = (kVertexRingBytes / stride) / 3u * 3u;
                const uint32_t n = std::min(vertexCount, maxVerts);
                const uint32_t bytes = n * stride;
                D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
                uint32_t start = (m_vertexCursor + stride - 1u) / stride * stride;
                if (start + bytes > kVertexRingBytes)
                {
                    start = 0;
                    mode = D3D11_MAP_WRITE_DISCARD;
                }
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(m_context->Map(m_vertexRing.Get(), 0, mode, 0, &m)))
                    return false;
                std::memcpy(static_cast<uint8_t *>(m.pData) + start, src, bytes);
                m_context->Unmap(m_vertexRing.Get(), 0);
                ID3D11Buffer *vb = m_vertexRing.Get();
                const UINT offset = 0;
                m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                m_context->Draw(n, start / stride);
                m_vertexCursor = start + bytes;
                m_stats.vertexBytes += bytes;
                ++m_stats.draws;
                src += bytes;
                vertexCount -= n;
            }
            return true;
        }

        void D3D11GfxDevice::EndPass()
        {
            if (!m_inPass)
                return;
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView *nullSrv[4] = {};
            m_context->PSSetShaderResources(0, 4, nullSrv);
            std::memset(m_psSrv, 0, sizeof(m_psSrv));
            m_inPass = false;
        }

        bool D3D11GfxDevice::SetPresentFilter(const char *source, std::string &error)
        {
            m_filterPs.Reset();
            if (!source)
                return true;
            const std::string full = std::string("struct PresentFilterIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 uv01 : TEXCOORD1; };\n") + source;
            ComPtr<ID3DBlob> blob;
            if (!Compile(full.c_str(), "gs_hw_present_filter", "PresentFilterMain", "ps_5_0", {}, blob, error) ||
                FAILED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_filterPs.GetAddressOf())))
            {
                if (error.empty())
                    error = "present filter shader creation failed";
                m_filterPs.Reset();
                return false;
            }
            return true;
        }

        bool D3D11GfxDevice::PresentSurface(Surface &surface, const Rect &rect, const FilterConstantsFn &filterConstants)
        {
            if (!m_swapchain || m_windowClosed)
                return false;
            if (m_inPass)
                EndPass();
            if (m_resizePending)
            {
                m_resizePending = false;
                RECT cr{};
                GetClientRect(m_hwnd, &cr);
                if (cr.right > 0 && cr.bottom > 0)
                {
                    m_backbufferRtv.Reset();
                    m_context->OMSetRenderTargets(0, nullptr, nullptr);
                    m_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
                    CreateBackbufferView();
                }
            }
            if (!m_backbufferRtv)
                return false;
            auto &s = static_cast<HwSurface &>(surface);
            const float black[4] = {0, 0, 0, 1};
            m_context->ClearRenderTargetView(m_backbufferRtv.Get(), black);
            ID3D11RenderTargetView *rtv = m_backbufferRtv.Get();
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            // Letterbox, aspect of the source rectangle (the GS output is 4:3 regardless of its pixels).
            const float bw = static_cast<float>(m_backbufferWidth), bh = static_cast<float>(m_backbufferHeight);
            const float aspect = 4.0f / 3.0f;
            float w = bw, h = bw / aspect;
            if (h > bh)
            {
                h = bh;
                w = bh * aspect;
            }
            D3D11_VIEWPORT vp{(bw - w) * 0.5f, (bh - h) * 0.5f, w, h, 0.0f, 1.0f};
            m_context->RSSetViewports(1, &vp);
            m_context->RSSetState(m_rasterizerNoScissor.Get());
            const float sw = static_cast<float>(s.desc.width), sh = static_cast<float>(s.desc.height);
            const float uvRect[4] = {rect.x / sw, rect.y / sh, rect.width / sw, rect.height / sh};
            ID3D11Buffer *cb = nullptr;
            UINT first = 0, num = 0;
            WriteConstants(uvRect, sizeof(uvRect), &cb, first, num);
            if (m_context1)
                m_context1->VSSetConstantBuffers1(1, 1, &cb, &first, &num);
            else
                m_context->VSSetConstantBuffers(1, 1, &cb);
            const bool filtered = m_filterPs && filterConstants;
            if (filtered)
            {
                m_filterConstants.clear();
                filterConstants(static_cast<uint32_t>(w), static_cast<uint32_t>(h), m_filterConstants);
                if (!m_filterConstants.empty())
                {
                    ID3D11Buffer *fcb = nullptr;
                    UINT ff = 0, fn = 0;
                    WriteConstants(m_filterConstants.data(), static_cast<uint32_t>(m_filterConstants.size()), &fcb, ff, fn);
                    if (m_context1)
                        m_context1->PSSetConstantBuffers1(0, 1, &fcb, &ff, &fn);
                    else
                        m_context->PSSetConstantBuffers(0, 1, &fcb);
                }
            }
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_blitVs.Get(), nullptr, 0);
            m_context->PSSetShader(filtered ? m_filterPs.Get() : m_blitPs.Get(), nullptr, 0);
            ID3D11ShaderResourceView *srv = s.srv.Get();
            m_context->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState *smp = (m_config.linearPresent || filtered) ? m_linearClamp.Get() : m_pointClamp.Get();
            m_context->PSSetSamplers(0, 1, &smp);
            m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            m_context->OMSetDepthStencilState(nullptr, 0);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView *nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);
            m_psSrv[0] = nullptr;
            m_cbValid = false; // the present filter may have replaced PS constant slot 0
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_boundProgram = nullptr;
            m_pipeline = nullptr;
            const HRESULT hr = m_swapchain->Present(m_config.vsync ? 1 : 0, m_config.vsync ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
            return SUCCEEDED(hr) || hr == DXGI_ERROR_WAS_STILL_DRAWING;
        }

        bool D3D11GfxDevice::PumpEvents()
        {
            if (!m_hwnd)
                return true;
            MSG msg;
            while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return !m_windowClosed;
        }

        void D3D11GfxDevice::WaitIdle()
        {
            m_context->End(m_idleQuery.Get());
            m_context->Flush();
            BOOL done = FALSE;
            while (m_context->GetData(m_idleQuery.Get(), &done, sizeof(done), 0) == S_FALSE)
                YieldProcessor();
        }
    }

    std::unique_ptr<GfxDevice> CreateD3D11GfxDevice(const GfxDeviceConfig &config, std::string &error)
    {
        auto device = std::make_unique<D3D11GfxDevice>();
        if (!device->Create(config, error))
            return nullptr;
        return device;
    }
}

#else

namespace gs_rhi
{
    std::unique_ptr<GfxDevice> CreateD3D11GfxDevice(const GfxDeviceConfig &, std::string &error)
    {
        error = "D3D11 is only available on Windows";
        return nullptr;
    }
}

#endif

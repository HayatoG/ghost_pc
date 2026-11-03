// D3D11 implementation of the thin GS RHI (include/runtime/gs/gs_rhi.h).
// Owns a Win32 output window + flip-model swapchain. Shaders are HLSL strings compiled at
// runtime with D3DCompile (d3dcompiler_47.dll ships with Windows 10+).
// Libraries: d3d11, dxgi, d3dcompiler (Windows SDK only, no extra dependency).

#include "runtime/gs/gs_rhi.h"

#include <cstring>
#include <iostream>

namespace gs_rhi
{
    const char *FormatName(Format format)
    {
        switch (format)
        {
        case Format::RGBA8:
            return "RGBA8";
        }
        return "?";
    }
}

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
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

        constexpr wchar_t kWindowClass[] = L"PS2X_GS_D3D11";
        constexpr UINT kSwapchainBuffers = 2;

        // Full-screen triangle from SV_VertexID; no vertex buffer, no constant buffer.
        constexpr char kBlitHlsl[] = R"(
Texture2D    gSource  : register(t0);
SamplerState gSampler : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // The GS output alpha is not a window alpha: present opaque.
    return float4(gSource.Sample(gSampler, i.uv).rgb, 1.0);
}
)";

        std::string HrText(HRESULT hr)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
            return buf;
        }

        std::string Narrow(const wchar_t *text)
        {
            if (!text || !*text)
                return {};
            const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1)
                return {};
            std::string out(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
            return out;
        }

        std::wstring Widen(const std::string &text)
        {
            if (text.empty())
                return {};
            const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            if (len <= 1)
                return {};
            std::wstring out(static_cast<size_t>(len - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
            return out;
        }

        const char *FeatureLevelName(D3D_FEATURE_LEVEL level)
        {
            switch (level)
            {
            case D3D_FEATURE_LEVEL_11_1:
                return "11_1";
            case D3D_FEATURE_LEVEL_11_0:
                return "11_0";
            case D3D_FEATURE_LEVEL_10_1:
                return "10_1";
            case D3D_FEATURE_LEVEL_10_0:
                return "10_0";
            default:
                return "other";
            }
        }

        DXGI_FORMAT ToDxgi(Format format)
        {
            switch (format)
            {
            case Format::RGBA8:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        bool CompileShader(const char *entry, const char *target, ComPtr<ID3DBlob> &out, std::string &error)
        {
            ComPtr<ID3DBlob> errors;
            const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            const HRESULT hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "gs_rhi_blit.hlsl", nullptr, nullptr,
                                          entry, target, flags, 0, out.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(hr))
            {
                error = std::string("D3DCompile ") + entry + " failed " + HrText(hr);
                if (errors)
                    error += ": " + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
                return false;
            }
            return true;
        }

        class D3D11Texture final : public Texture
        {
        public:
            uint32_t Width() const override { return desc.width; }
            uint32_t Height() const override { return desc.height; }
            Format GetFormat() const override { return desc.format; }

            TextureDesc desc{};
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView> rtv;
        };

        class D3D11Device final : public Device
        {
        public:
            ~D3D11Device() override;

            bool Create(const DeviceConfig &config, std::string &error);

            const AdapterInfo &Info() const override { return m_info; }
            std::unique_ptr<Texture> CreateTexture(const TextureDesc &desc) override;
            bool UpdateTexture(Texture &texture, const void *data, uint32_t rowPitch) override;
            bool ReadbackTexture(Texture &texture, void *dst, uint32_t dstRowPitch) override;
            bool PresentTexture(Texture &texture, uint32_t srcWidth, uint32_t srcHeight) override;
            void WaitIdle() override;
            bool PumpEvents() override;

        private:
            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
            bool CreateOutputWindow(const DeviceConfig &config, std::string &error);
            bool CreateDeviceAndSwapchain(const DeviceConfig &config, std::string &error);
            bool CreateBlitPipeline(const DeviceConfig &config, std::string &error);
            bool CreateBackbufferView();
            bool ResizeIfNeeded();

            HWND m_hwnd = nullptr;
            bool m_windowClosed = false;
            bool m_resizePending = false;
            bool m_vsync = false;
            AdapterInfo m_info{};

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            ComPtr<IDXGISwapChain1> m_swapchain;
            ComPtr<ID3D11RenderTargetView> m_backbufferRtv;
            UINT m_backbufferWidth = 0;
            UINT m_backbufferHeight = 0;

            ComPtr<ID3D11VertexShader> m_blitVs;
            ComPtr<ID3D11PixelShader> m_blitPs;
            ComPtr<ID3D11SamplerState> m_sampler;
            ComPtr<ID3D11RasterizerState> m_rasterizer;
            ComPtr<ID3D11Query> m_idleQuery;
        };

        D3D11Device::~D3D11Device()
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
                m_hwnd = nullptr;
            }
        }

        LRESULT CALLBACK D3D11Device::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            auto *self = reinterpret_cast<D3D11Device *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_SIZE:
                if (self)
                    self->m_resizePending = true;
                return 0;
            case WM_CLOSE:
                // Closing the GPU window only stops GPU presentation; the runtime keeps running.
                if (self)
                    self->m_windowClosed = true;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        }

        bool D3D11Device::CreateOutputWindow(const DeviceConfig &config, std::string &error)
        {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &D3D11Device::WndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW, independent of UNICODE
            wc.lpszClassName = kWindowClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                error = "RegisterClassExW failed " + std::to_string(GetLastError());
                return false;
            }

            RECT rect{0, 0, static_cast<LONG>(config.windowWidth), static_cast<LONG>(config.windowHeight)};
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            const std::wstring title = Widen(config.windowTitle);
            m_hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                     nullptr, nullptr, instance, nullptr);
            if (!m_hwnd)
            {
                error = "CreateWindowExW failed " + std::to_string(GetLastError());
                return false;
            }
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            return true;
        }

        bool D3D11Device::CreateDeviceAndSwapchain(const DeviceConfig &config, std::string &error)
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            if (config.debugLayer)
                flags |= D3D11_CREATE_DEVICE_DEBUG;

            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
            D3D_FEATURE_LEVEL obtained{};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                           m_device.GetAddressOf(), &obtained, m_context.GetAddressOf());
            if (FAILED(hr) && config.debugLayer)
            {
                // Debug layer missing on this machine: retry without it.
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       m_device.GetAddressOf(), &obtained, m_context.GetAddressOf());
            }
            if (FAILED(hr))
            {
                error = "D3D11CreateDevice failed " + HrText(hr);
                return false;
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
                FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
            {
                error = "cannot reach the DXGI factory (needs DXGI 1.2 / Windows 8+)";
                return false;
            }

            DXGI_ADAPTER_DESC adapterDesc{};
            adapter->GetDesc(&adapterDesc);
            m_info.api = "d3d11";
            m_info.description = Narrow(adapterDesc.Description);
            m_info.vendorId = adapterDesc.VendorId;
            m_info.deviceId = adapterDesc.DeviceId;
            m_info.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
            m_info.featureLevel = FeatureLevelName(obtained);
            m_info.software = adapterDesc.VendorId == 0x1414u && adapterDesc.DeviceId == 0x8cu; // Basic Render Driver

            DXGI_SWAP_CHAIN_DESC1 sc{};
            sc.Width = 0; // use the client area
            sc.Height = 0;
            sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sc.SampleDesc.Count = 1;
            sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc.BufferCount = kSwapchainBuffers;
            sc.Scaling = DXGI_SCALING_STRETCH;
            sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &sc, nullptr, nullptr, m_swapchain.GetAddressOf());
            if (FAILED(hr))
            {
                error = "CreateSwapChainForHwnd failed " + HrText(hr);
                return false;
            }
            factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (!CreateBackbufferView())
            {
                error = "backbuffer view creation failed";
                return false;
            }
            return true;
        }

        bool D3D11Device::CreateBlitPipeline(const DeviceConfig &config, std::string &error)
        {
            ComPtr<ID3DBlob> vsBlob;
            ComPtr<ID3DBlob> psBlob;
            if (!CompileShader("VSMain", "vs_4_0", vsBlob, error) || !CompileShader("PSMain", "ps_4_0", psBlob, error))
                return false;
            if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_blitVs.GetAddressOf())) ||
                FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_blitPs.GetAddressOf())))
            {
                error = "CreateVertexShader/CreatePixelShader failed";
                return false;
            }

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = config.filter == Filter::Linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = TRUE; // crops the letterboxed source region (see PresentTexture)
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_EVENT;
            if (FAILED(m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf())) ||
                FAILED(m_device->CreateRasterizerState(&rd, m_rasterizer.GetAddressOf())) ||
                FAILED(m_device->CreateQuery(&qd, m_idleQuery.GetAddressOf())))
            {
                error = "sampler/rasterizer/query creation failed";
                return false;
            }
            return true;
        }

        bool D3D11Device::Create(const DeviceConfig &config, std::string &error)
        {
            m_vsync = config.vsync;
            return CreateOutputWindow(config, error) && CreateDeviceAndSwapchain(config, error) &&
                   CreateBlitPipeline(config, error);
        }

        bool D3D11Device::CreateBackbufferView()
        {
            ComPtr<ID3D11Texture2D> backbuffer;
            if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()))))
                return false;
            D3D11_TEXTURE2D_DESC desc{};
            backbuffer->GetDesc(&desc);
            m_backbufferWidth = desc.Width;
            m_backbufferHeight = desc.Height;
            return SUCCEEDED(m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, m_backbufferRtv.ReleaseAndGetAddressOf()));
        }

        bool D3D11Device::ResizeIfNeeded()
        {
            if (!m_resizePending)
                return true;
            m_resizePending = false;
            RECT client{};
            GetClientRect(m_hwnd, &client);
            if (client.right <= 0 || client.bottom <= 0)
                return false; // minimized: skip presenting
            if (static_cast<UINT>(client.right) == m_backbufferWidth && static_cast<UINT>(client.bottom) == m_backbufferHeight)
                return true;
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_backbufferRtv.Reset();
            const HRESULT hr = m_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr))
            {
                std::cerr << "[gs-rhi] ResizeBuffers failed " << HrText(hr) << std::endl;
                return false;
            }
            return CreateBackbufferView();
        }

        std::unique_ptr<Texture> D3D11Device::CreateTexture(const TextureDesc &desc)
        {
            if (desc.width == 0 || desc.height == 0 || desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || (desc.cpuWritable && desc.renderTarget))
            {
                std::cerr << "[gs-rhi] invalid texture desc " << desc.width << "x" << desc.height << std::endl;
                return nullptr;
            }

            auto tex = std::make_unique<D3D11Texture>();
            tex->desc = desc;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = desc.width;
            td.Height = desc.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = ToDxgi(desc.format);
            td.SampleDesc.Count = 1;
            td.Usage = desc.cpuWritable ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (desc.renderTarget ? D3D11_BIND_RENDER_TARGET : 0u);
            td.CPUAccessFlags = desc.cpuWritable ? D3D11_CPU_ACCESS_WRITE : 0u;
            HRESULT hr = m_device->CreateTexture2D(&td, nullptr, tex->texture.GetAddressOf());
            if (SUCCEEDED(hr))
                hr = m_device->CreateShaderResourceView(tex->texture.Get(), nullptr, tex->srv.GetAddressOf());
            if (SUCCEEDED(hr) && desc.renderTarget)
                hr = m_device->CreateRenderTargetView(tex->texture.Get(), nullptr, tex->rtv.GetAddressOf());
            if (FAILED(hr))
            {
                std::cerr << "[gs-rhi] CreateTexture " << desc.width << "x" << desc.height << " failed " << HrText(hr) << std::endl;
                return nullptr;
            }
            return tex;
        }

        bool D3D11Device::UpdateTexture(Texture &texture, const void *data, uint32_t rowPitch)
        {
            auto &tex = static_cast<D3D11Texture &>(texture);
            const uint32_t rowBytes = tex.desc.width * 4u;
            if (!data || rowPitch < rowBytes)
                return false;

            if (!tex.desc.cpuWritable)
            {
                m_context->UpdateSubresource(tex.texture.Get(), 0, nullptr, data, rowPitch, 0);
                return true;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = m_context->Map(tex.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr))
            {
                std::cerr << "[gs-rhi] Map failed " << HrText(hr) << std::endl;
                return false;
            }
            const auto *src = static_cast<const uint8_t *>(data);
            auto *dst = static_cast<uint8_t *>(mapped.pData);
            if (mapped.RowPitch == rowPitch && rowPitch == rowBytes)
            {
                std::memcpy(dst, src, static_cast<size_t>(rowBytes) * tex.desc.height);
            }
            else
            {
                for (uint32_t y = 0; y < tex.desc.height; ++y)
                    std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch, src + static_cast<size_t>(y) * rowPitch, rowBytes);
            }
            m_context->Unmap(tex.texture.Get(), 0);
            return true;
        }

        bool D3D11Device::ReadbackTexture(Texture &texture, void *dst, uint32_t dstRowPitch)
        {
            auto &tex = static_cast<D3D11Texture &>(texture);
            const uint32_t rowBytes = tex.desc.width * 4u;
            if (!dst || dstRowPitch < rowBytes)
                return false;

            D3D11_TEXTURE2D_DESC td{};
            tex.texture->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging; // stage 7 will keep a pool; one-off is fine for now
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, staging.GetAddressOf())))
                return false;
            m_context->CopyResource(staging.Get(), tex.texture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return false;
            auto *out = static_cast<uint8_t *>(dst);
            const auto *in = static_cast<const uint8_t *>(mapped.pData);
            for (uint32_t y = 0; y < tex.desc.height; ++y)
                std::memcpy(out + static_cast<size_t>(y) * dstRowPitch, in + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
            m_context->Unmap(staging.Get(), 0);
            return true;
        }

        bool D3D11Device::PresentTexture(Texture &texture, uint32_t srcWidth, uint32_t srcHeight)
        {
            if (m_windowClosed || !ResizeIfNeeded() || !m_backbufferRtv)
                return false;
            auto &tex = static_cast<D3D11Texture &>(texture);
            srcWidth = std::clamp<uint32_t>(srcWidth, 1u, tex.desc.width);
            srcHeight = std::clamp<uint32_t>(srcHeight, 1u, tex.desc.height);

            // Letterbox: largest rect with the source aspect that fits the backbuffer. The viewport
            // covers the whole texture, scaled so that only the source region lands on screen.
            const float bbW = static_cast<float>(m_backbufferWidth);
            const float bbH = static_cast<float>(m_backbufferHeight);
            const float scale = std::min(bbW / static_cast<float>(srcWidth), bbH / static_cast<float>(srcHeight));
            const float dstW = static_cast<float>(srcWidth) * scale;
            const float dstH = static_cast<float>(srcHeight) * scale;

            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            m_context->ClearRenderTargetView(m_backbufferRtv.Get(), black);

            D3D11_VIEWPORT vp{};
            vp.TopLeftX = (bbW - dstW) * 0.5f;
            vp.TopLeftY = (bbH - dstH) * 0.5f;
            vp.Width = static_cast<float>(tex.desc.width) * scale;
            vp.Height = static_cast<float>(tex.desc.height) * scale;
            vp.MaxDepth = 1.0f;
            D3D11_RECT scissor{static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
                               static_cast<LONG>(vp.TopLeftX + dstW + 0.5f), static_cast<LONG>(vp.TopLeftY + dstH + 0.5f)};

            m_context->OMSetRenderTargets(1, m_backbufferRtv.GetAddressOf(), nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->RSSetScissorRects(1, &scissor);
            m_context->RSSetState(m_rasterizer.Get());
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_blitVs.Get(), nullptr, 0);
            m_context->PSSetShader(m_blitPs.Get(), nullptr, 0);
            m_context->PSSetShaderResources(0, 1, tex.srv.GetAddressOf());
            m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
            m_context->Draw(3, 0);

            ID3D11ShaderResourceView *nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);

            const HRESULT hr = m_swapchain->Present(m_vsync ? 1u : 0u, 0u);
            if (FAILED(hr))
            {
                std::cerr << "[gs-rhi] Present failed " << HrText(hr) << std::endl;
                return false;
            }
            return true;
        }

        void D3D11Device::WaitIdle()
        {
            m_context->End(m_idleQuery.Get());
            m_context->Flush();
            BOOL done = FALSE;
            while (m_context->GetData(m_idleQuery.Get(), &done, sizeof(done), 0) == S_FALSE)
                YieldProcessor();
        }

        bool D3D11Device::PumpEvents()
        {
            MSG msg{};
            while (m_hwnd && PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return !m_windowClosed;
        }
    }

    std::unique_ptr<Device> CreateD3D11Device(const DeviceConfig &config, std::string &error)
    {
        auto device = std::make_unique<D3D11Device>();
        if (!device->Create(config, error))
            return nullptr;
        return device;
    }
}

#else // !_WIN32

namespace gs_rhi
{
    std::unique_ptr<Device> CreateD3D11Device(const DeviceConfig &, std::string &error)
    {
        error = "D3D11 is only available on Windows";
        return nullptr;
    }
}

#endif

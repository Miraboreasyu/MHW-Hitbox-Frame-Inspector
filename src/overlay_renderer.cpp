#include "overlay_renderer.h"
#include "collision_collector.h"
#include "mhw_addresses.h"

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstring>
#include <vector>

namespace render {
namespace {

constexpr const char* kOverlayHlsl = R"(
struct VSIn {
    float4 pos : POSITION;
    float4 col : COLOR0;
};
struct PSIn {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
};
PSIn VSMain(VSIn i) {
    PSIn o;
    o.pos = i.pos;
    o.col = i.col;
    return o;
}
float4 PSMain(PSIn i) : SV_TARGET {
    return i.col;
}
)";

ID3DBlob* g_vsBlob = nullptr;
ID3DBlob* g_psBlob = nullptr;
bool g_shaderTried = false;

bool EnsureShaderBlobs() {
    if (g_vsBlob && g_psBlob) return true;
    if (g_shaderTried) return false;
    g_shaderTried = true;

    HMODULE d3dc = ::GetModuleHandleW(L"d3dcompiler_47.dll");
    if (!d3dc) d3dc = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dc) {
        app::Log("shader: failed to load d3dcompiler_47.dll (err=%lu)", ::GetLastError());
        return false;
    }

    auto fnCompile = reinterpret_cast<pD3DCompile>(::GetProcAddress(d3dc, "D3DCompile"));
    if (!fnCompile) {
        app::Log("shader: D3DCompile export not found");
        return false;
    }

    ID3DBlob* err = nullptr;
    HRESULT hr = fnCompile(kOverlayHlsl, std::strlen(kOverlayHlsl), "overlay.hlsl",
                           nullptr, nullptr, "VSMain", "vs_4_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_vsBlob, &err);
    if (FAILED(hr)) {
        app::Log("shader: VS compile failed hr=0x%08lX (%s)", hr,
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = fnCompile(kOverlayHlsl, std::strlen(kOverlayHlsl), "overlay.hlsl",
                   nullptr, nullptr, "PSMain", "ps_4_0",
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_psBlob, &err);
    if (FAILED(hr)) {
        app::Log("shader: PS compile failed hr=0x%08lX (%s)", hr,
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    app::Log("shader: compiled VS/PS blobs successfully");
    return true;
}

DXGI_FORMAT ResolveTypedRtvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:                                return fmt;
    }
}

// ============================================================================
//  DirectX 11 Overlay State & Renderer
// ============================================================================
struct D3D11State {
    ID3D11Device* device             = nullptr;
    ID3D11VertexShader* vs           = nullptr;
    ID3D11PixelShader* ps            = nullptr;
    ID3D11InputLayout* layout        = nullptr;
    ID3D11BlendState* blend          = nullptr;
    ID3D11RasterizerState* rast      = nullptr;
    ID3D11DepthStencilState* depth   = nullptr;
    ID3D11Buffer* vb                 = nullptr;
    std::size_t vbCapVerts           = 0;
    bool loggedInit                  = false;

    void release() {
        if (vb)     { vb->Release();     vb = nullptr; }
        if (depth)  { depth->Release();  depth = nullptr; }
        if (rast)   { rast->Release();   rast = nullptr; }
        if (blend)  { blend->Release();  blend = nullptr; }
        if (layout) { layout->Release(); layout = nullptr; }
        if (ps)     { ps->Release();     ps = nullptr; }
        if (vs)     { vs->Release();     vs = nullptr; }
        device = nullptr;
        vbCapVerts = 0;
    }

    bool ensure(ID3D11Device* dev, std::size_t needVerts) {
        if (device != dev) {
            release();
            device = dev;
        }
        if (!EnsureShaderBlobs()) return false;

        if (!vs) {
            if (FAILED(dev->CreateVertexShader(g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize(), nullptr, &vs)))
                return false;
        }
        if (!ps) {
            if (FAILED(dev->CreatePixelShader(g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize(), nullptr, &ps)))
                return false;
        }
        if (!layout) {
            const D3D11_INPUT_ELEMENT_DESC elems[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            if (FAILED(dev->CreateInputLayout(elems, 2, g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize(), &layout)))
                return false;
        }
        if (!blend) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateBlendState(&bd, &blend))) return false;
        }
        if (!rast) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode        = D3D11_FILL_SOLID;
            rd.CullMode        = D3D11_CULL_NONE;
            rd.DepthClipEnable = FALSE;
            rd.ScissorEnable   = FALSE;
            if (FAILED(dev->CreateRasterizerState(&rd, &rast))) return false;
        }
        if (!depth) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable   = FALSE;
            dd.StencilEnable = FALSE;
            if (FAILED(dev->CreateDepthStencilState(&dd, &depth))) return false;
        }
        if (!vb || vbCapVerts < needVerts) {
            if (vb) { vb->Release(); vb = nullptr; }
            std::size_t newCap = (vbCapVerts == 0) ? 32768 : vbCapVerts * 2;
            while (newCap < needVerts) newCap *= 2;
            D3D11_BUFFER_DESC vbd{};
            vbd.ByteWidth      = static_cast<UINT>(newCap * sizeof(coll::OverlayVertex));
            vbd.Usage          = D3D11_USAGE_DYNAMIC;
            vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
            vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&vbd, nullptr, &vb))) return false;
            vbCapVerts = newCap;
        }
        if (!loggedInit) {
            loggedInit = true;
            app::Log("render: D3D11 overlay initialized (device=%p)", dev);
        }
        return true;
    }
} g_d3d11;

// ============================================================================
//  DirectX 12 Overlay State & Renderer
// ============================================================================
struct D3D12FrameSlot {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource* vb            = nullptr;
    void* mapped                  = nullptr;
    std::size_t capVerts          = 0;
    UINT64 fenceVal               = 0;

    void release() {
        if (vb) {
            if (mapped) { vb->Unmap(0, nullptr); mapped = nullptr; }
            vb->Release();
            vb = nullptr;
        }
        if (alloc) { alloc->Release(); alloc = nullptr; }
        capVerts = 0;
        fenceVal = 0;
    }
};

struct D3D12State {
    ID3D12Device* device               = nullptr;
    ID3D12RootSignature* rootSig       = nullptr;
    ID3D12PipelineState* pso           = nullptr;
    DXGI_FORMAT psoFormat              = DXGI_FORMAT_UNKNOWN;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Fence* fence                 = nullptr;
    HANDLE fenceEvent                  = nullptr;
    UINT64 nextFenceVal                = 0;
    std::uint32_t slotIndex            = 0;
    D3D12FrameSlot slots[4]{};
    bool loggedInit                    = false;

    void waitForIdle() {
        if (!fence || !fenceEvent) return;
        const UINT64 target = nextFenceVal;
        if (target > 0 && fence->GetCompletedValue() < target) {
            fence->SetEventOnCompletion(target, fenceEvent);
            ::WaitForSingleObject(fenceEvent, 2000);
        }
    }

    void release() {
        waitForIdle();
        for (auto& s : slots) s.release();
        if (cmdList)    { cmdList->Release(); cmdList = nullptr; }
        if (pso)        { pso->Release();     pso = nullptr; }
        if (rootSig)    { rootSig->Release(); rootSig = nullptr; }
        if (fence)      { fence->Release();   fence = nullptr; }
        if (fenceEvent) { ::CloseHandle(fenceEvent); fenceEvent = nullptr; }
        device = nullptr;
        psoFormat = DXGI_FORMAT_UNKNOWN;
        nextFenceVal = 0;
        slotIndex = 0;
    }

    bool ensureSlotVb(ID3D12Device* dev, D3D12FrameSlot& s, std::size_t needVerts) {
        if (s.vb && s.capVerts >= needVerts) return true;
        if (s.vb) {
            if (s.mapped) { s.vb->Unmap(0, nullptr); s.mapped = nullptr; }
            s.vb->Release();
            s.vb = nullptr;
        }
        std::size_t newCap = (s.capVerts == 0) ? 32768 : s.capVerts * 2;
        while (newCap < needVerts) newCap *= 2;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = static_cast<UINT64>(newCap * sizeof(coll::OverlayVertex));
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&s.vb)))) {
            return false;
        }
        D3D12_RANGE readRange{0, 0};
        if (FAILED(s.vb->Map(0, &readRange, &s.mapped))) {
            s.vb->Release();
            s.vb = nullptr;
            return false;
        }
        s.capVerts = newCap;
        return true;
    }

    bool ensure(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
        if (device != dev) {
            release();
            device = dev;
        }
        if (!EnsureShaderBlobs()) return false;

        if (!rootSig) {
            HMODULE d3d12Mod = ::GetModuleHandleW(L"d3d12.dll");
            if (!d3d12Mod) return false;
            auto fnSerialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                ::GetProcAddress(d3d12Mod, "D3D12SerializeRootSignature"));
            if (!fnSerialize) return false;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob* sigBlob = nullptr;
            ID3DBlob* errBlob = nullptr;
            if (FAILED(fnSerialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) {
                if (errBlob) errBlob->Release();
                return false;
            }
            if (errBlob) errBlob->Release();
            HRESULT hr = dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                                  IID_PPV_ARGS(&rootSig));
            sigBlob->Release();
            if (FAILED(hr)) return false;
        }

        if (!pso || psoFormat != rtvFormat) {
            if (pso) {
                waitForIdle();
                pso->Release();
                pso = nullptr;
            }
            const D3D12_INPUT_ELEMENT_DESC elems[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rootSig;
            pd.VS = {g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize()};
            pd.PS = {g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize()};
            pd.BlendState.RenderTarget[0].BlendEnable           = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = FALSE;
            pd.DepthStencilState.DepthEnable   = FALSE;
            pd.DepthStencilState.StencilEnable = FALSE;
            pd.InputLayout                     = {elems, 2};
            pd.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets                = 1;
            pd.RTVFormats[0]                   = rtvFormat;
            pd.SampleDesc.Count                = 1;

            HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
            if (FAILED(hr)) {
                app::Log("render: D3D12 CreateGraphicsPipelineState failed hr=0x%08lX (fmt=%d)", hr, static_cast<int>(rtvFormat));
                return false;
            }
            psoFormat = rtvFormat;
        }

        for (auto& s : slots) {
            if (!s.alloc) {
                if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc))))
                    return false;
            }
        }

        if (!cmdList) {
            if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].alloc, pso,
                                              IID_PPV_ARGS(&cmdList))))
                return false;
            cmdList->Close();
        }

        if (!fence) {
            if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;
        }
        if (!fenceEvent) {
            fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent) return false;
        }
        if (!loggedInit) {
            loggedInit = true;
            app::Log("render: D3D12 overlay initialized (device=%p, fmt=%d)", dev, static_cast<int>(rtvFormat));
        }
        return true;
    }
} g_d3d12;

std::vector<coll::OverlayVertex> g_verts;

void RenderD3D11(std::uintptr_t exeBase,
                 std::uintptr_t d3dRender,
                 bool visEnabled,
                 freeze::Mode freezeMode,
                 app::FilterMode filterMode,
                 const app::Config& cfg,
                 bool showStartupToast) {
    if (!app::Readable(d3dRender, 0x1680)) return;
    const std::uint32_t scCount = *reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DSwapChainCount);
    if (scCount == 0) return;

    auto* dev  = *reinterpret_cast<ID3D11Device* const*>(d3dRender + mhw::kOffD3DDevice11);
    auto* ctx  = *reinterpret_cast<ID3D11DeviceContext* const*>(d3dRender + mhw::kOffD3DContext11);
    auto* swap = *reinterpret_cast<IDXGISwapChain* const*>(d3dRender + mhw::kOffD3DSwapChain11);
    if (!dev || !ctx || !swap) return;

    auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(d3dRender + mhw::kOffD3DPresentLock);
    ::EnterCriticalSection(cs);

    ID3D11Texture2D* backBuf = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuf))) && backBuf) {
        D3D11_TEXTURE2D_DESC td{};
        backBuf->GetDesc(&td);

        const DXGI_FORMAT rtvFmt = ResolveTypedRtvFormat(td.Format);
        coll::FrameStats stats{};
        coll::BuildOverlayFrame(exeBase,
                                static_cast<float>(td.Width),
                                static_cast<float>(td.Height),
                                static_cast<std::uint32_t>(rtvFmt),
                                visEnabled, freezeMode, filterMode, cfg, showStartupToast, g_verts, stats);

        if (g_verts.size() >= 3 && g_d3d11.ensure(dev, g_verts.size())) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format        = rtvFmt;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(backBuf, &rtvDesc, &rtv)) && rtv) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(g_d3d11.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    std::memcpy(mapped.pData, g_verts.data(), g_verts.size() * sizeof(coll::OverlayVertex));
                    ctx->Unmap(g_d3d11.vb, 0);

                    // Backup D3D11 state
                    ID3D11RenderTargetView* oldRtv = nullptr;
                    ID3D11DepthStencilView* oldDsv = nullptr;
                    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);

                    ID3D11BlendState* oldBlend = nullptr;
                    FLOAT oldBlendFactor[4]{};
                    UINT oldSampleMask = 0;
                    ctx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

                    ID3D11DepthStencilState* oldDepth = nullptr;
                    UINT oldStencilRef = 0;
                    ctx->OMGetDepthStencilState(&oldDepth, &oldStencilRef);

                    ID3D11RasterizerState* oldRast = nullptr;
                    ctx->RSGetState(&oldRast);

                    UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                    D3D11_VIEWPORT oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                    ctx->RSGetViewports(&numVps, oldVps);

                    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
                    ctx->IAGetPrimitiveTopology(&oldTopo);

                    ID3D11InputLayout* oldLayout = nullptr;
                    ctx->IAGetInputLayout(&oldLayout);

                    ID3D11Buffer* oldVb = nullptr;
                    UINT oldStride = 0, oldOffset = 0;
                    ctx->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);

                    ID3D11VertexShader* oldVs = nullptr;
                    ID3D11PixelShader* oldPs  = nullptr;
                    ID3D11GeometryShader* oldGs = nullptr;
                    ctx->VSGetShader(&oldVs, nullptr, nullptr);
                    ctx->PSGetShader(&oldPs, nullptr, nullptr);
                    ctx->GSGetShader(&oldGs, nullptr, nullptr);

                    // Bind overlay pipeline & draw
                    D3D11_VIEWPORT vp{};
                    vp.Width    = static_cast<FLOAT>(td.Width);
                    vp.Height   = static_cast<FLOAT>(td.Height);
                    vp.MinDepth = 0.0f;
                    vp.MaxDepth = 1.0f;
                    ctx->RSSetViewports(1, &vp);
                    ctx->RSSetState(g_d3d11.rast);

                    const FLOAT bf[4] = {0, 0, 0, 0};
                    ctx->OMSetBlendState(g_d3d11.blend, bf, 0xFFFFFFFFU);
                    ctx->OMSetDepthStencilState(g_d3d11.depth, 0);
                    ctx->OMSetRenderTargets(1, &rtv, nullptr);

                    const UINT stride = sizeof(coll::OverlayVertex);
                    const UINT offset = 0;
                    ctx->IASetInputLayout(g_d3d11.layout);
                    ctx->IASetVertexBuffers(0, 1, &g_d3d11.vb, &stride, &offset);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                    ctx->VSSetShader(g_d3d11.vs, nullptr, 0);
                    ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                    ctx->GSSetShader(nullptr, nullptr, 0);

                    ctx->Draw(static_cast<UINT>(g_verts.size()), 0);

                    // Restore D3D11 state
                    ctx->VSSetShader(oldVs, nullptr, 0);
                    ctx->PSSetShader(oldPs, nullptr, 0);
                    ctx->GSSetShader(oldGs, nullptr, 0);
                    ctx->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
                    ctx->IASetInputLayout(oldLayout);
                    ctx->IASetPrimitiveTopology(oldTopo);
                    if (numVps > 0) ctx->RSSetViewports(numVps, oldVps);
                    ctx->RSSetState(oldRast);
                    ctx->OMSetDepthStencilState(oldDepth, oldStencilRef);
                    ctx->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
                    ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);

                    if (oldGs)     oldGs->Release();
                    if (oldPs)     oldPs->Release();
                    if (oldVs)     oldVs->Release();
                    if (oldVb)     oldVb->Release();
                    if (oldLayout) oldLayout->Release();
                    if (oldRast)   oldRast->Release();
                    if (oldDepth)  oldDepth->Release();
                    if (oldBlend)  oldBlend->Release();
                    if (oldDsv)    oldDsv->Release();
                    if (oldRtv)    oldRtv->Release();
                }
                rtv->Release();
            }
        }
        backBuf->Release();
    }

    ::LeaveCriticalSection(cs);
}

void RenderD3D12(std::uintptr_t exeBase,
                 std::uintptr_t d3dRender,
                 bool visEnabled,
                 freeze::Mode freezeMode,
                 app::FilterMode filterMode,
                 const app::Config& cfg,
                 bool showStartupToast) {
    if (!app::Readable(d3dRender, 0x1680)) return;
    if (*reinterpret_cast<const std::uint8_t*>(d3dRender + mhw::kOffD3DOccludedByte) != 0) return;
    const std::uint32_t scCount = *reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DSwapChainCount);
    if (scCount == 0) return;

    auto* dev     = *reinterpret_cast<ID3D12Device* const*>(d3dRender + mhw::kOffD3DDevice12);
    auto* queue   = *reinterpret_cast<ID3D12CommandQueue* const*>(d3dRender + mhw::kOffD3DCmdQueue12);
    auto* swap    = *reinterpret_cast<IDXGISwapChain3* const*>(d3dRender + mhw::kOffD3DSwapChain12);
    auto* rtvHeap = *reinterpret_cast<ID3D12DescriptorHeap* const*>(d3dRender + mhw::kOffD3DRtvHeap12);
    if (!dev || !queue || !swap || !rtvHeap) return;

    auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(d3dRender + mhw::kOffD3DPresentLock);
    ::EnterCriticalSection(cs);

    BOOL fsState = FALSE;
    swap->GetFullscreenState(&fsState, nullptr);
    const bool curFs  = (fsState != FALSE);
    const bool prevFs = (*reinterpret_cast<const std::uint8_t*>(d3dRender + mhw::kOffD3DFullscreenLatch) != 0);
    if (curFs != prevFs) {
        g_d3d12.waitForIdle();
        ::LeaveCriticalSection(cs);
        return;
    }

    const UINT bbIdx = swap->GetCurrentBackBufferIndex();
    if (bbIdx >= 3) {
        ::LeaveCriticalSection(cs);
        return;
    }

    auto* backBuf = *reinterpret_cast<ID3D12Resource* const*>(d3dRender + mhw::kOffD3DBackBuffers12 + bbIdx * 8);
    if (!backBuf) {
        ::LeaveCriticalSection(cs);
        return;
    }

    const D3D12_RESOURCE_DESC bbDesc = backBuf->GetDesc();
    const DXGI_FORMAT rtvFmt = ResolveTypedRtvFormat(bbDesc.Format);
    coll::FrameStats stats{};
    coll::BuildOverlayFrame(exeBase,
                            static_cast<float>(bbDesc.Width),
                            static_cast<float>(bbDesc.Height),
                            static_cast<std::uint32_t>(rtvFmt),
                            visEnabled, freezeMode, filterMode, cfg, showStartupToast, g_verts, stats);

    if (g_verts.size() >= 3 && g_d3d12.ensure(dev, rtvFmt)) {
        D3D12FrameSlot& slot = g_d3d12.slots[g_d3d12.slotIndex];
        g_d3d12.slotIndex = (g_d3d12.slotIndex + 1) & 3U;

        if (slot.fenceVal > 0 && g_d3d12.fence->GetCompletedValue() < slot.fenceVal) {
            g_d3d12.fence->SetEventOnCompletion(slot.fenceVal, g_d3d12.fenceEvent);
            ::WaitForSingleObject(g_d3d12.fenceEvent, 2000);
        }

        if (g_d3d12.ensureSlotVb(dev, slot, g_verts.size()) &&
            SUCCEEDED(slot.alloc->Reset()) &&
            SUCCEEDED(g_d3d12.cmdList->Reset(slot.alloc, g_d3d12.pso))) {

            const std::size_t byteLen = g_verts.size() * sizeof(coll::OverlayVertex);
            std::memcpy(slot.mapped, g_verts.data(), byteLen);

            D3D12_RESOURCE_BARRIER bIn{};
            bIn.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bIn.Transition.pResource   = backBuf;
            bIn.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bIn.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            bIn.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_d3d12.cmdList->ResourceBarrier(1, &bIn);

            const std::uint32_t rtvDescSize = *reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DRtvDescSize12);
            const SIZE_T rtvStart           = *reinterpret_cast<const SIZE_T*>(d3dRender + mhw::kOffD3DRtvStart12);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{rtvStart + static_cast<SIZE_T>(bbIdx) * rtvDescSize};

            g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width), static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
            D3D12_RECT sc{0, 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height)};
            g_d3d12.cmdList->RSSetViewports(1, &vp);
            g_d3d12.cmdList->RSSetScissorRects(1, &sc);
            g_d3d12.cmdList->SetGraphicsRootSignature(g_d3d12.rootSig);
            g_d3d12.cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = slot.vb->GetGPUVirtualAddress();
            vbv.SizeInBytes    = static_cast<UINT>(byteLen);
            vbv.StrideInBytes  = sizeof(coll::OverlayVertex);
            g_d3d12.cmdList->IASetVertexBuffers(0, 1, &vbv);
            g_d3d12.cmdList->DrawInstanced(static_cast<UINT>(g_verts.size()), 1, 0, 0);

            D3D12_RESOURCE_BARRIER bOut{};
            bOut.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bOut.Transition.pResource   = backBuf;
            bOut.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bOut.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            bOut.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            g_d3d12.cmdList->ResourceBarrier(1, &bOut);

            if (SUCCEEDED(g_d3d12.cmdList->Close())) {
                ID3D12CommandList* lists[1] = {g_d3d12.cmdList};
                queue->ExecuteCommandLists(1, lists);
                const UINT64 fv = ++g_d3d12.nextFenceVal;
                queue->Signal(g_d3d12.fence, fv);
                slot.fenceVal = fv;
            }
        }
    }

    ::LeaveCriticalSection(cs);
}

} // namespace

void OnPresent(std::uintptr_t exeBase,
               std::uintptr_t d3dRender,
               bool visEnabled,
               freeze::Mode freezeMode,
               app::FilterMode filterMode,
               const app::Config& cfg,
               bool showStartupToast) {
    if (!exeBase || !d3dRender) return;
    if (!visEnabled && freezeMode == freeze::Mode::None && !showStartupToast) return;

    const bool isDx12 = (*reinterpret_cast<const std::uint8_t*>(exeBase + mhw::kRvaIsD3D12Byte) != 0);
    if (isDx12) {
        RenderD3D12(exeBase, d3dRender, visEnabled, freezeMode, filterMode, cfg, showStartupToast);
    } else {
        RenderD3D11(exeBase, d3dRender, visEnabled, freezeMode, filterMode, cfg, showStartupToast);
    }
}

HWND GetGameWindow(std::uintptr_t d3dRender) {
    if (!d3dRender || !app::Readable(d3dRender + mhw::kOffD3DSwapHwnd, sizeof(HWND))) return nullptr;
    return *reinterpret_cast<HWND const*>(d3dRender + mhw::kOffD3DSwapHwnd);
}

void Shutdown() {
    g_d3d12.release();
    g_d3d11.release();
    if (g_psBlob) { g_psBlob->Release(); g_psBlob = nullptr; }
    if (g_vsBlob) { g_vsBlob->Release(); g_vsBlob = nullptr; }
}

} // namespace render

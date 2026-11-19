#include "overlay_renderer.h"
#include "collision_collector.h"
#include "mhw_addresses.h"

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace render {
namespace {

constexpr const char* kOverlayHlsl = R"(
cbuffer OverlayCB : register(b0) {
    float2 gInvScreen;
    float2 gDepthActiveSize;
    float2 gPostUiUvScale;
    float2 gPreUiUvScale;
    float  gProjM32;
    float  gProjM22;
    float  gDepthBiasCm;
    uint   gFlags;
};

Texture2D<float>  tSceneDepth  : register(t0);
Texture2D<float4> tUiBackup    : register(t1);
Texture2D<float4> tPostUi      : register(t2);
Texture2D<float4> tPreUi0      : register(t3);
Texture2D<float4> tPreUi1      : register(t4);
SamplerState      sLinearClamp : register(s0);

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

PSIn VSFullscreen(uint vid : SV_VertexID) {
    PSIn o;
    float2 uv = float2((vid << 1) & 2u, vid & 2u);
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    o.col = float4(1.0f, 1.0f, 1.0f, 1.0f);
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET {
    return i.col;
}

float4 PSDepthClip(PSIn i) : SV_TARGET {
    if ((gFlags & 1u) != 0u) {
        float2 uv = i.pos.xy * gInvScreen;
        int2 maxCoord = max(int2(gDepthActiveSize) - int2(1, 1), int2(0, 0));
        int2 dp = clamp(int2(uv * gDepthActiveSize), int2(0, 0), maxCoord);
        float sd = tSceneDepth.Load(int3(dp, 0));
        if (sd > 1e-6f) {
            float dSceneCm = gProjM32 / max(sd + gProjM22, 1e-7f);
            float dSbcCm   = gProjM32 / max(i.pos.z + gProjM22, 1e-7f);
            float tolCm    = max(gDepthBiasCm, 0.012f * dSbcCm);
            if (dSceneCm + tolCm <= dSbcCm) {
                discard;
            }
        }
    }
    return i.col;
}

float rgbaRelMaxDiff(float4 a, float4 b) {
    float3 denomRgb = max(float3(1.0f, 1.0f, 1.0f), max(abs(a.rgb), abs(b.rgb)));
    float3 dRgb     = abs(a.rgb - b.rgb) / denomRgb;
    float  dA       = abs(saturate(a.a) - saturate(b.a));
    return max(max(dRgb.r, dRgb.g), max(dRgb.b, dA));
}

float uiDiffAt(int2 c, int2 maxPx) {
    int2 cc = clamp(c, int2(0, 0), maxPx);
    float4 postV;
    float4 pre0V;
    if ((gFlags & 4u) != 0u) {
        postV = tPostUi.Load(int3(cc, 0));
        pre0V = tPreUi0.Load(int3(cc, 0));
    } else {
        float2 uv = saturate((float2(cc) + 0.5f) * gInvScreen);
        postV = tPostUi.SampleLevel(sLinearClamp, uv * gPostUiUvScale, 0.0f);
        pre0V = tPreUi0.SampleLevel(sLinearClamp, uv * gPreUiUvScale, 0.0f);
    }
    float d = rgbaRelMaxDiff(postV, pre0V);
    if ((gFlags & 2u) != 0u) {
        float4 pre1V;
        if ((gFlags & 4u) != 0u) {
            pre1V = tPreUi1.Load(int3(cc, 0));
        } else {
            float2 uv = saturate((float2(cc) + 0.5f) * gInvScreen);
            pre1V = tPreUi1.SampleLevel(sLinearClamp, uv * gPreUiUvScale, 0.0f);
        }
        d = min(d, rgbaRelMaxDiff(postV, pre1V));
    }
    return d;
}

float4 PSRestoreUI(PSIn i) : SV_TARGET {
    int2 px = int2(i.pos.xy);
    int2 maxPx = max(int2(round(1.0f / max(gInvScreen, float2(1e-6f, 1e-6f)))) - int2(1, 1), int2(0, 0));
    float lo = ((gFlags & 4u) != 0u) ? 0.0065f : 0.014f;
    float hi = ((gFlags & 4u) != 0u) ? 0.0280f : 0.042f;

    float dC = uiDiffAt(px, maxPx);
    if (dC < hi) {
        // Bilateral opposing-pair bridge: fills 1-3px accidental color-match pinholes inside solid UI
        // while adding 0.0px outward halo on straight UI edges and text corners!
        float dL = uiDiffAt(px + int2(-2,  0), maxPx);
        float dR = uiDiffAt(px + int2( 2,  0), maxPx);
        float dU = uiDiffAt(px + int2( 0, -2), maxPx);
        float dD = uiDiffAt(px + int2( 0,  2), maxPx);
        dC = max(dC, max(min(dL, dR), min(dU, dD)));
    }

    float uiA = smoothstep(lo, hi, dC);
    if (uiA <= (1.0f / 255.0f)) {
        discard;
    }
    float3 origRgb = tUiBackup.Load(int3(px, 0)).rgb;
    return float4(origRgb, uiA);
}
)";

struct OverlayConstants {
    float         invScreenX;
    float         invScreenY;
    float         depthActiveW;
    float         depthActiveH;
    float         postUiUvScaleX;
    float         postUiUvScaleY;
    float         preUiUvScaleX;
    float         preUiUvScaleY;
    float         projM32;
    float         projM22;
    float         depthBiasCm;
    std::uint32_t flags;
};
static_assert(sizeof(OverlayConstants) == 48, "OverlayConstants must be 48 bytes (12 DWORDs)");

ID3DBlob* g_vsBlob           = nullptr;
ID3DBlob* g_vsFullscreenBlob = nullptr;
ID3DBlob* g_psBlob           = nullptr;
ID3DBlob* g_psDepthClipBlob  = nullptr;
ID3DBlob* g_psRestoreUiBlob  = nullptr;
bool g_shaderTried           = false;

bool CompileEntry(pD3DCompile fnCompile, const char* entry, const char* target, ID3DBlob** outBlob) {
    ID3DBlob* err = nullptr;
    HRESULT hr = fnCompile(kOverlayHlsl, std::strlen(kOverlayHlsl), "overlay.hlsl",
                           nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, outBlob, &err);
    if (FAILED(hr)) {
        app::Log("shader: %s (%s) compile failed hr=0x%08lX (%s)", entry, target, hr,
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

bool EnsureShaderBlobs() {
    if (g_vsBlob && g_vsFullscreenBlob && g_psBlob && g_psDepthClipBlob && g_psRestoreUiBlob) {
        return true;
    }
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

    if (!CompileEntry(fnCompile, "VSMain",       "vs_4_0", &g_vsBlob) ||
        !CompileEntry(fnCompile, "VSFullscreen", "vs_4_0", &g_vsFullscreenBlob) ||
        !CompileEntry(fnCompile, "PSMain",       "ps_4_0", &g_psBlob) ||
        !CompileEntry(fnCompile, "PSDepthClip",  "ps_4_0", &g_psDepthClipBlob) ||
        !CompileEntry(fnCompile, "PSRestoreUI",  "ps_4_0", &g_psRestoreUiBlob)) {
        return false;
    }

    app::Log("shader: compiled VS/VSFullscreen/PS/PSDepthClip/PSRestoreUI blobs successfully");
    return true;
}

DXGI_FORMAT ResolveTypedRtvFormat(DXGI_FORMAT fmt) noexcept {
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

DXGI_FORMAT ResolveTypelessFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:                                return fmt;
    }
}

DXGI_FORMAT ResolveColorSrvFormat(DXGI_FORMAT resFmt, DXGI_FORMAT preferredTypedFmt) noexcept {
    switch (resFmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return ResolveTypedRtvFormat(preferredTypedFmt != DXGI_FORMAT_UNKNOWN ? preferredTypedFmt : resFmt);
        default:
            return resFmt; // Typed resource must use its own typed format
    }
}

DXGI_FORMAT ResolveDepthSrvFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:                  return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:   return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:                  return DXGI_FORMAT_R16_UNORM;
        default:                                     return ResolveTypedRtvFormat(fmt);
    }
}

// ============================================================================
//  MHW Engine Scene Depth (tDepthMap) & 2D Player UI (uiTex) Resolver
// ============================================================================
inline bool IsAlignedPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && ((p & 0x7ULL) == 0);
}

// Exact MT Framework D3D12 resource state decoder (verified @ 0x1423A2B50)
bool DecodeMhD3D12State(std::uint16_t u, D3D12_RESOURCE_STATES& outState) noexcept {
    if (u < 0xFFu) {
        const std::uint32_t s = u;
        outState = static_cast<D3D12_RESOURCE_STATES>(
            ((((s & 0x20u) << 1) | (s & 0x10u)) << 1 | (s & 0xCu)) << 4 |
            ((s >> 1) & 0x20u) | (s & 0x3u));
        return true;
    }
    if (u == 0xFFu)  { outState = D3D12_RESOURCE_STATE_GENERIC_READ;     return true; }
    if (u == 0x100u) { outState = D3D12_RESOURCE_STATE_RENDER_TARGET;    return true; }
    if (u == 0x200u) { outState = D3D12_RESOURCE_STATE_DEPTH_WRITE;      return true; }
    if (u == 0x400u) { outState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; return true; }
    if (u == 0x800u) { outState = D3D12_RESOURCE_STATE_COPY_DEST;        return true; }
    return false;
}

struct EngineSceneTextures {
    bool                        hasDepth        = false;
    float                       depthActiveW    = 1.0f;
    float                       depthActiveH    = 1.0f;
    ID3D11ShaderResourceView*   depthSrv11      = nullptr;
    ID3D12Resource*             depthRes12      = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu12{0};
    D3D12_RESOURCE_STATES       depthState12    = D3D12_RESOURCE_STATE_COMMON;

    bool                        hasUi           = false;
    bool                        dualPreUi       = false;
    std::uint8_t                uiSourceKind    = 0; // 1=TAA, 2=Upscale/FXAA, 3=RTPostTarget
    float                       postUiActW      = 1.0f;
    float                       postUiActH      = 1.0f;
    float                       postUiUvScaleX  = 1.0f;
    float                       postUiUvScaleY  = 1.0f;
    float                       preUiActW       = 1.0f;
    float                       preUiActH       = 1.0f;
    float                       preUiUvScaleX   = 1.0f;
    float                       preUiUvScaleY   = 1.0f;

    ID3D11ShaderResourceView*   postUiSrv11     = nullptr;
    ID3D12Resource*             postUiRes12     = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE postUiSrvCpu12{0};
    D3D12_RESOURCE_STATES       postUiState12   = D3D12_RESOURCE_STATE_COMMON;

    ID3D11ShaderResourceView*   preUi0Srv11     = nullptr;
    ID3D12Resource*             preUi0Res12     = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE preUi0SrvCpu12{0};
    D3D12_RESOURCE_STATES       preUi0State12   = D3D12_RESOURCE_STATE_COMMON;

    ID3D11ShaderResourceView*   preUi1Srv11     = nullptr;
    ID3D12Resource*             preUi1Res12     = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE preUi1SrvCpu12{0};
    D3D12_RESOURCE_STATES       preUi1State12   = D3D12_RESOURCE_STATE_COMMON;
};

bool ExtractNDrawTexture(std::uintptr_t tex,
                         bool isDx12,
                         float dynScale,
                         float& outTexW,
                         float& outTexH,
                         float& outActW,
                         float& outActH,
                         ID3D11ShaderResourceView*& outSrv11,
                         ID3D12Resource*& outRes12,
                         D3D12_CPU_DESCRIPTOR_HANDLE& outSrvCpu12,
                         D3D12_RESOURCE_STATES& outState12) noexcept {
    if (!IsAlignedPtr(tex)) return false;

    const std::uint32_t packed = *reinterpret_cast<const std::uint32_t*>(tex + mhw::kOffTexPackedDim);
    const std::uint32_t texW   = packed & 0xFFFFu;
    const std::uint32_t texH   = (packed >> 16) & 0x3FFFu;
    if (texW < 64u || texW > 16384u || texH < 64u || texH > 16384u) return false;

    const std::int32_t texMode = *reinterpret_cast<const std::int32_t*>(tex + mhw::kOffTexMode);
    std::uintptr_t texWrap = (texMode == 6)
                                 ? *reinterpret_cast<const std::uintptr_t*>(tex + mhw::kOffTexWrapAlt)
                                 : *reinterpret_cast<const std::uintptr_t*>(tex + mhw::kOffTexWrap);
    if (!IsAlignedPtr(texWrap)) {
        texWrap = *reinterpret_cast<const std::uintptr_t*>(tex + mhw::kOffTexWrap);
    }
    if (!IsAlignedPtr(texWrap)) return false;

    const std::uintptr_t srvWrap = *reinterpret_cast<const std::uintptr_t*>(tex + mhw::kOffTexSrvWrap);

    float s = dynScale;
    if (!(s >= 0.2f && s <= 1.0f)) s = 1.0f;
    outTexW = static_cast<float>(texW);
    outTexH = static_cast<float>(texH);
    outActW = std::floor(outTexW * s + 0.5f);
    outActH = std::floor(outTexH * s + 0.5f);
    if (outActW < 1.0f) outActW = 1.0f;
    if (outActH < 1.0f) outActH = 1.0f;

    if (!isDx12) {
        if (!IsAlignedPtr(srvWrap)) return false;
        auto* res11 = *reinterpret_cast<ID3D11Resource* const*>(texWrap + mhw::kOffWrapRes11);
        auto* srv11 = *reinterpret_cast<ID3D11ShaderResourceView* const*>(srvWrap + mhw::kOffSrvWrapHandle);
        if (!IsAlignedPtr(reinterpret_cast<std::uintptr_t>(res11)) ||
            !IsAlignedPtr(reinterpret_cast<std::uintptr_t>(srv11))) {
            return false;
        }
        outSrv11 = srv11;
        return true;
    }

    auto* res12 = *reinterpret_cast<ID3D12Resource* const*>(texWrap + mhw::kOffWrapRes12);
    if (!IsAlignedPtr(reinterpret_cast<std::uintptr_t>(res12))) return false;

    const std::uintptr_t stateObj = *reinterpret_cast<const std::uintptr_t*>(texWrap + mhw::kOffWrapState12);
    if (IsAlignedPtr(stateObj)) {
        const std::uint16_t mhState = *reinterpret_cast<const std::uint16_t*>(stateObj + mhw::kOffState12Bits);
        if (!DecodeMhD3D12State(mhState, outState12)) return false;
    } else if (texMode >= 1 && texMode <= 3) {
        outState12 = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else {
        return false;
    }

    outSrvCpu12.ptr = 0;
    if (IsAlignedPtr(srvWrap)) {
        outSrvCpu12 = *reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(srvWrap + mhw::kOffSrvWrapHandle);
    }
    outRes12 = res12;
    return true;
}

void ResolveEngineSceneTexturesSEH(std::uintptr_t exeBase, bool isDx12, EngineSceneTextures* out) noexcept {
    *out = EngineSceneTextures{};
    if (!exeBase) return;

    __try {
        // Verified MHW 15.23 render pipeline (sCamera::draw @ 0x14228e770, cDraw::beginScene @ 0x1423c3790,
        // nGUI::Draw::ctor @ 0x1425a3c90 -> 0x14234ee00, System_CopyToBackBuffer @ 0x1422979f0):
        //   - Viewport 0 renders BOTH 3D Scene (DrawPass 1..0x0F) AND 2D Player UI uGUI (DrawPass 0x10..0x11)
        //     into sMhRender->mainScene (+0x1F450); Viewports 1..7 have mVisible == 0 during gameplay.
        //   - At DrawPass 0x10 start, MHW copies the finished post-tonemap/TAA/FXAA/DLSS 3D texture (puVar21)
        //     into RTScreen (*(mainScene+0x460/+0x4A0)), and uGUI then draws all 2D Player UI directly onto RTScreen!
        const std::uintptr_t mhRender = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhRenderInstance);
        if (!IsAlignedPtr(mhRender)) return;

        const std::uintptr_t mainScene = *reinterpret_cast<const std::uintptr_t*>(
            mhRender + mhw::kOffRenderSceneMain);
        if (!IsAlignedPtr(mainScene)) return;

        const std::uint32_t sceneFlags = *reinterpret_cast<const std::uint32_t*>(
            mainScene + mhw::kOffSceneFlags);
        float dynScale = *reinterpret_cast<const float*>(mainScene + mhw::kOffSceneDynamicScale);
        if (!std::isfinite(dynScale) || dynScale < 0.2f || dynScale > 1.0f) {
            dynScale = 1.0f;
        }

        const std::uintptr_t targets = *reinterpret_cast<const std::uintptr_t*>(
            mainScene + mhw::kOffSceneTargets);

        // 1. Main 3D Scene Depth Texture (tDepthMap #1 @ targets+0xA0, fallback #2 @ +0xA8, fallback RTDepth @ +0x90)
        if (IsAlignedPtr(targets)) {
            const std::uintptr_t depthCands[3] = {
                *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsDepthCopy),
                *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsDepthCopy2),
                *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsRtDepth)
            };
            for (std::uintptr_t dTex : depthCands) {
                if (!IsAlignedPtr(dTex)) continue;
                float dTexW = 1.0f, dTexH = 1.0f;
                if (ExtractNDrawTexture(dTex, isDx12, dynScale,
                                        dTexW, dTexH, out->depthActiveW, out->depthActiveH,
                                        out->depthSrv11, out->depthRes12, out->depthSrvCpu12, out->depthState12)) {
                    out->hasDepth = true;
                    break;
                }
            }
        }

        // 2. Post-UI Screen Texture (RTScreen @ *(*(mainScene+0x4A0)+0x58), read by System_CopyToBackBuffer)
        std::uintptr_t postUiTex = 0;
        const std::uintptr_t rtvFilter = *reinterpret_cast<const std::uintptr_t*>(
            mainScene + mhw::kOffSceneFilterTarget);
        if (IsAlignedPtr(rtvFilter)) {
            postUiTex = *reinterpret_cast<const std::uintptr_t*>(rtvFilter + mhw::kOffRenderTargetTex);
        }
        if (!IsAlignedPtr(postUiTex)) {
            const std::uintptr_t rtvOut = *reinterpret_cast<const std::uintptr_t*>(
                mainScene + mhw::kOffSceneOutputTarget);
            if (IsAlignedPtr(rtvOut)) {
                postUiTex = *reinterpret_cast<const std::uintptr_t*>(rtvOut + mhw::kOffRenderTargetTex);
            }
        }
        if (!IsAlignedPtr(postUiTex) && IsAlignedPtr(targets)) {
            postUiTex = *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsScreenTex);
        }
        if (!IsAlignedPtr(postUiTex)) return;

        float postTexW = 1.0f, postTexH = 1.0f;
        if (!ExtractNDrawTexture(postUiTex, isDx12, 1.0f,
                                 postTexW, postTexH, out->postUiActW, out->postUiActH,
                                 out->postUiSrv11, out->postUiRes12, out->postUiSrvCpu12, out->postUiState12)) {
            return;
        }
        out->postUiUvScaleX = (postTexW > 0.0f) ? (out->postUiActW / postTexW) : 1.0f;
        out->postUiUvScaleY = (postTexH > 0.0f) ? (out->postUiActH / postTexH) : 1.0f;

        // 3. Exact Pre-UI 3D Texture (puVar21 @ 0x1423c3790 lines 2724..3209, copied into RTScreen right before uGUI!)
        if ((sceneFlags & 0x800000u) != 0u && IsAlignedPtr(targets)) {
            // Branch A: TAA Temporal Upsample active -> rwOutput[0..1] @ targets + 0x138 / +0x140 (full-screen 1:1)
            const std::uintptr_t rw0 = *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsTaaOut0);
            const std::uintptr_t rw1 = *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsTaaOut1);

            float t0W = 1.0f, t0H = 1.0f, a0W = 1.0f, a0H = 1.0f;
            ID3D11ShaderResourceView* s0_11 = nullptr;
            ID3D12Resource* r0_12           = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE c0_12{0};
            D3D12_RESOURCE_STATES st0_12    = D3D12_RESOURCE_STATE_COMMON;
            const bool ok0 = (rw0 != postUiTex) &&
                             ExtractNDrawTexture(rw0, isDx12, 1.0f, t0W, t0H, a0W, a0H, s0_11, r0_12, c0_12, st0_12);

            float t1W = 1.0f, t1H = 1.0f, a1W = 1.0f, a1H = 1.0f;
            ID3D11ShaderResourceView* s1_11 = nullptr;
            ID3D12Resource* r1_12           = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE c1_12{0};
            D3D12_RESOURCE_STATES st1_12    = D3D12_RESOURCE_STATE_COMMON;
            const bool ok1 = (rw1 != postUiTex) && (rw1 != rw0) &&
                             ExtractNDrawTexture(rw1, isDx12, 1.0f, t1W, t1H, a1W, a1H, s1_11, r1_12, c1_12, st1_12);

            // Always bind BOTH rwOutput[0] & rwOutput[1] when valid (dualPreUi = true)!
            // MHW encodes GENERIC_READ (0xFF -> 0xAC3) on tHistory as well as COPY_SOURCE/PSR, so single-slot
            // state heuristics can pick last frame's tHistory and cause PSRestoreUI to erase Opaque .sbc during
            // camera motion. With dualPreUi, min(diff(post, rw0), diff(post, rw1)) is identically 0.0 on all non-UI!
            if (ok0 && ok1) {
                out->hasUi          = true;
                out->dualPreUi      = true;
                out->uiSourceKind   = 1;
                out->preUiActW      = a0W;
                out->preUiActH      = a0H;
                out->preUiUvScaleX  = (t0W > 0.0f) ? (a0W / t0W) : 1.0f;
                out->preUiUvScaleY  = (t0H > 0.0f) ? (a0H / t0H) : 1.0f;
                out->preUi0Srv11    = s0_11;
                out->preUi0Res12    = r0_12;
                out->preUi0SrvCpu12 = c0_12;
                out->preUi0State12  = st0_12;
                out->preUi1Srv11    = s1_11;
                out->preUi1Res12    = r1_12;
                out->preUi1SrvCpu12 = c1_12;
                out->preUi1State12  = st1_12;
                return;
            }
            if (ok0) {
                out->hasUi          = true;
                out->dualPreUi      = false;
                out->uiSourceKind   = 1;
                out->preUiActW      = a0W;
                out->preUiActH      = a0H;
                out->preUiUvScaleX  = (t0W > 0.0f) ? (a0W / t0W) : 1.0f;
                out->preUiUvScaleY  = (t0H > 0.0f) ? (a0H / t0H) : 1.0f;
                out->preUi0Srv11    = out->preUi1Srv11    = s0_11;
                out->preUi0Res12    = out->preUi1Res12    = r0_12;
                out->preUi0SrvCpu12 = out->preUi1SrvCpu12 = c0_12;
                out->preUi0State12  = out->preUi1State12  = st0_12;
                return;
            }
            if (ok1) {
                out->hasUi          = true;
                out->dualPreUi      = false;
                out->uiSourceKind   = 1;
                out->preUiActW      = a1W;
                out->preUiActH      = a1H;
                out->preUiUvScaleX  = (t1W > 0.0f) ? (a1W / t1W) : 1.0f;
                out->preUiUvScaleY  = (t1H > 0.0f) ? (a1H / t1H) : 1.0f;
                out->preUi0Srv11    = out->preUi1Srv11    = s1_11;
                out->preUi0Res12    = out->preUi1Res12    = r1_12;
                out->preUi0SrvCpu12 = out->preUi1SrvCpu12 = c1_12;
                out->preUi0State12  = out->preUi1State12  = st1_12;
                return;
            }
        }

        // Branch B: TAA OFF -> check DLSS/CAS UpscaleOut (+0x6A0), FXAA (+0x6A0), then RTPostTarget (+0x420->+0x58)
        std::uintptr_t preUiTex   = 0;
        float          preUiScale = dynScale;
        std::uint8_t   srcKind    = 3;

        const std::uintptr_t rtvPost = *reinterpret_cast<const std::uintptr_t*>(
            mainScene + mhw::kOffScenePostTarget);
        if (IsAlignedPtr(rtvPost)) {
            preUiTex = *reinterpret_cast<const std::uintptr_t*>(rtvPost + mhw::kOffRenderTargetTex);
        }
        if (!IsAlignedPtr(preUiTex) && IsAlignedPtr(targets)) {
            preUiTex = *reinterpret_cast<const std::uintptr_t*>(targets + mhw::kOffTargetsPostTex);
        }

        const bool sceneAaOn = (*reinterpret_cast<const std::uint8_t*>(mainScene + mhw::kOffSceneAaEnabled) != 0);
        const std::uintptr_t upscaleTex = *reinterpret_cast<const std::uintptr_t*>(
            mainScene + mhw::kOffSceneUpscaleTex);
        if (sceneAaOn && IsAlignedPtr(upscaleTex)) {
            const std::uintptr_t d3d78 = *reinterpret_cast<const std::uintptr_t*>(
                mhRender + mhw::kOffRenderD3DRender);
            const bool dlssBranch =
                (*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderDx12UpscaleFlag) != 0) &&
                ((*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderUpscaleBits) & 2u) != 0) &&
                IsAlignedPtr(d3d78) &&
                (*reinterpret_cast<const std::uint8_t*>(d3d78 + mhw::kOffD3DUpscaleActive) != 0);
            if (dlssBranch) {
                const float upRatio = *reinterpret_cast<const float*>(mhRender + mhw::kOffRenderUpscaleRatio);
                if (upRatio != 1.0f &&
                    *reinterpret_cast<const std::uintptr_t*>(d3d78 + mhw::kOffD3DUpscaleCtx) != 0) {
                    preUiTex   = upscaleTex;
                    preUiScale = 1.0f;
                    srcKind    = 2;
                }
            } else if (*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderFxaaEnabled) != 0) {
                preUiTex   = upscaleTex;
                preUiScale = 1.0f;
                srcKind    = 2;
            }
        }

        if (IsAlignedPtr(preUiTex) && preUiTex != postUiTex) {
            float preTexW = 1.0f, preTexH = 1.0f;
            if (ExtractNDrawTexture(preUiTex, isDx12, preUiScale,
                                    preTexW, preTexH, out->preUiActW, out->preUiActH,
                                    out->preUi0Srv11, out->preUi0Res12, out->preUi0SrvCpu12, out->preUi0State12)) {
                out->hasUi          = true;
                out->dualPreUi      = false;
                out->uiSourceKind   = srcKind;
                out->preUiUvScaleX  = (preTexW > 0.0f) ? (out->preUiActW / preTexW) : 1.0f;
                out->preUiUvScaleY  = (preTexH > 0.0f) ? (out->preUiActH / preTexH) : 1.0f;
                out->preUi1Srv11    = out->preUi0Srv11;
                out->preUi1Res12    = out->preUi0Res12;
                out->preUi1SrvCpu12 = out->preUi0SrvCpu12;
                out->preUi1State12  = out->preUi0State12;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = EngineSceneTextures{};
    }
}

// ============================================================================
//  DirectX 11 Overlay State & Renderer
// ============================================================================
struct D3D11State {
    ID3D11Device* device                 = nullptr;
    ID3D11VertexShader* vs               = nullptr;
    ID3D11VertexShader* vsFullscreen     = nullptr;
    ID3D11PixelShader* ps                = nullptr;
    ID3D11PixelShader* psDepthClip       = nullptr;
    ID3D11PixelShader* psRestoreUi       = nullptr;
    ID3D11InputLayout* layout            = nullptr;
    ID3D11BlendState* blend              = nullptr;
    ID3D11BlendState* blendOpaque        = nullptr;
    ID3D11RasterizerState* rast          = nullptr;
    ID3D11DepthStencilState* depth       = nullptr;
    ID3D11DepthStencilState* depthWrite  = nullptr;
    ID3D11DepthStencilState* depthRead   = nullptr;
    ID3D11SamplerState* linearSampler    = nullptr;
    ID3D11Buffer* cb                     = nullptr;
    ID3D11Texture2D* dsvTex              = nullptr;
    ID3D11DepthStencilView* dsv          = nullptr;
    UINT dsvWidth                        = 0;
    UINT dsvHeight                       = 0;
    ID3D11Texture2D* uiBackupTex         = nullptr;
    ID3D11ShaderResourceView* uiBackupSrv = nullptr;
    UINT backupWidth                     = 0;
    UINT backupHeight                    = 0;
    DXGI_FORMAT backupFormat             = DXGI_FORMAT_UNKNOWN;
    ID3D11Buffer* vb                     = nullptr;
    std::size_t vbCapVerts               = 0;
    bool loggedInit                      = false;

    void release() {
        if (uiBackupSrv)   { uiBackupSrv->Release();   uiBackupSrv = nullptr; }
        if (uiBackupTex)   { uiBackupTex->Release();   uiBackupTex = nullptr; }
        if (dsv)           { dsv->Release();           dsv = nullptr; }
        if (dsvTex)        { dsvTex->Release();        dsvTex = nullptr; }
        if (vb)            { vb->Release();            vb = nullptr; }
        if (cb)            { cb->Release();            cb = nullptr; }
        if (linearSampler) { linearSampler->Release(); linearSampler = nullptr; }
        if (depthRead)     { depthRead->Release();     depthRead = nullptr; }
        if (depthWrite)    { depthWrite->Release();    depthWrite = nullptr; }
        if (depth)         { depth->Release();         depth = nullptr; }
        if (rast)          { rast->Release();          rast = nullptr; }
        if (blendOpaque)   { blendOpaque->Release();   blendOpaque = nullptr; }
        if (blend)         { blend->Release();         blend = nullptr; }
        if (layout)        { layout->Release();        layout = nullptr; }
        if (psRestoreUi)   { psRestoreUi->Release();   psRestoreUi = nullptr; }
        if (psDepthClip)   { psDepthClip->Release();   psDepthClip = nullptr; }
        if (ps)            { ps->Release();            ps = nullptr; }
        if (vsFullscreen)  { vsFullscreen->Release();  vsFullscreen = nullptr; }
        if (vs)            { vs->Release();            vs = nullptr; }
        device = nullptr;
        dsvWidth = 0;
        dsvHeight = 0;
        backupWidth = 0;
        backupHeight = 0;
        backupFormat = DXGI_FORMAT_UNKNOWN;
        vbCapVerts = 0;
    }

    bool ensureSceneResources(ID3D11Device* dev, UINT w, UINT h, bool needDsv) {
        if (w == 0 || h == 0) return false;
        if (!vsFullscreen) {
            if (FAILED(dev->CreateVertexShader(g_vsFullscreenBlob->GetBufferPointer(),
                                               g_vsFullscreenBlob->GetBufferSize(), nullptr, &vsFullscreen)))
                return false;
        }
        if (!psDepthClip) {
            if (FAILED(dev->CreatePixelShader(g_psDepthClipBlob->GetBufferPointer(),
                                              g_psDepthClipBlob->GetBufferSize(), nullptr, &psDepthClip)))
                return false;
        }
        if (!psRestoreUi) {
            if (FAILED(dev->CreatePixelShader(g_psRestoreUiBlob->GetBufferPointer(),
                                              g_psRestoreUiBlob->GetBufferSize(), nullptr, &psRestoreUi)))
                return false;
        }
        if (!linearSampler) {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MinLOD         = 0.0f;
            sd.MaxLOD         = D3D11_FLOAT32_MAX;
            if (FAILED(dev->CreateSamplerState(&sd, &linearSampler))) return false;
        }
        if (!cb) {
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth      = sizeof(OverlayConstants);
            cbd.Usage          = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cb))) return false;
        }
        if (!needDsv) {
            return true;
        }
        if (!blendOpaque) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable           = FALSE;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateBlendState(&bd, &blendOpaque))) return false;
        }
        if (!depthWrite) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable    = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthFunc      = D3D11_COMPARISON_GREATER_EQUAL;
            dd.StencilEnable  = FALSE;
            if (FAILED(dev->CreateDepthStencilState(&dd, &depthWrite))) return false;
        }
        if (!depthRead) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable    = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc      = D3D11_COMPARISON_GREATER_EQUAL;
            dd.StencilEnable  = FALSE;
            if (FAILED(dev->CreateDepthStencilState(&dd, &depthRead))) return false;
        }
        if (!dsv || !dsvTex || dsvWidth != w || dsvHeight != h) {
            if (dsv)    { dsv->Release();    dsv = nullptr; }
            if (dsvTex) { dsvTex->Release(); dsvTex = nullptr; }

            D3D11_TEXTURE2D_DESC td{};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &dsvTex))) return false;

            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            vd.Format        = DXGI_FORMAT_D32_FLOAT;
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            if (FAILED(dev->CreateDepthStencilView(dsvTex, &vd, &dsv))) {
                dsvTex->Release();
                dsvTex = nullptr;
                return false;
            }
            dsvWidth  = w;
            dsvHeight = h;
        }
        return true;
    }

    bool ensureUiBackup(ID3D11Device* dev, DXGI_FORMAT rtvFmt, UINT w, UINT h) {
        if (w == 0 || h == 0) return false;
        if (uiBackupTex && uiBackupSrv && backupWidth == w && backupHeight == h && backupFormat == rtvFmt) {
            return true;
        }
        if (uiBackupSrv) { uiBackupSrv->Release(); uiBackupSrv = nullptr; }
        if (uiBackupTex) { uiBackupTex->Release(); uiBackupTex = nullptr; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = ResolveTypelessFormat(rtvFmt);
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &uiBackupTex))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = rtvFmt;
        sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        if (FAILED(dev->CreateShaderResourceView(uiBackupTex, &sd, &uiBackupSrv))) {
            uiBackupTex->Release();
            uiBackupTex = nullptr;
            return false;
        }
        backupWidth  = w;
        backupHeight = h;
        backupFormat = rtvFmt;
        return true;
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
    ID3D12Device* device                     = nullptr;
    ID3D12RootSignature* rootSig             = nullptr;
    ID3D12PipelineState* pso                 = nullptr;
    DXGI_FORMAT psoFormat                    = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState* psoOpaqueWrite      = nullptr;
    ID3D12PipelineState* psoDepthRead        = nullptr;
    ID3D12PipelineState* psoDepthClipNoDsv   = nullptr;
    ID3D12PipelineState* psoOverlayDepthRead = nullptr;
    ID3D12PipelineState* psoRestoreUi        = nullptr;
    DXGI_FORMAT depthPsoFormat               = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* dsvHeap            = nullptr;
    ID3D12DescriptorHeap* srvHeap            = nullptr; // 16 shader-visible SRVs (4 frame slots * 4 descriptors)
    UINT srvDescSize                         = 0;
    ID3D12Resource* dsvTex                   = nullptr;
    UINT64 dsvWidth                          = 0;
    UINT dsvHeight                           = 0;
    ID3D12Resource* uiBackupTex              = nullptr;
    UINT64 backupWidth                       = 0;
    UINT backupHeight                        = 0;
    DXGI_FORMAT backupFormat                 = DXGI_FORMAT_UNKNOWN;
    ID3D12GraphicsCommandList* cmdList       = nullptr;
    ID3D12Fence* fence                       = nullptr;
    HANDLE fenceEvent                        = nullptr;
    UINT64 nextFenceVal                      = 0;
    std::uint32_t slotIndex                  = 0;
    D3D12FrameSlot slots[4]{};
    bool loggedInit                          = false;

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
        if (cmdList)             { cmdList->Release();             cmdList = nullptr; }
        if (uiBackupTex)         { uiBackupTex->Release();         uiBackupTex = nullptr; }
        if (dsvTex)              { dsvTex->Release();              dsvTex = nullptr; }
        if (srvHeap)             { srvHeap->Release();             srvHeap = nullptr; }
        if (dsvHeap)             { dsvHeap->Release();             dsvHeap = nullptr; }
        if (psoRestoreUi)        { psoRestoreUi->Release();        psoRestoreUi = nullptr; }
        if (psoOverlayDepthRead) { psoOverlayDepthRead->Release(); psoOverlayDepthRead = nullptr; }
        if (psoDepthClipNoDsv)   { psoDepthClipNoDsv->Release();   psoDepthClipNoDsv = nullptr; }
        if (psoDepthRead)        { psoDepthRead->Release();        psoDepthRead = nullptr; }
        if (psoOpaqueWrite)      { psoOpaqueWrite->Release();      psoOpaqueWrite = nullptr; }
        if (pso)                 { pso->Release();                 pso = nullptr; }
        if (rootSig)             { rootSig->Release();             rootSig = nullptr; }
        if (fence)               { fence->Release();               fence = nullptr; }
        if (fenceEvent)          { ::CloseHandle(fenceEvent);      fenceEvent = nullptr; }
        device = nullptr;
        psoFormat = DXGI_FORMAT_UNKNOWN;
        depthPsoFormat = DXGI_FORMAT_UNKNOWN;
        srvDescSize = 0;
        dsvWidth = 0;
        dsvHeight = 0;
        backupWidth = 0;
        backupHeight = 0;
        backupFormat = DXGI_FORMAT_UNKNOWN;
        nextFenceVal = 0;
        slotIndex = 0;
    }

    bool ensureSceneResources(ID3D12Device* dev, DXGI_FORMAT rtvFormat, UINT64 w, UINT h, bool needDsv) {
        if (w == 0 || h == 0 || !rootSig || !g_vsBlob || !g_vsFullscreenBlob ||
            !g_psBlob || !g_psDepthClipBlob || !g_psRestoreUiBlob) {
            return false;
        }

        if (!psoOpaqueWrite || !psoDepthRead || !psoDepthClipNoDsv ||
            !psoOverlayDepthRead || !psoRestoreUi || depthPsoFormat != rtvFormat) {
            waitForIdle();
            if (psoRestoreUi)        { psoRestoreUi->Release();        psoRestoreUi = nullptr; }
            if (psoOverlayDepthRead) { psoOverlayDepthRead->Release(); psoOverlayDepthRead = nullptr; }
            if (psoDepthClipNoDsv)   { psoDepthClipNoDsv->Release();   psoDepthClipNoDsv = nullptr; }
            if (psoDepthRead)        { psoDepthRead->Release();        psoDepthRead = nullptr; }
            if (psoOpaqueWrite)      { psoOpaqueWrite->Release();      psoOpaqueWrite = nullptr; }

            const D3D12_INPUT_ELEMENT_DESC elems[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            auto releasePartialPsos = [&]() {
                if (psoRestoreUi)        { psoRestoreUi->Release();        psoRestoreUi = nullptr; }
                if (psoOverlayDepthRead) { psoOverlayDepthRead->Release(); psoOverlayDepthRead = nullptr; }
                if (psoDepthClipNoDsv)   { psoDepthClipNoDsv->Release();   psoDepthClipNoDsv = nullptr; }
                if (psoDepthRead)        { psoDepthRead->Release();        psoDepthRead = nullptr; }
                if (psoOpaqueWrite)      { psoOpaqueWrite->Release();      psoOpaqueWrite = nullptr; }
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rootSig;
            pd.VS = {g_vsBlob->GetBufferPointer(), g_vsBlob->GetBufferSize()};
            pd.PS = {g_psDepthClipBlob->GetBufferPointer(), g_psDepthClipBlob->GetBufferSize()};
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = FALSE;
            pd.DepthStencilState.DepthEnable    = TRUE;
            pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            pd.DepthStencilState.StencilEnable  = FALSE;
            pd.InputLayout                      = {elems, 2};
            pd.PrimitiveTopologyType            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets                 = 1;
            pd.RTVFormats[0]                    = rtvFormat;
            pd.DSVFormat                        = DXGI_FORMAT_D32_FLOAT;
            pd.SampleDesc.Count                 = 1;

            // PSO 1: Opaque .sbc triangles (PSDepthClip + Blend OFF + DepthWrite ALL + GREATER_EQUAL)
            pd.BlendState.RenderTarget[0].BlendEnable           = FALSE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoOpaqueWrite)))) {
                releasePartialPsos();
                return false;
            }

            // PSO 2a: Depth-tested .sbc creases & ground rings on Opaque .sbc (PSDepthClip + AlphaBlend + DepthRead)
            pd.DepthStencilState.DepthWriteMask                 = D3D12_DEPTH_WRITE_MASK_ZERO;
            pd.BlendState.RenderTarget[0].BlendEnable           = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoDepthRead)))) {
                releasePartialPsos();
                return false;
            }

            // PSO 3a: Entity 3D hitboxes/hurtboxes occluded by foreground Opaque .sbc walls (PSMain + AlphaBlend + DepthRead)
            pd.PS = {g_psBlob->GetBufferPointer(), g_psBlob->GetBufferSize()};
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoOverlayDepthRead)))) {
                releasePartialPsos();
                return false;
            }

            // PSO 2b: Semi-Transparent .sbc / Raw Wireframe / Ground Rings without plugin DSV (PSDepthClip + AlphaBlend + Depth OFF)
            pd.PS                               = {g_psDepthClipBlob->GetBufferPointer(), g_psDepthClipBlob->GetBufferSize()};
            pd.DepthStencilState.DepthEnable    = FALSE;
            pd.DSVFormat                        = DXGI_FORMAT_UNKNOWN;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoDepthClipNoDsv)))) {
                releasePartialPsos();
                return false;
            }

            // PSO 4: Fullscreen 2D Player UI Restore (VSFullscreen + PSRestoreUI + AlphaBlend + Depth OFF)
            pd.VS                               = {g_vsFullscreenBlob->GetBufferPointer(), g_vsFullscreenBlob->GetBufferSize()};
            pd.PS                               = {g_psRestoreUiBlob->GetBufferPointer(), g_psRestoreUiBlob->GetBufferSize()};
            pd.InputLayout                      = {nullptr, 0};
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoRestoreUi)))) {
                releasePartialPsos();
                return false;
            }

            depthPsoFormat = rtvFormat;
        }

        if (!srvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC shd{};
            shd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            shd.NumDescriptors = 32; // 4 frame slots * 8 descriptors (t0..t4 + spare)
            shd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srvHeap)))) return false;
            srvDescSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        if (!needDsv) {
            return true;
        }

        if (!dsvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC dhd{};
            dhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dhd.NumDescriptors = 1;
            dhd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsvHeap)))) return false;
        }

        if (!dsvTex || dsvWidth != w || dsvHeight != h) {
            if (dsvTex) {
                waitForIdle();
                dsvTex->Release();
                dsvTex = nullptr;
            }

            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = w;
            rd.Height           = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_D32_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE cv{};
            cv.Format               = DXGI_FORMAT_D32_FLOAT;
            cv.DepthStencil.Depth   = 0.0f;
            cv.DepthStencil.Stencil = 0;

            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                    IID_PPV_ARGS(&dsvTex)))) {
                return false;
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC vd{};
            vd.Format        = DXGI_FORMAT_D32_FLOAT;
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            vd.Flags         = D3D12_DSV_FLAG_NONE;
            dev->CreateDepthStencilView(dsvTex, &vd, dsvHeap->GetCPUDescriptorHandleForHeapStart());
            dsvWidth  = w;
            dsvHeight = h;
        }
        return true;
    }

    bool ensureUiBackup(ID3D12Device* dev, DXGI_FORMAT rtvFormat, UINT64 w, UINT h) {
        if (w == 0 || h == 0) return false;
        if (uiBackupTex && backupWidth == w && backupHeight == h && backupFormat == rtvFormat) {
            return true;
        }
        if (uiBackupTex) {
            waitForIdle();
            uiBackupTex->Release();
            uiBackupTex = nullptr;
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = w;
        rd.Height           = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = ResolveTypelessFormat(rtvFormat);
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&uiBackupTex)))) {
            return false;
        }
        backupWidth  = w;
        backupHeight = h;
        backupFormat = rtvFormat;
        return true;
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

            D3D12_DESCRIPTOR_RANGE srvRange{};
            srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors                    = 5; // t0..t4
            srvRange.BaseShaderRegister                = 0;
            srvRange.RegisterSpace                     = 0;
            srvRange.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER rootParams[2]{};
            rootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rootParams[0].Constants.ShaderRegister = 0; // b0
            rootParams[0].Constants.RegisterSpace  = 0;
            rootParams[0].Constants.Num32BitValues = sizeof(OverlayConstants) / 4;
            rootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

            rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
            rootParams[1].DescriptorTable.pDescriptorRanges   = &srvRange;
            rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.MipLODBias       = 0.0f;
            samp.MaxAnisotropy    = 1;
            samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
            samp.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            samp.MinLOD           = 0.0f;
            samp.MaxLOD           = D3D12_FLOAT32_MAX;
            samp.ShaderRegister   = 0; // s0
            samp.RegisterSpace    = 0;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters     = 2;
            rsd.pParameters       = rootParams;
            rsd.NumStaticSamplers = 1;
            rsd.pStaticSamplers   = &samp;
            rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ID3DBlob* sigBlob = nullptr;
            ID3DBlob* errBlob = nullptr;
            if (FAILED(fnSerialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) {
                app::Log("render: D3D12SerializeRootSignature failed (%s)",
                         errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
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
bool g_loggedSceneDepth = false;
bool g_loggedSceneUi    = false;

void LogSceneBindOnce(bool isDx12, const EngineSceneTextures& engTex, const coll::FrameStats& stats) noexcept {
    const bool newDepth = engTex.hasDepth && !g_loggedSceneDepth;
    const bool newUi    = engTex.hasUi && !g_loggedSceneUi;
    if (!newDepth && !newUi) return;
    if (engTex.hasDepth) g_loggedSceneDepth = true;
    if (engTex.hasUi)    g_loggedSceneUi    = true;
    app::Log("render: scenebind dx12=%d depth=%d(%.0fx%.0f st=0x%X) ui=%d(src=%u dual=%d post=%.0fx%.0f[st=0x%X] pre=%.0fx%.0f[st=0x%X,0x%X]) proj(m32=%.3f,m22=%.6f)",
             isDx12 ? 1 : 0,
             engTex.hasDepth ? 1 : 0, engTex.depthActiveW, engTex.depthActiveH, static_cast<unsigned>(engTex.depthState12),
             engTex.hasUi ? 1 : 0, static_cast<unsigned>(engTex.uiSourceKind), engTex.dualPreUi ? 1 : 0,
             engTex.postUiActW, engTex.postUiActH, static_cast<unsigned>(engTex.postUiState12),
             engTex.preUiActW, engTex.preUiActH, static_cast<unsigned>(engTex.preUi0State12), static_cast<unsigned>(engTex.preUi1State12),
             stats.projM32, stats.projM22);
}

OverlayConstants MakeOverlayConstants(float screenW,
                                      float screenH,
                                      const coll::FrameStats& stats,
                                      const EngineSceneTextures& engTex,
                                      bool visEnabled,
                                      const app::ViewFilterState& viewState) noexcept {
    const bool hideAll3D     = visEnabled && (viewState.backdrop == app::BackdropMode::HideAll3D);
    const bool hideStage     = visEnabled && (viewState.stageHide != app::StageHideMode::Off);
    const bool isOpaqueSbc   = (viewState.depthMode == app::DepthGuideMode::Opaque ||
                                viewState.depthMode == app::DepthGuideMode::OpaqueNoRings);
    const bool useSceneDepth = engTex.hasDepth && !hideAll3D;
    const bool exactLoad     = engTex.hasUi &&
                               std::fabs(engTex.postUiActW - screenW) < 0.5f &&
                               std::fabs(engTex.postUiActH - screenH) < 0.5f &&
                               std::fabs(engTex.preUiActW - screenW) < 0.5f &&
                               std::fabs(engTex.preUiActH - screenH) < 0.5f &&
                               std::fabs(engTex.postUiUvScaleX - 1.0f) < 1e-4f &&
                               std::fabs(engTex.postUiUvScaleY - 1.0f) < 1e-4f &&
                               std::fabs(engTex.preUiUvScaleX - 1.0f) < 1e-4f &&
                               std::fabs(engTex.preUiUvScaleY - 1.0f) < 1e-4f;

    std::uint32_t flags = 0u;
    if (useSceneDepth)            flags |= 1u;
    if (engTex.dualPreUi)         flags |= 2u;
    if (exactLoad)                flags |= 4u;

    OverlayConstants cb{};
    cb.invScreenX     = (screenW > 1.0f) ? (1.0f / screenW) : (1.0f / 1920.0f);
    cb.invScreenY     = (screenH > 1.0f) ? (1.0f / screenH) : (1.0f / 1080.0f);
    cb.depthActiveW   = (engTex.depthActiveW > 1.0f) ? engTex.depthActiveW : screenW;
    cb.depthActiveH   = (engTex.depthActiveH > 1.0f) ? engTex.depthActiveH : screenH;
    cb.postUiUvScaleX = engTex.postUiUvScaleX;
    cb.postUiUvScaleY = engTex.postUiUvScaleY;
    cb.preUiUvScaleX  = engTex.preUiUvScaleX;
    cb.preUiUvScaleY  = engTex.preUiUvScaleY;
    cb.projM32        = stats.projM32;
    cb.projM22        = stats.projM22;
    // Positive foreground depth margin (cm): scene pixel only occludes .sbc when dSceneCm + tolCm <= dSbcCm.
    // Fixes prior +6cm sign bug in KeepInteract/HideAll that discarded .sbc faces flush with interactive props,
    // and prevents minor 3D visual dirt/grass bumps from punching holes in Opaque .sbc when Hide Map is OFF.
    cb.depthBiasCm    = hideStage ? 12.0f : (isOpaqueSbc ? 45.0f : 24.0f);
    cb.flags          = flags;
    return cb;
}

void WriteD3D12SrvSlot(ID3D12Device* dev,
                       D3D12_CPU_DESCRIPTOR_HANDLE dstCpu,
                       ID3D12Resource* res,
                       D3D12_CPU_DESCRIPTOR_HANDLE srcCpu,
                       DXGI_FORMAT fallbackFmt,
                       bool isDepth) noexcept {
    if (res) {
        if (srcCpu.ptr != 0) {
            dev->CopyDescriptorsSimple(1, dstCpu, srcCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return;
        }
        const D3D12_RESOURCE_DESC rd = res->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = isDepth ? ResolveDepthSrvFormat(rd.Format)
                                               : ResolveColorSrvFormat(rd.Format, fallbackFmt);
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        dev->CreateShaderResourceView(res, &sd, dstCpu);
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSd{};
    nullSd.Format                    = fallbackFmt;
    nullSd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSd.Texture2D.MostDetailedMip = 0;
    nullSd.Texture2D.MipLevels       = 1;
    dev->CreateShaderResourceView(nullptr, &nullSd, dstCpu);
}

void RenderD3D11(std::uintptr_t exeBase,
                 std::uintptr_t d3dRender,
                 bool visEnabled,
                 freeze::Mode freezeMode,
                 const app::ViewFilterState& viewState,
                 const app::Config& cfg,
                 bool showStartupToast) {
    if (!d3dRender) return;
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
                                visEnabled, freezeMode, viewState, cfg, showStartupToast, g_verts, stats);

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
                    const UINT stride = sizeof(coll::OverlayVertex);
                    const UINT offset = 0;
                    ctx->IASetInputLayout(g_d3d11.layout);
                    ctx->IASetVertexBuffers(0, 1, &g_d3d11.vb, &stride, &offset);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                    ctx->VSSetShader(g_d3d11.vs, nullptr, 0);
                    ctx->GSSetShader(nullptr, nullptr, 0);

                    const UINT totalVerts = static_cast<UINT>(g_verts.size());
                    const UINT p1End = (stats.opaqueTriVerts <= totalVerts) ? stats.opaqueTriVerts : 0U;
                    const UINT p2End = (stats.sceneDepthEndVert >= p1End && stats.sceneDepthEndVert <= totalVerts)
                                           ? stats.sceneDepthEndVert
                                           : p1End;
                    const UINT p3End = (stats.worldOverlayEndVert >= p2End && stats.worldOverlayEndVert <= totalVerts)
                                           ? stats.worldOverlayEndVert
                                           : p2End;
                    const bool useOpaqueDsv       = (p1End >= 3U);
                    const bool useSceneComposite  = (p3End >= 3U) &&
                                                    g_d3d11.ensureSceneResources(dev, td.Width, td.Height, useOpaqueDsv);

                    if (useSceneComposite) {
                        EngineSceneTextures engTex{};
                        ResolveEngineSceneTexturesSEH(exeBase, false, &engTex);
                        LogSceneBindOnce(false, engTex, stats);
                        const OverlayConstants cbData = MakeOverlayConstants(
                            static_cast<float>(td.Width), static_cast<float>(td.Height),
                            stats, engTex, visEnabled, viewState);

                        const bool useSceneDepth = (p2End >= 3U) && ((cbData.flags & 1u) != 0u);
                        const bool canRestoreUi  = engTex.hasUi &&
                                                   g_d3d11.ensureUiBackup(dev, rtvFmt, td.Width, td.Height);
                        if (canRestoreUi) {
                            ctx->CopyResource(g_d3d11.uiBackupTex, backBuf);
                        }

                        D3D11_MAPPED_SUBRESOURCE cbMap{};
                        if (SUCCEEDED(ctx->Map(g_d3d11.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMap))) {
                            std::memcpy(cbMap.pData, &cbData, sizeof(cbData));
                            ctx->Unmap(g_d3d11.cb, 0);
                        }

                        ID3D11Buffer* oldPsCb = nullptr;
                        ID3D11ShaderResourceView* oldPsSrvs[5]{};
                        ID3D11SamplerState* oldPsSamp = nullptr;
                        ctx->PSGetConstantBuffers(0, 1, &oldPsCb);
                        ctx->PSGetShaderResources(0, 5, oldPsSrvs);
                        ctx->PSGetSamplers(0, 1, &oldPsSamp);

                        ID3D11ShaderResourceView* srvs[5] = {
                            useSceneDepth ? engTex.depthSrv11   : nullptr,
                            canRestoreUi  ? g_d3d11.uiBackupSrv : nullptr,
                            canRestoreUi  ? engTex.postUiSrv11  : nullptr,
                            canRestoreUi  ? engTex.preUi0Srv11  : nullptr,
                            canRestoreUi  ? engTex.preUi1Srv11  : nullptr
                        };
                        ctx->PSSetConstantBuffers(0, 1, &g_d3d11.cb);
                        ctx->PSSetShaderResources(0, 5, srvs);
                        ctx->PSSetSamplers(0, 1, &g_d3d11.linearSampler);

                        if (useOpaqueDsv) {
                            ctx->ClearDepthStencilView(g_d3d11.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
                            ctx->OMSetRenderTargets(1, &rtv, g_d3d11.dsv);

                            // Pass 1: Right-Side-Out Opaque .SBC Terrain (PSDepthClip + Blend OFF + Reversed-Z Write)
                            ctx->PSSetShader(g_d3d11.psDepthClip, nullptr, 0);
                            ctx->OMSetBlendState(g_d3d11.blendOpaque, bf, 0xFFFFFFFFU);
                            ctx->OMSetDepthStencilState(g_d3d11.depthWrite, 0);
                            ctx->Draw(p1End, 0);

                            ctx->OMSetBlendState(g_d3d11.blend, bf, 0xFFFFFFFFU);
                            ctx->OMSetDepthStencilState(g_d3d11.depthRead, 0);

                            // Pass 2: Scene- & Opaque-Depth-Tested .SBC Creases & Ground Rings
                            if (p2End > p1End) {
                                ctx->Draw(p2End - p1End, p1End);
                            }

                            // Pass 3: Entity 3D Hitboxes/Hurtboxes (occluded behind foreground Opaque .SBC walls)
                            if (p3End > p2End) {
                                ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                                ctx->Draw(p3End - p2End, p2End);
                            }

                            ctx->OMSetRenderTargets(1, &rtv, nullptr);
                            ctx->OMSetDepthStencilState(g_d3d11.depth, 0);
                        } else {
                            ctx->OMSetRenderTargets(1, &rtv, nullptr);
                            ctx->OMSetBlendState(g_d3d11.blend, bf, 0xFFFFFFFFU);
                            ctx->OMSetDepthStencilState(g_d3d11.depth, 0);

                            // Pass 2: Scene-Depth-Tested Semi-Transparent .SBC / Wireframe / Ground Rings
                            if (p2End > 0U) {
                                ctx->PSSetShader(g_d3d11.psDepthClip, nullptr, 0);
                                ctx->Draw(p2End, 0);
                            }

                            // Pass 3: Entity 3D Hitboxes/Hurtboxes
                            if (p3End > p2End) {
                                ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                                ctx->Draw(p3End - p2End, p2End);
                            }
                        }

                        // Pass 4: Restore 2D Player UI (uGUI / uMhGUI) on top of ALL 3D overlays!
                        if (canRestoreUi) {
                            ctx->IASetInputLayout(nullptr);
                            ctx->VSSetShader(g_d3d11.vsFullscreen, nullptr, 0);
                            ctx->PSSetShader(g_d3d11.psRestoreUi, nullptr, 0);
                            ctx->Draw(3, 0);
                            ctx->IASetInputLayout(g_d3d11.layout);
                            ctx->VSSetShader(g_d3d11.vs, nullptr, 0);
                        }

                        // Unbind engine SRVs before Pass 5 & state restore
                        ID3D11ShaderResourceView* nullSrvs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
                        ctx->PSSetShaderResources(0, 5, nullSrvs);

                        // Pass 5: Plugin 2D HUD Banner & [P] Filter Panel (Always Crisp on Top)
                        if (totalVerts > p3End) {
                            ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                            ctx->Draw(totalVerts - p3End, p3End);
                        }

                        ctx->PSSetSamplers(0, 1, &oldPsSamp);
                        ctx->PSSetShaderResources(0, 5, oldPsSrvs);
                        ctx->PSSetConstantBuffers(0, 1, &oldPsCb);
                        if (oldPsSamp) oldPsSamp->Release();
                        for (auto* s : oldPsSrvs) { if (s) s->Release(); }
                        if (oldPsCb)   oldPsCb->Release();
                    } else {
                        ctx->PSSetShader(g_d3d11.ps, nullptr, 0);
                        ctx->OMSetBlendState(g_d3d11.blend, bf, 0xFFFFFFFFU);
                        ctx->OMSetDepthStencilState(g_d3d11.depth, 0);
                        ctx->OMSetRenderTargets(1, &rtv, nullptr);
                        ctx->Draw(totalVerts, 0);
                    }

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
                 const app::ViewFilterState& viewState,
                 const app::Config& cfg,
                 bool showStartupToast) {
    if (!d3dRender) return;
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
                            visEnabled, freezeMode, viewState, cfg, showStartupToast, g_verts, stats);

    if (g_verts.size() >= 3 && g_d3d12.ensure(dev, rtvFmt)) {
        const UINT totalVerts = static_cast<UINT>(g_verts.size());
        const UINT p1End = (stats.opaqueTriVerts <= totalVerts) ? stats.opaqueTriVerts : 0U;
        const UINT p2End = (stats.sceneDepthEndVert >= p1End && stats.sceneDepthEndVert <= totalVerts)
                               ? stats.sceneDepthEndVert
                               : p1End;
        const UINT p3End = (stats.worldOverlayEndVert >= p2End && stats.worldOverlayEndVert <= totalVerts)
                               ? stats.worldOverlayEndVert
                               : p2End;
        const bool useOpaqueDsv      = (p1End >= 3U);
        const bool useSceneComposite = (p3End >= 3U) &&
                                       g_d3d12.ensureSceneResources(dev, rtvFmt, bbDesc.Width, bbDesc.Height, useOpaqueDsv);

        EngineSceneTextures engTex{};
        OverlayConstants cbData{};
        bool canRestoreUi = false;
        if (useSceneComposite) {
            ResolveEngineSceneTexturesSEH(exeBase, true, &engTex);
            LogSceneBindOnce(true, engTex, stats);
            cbData = MakeOverlayConstants(static_cast<float>(bbDesc.Width),
                                          static_cast<float>(bbDesc.Height),
                                          stats, engTex, visEnabled, viewState);
            canRestoreUi = engTex.hasUi &&
                           (engTex.postUiRes12 != backBuf) &&
                           (engTex.preUi0Res12 != backBuf) &&
                           (engTex.preUi1Res12 != backBuf) &&
                           g_d3d12.ensureUiBackup(dev, rtvFmt, bbDesc.Width, bbDesc.Height);
        }

        const std::uint32_t curSlotIdx = g_d3d12.slotIndex;
        D3D12FrameSlot& slot = g_d3d12.slots[curSlotIdx];
        g_d3d12.slotIndex = (curSlotIdx + 1) & 3U;

        if (slot.fenceVal > 0 && g_d3d12.fence->GetCompletedValue() < slot.fenceVal) {
            g_d3d12.fence->SetEventOnCompletion(slot.fenceVal, g_d3d12.fenceEvent);
            ::WaitForSingleObject(g_d3d12.fenceEvent, 2000);
        }

        ID3D12PipelineState* initPso = g_d3d12.pso;
        if (useSceneComposite) {
            if (useOpaqueDsv) {
                initPso = g_d3d12.psoOpaqueWrite;
            } else if (p2End > 0U) {
                initPso = g_d3d12.psoDepthClipNoDsv;
            }
        }

        if (g_d3d12.ensureSlotVb(dev, slot, g_verts.size()) &&
            SUCCEEDED(slot.alloc->Reset()) &&
            SUCCEEDED(g_d3d12.cmdList->Reset(slot.alloc, initPso))) {

            ID3D12PipelineState* curPso = initPso;
            auto setPso = [&](ID3D12PipelineState* next) noexcept {
                if (curPso != next) {
                    g_d3d12.cmdList->SetPipelineState(next);
                    curPso = next;
                }
            };

            const std::size_t byteLen = g_verts.size() * sizeof(coll::OverlayVertex);
            std::memcpy(slot.mapped, g_verts.data(), byteLen);

            const bool useSceneDepth = useSceneComposite && (p2End >= 3U) &&
                                       ((cbData.flags & 1u) != 0u) &&
                                       (engTex.depthRes12 != backBuf);

            struct TrackedBarrierTex {
                ID3D12Resource*       res;
                D3D12_RESOURCE_STATES origState;
            };
            TrackedBarrierTex barTex[4]{};
            UINT nBarTex = 0;
            auto trackEngineTex = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES st) noexcept {
                if (!res || res == backBuf) return;
                if ((st & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0) return;
                for (UINT i = 0; i < nBarTex; ++i) {
                    if (barTex[i].res == res) return;
                }
                if (nBarTex < 4U) {
                    barTex[nBarTex++] = {res, st};
                }
            };
            if (useSceneDepth) {
                trackEngineTex(engTex.depthRes12, engTex.depthState12);
            }
            if (canRestoreUi) {
                trackEngineTex(engTex.postUiRes12, engTex.postUiState12);
                trackEngineTex(engTex.preUi0Res12, engTex.preUi0State12);
                if (engTex.dualPreUi) {
                    trackEngineTex(engTex.preUi1Res12, engTex.preUi1State12);
                }
            }

            auto makeTransition = [](ID3D12Resource* res,
                                     D3D12_RESOURCE_STATES before,
                                     D3D12_RESOURCE_STATES after) noexcept -> D3D12_RESOURCE_BARRIER {
                D3D12_RESOURCE_BARRIER b{};
                b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = before;
                b.Transition.StateAfter  = after;
                return b;
            };

            if (canRestoreUi) {
                // Copy final tonemapped/PQ/scRGB backbuffer (containing 2D Player UI) to uiBackupTex before 3D overlays
                D3D12_RESOURCE_BARRIER copyIn[2] = {
                    makeTransition(backBuf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    makeTransition(g_d3d12.uiBackupTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
                };
                g_d3d12.cmdList->ResourceBarrier(2, copyIn);
                g_d3d12.cmdList->CopyResource(g_d3d12.uiBackupTex, backBuf);

                D3D12_RESOURCE_BARRIER bIn[6]{};
                UINT nIn = 0;
                bIn[nIn++] = makeTransition(backBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                bIn[nIn++] = makeTransition(g_d3d12.uiBackupTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                for (UINT i = 0; i < nBarTex; ++i) {
                    bIn[nIn++] = makeTransition(barTex[i].res, barTex[i].origState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                g_d3d12.cmdList->ResourceBarrier(nIn, bIn);
            } else {
                D3D12_RESOURCE_BARRIER bIn[5]{};
                UINT nIn = 0;
                bIn[nIn++] = makeTransition(backBuf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                for (UINT i = 0; i < nBarTex; ++i) {
                    bIn[nIn++] = makeTransition(barTex[i].res, barTex[i].origState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                g_d3d12.cmdList->ResourceBarrier(nIn, bIn);
            }

            const std::uint32_t rtvDescSize = *reinterpret_cast<const std::uint32_t*>(d3dRender + mhw::kOffD3DRtvDescSize12);
            const SIZE_T rtvStart           = *reinterpret_cast<const SIZE_T*>(d3dRender + mhw::kOffD3DRtvStart12);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{rtvStart + static_cast<SIZE_T>(bbIdx) * rtvDescSize};

            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<FLOAT>(bbDesc.Width), static_cast<FLOAT>(bbDesc.Height), 0.0f, 1.0f};
            D3D12_RECT sc{0, 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height)};
            g_d3d12.cmdList->RSSetViewports(1, &vp);
            g_d3d12.cmdList->RSSetScissorRects(1, &sc);

            if (useSceneComposite) {
                const SIZE_T srvStride = g_d3d12.srvDescSize;
                const SIZE_T slotCpuBase = g_d3d12.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                           static_cast<SIZE_T>(curSlotIdx) * 8U * srvStride;
                const UINT64 slotGpuBase = g_d3d12.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                                           static_cast<UINT64>(curSlotIdx) * 8ULL * srvStride;

                WriteD3D12SrvSlot(dev, D3D12_CPU_DESCRIPTOR_HANDLE{slotCpuBase + 0 * srvStride},
                                  useSceneDepth ? engTex.depthRes12 : nullptr,
                                  engTex.depthSrvCpu12, DXGI_FORMAT_R32_FLOAT, true);
                WriteD3D12SrvSlot(dev, D3D12_CPU_DESCRIPTOR_HANDLE{slotCpuBase + 1 * srvStride},
                                  canRestoreUi ? g_d3d12.uiBackupTex : nullptr,
                                  D3D12_CPU_DESCRIPTOR_HANDLE{0}, rtvFmt, false);
                WriteD3D12SrvSlot(dev, D3D12_CPU_DESCRIPTOR_HANDLE{slotCpuBase + 2 * srvStride},
                                  canRestoreUi ? engTex.postUiRes12 : nullptr,
                                  engTex.postUiSrvCpu12, rtvFmt, false);
                WriteD3D12SrvSlot(dev, D3D12_CPU_DESCRIPTOR_HANDLE{slotCpuBase + 3 * srvStride},
                                  canRestoreUi ? engTex.preUi0Res12 : nullptr,
                                  engTex.preUi0SrvCpu12, rtvFmt, false);
                WriteD3D12SrvSlot(dev, D3D12_CPU_DESCRIPTOR_HANDLE{slotCpuBase + 4 * srvStride},
                                  canRestoreUi ? engTex.preUi1Res12 : nullptr,
                                  engTex.preUi1SrvCpu12, rtvFmt, false);

                ID3D12DescriptorHeap* heaps[1] = {g_d3d12.srvHeap};
                g_d3d12.cmdList->SetDescriptorHeaps(1, heaps);
                g_d3d12.cmdList->SetGraphicsRootSignature(g_d3d12.rootSig);
                g_d3d12.cmdList->SetGraphicsRoot32BitConstants(0, sizeof(OverlayConstants) / 4, &cbData, 0);
                g_d3d12.cmdList->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{slotGpuBase});
            } else {
                g_d3d12.cmdList->SetGraphicsRootSignature(g_d3d12.rootSig);
            }

            g_d3d12.cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = slot.vb->GetGPUVirtualAddress();
            vbv.SizeInBytes    = static_cast<UINT>(byteLen);
            vbv.StrideInBytes  = sizeof(coll::OverlayVertex);
            g_d3d12.cmdList->IASetVertexBuffers(0, 1, &vbv);

            if (useSceneComposite) {
                if (useOpaqueDsv) {
                    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_d3d12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
                    g_d3d12.cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                    g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                    // Pass 1: Right-Side-Out Opaque .SBC Terrain (psoOpaqueWrite)
                    setPso(g_d3d12.psoOpaqueWrite);
                    g_d3d12.cmdList->DrawInstanced(p1End, 1, 0, 0);

                    // Pass 2: Scene- & Opaque-Depth-Tested .SBC Creases & Ground Rings (psoDepthRead)
                    if (p2End > p1End) {
                        setPso(g_d3d12.psoDepthRead);
                        g_d3d12.cmdList->DrawInstanced(p2End - p1End, 1, p1End, 0);
                    }

                    // Pass 3: Entity 3D Hitboxes/Hurtboxes occluded behind Opaque .SBC walls (psoOverlayDepthRead)
                    if (p3End > p2End) {
                        setPso(g_d3d12.psoOverlayDepthRead);
                        g_d3d12.cmdList->DrawInstanced(p3End - p2End, 1, p2End, 0);
                    }

                    // Unbind DSV before Depth-Off UI Restore & Plugin 2D HUD PSOs!
                    g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
                } else {
                    g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                    // Pass 2: Scene-Depth-Tested Semi-Transparent .SBC / Wireframe / Ground Rings
                    if (p2End > 0U) {
                        setPso(g_d3d12.psoDepthClipNoDsv);
                        g_d3d12.cmdList->DrawInstanced(p2End, 1, 0, 0);
                    }

                    // Pass 3: Entity 3D Hitboxes/Hurtboxes
                    if (p3End > p2End) {
                        setPso(g_d3d12.pso);
                        g_d3d12.cmdList->DrawInstanced(p3End - p2End, 1, p2End, 0);
                    }
                }

                // Pass 4: Restore 2D Player UI (uGUI / uMhGUI) on top of ALL 3D overlays!
                if (canRestoreUi) {
                    setPso(g_d3d12.psoRestoreUi);
                    g_d3d12.cmdList->DrawInstanced(3, 1, 0, 0);
                }

                // Pass 5: Plugin 2D HUD Banner & [P] Filter Panel
                if (totalVerts > p3End) {
                    setPso(g_d3d12.pso);
                    g_d3d12.cmdList->DrawInstanced(totalVerts - p3End, 1, p3End, 0);
                }
            } else {
                g_d3d12.cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
                setPso(g_d3d12.pso);
                g_d3d12.cmdList->DrawInstanced(totalVerts, 1, 0, 0);
            }

            D3D12_RESOURCE_BARRIER bOut[5]{};
            UINT nOut = 0;
            bOut[nOut++] = makeTransition(backBuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            for (UINT i = 0; i < nBarTex; ++i) {
                bOut[nOut++] = makeTransition(barTex[i].res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, barTex[i].origState);
            }
            g_d3d12.cmdList->ResourceBarrier(nOut, bOut);

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
               const app::ViewFilterState& viewState,
               const app::Config& cfg,
               bool showStartupToast) {
    if (!exeBase || !d3dRender) return;
    if (!visEnabled && freezeMode == freeze::Mode::None && !viewState.panelOpen && !showStartupToast) return;

    if (g_verts.capacity() < 65536) {
        g_verts.reserve(65536);
    }

    const bool isDx12 = (*reinterpret_cast<const std::uint8_t*>(exeBase + mhw::kRvaIsD3D12Byte) != 0);
    if (isDx12) {
        RenderD3D12(exeBase, d3dRender, visEnabled, freezeMode, viewState, cfg, showStartupToast);
    } else {
        RenderD3D11(exeBase, d3dRender, visEnabled, freezeMode, viewState, cfg, showStartupToast);
    }
}

HWND GetGameWindow(std::uintptr_t d3dRender) {
    if (d3dRender < 0x100000ULL || d3dRender > 0x00007FFFFFFF0000ULL) return nullptr;
    return *reinterpret_cast<HWND const*>(d3dRender + mhw::kOffD3DSwapHwnd);
}

void Shutdown() {
    g_d3d12.release();
    g_d3d11.release();
    if (g_psRestoreUiBlob)  { g_psRestoreUiBlob->Release();  g_psRestoreUiBlob = nullptr; }
    if (g_psDepthClipBlob)  { g_psDepthClipBlob->Release();  g_psDepthClipBlob = nullptr; }
    if (g_psBlob)           { g_psBlob->Release();           g_psBlob = nullptr; }
    if (g_vsFullscreenBlob) { g_vsFullscreenBlob->Release(); g_vsFullscreenBlob = nullptr; }
    if (g_vsBlob)           { g_vsBlob->Release();           g_vsBlob = nullptr; }
}

} // namespace render

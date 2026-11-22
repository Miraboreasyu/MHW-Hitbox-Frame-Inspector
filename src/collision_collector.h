#pragma once
#include "game_freeze.h"
#include "log_config.h"
#include <cstdint>
#include <vector>

namespace coll {

struct OverlayVertex {
    float x, y, z, w; // NDC coords (x,y in [-1..1], Reversed-Z z in [0..1], w=1)
    float r, g, b, a; // Output-encoded RGBA (sRGB, HDR10 ST.2084 PQ, or scRGB linear)
};

struct FrameStats {
    std::uint32_t totalShapes         = 0;
    std::uint32_t playerHitboxes      = 0;
    std::uint32_t playerHurtbox       = 0;
    std::uint32_t monsterHitbox       = 0;
    std::uint32_t monsterHurtbox      = 0;
    std::uint32_t envShapes           = 0;
    std::uint32_t sbcEdges            = 0;
    std::uint32_t otherShapes         = 0;
    std::uint32_t opaqueTriVerts      = 0; // [0 .. opaqueTriVerts): Pass 1 opaque .sbc tris (PSDepthClip + DepthWrite)
    std::uint32_t sceneDepthEndVert   = 0; // [opaqueTriVerts .. sceneDepthEndVert): Pass 2 .sbc faces/creases/rings (PSDepthClip + AlphaBlend)
    std::uint32_t worldOverlayEndVert = 0; // [sceneDepthEndVert .. worldOverlayEndVert): Pass 3a entity 3D hitboxes/hurtboxes
    std::uint32_t preUiLabelEndVert   = 0; // [worldOverlayEndVert .. preUiLabelEndVert): Pass 3b 3D part labels (Depth OFF, pre-UI restore!)
    float projM32                     = 10.0f; // |proj[14]| for exact Reversed-Z <-> world cm conversion
    float projM22                     = 0.0f;  // ||proj[10]| - 1| = zNear / (zFar - zNear)
    float dispX                       = 0.0f;  // Active BackBuffer display sub-rect left (px, 0x14229f090)
    float dispY                       = 0.0f;  // Active BackBuffer display sub-rect top  (px, 0x14229f090)
    float dispW                       = 0.0f;  // Active BackBuffer display sub-rect width  (px)
    float dispH                       = 0.0f;  // Active BackBuffer display sub-rect height (px)
    float sceneW                      = 0.0f;  // Active RTScreen logical width  (sMhRender + 0x198)
    float sceneH                      = 0.0f;  // Active RTScreen logical height (sMhRender + 0x19C)
    bool hasCamera                    = false;
};

struct EmInspectorCounts {
    std::uint32_t partCount    = 0;
    std::uint32_t monsterCount = 0;
};

EmInspectorCounts GetLastEmInspectorCounts() noexcept;

// Builds optional fullscreen collision backdrop + 3D wireframe quads + 2D HUD & [P] Filter Panel.
void BuildOverlayFrame(std::uintptr_t exeBase,
                       float screenW,
                       float screenH,
                       std::uint32_t backBufferDxgiFormat,
                       bool visEnabled,
                       freeze::Mode freezeMode,
                       const app::ViewFilterState& viewState,
                       const app::Config& cfg,
                       bool showStartupToast,
                       std::vector<OverlayVertex>& outVerts,
                       FrameStats& outStats);

} // namespace coll

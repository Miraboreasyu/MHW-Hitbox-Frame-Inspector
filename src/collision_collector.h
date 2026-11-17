#pragma once
#include "game_freeze.h"
#include "log_config.h"
#include <cstdint>
#include <vector>

namespace coll {

struct OverlayVertex {
    float x, y, z, w; // NDC coords (x,y in [-1..1], z=0, w=1)
    float r, g, b, a; // Output-encoded RGBA (sRGB, HDR10 ST.2084 PQ, or scRGB linear)
};

struct FrameStats {
    std::uint32_t totalShapes    = 0;
    std::uint32_t playerHitboxes = 0;
    std::uint32_t playerHurtbox  = 0;
    std::uint32_t monsterHitbox  = 0;
    std::uint32_t monsterHurtbox = 0;
    std::uint32_t otherShapes    = 0;
    bool hasCamera               = false;
};

// Builds all 3D wireframe quads + 2D HUD status banner into outVerts (TRIANGLELIST).
void BuildOverlayFrame(std::uintptr_t exeBase,
                       float screenW,
                       float screenH,
                       std::uint32_t backBufferDxgiFormat,
                       bool visEnabled,
                       freeze::Mode freezeMode,
                       app::FilterMode filterMode,
                       const app::Config& cfg,
                       bool showStartupToast,
                       std::vector<OverlayVertex>& outVerts,
                       FrameStats& outStats);

} // namespace coll

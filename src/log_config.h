#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace app {

// Fine-grained per-category visibility bitmask (8 independent toggles)
enum CatBit : std::uint32_t {
    kCatPlHurtbox = 1u << 0, // [1] Player Hurtbox + Defensive Frame State
    kCatPlHitbox  = 1u << 1, // [2] Player / Weapon / Kinsect / Shell / Claw Hitboxes
    kCatOtomo     = 1u << 2, // [3] Palico / Companion Hurtboxes & Hitboxes
    kCatEmHurtbox = 1u << 3, // [4] Monster Hurtboxes / Hitzones
    kCatEmHitbox  = 1u << 4, // [5] Monster / Breath / Monster-Shell Hitboxes
    kCatPushbox   = 1u << 5, // [6] Player & Monster physical body-push boxes
    kCatEnvObject = 1u << 6, // [7] Map Interactive Entities (pole, cart, barrels, boulders, traps, endemic)
    kCatStageMesh = 1u << 7, // [8] Surrounding Map Terrain / Wall / Ledge Meshes (.sbc)

    kCatMaskCombat   = kCatPlHurtbox | kCatPlHitbox | kCatEmHurtbox | kCatEmHitbox,
    kCatMaskPlEmOnly = kCatPlHurtbox | kCatPlHitbox | kCatOtomo | kCatEmHurtbox | kCatEmHitbox | kCatPushbox,
    kCatMaskMapOnly  = kCatEnvObject | kCatStageMesh,
    kCatMaskHitbox   = kCatPlHitbox  | kCatEmHitbox,
    kCatMaskHurtbox  = kCatPlHurtbox | kCatEmHurtbox,
    kCatMaskAll      = 0xFFu
};

enum class PresetId : int {
    Combat   = 0, // Player + Monster Hitboxes & Hurtboxes (default, ignores map)
    PlEmOnly = 1, // All Player + Monster + Palico + Pushboxes (ignores map entities & terrain)
    MapOnly  = 2, // Map Interactive Entities + .sbc Terrain ONLY (ignores Player & Monster)
    Hitbox   = 3, // Attack Hitboxes only (Player + Monster)
    Hurtbox  = 4, // Hurtboxes only (Player + Monster)
    All      = 5, // Everything (all 8 categories)
    Count    = 6
};

enum class StageHideMode : std::uint8_t {
    Off          = 0, // Normal map rendering
    KeepInteract = 1, // Hide static terrain/foliage/sky/water, KEEP interactive gimmicks/NPCs/endemic visible
    HideAll      = 2, // Hide ALL map & gimmick meshes (while keeping 100% interaction active!)
    Count        = 3
};

enum class BackdropMode : std::uint8_t {
    Off       = 0, // Normal 3D rendering behind wireframes
    HidePlVfx = 1, // Hide Player / Weapon / Palico / 3D VFX meshes (keeps Player UI 100% visible!)
    HideAll3D = 2, // Pure collision canvas: hide ALL 3D world + monster + player + VFX meshes (keeps Player UI 100% visible!)
    DimScrim  = 3, // Light 45% dark scrim when keeping 3D textures visible
    Count     = 4
};

enum class DepthGuideMode : std::uint8_t {
    Auto          = 0, // Semi-transparent 3D shaded .sbc + creases + auto ground rings
    Opaque        = 1, // 100% OPAQUE solid 3D shaded .sbc + hardware-occluded creases + auto ground rings
    OpaqueNoRings = 2, // 100% OPAQUE solid 3D shaded .sbc + hardware-occluded creases (no ground rings)
    AlwaysRings   = 3, // Semi-transparent 3D shaded .sbc + ground distance rings always visible
    NoRings       = 4, // Semi-transparent 3D shaded .sbc without ground distance rings
    RawWire       = 5, // Classic raw .sbc full triangle grid wireframe
    Count         = 6
};

inline constexpr int kSbcRadiusCount = 4;

inline float SbcRadiusMetersFromIdx(int idx) noexcept {
    static constexpr float kRadii[kSbcRadiusCount] = {15.0f, 25.0f, 45.0f, 80.0f};
    if (idx < 0) return kRadii[0];
    if (idx >= kSbcRadiusCount) return kRadii[kSbcRadiusCount - 1];
    return kRadii[idx];
}

struct ViewFilterState {
    std::uint32_t  visMask      = kCatMaskCombat;
    int            presetCursor = static_cast<int>(PresetId::Combat);
    int            sbcRadiusIdx = 1; // 0=15m, 1=25m (default), 2=45m, 3=80m
    BackdropMode   backdrop     = BackdropMode::Off;
    StageHideMode  stageHide    = StageHideMode::Off;
    DepthGuideMode depthMode    = DepthGuideMode::Auto;
    bool           hideMonster  = false;
    bool           panelOpen    = false;
    int            panelCursor  = 0; // 0..12
};

struct Config {
    bool enabled            = true;
    int vkToggleVis         = 'U';   // 0x55: Toggle hitbox/hurtbox display
    int vkToggleFreeze      = 'I';   // 0x49: Instant zero-delay full game freeze/resume
    int vkFreezeOthers      = 'K';   // 0x4B: Pause everything except the Player (World Freeze)
    int vkPersistHitbox     = 'J';   // 0x4A: Toggle keeping single-trigger hitboxes active while frozen
    int vkStepFrame         = 'O';   // 0x4F: Advance 1 frame while frozen (Full or World Freeze)
    int vkCycleFilter       = 'P';   // 0x50: Open/close interactive Filter & Scene Panel (Shift+P cycles preset)

    bool startVisOn         = false; // Whether hitbox display starts ON automatically
    bool keepHitboxesActive = true;  // Keep single-trigger frozen hitboxes active & repeatable in World Freeze
    bool blockCameraKeys    = true;  // Block O/K/L/; camera rotation keys while mod is loaded (except in chat)
    bool showHudBanner      = true;  // Show compact top-left status indicator
    float lineThickness     = 2.0f;  // Base wireframe thickness in pixels (at 1080p)
    float opacityScale      = 1.0f;  // Global opacity multiplier (0.1 .. 1.5)
    float sbcSurfaceOpacity = 1.0f;  // .sbc semi-transparent face opacity multiplier (0.1 .. 3.0)
    float hdrNits           = 0.0f;  // HDR overlay luminance in nits (0 = auto from game HDR paper white)

    std::uint32_t  defaultVisMask      = kCatMaskCombat;
    int            defaultSbcRadiusIdx = 1;
    bool           startHideMonster    = false;
    StageHideMode  startStageHide      = StageHideMode::Off;
    BackdropMode   startBackdrop       = BackdropMode::Off;
    DepthGuideMode startDepthMode      = DepthGuideMode::Auto;
};

void InitLog();
void CloseLog();
void Log(const char* fmt, ...);
std::string DllDirectory();
Config LoadConfig();

std::uint32_t PresetMask(PresetId p) noexcept;
const char* PresetName(PresetId p) noexcept;
const char* MatchPresetName(std::uint32_t visMask) noexcept;
const char* StageHideModeName(StageHideMode m) noexcept;
const char* BackdropModeName(BackdropMode m) noexcept;
const char* DepthGuideModeName(DepthGuideMode m) noexcept;

bool Readable(std::uintptr_t addr, std::size_t len);

template <typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) {
    if (!Readable(addr, sizeof(T))) return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

} // namespace app

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

enum class SbcMeshTier : std::uint8_t {
    LowOnly  = 0, // Low-poly physics collision only (*_col.sbc — fastest, true gameplay terrain)
    Both     = 1, // Both Low-poly physics (*_col.sbc) + High-poly detail/effect (*_col_eff.sbc)
    HighOnly = 2, // High-poly detail/effect/occlude collision only (*_col_eff.sbc / mOccludeSbc)
    Count    = 3
};

enum class SpecialSbcStyle : std::uint8_t {
    Off    = 0, // Hidden (physical solid terrain only)
    Ghost  = 1, // Ultra-transparent whisper ghost (~96% transparent)
    Light  = 2, // Default: light transparent (~93% transparent — subtly see-through)
    Medium = 3, // Medium translucent (~85% transparent)
    Vivid  = 4, // Clear tinted glass (~72% transparent)
    Opaque = 5, // 100% opaque solid (0% transparent; distinct colors from normal terrain)
    Count  = 6
};

enum class EmPartMode : std::uint8_t {
    Off        = 0, // Unified monster hurtbox / pushbox coloring & visibility
    Hitzone    = 1, // Fine hitzone partition (mBaseDmgGroup / sub-meat + live physical & elemental HZV)
    ClutchZone = 2, // Iceborne Clutch Claw soften zone partition (rScarParts / cScarPartsData +0x174)
    Count      = 3
};

enum class EmUnselectedStyle : std::uint8_t {
    Hide  = 0, // Completely hide unselected monster parts (hurtboxes + bound pushboxes)
    Ghost = 1, // Draw unselected monster parts as faint ghost outlines for posture context
    Count = 2
};

enum class EmPartLabelMode : std::uint8_t {
    Full     = 0, // 2-line 3D callout: Part ID + [Extract] + Phys (S/B/A) + Elem (F/W/T/I/D)
    PhysOnly = 1, // 1-line 3D callout: Part ID + [Extract] + Phys (S/B/A)
    IdOnly   = 2, // Compact 1-line 3D badge: Part ID + [Extract]
    Off      = 3, // Color-coded 3D wireframes only (no floating text badges)
    Count    = 4
};

inline constexpr int kMaxEmParts          = 32;
inline constexpr int kSbcRadiusCount      = 4;
inline constexpr int kLineScaleCount      = 8;
inline constexpr int kDefaultLineScaleIdx = 3; // 1.00x

inline float SbcRadiusMetersFromIdx(int idx) noexcept {
    static constexpr float kRadii[kSbcRadiusCount] = {15.0f, 25.0f, 45.0f, 80.0f};
    if (idx < 0) return kRadii[0];
    if (idx >= kSbcRadiusCount) return kRadii[kSbcRadiusCount - 1];
    return kRadii[idx];
}

inline float LineScaleFromIdx(int idx) noexcept {
    static constexpr float kScales[kLineScaleCount] = {
        0.35f, 0.55f, 0.75f, 1.00f, 1.35f, 1.75f, 2.25f, 3.00f
    };
    if (idx < 0) return kScales[0];
    if (idx >= kLineScaleCount) return kScales[kLineScaleCount - 1];
    return kScales[idx];
}

inline int NearestLineScaleIdx(float scale) noexcept {
    static constexpr float kScales[kLineScaleCount] = {
        0.35f, 0.55f, 0.75f, 1.00f, 1.35f, 1.75f, 2.25f, 3.00f
    };
    int bestIdx = kDefaultLineScaleIdx;
    float bestDiff = 1e9f;
    for (int i = 0; i < kLineScaleCount; ++i) {
        const float d = (scale > kScales[i]) ? (scale - kScales[i]) : (kScales[i] - scale);
        if (d < bestDiff) {
            bestDiff = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}

struct ViewFilterState {
    std::uint32_t     visMask         = kCatMaskCombat;
    int               presetCursor    = static_cast<int>(PresetId::Combat);
    int               sbcRadiusIdx    = 1; // 0=15m, 1=25m (default), 2=45m, 3=80m
    int               entityLineIdx   = kDefaultLineScaleIdx; // Entity / monster / player line width scale
    int               sbcLineIdx      = kDefaultLineScaleIdx; // Terrain .SBC / RAW WIRE line width scale
    BackdropMode      backdrop        = BackdropMode::Off;
    StageHideMode     stageHide       = StageHideMode::Off;
    DepthGuideMode    depthMode       = DepthGuideMode::Auto;
    SbcMeshTier       sbcTier         = SbcMeshTier::Both;
    SpecialSbcStyle   specialSbc      = SpecialSbcStyle::Light;
    EmPartMode        emPartMode      = EmPartMode::Off;
    EmUnselectedStyle emUnsStyle      = EmUnselectedStyle::Hide;
    EmPartLabelMode   emLabelMode     = EmPartLabelMode::Full;
    int               emTargetScope   = 0; // 0=Auto(Solo), 1=Auto(+Others), 2..9=Lock #1..#8(Solo)
    std::uint32_t     emPartMask      = 0xFFFFFFFFu;
    bool              hideMonster     = false;
    bool              showOverlay     = true;  // [\] / $show_overlay: Toggle 2D HUD banner, freeze pill & brackets
    bool              panelOpen       = false;
    int               panelPage       = 0; // 0=Main Filter & Scene, 1=Monster Parts Inspector
    int               panelCursor     = 0; // 0..17 (Page 0)
    int               partPanelCursor = 0; // 0..4+partCount (Page 1)
};

struct ChatCommandConfig {
    bool        enabled        = true;
    bool        caseSensitive  = false;
    std::string cmdVis         = "$draw_hitbox";   // U: Toggle hitbox/scene visualization
    std::string cmdFreeze      = "$full_freeze";   // I: Full Freeze / Resume
    std::string cmdWorldFreeze = "$ThE_WoRLD!!!";  // K: World Freeze / Resume
    std::string cmdStep        = "$step_frame";    // O: Step +1 frame
    std::string cmdKeepHitbox  = "$keep_hitbox";   // J: Keep single-trigger hitboxes active
    std::string cmdPanel       = "$show_panel";    // P: Open / close Filter & Scene Panel
    std::string cmdOverlay     = "$show_overlay";  // \: Show / hide 2D HUD overlay & freeze banner
    std::string cmdFreezeBurst = "$freeze_burst";  // Toggle World Freeze Delayed Burst (damage numbers / VFX / camera cues)
};

struct Config {
    bool enabled            = true;
    int vkToggleVis         = 'U';   // 0x55: Toggle hitbox/hurtbox display
    int vkToggleFreeze      = 'I';   // 0x49: Instant zero-delay full game freeze/resume
    int vkFreezeOthers      = 'K';   // 0x4B: Pause everything except the Player (World Freeze)
    int vkPersistHitbox     = 'J';   // 0x4A: Toggle keeping single-trigger hitboxes active while frozen
    int vkStepFrame         = 'O';   // 0x4F: Advance 1 frame while frozen (Full or World Freeze)
    int vkCycleFilter       = 'P';   // 0x50: Open/close interactive Filter & Scene Panel (Shift+P cycles preset)
    int vkToggleOverlay     = 0xDC;  // VK_OEM_5 ('\'): Show/hide 2D HUD overlay for clean screenshots

    bool startVisOn         = false; // Whether hitbox display starts ON automatically
    bool keepHitboxesActive = true;  // Keep single-trigger frozen hitboxes active & repeatable in World Freeze
    bool worldFreezeDelayedBurst = true; // Defer damage numbers / hit VFX / camera cues during World Freeze, burst on resume
    bool blockCameraKeys    = true;  // Block O/K/L/; camera rotation keys while mod is loaded (except in chat)
    bool gamepadPanel       = true;  // Controller D-Pad + A navigation when [P] panel is open
    ChatCommandConfig chat{};        // In-game chat exact-match triggers (configurable & hot-reloadable via .ini)
    bool showHudBanner      = true;  // Show compact top-left status indicator
    float lineThickness     = 2.0f;  // Base wireframe thickness in pixels (at 1080p)
    float opacityScale      = 1.0f;  // Global opacity multiplier (0.1 .. 1.5)
    float sbcSurfaceOpacity = 1.0f;  // .sbc semi-transparent face opacity multiplier (0.1 .. 3.0)
    float hdrNits           = 0.0f;  // HDR overlay luminance in nits (0 = auto from game HDR paper white)

    std::uint32_t     defaultVisMask       = kCatMaskCombat;
    int               defaultSbcRadiusIdx  = 1;
    int               defaultEntityLineIdx = kDefaultLineScaleIdx;
    int               defaultSbcLineIdx    = kDefaultLineScaleIdx;
    bool              startHideMonster     = false;
    StageHideMode     startStageHide       = StageHideMode::Off;
    BackdropMode      startBackdrop        = BackdropMode::Off;
    DepthGuideMode    startDepthMode       = DepthGuideMode::Auto;
    SbcMeshTier       startSbcTier         = SbcMeshTier::Both;
    SpecialSbcStyle   startSpecialSbc      = SpecialSbcStyle::Light;
    EmPartMode        startEmPartMode      = EmPartMode::Off;
    EmUnselectedStyle startEmUnsStyle      = EmUnselectedStyle::Hide;
    EmPartLabelMode   startEmLabelMode     = EmPartLabelMode::Full;
};

void InitLog();
void CloseLog();
void Log(const char* fmt, ...);
std::string DllDirectory();
Config LoadConfig();
// Checks mhw_hfi.ini mtime/size (zero polling; called only on chat send / panel open)
// and hot-reloads ChatCommands, ChatCaseSensitive, ChatCmd*, and GamepadPanel when changed.
bool HotReloadRuntimeConfig(ChatCommandConfig& ioChat, bool& ioGamepadPanel);

std::uint32_t PresetMask(PresetId p) noexcept;
const char* PresetName(PresetId p) noexcept;
const char* MatchPresetName(std::uint32_t visMask) noexcept;
const char* StageHideModeName(StageHideMode m) noexcept;
const char* BackdropModeName(BackdropMode m) noexcept;
const char* DepthGuideModeName(DepthGuideMode m) noexcept;
const char* SbcMeshTierName(SbcMeshTier t) noexcept;
const char* SpecialSbcStyleName(SpecialSbcStyle s) noexcept;
const char* EmPartModeName(EmPartMode m) noexcept;
const char* EmUnselectedStyleName(EmUnselectedStyle s) noexcept;
const char* EmPartLabelModeName(EmPartLabelMode l) noexcept;

bool Readable(std::uintptr_t addr, std::size_t len);

template <typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) {
    if (!Readable(addr, sizeof(T))) return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

} // namespace app

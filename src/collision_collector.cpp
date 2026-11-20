#include "collision_collector.h"
#include "mhw_addresses.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace coll {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Standard DXGI_FORMAT values used for HDR/SDR SwapChain detection
constexpr std::uint32_t kDxgiFmtR16G16B16A16_Typeless = 9;
constexpr std::uint32_t kDxgiFmtR16G16B16A16_Float    = 10;
constexpr std::uint32_t kDxgiFmtR10G10B10A2_Typeless  = 23;
constexpr std::uint32_t kDxgiFmtR10G10B10A2_Unorm     = 24;

enum class OwnerSide : std::uint8_t {
    Unknown = 0,
    Player  = 1,
    Otomo   = 2,
    Monster = 3,
    Env     = 4
};

enum class PlayerDefState : std::uint8_t {
    Normal     = 0,
    SuperArmor = 1, // Knockback/flinch immunity frames (Flinch Free / motion Super Armor)
    Guard      = 2, // Standard shield Guard
    Counter    = 3, // Counter / Foresight / Temporal Mantle auto-evade trigger window
    HyperArmor = 4, // Tackle / ZSD / Iai Spirit / Rocksteady Mantle damage+knockback armor
    GuardPoint = 5, // Active Guard Point (CB / Lance / SnS / HBG)
    Invincible = 6  // True i-frames (Evade roll/hop, Dive, Getup, Foresight i-frames, NoHit)
};

// Ordered strictly from back-to-front draw priority so Attack Hitboxes always render on top!
enum class VolumeCat : std::uint8_t {
    EnvPush        = 0,
    Pushbox        = 1,
    EnvHurtbox     = 2,
    OtomoHurtbox   = 3,
    MonsterHurtbox = 4,
    PlayerHurtbox  = 5,
    EnvHitbox      = 6,
    OtomoHitbox    = 7,
    PlayerHitbox   = 8,
    MonsterHitbox  = 9,
    Count          = 10
};

inline std::uint32_t CategoryBitForVolume(VolumeCat cat) noexcept {
    switch (cat) {
        case VolumeCat::PlayerHurtbox:  return app::kCatPlHurtbox;
        case VolumeCat::PlayerHitbox:   return app::kCatPlHitbox;
        case VolumeCat::OtomoHurtbox:
        case VolumeCat::OtomoHitbox:    return app::kCatOtomo;
        case VolumeCat::MonsterHurtbox: return app::kCatEmHurtbox;
        case VolumeCat::MonsterHitbox:  return app::kCatEmHitbox;
        case VolumeCat::Pushbox:        return app::kCatPushbox;
        case VolumeCat::EnvPush:
        case VolumeCat::EnvHurtbox:
        case VolumeCat::EnvHitbox:      return app::kCatEnvObject;
        default:                        return 0;
    }
}

constexpr std::uint8_t kEmPartNone      = 0xFFU;
constexpr std::uint8_t kEmFlagGhost     = 0x01U;

struct ShapeRecord {
    VolumeCat cat;
    PlayerDefState plDef;
    std::uint8_t emPartIdx; // kEmPartNone (0xFF) = unified color, 0..31 = monster part palette slot
    std::uint8_t emFlags;   // kEmFlagGhost = faint unselected monster part outline
    std::int32_t type;
    float p[20];
};

// .SBC semantic collision layer classification (verified @ 0x1425d2620 & sMhCollision::checkAttr 0x141ac60c0)
constexpr std::uint8_t kSbcLayerSolid   = 0; // Normal physical solid terrain (ground/wall/ceiling, incl. 0x3FFFFFFF & 0x0)
constexpr std::uint8_t kSbcLayerBarrier = 1; // Player pass-through / monster-leash / camera bumper (attrFlags & 1 != 0)
constexpr std::uint8_t kSbcLayerWater   = 2; // Special water / liquid detection plane (attrFlags & 0x00800000 != 0)
constexpr std::uint8_t kSbcLayerSpecial = 3; // Special non-solid probe sheet (camera/sound/IK: attrFlags & 0x3E000400 != 0)
constexpr std::uint8_t kSbcLayerAuxSbc  = 4; // Non-physical .sbc slot (mOccludeSbc = 0x400 / non-entity group)

inline std::uint8_t ClassifySbcLayer(bool isAuxSbc, std::uint32_t attrFlags) noexcept {
    if (isAuxSbc) {
        return kSbcLayerAuxSbc;
    }
    // Verified @ sMhCollision::checkAttr (0x141ac60c0):
    //   - 0x3FFFFFFF is MHW's DEFAULT SOLID TERRAIN attribute (collides with Player, Monster, Shell,
    //     Camera, and 0x3FFFFFFF; ignored ONLY by pure water/sound/special probes!).
    //   - 0x00000000 is also standard solid terrain (collides with all normal entities).
    if (attrFlags == 0x3FFFFFFFU || attrFlags == 0x00000000U) {
        return kSbcLayerSolid;
    }
    // Special non-solid probe layer bits (+0x4C: test eax, 0x3E800400)
    if ((attrFlags & 0x3E800400U) != 0U) {
        return ((attrFlags & 0x00800000U) != 0U) ? kSbcLayerWater : kSbcLayerSpecial;
    }
    // Exclusion bitmask (+0x82: test edx, eax; je collide): bit 0 set => Player passes through
    if ((attrFlags & 0x00000001U) != 0U) {
        return kSbcLayerBarrier;
    }
    return kSbcLayerSolid;
}

// Verified @ FUN_140249070 (0x140249070), FUN_140577d20 (0x140578216), FUN_1425D2620:
//   - sbcType == 0x400 / (sbcType & 1) == 0 / (sbcGroup & 1) == 0 : High-poly *_col_eff.sbc / mOccludeSbc
//   - rColl + 0x0C (cResource::mPath) contains "_eff"             : High-poly *_col_eff.sbc
//   - [rbx+4] == 4 stage detail SBC: all AttrInfo have 0x08000000 : High-detail VFX/IK/probe collision
inline bool IsHighCollisionSbc(std::uint32_t sbcType,
                               std::uint32_t sbcGroup,
                               std::uintptr_t rColl,
                               std::uintptr_t attrsBase,
                               std::uint16_t numAttrs) noexcept {
    if ((sbcType & 1U) == 0U || (sbcGroup & 1U) == 0U) {
        return true;
    }
    const auto* path = reinterpret_cast<const char*>(rColl + mhw::kOffRCollPath);
    for (int i = 0; i < 60 && path[i] != '\0'; ++i) {
        if (path[i] == '_' && path[i + 1] == 'e' && path[i + 2] == 'f' && path[i + 3] == 'f') {
            return true;
        }
    }
    if (attrsBase != 0U && numAttrs > 0U) {
        const std::uint32_t firstF = *reinterpret_cast<const std::uint32_t*>(attrsBase);
        if (firstF == 0x08000000U) {
            const std::uint32_t lastF = *reinterpret_cast<const std::uint32_t*>(
                attrsBase + static_cast<std::size_t>(numAttrs - 1U) * mhw::kRCollAttrStride);
            if (lastF == 0x08000000U) return true;
        }
    }
    return false;
}

struct SbcTriRecord {
    float x0, y0, z0;
    float x1, y1, z1;
    float x2, y2, z2;
    float fade;              // Smooth radial + vertical distance fade [0.0..1.0]
    std::int8_t  nx, ny, nz; // Quantized unit normal [-127..127] (view-independent, cached across frames!)
    std::uint8_t elevBand;   // 1 if gentle walkable slope sits on a 2m elevation contour band
    std::uint8_t layer;      // kSbcLayerSolid (0) or auxiliary/non-solid/high layer (1..4)
    std::uint8_t pad0;
    std::uint16_t pad1;
};

struct SbcMeshCache {
    std::uintptr_t   sColl       = 0;
    std::uint32_t    sbcCount    = 0;
    std::uint64_t    fingerprint = 0;
    float            radius      = 0.0f;
    float            center[3]{};
    float            prevCenter[3]{};
    float            camPos[3]{};
    std::uint32_t    triCount    = 0;
    std::uint32_t    edgeCount   = 0;
    std::uint32_t    ageFrames   = 0;
    app::SbcMeshTier sbcTier     = app::SbcMeshTier::Both;
    bool             includeAux  = true;
    bool             hasPrevCtr  = false;
    bool             valid       = false;
};

struct SbcEdgeRecord {
    float x0, y0, z0;
    float x1, y1, z1;
    float nx0, ny0, nz0; // First adjacent face unit normal (for crease-angle filtering)
    float fade;          // Max distance fade of adjacent faces [0.0..1.0]
    std::uint8_t isWall; // 0 = walkable floor/slope, 1 = wall/cliff/ledge
    std::uint8_t count;  // Number of triangles sharing this edge (1 = open boundary, 2+ = interior)
    std::uint8_t crease; // 0 = flat coplanar (<7 deg), 1 = soft slope (7..24 deg), 2 = sharp ledge/corner (>24 deg)
    std::uint8_t layer;  // kSbcLayerSolid (0) or auxiliary/non-solid/high layer (1..4)
};

// Compact 8-byte hash slot -> 65536 slots = 512 KB total (L2-cache resident, 8x smaller than 4.19 MB!)
struct SbcEdgeSlot {
    std::uint32_t key32;
    std::uint32_t idxAndGen; // low 20 bits = edgeIdx (0..1048575), high 12 bits = gen (1..4095)
};

struct SbcPartCand {
    const float*   activeMat;
    std::uintptr_t triNodesBase;
    std::uintptr_t vertArrBase;
    std::uintptr_t attrsBase;
    std::uint32_t  totalVerts;
    std::uint32_t  startTri;
    std::uint32_t  endTri;
    std::uint32_t  startVert;
    std::uint16_t  numAttrs;
    std::uint8_t   isHighSbc; // 0 = Low-poly physics (*_col.sbc), 1 = High-poly detail/effect (*_col_eff.sbc)
    float          distSq;
};

constexpr std::uint32_t kMaxShapes       = 3072;
constexpr std::uint32_t kMaxSbcTris      = 98304;
constexpr std::uint32_t kMaxSbcEdges     = 49152;
constexpr std::uint32_t kEdgeHashSize    = 65536; // Power of 2 (512 KB -> fits in L2 cache!)
constexpr std::uint32_t kMaxSbcPartCands = 4096;

template <std::uint32_t N>
struct PtrSet {
    std::uintptr_t keys[N];
    void clear() { std::memset(keys, 0, sizeof(keys)); }
    bool insert(std::uintptr_t k) {
        if (!k) return false;
        std::uint32_t idx = static_cast<std::uint32_t>((k >> 4) * 2654435761ULL) & (N - 1);
        for (std::uint32_t n = 0; n < 32; ++n) {
            const std::uintptr_t cur = keys[idx];
            if (cur == k) return false;
            if (cur == 0) {
                keys[idx] = k;
                return true;
            }
            idx = (idx + 1) & (N - 1);
        }
        return false;
    }
};

struct DtiCacheEntry {
    std::uintptr_t dti;
    OwnerSide side;
    bool skipUnit;
    std::uint16_t directCollOffset;
};

constexpr std::uint32_t kDtiCacheSize = 4096;

// Persistent POD caches (only accessed on the render/present thread)
DtiCacheEntry g_dtiCache[kDtiCacheSize]{};
PtrSet<4096>  g_seenColls;
PtrSet<8192>  g_seenGeoms;
ShapeRecord   g_shapes[kMaxShapes];
SbcTriRecord  g_sbcTris[kMaxSbcTris];
std::uint8_t  g_sbcTriBucket[kMaxSbcTris];
std::uint32_t g_sbcTriOrder[kMaxSbcTris];
std::uint32_t g_sbcAuxOrder[kMaxSbcTris];
SbcEdgeRecord g_sbcEdges[kMaxSbcEdges];
SbcEdgeSlot   g_sbcEdgeHash[kEdgeHashSize];
SbcPartCand   g_sbcPartCands[kMaxSbcPartCands];
SbcMeshCache  g_sbcCache{};
std::uint32_t g_sbcEdgeGen = 0;

struct EmPartSlotInfo {
    std::int32_t  sortKey;       // Deterministic ordering key across frames
    std::int16_t  baseGrp;       // mBaseDmgGroup (-1 if unmapped)
    std::int16_t  subGrp;        // Sub-part index (>0 when split sub-meat exists)
    std::int16_t  zoneId;        // Clutch Claw soften zone or mRideParts (0..4)
    std::int16_t  slash;         // Live Slash hitzone (-1 if N/A)
    std::int16_t  blunt;         // Live Blunt hitzone (-1 if N/A)
    std::int16_t  shot;          // Live Shot hitzone (-1 if N/A)
    std::int16_t  fire;          // Live Fire hitzone (-1 if N/A)
    std::int16_t  water;         // Live Water hitzone (-1 if N/A)
    std::int16_t  thunder;       // Live Thunder hitzone (-1 if N/A)
    std::int16_t  ice;           // Live Ice hitzone (-1 if N/A)
    std::int16_t  dragon;        // Live Dragon hitzone (-1 if N/A)
    std::int8_t   extract;       // 0=RED, 1=WHT, 2=ORG, 3=GRN, -1=none
    bool          tenderized;    // Active Iceborne Clutch Claw wound on this zone
    bool          changedState;  // mBaseDmgGroupForChanged active
    bool          ignoreHit;     // mEmAttr & 0x80
    bool          rideFallback;  // Coarse mode used mRideParts instead of Clutch Claw zones
    bool          isNoClaw;      // Unified unclutchable bucket in CLUTCH mode (e.g. Fatalis tail)
    bool          isAuxCenter;   // Monster center helper sphere (cat == MonsterHurtbox, baseGrp < 0)
    std::uint8_t  colorIdx;      // 0..15 palette, 16=NO-CLAW muted slate, 17=AUX CENTER ice-white
    std::uint16_t hurtCount;     // Active hurtbox volumes in this part
    std::uint16_t pushCount;     // Active pushbox volumes bound to this part
    float         anchorSum[3];  // Weighted world centroid sum
    float         anchorWeight;
    char          shortTag[16];  // Compact 3D tag, e.g. "P00", "P00.1", "Z0*", "NO-CLAW", "CTR"
    char          rowLabel[36];  // Panel ID label, e.g. "P00*", "CLAW Z0*", "NO-CLAW", "AUX CTR"
};

struct EmInspectorSnapshot {
    bool          hasTarget    = false;
    bool          targetLarge  = false;
    std::uint32_t monsterCount = 0;
    std::uint32_t targetSlot   = 0; // 0-based index among active monsters
    std::uint32_t emId         = 0;
    std::uint32_t subId        = 0;
    std::uint32_t partCount    = 0;
    char          targetName[32]{};
    EmPartSlotInfo parts[app::kMaxEmParts]{};
};

struct EmRawGeomCand {
    std::uintptr_t activeGeom;
    std::uintptr_t primGeom;
    std::uintptr_t sweptGeom;
    VolumeCat      cat;
    bool           nodeActive;
    bool           geomActive;
    std::int16_t   jnt0;
    std::int16_t   jnt1;
    std::int16_t   baseGrp;
    std::int16_t   partsGrp;
    std::int16_t   subGrp;
    std::int16_t   clawZone;
    std::int16_t   rideZone;
    std::int16_t   slash;
    std::int16_t   blunt;
    std::int16_t   shot;
    std::int16_t   fire;
    std::int16_t   water;
    std::int16_t   thunder;
    std::int16_t   ice;
    std::int16_t   dragon;
    std::int8_t    extract;
    bool           tenderized;
    bool           changedState;
    bool           ignoreHit;
    std::int16_t   partSlot;
    float          segA[3];
    float          segB[3];
    float          radius;
};

constexpr std::uint32_t kMaxEmRawGeoms = 512;
EmRawGeomCand       g_emRawGeoms[kMaxEmRawGeoms]{};
EmInspectorSnapshot g_emInspector{};
EmInspectorCounts   g_lastEmCounts{};

struct PlayerFrameReadout {
    bool valid = false;
    PlayerDefState state = PlayerDefState::Normal;
    int framesRemaining = 0;  // >0 when frame countdown is known
    int secondsRemaining = 0; // >0 when mantle second countdown is known
    char badge[32]{};
};

struct RawSnapshot {
    bool hasCamera;
    float view[16];
    float proj[16];
    float camPos[3];
    bool hasPlayerGround;
    float plFeetPos[3];
    float plForward[3];
    std::uint32_t shapeCount;
    std::uint32_t sbcTriCount;
    std::uint32_t sbcEdgeCount;
    bool hdrActive;
    float hdrWhiteNits;
    PlayerFrameReadout mainPlDef;
};

inline bool IsCanonicalPtr(std::uintptr_t p) {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL;
}

inline bool IsAlignedPtr(std::uintptr_t p) {
    return IsCanonicalPtr(p) && ((p & 0x7ULL) == 0);
}

inline bool IsFiniteCoord(float v) {
    return std::isfinite(v) && v > -500000.0f && v < 500000.0f;
}

inline bool IsValidVec3(const float* v) {
    return IsFiniteCoord(v[0]) && IsFiniteCoord(v[1]) && IsFiniteCoord(v[2]);
}

inline bool StartsWith(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

OwnerSide ClassifyClassName(const char* name) {
    if (!name || !IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name))) return OwnerSide::Unknown;
    const unsigned char c0 = static_cast<unsigned char>(name[0]);
    if (c0 < 0x20 || c0 > 0x7E) return OwnerSide::Unknown;

    // 1. Palico / Otomo / Companion (checked before Player so uOtomo* stays distinct)
    if (StartsWith(name, "uOtomo") || StartsWith(name, "uOt") ||
        StartsWith(name, "uAIOt") || StartsWith(name, "uPhotomo") ||
        StartsWith(name, "uCat") || StartsWith(name, "uMhActorOtomo") ||
        StartsWith(name, "uShellOtomo") || StartsWith(name, "uShellOtasuke") ||
        StartsWith(name, "uPugee") || StartsWith(name, "cAttackParamOt")) {
        return OwnerSide::Otomo;
    }

    // 2. Monster (checked before Player Slinger so uShellSlingerEmBlowCombo is Monster)
    if (StartsWith(name, "Monster") || StartsWith(name, "uEnemy") ||
        StartsWith(name, "uCaptureEnemy") ||
        StartsWith(name, "uEm0") || StartsWith(name, "uEm1") || StartsWith(name, "uEms") ||
        StartsWith(name, "uEmEmbankment") || StartsWith(name, "uEmFoodModel") ||
        StartsWith(name, "uEmClawGrab") || StartsWith(name, "cEm") ||
        StartsWith(name, "uAiEm") || StartsWith(name, "uAIEm") || StartsWith(name, "uAIEms") ||
        StartsWith(name, "uMhActorModelEnemy") ||
        StartsWith(name, "uShellEm") || StartsWith(name, "uShellBoundEm") ||
        StartsWith(name, "uShellLaserEm") || StartsWith(name, "uShellLaserMarkEm") ||
        StartsWith(name, "uShellTestEm") || StartsWith(name, "uShellSlingerEmBlowCombo") ||
        StartsWith(name, "cAttackParamEm") || StartsWith(name, "cCollNodeEm")) {
        return OwnerSide::Monster;
    }

    // 3. Player / Weapons / Kinsect / Player Shells / Claw
    if (StartsWith(name, "uPlayer") || StartsWith(name, "uPl") ||
        StartsWith(name, "uWeapon") || StartsWith(name, "uWp") ||
        StartsWith(name, "uShellPl") || StartsWith(name, "uShellAskill") ||
        StartsWith(name, "uShellClaw") || StartsWith(name, "uShellWp") ||
        StartsWith(name, "uShellHitWp") || StartsWith(name, "uShellMushiAmi") ||
        StartsWith(name, "uInsect") || StartsWith(name, "uFT031Insect") ||
        StartsWith(name, "uHumanClaw") || StartsWith(name, "uHumanDummyClaw") ||
        StartsWith(name, "uClaw") || StartsWith(name, "uMhActorPl") ||
        StartsWith(name, "cAttackParamPl") || StartsWith(name, "cCollNodePl")) {
        return OwnerSide::Player;
    }

    // 4. Map Interactive Entities / Gimmicks / Endemic Life / Stage Hazards / NPCs
    if (StartsWith(name, "uGm") || StartsWith(name, "uGimmick") ||
        StartsWith(name, "uStage") || StartsWith(name, "uHmGimmick") ||
        StartsWith(name, "uBg") || StartsWith(name, "uEnv") ||
        StartsWith(name, "uAnimal") || StartsWith(name, "uCageInsect") ||
        StartsWith(name, "uGuideInsect") || StartsWith(name, "uFish") ||
        StartsWith(name, "uFurniture") || StartsWith(name, "uInteractFurniture") ||
        StartsWith(name, "uNpc") || StartsWith(name, "uScholar") ||
        StartsWith(name, "uScrNpc") || StartsWith(name, "uWorker") ||
        StartsWith(name, "uDynamicSbc") || StartsWith(name, "uScrollCollision") ||
        StartsWith(name, "uSlantFloor") || StartsWith(name, "uIvy") ||
        StartsWith(name, "uPutFootRope") || StartsWith(name, "uRelayRope") ||
        StartsWith(name, "uRope") || StartsWith(name, "uBallistaRestraintRope") ||
        StartsWith(name, "uSnowField") || StartsWith(name, "uSnowShoveler") ||
        StartsWith(name, "uShellGm") || StartsWith(name, "uShellLaserGm") ||
        StartsWith(name, "uShellStage") || StartsWith(name, "uShellBallista") ||
        StartsWith(name, "uShellDragonCannon") || StartsWith(name, "uShellOnsenRock") ||
        StartsWith(name, "uShellBreakable") || StartsWith(name, "uShellAirIvy") ||
        StartsWith(name, "uShellFallIvy") || StartsWith(name, "uShellGroundIvy") ||
        StartsWith(name, "uShellPlant") || StartsWith(name, "uShellHitLightPlant") ||
        StartsWith(name, "uShellGrassWind") || StartsWith(name, "uShellNut")) {
        return OwnerSide::Env;
    }

    return OwnerSide::Unknown;
}

std::uintptr_t GetObjectDti(std::uintptr_t obj, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(obj)) return 0;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(obj);
    if (vft < exeBase || vft + 0x28 > exeEnd || (vft & 0x7ULL) != 0) return 0;
    const std::uintptr_t fnGetDti = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffVftGetDti);
    if (fnGetDti < exeBase || fnGetDti + 8 > exeEnd) return 0;
    // Every MtObject::getDti in MHW is `48 8D 05 xx xx xx xx C3` (lea rax, [rip+disp32]; ret)
    const auto* code = reinterpret_cast<const std::uint8_t*>(fnGetDti);
    if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05 && code[7] == 0xC3) {
        std::int32_t disp = 0;
        std::memcpy(&disp, code + 3, 4);
        const std::uintptr_t dti = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(fnGetDti + 7) + disp);
        if (dti >= exeBase && dti + 0x38 <= exeEnd) return dti;
    }
    return 0;
}

inline bool IsCpObjCollision(std::uintptr_t cand, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(cand)) return false;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(cand);
    if (vft < exeBase || vft + 0x28 > exeEnd || (vft & 0x7ULL) != 0) return false;
    const std::uintptr_t fnGetDti = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffVftGetDti);
    return fnGetDti == (exeBase + mhw::kRvaCpObjCollisionGetDti);
}

bool IsNonCollisionSystemClass(const char* name) {
    if (!name || !IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name))) return false;
    return StartsWith(name, "uMhGUI") || StartsWith(name, "uGUI") ||
           StartsWith(name, "uSound") || StartsWith(name, "uSnd") ||
           StartsWith(name, "uMhCamera") || StartsWith(name, "uCamera") ||
           StartsWith(name, "uLight") || StartsWith(name, "uCns") ||
           StartsWith(name, "uSky") || StartsWith(name, "uWeather") ||
           StartsWith(name, "uFog") || StartsWith(name, "uPost");
}

std::uint16_t ResolveKnownDirectCollOffset(const char* name) {
    if (!name || !IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name))) return 0;
    if (StartsWith(name, "uWeapon") || StartsWith(name, "uWp")) {
        return static_cast<std::uint16_t>(mhw::kOffUWpColl);
    }
    if (StartsWith(name, "uShell")) {
        return static_cast<std::uint16_t>(mhw::kOffUShellColl);
    }
    if (StartsWith(name, "uStageGimmick") || StartsWith(name, "uHmGimmick") || StartsWith(name, "uGm")) {
        return static_cast<std::uint16_t>(mhw::kOffUGimmickColl);
    }
    if (StartsWith(name, "uCharacterModel") || StartsWith(name, "uPlayer") ||
        StartsWith(name, "uOtomo") || StartsWith(name, "uEnemy") || StartsWith(name, "Monster")) {
        return static_cast<std::uint16_t>(mhw::kOffUPlayerColl);
    }
    return 0;
}

DtiCacheEntry* LookupOrCreateDtiEntry(std::uintptr_t dti, std::uintptr_t exeBase) {
    if (!dti) return nullptr;
    const std::uint32_t startIdx = static_cast<std::uint32_t>((dti >> 4) * 2654435761ULL) & (kDtiCacheSize - 1);
    std::uint32_t idx = startIdx;
    for (std::uint32_t i = 0; i < 24; ++i) {
        if (g_dtiCache[idx].dti == dti) return &g_dtiCache[idx];
        if (g_dtiCache[idx].dti == 0 || i == 23) {
            DtiCacheEntry& e = g_dtiCache[idx];
            e.dti = dti;
            e.side = OwnerSide::Unknown;
            e.skipUnit = false;
            e.directCollOffset = 0;

            std::uintptr_t cur = dti;
            for (int depth = 0; depth < 20 && IsAlignedPtr(cur); ++depth) {
                if (exeBase) {
                    if (cur == exeBase + mhw::kRvaDtiAttackParamEm ||
                        cur == exeBase + mhw::kRvaDtiCollNodeEm) {
                        e.side = OwnerSide::Monster;
                        break;
                    }
                    if (cur == exeBase + mhw::kRvaDtiAttackParamOt) {
                        e.side = OwnerSide::Otomo;
                        break;
                    }
                    if (cur == exeBase + mhw::kRvaDtiAttackParamPl ||
                        cur == exeBase + mhw::kRvaDtiAttackParamPlShell ||
                        cur == exeBase + mhw::kRvaDtiAttackParamPlBase ||
                        cur == exeBase + mhw::kRvaDtiCollNodePl) {
                        e.side = OwnerSide::Player;
                        break;
                    }
                }
                const char* clsName = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
                if (IsNonCollisionSystemClass(clsName)) {
                    e.skipUnit = true;
                    break;
                }
                if (e.directCollOffset == 0) {
                    e.directCollOffset = ResolveKnownDirectCollOffset(clsName);
                }
                if (e.side == OwnerSide::Unknown) {
                    const OwnerSide s = ClassifyClassName(clsName);
                    if (s != OwnerSide::Unknown) {
                        e.side = s;
                    }
                }
                if (e.side != OwnerSide::Unknown && e.directCollOffset != 0) break;
                cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
            }
            return &e;
        }
        idx = (idx + 1) & (kDtiCacheSize - 1);
    }
    return nullptr;
}

OwnerSide ResolveObjectDtiSide(std::uintptr_t obj, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(obj)) return OwnerSide::Unknown;
    const std::uintptr_t dti = GetObjectDti(obj, exeBase, exeEnd);
    DtiCacheEntry* entry = LookupOrCreateDtiEntry(dti, exeBase);
    return entry ? entry->side : OwnerSide::Unknown;
}

OwnerSide ResolveUnitSide(std::uintptr_t unit, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(unit)) return OwnerSide::Unknown;
    const std::uintptr_t dti = GetObjectDti(unit, exeBase, exeEnd);
    DtiCacheEntry* entry = LookupOrCreateDtiEntry(dti, exeBase);
    if (entry && entry->side != OwnerSide::Unknown) return entry->side;

    // For generic uShell instances, check shell creator at +0xB40 and +0x3C0
    std::uintptr_t cur = dti;
    for (int depth = 0; depth < 14 && IsAlignedPtr(cur); ++depth) {
        const char* clsName = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
        if (clsName && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(clsName)) &&
            StartsWith(clsName, "uShell")) {
            std::uintptr_t creator = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffUShellCreator);
            if (!IsAlignedPtr(creator) || creator == unit) {
                creator = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffUShellCreatorLegacy);
            }
            if (IsAlignedPtr(creator) && creator != unit) {
                const OwnerSide cs = ResolveObjectDtiSide(creator, exeBase, exeEnd);
                if (cs != OwnerSide::Unknown) return cs;
            }
            break;
        }
        cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
    }

    // Check player unit-group bit (0x200000000000ULL)
    const std::uint64_t unitGroup = *reinterpret_cast<const std::uint64_t*>(unit + mhw::kOffUnitGroup);
    if ((unitGroup & 0x200000000000ULL) != 0) return OwnerSide::Player;

    return OwnerSide::Unknown;
}

bool IsUPlayerUnit(std::uintptr_t unit, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    std::uintptr_t cur = GetObjectDti(unit, exeBase, exeEnd);
    for (int depth = 0; depth < 8 && IsAlignedPtr(cur); ++depth) {
        const char* name = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
        if (name && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name)) && StartsWith(name, "uPlayer")) {
            return true;
        }
        cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
    }
    return false;
}

PlayerFrameReadout EvaluatePlayerDefense(std::uintptr_t pl, std::uintptr_t coll) {
    PlayerFrameReadout out{};
    if (!IsAlignedPtr(pl)) return out;
    out.valid = true;

    const std::uint32_t defFlags   = *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlDefFlags) |
                                     *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlDefFlagsLatch);
    const std::uint32_t guardFlags = *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlGuardFlags) |
                                     *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlGuardFlagsLatch);
    const std::uint32_t guardRaw   = *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlGuardFlags);
    const std::uint32_t cntFlags   = *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlCounterFlags) |
                                     *reinterpret_cast<const std::uint32_t*>(pl + mhw::kOffPlCounterFlagsLatch);

    float evadeFrames = *reinterpret_cast<const float*>(pl + mhw::kOffPlEvadeIframeFrames);
    float noHitSec    = *reinterpret_cast<const float*>(pl + mhw::kOffPlNoHitSec);
    float hyperSec    = *reinterpret_cast<const float*>(pl + mhw::kOffPlHyperArmorSec);
    float superSec    = *reinterpret_cast<const float*>(pl + mhw::kOffPlSuperArmorSec);
    if (!std::isfinite(evadeFrames) || evadeFrames > 600.0f) evadeFrames = 0.0f;
    if (!std::isfinite(noHitSec)    || noHitSec > 600.0f)    noHitSec    = 0.0f;
    if (!std::isfinite(hyperSec)    || hyperSec > 600.0f)    hyperSec    = 0.0f;
    if (!std::isfinite(superSec)    || superSec > 600.0f)    superSec    = 0.0f;

    bool motionNoHit = false;
    const std::uintptr_t motCommon = *reinterpret_cast<const std::uintptr_t*>(pl + mhw::kOffPlMotionCommon);
    if (IsAlignedPtr(motCommon)) {
        const std::uint32_t mflg0 = *reinterpret_cast<const std::uint32_t*>(motCommon + 0x08);
        motionNoHit = ((mflg0 >> 26) & 1U) != 0;
    }

    bool collNoHit = false;
    if (IsAlignedPtr(coll)) {
        const std::uint32_t cState = *reinterpret_cast<const std::uint32_t*>(coll + mhw::kOffObjCollStateFlags);
        collNoHit = (cState & 0x02U) != 0;
    }

    // Active mantle check (Rocksteady Mantle = ID 3)
    int activeMantleId = -1;
    float mantleTimerSec = 0.0f;
    std::uintptr_t mantleCtrl = *reinterpret_cast<const std::uintptr_t*>(pl + mhw::kOffPlMantleCtrl);
    if (!IsAlignedPtr(mantleCtrl)) {
        mantleCtrl = *reinterpret_cast<const std::uintptr_t*>(pl + mhw::kOffPlPadCtrl);
    }
    if (IsAlignedPtr(mantleCtrl)) {
        const std::int32_t mid = *reinterpret_cast<const std::int32_t*>(mantleCtrl + mhw::kOffMantleActiveId);
        if (mid >= 0 && mid < 32) {
            const float mt = *reinterpret_cast<const float*>(
                mantleCtrl + mhw::kOffMantleTimers + static_cast<std::size_t>(mid) * 4);
            if (std::isfinite(mt) && mt > 0.0f && mt < 1000.0f) {
                activeMantleId = mid;
                mantleTimerSec = mt;
            }
        }
    }

    const bool hasCounterBit = (cntFlags & 0x80000000U) != 0;
    const bool isInvincible  = ((defFlags & 0x18000000U) != 0) ||
                               (evadeFrames > 0.01f) || (noHitSec > 0.001f) ||
                               motionNoHit || collNoHit;
    const bool guardAllowed  = (guardRaw & 0x04U) == 0;
    const bool isGuardPoint  = guardAllowed && ((guardFlags & 0x10U) != 0);
    const bool isGuard       = guardAllowed && ((guardFlags & 0x08U) != 0);
    const bool isRocksteady  = (activeMantleId == 3 && mantleTimerSec > 0.0f);
    const bool isHyperArmor  = ((defFlags & 0x01800000U) != 0) || (hyperSec > 0.001f) || isRocksteady;
    const bool isSuperArmor  = ((defFlags & 0x00400000U) != 0) || (superSec > 0.001f);

    auto ceilFrames = [](float f) -> int {
        const int v = static_cast<int>(std::ceil(f - 0.001f));
        return (v < 1) ? 1 : ((v > 9999) ? 9999 : v);
    };

    if (isInvincible) {
        out.state = PlayerDefState::Invincible;
        if (evadeFrames > 0.01f) {
            out.framesRemaining = ceilFrames(evadeFrames);
            std::snprintf(out.badge, sizeof(out.badge),
                          hasCounterBit ? "IFRAME+CNT %dF" : "IFRAME %dF", out.framesRemaining);
        } else if (noHitSec > 0.001f) {
            out.framesRemaining = ceilFrames(noHitSec * 60.0f);
            std::snprintf(out.badge, sizeof(out.badge), "NOHIT %dF", out.framesRemaining);
        } else {
            std::snprintf(out.badge, sizeof(out.badge), hasCounterBit ? "IFRAME+CNT" : "IFRAME");
        }
        return out;
    }

    if (isGuardPoint) {
        out.state = PlayerDefState::GuardPoint;
        std::snprintf(out.badge, sizeof(out.badge), hasCounterBit ? "GP+COUNTER" : "GUARD POINT");
        return out;
    }

    if (isHyperArmor) {
        out.state = PlayerDefState::HyperArmor;
        if (hyperSec > 0.001f) {
            out.framesRemaining = ceilFrames(hyperSec * 60.0f);
            std::snprintf(out.badge, sizeof(out.badge), "HYPER %dF", out.framesRemaining);
        } else if (isRocksteady) {
            out.secondsRemaining = ceilFrames(mantleTimerSec);
            std::snprintf(out.badge, sizeof(out.badge), "ROCKSTEADY %dS", out.secondsRemaining);
        } else {
            std::snprintf(out.badge, sizeof(out.badge), "HYPER ARMOR");
        }
        return out;
    }

    if (hasCounterBit) {
        out.state = PlayerDefState::Counter;
        if (activeMantleId >= 0 && mantleTimerSec > 0.0f) {
            out.secondsRemaining = ceilFrames(mantleTimerSec);
            std::snprintf(out.badge, sizeof(out.badge), "AUTO-EVADE %dS", out.secondsRemaining);
        } else {
            std::snprintf(out.badge, sizeof(out.badge), isGuard ? "COUNTER GRD" : "COUNTER");
        }
        return out;
    }

    if (isGuard) {
        out.state = PlayerDefState::Guard;
        std::snprintf(out.badge, sizeof(out.badge), "GUARD");
        return out;
    }

    if (isSuperArmor) {
        out.state = PlayerDefState::SuperArmor;
        if (superSec > 0.001f) {
            out.framesRemaining = ceilFrames(superSec * 60.0f);
            std::snprintf(out.badge, sizeof(out.badge), "SUPER %dF", out.framesRemaining);
        } else {
            std::snprintf(out.badge, sizeof(out.badge), "SUPER ARMOR");
        }
        return out;
    }

    out.state = PlayerDefState::Normal;
    std::snprintf(out.badge, sizeof(out.badge), "NORMAL");
    return out;
}

bool RecordMtGeometry(std::uintptr_t geom,
                      VolumeCat cat,
                      PlayerDefState plDef,
                      std::uint32_t visMask,
                      RawSnapshot& snap,
                      std::uint8_t emPartIdx = kEmPartNone,
                      std::uint8_t emFlags = 0U) {
    if (!IsAlignedPtr(geom)) return false;
    if (snap.shapeCount >= kMaxShapes) return false;
    if ((visMask & CategoryBitForVolume(cat)) == 0) return false;

    if (!g_seenGeoms.insert(geom)) return false;

    const std::int32_t type = *reinterpret_cast<const std::int32_t*>(geom + mhw::kOffGeomType);
    ShapeRecord& rec = g_shapes[snap.shapeCount];
    rec.cat       = cat;
    rec.plDef     = plDef;
    rec.emPartIdx = emPartIdx;
    rec.emFlags   = emFlags;
    rec.type      = type;

    if (type == mhw::kGeomSphere) {
        const float* c = reinterpret_cast<const float*>(geom + 0x20);
        const float r  = *reinterpret_cast<const float*>(geom + 0x2C);
        if (!IsValidVec3(c) || !(r > 0.5f && r < 25000.0f)) return false;
        if (std::fabs(c[0]) < 0.01f && std::fabs(c[1]) < 0.01f && std::fabs(c[2]) < 0.01f) return false;
        rec.p[0] = c[0]; rec.p[1] = c[1]; rec.p[2] = c[2]; rec.p[3] = r;
        ++snap.shapeCount;
        return true;
    }

    if (type == mhw::kGeomCapsule || type == mhw::kGeomLineSweptSphere || type == mhw::kGeomCylinder) {
        const float* p0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* p1 = reinterpret_cast<const float*>(geom + 0x30);
        const float r   = *reinterpret_cast<const float*>(geom + 0x40);
        if (!IsValidVec3(p0) || !IsValidVec3(p1) || !(r > 0.5f && r < 25000.0f)) return false;
        if (std::fabs(p0[0]) < 0.01f && std::fabs(p0[1]) < 0.01f && std::fabs(p0[2]) < 0.01f &&
            std::fabs(p1[0]) < 0.01f && std::fabs(p1[1]) < 0.01f && std::fabs(p1[2]) < 0.01f) return false;
        rec.p[0] = p0[0]; rec.p[1] = p0[1]; rec.p[2] = p0[2];
        rec.p[3] = p1[0]; rec.p[4] = p1[1]; rec.p[5] = p1[2];
        rec.p[6] = r;
        ++snap.shapeCount;
        return true;
    }

    if (type == mhw::kGeomOBB) {
        const float* f = reinterpret_cast<const float*>(geom + 0x20);
        if (!IsValidVec3(f + 12)) return false;
        if (!(f[16] > 0.5f && f[16] < 25000.0f &&
              f[17] > 0.5f && f[17] < 25000.0f &&
              f[18] > 0.5f && f[18] < 25000.0f)) return false;
        if (std::fabs(f[12]) < 0.01f && std::fabs(f[13]) < 0.01f && std::fabs(f[14]) < 0.01f) return false;
        for (int i = 0; i < 19; ++i) rec.p[i] = f[i];
        ++snap.shapeCount;
        return true;
    }

    if (type == mhw::kGeomAABB) {
        const float* mn = reinterpret_cast<const float*>(geom + 0x20);
        const float* mx = reinterpret_cast<const float*>(geom + 0x30);
        if (!IsValidVec3(mn) || !IsValidVec3(mx)) return false;
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        if (!(dx > 0.5f && dx < 50000.0f && dy > 0.5f && dy < 50000.0f && dz > 0.5f && dz < 50000.0f)) return false;
        rec.p[0] = mn[0]; rec.p[1] = mn[1]; rec.p[2] = mn[2];
        rec.p[3] = mx[0]; rec.p[4] = mx[1]; rec.p[5] = mx[2];
        ++snap.shapeCount;
        return true;
    }

    if (type == mhw::kGeomTriangle) {
        const float* v0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* v1 = reinterpret_cast<const float*>(geom + 0x30);
        const float* v2 = reinterpret_cast<const float*>(geom + 0x40);
        if (!IsValidVec3(v0) || !IsValidVec3(v1) || !IsValidVec3(v2)) return false;
        for (int i = 0; i < 3; ++i) {
            rec.p[i]     = v0[i];
            rec.p[3 + i] = v1[i];
            rec.p[6 + i] = v2[i];
        }
        ++snap.shapeCount;
        return true;
    }

    if (type == mhw::kGeomLineSegment) {
        const float* p0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* p1 = reinterpret_cast<const float*>(geom + 0x30);
        if (!IsValidVec3(p0) || !IsValidVec3(p1)) return false;
        rec.p[0] = p0[0]; rec.p[1] = p0[1]; rec.p[2] = p0[2];
        rec.p[3] = p1[0]; rec.p[4] = p1[1]; rec.p[5] = p1[2];
        ++snap.shapeCount;
        return true;
    }

    return false;
}

VolumeCat DetermineNodeCategory(OwnerSide side, bool isAttack, std::uint32_t nodeAttr) {
    // Pure body pushbox (press/push bits 0x10|0x20 set, damage/attack bits 0x03 clear)
    const bool isPushOnly = !isAttack && ((nodeAttr & 0x30U) != 0) && ((nodeAttr & 0x03U) == 0);

    // Map entities (Training Pole/Cart, Barrel Bombs, Boulders, Traps, Endemic Life) stay strictly Env!
    if (side == OwnerSide::Env) {
        if (isAttack)   return VolumeCat::EnvHitbox;
        if (isPushOnly) return VolumeCat::EnvPush;
        return VolumeCat::EnvHurtbox;
    }

    // Palico / Companion stays strictly Otomo!
    if (side == OwnerSide::Otomo) {
        if (isAttack)   return VolumeCat::OtomoHitbox;
        if (isPushOnly) return VolumeCat::Pushbox;
        return VolumeCat::OtomoHurtbox;
    }

    if (isPushOnly) return VolumeCat::Pushbox;

    if (isAttack) {
        if (side == OwnerSide::Player) return VolumeCat::PlayerHitbox;
        return VolumeCat::MonsterHitbox;
    }

    if (side == OwnerSide::Monster) return VolumeCat::MonsterHurtbox;
    if (side == OwnerSide::Player)  return VolumeCat::PlayerHurtbox;
    if ((nodeAttr & 0x01U) != 0)    return VolumeCat::EnvHurtbox;
    return VolumeCat::Pushbox;
}

void MarkGeomPairSeen(std::uintptr_t primGeom, std::uintptr_t sweptGeom) noexcept {
    if (IsAlignedPtr(primGeom))  g_seenGeoms.insert(primGeom);
    if (IsAlignedPtr(sweptGeom)) g_seenGeoms.insert(sweptGeom);
}

// cEmDamageCheck::onHit (0x1402c3a94..0x1402c3aa9) rejects cCollNodeEm nodes flagged (mEmAttr & 0x80)
// (IGNORE_HIT internal sensor/lock-on spheres). We suppress IGNORE_HIT nodes while intentionally retaining
// the monster's non-IGNORE_HIT center helper sphere (mBaseDmgGroup == -1 / non-cCollNodeEm) as AUX CENTER.
bool IsMonsterIgnoreHitNode(std::uintptr_t resCollNode,
                            std::uintptr_t exeBase,
                            std::uintptr_t exeEnd) noexcept {
    if (!IsAlignedPtr(resCollNode)) return false;
    if (GetObjectDti(resCollNode, exeBase, exeEnd) != exeBase + mhw::kRvaDtiCollNodeEm) return false;
    const std::uint32_t emAttr = *reinterpret_cast<const std::uint32_t*>(resCollNode + mhw::kOffCollEmAttr);
    return (emAttr & 0x80U) != 0U;
}

void HarvestCpObjCollision(std::uintptr_t coll,
                           OwnerSide hintSide,
                           std::uintptr_t exeBase,
                           std::uintptr_t exeEnd,
                           std::uint32_t visMask,
                           RawSnapshot& snap) {
    if (!IsCpObjCollision(coll, exeBase, exeEnd)) return;
    if (!g_seenColls.insert(coll)) return;

    const std::uint8_t collFlags = *reinterpret_cast<const std::uint8_t*>(coll + mhw::kOffObjCollFlags);
    if ((collFlags & 0x1U) == 0) return;

    const std::uintptr_t owner = *reinterpret_cast<const std::uintptr_t*>(coll + mhw::kOffObjCollOwner);
    if (!IsAlignedPtr(owner)) return;
    const std::uint8_t ownerAlive = *reinterpret_cast<const std::uint8_t*>(owner + mhw::kOffUnitAliveFlags);
    if ((ownerAlive & 0x0EU) == 0) return;

    OwnerSide baseSide = hintSide;
    if (baseSide == OwnerSide::Unknown) {
        baseSide = ResolveUnitSide(owner, exeBase, exeEnd);
    }
    // Only fall back to hitGroup when unit DTI did NOT identify Player/Otomo/Monster/Env!
    // (Preserves OwnerSide::Env on Training Pole, Barrels, Falling Boulders, Endemic Life, etc.)
    if (baseSide == OwnerSide::Unknown) {
        const std::int32_t hitGroup = *reinterpret_cast<const std::int32_t*>(coll + mhw::kOffObjCollHitGroup);
        if (hitGroup == 1) baseSide = OwnerSide::Player;
        else if (hitGroup == 2 || hitGroup == 3 || hitGroup == 5) baseSide = OwnerSide::Monster;
    }

    PlayerDefState unitPlDef = PlayerDefState::Normal;
    if (baseSide == OwnerSide::Player && IsUPlayerUnit(owner, exeBase, exeEnd)) {
        unitPlDef = EvaluatePlayerDefense(owner, coll).state;
    }

    const std::uint32_t nodeCount = *reinterpret_cast<const std::uint32_t*>(coll + mhw::kOffObjCollNodeCount);
    const std::uintptr_t nodeArr  = *reinterpret_cast<const std::uintptr_t*>(coll + mhw::kOffObjCollNodeArray);
    if (nodeCount == 0 || nodeCount > 512 || !IsAlignedPtr(nodeArr)) return;

    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const std::uintptr_t cnode = *reinterpret_cast<const std::uintptr_t*>(nodeArr + i * 8);
        if (!IsAlignedPtr(cnode)) continue;
        if (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeDestroyReq) != 0) continue;
        if (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeIsActive) == 0) continue;

        const std::uintptr_t atkParam    = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeAttackParam);
        const std::uintptr_t resCollNode = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeResCollNode);
        const std::uintptr_t snode       = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeSCollNode);

        // If owner unit is Env (gimmick/boulder/stage), keep it Env; otherwise inspect cAttackParam*/cCollNode* DTI
        // so monster breath/magic VFX (uMhEffect) are accurately classified as Monster!
        OwnerSide nodeSide = baseSide;
        if (baseSide != OwnerSide::Env) {
            if (IsAlignedPtr(atkParam)) {
                const OwnerSide atkSide = ResolveObjectDtiSide(atkParam, exeBase, exeEnd);
                if (atkSide != OwnerSide::Unknown) nodeSide = atkSide;
            }
            if (nodeSide == OwnerSide::Unknown && IsAlignedPtr(resCollNode)) {
                const OwnerSide resSide = ResolveObjectDtiSide(resCollNode, exeBase, exeEnd);
                if (resSide != OwnerSide::Unknown) nodeSide = resSide;
            }
        }

        std::uint32_t nodeAttr = 0;
        if (IsAlignedPtr(snode)) {
            nodeAttr = *reinterpret_cast<const std::uint32_t*>(snode + mhw::kOffNodeObjAttr);
        }
        const bool isAttack = (atkParam != 0) || ((nodeAttr & 0x02U) != 0 && (nodeAttr & 0x01U) == 0);
        const VolumeCat cat = DetermineNodeCategory(nodeSide, isAttack, nodeAttr);

        const std::uint32_t geomCount = *reinterpret_cast<const std::uint32_t*>(cnode + mhw::kOffCNodeGeomCount);
        const std::uintptr_t geomArr  = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeGeomArray);
        if (geomCount == 0 || geomCount > 128 || !IsAlignedPtr(geomArr)) continue;

        // Suppress IGNORE_HIT sensor spheres (mEmAttr & 0x80) while keeping the center helper sphere!
        if (hintSide == OwnerSide::Monster && cat == VolumeCat::MonsterHurtbox &&
            IsMonsterIgnoreHitNode(resCollNode, exeBase, exeEnd)) {
            for (std::uint32_t g = 0; g < geomCount; ++g) {
                const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                if (!IsAlignedPtr(cgeom)) continue;
                MarkGeomPairSeen(*reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary),
                                 *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept));
            }
            continue;
        }

        if ((visMask & CategoryBitForVolume(cat)) == 0) continue;

        const PlayerDefState plDef = (cat == VolumeCat::PlayerHurtbox) ? unitPlDef : PlayerDefState::Normal;

        for (std::uint32_t g = 0; g < geomCount; ++g) {
            const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
            if (!IsAlignedPtr(cgeom)) continue;
            if (*reinterpret_cast<const std::uint8_t*>(cgeom + mhw::kOffCGeomPartActive) == 0) continue;

            const std::int32_t sweptState  = *reinterpret_cast<const std::int32_t*>(cgeom + mhw::kOffCGeomSweptState);
            const std::uintptr_t sweptGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept);
            const std::uintptr_t primGeom  = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary);

            if (sweptState != 0 && IsAlignedPtr(sweptGeom)) {
                RecordMtGeometry(sweptGeom, cat, plDef, visMask, snap);
            } else if (IsAlignedPtr(primGeom)) {
                RecordMtGeometry(primGeom, cat, plDef, visMask, snap);
            }
            MarkGeomPairSeen(primGeom, sweptGeom);
        }
    }
}

void HarvestUnitComponents(std::uintptr_t unit,
                           OwnerSide hintSide,
                           std::uintptr_t exeBase,
                           std::uintptr_t exeEnd,
                           std::uint32_t visMask,
                           RawSnapshot& snap) {
    if (!IsAlignedPtr(unit)) return;
    std::uintptr_t comp = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
    for (int guard = 0; guard < 64 && IsAlignedPtr(comp); ++guard) {
        const std::uintptr_t nextComp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
        if (IsCpObjCollision(comp, exeBase, exeEnd)) {
            HarvestCpObjCollision(comp, hintSide, exeBase, exeEnd, visMask, snap);
        }
        comp = nextComp;
    }
}

// ============================================================================
//  Monster Part Partitioning, Live Hitzone Lookup & Pushbox-to-Part Binding
// ============================================================================

bool ExtractMtGeomSegment3D(std::uintptr_t geom,
                            float outA[3],
                            float outB[3],
                            float& outRadius) noexcept {
    if (!IsAlignedPtr(geom)) return false;
    const std::int32_t type = *reinterpret_cast<const std::int32_t*>(geom + mhw::kOffGeomType);
    if (type == mhw::kGeomSphere) {
        const float* c = reinterpret_cast<const float*>(geom + 0x20);
        const float r  = *reinterpret_cast<const float*>(geom + 0x2C);
        if (!IsValidVec3(c) || !(r > 0.5f && r < 25000.0f)) return false;
        if (std::fabs(c[0]) < 0.01f && std::fabs(c[1]) < 0.01f && std::fabs(c[2]) < 0.01f) return false;
        outA[0] = outB[0] = c[0];
        outA[1] = outB[1] = c[1];
        outA[2] = outB[2] = c[2];
        outRadius = r;
        return true;
    }
    if (type == mhw::kGeomCapsule || type == mhw::kGeomLineSweptSphere || type == mhw::kGeomCylinder) {
        const float* p0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* p1 = reinterpret_cast<const float*>(geom + 0x30);
        const float r   = *reinterpret_cast<const float*>(geom + 0x40);
        if (!IsValidVec3(p0) || !IsValidVec3(p1) || !(r > 0.5f && r < 25000.0f)) return false;
        if (std::fabs(p0[0]) < 0.01f && std::fabs(p0[1]) < 0.01f && std::fabs(p0[2]) < 0.01f &&
            std::fabs(p1[0]) < 0.01f && std::fabs(p1[1]) < 0.01f && std::fabs(p1[2]) < 0.01f) return false;
        outA[0] = p0[0]; outA[1] = p0[1]; outA[2] = p0[2];
        outB[0] = p1[0]; outB[1] = p1[1]; outB[2] = p1[2];
        outRadius = r;
        return true;
    }
    if (type == mhw::kGeomOBB) {
        const float* f = reinterpret_cast<const float*>(geom + 0x20);
        if (!IsValidVec3(f + 12)) return false;
        if (!(f[16] > 0.5f && f[16] < 25000.0f &&
              f[17] > 0.5f && f[17] < 25000.0f &&
              f[18] > 0.5f && f[18] < 25000.0f)) return false;
        if (std::fabs(f[12]) < 0.01f && std::fabs(f[13]) < 0.01f && std::fabs(f[14]) < 0.01f) return false;
        int major = 0;
        if (f[17] > f[16 + major]) major = 1;
        if (f[18] > f[16 + major]) major = 2;
        const float halfSpan = f[16 + major] * 0.65f;
        const float* dir = f + major * 4;
        outA[0] = f[12] - dir[0] * halfSpan;
        outA[1] = f[13] - dir[1] * halfSpan;
        outA[2] = f[14] - dir[2] * halfSpan;
        outB[0] = f[12] + dir[0] * halfSpan;
        outB[1] = f[13] + dir[1] * halfSpan;
        outB[2] = f[14] + dir[2] * halfSpan;
        outRadius = (f[16] < f[17]) ? ((f[16] < f[18]) ? f[16] : f[18]) : ((f[17] < f[18]) ? f[17] : f[18]);
        return true;
    }
    if (type == mhw::kGeomAABB) {
        const float* mn = reinterpret_cast<const float*>(geom + 0x20);
        const float* mx = reinterpret_cast<const float*>(geom + 0x30);
        if (!IsValidVec3(mn) || !IsValidVec3(mx)) return false;
        outA[0] = outB[0] = 0.5f * (mn[0] + mx[0]);
        outA[1] = outB[1] = 0.5f * (mn[1] + mx[1]);
        outA[2] = outB[2] = 0.5f * (mn[2] + mx[2]);
        outRadius = 0.25f * ((mx[0] - mn[0]) + (mx[2] - mn[2]));
        return outRadius > 0.5f && outRadius < 25000.0f;
    }
    if (type == mhw::kGeomLineSegment) {
        const float* p0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* p1 = reinterpret_cast<const float*>(geom + 0x30);
        if (!IsValidVec3(p0) || !IsValidVec3(p1)) return false;
        outA[0] = p0[0]; outA[1] = p0[1]; outA[2] = p0[2];
        outB[0] = p1[0]; outB[1] = p1[1]; outB[2] = p1[2];
        outRadius = 15.0f;
        return true;
    }
    if (type == mhw::kGeomTriangle) {
        const float* v0 = reinterpret_cast<const float*>(geom + 0x20);
        const float* v1 = reinterpret_cast<const float*>(geom + 0x30);
        const float* v2 = reinterpret_cast<const float*>(geom + 0x40);
        if (!IsValidVec3(v0) || !IsValidVec3(v1) || !IsValidVec3(v2)) return false;
        outA[0] = outB[0] = (v0[0] + v1[0] + v2[0]) * (1.0f / 3.0f);
        outA[1] = outB[1] = (v0[1] + v1[1] + v2[1]) * (1.0f / 3.0f);
        outA[2] = outB[2] = (v0[2] + v1[2] + v2[2]) * (1.0f / 3.0f);
        outRadius = 20.0f;
        return true;
    }
    return false;
}

float SegmentSegmentDistSq3D(const float p1[3],
                             const float q1[3],
                             const float p2[3],
                             const float q2[3]) noexcept {
    const float d1x = q1[0] - p1[0], d1y = q1[1] - p1[1], d1z = q1[2] - p1[2];
    const float d2x = q2[0] - p2[0], d2y = q2[1] - p2[1], d2z = q2[2] - p2[2];
    const float rx  = p1[0] - p2[0], ry  = p1[1] - p2[1], rz  = p1[2] - p2[2];

    const float a = d1x * d1x + d1y * d1y + d1z * d1z;
    const float e = d2x * d2x + d2y * d2y + d2z * d2z;
    const float f = d2x * rx  + d2y * ry  + d2z * rz;

    float s = 0.0f, t = 0.0f;
    if (a <= 1e-4f && e <= 1e-4f) {
        return rx * rx + ry * ry + rz * rz;
    }
    if (a <= 1e-4f) {
        t = f / e;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
    } else {
        const float c = d1x * rx + d1y * ry + d1z * rz;
        if (e <= 1e-4f) {
            s = -c / a;
            if (s < 0.0f) s = 0.0f;
            else if (s > 1.0f) s = 1.0f;
        } else {
            const float b = d1x * d2x + d1y * d2y + d1z * d2z;
            const float denom = a * e - b * b;
            if (denom > 1e-4f) {
                s = (b * f - c * e) / denom;
                if (s < 0.0f) s = 0.0f;
                else if (s > 1.0f) s = 1.0f;
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = -c / a;
                if (s < 0.0f) s = 0.0f;
                else if (s > 1.0f) s = 1.0f;
            } else if (t > 1.0f) {
                t = 1.0f;
                s = (b - c) / a;
                if (s < 0.0f) s = 0.0f;
                else if (s > 1.0f) s = 1.0f;
            }
        }
    }
    const float cx = rx + d1x * s - d2x * t;
    const float cy = ry + d1y * s - d2y * t;
    const float cz = rz + d1z * s - d2z * t;
    return cx * cx + cy * cy + cz * cz;
}

struct EmScarPartEntry {
    std::uintptr_t ptr;
    std::int16_t   zoneId;
};

struct EmScarTable {
    bool            hasTable  = false;
    std::uint32_t   count     = 0;
    std::uintptr_t  scarRes   = 0;
    std::uint32_t   rawResCnt = 0;
    std::uint32_t   exCnt     = 0;
    std::uint32_t   stockCnt  = 0;
    EmScarPartEntry entries[64]{};
};

// Reads rEmOffset via uEnemy::vft[+0x958] (0x141cb4060 / 0x141cb7330 / 0x140a58fe0 on Shara Ishvalda).
// Filters cJointConstAxisData (+0x348, up to 64 entries per uEnemy 64-bit mask @ 0x1402c9ca3) using the
// exact engine tenderize gates:
//   - mRedirectIdx (+0x17C) < 0  (0x140f0c3f1 / 0x14211d970: redirect anchors never create wounds)
//   - mScarStockIdx (+0x174) >= 0 (0x1402d5275 buildup & 0x140afc5c8 active-wound slot deduplication)
//   - mScarExIdx (+0x170) valid cJointConstScarExSetting with >=1 slot >= 0 (0x1402c7926..0x1402c79e5)
//   - At least one valid (mBaseDmgGroup >= 0, mPartsDmgGroup >= 0) pair (0x14169bce0)
EmScarTable BuildEmScarTable(std::uintptr_t em,
                             std::uintptr_t exeBase,
                             std::uintptr_t exeEnd) noexcept {
    EmScarTable out{};
    if (!IsAlignedPtr(em)) return out;

    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(em);
    if (vft < exeBase || vft >= exeEnd) return out;
    const std::uintptr_t fnAddr = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffEmVftGetScarRes);
    if (fnAddr < exeBase || fnAddr >= exeEnd) return out;

    using GetScarResFn = std::uintptr_t (*)(std::uintptr_t);
    const std::uintptr_t scarRes = reinterpret_cast<GetScarResFn>(fnAddr)(em);
    if (!IsAlignedPtr(scarRes)) return out;
    out.scarRes = scarRes;

    const std::uint32_t resCnt = *reinterpret_cast<const std::uint32_t*>(scarRes + mhw::kOffScarResCount);
    const std::uintptr_t resArr = *reinterpret_cast<const std::uintptr_t*>(scarRes + mhw::kOffScarResArray);
    const std::uint32_t exCnt   = *reinterpret_cast<const std::uint32_t*>(scarRes + mhw::kOffScarResExCount);
    const std::uintptr_t exArr  = *reinterpret_cast<const std::uintptr_t*>(scarRes + mhw::kOffScarResExArray);
    const std::uint32_t stockCnt = *reinterpret_cast<const std::uint32_t*>(scarRes + mhw::kOffScarResStockCount);
    out.rawResCnt = resCnt;
    out.exCnt     = exCnt;
    out.stockCnt  = stockCnt;
    if (resCnt == 0 || resCnt > 256 || !IsAlignedPtr(resArr)) return out;

    const bool checkEx = (exCnt > 0 && exCnt <= 128 && IsAlignedPtr(exArr));
    auto hasValidExSetting = [&](std::int32_t exIdx) noexcept -> bool {
        if (!checkEx) return true;
        if (exIdx < 0 || static_cast<std::uint32_t>(exIdx) >= exCnt) return false;
        const std::uintptr_t exPtr = *reinterpret_cast<const std::uintptr_t*>(
            exArr + static_cast<std::size_t>(exIdx) * 8);
        if (!IsAlignedPtr(exPtr)) return false;
        for (std::uint32_t s = 0; s < mhw::kScarExSlotCount; ++s) {
            const std::int32_t slot = *reinterpret_cast<const std::int32_t*>(
                exPtr + mhw::kOffScarExSlots + s * 4);
            if (slot >= 0) return true;
        }
        return false;
    };

    const std::uint32_t scanCnt = (resCnt <= 64U) ? resCnt : 64U;
    for (std::uint32_t i = 0; i < scanCnt; ++i) {
        const std::uintptr_t sp = *reinterpret_cast<const std::uintptr_t*>(resArr + i * 8);
        if (!IsAlignedPtr(sp)) continue;

        const std::int32_t redirIdx = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartRedirIdx);
        if (redirIdx >= 0) continue;

        const std::int32_t stockIdx = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartGroupId);
        if (stockIdx < 0 || stockIdx >= 64) continue;
        if (stockCnt > 0 && stockCnt <= 64 && static_cast<std::uint32_t>(stockIdx) >= stockCnt) continue;

        const std::int32_t exIdx = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartExIdx);
        if (!hasValidExSetting(exIdx)) continue;

        bool hasPair = false;
        for (std::uint32_t k = 0; k < mhw::kScarPartPairMax; ++k) {
            const std::int32_t b = *reinterpret_cast<const std::int32_t*>(
                sp + mhw::kOffScarPartBaseArr + k * mhw::kScarPartPairStride);
            const std::int32_t p = *reinterpret_cast<const std::int32_t*>(
                sp + mhw::kOffScarPartSubArr + k * mhw::kScarPartPairStride);
            if (b >= 0 && b < 64 && p >= 0 && p < 64) {
                hasPair = true;
                break;
            }
        }
        if (!hasPair) continue;

        out.entries[out.count].ptr    = sp;
        out.entries[out.count].zoneId = static_cast<std::int16_t>(stockIdx);
        ++out.count;
    }
    if (out.count == 0) return out;

    out.hasTable = true;
    return out;
}

// Strict engine-parity match against cJointConstAxisData (0x14169bce0):
// requires baseGrp >= 0, partsGrp >= 0, and exact (mBaseDmgGroup[k] == baseGrp && mPartsDmgGroup[k] == partsGrp).
std::int16_t ResolveEmScarZone(const EmScarTable& tbl,
                               std::int32_t baseGrp,
                               std::int32_t partsGrp) noexcept {
    if (!tbl.hasTable || baseGrp < 0 || partsGrp < 0) return -1;

    for (std::uint32_t e = 0; e < tbl.count; ++e) {
        const std::uintptr_t sp = tbl.entries[e].ptr;
        for (std::uint32_t k = 0; k < mhw::kScarPartPairMax; ++k) {
            const std::int32_t b = *reinterpret_cast<const std::int32_t*>(
                sp + mhw::kOffScarPartBaseArr + k * mhw::kScarPartPairStride);
            const std::int32_t p = *reinterpret_cast<const std::int32_t*>(
                sp + mhw::kOffScarPartSubArr + k * mhw::kScarPartPairStride);
            if (b == baseGrp && p == partsGrp) {
                return tbl.entries[e].zoneId;
            }
        }
    }
    return -1;
}

// Queries cEmScar::isPartTenderized (0x140afbf60 @ uEnemy + 0x1C450), the exact check called by
// cEmDamageCheck::getLiveHitzone (0x1402c0808). Fast-exits in 2 reads when 0 wounds are active.
bool IsEmPartTenderized(std::uintptr_t em,
                        std::uintptr_t exeBase,
                        std::int32_t activeBase,
                        std::int32_t activeParts) noexcept {
    if (!IsAlignedPtr(em) || activeBase < 0 || activeParts < 0) return false;
    const std::uintptr_t scarMgr = em + mhw::kOffEmScarMgr;
    const std::uint64_t activeCnt = *reinterpret_cast<const std::uint64_t*>(scarMgr + mhw::kOffScarRingCount);
    if (activeCnt == 0 || activeCnt > 16) return false;
    if (*reinterpret_cast<const std::uintptr_t*>(scarMgr + mhw::kOffScarOwnerEm) != em) return false;

    using ScarIsTenderizedFn = bool (*)(std::uintptr_t, std::int32_t, std::int32_t);
    const auto fn = reinterpret_cast<ScarIsTenderizedFn>(exeBase + mhw::kRvaScarIsTenderized);
    return fn(scarMgr, activeBase, activeParts);
}

// Computes exact live MHW 15.23 physical & elemental hitzones matching:
//   - Physical cols 0..2 (0x1402c0650 + 0x140a08bd0): live sub-state + vft[+0x690] + dtt_msf soften
//   - Elemental cols 3..7 (0x1402bcb68..0x1402bcba3): live sub-state + vft[+0x690] (NO soften!)
//     Verified cMeatData order (0x142f18570): +0x18=Fire(3), +0x1C=Water(4), +0x20=Ice(5), +0x24=Thunder(6), +0x28=Dragon(7)
void ResolveEmBaseGroupMeat(std::uintptr_t em,
                            std::uintptr_t exeBase,
                            std::uintptr_t exeEnd,
                            std::int32_t activeBase,
                            std::int32_t activeParts,
                            bool isTenderized,
                            std::int16_t& outSubGrp,
                            std::int16_t& outSlash,
                            std::int16_t& outBlunt,
                            std::int16_t& outShot,
                            std::int16_t& outFire,
                            std::int16_t& outWater,
                            std::int16_t& outThunder,
                            std::int16_t& outIce,
                            std::int16_t& outDragon,
                            std::int8_t& outExtract) noexcept {
    outSubGrp  = 0;
    outSlash   = -1;
    outBlunt   = -1;
    outShot    = -1;
    outFire    = -1;
    outWater   = -1;
    outThunder = -1;
    outIce     = -1;
    outDragon  = -1;
    outExtract = -1;
    if (activeBase < 0 || activeBase >= 64 || !IsAlignedPtr(em)) return;

    const std::uintptr_t holder = *reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffEmParamHolder);
    if (!IsAlignedPtr(holder)) return;
    const std::uintptr_t paramParts = *reinterpret_cast<const std::uintptr_t*>(holder + mhw::kOffHolderParamParts);
    if (!IsAlignedPtr(paramParts)) return;

    const std::uint32_t pgCount = *reinterpret_cast<const std::uint32_t*>(paramParts + mhw::kOffParamPartsGrpCount);
    const std::uintptr_t pgArr  = *reinterpret_cast<const std::uintptr_t*>(paramParts + mhw::kOffParamPartsGrpArray);
    if (static_cast<std::uint32_t>(activeBase) >= pgCount || pgCount > 64 || !IsAlignedPtr(pgArr)) return;

    const std::uintptr_t pg = *reinterpret_cast<const std::uintptr_t*>(pgArr + static_cast<std::size_t>(activeBase) * 8);
    if (!IsAlignedPtr(pg)) return;

    std::int32_t ext = *reinterpret_cast<const std::int32_t*>(pg + mhw::kOffPartsGrpExtract);
    if (ext == 4) {
        ext = *reinterpret_cast<const std::int32_t*>(em + mhw::kOffEmDynamicExtract);
    }
    if (ext >= 0 && ext <= 3) {
        outExtract = static_cast<std::int8_t>(ext);
    }

    const std::uint32_t subCnt = *reinterpret_cast<const std::uint32_t*>(pg + mhw::kOffPartsGrpSubCount);
    const std::uintptr_t subArr = *reinterpret_cast<const std::uintptr_t*>(pg + mhw::kOffPartsGrpSubArray);
    if (subCnt == 0 || subCnt > 16 || !IsAlignedPtr(subArr)) return;

    auto readSubMeatIdx = [&](std::uint32_t pIdx) noexcept -> std::int32_t {
        if (pIdx >= subCnt) return -1;
        const std::uintptr_t subObj = *reinterpret_cast<const std::uintptr_t*>(subArr + pIdx * 8);
        if (!IsAlignedPtr(subObj)) return -1;
        std::int32_t st = 0;
        if (activeBase < 32 && pIdx < 5) {
            st = *reinterpret_cast<const std::int32_t*>(
                em + mhw::kOffEmDamageCheck +
                static_cast<std::size_t>(activeBase) * mhw::kDmgCheckPartStride +
                static_cast<std::size_t>(pIdx) * mhw::kDmgCheckSubStride +
                mhw::kOffDmgCheckSubState);
            if (st < 0 || st > 3) st = 0;
        }
        std::int32_t mIdx = *reinterpret_cast<const std::int32_t*>(subObj + 0x10 + static_cast<std::size_t>(st) * 0x10);
        if (mIdx < 0 && st != 0) {
            mIdx = *reinterpret_cast<const std::int32_t*>(subObj + 0x10);
        }
        return mIdx;
    };

    const std::uint32_t usePIdx = (activeParts >= 0 && static_cast<std::uint32_t>(activeParts) < subCnt)
                                      ? static_cast<std::uint32_t>(activeParts) : 0U;
    const std::int32_t meatIdx = readSubMeatIdx(usePIdx);
    if (usePIdx > 0 && subCnt > 1) {
        const std::int32_t meatIdx0 = readSubMeatIdx(0U);
        const bool tend0 = IsEmPartTenderized(em, exeBase, activeBase, 0);
        if (meatIdx != meatIdx0 || isTenderized != tend0) {
            outSubGrp = static_cast<std::int16_t>(usePIdx);
        }
    }

    const std::uint32_t meatCnt = *reinterpret_cast<const std::uint32_t*>(paramParts + mhw::kOffParamMeatCount);
    const std::uintptr_t meatArr = *reinterpret_cast<const std::uintptr_t*>(paramParts + mhw::kOffParamMeatArray);
    if (meatIdx < 0 || static_cast<std::uint32_t>(meatIdx) >= meatCnt || meatCnt > 128 || !IsAlignedPtr(meatArr)) return;

    const std::uintptr_t meat = *reinterpret_cast<const std::uintptr_t*>(meatArr + static_cast<std::size_t>(meatIdx) * 8);
    if (!IsAlignedPtr(meat)) return;

    using GetMeatMultFn = float (*)(std::uintptr_t, std::int32_t, std::int32_t, std::int32_t);
    GetMeatMultFn multFn = nullptr;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(em);
    if (vft >= exeBase && vft < exeEnd) {
        const std::uintptr_t fnAddr = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffEmVftMeatMult);
        if (fnAddr >= exeBase && fnAddr < exeEnd && fnAddr != (exeBase + mhw::kRvaEmDefaultMeatMult)) {
            multFn = reinterpret_cast<GetMeatMultFn>(fnAddr);
        }
    }

    bool applySoften = false;
    std::int32_t softenAdd = 25;
    if (isTenderized) {
        const std::uintptr_t dttMsf = *reinterpret_cast<const std::uintptr_t*>(holder + mhw::kOffHolderMeatSoften);
        if (IsAlignedPtr(dttMsf)) {
            const std::uint32_t msfCnt = *reinterpret_cast<const std::uint32_t*>(dttMsf + mhw::kOffMsfCount);
            const std::uintptr_t msfArr = *reinterpret_cast<const std::uintptr_t*>(dttMsf + mhw::kOffMsfArray);
            if (msfCnt > 0 && msfCnt <= 64 && IsAlignedPtr(msfArr)) {
                const std::uint32_t sfIdxRaw = *reinterpret_cast<const std::uint32_t*>(
                    paramParts + mhw::kOffParamPartsSoftenIdx);
                const std::uint32_t sfIdx = (sfIdxRaw < msfCnt) ? sfIdxRaw : 0U;
                const std::uintptr_t msfEntry = *reinterpret_cast<const std::uintptr_t*>(msfArr + sfIdx * 8);
                if (IsAlignedPtr(msfEntry)) {
                    const std::int32_t bonus = *reinterpret_cast<const std::int32_t*>(msfEntry + mhw::kOffMsfBonus);
                    if (bonus >= -100 && bonus <= 100) {
                        softenAdd   = 25 + bonus;
                        applySoften = true;
                    }
                }
            }
        }
    }

    auto evalCol = [&](std::int32_t col, std::size_t meatOff, bool allowSoften) noexcept -> std::int16_t {
        const std::int32_t raw = *reinterpret_cast<const std::int32_t*>(meat + meatOff);
        float mult = 1.0f;
        if (multFn) {
            const float m = multFn(em, activeBase, static_cast<std::int32_t>(usePIdx), col);
            if (std::isfinite(m) && m >= 0.0f && m <= 20.0f) {
                mult = m;
            }
        }
        std::int32_t hz = static_cast<std::int32_t>(static_cast<float>(raw) * mult);
        if (allowSoften && applySoften) {
            hz = static_cast<std::int32_t>(std::floor(static_cast<float>(hz) * 0.75f)) + softenAdd;
        }
        if (hz < 0) return 0;
        if (hz > 999) return 999;
        return static_cast<std::int16_t>(hz);
    };

    outSlash   = evalCol(0, mhw::kOffMeatSlash,   true);
    outBlunt   = evalCol(1, mhw::kOffMeatBlunt,   true);
    outShot    = evalCol(2, mhw::kOffMeatShot,    true);
    outFire    = evalCol(3, mhw::kOffMeatFire,    false);
    outWater   = evalCol(4, mhw::kOffMeatWater,   false);
    outIce     = evalCol(5, mhw::kOffMeatIce,     false);
    outThunder = evalCol(6, mhw::kOffMeatThunder, false);
    outDragon  = evalCol(7, mhw::kOffMeatDragon,  false);
}

const char* ExtractTagChar(std::int8_t ext) noexcept {
    switch (ext) {
        case 0:  return "R";
        case 1:  return "W";
        case 2:  return "O";
        case 3:  return "G";
        default: return "-";
    }
}

const char* RideZoneShortName(std::int16_t rz) noexcept {
    switch (rz) {
        case 0:  return "HEAD";
        case 1:  return "BODY";
        case 2:  return "TAIL";
        case 3:  return "WING";
        default: return "BODY";
    }
}

void FormatMonsterIdent(std::uintptr_t em,
                        std::uint32_t emId,
                        std::uint32_t subId,
                        std::uintptr_t exeBase,
                        std::uintptr_t exeEnd,
                        char (&out)[32]) noexcept {
    const std::uintptr_t dti = GetObjectDti(em, exeBase, exeEnd);
    if (IsAlignedPtr(dti)) {
        const char* cls = *reinterpret_cast<const char* const*>(dti + mhw::kOffDtiName);
        if (cls && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(cls)) &&
            (StartsWith(cls, "uEm") || StartsWith(cls, "uEms"))) {
            const char* p = cls + 1; // Strip leading 'u' -> "Em002_00"
            int w = 0;
            while (p[w] != '\0' && w < 24) {
                char c = p[w];
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                out[w++] = c;
            }
            out[w] = '\0';
            return;
        }
    }
    if (subId > 0 && subId < 99) {
        std::snprintf(out, sizeof(out), "EM%03u_%02u", emId % 1000U, subId);
    } else {
        std::snprintf(out, sizeof(out), "EM%03u", emId % 1000U);
    }
}

constexpr std::uint8_t kEmNoClawColorIdx    = 16U;
constexpr std::uint8_t kEmAuxCenterColorIdx = 17U;

void FormatPartLabels(EmPartSlotInfo& s, app::EmPartMode mode) noexcept {
    const char* tnd = s.tenderized ? "*" : "";
    if (s.isAuxCenter) {
        std::snprintf(s.shortTag, sizeof(s.shortTag), "CTR");
        std::snprintf(s.rowLabel, sizeof(s.rowLabel), "AUX CTR");
        return;
    }
    if (s.isNoClaw) {
        std::snprintf(s.shortTag, sizeof(s.shortTag), "NO-CLAW");
        std::snprintf(s.rowLabel, sizeof(s.rowLabel), "NO-CLAW");
        return;
    }
    if (mode == app::EmPartMode::ClutchZone) {
        if (s.rideFallback) {
            if (s.zoneId >= 0 && s.zoneId < 4) {
                std::snprintf(s.shortTag, sizeof(s.shortTag), "M%d %s",
                              static_cast<int>(s.zoneId), RideZoneShortName(s.zoneId));
                std::snprintf(s.rowLabel, sizeof(s.rowLabel), "M%d %s",
                              static_cast<int>(s.zoneId), RideZoneShortName(s.zoneId));
            } else if (s.baseGrp >= 0) {
                std::snprintf(s.shortTag, sizeof(s.shortTag), "P%02d%s", static_cast<int>(s.baseGrp), tnd);
                std::snprintf(s.rowLabel, sizeof(s.rowLabel), "%s", s.shortTag);
            } else {
                std::snprintf(s.shortTag, sizeof(s.shortTag), "CTR");
                std::snprintf(s.rowLabel, sizeof(s.rowLabel), "AUX CTR");
            }
        } else if (s.zoneId >= 0) {
            std::snprintf(s.shortTag, sizeof(s.shortTag), "Z%d%s", static_cast<int>(s.zoneId), tnd);
            std::snprintf(s.rowLabel, sizeof(s.rowLabel), "CLAW Z%d%s", static_cast<int>(s.zoneId), tnd);
        } else {
            std::snprintf(s.shortTag, sizeof(s.shortTag), "NO-CLAW");
            std::snprintf(s.rowLabel, sizeof(s.rowLabel), "NO-CLAW");
        }
    } else {
        if (s.baseGrp >= 0) {
            if (s.subGrp > 0) {
                std::snprintf(s.shortTag, sizeof(s.shortTag), "P%02d.%d%s",
                              static_cast<int>(s.baseGrp), static_cast<int>(s.subGrp), tnd);
            } else {
                std::snprintf(s.shortTag, sizeof(s.shortTag), "P%02d%s",
                              static_cast<int>(s.baseGrp), tnd);
            }
            std::snprintf(s.rowLabel, sizeof(s.rowLabel), "%s", s.shortTag);
        } else {
            std::snprintf(s.shortTag, sizeof(s.shortTag), "CTR");
            std::snprintf(s.rowLabel, sizeof(s.rowLabel), "AUX CTR");
        }
    }
}

// One-shot diagnostic dump when inspecting a target monster in CLUTCH mode (fires once per monster/mode change).
std::uintptr_t g_lastLoggedClawEm    = 0;
std::uint32_t  g_lastLoggedClawEmId  = 0xFFFFFFFFU;
std::uint32_t  g_lastLoggedClawSubId = 0xFFFFFFFFU;

void LogEmClutchTableOnce(std::uintptr_t em,
                          const EmInspectorSnapshot& insp,
                          const EmScarTable& tbl,
                          const EmRawGeomCand* rawGeoms,
                          std::uint32_t rawCnt) noexcept {
    if (em == g_lastLoggedClawEm && insp.emId == g_lastLoggedClawEmId && insp.subId == g_lastLoggedClawSubId) {
        return;
    }
    g_lastLoggedClawEm    = em;
    g_lastLoggedClawEmId  = insp.emId;
    g_lastLoggedClawSubId = insp.subId;

    app::Log("emclutch: target=%s(id=%u,sub=%u) rEmOffset=%p rawAxes=%u exCnt=%u stockCnt=%u validAxes=%u hasTbl=%d parts=%u",
             insp.targetName, insp.emId, insp.subId,
             reinterpret_cast<const void*>(tbl.scarRes),
             tbl.rawResCnt, tbl.exCnt, tbl.stockCnt, tbl.count,
             tbl.hasTable ? 1 : 0, insp.partCount);

    if (IsAlignedPtr(tbl.scarRes) && tbl.rawResCnt > 0 && tbl.rawResCnt <= 256) {
        const std::uintptr_t resArr = *reinterpret_cast<const std::uintptr_t*>(tbl.scarRes + mhw::kOffScarResArray);
        const std::uintptr_t exArr  = *reinterpret_cast<const std::uintptr_t*>(tbl.scarRes + mhw::kOffScarResExArray);
        const bool hasExArr = (tbl.exCnt > 0 && tbl.exCnt <= 128 && IsAlignedPtr(exArr));
        if (IsAlignedPtr(resArr)) {
            const std::uint32_t dumpCnt = (tbl.rawResCnt <= 64U) ? tbl.rawResCnt : 64U;
            char line[512]{};
            int pos = 0;
            int inLine = 0;
            for (std::uint32_t i = 0; i < dumpCnt; ++i) {
                const std::uintptr_t sp = *reinterpret_cast<const std::uintptr_t*>(resArr + i * 8);
                if (!IsAlignedPtr(sp)) continue;
                const std::int32_t jnt   = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartJointId);
                const std::int32_t exIdx = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartExIdx);
                const std::int32_t stk   = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartGroupId);
                const std::int32_t rdr   = *reinterpret_cast<const std::int32_t*>(sp + mhw::kOffScarPartRedirIdx);
                int exOk = 1;
                if (hasExArr) {
                    exOk = 0;
                    if (exIdx >= 0 && static_cast<std::uint32_t>(exIdx) < tbl.exCnt) {
                        const std::uintptr_t exPtr = *reinterpret_cast<const std::uintptr_t*>(
                            exArr + static_cast<std::size_t>(exIdx) * 8);
                        if (IsAlignedPtr(exPtr)) {
                            for (std::uint32_t s = 0; s < mhw::kScarExSlotCount; ++s) {
                                if (*reinterpret_cast<const std::int32_t*>(exPtr + mhw::kOffScarExSlots + s * 4) >= 0) {
                                    exOk = 1;
                                    break;
                                }
                            }
                        }
                    }
                }
                char pairs[128]{};
                int pp = 0;
                for (std::uint32_t k = 0; k < mhw::kScarPartPairMax; ++k) {
                    const std::int32_t b = *reinterpret_cast<const std::int32_t*>(
                        sp + mhw::kOffScarPartBaseArr + k * mhw::kScarPartPairStride);
                    const std::int32_t p = *reinterpret_cast<const std::int32_t*>(
                        sp + mhw::kOffScarPartSubArr + k * mhw::kScarPartPairStride);
                    if (b >= 0 || p >= 0) {
                        const int n = std::snprintf(pairs + pp, sizeof(pairs) - static_cast<std::size_t>(pp),
                                                    "%s%d:%d", (pp > 0) ? "," : "", b, p);
                        if (n > 0 && pp + n < static_cast<int>(sizeof(pairs))) pp += n;
                    }
                }
                if (pp == 0) std::snprintf(pairs, sizeof(pairs), "-");

                const int w = std::snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos),
                                            "%s[%u:j=%d stk=%d ex=%d(%d) rdr=%d bp=%s]",
                                            (pos > 0) ? " " : "", i, jnt, stk, exIdx, exOk, rdr, pairs);
                if (w > 0 && pos + w < static_cast<int>(sizeof(line))) {
                    pos += w;
                }
                ++inLine;
                if (inLine >= 3 || pos > 380) {
                    app::Log("emclutch: axes %s", line);
                    line[0] = '\0';
                    pos = 0;
                    inLine = 0;
                }
            }
            if (pos > 0) {
                app::Log("emclutch: axes %s", line);
            }
        }
    }

    // Also log unique hurtbox (base,parts,ride)->clawZone mappings on this monster
    struct BpSeen { std::int16_t b, p, r, z; };
    BpSeen seen[48]{};
    std::uint32_t seenCnt = 0;
    for (std::uint32_t i = 0; i < rawCnt; ++i) {
        const EmRawGeomCand& rc = rawGeoms[i];
        if (rc.cat != VolumeCat::MonsterHurtbox) continue;
        bool dup = false;
        for (std::uint32_t k = 0; k < seenCnt; ++k) {
            if (seen[k].b == rc.baseGrp && seen[k].p == rc.partsGrp &&
                seen[k].r == rc.rideZone && seen[k].z == rc.clawZone) {
                dup = true;
                break;
            }
        }
        if (!dup && seenCnt < 48) {
            seen[seenCnt++] = {rc.baseGrp, rc.partsGrp, rc.rideZone, rc.clawZone};
        }
    }
    char mline[512]{};
    int mpos = 0;
    for (std::uint32_t k = 0; k < seenCnt; ++k) {
        const int w = std::snprintf(mline + mpos, sizeof(mline) - static_cast<std::size_t>(mpos),
                                    "%s(%d:%d r=%d->z=%d)",
                                    (mpos > 0) ? " " : "",
                                    static_cast<int>(seen[k].b), static_cast<int>(seen[k].p),
                                    static_cast<int>(seen[k].r), static_cast<int>(seen[k].z));
        if (w > 0 && mpos + w < static_cast<int>(sizeof(mline))) mpos += w;
    }
    if (mpos > 0) {
        app::Log("emclutch: nodes %s", mline);
    }
}

void HarvestSingleMonsterPartitioned(std::uintptr_t em,
                                     bool isTargetEm,
                                     bool soloTarget,
                                     bool emitShapes,
                                     std::uintptr_t exeBase,
                                     std::uintptr_t exeEnd,
                                     const app::ViewFilterState& viewState,
                                     RawSnapshot& snap) {
    // Collect unique cpObjCollision instances belonging to this monster
    std::uintptr_t colls[12]{};
    std::uint32_t collCnt = 0;
    auto pushColl = [&](std::uintptr_t c) {
        if (!IsCpObjCollision(c, exeBase, exeEnd)) return;
        for (std::uint32_t k = 0; k < collCnt; ++k) {
            if (colls[k] == c) return;
        }
        if (emitShapes && !g_seenColls.insert(c)) return;
        if (collCnt < 12) colls[collCnt++] = c;
    };

    pushColl(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterCollPrimary));
    pushColl(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterColl));
    std::uintptr_t comp = *reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffCompMgrHead);
    for (int guard = 0; guard < 64 && IsAlignedPtr(comp); ++guard) {
        const std::uintptr_t nextComp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
        pushColl(comp);
        comp = nextComp;
    }
    if (collCnt == 0) return;

    const std::uint32_t visMask = viewState.visMask;

    // Fast path for non-target monsters when part mode is active
    if (!isTargetEm) {
        if (!emitShapes) return;
        for (std::uint32_t ci = 0; ci < collCnt; ++ci) {
            const std::uintptr_t coll = colls[ci];
            if ((*reinterpret_cast<const std::uint8_t*>(coll + mhw::kOffObjCollFlags) & 0x1U) == 0) continue;
            const std::uint32_t nodeCount = *reinterpret_cast<const std::uint32_t*>(coll + mhw::kOffObjCollNodeCount);
            const std::uintptr_t nodeArr  = *reinterpret_cast<const std::uintptr_t*>(coll + mhw::kOffObjCollNodeArray);
            if (nodeCount == 0 || nodeCount > 512 || !IsAlignedPtr(nodeArr)) continue;

            for (std::uint32_t i = 0; i < nodeCount; ++i) {
                const std::uintptr_t cnode = *reinterpret_cast<const std::uintptr_t*>(nodeArr + i * 8);
                if (!IsAlignedPtr(cnode)) continue;
                if (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeDestroyReq) != 0) continue;
                if (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeIsActive) == 0) continue;

                const std::uintptr_t atkParam    = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeAttackParam);
                const std::uintptr_t resCollNode = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeResCollNode);
                const std::uintptr_t snode       = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeSCollNode);
                std::uint32_t nodeAttr = IsAlignedPtr(snode) ? *reinterpret_cast<const std::uint32_t*>(snode + mhw::kOffNodeObjAttr) : 0U;
                const bool isAttack = (atkParam != 0) || ((nodeAttr & 0x02U) != 0 && (nodeAttr & 0x01U) == 0);
                const VolumeCat cat = DetermineNodeCategory(OwnerSide::Monster, isAttack, nodeAttr);

                const std::uint32_t geomCount = *reinterpret_cast<const std::uint32_t*>(cnode + mhw::kOffCNodeGeomCount);
                const std::uintptr_t geomArr  = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeGeomArray);
                if (geomCount == 0 || geomCount > 128 || !IsAlignedPtr(geomArr)) continue;

                if (cat == VolumeCat::MonsterHurtbox &&
                    IsMonsterIgnoreHitNode(resCollNode, exeBase, exeEnd)) {
                    for (std::uint32_t g = 0; g < geomCount; ++g) {
                        const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                        if (!IsAlignedPtr(cgeom)) continue;
                        MarkGeomPairSeen(*reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary),
                                         *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept));
                    }
                    continue;
                }

                const bool filterOutNonTarget = soloTarget &&
                                                (cat == VolumeCat::MonsterHurtbox || cat == VolumeCat::Pushbox);
                for (std::uint32_t g = 0; g < geomCount; ++g) {
                    const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                    if (!IsAlignedPtr(cgeom)) continue;
                    if (*reinterpret_cast<const std::uint8_t*>(cgeom + mhw::kOffCGeomPartActive) == 0) continue;
                    const std::int32_t sweptState  = *reinterpret_cast<const std::int32_t*>(cgeom + mhw::kOffCGeomSweptState);
                    const std::uintptr_t sweptGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept);
                    const std::uintptr_t primGeom  = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary);
                    const std::uintptr_t actGeom   = (sweptState != 0 && IsAlignedPtr(sweptGeom)) ? sweptGeom : primGeom;
                    if (filterOutNonTarget) {
                        if (viewState.emUnsStyle == app::EmUnselectedStyle::Ghost) {
                            RecordMtGeometry(actGeom, cat, PlayerDefState::Normal, visMask, snap, kEmPartNone, kEmFlagGhost);
                        }
                        MarkGeomPairSeen(primGeom, sweptGeom);
                    } else {
                        RecordMtGeometry(actGeom, cat, PlayerDefState::Normal, visMask, snap);
                        MarkGeomPairSeen(primGeom, sweptGeom);
                    }
                }
            }
        }
        return;
    }

    // Target Monster: build rScarParts Clutch Claw zone table, harvest live hitzones, bind pushboxes
    const EmScarTable scarTbl = BuildEmScarTable(em, exeBase, exeEnd);
    const bool hasClawTable   = scarTbl.hasTable;

    std::uint32_t rawCnt = 0;
    const std::uintptr_t dtiCollNodeEm = exeBase + mhw::kRvaDtiCollNodeEm;

    for (std::uint32_t ci = 0; ci < collCnt; ++ci) {
        const std::uintptr_t coll = colls[ci];
        if ((*reinterpret_cast<const std::uint8_t*>(coll + mhw::kOffObjCollFlags) & 0x1U) == 0) continue;
        const std::uint32_t nodeCount = *reinterpret_cast<const std::uint32_t*>(coll + mhw::kOffObjCollNodeCount);
        const std::uintptr_t nodeArr  = *reinterpret_cast<const std::uintptr_t*>(coll + mhw::kOffObjCollNodeArray);
        if (nodeCount == 0 || nodeCount > 512 || !IsAlignedPtr(nodeArr)) continue;

        for (std::uint32_t i = 0; i < nodeCount; ++i) {
            const std::uintptr_t cnode = *reinterpret_cast<const std::uintptr_t*>(nodeArr + i * 8);
            if (!IsAlignedPtr(cnode)) continue;
            if (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeDestroyReq) != 0) continue;
            const bool nodeActive = (*reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeIsActive) != 0);

            const std::uintptr_t atkParam    = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeAttackParam);
            const std::uintptr_t resCollNode = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeResCollNode);
            const std::uintptr_t snode       = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeSCollNode);

            std::uint32_t nodeAttr = IsAlignedPtr(snode) ? *reinterpret_cast<const std::uint32_t*>(snode + mhw::kOffNodeObjAttr) : 0U;
            const bool isAttack = (atkParam != 0) || ((nodeAttr & 0x02U) != 0 && (nodeAttr & 0x01U) == 0);
            const VolumeCat cat = DetermineNodeCategory(OwnerSide::Monster, isAttack, nodeAttr);

            const std::uint32_t geomCount = *reinterpret_cast<const std::uint32_t*>(cnode + mhw::kOffCNodeGeomCount);
            const std::uintptr_t geomArr  = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeGeomArray);
            if (geomCount == 0 || geomCount > 128 || !IsAlignedPtr(geomArr)) continue;

            if (cat != VolumeCat::MonsterHurtbox && cat != VolumeCat::Pushbox) {
                if (emitShapes && nodeActive) {
                    for (std::uint32_t g = 0; g < geomCount; ++g) {
                        const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                        if (!IsAlignedPtr(cgeom)) continue;
                        if (*reinterpret_cast<const std::uint8_t*>(cgeom + mhw::kOffCGeomPartActive) == 0) continue;
                        const std::int32_t sweptState  = *reinterpret_cast<const std::int32_t*>(cgeom + mhw::kOffCGeomSweptState);
                        const std::uintptr_t sweptGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept);
                        const std::uintptr_t primGeom  = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary);
                        const std::uintptr_t actGeom   = (sweptState != 0 && IsAlignedPtr(sweptGeom)) ? sweptGeom : primGeom;
                        RecordMtGeometry(actGeom, cat, PlayerDefState::Normal, visMask, snap);
                        MarkGeomPairSeen(primGeom, sweptGeom);
                    }
                }
                continue;
            }

            std::int32_t activeBase  = -1;
            std::int32_t activeParts = -1;
            std::int16_t subGrp      = 0;
            std::int16_t clawZone    = -1;
            std::int16_t rideZone    = 4;
            std::int16_t slash = -1, blunt = -1, shot = -1;
            std::int16_t fire = -1, water = -1, thunder = -1, ice = -1, dragon = -1;
            std::int8_t  extract      = -1;
            bool         tenderized   = false;
            bool         changedState = false;
            bool         ignoreHit    = false;
            const bool   isCollNodeEm = IsAlignedPtr(resCollNode) &&
                                        (GetObjectDti(resCollNode, exeBase, exeEnd) == dtiCollNodeEm);

            if (isCollNodeEm) {
                const std::int32_t rawBase  = *reinterpret_cast<const std::int32_t*>(resCollNode + mhw::kOffCollEmBaseDmgGrp);
                const std::int32_t rawParts = *reinterpret_cast<const std::int32_t*>(resCollNode + mhw::kOffCollEmPartsDmgGrp);
                const std::int32_t chgBase  = *reinterpret_cast<const std::int32_t*>(resCollNode + mhw::kOffCollEmBaseDmgChg);
                const std::int32_t chgParts = *reinterpret_cast<const std::int32_t*>(resCollNode + mhw::kOffCollEmPartsDmgChg);
                const std::uint32_t emAttr  = *reinterpret_cast<const std::uint32_t*>(resCollNode + mhw::kOffCollEmAttr);
                const std::int32_t rawRide  = *reinterpret_cast<const std::int32_t*>(resCollNode + mhw::kOffCollEmRideParts);

                ignoreHit = ((emAttr & 0x80U) != 0);
                rideZone  = (rawRide >= 0 && rawRide <= 7) ? static_cast<std::int16_t>(rawRide) : 4;

                activeBase  = rawBase;
                activeParts = rawParts;
                if (rawBase >= 0 && rawBase < 32) {
                    const std::int32_t isChg = *reinterpret_cast<const std::int32_t*>(
                        em + mhw::kOffEmDamageCheck +
                        static_cast<std::size_t>(rawBase) * mhw::kDmgCheckPartStride +
                        mhw::kOffDmgCheckChanged);
                    if (isChg == 1 && chgBase >= 0) {
                        activeBase   = chgBase;
                        activeParts  = chgParts;
                        changedState = true;
                    }
                }
                if (!ignoreHit && activeBase >= 0 && activeBase < 64) {
                    clawZone   = ResolveEmScarZone(scarTbl, activeBase, activeParts);
                    tenderized = IsEmPartTenderized(em, exeBase, activeBase, activeParts);
                    if (changedState && clawZone < 0 && rawBase >= 0 && rawParts >= 0) {
                        clawZone = ResolveEmScarZone(scarTbl, rawBase, rawParts);
                    }
                    ResolveEmBaseGroupMeat(em, exeBase, exeEnd, activeBase, activeParts, tenderized,
                                           subGrp, slash, blunt, shot,
                                           fire, water, thunder, ice, dragon, extract);
                }
            }

            // Exclude IGNORE_HIT sensor/lock-on nodes (mEmAttr & 0x80) while RETAINING the non-IGNORE_HIT
            // center helper sphere (activeBase < 0 / non-cCollNodeEm) as a clean AUX CENTER part!
            if (cat == VolumeCat::MonsterHurtbox && ignoreHit) {
                if (emitShapes && nodeActive) {
                    for (std::uint32_t g = 0; g < geomCount; ++g) {
                        const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                        if (!IsAlignedPtr(cgeom)) continue;
                        MarkGeomPairSeen(*reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary),
                                         *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept));
                    }
                }
                continue;
            }

            for (std::uint32_t g = 0; g < geomCount; ++g) {
                const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
                if (!IsAlignedPtr(cgeom)) continue;
                const bool geomActive = (*reinterpret_cast<const std::uint8_t*>(cgeom + mhw::kOffCGeomPartActive) != 0);
                const std::int32_t sweptState  = *reinterpret_cast<const std::int32_t*>(cgeom + mhw::kOffCGeomSweptState);
                const std::uintptr_t sweptGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSwept);
                const std::uintptr_t primGeom  = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomPrimary);
                const std::uintptr_t actGeom   = (sweptState != 0 && IsAlignedPtr(sweptGeom)) ? sweptGeom : primGeom;

                std::int16_t jnt0 = -1, jnt1 = -1;
                const std::uintptr_t srcCollGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + mhw::kOffCGeomSrcCollGeom);
                if (IsAlignedPtr(srcCollGeom)) {
                    jnt0 = *reinterpret_cast<const std::int16_t*>(srcCollGeom + mhw::kOffCollGeomJnt0);
                    jnt1 = *reinterpret_cast<const std::int16_t*>(srcCollGeom + mhw::kOffCollGeomJnt1);
                }

                float segA[3]{}, segB[3]{}, rad = 20.0f;
                bool hasSeg = ExtractMtGeomSegment3D(actGeom, segA, segB, rad);
                if (!hasSeg && actGeom != primGeom) {
                    hasSeg = ExtractMtGeomSegment3D(primGeom, segA, segB, rad);
                }

                if (rawCnt < kMaxEmRawGeoms) {
                    EmRawGeomCand& rc = g_emRawGeoms[rawCnt++];
                    rc.activeGeom   = actGeom;
                    rc.primGeom     = primGeom;
                    rc.sweptGeom    = sweptGeom;
                    rc.cat          = cat;
                    rc.nodeActive   = nodeActive && hasSeg;
                    rc.geomActive   = geomActive && hasSeg;
                    rc.jnt0         = jnt0;
                    rc.jnt1         = jnt1;
                    rc.baseGrp      = (activeBase >= -1 && activeBase < 128) ? static_cast<std::int16_t>(activeBase) : -1;
                    rc.partsGrp     = (activeParts >= -1 && activeParts < 128) ? static_cast<std::int16_t>(activeParts) : -1;
                    rc.subGrp       = subGrp;
                    rc.clawZone     = clawZone;
                    rc.rideZone     = rideZone;
                    rc.slash        = slash;
                    rc.blunt        = blunt;
                    rc.shot         = shot;
                    rc.fire         = fire;
                    rc.water        = water;
                    rc.thunder      = thunder;
                    rc.ice          = ice;
                    rc.dragon       = dragon;
                    rc.extract      = extract;
                    rc.tenderized   = tenderized;
                    rc.changedState = changedState;
                    rc.ignoreHit    = ignoreHit;
                    rc.partSlot     = -1;
                    rc.segA[0] = segA[0]; rc.segA[1] = segA[1]; rc.segA[2] = segA[2];
                    rc.segB[0] = segB[0]; rc.segB[1] = segB[1]; rc.segB[2] = segB[2];
                    rc.radius  = rad;
                } else if (emitShapes && nodeActive && geomActive) {
                    MarkGeomPairSeen(primGeom, sweptGeom);
                }
            }
        }
    }

    const app::EmPartMode inspectMode =
        (viewState.emPartMode != app::EmPartMode::Off) ? viewState.emPartMode : app::EmPartMode::Hitzone;

    auto findOrAddSlot = [&](std::int32_t sortKey, const EmRawGeomCand* src) noexcept -> std::int16_t {
        for (std::uint32_t s = 0; s < g_emInspector.partCount; ++s) {
            if (g_emInspector.parts[s].sortKey == sortKey) {
                if (src) {
                    EmPartSlotInfo& dst = g_emInspector.parts[s];
                    const int curSum = static_cast<int>(dst.slash) + static_cast<int>(dst.blunt) + static_cast<int>(dst.shot);
                    const int newSum = static_cast<int>(src->slash) + static_cast<int>(src->blunt) + static_cast<int>(src->shot);
                    if (src->slash >= 0 && (dst.slash < 0 || newSum > curSum)) {
                        dst.slash   = src->slash;
                        dst.blunt   = src->blunt;
                        dst.shot    = src->shot;
                        dst.fire    = src->fire;
                        dst.water   = src->water;
                        dst.thunder = src->thunder;
                        dst.ice     = src->ice;
                        dst.dragon  = src->dragon;
                        if (src->extract >= 0) dst.extract = src->extract;
                    } else if (dst.fire < 0 && src->fire >= 0) {
                        dst.fire    = src->fire;
                        dst.water   = src->water;
                        dst.thunder = src->thunder;
                        dst.ice     = src->ice;
                        dst.dragon  = src->dragon;
                    }
                    if (dst.extract < 0 && src->extract >= 0) dst.extract = src->extract;
                    if (src->tenderized)   dst.tenderized   = true;
                    if (src->changedState) dst.changedState = true;
                }
                return static_cast<std::int16_t>(s);
            }
        }
        if (g_emInspector.partCount >= static_cast<std::uint32_t>(app::kMaxEmParts)) {
            return static_cast<std::int16_t>(app::kMaxEmParts - 1);
        }
        const std::uint32_t idx = g_emInspector.partCount++;
        EmPartSlotInfo& dst = g_emInspector.parts[idx];
        dst = EmPartSlotInfo{};
        dst.sortKey = sortKey;
        dst.baseGrp = src ? src->baseGrp : -1;
        dst.subGrp  = src ? src->subGrp  : 0;
        dst.zoneId  = -1;
        dst.slash   = src ? src->slash   : -1;
        dst.blunt   = src ? src->blunt   : -1;
        dst.shot    = src ? src->shot    : -1;
        dst.fire    = src ? src->fire    : -1;
        dst.water   = src ? src->water   : -1;
        dst.thunder = src ? src->thunder : -1;
        dst.ice     = src ? src->ice     : -1;
        dst.dragon  = src ? src->dragon  : -1;
        dst.extract = src ? src->extract : -1;
        dst.tenderized   = src ? src->tenderized   : false;
        dst.changedState = src ? src->changedState : false;
        dst.ignoreHit    = src ? src->ignoreHit    : false;
        dst.rideFallback = (inspectMode == app::EmPartMode::ClutchZone && !hasClawTable);
        dst.isNoClaw     = (inspectMode == app::EmPartMode::ClutchZone && hasClawTable && sortKey == 0x1000);
        dst.isAuxCenter  = (sortKey == 0x7FFF00);
        if (inspectMode == app::EmPartMode::ClutchZone) {
            dst.zoneId = dst.rideFallback ? (src ? src->rideZone : 4) : (src ? src->clawZone : -1);
        }
        return static_cast<std::int16_t>(idx);
    };

    auto keyForCand = [&](const EmRawGeomCand& rc) noexcept -> std::int32_t {
        if (rc.baseGrp < 0 || rc.slash < 0) {
            return 0x7FFF00; // AUX CENTER helper sphere (non-damage center volume)
        }
        if (inspectMode == app::EmPartMode::ClutchZone) {
            if (hasClawTable) {
                if (rc.clawZone >= 0) return static_cast<std::int32_t>(rc.clawZone);
                return 0x1000; // Single unified NO-CLAW slot for all unclutchable parts (e.g. Fatalis tail)
            }
            if (rc.rideZone >= 0 && rc.rideZone < 4) {
                return 0x100 + static_cast<std::int32_t>(rc.rideZone);
            }
            return 0x200 + static_cast<std::int32_t>(rc.baseGrp);
        }
        return (static_cast<std::int32_t>(rc.baseGrp) << 8) | static_cast<std::int32_t>(rc.subGrp & 0xFF);
    };

    // Register all Hurtbox slots first
    for (std::uint32_t i = 0; i < rawCnt; ++i) {
        if (g_emRawGeoms[i].cat == VolumeCat::MonsterHurtbox) {
            findOrAddSlot(keyForCand(g_emRawGeoms[i]), &g_emRawGeoms[i]);
        }
    }

    // Sort g_emInspector.parts ascending by sortKey for rock-solid deterministic slot order
    for (std::uint32_t i = 1; i < g_emInspector.partCount; ++i) {
        const EmPartSlotInfo tmp = g_emInspector.parts[i];
        std::uint32_t j = i;
        while (j > 0 && g_emInspector.parts[j - 1].sortKey > tmp.sortKey) {
            g_emInspector.parts[j] = g_emInspector.parts[j - 1];
            --j;
        }
        g_emInspector.parts[j] = tmp;
    }

    // Assign sorted slot indices to Hurtboxes
    for (std::uint32_t i = 0; i < rawCnt; ++i) {
        EmRawGeomCand& rc = g_emRawGeoms[i];
        if (rc.cat != VolumeCat::MonsterHurtbox) continue;
        const std::int32_t wantKey = keyForCand(rc);
        for (std::uint32_t s = 0; s < g_emInspector.partCount; ++s) {
            if (g_emInspector.parts[s].sortKey == wantKey) {
                rc.partSlot = static_cast<std::int16_t>(s);
                break;
            }
        }
    }

    // Bind every Monster Pushbox to its anatomical part (cCollNodeEm -> Joint mJnt0/mJnt1 -> 3D nearest anatomical Hurtbox)
    for (std::uint32_t i = 0; i < rawCnt; ++i) {
        EmRawGeomCand& pg = g_emRawGeoms[i];
        if (pg.cat != VolumeCat::Pushbox) continue;

        if (pg.baseGrp >= 0 && pg.slash >= 0 && g_emInspector.partCount > 0) {
            const std::int32_t pKey = keyForCand(pg);
            for (std::uint32_t s = 0; s < g_emInspector.partCount; ++s) {
                if (g_emInspector.parts[s].sortKey == pKey ||
                    (inspectMode == app::EmPartMode::Hitzone && g_emInspector.parts[s].baseGrp == pg.baseGrp)) {
                    pg.partSlot = static_cast<std::int16_t>(s);
                    break;
                }
            }
        }

        if (pg.partSlot < 0 && g_emInspector.partCount > 0) {
            for (int activePass = 0; activePass < 2 && pg.partSlot < 0; ++activePass) {
                int bestTier = 0;
                float bestScore = 1e30f;
                std::int16_t bestSlot = -1;
                for (std::uint32_t h = 0; h < rawCnt; ++h) {
                    const EmRawGeomCand& hg = g_emRawGeoms[h];
                    // Never bind physical pushboxes to the AUX CENTER helper sphere!
                    if (hg.cat != VolumeCat::MonsterHurtbox || hg.partSlot < 0 || hg.baseGrp < 0 || hg.slash < 0) continue;
                    if (activePass == 0 && !(hg.nodeActive && hg.geomActive)) continue;

                    int tier = 1;
                    if (pg.jnt0 >= 0 && pg.jnt1 >= 0 &&
                        ((pg.jnt0 == hg.jnt0 && pg.jnt1 == hg.jnt1) ||
                         (pg.jnt0 == hg.jnt1 && pg.jnt1 == hg.jnt0))) {
                        tier = 3;
                    } else if ((pg.jnt0 >= 0 && (pg.jnt0 == hg.jnt0 || pg.jnt0 == hg.jnt1)) ||
                               (pg.jnt1 >= 0 && (pg.jnt1 == hg.jnt0 || pg.jnt1 == hg.jnt1))) {
                        tier = 2;
                    }
                    if (tier < bestTier) continue;

                    const float dSq = SegmentSegmentDistSq3D(pg.segA, pg.segB, hg.segA, hg.segB);
                    const float score = std::sqrt(dSq) - 0.30f * (pg.radius + hg.radius);
                    if (tier > bestTier || score < bestScore) {
                        bestTier  = tier;
                        bestScore = score;
                        bestSlot  = hg.partSlot;
                    }
                }
                pg.partSlot = bestSlot;
            }
        }

        if (pg.partSlot < 0) {
            pg.partSlot = findOrAddSlot(0x7FFF00, &pg);
        }
    }

    std::uint32_t vividPaletteIdx = 0;
    for (std::uint32_t s = 0; s < g_emInspector.partCount; ++s) {
        EmPartSlotInfo& ps = g_emInspector.parts[s];
        if (ps.isNoClaw) {
            ps.colorIdx = kEmNoClawColorIdx;
        } else if (ps.isAuxCenter) {
            ps.colorIdx = kEmAuxCenterColorIdx;
        } else {
            ps.colorIdx = static_cast<std::uint8_t>(vividPaletteIdx++ & 15U);
        }
        FormatPartLabels(ps, inspectMode);
    }

    if (viewState.emPartMode == app::EmPartMode::ClutchZone) {
        LogEmClutchTableOnce(em, g_emInspector, scarTbl, g_emRawGeoms, rawCnt);
    } else {
        g_lastLoggedClawEm = 0;
    }

    // Tally active volumes & centroids, and emit filtered/styled ShapeRecords
    const bool filterActive = (viewState.emPartMode != app::EmPartMode::Off);
    for (std::uint32_t i = 0; i < rawCnt; ++i) {
        const EmRawGeomCand& cg = g_emRawGeoms[i];
        if (!cg.nodeActive || !cg.geomActive) continue;

        const std::int16_t slot = cg.partSlot;
        if (slot >= 0 && static_cast<std::uint32_t>(slot) < g_emInspector.partCount) {
            EmPartSlotInfo& ps = g_emInspector.parts[slot];
            const float w = (cg.cat == VolumeCat::MonsterHurtbox)
                                ? ((cg.radius > 10.0f) ? cg.radius : 10.0f)
                                : 0.05f;
            const float mx = 0.5f * (cg.segA[0] + cg.segB[0]);
            const float my = 0.5f * (cg.segA[1] + cg.segB[1]);
            const float mz = 0.5f * (cg.segA[2] + cg.segB[2]);
            ps.anchorSum[0] += mx * w;
            ps.anchorSum[1] += my * w;
            ps.anchorSum[2] += mz * w;
            ps.anchorWeight += w;
            if (cg.cat == VolumeCat::MonsterHurtbox) {
                if (ps.hurtCount < 65535U) ++ps.hurtCount;
            } else {
                if (ps.pushCount < 65535U) ++ps.pushCount;
            }
        }

        if (!emitShapes) continue;

        if (!filterActive) {
            RecordMtGeometry(cg.activeGeom, cg.cat, PlayerDefState::Normal, visMask, snap);
            MarkGeomPairSeen(cg.primGeom, cg.sweptGeom);
            continue;
        }

        const bool partSelected = (slot >= 0 && slot < 32)
                                      ? ((viewState.emPartMask & (1U << static_cast<std::uint32_t>(slot))) != 0U)
                                      : true;
        const std::uint8_t colSlot =
            (slot >= 0 && static_cast<std::uint32_t>(slot) < g_emInspector.partCount)
                ? g_emInspector.parts[slot].colorIdx
                : kEmPartNone;
        if (partSelected) {
            RecordMtGeometry(cg.activeGeom, cg.cat, PlayerDefState::Normal, visMask, snap, colSlot, 0U);
        } else if (viewState.emUnsStyle == app::EmUnselectedStyle::Ghost) {
            RecordMtGeometry(cg.activeGeom, cg.cat, PlayerDefState::Normal, visMask, snap, colSlot, kEmFlagGhost);
        }
        MarkGeomPairSeen(cg.primGeom, cg.sweptGeom);
    }
}

void HarvestMonstersWithPartFilter(std::uintptr_t sEnemy,
                                   bool emitShapes,
                                   std::uintptr_t exeBase,
                                   std::uintptr_t exeEnd,
                                   const app::ViewFilterState& viewState,
                                   RawSnapshot& snap) {
    g_emInspector = EmInspectorSnapshot{};
    if (!IsAlignedPtr(sEnemy)) {
        g_lastEmCounts = EmInspectorCounts{};
        return;
    }

    struct ActiveEmEntry {
        std::uintptr_t em;
        std::uint32_t  emId;
        std::uint32_t  subId;
        bool           isLarge;
        float          distSq;
    };
    ActiveEmEntry list[32]{};
    std::uint32_t cnt = 0;

    const float* refPos = snap.hasPlayerGround ? snap.plFeetPos : (snap.hasCamera ? snap.camPos : nullptr);
    using GetParentFn = std::uintptr_t (*)(std::uintptr_t);
    const auto getParent = reinterpret_cast<GetParentFn>(exeBase + mhw::kRvaAiEmGetParent);
    const auto* catTbl   = reinterpret_cast<const std::uint32_t*>(exeBase + mhw::kRvaEmCategoryTable);

    for (std::size_t i = 0; i < 128 && cnt < 32; ++i) {
        const std::uintptr_t aiEm = *reinterpret_cast<const std::uintptr_t*>(sEnemy + mhw::kOffSEnemyList + i * 8);
        if (!IsAlignedPtr(aiEm)) continue;
        const std::uintptr_t em = getParent(aiEm);
        if (!IsAlignedPtr(em)) continue;
        if ((*reinterpret_cast<const std::uint8_t*>(em + mhw::kOffUnitAliveFlags) & 0x0EU) == 0) continue;

        bool dup = false;
        for (std::uint32_t k = 0; k < cnt; ++k) {
            if (list[k].em == em) { dup = true; break; }
        }
        if (dup) continue;

        const std::uint32_t emId  = *reinterpret_cast<const std::uint32_t*>(em + mhw::kOffEmId);
        const std::uint32_t subId = *reinterpret_cast<const std::uint32_t*>(em + mhw::kOffEmSubId);
        const bool isLarge = (emId < mhw::kEmCategoryTableCount) && (catTbl[emId] == 1U || catTbl[emId] == 2U);

        float dSq = 1e30f;
        if (refPos) {
            const float* ep = reinterpret_cast<const float*>(em + mhw::kOffUCoordPos);
            if (!IsValidVec3(ep) ||
                (std::fabs(ep[0]) < 0.01f && std::fabs(ep[1]) < 0.01f && std::fabs(ep[2]) < 0.01f)) {
                ep = reinterpret_cast<const float*>(em + mhw::kOffUCoordWorldPos);
            }
            if (IsValidVec3(ep)) {
                const float dx = ep[0] - refPos[0];
                const float dy = ep[1] - refPos[1];
                const float dz = ep[2] - refPos[2];
                dSq = dx * dx + dy * dy + dz * dz;
            }
        }

        // Stable insertion: Large Monsters first (in stable sEnemy slot order), then Small Monsters
        std::uint32_t ins = cnt++;
        if (isLarge) {
            while (ins > 0 && !list[ins - 1].isLarge) {
                list[ins] = list[ins - 1];
                --ins;
            }
        }
        list[ins] = {em, emId, subId, isLarge, dSq};
    }

    if (cnt == 0) {
        g_lastEmCounts = EmInspectorCounts{};
        return;
    }

    std::uint32_t targetIdx = 0;
    if (viewState.emTargetScope <= 1) {
        bool anyLarge = list[0].isLarge;
        float bestD = 1e30f;
        for (std::uint32_t k = 0; k < cnt; ++k) {
            if (anyLarge && !list[k].isLarge) break;
            if (list[k].distSq < bestD) {
                bestD = list[k].distSq;
                targetIdx = k;
            }
        }
    } else {
        targetIdx = static_cast<std::uint32_t>(viewState.emTargetScope - 2) % cnt;
    }

    g_emInspector.hasTarget    = true;
    g_emInspector.targetLarge  = list[targetIdx].isLarge;
    g_emInspector.monsterCount = cnt;
    g_emInspector.targetSlot   = targetIdx;
    g_emInspector.emId         = list[targetIdx].emId;
    g_emInspector.subId        = list[targetIdx].subId;
    FormatMonsterIdent(list[targetIdx].em, list[targetIdx].emId, list[targetIdx].subId,
                       exeBase, exeEnd, g_emInspector.targetName);

    const bool filterActive = (viewState.emPartMode != app::EmPartMode::Off);
    const bool soloTarget   = filterActive && (viewState.emTargetScope != 1);
    const bool needInspector = filterActive || (viewState.panelOpen && viewState.panelPage == 1);

    if (!filterActive && !needInspector) {
        if (emitShapes) {
            for (std::uint32_t k = 0; k < cnt; ++k) {
                const std::uintptr_t em = list[k].em;
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterCollPrimary),
                                      OwnerSide::Monster, exeBase, exeEnd, viewState.visMask, snap);
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterColl),
                                      OwnerSide::Monster, exeBase, exeEnd, viewState.visMask, snap);
                HarvestUnitComponents(em, OwnerSide::Monster, exeBase, exeEnd, viewState.visMask, snap);
            }
        }
        g_lastEmCounts = {0U, cnt};
        return;
    }

    // Harvest target monster first (populates g_emInspector.parts), then remaining monsters
    HarvestSingleMonsterPartitioned(list[targetIdx].em, true, soloTarget, emitShapes,
                                    exeBase, exeEnd, viewState, snap);
    if (emitShapes) {
        for (std::uint32_t k = 0; k < cnt; ++k) {
            if (k == targetIdx) continue;
            HarvestSingleMonsterPartitioned(list[k].em, false, soloTarget, emitShapes,
                                            exeBase, exeEnd, viewState, snap);
        }
    }
    g_lastEmCounts = {g_emInspector.partCount, cnt};
}

void HarvestNodeObjectFallback(std::uintptr_t nodeObj,
                               bool fromActiveList,
                               std::uintptr_t exeBase,
                               std::uintptr_t exeEnd,
                               std::uint32_t visMask,
                               RawSnapshot& snap) {
    if (!IsAlignedPtr(nodeObj)) return;

    const std::uintptr_t userData = *reinterpret_cast<const std::uintptr_t*>(nodeObj + mhw::kOffNodeObjUserData);
    if (IsAlignedPtr(userData)) {
        const std::uintptr_t parentColl = *reinterpret_cast<const std::uintptr_t*>(userData + mhw::kOffCNodeParentColl);
        if (IsCpObjCollision(parentColl, exeBase, exeEnd)) {
            HarvestCpObjCollision(parentColl, OwnerSide::Unknown, exeBase, exeEnd, visMask, snap);
            return;
        }
    }

    const std::uint32_t geomCount = *reinterpret_cast<const std::uint32_t*>(nodeObj + mhw::kOffNodeObjGeomCount);
    const std::uintptr_t geomArr  = *reinterpret_cast<const std::uintptr_t*>(nodeObj + mhw::kOffNodeObjGeomArray);
    if (geomCount == 0 || geomCount > 128 || !IsAlignedPtr(geomArr)) return;

    const std::uintptr_t owner = *reinterpret_cast<const std::uintptr_t*>(nodeObj + mhw::kOffNodeObjOwner);
    OwnerSide side = OwnerSide::Unknown;
    if (IsAlignedPtr(owner)) {
        if (IsCpObjCollision(owner, exeBase, exeEnd)) {
            HarvestCpObjCollision(owner, OwnerSide::Unknown, exeBase, exeEnd, visMask, snap);
            return;
        }
        side = ResolveUnitSide(owner, exeBase, exeEnd);
    }

    const std::uint32_t attr = *reinterpret_cast<const std::uint32_t*>(nodeObj + mhw::kOffNodeObjAttr);
    const bool isAttack = fromActiveList && ((attr & 0x02U) != 0);
    const VolumeCat cat = DetermineNodeCategory(side, isAttack, attr);
    if ((visMask & CategoryBitForVolume(cat)) == 0) return;

    for (std::uint32_t g = 0; g < geomCount; ++g) {
        const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
        if (!IsAlignedPtr(cgeom)) continue;
        if (*reinterpret_cast<const std::uint8_t*>(cgeom + 0x08) == 0) continue;
        const std::uintptr_t mtGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + 0x10);
        RecordMtGeometry(mtGeom, cat, PlayerDefState::Normal, visMask, snap);
    }
}

// ============================================================================
//  .SBC (rCollision) Stage Terrain / Wall / Ledge Mesh Harvester
// ============================================================================

inline void TransformPointRowMajor(const float v[3], const float* m, float out[3]) noexcept {
    if (!m) {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        return;
    }
    out[0] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8]  + m[12];
    out[1] = v[0] * m[1] + v[1] * m[5] + v[2] * m[9]  + m[13];
    out[2] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
}

inline bool AabbOverlapsQueryBox(const float localMin[3],
                                 const float localMax[3],
                                 const float* mat,
                                 const float center[3],
                                 const float qMin[3],
                                 const float qMax[3],
                                 float& outDistSq) noexcept {
    if (!IsValidVec3(localMin) || !IsValidVec3(localMax)) return false;
    float minW[3], maxW[3], wcx, wcy, wcz;
    if (!mat) {
        if (localMax[0] < localMin[0] || localMax[1] < localMin[1] || localMax[2] < localMin[2]) return false;
        minW[0] = localMin[0]; minW[1] = localMin[1]; minW[2] = localMin[2];
        maxW[0] = localMax[0]; maxW[1] = localMax[1]; maxW[2] = localMax[2];
        wcx = 0.5f * (minW[0] + maxW[0]);
        wcy = 0.5f * (minW[1] + maxW[1]);
        wcz = 0.5f * (minW[2] + maxW[2]);
    } else {
        // Transform local AABB center + extents by 4x4 row-major matrix (exact MtOBB::transform bound)
        const float cx = 0.5f * (localMin[0] + localMax[0]);
        const float cy = 0.5f * (localMin[1] + localMax[1]);
        const float cz = 0.5f * (localMin[2] + localMax[2]);
        const float ex = 0.5f * (localMax[0] - localMin[0]);
        const float ey = 0.5f * (localMax[1] - localMin[1]);
        const float ez = 0.5f * (localMax[2] - localMin[2]);
        if (ex < 0.0f || ey < 0.0f || ez < 0.0f) return false;

        wcx = cx * mat[0] + cy * mat[4] + cz * mat[8]  + mat[12];
        wcy = cx * mat[1] + cy * mat[5] + cz * mat[9]  + mat[13];
        wcz = cx * mat[2] + cy * mat[6] + cz * mat[10] + mat[14];

        const float wex = ex * std::fabs(mat[0]) + ey * std::fabs(mat[4]) + ez * std::fabs(mat[8]);
        const float wey = ex * std::fabs(mat[1]) + ey * std::fabs(mat[5]) + ez * std::fabs(mat[9]);
        const float wez = ex * std::fabs(mat[2]) + ey * std::fabs(mat[6]) + ez * std::fabs(mat[10]);

        minW[0] = wcx - wex; maxW[0] = wcx + wex;
        minW[1] = wcy - wey; maxW[1] = wcy + wey;
        minW[2] = wcz - wez; maxW[2] = wcz + wez;
    }

    if (maxW[0] < qMin[0] || minW[0] > qMax[0] ||
        maxW[1] < qMin[1] || minW[1] > qMax[1] ||
        maxW[2] < qMin[2] || minW[2] > qMax[2]) {
        return false;
    }

    const float nx = (center[0] < minW[0]) ? minW[0] : ((center[0] > maxW[0]) ? maxW[0] : center[0]);
    const float ny = (center[1] < minW[1]) ? minW[1] : ((center[1] > maxW[1]) ? maxW[1] : center[1]);
    const float nz = (center[2] < minW[2]) ? minW[2] : ((center[2] > maxW[2]) ? maxW[2] : center[2]);
    const float dx = nx - center[0], dy = ny - center[1], dz = nz - center[2];
    const float cdx = wcx - center[0], cdy = wcy - center[1], cdz = wcz - center[2];
    outDistSq = (dx * dx + dy * dy + dz * dz) + 1e-5f * (cdx * cdx + cdy * cdy + cdz * cdz);
    return true;
}

inline std::uint32_t HashQuantizedEndpoint(const float v[3]) noexcept {
    // Branchless 2cm integer quantization (zero std::floor calls!)
    const auto qx = static_cast<std::uint32_t>(static_cast<std::int32_t>(v[0] * 0.5f));
    const auto qy = static_cast<std::uint32_t>(static_cast<std::int32_t>(v[1] * 0.5f));
    const auto qz = static_cast<std::uint32_t>(static_cast<std::int32_t>(v[2] * 0.5f));
    std::uint32_t h = (qx * 73856093U) ^ (qy * 19349669U) ^ (qz * 83492791U) ^ 0x9E3779B9U;
    h ^= (h >> 16);
    h *= 0x85EBCA6BU;
    h ^= (h >> 13);
    return (h == 0U) ? 1U : h;
}

void InsertSbcEdgeDedup(const float a[3],
                        const float b[3],
                        float unx,
                        float uny,
                        float unz,
                        float fade,
                        std::uint8_t isWall,
                        std::uint8_t layer,
                        std::uint32_t edgeGenShifted,
                        RawSnapshot& snap) noexcept {
    const std::uint32_t ha = HashQuantizedEndpoint(a);
    const std::uint32_t hb = HashQuantizedEndpoint(b);
    if (ha == hb) return;
    const std::uint32_t lo = (ha < hb) ? ha : hb;
    const std::uint32_t hi = (ha < hb) ? hb : ha;
    std::uint32_t key = (lo * 0x9E3779B9U) ^ (hi + 0x7F4A7C15U) ^ (static_cast<std::uint32_t>(layer) * 0x517CC1B7U);
    key ^= (key >> 16);
    if (key == 0U) key = 1U;

    std::uint32_t slot = key & (kEdgeHashSize - 1U);
    for (std::uint32_t probe = 0; probe < 12; ++probe) {
        SbcEdgeSlot& hs = g_sbcEdgeHash[slot];
        const std::uint32_t ig = hs.idxAndGen;
        if ((ig & 0xFFF00000U) == edgeGenShifted) {
            if (hs.key32 == key) {
                const std::uint32_t idx = ig & 0x000FFFFFU;
                if (idx < snap.sbcEdgeCount) {
                    SbcEdgeRecord& rec = g_sbcEdges[idx];
                    if (rec.count < 255U) ++rec.count;
                    if (fade > rec.fade) rec.fade = fade;

                    // Winding-invariant dihedral crease test: |dot(N0, N1)|
                    const float absDot = std::fabs(rec.nx0 * unx + rec.ny0 * uny + rec.nz0 * unz);
                    std::uint8_t c = 0;
                    if (absDot < 0.910f || rec.isWall != isWall) {
                        c = 2; // Sharp corner (>24 deg) OR floor-meets-wall ledge!
                    } else if (absDot < 0.992f) {
                        c = 1; // Gentle slope/ridge contour (7..24 deg)
                    }
                    if (c > rec.crease) rec.crease = c;
                    if (isWall) rec.isWall = 1;
                }
                return;
            }
        } else {
            if (snap.sbcEdgeCount >= kMaxSbcEdges) return;
            const std::uint32_t idx = snap.sbcEdgeCount++;
            hs.key32     = key;
            hs.idxAndGen = edgeGenShifted | (idx & 0x000FFFFFU);
            SbcEdgeRecord& rec = g_sbcEdges[idx];
            rec.x0     = a[0]; rec.y0  = a[1]; rec.z0  = a[2];
            rec.x1     = b[0]; rec.y1  = b[1]; rec.z1  = b[2];
            rec.nx0    = unx;  rec.ny0 = uny;  rec.nz0 = unz;
            rec.fade   = fade;
            rec.isWall = isWall;
            rec.count  = 1U;
            rec.crease = 0U;
            rec.layer  = layer;
            return;
        }
        slot = (slot + 1U) & (kEdgeHashSize - 1U);
    }
}

// Exact 3D closest point on triangle (a, b, c) to query point p (Ericson RTCD 5.1.5, division-free interior!)
inline void ClosestPointOnTriangle3D(const float p[3],
                                     const float a[3],
                                     const float b[3],
                                     const float c[3],
                                     float unx,
                                     float uny,
                                     float unz,
                                     float outQ[3]) noexcept {
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    const float apx = p[0] - a[0], apy = p[1] - a[1], apz = p[2] - a[2];

    const float d1 = abx * apx + aby * apy + abz * apz;
    const float d2 = acx * apx + acy * apy + acz * apz;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        outQ[0] = a[0]; outQ[1] = a[1]; outQ[2] = a[2];
        return;
    }

    const float bpx = p[0] - b[0], bpy = p[1] - b[1], bpz = p[2] - b[2];
    const float d3 = abx * bpx + aby * bpy + abz * bpz;
    const float d4 = acx * bpx + acy * bpy + acz * bpz;
    if (d3 >= 0.0f && d4 <= d3) {
        outQ[0] = b[0]; outQ[1] = b[1]; outQ[2] = b[2];
        return;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        const float v = (denom > 1e-6f) ? (d1 / denom) : 0.0f;
        outQ[0] = a[0] + v * abx;
        outQ[1] = a[1] + v * aby;
        outQ[2] = a[2] + v * abz;
        return;
    }

    const float cpx = p[0] - c[0], cpy = p[1] - c[1], cpz = p[2] - c[2];
    const float d5 = abx * cpx + aby * cpy + abz * cpz;
    const float d6 = acx * cpx + acy * cpy + acz * cpz;
    if (d6 >= 0.0f && d5 <= d6) {
        outQ[0] = c[0]; outQ[1] = c[1]; outQ[2] = c[2];
        return;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        const float w = (denom > 1e-6f) ? (d2 / denom) : 0.0f;
        outQ[0] = a[0] + w * acx;
        outQ[1] = a[1] + w * acy;
        outQ[2] = a[2] + w * acz;
        return;
    }

    const float va   = d3 * d6 - d5 * d4;
    const float d4m3 = d4 - d3;
    const float d5m6 = d5 - d6;
    if (va <= 0.0f && d4m3 >= 0.0f && d5m6 >= 0.0f) {
        const float denom = d4m3 + d5m6;
        const float w = (denom > 1e-6f) ? (d4m3 / denom) : 0.0f;
        outQ[0] = b[0] + w * (c[0] - b[0]);
        outQ[1] = b[1] + w * (c[1] - b[1]);
        outQ[2] = b[2] + w * (c[2] - b[2]);
        return;
    }

    // Interior face region: orthogonal projection onto plane using precomputed unit normal
    const float distPlane = apx * unx + apy * uny + apz * unz;
    outQ[0] = p[0] - distPlane * unx;
    outQ[1] = p[1] - distPlane * uny;
    outQ[2] = p[2] - distPlane * unz;
}

void HarvestStageSbcMeshes(std::uintptr_t sColl,
                           float radiusWorldUnits,
                           app::SbcMeshTier sbcTier,
                           bool includeAux,
                           RawSnapshot& snap) {
    if (!IsAlignedPtr(sColl) || !snap.hasCamera) return;

    const float* camPos = snap.camPos;
    const bool validCam = IsValidVec3(camPos);
    float center[3] = {camPos[0], camPos[1], camPos[2]};
    bool followPlayer = false;
    float camToPlDist = 0.0f;
    if (snap.hasPlayerGround) {
        const float dx = snap.plFeetPos[0] - camPos[0];
        const float dy = snap.plFeetPos[1] - camPos[1];
        const float dz = snap.plFeetPos[2] - camPos[2];
        const float dSq = dx * dx + dy * dy + dz * dz;
        // Follow player normally, but switch to camera automatically if FreeCam moved > 60m away!
        if (!validCam || dSq <= (6000.0f * 6000.0f)) {
            center[0]    = snap.plFeetPos[0];
            center[1]    = snap.plFeetPos[1] + 80.0f; // Waist height
            center[2]    = snap.plFeetPos[2];
            followPlayer = true;
            camToPlDist  = std::sqrt(dSq);
        }
    }
    if (!IsValidVec3(center)) return;

    const std::uint32_t sbcCount  = *reinterpret_cast<const std::uint32_t*>(sColl + mhw::kOffSCollSbcCount);
    const std::uintptr_t sbcArr   = *reinterpret_cast<const std::uintptr_t*>(sColl + mhw::kOffSCollSbcArray);
    std::uint32_t matRingSlot     = *reinterpret_cast<const std::uint32_t*>(sColl + mhw::kOffSCollSbcMatSlot);
    if (matRingSlot >= 3U) matRingSlot = 0U;
    if (sbcCount == 0 || sbcCount > 512 || !IsAlignedPtr(sbcArr)) {
        g_sbcCache.valid = false;
        return;
    }

    // Track per-frame travel speed to throttle harvest rate during Wingdrake flight (> ~7 m/s)
    bool fastMoving = false;
    if (g_sbcCache.hasPrevCtr) {
        const float vx = center[0] - g_sbcCache.prevCenter[0];
        const float vy = center[1] - g_sbcCache.prevCenter[1];
        const float vz = center[2] - g_sbcCache.prevCenter[2];
        const float stepSq = vx * vx + vy * vy + vz * vz;
        // Wingdrake flies at ~20-40 cm/frame; skip excessive rebuilds while flying fast,
        // but immediately refresh on teleport (> 8m single-frame jump)!
        fastMoving = (stepSq > (12.0f * 12.0f)) && (stepSq < (800.0f * 800.0f));
    }
    g_sbcCache.prevCenter[0] = center[0];
    g_sbcCache.prevCenter[1] = center[1];
    g_sbcCache.prevCenter[2] = center[2];
    g_sbcCache.hasPrevCtr    = true;

    const bool wantLow  = (sbcTier != app::SbcMeshTier::HighOnly);
    const bool wantHigh = (sbcTier == app::SbcMeshTier::HighOnly) ||
                          (sbcTier == app::SbcMeshTier::Both && includeAux);

    // High-poly *_col_eff.sbc has 10-20x higher triangle density than Low-poly *_col.sbc.
    // Capping High-poly's smooth fade sphere ensures all in-range triangles fit cleanly inside
    // its dedicated budget so High-poly chunks NEVER overflow and pop in/out while walking!
    const float highMaxRad = (sbcTier == app::SbcMeshTier::HighOnly) ? 3200.0f : 2200.0f;
    const float highRadius = (radiusWorldUnits < highMaxRad) ? radiusWorldUnits : highMaxRad;

    constexpr float kVertWeight = 0.60f;
    const float vertRadius = radiusWorldUnits * (1.0f / kVertWeight);
    const float camKeepRad = (radiusWorldUnits < 1800.0f) ? radiusWorldUnits : 1800.0f;
    const bool  needCamKeep = followPlayer && validCam && ((camToPlDist + camKeepRad) > radiusWorldUnits);

    float qMin[3] = {
        center[0] - radiusWorldUnits,
        center[1] - vertRadius,
        center[2] - radiusWorldUnits
    };
    float qMax[3] = {
        center[0] + radiusWorldUnits,
        center[1] + vertRadius,
        center[2] + radiusWorldUnits
    };
    if (needCamKeep) {
        if (camPos[0] - camKeepRad < qMin[0]) qMin[0] = camPos[0] - camKeepRad;
        if (camPos[1] - camKeepRad < qMin[1]) qMin[1] = camPos[1] - camKeepRad;
        if (camPos[2] - camKeepRad < qMin[2]) qMin[2] = camPos[2] - camKeepRad;
        if (camPos[0] + camKeepRad > qMax[0]) qMax[0] = camPos[0] + camKeepRad;
        if (camPos[1] + camKeepRad > qMax[1]) qMax[1] = camPos[1] + camKeepRad;
        if (camPos[2] + camKeepRad > qMax[2]) qMax[2] = camPos[2] + camKeepRad;
    }

    // Padded box (+4m) for localized stage fingerprint: distant animated props / Wingdrakes 300m away
    // NEVER invalidate our cache, while nearby moving elevators/boulders trigger an instant refresh!
    const float fpMin[3] = {qMin[0] - 400.0f, qMin[1] - 400.0f, qMin[2] - 400.0f};
    const float fpMax[3] = {qMax[0] + 400.0f, qMax[1] + 400.0f, qMax[2] + 400.0f};

    // Fast-travel Wingdrake throttle: while flying fast over a streaming map, refresh at ~7.5 Hz
    // instead of hammering full .sbc rebuilds every frame!
    const bool cacheMetaMatch =
        g_sbcCache.valid &&
        g_sbcCache.sColl == sColl &&
        g_sbcCache.sbcCount == sbcCount &&
        g_sbcCache.radius == radiusWorldUnits &&
        g_sbcCache.sbcTier == sbcTier &&
        g_sbcCache.includeAux == includeAux;

    if (cacheMetaMatch && fastMoving && g_sbcCache.ageFrames < 8U) {
        ++g_sbcCache.ageFrames;
        snap.sbcTriCount  = g_sbcCache.triCount;
        snap.sbcEdgeCount = g_sbcCache.edgeCount;
        return;
    }

    // Compute localized ~2 us fingerprint ONLY over .sbc slots overlapping fpMin..fpMax
    std::uint64_t fp = static_cast<std::uint64_t>(sbcCount) * 0x9E3779B97F4A7C15ULL;
    for (std::uint32_t i = 0; i < sbcCount; ++i) {
        const std::uintptr_t sbc = *reinterpret_cast<const std::uintptr_t*>(sbcArr + i * 8);
        if (!IsAlignedPtr(sbc)) continue;
        const std::uint8_t active = *reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcActive);
        if (active == 0) continue;

        const std::uintptr_t rColl = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcResource);
        if (!IsAlignedPtr(rColl)) continue;

        const float* sbcMat = nullptr;
        if (*reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcHasMatrix) != 0) {
            std::uintptr_t mp = *reinterpret_cast<const std::uintptr_t*>(
                sbc + mhw::kOffSbcMatrixPtr + static_cast<std::size_t>(matRingSlot) * 8U);
            if (!IsAlignedPtr(mp)) mp = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcMatrixPtr);
            if (IsAlignedPtr(mp)) sbcMat = reinterpret_cast<const float*>(mp);
        }

        const std::uint32_t sbcPartsCnt = *reinterpret_cast<const std::uint32_t*>(sbc + mhw::kOffSbcPartsCount);
        if (sbcPartsCnt == 0) {
            const float* sbcMin = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMin);
            const float* sbcMax = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMax);
            float dummyD = 0.0f;
            if (!AabbOverlapsQueryBox(sbcMin, sbcMax, sbcMat, center, fpMin, fpMax, dummyD)) continue;
        }

        fp = (fp * 0xBF58476D1CE4E5B9ULL) ^ rColl ^ (static_cast<std::uint64_t>(i) << 40);
        if (sbcMat) {
            const auto* m = reinterpret_cast<const std::uint32_t*>(sbcMat);
            fp ^= static_cast<std::uint64_t>(m[12]) ^
                  (static_cast<std::uint64_t>(m[13]) << 17) ^
                  (static_cast<std::uint64_t>(m[14]) << 33);
        }
        const std::uintptr_t sbcPartsArr = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcPartsArray);
        if (sbcPartsCnt > 0 && sbcPartsCnt <= 256 && IsAlignedPtr(sbcPartsArr)) {
            for (std::uint32_t p = 0; p < sbcPartsCnt; ++p) {
                const std::uintptr_t sp = sbcPartsArr + static_cast<std::size_t>(p) * mhw::kSbcPartStride;
                const std::uint8_t en = *reinterpret_cast<const std::uint8_t*>(sp + mhw::kOffSbcPartEnabled);
                fp ^= (static_cast<std::uint64_t>(en) << ((p + i) & 55U));
                if (en != 0 && *reinterpret_cast<const std::uint8_t*>(sp + mhw::kOffSbcPartHasMatrix) != 0) {
                    std::uintptr_t pmp = *reinterpret_cast<const std::uintptr_t*>(
                        sp + mhw::kOffSbcPartMatrixPtr + static_cast<std::size_t>(matRingSlot) * 8U);
                    if (!IsAlignedPtr(pmp)) pmp = *reinterpret_cast<const std::uintptr_t*>(sp + mhw::kOffSbcPartMatrixPtr);
                    if (IsAlignedPtr(pmp)) {
                        const auto* pm = reinterpret_cast<const float*>(pmp);
                        if (pm[12] >= fpMin[0] && pm[12] <= fpMax[0] &&
                            pm[13] >= fpMin[1] && pm[13] <= fpMax[1] &&
                            pm[14] >= fpMin[2] && pm[14] <= fpMax[2]) {
                            const auto* m = reinterpret_cast<const std::uint32_t*>(pmp);
                            fp ^= static_cast<std::uint64_t>(m[12]) ^
                                  (static_cast<std::uint64_t>(m[13]) << 17) ^
                                  (static_cast<std::uint64_t>(m[14]) << 33);
                        }
                    }
                }
            }
        }
    }

    if (cacheMetaMatch && g_sbcCache.fingerprint == fp && g_sbcCache.ageFrames < 120U) {
        const float cdx = center[0] - g_sbcCache.center[0];
        const float cdy = center[1] - g_sbcCache.center[1];
        const float cdz = center[2] - g_sbcCache.center[2];
        const bool centerOk = (cdx * cdx + cdy * cdy + cdz * cdz) <= (220.0f * 220.0f);
        bool camOk = true;
        if (needCamKeep) {
            const float mdx = camPos[0] - g_sbcCache.camPos[0];
            const float mdy = camPos[1] - g_sbcCache.camPos[1];
            const float mdz = camPos[2] - g_sbcCache.camPos[2];
            camOk = (mdx * mdx + mdy * mdy + mdz * mdz) <= (350.0f * 350.0f);
        }
        if (centerOk && camOk) {
            ++g_sbcCache.ageFrames;
            snap.sbcTriCount  = g_sbcCache.triCount;
            snap.sbcEdgeCount = g_sbcCache.edgeCount;
            return;
        }
    }

    const float invLowRadSq  = 1.0f / (radiusWorldUnits * radiusWorldUnits + 1.0f);
    const float invHighRadSq = 1.0f / (highRadius * highRadius + 1.0f);
    const float invCamKeepSq = 1.0f / (camKeepRad * camKeepRad + 1.0f);
    const float refFloorY    = snap.hasPlayerGround ? snap.plFeetPos[1] : center[1];

    const float highVertRad  = highRadius * (1.0f / kVertWeight);
    const float hqMin[3]     = {center[0] - highRadius, center[1] - highVertRad, center[2] - highRadius};
    const float hqMax[3]     = {center[0] + highRadius, center[1] + highVertRad, center[2] + highRadius};

    if (++g_sbcEdgeGen >= 4096U) {
        std::memset(g_sbcEdgeHash, 0, sizeof(g_sbcEdgeHash));
        g_sbcEdgeGen = 1U;
    }
    const std::uint32_t edgeGenShifted = (g_sbcEdgeGen & 0xFFFU) << 20;

    // 1. Gather overlapping Low-poly (*_col.sbc) and High-poly (*_col_eff.sbc) stage parts
    std::uint32_t candCount = 0;
    for (std::uint32_t i = 0; i < sbcCount && candCount < kMaxSbcPartCands; ++i) {
        const std::uintptr_t sbc = *reinterpret_cast<const std::uintptr_t*>(sbcArr + i * 8);
        if (!IsAlignedPtr(sbc)) continue;
        if (*reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcActive) == 0) continue;

        const std::uintptr_t rColl = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcResource);
        if (!IsAlignedPtr(rColl)) continue;

        const auto* magic = reinterpret_cast<const char*>(rColl + mhw::kOffRCollMagic);
        if (magic[0] != 'S' || magic[1] != 'B' || magic[2] != 'C') continue;

        const std::uint16_t numParts      = *reinterpret_cast<const std::uint16_t*>(rColl + mhw::kOffRCollNumParts);
        const std::uint16_t numAttrs      = *reinterpret_cast<const std::uint16_t*>(rColl + mhw::kOffRCollNumAttrs);
        const std::uint32_t totalTriNodes = *reinterpret_cast<const std::uint32_t*>(rColl + mhw::kOffRCollNumTriNodes);
        const std::uint32_t totalVerts    = *reinterpret_cast<const std::uint32_t*>(rColl + mhw::kOffRCollNumVerts);
        const std::uintptr_t partsInfoArr = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollPartsInfo);
        const std::uintptr_t attrsInfoArr = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollAttrsInfo);
        const std::uintptr_t triNodesBase = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollTriNodes);
        const std::uintptr_t vertArrBase  = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollVertices);

        if (numParts == 0 || numParts > 4096 ||
            totalTriNodes == 0 || totalTriNodes > 2000000U ||
            totalVerts == 0 || totalVerts > 2000000U ||
            !IsAlignedPtr(partsInfoArr) || !IsAlignedPtr(triNodesBase) || !IsAlignedPtr(vertArrBase)) {
            continue;
        }
        const std::uintptr_t validAttrsBase = (numAttrs > 0 && IsAlignedPtr(attrsInfoArr)) ? attrsInfoArr : 0U;

        const std::uint32_t sbcType  = *reinterpret_cast<const std::uint32_t*>(sbc + mhw::kOffSbcType);
        const std::uint32_t sbcGroup = *reinterpret_cast<const std::uint32_t*>(sbc + mhw::kOffSbcGroup);
        const bool isHigh = IsHighCollisionSbc(sbcType, sbcGroup, rColl, validAttrsBase, numAttrs);
        if (isHigh && !wantHigh) continue;
        if (!isHigh && !wantLow) continue;

        const float* sbcMat = nullptr;
        if (*reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcHasMatrix) != 0) {
            std::uintptr_t mp = *reinterpret_cast<const std::uintptr_t*>(
                sbc + mhw::kOffSbcMatrixPtr + static_cast<std::size_t>(matRingSlot) * 8U);
            if (!IsAlignedPtr(mp)) {
                mp = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcMatrixPtr);
            }
            if (IsAlignedPtr(mp)) sbcMat = reinterpret_cast<const float*>(mp);
        }

        const float* boxMin = isHigh ? hqMin : qMin;
        const float* boxMax = isHigh ? hqMax : qMax;

        const std::uintptr_t sbcPartsArr = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcPartsArray);
        const std::uint32_t  sbcPartsCnt = *reinterpret_cast<const std::uint32_t*>(sbc + mhw::kOffSbcPartsCount);

        if (sbcPartsCnt == 0) {
            const float* sbcMin = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMin);
            const float* sbcMax = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMax);
            float dummyDistSq = 0.0f;
            if (!AabbOverlapsQueryBox(sbcMin, sbcMax, sbcMat, center, boxMin, boxMax, dummyDistSq)) continue;
        }

        for (std::uint16_t p = 0; p < numParts && candCount < kMaxSbcPartCands; ++p) {
            const float* activeMat = sbcMat;
            if (p < sbcPartsCnt && IsAlignedPtr(sbcPartsArr)) {
                const std::uintptr_t sbcPart = sbcPartsArr + static_cast<std::size_t>(p) * mhw::kSbcPartStride;
                if (*reinterpret_cast<const std::uint8_t*>(sbcPart + mhw::kOffSbcPartEnabled) == 0) {
                    continue;
                }
                if (*reinterpret_cast<const std::uint8_t*>(sbcPart + mhw::kOffSbcPartHasMatrix) != 0) {
                    std::uintptr_t pmp = *reinterpret_cast<const std::uintptr_t*>(
                        sbcPart + mhw::kOffSbcPartMatrixPtr + static_cast<std::size_t>(matRingSlot) * 8U);
                    if (!IsAlignedPtr(pmp)) {
                        pmp = *reinterpret_cast<const std::uintptr_t*>(sbcPart + mhw::kOffSbcPartMatrixPtr);
                    }
                    if (IsAlignedPtr(pmp)) activeMat = reinterpret_cast<const float*>(pmp);
                }
            }

            const std::uintptr_t pInfo = partsInfoArr + static_cast<std::size_t>(p) * mhw::kRCollPartInfoStride;
            const float* pMin = reinterpret_cast<const float*>(pInfo + mhw::kOffPartInfoAabbMin);
            const float* pMax = reinterpret_cast<const float*>(pInfo + mhw::kOffPartInfoAabbMax);
            float partDistSq = 0.0f;
            if (!AabbOverlapsQueryBox(pMin, pMax, activeMat, center, boxMin, boxMax, partDistSq)) continue;

            const std::uint32_t startTri  = *reinterpret_cast<const std::uint32_t*>(pInfo + mhw::kOffPartInfoStartTri);
            const std::uint32_t endTri    = (static_cast<std::uint32_t>(p) + 1U < numParts)
                                                ? *reinterpret_cast<const std::uint32_t*>(pInfo + mhw::kRCollPartInfoStride + mhw::kOffPartInfoStartTri)
                                                : totalTriNodes;
            const std::uint32_t startVert = *reinterpret_cast<const std::uint32_t*>(pInfo + mhw::kOffPartInfoStartVert);
            if (startTri >= endTri || endTri > totalTriNodes) continue;

            SbcPartCand& pc = g_sbcPartCands[candCount++];
            pc.activeMat    = activeMat;
            pc.triNodesBase = triNodesBase;
            pc.vertArrBase  = vertArrBase;
            pc.attrsBase    = validAttrsBase;
            pc.totalVerts   = totalVerts;
            pc.startTri     = startTri;
            pc.endTri       = endTri;
            pc.startVert    = startVert;
            pc.numAttrs     = numAttrs;
            pc.isHighSbc    = isHigh ? 1U : 0U;
            pc.distSq       = partDistSq;
        }
    }

    // In-place Shell sort: Low-poly parts FIRST (Near-to-Far), then High-poly parts (Near-to-Far)
    // so Low-poly gameplay terrain NEVER gets starved by dense High-poly meshes!
    for (std::uint32_t gap = candCount >> 1; gap > 0; gap >>= 1) {
        for (std::uint32_t i = gap; i < candCount; ++i) {
            const SbcPartCand tmp = g_sbcPartCands[i];
            std::uint32_t j = i;
            while (j >= gap &&
                   (g_sbcPartCands[j - gap].isHighSbc > tmp.isHighSbc ||
                    (g_sbcPartCands[j - gap].isHighSbc == tmp.isHighSbc &&
                     g_sbcPartCands[j - gap].distSq > tmp.distSq))) {
                g_sbcPartCands[j] = g_sbcPartCands[j - gap];
                j -= gap;
            }
            g_sbcPartCands[j] = tmp;
        }
    }

    // Dedicated per-tier budgets (prevents High-poly from starving Low-poly AND prevents chunk-popping!)
    const std::uint32_t lowTriLimit  = (sbcTier == app::SbcMeshTier::LowOnly) ? kMaxSbcTris : 49152U;
    const std::uint32_t highTriLimit = (sbcTier == app::SbcMeshTier::HighOnly) ? kMaxSbcTris : 49152U;
    std::uint32_t lowTriHarvested  = 0;
    std::uint32_t highTriHarvested = 0;

    // 2. Harvest triangles & creases from nearest parts outward
    for (std::uint32_t ci = 0; ci < candCount && snap.sbcTriCount < kMaxSbcTris; ++ci) {
        const SbcPartCand& pc             = g_sbcPartCands[ci];
        const bool isHighPart             = (pc.isHighSbc != 0U);
        if (!isHighPart && lowTriHarvested >= lowTriLimit) continue;
        if (isHighPart && highTriHarvested >= highTriLimit) break;

        const float* activeMat            = pc.activeMat;
        const std::uintptr_t triNodesBase = pc.triNodesBase;
        const std::uintptr_t vertArrBase  = pc.vertArrBase;
        const std::uintptr_t attrsBase    = pc.attrsBase;
        const std::uint32_t totalVerts    = pc.totalVerts;
        const std::uint32_t startVert     = pc.startVert;
        const std::uint16_t numAttrs      = pc.numAttrs;

        const float* boxMin   = isHighPart ? hqMin : qMin;
        const float* boxMax   = isHighPart ? hqMax : qMax;
        const float  invRadSq = isHighPart ? invHighRadSq : invLowRadSq;
        const bool   camProt  = (!isHighPart) && needCamKeep;
        const bool   promoteHighPrimary = (sbcTier == app::SbcMeshTier::HighOnly);
        // Skip microscopic high-poly edges in Both mode (cuts L2/CPU load by ~90% during Wingdrake flight!)
        const float  minEdgeAreaSq = isHighPart ? (promoteHighPrimary ? 400.0f : 40000.0f) : 0.0f;

        for (std::uint32_t t = pc.startTri; t < pc.endTri && snap.sbcTriCount < kMaxSbcTris; ++t) {
            if (!isHighPart && lowTriHarvested >= lowTriLimit) break;
            if (isHighPart && highTriHarvested >= highTriLimit) break;

            const std::uintptr_t tri = triNodesBase + static_cast<std::size_t>(t) * mhw::kRCollTriStride;

            std::uint8_t layer = kSbcLayerSolid;
            if (isHighPart) {
                layer = promoteHighPrimary ? kSbcLayerSolid : kSbcLayerAuxSbc;
            } else {
                std::uint32_t attrFlags = 0x3FFFFFFFU;
                if (attrsBase != 0U) {
                    const std::uint16_t attrIdx = *reinterpret_cast<const std::uint16_t*>(tri + mhw::kOffTriAttrIndex);
                    if (attrIdx < numAttrs) {
                        attrFlags = *reinterpret_cast<const std::uint32_t*>(
                            attrsBase + static_cast<std::size_t>(attrIdx) * mhw::kRCollAttrStride);
                    }
                }
                layer = ClassifySbcLayer(false, attrFlags);
                if (layer != kSbcLayerSolid && !includeAux) continue;
            }

            const auto* idx = reinterpret_cast<const std::uint16_t*>(tri + mhw::kOffTriVertIndices);
            const std::uint32_t vi0 = startVert + idx[0];
            const std::uint32_t vi1 = startVert + idx[1];
            const std::uint32_t vi2 = startVert + idx[2];
            if (vi0 >= totalVerts || vi1 >= totalVerts || vi2 >= totalVerts) continue;

            float w0[3], w1[3], w2[3];
            TransformPointRowMajor(reinterpret_cast<const float*>(vertArrBase + static_cast<std::size_t>(vi0) * 0x10), activeMat, w0);
            TransformPointRowMajor(reinterpret_cast<const float*>(vertArrBase + static_cast<std::size_t>(vi1) * 0x10), activeMat, w1);
            TransformPointRowMajor(reinterpret_cast<const float*>(vertArrBase + static_cast<std::size_t>(vi2) * 0x10), activeMat, w2);
            if (!IsValidVec3(w0) || !IsValidVec3(w1) || !IsValidVec3(w2)) continue;

            // Fast broadphase triangle AABB vs query box
            const float minX = (w0[0] < w1[0]) ? ((w0[0] < w2[0]) ? w0[0] : w2[0]) : ((w1[0] < w2[0]) ? w1[0] : w2[0]);
            const float maxX = (w0[0] > w1[0]) ? ((w0[0] > w2[0]) ? w0[0] : w2[0]) : ((w1[0] > w2[0]) ? w1[0] : w2[0]);
            if (maxX < boxMin[0] || minX > boxMax[0]) continue;

            const float minY = (w0[1] < w1[1]) ? ((w0[1] < w2[1]) ? w0[1] : w2[1]) : ((w1[1] < w2[1]) ? w1[1] : w2[1]);
            const float maxY = (w0[1] > w1[1]) ? ((w0[1] > w2[1]) ? w0[1] : w2[1]) : ((w1[1] > w2[1]) ? w1[1] : w2[1]);
            if (maxY < boxMin[1] || minY > boxMax[1]) continue;

            const float minZ = (w0[2] < w1[2]) ? ((w0[2] < w2[2]) ? w0[2] : w2[2]) : ((w1[2] < w2[2]) ? w1[2] : w2[2]);
            const float maxZ = (w0[2] > w1[2]) ? ((w0[2] > w2[2]) ? w0[2] : w2[2]) : ((w1[2] > w2[2]) ? w1[2] : w2[2]);
            if (maxZ < boxMin[2] || minZ > boxMax[2]) continue;

            // Fast AABB-sphere lower-bound reject BEFORE cross-product & ClosestPointOnTriangle3D!
            const float bx = (center[0] < minX) ? (minX - center[0]) : ((center[0] > maxX) ? (center[0] - maxX) : 0.0f);
            const float by = ((center[1] < minY) ? (minY - center[1]) : ((center[1] > maxY) ? (center[1] - maxY) : 0.0f)) * kVertWeight;
            const float bz = (center[2] < minZ) ? (minZ - center[2]) : ((center[2] > maxZ) ? (center[2] - maxZ) : 0.0f);
            const float aabbNormSq = (bx * bx + by * by + bz * bz) * invRadSq;
            if (aabbNormSq >= 1.0f) {
                if (!camProt) continue;
                const float cbx = (camPos[0] < minX) ? (minX - camPos[0]) : ((camPos[0] > maxX) ? (camPos[0] - maxX) : 0.0f);
                const float cby = ((camPos[1] < minY) ? (minY - camPos[1]) : ((camPos[1] > maxY) ? (camPos[1] - maxY) : 0.0f)) * kVertWeight;
                const float cbz = (camPos[2] < minZ) ? (minZ - camPos[2]) : ((camPos[2] > maxZ) ? (camPos[2] - maxZ) : 0.0f);
                if ((cbx * cbx + cby * cby + cbz * cbz) * invCamKeepSq >= 1.0f) continue;
            }

            const float e1x = w1[0] - w0[0], e1y = w1[1] - w0[1], e1z = w1[2] - w0[2];
            const float e2x = w2[0] - w0[0], e2y = w2[1] - w0[1], e2z = w2[2] - w0[2];
            const float rnx = e1y * e2z - e1z * e2y;
            const float rny = e1z * e2x - e1x * e2z;
            const float rnz = e1x * e2y - e1y * e2x;
            const float lenSq = rnx * rnx + rny * rny + rnz * rnz;
            if (lenSq < 1.0f) continue;

            const float invLen = 1.0f / std::sqrt(lenSq);
            const float unx = rnx * invLen;
            const float uny = rny * invLen;
            const float unz = rnz * invLen;

            // Upper-bound fast accept: ~80% of interior triangles skip ClosestPointOnTriangle3D completely!
            const float cx = (w0[0] + w1[0] + w2[0]) * (1.0f / 3.0f);
            const float cy = (w0[1] + w1[1] + w2[1]) * (1.0f / 3.0f);
            const float cz = (w0[2] + w1[2] + w2[2]) * (1.0f / 3.0f);
            const float cdx0 = cx - center[0];
            const float cdy0 = (cy - center[1]) * kVertWeight;
            const float cdz0 = cz - center[2];
            float normDistSq = (cdx0 * cdx0 + cdy0 * cdy0 + cdz0 * cdz0) * invRadSq;

            if (normDistSq >= 0.72f) {
                // Only run exact 3D point-to-triangle distance near the outer boundary shell!
                float qPt[3];
                ClosestPointOnTriangle3D(center, w0, w1, w2, unx, uny, unz, qPt);
                const float rdx = qPt[0] - center[0];
                const float rdy = (qPt[1] - center[1]) * kVertWeight;
                const float rdz = qPt[2] - center[2];
                normDistSq = (rdx * rdx + rdy * rdy + rdz * rdz) * invRadSq;

                if (camProt && normDistSq > 0.25f) {
                    float qCam[3];
                    ClosestPointOnTriangle3D(camPos, w0, w1, w2, unx, uny, unz, qCam);
                    const float cdx = qCam[0] - camPos[0];
                    const float cdy = (qCam[1] - camPos[1]) * kVertWeight;
                    const float cdz = qCam[2] - camPos[2];
                    const float camNormSq = (cdx * cdx + cdy * cdy + cdz * cdz) * invCamKeepSq;
                    if (camNormSq < normDistSq) {
                        normDistSq = camNormSq;
                    }
                }
                if (normDistSq >= 1.0f) continue;
            }

            float fade = 1.0f - normDistSq;
            fade = fade * fade; // Smooth 0-sqrt quadratic falloff at outer boundary

            // Gently dim multi-story caverns far above/below the player's current floor (> 7.5m vertical delta)
            const float vDelta = std::fabs(cy - refFloorY);
            if (vDelta > 750.0f) {
                const float vMul = 1.0f - (vDelta - 750.0f) * (1.0f / 1800.0f);
                fade *= (vMul < 0.22f) ? 0.22f : vMul;
            }

            const float absNy = std::fabs(uny);
            const std::uint8_t isWall = (absNy < 0.68f) ? 1U : 0U;

            std::uint8_t elevBand = 0U;
            if (absNy >= 0.68f && absNy < 0.985f) {
                const float yAbs = std::fabs(cy);
                const float band = yAbs - std::floor(yAbs * 0.005f) * 200.0f;
                if (band < 16.0f) elevBand = 1U;
            }

            auto qNorm = [](float n) noexcept -> std::int8_t {
                int v = static_cast<int>(n * 127.0f + ((n >= 0.0f) ? 0.5f : -0.5f));
                if (v > 127) v = 127;
                else if (v < -127) v = -127;
                return static_cast<std::int8_t>(v);
            };

            SbcTriRecord& tr = g_sbcTris[snap.sbcTriCount++];
            tr.x0 = w0[0]; tr.y0 = w0[1]; tr.z0 = w0[2];
            tr.x1 = w1[0]; tr.y1 = w1[1]; tr.z1 = w1[2];
            tr.x2 = w2[0]; tr.y2 = w2[1]; tr.z2 = w2[2];
            tr.fade     = fade;
            tr.nx       = qNorm(unx);
            tr.ny       = qNorm(uny);
            tr.nz       = qNorm(unz);
            tr.elevBand = elevBand;
            tr.layer    = layer;
            tr.pad0     = 0U;
            tr.pad1     = 0U;

            if (isHighPart) ++highTriHarvested;
            else            ++lowTriHarvested;

            if (lenSq >= minEdgeAreaSq && snap.sbcEdgeCount < kMaxSbcEdges) {
                InsertSbcEdgeDedup(w0, w1, unx, uny, unz, fade, isWall, layer, edgeGenShifted, snap);
                InsertSbcEdgeDedup(w1, w2, unx, uny, unz, fade, isWall, layer, edgeGenShifted, snap);
                InsertSbcEdgeDedup(w2, w0, unx, uny, unz, fade, isWall, layer, edgeGenShifted, snap);
            }
        }
    }

    g_sbcCache.sColl       = sColl;
    g_sbcCache.sbcCount    = sbcCount;
    g_sbcCache.fingerprint = fp;
    g_sbcCache.radius      = radiusWorldUnits;
    g_sbcCache.center[0]   = center[0];
    g_sbcCache.center[1]   = center[1];
    g_sbcCache.center[2]   = center[2];
    g_sbcCache.camPos[0]   = camPos[0];
    g_sbcCache.camPos[1]   = camPos[1];
    g_sbcCache.camPos[2]   = camPos[2];
    g_sbcCache.triCount    = snap.sbcTriCount;
    g_sbcCache.edgeCount   = snap.sbcEdgeCount;
    g_sbcCache.ageFrames   = 0U;
    g_sbcCache.sbcTier     = sbcTier;
    g_sbcCache.includeAux  = includeAux;
    g_sbcCache.valid       = true;
}

void CollectSnapshotSEH(std::uintptr_t exeBase,
                        std::uintptr_t exeEnd,
                        bool collectShapes,
                        const app::ViewFilterState& viewState,
                        float sbcRadiusWorldUnits,
                        bool includeAuxSbc,
                        RawSnapshot* outSnap) {
    const std::uint32_t    visMask = viewState.visMask;
    const app::SbcMeshTier sbcTier = viewState.sbcTier;
    outSnap->hasCamera       = false;
    outSnap->hasPlayerGround = false;
    outSnap->shapeCount      = 0;
    outSnap->sbcTriCount     = 0;
    outSnap->sbcEdgeCount    = 0;
    outSnap->hdrActive       = false;
    outSnap->hdrWhiteNits    = 200.0f;
    outSnap->mainPlDef       = PlayerFrameReadout{};
    g_emInspector            = EmInspectorSnapshot{};

    __try {
        // Read sMhRender HDR state even when only drawing the 2D HUD banner / panel
        const std::uintptr_t mhRender = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhRenderInstance);
        if (IsAlignedPtr(mhRender)) {
            outSnap->hdrActive = (*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderHdrOutput) != 0);
            const float whiteNits = *reinterpret_cast<const float*>(mhRender + mhw::kOffRenderHdrWhiteNits);
            if (std::isfinite(whiteNits) && whiteNits >= 50.0f && whiteNits <= 2000.0f) {
                outSnap->hdrWhiteNits = whiteNits;
            }
        }

        // Read Main Player defensive frame state + ground coordinates (for metric distance rings)
        const std::uintptr_t sPlayer = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSPlayerInstance);
        std::uintptr_t mainPl = 0;
        if (IsAlignedPtr(sPlayer)) {
            using GetMainPlayerFn = std::uintptr_t (*)(std::uintptr_t);
            const auto getMainPl = reinterpret_cast<GetMainPlayerFn>(exeBase + mhw::kRvaGetMainPlayer);
            mainPl = getMainPl(sPlayer);
            if (IsAlignedPtr(mainPl)) {
                const std::uint8_t alive = *reinterpret_cast<const std::uint8_t*>(mainPl + mhw::kOffUnitAliveFlags);
                if ((alive & 0x0EU) != 0) {
                    const std::uintptr_t plColl = *reinterpret_cast<const std::uintptr_t*>(mainPl + mhw::kOffUPlayerColl);
                    outSnap->mainPlDef = EvaluatePlayerDefense(mainPl, plColl);

                    const float* plPos = reinterpret_cast<const float*>(mainPl + mhw::kOffUCoordPos);
                    if (!IsValidVec3(plPos) ||
                        (std::fabs(plPos[0]) < 0.01f && std::fabs(plPos[1]) < 0.01f && std::fabs(plPos[2]) < 0.01f)) {
                        plPos = reinterpret_cast<const float*>(mainPl + mhw::kOffUCoordWorldPos);
                    }
                    if (IsValidVec3(plPos)) {
                        outSnap->hasPlayerGround = true;
                        outSnap->plFeetPos[0] = plPos[0];
                        outSnap->plFeetPos[1] = plPos[1];
                        outSnap->plFeetPos[2] = plPos[2];
                        const float* fwd = reinterpret_cast<const float*>(mainPl + 0x1F0); // WorldMatrix row 2 (forward)
                        if (IsValidVec3(fwd)) {
                            outSnap->plForward[0] = fwd[0];
                            outSnap->plForward[1] = fwd[1];
                            outSnap->plForward[2] = fwd[2];
                        } else {
                            outSnap->plForward[0] = 0.0f;
                            outSnap->plForward[1] = 0.0f;
                            outSnap->plForward[2] = 1.0f;
                        }
                    }
                }
            }
        }

        const bool inspectOnly = !collectShapes && viewState.panelOpen && (viewState.panelPage == 1);
        if (!collectShapes) {
            if (inspectOnly) {
                const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
                HarvestMonstersWithPartFilter(sEnemy, false, exeBase, exeEnd, viewState, *outSnap);
            }
            return;
        }

        const std::uintptr_t mhMain = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhMainInstance);
        if (!IsAlignedPtr(mhMain)) return;

        // 1. Read Active 3D Camera View & Projection Matrices (main viewport has subScene == 0, 0x14228e770)
        std::uintptr_t camSys = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSCameraInstance);
        if (!IsAlignedPtr(camSys)) {
            camSys = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCamera);
        }
        if (IsAlignedPtr(camSys)) {
            for (int vpIdx = 0; vpIdx < 8; ++vpIdx) {
                const std::uintptr_t vp       = camSys + static_cast<std::size_t>(vpIdx) * mhw::kCameraViewportStride;
                const std::uint8_t visible    = *reinterpret_cast<const std::uint8_t*>(vp + mhw::kOffVpVisible);
                const std::uintptr_t camObj   = *reinterpret_cast<const std::uintptr_t*>(vp + mhw::kOffVpCameraPtr);
                const std::uintptr_t subScene = *reinterpret_cast<const std::uintptr_t*>(vp + mhw::kOffVpSubScenePtr);
                if (visible != 0 && subScene == 0 && IsAlignedPtr(camObj)) {
                    const float interpT = *reinterpret_cast<const float*>(camSys + mhw::kOffSCamInterpRate);
                    if (std::isfinite(interpT) && interpT > 0.0f && interpT <= 1.0f) {
                        // Exact sCamera::Viewport::draw (0x14228f3b0) sub-frame matrix lerp
                        const float invT = 1.0f - interpT;
                        const auto* curV  = reinterpret_cast<const float*>(vp + mhw::kOffVpViewMat);
                        const auto* prevV = reinterpret_cast<const float*>(vp + mhw::kOffVpPrevViewMat);
                        const auto* curP  = reinterpret_cast<const float*>(vp + mhw::kOffVpProjMat);
                        const auto* prevP = reinterpret_cast<const float*>(vp + mhw::kOffVpPrevProjMat);
                        for (int k = 0; k < 16; ++k) {
                            outSnap->view[k] = invT * curV[k] + interpT * prevV[k];
                            outSnap->proj[k] = invT * curP[k] + interpT * prevP[k];
                        }
                    } else {
                        std::memcpy(outSnap->view, reinterpret_cast<const void*>(vp + mhw::kOffVpViewMat), 64);
                        std::memcpy(outSnap->proj, reinterpret_cast<const void*>(vp + mhw::kOffVpProjMat), 64);
                    }
                    const float* vm = outSnap->view;
                    outSnap->camPos[0] = -(vm[12] * vm[0] + vm[13] * vm[1] + vm[14] * vm[2]);
                    outSnap->camPos[1] = -(vm[12] * vm[4] + vm[13] * vm[5] + vm[14] * vm[6]);
                    outSnap->camPos[2] = -(vm[12] * vm[8] + vm[13] * vm[9] + vm[14] * vm[10]);
                    outSnap->hasCamera = true;
                    break;
                }
            }
        }
        const bool needEmInspector = (viewState.emPartMode != app::EmPartMode::Off) ||
                                     (viewState.panelOpen && viewState.panelPage == 1);
        if (!outSnap->hasCamera || (visMask & app::kCatMaskAll) == 0) {
            if (needEmInspector) {
                const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
                HarvestMonstersWithPartFilter(sEnemy, false, exeBase, exeEnd, viewState, *outSnap);
            }
            return;
        }

        std::uintptr_t sColl = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSCollisionInstance);
        if (!IsAlignedPtr(sColl)) {
            sColl = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCollision);
        }

        // 2. Harvest surrounding .sbc Stage Terrain / Wall / Ledge Meshes if enabled
        if ((visMask & app::kCatStageMesh) != 0 && IsAlignedPtr(sColl)) {
            HarvestStageSbcMeshes(sColl, sbcRadiusWorldUnits, sbcTier, includeAuxSbc, *outSnap);
        }

        const std::uint32_t entityBits = visMask & ~std::uint32_t(app::kCatStageMesh);
        if (entityBits == 0) {
            if (needEmInspector) {
                const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
                HarvestMonstersWithPartFilter(sEnemy, false, exeBase, exeEnd, viewState, *outSnap);
            }
            return;
        }

        g_seenColls.clear();
        g_seenGeoms.clear();

        // 3. Direct Player Collision Components
        const bool wantAnyPlayer = (visMask & (app::kCatPlHurtbox | app::kCatPlHitbox | app::kCatPushbox)) != 0;
        if (wantAnyPlayer && IsAlignedPtr(sPlayer)) {
            if (IsAlignedPtr(mainPl)) {
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(mainPl + mhw::kOffUPlayerColl),
                                      OwnerSide::Player, exeBase, exeEnd, visMask, *outSnap);
                HarvestUnitComponents(mainPl, OwnerSide::Player, exeBase, exeEnd, visMask, *outSnap);
            }
            const std::uint32_t plCnt = *reinterpret_cast<const std::uint32_t*>(sPlayer + mhw::kOffSPlayerGlobalCnt);
            for (std::uint32_t i = 0; i < plCnt && i < 4; ++i) {
                const std::uintptr_t pl = *reinterpret_cast<const std::uintptr_t*>(sPlayer + mhw::kOffSPlayerGlobalArr + i * 8);
                if (IsAlignedPtr(pl)) {
                    HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(pl + mhw::kOffUPlayerColl),
                                          OwnerSide::Player, exeBase, exeEnd, visMask, *outSnap);
                    HarvestUnitComponents(pl, OwnerSide::Player, exeBase, exeEnd, visMask, *outSnap);
                }
            }
        }

        // 4. Direct Monster Collision Components (+ Monster Part Partitioning & Pushbox Binding)
        const bool wantAnyMonster = (visMask & (app::kCatEmHurtbox | app::kCatEmHitbox | app::kCatPushbox)) != 0;
        const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
        if ((wantAnyMonster || needEmInspector) && IsAlignedPtr(sEnemy)) {
            HarvestMonstersWithPartFilter(sEnemy, wantAnyMonster, exeBase, exeEnd, viewState, *outSnap);
        }

        // 5. Fast O(1)-cached sMhUnit Traversal (weapons, shells, Palico, map gimmicks, VFX)
        std::uintptr_t unitSys = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhUnitInstance);
        if (!IsAlignedPtr(unitSys)) {
            unitSys = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainUnit);
        }
        if (IsAlignedPtr(unitSys)) {
            const std::uint32_t lineCount = *reinterpret_cast<const std::uint32_t*>(unitSys + mhw::kOffMoveLineCount);
            const std::uint32_t maxLines = (lineCount <= 64) ? lineCount : 64;
            for (std::uint32_t line = 0; line < maxLines; ++line) {
                std::uintptr_t unit = *reinterpret_cast<const std::uintptr_t*>(
                    unitSys + line * mhw::kMoveLineStride + mhw::kOffMoveLineFirstUnit);
                for (int guard = 0; guard < 2048 && IsAlignedPtr(unit); ++guard) {
                    const std::uintptr_t nextUnit = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffUnitNext);
                    const std::uint8_t alive = *reinterpret_cast<const std::uint8_t*>(unit + mhw::kOffUnitAliveFlags);
                    if ((alive & 0x0EU) != 0) {
                        const std::uintptr_t compHead = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
                        const std::uintptr_t dti = GetObjectDti(unit, exeBase, exeEnd);
                        DtiCacheEntry* entry = LookupOrCreateDtiEntry(dti, exeBase);
                        if (!entry || !entry->skipUnit) {
                            const OwnerSide unitSide = entry ? entry->side : OwnerSide::Unknown;
                            if (IsAlignedPtr(compHead)) {
                                HarvestUnitComponents(unit, unitSide, exeBase, exeEnd, visMask, *outSnap);
                            }
                            if (entry && entry->directCollOffset != 0) {
                                const std::uintptr_t coll =
                                    *reinterpret_cast<const std::uintptr_t*>(unit + entry->directCollOffset);
                                HarvestCpObjCollision(coll, unitSide, exeBase, exeEnd, visMask, *outSnap);
                            }
                        }
                    }
                    unit = nextUnit;
                }
            }
        }

        // 6. Fallback sCollision::Collider NodeLists
        if (IsAlignedPtr(sColl)) {
            const std::uintptr_t collider = sColl + mhw::kOffSCollCollider;
            const std::uint32_t layerCnt = *reinterpret_cast<const std::uint32_t*>(collider + mhw::kOffCollLayerCount);
            const std::uintptr_t layerArr = *reinterpret_cast<const std::uintptr_t*>(collider + mhw::kOffCollLayerArray);
            if (layerCnt <= 32 && IsAlignedPtr(layerArr)) {
                for (std::uint32_t l = 0; l < layerCnt; ++l) {
                    const std::uintptr_t listMtArr = *reinterpret_cast<const std::uintptr_t*>(layerArr + l * 8);
                    if (!IsAlignedPtr(listMtArr)) continue;
                    const std::uint32_t listCnt = *reinterpret_cast<const std::uint32_t*>(listMtArr + 0x08);
                    const std::uintptr_t listPtrs = *reinterpret_cast<const std::uintptr_t*>(listMtArr + 0x18);
                    if (listCnt == 0 || listCnt > 8 || !IsAlignedPtr(listPtrs)) continue;

                    for (std::uint32_t s = 0; s < listCnt; ++s) {
                        const std::uintptr_t nl = *reinterpret_cast<const std::uintptr_t*>(listPtrs + s * 8);
                        if (!IsAlignedPtr(nl)) continue;
                        if (*reinterpret_cast<const std::uint8_t*>(nl + 0x08) == 0) continue;

                        const std::uint32_t grpCnt = *reinterpret_cast<const std::uint32_t*>(nl + 0x1B80);
                        const std::uintptr_t grpArr = *reinterpret_cast<const std::uintptr_t*>(nl + 0x1B90);
                        if (grpCnt <= 512 && IsAlignedPtr(grpArr)) {
                            for (std::uint32_t g = 0; g < grpCnt; ++g) {
                                const std::uintptr_t grp = *reinterpret_cast<const std::uintptr_t*>(grpArr + g * 8);
                                if (!IsAlignedPtr(grp)) continue;
                                const std::uint32_t nCnt = *reinterpret_cast<const std::uint32_t*>(grp + 0x38);
                                const std::uintptr_t nArr = *reinterpret_cast<const std::uintptr_t*>(grp + 0x48);
                                if (nCnt <= 256 && IsAlignedPtr(nArr)) {
                                    for (std::uint32_t n = 0; n < nCnt; ++n) {
                                        HarvestNodeObjectFallback(
                                            *reinterpret_cast<const std::uintptr_t*>(nArr + n * 8),
                                            false, exeBase, exeEnd, visMask, *outSnap);
                                    }
                                }
                            }
                        }

                        const std::uint32_t ppCnt = *reinterpret_cast<const std::uint32_t*>(nl + 0x40D0);
                        const std::uintptr_t ppArr = *reinterpret_cast<const std::uintptr_t*>(nl + 0x40E0);
                        if (ppCnt <= 1024 && IsAlignedPtr(ppArr)) {
                            for (std::uint32_t n = 0; n < ppCnt; ++n) {
                                const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(ppArr + n * 8);
                                if (IsAlignedPtr(entry)) {
                                    HarvestNodeObjectFallback(
                                        *reinterpret_cast<const std::uintptr_t*>(entry + 0x10),
                                        false, exeBase, exeEnd, visMask, *outSnap);
                                }
                            }
                        }

                        const std::uint32_t paCnt = *reinterpret_cast<const std::uint32_t*>(nl + 0x40B0);
                        const std::uintptr_t paArr = *reinterpret_cast<const std::uintptr_t*>(nl + 0x40C0);
                        if (paCnt <= 1024 && IsAlignedPtr(paArr)) {
                            for (std::uint32_t n = 0; n < paCnt; ++n) {
                                const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(paArr + n * 8);
                                if (IsAlignedPtr(entry)) {
                                    HarvestNodeObjectFallback(
                                        *reinterpret_cast<const std::uintptr_t*>(entry + 0x08),
                                        true, exeBase, exeEnd, visMask, *outSnap);
                                }
                            }
                        }

                        for (std::uint32_t b = 0; b < 0x49; ++b) {
                            const std::uintptr_t bkt = nl + 0x940 + b * 0x20;
                            const std::uint32_t bCnt = *reinterpret_cast<const std::uint32_t*>(bkt + 0x00);
                            const std::uintptr_t bArr = *reinterpret_cast<const std::uintptr_t*>(bkt + 0x10);
                            if (bCnt > 0 && bCnt <= 512 && IsAlignedPtr(bArr)) {
                                for (std::uint32_t n = 0; n < bCnt; ++n) {
                                    const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(bArr + n * 8);
                                    if (IsAlignedPtr(entry)) {
                                        HarvestNodeObjectFallback(
                                            *reinterpret_cast<const std::uintptr_t*>(entry + 0x08),
                                            true, exeBase, exeEnd, visMask, *outSnap);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Safely ignore transient pointer races during level/quest unload
    }
}

// ============================================================================
//  HDR (ST.2084 PQ + Rec.2020 & scRGB) / SDR Color Encoder
// ============================================================================

struct Color4 {
    float r, g, b, a;
};

inline float SrgbToLinear(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSt2084Pq(float lNorm) {
    if (lNorm <= 0.0f) return 0.0f;
    if (lNorm >= 1.0f) return 1.0f;
    constexpr float m1 = 0.1593017578125f; // 2610 / 16384
    constexpr float m2 = 78.84375f;        // 2523 / 32
    constexpr float c1 = 0.8359375f;       // 3424 / 4096
    constexpr float c2 = 18.8515625f;      // 2413 / 128
    constexpr float c3 = 18.6875f;         // 2392 / 128
    const float ym = std::pow(lNorm, m1);
    return std::pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
}

struct ColorSpaceEncoder {
    enum class Mode : std::uint8_t { Sdr = 0, Hdr10Pq = 1, ScRgb = 2 };
    Mode mode = Mode::Sdr;
    float targetNits = 175.0f;

    void init(std::uint32_t dxgiFmt, bool gameHdrActive, float gameWhiteNits, float cfgHdrNits) {
        float nits = (cfgHdrNits >= 40.0f) ? cfgHdrNits : (gameWhiteNits * 0.65f);
        if (nits < 100.0f) nits = 100.0f;
        if (nits > 400.0f && cfgHdrNits <= 0.0f) nits = 240.0f;
        targetNits = nits;

        if (dxgiFmt == kDxgiFmtR16G16B16A16_Float || dxgiFmt == kDxgiFmtR16G16B16A16_Typeless) {
            mode = Mode::ScRgb;
        } else if ((dxgiFmt == kDxgiFmtR10G10B10A2_Unorm || dxgiFmt == kDxgiFmtR10G10B10A2_Typeless) &&
                   gameHdrActive) {
            mode = Mode::Hdr10Pq;
        } else {
            mode = Mode::Sdr;
        }
    }

    Color4 encode(const Color4& srgb) const {
        if (mode == Mode::Sdr) return srgb;

        const float lr = SrgbToLinear(srgb.r);
        const float lg = SrgbToLinear(srgb.g);
        const float lb = SrgbToLinear(srgb.b);

        if (mode == Mode::ScRgb) {
            const float scale = targetNits / 80.0f;
            return {lr * scale, lg * scale, lb * scale, srgb.a};
        }

        // Mode::Hdr10Pq — Rec.709 linear -> Rec.2020 linear -> nits/10000 -> SMPTE ST.2084 PQ
        const float r2020 = 0.6274040f * lr + 0.3292820f * lg + 0.0433136f * lb;
        const float g2020 = 0.0690970f * lr + 0.9195400f * lg + 0.0113612f * lb;
        const float b2020 = 0.0163916f * lr + 0.0880132f * lg + 0.8955950f * lb;
        const float nitScale = targetNits / 10000.0f;
        return {
            LinearToSt2084Pq(r2020 * nitScale),
            LinearToSt2084Pq(g2020 * nitScale),
            LinearToSt2084Pq(b2020 * nitScale),
            srgb.a
        };
    }
};

Color4 PlayerDefSrgbColor(PlayerDefState s, float alpha, float baseHalfThick, float& outHalfThick) {
    switch (s) {
        case PlayerDefState::Invincible:
            outHalfThick = baseHalfThick * 1.55f;
            return {0.97f, 0.99f, 1.00f, alpha}; // Bright Ice-White
        case PlayerDefState::GuardPoint:
            outHalfThick = baseHalfThick * 1.45f;
            return {0.56f, 0.44f, 1.00f, alpha}; // Vivid Royal Indigo-Violet
        case PlayerDefState::HyperArmor:
            outHalfThick = baseHalfThick * 1.45f;
            return {1.00f, 0.46f, 0.08f, alpha}; // Vivid Flame-Orange
        case PlayerDefState::Counter:
            outHalfThick = baseHalfThick * 1.40f;
            return {0.16f, 1.00f, 0.74f, alpha}; // Electric Mint-Aqua
        case PlayerDefState::Guard:
            outHalfThick = baseHalfThick * 1.25f;
            return {0.24f, 0.58f, 1.00f, alpha}; // Azure Shield Blue
        case PlayerDefState::SuperArmor:
            outHalfThick = baseHalfThick * 1.25f;
            return {0.95f, 0.34f, 0.90f, alpha}; // Vivid Magenta-Pink
        default:
            outHalfThick = baseHalfThick * 1.00f;
            return {0.15f, 0.85f, 1.00f, alpha}; // Crisp Cyan
    }
}

// ============================================================================
//  3D -> Clip -> NDC Thick-Line Quad Builder & 2D Crisp Vector HUD
// ============================================================================

struct Vec3 {
    float x, y, z;
};

struct ClipVert4 {
    float x, y, z, w;
};

struct LineProjector {
    float vp[16]{};
    float camPos[3]{};
    float screenW = 1920.0f;
    float screenH = 1080.0f;
    float invW2   = 2.0f / 1920.0f;
    float invH2   = 2.0f / 1080.0f;
    float projM22 = 0.0f;
    float zBiasCm = 0.0f; // Distance-invariant camera-ward depth bias in world cm

    void init(const float view[16], const float proj[16], const float cam[3], float w, float h) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                vp[r * 4 + c] =
                    view[r * 4 + 0] * proj[0 * 4 + c] +
                    view[r * 4 + 1] * proj[1 * 4 + c] +
                    view[r * 4 + 2] * proj[2 * 4 + c] +
                    view[r * 4 + 3] * proj[3 * 4 + c];
            }
        }
        // Apply MHW cDraw::setViewProjection (0x1423d0e70) Reversed-Z flip: z_clip_rev = w_clip - z_clip_std
        // Converts Standard-Z (0=near, 1=far) into hardware Reversed-Z (1=near, 0=far) matching tDepthMap!
        for (int r = 0; r < 4; ++r) {
            vp[r * 4 + 2] = vp[r * 4 + 3] - vp[r * 4 + 2];
        }
        const float m22 = std::fabs(std::fabs(proj[10]) - 1.0f);
        projM22 = (std::isfinite(m22) && m22 < 0.5f) ? m22 : 0.0f;
        zBiasCm = 0.0f;
        if (cam) {
            camPos[0] = cam[0];
            camPos[1] = cam[1];
            camPos[2] = cam[2];
        }
        screenW = (w > 1.0f) ? w : 1920.0f;
        screenH = (h > 1.0f) ? h : 1080.0f;
        invW2 = 2.0f / screenW;
        invH2 = 2.0f / screenH;
    }

    float computeNdcZ(float cz, float cw) const noexcept {
        constexpr float kMinW = 0.05f;
        float z = 0.0f;
        if (zBiasCm > 0.0f) {
            const float denom = std::fmax(kMinW, std::fmax(cz, cw - zBiasCm));
            z = (cz + projM22 * zBiasCm) / denom;
        } else {
            z = cz / std::fmax(kMinW, cw);
        }
        if (z < 0.0f) z = 0.0f;
        else if (z > 1.0f) z = 1.0f;
        return z;
    }

    float distToCam(const Vec3& p) const noexcept {
        const float dx = p.x - camPos[0];
        const float dy = p.y - camPos[1];
        const float dz = p.z - camPos[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    bool projectPoint2D(const Vec3& p, float& outSx, float& outSy, float* outZ = nullptr) const noexcept {
        const float cx = p.x * vp[0] + p.y * vp[4] + p.z * vp[8]  + vp[12];
        const float cy = p.x * vp[1] + p.y * vp[5] + p.z * vp[9]  + vp[13];
        const float cz = p.x * vp[2] + p.y * vp[6] + p.z * vp[10] + vp[14];
        const float cw = p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15];
        if (cz < 0.0f || cw < 0.05f || cz > cw) return false;
        const float ndcX = cx / cw;
        const float ndcY = cy / cw;
        if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f) return false;
        outSx = (ndcX * 0.5f + 0.5f) * screenW;
        outSy = (0.5f - ndcY * 0.5f) * screenH;
        if (outZ) {
            *outZ = computeNdcZ(cz, cw);
        }
        return true;
    }

    void emitScreenQuad(std::vector<OverlayVertex>& out,
                        float sx0, float sy0, float sx1, float sy1,
                        float halfThicknessPx, const Color4& col,
                        float z0 = 0.0f, float z1 = 0.0f) const {
        const float dx = sx1 - sx0;
        const float dy = sy1 - sy0;
        const float lenSq = dx * dx + dy * dy;
        if (lenSq < 0.04f) return;
        const float invLen = 1.0f / std::sqrt(lenSq);
        const float nx = -dy * invLen * halfThicknessPx;
        const float ny =  dx * invLen * halfThicknessPx;

        auto toNdc = [&](float px, float py, float z) -> OverlayVertex {
            return {
                px * invW2 - 1.0f,
                1.0f - py * invH2,
                z, 1.0f,
                col.r, col.g, col.b, col.a
            };
        };

        const OverlayVertex v0 = toNdc(sx0 - nx, sy0 - ny, z0);
        const OverlayVertex v1 = toNdc(sx0 + nx, sy0 + ny, z0);
        const OverlayVertex v2 = toNdc(sx1 + nx, sy1 + ny, z1);
        const OverlayVertex v3 = toNdc(sx1 - nx, sy1 - ny, z1);

        out.push_back(v0); out.push_back(v1); out.push_back(v2);
        out.push_back(v0); out.push_back(v2); out.push_back(v3);
    }

    void emitRect2D(std::vector<OverlayVertex>& out,
                    float x0, float y0, float x1, float y1, const Color4& col,
                    float z = 0.0f) const {
        auto toNdc = [&](float px, float py) -> OverlayVertex {
            return {
                px * invW2 - 1.0f,
                1.0f - py * invH2,
                z, 1.0f,
                col.r, col.g, col.b, col.a
            };
        };
        const OverlayVertex v0 = toNdc(x0, y0);
        const OverlayVertex v1 = toNdc(x1, y0);
        const OverlayVertex v2 = toNdc(x1, y1);
        const OverlayVertex v3 = toNdc(x0, y1);
        out.push_back(v0); out.push_back(v1); out.push_back(v2);
        out.push_back(v0); out.push_back(v2); out.push_back(v3);
    }

    static inline std::uint32_t FrustumRejectCode(const ClipVert4& v) noexcept {
        // 7-plane homogeneous clip-space outcode (convex half-spaces; valid even when w <= 0!)
        //   Frustum bounds: -w <= x <= w, -w <= y <= w, 0 <= z <= w, w >= 0.25
        static constexpr float kMinW = 0.25f;
        std::uint32_t code = 0U;
        if (v.x + v.w < 0.0f) code |= 1U << 0;
        if (v.w - v.x < 0.0f) code |= 1U << 1;
        if (v.y + v.w < 0.0f) code |= 1U << 2;
        if (v.w - v.y < 0.0f) code |= 1U << 3;
        if (v.z       < 0.0f) code |= 1U << 4;
        if (v.w - v.z < 0.0f) code |= 1U << 5;
        if (v.w       < kMinW) code |= 1U << 6;
        return code;
    }

    static inline bool InsideGuardBand(const ClipVert4& v) noexcept {
        static constexpr float kMinW  = 0.25f;
        static constexpr float kGuard = 2.0f;
        if (v.w < kMinW || v.z < 0.0f || v.z > v.w) return false;
        const float gw = kGuard * v.w;
        return (v.x >= -gw && v.x <= gw && v.y >= -gw && v.y <= gw);
    }

    // Full 6-frustum + W guard-band Sutherland-Hodgman clipper with O(1) outcode trivial-reject
    // and trivial-accept fast paths (avoids 7-pass clipping on >96% of triangles while preventing
    // GPU 16.8 fixed-point rasterizer guard-band overflow on large near-plane .sbc triangles!)
    void addTri3D(std::vector<OverlayVertex>& out,
                  const Vec3& a, const Vec3& b, const Vec3& c,
                  const Color4& col) const {
        if (col.a <= 0.004f) return;

        auto toClip = [&](const Vec3& p) noexcept -> ClipVert4 {
            return {
                p.x * vp[0] + p.y * vp[4] + p.z * vp[8]  + vp[12],
                p.x * vp[1] + p.y * vp[5] + p.z * vp[9]  + vp[13],
                p.x * vp[2] + p.y * vp[6] + p.z * vp[10] + vp[14],
                p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15]
            };
        };

        const ClipVert4 v0 = toClip(a);
        const ClipVert4 v1 = toClip(b);
        const ClipVert4 v2 = toClip(c);

        // Fast-path 1: 7-plane homogeneous trivial reject (~60% of stage triangles exit in ~5 ns!)
        if ((FrustumRejectCode(v0) & FrustumRejectCode(v1) & FrustumRejectCode(v2)) != 0U) {
            return;
        }

        // Fast-path 2: All 3 vertices already inside guard band (>95% of visible triangles!)
        if (InsideGuardBand(v0) && InsideGuardBand(v1) && InsideGuardBand(v2)) {
            const float iw0 = 1.0f / v0.w;
            const float iw1 = 1.0f / v1.w;
            const float iw2 = 1.0f / v2.w;
            out.push_back({v0.x * iw0, v0.y * iw0, computeNdcZ(v0.z, v0.w), 1.0f, col.r, col.g, col.b, col.a});
            out.push_back({v1.x * iw1, v1.y * iw1, computeNdcZ(v1.z, v1.w), 1.0f, col.r, col.g, col.b, col.a});
            out.push_back({v2.x * iw2, v2.y * iw2, computeNdcZ(v2.z, v2.w), 1.0f, col.r, col.g, col.b, col.a});
            return;
        }

        ClipVert4 polyA[16]{v0, v1, v2};
        ClipVert4 polyB[16]{};
        int cnt = 3;

        auto clipPlane = [](const ClipVert4* src, int srcCnt, ClipVert4* dst, auto evalDist) noexcept -> int {
            int dstCnt = 0;
            for (int i = 0; i < srcCnt; ++i) {
                const ClipVert4& cur = src[i];
                const ClipVert4& nxt = src[(i + 1 == srcCnt) ? 0 : (i + 1)];
                const float dCur = evalDist(cur);
                const float dNxt = evalDist(nxt);
                const bool inCur = (dCur >= 0.0f);
                const bool inNxt = (dNxt >= 0.0f);
                if (inCur && dstCnt < 16) {
                    dst[dstCnt++] = cur;
                }
                if (inCur != inNxt && dstCnt < 16) {
                    const float denom = dCur - dNxt;
                    float t = (std::fabs(denom) > 1e-7f) ? (dCur / denom) : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    dst[dstCnt++] = {
                        cur.x + (nxt.x - cur.x) * t,
                        cur.y + (nxt.y - cur.y) * t,
                        cur.z + (nxt.z - cur.z) * t,
                        cur.w + (nxt.w - cur.w) * t
                    };
                }
            }
            return dstCnt;
        };

        static constexpr float kMinW  = 0.25f;
        static constexpr float kGuard = 2.0f;

        // 1. Reversed-Z depth & positive W planes: z >= 0, w >= kMinW, z <= w
        cnt = clipPlane(polyA, cnt, polyB, [](const ClipVert4& v) noexcept { return v.z; });
        if (cnt < 3) return;
        cnt = clipPlane(polyB, cnt, polyA, [](const ClipVert4& v) noexcept { return v.w - kMinW; });
        if (cnt < 3) return;
        cnt = clipPlane(polyA, cnt, polyB, [](const ClipVert4& v) noexcept { return v.w - v.z; });
        if (cnt < 3) return;

        // 2. X/Y homogeneous guard-band planes (|x| <= 2*w, |y| <= 2*w -> |ndcX|,|ndcY| <= 2.0f)
        cnt = clipPlane(polyB, cnt, polyA, [](const ClipVert4& v) noexcept { return kGuard * v.w + v.x; });
        if (cnt < 3) return;
        cnt = clipPlane(polyA, cnt, polyB, [](const ClipVert4& v) noexcept { return kGuard * v.w - v.x; });
        if (cnt < 3) return;
        cnt = clipPlane(polyB, cnt, polyA, [](const ClipVert4& v) noexcept { return kGuard * v.w + v.y; });
        if (cnt < 3) return;
        cnt = clipPlane(polyA, cnt, polyB, [](const ClipVert4& v) noexcept { return kGuard * v.w - v.y; });
        if (cnt < 3) return;

        OverlayVertex ndc[16]{};
        float minX =  1e9f, maxX = -1e9f;
        float minY =  1e9f, maxY = -1e9f;
        for (int i = 0; i < cnt; ++i) {
            const float wSafe = (polyB[i].w > kMinW) ? polyB[i].w : kMinW;
            const float invW  = 1.0f / wSafe;
            const float nx = polyB[i].x * invW;
            const float ny = polyB[i].y * invW;
            const float nz = computeNdcZ(polyB[i].z, wSafe);
            if (nx < minX) minX = nx;
            if (nx > maxX) maxX = nx;
            if (ny < minY) minY = ny;
            if (ny > maxY) maxY = ny;
            ndc[i] = {nx, ny, nz, 1.0f, col.r, col.g, col.b, col.a};
        }
        if (maxX < -1.05f || minX > 1.05f || maxY < -1.05f || minY > 1.05f) return;

        for (int i = 1; i + 1 < cnt; ++i) {
            out.push_back(ndc[0]);
            out.push_back(ndc[i]);
            out.push_back(ndc[i + 1]);
        }
    }

    void addLine3D(std::vector<OverlayVertex>& out,
                   const Vec3& a, const Vec3& b,
                   float halfThicknessPx, const Color4& col,
                   bool perspectiveScale = false) const {
        if (col.a <= 0.004f) return;

        ClipVert4 va{
            a.x * vp[0] + a.y * vp[4] + a.z * vp[8]  + vp[12],
            a.x * vp[1] + a.y * vp[5] + a.z * vp[9]  + vp[13],
            a.x * vp[2] + a.y * vp[6] + a.z * vp[10] + vp[14],
            a.x * vp[3] + a.y * vp[7] + a.z * vp[11] + vp[15]
        };
        ClipVert4 vb{
            b.x * vp[0] + b.y * vp[4] + b.z * vp[8]  + vp[12],
            b.x * vp[1] + b.y * vp[5] + b.z * vp[9]  + vp[13],
            b.x * vp[2] + b.y * vp[6] + b.z * vp[10] + vp[14],
            b.x * vp[3] + b.y * vp[7] + b.z * vp[11] + vp[15]
        };

        // Fast-path 1: 7-plane homogeneous trivial reject BEFORE any sqrt or clipping!
        if ((FrustumRejectCode(va) & FrustumRejectCode(vb)) != 0U) {
            return;
        }

        static constexpr float kMinW  = 0.25f;
        static constexpr float kGuard = 2.0f;

        // Fast-path 2: Skip 7-plane segment clipping when both endpoints already sit inside guard band!
        if (!InsideGuardBand(va) || !InsideGuardBand(vb)) {
            auto clipSegPlane = [](ClipVert4& p0, ClipVert4& p1, auto evalDist) noexcept -> bool {
                const float d0 = evalDist(p0);
                const float d1 = evalDist(p1);
                if (d0 < 0.0f && d1 < 0.0f) return false;
                if (d0 < 0.0f) {
                    const float denom = d0 - d1;
                    float t = (std::fabs(denom) > 1e-7f) ? (d0 / denom) : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    p0.x += (p1.x - p0.x) * t;
                    p0.y += (p1.y - p0.y) * t;
                    p0.z += (p1.z - p0.z) * t;
                    p0.w += (p1.w - p0.w) * t;
                } else if (d1 < 0.0f) {
                    const float denom = d1 - d0;
                    float t = (std::fabs(denom) > 1e-7f) ? (d1 / denom) : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    else if (t > 1.0f) t = 1.0f;
                    p1.x += (p0.x - p1.x) * t;
                    p1.y += (p0.y - p1.y) * t;
                    p1.z += (p0.z - p1.z) * t;
                    p1.w += (p0.w - p1.w) * t;
                }
                return true;
            };

            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return v.z; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return v.w - kMinW; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return v.w - v.z; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return kGuard * v.w + v.x; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return kGuard * v.w - v.x; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return kGuard * v.w + v.y; })) return;
            if (!clipSegPlane(va, vb, [](const ClipVert4& v) noexcept { return kGuard * v.w - v.y; })) return;
        }

        const float awSafe = (va.w > kMinW) ? va.w : kMinW;
        const float bwSafe = (vb.w > kMinW) ? vb.w : kMinW;
        const float ndcAx = va.x / awSafe;
        const float ndcAy = va.y / awSafe;
        const float ndcBx = vb.x / bwSafe;
        const float ndcBy = vb.y / bwSafe;

        if ((ndcAx < -1.1f && ndcBx < -1.1f) || (ndcAx > 1.1f && ndcBx > 1.1f) ||
            (ndcAy < -1.1f && ndcBy < -1.1f) || (ndcAy > 1.1f && ndcBy > 1.1f)) {
            return;
        }

        float effHalfThick = halfThicknessPx;
        Color4 effCol = col;
        if (perspectiveScale) {
            // Reuse clip-space view depth w directly (0 extra distToCam sqrt, 0 work on offscreen edges!)
            const float dMid = 0.5f * (awSafe + bwSafe);
            float pScale = std::sqrt(850.0f / (dMid + 220.0f));
            if (pScale < 0.44f) pScale = 0.44f;
            else if (pScale > 1.30f) pScale = 1.30f;
            effHalfThick = halfThicknessPx * pScale;
            if (effHalfThick < 0.42f) effHalfThick = 0.42f;

            float distA = 1.12f - dMid * (1.0f / 11000.0f);
            if (distA > 1.0f) distA = 1.0f;
            else if (distA < 0.36f) distA = 0.36f;
            effCol.a *= distA;
            if (effCol.a <= 0.004f) return;
        }

        const float sx0 = (ndcAx * 0.5f + 0.5f) * screenW;
        const float sy0 = (0.5f - ndcAy * 0.5f) * screenH;
        const float sx1 = (ndcBx * 0.5f + 0.5f) * screenW;
        const float sy1 = (0.5f - ndcBy * 0.5f) * screenH;

        const float z0 = computeNdcZ(va.z, awSafe);
        const float z1 = computeNdcZ(vb.z, bwSafe);

        emitScreenQuad(out, sx0, sy0, sx1, sy1, effHalfThick, effCol, z0, z1);
    }

    // Perspective depth-scaled + front/back hemisphere-cued 3D line (eliminates flat Necker-cube illusion!)
    void addDepthLine3D(std::vector<OverlayVertex>& out,
                        const Vec3& a, const Vec3& b,
                        float baseHalfThicknessPx,
                        const Color4& baseCol,
                        float volCenterDist,
                        float volCharRadius,
                        bool depthCueFrontBack) const {
        if (!depthCueFrontBack) {
            addLine3D(out, a, b, baseHalfThicknessPx, baseCol, true);
            return;
        }
        const Vec3 mid{0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z)};
        const float dMid = distToCam(mid);

        // Perspective line-width tapering: ~1.25x at 3m, 1.0x at 6.3m, 0.72x at 14m, 0.45x at 35m+
        float perspW = std::sqrt(850.0f / (dMid + 220.0f));
        if (perspW > 1.30f) perspW = 1.30f;
        else if (perspW < 0.44f) perspW = 0.44f;

        // Gentle atmospheric distance attenuation so far-away clusters don't over-brighten
        float distA = 1.12f - dMid * (1.0f / 11000.0f);
        if (distA > 1.0f) distA = 1.0f;
        else if (distA < 0.36f) distA = 0.36f;

        float fbW = 1.0f;
        float fbA = 1.0f;
        if (depthCueFrontBack && volCharRadius > 1.0f) {
            // Smooth front-vs-back transition across [-0.25R .. +0.35R] around volume center
            const float rel = (dMid - volCenterDist) / volCharRadius;
            if (rel > 0.15f) {
                float t = (rel - 0.15f) * 2.2f;
                if (t > 1.0f) t = 1.0f;
                fbW = 1.06f - 0.36f * t; // Back side tapers to 0.70x width
                fbA = 1.00f - 0.54f * t; // Back side dims to 0.46x alpha
            } else {
                fbW = 1.06f;             // Front camera-facing side stays bold & crisp
            }
        }

        float ht = baseHalfThicknessPx * perspW * fbW;
        if (ht < 0.42f) ht = 0.42f;
        Color4 c = baseCol;
        c.a *= distA * fbA;
        addLine3D(out, a, b, ht, c);
    }
};

void MakeOrthonormalBasis(const Vec3& axis, Vec3& outU, Vec3& outV) {
    Vec3 ref = (std::fabs(axis.y) < 0.9f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    outU = {
        axis.y * ref.z - axis.z * ref.y,
        axis.z * ref.x - axis.x * ref.z,
        axis.x * ref.y - axis.y * ref.x
    };
    const float len = std::sqrt(outU.x * outU.x + outU.y * outU.y + outU.z * outU.z);
    if (len > 1e-5f) {
        outU.x /= len; outU.y /= len; outU.z /= len;
    } else {
        outU = {1.0f, 0.0f, 0.0f};
    }
    outV = {
        axis.y * outU.z - axis.z * outU.y,
        axis.z * outU.x - axis.x * outU.z,
        axis.x * outU.y - axis.y * outU.x
    };
}

void EmitSphere(std::vector<OverlayVertex>& out,
                const LineProjector& proj,
                const Vec3& c,
                float r,
                float halfThick,
                const Color4& col,
                bool depthCue,
                float fillAlphaRatio) {
    const float dCenter = proj.distToCam(c);
    constexpr int kSegs = 20;
    float cs[kSegs + 1], sn[kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kSegs);
        cs[i] = std::cos(a);
        sn[i] = std::sin(a);
    }

    // View-aligned outer silhouette ring + optional translucent volume core disc (makes spheres instantly 3D!)
    if (dCenter > r * 1.05f && depthCue) {
        const float invD = 1.0f / dCenter;
        const Vec3 viewDir{(proj.camPos[0] - c.x) * invD,
                           (proj.camPos[1] - c.y) * invD,
                           (proj.camPos[2] - c.z) * invD};
        Vec3 su{}, sv{};
        MakeOrthonormalBasis(viewDir, su, sv);
        const float ratio = r * invD;
        const float silR  = r * std::sqrt((1.0f - ratio * ratio > 0.05f) ? (1.0f - ratio * ratio) : 0.05f);
        const float silOff = r * ratio;
        const Vec3 silCenter{c.x + viewDir.x * silOff, c.y + viewDir.y * silOff, c.z + viewDir.z * silOff};

        Vec3 silPts[kSegs + 1];
        for (int i = 0; i <= kSegs; ++i) {
            const float ca = cs[i] * silR;
            const float sa = sn[i] * silR;
            silPts[i] = {
                silCenter.x + su.x * ca + sv.x * ca * 0.0f + sv.x * sa,
                silCenter.y + su.y * ca + sv.y * sa,
                silCenter.z + su.z * ca + sv.z * sa
            };
        }
        if (fillAlphaRatio > 0.001f) {
            Color4 fillCol = col;
            fillCol.a *= fillAlphaRatio;
            for (int i = 0; i < kSegs; ++i) {
                proj.addTri3D(out, silCenter, silPts[i], silPts[i + 1], fillCol);
            }
        }
        for (int i = 0; i < kSegs; ++i) {
            proj.addDepthLine3D(out, silPts[i], silPts[i + 1], halfThick * 1.08f, col, dCenter, 0.0f, false);
        }
    }

    const float ringScale = depthCue ? 0.80f : 1.0f;
    Color4 ringCol = col;
    if (depthCue) ringCol.a *= 0.78f;

    for (int i = 0; i < kSegs; ++i) {
        const float c0 = cs[i] * r, s0 = sn[i] * r;
        const float c1 = cs[i + 1] * r, s1 = sn[i + 1] * r;
        proj.addDepthLine3D(out, {c.x + c0, c.y + s0, c.z}, {c.x + c1, c.y + s1, c.z},
                            halfThick * ringScale, ringCol, dCenter, r, depthCue);
        proj.addDepthLine3D(out, {c.x + c0, c.y, c.z + s0}, {c.x + c1, c.y, c.z + s1},
                            halfThick * ringScale, ringCol, dCenter, r, depthCue);
        proj.addDepthLine3D(out, {c.x, c.y + c0, c.z + s0}, {c.x, c.y + c1, c.z + s1},
                            halfThick * ringScale, ringCol, dCenter, r, depthCue);
    }
}

void EmitCapsuleOrCylinder(std::vector<OverlayVertex>& out,
                           const LineProjector& proj,
                           const Vec3& p0,
                           const Vec3& p1,
                           float r,
                           bool hemisphereCaps,
                           float halfThick,
                           const Color4& col,
                           bool depthCue,
                           float fillAlphaRatio) {
    const float dx = p1.x - p0.x, dy = p1.y - p0.y, dz = p1.z - p0.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.5f) {
        EmitSphere(out, proj, p0, r, halfThick, col, depthCue, fillAlphaRatio);
        return;
    }

    const Vec3 center{0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y), 0.5f * (p0.z + p1.z)};
    const float dCenter = proj.distToCam(center);
    const float charR   = (r > 0.35f * len) ? r : (0.35f * len);

    const Vec3 axis{dx / len, dy / len, dz / len};
    Vec3 u{}, v{};
    MakeOrthonormalBasis(axis, u, v);

    // View-facing silhouette side rails + subtle core ribbon for unmistakable 3D cylinder orientation!
    if (depthCue) {
        const Vec3 toCam{proj.camPos[0] - center.x, proj.camPos[1] - center.y, proj.camPos[2] - center.z};
        Vec3 side{
            axis.y * toCam.z - axis.z * toCam.y,
            axis.z * toCam.x - axis.x * toCam.z,
            axis.x * toCam.y - axis.y * toCam.x
        };
        const float sLen = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
        if (sLen > 1e-3f) {
            const float sr = r / sLen;
            side.x *= sr; side.y *= sr; side.z *= sr;
            const Vec3 a0{p0.x + side.x, p0.y + side.y, p0.z + side.z};
            const Vec3 a1{p1.x + side.x, p1.y + side.y, p1.z + side.z};
            const Vec3 b0{p0.x - side.x, p0.y - side.y, p0.z - side.z};
            const Vec3 b1{p1.x - side.x, p1.y - side.y, p1.z - side.z};
            if (fillAlphaRatio > 0.001f) {
                Color4 fillCol = col;
                fillCol.a *= fillAlphaRatio;
                proj.addTri3D(out, a0, a1, b1, fillCol);
                proj.addTri3D(out, a0, b1, b0, fillCol);
            }
            proj.addDepthLine3D(out, a0, a1, halfThick * 1.08f, col, dCenter, 0.0f, false);
            proj.addDepthLine3D(out, b0, b1, halfThick * 1.08f, col, dCenter, 0.0f, false);
        }
    }

    constexpr int kSegs = 16;
    Vec3 ring0[kSegs + 1], ring1[kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kSegs);
        const float ca = std::cos(a) * r;
        const float sa = std::sin(a) * r;
        const float ox = u.x * ca + v.x * sa;
        const float oy = u.y * ca + v.y * sa;
        const float oz = u.z * ca + v.z * sa;
        ring0[i] = {p0.x + ox, p0.y + oy, p0.z + oz};
        ring1[i] = {p1.x + ox, p1.y + oy, p1.z + oz};
    }

    const float dP0 = proj.distToCam(p0);
    const float dP1 = proj.distToCam(p1);
    for (int i = 0; i < kSegs; ++i) {
        proj.addDepthLine3D(out, ring0[i], ring0[i + 1], halfThick, col, dP0, r, depthCue);
        proj.addDepthLine3D(out, ring1[i], ring1[i + 1], halfThick, col, dP1, r, depthCue);
    }
    for (int i = 0; i < kSegs; i += (kSegs / 4)) {
        proj.addDepthLine3D(out, ring0[i], ring1[i], halfThick * 0.85f, col, dCenter, charR, depthCue);
    }

    if (hemisphereCaps) {
        constexpr int kCapSegs = 10;
        for (int i = 0; i < kCapSegs; ++i) {
            const float a0 = (kPi * static_cast<float>(i)) / static_cast<float>(kCapSegs);
            const float a1 = (kPi * static_cast<float>(i + 1)) / static_cast<float>(kCapSegs);
            const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r;
            const float c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;

            proj.addDepthLine3D(out,
                {p0.x + u.x * c0 - axis.x * s0, p0.y + u.y * c0 - axis.y * s0, p0.z + u.z * c0 - axis.z * s0},
                {p0.x + u.x * c1 - axis.x * s1, p0.y + u.y * c1 - axis.y * s1, p0.z + u.z * c1 - axis.z * s1},
                halfThick * 0.90f, col, dP0, r, depthCue);
            proj.addDepthLine3D(out,
                {p0.x + v.x * c0 - axis.x * s0, p0.y + v.y * c0 - axis.y * s0, p0.z + v.z * c0 - axis.z * s0},
                {p0.x + v.x * c1 - axis.x * s1, p0.y + v.y * c1 - axis.y * s1, p0.z + v.z * c1 - axis.z * s1},
                halfThick * 0.90f, col, dP0, r, depthCue);

            proj.addDepthLine3D(out,
                {p1.x + u.x * c0 + axis.x * s0, p1.y + u.y * c0 + axis.y * s0, p1.z + u.z * c0 + axis.z * s0},
                {p1.x + u.x * c1 + axis.x * s1, p1.y + u.y * c1 + axis.y * s1, p1.z + u.z * c1 + axis.z * s1},
                halfThick * 0.90f, col, dP1, r, depthCue);
            proj.addDepthLine3D(out,
                {p1.x + v.x * c0 + axis.x * s0, p1.y + v.y * c0 + axis.y * s0, p1.z + v.z * c0 + axis.z * s0},
                {p1.x + v.x * c1 + axis.x * s1, p1.y + v.y * c1 + axis.y * s1, p1.z + v.z * c1 + axis.z * s1},
                halfThick * 0.90f, col, dP1, r, depthCue);
        }
    }
}

void EmitBoxCorners(std::vector<OverlayVertex>& out,
                    const LineProjector& proj,
                    const Vec3 c[8],
                    float halfThick,
                    const Color4& col,
                    bool depthCue,
                    float fillAlphaRatio) {
    Vec3 center{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; ++i) {
        center.x += c[i].x;
        center.y += c[i].y;
        center.z += c[i].z;
    }
    center.x *= 0.125f; center.y *= 0.125f; center.z *= 0.125f;
    const float dx = c[7].x - c[0].x, dy = c[7].y - c[0].y, dz = c[7].z - c[0].z;
    const float charR   = 0.35f * std::sqrt(dx * dx + dy * dy + dz * dz);
    const float dCenter = proj.distToCam(center);

    if (fillAlphaRatio > 0.001f && depthCue) {
        static constexpr int kFaces[6][4] = {
            {0, 1, 3, 2}, {4, 5, 7, 6},
            {0, 1, 5, 4}, {2, 3, 7, 6},
            {0, 2, 6, 4}, {1, 3, 7, 5}
        };
        Color4 fCol = col;
        fCol.a *= fillAlphaRatio * 0.72f;
        for (const auto& f : kFaces) {
            const Vec3 fc{
                0.25f * (c[f[0]].x + c[f[1]].x + c[f[2]].x + c[f[3]].x),
                0.25f * (c[f[0]].y + c[f[1]].y + c[f[2]].y + c[f[3]].y),
                0.25f * (c[f[0]].z + c[f[1]].z + c[f[2]].z + c[f[3]].z)
            };
            // Only tint camera-facing front faces of the box!
            if (proj.distToCam(fc) <= dCenter) {
                proj.addTri3D(out, c[f[0]], c[f[1]], c[f[2]], fCol);
                proj.addTri3D(out, c[f[0]], c[f[2]], c[f[3]], fCol);
            }
        }
    }

    static constexpr int kEdges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},
        {4,5},{5,7},{7,6},{6,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& e : kEdges) {
        proj.addDepthLine3D(out, c[e[0]], c[e[1]], halfThick, col, dCenter, charR, depthCue);
    }
}

// ============================================================================
//  Built-in 5x7 Pixel Bitmap Font (Zero Texture / Zero Font-File Dependency!)
// ============================================================================
const std::uint8_t* Glyph5x7(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    switch (ch) {
        case 'A': { static const std::uint8_t g[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
        case 'B': { static const std::uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g; }
        case 'C': { static const std::uint8_t g[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g; }
        case 'D': { static const std::uint8_t g[7]={0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return g; }
        case 'E': { static const std::uint8_t g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g; }
        case 'F': { static const std::uint8_t g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g; }
        case 'G': { static const std::uint8_t g[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; return g; }
        case 'H': { static const std::uint8_t g[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
        case 'I': { static const std::uint8_t g[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; return g; }
        case 'J': { static const std::uint8_t g[7]={0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; return g; }
        case 'K': { static const std::uint8_t g[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g; }
        case 'L': { static const std::uint8_t g[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g; }
        case 'M': { static const std::uint8_t g[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g; }
        case 'N': { static const std::uint8_t g[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return g; }
        case 'O': { static const std::uint8_t g[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
        case 'P': { static const std::uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g; }
        case 'Q': { static const std::uint8_t g[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g; }
        case 'R': { static const std::uint8_t g[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g; }
        case 'S': { static const std::uint8_t g[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g; }
        case 'T': { static const std::uint8_t g[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
        case 'U': { static const std::uint8_t g[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
        case 'V': { static const std::uint8_t g[7]={0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g; }
        case 'W': { static const std::uint8_t g[7]={0x11,0x11,0x11,0x15,0x15,0x1B,0x11}; return g; }
        case 'X': { static const std::uint8_t g[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g; }
        case 'Y': { static const std::uint8_t g[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g; }
        case 'Z': { static const std::uint8_t g[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g; }
        case '0': { static const std::uint8_t g[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g; }
        case '1': { static const std::uint8_t g[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g; }
        case '2': { static const std::uint8_t g[7]={0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}; return g; }
        case '3': { static const std::uint8_t g[7]={0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return g; }
        case '4': { static const std::uint8_t g[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g; }
        case '5': { static const std::uint8_t g[7]={0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; return g; }
        case '6': { static const std::uint8_t g[7]={0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; return g; }
        case '7': { static const std::uint8_t g[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g; }
        case '8': { static const std::uint8_t g[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g; }
        case '9': { static const std::uint8_t g[7]={0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; return g; }
        case ':': { static const std::uint8_t g[7]={0x00,0x04,0x04,0x00,0x04,0x04,0x00}; return g; }
        case '-': { static const std::uint8_t g[7]={0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g; }
        case '_': { static const std::uint8_t g[7]={0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; return g; }
        case '+': { static const std::uint8_t g[7]={0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return g; }
        case '/': { static const std::uint8_t g[7]={0x01,0x02,0x02,0x04,0x08,0x08,0x10}; return g; }
        case '|': { static const std::uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
        case '[': { static const std::uint8_t g[7]={0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return g; }
        case ']': { static const std::uint8_t g[7]={0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return g; }
        case '(': { static const std::uint8_t g[7]={0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g; }
        case ')': { static const std::uint8_t g[7]={0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g; }
        case '.': { static const std::uint8_t g[7]={0x00,0x00,0x00,0x00,0x00,0x06,0x06}; return g; }
        case ',': { static const std::uint8_t g[7]={0x00,0x00,0x00,0x00,0x06,0x04,0x08}; return g; }
        case ';': { static const std::uint8_t g[7]={0x00,0x04,0x04,0x00,0x04,0x04,0x08}; return g; }
        case '\'':{ static const std::uint8_t g[7]={0x04,0x04,0x08,0x00,0x00,0x00,0x00}; return g; }
        case '#': { static const std::uint8_t g[7]={0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}; return g; }
        case '=': { static const std::uint8_t g[7]={0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}; return g; }
        case '*': { static const std::uint8_t g[7]={0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00}; return g; }
        case '<': { static const std::uint8_t g[7]={0x02,0x04,0x08,0x10,0x08,0x04,0x02}; return g; }
        case '>': { static const std::uint8_t g[7]={0x08,0x04,0x02,0x01,0x02,0x04,0x08}; return g; }
        case '%': { static const std::uint8_t g[7]={0x19,0x19,0x02,0x04,0x08,0x13,0x13}; return g; }
        case '&': { static const std::uint8_t g[7]={0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}; return g; }
        case '!': { static const std::uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return g; }
        default:  return nullptr;
    }
}

void DrawText5x7(std::vector<OverlayVertex>& out,
                 const LineProjector& proj,
                 float x, float y, float pxScale,
                 const char* text, const Color4& col,
                 float z = 0.0f) {
    float curX = x;
    for (const char* p = text; *p; ++p) {
        if (*p != ' ') {
            const std::uint8_t* rows = Glyph5x7(*p);
            if (rows) {
                for (int ry = 0; ry < 7; ++ry) {
                    const std::uint8_t bits = rows[ry];
                    if (!bits) continue;
                    int rx = 0;
                    while (rx < 5) {
                        if ((bits & (0x10 >> rx)) == 0) { ++rx; continue; }
                        int runEnd = rx + 1;
                        while (runEnd < 5 && (bits & (0x10 >> runEnd)) != 0) ++runEnd;
                        proj.emitRect2D(out,
                                        curX + static_cast<float>(rx) * pxScale,
                                        y + static_cast<float>(ry) * pxScale,
                                        curX + static_cast<float>(runEnd) * pxScale,
                                        y + static_cast<float>(ry + 1) * pxScale,
                                        col, z);
                        rx = runEnd;
                    }
                }
            }
        }
        curX += 6.0f * pxScale;
    }
}

// Concentric metric ground rings (5m / 10m / 15m / 20m) + airborne hitbox plumb drop-pins
void EmitGroundRadarAndDropPins(std::vector<OverlayVertex>& outVerts,
                                const LineProjector& proj,
                                const ColorSpaceEncoder& csEnc,
                                const RawSnapshot& snap,
                                float baseHalfThick,
                                float px,
                                float alphaMul) {
    if (!snap.hasPlayerGround || !snap.hasCamera) return;

    auto clampA = [&](float a) -> float {
        const float v = a * alphaMul;
        return (v > 1.0f) ? 1.0f : ((v < 0.04f) ? 0.04f : v);
    };

    const float gx = snap.plFeetPos[0];
    const float gy = snap.plFeetPos[1] + 3.0f; // 3cm above floor to avoid z-fighting visually
    const float gz = snap.plFeetPos[2];

    const Color4 colRingMinor = csEnc.encode({0.28f, 0.76f, 0.94f, clampA(0.32f)});
    const Color4 colRingMajor = csEnc.encode({0.46f, 0.92f, 1.00f, clampA(0.48f)});
    const Color4 colTick      = csEnc.encode({0.38f, 0.84f, 0.98f, clampA(0.36f)});
    const Color4 colFwd       = csEnc.encode({0.25f, 1.00f, 0.72f, clampA(0.62f)});
    const Color4 colLblBg     = csEnc.encode({0.04f, 0.06f, 0.10f, clampA(0.72f)});
    const Color4 colLblTxt    = csEnc.encode({0.70f, 0.94f, 1.00f, clampA(0.90f)});

    // Camera horizontal look direction on ground (so "5M / 10M / 15M / 20M" tags sit right in front of view!)
    float lookX = gx - snap.camPos[0];
    float lookZ = gz - snap.camPos[2];
    const float lookLen = std::sqrt(lookX * lookX + lookZ * lookZ);
    if (lookLen > 1.0f) {
        lookX /= lookLen;
        lookZ /= lookLen;
    } else {
        lookX = 0.0f;
        lookZ = 1.0f;
    }

    static constexpr int   kRadiiM[4]  = {5, 10, 15, 20};
    static constexpr int   kRingSegs   = 48;
    float rCos[kRingSegs + 1], rSin[kRingSegs + 1];
    for (int s = 0; s <= kRingSegs; ++s) {
        const float a = (2.0f * kPi * static_cast<float>(s)) / static_cast<float>(kRingSegs);
        rCos[s] = std::cos(a);
        rSin[s] = std::sin(a);
    }

    for (int ri = 0; ri < 4; ++ri) {
        const float radCm = static_cast<float>(kRadiiM[ri]) * 100.0f;
        const bool isMajor = (kRadiiM[ri] == 10 || kRadiiM[ri] == 20);
        const Color4& rCol = isMajor ? colRingMajor : colRingMinor;
        const float rThick = baseHalfThick * (isMajor ? 0.72f : 0.52f);

        for (int s = 0; s < kRingSegs; ++s) {
            const Vec3 p0{gx + rCos[s] * radCm,     gy, gz + rSin[s] * radCm};
            const Vec3 p1{gx + rCos[s + 1] * radCm, gy, gz + rSin[s + 1] * radCm};
            proj.addDepthLine3D(outVerts, p0, p1, rThick, rCol, 0.0f, 0.0f, false);
        }

        // Small metric distance tag ("5M", "10M", "15M", "20M") projected onto the ring ahead of camera
        float sx = 0.0f, sy = 0.0f, zTag = 0.0f;
        if (proj.projectPoint2D({gx + lookX * radCm, gy, gz + lookZ * radCm}, sx, sy, &zTag)) {
            char tag[8]{};
            std::snprintf(tag, sizeof(tag), "%dM", kRadiiM[ri]);
            const float ts = px * 0.78f;
            const float tw = static_cast<float>(std::strlen(tag)) * 6.0f * ts;
            const float th = 7.0f * ts;
            proj.emitRect2D(outVerts, sx - tw * 0.5f - 2.0f, sy - th * 0.5f - 1.5f,
                            sx + tw * 0.5f + 2.0f, sy + th * 0.5f + 1.5f, colLblBg, zTag);
            DrawText5x7(outVerts, proj, sx - tw * 0.5f, sy - th * 0.5f, ts, tag, colLblTxt, zTag);
        }
    }

    // 8 subtle compass ticks from 0.8m..20m + Player forward direction arrow (0..2.8m)
    for (int d = 0; d < 8; ++d) {
        const float a = (2.0f * kPi * static_cast<float>(d)) / 8.0f;
        const float ca = std::cos(a), sa = std::sin(a);
        proj.addDepthLine3D(outVerts,
                            {gx + ca * 80.0f,   gy, gz + sa * 80.0f},
                            {gx + ca * 2000.0f, gy, gz + sa * 2000.0f},
                            baseHalfThick * 0.36f, colTick, 0.0f, 0.0f, false);
    }

    const float fLen = std::sqrt(snap.plForward[0] * snap.plForward[0] + snap.plForward[2] * snap.plForward[2]);
    if (fLen > 0.05f) {
        const float fx = snap.plForward[0] / fLen;
        const float fz = snap.plForward[2] / fLen;
        proj.addDepthLine3D(outVerts,
                            {gx + fx * 35.0f,  gy + 2.0f, gz + fz * 35.0f},
                            {gx + fx * 280.0f, gy + 2.0f, gz + fz * 280.0f},
                            baseHalfThick * 0.88f, colFwd, 0.0f, 0.0f, false);
    }

    // Airborne Hitbox Plumb Drop-Pins (instant horizontal X/Z distance reference for flying attacks & boulders!)
    const Color4 colEmPin  = csEnc.encode({1.00f, 0.30f, 0.30f, clampA(0.58f)});
    const Color4 colEnvPin = csEnc.encode({1.00f, 0.62f, 0.22f, clampA(0.55f)});
    std::uint32_t pinsDrawn = 0;
    for (std::uint32_t i = 0; i < snap.shapeCount && pinsDrawn < 24; ++i) {
        const ShapeRecord& s = g_shapes[i];
        if (s.cat != VolumeCat::MonsterHitbox && s.cat != VolumeCat::EnvHitbox) continue;

        Vec3 sc{};
        float lowY = 0.0f;
        if (s.type == mhw::kGeomSphere) {
            sc = {s.p[0], s.p[1], s.p[2]};
            lowY = sc.y - s.p[3];
        } else if (s.type == mhw::kGeomCapsule || s.type == mhw::kGeomLineSweptSphere || s.type == mhw::kGeomCylinder) {
            sc = {0.5f * (s.p[0] + s.p[3]), 0.5f * (s.p[1] + s.p[4]), 0.5f * (s.p[2] + s.p[5])};
            lowY = ((s.p[1] < s.p[4]) ? s.p[1] : s.p[4]) - s.p[6];
        } else if (s.type == mhw::kGeomOBB) {
            sc = {s.p[12], s.p[13], s.p[14]};
            lowY = sc.y - s.p[17];
        } else {
            continue;
        }

        const float hAbove = sc.y - gy;
        const float dx = sc.x - gx, dz = sc.z - gz;
        if (hAbove < 75.0f || hAbove > 1800.0f || (dx * dx + dz * dz) > (3500.0f * 3500.0f)) continue;

        const float topY = (lowY > gy + 20.0f) ? lowY : sc.y;
        const Color4& pCol = (s.cat == VolumeCat::MonsterHitbox) ? colEmPin : colEnvPin;
        proj.addDepthLine3D(outVerts, {sc.x, topY, sc.z}, {sc.x, gy, sc.z},
                            baseHalfThick * 0.55f, pCol, 0.0f, 0.0f, false);
        constexpr float kCross = 32.0f;
        proj.addDepthLine3D(outVerts, {sc.x - kCross, gy, sc.z}, {sc.x + kCross, gy, sc.z},
                            baseHalfThick * 0.65f, pCol, 0.0f, 0.0f, false);
        proj.addDepthLine3D(outVerts, {sc.x, gy, sc.z - kCross}, {sc.x, gy, sc.z + kCross},
                            baseHalfThick * 0.65f, pCol, 0.0f, 0.0f, false);
        ++pinsDrawn;
    }
}

static constexpr float kEmPartPaletteSrgb[18][3] = {
    {0.18f, 0.96f, 0.44f}, //  0: Vivid Emerald
    {0.20f, 0.86f, 1.00f}, //  1: Electric Cyan
    {1.00f, 0.50f, 0.22f}, //  2: Coral Tangerine
    {0.88f, 0.36f, 1.00f}, //  3: Orchid Magenta
    {0.62f, 0.98f, 0.22f}, //  4: Chartreuse Lime
    {0.36f, 0.56f, 1.00f}, //  5: Royal Azure
    {1.00f, 0.38f, 0.68f}, //  6: Hot Flamingo
    {0.16f, 0.96f, 0.80f}, //  7: Turquoise Mint
    {1.00f, 0.68f, 0.32f}, //  8: Warm Apricot
    {0.62f, 0.46f, 1.00f}, //  9: Iris Violet
    {0.42f, 0.94f, 0.58f}, // 10: Spring Jade
    {0.48f, 0.80f, 1.00f}, // 11: Sky Ice
    {0.96f, 0.52f, 0.86f}, // 12: Bubblegum Orchid
    {0.82f, 0.92f, 0.38f}, // 13: Citron Pear
    {0.28f, 0.72f, 0.88f}, // 14: Cerulean Teal
    {0.92f, 0.66f, 0.60f}, // 15: Salmon Rose
    {0.46f, 0.50f, 0.58f}, // 16: Muted Slate-Grey (CLUTCH NO-CLAW unclutchable)
    {0.86f, 0.90f, 0.96f}  // 17: Crisp Ice-White (AUX CENTER helper sphere)
};

// Standard MHW Hunter's Notes elemental order: F(ire), W(ater), T(hunder), I(ce), D(ragon)
static constexpr float kElemPaletteSrgb[5][3] = {
    {1.00f, 0.44f, 0.22f}, // F: Fire (Flame Orange-Red)
    {0.26f, 0.78f, 1.00f}, // W: Water (Aqua Sky-Blue)
    {1.00f, 0.90f, 0.24f}, // T: Thunder (Electric Yellow)
    {0.62f, 0.95f, 1.00f}, // I: Ice (Frost Cyan)
    {0.84f, 0.48f, 1.00f}  // D: Dragon (Violet-Magenta)
};

static constexpr char kElemChars[5] = {'F', 'W', 'T', 'I', 'D'};

inline Color4 ExtractSrgbColor(std::int8_t ext, float alpha) noexcept {
    switch (ext) {
        case 0:  return {1.00f, 0.28f, 0.30f, alpha}; // Red (Attack)
        case 1:  return {0.96f, 0.98f, 1.00f, alpha}; // White (Speed)
        case 2:  return {1.00f, 0.64f, 0.14f, alpha}; // Orange (Defense)
        case 3:  return {0.26f, 0.98f, 0.46f, alpha}; // Green (Heal)
        default: return {0.56f, 0.60f, 0.68f, alpha};
    }
}

inline std::uint32_t CountSelectedEmParts(std::uint32_t mask, std::uint32_t partCount, int& outSoloIdx) noexcept {
    outSoloIdx = -1;
    std::uint32_t onCnt = 0;
    const std::uint32_t lim = (partCount <= 32U) ? partCount : 32U;
    for (std::uint32_t p = 0; p < lim; ++p) {
        if ((mask & (1U << p)) != 0U) {
            outSoloIdx = (onCnt == 0U) ? static_cast<int>(p) : -1;
            ++onCnt;
        }
    }
    return onCnt;
}

void EmitMonsterPart3DLabels(std::vector<OverlayVertex>& outVerts,
                             const LineProjector& proj,
                             const ColorSpaceEncoder& csEnc,
                             const app::ViewFilterState& vs,
                             float px,
                             float resScale,
                             float alphaMul) {
    if (vs.emPartMode == app::EmPartMode::Off || vs.emLabelMode == app::EmPartLabelMode::Off) return;
    if (!g_emInspector.hasTarget || g_emInspector.partCount == 0) return;

    const bool wantPush = (vs.visMask & app::kCatPushbox) != 0U;

    auto clampA = [&](float a) noexcept -> float {
        const float v = a * alphaMul;
        return (v > 1.0f) ? 1.0f : ((v < 0.05f) ? 0.05f : v);
    };

    const bool showPhys = (vs.emLabelMode == app::EmPartLabelMode::Full ||
                           vs.emLabelMode == app::EmPartLabelMode::PhysOnly);
    const bool showElem = (vs.emLabelMode == app::EmPartLabelMode::Full);

    struct ScreenBadge {
        float sx, sy, tw, th;
        std::uint32_t slot;
        bool hasExt;
        bool hasPhys;
        bool hasElem;
        char physText[20];
        char elemTok[5][8];
    };
    ScreenBadge badges[app::kMaxEmParts]{};
    std::uint32_t bCnt = 0;

    const float ts1     = px * 0.82f;
    const float ts2     = px * 0.72f;
    const float line1H  = 7.0f * ts1;
    const float line2H  = 7.0f * ts2;
    const float lineGap = 2.0f * resScale;

    for (std::uint32_t s = 0; s < g_emInspector.partCount && s < 32U; ++s) {
        if ((vs.emPartMask & (1U << s)) == 0U) continue;
        const EmPartSlotInfo& ps = g_emInspector.parts[s];
        if (ps.anchorWeight <= 1e-4f) continue;
        if (ps.isAuxCenter && ps.hurtCount == 0 && !wantPush) continue;

        const float invW = 1.0f / ps.anchorWeight;
        const Vec3 worldPt{ps.anchorSum[0] * invW, ps.anchorSum[1] * invW, ps.anchorSum[2] * invW};
        float sx = 0.0f, sy = 0.0f;
        if (!proj.projectPoint2D(worldPt, sx, sy, nullptr)) continue;

        ScreenBadge& b = badges[bCnt++];
        b.sx      = sx;
        b.sy      = sy;
        b.slot    = s;
        b.hasExt  = (!ps.isNoClaw && !ps.isAuxCenter && ps.extract >= 0 && ps.extract <= 3);
        b.hasPhys = (showPhys && !ps.isNoClaw && !ps.isAuxCenter && ps.slash >= 0);
        b.hasElem = (showElem && !ps.isNoClaw && !ps.isAuxCenter && ps.fire >= 0);

        std::size_t chars1 = std::strlen(ps.shortTag);
        if (b.hasExt) chars1 += 4; // " [R]"
        if (b.hasPhys) {
            std::snprintf(b.physText, sizeof(b.physText), " %d/%d/%d", ps.slash, ps.blunt, ps.shot);
            chars1 += std::strlen(b.physText);
        }
        const float w1 = static_cast<float>(chars1) * 6.0f * ts1;

        float w2 = 0.0f;
        if (b.hasElem) {
            const std::int16_t ev[5] = {ps.fire, ps.water, ps.thunder, ps.ice, ps.dragon};
            std::size_t chars2 = 0;
            for (int ei = 0; ei < 5; ++ei) {
                std::snprintf(b.elemTok[ei], sizeof(b.elemTok[ei]), "%c%d", kElemChars[ei], static_cast<int>(ev[ei]));
                chars2 += std::strlen(b.elemTok[ei]);
                if (ei > 0) ++chars2; // single space between tokens
            }
            w2 = static_cast<float>(chars2) * 6.0f * ts2;
        }

        b.tw = (w1 > w2) ? w1 : w2;
        b.th = b.hasElem ? (line1H + lineGap + line2H) : line1H;
    }

    // Lightweight anti-overlap vertical nudge accounting for 1-line vs 2-line badge heights
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint32_t i = 0; i < bCnt; ++i) {
            for (std::uint32_t j = 0; j < i; ++j) {
                const float overlapX = 0.5f * (badges[i].tw + badges[j].tw) + 10.0f * resScale;
                const float minSepY  = 0.5f * (badges[i].th + badges[j].th) + 5.5f * resScale;
                if (std::fabs(badges[i].sx - badges[j].sx) < overlapX &&
                    std::fabs(badges[i].sy - badges[j].sy) < minSepY) {
                    if (badges[i].sy >= badges[j].sy) {
                        badges[i].sy = badges[j].sy + minSepY;
                    } else {
                        badges[i].sy = badges[j].sy - minSepY;
                    }
                }
            }
        }
    }

    const Color4 colBg       = csEnc.encode({0.04f, 0.05f, 0.09f, clampA(0.86f)});
    const Color4 colTxt      = csEnc.encode({0.96f, 0.98f, 1.00f, clampA(0.96f)});
    const Color4 colTxtSoft  = csEnc.encode({1.00f, 0.90f, 0.34f, clampA(0.99f)});
    const Color4 colTxtMuted = csEnc.encode({0.70f, 0.74f, 0.82f, clampA(0.92f)});
    const float padX = 4.0f * resScale;
    const float padY = 2.5f * resScale;
    const float barW = 3.0f * resScale;

    for (std::uint32_t i = 0; i < bCnt; ++i) {
        const ScreenBadge& b = badges[i];
        const EmPartSlotInfo& ps = g_emInspector.parts[b.slot];
        const std::uint8_t palIdx = (ps.colorIdx < 18U) ? ps.colorIdx : 0U;
        const auto& rgb = kEmPartPaletteSrgb[palIdx];
        const Color4 colBar = csEnc.encode({rgb[0], rgb[1], rgb[2], clampA(0.98f)});
        const float xL = b.sx - b.tw * 0.5f - padX - barW * 0.5f;
        const float xR = b.sx + b.tw * 0.5f + padX + barW * 0.5f;
        const float yT = b.sy - b.th * 0.5f - padY;
        const float yB = b.sy + b.th * 0.5f + padY;
        proj.emitRect2D(outVerts, xL, yT, xR, yB, colBg);
        proj.emitRect2D(outVerts, xL, yT, xL + barW, yB, colBar);

        const float textX0 = xL + barW + padX;
        const float textY1 = b.sy - b.th * 0.5f;
        const Color4 mainCol = ps.isNoClaw ? colTxtMuted : (ps.tenderized ? colTxtSoft : colTxt);

        float cx = textX0;
        DrawText5x7(outVerts, proj, cx, textY1, ts1, ps.shortTag, mainCol);
        cx += static_cast<float>(std::strlen(ps.shortTag)) * 6.0f * ts1;

        if (b.hasExt) {
            cx += 6.0f * ts1; // single space
            char extBuf[8]{};
            std::snprintf(extBuf, sizeof(extBuf), "[%s]", ExtractTagChar(ps.extract));
            const Color4 colExt = csEnc.encode(ExtractSrgbColor(ps.extract, clampA(0.98f)));
            DrawText5x7(outVerts, proj, cx, textY1, ts1, extBuf, colExt);
            cx += 3.0f * 6.0f * ts1;
        }
        if (b.hasPhys) {
            DrawText5x7(outVerts, proj, cx, textY1, ts1, b.physText, mainCol);
        }

        if (b.hasElem) {
            const float textY2 = textY1 + line1H + lineGap;
            const std::int16_t ev[5] = {ps.fire, ps.water, ps.thunder, ps.ice, ps.dragon};
            float ex = textX0;
            for (int ei = 0; ei < 5; ++ei) {
                const float dim = (ev[ei] > 0) ? 0.98f : 0.42f;
                const Color4 ec = csEnc.encode({
                    kElemPaletteSrgb[ei][0],
                    kElemPaletteSrgb[ei][1],
                    kElemPaletteSrgb[ei][2],
                    clampA(dim)
                });
                DrawText5x7(outVerts, proj, ex, textY2, ts2, b.elemTok[ei], ec);
                ex += static_cast<float>(std::strlen(b.elemTok[ei]) + 1U) * 6.0f * ts2;
            }
        }
    }
}

void DrawMonsterPartInspectorPanel(std::vector<OverlayVertex>& outVerts,
                                   const LineProjector& proj,
                                   const ColorSpaceEncoder& csEnc,
                                   const app::ViewFilterState& vs,
                                   float x0,
                                   float y0,
                                   float px,
                                   float resScale) {
    constexpr int kCtrlRows    = 5;
    constexpr int kMaxVisParts = 14;

    const int partCnt = static_cast<int>(g_emInspector.partCount);
    const int visParts = (partCnt > kMaxVisParts) ? kMaxVisParts : ((partCnt > 0) ? partCnt : 1);

    const float ps     = px * 0.92f;
    const float charW  = 6.0f * ps;
    const float padX   = 9.0f * resScale;
    const float padY   = 7.0f * resScale;
    const float rowH   = 10.5f * ps;
    const float hdrH   = 11.5f * ps;
    const float subH   = 10.0f * ps;
    const float ftrH   = 10.5f * ps;
    const float sepGap = 5.0f * resScale;
    const float panelW = 68.0f * charW + padX * 2.0f;
    const float panelH = padY * 2.0f + hdrH + subH +
                         static_cast<float>(kCtrlRows + visParts + 1) * rowH + sepGap * 2.0f + ftrH;

    proj.emitRect2D(outVerts, x0, y0, x0 + panelW, y0 + panelH,
                    csEnc.encode({0.04f, 0.05f, 0.08f, 0.92f}));
    proj.emitRect2D(outVerts, x0, y0, x0 + 3.5f * resScale, y0 + panelH,
                    csEnc.encode({0.18f, 0.96f, 0.52f, 0.96f}));

    const Color4 colHeader = csEnc.encode({0.35f, 0.98f, 0.68f, 0.98f});
    const Color4 colSubHdr = csEnc.encode({0.78f, 0.88f, 0.98f, 0.95f});
    const Color4 colNormal = csEnc.encode({0.86f, 0.90f, 0.95f, 0.95f});
    const Color4 colActive = csEnc.encode({1.00f, 0.98f, 0.85f, 1.00f});
    const Color4 colOn     = csEnc.encode({0.20f, 0.98f, 0.52f, 0.98f});
    const Color4 colOff    = csEnc.encode({0.52f, 0.56f, 0.64f, 0.88f});
    const Color4 colValue  = csEnc.encode({1.00f, 0.86f, 0.25f, 0.98f});
    const Color4 colFooter = csEnc.encode({0.64f, 0.74f, 0.86f, 0.92f});
    const Color4 colRowSel = csEnc.encode({0.14f, 0.26f, 0.24f, 0.92f});
    const Color4 colSlash  = csEnc.encode({0.46f, 0.52f, 0.60f, 0.85f});

    DrawText5x7(outVerts, proj, x0 + padX, y0 + padY, ps,
                "MONSTER PARTS INSPECTOR   ([TAB] <<< MAIN PANEL)", colHeader);

    char subLine[88]{};
    if (g_emInspector.hasTarget) {
        std::snprintf(subLine, sizeof(subLine),
                      "TARGET: %s [%s #%u/%u]  %u PARTS  |  [EXT] S/B/A   F/W/T/I/D",
                      g_emInspector.targetName,
                      g_emInspector.targetLarge ? "LARGE" : "SMALL",
                      g_emInspector.targetSlot + 1U,
                      g_emInspector.monsterCount,
                      g_emInspector.partCount);
    } else {
        std::snprintf(subLine, sizeof(subLine),
                      "TARGET: NO ACTIVE MONSTER IN SCENE (ENTER QUEST / TRAINING)");
    }
    DrawText5x7(outVerts, proj, x0 + padX, y0 + padY + hdrH - 1.0f * resScale, ps * 0.88f,
                subLine, g_emInspector.hasTarget ? colSubHdr : colOff);

    proj.emitRect2D(outVerts, x0, y0 + padY + hdrH + subH - 2.5f * resScale,
                    x0 + panelW, y0 + padY + hdrH + subH - 1.5f * resScale,
                    csEnc.encode({0.22f, 0.34f, 0.32f, 0.85f}));

    float curY = y0 + padY + hdrH + subH + 1.0f * resScale;
    const float valX = x0 + panelW - padX - 22.0f * charW;

    const int backRow   = kCtrlRows + partCnt;
    const int totalRows = backRow + 1;
    const int curRow = (totalRows > 0)
                           ? ((vs.partPanelCursor < 0) ? 0
                              : ((vs.partPanelCursor >= totalRows) ? (totalRows - 1) : vs.partPanelCursor))
                           : 0;

    int soloIdx = -1;
    const std::uint32_t selParts = CountSelectedEmParts(vs.emPartMask, g_emInspector.partCount, soloIdx);

    const char* kCtrlLabels[kCtrlRows] = {
        "[<>] PARTITION MODE",
        "[<>] UNSELECTED PARTS",
        "[<>] 3D PART LABELS",
        "[<>] TARGET MONSTER",
        "[=]  QUICK PART SELECT"
    };

    for (int r = 0; r < kCtrlRows; ++r) {
        const bool sel = (curRow == r);
        if (sel) {
            proj.emitRect2D(outVerts, x0 + 4.0f * resScale, curY - 1.0f * resScale,
                            x0 + panelW - 3.0f * resScale, curY + rowH - 1.5f * resScale, colRowSel);
            DrawText5x7(outVerts, proj, x0 + padX * 0.45f, curY + 1.0f * ps, ps, ">", colValue);
        }
        DrawText5x7(outVerts, proj, x0 + padX + 6.5f * ps, curY + 1.0f * ps, ps,
                    kCtrlLabels[r], sel ? colActive : colNormal);

        char valBuf[36]{};
        Color4 vCol = colOff;
        if (r == 0) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::EmPartModeName(vs.emPartMode));
            vCol = (vs.emPartMode != app::EmPartMode::Off) ? colOn : colOff;
        } else if (r == 1) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::EmUnselectedStyleName(vs.emUnsStyle));
            vCol = (vs.emUnsStyle == app::EmUnselectedStyle::Ghost) ? colValue : colOn;
        } else if (r == 2) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::EmPartLabelModeName(vs.emLabelMode));
            vCol = (vs.emLabelMode != app::EmPartLabelMode::Off) ? colOn : colOff;
        } else if (r == 3) {
            if (vs.emTargetScope == 0) {
                std::snprintf(valBuf, sizeof(valBuf), "< AUTO (SOLO) >");
                vCol = colOn;
            } else if (vs.emTargetScope == 1) {
                std::snprintf(valBuf, sizeof(valBuf), "< AUTO (+OTHERS) >");
                vCol = colValue;
            } else if (g_emInspector.hasTarget) {
                std::snprintf(valBuf, sizeof(valBuf), "< #%u %s >",
                              g_emInspector.targetSlot + 1U, g_emInspector.targetName);
                vCol = colValue;
            } else {
                std::snprintf(valBuf, sizeof(valBuf), "< LOCK #%d >", vs.emTargetScope - 1);
                vCol = colValue;
            }
        } else if (r == 4) {
            if (g_emInspector.partCount == 0) {
                std::snprintf(valBuf, sizeof(valBuf), "< ALL ON >");
                vCol = colOff;
            } else if (soloIdx >= 0 && static_cast<std::uint32_t>(soloIdx) < g_emInspector.partCount) {
                std::snprintf(valBuf, sizeof(valBuf), "< SOLO: %s >", g_emInspector.parts[soloIdx].shortTag);
                vCol = colValue;
            } else if (selParts >= g_emInspector.partCount) {
                std::snprintf(valBuf, sizeof(valBuf), "< ALL ON (%u/%u) >", selParts, g_emInspector.partCount);
                vCol = colOn;
            } else {
                std::snprintf(valBuf, sizeof(valBuf), "< %u / %u ON >", selParts, g_emInspector.partCount);
                vCol = (selParts > 0) ? colValue : colOff;
            }
        }
        DrawText5x7(outVerts, proj, valX, curY + 1.0f * ps, ps, valBuf, vCol);
        curY += rowH;
    }

    proj.emitRect2D(outVerts, x0 + padX, curY + 1.5f * resScale,
                    x0 + panelW - padX, curY + 2.5f * resScale,
                    csEnc.encode({0.22f, 0.34f, 0.32f, 0.80f}));
    curY += sepGap;

    if (partCnt == 0) {
        DrawText5x7(outVerts, proj, x0 + padX + 6.5f * ps, curY + 1.0f * ps, ps,
                    "WAITING FOR ACTIVE MONSTER COLLISION...", colOff);
        curY += rowH;
    } else {
        int startP = 0;
        if (partCnt > kMaxVisParts && curRow >= kCtrlRows && curRow < kCtrlRows + partCnt) {
            startP = (curRow - kCtrlRows) - (kMaxVisParts / 2);
            if (startP < 0) startP = 0;
            if (startP + kMaxVisParts > partCnt) startP = partCnt - kMaxVisParts;
        }
        static constexpr const char* kQuickKeys[14] = {
            "[1]", "[2]", "[3]", "[4]", "[5]", "[6]", "[7]",
            "[8]", "[9]", "[0]", "[-]", "[/]", "[,]", "[.]"
        };

        for (int vi = 0; vi < visParts; ++vi) {
            const int p = startP + vi;
            const int rowIdx = kCtrlRows + p;
            const bool sel = (curRow == rowIdx);
            if (sel) {
                proj.emitRect2D(outVerts, x0 + 4.0f * resScale, curY - 1.0f * resScale,
                                x0 + panelW - 3.0f * resScale, curY + rowH - 1.5f * resScale, colRowSel);
                DrawText5x7(outVerts, proj, x0 + padX * 0.45f, curY + 1.0f * ps, ps, ">", colValue);
            }

            const EmPartSlotInfo& pi = g_emInspector.parts[p];
            const bool partOn = (p < 32) ? ((vs.emPartMask & (1U << static_cast<std::uint32_t>(p))) != 0U) : true;
            const std::uint8_t palIdx = (pi.colorIdx < 18U) ? pi.colorIdx : 0U;
            const auto& rgb = kEmPartPaletteSrgb[palIdx];
            const float swAlpha = (vs.emPartMode == app::EmPartMode::Off || partOn) ? 0.96f : 0.28f;

            float lx = x0 + padX + 5.5f * ps;
            proj.emitRect2D(outVerts, lx, curY + 1.2f * ps, lx + 5.2f * ps, curY + 6.8f * ps,
                            csEnc.encode({rgb[0], rgb[1], rgb[2], swAlpha}));
            lx += 7.0f * ps;

            const float textY = curY + 1.0f * ps;
            const Color4 rowCol = sel ? colActive
                                      : (partOn ? (pi.tenderized ? colValue : colNormal) : colOff);

            // Column 1: [1] + Part ID (12 chars)
            char idBuf[24]{};
            const char* kTag = (p < 14) ? kQuickKeys[p] : "   ";
            std::snprintf(idBuf, sizeof(idBuf), "%s %-8s", kTag, pi.rowLabel);
            DrawText5x7(outVerts, proj, lx, textY, ps, idBuf, rowCol);
            float colX = lx + 12.5f * charW;

            if (pi.isNoClaw) {
                DrawText5x7(outVerts, proj, colX, textY, ps, "UNCLUTCHABLE", sel ? colActive : colOff);
                colX += 29.0f * charW;
            } else if (pi.isAuxCenter) {
                DrawText5x7(outVerts, proj, colX, textY, ps, "CENTER HELPER SPHERE", sel ? colActive : colSubHdr);
                colX += 29.0f * charW;
            } else {
                // Column 2: Color-coded Kinsect Extract [R/W/O/G] (3 chars)
                char extBuf[8]{};
                std::snprintf(extBuf, sizeof(extBuf), "[%s]", ExtractTagChar(pi.extract));
                const float extA = (vs.emPartMode == app::EmPartMode::Off || partOn) ? 0.98f : 0.45f;
                const Color4 extCol = csEnc.encode(ExtractSrgbColor(pi.extract, extA));
                DrawText5x7(outVerts, proj, colX, textY, ps, extBuf, extCol);
                colX += 4.0f * charW;

                // Column 3: Live Physical Hitzones S/B/A (8 chars)
                char physBuf[16]{};
                if (pi.slash >= 0) {
                    std::snprintf(physBuf, sizeof(physBuf), "%2d/%2d/%2d", pi.slash, pi.blunt, pi.shot);
                } else {
                    std::snprintf(physBuf, sizeof(physBuf), "--/--/--");
                }
                DrawText5x7(outVerts, proj, colX, textY, ps, physBuf, rowCol);
                colX += 9.8f * charW;

                // Column 4: Live Color-Coded Elemental Hitzones F/W/T/I/D (14 chars)
                if (pi.fire >= 0) {
                    const std::int16_t ev[5] = {pi.fire, pi.water, pi.thunder, pi.ice, pi.dragon};
                    float ex = colX;
                    for (int ei = 0; ei < 5; ++ei) {
                        char eb[8]{};
                        std::snprintf(eb, sizeof(eb), "%2d", static_cast<int>(ev[ei]));
                        const float ea = partOn ? ((ev[ei] > 0) ? 0.98f : 0.44f) : 0.32f;
                        const Color4 ec = csEnc.encode({
                            kElemPaletteSrgb[ei][0],
                            kElemPaletteSrgb[ei][1],
                            kElemPaletteSrgb[ei][2],
                            ea
                        });
                        DrawText5x7(outVerts, proj, ex, textY, ps, eb, ec);
                        ex += static_cast<float>(std::strlen(eb)) * charW;
                        if (ei < 4) {
                            DrawText5x7(outVerts, proj, ex, textY, ps, "/", colSlash);
                            ex += charW;
                        }
                    }
                } else {
                    DrawText5x7(outVerts, proj, colX, textY, ps, "--/--/--/--/--", colOff);
                }
                colX += 15.2f * charW;
            }

            // Column 5: Volume counts (hurt + push)
            char cntBuf[16]{};
            std::snprintf(cntBuf, sizeof(cntBuf), "(%uH+%uP)", pi.hurtCount, pi.pushCount);
            DrawText5x7(outVerts, proj, colX, textY, ps, cntBuf, sel ? colActive : colFooter);

            char stBuf[24]{};
            Color4 stCol = colOff;
            if (vs.emPartMode == app::EmPartMode::Off) {
                std::snprintf(stBuf, sizeof(stBuf), "[MODE:OFF]");
                stCol = colOff;
            } else if (soloIdx == p) {
                std::snprintf(stBuf, sizeof(stBuf), "[SOLO]");
                stCol = colValue;
            } else if (partOn) {
                std::snprintf(stBuf, sizeof(stBuf), "[ON]");
                stCol = colOn;
            } else {
                std::snprintf(stBuf, sizeof(stBuf),
                              (vs.emUnsStyle == app::EmUnselectedStyle::Ghost) ? "[GHOST]" : "[HIDDEN]");
                stCol = colOff;
            }
            const float rightStX = x0 + panelW - padX - 10.2f * charW;
            DrawText5x7(outVerts, proj, rightStX, textY, ps, stBuf, stCol);
            curY += rowH;
        }
    }

    proj.emitRect2D(outVerts, x0 + padX, curY + 1.5f * resScale,
                    x0 + panelW - padX, curY + 2.5f * resScale,
                    csEnc.encode({0.22f, 0.34f, 0.32f, 0.80f}));
    curY += sepGap;

    {
        const bool selBack = (curRow == backRow);
        if (selBack) {
            proj.emitRect2D(outVerts, x0 + 4.0f * resScale, curY - 1.0f * resScale,
                            x0 + panelW - 3.0f * resScale, curY + rowH - 1.5f * resScale, colRowSel);
            DrawText5x7(outVerts, proj, x0 + padX * 0.45f, curY + 1.0f * ps, ps, ">", colValue);
        }
        DrawText5x7(outVerts, proj, x0 + padX + 6.5f * ps, curY + 1.0f * ps, ps,
                    "[TAB] <<< BACK TO MAIN PANEL", selBack ? colActive : colHeader);
        DrawText5x7(outVerts, proj, valX, curY + 1.0f * ps, ps, "[SPACE / A]", selBack ? colValue : colFooter);
        curY += rowH;
    }

    proj.emitRect2D(outVerts, x0, curY + 1.0f * resScale,
                    x0 + panelW, curY + 2.0f * resScale,
                    csEnc.encode({0.22f, 0.34f, 0.32f, 0.80f}));
    DrawText5x7(outVerts, proj, x0 + padX, curY + 3.0f * resScale, ps * 0.86f,
                "ARROWS/DPAD:NAV  SPACE/A:SET  TAB:MAIN  =:SOLO  1-0:PART  P:EXIT", colFooter);
}

void DrawInteractiveFilterPanel(std::vector<OverlayVertex>& outVerts,
                                const LineProjector& proj,
                                const ColorSpaceEncoder& csEnc,
                                const app::ViewFilterState& vs,
                                float x0,
                                float y0,
                                float px,
                                float resScale) {
    if (vs.panelPage == 1) {
        DrawMonsterPartInspectorPanel(outVerts, proj, csEnc, vs, x0, y0, px, resScale);
        return;
    }

    constexpr int kMainRows = 18;
    const float ps     = px * 0.92f;
    const float padX   = 9.0f * resScale;
    const float padY   = 7.0f * resScale;
    const float rowH   = 10.5f * ps;
    const float hdrH   = 11.5f * ps;
    const float ftrH   = 10.5f * ps;
    const float sepGap = 5.0f * resScale;
    const float panelW = 60.0f * 6.0f * ps + padX * 2.0f;
    const float panelH = padY * 2.0f + hdrH + static_cast<float>(kMainRows) * rowH + sepGap * 2.0f + ftrH;

    // Panel background + left accent + top/bottom borders
    proj.emitRect2D(outVerts, x0, y0, x0 + panelW, y0 + panelH,
                    csEnc.encode({0.04f, 0.05f, 0.08f, 0.90f}));
    proj.emitRect2D(outVerts, x0, y0, x0 + 3.5f * resScale, y0 + panelH,
                    csEnc.encode({0.20f, 0.85f, 1.00f, 0.96f}));
    proj.emitRect2D(outVerts, x0, y0 + padY + hdrH - 2.0f * resScale,
                    x0 + panelW, y0 + padY + hdrH - 1.0f * resScale,
                    csEnc.encode({0.22f, 0.30f, 0.42f, 0.85f}));

    const Color4 colHeader = csEnc.encode({0.45f, 0.92f, 1.00f, 0.98f});
    const Color4 colNormal = csEnc.encode({0.86f, 0.90f, 0.95f, 0.95f});
    const Color4 colActive = csEnc.encode({1.00f, 0.98f, 0.85f, 1.00f});
    const Color4 colOn     = csEnc.encode({0.20f, 0.98f, 0.52f, 0.98f});
    const Color4 colOff    = csEnc.encode({0.52f, 0.56f, 0.64f, 0.88f});
    const Color4 colValue  = csEnc.encode({1.00f, 0.86f, 0.25f, 0.98f});
    const Color4 colFooter = csEnc.encode({0.64f, 0.74f, 0.86f, 0.92f});
    const Color4 colRowSel = csEnc.encode({0.14f, 0.24f, 0.36f, 0.92f});

    DrawText5x7(outVerts, proj, x0 + padX, y0 + padY, ps,
                "FILTER & SCENE PANEL   ([TAB] EM PARTS  [=] SCOPE)", colHeader);

    float curY = y0 + padY + hdrH + 1.5f * resScale;
    const float valX = x0 + panelW - padX - 19.5f * 6.0f * ps;

    struct RowSpec {
        const char* label;
        bool hasSwatch;
        Color4 swatchSrgb;
    };

    const RowSpec kRows[kMainRows] = {
        {"[<>] SCOPE PRESET",          false, {0, 0, 0, 0}},
        {"[1]  PL HURTBOX (+IFRAME)",  true,  {0.15f, 0.85f, 1.00f, 0.96f}},
        {"[2]  PL HITBOX (WEAPON/ATK)",true,  {1.00f, 0.86f, 0.10f, 0.96f}},
        {"[3]  PALICO / OTOMO",        true,  {0.38f, 0.78f, 1.00f, 0.96f}},
        {"[4]  MONSTER HURTBOX",       true,  {0.15f, 0.95f, 0.38f, 0.96f}},
        {"[5]  MONSTER HITBOX (ATK)",  true,  {1.00f, 0.16f, 0.18f, 0.96f}},
        {"[6]  BODY PUSHBOX (PL+EM)",  true,  {0.72f, 0.56f, 1.00f, 0.92f}},
        {"[7]  MAP ENTITIES (POLE/GM)",true,  {0.22f, 0.94f, 0.76f, 0.96f}},
        {"[8]  MAP TERRAIN (.SBC)",    true,  {0.92f, 0.64f, 0.26f, 0.96f}},
        {"[9]  HIDE MONSTER MESH",     false, {0, 0, 0, 0}},
        {"[0]  HIDE MAP/STAGE MESH",   false, {0, 0, 0, 0}},
        {"[-]  PURE 3D / CANVAS",      false, {0, 0, 0, 0}},
        {"[/]  TERRAIN STYLE & RINGS", false, {0, 0, 0, 0}},
        {"[,]  SBC COLLISION TIER",    true,  {0.46f, 0.78f, 0.98f, 0.94f}},
        {"[.]  SPECIAL SBC LAYERS",    true,  {0.74f, 0.44f, 0.94f, 0.92f}},
        {"[<>] ENTITY LINE WIDTH",     false, {0, 0, 0, 0}},
        {"[<>] SBC / RAW LINE WIDTH",  false, {0, 0, 0, 0}},
        {"[TAB] MONSTER PART FILTER",  true,  {0.18f, 0.96f, 0.52f, 0.96f}}
    };

    const std::uint32_t kRowBits[9] = {
        0,
        app::kCatPlHurtbox,
        app::kCatPlHitbox,
        app::kCatOtomo,
        app::kCatEmHurtbox,
        app::kCatEmHitbox,
        app::kCatPushbox,
        app::kCatEnvObject,
        app::kCatStageMesh
    };

    for (int r = 0; r < kMainRows; ++r) {
        if (r == 9 || r == 17) {
            proj.emitRect2D(outVerts, x0 + padX, curY + 1.5f * resScale,
                            x0 + panelW - padX, curY + 2.5f * resScale,
                            csEnc.encode({0.22f, 0.30f, 0.42f, 0.80f}));
            curY += sepGap;
        }

        const bool sel = (vs.panelCursor == r);
        if (sel) {
            proj.emitRect2D(outVerts, x0 + 4.0f * resScale, curY - 1.0f * resScale,
                            x0 + panelW - 3.0f * resScale, curY + rowH - 1.5f * resScale,
                            colRowSel);
            DrawText5x7(outVerts, proj, x0 + padX * 0.45f, curY + 1.0f * ps, ps, ">", colValue);
        }

        float lx = x0 + padX + 6.5f * ps;
        if (kRows[r].hasSwatch) {
            proj.emitRect2D(outVerts, lx, curY + 1.2f * ps, lx + 5.2f * ps, curY + 6.8f * ps,
                            csEnc.encode(kRows[r].swatchSrgb));
        }
        lx += 7.5f * ps;

        DrawText5x7(outVerts, proj, lx, curY + 1.0f * ps, ps, kRows[r].label,
                    sel ? colActive : colNormal);

        char valBuf[36]{};
        Color4 vCol = colOff;
        if (r == 0) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::MatchPresetName(vs.visMask));
            vCol = colValue;
        } else if (r >= 1 && r <= 7) {
            const bool on = (vs.visMask & kRowBits[r]) != 0;
            std::snprintf(valBuf, sizeof(valBuf), on ? "[ON]" : "[OFF]");
            vCol = on ? colOn : colOff;
        } else if (r == 8) {
            const bool on = (vs.visMask & app::kCatStageMesh) != 0;
            const int radM = static_cast<int>(app::SbcRadiusMetersFromIdx(vs.sbcRadiusIdx) + 0.5f);
            if (on) {
                std::snprintf(valBuf, sizeof(valBuf), "< ON : %dM >", radM);
                vCol = colOn;
            } else {
                std::snprintf(valBuf, sizeof(valBuf), "< OFF (%dM) >", radM);
                vCol = colOff;
            }
        } else if (r == 9) {
            std::snprintf(valBuf, sizeof(valBuf), vs.hideMonster ? "[ON: HIDDEN]" : "[OFF]");
            vCol = vs.hideMonster ? colValue : colOff;
        } else if (r == 10) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::StageHideModeName(vs.stageHide));
            vCol = (vs.stageHide != app::StageHideMode::Off) ? colValue : colOff;
        } else if (r == 11) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::BackdropModeName(vs.backdrop));
            vCol = (vs.backdrop != app::BackdropMode::Off) ? colValue : colOff;
        } else if (r == 12) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::DepthGuideModeName(vs.depthMode));
            vCol = (vs.depthMode != app::DepthGuideMode::RawWire) ? colOn : colOff;
        } else if (r == 13) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::SbcMeshTierName(vs.sbcTier));
            vCol = (vs.sbcTier == app::SbcMeshTier::HighOnly) ? colValue : colOn;
        } else if (r == 14) {
            std::snprintf(valBuf, sizeof(valBuf), "< %s >", app::SpecialSbcStyleName(vs.specialSbc));
            vCol = (vs.specialSbc == app::SpecialSbcStyle::Off)
                       ? colOff
                       : ((vs.specialSbc == app::SpecialSbcStyle::Opaque) ? colValue : colOn);
        } else if (r == 15) {
            const float sc = app::LineScaleFromIdx(vs.entityLineIdx);
            std::snprintf(valBuf, sizeof(valBuf), "< %.2fx >", sc);
            vCol = (vs.entityLineIdx == app::kDefaultLineScaleIdx) ? colOn : colValue;
        } else if (r == 16) {
            const float sc = app::LineScaleFromIdx(vs.sbcLineIdx);
            std::snprintf(valBuf, sizeof(valBuf), "< %.2fx >", sc);
            vCol = (vs.sbcLineIdx == app::kDefaultLineScaleIdx) ? colOn : colValue;
        } else if (r == 17) {
            if (vs.emPartMode == app::EmPartMode::Off) {
                std::snprintf(valBuf, sizeof(valBuf), "< OFF  [OPEN >>>] >");
                vCol = colOff;
            } else {
                int sIdx = -1;
                const std::uint32_t onN = CountSelectedEmParts(vs.emPartMask, g_emInspector.partCount, sIdx);
                const char* mShort = (vs.emPartMode == app::EmPartMode::Hitzone) ? "HZ" : "CLAW";
                if (g_emInspector.partCount > 0) {
                    std::snprintf(valBuf, sizeof(valBuf), "< %s %u/%u >>> >", mShort, onN, g_emInspector.partCount);
                } else {
                    std::snprintf(valBuf, sizeof(valBuf), "< %s ON >>> >", mShort);
                }
                vCol = colOn;
            }
        }

        DrawText5x7(outVerts, proj, valX, curY + 1.0f * ps, ps, valBuf, vCol);
        curY += rowH;
    }

    proj.emitRect2D(outVerts, x0, curY + 1.0f * resScale,
                    x0 + panelW, curY + 2.0f * resScale,
                    csEnc.encode({0.22f, 0.30f, 0.42f, 0.80f}));
    DrawText5x7(outVerts, proj, x0 + padX, curY + 3.0f * resScale, ps * 0.88f,
                "ARROWS/DPAD:NAV  SPACE/A:SET  TAB:PARTS  1-0/-///,/./=:KB", colFooter);
}

} // namespace

EmInspectorCounts GetLastEmInspectorCounts() noexcept {
    return g_lastEmCounts;
}

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
                       FrameStats& outStats) {
    outVerts.clear();
    outStats = FrameStats{};

    const bool anyFreeze    = (freezeMode != freeze::Mode::None);
    const bool activePure3D = visEnabled &&
                              (viewState.backdrop == app::BackdropMode::HidePlVfx ||
                               viewState.backdrop == app::BackdropMode::HideAll3D);
    const bool hasDimScrim  = visEnabled &&
                              (viewState.backdrop == app::BackdropMode::DimScrim);
    const bool hasBackdrop  = activePure3D || hasDimScrim;
    const bool activeHideEm = visEnabled && viewState.hideMonster;
    const bool activeHideSt = visEnabled &&
                              (viewState.stageHide != app::StageHideMode::Off);
    const bool anySceneMod  = activeHideEm || activeHideSt || hasBackdrop;
    if (!exeBase || screenW < 64.0f || screenH < 64.0f) return;
    if (!visEnabled && !anyFreeze && !viewState.panelOpen && !anySceneMod && !showStartupToast) return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeBase + dos->e_lfanew);
    const std::uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // MHW world coordinates are in centimeters (1 meter = 100.0f units)
    const float sbcRadiusWorldUnits = app::SbcRadiusMetersFromIdx(viewState.sbcRadiusIdx) * 100.0f;
    const bool  isOpaqueSbc         = (viewState.depthMode == app::DepthGuideMode::Opaque ||
                                       viewState.depthMode == app::DepthGuideMode::OpaqueNoRings);
    const bool  includeAuxSbc       = (viewState.specialSbc != app::SpecialSbcStyle::Off);

    RawSnapshot snap{};
    CollectSnapshotSEH(exeBase, exeEnd, visEnabled, viewState, sbcRadiusWorldUnits,
                       includeAuxSbc, &snap);
    outStats.hasCamera   = snap.hasCamera;
    outStats.totalShapes = snap.shapeCount;
    outStats.sbcEdges    = snap.sbcEdgeCount;
    if (snap.hasCamera) {
        const float m32 = std::fabs(snap.proj[14]);
        const float m22 = std::fabs(std::fabs(snap.proj[10]) - 1.0f);
        if (std::isfinite(m32) && m32 >= 0.5f && m32 <= 2000.0f &&
            std::isfinite(m22) && m22 < 0.5f) {
            outStats.projM32 = m32;
            outStats.projM22 = m22;
        }
    }

    ColorSpaceEncoder csEnc;
    csEnc.init(backBufferDxgiFormat, snap.hdrActive, snap.hdrWhiteNits, cfg.hdrNits);

    const float resScale = screenH / 1080.0f;
    const float baseHalfThick   = 0.5f * cfg.lineThickness * ((resScale > 0.75f) ? resScale : 0.75f);
    const float entityHalfThick = baseHalfThick * app::LineScaleFromIdx(viewState.entityLineIdx);
    const float sbcHalfThick    = baseHalfThick * app::LineScaleFromIdx(viewState.sbcLineIdx);
    const float px = (resScale >= 0.8f) ? std::floor(resScale * 2.0f + 0.5f) : 2.0f;
    const float alphaMul = cfg.opacityScale;

    LineProjector proj;
    if (snap.hasCamera) {
        proj.init(snap.view, snap.proj, snap.camPos, screenW, screenH);
    } else {
        float ident[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float zero[3]{0,0,0};
        proj.init(ident, ident, zero, screenW, screenH);
    }

    // Note: DIM 45% (BackdropMode::DimScrim) is applied pre-UI via MHW's 3D uColorCorrectFilter
    // in scene_visibility.cpp so 2D Player UI (uGUI / uBGTexture) always remains 100% full brightness!

    auto clampA = [&](float a) -> float {
        const float v = a * alphaMul;
        return (v > 1.0f) ? 1.0f : ((v < 0.012f) ? 0.012f : v);
    };

    const bool use3DShade   = (viewState.depthMode != app::DepthGuideMode::RawWire);
    const bool sbcOn        = (viewState.visMask & app::kCatStageMesh) != 0;
    const bool showRings    = (viewState.depthMode == app::DepthGuideMode::AlwaysRings) ||
                              ((viewState.depthMode == app::DepthGuideMode::Auto ||
                                viewState.depthMode == app::DepthGuideMode::Opaque) &&
                               (activeHideSt || activeHideEm || hasBackdrop || sbcOn));

    // 2. 3D Stage .SBC Shaded Terrain + Ground Metric Radar Rings + Depth-Cued Entity Volumes
    if (visEnabled && snap.hasCamera &&
        (snap.sbcTriCount > 0 || snap.sbcEdgeCount > 0 || snap.shapeCount > 0 || showRings)) {
        outVerts.reserve(outVerts.size() +
                         static_cast<std::size_t>(snap.sbcTriCount) * 2 +
                         static_cast<std::size_t>(snap.sbcEdgeCount) * 2 +
                         static_cast<std::size_t>(snap.shapeCount) * 180 +
                         2048);

        // 2a. Lit 3D Shaded .SBC Surfaces (Opaque Z-Buffered OR Semi-Transparent Painter's Sort) + Creases
        //     Solid physical terrain (layer 0) uses primary architectural styling; special / non-solid
        //     layers (barriers, water sheets, special probes, mOccludeSbc) follow viewState.specialSbc
        //     (OFF / GHOST 96%T / LIGHT 93%T [default] / MED 85%T / VIVID 72%T / OPAQUE 0%T).
        if (snap.sbcTriCount > 0 || snap.sbcEdgeCount > 0) {
            static constexpr float kAuxFaceRgb[4][3] = {
                {0.74f, 0.40f, 0.94f}, // 1 (idx 0): Barrier / Pass-Through Curtain (Soft Lavender-Violet)
                {0.16f, 0.86f, 0.80f}, // 2 (idx 1): Water Surface Layer (Seafoam-Aqua)
                {0.48f, 0.78f, 0.64f}, // 3 (idx 2): Special Probe Sheet (Muted Sage-Mint)
                {0.56f, 0.62f, 0.76f}  // 4 (idx 3): Non-Physical / Occluder .SBC (Whisper Slate-Grey)
            };
            static constexpr float kAuxEdgeRgb[4][3] = {
                {0.82f, 0.52f, 1.00f}, // Barrier outline
                {0.30f, 0.94f, 0.88f}, // Water outline
                {0.58f, 0.86f, 0.72f}, // Special probe outline
                {0.66f, 0.72f, 0.86f}  // Occluder / non-physical outline
            };
            static constexpr float kAuxSharpA[4]  = {0.36f, 0.34f, 0.26f, 0.22f};
            static constexpr float kAuxSoftA[4]   = {0.18f, 0.16f, 0.13f, 0.11f};
            static constexpr float kAuxSharpHt[4] = {0.56f, 0.52f, 0.46f, 0.42f};
            static constexpr float kAuxSoftHt[4]  = {0.38f, 0.36f, 0.32f, 0.30f};

            float auxFaceMul = 0.68f; // Default: Light (93% transparent — slightly more transparent than before!)
            float auxEdgeMul = 0.78f;
            float auxCapMul  = 1.00f;
            bool  auxOpaqueSolid = false;
            switch (viewState.specialSbc) {
                case app::SpecialSbcStyle::Off:    auxFaceMul = 0.00f; auxEdgeMul = 0.00f; break;
                case app::SpecialSbcStyle::Ghost:  auxFaceMul = 0.42f; auxEdgeMul = 0.52f; auxCapMul = 0.55f; break;
                case app::SpecialSbcStyle::Light:  auxFaceMul = 0.68f; auxEdgeMul = 0.78f; auxCapMul = 0.85f; break;
                case app::SpecialSbcStyle::Medium: auxFaceMul = 1.45f; auxEdgeMul = 1.18f; auxCapMul = 1.60f; break;
                case app::SpecialSbcStyle::Vivid:  auxFaceMul = 2.65f; auxEdgeMul = 1.55f; auxCapMul = 2.60f; break;
                case app::SpecialSbcStyle::Opaque: auxFaceMul = 4.50f; auxEdgeMul = 1.85f; auxCapMul = 3.80f; auxOpaqueSolid = true; break;
                default: break;
            }

            Color4 auxEdgeSharp[4]{};
            Color4 auxEdgeSoft[4]{};
            for (int ak = 0; ak < 4; ++ak) {
                float sa = kAuxSharpA[ak] * auxEdgeMul;
                float fa = kAuxSoftA[ak]  * auxEdgeMul;
                if (sa > 0.90f) sa = 0.90f;
                if (fa > 0.60f) fa = 0.60f;
                auxEdgeSharp[ak] = csEnc.encode({kAuxEdgeRgb[ak][0], kAuxEdgeRgb[ak][1], kAuxEdgeRgb[ak][2], (sa > 0.0f) ? clampA(sa) : 0.0f});
                auxEdgeSoft[ak]  = csEnc.encode({kAuxEdgeRgb[ak][0], kAuxEdgeRgb[ak][1], kAuxEdgeRgb[ak][2], (fa > 0.0f) ? clampA(fa) : 0.0f});
            }

            auto buildAuxFaceLut = [&](Color4 outLut[4][16], bool opaqueTerrainMode) {
                static constexpr float kAuxOpaqueA[4] = {0.085f, 0.095f, 0.055f, 0.032f};
                static constexpr float kAuxSemiA[4]   = {0.055f, 0.065f, 0.040f, 0.024f};
                static constexpr float kAuxMaxA[4]    = {0.160f, 0.170f, 0.100f, 0.060f};
                const float opScale = opaqueTerrainMode ? 1.0f : (0.75f + 0.25f * cfg.sbcSurfaceOpacity);
                for (int ak = 0; ak < 4; ++ak) {
                    float ba = (opaqueTerrainMode ? kAuxOpaqueA[ak] : kAuxSemiA[ak]) * auxFaceMul * opScale * alphaMul;
                    float capA = kAuxMaxA[ak] * auxCapMul;
                    if (capA > 0.72f) capA = 0.72f;
                    if (ba > capA) ba = capA;
                    if (ba < 0.008f) ba = 0.008f;
                    for (int s = 0; s < 16; ++s) {
                        const float sh = 0.44f + (0.62f / 15.0f) * static_cast<float>(s);
                        auto cl = [](float v) { return (v > 1.0f) ? 1.0f : ((v < 0.0f) ? 0.0f : v); };
                        outLut[ak][s] = csEnc.encode({
                            cl(kAuxFaceRgb[ak][0] * sh),
                            cl(kAuxFaceRgb[ak][1] * sh),
                            cl(kAuxFaceRgb[ak][2] * sh),
                            ba
                        });
                    }
                }
            };

            // Fast 0-sqrt view-oriented shading + Floor/Wall/Ceiling classification from cached quantized normal
            constexpr float kSunX = 0.380f;
            constexpr float kSunY = 0.843f;
            constexpr float kSunZ = 0.380f;
            const float camX = snap.camPos[0], camY = snap.camPos[1], camZ = snap.camPos[2];
            const float invDepthBucket = 255.0f / ((sbcRadiusWorldUnits + 3600.0f > 500.0f) ? (sbcRadiusWorldUnits + 3600.0f) : 500.0f);
            const float vpW0 = proj.vp[3], vpW1 = proj.vp[7], vpW2 = proj.vp[11], vpW3 = proj.vp[15];

            auto evalShadeAndKind = [&](const SbcTriRecord& tr, std::uint8_t& outKind) noexcept -> float {
                float nx = static_cast<float>(tr.nx) * (1.0f / 127.0f);
                float ny = static_cast<float>(tr.ny) * (1.0f / 127.0f);
                float nz = static_cast<float>(tr.nz) * (1.0f / 127.0f);
                const float vdotn = (camX - tr.x0) * nx + (camY - tr.y0) * ny + (camZ - tr.z0) * nz;
                if (vdotn < 0.0f) {
                    nx = -nx; ny = -ny; nz = -nz;
                }
                if (ny >= 0.68f) {
                    outKind = (tr.elevBand != 0U) ? 3U : 0U;
                } else if (ny <= -0.68f) {
                    outKind = 2U;
                } else {
                    outKind = 1U;
                }
                const float sun = 0.5f + 0.5f * (nx * kSunX + ny * kSunY + nz * kSunZ);
                return 0.42f + 0.64f * sun;
            };

            if (isOpaqueSbc && snap.sbcTriCount > 0) {
                proj.zBiasCm = 0.0f;
                // Pre-encode 32 HDR/SDR-accurate solid opaque shade steps per surface type (alpha = 1.0f)
                Color4 opaqueLut[4][32]{};
                Color4 auxOpaqueLut[4][32]{};
                static constexpr float kOpaqueLowRgb[4][3] = {
                    {0.15f, 0.36f, 0.50f}, // 0: Walkable Floor (Architectural Slate-Blue)
                    {0.72f, 0.43f, 0.18f}, // 1: Wall / Cliff / Pillar (Warm Sandstone-Amber)
                    {0.18f, 0.17f, 0.28f}, // 2: Ceiling / Overhang (Deep Muted Indigo)
                    {0.22f, 0.50f, 0.68f}  // 3: Slope 2m Elevation Contour Band
                };
                static constexpr float kOpaqueHighRgb[4][3] = {
                    {0.24f, 0.32f, 0.56f}, // 0: High-Poly Floor (Amethyst-Indigo)
                    {0.64f, 0.34f, 0.44f}, // 1: High-Poly Wall/Rock (Rose-Copper)
                    {0.22f, 0.18f, 0.32f}, // 2: High-Poly Ceiling (Deep Violet)
                    {0.34f, 0.44f, 0.72f}  // 3: High-Poly Slope Contour Band
                };
                const auto& kOpaqueRgb = (viewState.sbcTier == app::SbcMeshTier::HighOnly)
                                             ? kOpaqueHighRgb : kOpaqueLowRgb;
                static constexpr float kFogRgb[3] = {0.06f, 0.08f, 0.11f};
                auto fillOpaqueLut = [&](Color4 dst[4][32], const float rgb[4][3], float lumScale) {
                    for (int k = 0; k < 4; ++k) {
                        for (int s = 0; s < 32; ++s) {
                            const float sh = (0.08f + (1.04f / 31.0f) * static_cast<float>(s)) * lumScale;
                            const float fogT = (sh < 0.34f) ? ((0.34f - sh) * (1.0f / 0.26f)) : 0.0f;
                            auto cl = [](float v) { return (v > 1.0f) ? 1.0f : ((v < 0.0f) ? 0.0f : v); };
                            const float rLit = rgb[k][0] * sh;
                            const float gLit = rgb[k][1] * sh;
                            const float bLit = rgb[k][2] * sh;
                            dst[k][s] = csEnc.encode({
                                cl(rLit * (1.0f - fogT * 0.55f) + kFogRgb[0] * (fogT * 0.55f)),
                                cl(gLit * (1.0f - fogT * 0.55f) + kFogRgb[1] * (fogT * 0.55f)),
                                cl(bLit * (1.0f - fogT * 0.55f) + kFogRgb[2] * (fogT * 0.55f)),
                                1.0f
                            });
                        }
                    }
                };
                fillOpaqueLut(opaqueLut, kOpaqueRgb, 1.0f);
                if (auxOpaqueSolid) {
                    fillOpaqueLut(auxOpaqueLut, kAuxFaceRgb, 0.85f);
                }

                // O(N) 256-bucket Near-to-Far counting sort using 0-sqrt clip-W depth (skip behind-camera tris!)
                std::uint32_t bCountsP1[256]{};
                std::uint32_t bCountsAux[256]{};
                std::uint32_t p1TriCount  = 0;
                std::uint32_t auxTriCount = 0;
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const SbcTriRecord& tr = g_sbcTris[i];
                    if (tr.layer != kSbcLayerSolid && !includeAuxSbc) {
                        g_sbcTriBucket[i] = 255U;
                        continue;
                    }
                    const float w0 = tr.x0 * vpW0 + tr.y0 * vpW1 + tr.z0 * vpW2 + vpW3;
                    const float w1 = tr.x1 * vpW0 + tr.y1 * vpW1 + tr.z1 * vpW2 + vpW3;
                    const float w2 = tr.x2 * vpW0 + tr.y2 * vpW1 + tr.z2 * vpW2 + vpW3;
                    if (w0 < 0.25f && w1 < 0.25f && w2 < 0.25f) {
                        g_sbcTriBucket[i] = 255U; // Completely behind camera -> skip sorting & projection!
                        continue;
                    }
                    const float wAvg = (w0 + w1 + w2) * (1.0f / 3.0f);
                    int dk = static_cast<int>(wAvg * invDepthBucket);
                    if (dk < 0) dk = 0;
                    else if (dk > 254) dk = 254;
                    g_sbcTriBucket[i] = static_cast<std::uint8_t>(dk);
                    if (tr.layer == kSbcLayerSolid || auxOpaqueSolid) {
                        ++bCountsP1[dk];
                        ++p1TriCount;
                    } else {
                        ++bCountsAux[dk];
                        ++auxTriCount;
                    }
                }

                std::uint32_t bOffP1[256]{};
                std::uint32_t runP1 = 0;
                for (int b = 0; b < 255; ++b) {
                    bOffP1[b] = runP1;
                    runP1 += bCountsP1[b];
                }
                std::uint32_t bOffAux[256]{};
                std::uint32_t runAux = 0;
                for (int b = 254; b >= 0; --b) {
                    bOffAux[b] = runAux;
                    runAux += bCountsAux[b];
                }
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const std::uint8_t b = g_sbcTriBucket[i];
                    if (b == 255U) continue;
                    const SbcTriRecord& tr = g_sbcTris[i];
                    if (tr.layer == kSbcLayerSolid || auxOpaqueSolid) {
                        g_sbcTriOrder[bOffP1[b]++] = i;
                    } else {
                        g_sbcAuxOrder[bOffAux[b]++] = i;
                    }
                }

                // Pass 1: Opaque terrain (Near->Far for Early-Z, Blend OFF + Reversed-Z Write)
                for (std::uint32_t oi = 0; oi < p1TriCount; ++oi) {
                    const SbcTriRecord& tr = g_sbcTris[g_sbcTriOrder[oi]];
                    std::uint8_t surfKind = 0U;
                    const float shade = evalShadeAndKind(tr, surfKind);
                    const float effSh = shade * (0.26f + 0.74f * tr.fade);
                    int si = static_cast<int>((effSh - 0.08f) * (31.0f / 1.04f) + 0.5f);
                    if (si < 0) si = 0;
                    else if (si > 31) si = 31;
                    const Color4& col = (tr.layer == kSbcLayerSolid)
                                            ? opaqueLut[surfKind & 3u][si]
                                            : auxOpaqueLut[(tr.layer - 1U) & 3U][si];
                    proj.addTri3D(outVerts,
                                  {tr.x0, tr.y0, tr.z0},
                                  {tr.x1, tr.y1, tr.z1},
                                  {tr.x2, tr.y2, tr.z2},
                                  col);
                }

                // Pass 1 ends here: [0 .. opaqueTriVerts) writes Reversed-Z D32_FLOAT with Blend OFF
                outStats.opaqueTriVerts = static_cast<std::uint32_t>(outVerts.size());

                // Pass 2a: Translucent special/auxiliary layers (Far->Near), depth-tested against Pass 1 solid terrain!
                if (auxTriCount > 0) {
                    Color4 auxFaceLut[4][16]{};
                    buildAuxFaceLut(auxFaceLut, true);
                    proj.zBiasCm = 2.0f;
                    for (std::uint32_t oi = 0; oi < auxTriCount; ++oi) {
                        const SbcTriRecord& tr = g_sbcTris[g_sbcAuxOrder[oi]];
                        std::uint8_t dummyKind = 0U;
                        const float shade = evalShadeAndKind(tr, dummyKind);
                        const std::uint32_t ak = (tr.layer - 1U) & 3U;
                        int si = static_cast<int>((shade - 0.34f) * (15.0f / 0.76f) + 0.5f);
                        if (si < 0) si = 0;
                        else if (si > 15) si = 15;
                        Color4 col = auxFaceLut[ak][si];
                        col.a *= (0.25f + 0.75f * tr.fade);
                        proj.addTri3D(outVerts,
                                      {tr.x0, tr.y0, tr.z0},
                                      {tr.x1, tr.y1, tr.z1},
                                      {tr.x2, tr.y2, tr.z2},
                                      col);
                    }
                }

                // Pass 2b: Depth-tested .sbc geometric creases (4 cm camera-ward bias so creases sit crisply on faces!)
                proj.zBiasCm = 4.0f;
                const float htSoft  = sbcHalfThick * 0.52f;
                const float htFloor = sbcHalfThick * 0.84f;
                const float htWall  = sbcHalfThick * 1.04f;
                const Color4 colSoftFloor = csEnc.encode({0.38f, 0.84f, 1.00f, clampA(0.45f)});
                const Color4 colSoftWall  = csEnc.encode({1.00f, 0.72f, 0.34f, clampA(0.50f)});
                const Color4 colSharpFlr  = csEnc.encode({0.52f, 0.95f, 1.00f, clampA(0.86f)});
                const Color4 colSharpWall = csEnc.encode({1.00f, 0.82f, 0.32f, clampA(0.94f)});

                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.count >= 2U && e.crease == 0U) {
                        continue; // Suppress flat coplanar tessellation seams!
                    }
                    const bool sharp = (e.crease >= 2U || e.count == 1U);
                    if (e.layer == kSbcLayerSolid) {
                        Color4 c = sharp ? (e.isWall ? colSharpWall : colSharpFlr)
                                         : (e.isWall ? colSoftWall  : colSoftFloor);
                        c.a *= (0.25f + 0.75f * e.fade);
                        const float ht = sharp ? (e.isWall ? htWall : htFloor) : htSoft;
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            ht, c, 0.0f, 0.0f, false);
                    } else if (includeAuxSbc) {
                        const std::uint32_t ak = (e.layer - 1U) & 3U;
                        Color4 c = sharp ? auxEdgeSharp[ak] : auxEdgeSoft[ak];
                        c.a *= (0.22f + 0.78f * e.fade);
                        const float ht = sbcHalfThick * (sharp ? kAuxSharpHt[ak] : kAuxSoftHt[ak]);
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            ht, c, 0.0f, 0.0f, false);
                    }
                }
            } else if (use3DShade && snap.sbcTriCount > 0) {
                proj.zBiasCm = 0.0f;
                // Pre-encode 16 HDR/SDR-accurate shade steps per surface type (0 pow() per triangle!)
                const float surfAlphaScale = ((activeHideSt || activePure3D) ? 1.15f : 1.00f) * cfg.sbcSurfaceOpacity;
                Color4 shadeLut[4][16]{};
                static constexpr float kBaseLowRgb[4][3] = {
                    {0.15f, 0.44f, 0.66f}, // 0: Walkable Floor (Cool Slate-Blue)
                    {0.88f, 0.52f, 0.18f}, // 1: Wall / Cliff / Pillar (Warm Terracotta-Amber)
                    {0.24f, 0.22f, 0.40f}, // 2: Ceiling / Overhang (Muted Indigo)
                    {0.22f, 0.58f, 0.82f}  // 3: Slope 2m Elevation Contour Band
                };
                static constexpr float kBaseHighRgb[4][3] = {
                    {0.34f, 0.40f, 0.78f}, // 0: High-Poly Floor (Amethyst-Indigo)
                    {0.82f, 0.42f, 0.56f}, // 1: High-Poly Wall/Rock (Rose-Copper)
                    {0.30f, 0.24f, 0.46f}, // 2: High-Poly Ceiling (Deep Violet)
                    {0.44f, 0.54f, 0.88f}  // 3: High-Poly Slope Contour Band
                };
                const auto& kBaseRgb = (viewState.sbcTier == app::SbcMeshTier::HighOnly)
                                           ? kBaseHighRgb : kBaseLowRgb;
                static constexpr float kBaseA[4] = {0.10f, 0.15f, 0.04f, 0.13f};
                for (int k = 0; k < 4; ++k) {
                    const float ba = clampA(kBaseA[k] * surfAlphaScale);
                    for (int s = 0; s < 16; ++s) {
                        const float sh = 0.34f + (0.76f / 15.0f) * static_cast<float>(s);
                        auto cl = [](float v) { return (v > 1.0f) ? 1.0f : ((v < 0.0f) ? 0.0f : v); };
                        shadeLut[k][s] = csEnc.encode({
                            cl(kBaseRgb[k][0] * sh),
                            cl(kBaseRgb[k][1] * sh),
                            cl(kBaseRgb[k][2] * sh),
                            ba
                        });
                    }
                }
                Color4 auxFaceLut[4][16]{};
                buildAuxFaceLut(auxFaceLut, false);

                // O(N) 256-bucket Far-to-Near Painter's counting sort using 0-sqrt clip-W depth (skip behind-camera!)
                std::uint32_t bCounts[256]{};
                std::uint32_t visTriCount = 0;
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const SbcTriRecord& tr = g_sbcTris[i];
                    if (tr.layer != kSbcLayerSolid && !includeAuxSbc) {
                        g_sbcTriBucket[i] = 255U;
                        continue;
                    }
                    const float w0 = tr.x0 * vpW0 + tr.y0 * vpW1 + tr.z0 * vpW2 + vpW3;
                    const float w1 = tr.x1 * vpW0 + tr.y1 * vpW1 + tr.z1 * vpW2 + vpW3;
                    const float w2 = tr.x2 * vpW0 + tr.y2 * vpW1 + tr.z2 * vpW2 + vpW3;
                    if (w0 < 0.25f && w1 < 0.25f && w2 < 0.25f) {
                        g_sbcTriBucket[i] = 255U;
                        continue;
                    }
                    const float wAvg = (w0 + w1 + w2) * (1.0f / 3.0f);
                    int dk = static_cast<int>(wAvg * invDepthBucket);
                    if (dk < 0) dk = 0;
                    else if (dk > 254) dk = 254;
                    g_sbcTriBucket[i] = static_cast<std::uint8_t>(dk);
                    ++bCounts[dk];
                    ++visTriCount;
                }
                std::uint32_t bOffsets[256]{};
                std::uint32_t running = 0;
                for (int b = 254; b >= 0; --b) {
                    bOffsets[b] = running;
                    running += bCounts[b];
                }
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const std::uint8_t b = g_sbcTriBucket[i];
                    if (b == 255U) continue;
                    g_sbcTriOrder[bOffsets[b]++] = i;
                }

                for (std::uint32_t oi = 0; oi < visTriCount; ++oi) {
                    const SbcTriRecord& tr = g_sbcTris[g_sbcTriOrder[oi]];
                    std::uint8_t surfKind = 0U;
                    const float shade = evalShadeAndKind(tr, surfKind);
                    int si = static_cast<int>((shade - 0.34f) * (15.0f / 0.76f) + 0.5f);
                    if (si < 0) si = 0;
                    else if (si > 15) si = 15;
                    Color4 col = (tr.layer == kSbcLayerSolid)
                                     ? shadeLut[surfKind & 3u][si]
                                     : auxFaceLut[(tr.layer - 1U) & 3U][si];
                    col.a *= tr.fade;
                    proj.addTri3D(outVerts,
                                  {tr.x0, tr.y0, tr.z0},
                                  {tr.x1, tr.y1, tr.z1},
                                  {tr.x2, tr.y2, tr.z2},
                                  col);
                }

                // Draw ONLY real geometric creases & boundaries (skip flat coplanar internal grid lines!)
                proj.zBiasCm = 4.0f;
                const float htSoft  = sbcHalfThick * 0.46f;
                const float htFloor = sbcHalfThick * 0.78f;
                const float htWall  = sbcHalfThick * 0.96f;
                const Color4 colSoftFloor = csEnc.encode({0.30f, 0.78f, 0.96f, clampA(0.28f)});
                const Color4 colSoftWall  = csEnc.encode({0.95f, 0.66f, 0.28f, clampA(0.32f)});
                const Color4 colSharpFlr  = csEnc.encode({0.38f, 0.90f, 1.00f, clampA(0.68f)});
                const Color4 colSharpWall = csEnc.encode({1.00f, 0.74f, 0.24f, clampA(0.84f)});

                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.count >= 2U && e.crease == 0U) {
                        continue; // Flat coplanar interior tessellation seam -> suppress grid clutter!
                    }
                    const bool sharp = (e.crease >= 2U || e.count == 1U);
                    if (e.layer == kSbcLayerSolid) {
                        Color4 c = sharp ? (e.isWall ? colSharpWall : colSharpFlr)
                                         : (e.isWall ? colSoftWall  : colSoftFloor);
                        c.a *= e.fade;
                        const float ht = sharp ? (e.isWall ? htWall : htFloor) : htSoft;
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            ht, c, 0.0f, 0.0f, false);
                    } else if (includeAuxSbc) {
                        const std::uint32_t ak = (e.layer - 1U) & 3U;
                        Color4 c = sharp ? auxEdgeSharp[ak] : auxEdgeSoft[ak];
                        c.a *= e.fade;
                        const float ht = sbcHalfThick * (sharp ? kAuxSharpHt[ak] : kAuxSoftHt[ak]);
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            ht, c, 0.0f, 0.0f, false);
                    }
                }
            } else {
                // Classic raw full wireframe (with subtle muted styling for auxiliary/non-solid layers)
                proj.zBiasCm = 4.0f;
                const float htFloor = sbcHalfThick * 0.60f;
                const float htWall  = sbcHalfThick * 0.85f;
                const Color4 colFloor = csEnc.encode({0.20f, 0.66f, 0.82f, clampA(0.42f)});
                const Color4 colWall  = csEnc.encode({0.94f, 0.64f, 0.24f, clampA(0.68f)});

                if (includeAuxSbc) {
                    for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                        const SbcEdgeRecord& e = g_sbcEdges[i];
                        if (e.layer != kSbcLayerSolid) {
                            if (e.count >= 2U && e.crease == 0U) continue;
                            const std::uint32_t ak = (e.layer - 1U) & 3U;
                            Color4 c = auxEdgeSoft[ak];
                            c.a *= e.fade;
                            proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                                sbcHalfThick * kAuxSoftHt[ak], c, 0.0f, 0.0f, false);
                        }
                    }
                }
                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.layer == kSbcLayerSolid && e.isWall == 0) {
                        Color4 c = colFloor; c.a *= e.fade;
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            htFloor, c, 0.0f, 0.0f, false);
                    }
                }
                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.layer == kSbcLayerSolid && e.isWall != 0) {
                        Color4 c = colWall; c.a *= e.fade;
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            htWall, c, 0.0f, 0.0f, false);
                    }
                }
            }
        }

        // 2b. Concentric Metric Ground Range Rings (5m / 10m / 15m / 20m) + Airborne Drop-Pins
        if (showRings) {
            proj.zBiasCm = 4.0f;
            EmitGroundRadarAndDropPins(outVerts, proj, csEnc, snap, sbcHalfThick, px, alphaMul);
        }

        // End of Pass 2: [opaqueTriVerts .. sceneDepthEndVert) is tested against MHW's 3D tDepthMap in ALL modes!
        outStats.sceneDepthEndVert = static_cast<std::uint32_t>(outVerts.size());

        // 2c. Draw Entity Collision Volumes (ordered back-to-front by VolumeCat, with 3D front/back depth cue!)
        proj.zBiasCm = (outStats.opaqueTriVerts > 0) ? 18.0f : 0.0f;
        if (snap.shapeCount > 0) {
            const float htMonsterAtk  = entityHalfThick * 1.45f;
            const float htPlayerAtk   = entityHalfThick * 1.35f;
            const float htOtomoAtk    = entityHalfThick * 1.15f;
            const float htEnvAtk      = entityHalfThick * 1.30f;
            const float htMonsterHurt = entityHalfThick * 0.90f;
            const float htOtomoHurt   = entityHalfThick * 0.85f;
            const float htEnvHurt     = entityHalfThick * 0.95f;
            const float htPushbox     = entityHalfThick * 0.75f;
            const float htEnvPush     = entityHalfThick * 0.70f;
            const float htEmGhost     = entityHalfThick * 0.48f;

            const Color4 colMonsterAtk  = csEnc.encode({1.00f, 0.16f, 0.18f, clampA(0.95f)});
            const Color4 colPlayerAtk   = csEnc.encode({1.00f, 0.86f, 0.10f, clampA(0.95f)});
            const Color4 colOtomoAtk    = csEnc.encode({1.00f, 0.72f, 0.22f, clampA(0.90f)});
            const Color4 colEnvAtk      = csEnc.encode({1.00f, 0.48f, 0.12f, clampA(0.94f)});
            const Color4 colMonsterHurt = csEnc.encode({0.15f, 0.95f, 0.38f, clampA(0.72f)});
            const Color4 colOtomoHurt   = csEnc.encode({0.38f, 0.78f, 1.00f, clampA(0.75f)});
            const Color4 colEnvHurt     = csEnc.encode({0.22f, 0.94f, 0.76f, clampA(0.84f)});
            const Color4 colPushbox     = csEnc.encode({0.72f, 0.56f, 1.00f, clampA(0.42f)});
            const Color4 colEnvPush     = csEnc.encode({0.46f, 0.62f, 0.84f, clampA(0.45f)});
            const Color4 colEmGhostHurt = csEnc.encode({0.58f, 0.68f, 0.78f, clampA(0.14f)});
            const Color4 colEmGhostPush = csEnc.encode({0.62f, 0.58f, 0.78f, clampA(0.10f)});

            Color4 colEmPartHurt[18]{};
            Color4 colEmPartPush[18]{};
            if (viewState.emPartMode != app::EmPartMode::Off) {
                for (int c = 0; c < 18; ++c) {
                    const float r = kEmPartPaletteSrgb[c][0];
                    const float g = kEmPartPaletteSrgb[c][1];
                    const float b = kEmPartPaletteSrgb[c][2];
                    const float hurtA = (c == static_cast<int>(kEmNoClawColorIdx)) ? 0.54f : 0.84f;
                    const float pushA = (c == static_cast<int>(kEmNoClawColorIdx)) ? 0.30f : 0.46f;
                    colEmPartHurt[c] = csEnc.encode({r, g, b, clampA(hurtA)});
                    // Soft pastel tint of the same part hue for bound Pushboxes so Hurtbox vs Pushbox stay distinct
                    colEmPartPush[c] = csEnc.encode({
                        0.65f * r + 0.35f * 0.84f,
                        0.65f * g + 0.35f * 0.78f,
                        0.65f * b + 0.35f * 1.00f,
                        clampA(pushA)
                    });
                }
            }

            Color4 colPlDef[7]{};
            float  htPlDef[7]{};
            for (int d = 0; d < 7; ++d) {
                const float a = (d == 0) ? clampA(0.82f) : clampA(0.96f);
                colPlDef[d] = csEnc.encode(PlayerDefSrgbColor(static_cast<PlayerDefState>(d), a, entityHalfThick, htPlDef[d]));
            }

            for (std::uint32_t i = 0; i < snap.shapeCount; ++i) {
                if ((g_shapes[i].emFlags & kEmFlagGhost) != 0U) continue;
                switch (g_shapes[i].cat) {
                    case VolumeCat::MonsterHitbox:  ++outStats.monsterHitbox;  break;
                    case VolumeCat::PlayerHitbox:   ++outStats.playerHitboxes; break;
                    case VolumeCat::MonsterHurtbox: ++outStats.monsterHurtbox; break;
                    case VolumeCat::PlayerHurtbox:  ++outStats.playerHurtbox;  break;
                    case VolumeCat::EnvPush:
                    case VolumeCat::EnvHurtbox:
                    case VolumeCat::EnvHitbox:      ++outStats.envShapes;      break;
                    default:                        ++outStats.otherShapes;    break;
                }
            }

            for (int pass = 0; pass < static_cast<int>(VolumeCat::Count); ++pass) {
                const auto wantCat = static_cast<VolumeCat>(pass);

                // Sub-pass 0 draws faint ghost outlines first; sub-pass 1 draws selected active parts on top!
                for (int ghostSub = 0; ghostSub < 2; ++ghostSub) {
                    const bool wantGhost = (ghostSub == 0);

                    for (std::uint32_t i = 0; i < snap.shapeCount; ++i) {
                        const ShapeRecord& s = g_shapes[i];
                        if (s.cat != wantCat) continue;
                        const bool isGhost = ((s.emFlags & kEmFlagGhost) != 0U);
                        if (isGhost != wantGhost) continue;

                        Color4 col{};
                        float halfThick = entityHalfThick;
                        float fillRatio = 0.0f;
                        switch (wantCat) {
                            case VolumeCat::MonsterHitbox:
                                col = colMonsterAtk; halfThick = htMonsterAtk; fillRatio = 0.13f; break;
                            case VolumeCat::PlayerHitbox:
                                col = colPlayerAtk; halfThick = htPlayerAtk; fillRatio = 0.11f; break;
                            case VolumeCat::OtomoHitbox:
                                col = colOtomoAtk; halfThick = htOtomoAtk; fillRatio = 0.09f; break;
                            case VolumeCat::EnvHitbox:
                                col = colEnvAtk; halfThick = htEnvAtk; fillRatio = 0.12f; break;
                            case VolumeCat::MonsterHurtbox:
                                if (isGhost) {
                                    col = colEmGhostHurt;
                                    halfThick = htEmGhost;
                                    fillRatio = 0.0f;
                                } else if (viewState.emPartMode != app::EmPartMode::Off && s.emPartIdx != kEmPartNone) {
                                    const std::uint8_t ci = (s.emPartIdx < 18U) ? s.emPartIdx : 0U;
                                    col = colEmPartHurt[ci];
                                    halfThick = (ci == kEmNoClawColorIdx)
                                                    ? (entityHalfThick * 0.78f)
                                                    : (entityHalfThick * 0.98f);
                                    fillRatio = (activeHideEm || viewState.backdrop == app::BackdropMode::HideAll3D)
                                                    ? 0.08f : 0.0f;
                                } else {
                                    col = colMonsterHurt;
                                    halfThick = htMonsterHurt;
                                    fillRatio = (activeHideEm || viewState.backdrop == app::BackdropMode::HideAll3D)
                                                    ? 0.07f : 0.0f;
                                }
                                break;
                            case VolumeCat::OtomoHurtbox:
                                col = colOtomoHurt; halfThick = htOtomoHurt; break;
                            case VolumeCat::EnvHurtbox:
                                col = colEnvHurt; halfThick = htEnvHurt;
                                fillRatio = (viewState.stageHide == app::StageHideMode::HideAll ||
                                             viewState.backdrop == app::BackdropMode::HideAll3D)
                                                ? 0.09f : 0.0f;
                                break;
                            case VolumeCat::PlayerHurtbox: {
                                const int di = (static_cast<int>(s.plDef) >= 0 && static_cast<int>(s.plDef) < 7)
                                                   ? static_cast<int>(s.plDef) : 0;
                                col = colPlDef[di];
                                halfThick = htPlDef[di];
                                fillRatio = (di != 0) ? 0.11f : 0.0f;
                                break;
                            }
                            case VolumeCat::EnvPush:
                                col = colEnvPush; halfThick = htEnvPush; break;
                            default:
                                if (isGhost) {
                                    col = colEmGhostPush;
                                    halfThick = htEmGhost * 0.90f;
                                } else if (viewState.emPartMode != app::EmPartMode::Off && s.emPartIdx != kEmPartNone) {
                                    const std::uint8_t ci = (s.emPartIdx < 18U) ? s.emPartIdx : 0U;
                                    col = colEmPartPush[ci];
                                    halfThick = htPushbox;
                                } else {
                                    col = colPushbox;
                                    halfThick = htPushbox;
                                }
                                break;
                        }

                        if (s.type == mhw::kGeomSphere) {
                            EmitSphere(outVerts, proj, {s.p[0], s.p[1], s.p[2]}, s.p[3],
                                       halfThick, col, use3DShade, fillRatio);
                        } else if (s.type == mhw::kGeomCapsule || s.type == mhw::kGeomLineSweptSphere) {
                            EmitCapsuleOrCylinder(outVerts, proj,
                                                  {s.p[0], s.p[1], s.p[2]},
                                                  {s.p[3], s.p[4], s.p[5]},
                                                  s.p[6], true, halfThick, col, use3DShade, fillRatio);
                        } else if (s.type == mhw::kGeomCylinder) {
                            EmitCapsuleOrCylinder(outVerts, proj,
                                                  {s.p[0], s.p[1], s.p[2]},
                                                  {s.p[3], s.p[4], s.p[5]},
                                                  s.p[6], false, halfThick, col, use3DShade, fillRatio);
                        } else if (s.type == mhw::kGeomOBB) {
                            const Vec3 ax{s.p[0] * s.p[16], s.p[1] * s.p[16], s.p[2] * s.p[16]};
                            const Vec3 ay{s.p[4] * s.p[17], s.p[5] * s.p[17], s.p[6] * s.p[17]};
                            const Vec3 az{s.p[8] * s.p[18], s.p[9] * s.p[18], s.p[10] * s.p[18]};
                            const Vec3 ct{s.p[12], s.p[13], s.p[14]};
                            Vec3 corners[8];
                            for (int k = 0; k < 8; ++k) {
                                const float sx = (k & 1) ? 1.0f : -1.0f;
                                const float sy = (k & 2) ? 1.0f : -1.0f;
                                const float sz = (k & 4) ? 1.0f : -1.0f;
                                corners[k] = {
                                    ct.x + sx * ax.x + sy * ay.x + sz * az.x,
                                    ct.y + sx * ax.y + sy * ay.y + sz * az.y,
                                    ct.z + sx * ax.z + sy * ay.z + sz * az.z
                                };
                            }
                            EmitBoxCorners(outVerts, proj, corners, halfThick, col, use3DShade, fillRatio);
                        } else if (s.type == mhw::kGeomAABB) {
                            const float x0 = s.p[0], y0 = s.p[1], z0 = s.p[2];
                            const float x1 = s.p[3], y1 = s.p[4], z1 = s.p[5];
                            const Vec3 corners[8] = {
                                {x0,y0,z0}, {x1,y0,z0}, {x0,y1,z0}, {x1,y1,z0},
                                {x0,y0,z1}, {x1,y0,z1}, {x0,y1,z1}, {x1,y1,z1}
                            };
                            EmitBoxCorners(outVerts, proj, corners, halfThick, col, use3DShade, fillRatio);
                        } else if (s.type == mhw::kGeomTriangle) {
                            const Vec3 v0{s.p[0], s.p[1], s.p[2]};
                            const Vec3 v1{s.p[3], s.p[4], s.p[5]};
                            const Vec3 v2{s.p[6], s.p[7], s.p[8]};
                            if (fillRatio > 0.001f && use3DShade) {
                                Color4 fc = col; fc.a *= fillRatio;
                                proj.addTri3D(outVerts, v0, v1, v2, fc);
                            }
                            proj.addDepthLine3D(outVerts, v0, v1, halfThick, col, 0.0f, 0.0f, false);
                            proj.addDepthLine3D(outVerts, v1, v2, halfThick, col, 0.0f, 0.0f, false);
                            proj.addDepthLine3D(outVerts, v2, v0, halfThick, col, 0.0f, 0.0f, false);
                        } else if (s.type == mhw::kGeomLineSegment) {
                            proj.addDepthLine3D(outVerts,
                                                {s.p[0], s.p[1], s.p[2]},
                                                {s.p[3], s.p[4], s.p[5]},
                                                halfThick, col, 0.0f, 0.0f, false);
                        }
                    }
                }
            }
        }

        // End of Pass 3a: [sceneDepthEndVert .. worldOverlayEndVert) 3D entity wireframes
        outStats.worldOverlayEndVert = static_cast<std::uint32_t>(outVerts.size());
        proj.zBiasCm = 0.0f;
    }

    // 2d. Pass 3b: Floating 3D Monster Part Callouts (drawn with Depth OFF so Opaque .SBC terrain
    //     never clips z=0 badges, yet STILL before Pass 4 2D Player UI Restore so HUD/Panel stay on top!)
    if (visEnabled && snap.hasCamera &&
        viewState.emPartMode != app::EmPartMode::Off &&
        viewState.emLabelMode != app::EmPartLabelMode::Off) {
        EmitMonsterPart3DLabels(outVerts, proj, csEnc, viewState, px, resScale, alphaMul);
    }
    outStats.preUiLabelEndVert = static_cast<std::uint32_t>(outVerts.size());

    // 3. 2D Top-Left Status HUD Banner + Prominent Freeze Alert + Interactive [P] Filter & Scene Panel
    const float x0 = 18.0f * resScale;
    const float y0 = 18.0f * resScale;
    float panelTopY     = y0;
    float leftBarRight  = x0;
    float leftBarBottom = y0;

    if (cfg.showHudBanner && (visEnabled || anyFreeze || viewState.panelOpen || anySceneMod || showStartupToast)) {
        char linePrefix[260]{};
        char freezeSeg[40]{};
        char tailSeg[16]{};
        char stateSeg[48]{};

        if (!visEnabled && !anyFreeze && !viewState.panelOpen && !anySceneMod && showStartupToast) {
            std::snprintf(linePrefix, sizeof(linePrefix),
                          "MHW HFI READY  |  [U] VIS  [I] FREEZE  [K] WORLD  [J] KEEP  [O] +1F  [P] PANEL");
        } else {
            char mapSeg[48]{};
            if ((viewState.visMask & (app::kCatEnvObject | app::kCatStageMesh)) != 0) {
                std::snprintf(mapSeg, sizeof(mapSeg), " MAP:%u SBC:%u",
                              outStats.envShapes, outStats.sbcEdges);
            }
            char partSeg[48]{};
            if (viewState.emPartMode != app::EmPartMode::Off) {
                int sIdx = -1;
                const std::uint32_t onN = CountSelectedEmParts(viewState.emPartMask, g_emInspector.partCount, sIdx);
                const char* mTag = (viewState.emPartMode == app::EmPartMode::Hitzone) ? "HZ" : "CLAW";
                if (g_emInspector.partCount > 0) {
                    if (sIdx >= 0 && static_cast<std::uint32_t>(sIdx) < g_emInspector.partCount) {
                        std::snprintf(partSeg, sizeof(partSeg), " PARTS:%s(%s)",
                                      mTag, g_emInspector.parts[sIdx].shortTag);
                    } else {
                        std::snprintf(partSeg, sizeof(partSeg), " PARTS:%s(%u/%u)",
                                      mTag, onN, g_emInspector.partCount);
                    }
                } else {
                    std::snprintf(partSeg, sizeof(partSeg), " PARTS:%s", mTag);
                }
            }
            char sceneSeg[48]{};
            if (activePure3D && viewState.backdrop == app::BackdropMode::HideAll3D) {
                std::snprintf(sceneSeg, sizeof(sceneSeg), "  |  HIDE:ALL3D");
            } else if (activeHideEm && activeHideSt) {
                std::snprintf(sceneSeg, sizeof(sceneSeg), "  |  HIDE:EM+%s",
                              (viewState.stageHide == app::StageHideMode::KeepInteract) ? "BG" : "ALL");
            } else if (activeHideEm) {
                std::snprintf(sceneSeg, sizeof(sceneSeg), "  |  HIDE:EM");
            } else if (activeHideSt) {
                std::snprintf(sceneSeg, sizeof(sceneSeg), "  |  HIDE:%s",
                              (viewState.stageHide == app::StageHideMode::KeepInteract) ? "BG" : "MAP");
            } else if (activePure3D && viewState.backdrop == app::BackdropMode::HidePlVfx) {
                std::snprintf(sceneSeg, sizeof(sceneSeg), "  |  HIDE:PL");
            }

            std::snprintf(linePrefix, sizeof(linePrefix),
                          "VIS:%s [%s]  PL:%u/%u EM:%u/%u%s%s%s  |  ",
                          visEnabled ? "ON" : "OFF",
                          app::MatchPresetName(viewState.visMask),
                          outStats.playerHurtbox, outStats.playerHitboxes,
                          outStats.monsterHurtbox, outStats.monsterHitbox,
                          partSeg,
                          mapSeg,
                          sceneSeg);

            if (freezeMode == freeze::Mode::Full) {
                std::snprintf(freezeSeg, sizeof(freezeSeg), "FREEZE:ALL");
            } else if (freezeMode == freeze::Mode::World) {
                std::snprintf(freezeSeg, sizeof(freezeSeg), "FREEZE:WORLD");
            } else {
                std::snprintf(freezeSeg, sizeof(freezeSeg), "FREEZE:OFF");
            }
            std::snprintf(tailSeg, sizeof(tailSeg), "  [P]");

            if (snap.mainPlDef.valid) {
                std::snprintf(stateSeg, sizeof(stateSeg), "  |  FRAME:%s", snap.mainPlDef.badge);
            }
        }

        const std::size_t lenPre  = std::strlen(linePrefix);
        const std::size_t lenFrz  = std::strlen(freezeSeg);
        const std::size_t lenTail = std::strlen(tailSeg);
        const std::size_t lenSt   = std::strlen(stateSeg);
        const float textW1 = static_cast<float>(lenPre + lenFrz + lenTail + lenSt) * 6.0f * px;
        const bool showLegendBar = visEnabled && !viewState.panelOpen;
        const float minLegendW = showLegendBar ? (66.0f * 6.0f * 0.85f * px) : 0.0f;
        const float boxInnerW = (textW1 > minLegendW) ? textW1 : minLegendW;

        const float padX = 8.0f * resScale;
        const float padY = 6.0f * resScale;
        const float barH = 7.0f * px + padY * 2.0f + (showLegendBar ? (10.0f * px) : 0.0f);

        Color4 accentSrgb = {0.15f, 0.90f, 0.65f, 0.95f};
        if (freezeMode == freeze::Mode::Full)       accentSrgb = {1.00f, 0.28f, 0.20f, 0.98f};
        else if (freezeMode == freeze::Mode::World) accentSrgb = {1.00f, 0.72f, 0.12f, 0.98f};

        leftBarRight  = x0 + boxInnerW + padX * 2.0f;
        leftBarBottom = y0 + barH;

        proj.emitRect2D(outVerts, x0, y0, leftBarRight, leftBarBottom,
                        csEnc.encode({0.05f, 0.06f, 0.09f, 0.82f}));
        proj.emitRect2D(outVerts, x0, y0, x0 + 3.5f * resScale, leftBarBottom,
                        csEnc.encode(accentSrgb));

        const Color4 colTextMain = csEnc.encode({0.95f, 0.97f, 1.00f, 0.96f});
        float tx = x0 + padX;
        DrawText5x7(outVerts, proj, tx, y0 + padY, px, linePrefix, colTextMain);
        tx += static_cast<float>(lenPre) * 6.0f * px;

        if (lenFrz > 0) {
            Color4 frzTextCol = colTextMain;
            if (freezeMode == freeze::Mode::Full)       frzTextCol = csEnc.encode({1.00f, 0.36f, 0.28f, 1.00f});
            else if (freezeMode == freeze::Mode::World) frzTextCol = csEnc.encode({1.00f, 0.82f, 0.22f, 1.00f});
            DrawText5x7(outVerts, proj, tx, y0 + padY, px, freezeSeg, frzTextCol);
            tx += static_cast<float>(lenFrz) * 6.0f * px;
        }
        if (lenTail > 0) {
            DrawText5x7(outVerts, proj, tx, y0 + padY, px, tailSeg, colTextMain);
            tx += static_cast<float>(lenTail) * 6.0f * px;
        }
        if (lenSt > 0) {
            float dummyHt = 0.0f;
            Color4 badgeSrgb = PlayerDefSrgbColor(snap.mainPlDef.state, 0.98f, baseHalfThick, dummyHt);
            if (snap.mainPlDef.state == PlayerDefState::Normal) {
                badgeSrgb = {0.72f, 0.88f, 0.98f, 0.95f};
            }
            DrawText5x7(outVerts, proj, tx, y0 + padY, px, stateSeg, csEnc.encode(badgeSrgb));
        }

        if (showLegendBar) {
            const float ly = y0 + padY + 9.5f * px;
            float lx = x0 + padX;
            const Color4 colLbl = csEnc.encode({0.85f, 0.88f, 0.92f, 0.92f});
            auto drawChip = [&](const char* lbl, const Color4& srgbCol) {
                proj.emitRect2D(outVerts, lx, ly, lx + 5.0f * px, ly + 6.0f * px, csEnc.encode(srgbCol));
                lx += 6.5f * px;
                DrawText5x7(outVerts, proj, lx, ly - 0.5f * px, px * 0.85f, lbl, colLbl);
                lx += (static_cast<float>(std::strlen(lbl)) * 6.0f * 0.85f + 2.2f) * px;
            };
            drawChip("PL-HURT", {0.15f, 0.85f, 1.00f, 0.95f});
            drawChip("IFRAME",  {0.97f, 0.99f, 1.00f, 0.98f});
            drawChip("HYPER",   {1.00f, 0.46f, 0.08f, 0.95f});
            drawChip("PL-ATK",  {1.00f, 0.86f, 0.10f, 0.95f});
            drawChip("EM-HURT", {0.15f, 0.95f, 0.38f, 0.95f});
            drawChip("EM-ATK",  {1.00f, 0.16f, 0.18f, 0.95f});
            drawChip("ENV",     {0.22f, 0.94f, 0.76f, 0.95f});
            drawChip("SBC",     {0.94f, 0.64f, 0.24f, 0.95f});
        }

        panelTopY = leftBarBottom + 6.0f * resScale;

        // Prominent Top-Center Freeze Pill + 4 Viewfinder Corner Brackets (unmistakable even when VIS:OFF!)
        if (anyFreeze) {
            const bool isFull = (freezeMode == freeze::Mode::Full);
            const Color4 borderCol = csEnc.encode(isFull ? Color4{1.00f, 0.24f, 0.18f, 0.98f}
                                                         : Color4{1.00f, 0.72f, 0.12f, 0.98f});
            const Color4 bgCol     = csEnc.encode(isFull ? Color4{0.20f, 0.03f, 0.03f, 0.90f}
                                                         : Color4{0.20f, 0.12f, 0.02f, 0.90f});
            const Color4 titleCol  = csEnc.encode(isFull ? Color4{1.00f, 0.95f, 0.95f, 1.00f}
                                                         : Color4{1.00f, 0.93f, 0.58f, 1.00f});
            const Color4 subCol    = csEnc.encode({0.93f, 0.95f, 0.98f, 0.95f});
            const Color4 brkCol    = csEnc.encode(isFull ? Color4{1.00f, 0.26f, 0.20f, 0.85f}
                                                         : Color4{1.00f, 0.74f, 0.14f, 0.85f});

            // 4 sleek viewfinder corner brackets around the screen perimeter
            const float bm = 8.0f * resScale;
            const float bl = 32.0f * resScale;
            const float bt = 3.0f * resScale;
            // Top-Left
            proj.emitRect2D(outVerts, bm, bm, bm + bl, bm + bt, brkCol);
            proj.emitRect2D(outVerts, bm, bm + bt, bm + bt, bm + bl, brkCol);
            // Top-Right
            proj.emitRect2D(outVerts, screenW - bm - bl, bm, screenW - bm, bm + bt, brkCol);
            proj.emitRect2D(outVerts, screenW - bm - bt, bm + bt, screenW - bm, bm + bl, brkCol);
            // Bottom-Left
            proj.emitRect2D(outVerts, bm, screenH - bm - bt, bm + bl, screenH - bm, brkCol);
            proj.emitRect2D(outVerts, bm, screenH - bm - bl, bm + bt, screenH - bm - bt, brkCol);
            // Bottom-Right
            proj.emitRect2D(outVerts, screenW - bm - bl, screenH - bm - bt, screenW - bm, screenH - bm, brkCol);
            proj.emitRect2D(outVerts, screenW - bm - bt, screenH - bm - bl, screenW - bm, screenH - bm - bt, brkCol);

            // Top-Center bold Freeze Banner Pill
            const char* titleStr = isFull ? "FULL FREEZE" : "WORLD FREEZE";
            const char* subStr   = isFull
                                       ? "[I] RESUME   [O] +1F"
                                       : (freeze::IsKeepHitboxesActive()
                                              ? "[K] RESUME   [O] +1F   [J] KEEP:ON"
                                              : "[K] RESUME   [O] +1F   [J] KEEP:OFF");

            const float fpx      = std::floor(px * 1.45f + 0.5f);
            const float spx      = px;
            const float iconBarW = 2.0f * fpx;
            const float iconGap  = 1.5f * fpx;
            const float iconW    = iconBarW * 2.0f + iconGap;
            const float iconPad  = 3.5f * fpx;
            const float titleTxtW = static_cast<float>(std::strlen(titleStr)) * 6.0f * fpx - fpx;
            const float row1W    = iconW + iconPad + titleTxtW;
            const float row2W    = static_cast<float>(std::strlen(subStr)) * 6.0f * spx - spx;
            const float innerW   = (row1W > row2W) ? row1W : row2W;

            const float pPadX    = 18.0f * resScale;
            const float pPadY    = 7.0f * resScale;
            const float rowGap   = 5.0f * resScale;
            const float pillW    = innerW + pPadX * 2.0f;
            const float pillH    = 7.0f * fpx + rowGap + 7.0f * spx + pPadY * 2.0f;
            const float pillX0   = std::floor((screenW - pillW) * 0.5f);
            const float pillY0   = (leftBarRight + 14.0f * resScale > pillX0)
                                       ? (leftBarBottom + 8.0f * resScale)
                                       : y0;
            const float pillX1   = pillX0 + pillW;
            const float pillY1   = pillY0 + pillH;
            const float bTh      = 2.5f * resScale;
            const float sideBarW = 5.0f * resScale;

            // Tinted dark body + solid left/right side accent tabs + full 2.5px vivid border
            proj.emitRect2D(outVerts, pillX0, pillY0, pillX1, pillY1, bgCol);
            proj.emitRect2D(outVerts, pillX0, pillY0, pillX0 + sideBarW, pillY1, borderCol);
            proj.emitRect2D(outVerts, pillX1 - sideBarW, pillY0, pillX1, pillY1, borderCol);
            proj.emitRect2D(outVerts, pillX0 - bTh, pillY0 - bTh, pillX1 + bTh, pillY0, borderCol);
            proj.emitRect2D(outVerts, pillX0 - bTh, pillY1, pillX1 + bTh, pillY1 + bTh, borderCol);
            proj.emitRect2D(outVerts, pillX0 - bTh, pillY0, pillX0, pillY1, borderCol);
            proj.emitRect2D(outVerts, pillX1, pillY0, pillX1 + bTh, pillY1, borderCol);

            // Row 1: Crisp geometric pause icon || + bold title
            const float row1X = pillX0 + std::floor((pillW - row1W) * 0.5f);
            const float row1Y = pillY0 + pPadY;
            proj.emitRect2D(outVerts, row1X, row1Y, row1X + iconBarW, row1Y + 7.0f * fpx, borderCol);
            proj.emitRect2D(outVerts, row1X + iconBarW + iconGap, row1Y, row1X + iconW, row1Y + 7.0f * fpx, borderCol);
            DrawText5x7(outVerts, proj, row1X + iconW + iconPad, row1Y, fpx, titleStr, titleCol);

            // Row 2: Action hotkey reminder
            const float row2X = pillX0 + std::floor((pillW - row2W) * 0.5f);
            const float row2Y = row1Y + 7.0f * fpx + rowGap;
            DrawText5x7(outVerts, proj, row2X, row2Y, spx, subStr, subCol);
        }
    }

    if (viewState.panelOpen) {
        DrawInteractiveFilterPanel(outVerts, proj, csEnc, viewState, x0, panelTopY, px, resScale);
    }
}

} // namespace coll

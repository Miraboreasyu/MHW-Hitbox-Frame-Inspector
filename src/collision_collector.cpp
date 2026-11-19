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

struct ShapeRecord {
    VolumeCat cat;
    PlayerDefState plDef;
    std::int32_t type;
    float p[20];
};

struct SbcTriRecord {
    float x0, y0, z0;
    float x1, y1, z1;
    float x2, y2, z2;
    float shade;           // Directional sun + camera headlight [0.30..1.10]
    float fade;            // Smooth radial + vertical distance fade [0.0..1.0]
    std::uint16_t distKey; // Quantized camera distance [0..255] for O(N) far-to-near Painter sort
    std::uint8_t surfKind; // 0 = Walkable Floor, 1 = Wall/Cliff, 2 = Ceiling, 3 = Floor 2m Elev Band
};

struct SbcEdgeRecord {
    float x0, y0, z0;
    float x1, y1, z1;
    float nx0, ny0, nz0; // First adjacent face unit normal (for crease-angle filtering)
    float fade;          // Max distance fade of adjacent faces [0.0..1.0]
    std::uint8_t isWall; // 0 = walkable floor/slope, 1 = wall/cliff/ledge
    std::uint8_t count;  // Number of triangles sharing this edge (1 = open boundary, 2+ = interior)
    std::uint8_t crease; // 0 = flat coplanar (<7 deg), 1 = soft slope (7..24 deg), 2 = sharp ledge/corner (>24 deg)
};

struct SbcEdgeSlot {
    std::uint64_t key;
    std::uint32_t edgeIdxPlus1;
    std::uint32_t gen;
};

struct SbcPartCand {
    const float*   activeMat;
    std::uintptr_t triNodesBase;
    std::uintptr_t vertArrBase;
    std::uint32_t  totalVerts;
    std::uint32_t  startTri;
    std::uint32_t  endTri;
    std::uint32_t  startVert;
    float          distSq;
};

constexpr std::uint32_t kMaxShapes       = 3072;
constexpr std::uint32_t kMaxSbcTris      = 65536;
constexpr std::uint32_t kMaxSbcEdges     = 65536;
constexpr std::uint32_t kEdgeHashSize    = 131072; // Power of 2
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
std::uint32_t g_sbcTriOrder[kMaxSbcTris];
SbcEdgeRecord g_sbcEdges[kMaxSbcEdges];
SbcEdgeSlot   g_sbcEdgeHash[kEdgeHashSize];
SbcPartCand   g_sbcPartCands[kMaxSbcPartCands];
std::uint32_t g_sbcEdgeGen = 0;

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
                      RawSnapshot& snap) {
    if (!IsAlignedPtr(geom)) return false;
    if (snap.shapeCount >= kMaxShapes) return false;
    if ((visMask & CategoryBitForVolume(cat)) == 0) return false;

    if (!g_seenGeoms.insert(geom)) return false;

    const std::int32_t type = *reinterpret_cast<const std::int32_t*>(geom + mhw::kOffGeomType);
    ShapeRecord& rec = g_shapes[snap.shapeCount];
    rec.cat   = cat;
    rec.plDef = plDef;
    rec.type  = type;

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
        if ((visMask & CategoryBitForVolume(cat)) == 0) continue;

        const PlayerDefState plDef = (cat == VolumeCat::PlayerHurtbox) ? unitPlDef : PlayerDefState::Normal;

        const std::uint32_t geomCount = *reinterpret_cast<const std::uint32_t*>(cnode + mhw::kOffCNodeGeomCount);
        const std::uintptr_t geomArr  = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeGeomArray);
        if (geomCount == 0 || geomCount > 128 || !IsAlignedPtr(geomArr)) continue;

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

inline std::uint64_t HashQuantizedEndpoint(const float v[3]) noexcept {
    const auto qx = static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(v[0] + 0.5f))));
    const auto qy = static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(v[1] + 0.5f))));
    const auto qz = static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(v[2] + 0.5f))));
    std::uint64_t h = (qx * 73856093ULL) ^ (qy * 19349669ULL) ^ (qz * 83492791ULL) ^ 0x9E3779B97F4A7C15ULL;
    h ^= (h >> 30);
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= (h >> 27);
    return (h == 0) ? 1ULL : h;
}

void InsertSbcEdgeDedup(const float a[3],
                        const float b[3],
                        float unx,
                        float uny,
                        float unz,
                        float fade,
                        std::uint8_t isWall,
                        std::uint32_t edgeGen,
                        RawSnapshot& snap) noexcept {
    const std::uint64_t ha = HashQuantizedEndpoint(a);
    const std::uint64_t hb = HashQuantizedEndpoint(b);
    if (ha == hb) return;
    const std::uint64_t lo = (ha < hb) ? ha : hb;
    const std::uint64_t hi = (ha < hb) ? hb : ha;
    std::uint64_t key = (lo * 0x9E3779B97F4A7C15ULL) ^ (hi + 0x94D049BB133111EBULL);
    if (key == 0) key = 1ULL;

    std::uint32_t slot = static_cast<std::uint32_t>(key) & (kEdgeHashSize - 1U);
    for (std::uint32_t probe = 0; probe < 24; ++probe) {
        SbcEdgeSlot& hs = g_sbcEdgeHash[slot];
        if (hs.gen == edgeGen && hs.key == key) {
            const std::uint32_t idx = hs.edgeIdxPlus1 - 1U;
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
        if (hs.gen != edgeGen) {
            if (snap.sbcEdgeCount >= kMaxSbcEdges) return;
            const std::uint32_t idx = snap.sbcEdgeCount++;
            hs.key          = key;
            hs.edgeIdxPlus1 = idx + 1U;
            hs.gen          = edgeGen;
            SbcEdgeRecord& rec = g_sbcEdges[idx];
            rec.x0     = a[0]; rec.y0  = a[1]; rec.z0  = a[2];
            rec.x1     = b[0]; rec.y1  = b[1]; rec.z1  = b[2];
            rec.nx0    = unx;  rec.ny0 = uny;  rec.nz0 = unz;
            rec.fade   = fade;
            rec.isWall = isWall;
            rec.count  = 1U;
            rec.crease = 0U;
            return;
        }
        slot = (slot + 1U) & (kEdgeHashSize - 1U);
    }
}

void HarvestStageSbcMeshes(std::uintptr_t sColl,
                           float radiusWorldUnits,
                           RawSnapshot& snap) {
    if (!IsAlignedPtr(sColl) || !snap.hasCamera) return;

    const float* camPos = snap.camPos;
    float center[3] = {camPos[0], camPos[1], camPos[2]};
    if (snap.hasPlayerGround) {
        const float dx = snap.plFeetPos[0] - camPos[0];
        const float dy = snap.plFeetPos[1] - camPos[1];
        const float dz = snap.plFeetPos[2] - camPos[2];
        // Follow player normally, but switch to camera automatically if FreeCam moved > 60m away!
        if (!IsValidVec3(camPos) || (dx * dx + dy * dy + dz * dz) <= (6000.0f * 6000.0f)) {
            center[0] = snap.plFeetPos[0];
            center[1] = snap.plFeetPos[1] + 80.0f; // Waist height
            center[2] = snap.plFeetPos[2];
        }
    }
    if (!IsValidVec3(center)) return;

    const float qMin[3] = {
        center[0] - radiusWorldUnits,
        center[1] - radiusWorldUnits,
        center[2] - radiusWorldUnits
    };
    const float qMax[3] = {
        center[0] + radiusWorldUnits,
        center[1] + radiusWorldUnits,
        center[2] + radiusWorldUnits
    };

    const float invRadSq     = 1.0f / (radiusWorldUnits * radiusWorldUnits + 1.0f);
    const float maxCamDist   = radiusWorldUnits + 2800.0f;
    const float invMaxCamDst = 255.0f / ((maxCamDist > 500.0f) ? maxCamDist : 500.0f);
    const float refFloorY    = snap.hasPlayerGround ? snap.plFeetPos[1] : center[1];

    // Normalized angled overhead sun for 3D facet relief: L = normalize(0.38, 0.84, 0.38)
    constexpr float kSunX = 0.380f;
    constexpr float kSunY = 0.843f;
    constexpr float kSunZ = 0.380f;

    if (++g_sbcEdgeGen == 0U) {
        std::memset(g_sbcEdgeHash, 0, sizeof(g_sbcEdgeHash));
        g_sbcEdgeGen = 1U;
    }
    const std::uint32_t edgeGen = g_sbcEdgeGen;

    const std::uint32_t sbcCount = *reinterpret_cast<const std::uint32_t*>(sColl + mhw::kOffSCollSbcCount);
    const std::uintptr_t sbcArr  = *reinterpret_cast<const std::uintptr_t*>(sColl + mhw::kOffSCollSbcArray);
    if (sbcCount == 0 || sbcCount > 512 || !IsAlignedPtr(sbcArr)) return;

    // 1. Gather overlapping .sbc stage parts first, then sort Near-to-Far around `center`
    //    so high-radius queries (45M / 80M) NEVER starve parts directly around the player!
    std::uint32_t candCount = 0;
    for (std::uint32_t i = 0; i < sbcCount && candCount < kMaxSbcPartCands; ++i) {
        const std::uintptr_t sbc = *reinterpret_cast<const std::uintptr_t*>(sbcArr + i * 8);
        if (!IsAlignedPtr(sbc)) continue;
        if (*reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcActive) == 0) continue;

        const std::uintptr_t rColl = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcResource);
        if (!IsAlignedPtr(rColl)) continue;

        const auto* magic = reinterpret_cast<const char*>(rColl + mhw::kOffRCollMagic);
        if (magic[0] != 'S' || magic[1] != 'B' || magic[2] != 'C') continue;

        const float* sbcMat = nullptr;
        if (*reinterpret_cast<const std::uint8_t*>(sbc + mhw::kOffSbcHasMatrix) != 0) {
            const std::uintptr_t mp = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcMatrixPtr);
            if (IsAlignedPtr(mp)) sbcMat = reinterpret_cast<const float*>(mp);
        }

        const std::uint16_t numParts      = *reinterpret_cast<const std::uint16_t*>(rColl + mhw::kOffRCollNumParts);
        const std::uint32_t totalTriNodes = *reinterpret_cast<const std::uint32_t*>(rColl + mhw::kOffRCollNumTriNodes);
        const std::uint32_t totalVerts    = *reinterpret_cast<const std::uint32_t*>(rColl + mhw::kOffRCollNumVerts);
        const std::uintptr_t partsInfoArr = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollPartsInfo);
        const std::uintptr_t triNodesBase = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollTriNodes);
        const std::uintptr_t vertArrBase  = *reinterpret_cast<const std::uintptr_t*>(rColl + mhw::kOffRCollVertices);

        if (numParts == 0 || numParts > 4096 ||
            totalTriNodes == 0 || totalTriNodes > 2000000U ||
            totalVerts == 0 || totalVerts > 2000000U ||
            !IsAlignedPtr(partsInfoArr) || !IsAlignedPtr(triNodesBase) || !IsAlignedPtr(vertArrBase)) {
            continue;
        }

        const std::uintptr_t sbcPartsArr = *reinterpret_cast<const std::uintptr_t*>(sbc + mhw::kOffSbcPartsArray);
        const std::uint32_t  sbcPartsCnt = *reinterpret_cast<const std::uint32_t*>(sbc + mhw::kOffSbcPartsCount);

        // If no per-part overrides can move parts independently, fast-cull whole .sbc AABB first
        if (sbcPartsCnt == 0) {
            const float* sbcMin = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMin);
            const float* sbcMax = reinterpret_cast<const float*>(rColl + mhw::kOffRCollAabbMax);
            float dummyDistSq = 0.0f;
            if (!AabbOverlapsQueryBox(sbcMin, sbcMax, sbcMat, center, qMin, qMax, dummyDistSq)) continue;
        }

        for (std::uint16_t p = 0; p < numParts && candCount < kMaxSbcPartCands; ++p) {
            const float* activeMat = sbcMat;
            if (p < sbcPartsCnt && IsAlignedPtr(sbcPartsArr)) {
                const std::uintptr_t sbcPart = sbcPartsArr + static_cast<std::size_t>(p) * mhw::kSbcPartStride;
                if (*reinterpret_cast<const std::uint8_t*>(sbcPart + mhw::kOffSbcPartEnabled) == 0) {
                    continue;
                }
                if (*reinterpret_cast<const std::uint8_t*>(sbcPart + mhw::kOffSbcPartHasMatrix) != 0) {
                    const std::uintptr_t pmp = *reinterpret_cast<const std::uintptr_t*>(sbcPart + mhw::kOffSbcPartMatrixPtr);
                    if (IsAlignedPtr(pmp)) activeMat = reinterpret_cast<const float*>(pmp);
                }
            }

            const std::uintptr_t pInfo = partsInfoArr + static_cast<std::size_t>(p) * mhw::kRCollPartInfoStride;
            const float* pMin = reinterpret_cast<const float*>(pInfo + mhw::kOffPartInfoAabbMin);
            const float* pMax = reinterpret_cast<const float*>(pInfo + mhw::kOffPartInfoAabbMax);
            float partDistSq = 0.0f;
            if (!AabbOverlapsQueryBox(pMin, pMax, activeMat, center, qMin, qMax, partDistSq)) continue;

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
            pc.totalVerts   = totalVerts;
            pc.startTri     = startTri;
            pc.endTri       = endTri;
            pc.startVert    = startVert;
            pc.distSq       = partDistSq;
        }
    }

    // In-place Shell sort Near-to-Far (SEH-safe POD sort, < 2 us for ~50-150 parts)
    for (std::uint32_t gap = candCount >> 1; gap > 0; gap >>= 1) {
        for (std::uint32_t i = gap; i < candCount; ++i) {
            const SbcPartCand tmp = g_sbcPartCands[i];
            std::uint32_t j = i;
            while (j >= gap && g_sbcPartCands[j - gap].distSq > tmp.distSq) {
                g_sbcPartCands[j] = g_sbcPartCands[j - gap];
                j -= gap;
            }
            g_sbcPartCands[j] = tmp;
        }
    }

    // 2. Harvest triangles & creases from nearest parts outward
    for (std::uint32_t ci = 0;
         ci < candCount && (snap.sbcTriCount < kMaxSbcTris || snap.sbcEdgeCount < kMaxSbcEdges);
         ++ci) {
        const SbcPartCand& pc         = g_sbcPartCands[ci];
        const float* activeMat        = pc.activeMat;
        const std::uintptr_t triNodesBase = pc.triNodesBase;
        const std::uintptr_t vertArrBase  = pc.vertArrBase;
        const std::uint32_t totalVerts    = pc.totalVerts;
        const std::uint32_t startVert     = pc.startVert;

        for (std::uint32_t t = pc.startTri;
             t < pc.endTri && (snap.sbcTriCount < kMaxSbcTris || snap.sbcEdgeCount < kMaxSbcEdges);
             ++t) {
            const std::uintptr_t tri = triNodesBase + static_cast<std::size_t>(t) * mhw::kRCollTriStride;
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

            // Triangle AABB vs query box (keeps large floor triangles directly underfoot!)
            const float minX = (w0[0] < w1[0]) ? ((w0[0] < w2[0]) ? w0[0] : w2[0]) : ((w1[0] < w2[0]) ? w1[0] : w2[0]);
            const float maxX = (w0[0] > w1[0]) ? ((w0[0] > w2[0]) ? w0[0] : w2[0]) : ((w1[0] > w2[0]) ? w1[0] : w2[0]);
            if (maxX < qMin[0] || minX > qMax[0]) continue;

            const float minY = (w0[1] < w1[1]) ? ((w0[1] < w2[1]) ? w0[1] : w2[1]) : ((w1[1] < w2[1]) ? w1[1] : w2[1]);
            const float maxY = (w0[1] > w1[1]) ? ((w0[1] > w2[1]) ? w0[1] : w2[1]) : ((w1[1] > w2[1]) ? w1[1] : w2[1]);
            if (maxY < qMin[1] || minY > qMax[1]) continue;

            const float minZ = (w0[2] < w1[2]) ? ((w0[2] < w2[2]) ? w0[2] : w2[2]) : ((w1[2] < w2[2]) ? w1[2] : w2[2]);
            const float maxZ = (w0[2] > w1[2]) ? ((w0[2] > w2[2]) ? w0[2] : w2[2]) : ((w1[2] > w2[2]) ? w1[2] : w2[2]);
            if (maxZ < qMin[2] || minZ > qMax[2]) continue;

            const float e1x = w1[0] - w0[0], e1y = w1[1] - w0[1], e1z = w1[2] - w0[2];
            const float e2x = w2[0] - w0[0], e2y = w2[1] - w0[1], e2z = w2[2] - w0[2];
            const float rnx = e1y * e2z - e1z * e2y;
            const float rny = e1z * e2x - e1x * e2z;
            const float rnz = e1x * e2y - e1y * e2x;
            const float lenSq = rnx * rnx + rny * rny + rnz * rnz;
            if (lenSq < 1.0f) continue;

            const float invLen = 1.0f / std::sqrt(lenSq);
            float unx = rnx * invLen;
            float uny = rny * invLen;
            float unz = rnz * invLen;

            // Centroid & closest-point radial distance fade (so large floor tris underfoot stay 100% opaque!)
            const float cx = (w0[0] + w1[0] + w2[0]) * (1.0f / 3.0f);
            const float cy = (w0[1] + w1[1] + w2[1]) * (1.0f / 3.0f);
            const float cz = (w0[2] + w1[2] + w2[2]) * (1.0f / 3.0f);

            const float nearX = (center[0] < minX) ? minX : ((center[0] > maxX) ? maxX : center[0]);
            const float nearY = (center[1] < minY) ? minY : ((center[1] > maxY) ? maxY : center[1]);
            const float nearZ = (center[2] < minZ) ? minZ : ((center[2] > maxZ) ? maxZ : center[2]);
            const float rdx = nearX - center[0], rdy = nearY - center[1], rdz = nearZ - center[2];
            const float normDistSq = (rdx * rdx + rdy * rdy + rdz * rdz) * invRadSq;
            if (normDistSq >= 1.0f) continue;

            float fade = 1.0f - normDistSq;
            fade = fade * std::sqrt(fade); // Smooth (1 - r^2)^1.5 falloff at outer boundary

            // Gently dim multi-story caverns far above/below the player's current floor (> 7.5m vertical delta)
            const float vDelta = std::fabs(cy - refFloorY);
            if (vDelta > 750.0f) {
                const float vMul = 1.0f - (vDelta - 750.0f) * (1.0f / 1800.0f);
                fade *= (vMul < 0.22f) ? 0.22f : vMul;
            }

            // Orient normal toward camera V so floor/wall/ceiling & directional sun work regardless of winding!
            const float vx = camPos[0] - cx;
            const float vy = camPos[1] - cy;
            const float vz = camPos[2] - cz;
            const float camDist = std::sqrt(vx * vx + vy * vy + vz * vz);
            float ndotv = 0.7f;
            if (camDist > 1.0f) {
                const float iv = 1.0f / camDist;
                const float vdotn = (vx * unx + vy * uny + vz * unz) * iv;
                if (vdotn < 0.0f) {
                    unx = -unx; uny = -uny; unz = -unz;
                    ndotv = -vdotn;
                } else {
                    ndotv = vdotn;
                }
            }

            const std::uint8_t isWall = (std::fabs(uny) < 0.68f) ? 1U : 0U;

            if (snap.sbcTriCount < kMaxSbcTris) {
                const float sun = 0.5f + 0.5f * (unx * kSunX + uny * kSunY + unz * kSunZ);
                const float shade = 0.34f + 0.46f * sun + 0.28f * ndotv;

                std::uint8_t surfKind = 1U; // Wall / Cliff
                if (uny >= 0.68f) {
                    // Subtle 2-meter topographic elevation band on slopes
                    const float band = std::fmod(std::fabs(cy), 200.0f);
                    surfKind = (uny < 0.985f && band < 16.0f) ? 3U : 0U;
                } else if (uny <= -0.68f) {
                    surfKind = 2U; // Ceiling / Overhang
                }

                int dk = static_cast<int>(camDist * invMaxCamDst);
                if (dk < 0) dk = 0;
                else if (dk > 255) dk = 255;

                SbcTriRecord& tr = g_sbcTris[snap.sbcTriCount++];
                tr.x0 = w0[0]; tr.y0 = w0[1]; tr.z0 = w0[2];
                tr.x1 = w1[0]; tr.y1 = w1[1]; tr.z1 = w1[2];
                tr.x2 = w2[0]; tr.y2 = w2[1]; tr.z2 = w2[2];
                tr.shade    = shade;
                tr.fade     = fade;
                tr.distKey  = static_cast<std::uint16_t>(dk);
                tr.surfKind = surfKind;
            }

            InsertSbcEdgeDedup(w0, w1, unx, uny, unz, fade, isWall, edgeGen, snap);
            InsertSbcEdgeDedup(w1, w2, unx, uny, unz, fade, isWall, edgeGen, snap);
            InsertSbcEdgeDedup(w2, w0, unx, uny, unz, fade, isWall, edgeGen, snap);
        }
    }
}

void CollectSnapshotSEH(std::uintptr_t exeBase,
                        std::uintptr_t exeEnd,
                        bool collectShapes,
                        std::uint32_t visMask,
                        float sbcRadiusWorldUnits,
                        RawSnapshot* outSnap) {
    outSnap->hasCamera       = false;
    outSnap->hasPlayerGround = false;
    outSnap->shapeCount      = 0;
    outSnap->sbcTriCount     = 0;
    outSnap->sbcEdgeCount    = 0;
    outSnap->hdrActive       = false;
    outSnap->hdrWhiteNits    = 200.0f;
    outSnap->mainPlDef       = PlayerFrameReadout{};

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

        if (!collectShapes) return;

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
                    std::memcpy(outSnap->view, reinterpret_cast<const void*>(vp + mhw::kOffVpViewMat), 64);
                    std::memcpy(outSnap->proj, reinterpret_cast<const void*>(vp + mhw::kOffVpProjMat), 64);
                    const float* vm = outSnap->view;
                    outSnap->camPos[0] = -(vm[12] * vm[0] + vm[13] * vm[1] + vm[14] * vm[2]);
                    outSnap->camPos[1] = -(vm[12] * vm[4] + vm[13] * vm[5] + vm[14] * vm[6]);
                    outSnap->camPos[2] = -(vm[12] * vm[8] + vm[13] * vm[9] + vm[14] * vm[10]);
                    outSnap->hasCamera = true;
                    break;
                }
            }
        }
        if (!outSnap->hasCamera || (visMask & app::kCatMaskAll) == 0) return;

        std::uintptr_t sColl = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSCollisionInstance);
        if (!IsAlignedPtr(sColl)) {
            sColl = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCollision);
        }

        // 2. Harvest surrounding .sbc Stage Terrain / Wall / Ledge Meshes if enabled
        if ((visMask & app::kCatStageMesh) != 0 && IsAlignedPtr(sColl)) {
            HarvestStageSbcMeshes(sColl, sbcRadiusWorldUnits, *outSnap);
        }

        const std::uint32_t entityBits = visMask & ~std::uint32_t(app::kCatStageMesh);
        if (entityBits == 0) return;

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

        // 4. Direct Monster Collision Components
        const bool wantAnyMonster = (visMask & (app::kCatEmHurtbox | app::kCatEmHitbox | app::kCatPushbox)) != 0;
        const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
        if (wantAnyMonster && IsAlignedPtr(sEnemy)) {
            using GetParentFn = std::uintptr_t (*)(std::uintptr_t);
            const auto getParent = reinterpret_cast<GetParentFn>(exeBase + mhw::kRvaAiEmGetParent);
            for (std::size_t i = 0; i < 128; ++i) {
                const std::uintptr_t aiEm = *reinterpret_cast<const std::uintptr_t*>(sEnemy + mhw::kOffSEnemyList + i * 8);
                if (!IsAlignedPtr(aiEm)) continue;
                const std::uintptr_t em = getParent(aiEm);
                if (!IsAlignedPtr(em)) continue;
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterCollPrimary),
                                      OwnerSide::Monster, exeBase, exeEnd, visMask, *outSnap);
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterColl),
                                      OwnerSide::Monster, exeBase, exeEnd, visMask, *outSnap);
                HarvestUnitComponents(em, OwnerSide::Monster, exeBase, exeEnd, visMask, *outSnap);
            }
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

    // Sutherland-Hodgman near-plane clipped 3D filled triangle (for lit .sbc surfaces & volume cores)
    void addTri3D(std::vector<OverlayVertex>& out,
                  const Vec3& a, const Vec3& b, const Vec3& c,
                  const Color4& col) const {
        if (col.a <= 0.004f) return;

        auto toClip = [&](const Vec3& p) -> ClipVert4 {
            return {
                p.x * vp[0] + p.y * vp[4] + p.z * vp[8]  + vp[12],
                p.x * vp[1] + p.y * vp[5] + p.z * vp[9]  + vp[13],
                p.x * vp[2] + p.y * vp[6] + p.z * vp[10] + vp[14],
                p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15]
            };
        };

        ClipVert4 polyA[8]{toClip(a), toClip(b), toClip(c)};
        int cntA = 3;

        // Clip 1: z >= 0.0f
        ClipVert4 polyB[8]{};
        int cntB = 0;
        for (int i = 0; i < cntA; ++i) {
            const ClipVert4& cur = polyA[i];
            const ClipVert4& nxt = polyA[(i + 1) % cntA];
            const bool inCur = (cur.z >= 0.0f);
            const bool inNxt = (nxt.z >= 0.0f);
            if (inCur) polyB[cntB++] = cur;
            if (inCur != inNxt) {
                const float denom = nxt.z - cur.z;
                const float t = (std::fabs(denom) > 1e-6f) ? ((0.0f - cur.z) / denom) : 0.0f;
                polyB[cntB++] = {
                    cur.x + (nxt.x - cur.x) * t,
                    cur.y + (nxt.y - cur.y) * t,
                    0.0f,
                    cur.w + (nxt.w - cur.w) * t
                };
            }
        }
        if (cntB < 3) return;

        // Clip 2: w >= 0.05f
        constexpr float kMinW = 0.05f;
        cntA = 0;
        for (int i = 0; i < cntB; ++i) {
            const ClipVert4& cur = polyB[i];
            const ClipVert4& nxt = polyB[(i + 1) % cntB];
            const bool inCur = (cur.w >= kMinW);
            const bool inNxt = (nxt.w >= kMinW);
            if (inCur) polyA[cntA++] = cur;
            if (inCur != inNxt) {
                const float denom = nxt.w - cur.w;
                const float t = (std::fabs(denom) > 1e-6f) ? ((kMinW - cur.w) / denom) : 0.0f;
                polyA[cntA++] = {
                    cur.x + (nxt.x - cur.x) * t,
                    cur.y + (nxt.y - cur.y) * t,
                    cur.z + (nxt.z - cur.z) * t,
                    kMinW
                };
            }
        }
        if (cntA < 3) return;

        // Clip 3: z <= w (Reversed-Z near plane nz <= 1.0f)
        cntB = 0;
        for (int i = 0; i < cntA; ++i) {
            const ClipVert4& cur = polyA[i];
            const ClipVert4& nxt = polyA[(i + 1) % cntA];
            const float dCur = cur.w - cur.z;
            const float dNxt = nxt.w - nxt.z;
            const bool inCur = (dCur >= 0.0f);
            const bool inNxt = (dNxt >= 0.0f);
            if (inCur) polyB[cntB++] = cur;
            if (inCur != inNxt) {
                const float denom = dCur - dNxt;
                const float t = (std::fabs(denom) > 1e-6f) ? (dCur / denom) : 0.0f;
                const float wInt = cur.w + (nxt.w - cur.w) * t;
                polyB[cntB++] = {
                    cur.x + (nxt.x - cur.x) * t,
                    cur.y + (nxt.y - cur.y) * t,
                    wInt,
                    wInt
                };
            }
        }
        if (cntB < 3) return;

        OverlayVertex ndc[8]{};
        float minX =  1e9f, maxX = -1e9f;
        float minY =  1e9f, maxY = -1e9f;
        for (int i = 0; i < cntB; ++i) {
            const float invW = 1.0f / polyB[i].w;
            const float nx = polyB[i].x * invW;
            const float ny = polyB[i].y * invW;
            const float nz = computeNdcZ(polyB[i].z, polyB[i].w);
            if (nx < minX) minX = nx;
            if (nx > maxX) maxX = nx;
            if (ny < minY) minY = ny;
            if (ny > maxY) maxY = ny;
            ndc[i] = {nx, ny, nz, 1.0f, col.r, col.g, col.b, col.a};
        }
        if (maxX < -1.5f || minX > 1.5f || maxY < -1.5f || minY > 1.5f) return;

        for (int i = 1; i + 1 < cntB; ++i) {
            out.push_back(ndc[0]);
            out.push_back(ndc[i]);
            out.push_back(ndc[i + 1]);
        }
    }

    void addLine3D(std::vector<OverlayVertex>& out,
                   const Vec3& a, const Vec3& b,
                   float halfThicknessPx, const Color4& col) const {
        if (col.a <= 0.004f) return;

        float ax = a.x * vp[0] + a.y * vp[4] + a.z * vp[8]  + vp[12];
        float ay = a.x * vp[1] + a.y * vp[5] + a.z * vp[9]  + vp[13];
        float az = a.x * vp[2] + a.y * vp[6] + a.z * vp[10] + vp[14];
        float aw = a.x * vp[3] + a.y * vp[7] + a.z * vp[11] + vp[15];

        float bx = b.x * vp[0] + b.y * vp[4] + b.z * vp[8]  + vp[12];
        float by = b.x * vp[1] + b.y * vp[5] + b.z * vp[9]  + vp[13];
        float bz = b.x * vp[2] + b.y * vp[6] + b.z * vp[10] + vp[14];
        float bw = b.x * vp[3] + b.y * vp[7] + b.z * vp[11] + vp[15];

        constexpr float kMinW = 0.05f;

        if (az < 0.0f && bz < 0.0f) return;
        if (az < 0.0f) {
            const float t = (0.0f - az) / (bz - az);
            ax += (bx - ax) * t; ay += (by - ay) * t; az = 0.0f; aw += (bw - aw) * t;
        } else if (bz < 0.0f) {
            const float t = (0.0f - bz) / (az - bz);
            bx += (ax - bx) * t; by += (ay - by) * t; bz = 0.0f; bw += (aw - bw) * t;
        }

        if (aw < kMinW && bw < kMinW) return;
        if (aw < kMinW) {
            const float t = (kMinW - aw) / (bw - aw);
            ax += (bx - ax) * t; ay += (by - ay) * t; az += (bz - az) * t; aw = kMinW;
        } else if (bw < kMinW) {
            const float t = (kMinW - bw) / (aw - bw);
            bx += (ax - bx) * t; by += (ay - by) * t; bz += (az - bz) * t; bw = kMinW;
        }

        const float da = aw - az;
        const float db = bw - bz;
        if (da < 0.0f && db < 0.0f) return;
        if (da < 0.0f) {
            const float t = da / (da - db);
            ax += (bx - ax) * t; ay += (by - ay) * t; aw += (bw - aw) * t; az = aw;
        } else if (db < 0.0f) {
            const float t = db / (db - da);
            bx += (ax - bx) * t; by += (ay - by) * t; bw += (aw - bw) * t; bz = bw;
        }

        const float ndcAx = ax / aw;
        const float ndcAy = ay / aw;
        const float ndcBx = bx / bw;
        const float ndcBy = by / bw;

        if ((ndcAx < -2.5f && ndcBx < -2.5f) || (ndcAx > 2.5f && ndcBx > 2.5f) ||
            (ndcAy < -2.5f && ndcBy < -2.5f) || (ndcAy > 2.5f && ndcBy > 2.5f)) {
            return;
        }

        const float sx0 = (ndcAx * 0.5f + 0.5f) * screenW;
        const float sy0 = (0.5f - ndcAy * 0.5f) * screenH;
        const float sx1 = (ndcBx * 0.5f + 0.5f) * screenW;
        const float sy1 = (0.5f - ndcBy * 0.5f) * screenH;

        const float z0 = computeNdcZ(az, aw);
        const float z1 = computeNdcZ(bz, bw);

        emitScreenQuad(out, sx0, sy0, sx1, sy1, halfThicknessPx, col, z0, z1);
    }

    // Perspective depth-scaled + front/back hemisphere-cued 3D line (eliminates flat Necker-cube illusion!)
    void addDepthLine3D(std::vector<OverlayVertex>& out,
                        const Vec3& a, const Vec3& b,
                        float baseHalfThicknessPx,
                        const Color4& baseCol,
                        float volCenterDist,
                        float volCharRadius,
                        bool depthCueFrontBack) const {
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

void DrawInteractiveFilterPanel(std::vector<OverlayVertex>& outVerts,
                                const LineProjector& proj,
                                const ColorSpaceEncoder& csEnc,
                                const app::ViewFilterState& vs,
                                float x0,
                                float y0,
                                float px,
                                float resScale) {
    const float ps     = px * 0.92f;
    const float padX   = 9.0f * resScale;
    const float padY   = 7.0f * resScale;
    const float rowH   = 10.5f * ps;
    const float hdrH   = 11.5f * ps;
    const float ftrH   = 10.5f * ps;
    const float sepGap = 5.0f * resScale;
    const float panelW = 60.0f * 6.0f * ps + padX * 2.0f;
    const float panelH = padY * 2.0f + hdrH + 13.0f * rowH + sepGap + ftrH;

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
                "FILTER & SCENE PANEL   ([=] PL+EM / MAP)", colHeader);

    float curY = y0 + padY + hdrH + 1.5f * resScale;
    const float valX = x0 + panelW - padX - 19.5f * 6.0f * ps;

    struct RowSpec {
        const char* label;
        bool hasSwatch;
        Color4 swatchSrgb;
    };

    const RowSpec kRows[13] = {
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
        {"[/]  TERRAIN STYLE & RINGS", false, {0, 0, 0, 0}}
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

    for (int r = 0; r < 13; ++r) {
        if (r == 9) {
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
        }

        DrawText5x7(outVerts, proj, valX, curY + 1.0f * ps, ps, valBuf, vCol);
        curY += rowH;
    }

    proj.emitRect2D(outVerts, x0, curY + 1.0f * resScale,
                    x0 + panelW, curY + 2.0f * resScale,
                    csEnc.encode({0.22f, 0.30f, 0.42f, 0.80f}));
    DrawText5x7(outVerts, proj, x0 + padX, curY + 3.0f * resScale, ps * 0.88f,
                "UP/DN:ROW  LT/RT/ENTER:SET  1-0/-///=:QUICK  P/ESC:CLOSE", colFooter);
}

} // namespace

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
    const bool activePure3D = (visEnabled || viewState.panelOpen) &&
                              (viewState.backdrop == app::BackdropMode::HidePlVfx ||
                               viewState.backdrop == app::BackdropMode::HideAll3D);
    const bool hasDimScrim  = (visEnabled || viewState.panelOpen) &&
                              (viewState.backdrop == app::BackdropMode::DimScrim);
    const bool hasBackdrop  = activePure3D || hasDimScrim;
    const bool activeHideEm = (visEnabled || viewState.panelOpen) && viewState.hideMonster;
    const bool activeHideSt = (visEnabled || viewState.panelOpen) &&
                              (viewState.stageHide != app::StageHideMode::Off);
    const bool anySceneMod  = activeHideEm || activeHideSt || hasBackdrop;
    if (!exeBase || screenW < 64.0f || screenH < 64.0f) return;
    if (!visEnabled && !anyFreeze && !viewState.panelOpen && !anySceneMod && !showStartupToast) return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeBase + dos->e_lfanew);
    const std::uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // MHW world coordinates are in centimeters (1 meter = 100.0f units)
    const float sbcRadiusWorldUnits = app::SbcRadiusMetersFromIdx(viewState.sbcRadiusIdx) * 100.0f;

    RawSnapshot snap{};
    CollectSnapshotSEH(exeBase, exeEnd, visEnabled, viewState.visMask, sbcRadiusWorldUnits, &snap);
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
    const float baseHalfThick = 0.5f * cfg.lineThickness * ((resScale > 0.75f) ? resScale : 0.75f);
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

    const bool isOpaqueSbc  = (viewState.depthMode == app::DepthGuideMode::Opaque ||
                               viewState.depthMode == app::DepthGuideMode::OpaqueNoRings);
    const bool use3DShade   = (viewState.depthMode != app::DepthGuideMode::RawWire);
    const bool sbcOn        = (viewState.visMask & app::kCatStageMesh) != 0;
    const bool showRings    = (viewState.depthMode == app::DepthGuideMode::AlwaysRings) ||
                              ((viewState.depthMode == app::DepthGuideMode::Auto ||
                                viewState.depthMode == app::DepthGuideMode::Opaque) &&
                               (activeHideSt || activeHideEm || hasBackdrop || sbcOn));

    // 2. 3D Stage .SBC Shaded Terrain + Ground Metric Radar Rings + Depth-Cued Entity Volumes
    if (visEnabled && snap.hasCamera && (snap.sbcEdgeCount > 0 || snap.shapeCount > 0 || showRings)) {
        outVerts.reserve(outVerts.size() +
                         static_cast<std::size_t>(snap.sbcTriCount) * 3 +
                         static_cast<std::size_t>(snap.sbcEdgeCount) * 6 +
                         static_cast<std::size_t>(snap.shapeCount) * 260 +
                         2048);

        // 2a. Lit 3D Shaded .SBC Surfaces (Opaque Z-Buffered OR Semi-Transparent Painter's Sort) + Creases
        if (snap.sbcEdgeCount > 0) {
            if (isOpaqueSbc && snap.sbcTriCount > 0) {
                proj.zBiasCm = 0.0f;
                // Pre-encode 32 HDR/SDR-accurate solid opaque shade steps per surface type (alpha = 1.0f)
                Color4 opaqueLut[4][32]{};
                static constexpr float kOpaqueRgb[4][3] = {
                    {0.15f, 0.36f, 0.50f}, // 0: Walkable Floor (Architectural Slate-Blue)
                    {0.72f, 0.43f, 0.18f}, // 1: Wall / Cliff / Pillar (Warm Sandstone-Amber)
                    {0.18f, 0.17f, 0.28f}, // 2: Ceiling / Overhang (Deep Muted Indigo)
                    {0.22f, 0.50f, 0.68f}  // 3: Slope 2m Elevation Contour Band
                };
                static constexpr float kFogRgb[3] = {0.06f, 0.08f, 0.11f};
                for (int k = 0; k < 4; ++k) {
                    for (int s = 0; s < 32; ++s) {
                        const float sh = 0.08f + (1.04f / 31.0f) * static_cast<float>(s);
                        const float fogT = (sh < 0.34f) ? ((0.34f - sh) * (1.0f / 0.26f)) : 0.0f;
                        auto cl = [](float v) { return (v > 1.0f) ? 1.0f : ((v < 0.0f) ? 0.0f : v); };
                        const float rLit = kOpaqueRgb[k][0] * sh;
                        const float gLit = kOpaqueRgb[k][1] * sh;
                        const float bLit = kOpaqueRgb[k][2] * sh;
                        opaqueLut[k][s] = csEnc.encode({
                            cl(rLit * (1.0f - fogT * 0.55f) + kFogRgb[0] * (fogT * 0.55f)),
                            cl(gLit * (1.0f - fogT * 0.55f) + kFogRgb[1] * (fogT * 0.55f)),
                            cl(bLit * (1.0f - fogT * 0.55f) + kFogRgb[2] * (fogT * 0.55f)),
                            1.0f
                        });
                    }
                }

                // O(N) 256-bucket Near-to-Far counting sort (bucket 0 = nearest -> 255 = farthest) for GPU Early-Z!
                std::uint32_t bCounts[256]{};
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    ++bCounts[g_sbcTris[i].distKey & 0xFFu];
                }
                std::uint32_t bOffsets[256]{};
                std::uint32_t running = 0;
                for (int b = 0; b < 256; ++b) {
                    bOffsets[b] = running;
                    running += bCounts[b];
                }
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const std::uint32_t b = g_sbcTris[i].distKey & 0xFFu;
                    g_sbcTriOrder[bOffsets[b]++] = i;
                }

                for (std::uint32_t oi = 0; oi < snap.sbcTriCount; ++oi) {
                    const SbcTriRecord& tr = g_sbcTris[g_sbcTriOrder[oi]];
                    const float effSh = tr.shade * (0.26f + 0.74f * tr.fade);
                    int si = static_cast<int>((effSh - 0.08f) * (31.0f / 1.04f) + 0.5f);
                    if (si < 0) si = 0;
                    else if (si > 31) si = 31;
                    proj.addTri3D(outVerts,
                                  {tr.x0, tr.y0, tr.z0},
                                  {tr.x1, tr.y1, tr.z1},
                                  {tr.x2, tr.y2, tr.z2},
                                  opaqueLut[tr.surfKind & 3u][si]);
                }

                // Pass 1 ends here: [0 .. opaqueTriVerts) writes Reversed-Z D32_FLOAT with Blend OFF
                outStats.opaqueTriVerts = static_cast<std::uint32_t>(outVerts.size());

                // Pass 2: Depth-tested .sbc geometric creases (4 cm camera-ward bias so creases sit crisply on faces!)
                proj.zBiasCm = 4.0f;
                const float htSoft  = baseHalfThick * 0.52f;
                const float htFloor = baseHalfThick * 0.84f;
                const float htWall  = baseHalfThick * 1.04f;
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
                    Color4 c = sharp ? (e.isWall ? colSharpWall : colSharpFlr)
                                     : (e.isWall ? colSoftWall  : colSoftFloor);
                    c.a *= (0.25f + 0.75f * e.fade);
                    const float ht = sharp ? (e.isWall ? htWall : htFloor) : htSoft;
                    proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                        ht, c, 0.0f, 0.0f, false);
                }
            } else if (use3DShade && snap.sbcTriCount > 0) {
                proj.zBiasCm = 0.0f;
                // Pre-encode 16 HDR/SDR-accurate shade steps per surface type (0 pow() per triangle!)
                const float surfAlphaScale = ((activeHideSt || activePure3D) ? 1.15f : 1.00f) * cfg.sbcSurfaceOpacity;
                Color4 shadeLut[4][16]{};
                static constexpr float kBaseRgb[4][3] = {
                    {0.15f, 0.44f, 0.66f}, // 0: Walkable Floor (Cool Slate-Blue)
                    {0.88f, 0.52f, 0.18f}, // 1: Wall / Cliff / Pillar (Warm Terracotta-Amber)
                    {0.24f, 0.22f, 0.40f}, // 2: Ceiling / Overhang (Muted Indigo)
                    {0.22f, 0.58f, 0.82f}  // 3: Slope 2m Elevation Contour Band
                };
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

                // O(N) 256-bucket Far-to-Near Painter's counting sort (bucket 255 = farthest -> 0 = nearest)
                std::uint32_t bCounts[256]{};
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    ++bCounts[g_sbcTris[i].distKey & 0xFFu];
                }
                std::uint32_t bOffsets[256]{};
                std::uint32_t running = 0;
                for (int b = 255; b >= 0; --b) {
                    bOffsets[b] = running;
                    running += bCounts[b];
                }
                for (std::uint32_t i = 0; i < snap.sbcTriCount; ++i) {
                    const std::uint32_t b = g_sbcTris[i].distKey & 0xFFu;
                    g_sbcTriOrder[bOffsets[b]++] = i;
                }

                for (std::uint32_t oi = 0; oi < snap.sbcTriCount; ++oi) {
                    const SbcTriRecord& tr = g_sbcTris[g_sbcTriOrder[oi]];
                    int si = static_cast<int>((tr.shade - 0.34f) * (15.0f / 0.76f) + 0.5f);
                    if (si < 0) si = 0;
                    else if (si > 15) si = 15;
                    Color4 col = shadeLut[tr.surfKind & 3u][si];
                    col.a *= tr.fade;
                    proj.addTri3D(outVerts,
                                  {tr.x0, tr.y0, tr.z0},
                                  {tr.x1, tr.y1, tr.z1},
                                  {tr.x2, tr.y2, tr.z2},
                                  col);
                }

                // Draw ONLY real geometric creases & boundaries (skip flat coplanar internal grid lines!)
                proj.zBiasCm = 4.0f;
                const float htSoft  = baseHalfThick * 0.46f;
                const float htFloor = baseHalfThick * 0.78f;
                const float htWall  = baseHalfThick * 0.96f;
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
                    Color4 c = sharp ? (e.isWall ? colSharpWall : colSharpFlr)
                                     : (e.isWall ? colSoftWall  : colSoftFloor);
                    c.a *= e.fade;
                    const float ht = sharp ? (e.isWall ? htWall : htFloor) : htSoft;
                    proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                        ht, c, 0.0f, 0.0f, false);
                }
            } else {
                // Classic raw full wireframe (with perspective depth-scaled thickness & radial fade)
                proj.zBiasCm = 4.0f;
                const float htFloor = baseHalfThick * 0.60f;
                const float htWall  = baseHalfThick * 0.85f;
                const Color4 colFloor = csEnc.encode({0.20f, 0.66f, 0.82f, clampA(0.42f)});
                const Color4 colWall  = csEnc.encode({0.94f, 0.64f, 0.24f, clampA(0.68f)});

                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.isWall == 0) {
                        Color4 c = colFloor; c.a *= e.fade;
                        proj.addDepthLine3D(outVerts, {e.x0, e.y0, e.z0}, {e.x1, e.y1, e.z1},
                                            htFloor, c, 0.0f, 0.0f, false);
                    }
                }
                for (std::uint32_t i = 0; i < snap.sbcEdgeCount; ++i) {
                    const SbcEdgeRecord& e = g_sbcEdges[i];
                    if (e.isWall != 0) {
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
            EmitGroundRadarAndDropPins(outVerts, proj, csEnc, snap, baseHalfThick, px, alphaMul);
        }

        // End of Pass 2: [opaqueTriVerts .. sceneDepthEndVert) is tested against MHW's 3D tDepthMap in ALL modes!
        outStats.sceneDepthEndVert = static_cast<std::uint32_t>(outVerts.size());

        // 2c. Draw Entity Collision Volumes (ordered back-to-front by VolumeCat, with 3D front/back depth cue!)
        proj.zBiasCm = (outStats.opaqueTriVerts > 0) ? 18.0f : 0.0f;
        if (snap.shapeCount > 0) {
            const float htMonsterAtk  = baseHalfThick * 1.45f;
            const float htPlayerAtk   = baseHalfThick * 1.35f;
            const float htOtomoAtk    = baseHalfThick * 1.15f;
            const float htEnvAtk      = baseHalfThick * 1.30f;
            const float htMonsterHurt = baseHalfThick * 0.90f;
            const float htOtomoHurt   = baseHalfThick * 0.85f;
            const float htEnvHurt     = baseHalfThick * 0.95f;
            const float htPushbox     = baseHalfThick * 0.75f;
            const float htEnvPush     = baseHalfThick * 0.70f;

            const Color4 colMonsterAtk  = csEnc.encode({1.00f, 0.16f, 0.18f, clampA(0.95f)});
            const Color4 colPlayerAtk   = csEnc.encode({1.00f, 0.86f, 0.10f, clampA(0.95f)});
            const Color4 colOtomoAtk    = csEnc.encode({1.00f, 0.72f, 0.22f, clampA(0.90f)});
            const Color4 colEnvAtk      = csEnc.encode({1.00f, 0.48f, 0.12f, clampA(0.94f)});
            const Color4 colMonsterHurt = csEnc.encode({0.15f, 0.95f, 0.38f, clampA(0.72f)});
            const Color4 colOtomoHurt   = csEnc.encode({0.38f, 0.78f, 1.00f, clampA(0.75f)});
            const Color4 colEnvHurt     = csEnc.encode({0.22f, 0.94f, 0.76f, clampA(0.84f)});
            const Color4 colPushbox     = csEnc.encode({0.72f, 0.56f, 1.00f, clampA(0.42f)});
            const Color4 colEnvPush     = csEnc.encode({0.46f, 0.62f, 0.84f, clampA(0.45f)});

            Color4 colPlDef[7]{};
            float  htPlDef[7]{};
            for (int d = 0; d < 7; ++d) {
                const float a = (d == 0) ? clampA(0.82f) : clampA(0.96f);
                colPlDef[d] = csEnc.encode(PlayerDefSrgbColor(static_cast<PlayerDefState>(d), a, baseHalfThick, htPlDef[d]));
            }

            for (std::uint32_t i = 0; i < snap.shapeCount; ++i) {
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

                for (std::uint32_t i = 0; i < snap.shapeCount; ++i) {
                    const ShapeRecord& s = g_shapes[i];
                    if (s.cat != wantCat) continue;

                    Color4 col{};
                    float halfThick = baseHalfThick;
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
                            col = colMonsterHurt; halfThick = htMonsterHurt;
                            fillRatio = (activeHideEm || viewState.backdrop == app::BackdropMode::HideAll3D)
                                            ? 0.07f : 0.0f;
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
                            col = colPushbox; halfThick = htPushbox; break;
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

        // End of Pass 3: [sceneDepthEndVert .. worldOverlayEndVert) completes BEFORE 2D Player UI Restore!
        outStats.worldOverlayEndVert = static_cast<std::uint32_t>(outVerts.size());
        proj.zBiasCm = 0.0f;
    }

    // 3. 2D Top-Left Status HUD Banner + Interactive [P] Filter & Scene Panel
    const float x0 = 18.0f * resScale;
    const float y0 = 18.0f * resScale;
    float panelTopY = y0;

    if (cfg.showHudBanner && (visEnabled || anyFreeze || viewState.panelOpen || anySceneMod || showStartupToast)) {
        const char* freezeStr = "OFF";
        if (freezeMode == freeze::Mode::Full) {
            freezeStr = "ALL ([O] STEP)";
        } else if (freezeMode == freeze::Mode::World) {
            freezeStr = freeze::IsKeepHitboxesActive()
                            ? "WORLD ([O] STEP [J] KEEP:ON)"
                            : "WORLD ([O] STEP [J] KEEP:OFF)";
        }

        char line1[256]{};
        char stateSeg[48]{};

        if (!visEnabled && !anyFreeze && !viewState.panelOpen && !anySceneMod && showStartupToast) {
            std::snprintf(line1, sizeof(line1),
                          "DEBUG HITBOX READY  |  [U] VIS  [I] FREEZE  [K] WORLD  [J] KEEP  [O] +1F  [P] PANEL");
        } else {
            char mapSeg[48]{};
            if ((viewState.visMask & (app::kCatEnvObject | app::kCatStageMesh)) != 0) {
                std::snprintf(mapSeg, sizeof(mapSeg), " MAP:%u SBC:%u",
                              outStats.envShapes, outStats.sbcEdges);
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

            std::snprintf(line1, sizeof(line1),
                          "VIS:%s [%s]  PL:%u/%u EM:%u/%u%s%s  |  FREEZE:%s  [P]",
                          visEnabled ? "ON" : "OFF",
                          app::MatchPresetName(viewState.visMask),
                          outStats.playerHurtbox, outStats.playerHitboxes,
                          outStats.monsterHurtbox, outStats.monsterHitbox,
                          mapSeg,
                          sceneSeg,
                          freezeStr);
            if (snap.mainPlDef.valid) {
                std::snprintf(stateSeg, sizeof(stateSeg), "  |  FRAME:%s", snap.mainPlDef.badge);
            }
        }

        const std::size_t len1 = std::strlen(line1);
        const std::size_t len2 = std::strlen(stateSeg);
        const float textW1 = static_cast<float>(len1 + len2) * 6.0f * px;
        const bool showLegendBar = visEnabled && !viewState.panelOpen;
        const float minLegendW = showLegendBar ? (66.0f * 6.0f * 0.85f * px) : 0.0f;
        const float boxInnerW = (textW1 > minLegendW) ? textW1 : minLegendW;

        const float padX = 8.0f * resScale;
        const float padY = 6.0f * resScale;
        const float barH = 7.0f * px + padY * 2.0f + (showLegendBar ? (10.0f * px) : 0.0f);

        Color4 accentSrgb = {0.15f, 0.90f, 0.65f, 0.95f};
        if (freezeMode == freeze::Mode::Full)       accentSrgb = {1.00f, 0.30f, 0.20f, 0.95f};
        else if (freezeMode == freeze::Mode::World) accentSrgb = {1.00f, 0.68f, 0.14f, 0.95f};

        proj.emitRect2D(outVerts, x0, y0, x0 + boxInnerW + padX * 2.0f, y0 + barH,
                        csEnc.encode({0.05f, 0.06f, 0.09f, 0.82f}));
        proj.emitRect2D(outVerts, x0, y0, x0 + 3.5f * resScale, y0 + barH,
                        csEnc.encode(accentSrgb));

        const Color4 colTextMain = csEnc.encode({0.95f, 0.97f, 1.00f, 0.96f});
        DrawText5x7(outVerts, proj, x0 + padX, y0 + padY, px, line1, colTextMain);

        if (len2 > 0) {
            float dummyHt = 0.0f;
            Color4 badgeSrgb = PlayerDefSrgbColor(snap.mainPlDef.state, 0.98f, baseHalfThick, dummyHt);
            if (snap.mainPlDef.state == PlayerDefState::Normal) {
                badgeSrgb = {0.72f, 0.88f, 0.98f, 0.95f};
            }
            DrawText5x7(outVerts, proj,
                        x0 + padX + static_cast<float>(len1) * 6.0f * px,
                        y0 + padY, px, stateSeg, csEnc.encode(badgeSrgb));
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

        panelTopY = y0 + barH + 6.0f * resScale;
    }

    if (viewState.panelOpen) {
        DrawInteractiveFilterPanel(outVerts, proj, csEnc, viewState, x0, panelTopY, px, resScale);
    }
}

} // namespace coll

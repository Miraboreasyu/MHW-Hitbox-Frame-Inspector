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
    Monster = 2,
    Env     = 3
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

// Ordered from back-to-front draw priority so Attack Hitboxes always render on top!
enum class VolumeCat : std::uint8_t {
    OtherPush      = 0,
    MonsterHurtbox = 1,
    PlayerHurtbox  = 2,
    PlayerHitbox   = 3,
    MonsterHitbox  = 4,
    Count          = 5
};

struct ShapeRecord {
    VolumeCat cat;
    PlayerDefState plDef;
    std::int32_t type;
    float p[20];
};

constexpr std::uint32_t kMaxShapes = 2560;

template <std::uint32_t N>
struct PtrSet {
    std::uintptr_t keys[N];
    void clear() { std::memset(keys, 0, sizeof(keys)); }
    bool insert(std::uintptr_t k) {
        if (!k) return false;
        std::uint32_t idx = static_cast<std::uint32_t>((k >> 4) * 2654435761ULL) & (N - 1);
        for (std::uint32_t n = 0; n < N; ++n) {
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
    std::uint8_t probeCount;
    std::uint8_t numOffsets;
    std::uint32_t offsets[8];
};

struct TrackedCollEntry {
    std::uintptr_t coll;
    OwnerSide side;
};

constexpr std::uint32_t kDtiCacheSize    = 1024;
constexpr std::uint32_t kMaxTrackedColls = 512;

// Persistent POD caches (only accessed on the render/present thread)
DtiCacheEntry g_dtiCache[kDtiCacheSize]{};
TrackedCollEntry g_trackedColls[kMaxTrackedColls]{};
std::uint32_t g_trackedCollCount = 0;
PtrSet<2048> g_seenColls;
PtrSet<4096> g_seenGeoms;
ShapeRecord g_shapes[kMaxShapes];

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
    std::uint32_t shapeCount;
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

    if (StartsWith(name, "uPlayer") || StartsWith(name, "uPl") ||
        StartsWith(name, "uWeapon") || StartsWith(name, "uWp") ||
        StartsWith(name, "uShellPl") || StartsWith(name, "uOtomo") ||
        StartsWith(name, "uOt") || StartsWith(name, "uInsect") ||
        StartsWith(name, "uHumanClaw") || StartsWith(name, "uClaw") ||
        StartsWith(name, "cAttackParamPl") || StartsWith(name, "cAttackParamOt") ||
        StartsWith(name, "cCollNodePl")) {
        return OwnerSide::Player;
    }
    if (StartsWith(name, "Monster") || StartsWith(name, "uEnemy") ||
        StartsWith(name, "uEm") || StartsWith(name, "cEm") ||
        StartsWith(name, "uAiEm") || StartsWith(name, "uShellEm") ||
        StartsWith(name, "uShellStage") ||
        StartsWith(name, "cAttackParamEm") || StartsWith(name, "cCollNodeEm")) {
        return OwnerSide::Monster;
    }
    if (StartsWith(name, "uGm") || StartsWith(name, "uStage") || StartsWith(name, "uBg")) {
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

DtiCacheEntry* LookupOrCreateDtiEntry(std::uintptr_t dti, std::uintptr_t exeBase) {
    if (!dti) return nullptr;
    std::uint32_t idx = static_cast<std::uint32_t>((dti >> 4) * 2654435761ULL) & (kDtiCacheSize - 1);
    for (std::uint32_t i = 0; i < kDtiCacheSize; ++i) {
        if (g_dtiCache[idx].dti == dti) return &g_dtiCache[idx];
        if (g_dtiCache[idx].dti == 0) {
            DtiCacheEntry& e = g_dtiCache[idx];
            e.dti = dti;
            e.side = OwnerSide::Unknown;
            e.probeCount = 0;
            e.numOffsets = 0;

            std::uintptr_t cur = dti;
            for (int depth = 0; depth < 20 && IsAlignedPtr(cur); ++depth) {
                if (exeBase) {
                    if (cur == exeBase + mhw::kRvaDtiAttackParamEm ||
                        cur == exeBase + mhw::kRvaDtiCollNodeEm) {
                        e.side = OwnerSide::Monster;
                        break;
                    }
                    if (cur == exeBase + mhw::kRvaDtiAttackParamPl ||
                        cur == exeBase + mhw::kRvaDtiAttackParamPlShell ||
                        cur == exeBase + mhw::kRvaDtiAttackParamPlBase ||
                        cur == exeBase + mhw::kRvaDtiAttackParamOt ||
                        cur == exeBase + mhw::kRvaDtiCollNodePl) {
                        e.side = OwnerSide::Player;
                        break;
                    }
                }
                const char* clsName = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
                const OwnerSide s = ClassifyClassName(clsName);
                if (s != OwnerSide::Unknown) {
                    e.side = s;
                    break;
                }
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

void RememberTrackedCollision(std::uintptr_t coll, OwnerSide side) {
    if (!coll) return;
    for (std::uint32_t i = 0; i < g_trackedCollCount; ++i) {
        if (g_trackedColls[i].coll == coll) {
            if (side != OwnerSide::Unknown) g_trackedColls[i].side = side;
            return;
        }
    }
    if (g_trackedCollCount < kMaxTrackedColls) {
        g_trackedColls[g_trackedCollCount++] = {coll, side};
    }
}

bool RecordMtGeometry(std::uintptr_t geom,
                      VolumeCat cat,
                      PlayerDefState plDef,
                      app::FilterMode filterMode,
                      RawSnapshot& snap) {
    if (!IsAlignedPtr(geom)) return false;
    if (snap.shapeCount >= kMaxShapes) return false;

    if (filterMode == app::FilterMode::Combat && cat == VolumeCat::OtherPush) return false;
    if (filterMode == app::FilterMode::Hitbox &&
        cat != VolumeCat::PlayerHitbox && cat != VolumeCat::MonsterHitbox) return false;
    if (filterMode == app::FilterMode::Hurtbox &&
        cat != VolumeCat::PlayerHurtbox && cat != VolumeCat::MonsterHurtbox) return false;

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
    if (isPushOnly) return VolumeCat::OtherPush;

    if (isAttack) {
        // Only verified Player/Weapon/Palico/Kinsect attacks are Gold; all Monster/Magic/Breath/Hazard attacks are Red!
        if (side == OwnerSide::Player) return VolumeCat::PlayerHitbox;
        return VolumeCat::MonsterHitbox;
    }

    if (side == OwnerSide::Monster) return VolumeCat::MonsterHurtbox;
    if (side == OwnerSide::Player)  return VolumeCat::PlayerHurtbox;
    // Damageable gimmick targets (e.g. Training Area pole/cart, barrel bombs) have bit 0 (0x01) set!
    if ((nodeAttr & 0x01U) != 0)    return VolumeCat::MonsterHurtbox;
    return VolumeCat::OtherPush;
}

void HarvestCpObjCollision(std::uintptr_t coll,
                           OwnerSide hintSide,
                           std::uintptr_t exeBase,
                           std::uintptr_t exeEnd,
                           app::FilterMode filterMode,
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
    const std::int32_t hitGroup = *reinterpret_cast<const std::int32_t*>(coll + mhw::kOffObjCollHitGroup);
    if (baseSide == OwnerSide::Unknown || baseSide == OwnerSide::Env) {
        if (hitGroup == 1) baseSide = OwnerSide::Player;
        else if (hitGroup == 2 || hitGroup == 3 || hitGroup == 5) baseSide = OwnerSide::Monster;
    }

    RememberTrackedCollision(coll, baseSide);

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

        // Inspect cAttackParam* / cCollNode* MtDTI first so monster magic/breath VFX (uMhEfc) are 100% Monster!
        OwnerSide nodeSide = baseSide;
        if (IsAlignedPtr(atkParam)) {
            const OwnerSide atkSide = ResolveObjectDtiSide(atkParam, exeBase, exeEnd);
            if (atkSide != OwnerSide::Unknown) nodeSide = atkSide;
        }
        if (nodeSide == OwnerSide::Unknown && IsAlignedPtr(resCollNode)) {
            const OwnerSide resSide = ResolveObjectDtiSide(resCollNode, exeBase, exeEnd);
            if (resSide != OwnerSide::Unknown) nodeSide = resSide;
        }

        std::uint32_t nodeAttr = 0;
        if (IsAlignedPtr(snode)) {
            nodeAttr = *reinterpret_cast<const std::uint32_t*>(snode + mhw::kOffNodeObjAttr);
        }
        const bool isAttack = (atkParam != 0) || ((nodeAttr & 0x02U) != 0 && (nodeAttr & 0x01U) == 0);
        const VolumeCat cat = DetermineNodeCategory(nodeSide, isAttack, nodeAttr);
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
                RecordMtGeometry(sweptGeom, cat, plDef, filterMode, snap);
            } else if (IsAlignedPtr(primGeom)) {
                RecordMtGeometry(primGeom, cat, plDef, filterMode, snap);
            }
        }
    }
}

void HarvestUnitComponents(std::uintptr_t unit,
                           OwnerSide hintSide,
                           std::uintptr_t exeBase,
                           std::uintptr_t exeEnd,
                           app::FilterMode filterMode,
                           RawSnapshot& snap) {
    if (!IsAlignedPtr(unit)) return;
    std::uintptr_t comp = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
    for (int guard = 0; guard < 64 && IsAlignedPtr(comp); ++guard) {
        const std::uintptr_t nextComp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
        if (IsCpObjCollision(comp, exeBase, exeEnd)) {
            HarvestCpObjCollision(comp, hintSide, exeBase, exeEnd, filterMode, snap);
        }
        comp = nextComp;
    }
}

void HarvestNodeObjectFallback(std::uintptr_t nodeObj,
                               bool fromActiveList,
                               std::uintptr_t exeBase,
                               std::uintptr_t exeEnd,
                               app::FilterMode filterMode,
                               RawSnapshot& snap) {
    if (!IsAlignedPtr(nodeObj)) return;

    const std::uintptr_t userData = *reinterpret_cast<const std::uintptr_t*>(nodeObj + mhw::kOffNodeObjUserData);
    if (IsAlignedPtr(userData)) {
        const std::uintptr_t parentColl = *reinterpret_cast<const std::uintptr_t*>(userData + mhw::kOffCNodeParentColl);
        if (IsCpObjCollision(parentColl, exeBase, exeEnd)) {
            HarvestCpObjCollision(parentColl, OwnerSide::Unknown, exeBase, exeEnd, filterMode, snap);
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
            HarvestCpObjCollision(owner, OwnerSide::Unknown, exeBase, exeEnd, filterMode, snap);
            return;
        }
        side = ResolveUnitSide(owner, exeBase, exeEnd);
    }

    const std::uint32_t attr = *reinterpret_cast<const std::uint32_t*>(nodeObj + mhw::kOffNodeObjAttr);
    const bool isAttack = fromActiveList && ((attr & 0x02U) != 0);
    const VolumeCat cat = DetermineNodeCategory(side, isAttack, attr);

    for (std::uint32_t g = 0; g < geomCount; ++g) {
        const std::uintptr_t cgeom = *reinterpret_cast<const std::uintptr_t*>(geomArr + g * 8);
        if (!IsAlignedPtr(cgeom)) continue;
        if (*reinterpret_cast<const std::uint8_t*>(cgeom + 0x08) == 0) continue;
        const std::uintptr_t mtGeom = *reinterpret_cast<const std::uintptr_t*>(cgeom + 0x10);
        RecordMtGeometry(mtGeom, cat, PlayerDefState::Normal, filterMode, snap);
    }
}

void CollectSnapshotSEH(std::uintptr_t exeBase,
                        std::uintptr_t exeEnd,
                        bool collectShapes,
                        app::FilterMode filterMode,
                        RawSnapshot* outSnap) {
    outSnap->hasCamera    = false;
    outSnap->shapeCount   = 0;
    outSnap->hdrActive    = false;
    outSnap->hdrWhiteNits = 200.0f;
    outSnap->mainPlDef    = PlayerFrameReadout{};

    __try {
        // Read sMhRender HDR state even when only drawing the 2D HUD banner
        const std::uintptr_t mhRender = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhRenderInstance);
        if (IsAlignedPtr(mhRender)) {
            outSnap->hdrActive = (*reinterpret_cast<const std::uint8_t*>(mhRender + mhw::kOffRenderHdrOutput) != 0);
            const float whiteNits = *reinterpret_cast<const float*>(mhRender + mhw::kOffRenderHdrWhiteNits);
            if (std::isfinite(whiteNits) && whiteNits >= 50.0f && whiteNits <= 2000.0f) {
                outSnap->hdrWhiteNits = whiteNits;
            }
        }

        // Read Main Player defensive frame state for HUD + hurtbox coloring
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
                }
            }
        }

        if (!collectShapes) return;

        const std::uintptr_t mhMain = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhMainInstance);
        if (!IsAlignedPtr(mhMain)) return;

        // 1. Read Active Camera View & Projection Matrices (sMhCamera::GetActiveCamera)
        const std::uintptr_t camSys = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCamera);
        if (IsAlignedPtr(camSys)) {
            for (int vpIdx = 0; vpIdx < 8; ++vpIdx) {
                const std::uintptr_t vp = camSys + static_cast<std::size_t>(vpIdx) * mhw::kCameraViewportStride;
                const std::uint8_t visible = *reinterpret_cast<const std::uint8_t*>(vp + mhw::kOffVpVisible);
                const std::uintptr_t camObj = *reinterpret_cast<const std::uintptr_t*>(vp + mhw::kOffVpCameraPtr);
                if (visible != 0 && IsAlignedPtr(camObj)) {
                    std::memcpy(outSnap->view, reinterpret_cast<const void*>(vp + mhw::kOffVpViewMat), 64);
                    std::memcpy(outSnap->proj, reinterpret_cast<const void*>(vp + mhw::kOffVpProjMat), 64);
                    outSnap->hasCamera = true;
                    break;
                }
            }
        }
        if (!outSnap->hasCamera) return;

        g_seenColls.clear();
        g_seenGeoms.clear();

        // 2. Direct Player Collision Components (Main Player + Multiplayer Global Players 0..3)
        if (IsAlignedPtr(sPlayer)) {
            if (IsAlignedPtr(mainPl)) {
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(mainPl + mhw::kOffUPlayerColl),
                                      OwnerSide::Player, exeBase, exeEnd, filterMode, *outSnap);
                HarvestUnitComponents(mainPl, OwnerSide::Player, exeBase, exeEnd, filterMode, *outSnap);
            }
            const std::uint32_t plCnt = *reinterpret_cast<const std::uint32_t*>(sPlayer + mhw::kOffSPlayerGlobalCnt);
            for (std::uint32_t i = 0; i < plCnt && i < 4; ++i) {
                const std::uintptr_t pl = *reinterpret_cast<const std::uintptr_t*>(sPlayer + mhw::kOffSPlayerGlobalArr + i * 8);
                if (IsAlignedPtr(pl)) {
                    HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(pl + mhw::kOffUPlayerColl),
                                          OwnerSide::Player, exeBase, exeEnd, filterMode, *outSnap);
                    HarvestUnitComponents(pl, OwnerSide::Player, exeBase, exeEnd, filterMode, *outSnap);
                }
            }
        }

        // 3. Direct Monster Collision Components (BOTH +0x6A8 primary hurtboxes AND +0x7628 secondary + component list!)
        const std::uintptr_t sEnemy = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSEnemyInstance);
        if (IsAlignedPtr(sEnemy)) {
            using GetParentFn = std::uintptr_t (*)(std::uintptr_t);
            const auto getParent = reinterpret_cast<GetParentFn>(exeBase + mhw::kRvaAiEmGetParent);
            for (std::size_t i = 0; i < 128; ++i) {
                const std::uintptr_t aiEm = *reinterpret_cast<const std::uintptr_t*>(sEnemy + mhw::kOffSEnemyList + i * 8);
                if (!IsAlignedPtr(aiEm)) continue;
                const std::uintptr_t em = getParent(aiEm);
                if (!IsAlignedPtr(em)) continue;
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterCollPrimary),
                                      OwnerSide::Monster, exeBase, exeEnd, filterMode, *outSnap);
                HarvestCpObjCollision(*reinterpret_cast<const std::uintptr_t*>(em + mhw::kOffMonsterColl),
                                      OwnerSide::Monster, exeBase, exeEnd, filterMode, *outSnap);
                HarvestUnitComponents(em, OwnerSide::Monster, exeBase, exeEnd, filterMode, *outSnap);
            }
        }

        // 4. Universal sMhUnit Traversal (walks cComponentManager list +0x78 AND probes member offsets up to 256 KB)
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
                        const std::uintptr_t dti = GetObjectDti(unit, exeBase, exeEnd);
                        DtiCacheEntry* entry = LookupOrCreateDtiEntry(dti, exeBase);
                        const OwnerSide unitSide = entry ? entry->side : OwnerSide::Unknown;

                        // 4a. Walk cUnit's component linked list (+0x78 -> +0x08) — catches every cpObjCollision regardless of struct size!
                        HarvestUnitComponents(unit, unitSide, exeBase, exeEnd, filterMode, *outSnap);

                        // 4b. Also probe direct pointer fields up to 0x40000 bytes (8 slots)
                        if (entry) {
                            if (entry->numOffsets == 0 && entry->probeCount < 12) {
                                ++entry->probeCount;
                                const std::uint32_t sizeFlags = *reinterpret_cast<const std::uint32_t*>(dti + mhw::kOffDtiSizeFlags);
                                std::uint32_t objBytes = (sizeFlags & 0x7FFFFFU) * 4U;
                                if (objBytes > 0x40000U) objBytes = 0x40000U;
                                for (std::uint32_t off = 0x38; off + 8 <= objBytes && entry->numOffsets < 8; off += 8) {
                                    const std::uintptr_t cand = *reinterpret_cast<const std::uintptr_t*>(unit + off);
                                    if (IsCpObjCollision(cand, exeBase, exeEnd)) {
                                        entry->offsets[entry->numOffsets++] = off;
                                    }
                                }
                            }
                            for (std::uint8_t k = 0; k < entry->numOffsets; ++k) {
                                const std::uintptr_t coll = *reinterpret_cast<const std::uintptr_t*>(unit + entry->offsets[k]);
                                HarvestCpObjCollision(coll, entry->side, exeBase, exeEnd, filterMode, *outSnap);
                            }
                        }
                    }
                    unit = nextUnit;
                }
            }
        }

        // 5. Fallback sCollision::Collider NodeLists
        std::uintptr_t sColl = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSCollisionInstance);
        if (!IsAlignedPtr(sColl)) {
            sColl = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCollision);
        }
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
                                            false, exeBase, exeEnd, filterMode, *outSnap);
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
                                        false, exeBase, exeEnd, filterMode, *outSnap);
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
                                        true, exeBase, exeEnd, filterMode, *outSnap);
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
                                            true, exeBase, exeEnd, filterMode, *outSnap);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 6. Sweep persistent tracked cpObjCollision* cache (guarantees paused-frame stability & prunes freed units)
        std::uint32_t writeIdx = 0;
        for (std::uint32_t i = 0; i < g_trackedCollCount; ++i) {
            const TrackedCollEntry tc = g_trackedColls[i];
            if (!app::Readable(tc.coll, 0x200) || !IsCpObjCollision(tc.coll, exeBase, exeEnd)) continue;
            const std::uint8_t cFlags = *reinterpret_cast<const std::uint8_t*>(tc.coll + mhw::kOffObjCollFlags);
            const std::uintptr_t owner = *reinterpret_cast<const std::uintptr_t*>(tc.coll + mhw::kOffObjCollOwner);
            if ((cFlags & 0x01U) == 0 || !IsAlignedPtr(owner) || !app::Readable(owner, 0x40)) continue;
            const std::uint8_t oAlive = *reinterpret_cast<const std::uint8_t*>(owner + mhw::kOffUnitAliveFlags);
            if ((oAlive & 0x0EU) == 0) continue;

            g_trackedColls[writeIdx++] = tc;
            HarvestCpObjCollision(tc.coll, tc.side, exeBase, exeEnd, filterMode, *outSnap);
        }
        g_trackedCollCount = writeIdx;
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

struct LineProjector {
    float vp[16]{};
    float screenW = 1920.0f;
    float screenH = 1080.0f;
    float invW2   = 2.0f / 1920.0f;
    float invH2   = 2.0f / 1080.0f;

    void init(const float view[16], const float proj[16], float w, float h) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                vp[r * 4 + c] =
                    view[r * 4 + 0] * proj[0 * 4 + c] +
                    view[r * 4 + 1] * proj[1 * 4 + c] +
                    view[r * 4 + 2] * proj[2 * 4 + c] +
                    view[r * 4 + 3] * proj[3 * 4 + c];
            }
        }
        screenW = (w > 1.0f) ? w : 1920.0f;
        screenH = (h > 1.0f) ? h : 1080.0f;
        invW2 = 2.0f / screenW;
        invH2 = 2.0f / screenH;
    }

    void emitScreenQuad(std::vector<OverlayVertex>& out,
                        float sx0, float sy0, float sx1, float sy1,
                        float halfThicknessPx, const Color4& col) const {
        const float dx = sx1 - sx0;
        const float dy = sy1 - sy0;
        const float lenSq = dx * dx + dy * dy;
        if (lenSq < 0.04f) return;
        const float invLen = 1.0f / std::sqrt(lenSq);
        const float nx = -dy * invLen * halfThicknessPx;
        const float ny =  dx * invLen * halfThicknessPx;

        auto toNdc = [&](float px, float py) -> OverlayVertex {
            return {
                px * invW2 - 1.0f,
                1.0f - py * invH2,
                0.0f, 1.0f,
                col.r, col.g, col.b, col.a
            };
        };

        const OverlayVertex v0 = toNdc(sx0 - nx, sy0 - ny);
        const OverlayVertex v1 = toNdc(sx0 + nx, sy0 + ny);
        const OverlayVertex v2 = toNdc(sx1 + nx, sy1 + ny);
        const OverlayVertex v3 = toNdc(sx1 - nx, sy1 - ny);

        out.push_back(v0); out.push_back(v1); out.push_back(v2);
        out.push_back(v0); out.push_back(v2); out.push_back(v3);
    }

    void emitRect2D(std::vector<OverlayVertex>& out,
                    float x0, float y0, float x1, float y1, const Color4& col) const {
        auto toNdc = [&](float px, float py) -> OverlayVertex {
            return {
                px * invW2 - 1.0f,
                1.0f - py * invH2,
                0.0f, 1.0f,
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

    void addLine3D(std::vector<OverlayVertex>& out,
                   const Vec3& a, const Vec3& b,
                   float halfThicknessPx, const Color4& col) const {
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

        emitScreenQuad(out, sx0, sy0, sx1, sy1, halfThicknessPx, col);
    }
};

void EmitSphere(std::vector<OverlayVertex>& out, const LineProjector& proj,
                const Vec3& c, float r, float halfThick, const Color4& col) {
    constexpr int kSegs = 20;
    float cs[kSegs + 1], sn[kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kSegs);
        cs[i] = std::cos(a) * r;
        sn[i] = std::sin(a) * r;
    }
    for (int i = 0; i < kSegs; ++i) {
        proj.addLine3D(out, {c.x + cs[i], c.y + sn[i], c.z}, {c.x + cs[i + 1], c.y + sn[i + 1], c.z}, halfThick, col);
        proj.addLine3D(out, {c.x + cs[i], c.y, c.z + sn[i]}, {c.x + cs[i + 1], c.y, c.z + sn[i + 1]}, halfThick, col);
        proj.addLine3D(out, {c.x, c.y + cs[i], c.z + sn[i]}, {c.x, c.y + cs[i + 1], c.z + sn[i + 1]}, halfThick, col);
    }
}

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

void EmitCapsuleOrCylinder(std::vector<OverlayVertex>& out, const LineProjector& proj,
                           const Vec3& p0, const Vec3& p1, float r, bool hemisphereCaps,
                           float halfThick, const Color4& col) {
    const float dx = p1.x - p0.x, dy = p1.y - p0.y, dz = p1.z - p0.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.5f) {
        EmitSphere(out, proj, p0, r, halfThick, col);
        return;
    }
    const Vec3 axis{dx / len, dy / len, dz / len};
    Vec3 u{}, v{};
    MakeOrthonormalBasis(axis, u, v);

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

    for (int i = 0; i < kSegs; ++i) {
        proj.addLine3D(out, ring0[i], ring0[i + 1], halfThick, col);
        proj.addLine3D(out, ring1[i], ring1[i + 1], halfThick, col);
    }
    for (int i = 0; i < kSegs; i += (kSegs / 4)) {
        proj.addLine3D(out, ring0[i], ring1[i], halfThick, col);
    }

    if (hemisphereCaps) {
        constexpr int kCapSegs = 10;
        for (int i = 0; i < kCapSegs; ++i) {
            const float a0 = (kPi * static_cast<float>(i)) / static_cast<float>(kCapSegs);
            const float a1 = (kPi * static_cast<float>(i + 1)) / static_cast<float>(kCapSegs);
            const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r;
            const float c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;

            proj.addLine3D(out,
                {p0.x + u.x * c0 - axis.x * s0, p0.y + u.y * c0 - axis.y * s0, p0.z + u.z * c0 - axis.z * s0},
                {p0.x + u.x * c1 - axis.x * s1, p0.y + u.y * c1 - axis.y * s1, p0.z + u.z * c1 - axis.z * s1},
                halfThick, col);
            proj.addLine3D(out,
                {p0.x + v.x * c0 - axis.x * s0, p0.y + v.y * c0 - axis.y * s0, p0.z + v.z * c0 - axis.z * s0},
                {p0.x + v.x * c1 - axis.x * s1, p0.y + v.y * c1 - axis.y * s1, p0.z + v.z * c1 - axis.z * s1},
                halfThick, col);

            proj.addLine3D(out,
                {p1.x + u.x * c0 + axis.x * s0, p1.y + u.y * c0 + axis.y * s0, p1.z + u.z * c0 + axis.z * s0},
                {p1.x + u.x * c1 + axis.x * s1, p1.y + u.y * c1 + axis.y * s1, p1.z + u.z * c1 + axis.z * s1},
                halfThick, col);
            proj.addLine3D(out,
                {p1.x + v.x * c0 + axis.x * s0, p1.y + v.y * c0 + axis.y * s0, p1.z + v.z * c0 + axis.z * s0},
                {p1.x + v.x * c1 + axis.x * s1, p1.y + v.y * c1 + axis.y * s1, p1.z + v.z * c1 + axis.z * s1},
                halfThick, col);
        }
    }
}

void EmitBoxCorners(std::vector<OverlayVertex>& out, const LineProjector& proj,
                    const Vec3 c[8], float halfThick, const Color4& col) {
    static constexpr int kEdges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},
        {4,5},{5,7},{7,6},{6,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (const auto& e : kEdges) {
        proj.addLine3D(out, c[e[0]], c[e[1]], halfThick, col);
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
        case '!': { static const std::uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return g; }
        default:  return nullptr;
    }
}

void DrawText5x7(std::vector<OverlayVertex>& out,
                 const LineProjector& proj,
                 float x, float y, float pxScale,
                 const char* text, const Color4& col) {
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
                                        col);
                        rx = runEnd;
                    }
                }
            }
        }
        curX += 6.0f * pxScale;
    }
}

} // namespace

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
                       FrameStats& outStats) {
    outVerts.clear();
    outStats = FrameStats{};

    const bool anyFreeze = (freezeMode != freeze::Mode::None);
    if (!exeBase || screenW < 64.0f || screenH < 64.0f) return;
    if (!visEnabled && !anyFreeze && !showStartupToast) return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeBase + dos->e_lfanew);
    const std::uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    RawSnapshot snap{};
    CollectSnapshotSEH(exeBase, exeEnd, visEnabled, filterMode, &snap);
    outStats.hasCamera   = snap.hasCamera;
    outStats.totalShapes = snap.shapeCount;

    ColorSpaceEncoder csEnc;
    csEnc.init(backBufferDxgiFormat, snap.hdrActive, snap.hdrWhiteNits, cfg.hdrNits);

    const float resScale = screenH / 1080.0f;
    const float baseHalfThick = 0.5f * cfg.lineThickness * ((resScale > 0.75f) ? resScale : 0.75f);
    const float alphaMul = cfg.opacityScale;

    LineProjector proj;
    if (snap.hasCamera) {
        proj.init(snap.view, snap.proj, screenW, screenH);
    } else {
        float ident[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        proj.init(ident, ident, screenW, screenH);
    }

    auto clampA = [&](float a) -> float {
        const float v = a * alphaMul;
        return (v > 1.0f) ? 1.0f : ((v < 0.05f) ? 0.05f : v);
    };

    if (visEnabled && snap.hasCamera && snap.shapeCount > 0) {
        outVerts.reserve(static_cast<std::size_t>(snap.shapeCount) * 240);

        // Pre-encode palette once per frame (avoids per-vertex powf in HDR PQ mode)
        float htMonsterAtk  = baseHalfThick * 1.45f;
        float htPlayerAtk   = baseHalfThick * 1.35f;
        float htMonsterHurt = baseHalfThick * 0.90f;
        float htOtherPush   = baseHalfThick * 0.75f;
        const Color4 colMonsterAtk  = csEnc.encode({1.00f, 0.16f, 0.18f, clampA(0.95f)});
        const Color4 colPlayerAtk   = csEnc.encode({1.00f, 0.86f, 0.10f, clampA(0.95f)});
        const Color4 colMonsterHurt = csEnc.encode({0.15f, 0.95f, 0.38f, clampA(0.72f)});
        const Color4 colOtherPush   = csEnc.encode({0.72f, 0.56f, 1.00f, clampA(0.42f)});

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
                switch (wantCat) {
                    case VolumeCat::MonsterHitbox:
                        col = colMonsterAtk; halfThick = htMonsterAtk; break;
                    case VolumeCat::PlayerHitbox:
                        col = colPlayerAtk; halfThick = htPlayerAtk; break;
                    case VolumeCat::MonsterHurtbox:
                        col = colMonsterHurt; halfThick = htMonsterHurt; break;
                    case VolumeCat::PlayerHurtbox: {
                        const int di = (static_cast<int>(s.plDef) >= 0 && static_cast<int>(s.plDef) < 7)
                                           ? static_cast<int>(s.plDef) : 0;
                        col = colPlDef[di];
                        halfThick = htPlDef[di];
                        break;
                    }
                    default:
                        col = colOtherPush; halfThick = htOtherPush; break;
                }

                if (s.type == mhw::kGeomSphere) {
                    EmitSphere(outVerts, proj, {s.p[0], s.p[1], s.p[2]}, s.p[3], halfThick, col);
                } else if (s.type == mhw::kGeomCapsule || s.type == mhw::kGeomLineSweptSphere) {
                    EmitCapsuleOrCylinder(outVerts, proj,
                                          {s.p[0], s.p[1], s.p[2]},
                                          {s.p[3], s.p[4], s.p[5]},
                                          s.p[6], true, halfThick, col);
                } else if (s.type == mhw::kGeomCylinder) {
                    EmitCapsuleOrCylinder(outVerts, proj,
                                          {s.p[0], s.p[1], s.p[2]},
                                          {s.p[3], s.p[4], s.p[5]},
                                          s.p[6], false, halfThick, col);
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
                    EmitBoxCorners(outVerts, proj, corners, halfThick, col);
                } else if (s.type == mhw::kGeomAABB) {
                    const float x0 = s.p[0], y0 = s.p[1], z0 = s.p[2];
                    const float x1 = s.p[3], y1 = s.p[4], z1 = s.p[5];
                    const Vec3 corners[8] = {
                        {x0,y0,z0}, {x1,y0,z0}, {x0,y1,z0}, {x1,y1,z0},
                        {x0,y0,z1}, {x1,y0,z1}, {x0,y1,z1}, {x1,y1,z1}
                    };
                    EmitBoxCorners(outVerts, proj, corners, halfThick, col);
                } else if (s.type == mhw::kGeomTriangle) {
                    const Vec3 v0{s.p[0], s.p[1], s.p[2]};
                    const Vec3 v1{s.p[3], s.p[4], s.p[5]};
                    const Vec3 v2{s.p[6], s.p[7], s.p[8]};
                    proj.addLine3D(outVerts, v0, v1, halfThick, col);
                    proj.addLine3D(outVerts, v1, v2, halfThick, col);
                    proj.addLine3D(outVerts, v2, v0, halfThick, col);
                } else if (s.type == mhw::kGeomLineSegment) {
                    proj.addLine3D(outVerts,
                                   {s.p[0], s.p[1], s.p[2]},
                                   {s.p[3], s.p[4], s.p[5]},
                                   halfThick, col);
                }
            }
        }
    }

    // 2D Top-Left Status HUD Banner
    if (cfg.showHudBanner && (visEnabled || anyFreeze || showStartupToast)) {
        const float px = (resScale >= 0.8f) ? std::floor(resScale * 2.0f + 0.5f) : 2.0f;
        const float x0 = 18.0f * resScale;
        const float y0 = 18.0f * resScale;

        const char* freezeStr = "OFF";
        if (freezeMode == freeze::Mode::Full) {
            freezeStr = "ALL ([O] STEP)";
        } else if (freezeMode == freeze::Mode::World) {
            freezeStr = freeze::IsKeepHitboxesActive()
                            ? "WORLD ([O] STEP [J] KEEP:ON)"
                            : "WORLD ([O] STEP [J] KEEP:OFF)";
        }

        char line1[192]{};
        char stateSeg[48]{};
        if (!visEnabled && !anyFreeze && showStartupToast) {
            std::snprintf(line1, sizeof(line1),
                          "DEBUG HITBOX READY  [U] VIS  [I] FREEZE  [K] WORLD  [J] KEEP  [O] STEP  [P] MODE");
        } else {
            std::snprintf(line1, sizeof(line1),
                          "VIS:%s [%s]  PL:%u/%u EM:%u/%u  |  FREEZE:%s",
                          visEnabled ? "ON" : "OFF",
                          app::FilterModeName(filterMode),
                          outStats.playerHurtbox, outStats.playerHitboxes,
                          outStats.monsterHurtbox, outStats.monsterHitbox,
                          freezeStr);
            if (snap.mainPlDef.valid) {
                std::snprintf(stateSeg, sizeof(stateSeg), "  |  FRAME:%s", snap.mainPlDef.badge);
            }
        }

        const std::size_t len1 = std::strlen(line1);
        const std::size_t len2 = std::strlen(stateSeg);
        const float textW1 = static_cast<float>(len1 + len2) * 6.0f * px;
        const float minLegendW = visEnabled ? (56.0f * 6.0f * 0.85f * px) : 0.0f;
        const float boxInnerW = (textW1 > minLegendW) ? textW1 : minLegendW;
        const float padX  = 8.0f * resScale;
        const float padY  = 6.0f * resScale;
        const float barH  = 7.0f * px + padY * 2.0f + (visEnabled ? (10.0f * px) : 0.0f);

        Color4 accentSrgb = {0.15f, 0.90f, 0.65f, 0.95f};
        if (freezeMode == freeze::Mode::Full)       accentSrgb = {1.00f, 0.30f, 0.20f, 0.95f};
        else if (freezeMode == freeze::Mode::World) accentSrgb = {1.00f, 0.68f, 0.14f, 0.95f};

        proj.emitRect2D(outVerts, x0, y0, x0 + boxInnerW + padX * 2.0f, y0 + barH,
                        csEnc.encode({0.05f, 0.06f, 0.09f, 0.80f}));
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

        if (visEnabled) {
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
            drawChip("SUPER",   {0.95f, 0.34f, 0.90f, 0.95f});
            drawChip("GP/GRD",  {0.42f, 0.52f, 1.00f, 0.95f});
            drawChip("PL-ATK",  {1.00f, 0.86f, 0.10f, 0.95f});
            drawChip("EM-HURT", {0.15f, 0.95f, 0.38f, 0.95f});
            drawChip("EM-ATK",  {1.00f, 0.16f, 0.18f, 0.95f});
        }
    }
}

} // namespace coll

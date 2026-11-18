#include "game_freeze.h"
#include "hook.h"
#include "mhw_addresses.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace freeze {
namespace {

using UnitGenericMoveFn = void(__fastcall*)(std::uintptr_t);
using CpObjCollRegisterFn = void(__fastcall*)(std::uintptr_t);
using CpObjCollMoveFn = void(__fastcall*)(std::uintptr_t);
using CCacheClearFn = void(__fastcall*)(std::uintptr_t);
using GetCritSecObjFn = LPCRITICAL_SECTION(__fastcall*)();
using UnitHandleSetFn = void(__fastcall*)(LPCRITICAL_SECTION, std::uintptr_t, std::uintptr_t);
using GetMainPlayerFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
using HitDispatchFn = int(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
using GuardDispatchFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
using UnitOnEventFn = void(__fastcall*)(std::uintptr_t, void*);

std::uintptr_t g_exeBase = 0;
std::uintptr_t g_exeEnd  = 0;
UnitGenericMoveFn g_origUnitGenericMove = nullptr;
HitDispatchFn     g_origHitDispatch     = nullptr;
GuardDispatchFn   g_origGuardDispatch   = nullptr;

std::atomic<bool> g_frozen{false};
std::atomic<bool> g_worldFrozen{false};
std::atomic<bool> g_keepHitboxesActive{true};
std::atomic<bool> g_stepCameraGuard{false};
std::atomic<bool> g_suppressKbActive{false};
std::atomic<std::uint8_t> g_unitMoveHookFlags{0};
std::atomic<int>  g_worldStepReq{0};
std::atomic<bool> g_worldStepActive{false};
std::atomic<std::uint32_t> g_worldSettledTicks{0};
int g_suppressKbFramesLeft = 0;
std::atomic<std::uintptr_t> g_cachedMainPlayer{0};
std::atomic<std::uintptr_t> g_cachedActiveCam{0};
bool g_prevPlActionDef = false;

constexpr std::uint8_t kMoveFlagWorldFreeze  = 0x01U;
constexpr std::uint8_t kMoveFlagStepCamGuard = 0x02U;
constexpr std::uint8_t kMoveFlagSuppressKb   = 0x04U;

inline void RefreshUnitMoveFlags() {
    std::uint8_t f = 0;
    const bool worldFrz = g_worldFrozen.load(std::memory_order_relaxed) &&
                          !g_frozen.load(std::memory_order_relaxed);
    if (worldFrz)
        f |= kMoveFlagWorldFreeze;
    if (g_stepCameraGuard.load(std::memory_order_relaxed))
        f |= kMoveFlagStepCamGuard;
    if (g_suppressKbActive.load(std::memory_order_relaxed))
        f |= kMoveFlagSuppressKb;
    g_unitMoveHookFlags.store(f, std::memory_order_release);

    const bool needHitKeep = worldFrz && g_keepHitboxesActive.load(std::memory_order_relaxed);
    if (needHitKeep) {
        hook::EnsureLazyGuardHookInstalled();
    }
    hook::SetDynamicHook5Enabled(hook::kDynHitDispatch, needHitKeep);
    hook::SetDynamicHook5Enabled(hook::kDynGenericMove, f != 0);
}

// Rolling live camera angle snapshot to undo any 1-frame hotkey nudge on freeze entry
struct CamAngles {
    std::uintptr_t cam = 0;
    float pitch = 0.0f;
    float yaw   = 0.0f;
    bool valid  = false;
};
CamAngles g_camHistory[4]{};
std::uint32_t g_camHistIdx = 0;

// Lock-free MtDTI -> WorldFreeze policy cache (safe across concurrent sUnit worker threads)
enum class DtiPolicy : std::uint8_t {
    Unknown           = 0,
    Allow             = 1,
    Freeze            = 2,
    CheckShellCreator = 3,
    CheckEffectOwner  = 4
};

struct AtomicDtiSlot {
    std::atomic<std::uintptr_t> dti{0};
    std::atomic<std::uint8_t> policy{0};
};
constexpr std::uint32_t kDtiPolicySlots = 1024;
AtomicDtiSlot g_dtiPolicyCache[kDtiPolicySlots]{};

// Inverted VFX freeze set: stores ONLY pre-existing world/monster uMhEfc units (and stepped monster effects).
// Any uMhEfc spawned while monsters are frozen (Player attacks, Empowered TCS flash/blur, hitsparks, mantles)
// is NEVER frozen and expires naturally — zero accumulation even across 100,000 hits!
constexpr std::uint32_t kWorldEfcSlots = 4096;
std::atomic<std::uintptr_t> g_frozenWorldEffects[kWorldEfcSlots]{};

// Pinned active attack cNodes on frozen units when KeepHitboxesActive [J] is enabled
constexpr std::uint32_t kPinnedNodeSlots = 1024;
std::atomic<std::uintptr_t> g_pinnedActiveNodes[kPinnedNodeSlots]{};

inline bool IsCanonicalPtr(std::uintptr_t p) {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL;
}

inline bool IsAlignedPtr(std::uintptr_t p) {
    return IsCanonicalPtr(p) && ((p & 0x7ULL) == 0);
}

inline bool StartsWith(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

void ClearWorldEffectsTable() {
    for (std::uint32_t i = 0; i < kWorldEfcSlots; ++i) {
        g_frozenWorldEffects[i].store(0, std::memory_order_relaxed);
    }
}

void InsertFrozenWorldEffect(std::uintptr_t unit) {
    if (!unit) return;
    std::uint32_t idx = static_cast<std::uint32_t>((unit >> 4) * 2654435761ULL) & (kWorldEfcSlots - 1);
    std::int32_t firstTomb = -1;
    for (std::uint32_t n = 0; n < 32; ++n) {
        const std::uintptr_t cur = g_frozenWorldEffects[idx].load(std::memory_order_relaxed);
        if (cur == unit) return;
        if (cur == 1ULL && firstTomb < 0) firstTomb = static_cast<std::int32_t>(idx);
        if (cur == 0) {
            const std::uint32_t target = (firstTomb >= 0) ? static_cast<std::uint32_t>(firstTomb) : idx;
            std::uintptr_t expected = (firstTomb >= 0) ? 1ULL : 0ULL;
            if (g_frozenWorldEffects[target].compare_exchange_strong(
                    expected, unit, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return;
            }
            if (expected == unit) return;
            firstTomb = -1;
        }
        idx = (idx + 1) & (kWorldEfcSlots - 1);
    }
    if (firstTomb >= 0) {
        g_frozenWorldEffects[firstTomb].store(unit, std::memory_order_relaxed);
    }
}

void RemoveFrozenWorldEffect(std::uintptr_t unit) {
    if (!unit) return;
    std::uint32_t idx = static_cast<std::uint32_t>((unit >> 4) * 2654435761ULL) & (kWorldEfcSlots - 1);
    for (std::uint32_t n = 0; n < 32; ++n) {
        const std::uintptr_t cur = g_frozenWorldEffects[idx].load(std::memory_order_relaxed);
        if (cur == unit) {
            g_frozenWorldEffects[idx].store(1ULL, std::memory_order_relaxed);
            return;
        }
        if (cur == 0) return;
        idx = (idx + 1) & (kWorldEfcSlots - 1);
    }
}

bool IsFrozenWorldEffect(std::uintptr_t unit) {
    if (!unit) return false;
    std::uint32_t idx = static_cast<std::uint32_t>((unit >> 4) * 2654435761ULL) & (kWorldEfcSlots - 1);
    for (std::uint32_t n = 0; n < 32; ++n) {
        const std::uintptr_t cur = g_frozenWorldEffects[idx].load(std::memory_order_relaxed);
        if (cur == unit) return true;
        if (cur == 0) return false;
        idx = (idx + 1) & (kWorldEfcSlots - 1);
    }
    return false;
}

void ClearPinnedNodesTable() {
    for (std::uint32_t i = 0; i < kPinnedNodeSlots; ++i) {
        g_pinnedActiveNodes[i].store(0, std::memory_order_relaxed);
    }
}

void InsertPinnedNode(std::uintptr_t cnode) {
    if (!cnode) return;
    std::uint32_t idx = static_cast<std::uint32_t>((cnode >> 4) * 2654435761ULL) & (kPinnedNodeSlots - 1);
    for (std::uint32_t n = 0; n < 24; ++n) {
        const std::uintptr_t cur = g_pinnedActiveNodes[idx].load(std::memory_order_relaxed);
        if (cur == cnode) return;
        if (cur == 0) {
            std::uintptr_t expected = 0;
            if (g_pinnedActiveNodes[idx].compare_exchange_weak(
                    expected, cnode, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return;
            }
            if (expected == cnode) return;
        }
        idx = (idx + 1) & (kPinnedNodeSlots - 1);
    }
}

bool IsPinnedNode(std::uintptr_t cnode) {
    if (!cnode) return false;
    std::uint32_t idx = static_cast<std::uint32_t>((cnode >> 4) * 2654435761ULL) & (kPinnedNodeSlots - 1);
    for (std::uint32_t n = 0; n < 24; ++n) {
        const std::uintptr_t cur = g_pinnedActiveNodes[idx].load(std::memory_order_relaxed);
        if (cur == cnode) return true;
        if (cur == 0) return false;
        idx = (idx + 1) & (kPinnedNodeSlots - 1);
    }
    return false;
}

std::uintptr_t GetObjectDtiFast(std::uintptr_t obj, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(obj)) return 0;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(obj);
    if (vft < exeBase || vft + 0x28 > exeEnd || (vft & 0x7ULL) != 0) return 0;
    const std::uintptr_t fnGetDti = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffVftGetDti);
    if (fnGetDti < exeBase || fnGetDti + 8 > exeEnd) return 0;
    const auto* code = reinterpret_cast<const std::uint8_t*>(fnGetDti);
    if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05 && code[7] == 0xC3) {
        std::int32_t disp = 0;
        std::memcpy(&disp, code + 3, 4);
        const std::uintptr_t dti = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(fnGetDti + 7) + disp);
        if (dti >= exeBase && dti + 0x38 <= exeEnd) return dti;
    }
    return 0;
}

inline bool IsCpObjCollisionFast(std::uintptr_t cand, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(cand)) return false;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(cand);
    if (vft < exeBase || vft + 0x28 > exeEnd || (vft & 0x7ULL) != 0) return false;
    const std::uintptr_t fnGetDti = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffVftGetDti);
    return fnGetDti == (exeBase + mhw::kRvaCpObjCollisionGetDti);
}

DtiPolicy ComputeDtiPolicy(std::uintptr_t dti) {
    std::uintptr_t cur = dti;
    for (int depth = 0; depth < 20 && IsAlignedPtr(cur); ++depth) {
        const char* name = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
        if (name && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name))) {
            const unsigned char c0 = static_cast<unsigned char>(name[0]);
            if (c0 >= 0x20 && c0 <= 0x7E) {
                // Player & Player Weapon / Kinsect / Claw / Player Shell -> ALLOW
                if (StartsWith(name, "uPlayer") || StartsWith(name, "uPl") ||
                    StartsWith(name, "uWeapon") || StartsWith(name, "uWp") ||
                    StartsWith(name, "uInsect") || StartsWith(name, "uHumanClaw") ||
                    StartsWith(name, "uClaw")   || StartsWith(name, "uRope") ||
                    StartsWith(name, "uShellPl") || StartsWith(name, "uAccessory") ||
                    StartsWith(name, "uArmor")) {
                    return DtiPolicy::Allow;
                }

                // Monsters, AI, Palico/Otomo, Endemic Life, NPCs, Monster Shells, Stage Gimmicks -> FREEZE
                if (StartsWith(name, "Monster")  || StartsWith(name, "uEnemy") ||
                    StartsWith(name, "uEm")      || StartsWith(name, "cEm") ||
                    StartsWith(name, "uAiEm")    || StartsWith(name, "uOtomo") ||
                    StartsWith(name, "uOt")      || StartsWith(name, "uAnimal") ||
                    StartsWith(name, "uBug")     || StartsWith(name, "uFish") ||
                    StartsWith(name, "uBird")    || StartsWith(name, "uEnvCreature") ||
                    StartsWith(name, "uNpc")     || StartsWith(name, "uShellEm") ||
                    StartsWith(name, "uShellOt") || StartsWith(name, "uShellStage") ||
                    StartsWith(name, "uGm")      || StartsWith(name, "uStageGimmick") ||
                    StartsWith(name, "uHmGimmick")) {
                    return DtiPolicy::Freeze;
                }

                // Generic projectile base (not already matched by uShellPl / uShellEm)
                if (StartsWith(name, "uShell")) {
                    return DtiPolicy::CheckShellCreator;
                }

                // VFX / Breath / Laser / Slash particle units
                if (StartsWith(name, "uMhEfc") || StartsWith(name, "uEfc") || StartsWith(name, "uEffect")) {
                    return DtiPolicy::CheckEffectOwner;
                }

                // Non-player uHuman (since uPlayer was already matched earlier in the chain)
                if (StartsWith(name, "uHuman")) {
                    return DtiPolicy::Freeze;
                }
            }
        }
        cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
    }
    // System, Camera, GUI, Sound, Lighting, Stage Manager -> Allow
    return DtiPolicy::Allow;
}

DtiPolicy LookupDtiPolicy(std::uintptr_t dti) {
    if (!dti) return DtiPolicy::Allow;
    std::uint32_t idx = static_cast<std::uint32_t>((dti >> 4) * 2654435761ULL) & (kDtiPolicySlots - 1);
    for (std::uint32_t n = 0; n < 16; ++n) {
        const std::uintptr_t curDti = g_dtiPolicyCache[idx].dti.load(std::memory_order_acquire);
        if (curDti == dti) {
            const auto pol = static_cast<DtiPolicy>(g_dtiPolicyCache[idx].policy.load(std::memory_order_relaxed));
            if (pol != DtiPolicy::Unknown) return pol;
        }
        if (curDti == 0) {
            const DtiPolicy computed = ComputeDtiPolicy(dti);
            g_dtiPolicyCache[idx].policy.store(static_cast<std::uint8_t>(computed), std::memory_order_relaxed);
            std::uintptr_t expected = 0;
            g_dtiPolicyCache[idx].dti.compare_exchange_strong(
                expected, dti, std::memory_order_release, std::memory_order_relaxed);
            return computed;
        }
        idx = (idx + 1) & (kDtiPolicySlots - 1);
    }
    return ComputeDtiPolicy(dti);
}

bool IsPlayerOwnedCreator(std::uintptr_t creator, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    if (!IsAlignedPtr(creator)) return false;
    const std::uint64_t grp = *reinterpret_cast<const std::uint64_t*>(creator + mhw::kOffUnitGroup);
    const std::uintptr_t dti = GetObjectDtiFast(creator, exeBase, exeEnd);
    const DtiPolicy pol = LookupDtiPolicy(dti);
    if (pol == DtiPolicy::Freeze) return false;
    if ((grp & 0x200000000000ULL) != 0) return true;

    std::uintptr_t cur = dti;
    for (int depth = 0; depth < 16 && IsAlignedPtr(cur); ++depth) {
        const char* name = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
        if (name && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(name))) {
            if (StartsWith(name, "uPlayer") || StartsWith(name, "uPl") ||
                StartsWith(name, "uWeapon") || StartsWith(name, "uWp") ||
                StartsWith(name, "uInsect") || StartsWith(name, "uShellPl") ||
                StartsWith(name, "uHumanClaw") || StartsWith(name, "uClaw")) {
                return true;
            }
        }
        cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
    }
    return false;
}

int CheckUnitCollisionHitGroup(std::uintptr_t unit, std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    std::uintptr_t comp = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
    for (int guard = 0; guard < 32 && IsAlignedPtr(comp); ++guard) {
        if (IsCpObjCollisionFast(comp, exeBase, exeEnd)) {
            const std::int32_t hg = *reinterpret_cast<const std::int32_t*>(comp + mhw::kOffObjCollHitGroup);
            if (hg != 0) return hg;
        }
        comp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
    }
    return 0;
}

std::uintptr_t GetMainPlayerUnit(std::uintptr_t exeBase) {
    if (!exeBase) return 0;
    const std::uintptr_t sPlayer = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSPlayerInstance);
    if (!IsAlignedPtr(sPlayer)) return 0;
    const auto getMainPl = reinterpret_cast<GetMainPlayerFn>(exeBase + mhw::kRvaGetMainPlayer);
    const std::uintptr_t pl = getMainPl(sPlayer);
    return IsAlignedPtr(pl) ? pl : 0;
}

std::uintptr_t GetActiveCameraUnit(std::uintptr_t exeBase) {
    if (!exeBase) return 0;
    const std::uintptr_t mhMain = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhMainInstance);
    if (!IsAlignedPtr(mhMain)) return 0;
    const std::uintptr_t camSys = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCamera);
    if (!IsAlignedPtr(camSys)) return 0;
    for (int vpIdx = 0; vpIdx < 8; ++vpIdx) {
        const std::uintptr_t vp = camSys + static_cast<std::size_t>(vpIdx) * mhw::kCameraViewportStride;
        const std::uint8_t visible = *reinterpret_cast<const std::uint8_t*>(vp + mhw::kOffVpVisible);
        const std::uintptr_t camObj = *reinterpret_cast<const std::uintptr_t*>(vp + mhw::kOffVpCameraPtr);
        if (visible != 0 && IsAlignedPtr(camObj)) return camObj;
    }
    return 0;
}

void ClearKeyboardCamInput(std::uintptr_t exeBase) {
    const std::uintptr_t sKb = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSKeyboardInstance);
    if (IsAlignedPtr(sKb)) {
        *reinterpret_cast<volatile float*>(sKb + mhw::kOffKbCamYaw)   = 0.0f;
        *reinterpret_cast<volatile float*>(sKb + mhw::kOffKbCamPitch) = 0.0f;
    }
}

void FlushAllCameraInputDeltas(std::uintptr_t exeBase, bool snapCameraTarget) {
    if (!exeBase) return;

    const std::uintptr_t sMouse = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMouseInstance);
    if (IsAlignedPtr(sMouse)) {
        *reinterpret_cast<volatile std::uint8_t*>(sMouse + mhw::kOffSMouseDiscardDelta) = 1;
    }
    const std::uintptr_t sMhMouse = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhMouseInstance);
    if (IsAlignedPtr(sMhMouse)) {
        *reinterpret_cast<volatile std::int32_t*>(sMhMouse + mhw::kOffSMhMouseDeltaX) = 0;
        *reinterpret_cast<volatile std::int32_t*>(sMhMouse + mhw::kOffSMhMouseDeltaY) = 0;
        *reinterpret_cast<volatile std::int32_t*>(sMhMouse + mhw::kOffSMhMouseAccumX) = 0;
        *reinterpret_cast<volatile std::int32_t*>(sMhMouse + mhw::kOffSMhMouseAccumY) = 0;
    }

    ClearKeyboardCamInput(exeBase);

    const std::uintptr_t mainPl = GetMainPlayerUnit(exeBase);
    if (IsAlignedPtr(mainPl)) {
        const std::uintptr_t padCtrl = *reinterpret_cast<const std::uintptr_t*>(mainPl + mhw::kOffPlPadCtrl);
        if (IsAlignedPtr(padCtrl)) {
            const std::uintptr_t hmCtrl = padCtrl + mhw::kOffHmCtrlSub;
            *reinterpret_cast<volatile float*>(hmCtrl + mhw::kOffHmCtrlMouseSmoothX) = 0.0f;
            *reinterpret_cast<volatile float*>(hmCtrl + mhw::kOffHmCtrlMouseSmoothY) = 0.0f;
        }
    }

    const std::uintptr_t cam = GetActiveCameraUnit(exeBase);
    if (IsAlignedPtr(cam)) {
        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInputPitch) = 0.0f;
        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInputYaw)   = 0.0f;
        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamPadPitch)   = 0.0f;
        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamPadYaw)     = 0.0f;

        if (snapCameraTarget) {
            const float curPitch = *reinterpret_cast<const float*>(cam + mhw::kOffCamPitch);
            const float curYaw   = *reinterpret_cast<const float*>(cam + mhw::kOffCamYaw);
            if (std::isfinite(curPitch) && std::isfinite(curYaw)) {
                *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInterpPitch) = curPitch;
                *reinterpret_cast<volatile float*>(cam + mhw::kOffCamTargetPitch) = curPitch;
                *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInterpYaw)   = curYaw;
                *reinterpret_cast<volatile float*>(cam + mhw::kOffCamTargetYaw)   = curYaw;
            }
        }
    }
}

void RunGuardedCameraStepMove(std::uintptr_t exeBase, std::uintptr_t cam) {
    __try {
        FlushAllCameraInputDeltas(exeBase, true);

        const float savedPitch = *reinterpret_cast<const float*>(cam + mhw::kOffCamPitch);
        const float savedYaw   = *reinterpret_cast<const float*>(cam + mhw::kOffCamYaw);
        const std::uint8_t savedAutoPitch = *reinterpret_cast<const std::uint8_t*>(cam + mhw::kOffCamAutoTerrainPitch);
        *reinterpret_cast<volatile std::uint8_t*>(cam + mhw::kOffCamAutoTerrainPitch) = 0;

        const std::uintptr_t mainPl = GetMainPlayerUnit(exeBase);
        std::uint32_t savedPlMask = 0;
        if (IsAlignedPtr(mainPl)) {
            savedPlMask = *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlCamInputMask);
            *reinterpret_cast<volatile std::uint32_t*>(mainPl + mhw::kOffPlCamInputMask) = savedPlMask | 0x100U;
        }

        g_origUnitGenericMove(cam);

        if (IsAlignedPtr(mainPl)) {
            *reinterpret_cast<volatile std::uint32_t*>(mainPl + mhw::kOffPlCamInputMask) = savedPlMask;
        }
        *reinterpret_cast<volatile std::uint8_t*>(cam + mhw::kOffCamAutoTerrainPitch) = savedAutoPitch;

        if (std::isfinite(savedPitch) && std::isfinite(savedYaw)) {
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamPitch)       = savedPitch;
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInterpPitch) = savedPitch;
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamTargetPitch) = savedPitch;
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamYaw)         = savedYaw;
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamInterpYaw)   = savedYaw;
            *reinterpret_cast<volatile float*>(cam + mhw::kOffCamTargetYaw)   = savedYaw;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_origUnitGenericMove) g_origUnitGenericMove(cam);
    }
}

bool ShouldFreezeUnitSEH(std::uintptr_t exeBase, std::uintptr_t exeEnd, std::uintptr_t unit, bool stepActive);

// Advances cpObjCollision timers/caches on a frozen unit (fixing 32-slot cCache 0.0f eviction lockup!)
// and optionally keeps single-trigger attack nodes active while WorldFreeze + KeepHitboxesActive is ON.
void ProcessFrozenObjCollision(std::uintptr_t exeBase, std::uintptr_t comp, bool keepSingleHitActive) {
    const auto fnMove        = reinterpret_cast<CpObjCollMoveFn>(exeBase + mhw::kRvaCpObjCollMove);
    const auto fnRegister    = reinterpret_cast<CpObjCollRegisterFn>(exeBase + mhw::kRvaCpObjCollRegisterNodes);
    const auto fnCacheClear  = reinterpret_cast<CCacheClearFn>(exeBase + mhw::kRvaCCacheClear);
    const auto fnGetCritSec  = reinterpret_cast<GetCritSecObjFn>(exeBase + mhw::kRvaGetCritSecObj);
    const auto fnHandleSet   = reinterpret_cast<UnitHandleSetFn>(exeBase + mhw::kRvaUnitHandleSet);

    if (keepSingleHitActive) {
        // 1a. Pin & restore active attack cNodes (+0xA8 != 0) so single-hit nodes never deactivate
        const std::uint32_t nodeCnt = *reinterpret_cast<const std::uint32_t*>(comp + mhw::kOffObjCollNodeCount);
        const std::uintptr_t nodeArr = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffObjCollNodeArray);
        if (nodeCnt > 0 && nodeCnt <= 128 && IsAlignedPtr(nodeArr)) {
            for (std::uint32_t i = 0; i < nodeCnt; ++i) {
                const std::uintptr_t cnode = *reinterpret_cast<const std::uintptr_t*>(nodeArr + i * 8);
                if (!IsAlignedPtr(cnode)) continue;
                const std::uintptr_t atkParam = *reinterpret_cast<const std::uintptr_t*>(cnode + mhw::kOffCNodeAttackParam);
                if (!atkParam) continue;

                const std::uint8_t destroyReq = *reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeDestroyReq);
                const std::uint8_t isActive   = *reinterpret_cast<const std::uint8_t*>(cnode + mhw::kOffCNodeIsActive);
                if (destroyReq == 0 && isActive != 0) {
                    InsertPinnedNode(cnode);
                } else if (IsPinnedNode(cnode)) {
                    *reinterpret_cast<volatile std::uint8_t*>(cnode + mhw::kOffCNodeDestroyReq) = 0;
                    *reinterpret_cast<volatile std::uint8_t*>(cnode + mhw::kOffCNodeIsActive)   = 1;
                }
            }
        }

        // 1b. Clear already-hit target handle list (+0x760, count at +0x828) using native cUnitHandle::set
        const std::uint64_t hitUnitCnt = *reinterpret_cast<const std::uint64_t*>(comp + mhw::kOffObjCollHitUnitCount);
        if (hitUnitCnt > 0 && hitUnitCnt <= 8) {
            LPCRITICAL_SECTION cs = fnGetCritSec();
            if (cs) {
                for (std::uint64_t i = 0; i < hitUnitCnt; ++i) {
                    const std::uintptr_t hAddr = comp + mhw::kOffObjCollHitUnits + 0x08 + i * 0x18;
                    if (*reinterpret_cast<const std::uintptr_t*>(hAddr) != 0) {
                        fnHandleSet(cs, 0, hAddr);
                    }
                }
                *reinterpret_cast<volatile std::uint64_t*>(comp + mhw::kOffObjCollHitUnitCount) = 0;
            }
        }

        // 1c. Clear 16-slot per-target hit-count cap table (+0x5400)
        for (std::size_t s = 0; s < 16; ++s) {
            const std::uintptr_t slot = comp + mhw::kOffObjCollHitHist + s * 0x20;
            if (*reinterpret_cast<const std::uintptr_t*>(slot) != 0) {
                *reinterpret_cast<volatile std::uintptr_t*>(slot + 0x00) = 0;
                *reinterpret_cast<volatile std::uintptr_t*>(slot + 0x08) = 0;
                *reinterpret_cast<volatile std::int32_t*>(slot + 0x10)   = 0;
            }
        }
    }

    // 2. Run cpObjCollision::move(comp) while preserving frozen unit's per-frame state masks (+0x190..+0x19C).
    //    This advances all 32 cCache mAge (+0x58) & mTimer (+0x54) fields, runs cCache::clear on expired
    //    multi-tick attacks, clears +0x504C timers, and frees the per-frame contact MtArray at comp[0x31].
    const std::uint32_t f190 = *reinterpret_cast<const std::uint32_t*>(comp + mhw::kOffObjCollFlags190);
    const std::uint32_t f194 = *reinterpret_cast<const std::uint32_t*>(comp + mhw::kOffObjCollFlags194);
    const std::uint32_t f198 = *reinterpret_cast<const std::uint32_t*>(comp + mhw::kOffObjCollStateFlags);
    const std::uint32_t f19C = *reinterpret_cast<const std::uint32_t*>(comp + mhw::kOffObjCollFlags19C);

    fnMove(comp);

    *reinterpret_cast<volatile std::uint32_t*>(comp + mhw::kOffObjCollFlags190)   = f190;
    *reinterpret_cast<volatile std::uint32_t*>(comp + mhw::kOffObjCollFlags194)   = f194;
    *reinterpret_cast<volatile std::uint32_t*>(comp + mhw::kOffObjCollStateFlags) = f198;
    *reinterpret_cast<volatile std::uint32_t*>(comp + mhw::kOffObjCollFlags19C)   = f19C;

    // 3. Guarantee positive mAge on every occupied cCache[32] slot AND garbage-collect finished swings (>120f)
    //    so FUN_1412d5100 NEVER hits the 0.0f < 0.0f eviction lockup regardless of hit count or sharpness drop!
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uintptr_t slot = comp + mhw::kOffObjCollCacheBase + i * mhw::kObjCollCacheStride;
        const std::int32_t uid = *reinterpret_cast<const std::int32_t*>(slot + mhw::kOffCacheUid);
        if (uid == 0) continue;

        float age = *reinterpret_cast<const float*>(slot + mhw::kOffCacheAge);
        if (!(age >= 1.0f)) {
            age = 1.0f;
            *reinterpret_cast<volatile float*>(slot + mhw::kOffCacheAge) = age;
        }
        if (age > 120.0f) {
            const std::uintptr_t atkNode = *reinterpret_cast<const std::uintptr_t*>(slot + mhw::kOffCacheAtkNode);
            const bool stillActive = IsAlignedPtr(atkNode) &&
                                     (*reinterpret_cast<const std::uint8_t*>(atkNode + mhw::kOffCNodeIsActive) == 1);
            if (!stillActive) {
                fnCacheClear(slot);
            }
        }
    }

    // 4. Re-register world-space cNodes into sCollision::Collider for the current frame
    fnRegister(comp);
}

void KeepFrozenUnitCollisionsAlive(std::uintptr_t exeBase, std::uintptr_t exeEnd, std::uintptr_t unit) {
    __try {
        const std::uint8_t alive = *reinterpret_cast<const std::uint8_t*>(unit + mhw::kOffUnitAliveFlags);
        if ((alive & 0x0EU) == 0) return;

        const std::uintptr_t sColl = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSCollisionInstance);
        if (!IsAlignedPtr(sColl)) return;

        // Zero unit delta time so cpObjCollision::registerNodes computes zero swept velocity
        *reinterpret_cast<volatile float*>(unit + mhw::kOffUnitDeltaTime) = 0.0f;
        *reinterpret_cast<volatile float*>(unit + mhw::kOffUnitDeltaSec)  = 0.0f;

        const bool keepActive = g_keepHitboxesActive.load(std::memory_order_relaxed);
        int processed = 0;
        std::uintptr_t comp = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
        for (int guard = 0; guard < 64 && IsAlignedPtr(comp); ++guard) {
            const std::uintptr_t nextComp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
            if (IsCpObjCollisionFast(comp, exeBase, exeEnd)) {
                const std::uint8_t flags = *reinterpret_cast<const std::uint8_t*>(comp + mhw::kOffObjCollFlags);
                const std::uintptr_t owner = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffObjCollOwner);
                if ((flags & 0x01U) != 0 && owner == unit) {
                    ProcessFrozenObjCollision(exeBase, comp, keepActive);
                    ++processed;
                }
            }
            comp = nextComp;
        }
        if (processed == 0) {
            const std::uintptr_t primaryColl = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffMonsterCollPrimary);
            if (IsCpObjCollisionFast(primaryColl, exeBase, exeEnd)) {
                const std::uint8_t flags = *reinterpret_cast<const std::uint8_t*>(primaryColl + mhw::kOffObjCollFlags);
                const std::uintptr_t owner = *reinterpret_cast<const std::uintptr_t*>(primaryColl + mhw::kOffObjCollOwner);
                if ((flags & 0x01U) != 0 && owner == unit) {
                    ProcessFrozenObjCollision(exeBase, primaryColl, keepActive);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Runs synchronously on the simulation thread right before mainPlayer ticks in WorldFreeze:
// expires frozen-attacker entries from mainPlayer's cCache[32] (+0x9F0) so frozen hitboxes can re-trigger
// cleanly on repeated rolls / Foresight Slashes / Guard Points without 60Hz multi-hitting.
void RearmPlayerFrozenHitCacheSEH(std::uintptr_t exeBase, std::uintptr_t exeEnd, std::uintptr_t mainPl) {
    __try {
        if (!IsAlignedPtr(mainPl)) return;

        // Detect rising edge of active player dodge / counter / guard / tackle input
        const std::uint32_t defFlags = *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlDefFlags) |
                                       *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlDefFlagsLatch);
        const std::uint32_t grdFlags = *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlGuardFlags) |
                                       *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlGuardFlagsLatch);
        const std::uint32_t cntFlags = *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlCounterFlags) |
                                       *reinterpret_cast<const std::uint32_t*>(mainPl + mhw::kOffPlCounterFlagsLatch);
        const float evadeFrames      = *reinterpret_cast<const float*>(mainPl + mhw::kOffPlEvadeIframeFrames);
        const float noHitSec         = *reinterpret_cast<const float*>(mainPl + mhw::kOffPlNoHitSec);
        const float hyperSec         = *reinterpret_cast<const float*>(mainPl + mhw::kOffPlHyperArmorSec);

        const bool guardActive = ((grdFlags & 0x04U) == 0) && ((grdFlags & 0x18U) != 0);
        const bool actionDefNow =
            ((defFlags & 0x19800000U) != 0) || // NoHit or motion HyperArmor bits (excludes passive mantle)
            ((cntFlags & 0x80000000U) != 0) ||
            guardActive ||
            (std::isfinite(evadeFrames) && evadeFrames > 0.0f) ||
            (std::isfinite(noHitSec) && noHitSec > 0.0f) ||
            (std::isfinite(hyperSec) && hyperSec > 0.0f);

        const bool freshDefAction = actionDefNow && !g_prevPlActionDef;
        g_prevPlActionDef = actionDefNow;

        const std::uintptr_t plColl = *reinterpret_cast<const std::uintptr_t*>(mainPl + mhw::kOffUPlayerColl);
        if (!IsCpObjCollisionFast(plColl, exeBase, exeEnd)) return;

        const auto fnCacheClear = reinterpret_cast<CCacheClearFn>(exeBase + mhw::kRvaCCacheClear);
        for (std::size_t i = 0; i < 32; ++i) {
            const std::uintptr_t slot = plColl + mhw::kOffObjCollCacheBase + i * mhw::kObjCollCacheStride;
            const std::int32_t uid = *reinterpret_cast<const std::int32_t*>(slot + mhw::kOffCacheUid);
            if (uid == 0) continue;

            std::uintptr_t atkUnit = *reinterpret_cast<const std::uintptr_t*>(slot + mhw::kOffCacheAtkUnit);
            if (!IsAlignedPtr(atkUnit)) {
                const std::uintptr_t atkNode = *reinterpret_cast<const std::uintptr_t*>(slot + mhw::kOffCacheAtkNode);
                if (IsAlignedPtr(atkNode)) {
                    const std::uintptr_t atkColl = *reinterpret_cast<const std::uintptr_t*>(atkNode + mhw::kOffCNodeParentColl);
                    if (IsAlignedPtr(atkColl)) {
                        atkUnit = *reinterpret_cast<const std::uintptr_t*>(atkColl + mhw::kOffObjCollOwner);
                    }
                }
            }
            if (!IsAlignedPtr(atkUnit) || !ShouldFreezeUnitSEH(exeBase, exeEnd, atkUnit, false)) continue;

            const float age = *reinterpret_cast<const float*>(slot + mhw::kOffCacheAge);
            if (age >= 45.0f || (freshDefAction && age >= 8.0f)) {
                fnCacheClear(slot);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ShouldFreezeUnitSEH(std::uintptr_t exeBase, std::uintptr_t exeEnd, std::uintptr_t unit, bool stepActive) {
    __try {
        if (!IsAlignedPtr(unit)) return false;
        const std::uintptr_t dti = GetObjectDtiFast(unit, exeBase, exeEnd);
        const DtiPolicy pol = LookupDtiPolicy(dti);

        if (pol == DtiPolicy::Allow) {
            const std::uintptr_t mainPl = g_cachedMainPlayer.load(std::memory_order_relaxed);
            if (IsAlignedPtr(mainPl) && unit != mainPl) {
                const char* clsName = dti ? *reinterpret_cast<const char* const*>(dti + mhw::kOffDtiName) : nullptr;
                if (clsName && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(clsName)) &&
                    StartsWith(clsName, "uPlayer")) {
                    const std::uint8_t mainBit = *reinterpret_cast<const std::uint8_t*>(unit + mhw::kOffPlMainFlags);
                    if ((mainBit & 1U) == 0) return !stepActive;
                }
            }
            return false;
        }

        if (pol == DtiPolicy::Freeze) {
            return !stepActive;
        }

        if (pol == DtiPolicy::CheckShellCreator) {
            const int hg = CheckUnitCollisionHitGroup(unit, exeBase, exeEnd);
            if (hg == 1) return false;
            if (hg == 2 || hg == 3 || hg == 5) return !stepActive;

            std::uintptr_t creator = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffUShellCreator);
            if (!IsAlignedPtr(creator)) {
                creator = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffUShellCreatorLegacy);
            }
            if (IsPlayerOwnedCreator(creator, exeBase, exeEnd)) return false;
            return !stepActive;
        }

        if (pol == DtiPolicy::CheckEffectOwner) {
            const int hg = CheckUnitCollisionHitGroup(unit, exeBase, exeEnd);
            if (hg == 1) return false;
            if (hg == 2 || hg == 3 || hg == 5) return !stepActive;

            const std::uint32_t moveCnt = *reinterpret_cast<const std::uint32_t*>(unit + mhw::kOffUnitMoveCount);
            if (stepActive) {
                // During a stepped world frame, monsters may spawn new effects -> mark them to freeze after the step!
                if (moveCnt == 0) InsertFrozenWorldEffect(unit);
                return false;
            }
            if (g_worldSettledTicks.load(std::memory_order_relaxed) <= 1U) {
                // First full cycle of WorldFreeze: capture all pre-existing world/monster effects (moveCnt > 0)
                if (moveCnt > 0) {
                    InsertFrozenWorldEffect(unit);
                    return true;
                }
                RemoveFrozenWorldEffect(unit);
                return false;
            }
            if (moveCnt == 0) {
                // Spawned while monsters/world are frozen => 100% Player / Weapon / Hitspark / TCS zoom VFX!
                // Evict any stale address from a freed world effect and let this VFX run to completion naturally.
                RemoveFrozenWorldEffect(unit);
                return false;
            }
            return IsFrozenWorldEffect(unit);
        }

        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryDeliverDefenderOnlyHitSEH(std::uintptr_t exeBase,
                                  std::uintptr_t exeEnd,
                                  std::uintptr_t hitInfo,
                                  std::uint32_t defAttr,
                                  std::uint32_t atkGroup,
                                  std::uint32_t defGroup,
                                  int& outResult) {
    __try {
        if (!IsAlignedPtr(hitInfo)) return false;
        const std::uintptr_t atkUnit = *reinterpret_cast<const std::uintptr_t*>(hitInfo + mhw::kOffHitInfoAtkUnit);
        if (!IsAlignedPtr(atkUnit) || !ShouldFreezeUnitSEH(exeBase, exeEnd, atkUnit, false)) {
            return false; // Attacker is NOT frozen (e.g. Player hitting Monster) -> use original function!
        }

        outResult = 0;
        const std::uintptr_t defUnit = *reinterpret_cast<const std::uintptr_t*>(hitInfo + mhw::kOffHitInfoDefUnit);
        if (!IsAlignedPtr(defUnit)) return true;
        if ((*reinterpret_cast<const std::uint8_t*>(defUnit + mhw::kOffUnitAliveFlags) & 0x0EU) == 0) return true;

        bool deliver = false;
        if ((defAttr & 0x08U) != 0) {
            const std::uintptr_t atkParam = *reinterpret_cast<const std::uintptr_t*>(hitInfo + mhw::kOffHitInfoAtkParam);
            const std::uint32_t mask = IsAlignedPtr(atkParam)
                                           ? *reinterpret_cast<const std::uint32_t*>(atkParam + 0x0CU)
                                           : 0U;
            if (mask == 0U) {
                if (atkGroup > 0 && atkGroup < 8 && defGroup > 0 && defGroup < 8) {
                    const auto* matrix = reinterpret_cast<const std::uint8_t*>(exeBase + mhw::kRvaHitGroupMatrix);
                    if (matrix[defGroup + atkGroup * 8] != 0) deliver = true;
                }
            } else if (((mask >> (defGroup & 0x1FU)) & 1U) != 0) {
                deliver = true;
            }
        }
        if (!deliver && (defAttr & 0x40004U) != 0) {
            deliver = true;
        }
        if (!deliver) return true;

        struct alignas(8) DefDamagedEvent {
            std::uintptr_t vft;
            std::uint64_t  unk08;
            std::int32_t   result;
            std::int32_t   pad14;
            std::uintptr_t hitInfoPtr;
        } ev{exeBase + mhw::kRvaEvVftDamaged, 0ULL, 0, 0, hitInfo};

        const std::uintptr_t defVft = *reinterpret_cast<const std::uintptr_t*>(defUnit);
        if (defVft >= exeBase && defVft + 0xA8 <= exeEnd) {
            const auto fnOnEvent = *reinterpret_cast<UnitOnEventFn const*>(defVft + mhw::kOffUnitVftOnEvent);
            fnOnEvent(defUnit, &ev);
            outResult = ev.result;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryDeliverDefenderOnlyGuardSEH(std::uintptr_t exeBase,
                                    std::uintptr_t exeEnd,
                                    std::uintptr_t hitInfo) {
    __try {
        if (!IsAlignedPtr(hitInfo)) return false;
        const std::uintptr_t atkUnit = *reinterpret_cast<const std::uintptr_t*>(hitInfo + mhw::kOffHitInfoAtkUnit);
        if (!IsAlignedPtr(atkUnit) || !ShouldFreezeUnitSEH(exeBase, exeEnd, atkUnit, false)) {
            return false;
        }

        const std::uintptr_t defUnit = *reinterpret_cast<const std::uintptr_t*>(hitInfo + mhw::kOffHitInfoDefUnit);
        if (!IsAlignedPtr(defUnit)) return true;

        struct alignas(8) DefGuardedEvent {
            std::uintptr_t vft;
            std::uint64_t  unk08;
            std::uintptr_t hitInfoPtr;
        } ev{exeBase + mhw::kRvaEvVftGuarded, 0ULL, hitInfo};

        const std::uintptr_t defVft = *reinterpret_cast<const std::uintptr_t*>(defUnit);
        if (defVft >= exeBase && defVft + 0xA8 <= exeEnd) {
            const auto fnOnEvent = *reinterpret_cast<UnitOnEventFn const*>(defVft + mhw::kOffUnitVftOnEvent);
            fnOnEvent(defUnit, &ev);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyMainFreezeSEH(std::uintptr_t exeBase, int mode) {
    // mode: 0 = unfreeze, 1 = freeze immediately, 2 = step 1 frame, 3 = per-frame sync
    __try {
        if (!exeBase) return false;
        const std::uintptr_t mhMain = *reinterpret_cast<const volatile std::uintptr_t*>(
            exeBase + mhw::kRvaSMhMainInstance);
        if (!IsAlignedPtr(mhMain)) return false;

        auto* pStepCount = reinterpret_cast<volatile std::int32_t*>(mhMain + mhw::kOffMainStepCount);
        auto* pSkip      = reinterpret_cast<volatile std::uint8_t*>(mhMain + mhw::kOffMainSkip);
        auto* pPause     = reinterpret_cast<volatile std::uint8_t*>(mhMain + mhw::kOffMainPause);
        auto* pPauseNext = reinterpret_cast<volatile std::uint8_t*>(mhMain + mhw::kOffMainPauseNextFrame);

        if (mode == 1) {
            const std::uintptr_t cam = GetActiveCameraUnit(exeBase);
            if (IsAlignedPtr(cam)) {
                const CamAngles& hist = g_camHistory[(g_camHistIdx + 2) & 3U];
                if (hist.valid && hist.cam == cam && std::isfinite(hist.pitch) && std::isfinite(hist.yaw)) {
                    const float curP = *reinterpret_cast<const float*>(cam + mhw::kOffCamPitch);
                    if (std::fabs(curP - hist.pitch) < 0.15f) {
                        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamPitch) = hist.pitch;
                        *reinterpret_cast<volatile float*>(cam + mhw::kOffCamYaw)   = hist.yaw;
                    }
                }
            }
            FlushAllCameraInputDeltas(exeBase, true);
            *pStepCount = 0;
            *pPauseNext = 1;
            *pSkip      = 1;
            *pPause     = 0;
        } else if (mode == 0) {
            FlushAllCameraInputDeltas(exeBase, true);
            *pStepCount = 0;
            *pPauseNext = 0;
            *pSkip      = 0;
            *pPause     = 0;
        } else if (mode == 2) {
            FlushAllCameraInputDeltas(exeBase, true);
            g_stepCameraGuard.store(true, std::memory_order_release);
            RefreshUnitMoveFlags();
            *pPauseNext = 1;
            *pSkip      = 1;
            *pStepCount = 1;
        } else if (mode == 3) {
            if (*pStepCount == 0) {
                FlushAllCameraInputDeltas(exeBase, true);
            }
            if (*pPauseNext == 0) {
                *pPauseNext = 1;
                *pSkip      = 1;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UpdateLiveCamSnapshotSEH(std::uintptr_t exeBase, bool suppressKbCam, bool needMainPlayer) {
    __try {
        if (suppressKbCam) {
            ClearKeyboardCamInput(exeBase);
        }
        if (needMainPlayer) {
            const std::uintptr_t mainPl = GetMainPlayerUnit(exeBase);
            g_cachedMainPlayer.store(mainPl, std::memory_order_relaxed);
        }
        const std::uintptr_t cam = GetActiveCameraUnit(exeBase);
        g_cachedActiveCam.store(cam, std::memory_order_relaxed);

        if (g_frozen.load(std::memory_order_relaxed)) return;
        if (IsAlignedPtr(cam)) {
            CamAngles& slot = g_camHistory[g_camHistIdx & 3U];
            slot.cam   = cam;
            slot.pitch = *reinterpret_cast<const float*>(cam + mhw::kOffCamPitch);
            slot.yaw   = *reinterpret_cast<const float*>(cam + mhw::kOffCamYaw);
            slot.valid = std::isfinite(slot.pitch) && std::isfinite(slot.yaw);
            ++g_camHistIdx;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

void InitModuleRange(std::uintptr_t exeBase, std::uintptr_t exeEnd) {
    g_exeBase = exeBase;
    g_exeEnd  = exeEnd;
}

void SetOrigUnitGenericMove(void* origFn) {
    g_origUnitGenericMove = reinterpret_cast<UnitGenericMoveFn>(origFn);
}

void SetOrigHitDispatch(void* origFn) {
    g_origHitDispatch = reinterpret_cast<HitDispatchFn>(origFn);
}

void SetOrigGuardDispatch(void* origFn) {
    g_origGuardDispatch = reinterpret_cast<GuardDispatchFn>(origFn);
}

void __fastcall Hook_UnitGenericMove(std::uintptr_t unit) {
    if (!g_origUnitGenericMove) return;

    // Ultra-fast 1-byte check across all ~8,000 unit moves per frame when idle!
    const std::uint8_t fastFlags = g_unitMoveHookFlags.load(std::memory_order_relaxed);
    if (fastFlags == 0) {
        g_origUnitGenericMove(unit);
        return;
    }

    const std::uintptr_t exeBase = g_exeBase;
    const std::uintptr_t exeEnd  = g_exeEnd;
    if (!exeBase || !exeEnd) {
        g_origUnitGenericMove(unit);
        return;
    }

    if ((fastFlags & kMoveFlagSuppressKb) != 0) {
        __try {
            ClearKeyboardCamInput(exeBase);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Guard camera pitch/yaw during FullFreeze single-frame step
    if ((fastFlags & kMoveFlagStepCamGuard) != 0 && g_stepCameraGuard.load(std::memory_order_acquire)) {
        std::uintptr_t activeCam = g_cachedActiveCam.load(std::memory_order_relaxed);
        if (!activeCam) {
            __try { activeCam = GetActiveCameraUnit(exeBase); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (activeCam != 0 && unit == activeCam) {
            if (g_stepCameraGuard.exchange(false, std::memory_order_acq_rel)) {
                RefreshUnitMoveFlags();
                RunGuardedCameraStepMove(exeBase, unit);
                return;
            }
        }
    }

    // WorldFreeze ("Pause Everything Except Player")
    if ((fastFlags & kMoveFlagWorldFreeze) != 0 &&
        g_worldFrozen.load(std::memory_order_acquire) &&
        !g_frozen.load(std::memory_order_relaxed)) {
        const std::uintptr_t mainPl = g_cachedMainPlayer.load(std::memory_order_relaxed);
        std::uintptr_t anchorUnit = mainPl;
        if (!anchorUnit) anchorUnit = g_cachedActiveCam.load(std::memory_order_relaxed);

        if (anchorUnit != 0 && unit == anchorUnit) {
            if (g_worldStepActive.load(std::memory_order_relaxed)) {
                g_worldStepActive.store(false, std::memory_order_release);
            } else if (g_worldStepReq.exchange(0, std::memory_order_acq_rel) != 0) {
                ClearPinnedNodesTable();
                g_worldStepActive.store(true, std::memory_order_release);
            } else {
                g_worldSettledTicks.fetch_add(1, std::memory_order_relaxed);
            }

            if (!g_worldStepActive.load(std::memory_order_relaxed) &&
                g_keepHitboxesActive.load(std::memory_order_relaxed) &&
                mainPl != 0 && unit == mainPl) {
                RearmPlayerFrozenHitCacheSEH(exeBase, exeEnd, mainPl);
            }
        }

        const bool stepActive = g_worldStepActive.load(std::memory_order_acquire);
        if (ShouldFreezeUnitSEH(exeBase, exeEnd, unit, stepActive)) {
            KeepFrozenUnitCollisionsAlive(exeBase, exeEnd, unit);
            return;
        }
    }

    g_origUnitGenericMove(unit);
}

int __fastcall Hook_HitDispatch(std::uintptr_t defenderColl,
                                std::uintptr_t hitInfo,
                                std::uint32_t atkAttr,
                                std::uint32_t defAttr,
                                std::uint32_t atkGroup,
                                std::uint32_t defGroup) {
    if (g_worldFrozen.load(std::memory_order_acquire) &&
        !g_frozen.load(std::memory_order_relaxed) &&
        !g_worldStepActive.load(std::memory_order_relaxed) &&
        g_keepHitboxesActive.load(std::memory_order_relaxed) &&
        g_exeBase != 0 && g_exeEnd != 0) {
        int customRes = 0;
        if (TryDeliverDefenderOnlyHitSEH(g_exeBase, g_exeEnd, hitInfo, defAttr, atkGroup, defGroup, customRes)) {
            return customRes;
        }
    }
    return g_origHitDispatch
               ? g_origHitDispatch(defenderColl, hitInfo, atkAttr, defAttr, atkGroup, defGroup)
               : 0;
}

std::uint64_t __fastcall Hook_GuardDispatch(std::uintptr_t defenderColl, std::uintptr_t hitInfo) {
    if (g_worldFrozen.load(std::memory_order_acquire) &&
        !g_frozen.load(std::memory_order_relaxed) &&
        !g_worldStepActive.load(std::memory_order_relaxed) &&
        g_keepHitboxesActive.load(std::memory_order_relaxed) &&
        g_exeBase != 0 && g_exeEnd != 0) {
        if (TryDeliverDefenderOnlyGuardSEH(g_exeBase, g_exeEnd, hitInfo)) {
            return 0;
        }
    }
    return g_origGuardDispatch ? g_origGuardDispatch(defenderColl, hitInfo) : 0;
}

bool Toggle(std::uintptr_t exeBase) {
    const bool next = !g_frozen.load(std::memory_order_relaxed);
    SetFrozen(exeBase, next);
    return g_frozen.load(std::memory_order_relaxed);
}

void SetFrozen(std::uintptr_t exeBase, bool frozen) {
    if (frozen) {
        UpdateLiveCamSnapshotSEH(exeBase, false, false);
        if (g_worldFrozen.load(std::memory_order_relaxed)) {
            g_worldFrozen.store(false, std::memory_order_release);
            g_worldStepActive.store(false, std::memory_order_relaxed);
            g_worldStepReq.store(0, std::memory_order_relaxed);
            ClearWorldEffectsTable();
            ClearPinnedNodesTable();
        }
    }
    g_stepCameraGuard.store(false, std::memory_order_relaxed);
    if (ApplyMainFreezeSEH(exeBase, frozen ? 1 : 0)) {
        g_frozen.store(frozen, std::memory_order_release);
        RefreshUnitMoveFlags();
        app::Log("freeze: Full Freeze %s", frozen ? "ON (zero-delay instant pause)" : "OFF (resumed)");
    } else {
        g_frozen.store(false, std::memory_order_release);
        RefreshUnitMoveFlags();
        app::Log("freeze: sMhMain not ready yet");
    }
}

bool ToggleWorld(std::uintptr_t exeBase) {
    const bool next = !g_worldFrozen.load(std::memory_order_relaxed);
    SetWorldFrozen(exeBase, next);
    return g_worldFrozen.load(std::memory_order_relaxed);
}

void SetWorldFrozen(std::uintptr_t exeBase, bool worldFrozen) {
    if (worldFrozen) {
        if (g_frozen.load(std::memory_order_relaxed)) {
            ApplyMainFreezeSEH(exeBase, 0);
            g_frozen.store(false, std::memory_order_release);
        }
        UpdateLiveCamSnapshotSEH(exeBase, false, true);
        ClearWorldEffectsTable();
        ClearPinnedNodesTable();
        g_prevPlActionDef = false;
        g_worldSettledTicks.store(0, std::memory_order_relaxed);
        g_worldStepReq.store(0, std::memory_order_relaxed);
        g_worldStepActive.store(false, std::memory_order_relaxed);
        g_worldFrozen.store(true, std::memory_order_release);
        RefreshUnitMoveFlags();
        app::Log("freeze: World Freeze ON (everything paused except Player, keepHitboxes=%s)",
                 g_keepHitboxesActive.load(std::memory_order_relaxed) ? "ON" : "OFF");
    } else {
        g_worldFrozen.store(false, std::memory_order_release);
        g_worldStepReq.store(0, std::memory_order_relaxed);
        g_worldStepActive.store(false, std::memory_order_relaxed);
        RefreshUnitMoveFlags();
        ClearWorldEffectsTable();
        ClearPinnedNodesTable();
        app::Log("freeze: World Freeze OFF (world resumed)");
    }
}

bool ToggleKeepHitboxesActive() {
    const bool next = !g_keepHitboxesActive.load(std::memory_order_relaxed);
    SetKeepHitboxesActive(next);
    return next;
}

void SetKeepHitboxesActive(bool keepActive) {
    g_keepHitboxesActive.store(keepActive, std::memory_order_release);
    if (!keepActive) {
        ClearPinnedNodesTable();
    }
    RefreshUnitMoveFlags();
    app::Log("freeze: Keep Single-Trigger Hitboxes Active = %s", keepActive ? "ON" : "OFF");
}

bool IsKeepHitboxesActive() {
    return g_keepHitboxesActive.load(std::memory_order_relaxed);
}

bool StepOneFrame(std::uintptr_t exeBase) {
    if (g_frozen.load(std::memory_order_relaxed)) {
        if (ApplyMainFreezeSEH(exeBase, 2)) {
            app::Log("freeze: Full Freeze stepped +1 frame");
            return true;
        }
        return false;
    }
    if (g_worldFrozen.load(std::memory_order_relaxed)) {
        g_worldStepReq.store(1, std::memory_order_release);
        app::Log("freeze: World Freeze stepped +1 frame");
        return true;
    }
    return false;
}

bool SyncActiveFrame(std::uintptr_t exeBase, bool anyModKeyHeld) {
    if (anyModKeyHeld) {
        g_suppressKbFramesLeft = 10;
    } else if (g_suppressKbFramesLeft > 0) {
        --g_suppressKbFramesLeft;
    }

    const bool suppressNow = (g_suppressKbFramesLeft > 0);
    if (suppressNow != g_suppressKbActive.load(std::memory_order_relaxed)) {
        g_suppressKbActive.store(suppressNow, std::memory_order_relaxed);
        RefreshUnitMoveFlags();
    }

    const bool fullFrz  = g_frozen.load(std::memory_order_relaxed);
    const bool worldFrz = g_worldFrozen.load(std::memory_order_relaxed);
    if (suppressNow || worldFrz) {
        UpdateLiveCamSnapshotSEH(exeBase, suppressNow, worldFrz);
    }
    if (fullFrz) {
        ApplyMainFreezeSEH(exeBase, 3);
    }
    return suppressNow || fullFrz || worldFrz;
}

bool IsFrozen() {
    return g_frozen.load(std::memory_order_relaxed);
}

bool IsWorldFrozen() {
    return g_worldFrozen.load(std::memory_order_relaxed);
}

Mode CurrentMode() {
    if (g_frozen.load(std::memory_order_relaxed))      return Mode::Full;
    if (g_worldFrozen.load(std::memory_order_relaxed)) return Mode::World;
    return Mode::None;
}

} // namespace freeze

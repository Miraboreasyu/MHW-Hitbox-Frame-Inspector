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
std::uintptr_t g_worldFreezeOwnerPl = 0;
int g_worldFreezeNullPlFrames = 0;
bool g_prevPlActionDef = false;

constexpr std::uint8_t kMoveFlagWorldFreeze  = 0x01U;
constexpr std::uint8_t kMoveFlagStepCamGuard = 0x02U;
constexpr std::uint8_t kMoveFlagSuppressKb   = 0x04U;
constexpr std::uint8_t kMoveFlagBurstDrain   = 0x08U;

// World Freeze Delayed Burst state (queues/hooks implemented at the end of this file)
std::atomic<bool> g_burstEnabled{false};      // user option (WorldFreezeDelayedBurst)
std::atomic<bool> g_burstDrainPending{false}; // deferred records waiting to burst out after resume

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
    if (g_burstDrainPending.load(std::memory_order_relaxed) &&
        !g_frozen.load(std::memory_order_relaxed) && !worldFrz)
        f |= kMoveFlagBurstDrain;
    g_unitMoveHookFlags.store(f, std::memory_order_release);

    const bool needBurstCapture = worldFrz && g_burstEnabled.load(std::memory_order_relaxed);
    hook::SetDynamicHook5Enabled(hook::kDynDmgNum,      needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynDmgNumZorah, needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynDmgNumEx,    needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynHitEfc,      needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynShake1,      needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynShake2,      needBurstCapture);
    hook::SetDynamicHook5Enabled(hook::kDynHitFeedback, needBurstCapture);

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

constexpr std::uint32_t kDtiPolicySlots = 1024;
// Packed atomic slot: bits [0..55] = canonical MtDTI ptr, bits [56..63] = DtiPolicy (0 = empty)
std::atomic<std::uint64_t> g_dtiPolicyCache[kDtiPolicySlots]{};

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
    const std::uint64_t dtiKey = static_cast<std::uint64_t>(dti);
    if ((dtiKey >> 56) != 0) return ComputeDtiPolicy(dti);

    std::uint32_t idx = static_cast<std::uint32_t>((dti >> 4) * 2654435761ULL) & (kDtiPolicySlots - 1);
    for (std::uint32_t n = 0; n < 16; ++n) {
        const std::uint64_t cur = g_dtiPolicyCache[idx].load(std::memory_order_acquire);
        if ((cur & 0x00FFFFFFFFFFFFFFULL) == dtiKey) {
            const auto pol = static_cast<DtiPolicy>(cur >> 56);
            if (pol != DtiPolicy::Unknown) return pol;
        }
        if (cur == 0) {
            const DtiPolicy computed = ComputeDtiPolicy(dti);
            const std::uint64_t packed = (static_cast<std::uint64_t>(computed) << 56) | dtiKey;
            std::uint64_t expected = 0;
            g_dtiPolicyCache[idx].compare_exchange_strong(
                expected, packed, std::memory_order_release, std::memory_order_relaxed);
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

        // 1b. comp + 0x760 (count at +0x828) is cpObjCollision's ignore-unit blocklist (e.g. a uShellEm
        //     stores its own caster Monster here so fireballs/explosions never damage their own owner!).
        //     NEVER wipe non-player / frozen handles from +0x760 — only clear mainPlayer if ever added.
        const std::uintptr_t mainPl = g_cachedMainPlayer.load(std::memory_order_relaxed);
        const std::uint64_t hitUnitCnt = *reinterpret_cast<const std::uint64_t*>(comp + mhw::kOffObjCollHitUnitCount);
        if (mainPl != 0 && hitUnitCnt > 0 && hitUnitCnt <= 8) {
            LPCRITICAL_SECTION cs = nullptr;
            for (std::uint64_t i = 0; i < hitUnitCnt; ++i) {
                const std::uintptr_t hAddr = comp + mhw::kOffObjCollHitUnits + 0x08 + i * 0x18;
                if (*reinterpret_cast<const std::uintptr_t*>(hAddr) == mainPl) {
                    if (!cs) cs = fnGetCritSec();
                    if (cs) fnHandleSet(cs, 0, hAddr);
                }
            }
        }

        // 1c. Clear ONLY mainPlayer entries in the 16-slot per-target hit-count cap table (+0x5400)
        //     (slot layout verified via FUN_1412d4e80: +0x00=atkNode, +0x08=defUnit, +0x10=hitCount).
        if (mainPl != 0) {
            for (std::size_t s = 0; s < 16; ++s) {
                const std::uintptr_t slot = comp + mhw::kOffObjCollHitHist + s * 0x20;
                if (*reinterpret_cast<const std::uintptr_t*>(slot + 0x08) == mainPl) {
                    *reinterpret_cast<volatile std::uintptr_t*>(slot + 0x00) = 0;
                    *reinterpret_cast<volatile std::uintptr_t*>(slot + 0x08) = 0;
                    *reinterpret_cast<volatile std::int32_t*>(slot + 0x10)   = 0;
                }
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

    // 3. Guarantee positive mAge on every occupied cCache[32] slot AND garbage-collect finished swings (>=4f)
    //    so FUN_1412d5100 NEVER hits the 0.0f < 0.0f eviction lockup or 32-slot saturation under high hit-rate spam!
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uintptr_t slot = comp + mhw::kOffObjCollCacheBase + i * mhw::kObjCollCacheStride;
        const std::int32_t uid = *reinterpret_cast<const std::int32_t*>(slot + mhw::kOffCacheUid);
        if (uid == 0) continue;

        float age = *reinterpret_cast<const float*>(slot + mhw::kOffCacheAge);
        if (!(age >= 1.0f)) {
            age = 1.0f;
            *reinterpret_cast<volatile float*>(slot + mhw::kOffCacheAge) = age;
        }
        if (age >= 4.0f) {
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
        // Never allow a frozen attacker (Monster / uShellEm / uMhEfc) to damage itself or any other
        // frozen unit (Monster / Palico / Gimmick) during World Freeze — deliver only to unfrozen defenders (Player).
        if (defUnit == atkUnit || ShouldFreezeUnitSEH(exeBase, exeEnd, defUnit, false)) {
            return true;
        }

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
        if (defUnit == atkUnit || ShouldFreezeUnitSEH(exeBase, exeEnd, defUnit, false)) {
            return true;
        }

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

    // Delayed Burst: replay deferred damage numbers / camera cues once per frame on the sim thread
    if ((fastFlags & kMoveFlagBurstDrain) != 0) {
        std::uintptr_t drainAnchor = g_cachedMainPlayer.load(std::memory_order_relaxed);
        if (!drainAnchor) drainAnchor = g_cachedActiveCam.load(std::memory_order_relaxed);
        if (drainAnchor != 0 && unit == drainAnchor) {
            BurstDrainTick(exeBase);
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
            g_worldFreezeOwnerPl = 0;
            g_worldFreezeNullPlFrames = 0;
            BurstOnWorldResume();
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
        g_worldFreezeOwnerPl      = g_cachedMainPlayer.load(std::memory_order_relaxed);
        g_worldFreezeNullPlFrames = 0;
        ClearWorldEffectsTable();
        ClearPinnedNodesTable();
        g_prevPlActionDef = false;
        g_worldSettledTicks.store(0, std::memory_order_relaxed);
        g_worldStepReq.store(0, std::memory_order_relaxed);
        g_worldStepActive.store(false, std::memory_order_relaxed);
        BurstBeginSession();
        g_worldFrozen.store(true, std::memory_order_release);
        RefreshUnitMoveFlags();
        app::Log("freeze: World Freeze ON (everything paused except Player, keepHitboxes=%s)",
                 g_keepHitboxesActive.load(std::memory_order_relaxed) ? "ON" : "OFF");
    } else {
        g_worldFrozen.store(false, std::memory_order_release);
        g_worldStepReq.store(0, std::memory_order_relaxed);
        g_worldStepActive.store(false, std::memory_order_relaxed);
        g_worldFreezeOwnerPl      = 0;
        g_worldFreezeNullPlFrames = 0;
        BurstOnWorldResume();
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
    bool worldFrz       = g_worldFrozen.load(std::memory_order_relaxed);
    if (suppressNow || worldFrz) {
        UpdateLiveCamSnapshotSEH(exeBase, suppressNow, worldFrz);
    }
    if (worldFrz) {
        const std::uintptr_t curPl = g_cachedMainPlayer.load(std::memory_order_relaxed);
        if (g_worldFreezeOwnerPl == 0) {
            if (curPl != 0) {
                g_worldFreezeOwnerPl      = curPl;
                g_worldFreezeNullPlFrames = 0;
            }
        } else if (curPl == g_worldFreezeOwnerPl) {
            g_worldFreezeNullPlFrames = 0;
        } else if (curPl != 0 || ++g_worldFreezeNullPlFrames >= 15) {
            app::Log("freeze: stage/quest transition detected (pl %p -> %p), auto-resuming World Freeze",
                     reinterpret_cast<void*>(g_worldFreezeOwnerPl),
                     reinterpret_cast<void*>(curPl));
            BurstAbort();
            SetWorldFrozen(exeBase, false);
            worldFrz = false;
        }
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

// ============================================================================
//  World Freeze Delayed Burst
//
//  Engine limits being protected (MHW:I 15.23):
//    - uGUIHudDamage      : 20 slots   -> drop hits beyond 20 unless paced
//    - cCameraShakeManager:  8 slots   -> priority/sequence eviction
//    - sMhUnit MoveLine[0x13] (uMhEffect units, hit VFX) -> pool exhaustion crash
//  Strategy:
//    - damage numbers: queued as POD records with hit-time monster pos, replayed paced while
//      free HUD slots exist (tracking live monster displacement)
//    - hit VFX: live hit VFX + native hit sound play normally during World Freeze (with
//      MoveLine[0x13] pool guard fallback to sound-only above 220 units), while the full
//      0xF0-byte cMhEfcParam POD is queued to burst out again paced when World Freeze resumes
//    - camera shakes: live camera shakes play normally during World Freeze (8-slot priority
//      eviction is overflow-safe), while 0xD0-byte cShakeParam records are also queued to
//      replay cascadingly by priority/amplitude when World Freeze resumes
//    - player hitstop ("卡肉"): 100% native during World Freeze; hit count feeds resume FOV punch
// ============================================================================
namespace {

using DmgNumFn   = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::int32_t, const float*,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint8_t);
using DmgNumExFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::int32_t, const float*,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t,
                                     std::uint32_t, std::uint32_t, std::uint8_t);
using HitEfcFn   = void(__fastcall*)(long long*, std::uintptr_t, std::int32_t, std::int32_t, std::uintptr_t);
using Shake1Fn   = std::int32_t(__fastcall*)(std::uintptr_t, const void*, const float*);
using Shake2Fn   = std::int32_t(__fastcall*)(std::uintptr_t, const void*, std::uint32_t, const float*);
using HitFbFn    = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::int32_t);
using EpvGetElemFn = std::uintptr_t(__fastcall*)(std::uintptr_t epv, std::int32_t elemIdx, std::int32_t efxIdx);
using WwiseCallByIdFn = void(__fastcall*)(std::uintptr_t container, std::uintptr_t unit,
                                          std::uint32_t bankId, std::uint32_t eventId, const void* param);
using SMhEffectCreateFn = std::uintptr_t(__fastcall*)(std::uintptr_t sMhEffect, std::uintptr_t dti,
                                                      std::uintptr_t epv, std::int32_t elemIdx,
                                                      std::int32_t efxIdx, void* efcp,
                                                      std::uintptr_t parentUnit, std::uintptr_t elemOverride);

DmgNumFn   g_origDmgNum       = nullptr;
DmgNumFn   g_origDmgNumZorah  = nullptr;
DmgNumExFn g_origDmgNumEx     = nullptr;
HitEfcFn   g_origHitEfc       = nullptr;
Shake1Fn   g_origShake1       = nullptr;
Shake2Fn   g_origShake2       = nullptr;
HitFbFn    g_origHitFeedback  = nullptr;

constexpr std::size_t   kMaxDeferredDmg     = 2048;
constexpr std::size_t   kMaxDeferredVfx     = 2048;
constexpr std::size_t   kMaxDeferredShakes  = 128;
constexpr std::size_t   kCMhEfcParamSize    = 0xF0;
constexpr std::size_t   kShakeParamSize     = 0xD0;
constexpr std::uint16_t kVfxPoolSafeLimit   = 220; // engine guard is < 280 units on MoveLine[0x13]
constexpr std::uint32_t kDmgInitialBurst    = 6;
constexpr std::uint32_t kDmgPerTick         = 4;
constexpr std::uint32_t kVfxPerTick         = 6;
constexpr int           kShakeIntervalTicks = 4;
constexpr int           kBurstTimeoutTicks  = 1800; // ~30 s @ 60 fps, then abandon leftovers
constexpr int           kPunchFrames        = 16;
constexpr std::uint32_t kHitWeightCap       = 128;

struct DmgRec {
    std::uintptr_t gui;
    std::uintptr_t mon;
    std::int32_t   dmg;
    std::uint32_t  a[5];
    std::uint8_t   b[2];
    std::uint8_t   kind; // 0 = ShowDamageNumber, 1 = Zorah, 2 = Ex
    std::uint8_t   hasPos;
    float          pos[4];
    std::uint8_t   hasMonPos;
    float          monPosAtHit[3];
};

struct VfxRec {
    std::uintptr_t epv;
    std::int32_t   elemIdx;
    std::int32_t   efxIdx;
    std::uintptr_t mon;
    std::uint8_t   hasMonPos;
    float          monPosAtHit[3];
    alignas(8) std::uint8_t efcp[kCMhEfcParamSize];
};

struct ShakeRec {
    std::uintptr_t cam;
    std::uint32_t  extra;
    std::uint8_t   kind; // 1 = addShake1, 2 = addShake2
    std::uint8_t   hasPos;
    float          pos[4];
    std::uint8_t   param[kShakeParamSize];
};

std::atomic_flag g_burstLock = ATOMIC_FLAG_INIT;

struct BurstLockGuard {
    BurstLockGuard() { while (g_burstLock.test_and_set(std::memory_order_acquire)) YieldProcessor(); }
    ~BurstLockGuard() { g_burstLock.clear(std::memory_order_release); }
    BurstLockGuard(const BurstLockGuard&) = delete;
    BurstLockGuard& operator=(const BurstLockGuard&) = delete;
};

DmgRec        g_dmgQ[kMaxDeferredDmg];
std::uint32_t g_dmgHead  = 0;
std::uint32_t g_dmgCount = 0;
std::atomic<std::uint32_t> g_dmgQueued{0};

VfxRec        g_vfxQ[kMaxDeferredVfx];
std::uint32_t g_vfxHead          = 0;
std::uint32_t g_vfxCount         = 0;
std::uint32_t g_vfxTotalCaptured = 0;

ShakeRec      g_shakeQ[kMaxDeferredShakes];
std::uint32_t g_shakeCount = 0;

std::uint32_t g_hitWeight  = 0;
std::int32_t  g_hitMaxTier = 0;

// Sim-thread-only replay state
int   g_drainTicks = 0;
int   g_punchLeft  = 0;
float g_punchAmp   = 0.0f;

inline bool BurstCaptureActive() {
    return g_burstEnabled.load(std::memory_order_relaxed) &&
           g_worldFrozen.load(std::memory_order_relaxed) &&
           !g_frozen.load(std::memory_order_relaxed);
}

void ResetBurstStateLocked() {
    g_dmgHead = g_dmgCount = 0;
    g_dmgQueued.store(0, std::memory_order_relaxed);
    g_vfxHead = g_vfxCount = g_vfxTotalCaptured = 0;
    g_shakeCount = 0;
    g_hitWeight = 0;
    g_hitMaxTier = 0;
    g_drainTicks = 0;
    g_punchLeft = 0;
    g_punchAmp = 0.0f;
}

bool BurstHasAnythingLocked() {
    return g_dmgCount != 0 || g_vfxCount != 0 || g_shakeCount != 0 || g_hitWeight != 0 || g_punchLeft != 0;
}

inline std::int32_t ShakePriority(const ShakeRec& r) {
    std::int32_t p = 0;
    std::memcpy(&p, r.param + 0x10, sizeof(p));
    return p;
}

inline float ShakeAmpSq(const ShakeRec& r) {
    float a[3]{};
    std::memcpy(a, r.param + 0x30, sizeof(a));
    const float s = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    return std::isfinite(s) ? s : 0.0f;
}

inline bool ShakeIsStronger(const ShakeRec& cand, const ShakeRec& cur) {
    const std::int32_t pCand = ShakePriority(cand);
    const std::int32_t pCur  = ShakePriority(cur);
    if (pCand != pCur) return pCand < pCur;
    return ShakeAmpSq(cand) > ShakeAmpSq(cur);
}

inline bool HasVfxSound(const VfxRec& r) {
    std::uintptr_t sp = 0;
    std::memcpy(&sp, r.efcp + 0xE0, sizeof(sp));
    return sp != 0;
}

// ---- POD-only SEH helpers (MSVC C2712: no C++ destructors in __try frames) ----

bool CopyBytesSEH(void* dst, const void* src, std::size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadUnitPosSEH(std::uintptr_t unit, float outPos[3]) {
    __try {
        if (!IsAlignedPtr(unit)) return false;
        const std::uint8_t alive = *reinterpret_cast<const std::uint8_t*>(unit + mhw::kOffUnitAliveFlags);
        if ((alive & 0x0EU) == 0) return false;
        const auto* p = reinterpret_cast<const float*>(unit + mhw::kOffUCoordPos);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
        outPos[0] = p[0];
        outPos[1] = p[1];
        outPos[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t CountFreeHudSlotsSEH(std::uintptr_t gui) {
    __try {
        if (gui < 0x10000ULL) return 0;
        const std::uintptr_t hud = *reinterpret_cast<const std::uintptr_t*>(gui + mhw::kOffSMhGuiHudDamage);
        if (hud < 0x10000ULL) return 0;
        std::uint32_t freeCnt = 0;
        for (std::size_t i = 0; i < mhw::kHudDmgSlotCount; ++i) {
            const std::uint8_t active =
                *reinterpret_cast<const std::uint8_t*>(hud + mhw::kOffHudDmgActive + i * mhw::kHudDmgSlotStride);
            if (active == 0) ++freeCnt;
        }
        return freeCnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReplayDmgSEH(const DmgRec& r) {
    __try {
        float adjPos[4]{r.pos[0], r.pos[1], r.pos[2], r.pos[3]};
        if (r.hasPos && r.hasMonPos && IsAlignedPtr(r.mon)) {
            float curMon[3]{};
            if (ReadUnitPosSEH(r.mon, curMon)) {
                const float dx = curMon[0] - r.monPosAtHit[0];
                const float dy = curMon[1] - r.monPosAtHit[1];
                const float dz = curMon[2] - r.monPosAtHit[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(d2) && d2 < (1500.0f * 1500.0f)) {
                    adjPos[0] += dx;
                    adjPos[1] += dy;
                    adjPos[2] += dz;
                }
            }
        }
        const float* pos = r.hasPos ? adjPos : nullptr;
        if (r.kind == 0 && g_origDmgNum) {
            g_origDmgNum(r.gui, r.mon, r.dmg, pos, r.a[0], r.a[1], r.a[2],
                         static_cast<std::int32_t>(r.a[3]), r.b[0]);
        } else if (r.kind == 1 && g_origDmgNumZorah) {
            g_origDmgNumZorah(r.gui, r.mon, r.dmg, pos, r.a[0], r.a[1], r.a[2],
                              static_cast<std::int32_t>(r.a[3]), r.b[0]);
        } else if (r.kind == 2 && g_origDmgNumEx) {
            g_origDmgNumEx(r.gui, r.mon, r.dmg, pos, r.a[0], r.a[1], r.a[2], r.b[0], r.a[3], r.a[4], r.b[1]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CaptureVfxSEH(long long* ctx, std::uintptr_t epv, std::int32_t elemIdx, std::int32_t efxIdx,
                   std::uintptr_t efcp, VfxRec& out, std::uintptr_t& outSoundParam) {
    outSoundParam = 0;
    __try {
        if (!ctx || !IsAlignedPtr(epv) || !IsAlignedPtr(efcp)) return false;
        const std::uintptr_t dmgChk = static_cast<std::uintptr_t>(ctx[0]);
        if (!IsAlignedPtr(dmgChk)) return false;
        const std::uintptr_t mon = *reinterpret_cast<const std::uintptr_t*>(dmgChk + 8);
        if (!IsAlignedPtr(mon)) return false;

        std::memcpy(out.efcp, reinterpret_cast<const void*>(efcp), kCMhEfcParamSize);
        std::memcpy(&outSoundParam, out.efcp + 0xE0, sizeof(outSoundParam));
        out.epv     = epv;
        out.elemIdx = elemIdx;
        out.efxIdx  = efxIdx;
        out.mon     = mon;
        out.hasMonPos = ReadUnitPosSEH(mon, out.monPosAtHit) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PlayEpvSoundImmediateSEH(std::uintptr_t exeBase, std::uintptr_t epv, std::int32_t elemIdx,
                              std::int32_t efxIdx, std::uintptr_t monster, std::uintptr_t soundParam) {
    __try {
        if (!exeBase || !IsAlignedPtr(epv) || !IsAlignedPtr(monster) || !IsAlignedPtr(soundParam)) return;
        const auto fnGetElem = reinterpret_cast<EpvGetElemFn>(exeBase + mhw::kRvaEpvGetEffectElement);
        const std::uintptr_t epvElem = fnGetElem(epv, elemIdx, efxIdx);
        if (!IsAlignedPtr(epvElem)) return;

        const std::uint16_t cat   = *reinterpret_cast<const std::uint16_t*>(epvElem + 0x2B2);
        const std::uint16_t subId = *reinterpret_cast<const std::uint16_t*>(epvElem + 0x2B0);
        const std::uintptr_t epvSoundRes = *reinterpret_cast<const std::uintptr_t*>(soundParam);
        if (!IsAlignedPtr(epvSoundRes)) return;

        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(epvSoundRes + 0xC0);
        const std::uintptr_t arr  = *reinterpret_cast<const std::uintptr_t*>(epvSoundRes + 0xD0);
        if (count == 0 || count > 4096 || !IsAlignedPtr(arr)) return;

        const auto* vZero  = reinterpret_cast<const float*>(exeBase + mhw::kRvaConstVec3Zero);
        const auto* vUnitZ = reinterpret_cast<const float*>(exeBase + mhw::kRvaConstVec3UnitZ);
        const auto fnCallById = reinterpret_cast<WwiseCallByIdFn>(exeBase + mhw::kRvaWwiseCallEventById);

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(arr + i * 8);
            if (!IsAlignedPtr(entry)) continue;
            const std::int32_t evId = *reinterpret_cast<const std::int32_t*>(entry + 0x0C);
            if (evId == -1) continue;
            if (*reinterpret_cast<const std::uint32_t*>(entry + 0x14) != cat) continue;
            if (*reinterpret_cast<const std::uint32_t*>(entry + 0x18) != subId) continue;

            const std::uint32_t bankId = *reinterpret_cast<const std::uint32_t*>(entry + 0x08);
            alignas(16) std::uint8_t wwiseParam[0x88]{};
            std::memcpy(wwiseParam + 0x00, vZero, 12);
            std::memcpy(wwiseParam + 0x10, vUnitZ, 12);
            std::memcpy(wwiseParam + 0x20, vZero, 12);
            std::memcpy(wwiseParam + 0x30, &monster, sizeof(monster));
            const std::uint64_t flags = 0x10ULL;
            std::memcpy(wwiseParam + 0x80, &flags, sizeof(flags));
            fnCallById(soundParam + 8, monster, bankId, static_cast<std::uint32_t>(evId), wwiseParam);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReplayVfxSEH(std::uintptr_t exeBase, const VfxRec& r, bool allowBurstSound) {
    __try {
        if (!exeBase || !IsAlignedPtr(r.epv) || !IsAlignedPtr(r.mon)) return false;
        const std::uint8_t alive = *reinterpret_cast<const std::uint8_t*>(r.mon + mhw::kOffUnitAliveFlags);
        if ((alive & 0x0EU) == 0) return false;

        const std::uintptr_t sMhEffect = *reinterpret_cast<const std::uintptr_t*>(exeBase + mhw::kRvaSMhEffectInstance);
        if (!IsAlignedPtr(sMhEffect)) return false;
        const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(sMhEffect);
        if (!IsAlignedPtr(vft)) return false;
        const auto fnCreate = *reinterpret_cast<const SMhEffectCreateFn*>(vft + mhw::kOffSMhEffectCreateVft);
        if (!fnCreate) return false;

        alignas(16) std::uint8_t efcpCopy[kCMhEfcParamSize];
        std::memcpy(efcpCopy, r.efcp, kCMhEfcParamSize);
        const std::uintptr_t paramVft = exeBase + mhw::kRvaCMhEfcParamVft;
        std::memcpy(efcpCopy + 0x00, &paramVft, sizeof(paramVft));

        std::int32_t jntId = -1;
        std::memcpy(&jntId, efcpCopy + 0x60, sizeof(jntId));
        if (jntId < 0 && r.hasMonPos) {
            float curMon[3]{};
            if (ReadUnitPosSEH(r.mon, curMon)) {
                const float dx = curMon[0] - r.monPosAtHit[0];
                const float dy = curMon[1] - r.monPosAtHit[1];
                const float dz = curMon[2] - r.monPosAtHit[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(d2) && d2 < (1500.0f * 1500.0f)) {
                    float pos[3]{};
                    std::memcpy(pos, efcpCopy + 0x10, sizeof(pos));
                    pos[0] += dx;
                    pos[1] += dy;
                    pos[2] += dz;
                    std::memcpy(efcpCopy + 0x10, pos, sizeof(pos));
                }
            }
        }

        if (!allowBurstSound) {
            const std::uintptr_t zeroPtr = 0;
            std::memcpy(efcpCopy + 0xE0, &zeroPtr, sizeof(zeroPtr));
        }

        fnCreate(sMhEffect, exeBase + mhw::kRvaUMhEffectDti, r.epv, r.elemIdx, r.efxIdx, efcpCopy, r.mon, 0ULL);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReplayShakeSEH(const ShakeRec& r) {
    __try {
        const float* pos = r.hasPos ? r.pos : nullptr;
        if (r.kind == 1 && g_origShake1) {
            g_origShake1(r.cam, r.param, pos);
        } else if (r.kind == 2 && g_origShake2) {
            g_origShake2(r.cam, r.param, r.extra, pos);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint16_t HitEfcPoolCountSEH() {
    __try {
        const std::uintptr_t base = g_exeBase;
        if (!base) return 0xFFFFu;
        const std::uintptr_t unitSys = *reinterpret_cast<const std::uintptr_t*>(base + mhw::kRvaSMhUnitInstance);
        if (unitSys < 0x10000ULL) return 0xFFFFu;
        return *reinterpret_cast<const std::uint16_t*>(unitSys + mhw::kOffMoveLine13UnitNum);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFu;
    }
}

bool ApplyFovPunchSEH(std::uintptr_t exeBase, float scale) {
    __try {
        const std::uintptr_t cam = GetActiveCameraUnit(exeBase);
        if (!IsAlignedPtr(cam)) return false;
        const float baseFov = *reinterpret_cast<const float*>(cam + mhw::kOffCamVerticalFov);
        if (!(baseFov > 0.05f) || !(baseFov < 200.0f)) return false;
        *reinterpret_cast<float*>(cam + mhw::kOffCamFovOverride) = baseFov * scale;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Queue helpers (take lock, no SEH) ----

void PushDmg(const DmgRec& r) {
    BurstLockGuard lk;
    if (g_dmgCount >= kMaxDeferredDmg) return;
    g_dmgQ[(g_dmgHead + g_dmgCount) % kMaxDeferredDmg] = r;
    ++g_dmgCount;
    g_dmgQueued.store(g_dmgCount, std::memory_order_relaxed);
}

bool PeekPopDmg(DmgRec& out, bool pop) {
    BurstLockGuard lk;
    if (g_dmgCount == 0) return false;
    out = g_dmgQ[g_dmgHead];
    if (pop) {
        g_dmgHead = (g_dmgHead + 1) % kMaxDeferredDmg;
        --g_dmgCount;
        g_dmgQueued.store(g_dmgCount, std::memory_order_relaxed);
    }
    return true;
}

void PushVfx(const VfxRec& r) {
    BurstLockGuard lk;
    if (g_vfxCount >= kMaxDeferredVfx) return;
    g_vfxQ[(g_vfxHead + g_vfxCount) % kMaxDeferredVfx] = r;
    ++g_vfxCount;
    ++g_vfxTotalCaptured;
}

bool PopVfx(VfxRec& out) {
    BurstLockGuard lk;
    if (g_vfxCount == 0) return false;
    out = g_vfxQ[g_vfxHead];
    g_vfxHead = (g_vfxHead + 1) % kMaxDeferredVfx;
    --g_vfxCount;
    return true;
}

void PushShake(const ShakeRec& r) {
    BurstLockGuard lk;
    if (g_shakeCount < kMaxDeferredShakes) {
        g_shakeQ[g_shakeCount++] = r;
        return;
    }
    std::uint32_t weak = 0;
    for (std::uint32_t i = 1; i < g_shakeCount; ++i) {
        if (ShakeIsStronger(g_shakeQ[weak], g_shakeQ[i])) weak = i;
    }
    if (ShakeIsStronger(r, g_shakeQ[weak])) {
        g_shakeQ[weak] = r;
    }
}

bool PopBestShake(ShakeRec& out) {
    BurstLockGuard lk;
    if (g_shakeCount == 0) return false;
    std::uint32_t best = 0;
    for (std::uint32_t i = 1; i < g_shakeCount; ++i) {
        if (ShakeIsStronger(g_shakeQ[i], g_shakeQ[best])) best = i;
    }
    out = g_shakeQ[best];
    g_shakeQ[best] = g_shakeQ[--g_shakeCount];
    return true;
}

void CaptureDmg(std::uint8_t kind, std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4,
                std::uint8_t b0, std::uint8_t b1) {
    DmgRec r{};
    r.gui = gui; r.mon = mon; r.dmg = dmg; r.kind = kind;
    r.a[0] = a0; r.a[1] = a1; r.a[2] = a2; r.a[3] = a3; r.a[4] = a4;
    r.b[0] = b0; r.b[1] = b1;
    r.hasPos = (pos != nullptr && CopyBytesSEH(r.pos, pos, sizeof(r.pos))) ? 1 : 0;
    r.hasMonPos = ReadUnitPosSEH(mon, r.monPosAtHit) ? 1 : 0;
    PushDmg(r);
}

void CaptureShake(std::uint8_t kind, std::uintptr_t cam, const void* param, std::uint32_t extra, const float* pos) {
    if (!param) return;
    ShakeRec r{};
    r.cam = cam; r.kind = kind; r.extra = extra;
    if (!CopyBytesSEH(r.param, param, kShakeParamSize)) return;
    r.hasPos = (pos != nullptr && CopyBytesSEH(r.pos, pos, sizeof(r.pos))) ? 1 : 0;
    PushShake(r);
}

} // namespace

void SetOrigBurstHook(int which, void* origFn) {
    switch (which) {
        case 0: g_origDmgNum      = reinterpret_cast<DmgNumFn>(origFn);   break;
        case 1: g_origDmgNumZorah = reinterpret_cast<DmgNumFn>(origFn);   break;
        case 2: g_origDmgNumEx    = reinterpret_cast<DmgNumExFn>(origFn); break;
        case 3: g_origHitEfc      = reinterpret_cast<HitEfcFn>(origFn);   break;
        case 4: g_origShake1      = reinterpret_cast<Shake1Fn>(origFn);   break;
        case 5: g_origShake2      = reinterpret_cast<Shake2Fn>(origFn);   break;
        case 6: g_origHitFeedback = reinterpret_cast<HitFbFn>(origFn);    break;
        default: break;
    }
}

void __fastcall Hook_BurstDmgNum(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                 std::uint32_t color, std::uint32_t crit, std::uint32_t buff,
                                 std::int32_t p8, std::uint8_t p9) {
    if (BurstCaptureActive()) {
        CaptureDmg(0, gui, mon, dmg, pos, color, crit, buff, static_cast<std::uint32_t>(p8), 0, p9, 0);
        return;
    }
    if (g_origDmgNum) g_origDmgNum(gui, mon, dmg, pos, color, crit, buff, p8, p9);
}

void __fastcall Hook_BurstDmgNumZorah(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                      std::uint32_t color, std::uint32_t crit, std::uint32_t buff,
                                      std::int32_t p8, std::uint8_t p9) {
    if (BurstCaptureActive()) {
        CaptureDmg(1, gui, mon, dmg, pos, color, crit, buff, static_cast<std::uint32_t>(p8), 0, p9, 0);
        return;
    }
    if (g_origDmgNumZorah) g_origDmgNumZorah(gui, mon, dmg, pos, color, crit, buff, p8, p9);
}

void __fastcall Hook_BurstDmgNumEx(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                   std::uint32_t p5, std::uint32_t p6, std::uint32_t p7, std::uint8_t p8,
                                   std::uint32_t p9, std::uint32_t p10, std::uint8_t p11) {
    if (BurstCaptureActive()) {
        CaptureDmg(2, gui, mon, dmg, pos, p5, p6, p7, p9, p10, p8, p11);
        return;
    }
    if (g_origDmgNumEx) g_origDmgNumEx(gui, mon, dmg, pos, p5, p6, p7, p8, p9, p10, p11);
}

void __fastcall Hook_BurstHitEfc(long long* ctx, std::uintptr_t epv, std::int32_t elemIdx,
                                 std::int32_t efxIdx, std::uintptr_t efcp) {
    if (BurstCaptureActive()) {
        VfxRec rec{};
        std::uintptr_t soundParam = 0;
        if (CaptureVfxSEH(ctx, epv, elemIdx, efxIdx, efcp, rec, soundParam)) {
            PushVfx(rec);
            if (HitEfcPoolCountSEH() >= kVfxPoolSafeLimit) {
                PlayEpvSoundImmediateSEH(g_exeBase, epv, elemIdx, efxIdx, rec.mon, soundParam);
                return;
            }
        }
    }
    if (g_origHitEfc) g_origHitEfc(ctx, epv, elemIdx, efxIdx, efcp);
}

std::int32_t __fastcall Hook_BurstShake1(std::uintptr_t cam, const void* param, const float* pos) {
    if (BurstCaptureActive()) {
        CaptureShake(1, cam, param, 0, pos);
    }
    return g_origShake1 ? g_origShake1(cam, param, pos) : -1;
}

std::int32_t __fastcall Hook_BurstShake2(std::uintptr_t cam, const void* param, std::uint32_t extra, const float* pos) {
    if (BurstCaptureActive()) {
        CaptureShake(2, cam, param, extra, pos);
    }
    return g_origShake2 ? g_origShake2(cam, param, extra, pos) : -1;
}

void __fastcall Hook_BurstHitFeedback(std::uintptr_t player, std::uintptr_t hitInfo, std::int32_t tier) {
    if (g_origHitFeedback) g_origHitFeedback(player, hitInfo, tier);
    if (BurstCaptureActive()) {
        BurstLockGuard lk;
        if (g_hitWeight < kHitWeightCap) ++g_hitWeight;
        if (tier > g_hitMaxTier) g_hitMaxTier = tier;
    }
}

void SetWorldFreezeBurst(bool enabled) {
    g_burstEnabled.store(enabled, std::memory_order_release);
    RefreshUnitMoveFlags();
    app::Log("freeze: World Freeze Delayed Burst = %s", enabled ? "ON" : "OFF");
}

bool ToggleWorldFreezeBurst() {
    const bool next = !g_burstEnabled.load(std::memory_order_relaxed);
    SetWorldFreezeBurst(next);
    return next;
}

bool IsWorldFreezeBurstEnabled() {
    return g_burstEnabled.load(std::memory_order_relaxed);
}

std::uint32_t BurstDeferredCount() {
    return g_dmgQueued.load(std::memory_order_relaxed);
}

void BurstBeginSession() {
    {
        BurstLockGuard lk;
        ResetBurstStateLocked();
    }
    g_burstDrainPending.store(false, std::memory_order_release);
}

void BurstOnWorldResume() {
    bool any = false;
    std::uint32_t dmgCnt = 0, vfxCnt = 0, shakeCnt = 0, weight = 0;
    {
        BurstLockGuard lk;
        any = BurstHasAnythingLocked();
        if (any) {
            g_drainTicks = 0;
            if (g_hitWeight != 0) {
                const std::uint32_t w = g_hitWeight < 10u ? g_hitWeight : 10u;
                const float tierBonus = (g_hitMaxTier >= 2) ? 0.02f : 0.0f;
                g_punchAmp = 0.04f + 0.008f * static_cast<float>(w) + tierBonus; // 4% .. 14%
                if (g_punchAmp > 0.14f) g_punchAmp = 0.14f;
                g_punchLeft = kPunchFrames;
            }
            dmgCnt = g_dmgCount;
            vfxCnt = g_vfxCount;
            shakeCnt = g_shakeCount;
            weight = g_hitWeight;
        }
    }
    g_burstDrainPending.store(any, std::memory_order_release);
    if (any) {
        app::Log("burst: World Freeze resumed -> replaying %u damage number(s), %u hit VFX, %u shake(s), hit weight %u",
                 dmgCnt, vfxCnt, shakeCnt, weight);
    }
}

void BurstAbort() {
    {
        BurstLockGuard lk;
        ResetBurstStateLocked();
    }
    g_burstDrainPending.store(false, std::memory_order_release);
    RefreshUnitMoveFlags();
}

void BurstDrainTick(std::uintptr_t exeBase) {
    if (!g_burstDrainPending.load(std::memory_order_acquire)) return;
    ++g_drainTicks;

    // 1. Damage numbers: paced replay while 20-slot HUD pool has free slots
    DmgRec front{};
    if (PeekPopDmg(front, false)) {
        const std::uint32_t maxDmg = (g_drainTicks == 1) ? kDmgInitialBurst : kDmgPerTick;
        const std::uint32_t freeSlots = CountFreeHudSlotsSEH(front.gui);
        std::uint32_t emitted = 0;
        while (emitted < maxDmg && emitted < freeSlots) {
            DmgRec r{};
            if (!PeekPopDmg(r, true)) break;
            ReplayDmgSEH(r);
            ++emitted;
        }
    }

    // 2. Hit VFX: paced replay under MoveLine[0x13] pool guard with non-clipping cascading sound
    if (HitEfcPoolCountSEH() < kVfxPoolSafeLimit) {
        bool frameSoundPlayed = false;
        for (std::uint32_t emitted = 0; emitted < kVfxPerTick; ++emitted) {
            VfxRec r{};
            if (!PopVfx(r)) break;
            bool allowSound = false;
            if (!frameSoundPlayed && HasVfxSound(r)) {
                if ((r.elemIdx >= 0x7D0 && r.elemIdx <= 0x7DB) || (((g_drainTicks & 1) == 1) && emitted == 0)) {
                    allowSound = true;
                    frameSoundPlayed = true;
                }
            }
            ReplayVfxSEH(exeBase, r, allowSound);
        }
    }

    // 3. Camera shakes: prioritized cascading replay synchronized with visual burst
    std::uint32_t dmgRem = 0, vfxRem = 0;
    {
        BurstLockGuard lk;
        dmgRem = g_dmgCount;
        vfxRem = g_vfxCount;
    }
    if (g_drainTicks == 1 || ((g_drainTicks % kShakeIntervalTicks) == 0 && (dmgRem > 0 || vfxRem > 0))) {
        ShakeRec s{};
        if (PopBestShake(s)) {
            ReplayShakeSEH(s);
        }
    }

    // 4. FOV punch: 2-frame snap-in + 14-frame cubic ease-out
    if (g_punchLeft > 0) {
        const int elapsed = kPunchFrames - g_punchLeft;
        float env = 0.0f;
        if (elapsed == 0) env = 0.65f;
        else if (elapsed == 1) env = 1.0f;
        else {
            const float t = static_cast<float>(g_punchLeft) / static_cast<float>(kPunchFrames - 2);
            env = t * t * t;
        }
        ApplyFovPunchSEH(exeBase, 1.0f - g_punchAmp * env);
        --g_punchLeft;
    }

    bool done = false;
    {
        BurstLockGuard lk;
        done = (g_dmgCount == 0 && g_vfxCount == 0 && g_punchLeft == 0) || g_drainTicks > kBurstTimeoutTicks;
        if (done) ResetBurstStateLocked();
    }
    if (done) {
        g_burstDrainPending.store(false, std::memory_order_release);
        RefreshUnitMoveFlags();
        app::Log("burst: replay finished");
    }
}

} // namespace freeze

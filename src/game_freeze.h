#pragma once
#include "log_config.h"
#include <cstdint>

namespace freeze {

enum class Mode : std::uint8_t {
    None  = 0, // Live normal gameplay
    Full  = 1, // [I] Entire game frozen via sMhMain
    World = 2  // [K] Everything except Player frozen via cUnit::genericMove
};

void InitModuleRange(std::uintptr_t exeBase, std::uintptr_t exeEnd);
void SetOrigUnitGenericMove(void* origFn);
void SetOrigHitDispatch(void* origFn);
void SetOrigGuardDispatch(void* origFn);

void __fastcall Hook_UnitGenericMove(std::uintptr_t unit);
int __fastcall Hook_HitDispatch(std::uintptr_t defenderColl,
                                std::uintptr_t hitInfo,
                                std::uint32_t atkAttr,
                                std::uint32_t defAttr,
                                std::uint32_t atkGroup,
                                std::uint32_t defGroup);
std::uint64_t __fastcall Hook_GuardDispatch(std::uintptr_t defenderColl,
                                            std::uintptr_t hitInfo);

bool Toggle(std::uintptr_t exeBase);
void SetFrozen(std::uintptr_t exeBase, bool frozen);

bool ToggleWorld(std::uintptr_t exeBase);
void SetWorldFrozen(std::uintptr_t exeBase, bool worldFrozen);

bool ToggleKeepHitboxesActive();
void SetKeepHitboxesActive(bool keepActive);
bool IsKeepHitboxesActive();

bool StepOneFrame(std::uintptr_t exeBase);
// Returns true if per-frame camera/freeze maintenance still needs Present ticks.
bool SyncActiveFrame(std::uintptr_t exeBase, bool anyModKeyHeld);

bool IsFrozen();
bool IsWorldFrozen();
Mode CurrentMode();

// ---- World Freeze Delayed Burst ----
// While World Freeze is active (and the option is enabled), damage numbers, hit VFX spawns,
// camera shakes and player hitstop are intercepted so engine pools (20 HUD slots, 8 shake slots,
// effect unit lines) can never be exhausted; they burst out when World Freeze resumes.
void SetWorldFreezeBurst(bool enabled);
bool ToggleWorldFreezeBurst();
bool IsWorldFreezeBurstEnabled();
std::uint32_t BurstDeferredCount(); // damage numbers currently waiting to burst out
void BurstBeginSession();           // called when World Freeze starts
void BurstOnWorldResume();          // called when World Freeze ends: arms the paced replay
void BurstAbort();                  // drop everything (stage change, detach)
void BurstDrainTick(std::uintptr_t exeBase); // sim-thread, once per frame, after resume

void SetOrigBurstHook(int which, void* origFn); // 0..6 in hook::kDynDmgNum..kDynHitFeedback order

void __fastcall Hook_BurstDmgNum(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                 std::uint32_t color, std::uint32_t crit, std::uint32_t buff,
                                 std::int32_t p8, std::uint8_t p9);
void __fastcall Hook_BurstDmgNumZorah(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                      std::uint32_t color, std::uint32_t crit, std::uint32_t buff,
                                      std::int32_t p8, std::uint8_t p9);
void __fastcall Hook_BurstDmgNumEx(std::uintptr_t gui, std::uintptr_t mon, std::int32_t dmg, const float* pos,
                                   std::uint32_t p5, std::uint32_t p6, std::uint32_t p7, std::uint8_t p8,
                                   std::uint32_t p9, std::uint32_t p10, std::uint8_t p11);
void __fastcall Hook_BurstHitEfc(long long* ctx, std::uintptr_t epv, std::int32_t elemIdx,
                                 std::int32_t efxIdx, std::uintptr_t efcp);
std::int32_t __fastcall Hook_BurstShake1(std::uintptr_t cam, const void* param, const float* pos);
std::int32_t __fastcall Hook_BurstShake2(std::uintptr_t cam, const void* param, std::uint32_t extra, const float* pos);
void __fastcall Hook_BurstHitFeedback(std::uintptr_t player, std::uintptr_t hitInfo, std::int32_t tier);

} // namespace freeze

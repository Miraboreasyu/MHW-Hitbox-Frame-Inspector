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

} // namespace freeze

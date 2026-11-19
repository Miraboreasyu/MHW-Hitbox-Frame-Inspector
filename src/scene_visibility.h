#pragma once
#include "log_config.h"
#include <cstdint>

namespace scene {

void InitModuleRange(std::uintptr_t exeBase, std::uintptr_t exeEnd) noexcept;
void SetOrigSUnitDraw(void* origFn) noexcept;

// Dynamic 5-byte hook target for sUnit::draw (0x14221A050).
// Stays 100% unpatched in .text when all scene/3D-mesh hide options are OFF!
void __fastcall Hook_SUnitDraw(std::uintptr_t unitSys, std::uintptr_t draw);

// Called on the Present thread when visibility toggles change or during active scene hiding.
void SyncState(std::uintptr_t exeBase,
               bool hideMonster,
               app::StageHideMode stageHide,
               app::BackdropMode backdrop) noexcept;

// Restores any engine hooks before DLL unload.
void Shutdown(std::uintptr_t exeBase) noexcept;

} // namespace scene

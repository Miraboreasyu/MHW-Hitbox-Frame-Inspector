#pragma once
#include "game_freeze.h"
#include "log_config.h"
#include <windows.h>
#include <cstdint>

namespace render {

// Called from cD3DRender::present hook before SwapChain->Present.
void OnPresent(std::uintptr_t exeBase,
               std::uintptr_t d3dRender,
               bool visEnabled,
               freeze::Mode freezeMode,
               const app::ViewFilterState& viewState,
               const app::Config& cfg,
               bool showStartupToast);

HWND GetGameWindow(std::uintptr_t d3dRender);
void Shutdown();

} // namespace render

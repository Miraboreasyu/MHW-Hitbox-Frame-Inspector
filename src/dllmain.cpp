// ============================================================================
//  MHW:I 15.23 Real-Time Collision/Hitbox Visualizer & Zero-Delay Freeze Mod
//  Loaded via Stracker's Loader: nativePC\plugins\debug_hitbox.dll
//
//  Default Hotkeys (customizable in optional debug_hitbox.ini):
//    U : Toggle real-time collision / hitbox / player frame state visualization
//    I : Instant zero-delay full game freeze / resume
//    K : Pause everything except the Player (World Freeze / Resume)
//    J : Toggle keeping single-trigger hitboxes active & repeatable while frozen
//    O : Advance +1 frame while frozen (works in both Full & World Freeze)
//    P : Cycle display filter (COMBAT -> HITBOX -> HURTBOX -> ALL)
// ============================================================================
#include "collision_collector.h"
#include "game_freeze.h"
#include "hook.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "overlay_renderer.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

app::Config g_cfg;
std::uintptr_t g_exeBase = 0;
std::atomic<bool> g_visEnabled{false};
std::atomic<int> g_filterMode{static_cast<int>(app::FilterMode::Combat)};
ULONGLONG g_firstPresentTick = 0;
bool g_loggedBackend = false;

struct KeyLatch {
    bool wasDown = false;
    bool pollEdge(int vk) {
        if (vk <= 0 || vk >= 256) return false;
        const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }
    void reset() { wasDown = false; }
};

KeyLatch g_keyVis;
KeyLatch g_keyFreeze;
KeyLatch g_keyWorldFreeze;
KeyLatch g_keyKeepHitbox;
KeyLatch g_keyStep;
KeyLatch g_keyFilter;

bool IsGameForeground(std::uintptr_t d3dRender) {
    const HWND fg = ::GetForegroundWindow();
    if (!fg) return false;
    const HWND gameHwnd = render::GetGameWindow(d3dRender);
    if (gameHwnd && fg == gameHwnd) return true;
    DWORD fgPid = 0;
    ::GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == ::GetCurrentProcessId();
}

bool PollHotkeys(std::uintptr_t d3dRender) {
    if (!IsGameForeground(d3dRender)) {
        g_keyVis.reset();
        g_keyFreeze.reset();
        g_keyWorldFreeze.reset();
        g_keyKeepHitbox.reset();
        g_keyStep.reset();
        g_keyFilter.reset();
        return false;
    }

    // Ignore plain letter hotkeys if Ctrl or Alt is held down
    const bool modHeld = ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                         ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    if (modHeld) return true;

    if (g_keyVis.pollEdge(g_cfg.vkToggleVis)) {
        const bool next = !g_visEnabled.load(std::memory_order_relaxed);
        g_visEnabled.store(next, std::memory_order_relaxed);
        app::Log("hotkey: Visualization %s (filter=%s)",
                 next ? "ON" : "OFF",
                 app::FilterModeName(static_cast<app::FilterMode>(g_filterMode.load(std::memory_order_relaxed))));
    }

    if (g_keyFreeze.pollEdge(g_cfg.vkToggleFreeze)) {
        freeze::Toggle(g_exeBase);
    }

    if (g_keyWorldFreeze.pollEdge(g_cfg.vkFreezeOthers)) {
        freeze::ToggleWorld(g_exeBase);
    }

    if (g_keyKeepHitbox.pollEdge(g_cfg.vkPersistHitbox)) {
        freeze::ToggleKeepHitboxesActive();
    }

    if (g_keyStep.pollEdge(g_cfg.vkStepFrame)) {
        if (freeze::CurrentMode() != freeze::Mode::None) {
            freeze::StepOneFrame(g_exeBase);
        }
    }

    if (g_keyFilter.pollEdge(g_cfg.vkCycleFilter)) {
        const int next = (g_filterMode.load(std::memory_order_relaxed) + 1) %
                         static_cast<int>(app::FilterMode::Count);
        g_filterMode.store(next, std::memory_order_relaxed);
        app::Log("hotkey: Filter mode -> %s",
                 app::FilterModeName(static_cast<app::FilterMode>(next)));
    }

    return true;
}

void OnD3DPresent(hook::Ctx* c) {
    const std::uintptr_t d3dRender = static_cast<std::uintptr_t>(c->rcx);
    if (!d3dRender || !g_exeBase) return;

    const ULONGLONG now = ::GetTickCount64();
    if (g_firstPresentTick == 0) {
        g_firstPresentTick = now;
    }
    if (!g_loggedBackend) {
        g_loggedBackend = true;
        const bool dx12 = (*reinterpret_cast<const std::uint8_t*>(g_exeBase + mhw::kRvaIsD3D12Byte) != 0);
        app::Log("present: first frame reached (d3dRender=%p, backend=%s)",
                 reinterpret_cast<void*>(d3dRender), dx12 ? "DirectX 12" : "DirectX 11");
    }

    const bool fg = PollHotkeys(d3dRender);
    freeze::SyncPerFrame(g_exeBase, g_cfg, fg);

    const bool showStartupToast = (now - g_firstPresentTick) < 6000ULL;
    const bool vis = g_visEnabled.load(std::memory_order_relaxed);
    const freeze::Mode freezeMode = freeze::CurrentMode();
    const auto mode = static_cast<app::FilterMode>(g_filterMode.load(std::memory_order_relaxed));

    render::OnPresent(g_exeBase, d3dRender, vis, freezeMode, mode, g_cfg, showStartupToast);
}

bool TryInstallHooks(HMODULE exe) {
    const auto base = reinterpret_cast<std::uintptr_t>(exe);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

    const std::uintptr_t presentSite = base + mhw::kRvaD3DPresent;
    const std::uintptr_t moveSite    = base + mhw::kRvaUnitGenericMove;
    if (!app::Readable(presentSite, sizeof(mhw::kPresentPrologue)) ||
        !app::Readable(moveSite, sizeof(mhw::kGenericMovePrologue))) {
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(presentSite),
                    mhw::kPresentPrologue,
                    sizeof(mhw::kPresentPrologue)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(moveSite),
                    mhw::kGenericMovePrologue,
                    sizeof(mhw::kGenericMovePrologue)) != 0) {
        return false; // SteamStub still encrypted or site modified
    }

    if (!hook::InitArena(base, end, 0x4000)) {
        app::Log("init: hook::InitArena failed near [%p..%p)",
                 reinterpret_cast<void*>(base), reinterpret_cast<void*>(end));
        return false;
    }

    freeze::InitModuleRange(base, end);

    void* origGenericMove = nullptr;
    if (!hook::InstallDetour(moveSite,
                             sizeof(mhw::kGenericMovePrologue),
                             reinterpret_cast<void*>(&freeze::Hook_UnitGenericMove),
                             &origGenericMove,
                             mhw::kGenericMovePrologue,
                             sizeof(mhw::kGenericMovePrologue))) {
        app::Log("init: InstallDetour(cUnit::genericMove) failed at %p", reinterpret_cast<void*>(moveSite));
        return false;
    }
    freeze::SetOrigUnitGenericMove(origGenericMove);

    // Install detours on hit/guard dispatchers so single-trigger frozen hitboxes never self-destruct on contact
    const std::uintptr_t hitSite   = base + mhw::kRvaHitDispatch;
    const std::uintptr_t guardSite = base + mhw::kRvaGuardDispatch;
    void* origHitDispatch = nullptr;
    if (hook::InstallDetour(hitSite,
                            sizeof(mhw::kHitDispatchPrologue),
                            reinterpret_cast<void*>(&freeze::Hook_HitDispatch),
                            &origHitDispatch,
                            mhw::kHitDispatchPrologue,
                            sizeof(mhw::kHitDispatchPrologue))) {
        freeze::SetOrigHitDispatch(origHitDispatch);
    } else {
        app::Log("init: optional detour on HitDispatch (%p) skipped (prologue modified)",
                 reinterpret_cast<void*>(hitSite));
    }

    void* origGuardDispatch = nullptr;
    if (hook::InstallDetour(guardSite,
                            sizeof(mhw::kGuardDispatchPrologue),
                            reinterpret_cast<void*>(&freeze::Hook_GuardDispatch),
                            &origGuardDispatch,
                            mhw::kGuardDispatchPrologue,
                            sizeof(mhw::kGuardDispatchPrologue))) {
        freeze::SetOrigGuardDispatch(origGuardDispatch);
    } else {
        app::Log("init: optional detour on GuardDispatch (%p) skipped (prologue modified)",
                 reinterpret_cast<void*>(guardSite));
    }

    if (!hook::InstallCtxHook(presentSite, sizeof(mhw::kPresentPrologue), &OnD3DPresent,
                              mhw::kPresentPrologue, sizeof(mhw::kPresentPrologue))) {
        app::Log("init: InstallCtxHook(cD3DRender::present) failed at %p", reinterpret_cast<void*>(presentSite));
        hook::RemoveAllHooks();
        return false;
    }

    g_exeBase = base;
    app::Log("init: hooked cD3DRender::present (%p), cUnit::genericMove (%p), HitDispatch (%p), GuardDispatch (%p) (arena=%p)",
             reinterpret_cast<void*>(presentSite),
             reinterpret_cast<void*>(moveSite),
             origHitDispatch ? reinterpret_cast<void*>(hitSite) : nullptr,
             origGuardDispatch ? reinterpret_cast<void*>(guardSite) : nullptr,
             reinterpret_cast<void*>(hook::ArenaBase()));
    return true;
}

DWORD WINAPI InitWorker(LPVOID) {
    app::InitLog();
    g_cfg = app::LoadConfig();
    if (!g_cfg.enabled) {
        app::Log("init: disabled via debug_hitbox.ini");
        return 0;
    }

    g_visEnabled.store(g_cfg.startVisOn, std::memory_order_relaxed);
    freeze::SetKeepHitboxesActive(g_cfg.keepHitboxesActive);

    HMODULE exe = ::GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!exe) exe = ::GetModuleHandleW(nullptr);
    app::Log("init: MonsterHunterWorld.exe base = %p", exe);
    if (!exe) return 1;

    // Wait up to 120s for SteamStub DRM to unpack .text
    for (int attempt = 0; attempt < 2400; ++attempt) {
        if (TryInstallHooks(exe)) {
            app::Log("init: ready! U=Toggle Hitboxes, I=Full Freeze, K=World Freeze, J=Keep Hitboxes Active, O=Step Frame, P=Cycle Filter");
            return 0;
        }
        ::Sleep(50);
    }

    const std::uintptr_t site = reinterpret_cast<std::uintptr_t>(exe) + mhw::kRvaD3DPresent;
    if (app::Readable(site, 12)) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(site);
        app::Log("init: timed out waiting for expected bytes at %p; actual=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 reinterpret_cast<void*>(site),
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
    } else {
        app::Log("init: timed out; site %p is not readable", reinterpret_cast<void*>(site));
    }
    return 1;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        HANDLE th = ::CreateThread(nullptr, 0, InitWorker, nullptr, 0, nullptr);
        if (th) ::CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        if (g_exeBase) {
            freeze::SetWorldFrozen(g_exeBase, false);
            freeze::SetFrozen(g_exeBase, false);
        }
        hook::RemoveAllHooks();
        render::Shutdown();
    }
    return TRUE;
}

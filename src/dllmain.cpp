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
//
//  Zero-Idle-Overhead Design:
//    - 0 background threads after attach.
//    - 0 per-frame Win32k/GetAsyncKeyState/GetForegroundWindow syscalls
//      (event-driven in-process WH_KEYBOARD hook on the game window thread).
//    - 0 simulation/worker hooks patched in .text during normal gameplay
//      (cUnit::genericMove & HitDispatch stay 100% vanilla bytes until Freeze!).
//    - Present idle path is a 1-byte relaxed check + tail-jump once per frame.
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

using D3DPresentFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t);

enum KeyActionBit : std::uint8_t {
    kKeyVis         = 0x01,
    kKeyFreeze      = 0x02,
    kKeyWorldFreeze = 0x04,
    kKeyKeepHitbox  = 0x08,
    kKeyStep        = 0x10,
    kKeyFilter      = 0x20
};

enum PresentWorkBit : std::uint8_t {
    kWorkInitKbHook   = 0x01,
    kWorkStartupToast = 0x02,
    kWorkHotkeyGuard  = 0x04,
    kWorkVisEnabled   = 0x08,
    kWorkFreezeActive = 0x10
};

HMODULE                   g_selfModule        = nullptr;
app::Config               g_cfg{};
std::uintptr_t            g_exeBase           = 0;
D3DPresentFn              g_origPresent       = nullptr;
HHOOK                     g_kbHook            = nullptr;
bool                      g_loggedBackend     = false;
bool                      g_useFallbackPoll   = false;
std::uint32_t             g_fallbackFrameDiv  = 0;
int                       g_startupFramesLeft = 210;
int                       g_heldWatchdog      = 0;
int                       g_filterMode        = static_cast<int>(app::FilterMode::Combat);

std::uint8_t              g_vkToAction[256]{};
int                       g_actionVks[6]{};
std::atomic<std::uint8_t> g_presentWorkMask{0};
std::atomic<std::uint8_t> g_edgeKeyMask{0};
std::atomic<std::uint8_t> g_heldKeyMask{0};

void BuildHotkeyTable() noexcept {
    std::memset(g_vkToAction, 0, sizeof(g_vkToAction));
    auto bind = [](int idx, int vk, std::uint8_t bit) {
        g_actionVks[idx] = vk;
        if (vk > 0 && vk < 256) {
            g_vkToAction[vk] |= bit;
        }
    };
    bind(0, g_cfg.vkToggleVis,     kKeyVis);
    bind(1, g_cfg.vkToggleFreeze,  kKeyFreeze);
    bind(2, g_cfg.vkFreezeOthers,  kKeyWorldFreeze);
    bind(3, g_cfg.vkPersistHitbox, kKeyKeepHitbox);
    bind(4, g_cfg.vkStepFrame,     kKeyStep);
    bind(5, g_cfg.vkCycleFilter,   kKeyFilter);
}

LRESULT CALLBACK GameKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam > 0 && wParam < 256) {
        const std::uint8_t bit = g_vkToAction[wParam];
        if (bit != 0) {
            const auto lp       = static_cast<std::uint32_t>(lParam);
            const bool isUp     = ((lp & 0x80000000U) != 0);
            const bool wasDown  = ((lp & 0x40000000U) != 0);
            const bool altHeld  = ((lp & 0x20000000U) != 0);
            if (isUp) {
                g_heldKeyMask.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_relaxed);
            } else if (!altHeld && (::GetKeyState(VK_CONTROL) & 0x8000) == 0) {
                g_heldKeyMask.fetch_or(bit, std::memory_order_relaxed);
                if (!wasDown) {
                    g_edgeKeyMask.fetch_or(bit, std::memory_order_release);
                }
                g_presentWorkMask.fetch_or(kWorkHotkeyGuard, std::memory_order_release);
            }
        }
    }
    return ::CallNextHookEx(g_kbHook, code, wParam, lParam);
}

void PollFallbackIfHookUnavailable(HWND gameHwnd) noexcept {
    // Only used if SetWindowsHookExW failed on an exotic environment
    if ((++g_fallbackFrameDiv & 7U) != 0) return;
    if (!gameHwnd || ::GetForegroundWindow() != gameHwnd) {
        g_heldKeyMask.store(0, std::memory_order_relaxed);
        return;
    }
    if (((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0)) {
        return;
    }
    const std::uint8_t prevHeld = g_heldKeyMask.load(std::memory_order_relaxed);
    std::uint8_t nowHeld = 0;
    static constexpr std::uint8_t kBits[6] = {
        kKeyVis, kKeyFreeze, kKeyWorldFreeze, kKeyKeepHitbox, kKeyStep, kKeyFilter
    };
    for (int i = 0; i < 6; ++i) {
        const int vk = g_actionVks[i];
        if (vk > 0 && vk < 256 && (::GetAsyncKeyState(vk) & 0x8000) != 0) {
            nowHeld |= kBits[i];
        }
    }
    g_heldKeyMask.store(nowHeld, std::memory_order_relaxed);
    const std::uint8_t rising = static_cast<std::uint8_t>(nowHeld & ~prevHeld);
    if (rising != 0) {
        g_edgeKeyMask.fetch_or(rising, std::memory_order_release);
    }
}

__declspec(noinline) void ProcessPresentWork(std::uintptr_t d3dRender) {
    if (!d3dRender || !g_exeBase) return;

    std::uint8_t work = g_presentWorkMask.load(std::memory_order_acquire);

    if ((work & kWorkInitKbHook) != 0) {
        if (!g_loggedBackend) {
            g_loggedBackend = true;
            const bool dx12 = (*reinterpret_cast<const std::uint8_t*>(g_exeBase + mhw::kRvaIsD3D12Byte) != 0);
            app::Log("present: first frame reached (d3dRender=%p, backend=%s)",
                     reinterpret_cast<void*>(d3dRender), dx12 ? "DirectX 12" : "DirectX 11");
        }
        const HWND hwnd = render::GetGameWindow(d3dRender);
        if (hwnd) {
            DWORD wndTid = ::GetWindowThreadProcessId(hwnd, nullptr);
            if (wndTid != 0) {
                g_kbHook = ::SetWindowsHookExW(WH_KEYBOARD, &GameKeyboardHookProc, g_selfModule, wndTid);
                if (g_kbHook) {
                    g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkInitKbHook), std::memory_order_release);
                    app::Log("input: event-driven WH_KEYBOARD hook active (tid=%lu, zero per-frame polling!)", wndTid);
                } else {
                    g_useFallbackPoll = true;
                    app::Log("input: WH_KEYBOARD unavailable (err=%lu), using low-rate fallback", ::GetLastError());
                }
            }
        }
    }

    if (g_useFallbackPoll) {
        PollFallbackIfHookUnavailable(render::GetGameWindow(d3dRender));
    }

    const std::uint8_t edges = g_edgeKeyMask.exchange(0, std::memory_order_acq_rel);
    std::uint8_t held        = g_heldKeyMask.load(std::memory_order_relaxed);

    // Clear any stale held-bit if user Alt-Tabbed while holding a hotkey
    if (held != 0) {
        if (++g_heldWatchdog >= 16) {
            g_heldWatchdog = 0;
            std::uint8_t verified = 0;
            static constexpr std::uint8_t kBits[6] = {
                kKeyVis, kKeyFreeze, kKeyWorldFreeze, kKeyKeepHitbox, kKeyStep, kKeyFilter
            };
            for (int i = 0; i < 6; ++i) {
                const int vk = g_actionVks[i];
                if ((held & kBits[i]) != 0 && vk > 0 && vk < 256 && (::GetAsyncKeyState(vk) & 0x8000) != 0) {
                    verified |= kBits[i];
                }
            }
            held = verified;
            g_heldKeyMask.store(verified, std::memory_order_relaxed);
        }
    } else {
        g_heldWatchdog = 0;
    }

    if (edges != 0) {
        // Arm camera-nudge guard BEFORE toggling freeze
        freeze::SyncActiveFrame(g_exeBase, true);

        if ((edges & kKeyVis) != 0) {
            const bool wasVis = ((g_presentWorkMask.load(std::memory_order_relaxed) & kWorkVisEnabled) != 0);
            const bool nextVis = !wasVis;
            if (nextVis) g_presentWorkMask.fetch_or(kWorkVisEnabled, std::memory_order_release);
            else         g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkVisEnabled), std::memory_order_release);
            app::Log("hotkey: Visualization %s (filter=%s)",
                     nextVis ? "ON" : "OFF",
                     app::FilterModeName(static_cast<app::FilterMode>(g_filterMode)));
        }

        if ((edges & kKeyFreeze) != 0) {
            freeze::Toggle(g_exeBase);
        }

        if ((edges & kKeyWorldFreeze) != 0) {
            freeze::ToggleWorld(g_exeBase);
        }

        if ((edges & kKeyKeepHitbox) != 0) {
            freeze::ToggleKeepHitboxesActive();
        }

        if ((edges & kKeyStep) != 0) {
            if (freeze::CurrentMode() != freeze::Mode::None) {
                freeze::StepOneFrame(g_exeBase);
            }
        }

        if ((edges & kKeyFilter) != 0) {
            g_filterMode = (g_filterMode + 1) % static_cast<int>(app::FilterMode::Count);
            app::Log("hotkey: Filter mode -> %s",
                     app::FilterModeName(static_cast<app::FilterMode>(g_filterMode)));
        }
    }

    const bool anyHeldOrEdges = (edges != 0) || (held != 0);
    const bool needFreezeSync = freeze::SyncActiveFrame(g_exeBase, anyHeldOrEdges);
    const freeze::Mode freezeMode = freeze::CurrentMode();

    if (freezeMode != freeze::Mode::None) {
        g_presentWorkMask.fetch_or(kWorkFreezeActive, std::memory_order_release);
    } else {
        g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkFreezeActive), std::memory_order_release);
    }

    if (!needFreezeSync && !g_useFallbackPoll && g_edgeKeyMask.load(std::memory_order_relaxed) == 0) {
        g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkHotkeyGuard), std::memory_order_release);
    }

    bool showStartupToast = false;
    if ((g_presentWorkMask.load(std::memory_order_relaxed) & kWorkStartupToast) != 0) {
        if (g_startupFramesLeft > 0) {
            --g_startupFramesLeft;
            showStartupToast = true;
        } else {
            g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkStartupToast), std::memory_order_release);
        }
    }

    const bool vis = ((g_presentWorkMask.load(std::memory_order_relaxed) & kWorkVisEnabled) != 0);
    if (vis || freezeMode != freeze::Mode::None || showStartupToast) {
        render::OnPresent(g_exeBase,
                          d3dRender,
                          vis,
                          freezeMode,
                          static_cast<app::FilterMode>(g_filterMode),
                          g_cfg,
                          showStartupToast);
    }
}

std::uint64_t __fastcall Hook_D3DPresent(std::uintptr_t d3dRender,
                                         std::uint64_t rdx,
                                         std::uint64_t r8,
                                         std::uint64_t r9) {
    // 1-byte relaxed check + tail-jump when idle!
    if (g_presentWorkMask.load(std::memory_order_relaxed) != 0) {
        ProcessPresentWork(d3dRender);
    }
    return g_origPresent(d3dRender, rdx, r8, r9);
}

bool HasValidOrChainedPrologue(std::uintptr_t site, const std::uint8_t* expected, std::size_t n) {
    if (!app::Readable(site, n)) return false;
    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    if (std::memcmp(cur, expected, n) == 0) return true;
    return cur[0] == 0xE9;
}

bool TryInstallHooks(HMODULE exe) {
    const auto base = reinterpret_cast<std::uintptr_t>(exe);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

    const std::uintptr_t presentSite = base + mhw::kRvaD3DPresent;
    const std::uintptr_t moveSite    = base + mhw::kRvaUnitGenericMove;
    const std::uintptr_t hitSite     = base + mhw::kRvaHitDispatch;
    const std::uintptr_t guardSite   = base + mhw::kRvaGuardDispatch;

    if (!app::Readable(presentSite, sizeof(mhw::kPresentPrologue)) ||
        !app::Readable(moveSite, sizeof(mhw::kGenericMovePrologue))) {
        return false;
    }
    // Wait for SteamStub unpack by verifying bytes [5..11] of Present (untouched even if chained)
    if (std::memcmp(reinterpret_cast<const void*>(presentSite + 5),
                    mhw::kPresentPrologue + 5,
                    sizeof(mhw::kPresentPrologue) - 5) != 0 ||
        !HasValidOrChainedPrologue(presentSite, mhw::kPresentPrologue, 5) ||
        !HasValidOrChainedPrologue(moveSite, mhw::kGenericMovePrologue, 5)) {
        return false;
    }

    if (!hook::InitArena(base, end)) {
        app::Log("init: hook::InitArena failed near [%p..%p)",
                 reinterpret_cast<void*>(base), reinterpret_cast<void*>(end));
        return false;
    }

    void* origPresent = nullptr;
    if (!hook::PreparePermanentDetour5(presentSite,
                                       reinterpret_cast<void*>(&Hook_D3DPresent),
                                       &origPresent,
                                       mhw::kPresentPrologue)) {
        app::Log("init: PreparePermanentDetour5(Present) failed at %p", reinterpret_cast<void*>(presentSite));
        return false;
    }
    g_origPresent = reinterpret_cast<D3DPresentFn>(origPresent);

    void* origGenericMove = nullptr;
    if (hook::PrepareDynamicHook5(hook::kDynGenericMove,
                                  moveSite,
                                  reinterpret_cast<void*>(&freeze::Hook_UnitGenericMove),
                                  &origGenericMove,
                                  mhw::kGenericMovePrologue)) {
        freeze::SetOrigUnitGenericMove(origGenericMove);
    }

    void* origHitDispatch = nullptr;
    if (HasValidOrChainedPrologue(hitSite, mhw::kHitDispatchPrologue, 5) &&
        hook::PrepareDynamicHook5(hook::kDynHitDispatch,
                                  hitSite,
                                  reinterpret_cast<void*>(&freeze::Hook_HitDispatch),
                                  &origHitDispatch,
                                  mhw::kHitDispatchPrologue)) {
        freeze::SetOrigHitDispatch(origHitDispatch);
    }

    void* origGuardDispatch = nullptr;
    if (HasValidOrChainedPrologue(guardSite, mhw::kGuardDispatchPrologue, sizeof(mhw::kGuardDispatchPrologue)) &&
        hook::PrepareLazyGuardHook(guardSite,
                                   sizeof(mhw::kGuardDispatchPrologue),
                                   reinterpret_cast<void*>(&freeze::Hook_GuardDispatch),
                                   &origGuardDispatch,
                                   mhw::kGuardDispatchPrologue,
                                   sizeof(mhw::kGuardDispatchPrologue))) {
        freeze::SetOrigGuardDispatch(origGuardDispatch);
    }

    g_exeBase = base;
    freeze::InitModuleRange(base, end);
    freeze::SetKeepHitboxesActive(g_cfg.keepHitboxesActive);

    std::uint8_t initialWork = kWorkInitKbHook;
    if (g_cfg.showHudBanner) initialWork |= kWorkStartupToast;
    if (g_cfg.startVisOn)    initialWork |= kWorkVisEnabled;
    g_presentWorkMask.store(initialWork, std::memory_order_release);

    if (!hook::FinalizeAndInstallPresentHook()) {
        app::Log("init: FinalizeAndInstallPresentHook failed");
        g_exeBase = 0;
        return false;
    }

    app::Log("init: hooked ONLY Present (%p); genericMove (%p) & HitDispatch (%p) stay 100%% vanilla until Freeze! (arena=%p)",
             reinterpret_cast<void*>(presentSite),
             reinterpret_cast<void*>(moveSite),
             reinterpret_cast<void*>(hitSite),
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

    BuildHotkeyTable();

    HMODULE exe = ::GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!exe) exe = ::GetModuleHandleW(nullptr);
    app::Log("init: MonsterHunterWorld.exe base = %p", exe);
    if (!exe) return 1;

    for (int attempt = 0; attempt < 2400; ++attempt) {
        if (TryInstallHooks(exe)) {
            app::Log("init: ready! U=Toggle Hitboxes, I=Full Freeze, K=World Freeze, J=Keep Hitboxes, O=Step Frame, P=Cycle Filter (0 background threads)");
            return 0;
        }
        ::Sleep(50);
    }

    app::Log("init: timed out waiting for MonsterHunterWorld.exe unpack");
    return 1;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        ::DisableThreadLibraryCalls(module);
        HANDLE th = ::CreateThread(nullptr, 0, InitWorker, nullptr, 0, nullptr);
        if (th) ::CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        if (g_kbHook) {
            ::UnhookWindowsHookEx(g_kbHook);
            g_kbHook = nullptr;
        }
        if (g_exeBase) {
            freeze::SetWorldFrozen(g_exeBase, false);
            freeze::SetFrozen(g_exeBase, false);
        }
        hook::RemoveAllHooks();
        render::Shutdown();
        app::CloseLog();
    }
    return TRUE;
}

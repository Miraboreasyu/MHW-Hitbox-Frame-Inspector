// ============================================================================
//  MHW Hitbox & Frame Inspector (MHW:I 15.23, DX11 + DX12 + HDR)
//  Loaded via Stracker's Loader: nativePC\plugins\mhw_hfi.dll
//
//  Default Hotkeys (customizable in optional mhw_hfi.ini):
//    U       : Master toggle for collision / scene inspection view
//    I       : Instant zero-delay full game freeze / resume
//    K       : Pause everything except the Player (World Freeze / Resume)
//    J       : Toggle keeping single-trigger hitboxes active & repeatable while frozen
//    O       : Advance +1 frame while frozen (works in both Full & World Freeze)
//    P       : Open / close interactive Filter & Scene Panel (Shift+P quick-cycles preset)
//
//  Zero-Idle-Overhead Design:
//    - 0 background threads after attach.
//    - 0 per-frame Win32k/GetAsyncKeyState/GetForegroundWindow syscalls
//      (event-driven in-process WH_KEYBOARD hook on the game window thread).
//    - 0 simulation/render hooks patched in .text during normal gameplay
//      (genericMove, HitDispatch & sUnit::draw stay 100% vanilla until engaged!).
//    - Present idle path is a 1-byte relaxed check + tail-jump once per frame.
// ============================================================================
#include "collision_collector.h"
#include "game_freeze.h"
#include "hook.h"
#include "log_config.h"
#include "mhw_addresses.h"
#include "overlay_renderer.h"
#include "scene_visibility.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {

using D3DPresentFn     = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t);
using KbUpdateStateFn  = void(__fastcall*)(std::uintptr_t, std::uint32_t*);
using ChatDispatchFn   = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint8_t);
using PadBuildMergedFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);

struct RawXInputGamepad {
    std::uint16_t wButtons;
    std::uint8_t  bLeftTrigger;
    std::uint8_t  bRightTrigger;
    std::int16_t  sThumbLX;
    std::int16_t  sThumbLY;
    std::int16_t  sThumbRX;
    std::int16_t  sThumbRY;
};
struct RawXInputState {
    std::uint32_t    dwPacketNumber;
    RawXInputGamepad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, RawXInputState*);

enum KeyActionBit : std::uint8_t {
    kKeyVis           = 0x01,
    kKeyFreeze        = 0x02,
    kKeyWorldFreeze   = 0x04,
    kKeyKeepHitbox    = 0x08,
    kKeyStep          = 0x10,
    kKeyFilter        = 0x20,
    kKeyQuickPreset   = 0x40,
    kKeyToggleOverlay = 0x80
};

enum PresentWorkBit : std::uint8_t {
    kWorkInitKbHook   = 0x01,
    kWorkStartupToast = 0x02,
    kWorkHotkeyGuard  = 0x04,
    kWorkVisEnabled   = 0x08,
    kWorkFreezeActive = 0x10,
    kWorkPanelOpen    = 0x20,
    kWorkFallbackPoll = 0x40
};

enum PanelCmdBit : std::uint32_t {
    kPanelUp         = 1u << 0,
    kPanelDown       = 1u << 1,
    kPanelLeft       = 1u << 2,
    kPanelRight      = 1u << 3,
    kPanelEnter      = 1u << 4,
    kPanelClose      = 1u << 5,
    kPanelFlipScope  = 1u << 6,
    kPanelSwitchPage = 1u << 7,
    kPanelRow1       = 1u << 8,
    kPanelRow2       = 1u << 9,
    kPanelRow3       = 1u << 10,
    kPanelRow4       = 1u << 11,
    kPanelRow5       = 1u << 12,
    kPanelRow6       = 1u << 13,
    kPanelRow7       = 1u << 14,
    kPanelRow8       = 1u << 15,
    kPanelRow9       = 1u << 16,
    kPanelRow10      = 1u << 17,
    kPanelRow11      = 1u << 18,
    kPanelRow12      = 1u << 19,
    kPanelRow13      = 1u << 20,
    kPanelRow14      = 1u << 21
};

HMODULE                    g_selfModule        = nullptr;
app::Config                g_cfg{};
app::ViewFilterState       g_viewState{};
std::uintptr_t             g_exeBase           = 0;
D3DPresentFn               g_origPresent        = nullptr;
KbUpdateStateFn            g_origKbUpdateState  = nullptr;
ChatDispatchFn             g_origChatDispatch   = nullptr;
PadBuildMergedFn           g_origPadBuildMerged = nullptr;
HHOOK                      g_kbHook             = nullptr;
bool                       g_loggedBackend      = false;
bool                       g_useFallbackPoll    = false;
std::uint32_t              g_fallbackFrameDiv   = 0;
int                        g_startupFramesLeft  = 210;
int                        g_heldWatchdog       = 0;

struct GamepadPanelNavState {
    std::uint32_t prevBtns       = 0;
    std::uint32_t repeatBtn      = 0;
    std::uint64_t nextRepeatTick = 0;

    void Reset() noexcept {
        prevBtns       = 0;
        repeatBtn      = 0;
        nextRepeatTick = 0;
    }
};

constexpr std::uint32_t kPadPanelConsumeMask =
    mhw::kPadBitDpadUp | mhw::kPadBitDpadRight | mhw::kPadBitDpadDown | mhw::kPadBitDpadLeft | mhw::kPadBitA;

GamepadPanelNavState       g_padHookNav{};
bool                       g_padHookWasOpen     = false;
std::uint32_t              g_padSwallowHoldMask = 0;
std::atomic<std::uint32_t> g_padHookHeartbeat{0};
std::uint32_t              g_lastSeenPadBeat    = 0;
std::uint32_t              g_padBeatStallFrames = 0;
GamepadPanelNavState       g_xinputFallbackNav{};
XInputGetStateFn           g_xinputGetState     = nullptr;
bool                       g_xinputResolved     = false;

std::mutex                 g_liveRuntimeCfgMx;
app::ChatCommandConfig     g_liveChatCfg{};
std::atomic<bool>          g_gamepadPanelEnabled{true};

std::uint8_t               g_vkToAction[256]{};
int                        g_actionVks[7]{};
std::atomic<std::uint8_t>  g_presentWorkMask{0};
std::atomic<std::uint8_t>  g_edgeKeyMask{0};
std::atomic<std::uint8_t>  g_heldKeyMask{0};
std::atomic<bool>          g_panelOpenAtomic{false};
std::atomic<std::uint32_t> g_panelCmdMask{0};
std::atomic<std::uint8_t>  g_suppressedCamVkMask{0};

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
    bind(6, g_cfg.vkToggleOverlay, kKeyToggleOverlay);
}

std::uint32_t MapVkToPanelCmd(WPARAM vk) noexcept {
    switch (vk) {
        case VK_UP:       return kPanelUp;
        case VK_DOWN:     return kPanelDown;
        case VK_LEFT:     return kPanelLeft;
        case VK_RIGHT:    return kPanelRight;
        case VK_RETURN:
        case VK_SPACE:    return kPanelEnter;
        case VK_ESCAPE:
        case VK_BACK:     return kPanelClose;
        case VK_TAB:      return kPanelSwitchPage;
        case VK_OEM_PLUS:
        case VK_ADD:      return kPanelFlipScope;
        case '1': case VK_NUMPAD1: return kPanelRow1;
        case '2': case VK_NUMPAD2: return kPanelRow2;
        case '3': case VK_NUMPAD3: return kPanelRow3;
        case '4': case VK_NUMPAD4: return kPanelRow4;
        case '5': case VK_NUMPAD5: return kPanelRow5;
        case '6': case VK_NUMPAD6: return kPanelRow6;
        case '7': case VK_NUMPAD7: return kPanelRow7;
        case '8': case VK_NUMPAD8: return kPanelRow8;
        case '9': case VK_NUMPAD9: return kPanelRow9;
        case '0': case VK_NUMPAD0: return kPanelRow10;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:          return kPanelRow11;
        case VK_OEM_2:
        case VK_DIVIDE:            return kPanelRow12;
        case VK_OEM_COMMA:         return kPanelRow13;
        case VK_OEM_PERIOD:
        case VK_DECIMAL:           return kPanelRow14;
        default:                   return 0;
    }
}

bool IsGameTextInputActive(std::uintptr_t exeBase) noexcept {
    if (!exeBase) return false;
    __try {
        const std::uintptr_t mhInputText = *reinterpret_cast<const std::uintptr_t*>(
            exeBase + mhw::kRvaSMhInputTextInstance);
        if (mhInputText < 0x100000ULL || mhInputText > 0x00007FFFFFFF0000ULL || (mhInputText & 0x7ULL) != 0) {
            return false;
        }
        // 0x1404810f0 (WM_KEYDOWN/WM_CHAR): +0x1DA00 is 0 ONLY while an input box holds focus
        // (set to 0 in 0x140482e00 on focus gain, set to 1 in 0x140482f80 on focus loss).
        if (*reinterpret_cast<const std::uint8_t*>(mhInputText + mhw::kOffMhInputTextUnfocused) != 0) {
            return false;
        }
        const std::uintptr_t editor = *reinterpret_cast<const std::uintptr_t*>(
            mhInputText + mhw::kOffInputTextEditor);
        if (editor < 0x100000ULL || editor > 0x00007FFFFFFF0000ULL || (editor & 0x7ULL) != 0) {
            return false;
        }
        // 0x14239f460 -> 0x1423af770: state == 1 is STANDBY (chat summonable but closed!);
        // ONLY state == 2 means the chat / naming / search input box is actually open & typing!
        return (*reinterpret_cast<const std::int32_t*>(editor + mhw::kOffInputEditorState) == 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline std::uint8_t BlockedCameraBit(WPARAM vk) noexcept {
    if (!g_cfg.blockCameraKeys) return 0;
    switch (vk) {
        case 'O':      return 0x01;
        case 'K':      return 0x02;
        case 'L':      return 0x04;
        case VK_OEM_1: return 0x08;
        default:       return 0;
    }
}

void __fastcall Hook_KeyboardUpdateState(std::uintptr_t kb, std::uint32_t* keyBits) {
    g_origKbUpdateState(kb, keyBits);
    if (!keyBits) return;

    const bool blockCam  = g_cfg.blockCameraKeys;
    const bool panelOpen = g_panelOpenAtomic.load(std::memory_order_relaxed);
    if ((!blockCam && !panelOpen) || IsGameTextInputActive(g_exeBase)) return;

    auto clearVk = [&](int vk) noexcept {
        if (vk > 0 && vk < 256) {
            keyBits[vk >> 5] &= ~(1u << (vk & 31));
        }
    };

    if (blockCam) {
        clearVk('O');
        clearVk('K');
        clearVk('L');
        clearVk(VK_OEM_1); // ';'
    }

    if (panelOpen) {
        clearVk(g_cfg.vkCycleFilter);
        clearVk(VK_TAB);
        clearVk(VK_UP);
        clearVk(VK_DOWN);
        clearVk(VK_LEFT);
        clearVk(VK_RIGHT);
        clearVk(VK_RETURN);
        clearVk(VK_SPACE);
        clearVk(VK_ESCAPE);
        clearVk(VK_BACK);
        clearVk(VK_OEM_PLUS);
        clearVk(VK_ADD);
        clearVk(VK_OEM_MINUS);
        clearVk(VK_SUBTRACT);
        clearVk(VK_OEM_2);
        clearVk(VK_DIVIDE);
        clearVk(VK_OEM_COMMA);
        clearVk(VK_OEM_PERIOD);
        clearVk(VK_DECIMAL);
        for (int k = '0'; k <= '9'; ++k) clearVk(k);
        for (int k = VK_NUMPAD0; k <= VK_NUMPAD9; ++k) clearVk(k);
    }
}

bool ReadLocalChatMessageSEH(std::uintptr_t netMsg,
                             char outText[129],
                             int& outMsgType) noexcept {
    __try {
        if (netMsg < 0x100000ULL || netMsg > 0x00007FFFFFFF0000ULL) return false;
        const auto msgType = static_cast<int>(*reinterpret_cast<const std::int8_t*>(netMsg + mhw::kOffChatNetMsgType));
        // Only explicit player-sent chat (1 = typed chat, 2/3 = manual & radial menu custom shoutouts).
        // Excludes auto-shoutouts (4) and stickers (5).
        if (msgType != 1 && msgType != 2 && msgType != 3) return false;

        const auto* src = reinterpret_cast<const char*>(netMsg + mhw::kOffChatNetMsgText);
        std::size_t n = 0;
        while (n < 128U && src[n] != '\0') {
            outText[n] = src[n];
            ++n;
        }
        outText[n] = '\0';
        if (n == 0) return false;
        outMsgType = msgType;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ExactChatStringEquals(const char* msg, const std::string& pattern, bool caseSensitive) noexcept {
    if (pattern.empty()) return false;
    const std::size_t len = pattern.size();
    if (caseSensitive) {
        return std::strncmp(msg, pattern.c_str(), len) == 0 && msg[len] == '\0';
    }
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char a = static_cast<unsigned char>(msg[i]);
        unsigned char b = static_cast<unsigned char>(pattern[i]);
        if (a == 0) return false;
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return msg[len] == '\0';
}

void RefreshHotRuntimeConfig() {
    std::lock_guard<std::mutex> lk(g_liveRuntimeCfgMx);
    bool padEn = g_gamepadPanelEnabled.load(std::memory_order_relaxed);
    if (app::HotReloadRuntimeConfig(g_liveChatCfg, padEn)) {
        g_gamepadPanelEnabled.store(padEn, std::memory_order_release);
    }
}

bool MatchChatCommandHot(const char* msg, std::uint8_t& outActionBit, const char*& outActionName) {
    std::lock_guard<std::mutex> lk(g_liveRuntimeCfgMx);
    bool padEn = g_gamepadPanelEnabled.load(std::memory_order_relaxed);
    if (app::HotReloadRuntimeConfig(g_liveChatCfg, padEn)) {
        g_gamepadPanelEnabled.store(padEn, std::memory_order_release);
    }
    if (!g_liveChatCfg.enabled) return false;

    const bool cs = g_liveChatCfg.caseSensitive;
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdVis, cs)) {
        outActionBit  = kKeyVis;
        outActionName = "U (Vis)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdFreeze, cs)) {
        outActionBit  = kKeyFreeze;
        outActionName = "I (FullFreeze)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdWorldFreeze, cs)) {
        outActionBit  = kKeyWorldFreeze;
        outActionName = "K (WorldFreeze)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdStep, cs)) {
        outActionBit  = kKeyStep;
        outActionName = "O (StepFrame)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdKeepHitbox, cs)) {
        outActionBit  = kKeyKeepHitbox;
        outActionName = "J (KeepHitbox)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdPanel, cs)) {
        outActionBit  = kKeyFilter;
        outActionName = "P (Panel)";
        return true;
    }
    if (ExactChatStringEquals(msg, g_liveChatCfg.cmdOverlay, cs)) {
        outActionBit  = kKeyToggleOverlay;
        outActionName = "\\ (Overlay)";
        return true;
    }
    return false;
}

void __fastcall Hook_ChatDispatch(std::uintptr_t sChat,
                                  std::uintptr_t netMsg,
                                  std::uint32_t targetChannel,
                                  std::uint8_t isLocal) {
    // Strictly local player only (network incoming messages from other players pass isLocal == 0)
    char msgBuf[129]{};
    int msgType = 0;
    const bool hasLocalText = (isLocal != 0 && netMsg != 0) &&
                              ReadLocalChatMessageSEH(netMsg, msgBuf, msgType);

    if (g_origChatDispatch) {
        g_origChatDispatch(sChat, netMsg, targetChannel, isLocal);
    }

    if (hasLocalText) {
        std::uint8_t actionBit = 0;
        const char* actionName = "";
        if (MatchChatCommandHot(msgBuf, actionBit, actionName)) {
            g_edgeKeyMask.fetch_or(actionBit, std::memory_order_release);
            g_presentWorkMask.fetch_or(kWorkHotkeyGuard, std::memory_order_release);
            app::Log("chat: exact-match \"%s\" (type=%d) -> %s", msgBuf, msgType, actionName);
        }
    }
}

void EmitGamepadPanelCmds(GamepadPanelNavState& st, std::uint32_t curBtns) noexcept {
    const std::uint32_t rising = curBtns & ~st.prevBtns;
    st.prevBtns = curBtns;

    std::uint32_t pcmd = 0;
    if ((rising & mhw::kPadBitDpadUp) != 0)    pcmd |= kPanelUp;
    if ((rising & mhw::kPadBitDpadDown) != 0)  pcmd |= kPanelDown;
    if ((rising & mhw::kPadBitDpadLeft) != 0)  pcmd |= kPanelLeft;
    if ((rising & mhw::kPadBitDpadRight) != 0) pcmd |= kPanelRight;
    if ((rising & mhw::kPadBitA) != 0)         pcmd |= kPanelEnter;

    // Auto-repeat on D-Pad Up/Down only (matches keyboard Up/Down auto-repeat behavior)
    static constexpr std::uint32_t kRepeatDirMask = mhw::kPadBitDpadUp | mhw::kPadBitDpadDown;
    const std::uint32_t heldDir = curBtns & kRepeatDirMask;
    const std::uint64_t now = ::GetTickCount64();
    if ((rising & kRepeatDirMask) != 0) {
        st.repeatBtn = ((rising & mhw::kPadBitDpadUp) != 0) ? mhw::kPadBitDpadUp : mhw::kPadBitDpadDown;
        st.nextRepeatTick = now + 360ULL;
    } else if (st.repeatBtn != 0 && (heldDir & st.repeatBtn) != 0) {
        if (now >= st.nextRepeatTick) {
            pcmd |= (st.repeatBtn == mhw::kPadBitDpadUp) ? kPanelUp : kPanelDown;
            st.nextRepeatTick = now + 95ULL;
        }
    } else {
        st.repeatBtn = 0;
    }

    if (pcmd != 0) {
        g_panelCmdMask.fetch_or(pcmd, std::memory_order_release);
        g_presentWorkMask.fetch_or(kWorkHotkeyGuard, std::memory_order_release);
    }
}

void HandlePadBuildMergedSEH(std::uintptr_t padInfo, bool panelOpen) noexcept {
    __try {
        if (padInfo < 0x100000ULL || padInfo > 0x00007FFFFFFF0000ULL) return;
        auto* pOn  = reinterpret_cast<std::uint32_t*>(padInfo + mhw::kOffPadInfoOn);
        auto* pTrg = reinterpret_cast<std::uint32_t*>(padInfo + mhw::kOffPadInfoTrg);
        auto* pRel = reinterpret_cast<std::uint32_t*>(padInfo + mhw::kOffPadInfoRel);
        auto* pRep = reinterpret_cast<std::uint32_t*>(padInfo + mhw::kOffPadInfoRep);

        const std::uint32_t mOn = *pOn;
        if (panelOpen) {
            g_padHookHeartbeat.fetch_add(1U, std::memory_order_relaxed);
            // Pass through untouched when typing in chat/IME or while holding LB (Radial Menu wheel switch)
            if (IsGameTextInputActive(g_exeBase) || (mOn & mhw::kPadBitLB) != 0U) {
                g_padHookNav.Reset();
                g_padHookWasOpen = false;
                if (g_padSwallowHoldMask != 0U) {
                    const std::uint32_t mask = g_padSwallowHoldMask;
                    *pOn  &= ~mask;
                    *pTrg &= ~mask;
                    *pRel &= ~mask;
                    *pRep &= ~mask;
                    g_padSwallowHoldMask &= mOn;
                }
                return;
            }
            const std::uint32_t curBtns = mOn & kPadPanelConsumeMask;
            if (!g_padHookWasOpen) {
                // Seed already-held buttons on the first frame panel opens to prevent phantom triggers
                g_padHookWasOpen = true;
                g_padHookNav.prevBtns = curBtns;
                g_padHookNav.repeatBtn = 0;
            } else {
                EmitGamepadPanelCmds(g_padHookNav, curBtns);
            }
            g_padSwallowHoldMask |= curBtns;
            *pOn  &= ~kPadPanelConsumeMask;
            *pTrg &= ~kPadPanelConsumeMask;
            *pRel &= ~kPadPanelConsumeMask;
            *pRep &= ~kPadPanelConsumeMask;
        } else {
            g_padHookWasOpen = false;
            g_padHookNav.Reset();
            if (g_padSwallowHoldMask != 0U) {
                const std::uint32_t mask = g_padSwallowHoldMask;
                *pOn  &= ~mask;
                *pTrg &= ~mask;
                *pRel &= ~mask;
                *pRep &= ~mask;
                g_padSwallowHoldMask &= mOn;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall Hook_PadBuildMerged(std::uintptr_t padInfo, std::uintptr_t rawPad) {
    if (g_origPadBuildMerged) {
        g_origPadBuildMerged(padInfo, rawPad);
    }
    const bool panelOpen = g_gamepadPanelEnabled.load(std::memory_order_relaxed) &&
                           g_panelOpenAtomic.load(std::memory_order_relaxed);
    if (!panelOpen && !g_padHookWasOpen && g_padSwallowHoldMask == 0U) return;
    HandlePadBuildMergedSEH(padInfo, panelOpen);
}

void PollXInputPanelFallbackIfPaused(bool seedOnly) noexcept {
    if (!g_xinputResolved) {
        g_xinputResolved = true;
        static constexpr const wchar_t* kXInputDlls[3] = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
        };
        for (const wchar_t* dll : kXInputDlls) {
            if (HMODULE hm = ::GetModuleHandleW(dll)) {
                auto fn = reinterpret_cast<XInputGetStateFn>(::GetProcAddress(hm, "XInputGetState"));
                if (fn) {
                    g_xinputGetState = fn;
                    break;
                }
            }
        }
    }
    if (!g_xinputGetState || IsGameTextInputActive(g_exeBase)) {
        g_xinputFallbackNav.Reset();
        return;
    }

    std::uint32_t mergedBtns = 0;
    bool lbHeld = false;
    for (DWORD port = 0; port < 4; ++port) {
        RawXInputState st{};
        if (g_xinputGetState(port, &st) == ERROR_SUCCESS) {
            const std::uint16_t b = st.Gamepad.wButtons;
            if ((b & 0x0100u) != 0) lbHeld = true;
            if ((b & 0x0001u) != 0) mergedBtns |= mhw::kPadBitDpadUp;
            if ((b & 0x0002u) != 0) mergedBtns |= mhw::kPadBitDpadDown;
            if ((b & 0x0004u) != 0) mergedBtns |= mhw::kPadBitDpadLeft;
            if ((b & 0x0008u) != 0) mergedBtns |= mhw::kPadBitDpadRight;
            if ((b & 0x1000u) != 0) mergedBtns |= mhw::kPadBitA;
        }
    }
    if (lbHeld) {
        g_xinputFallbackNav.Reset();
        return;
    }
    if (seedOnly) {
        g_xinputFallbackNav.prevBtns  = mergedBtns;
        g_xinputFallbackNav.repeatBtn = 0;
        return;
    }
    EmitGamepadPanelCmds(g_xinputFallbackNav, mergedBtns);
}

LRESULT CALLBACK GameKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam > 0 && wParam < 256) {
        if (IsGameTextInputActive(g_exeBase)) {
            g_heldKeyMask.store(0, std::memory_order_relaxed);
            g_suppressedCamVkMask.store(0, std::memory_order_relaxed);
            return ::CallNextHookEx(g_kbHook, code, wParam, lParam);
        }

        const auto lp       = static_cast<std::uint32_t>(lParam);
        const bool isUp     = ((lp & 0x80000000U) != 0);
        const bool wasDown  = ((lp & 0x40000000U) != 0);
        const bool altHeld  = ((lp & 0x20000000U) != 0);
        const bool ctrlHeld = ((::GetKeyState(VK_CONTROL) & 0x8000) != 0);
        const std::uint8_t camBit = BlockedCameraBit(wParam);

        const std::uint8_t bit = g_vkToAction[wParam];
        if (bit != 0) {
            if (isUp) {
                g_heldKeyMask.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_relaxed);
                const bool wasSuppressedCam =
                    (camBit != 0) &&
                    ((g_suppressedCamVkMask.fetch_and(static_cast<std::uint8_t>(~camBit), std::memory_order_relaxed) & camBit) != 0);
                if ((g_panelOpenAtomic.load(std::memory_order_relaxed) && bit == kKeyFilter) ||
                    wasSuppressedCam) {
                    return 1;
                }
            } else if (!altHeld && !ctrlHeld) {
                std::uint8_t emitBit = bit;
                if (bit == kKeyFilter && (::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                    emitBit = kKeyQuickPreset;
                }
                g_heldKeyMask.fetch_or(bit, std::memory_order_relaxed);
                if (!wasDown) {
                    g_edgeKeyMask.fetch_or(emitBit, std::memory_order_release);
                }
                g_presentWorkMask.fetch_or(kWorkHotkeyGuard, std::memory_order_release);
                if (camBit != 0) {
                    g_suppressedCamVkMask.fetch_or(camBit, std::memory_order_relaxed);
                    return 1;
                }
                if (g_panelOpenAtomic.load(std::memory_order_relaxed) && bit == kKeyFilter) {
                    return 1;
                }
            }
        } else {
            if (g_panelOpenAtomic.load(std::memory_order_relaxed) && !altHeld && !ctrlHeld) {
                const std::uint32_t pcmd = MapVkToPanelCmd(wParam);
                if (pcmd != 0) {
                    if (!isUp) {
                        // Allow auto-repeat on Up/Down cursor movement; single-edge on toggles
                        const bool allowRepeat = (pcmd == kPanelUp || pcmd == kPanelDown);
                        if (!wasDown || allowRepeat) {
                            g_panelCmdMask.fetch_or(pcmd, std::memory_order_release);
                            g_presentWorkMask.fetch_or(kWorkHotkeyGuard, std::memory_order_release);
                        }
                    }
                    return 1; // Consume panel navigation/toggle key so game menus/camera don't react!
                }
            }
            if (camBit != 0) {
                if (isUp) {
                    if ((g_suppressedCamVkMask.fetch_and(static_cast<std::uint8_t>(~camBit), std::memory_order_relaxed) & camBit) != 0) {
                        return 1;
                    }
                } else if (!altHeld && !ctrlHeld) {
                    g_suppressedCamVkMask.fetch_or(camBit, std::memory_order_relaxed);
                    return 1; // Suppress O/K/L/; Win32 key messages while mod is active (outside chat)
                }
            }
        }
    }
    return ::CallNextHookEx(g_kbHook, code, wParam, lParam);
}

void PollFallbackIfHookUnavailable(HWND gameHwnd) noexcept {
    if ((++g_fallbackFrameDiv & 7U) != 0) return;
    if (!gameHwnd || ::GetForegroundWindow() != gameHwnd || IsGameTextInputActive(g_exeBase)) {
        g_heldKeyMask.store(0, std::memory_order_relaxed);
        return;
    }
    if (((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
        ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0)) {
        return;
    }
    const std::uint8_t prevHeld = g_heldKeyMask.load(std::memory_order_relaxed);
    std::uint8_t nowHeld = 0;
    static constexpr std::uint8_t kBits[7] = {
        kKeyVis, kKeyFreeze, kKeyWorldFreeze, kKeyKeepHitbox, kKeyStep, kKeyFilter, kKeyToggleOverlay
    };
    for (int i = 0; i < 7; ++i) {
        const int vk = g_actionVks[i];
        if (vk > 0 && vk < 256 && (::GetAsyncKeyState(vk) & 0x8000) != 0) {
            nowHeld |= kBits[i];
        }
    }
    g_heldKeyMask.store(nowHeld, std::memory_order_relaxed);
    std::uint8_t rising = static_cast<std::uint8_t>(nowHeld & ~prevHeld);
    if ((rising & kKeyFilter) != 0 && (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        rising = static_cast<std::uint8_t>((rising & ~kKeyFilter) | kKeyQuickPreset);
    }
    if (rising != 0) {
        g_edgeKeyMask.fetch_or(rising, std::memory_order_release);
    }
}

void EnsureVisEnabled() noexcept {
    g_presentWorkMask.fetch_or(kWorkVisEnabled, std::memory_order_release);
}

void EnsureEmPartVisEnabled() noexcept {
    if ((g_viewState.visMask & (app::kCatEmHurtbox | app::kCatPushbox)) == 0U) {
        g_viewState.visMask |= app::kCatEmHurtbox;
    }
    EnsureVisEnabled();
}

void ApplyPanelCommands(std::uint32_t cmds) noexcept {
    static constexpr std::uint32_t kRowCatBits[9] = {
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
    static constexpr std::uint32_t kDirectRowBits[14] = {
        kPanelRow1, kPanelRow2, kPanelRow3, kPanelRow4, kPanelRow5, kPanelRow6,
        kPanelRow7, kPanelRow8, kPanelRow9, kPanelRow10, kPanelRow11, kPanelRow12,
        kPanelRow13, kPanelRow14
    };

    const app::ViewFilterState prev = g_viewState;

    if ((cmds & kPanelClose) != 0) {
        g_viewState.panelOpen = false;
        g_panelOpenAtomic.store(false, std::memory_order_release);
        if (prev.panelOpen) {
            app::Log("hotkey: Filter & Scene Panel CLOSED");
        }
        return;
    }

    if ((cmds & kPanelSwitchPage) != 0) {
        g_viewState.panelPage = (g_viewState.panelPage == 0) ? 1 : 0;
    }

    // Page 1: Monster Parts Inspector
    if (g_viewState.panelPage == 1) {
        const coll::EmInspectorCounts cnts = coll::GetLastEmInspectorCounts();
        const int partCnt   = static_cast<int>(cnts.partCount);
        const int backRow   = 5 + partCnt;
        const int totalRows = backRow + 1;

        if (g_viewState.partPanelCursor < 0) g_viewState.partPanelCursor = 0;
        if (g_viewState.partPanelCursor >= totalRows) g_viewState.partPanelCursor = totalRows - 1;

        if ((cmds & kPanelUp) != 0) {
            g_viewState.partPanelCursor = (g_viewState.partPanelCursor + totalRows - 1) % totalRows;
        }
        if ((cmds & kPanelDown) != 0) {
            g_viewState.partPanelCursor = (g_viewState.partPanelCursor + 1) % totalRows;
        }

        const std::uint32_t activeAllMask =
            (partCnt <= 0 || partCnt >= 32) ? 0xFFFFFFFFu : ((1U << static_cast<std::uint32_t>(partCnt)) - 1U);

        auto findSingleSoloIdx = [&]() noexcept -> int {
            int found = -1;
            const int lim = (partCnt <= 32) ? partCnt : 32;
            for (int p = 0; p < lim; ++p) {
                if ((g_viewState.emPartMask & (1U << static_cast<std::uint32_t>(p))) != 0U) {
                    if (found >= 0) return -1;
                    found = p;
                }
            }
            return found;
        };

        if ((cmds & kPanelFlipScope) != 0) {
            if (g_viewState.emPartMode == app::EmPartMode::Off) {
                g_viewState.emPartMode = app::EmPartMode::Hitzone;
            }
            if (partCnt > 0 && g_viewState.partPanelCursor >= 5 && (g_viewState.partPanelCursor - 5) < 32) {
                const std::uint32_t soloBit = 1U << static_cast<std::uint32_t>(g_viewState.partPanelCursor - 5);
                if ((g_viewState.emPartMask & activeAllMask) == soloBit) {
                    g_viewState.emPartMask = 0xFFFFFFFFu;
                } else {
                    g_viewState.emPartMask = soloBit;
                }
            } else {
                if ((g_viewState.emPartMask & activeAllMask) == activeAllMask && partCnt > 1) {
                    g_viewState.emPartMask = 1U;
                } else {
                    g_viewState.emPartMask = 0xFFFFFFFFu;
                }
            }
            EnsureEmPartVisEnabled();
        }

        auto stepPartRow = [&](int row, int dir) {
            if (row == 0) {
                const int cnt  = static_cast<int>(app::EmPartMode::Count);
                const int cur  = static_cast<int>(g_viewState.emPartMode);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.emPartMode = static_cast<app::EmPartMode>(next);
                g_viewState.emPartMask = 0xFFFFFFFFu;
                if (g_viewState.emPartMode != app::EmPartMode::Off) {
                    EnsureEmPartVisEnabled();
                }
            } else if (row == 1) {
                const int cnt  = static_cast<int>(app::EmUnselectedStyle::Count);
                const int cur  = static_cast<int>(g_viewState.emUnsStyle);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.emUnsStyle = static_cast<app::EmUnselectedStyle>(next);
            } else if (row == 2) {
                const int cnt  = static_cast<int>(app::EmPartLabelMode::Count);
                const int cur  = static_cast<int>(g_viewState.emLabelMode);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.emLabelMode = static_cast<app::EmPartLabelMode>(next);
            } else if (row == 3) {
                const int emSlots = (cnts.monsterCount > 8U) ? 8 : static_cast<int>(cnts.monsterCount);
                const int maxScopes = 2 + emSlots;
                const int delta = (dir < 0) ? (maxScopes - 1) : 1;
                g_viewState.emTargetScope = (g_viewState.emTargetScope + delta) % maxScopes;
            } else if (row == 4) {
                if (g_viewState.emPartMode == app::EmPartMode::Off) {
                    g_viewState.emPartMode = app::EmPartMode::Hitzone;
                }
                if (partCnt <= 0) {
                    g_viewState.emPartMask = 0xFFFFFFFFu;
                } else if (dir == 0) {
                    if ((g_viewState.emPartMask & activeAllMask) == activeAllMask && partCnt > 1) {
                        g_viewState.emPartMask = 1U;
                    } else {
                        g_viewState.emPartMask = 0xFFFFFFFFu;
                    }
                } else {
                    const int lim = (partCnt <= 32) ? partCnt : 32;
                    const int curSolo = findSingleSoloIdx();
                    int nextSolo = 0;
                    if (curSolo >= 0) {
                        nextSolo = (dir < 0) ? ((curSolo + lim - 1) % lim) : ((curSolo + 1) % lim);
                    } else {
                        nextSolo = (dir < 0) ? (lim - 1) : 0;
                    }
                    g_viewState.emPartMask = 1U << static_cast<std::uint32_t>(nextSolo);
                }
                EnsureEmPartVisEnabled();
            } else if (row == backRow) {
                g_viewState.panelPage = 0;
            } else if (row >= 5) {
                const int pIdx = row - 5;
                if (pIdx >= 0 && pIdx < 32) {
                    if (g_viewState.emPartMode == app::EmPartMode::Off) {
                        g_viewState.emPartMode = app::EmPartMode::Hitzone;
                    }
                    g_viewState.emPartMask ^= (1U << static_cast<std::uint32_t>(pIdx));
                    EnsureEmPartVisEnabled();
                }
            }
        };

        if ((cmds & kPanelLeft) != 0)  stepPartRow(g_viewState.partPanelCursor, -1);
        if ((cmds & kPanelRight) != 0) stepPartRow(g_viewState.partPanelCursor, +1);
        if ((cmds & kPanelEnter) != 0) stepPartRow(g_viewState.partPanelCursor, 0);

        for (int i = 0; i < 14; ++i) {
            if ((cmds & kDirectRowBits[i]) != 0 && i < partCnt) {
                g_viewState.partPanelCursor = 5 + i;
                stepPartRow(5 + i, 0);
            }
        }
    } else {
        // Page 0: Main Filter & Scene Panel (18 rows: 0..17)
        constexpr int kMainPanelRows = 18;
        if ((cmds & kPanelUp) != 0) {
            g_viewState.panelCursor = (g_viewState.panelCursor + kMainPanelRows - 1) % kMainPanelRows;
        }
        if ((cmds & kPanelDown) != 0) {
            g_viewState.panelCursor = (g_viewState.panelCursor + 1) % kMainPanelRows;
        }

        if ((cmds & kPanelFlipScope) != 0) {
            if ((g_viewState.visMask & app::kCatMaskAll) == app::kCatMaskMapOnly) {
                g_viewState.presetCursor = static_cast<int>(app::PresetId::PlEmOnly);
                g_viewState.visMask      = app::kCatMaskPlEmOnly;
            } else {
                g_viewState.presetCursor = static_cast<int>(app::PresetId::MapOnly);
                g_viewState.visMask      = app::kCatMaskMapOnly;
            }
            EnsureVisEnabled();
        }

        auto stepRow = [&](int row, int dir) {
            if (row == 0) {
                const int cnt = static_cast<int>(app::PresetId::Count);
                const int delta = (dir < 0) ? (cnt - 1) : 1;
                g_viewState.presetCursor = (g_viewState.presetCursor + delta) % cnt;
                g_viewState.visMask = app::PresetMask(static_cast<app::PresetId>(g_viewState.presetCursor));
                EnsureVisEnabled();
            } else if (row >= 1 && row <= 7) {
                g_viewState.visMask ^= kRowCatBits[row];
                if ((g_viewState.visMask & kRowCatBits[row]) != 0) EnsureVisEnabled();
            } else if (row == 8) {
                const bool wasOn = (g_viewState.visMask & app::kCatStageMesh) != 0;
                if (dir == 0) {
                    g_viewState.visMask ^= app::kCatStageMesh;
                    if (!wasOn) EnsureVisEnabled();
                } else if (dir > 0) {
                    if (!wasOn) {
                        g_viewState.sbcRadiusIdx = 0;
                        g_viewState.visMask |= app::kCatStageMesh;
                        EnsureVisEnabled();
                    } else if (g_viewState.sbcRadiusIdx + 1 < app::kSbcRadiusCount) {
                        ++g_viewState.sbcRadiusIdx;
                    } else {
                        g_viewState.visMask &= ~std::uint32_t(app::kCatStageMesh);
                    }
                } else {
                    if (!wasOn) {
                        g_viewState.sbcRadiusIdx = app::kSbcRadiusCount - 1;
                        g_viewState.visMask |= app::kCatStageMesh;
                        EnsureVisEnabled();
                    } else if (g_viewState.sbcRadiusIdx > 0) {
                        --g_viewState.sbcRadiusIdx;
                    } else {
                        g_viewState.visMask &= ~std::uint32_t(app::kCatStageMesh);
                    }
                }
            } else if (row == 9) {
                g_viewState.hideMonster = !g_viewState.hideMonster;
                if (g_viewState.hideMonster) EnsureVisEnabled();
            } else if (row == 10) {
                const int cnt  = static_cast<int>(app::StageHideMode::Count);
                const int cur  = static_cast<int>(g_viewState.stageHide);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.stageHide = static_cast<app::StageHideMode>(next);
                if (g_viewState.stageHide != app::StageHideMode::Off) EnsureVisEnabled();
            } else if (row == 11) {
                const int cnt  = static_cast<int>(app::BackdropMode::Count);
                const int cur  = static_cast<int>(g_viewState.backdrop);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.backdrop = static_cast<app::BackdropMode>(next);
                if (g_viewState.backdrop != app::BackdropMode::Off) EnsureVisEnabled();
            } else if (row == 12) {
                const int cnt  = static_cast<int>(app::DepthGuideMode::Count);
                const int cur  = static_cast<int>(g_viewState.depthMode);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.depthMode = static_cast<app::DepthGuideMode>(next);
                if (g_viewState.depthMode == app::DepthGuideMode::Opaque ||
                    g_viewState.depthMode == app::DepthGuideMode::OpaqueNoRings) {
                    g_viewState.visMask |= app::kCatStageMesh;
                }
                EnsureVisEnabled();
            } else if (row == 13) {
                const int cnt  = static_cast<int>(app::SbcMeshTier::Count);
                const int cur  = static_cast<int>(g_viewState.sbcTier);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.sbcTier = static_cast<app::SbcMeshTier>(next);
                g_viewState.visMask |= app::kCatStageMesh;
                EnsureVisEnabled();
            } else if (row == 14) {
                const int cnt  = static_cast<int>(app::SpecialSbcStyle::Count);
                const int cur  = static_cast<int>(g_viewState.specialSbc);
                const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                g_viewState.specialSbc = static_cast<app::SpecialSbcStyle>(next);
                if (g_viewState.specialSbc != app::SpecialSbcStyle::Off) {
                    g_viewState.visMask |= app::kCatStageMesh;
                    EnsureVisEnabled();
                }
            } else if (row == 15) {
                if (dir == 0) {
                    g_viewState.entityLineIdx = (g_viewState.entityLineIdx + 1) % app::kLineScaleCount;
                } else if (dir < 0) {
                    if (g_viewState.entityLineIdx > 0) --g_viewState.entityLineIdx;
                } else {
                    if (g_viewState.entityLineIdx + 1 < app::kLineScaleCount) ++g_viewState.entityLineIdx;
                }
                EnsureVisEnabled();
            } else if (row == 16) {
                if (dir == 0) {
                    g_viewState.sbcLineIdx = (g_viewState.sbcLineIdx + 1) % app::kLineScaleCount;
                } else if (dir < 0) {
                    if (g_viewState.sbcLineIdx > 0) --g_viewState.sbcLineIdx;
                } else {
                    if (g_viewState.sbcLineIdx + 1 < app::kLineScaleCount) ++g_viewState.sbcLineIdx;
                }
                EnsureVisEnabled();
            } else if (row == 17) {
                if (dir == 0) {
                    g_viewState.panelPage = 1;
                } else {
                    const int cnt  = static_cast<int>(app::EmPartMode::Count);
                    const int cur  = static_cast<int>(g_viewState.emPartMode);
                    const int next = (dir < 0) ? ((cur + cnt - 1) % cnt) : ((cur + 1) % cnt);
                    g_viewState.emPartMode = static_cast<app::EmPartMode>(next);
                    g_viewState.emPartMask = 0xFFFFFFFFu;
                    if (g_viewState.emPartMode != app::EmPartMode::Off) {
                        EnsureEmPartVisEnabled();
                    }
                }
            }
        };

        if ((cmds & kPanelLeft) != 0)  stepRow(g_viewState.panelCursor, -1);
        if ((cmds & kPanelRight) != 0) stepRow(g_viewState.panelCursor, +1);
        if ((cmds & kPanelEnter) != 0) stepRow(g_viewState.panelCursor, 0);

        for (int i = 0; i < 14; ++i) {
            if ((cmds & kDirectRowBits[i]) != 0) {
                const int targetRow = i + 1;
                g_viewState.panelCursor = targetRow;
                stepRow(targetRow, (targetRow >= 10) ? +1 : 0);
            }
        }
    }

    if (g_viewState.visMask != prev.visMask ||
        g_viewState.sbcRadiusIdx != prev.sbcRadiusIdx ||
        g_viewState.entityLineIdx != prev.entityLineIdx ||
        g_viewState.sbcLineIdx != prev.sbcLineIdx ||
        g_viewState.hideMonster != prev.hideMonster ||
        g_viewState.stageHide != prev.stageHide ||
        g_viewState.backdrop != prev.backdrop ||
        g_viewState.depthMode != prev.depthMode ||
        g_viewState.sbcTier != prev.sbcTier ||
        g_viewState.specialSbc != prev.specialSbc ||
        g_viewState.emPartMode != prev.emPartMode ||
        g_viewState.emUnsStyle != prev.emUnsStyle ||
        g_viewState.emLabelMode != prev.emLabelMode ||
        g_viewState.emTargetScope != prev.emTargetScope ||
        g_viewState.emPartMask != prev.emPartMask) {
        app::Log("panel: page=%d mask=0x%02X(%s) line=(em=%.2fx,sbc=%.2fx) emPart=%s(mask=0x%08X,uns=%s,lbl=%s,tgt=%d) sbc=%s(%dm,tier=%s,spec=%s,style=%s) hideEm=%d stageHide=%s backdrop=%s",
                 g_viewState.panelPage,
                 g_viewState.visMask,
                 app::MatchPresetName(g_viewState.visMask),
                 app::LineScaleFromIdx(g_viewState.entityLineIdx),
                 app::LineScaleFromIdx(g_viewState.sbcLineIdx),
                 app::EmPartModeName(g_viewState.emPartMode),
                 g_viewState.emPartMask,
                 app::EmUnselectedStyleName(g_viewState.emUnsStyle),
                 app::EmPartLabelModeName(g_viewState.emLabelMode),
                 g_viewState.emTargetScope,
                 ((g_viewState.visMask & app::kCatStageMesh) != 0) ? "ON" : "OFF",
                 static_cast<int>(app::SbcRadiusMetersFromIdx(g_viewState.sbcRadiusIdx) + 0.5f),
                 app::SbcMeshTierName(g_viewState.sbcTier),
                 app::SpecialSbcStyleName(g_viewState.specialSbc),
                 app::DepthGuideModeName(g_viewState.depthMode),
                 g_viewState.hideMonster ? 1 : 0,
                 app::StageHideModeName(g_viewState.stageHide),
                 app::BackdropModeName(g_viewState.backdrop));
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
                    g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkInitKbHook), std::memory_order_release);
                    g_presentWorkMask.fetch_or(kWorkFallbackPoll, std::memory_order_release);
                    app::Log("input: WH_KEYBOARD unavailable (err=%lu), using low-rate fallback", ::GetLastError());
                }
            }
        }
    }

    if (g_useFallbackPoll) {
        PollFallbackIfHookUnavailable(render::GetGameWindow(d3dRender));
    }

    // If [P] panel is open and sMhPad::move is paused (e.g. during Full Freeze [I]),
    // automatically fall back to XInputGetState so controller D-Pad + A stay responsive!
    if (g_gamepadPanelEnabled.load(std::memory_order_relaxed) && g_viewState.panelOpen) {
        const std::uint32_t beat = g_padHookHeartbeat.load(std::memory_order_relaxed);
        if (beat != g_lastSeenPadBeat) {
            g_lastSeenPadBeat    = beat;
            g_padBeatStallFrames = 0;
            g_xinputFallbackNav.Reset();
        } else if (++g_padBeatStallFrames >= 2U) {
            PollXInputPanelFallbackIfPaused(g_padBeatStallFrames == 2U);
        }
    } else {
        g_padBeatStallFrames = 0;
        g_xinputFallbackNav.Reset();
    }

    const std::uint8_t edges     = g_edgeKeyMask.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t panelCmd = g_panelCmdMask.exchange(0, std::memory_order_acq_rel);
    std::uint8_t held            = g_heldKeyMask.load(std::memory_order_relaxed);

    // Clear any stale held-bit if user Alt-Tabbed while holding a hotkey
    if (held != 0) {
        if (++g_heldWatchdog >= 16) {
            g_heldWatchdog = 0;
            std::uint8_t verified = 0;
            static constexpr std::uint8_t kBits[7] = {
                kKeyVis, kKeyFreeze, kKeyWorldFreeze, kKeyKeepHitbox, kKeyStep, kKeyFilter, kKeyToggleOverlay
            };
            for (int i = 0; i < 7; ++i) {
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

    static constexpr std::uint8_t kFreezeCamKeyMask =
        kKeyFreeze | kKeyWorldFreeze | kKeyKeepHitbox | kKeyStep;

    if (edges != 0 || panelCmd != 0) {
        // Arm camera-nudge guard ONLY when freeze/step hotkeys (I/K/J/O) are pressed —
        // never patch cUnit::genericMove for [P] panel navigation or [U] toggle!
        if ((edges & kFreezeCamKeyMask) != 0) {
            freeze::SyncActiveFrame(g_exeBase, true);
        }

        if ((edges & kKeyVis) != 0) {
            const bool wasVis = ((g_presentWorkMask.load(std::memory_order_relaxed) & kWorkVisEnabled) != 0);
            const bool nextVis = !wasVis;
            if (nextVis) g_presentWorkMask.fetch_or(kWorkVisEnabled, std::memory_order_release);
            else         g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkVisEnabled), std::memory_order_release);
            app::Log("hotkey: Visualization %s (preset=%s, mask=0x%02X)",
                     nextVis ? "ON" : "OFF",
                     app::MatchPresetName(g_viewState.visMask),
                     g_viewState.visMask);
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
            if (!g_viewState.panelOpen) {
                // Opportunistically hot-reload mhw_hfi.ini on panel open (zero polling!)
                RefreshHotRuntimeConfig();
            }
            g_viewState.panelOpen = !g_viewState.panelOpen;
            g_panelOpenAtomic.store(g_viewState.panelOpen, std::memory_order_release);
            app::Log("hotkey: Filter & Scene Panel %s", g_viewState.panelOpen ? "OPEN" : "CLOSED");
        }

        if ((edges & kKeyQuickPreset) != 0) {
            const int cnt = static_cast<int>(app::PresetId::Count);
            g_viewState.presetCursor = (g_viewState.presetCursor + 1) % cnt;
            g_viewState.visMask = app::PresetMask(static_cast<app::PresetId>(g_viewState.presetCursor));
            EnsureVisEnabled();
            app::Log("hotkey: Quick Preset -> %s (mask=0x%02X)",
                     app::MatchPresetName(g_viewState.visMask), g_viewState.visMask);
        }

        if ((edges & kKeyToggleOverlay) != 0) {
            g_viewState.showOverlay = !g_viewState.showOverlay;
            if (!g_viewState.showOverlay && g_viewState.panelOpen) {
                g_viewState.panelOpen = false;
                g_panelOpenAtomic.store(false, std::memory_order_release);
            }
            app::Log("hotkey: HUD Overlay %s", g_viewState.showOverlay ? "SHOWN" : "HIDDEN (clean screenshot mode)");
        }

        if (panelCmd != 0 && g_viewState.panelOpen) {
            ApplyPanelCommands(panelCmd);
        }
    }

    if (g_viewState.panelOpen) {
        g_presentWorkMask.fetch_or(kWorkPanelOpen, std::memory_order_release);
    } else {
        g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkPanelOpen), std::memory_order_release);
    }

    const bool anyFreezeCamKey = ((edges | held) & kFreezeCamKeyMask) != 0;
    const bool needFreezeSync  = freeze::SyncActiveFrame(g_exeBase, anyFreezeCamKey);
    const freeze::Mode freezeMode = freeze::CurrentMode();

    if (freezeMode != freeze::Mode::None) {
        g_presentWorkMask.fetch_or(kWorkFreezeActive, std::memory_order_release);
    } else {
        g_presentWorkMask.fetch_and(static_cast<std::uint8_t>(~kWorkFreezeActive), std::memory_order_release);
    }

    const bool vis = ((g_presentWorkMask.load(std::memory_order_relaxed) & kWorkVisEnabled) != 0);
    scene::SyncState(g_exeBase,
                     vis && g_viewState.hideMonster,
                     vis ? g_viewState.stageHide : app::StageHideMode::Off,
                     vis ? g_viewState.backdrop  : app::BackdropMode::Off);

    if (!needFreezeSync && !g_useFallbackPoll &&
        g_edgeKeyMask.load(std::memory_order_relaxed) == 0 &&
        g_panelCmdMask.load(std::memory_order_relaxed) == 0) {
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

    const bool needOverlayDraw = g_viewState.showOverlay &&
                                 (freezeMode != freeze::Mode::None || showStartupToast);
    if (vis || g_viewState.panelOpen || needOverlayDraw) {
        render::OnPresent(g_exeBase,
                          d3dRender,
                          vis,
                          freezeMode,
                          g_viewState,
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

    const std::uintptr_t presentSite  = base + mhw::kRvaD3DPresent;
    const std::uintptr_t kbUpdateSite = base + mhw::kRvaSMhKeyboardUpdateState;
    const std::uintptr_t chatSite     = base + mhw::kRvaSChatDispatchEntry;
    const std::uintptr_t padMergeSite = base + mhw::kRvaSMhPadBuildMerged;
    const std::uintptr_t moveSite     = base + mhw::kRvaUnitGenericMove;
    const std::uintptr_t hitSite      = base + mhw::kRvaHitDispatch;
    const std::uintptr_t guardSite    = base + mhw::kRvaGuardDispatch;
    const std::uintptr_t unitDrawSite = base + mhw::kRvaSUnitDraw;

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

    void* origKbUpdate = nullptr;
    if (HasValidOrChainedPrologue(kbUpdateSite, mhw::kKbUpdateStatePrologue, 5) &&
        hook::PreparePermanentDetour5(kbUpdateSite,
                                      reinterpret_cast<void*>(&Hook_KeyboardUpdateState),
                                      &origKbUpdate,
                                      mhw::kKbUpdateStatePrologue)) {
        g_origKbUpdateState = reinterpret_cast<KbUpdateStateFn>(origKbUpdate);
    } else {
        app::Log("init: warning - sMhKeyboard::updateState hook unavailable at %p",
                 reinterpret_cast<void*>(kbUpdateSite));
    }

    // Always prepare Chat & Pad hooks so hot-reloading ChatCommands/GamepadPanel works live without restart
    {
        void* origChat = nullptr;
        if (HasValidOrChainedPrologue(chatSite, mhw::kSChatDispatchPrologue, 5) &&
            hook::PreparePermanentDetour5(chatSite,
                                          reinterpret_cast<void*>(&Hook_ChatDispatch),
                                          &origChat,
                                          mhw::kSChatDispatchPrologue)) {
            g_origChatDispatch = reinterpret_cast<ChatDispatchFn>(origChat);
        } else {
            app::Log("init: warning - sChat::DispatchChatEntry hook unavailable at %p",
                     reinterpret_cast<void*>(chatSite));
        }
    }

    {
        void* origPadMerge = nullptr;
        if (HasValidOrChainedPrologue(padMergeSite, mhw::kSMhPadBuildMergedPrologue, 5) &&
            hook::PreparePermanentDetour5(padMergeSite,
                                          reinterpret_cast<void*>(&Hook_PadBuildMerged),
                                          &origPadMerge,
                                          mhw::kSMhPadBuildMergedPrologue)) {
            g_origPadBuildMerged = reinterpret_cast<PadBuildMergedFn>(origPadMerge);
        } else {
            app::Log("init: warning - sMhPad::buildMergedPadState hook unavailable at %p",
                     reinterpret_cast<void*>(padMergeSite));
        }
    }

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

    void* origUnitDraw = nullptr;
    if (HasValidOrChainedPrologue(unitDrawSite, mhw::kSUnitDrawPrologue, 5) &&
        hook::PrepareDynamicHook5(hook::kDynUnitDraw,
                                  unitDrawSite,
                                  reinterpret_cast<void*>(&scene::Hook_SUnitDraw),
                                  &origUnitDraw,
                                  mhw::kSUnitDrawPrologue)) {
        scene::SetOrigSUnitDraw(origUnitDraw);
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
    {
        std::lock_guard<std::mutex> lk(g_liveRuntimeCfgMx);
        g_liveChatCfg = g_cfg.chat;
    }
    g_gamepadPanelEnabled.store(g_cfg.gamepadPanel, std::memory_order_release);
    freeze::InitModuleRange(base, end);
    freeze::SetKeepHitboxesActive(g_cfg.keepHitboxesActive);
    scene::InitModuleRange(base, end);

    g_viewState.visMask       = g_cfg.defaultVisMask;
    g_viewState.sbcRadiusIdx  = g_cfg.defaultSbcRadiusIdx;
    g_viewState.entityLineIdx = g_cfg.defaultEntityLineIdx;
    g_viewState.sbcLineIdx    = g_cfg.defaultSbcLineIdx;
    g_viewState.hideMonster   = g_cfg.startHideMonster;
    g_viewState.showOverlay   = g_cfg.showHudBanner;
    g_viewState.stageHide    = g_cfg.startStageHide;
    g_viewState.backdrop     = g_cfg.startBackdrop;
    g_viewState.depthMode    = g_cfg.startDepthMode;
    g_viewState.sbcTier      = g_cfg.startSbcTier;
    g_viewState.specialSbc   = g_cfg.startSpecialSbc;
    g_viewState.emPartMode   = g_cfg.startEmPartMode;
    g_viewState.emUnsStyle   = g_cfg.startEmUnsStyle;
    g_viewState.emLabelMode  = g_cfg.startEmLabelMode;

    std::uint8_t initialWork = kWorkInitKbHook;
    if (g_cfg.showHudBanner) initialWork |= kWorkStartupToast;
    if (g_cfg.startVisOn)    initialWork |= kWorkVisEnabled;
    g_presentWorkMask.store(initialWork, std::memory_order_release);

    if (!hook::FinalizeAndInstallPresentHook()) {
        app::Log("init: FinalizeAndInstallPresentHook failed");
        g_exeBase = 0;
        return false;
    }

    app::Log("init: hooked Present (%p) + KbUpdate (%p, blockOKL=%d) + Chat (%p, en=%d, cs=%d, hotReload=1) + Pad (%p, en=%d); genericMove (%p), HitDispatch (%p) & sUnit::draw (%p) stay 100%% vanilla until engaged! (arena=%p)",
             reinterpret_cast<void*>(presentSite),
             reinterpret_cast<void*>(kbUpdateSite),
             g_cfg.blockCameraKeys ? 1 : 0,
             reinterpret_cast<void*>(g_origChatDispatch ? chatSite : 0),
             g_cfg.chat.enabled ? 1 : 0,
             g_cfg.chat.caseSensitive ? 1 : 0,
             reinterpret_cast<void*>(g_origPadBuildMerged ? padMergeSite : 0),
             g_cfg.gamepadPanel ? 1 : 0,
             reinterpret_cast<void*>(moveSite),
             reinterpret_cast<void*>(hitSite),
             reinterpret_cast<void*>(unitDrawSite),
             reinterpret_cast<void*>(hook::ArenaBase()));
    return true;
}

DWORD WINAPI InitWorker(LPVOID) {
    app::InitLog();
    g_cfg = app::LoadConfig();
    if (!g_cfg.enabled) {
        app::Log("init: disabled via mhw_hfi.ini");
        return 0;
    }

    BuildHotkeyTable();

    HMODULE exe = ::GetModuleHandleW(L"MonsterHunterWorld.exe");
    if (!exe) exe = ::GetModuleHandleW(nullptr);
    app::Log("init: MonsterHunterWorld.exe base = %p", exe);
    if (!exe) return 1;

    for (int attempt = 0; attempt < 2400; ++attempt) {
        if (TryInstallHooks(exe)) {
            app::Log("init: ready! U=Toggle Vis, I=Full Freeze, K=World Freeze, J=Keep Hitboxes, O=Step, P=Filter & Scene Panel (0 background threads)");
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
            scene::Shutdown(g_exeBase);
            freeze::SetWorldFrozen(g_exeBase, false);
            freeze::SetFrozen(g_exeBase, false);
        }
        hook::RemoveAllHooks();
        render::Shutdown();
        app::CloseLog();
    }
    return TRUE;
}

#include "hook.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

constexpr std::size_t kCodeArenaSize = 0x1000; // Single 4 KB PAGE_EXECUTE_READ page

struct PermanentHookInfo {
    bool           valid = false;
    bool           active = false;
    std::uintptr_t site  = 0;
    std::uintptr_t relay = 0;
    std::uint8_t   orig5[5]{};
};

struct DynHook5Info {
    bool           prepared = false;
    bool           enabled  = false;
    bool           wasChained = false;
    std::uintptr_t site     = 0;
    std::uint8_t   orig5[5]{};
    std::uint8_t   jmp5[5]{};
};

struct LazyHookInfo {
    bool           prepared  = false;
    bool           installed = false;
    std::uintptr_t site      = 0;
    std::size_t    len       = 0;
    std::uintptr_t relay     = 0;
    std::uint8_t   orig[16]{};
};

std::uintptr_t    g_codeArena = 0;
std::size_t       g_codeUsed  = 0;
bool              g_sealed    = false;
PermanentHookInfo g_presentHook{};
DynHook5Info      g_dynHooks[kDynCount]{};
LazyHookInfo      g_lazyGuard{};
std::mutex        g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to) noexcept {
    const std::int64_t d = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocReadWriteAt(std::uintptr_t addr, std::size_t size) noexcept {
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

bool AtomicSwap5BytesAligned(std::uintptr_t site, const std::uint8_t fiveBytes[5]) noexcept {
    if ((site & 0x7ULL) != 0) return false; // Guaranteed 8B-aligned (0x...290, 0x...F50, 0x...7C0)
    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(site), 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    auto* qwordPtr = reinterpret_cast<volatile LONG64*>(site);
    LONG64 cur = *qwordPtr;
    for (;;) {
        LONG64 desired = cur;
        std::memcpy(&desired, fiveBytes, 5); // Bytes [5..7] preserved verbatim!
        const LONG64 prev = _InterlockedCompareExchange64(qwordPtr, desired, cur);
        if (prev == cur) break;
        cur = prev;
    }
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(site), 8, oldProt, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), 8);
    return true;
}

bool WriteBytes(std::uintptr_t addr, const void* bytes, std::size_t n) noexcept {
    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr), n, oldProt, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool EmitRelayAndTrampoline(std::uintptr_t site,
                            std::size_t len,
                            void* detourFn,
                            const std::uint8_t* displaced,
                            std::size_t dlen,
                            std::uintptr_t& outRelay,
                            void** outOrigTrampoline,
                            bool& outWasChained) {
    const std::size_t alignedOff = (g_codeUsed + 63U) & ~std::size_t(63U);
    if (alignedOff + 64U > kCodeArenaSize) return false;

    const std::uintptr_t base = g_codeArena + alignedOff;
    if (!InRel32(site + 5U, base)) return false;

    auto* out = reinterpret_cast<std::uint8_t*>(base);

    // [0..13] Relay: jmp qword ptr [rip + 0] + u64 detourFn
    out[0] = 0xFF; out[1] = 0x25;
    out[2] = 0x00; out[3] = 0x00; out[4] = 0x00; out[5] = 0x00;
    const auto fnAddr = reinterpret_cast<std::uint64_t>(detourFn);
    std::memcpy(out + 6, &fnAddr, 8);

    // [16..] origTramp (16-byte aligned!)
    const std::uintptr_t trampAddr = base + 16U;
    auto* tramp = out + 16U;

    const auto* curSite = reinterpret_cast<const std::uint8_t*>(site);
    if (curSite[0] == 0xE9 && len >= 5U) {
        // Chain cleanly if another plugin (e.g. debug_tas.dll) already hooked this site
        outWasChained = true;
        std::int32_t prevRel = 0;
        std::memcpy(&prevRel, curSite + 1, 4);
        const auto prevTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(site + 5U) + prevRel);
        if (!InRel32(trampAddr + 5U, prevTarget)) return false;

        tramp[0] = 0xE9;
        const auto chainRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(prevTarget) - static_cast<std::int64_t>(trampAddr + 5U));
        std::memcpy(tramp + 1, &chainRel, 4);
        g_codeUsed = alignedOff + 24U;
    } else {
        outWasChained = false;
        const std::uintptr_t backNext = trampAddr + dlen + 5U;
        if (!InRel32(backNext, site + len)) return false;

        std::memcpy(tramp, displaced, dlen);
        tramp[dlen] = 0xE9;
        const auto backRel = static_cast<std::int32_t>(
            static_cast<std::int64_t>(site + len) - static_cast<std::int64_t>(backNext));
        std::memcpy(tramp + dlen + 1U, &backRel, 4);
        g_codeUsed = alignedOff + 16U + dlen + 5U;
    }

    outRelay           = base;
    *outOrigTrampoline = reinterpret_cast<void*>(trampAddr);
    return true;
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_codeArena) return true;

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t span = 0x7F000000ULL;

    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + kCodeArenaSize, lo)) continue;
        if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
            g_codeArena = p;
            break;
        }
    }
    if (!g_codeArena) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + kCodeArenaSize)) break;
            if (std::uintptr_t p = TryAllocReadWriteAt(a, kCodeArenaSize)) {
                g_codeArena = p;
                break;
            }
        }
    }
    if (!g_codeArena) {
        for (int i = 0; i < 64 && !g_codeArena; ++i) {
            void* p = ::VirtualAlloc(nullptr, kCodeArenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) break;
            const auto a = reinterpret_cast<std::uintptr_t>(p);
            if (InRel32(a, hi) && InRel32(a + kCodeArenaSize, lo)) g_codeArena = a;
            else ::VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!g_codeArena) return false;

    g_codeUsed = 0;
    g_sealed   = false;
    std::memset(reinterpret_cast<void*>(g_codeArena), 0xCC, kCodeArenaSize);
    return true;
}

std::uintptr_t ArenaBase() { return g_codeArena; }

bool PreparePermanentDetour5(std::uintptr_t site,
                             void* detourFn,
                             void** outOrigTrampoline,
                             const std::uint8_t expected5[5]) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed || !site || !detourFn || !outOrigTrampoline || (site & 0x7ULL) != 0) {
        return false;
    }
    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    if (std::memcmp(cur, expected5, 5) != 0 && cur[0] != 0xE9) return false;

    std::uintptr_t relay = 0;
    bool chained = false;
    if (!EmitRelayAndTrampoline(site, 5U, detourFn, expected5, 5U, relay, outOrigTrampoline, chained)) {
        return false;
    }

    g_presentHook.valid = true;
    g_presentHook.active = false;
    g_presentHook.site  = site;
    g_presentHook.relay = relay;
    std::memcpy(g_presentHook.orig5, cur, 5);
    return true;
}

bool PrepareDynamicHook5(DynHookId id,
                         std::uintptr_t site,
                         void* detourFn,
                         void** outOrigTrampoline,
                         const std::uint8_t expected5[5]) {
    std::lock_guard<std::mutex> lk(g_mx);
    const auto idx = static_cast<std::size_t>(id);
    if (!g_codeArena || g_sealed || idx >= kDynCount || !site || !detourFn || !outOrigTrampoline || (site & 0x7ULL) != 0) {
        return false;
    }
    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    if (std::memcmp(cur, expected5, 5) != 0 && cur[0] != 0xE9) return false;

    std::uintptr_t relay = 0;
    bool chained = false;
    if (!EmitRelayAndTrampoline(site, 5U, detourFn, expected5, 5U, relay, outOrigTrampoline, chained)) {
        return false;
    }

    DynHook5Info& d = g_dynHooks[idx];
    d.prepared   = true;
    d.enabled    = false;
    d.wasChained = chained;
    d.site       = site;
    std::memcpy(d.orig5, cur, 5);

    d.jmp5[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(relay) - static_cast<std::int64_t>(site + 5U));
    std::memcpy(d.jmp5 + 1, &rel, 4);
    return true; // NOTE: .text is NOT patched! Stays 100% vanilla until Freeze is used!
}

bool PrepareLazyGuardHook(std::uintptr_t site,
                          std::size_t len,
                          void* detourFn,
                          void** outOrigTrampoline,
                          const std::uint8_t* displaced,
                          std::size_t dlen) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed || !site || !detourFn || !outOrigTrampoline || len < 5 || len > 16) {
        return false;
    }
    const auto* cur = reinterpret_cast<const std::uint8_t*>(site);
    if (std::memcmp(cur, displaced, dlen) != 0 && cur[0] != 0xE9) return false;

    std::uintptr_t relay = 0;
    bool chained = false;
    if (!EmitRelayAndTrampoline(site, len, detourFn, displaced, dlen, relay, outOrigTrampoline, chained)) {
        return false;
    }

    g_lazyGuard.prepared  = true;
    g_lazyGuard.installed = false;
    g_lazyGuard.site      = site;
    g_lazyGuard.len       = chained ? 5U : len;
    g_lazyGuard.relay     = relay;
    std::memcpy(g_lazyGuard.orig, cur, g_lazyGuard.len);
    return true; // NOTE: .text is NOT patched at startup!
}

bool FinalizeAndInstallPresentHook() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_codeArena || g_sealed || !g_presentHook.valid) return false;

    // 1. Seal trampoline arena as PAGE_EXECUTE_READ (strict W^X — zero RWX pages!)
    DWORD oldProt = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(g_codeArena), kCodeArenaSize,
                          PAGE_EXECUTE_READ, &oldProt)) {
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_codeArena), kCodeArenaSize);
    g_sealed = true;

    // 2. Install ONLY the 5-byte Present jump! All simulation/collision functions remain 100% vanilla!
    std::uint8_t jmp5[5]{};
    jmp5[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(g_presentHook.relay) - static_cast<std::int64_t>(g_presentHook.site + 5U));
    std::memcpy(jmp5 + 1, &rel, 4);

    if (!AtomicSwap5BytesAligned(g_presentHook.site, jmp5)) {
        return false;
    }
    g_presentHook.active = true;
    return true;
}

void SetDynamicHook5Enabled(DynHookId id, bool enabled) noexcept {
    const auto idx = static_cast<std::size_t>(id);
    if (idx >= kDynCount) return;
    DynHook5Info& d = g_dynHooks[idx];
    if (!g_sealed || !d.prepared || d.enabled == enabled) return;

    std::lock_guard<std::mutex> lk(g_mx);
    if (d.enabled == enabled) return;

    // Safety check if another plugin chained over our site after us: don't clobber foreign jmp on disable
    if (!enabled) {
        const auto* cur = reinterpret_cast<const std::uint8_t*>(d.site);
        if (std::memcmp(cur, d.jmp5, 5) != 0) {
            d.enabled = false;
            return;
        }
    }

    if (AtomicSwap5BytesAligned(d.site, enabled ? d.jmp5 : d.orig5)) {
        d.enabled = enabled;
    }
}

void EnsureLazyGuardHookInstalled() noexcept {
    if (!g_sealed || !g_lazyGuard.prepared || g_lazyGuard.installed) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_lazyGuard.installed) return;

    std::uint8_t p[16]{};
    p[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(g_lazyGuard.relay) - static_cast<std::int64_t>(g_lazyGuard.site + 5U));
    std::memcpy(p + 1, &rel, 4);
    for (std::size_t i = 5; i < g_lazyGuard.len; ++i) p[i] = 0x90;

    if (WriteBytes(g_lazyGuard.site, p, g_lazyGuard.len)) {
        g_lazyGuard.installed = true;
    }
}

void RemoveAllHooks() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_lazyGuard.installed && g_lazyGuard.site) {
        WriteBytes(g_lazyGuard.site, g_lazyGuard.orig, g_lazyGuard.len);
        g_lazyGuard.installed = false;
    }
    for (std::size_t i = 0; i < kDynCount; ++i) {
        if (g_dynHooks[i].enabled && g_dynHooks[i].site) {
            AtomicSwap5BytesAligned(g_dynHooks[i].site, g_dynHooks[i].orig5);
            g_dynHooks[i].enabled = false;
        }
    }
    if (g_presentHook.active && g_presentHook.site) {
        AtomicSwap5BytesAligned(g_presentHook.site, g_presentHook.orig5);
        g_presentHook.active = false;
    }
}

} // namespace hook

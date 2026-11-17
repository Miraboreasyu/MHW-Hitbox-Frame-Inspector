#include "hook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

struct SavedPatch {
    std::uintptr_t site = 0;
    std::size_t len = 0;
    std::uint8_t orig[16]{};
};

std::uintptr_t g_arena = 0;
std::size_t g_arenaSize = 0;
std::size_t g_arenaUsed = 0;
SavedPatch g_patches[16]{};
std::size_t g_patchCount = 0;
std::mutex g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to) {
    const std::int64_t d = (std::int64_t)to - (std::int64_t)from;
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocAt(std::uintptr_t addr, std::size_t size) {
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

void RecordPatch(std::uintptr_t site, std::size_t len) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_patchCount >= 16 || len > 16) return;
    SavedPatch& sp = g_patches[g_patchCount++];
    sp.site = site;
    sp.len = len;
    std::memcpy(sp.orig, reinterpret_cast<const void*>(site), len);
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_arena) return true;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;

    const std::uintptr_t span = 0x7F000000;
    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + size, lo)) continue;
        if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
    }
    if (!g_arena) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + size)) break;
            if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
        }
    }
    if (!g_arena) {
        for (int i = 0; i < 64 && !g_arena; ++i) {
            void* p = ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!p) break;
            const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
            if (InRel32(a, hi) && InRel32(a + size, lo)) g_arena = a;
            else ::VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!g_arena) return false;
    g_arenaSize = size;
    g_arenaUsed = 0;
    std::memset(reinterpret_cast<void*>(g_arena), 0xCC, size);
    return true;
}

std::uintptr_t ArenaBase() { return g_arena; }

std::uintptr_t Commit(const Emitter& e) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_arena) return 0;
    const std::size_t at = (g_arenaUsed + 15) & ~std::size_t(15);
    if (at + e.size() > g_arenaSize) return 0;
    std::memcpy(reinterpret_cast<void*>(g_arena + at), e.bytes().data(), e.size());
    g_arenaUsed = at + e.size();
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_arena + at), e.size());
    return g_arena + at;
}

bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n) {
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub) {
    if (len < 5 || len > 16 || !stub || !InRel32(site + 5, stub)) return false;
    RecordPatch(site, len);
    std::uint8_t p[16];
    p[0] = 0xE9;
    const std::int32_t rel = (std::int32_t)((std::int64_t)stub - (std::int64_t)(site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (std::size_t i = 5; i < len; ++i) p[i] = 0x90;
    return WriteMem(site, p, len);
}

bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen) {
    Emitter e(256);
    e.b({0x9C});                                        // pushfq
    e.b({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});    // push rax rcx rdx rbx rbp rsi rdi
    for (std::uint8_t r = 0; r < 8; ++r) e.b({0x41, std::uint8_t(0x50 + r)});   // push r8..r15
    e.b({0x48, 0x89, 0xE5});                            // mov rbp, rsp      (rbp = &Ctx)
    e.b({0x48, 0x83, 0xEC, 0x60});                      // sub rsp, 0x60
    e.b({0x48, 0x83, 0xE4, 0xF0});                      // and rsp, -16
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa [rsp+n*16], xmmN
        e.b({0x66, 0x0F, 0x7F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x83, 0xEC, 0x20});                      // sub rsp, 0x20     (shadow space)
    e.b({0x48, 0x89, 0xE9});                            // mov rcx, rbp
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(h));   // mov rax, handler
    e.b({0xFF, 0xD0});                                  // call rax
    e.b({0x48, 0x83, 0xC4, 0x20});                      // add rsp, 0x20
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa xmmN, [rsp+n*16]
        e.b({0x66, 0x0F, 0x6F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x89, 0xEC});                            // mov rsp, rbp
    for (int r = 7; r >= 0; --r) e.b({0x41, std::uint8_t(0x58 + r)});   // pop r15..r8
    e.b({0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58});    // pop rdi rsi rbp rbx rdx rcx rax
    e.b({0x9D});                                        // popfq
    e.raw(displaced, dlen);
    e.jmpAbs(site + len);

    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, len, stub);
}

bool InstallDetour(std::uintptr_t site, std::size_t len, void* detourFn,
                   void** outOrigTrampoline, const std::uint8_t* displaced, std::size_t dlen) {
    if (!site || !detourFn || !outOrigTrampoline || !displaced || len < 5 || len > 16) return false;

    Emitter origEmit(64);
    origEmit.raw(displaced, dlen);
    origEmit.jmpAbs(site + len);
    const std::uintptr_t origTramp = Commit(origEmit);
    if (!origTramp) return false;
    *outOrigTrampoline = reinterpret_cast<void*>(origTramp);

    Emitter relayEmit(32);
    relayEmit.jmpAbs(reinterpret_cast<std::uint64_t>(detourFn));
    const std::uintptr_t relay = Commit(relayEmit);
    if (!relay) return false;

    return PatchJump(site, len, relay);
}

void RemoveAllHooks() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (std::size_t i = g_patchCount; i > 0; --i) {
        const SavedPatch& sp = g_patches[i - 1];
        if (sp.site && sp.len > 0) {
            WriteMem(sp.site, sp.orig, sp.len);
        }
    }
    g_patchCount = 0;
}

} // namespace hook

#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

// Dynamic on-demand hooks that stay 100% UNPATCHED in MonsterHunterWorld.exe
// during normal gameplay (and even while [U] Hitbox display is ON!), and only
// patch/unpatch atomically when World Freeze [K] or Step [O] is engaged!
enum DynHookId : std::uint8_t {
    kDynGenericMove = 0, // cUnit::genericMove (0x142228F50, 5-byte single insn 48 89 5C 24 08)
    kDynHitDispatch = 1, // HitDispatch        (0x1412DD7C0, 5-byte single insn 48 89 5C 24 10)
    kDynCount       = 2
};

bool InitArena(std::uintptr_t lo, std::uintptr_t hi);
std::uintptr_t ArenaBase();

// Prepares the permanent 5-byte detour on cD3DRender::present (chains cleanly if another DLL hooked it).
bool PreparePermanentDetour5(std::uintptr_t site,
                             void* detourFn,
                             void** outOrigTrampoline,
                             const std::uint8_t expected5[5]);

// Pre-emits trampoline in the RX arena WITHOUT touching .text at startup!
bool PrepareDynamicHook5(DynHookId id,
                         std::uintptr_t site,
                         void* detourFn,
                         void** outOrigTrampoline,
                         const std::uint8_t expected5[5]);

// Pre-emits trampoline for GuardDispatch (6+ bytes); installed lazily on first World Freeze [K] use.
bool PrepareLazyGuardHook(std::uintptr_t site,
                          std::size_t len,
                          void* detourFn,
                          void** outOrigTrampoline,
                          const std::uint8_t* displaced,
                          std::size_t dlen);

// Seals the trampoline page as PAGE_EXECUTE_READ (strict W^X, zero RWX pages!)
// and installs ONLY the permanent Present hook.
bool FinalizeAndInstallPresentHook();

// Hardware-atomic 8-byte LOCK CMPXCHG swap of a 5-byte single instruction at an 8B-aligned site.
// When enabled=false, restores exact vanilla .text bytes so worker threads execute 100% native code!
void SetDynamicHook5Enabled(DynHookId id, bool enabled) noexcept;
void EnsureLazyGuardHookInstalled() noexcept;

void RemoveAllHooks();

} // namespace hook

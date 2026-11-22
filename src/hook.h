#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

// Dynamic on-demand hooks that stay 100% UNPATCHED in MonsterHunterWorld.exe
// during normal gameplay (and even while [U] Hitbox display is ON!), and only
// patch/unpatch atomically when World Freeze [K], Step [O], or Mesh Hiding is engaged!
enum DynHookId : std::uint8_t {
    kDynGenericMove = 0, // cUnit::genericMove (0x142228F50, 5-byte single insn 48 89 5C 24 08)
    kDynHitDispatch = 1, // HitDispatch        (0x1412DD7C0, 5-byte single insn 48 89 5C 24 10)
    kDynUnitDraw    = 2, // sUnit::draw        (0x14221A050, 5-byte single insn 48 89 5C 24 18)
    // World Freeze Delayed Burst capture hooks (enabled ONLY while World Freeze + burst option are active)
    kDynDmgNum      = 3, // sMhGUI::ShowDamageNumber       (0x141AE3140)
    kDynDmgNumZorah = 4, // sMhGUI::ShowDamageNumberZorah  (0x141AE3210)
    kDynDmgNumEx    = 5, // sMhGUI::ShowDamageNumberEx     (0x141AE32E0)
    kDynHitEfc      = 6, // cEmDamageCheck::spawnHitEfcHelper (0x1402BB830)
    kDynShake1      = 7, // sMhCamera::addShake1           (0x141AC5010)
    kDynShake2      = 8, // sMhCamera::addShake2           (0x141AC50B0)
    kDynHitFeedback = 9, // uPlayer::onAttackHitFeedback   (0x142035720)
    kDynCount       = 10
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

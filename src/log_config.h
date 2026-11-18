#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace app {

enum class FilterMode : int {
    Combat  = 0, // Player + Monster Hitboxes & Hurtboxes (default)
    Hitbox  = 1, // Attack Judgment Volumes (Hitboxes) only
    Hurtbox = 2, // Damageable Hitzones / Hurtboxes only
    All     = 3, // Everything including Pushboxes & Environmental Gimmicks
    Count   = 4
};

struct Config {
    bool enabled            = true;
    int vkToggleVis         = 'U';   // 0x55: Toggle hitbox/hurtbox display
    int vkToggleFreeze      = 'I';   // 0x49: Instant zero-delay full game freeze/resume
    int vkFreezeOthers      = 'K';   // 0x4B: Pause everything except the Player (World Freeze)
    int vkPersistHitbox     = 'J';   // 0x4A: Toggle keeping single-trigger hitboxes active while frozen
    int vkStepFrame         = 'O';   // 0x4F: Advance 1 frame while frozen (Full or World Freeze)
    int vkCycleFilter       = 'P';   // 0x50: Cycle display filter mode

    bool startVisOn         = false; // Whether hitbox display starts ON automatically
    bool keepHitboxesActive = true;  // Keep single-trigger frozen hitboxes active & repeatable in World Freeze
    bool showHudBanner      = true;  // Show compact top-left status indicator
    float lineThickness     = 2.0f;  // Base wireframe thickness in pixels (at 1080p)
    float opacityScale      = 1.0f;  // Global opacity multiplier (0.1 .. 1.5)
    float hdrNits           = 0.0f;  // HDR overlay luminance in nits (0 = auto from game HDR paper white)
};

void InitLog();
void CloseLog();
void Log(const char* fmt, ...);
std::string DllDirectory();
Config LoadConfig();
const char* FilterModeName(FilterMode m);

bool Readable(std::uintptr_t addr, std::size_t len);

template <typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) {
    if (!Readable(addr, sizeof(T))) return false;
    out = *reinterpret_cast<const T*>(addr);
    return true;
}

} // namespace app

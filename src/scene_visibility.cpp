#include "scene_visibility.h"
#include "hook.h"
#include "mhw_addresses.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace scene {
namespace {

using SUnitDrawFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);

enum class UnitRenderCat : std::uint8_t {
    AlwaysKeep  = 0, // 2D Player UI (uGUI/uMhGUI/uBGTexture/uDrawSubScene), Cameras, Lights, ToneMap/AA, Non-visual logic
    PlayerVfx   = 1, // Player, Weapons, Armor, Kinsect, Palico/Otomo, 3D VFX (hidden ONLY in HidePlVfx / HideAll3D)
    Monster     = 2, // Large/Small Monsters, Monster Part/Sub models, Monster Shells
    Interactive = 3, // Map Gimmicks, *InstanceTakeOver batches, cpBind* nodes, Wedge Beetles, Endemic Life, NPCs, Camp
    StageStatic = 4  // Static Stage/Map terrain, Static instances, Sky, Water, Fog, Decals, GodRays, Haze
};

struct DrawJob {
    std::uintptr_t unit;
    std::uintptr_t fn;
    std::uintptr_t pad0;
    std::uintptr_t pad1;
};

struct VisDtiEntry {
    std::uintptr_t dti;
    UnitRenderCat  cat;
    std::uint8_t   takeOverKind;        // 1=BaseModelTakeOver, 2=ModelTakeOver, 3=SpeedTreeTakeOver
    bool           checkModelGroupBits; // True on generic uMhModel / uModel / uBaseModel / uHavok*Model
    bool           checkBindComp;       // True on instanced / generic models that may carry cpBind*
    bool           isColorCorrect;      // True on uColorCorrectFilter (for pre-UI 3D dimming!)
};

constexpr std::uint32_t kMaxDrawJobs  = 10000;
constexpr std::uint32_t kVisCacheSize = 4096;

std::uintptr_t                  g_exeBase       = 0;
std::uintptr_t                  g_exeEnd        = 0;
SUnitDrawFn                     g_origSUnitDraw = nullptr;
std::atomic<bool>               g_hideMonster{false};
std::atomic<app::StageHideMode> g_stageHide{app::StageHideMode::Off};
std::atomic<app::BackdropMode>  g_backdrop{app::BackdropMode::Off};
std::atomic<DWORD>              g_activeDrawTid{0};

// Saved uColorCorrectFilter matrix/blend state for 3D-only DimScrim (never dims 2D Player UI!)
std::atomic_flag  g_ccDimLock     = ATOMIC_FLAG_INIT;
std::uintptr_t    g_savedCCUnit   = 0;
float             g_savedCCMat[16]{};
float             g_savedCCBlend  = 1.0f;
bool              g_hasSavedCCDim = false;

// Dedicated render-thread DTI cache & non-destructive filtered draw buffers (~1 MB static BSS, 0 heap allocs)
VisDtiEntry       g_visDtiCache[kVisCacheSize]{};
std::uintptr_t    g_filtUnits[kMaxDrawJobs]{};
DrawJob           g_filtJobs0[kMaxDrawJobs]{};
DrawJob           g_filtJobs1[kMaxDrawJobs]{};
DrawJob           g_filtJobs2[kMaxDrawJobs]{};

inline bool IsCanonicalPtr(std::uintptr_t p) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL;
}

inline bool IsAlignedPtr(std::uintptr_t p) noexcept {
    return IsCanonicalPtr(p) && ((p & 0x7ULL) == 0);
}

inline bool StartsWith(const char* s, const char* prefix) noexcept {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

std::uintptr_t GetUnitDti(std::uintptr_t obj) noexcept {
    if (!IsAlignedPtr(obj)) return 0;
    const std::uintptr_t vft = *reinterpret_cast<const std::uintptr_t*>(obj);
    if (vft < g_exeBase || vft + 0x28 > g_exeEnd || (vft & 0x7ULL) != 0) return 0;
    const std::uintptr_t fnGetDti = *reinterpret_cast<const std::uintptr_t*>(vft + mhw::kOffVftGetDti);
    if (fnGetDti < g_exeBase || fnGetDti + 8 > g_exeEnd) return 0;
    const auto* code = reinterpret_cast<const std::uint8_t*>(fnGetDti);
    if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05 && code[7] == 0xC3) {
        std::int32_t disp = 0;
        std::memcpy(&disp, code + 3, 4);
        const std::uintptr_t dti = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(fnGetDti + 7) + disp);
        if (dti >= g_exeBase && dti + 0x38 <= g_exeEnd) return dti;
    }
    return 0;
}

std::uint8_t DetectTakeOverKind(const char* n) noexcept {
    if (StartsWith(n, "uBaseModelInstanceTakeOver") || StartsWith(n, "uMhBaseModelInstanceTakeOver")) {
        return 1;
    }
    if (StartsWith(n, "uModelInstanceTakeOver") || StartsWith(n, "uMhModelInstanceTakeOver")) {
        return 2;
    }
    if (StartsWith(n, "uSpeedTreeInstanceTakeOver") || StartsWith(n, "uMhSpeedTreeInstanceTakeOver")) {
        return 3;
    }
    return 0;
}

bool IsAlwaysKeepClass(const char* n) noexcept {
    // 1. 2D Player UI / HUD / Subtitles / Sub-scene & uBGTexture (0x142454de0 composites 2D UI layers!)
    if (StartsWith(n, "uGUI") || StartsWith(n, "uMhGUI") || StartsWith(n, "uMhSubtitle") ||
        StartsWith(n, "uBGTexture") || StartsWith(n, "uScreenSpace") ||
        StartsWith(n, "uDrawSubScene") || StartsWith(n, "uCinemaScope")) {
        return true;
    }
    // 2. Non-visual collision / AI / constraint / scheduler / audio logic units
    if (StartsWith(n, "uDynamicSbc") || StartsWith(n, "uScrollCollision") ||
        StartsWith(n, "uGeometry2Collider") || StartsWith(n, "uGeometry2GroupCollider") ||
        StartsWith(n, "uAI") || StartsWith(n, "uAi") ||
        StartsWith(n, "uConstraint") || StartsWith(n, "uScheduler") || StartsWith(n, "uWwise")) {
        return true;
    }
    // 3. Cameras, Lights, Probes, Exposure/ToneMap/AA/Color post-process filters
    if (StartsWith(n, "uCamera") || StartsWith(n, "uMhCamera") ||
        StartsWith(n, "uMhFollowCamera") || StartsWith(n, "uLlkFollowCamera") ||
        StartsWith(n, "LightBase") ||
        StartsWith(n, "uPointLight") || StartsWith(n, "uSpotLight") ||
        StartsWith(n, "uInfiniteLight") || StartsWith(n, "uHemiSphereLight") ||
        StartsWith(n, "uVolumeSpotLight") || StartsWith(n, "uMhPointLight") ||
        StartsWith(n, "uMhSpotLight") || StartsWith(n, "uMhInfiniteLight") ||
        StartsWith(n, "uMhHemiSphereLight") || StartsWith(n, "uMhVolumeSpotLight") ||
        StartsWith(n, "uMhActorInfiniteLight") || StartsWith(n, "uMhActorPointLight") ||
        StartsWith(n, "uMhActorSpotLight") || StartsWith(n, "uLlkActorInfiniteLight") ||
        StartsWith(n, "uLlkHemiSphereLight") || StartsWith(n, "uLlkInfiniteLight") ||
        StartsWith(n, "uEnvMapper") || StartsWith(n, "uLightProbe") || StartsWith(n, "uSpecularProbe") ||
        StartsWith(n, "uSdlLightProbe") || StartsWith(n, "uSdlInfinite") ||
        StartsWith(n, "uSdlGI") || StartsWith(n, "uGIOverrider") ||
        StartsWith(n, "uBoardLightness") || StartsWith(n, "uFilterOverrider") ||
        StartsWith(n, "uToneMap") || StartsWith(n, "uSdlToneMap") ||
        StartsWith(n, "uColorCorrect") || StartsWith(n, "uColorCube") || StartsWith(n, "uSdlColor") ||
        StartsWith(n, "uAntiAlias") || StartsWith(n, "uTemporalAA") || StartsWith(n, "uSdlTemporalAA") ||
        StartsWith(n, "uFXAA") || StartsWith(n, "uSdlFXAA") ||
        StartsWith(n, "uOutlineFilter") || StartsWith(n, "uKerareFilter") ||
        StartsWith(n, "uImagePlaneFilter") || StartsWith(n, "uBloomFilter") ||
        StartsWith(n, "uFFTBloomFilter")) {
        return true;
    }
    return false;
}

bool IsPlayerOrVfxClass(const char* n) noexcept {
    // Player, Weapons, Armor, Kinsect, Claw, Player Shells, Palico/Otomo, and 3D VFX
    return StartsWith(n, "uPlayer") || StartsWith(n, "uPl") ||
           StartsWith(n, "uWeapon") || StartsWith(n, "uWp") ||
           StartsWith(n, "uHuman") || StartsWith(n, "uAccessory") ||
           StartsWith(n, "uMhActorPl") || StartsWith(n, "uHmDebuffBody") ||
           StartsWith(n, "uDummyArmor") || StartsWith(n, "uDummyWeapon") ||
           StartsWith(n, "uShellPl") || StartsWith(n, "uShellAskill") ||
           StartsWith(n, "uShellClaw") || StartsWith(n, "uShellWp") ||
           StartsWith(n, "uShellHitWp") || StartsWith(n, "uShellMushiAmi") ||
           StartsWith(n, "uFT031Insect") || StartsWith(n, "uInsect") ||
           StartsWith(n, "uOtomo") || StartsWith(n, "uOt") ||
           StartsWith(n, "uPhotomo") || StartsWith(n, "uCat") ||
           StartsWith(n, "uMhActorOtomo") || StartsWith(n, "uShellOtomo") ||
           StartsWith(n, "uShellOtasuke") || StartsWith(n, "uPugee") ||
           StartsWith(n, "uSdlAddonPl") || StartsWith(n, "uSdlAddonOtomo") ||
           StartsWith(n, "uBaseEffect") || StartsWith(n, "uEffect") ||
           StartsWith(n, "uMhEffect") || StartsWith(n, "uEpv") || StartsWith(n, "uMhEpv");
}

bool IsMonsterVisualClass(const char* n) noexcept {
    return StartsWith(n, "Monster") || StartsWith(n, "uEnemy") ||
           StartsWith(n, "uCaptureEnemy") ||
           StartsWith(n, "uEm0") || StartsWith(n, "uEm1") || StartsWith(n, "uEms") ||
           StartsWith(n, "uEmEmbankment") || StartsWith(n, "uEmFoodModel") ||
           StartsWith(n, "uEmClawGrab") || StartsWith(n, "uMhActorModelEnemy") ||
           StartsWith(n, "uSdlAddonEnemy") || StartsWith(n, "uShellEm") ||
           StartsWith(n, "uShellBoundEm") || StartsWith(n, "uShellLaserEm") ||
           StartsWith(n, "uShellLaserMarkEm") || StartsWith(n, "uShellTestEm") ||
           StartsWith(n, "uShellSlingerEmBlowCombo");
}

bool IsInteractiveMapClass(const char* n) noexcept {
    // Map Gimmicks, Gathering Nodes, Wedge Beetles / Endemic Life, Camp / Quest Board / Item Box,
    // NPCs, Scoutfly Traces, Vines, Ropes, Traps, Boulders
    return StartsWith(n, "uGm") || StartsWith(n, "uGimmick") || StartsWith(n, "uHmGimmick") ||
           StartsWith(n, "uStageGimmick") ||
           StartsWith(n, "uBindInstance") || StartsWith(n, "uBindBaseModel") ||
           StartsWith(n, "uBindModel") || StartsWith(n, "uBindSpeedTree") ||
           StartsWith(n, "uBindBakeInstance") ||
           StartsWith(n, "uAnimal") || StartsWith(n, "uMhActorModelAnimal") ||
           StartsWith(n, "uCageInsect") || StartsWith(n, "uGuideInsect") || StartsWith(n, "uFish") ||
           StartsWith(n, "uFurniture") || StartsWith(n, "uInteractFurniture") || StartsWith(n, "uFtDecoration") ||
           StartsWith(n, "uNpc") || StartsWith(n, "uScholar") || StartsWith(n, "uScrNpc") ||
           StartsWith(n, "uWorker") || StartsWith(n, "uFacialNPC") ||
           StartsWith(n, "uTraceModel") || StartsWith(n, "uReactTrace") || StartsWith(n, "uMhActorModelTrace") ||
           StartsWith(n, "uSlantFloor") || StartsWith(n, "uIvy") ||
           StartsWith(n, "uPutFootRope") || StartsWith(n, "uRelayRope") ||
           StartsWith(n, "uRope") || StartsWith(n, "uBallistaRestraintRope") ||
           StartsWith(n, "uShellGm") || StartsWith(n, "uShellLaserGm") ||
           StartsWith(n, "uShellStage") || StartsWith(n, "uShellBallista") ||
           StartsWith(n, "uShellDragonCannon") || StartsWith(n, "uShellOnsenRock") ||
           StartsWith(n, "uShellBreakable") || StartsWith(n, "uShellAirIvy") ||
           StartsWith(n, "uShellFallIvy") || StartsWith(n, "uShellGroundIvy") ||
           StartsWith(n, "uShellPlant") || StartsWith(n, "uShellHitLightPlant") ||
           StartsWith(n, "uShellNut");
}

bool IsBindCapableInstanceClass(const char* n) noexcept {
    return StartsWith(n, "uBaseModelInstance") || StartsWith(n, "uMhBaseModelInstance") ||
           StartsWith(n, "uModelInstance") || StartsWith(n, "uMhModelInstance") ||
           StartsWith(n, "uSpeedTree") || StartsWith(n, "uMhSpeedTree");
}

bool IsStaticStageVisualClass(const char* n) noexcept {
    // Note: uBGTexture is in IsAlwaysKeepClass (checked first!), and *InstanceTakeOver is checked before this!
    return IsBindCapableInstanceClass(n) ||
           StartsWith(n, "uBakeInstance") ||
           StartsWith(n, "uInstancePlacement") || StartsWith(n, "uScrInstance") ||
           StartsWith(n, "uStage") || StartsWith(n, "uBg") ||
           StartsWith(n, "uEnvObject") || StartsWith(n, "uEnvRock") || StartsWith(n, "uEnvTree") ||
           StartsWith(n, "uSky") || StartsWith(n, "uMhSky") || StartsWith(n, "uSdlSkyModel") ||
           StartsWith(n, "uStarrySky") || StartsWith(n, "uOcean") ||
           StartsWith(n, "uWater") || StartsWith(n, "uMhWater") ||
           StartsWith(n, "uDistanceFog") || StartsWith(n, "uMhDistanceFog") || StartsWith(n, "uSdlDistanceFog") ||
           StartsWith(n, "uVolumeFog") || StartsWith(n, "uSdlVolumeFog") || StartsWith(n, "uVolumetric") ||
           StartsWith(n, "uHaze") || StartsWith(n, "uSdlHaze") ||
           StartsWith(n, "uGodRays") || StartsWith(n, "uSdlGodRays") || StartsWith(n, "uLightShaft") ||
           StartsWith(n, "uDecal") || StartsWith(n, "uMhDecal") || StartsWith(n, "uAssetBasic") ||
           StartsWith(n, "uGeometry2") || StartsWith(n, "uGeometry3") ||
           StartsWith(n, "uLlkBaseModel") || StartsWith(n, "uScrModel") || StartsWith(n, "uScrMotionModel") ||
           StartsWith(n, "uMhActorModelSimple") || StartsWith(n, "uMhTinyChainModel") ||
           StartsWith(n, "uSnowField") || StartsWith(n, "uSnowShoveler") ||
           StartsWith(n, "uShellGrassWind");
}

VisDtiEntry* ResolveVisDti(std::uintptr_t dti) noexcept {
    if (!dti) return nullptr;
    std::uint32_t idx = static_cast<std::uint32_t>((dti >> 4) * 2654435761ULL) & (kVisCacheSize - 1);
    for (std::uint32_t probe = 0; probe < 24; ++probe) {
        if (g_visDtiCache[idx].dti == dti) return &g_visDtiCache[idx];
        if (g_visDtiCache[idx].dti == 0 || probe == 23) {
            VisDtiEntry& e = g_visDtiCache[idx];
            e.dti                 = dti;
            e.cat                 = UnitRenderCat::AlwaysKeep;
            e.takeOverKind        = 0;
            e.checkModelGroupBits = false;
            e.checkBindComp       = false;
            e.isColorCorrect      = false;

            // Walk from leaf DTI upward: check specialized leaf classes BEFORE base classes (uMhModel/uModel)
            std::uintptr_t cur = dti;
            bool decided = false;
            bool sawGenericModel = false;
            for (int depth = 0; depth < 20 && IsAlignedPtr(cur); ++depth) {
                const char* cls = *reinterpret_cast<const char* const*>(cur + mhw::kOffDtiName);
                if (cls && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(cls))) {
                    const unsigned char c0 = static_cast<unsigned char>(cls[0]);
                    if (c0 >= 0x20 && c0 <= 0x7E) {
                        if (std::strcmp(cls, "uColorCorrectFilter") == 0) {
                            e.isColorCorrect = true;
                        }
                        const std::uint8_t tk = DetectTakeOverKind(cls);
                        if (tk != 0) {
                            // *InstanceTakeOver batches uGimmickModel & uGimmickSpeedTree (0x1424d62c0/0x1423e5590/0x1426db810)!
                            e.cat          = UnitRenderCat::Interactive;
                            e.takeOverKind = tk;
                            decided        = true;
                            break;
                        }
                        if (IsAlwaysKeepClass(cls)) {
                            e.cat   = UnitRenderCat::AlwaysKeep;
                            decided = true;
                            break;
                        }
                        if (IsMonsterVisualClass(cls)) {
                            e.cat   = UnitRenderCat::Monster;
                            decided = true;
                            break;
                        }
                        if (IsPlayerOrVfxClass(cls)) {
                            e.cat   = UnitRenderCat::PlayerVfx;
                            decided = true;
                            break;
                        }
                        if (IsInteractiveMapClass(cls)) {
                            e.cat   = UnitRenderCat::Interactive;
                            decided = true;
                            break;
                        }
                        if (IsStaticStageVisualClass(cls)) {
                            e.cat           = UnitRenderCat::StageStatic;
                            e.checkBindComp = IsBindCapableInstanceClass(cls);
                            decided         = true;
                            break;
                        }
                        // Generic stage/bake meshes & uGimmick child models are direct instances of
                        // uMhModel / uMhBaseModel / uModel / uBaseModel / uHavokBaseModel / uHavokScrollModel
                        if (std::strcmp(cls, "uMhModel") == 0 ||
                            std::strcmp(cls, "uMhBaseModel") == 0 ||
                            std::strcmp(cls, "uModel") == 0 ||
                            std::strcmp(cls, "uBaseModel") == 0 ||
                            StartsWith(cls, "uHavokBaseModel") ||
                            StartsWith(cls, "uHavokScrollModel")) {
                            sawGenericModel = true;
                        }
                    }
                }
                cur = *reinterpret_cast<const std::uintptr_t*>(cur + mhw::kOffDtiParent);
            }

            if (!decided && sawGenericModel) {
                e.cat                 = UnitRenderCat::StageStatic;
                e.checkModelGroupBits = true;
                e.checkBindComp       = true;
            }
            return &e;
        }
        idx = (idx + 1) & (kVisCacheSize - 1);
    }
    return nullptr;
}

// Fast O(1) check: static instances have mpRoot == 0 (1 load); bound gathering/destructible instances
// spawned by uBindInstancePlacement / uBindBakeInstancePlacement carry cpBindInstanceController!
inline bool HasBindComponent(std::uintptr_t unit) noexcept {
    const std::uintptr_t head = *reinterpret_cast<const std::uintptr_t*>(unit + mhw::kOffCompMgrHead);
    if (!IsAlignedPtr(head)) return false;
    std::uintptr_t comp = head;
    for (int i = 0; i < 12 && IsAlignedPtr(comp); ++i) {
        const std::uintptr_t cdti = GetUnitDti(comp);
        if (cdti) {
            const char* cname = *reinterpret_cast<const char* const*>(cdti + mhw::kOffDtiName);
            if (cname && IsCanonicalPtr(reinterpret_cast<std::uintptr_t>(cname)) &&
                StartsWith(cname, "cpBind")) {
                return true;
            }
        }
        comp = *reinterpret_cast<const std::uintptr_t*>(comp + mhw::kOffCompNext);
    }
    return false;
}

inline bool ShouldSkipUnitDraw(std::uintptr_t unit,
                               bool hideEm,
                               app::StageHideMode stageMode,
                               app::BackdropMode backdrop,
                               std::uint8_t& outTakeOverKind) noexcept {
    outTakeOverKind = 0;
    if (!IsAlignedPtr(unit)) return false;
    const std::uintptr_t dti = GetUnitDti(unit);
    VisDtiEntry* e = ResolveVisDti(dti);
    if (!e || e->cat == UnitRenderCat::AlwaysKeep) return false;

    outTakeOverKind = e->takeOverKind;
    UnitRenderCat effCat = e->cat;

    if (e->checkModelGroupBits) {
        const std::uint64_t grp = *reinterpret_cast<const std::uint64_t*>(unit + mhw::kOffUnitGroup);
        if ((grp & (mhw::kUnitGroupPlayerBit | mhw::kUnitGroupPlSubModelBit)) != 0) {
            effCat = UnitRenderCat::PlayerVfx;
        } else if ((grp & mhw::kUnitGroupEmSubModelBit) != 0) {
            effCat = UnitRenderCat::Monster;
        } else if ((grp & (mhw::kUnitGroupGimmickBit | mhw::kUnitGroupGmSubModelBit)) != 0) {
            effCat = UnitRenderCat::Interactive;
        }
    }

    if (effCat == UnitRenderCat::StageStatic && e->checkBindComp && HasBindComponent(unit)) {
        effCat = UnitRenderCat::Interactive;
    }

    const bool hideAll3D = (backdrop == app::BackdropMode::HideAll3D);

    switch (effCat) {
        case UnitRenderCat::PlayerVfx:
            return hideAll3D || (backdrop == app::BackdropMode::HidePlVfx);
        case UnitRenderCat::Monster:
            return hideAll3D || hideEm;
        case UnitRenderCat::Interactive:
            return hideAll3D || (stageMode == app::StageHideMode::HideAll);
        case UnitRenderCat::StageStatic:
            return hideAll3D || (stageMode != app::StageHideMode::Off);
        default:
            return false;
    }
}

// Replicates cUnit::genericDraw (0x142228de0) + uCoord::markDrawn (0x1422290f0) + *InstanceTakeOver
// child stamping (0x1423e1b90) across ALL main-camera passes (deferred G-Buffer 0x80|0x100, Z 0x20,
// forward 0x01, etc., excluding shadow 0x0602 and cubemap 0x0008).
// Without stamping +0x54 (mCurFrameDrawnPassMask) on both direct models and batched uGimmickModel /
// uGimmickSpeedTree children, uGimmick::move (0x141ceb070) sees *(int32_t*)(model + 0x50) == 0 and
// calls cpObjCollision::setEnabled(coll, 0), disabling map interactions!
inline void PreserveSkippedUnitVisibility(std::uintptr_t u,
                                          std::uint32_t passMask,
                                          std::uint32_t viewBit,
                                          std::uint8_t takeOverKind) noexcept {
    const std::uint32_t camPass = passMask & 0xF9F5u;
    if (camPass == 0 || !IsAlignedPtr(u)) return;

    std::uint32_t stampedPass = 0;
    if (*reinterpret_cast<const std::int32_t*>(u + mhw::kOffUnitAliveFlags) == 8 &&
        (*reinterpret_cast<const std::uint8_t*>(u + mhw::kOffUnitDrawFlags) & 0x12u) == 0x12u &&
        (*reinterpret_cast<const std::uint32_t*>(u + mhw::kOffUnitViewportMask) & viewBit) != 0) {
        stampedPass = *reinterpret_cast<const std::uint32_t*>(u + mhw::kOffUnitPassMask) & camPass;
        if (stampedPass != 0) {
            *reinterpret_cast<std::uint32_t*>(u + mhw::kOffUnitCurDrawnMask) |= stampedPass;
        }
    }

    if (takeOverKind != 0 && stampedPass != 0) {
        std::size_t mgrOff = 0;
        if (takeOverKind == 1)      mgrOff = mhw::kOffBaseModelTakeOverMgr;
        else if (takeOverKind == 2) mgrOff = mhw::kOffModelTakeOverMgr;
        else if (takeOverKind == 3) mgrOff = mhw::kOffSpeedTreeTakeOverMgr;

        if (mgrOff != 0) {
            const std::uintptr_t arrPtr = *reinterpret_cast<const std::uintptr_t*>(
                u + mgrOff + mhw::kOffTakeOverUnitArray);
            const std::uint32_t cnt = *reinterpret_cast<const std::uint32_t*>(
                u + mgrOff + mhw::kOffTakeOverUnitCount);
            if (cnt > 0 && cnt <= 4096 && IsAlignedPtr(arrPtr)) {
                const auto* children = reinterpret_cast<const std::uintptr_t*>(arrPtr);
                for (std::uint32_t k = 0; k < cnt; ++k) {
                    const std::uintptr_t ch = children[k];
                    if (!IsAlignedPtr(ch)) continue;
                    if (*reinterpret_cast<const std::int32_t*>(ch + mhw::kOffUnitAliveFlags) == 8 &&
                        (*reinterpret_cast<const std::uint8_t*>(ch + mhw::kOffUnitDrawFlags) & 0x12u) == 0x12u &&
                        (*reinterpret_cast<const std::uint32_t*>(ch + mhw::kOffUnitViewportMask) & viewBit) != 0) {
                        *reinterpret_cast<std::uint32_t*>(ch + mhw::kOffUnitCurDrawnMask) |= stampedPass;
                    }
                }
            }
        }
    }
}

void RestoreSavedCCDimFilterSEH() noexcept {
    while (g_ccDimLock.test_and_set(std::memory_order_acquire)) {}
    if (g_hasSavedCCDim) {
        const std::uintptr_t cc = g_savedCCUnit;
        __try {
            if (IsAlignedPtr(cc) && GetUnitDti(cc) != 0) {
                std::memcpy(reinterpret_cast<void*>(cc + mhw::kOffCCFilterMatrix),
                            g_savedCCMat, sizeof(g_savedCCMat));
                *reinterpret_cast<float*>(cc + mhw::kOffCCFilterBlendRate) = g_savedCCBlend;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_hasSavedCCDim = false;
        g_savedCCUnit   = 0;
    }
    g_ccDimLock.clear(std::memory_order_release);
}

void ApplyCCDimFilterOnUnit(std::uintptr_t cc, float brightnessMul) noexcept {
    static constexpr float kIdent4x4[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    while (g_ccDimLock.test_and_set(std::memory_order_acquire)) {}
    if (!g_hasSavedCCDim && IsAlignedPtr(cc)) {
        auto* matPtr = reinterpret_cast<float*>(cc + mhw::kOffCCFilterMatrix);
        auto* bldPtr = reinterpret_cast<float*>(cc + mhw::kOffCCFilterBlendRate);
        std::memcpy(g_savedCCMat, matPtr, sizeof(g_savedCCMat));
        g_savedCCBlend  = *bldPtr;
        g_savedCCUnit   = cc;
        g_hasSavedCCDim = true;

        float b = g_savedCCBlend;
        if (b < 0.0f) b = 0.0f;
        else if (b > 1.0f) b = 1.0f;

        // Blend original stage matrix with identity, then scale RGB rows/cols [0..14] by brightnessMul
        // (100% invariant to row-major vs column-major!). Runs strictly in 3D sFilterSwapchain BEFORE 2D Player UI!
        for (int i = 0; i < 15; ++i) {
            const float eff = b * g_savedCCMat[i] + (1.0f - b) * kIdent4x4[i];
            matPtr[i] = eff * brightnessMul;
        }
        matPtr[15] = 1.0f;
        *bldPtr    = 1.0f;
    }
    g_ccDimLock.clear(std::memory_order_release);
}

void RunFilteredSUnitDrawSEH(std::uintptr_t unitSys,
                             std::uintptr_t draw,
                             bool hideEm,
                             app::StageHideMode stageMode,
                             app::BackdropMode backdrop) noexcept {
    // Lock-free re-entrancy & single-writer guard for static filtered job buffers (0 syscalls: gs:[48h])
    const DWORD tid = ::GetCurrentThreadId();
    DWORD expectedTid = 0;
    if (!g_activeDrawTid.compare_exchange_strong(
            expectedTid, tid, std::memory_order_acquire, std::memory_order_relaxed)) {
        g_origSUnitDraw(unitSys, draw);
        return;
    }

    bool swapped            = false;
    std::uintptr_t* pUnits  = nullptr;
    std::uintptr_t* pJobs0  = nullptr;
    std::uintptr_t* pJobs1  = nullptr;
    std::uintptr_t* pJobs2  = nullptr;
    std::uint32_t*  pCnt0   = nullptr;
    std::uint32_t*  pCnt1   = nullptr;
    std::uint32_t*  pCnt2   = nullptr;
    std::uintptr_t origUnits = 0;
    std::uintptr_t origJobs0 = 0;
    std::uintptr_t origJobs1 = 0;
    std::uintptr_t origJobs2 = 0;
    std::uint32_t  origCnt0  = 0;
    std::uint32_t  origCnt1  = 0;
    std::uint32_t  origCnt2  = 0;

    __try {
        if (!IsAlignedPtr(unitSys) || !IsAlignedPtr(draw) ||
            *reinterpret_cast<const std::uint8_t*>(unitSys + mhw::kOffUnitSysUseDrawList) == 0) {
            g_origSUnitDraw(unitSys, draw);
            g_activeDrawTid.store(0, std::memory_order_release);
            return;
        }

        const std::uint32_t passMask = *reinterpret_cast<const std::uint16_t*>(draw + mhw::kOffCDrawPassMask);
        const std::uint32_t viewIdx  = *reinterpret_cast<const std::uint16_t*>(draw + mhw::kOffCDrawViewIdx) & 0xFu;
        const std::uint32_t viewBit  = 1u << viewIdx;

        // Restore any previous 3D viewport's temporary uColorCorrectFilter dim state before new viewport draw
        // (and immediately when 2D GUI viewport 7 runs right after 3D sFilterSwapchain::execute @ 0x1424f1fc0!)
        if (passMask == 1u) {
            RestoreSavedCCDimFilterSEH();
        }

        // Never filter non-camera 2D GUI sub-scene viewports (e.g. viewport 7 in uDrawSubScene @ 0x1424010e0)
        if (passMask == 1u && g_exeBase != 0) {
            const std::uintptr_t mhMain = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + mhw::kRvaSMhMainInstance);
            if (IsAlignedPtr(mhMain)) {
                const std::uintptr_t camSys = *reinterpret_cast<const std::uintptr_t*>(mhMain + mhw::kOffMhMainCamera);
                if (IsAlignedPtr(camSys)) {
                    const std::uintptr_t vp = camSys + static_cast<std::size_t>(viewIdx) * mhw::kCameraViewportStride;
                    if (*reinterpret_cast<const std::uintptr_t*>(vp + mhw::kOffVpCameraPtr) == 0) {
                        g_origSUnitDraw(unitSys, draw);
                        g_activeDrawTid.store(0, std::memory_order_release);
                        return;
                    }
                }
            }
        }

        pUnits = reinterpret_cast<std::uintptr_t*>(unitSys + mhw::kOffUnitSysDrawUnits);
        pJobs0 = reinterpret_cast<std::uintptr_t*>(unitSys + mhw::kOffUnitSysDrawJobs0);
        pJobs1 = reinterpret_cast<std::uintptr_t*>(unitSys + mhw::kOffUnitSysDrawJobs1);
        pJobs2 = reinterpret_cast<std::uintptr_t*>(unitSys + mhw::kOffUnitSysDrawJobs2);
        pCnt0  = reinterpret_cast<std::uint32_t*>(unitSys + mhw::kOffUnitSysDrawCnt0);
        pCnt1  = reinterpret_cast<std::uint32_t*>(unitSys + mhw::kOffUnitSysDrawCnt1);
        pCnt2  = reinterpret_cast<std::uint32_t*>(unitSys + mhw::kOffUnitSysDrawCnt2);

        origUnits = *pUnits;
        origJobs0 = *pJobs0;
        origJobs1 = *pJobs1;
        origJobs2 = *pJobs2;
        origCnt0  = *pCnt0;
        origCnt1  = *pCnt1;
        origCnt2  = *pCnt2;

        if (!IsAlignedPtr(origUnits) || !IsAlignedPtr(origJobs0) ||
            !IsAlignedPtr(origJobs1) || !IsAlignedPtr(origJobs2) ||
            origCnt0 > kMaxDrawJobs || origCnt1 > kMaxDrawJobs || origCnt2 > kMaxDrawJobs) {
            g_origSUnitDraw(unitSys, draw);
            g_activeDrawTid.store(0, std::memory_order_release);
            return;
        }

        const auto* srcUnits = reinterpret_cast<const std::uintptr_t*>(origUnits);
        const auto* srcJobs0 = reinterpret_cast<const DrawJob*>(origJobs0);
        const auto* srcJobs1 = reinterpret_cast<const DrawJob*>(origJobs1);
        const auto* srcJobs2 = reinterpret_cast<const DrawJob*>(origJobs2);

        const bool want3DDim = (passMask == 1u) && (backdrop == app::BackdropMode::DimScrim);
        bool applied3DDim    = false;
        auto tryApplyCCDim = [&](std::uintptr_t u) noexcept {
            if (want3DDim && !applied3DDim && IsAlignedPtr(u)) {
                VisDtiEntry* ce = ResolveVisDti(GetUnitDti(u));
                if (ce && ce->isColorCorrect &&
                    *reinterpret_cast<const std::int32_t*>(u + mhw::kOffUnitAliveFlags) == 8 &&
                    (*reinterpret_cast<const std::uint8_t*>(u + mhw::kOffUnitDrawFlags) & 0x12u) == 0x12u &&
                    (*reinterpret_cast<const std::uint32_t*>(u + mhw::kOffUnitViewportMask) & viewBit) != 0) {
                    // Dim 3D scene by 45% (-> 55% brightness) before 2D Player UI composites!
                    ApplyCCDimFilterOnUnit(u, 0.55f);
                    applied3DDim = true;
                }
            }
        };

        std::uint32_t out0 = 0;
        for (std::uint32_t i = 0; i < origCnt0; ++i) {
            const std::uintptr_t u = srcJobs0[i].unit;
            tryApplyCCDim(u);
            std::uint8_t tk = 0;
            if (!ShouldSkipUnitDraw(u, hideEm, stageMode, backdrop, tk)) {
                g_filtUnits[out0] = srcUnits[i];
                g_filtJobs0[out0] = srcJobs0[i];
                ++out0;
            } else {
                PreserveSkippedUnitVisibility(u, passMask, viewBit, tk);
            }
        }

        std::uint32_t out1 = 0;
        for (std::uint32_t i = 0; i < origCnt1; ++i) {
            const std::uintptr_t u = srcJobs1[i].unit;
            tryApplyCCDim(u);
            std::uint8_t tk = 0;
            if (!ShouldSkipUnitDraw(u, hideEm, stageMode, backdrop, tk)) {
                g_filtJobs1[out1++] = srcJobs1[i];
            } else {
                PreserveSkippedUnitVisibility(u, passMask, viewBit, tk);
            }
        }

        std::uint32_t out2 = 0;
        for (std::uint32_t i = 0; i < origCnt2; ++i) {
            const std::uintptr_t u = srcJobs2[i].unit;
            tryApplyCCDim(u);
            std::uint8_t tk = 0;
            if (!ShouldSkipUnitDraw(u, hideEm, stageMode, backdrop, tk)) {
                g_filtJobs2[out2++] = srcJobs2[i];
            } else {
                PreserveSkippedUnitVisibility(u, passMask, viewBit, tk);
            }
        }

        // Non-destructively swap to filtered draw buffers for this pass, then restore verbatim!
        *pUnits = reinterpret_cast<std::uintptr_t>(g_filtUnits);
        *pJobs0 = reinterpret_cast<std::uintptr_t>(g_filtJobs0);
        *pJobs1 = reinterpret_cast<std::uintptr_t>(g_filtJobs1);
        *pJobs2 = reinterpret_cast<std::uintptr_t>(g_filtJobs2);
        *pCnt0  = out0;
        *pCnt1  = out1;
        *pCnt2  = out2;
        swapped = true;

        g_origSUnitDraw(unitSys, draw);

        *pUnits = origUnits;
        *pJobs0 = origJobs0;
        *pJobs1 = origJobs1;
        *pJobs2 = origJobs2;
        *pCnt0  = origCnt0;
        *pCnt1  = origCnt1;
        *pCnt2  = origCnt2;
        swapped = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (swapped) {
            *pUnits = origUnits;
            *pJobs0 = origJobs0;
            *pJobs1 = origJobs1;
            *pJobs2 = origJobs2;
            *pCnt0  = origCnt0;
            *pCnt1  = origCnt1;
            *pCnt2  = origCnt2;
        } else if (g_origSUnitDraw) {
            g_origSUnitDraw(unitSys, draw);
        }
    }

    g_activeDrawTid.store(0, std::memory_order_release);
}

} // namespace

void InitModuleRange(std::uintptr_t exeBase, std::uintptr_t exeEnd) noexcept {
    g_exeBase = exeBase;
    g_exeEnd  = exeEnd;
}

void SetOrigSUnitDraw(void* origFn) noexcept {
    g_origSUnitDraw = reinterpret_cast<SUnitDrawFn>(origFn);
}

void __fastcall Hook_SUnitDraw(std::uintptr_t unitSys, std::uintptr_t draw) {
    if (!g_origSUnitDraw) return;
    const bool hideEm                  = g_hideMonster.load(std::memory_order_relaxed);
    const app::StageHideMode stageMode = g_stageHide.load(std::memory_order_relaxed);
    const app::BackdropMode  backdrop  = g_backdrop.load(std::memory_order_relaxed);
    const bool needUnitFilter = hideEm ||
                                (stageMode != app::StageHideMode::Off) ||
                                (backdrop != app::BackdropMode::Off);
    if (!needUnitFilter) {
        RestoreSavedCCDimFilterSEH();
        g_origSUnitDraw(unitSys, draw);
        return;
    }
    RunFilteredSUnitDrawSEH(unitSys, draw, hideEm, stageMode, backdrop);
}

void SyncState(std::uintptr_t /*exeBase*/,
               bool hideMonster,
               app::StageHideMode stageHide,
               app::BackdropMode backdrop) noexcept {
    // Present runs after sCamera::draw + sFilterSwapchain::execute have finished for the frame
    RestoreSavedCCDimFilterSEH();
    g_hideMonster.store(hideMonster, std::memory_order_relaxed);
    g_stageHide.store(stageHide, std::memory_order_relaxed);
    g_backdrop.store(backdrop, std::memory_order_relaxed);
    const bool needHook = hideMonster ||
                          (stageHide != app::StageHideMode::Off) ||
                          (backdrop != app::BackdropMode::Off);
    hook::SetDynamicHook5Enabled(hook::kDynUnitDraw, needHook);
}

void Shutdown(std::uintptr_t /*exeBase*/) noexcept {
    g_hideMonster.store(false, std::memory_order_relaxed);
    g_stageHide.store(app::StageHideMode::Off, std::memory_order_relaxed);
    g_backdrop.store(app::BackdropMode::Off, std::memory_order_relaxed);
    RestoreSavedCCDimFilterSEH();
    hook::SetDynamicHook5Enabled(hook::kDynUnitDraw, false);
}

} // namespace scene

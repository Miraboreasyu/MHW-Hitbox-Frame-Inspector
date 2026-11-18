#pragma once
// ============================================================================
//  Verified MHW:I 15.23 (Steam build 15539686, ImageBase 0x140000000) RVAs &
//  struct offsets for Pure Debug Hitbox & Freeze Mod (debug_hitbox.dll).
// ============================================================================
#include <cstddef>
#include <cstdint>

namespace mhw {

// ---- Code RVAs ----
// cD3DRender::present(cD3DRender* this, UINT swapInterval, bool param_3) @ 0x1423aa290
// First instruction is an exact 5-byte mov: 48 89 5C 24 18 (mov [rsp+18h], rbx)
constexpr std::uintptr_t kRvaD3DPresent = 0x23AA290;
constexpr std::uint8_t kPresentPrologue[12] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56
};

// cUnit::genericMove(cUnit* this) @ 0x142228f50
// First instruction is an exact 5-byte mov: 48 89 5C 24 08 (mov [rsp+08h], rbx)
constexpr std::uintptr_t kRvaUnitGenericMove = 0x2228F50;
constexpr std::uint8_t kGenericMovePrologue[13] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x57,
    0x48, 0x83, 0xEC, 0x50,
    0x48, 0x8B, 0xF9
};

// cpObjCollision::registerAndContactNodes(cpObjCollision* this) @ 0x1412da520
constexpr std::uintptr_t kRvaCpObjCollRegisterNodes = 0x12DA520;
// cpObjCollision::move(cpObjCollision* this) @ 0x1412dd520
constexpr std::uintptr_t kRvaCpObjCollMove          = 0x12DD520;
// cpObjCollision::cCache::clear(cCache* this) @ 0x1412d7890
constexpr std::uintptr_t kRvaCCacheClear            = 0x12D7890;
// GetCriticalSectionObject() @ 0x142229230 & cUnitHandle::set(LPCRITICAL_SECTION, cUnit*, void*) @ 0x142229160
constexpr std::uintptr_t kRvaGetCritSecObj          = 0x2229230;
constexpr std::uintptr_t kRvaUnitHandleSet          = 0x2229160;

// cpObjCollision hit dispatchers (only hooked while WorldFreeze [K] + KeepHitboxes [J] is active):
//   FUN_1412dd7c0 (normal/evade/counter hit) @ 0x1412dd7c0 (first insn: 5B 48 89 5C 24 10)
//   FUN_1412dd920 (guard/shield block hit)   @ 0x1412dd920 (13B prologue, zero RIP-rel)
constexpr std::uintptr_t kRvaHitDispatch            = 0x12DD7C0;
constexpr std::uint8_t kHitDispatchPrologue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,
    0x48, 0x89, 0x6C, 0x24, 0x18,
    0x56,
    0x48, 0x83, 0xEC, 0x40
};
constexpr std::uintptr_t kRvaGuardDispatch          = 0x12DD920;
constexpr std::uint8_t kGuardDispatchPrologue[13] = {
    0x40, 0x53,
    0x48, 0x83, 0xEC, 0x50,
    0x48, 0x8B, 0x4A, 0x08,
    0x48, 0x8B, 0xDA
};
constexpr std::uintptr_t kRvaHitGroupMatrix         = 0x3249600;
constexpr std::uintptr_t kRvaEvVftDamaged           = 0x3249D08;
constexpr std::uintptr_t kRvaEvVftGuarded           = 0x3249D58;

// cpObjCollision::getDti() @ 0x1412dc880 (vft[4] of cpObjCollision::vft)
constexpr std::uintptr_t kRvaCpObjCollisionGetDti   = 0x12DC880;
// cpObjCollision::DTI::Instance @ 0x144fa30e0
constexpr std::uintptr_t kRvaCpObjCollisionDti      = 0x4FA30E0;
// uAiEm::getParent(uAiEm* this) @ 0x141bad520
constexpr std::uintptr_t kRvaAiEmGetParent          = 0x1BAD520;
// sPlayer::getMainPlayer(sPlayer* this) @ 0x141b42010
constexpr std::uintptr_t kRvaGetMainPlayer          = 0x1B42010;

// ---- Attack & Collision Resource MtDTI RVAs ----
constexpr std::uintptr_t kRvaDtiAttackParamEm      = 0x4C992B0;
constexpr std::uintptr_t kRvaDtiAttackParamOt      = 0x4C992E8;
constexpr std::uintptr_t kRvaDtiAttackParamPl      = 0x4C99320;
constexpr std::uintptr_t kRvaDtiAttackParamPlBase  = 0x4C99358;
constexpr std::uintptr_t kRvaDtiAttackParamPlShell = 0x4C99390;
constexpr std::uintptr_t kRvaDtiCollNodeEm         = 0x4C9A0A0;
constexpr std::uintptr_t kRvaDtiCollNodePl         = 0x4C9A0D8;

// ---- Global Singleton Pointer RVAs ----
constexpr std::uintptr_t kRvaSMhMainInstance      = 0x51C4148; // sMhMain*
constexpr std::uintptr_t kRvaSMhRenderInstance    = 0x51C4480; // sMhRender*
constexpr std::uintptr_t kRvaSMhUnitInstance      = 0x51238C8; // sMhUnit*
constexpr std::uintptr_t kRvaSCollisionInstance   = 0x51C4C50; // sCollision*
constexpr std::uintptr_t kRvaSMouseInstance       = 0x51C5370; // sMouse*
constexpr std::uintptr_t kRvaSKeyboardInstance    = 0x51C53B0; // sKeyboard*
constexpr std::uintptr_t kRvaSMhMouseInstance     = 0x51C53F0; // sMhMouse*
constexpr std::uintptr_t kRvaCD3DRenderInstance   = 0x51C7050; // cD3DRender*
constexpr std::uintptr_t kRvaIsD3D12Byte          = 0x51CC080; // uint8_t: 0 = DX11, !=0 = DX12
constexpr std::uintptr_t kRvaSPlayerInstance      = 0x500ECA0; // sPlayer*
constexpr std::uintptr_t kRvaSEnemyInstance       = 0x500CF40; // sEnemy*

// ---- sMhRender HDR Output Offsets ----
constexpr std::size_t kOffRenderHdrOutput     = 0x21A;
constexpr std::size_t kOffRenderHdrWhiteNits  = 0x21C;

// ---- Camera Drift Suppression Offsets ----
constexpr std::size_t kOffSMouseDiscardDelta  = 0xE0;
constexpr std::size_t kOffSMhMouseDeltaX      = 0xFC;
constexpr std::size_t kOffSMhMouseDeltaY      = 0x100;
constexpr std::size_t kOffSMhMouseAccumX      = 0x1550;
constexpr std::size_t kOffSMhMouseAccumY      = 0x1554;
constexpr std::size_t kOffKbCamYaw            = 0x27B0;
constexpr std::size_t kOffKbCamPitch          = 0x27B4;

// ---- sMain / sMhMain Field Offsets (0x14225f9f0) ----
constexpr std::size_t kOffMainStepCount       = 0x88;     // int32_t: frames to advance while paused
constexpr std::size_t kOffMainSkip            = 0x8C;     // bool (mSkip): active pause state
constexpr std::size_t kOffMainPause           = 0x8D;     // bool (mPause): single-step frame pulse
constexpr std::size_t kOffMainPauseNextFrame  = 0x8E;     // bool (mPauseNextFrame): master pause latch
constexpr std::size_t kOffMhMainUnit          = 0xAF8D0;  // sMhUnit*
constexpr std::size_t kOffMhMainCamera        = 0xAF8D8;  // sMhCamera*
constexpr std::size_t kOffMhMainCollision     = 0xAF8E8;  // sMhCollision*

// ---- cD3DRender Field Offsets ----
constexpr std::size_t kOffD3DDevice12         = 0x18;
constexpr std::size_t kOffD3DCmdQueue12       = 0x20;
constexpr std::size_t kOffD3DRtvDescSize12    = 0x1D8;
constexpr std::size_t kOffD3DBackBuffers12    = 0x240;
constexpr std::size_t kOffD3DRtvHeap12        = 0x258;
constexpr std::size_t kOffD3DRtvStart12       = 0x260;
constexpr std::size_t kOffD3DFullscreenLatch  = 0x26C;
constexpr std::size_t kOffD3DDevice11         = 0x13F8;
constexpr std::size_t kOffD3DContext11        = 0x1400;
constexpr std::size_t kOffD3DSwapHwnd         = 0x1460;
constexpr std::size_t kOffD3DSwapChain12      = 0x1470;
constexpr std::size_t kOffD3DOccludedByte     = 0x1482;
constexpr std::size_t kOffD3DSwapChain11      = 0x1488;
constexpr std::size_t kOffD3DSwapChainCount   = 0x14A0;
constexpr std::size_t kOffD3DPresentLock      = 0x1620;

// ---- sCamera / Viewport / uMhCamera Offsets ----
constexpr std::size_t kCameraViewportStride   = 0x1A0;
constexpr std::size_t kOffVpCameraPtr         = 0x58;
constexpr std::size_t kOffVpVisible           = 0x70;
constexpr std::size_t kOffVpRegion            = 0x78;
constexpr std::size_t kOffVpViewMat           = 0xF0;
constexpr std::size_t kOffVpProjMat           = 0x130;

constexpr std::size_t kOffCamPitch            = 0x998;
constexpr std::size_t kOffCamYaw              = 0x99C;
constexpr std::size_t kOffCamInputPitch       = 0xA78;
constexpr std::size_t kOffCamInputYaw         = 0xA7C;
constexpr std::size_t kOffCamPadPitch         = 0xA90;
constexpr std::size_t kOffCamPadYaw           = 0xA94;
constexpr std::size_t kOffCamInterpPitch      = 0x14D0;
constexpr std::size_t kOffCamInterpYaw        = 0x14D4;
constexpr std::size_t kOffCamTargetPitch      = 0x1530;
constexpr std::size_t kOffCamTargetYaw        = 0x1534;
constexpr std::size_t kOffCamAutoTerrainPitch = 0x15E8;

// ---- sMhUnit / cUnit / cComponent / MtDTI Offsets ----
constexpr std::size_t kMoveLineStride         = 0xF8;
constexpr std::size_t kOffMoveLineFirstUnit   = 0x80;
constexpr std::size_t kOffMoveLineFlags       = 0x98;
constexpr std::size_t kOffMoveLineCount       = 0x3E38;
constexpr std::size_t kOffUnitAliveFlags      = 0x0C;
constexpr std::size_t kOffUnitMoveCount       = 0x2C;
constexpr std::size_t kOffUnitNext            = 0x30;
constexpr std::size_t kOffUnitGroup           = 0x40;
constexpr std::size_t kOffUnitDeltaTime       = 0x68;
constexpr std::size_t kOffUnitDeltaSec        = 0x6C;
constexpr std::size_t kOffUnitCompMgr         = 0x70;
constexpr std::size_t kOffCompMgrHead         = 0x78;
constexpr std::size_t kOffCompNext            = 0x08;
constexpr std::size_t kOffVftGetDti           = 0x20;
constexpr std::size_t kOffDtiName             = 0x08;
constexpr std::size_t kOffDtiParent           = 0x20;
constexpr std::size_t kOffDtiSizeFlags        = 0x30;

// ---- Entity Direct cpObjCollision* & Owner Offsets ----
constexpr std::size_t kOffSPlayerGlobalArr    = 0xAE48;
constexpr std::size_t kOffSPlayerGlobalCnt    = 0xAEC8;
constexpr std::size_t kOffUPlayerColl         = 0x6A8;
constexpr std::size_t kOffSEnemyList          = 0x38;
constexpr std::size_t kOffMonsterCollPrimary  = 0x6A8;
constexpr std::size_t kOffMonsterColl         = 0x7628;
constexpr std::size_t kOffUWpColl             = 0x4B8;
constexpr std::size_t kOffUShellCreatorLegacy = 0x3C0;
constexpr std::size_t kOffUShellColl          = 0x3D8;
constexpr std::size_t kOffUShellCreator       = 0xB40;
constexpr std::size_t kOffUGimmickColl        = 0x868;

// ---- uPlayer Defensive Frame State & Controller Offsets ----
constexpr std::size_t kOffPlMantleCtrl        = 0x14F8;
constexpr std::size_t kOffPlStatusMgr         = 0x7D20;
constexpr std::size_t kOffPlMainFlags         = 0x8938;
constexpr std::size_t kOffPlDefFlags          = 0x8948;
constexpr std::size_t kOffPlGuardFlags        = 0x894C;
constexpr std::size_t kOffPlCounterFlags      = 0x8950;
constexpr std::size_t kOffPlDefFlagsLatch     = 0x8978;
constexpr std::size_t kOffPlGuardFlagsLatch   = 0x897C;
constexpr std::size_t kOffPlCounterFlagsLatch = 0x8980;
constexpr std::size_t kOffPlSuperArmorSec     = 0xE158;
constexpr std::size_t kOffPlHyperArmorSec     = 0xE15C;
constexpr std::size_t kOffPlNoHitSec          = 0xE164;
constexpr std::size_t kOffPlEvadeIframeFrames = 0xE168;
constexpr std::size_t kOffPlMotionCommon      = 0xE470;
constexpr std::size_t kOffPlPadCtrl           = 0x12608;
constexpr std::size_t kOffPlCamInputMask      = 0x1261C;
constexpr std::size_t kOffHmCtrlSub           = 0x1410;
constexpr std::size_t kOffHmCtrlMouseSmoothX  = 0x3198;
constexpr std::size_t kOffHmCtrlMouseSmoothY  = 0x319C;
constexpr std::size_t kOffMantleActiveId      = 0xE04;
constexpr std::size_t kOffMantleTimers        = 0xEFC;

constexpr std::size_t kOffActionCtrlBase      = 0x61C8;
constexpr std::size_t kOffActCtrlCurSet       = 0xAC;

// ---- cpObjCollision / cNode / cGeom Offsets ----
constexpr std::size_t kOffObjCollFlags        = 0x5C;
constexpr std::size_t kOffObjCollNodeCount    = 0x158;
constexpr std::size_t kOffObjCollNodeArray    = 0x168;
constexpr std::size_t kOffObjCollFlags190     = 0x190;
constexpr std::size_t kOffObjCollFlags194     = 0x194;
constexpr std::size_t kOffObjCollStateFlags   = 0x198;
constexpr std::size_t kOffObjCollFlags19C     = 0x19C;
constexpr std::size_t kOffObjCollOwner        = 0x1A8;
constexpr std::size_t kOffObjCollHitGroup     = 0x1F4;
constexpr std::size_t kOffObjCollHitUnits     = 0x760;
constexpr std::size_t kOffObjCollHitUnitCount = 0x828;
constexpr std::size_t kOffObjCollCacheBase    = 0x9F0;
constexpr std::size_t kObjCollCacheStride     = 0x70;
constexpr std::size_t kOffCacheAtkNode        = 0x08;
constexpr std::size_t kOffCacheAtkUnit        = 0x28;
constexpr std::size_t kOffCacheUid            = 0x50;
constexpr std::size_t kOffCacheTimer          = 0x54;
constexpr std::size_t kOffCacheAge            = 0x58;
constexpr std::size_t kOffCacheUseTimer       = 0x6C;
constexpr std::size_t kOffObjCollHitHist      = 0x5400;

constexpr std::size_t kOffHitInfoAtkUnit      = 0x08;
constexpr std::size_t kOffHitInfoDefUnit      = 0x20;
constexpr std::size_t kOffHitInfoAtkNode      = 0x38;
constexpr std::size_t kOffHitInfoAtkParam     = 0xB8;
constexpr std::size_t kOffUnitVftOnEvent      = 0xA0;

constexpr std::size_t kOffCNodeGeomCount      = 0x58;
constexpr std::size_t kOffCNodeGeomArray      = 0x68;
constexpr std::size_t kOffCNodeUid            = 0x70;
constexpr std::size_t kOffCNodeParentColl     = 0x78;
constexpr std::size_t kOffCNodeSCollNode      = 0x80;
constexpr std::size_t kOffCNodeResCollNode    = 0xA0;
constexpr std::size_t kOffCNodeAttackParam    = 0xA8;
constexpr std::size_t kOffCNodeDestroyReq     = 0xB0;
constexpr std::size_t kOffCNodeIsActive       = 0xB1;

constexpr std::size_t kOffCGeomPrimary        = 0x18;
constexpr std::size_t kOffCGeomSwept          = 0x20;
constexpr std::size_t kOffCGeomPartActive     = 0x38;
constexpr std::size_t kOffCGeomSweptState     = 0x3C;

// ---- sCollision / Collider / cCollisionNodeObject Offsets ----
constexpr std::size_t kOffSCollCollider       = 0x84E8;
constexpr std::size_t kOffCollLayerCount      = 0x10;
constexpr std::size_t kOffCollLayerArray      = 0x20;
constexpr std::size_t kOffNodeObjGeomCount    = 0x20;
constexpr std::size_t kOffNodeObjGeomArray    = 0x30;
constexpr std::size_t kOffNodeObjOwner        = 0x80;
constexpr std::size_t kOffNodeObjAttr         = 0x98;
constexpr std::size_t kOffNodeObjUserData     = 0xB8;

// ---- MtGeometry Shape Type IDs (geom + 0x08) ----
constexpr std::size_t kOffGeomType            = 0x08;
constexpr std::int32_t kGeomLineSegment       = 2;
constexpr std::int32_t kGeomSphere            = 5;
constexpr std::int32_t kGeomCapsule           = 6;
constexpr std::int32_t kGeomAABB              = 7;
constexpr std::int32_t kGeomOBB               = 8;
constexpr std::int32_t kGeomCylinder          = 9;
constexpr std::int32_t kGeomTriangle          = 11;
constexpr std::int32_t kGeomLineSweptSphere   = 19;

} // namespace mhw

#pragma once
// ============================================================================
//  Verified MHW:I 15.23 (Steam build 15539686, ImageBase 0x140000000) RVAs &
//  struct offsets extracted directly from mhw_15.23 Ghidra project & mhw.db.
// ============================================================================
#include <cstddef>
#include <cstdint>

namespace mhw {

// ---- Code RVAs ----
// cD3DRender::present(cD3DRender* this, UINT swapInterval, bool param_3) @ 0x1423aa290
constexpr std::uintptr_t kRvaD3DPresent = 0x23AA290;
// Expected first 12 prologue bytes of cD3DRender::present:
//   48 89 5C 24 18   mov [rsp+18h], rbx
//   48 89 7C 24 20   mov [rsp+20h], rdi
//   41 56            push r14
constexpr std::uint8_t kPresentPrologue[12] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56
};

// cUnit::genericMove(cUnit* this) @ 0x142228f50
constexpr std::uintptr_t kRvaUnitGenericMove = 0x2228F50;
// Expected first 13 prologue bytes of cUnit::genericMove (zero RIP-relative ops):
//   48 89 5C 24 08   mov [rsp+08h], rbx
//   57               push rdi
//   48 83 EC 50      sub rsp, 50h
//   48 8B F9         mov rdi, rcx
constexpr std::uint8_t kGenericMovePrologue[13] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x57,
    0x48, 0x83, 0xEC, 0x50,
    0x48, 0x8B, 0xF9
};

// cpObjCollision::registerAndContactNodes(cpObjCollision* this) @ 0x1412da520
// Re-registers existing world-space cNode geometries into sCollision::Collider without advancing timers/sweeps.
constexpr std::uintptr_t kRvaCpObjCollRegisterNodes = 0x12DA520;
// cpObjCollision::move(cpObjCollision* this) @ 0x1412dd520
constexpr std::uintptr_t kRvaCpObjCollMove          = 0x12DD520;
// cpObjCollision::cCache::clear(cCache* this) @ 0x1412d7890
constexpr std::uintptr_t kRvaCCacheClear            = 0x12D7890;
// GetCriticalSectionObject() @ 0x142229230 & cUnitHandle::set(LPCRITICAL_SECTION, cUnit*, void*) @ 0x142229160
constexpr std::uintptr_t kRvaGetCritSecObj          = 0x2229230;
constexpr std::uintptr_t kRvaUnitHandleSet          = 0x2229160;

// cpObjCollision hit dispatchers:
//   FUN_1412dd7c0 (normal/evade/counter hit) @ 0x1412dd7c0 (15-byte prologue, zero RIP-relative ops)
//   FUN_1412dd920 (guard/shield block hit)   @ 0x1412dd920 (13-byte prologue, zero RIP-relative ops)
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
constexpr std::uintptr_t kRvaHitGroupMatrix         = 0x3249600; // uint8_t[8][8] default atk-vs-def group matrix
constexpr std::uintptr_t kRvaEvVftDamaged           = 0x3249D08; // defender hit event vft
constexpr std::uintptr_t kRvaEvVftGuarded           = 0x3249D58; // defender guard event vft

// cpObjCollision::getDti() @ 0x1412dc880 (vft[4] of cpObjCollision::vft)
constexpr std::uintptr_t kRvaCpObjCollisionGetDti = 0x12DC880;
// cpObjCollision::DTI::Instance @ 0x144fa30e0
constexpr std::uintptr_t kRvaCpObjCollisionDti    = 0x4FA30E0;
// uAiEm::getParent(uAiEm* this) @ 0x141bad520
constexpr std::uintptr_t kRvaAiEmGetParent        = 0x1BAD520;
// sPlayer::getMainPlayer(sPlayer* this) @ 0x141b42010
constexpr std::uintptr_t kRvaGetMainPlayer        = 0x1B42010;

// ---- Attack & Collision Resource MtDTI RVAs ----
constexpr std::uintptr_t kRvaDtiAttackParamEm      = 0x4C992B0; // cAttackParamEm::DTI::Instance
constexpr std::uintptr_t kRvaDtiAttackParamOt      = 0x4C992E8; // cAttackParamOt::DTI::Instance
constexpr std::uintptr_t kRvaDtiAttackParamPl      = 0x4C99320; // cAttackParamPl::DTI::Instance
constexpr std::uintptr_t kRvaDtiAttackParamPlBase  = 0x4C99358; // cAttackParamPlBase::DTI::Instance
constexpr std::uintptr_t kRvaDtiAttackParamPlShell = 0x4C99390; // cAttackParamPlShell::DTI::Instance
constexpr std::uintptr_t kRvaDtiCollNodeEm         = 0x4C9A0A0; // cCollNodeEm::DTI::Instance
constexpr std::uintptr_t kRvaDtiCollNodePl         = 0x4C9A0D8; // cCollNodePl::DTI::Instance

// ---- Global Singleton Pointer RVAs ----
constexpr std::uintptr_t kRvaSMhMainInstance      = 0x51C4148; // sMhMain*
constexpr std::uintptr_t kRvaSMhRenderInstance    = 0x51C4480; // sMhRender*
constexpr std::uintptr_t kRvaSMhUnitInstance      = 0x51238C8; // sMhUnit*
constexpr std::uintptr_t kRvaSCollisionInstance   = 0x51C4C50; // sCollision* (sMhCollision*)
constexpr std::uintptr_t kRvaSMouseInstance       = 0x51C5370; // sMouse*
constexpr std::uintptr_t kRvaSKeyboardInstance    = 0x51C53B0; // sKeyboard*
constexpr std::uintptr_t kRvaSMhMouseInstance     = 0x51C53F0; // sMhMouse*
constexpr std::uintptr_t kRvaCD3DRenderInstance   = 0x51C7050; // cD3DRender*
constexpr std::uintptr_t kRvaIsD3D12Byte          = 0x51CC080; // uint8_t: 0 = DX11, !=0 = DX12
constexpr std::uintptr_t kRvaSPlayerInstance      = 0x500ECA0; // sPlayer*
constexpr std::uintptr_t kRvaSEnemyInstance       = 0x500CF40; // sEnemy*

// ---- sMhRender HDR Output Offsets ----
constexpr std::size_t kOffRenderHdrOutput     = 0x21A;    // uint8_t: !=0 when HDR PQ/scRGB output active
constexpr std::size_t kOffRenderHdrWhiteNits  = 0x21C;    // float: game HDR reference white level in nits

// ---- Input / Camera Drift Suppression Offsets ----
constexpr std::size_t kOffSMouseDiscardDelta  = 0xE0;     // uint8_t: 1 = discard next DirectInput mouse delta
constexpr std::size_t kOffSMhMouseDeltaX      = 0xFC;     // int32_t
constexpr std::size_t kOffSMhMouseDeltaY      = 0x100;    // int32_t
constexpr std::size_t kOffSMhMouseAccumX      = 0x1550;   // int32_t
constexpr std::size_t kOffSMhMouseAccumY      = 0x1554;   // int32_t
constexpr std::size_t kOffKbCamYaw            = 0x27B0;   // float: keyboard camera left/right
constexpr std::size_t kOffKbCamPitch          = 0x27B4;   // float: keyboard camera up/down

// ---- sMain / sMhMain Field Offsets ----
constexpr std::size_t kOffMainStepCount       = 0x88;     // int32_t: frames to advance while paused
constexpr std::size_t kOffMainSkip            = 0x8C;     // bool (mSkip): active pause state
constexpr std::size_t kOffMainPause           = 0x8D;     // bool (mPause): single-step frame pulse
constexpr std::size_t kOffMainPauseNextFrame  = 0x8E;     // bool (mPauseNextFrame): master pause latch
constexpr std::size_t kOffMainGlobalSpeed     = 0xA4;     // float (mGlobalSpeed, 1.0f)
constexpr std::size_t kOffMhMainUnit          = 0xAF8D0;  // sMhUnit*
constexpr std::size_t kOffMhMainCamera        = 0xAF8D8;  // sMhCamera* (sCamera*)
constexpr std::size_t kOffMhMainCollision     = 0xAF8E8;  // sMhCollision*

// ---- cD3DRender Field Offsets (initForD3D11 0x1423a3e20 / initForD3D12 0x1423a48e0) ----
constexpr std::size_t kOffD3DDevice12         = 0x18;     // ID3D12Device*
constexpr std::size_t kOffD3DCmdQueue12       = 0x20;     // ID3D12CommandQueue*
constexpr std::size_t kOffD3DRtvDescSize12    = 0x1D8;    // uint32_t
constexpr std::size_t kOffD3DBackBuffers12    = 0x240;    // ID3D12Resource*[3]
constexpr std::size_t kOffD3DRtvHeap12        = 0x258;    // ID3D12DescriptorHeap*
constexpr std::size_t kOffD3DRtvStart12       = 0x260;    // D3D12_CPU_DESCRIPTOR_HANDLE (SIZE_T)
constexpr std::size_t kOffD3DFullscreenLatch  = 0x26C;    // uint8_t
constexpr std::size_t kOffD3DDevice11         = 0x13F8;   // ID3D11Device*
constexpr std::size_t kOffD3DContext11        = 0x1400;   // ID3D11DeviceContext*
constexpr std::size_t kOffD3DSwapHwnd         = 0x1460;   // HWND (mpSwapchains[0] window)
constexpr std::size_t kOffD3DSwapChain12      = 0x1470;   // IDXGISwapChain3*
constexpr std::size_t kOffD3DOccludedByte     = 0x1482;   // bool
constexpr std::size_t kOffD3DSwapChain11      = 0x1488;   // IDXGISwapChain*
constexpr std::size_t kOffD3DSwapChainCount   = 0x14A0;   // uint32_t
constexpr std::size_t kOffD3DPresentLock      = 0x1620;   // CRITICAL_SECTION

// ---- sCamera / Viewport / uMhCamera Offsets (0x14228eb60, 0x141fa11a0, 0x141fa34e0) ----
constexpr std::size_t kCameraViewportStride   = 0x1A0;    // 8 viewports: camSys + idx * 0x1A0
constexpr std::size_t kOffVpCameraPtr         = 0x58;     // uCamera* (uMhCamera*)
constexpr std::size_t kOffVpVisible           = 0x70;     // bool
constexpr std::size_t kOffVpRegion            = 0x78;     // int32_t[4] (L, T, R, B)
constexpr std::size_t kOffVpViewMat           = 0xF0;     // float[16] (row-vector v * View)
constexpr std::size_t kOffVpProjMat           = 0x130;    // float[16] (row-vector v * Proj)

constexpr std::size_t kOffCamPitch            = 0x998;    // float mPitch
constexpr std::size_t kOffCamYaw              = 0x99C;    // float mYaw
constexpr std::size_t kOffCamQuat             = 0x9A0;    // float mQuat[4]
constexpr std::size_t kOffCamInputPitch       = 0xA78;    // float per-frame delta pitch
constexpr std::size_t kOffCamInputYaw         = 0xA7C;    // float per-frame delta yaw
constexpr std::size_t kOffCamPadPitch         = 0xA90;    // float stick delta pitch
constexpr std::size_t kOffCamPadYaw           = 0xA94;    // float stick delta yaw
constexpr std::size_t kOffCamInterpPitch      = 0x14D0;   // float mInterpPitch
constexpr std::size_t kOffCamInterpYaw        = 0x14D4;   // float mInterpYaw
constexpr std::size_t kOffCamTargetPitch      = 0x1530;   // float mTargetPitch
constexpr std::size_t kOffCamTargetYaw        = 0x1534;   // float mTargetYaw
constexpr std::size_t kOffCamAutoTerrainPitch = 0x15E8;   // uint8_t bool auto terrain elevation

// ---- sMhUnit / cUnit / cComponent / MtDTI Offsets ----
constexpr std::size_t kMoveLineStride         = 0xF8;
constexpr std::size_t kOffMoveLineFirstUnit   = 0x80;     // sMhUnit + line * 0xF8 + 0x80 -> cUnit*
constexpr std::size_t kOffMoveLineFlags       = 0x98;     // uint32_t (bit 3 = mPause)
constexpr std::size_t kOffMoveLineCount       = 0x3E38;   // uint32_t (up to 64)
constexpr std::size_t kOffUnitAliveFlags      = 0x0C;     // uint8_t: (& 0x0E) != 0 => active
constexpr std::size_t kOffUnitMoveCount       = 0x2C;     // uint32_t: incremented at end of genericMove
constexpr std::size_t kOffUnitNext            = 0x30;     // cUnit* mpNextUnit
constexpr std::size_t kOffUnitGroup           = 0x40;     // uint64_t mUnitGroup (0x200000000000 = Player)
constexpr std::size_t kOffUnitDeltaTime       = 0x68;     // float mDeltaTime (1.0f at 60fps)
constexpr std::size_t kOffUnitDeltaSec        = 0x6C;     // float mDeltaSec  (1/60s at 60fps)
constexpr std::size_t kOffUnitCompMgr         = 0x70;     // cComponentManager
constexpr std::size_t kOffCompMgrHead         = 0x78;     // cpComponent* head of unit's component list
constexpr std::size_t kOffCompNext            = 0x08;     // cpComponent* mpNext in unit's component list
constexpr std::size_t kOffVftGetDti           = 0x20;     // vft[4] -> MtDTI* getDti()
constexpr std::size_t kOffDtiName             = 0x08;     // const char*
constexpr std::size_t kOffDtiParent           = 0x20;     // MtDTI*
constexpr std::size_t kOffDtiSizeFlags        = 0x30;     // ((u32 & 0x7FFFFF) * 4) = byte size

// ---- Entity Direct cpObjCollision* & Owner Offsets ----
constexpr std::size_t kOffSPlayerGlobalArr    = 0xAE48;   // uPlayer*[4]
constexpr std::size_t kOffSPlayerGlobalCnt    = 0xAEC8;   // uint32_t
constexpr std::size_t kOffUPlayerColl         = 0x6A8;    // cpObjCollision* (inherited from uCharacterModel)
constexpr std::size_t kOffSEnemyList          = 0x38;     // uAiEm*[128]
constexpr std::size_t kOffMonsterCollPrimary  = 0x6A8;    // cpObjCollision* (primary body hurtboxes!)
constexpr std::size_t kOffMonsterColl         = 0x7628;   // cpObjCollision* (secondary monster collision)
constexpr std::size_t kOffUWpColl             = 0x4B8;    // cpObjCollision*
constexpr std::size_t kOffUShellCreatorLegacy = 0x3C0;    // cUnit*
constexpr std::size_t kOffUShellColl          = 0x3D8;    // cpObjCollision*
constexpr std::size_t kOffUShellCreator       = 0xB40;    // cUnit* mpCreatorUnit (verified in 0x141217ac0)
constexpr std::size_t kOffUGimmickColl        = 0x868;    // cpObjCollision*

// ---- uPlayer Defensive Frame State & Controller Offsets (0x141f6bd80, 0x141217e50) ----
constexpr std::size_t kOffPlMantleCtrl        = 0x14F8;   // cMantleController* (also at +0x12608)
constexpr std::size_t kOffPlStatusMgr         = 0x7D20;   // cHumStatusManager*
constexpr std::size_t kOffPlMainFlags         = 0x8938;   // uint8_t bit 0 = isMainPlayer
constexpr std::size_t kOffPlDefFlags          = 0x8948;   // uint32_t: bit22=Super, bits23|24=Hyper, bits27|28=NoHit/Invuln
constexpr std::size_t kOffPlGuardFlags        = 0x894C;   // uint32_t: bit2=DisableGuard, bit3=Guard, bit4=GuardPoint
constexpr std::size_t kOffPlCounterFlags      = 0x8950;   // uint32_t: bit31=Counter / Auto-Evade trigger
constexpr std::size_t kOffPlDefFlagsLatch     = 0x8978;   // uint32_t: latched defense flags across frame
constexpr std::size_t kOffPlGuardFlagsLatch   = 0x897C;   // uint32_t: latched guard flags
constexpr std::size_t kOffPlCounterFlagsLatch = 0x8980;   // uint32_t: latched counter flags
constexpr std::size_t kOffPlSuperArmorSec     = 0xE158;   // float: remaining motion Super Armor seconds
constexpr std::size_t kOffPlHyperArmorSec     = 0xE15C;   // float: remaining motion Hyper Armor seconds
constexpr std::size_t kOffPlNoHitSec          = 0xE164;   // float: remaining motion NoHit seconds
constexpr std::size_t kOffPlEvadeIframeFrames = 0xE168;   // float: remaining Evade i-frame count (60fps frames)
constexpr std::size_t kOffPlMotionCommon      = 0xE470;   // nTimelineParam::PlMotionCommon* (+0x08 bit26 = NoHit)
constexpr std::size_t kOffPlPadCtrl           = 0x12608;  // player controller wrapper (+0x1410 = cHumanController)
constexpr std::size_t kOffPlCamInputMask      = 0x1261C;  // uint32_t: bit 0x100 blocks camera input delta
constexpr std::size_t kOffHmCtrlSub           = 0x1410;   // cHumanController offset inside padCtrl
constexpr std::size_t kOffHmCtrlMouseSmoothX  = 0x3198;   // float smoothed mouse yaw
constexpr std::size_t kOffHmCtrlMouseSmoothY  = 0x319C;   // float smoothed mouse pitch
constexpr std::size_t kOffMantleActiveId      = 0xE04;    // int32_t active mantle ID (3 = Rocksteady)
constexpr std::size_t kOffMantleTimers        = 0xEFC;    // float[32] remaining mantle timer seconds

// ---- cpObjCollision / cNode / cGeom Offsets (0x1412da520, 0x1412dd520, 0x1412d7890) ----
constexpr std::size_t kOffObjCollFlags        = 0x5C;     // uint8_t: (& 1) != 0 => active
constexpr std::size_t kOffObjCollNodeCount    = 0x158;    // uint32_t (MtArray mNodeArray.mCount)
constexpr std::size_t kOffObjCollNodeArray    = 0x168;    // cpObjCollision::cNode**
constexpr std::size_t kOffObjCollFlags190     = 0x190;    // uint32_t preserved across frozen move()
constexpr std::size_t kOffObjCollFlags194     = 0x194;    // uint32_t preserved across frozen move()
constexpr std::size_t kOffObjCollStateFlags   = 0x198;    // uint32_t: bit 1 (0x2) = NoHit invulnerability
constexpr std::size_t kOffObjCollFlags19C     = 0x19C;    // uint32_t preserved across frozen move()
constexpr std::size_t kOffObjCollOwner        = 0x1A8;    // cUnit* mpOwner
constexpr std::size_t kOffObjCollHitGroup     = 0x1F4;    // int32_t mHitGroup (1=Player, 2/3/5=Monster)
constexpr std::size_t kOffObjCollHitUnits     = 0x760;    // already-hit target cUnitHandle[8] (+0x08 + i*0x18)
constexpr std::size_t kOffObjCollHitUnitCount = 0x828;    // uint64_t count of entries in +0x760
constexpr std::size_t kOffObjCollCacheBase    = 0x9F0;    // cpObjCollision::cCache[32] (32 * 0x70 bytes)
constexpr std::size_t kObjCollCacheStride     = 0x70;
constexpr std::size_t kOffCacheAtkNode        = 0x08;     // cNode* attacker node
constexpr std::size_t kOffCacheAtkUnit        = 0x28;     // cUnit* mpAtkModel (cUnitHandle)
constexpr std::size_t kOffCacheUid            = 0x50;     // int32_t mUID (unique attack ID)
constexpr std::size_t kOffCacheTimer          = 0x54;     // float mTimer (multi-hit cooldown)
constexpr std::size_t kOffCacheAge            = 0x58;     // float mAge (elapsed frames since hit; must >0 for eviction!)
constexpr std::size_t kOffCacheUseTimer       = 0x6C;     // uint8_t mIsUseTimer
constexpr std::size_t kOffObjCollHitHist      = 0x5400;   // 16 * 0x20: [cNode* +0x00, cUnit* +0x08, int32 hitCnt +0x10]

constexpr std::size_t kOffHitInfoAtkUnit      = 0x08;     // cUnit* attacker owner in cHitInfo
constexpr std::size_t kOffHitInfoDefUnit      = 0x20;     // cUnit* defender owner in cHitInfo
constexpr std::size_t kOffHitInfoAtkNode      = 0x38;     // cNode* attacker node in cHitInfo
constexpr std::size_t kOffHitInfoAtkParam     = 0xB8;     // cAttackParam* in cHitInfo
constexpr std::size_t kOffUnitVftOnEvent      = 0xA0;     // vft[20] -> void onEvent(cUnit*, void* ev)

constexpr std::size_t kOffCNodeGeomCount      = 0x58;     // uint32_t (MtArray mGeomArray.mCount)
constexpr std::size_t kOffCNodeGeomArray      = 0x68;     // cpObjCollision::cGeom**
constexpr std::size_t kOffCNodeUid            = 0x70;     // int32_t mUniqueAttackId
constexpr std::size_t kOffCNodeParentColl     = 0x78;     // cpObjCollision*
constexpr std::size_t kOffCNodeSCollNode      = 0x80;     // sCollision::Node* (cCollisionNodeObject*)
constexpr std::size_t kOffCNodeResCollNode    = 0xA0;     // rCollNode* / cCollNode*
constexpr std::size_t kOffCNodeAttackParam    = 0xA8;     // cAttackParam* (!=0 => Attack Hitbox!)
constexpr std::size_t kOffCNodeDestroyReq     = 0xB0;     // uint8_t (==0 => valid)
constexpr std::size_t kOffCNodeIsActive       = 0xB1;     // uint8_t (!=0 => active this frame!)

constexpr std::size_t kOffCGeomPrimary        = 0x18;     // MtGeometry*
constexpr std::size_t kOffCGeomSwept          = 0x20;     // MtGeometry* (swept capsule when +0x3C != 0)
constexpr std::size_t kOffCGeomPartActive     = 0x38;     // uint8_t (!=0 => part enabled)
constexpr std::size_t kOffCGeomSweptState     = 0x3C;     // int32_t (!=0 => swept capsule active)

// ---- sCollision / Collider / cCollisionNodeObject Offsets ----
constexpr std::size_t kOffSCollCollider       = 0x84E8;   // sCollision::Collider
constexpr std::size_t kOffCollLayerCount      = 0x10;     // uint32_t
constexpr std::size_t kOffCollLayerArray      = 0x20;     // MtArray**
constexpr std::size_t kOffNodeObjGeomCount    = 0x20;     // uint32_t
constexpr std::size_t kOffNodeObjGeomArray    = 0x30;     // nCollision::cGeometry**
constexpr std::size_t kOffNodeObjOwner        = 0x80;     // MtObject*
constexpr std::size_t kOffNodeObjAttr         = 0x98;     // uint32_t mAttribute
constexpr std::size_t kOffNodeObjUserData     = 0xB8;     // cpObjCollision::cNode*

// ---- MtGeometry Shape Type IDs (geom + 0x08) ----
constexpr std::size_t kOffGeomType            = 0x08;     // int32_t
constexpr std::int32_t kGeomLineSegment       = 2;        // +0x20 p0[3], +0x30 p1[3]
constexpr std::int32_t kGeomSphere            = 5;        // +0x20 center[3], +0x2C radius
constexpr std::int32_t kGeomCapsule           = 6;        // +0x20 p0[3], +0x30 p1[3], +0x40 radius
constexpr std::int32_t kGeomAABB              = 7;        // +0x20 min[3], +0x30 max[3]
constexpr std::int32_t kGeomOBB               = 8;        // +0x20 axX[4], +0x30 axY[4], +0x40 axZ[4], +0x50 center[4], +0x60 halfExt[3]
constexpr std::int32_t kGeomCylinder          = 9;        // +0x20 p0[3], +0x30 p1[3], +0x40 radius
constexpr std::int32_t kGeomTriangle          = 11;       // +0x20 v0[3], +0x30 v1[3], +0x40 v2[3]
constexpr std::int32_t kGeomLineSweptSphere   = 19;       // +0x20 p0[3], +0x30 p1[3], +0x40 radius

} // namespace mhw

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

// sUnit::draw(sMhUnit* this, cDraw* draw) @ 0x14221a050 (vft[16] of sUnit/sMhUnit)
// First instruction is an exact 5-byte mov: 48 89 5C 24 18 (mov [rsp+18h], rbx)
constexpr std::uintptr_t kRvaSUnitDraw = 0x221A050;
constexpr std::uint8_t kSUnitDrawPrologue[12] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57
};

// sMhKeyboard::updateState(sMhKeyboard* this, uint32_t* keyBits) @ 0x1422e7ad0 (16B-aligned)
// First instruction is an exact 5-byte mov: 48 89 5C 24 18 (mov [rsp+18h], rbx)
constexpr std::uintptr_t kRvaSMhKeyboardUpdateState = 0x22E7AD0;
constexpr std::uint8_t kKbUpdateStatePrologue[12] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57
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
constexpr std::uintptr_t kRvaSCameraInstance      = 0x51C4400; // sCamera* (0x14229a1e0 / 0x14228e770)
constexpr std::uintptr_t kRvaSMhRenderInstance    = 0x51C4480; // sMhRender*
constexpr std::uintptr_t kRvaSMhUnitInstance      = 0x51238C8; // sMhUnit*
constexpr std::uintptr_t kRvaSCollisionInstance   = 0x51C4C50; // sCollision*
constexpr std::uintptr_t kRvaSInstancingInstance  = 0x51C5150; // sInstancing*
constexpr std::uintptr_t kRvaSMouseInstance       = 0x51C5370; // sMouse*
constexpr std::uintptr_t kRvaSKeyboardInstance    = 0x51C53B0; // sKeyboard*
constexpr std::uintptr_t kRvaSMhMouseInstance     = 0x51C53F0; // sMhMouse*
constexpr std::uintptr_t kRvaSInputTextInstance   = 0x51C7010; // sInputText* (chat / naming / IME text input)
constexpr std::uintptr_t kRvaCD3DRenderInstance   = 0x51C7050; // cD3DRender*
constexpr std::uintptr_t kRvaIsD3D12Byte          = 0x51CC080; // uint8_t: 0 = DX11, !=0 = DX12
constexpr std::uintptr_t kRvaSMhInputTextInstance = 0x4CB8498; // sMhInputText*
constexpr std::uintptr_t kRvaSPlayerInstance      = 0x500ECA0; // sPlayer*
constexpr std::uintptr_t kRvaSEnemyInstance       = 0x500CF40; // sEnemy*

// ---- sMhInputText / cInputTextEditor Offsets (0x1404810f0, 0x140482e00/f80, 0x1423af770) ----
// NOTE: editor+0x188 == 1 is STANDBY (chat summonable but CLOSED, with sMhInputText+0x1DA00 == 1)!
// Text input is ONLY open & focused when *(int32_t*)(editor + 0x188) == 2 && *(uint8_t*)(mhInputText + 0x1DA00) == 0!
constexpr std::size_t kOffInputTextEditor       = 0x1D9E8; // cInputTextEditor*
constexpr std::size_t kOffMhInputTextUnfocused  = 0x1DA00; // uint8_t: 0 = focused (0x140482e00), !=0 = unfocused (0x140482f80)
constexpr std::size_t kOffInputEditorState      = 0x188;   // int32_t: 0 = none, 1 = standby, 2 = active typing

// ---- uColorCorrectFilter Pre-UI 3D Dimming Offsets (0x1424f7c00 / 0x1424f9110) ----
constexpr std::size_t kOffCCFilterMatrix        = 0x470;   // float fMatrix[4][4] (16 floats, identity @ 0x143fde920)
constexpr std::size_t kOffCCFilterBlendRate     = 0x570;   // float fBlendRate (0.0 .. 1.0)

// ---- sMhRender HDR & Scene Target / nDraw::Texture Offsets ----
constexpr std::size_t kOffRenderD3DRender       = 0x78;    // cD3DRender* on sMhRender (0x1423c3790)
constexpr std::size_t kOffRenderDx12UpscaleFlag = 0xA6;    // uint8_t upscale enable byte on sMhRender (0x1423c3790)
constexpr std::size_t kOffRenderUpscaleBits     = 0xB4;    // uint8_t (& 2 = upscale active) on sMhRender
constexpr std::size_t kOffRenderFxaaEnabled     = 0xBC;    // uint8_t FXAA enable on sMhRender (0x1423c3790)
constexpr std::size_t kOffRenderUpscaleRatio    = 0x204;   // float upscale factor on sMhRender (!= 1.0f when upscaling)
constexpr std::size_t kOffRenderHdrOutput       = 0x21A;
constexpr std::size_t kOffRenderHdrWhiteNits    = 0x21C;
constexpr std::size_t kOffRenderSceneMain       = 0x1F450; // cRenderScene* (main 3D + 2D uGUI scene on sMhRender)
constexpr std::size_t kOffD3DUpscaleCtx         = 0x42700; // void* DLSS/CAS context on cD3DRender (0x1423c3790)
constexpr std::size_t kOffD3DUpscaleActive      = 0x42718; // uint8_t DLSS/CAS active on cD3DRender (0x1423c3790)

constexpr std::size_t kOffSceneFlags            = 0x58;    // uint32_t flags on cRenderScene (0x800000 = TAA TemporalUpsample)
constexpr std::size_t kOffScenePostTarget       = 0x420;   // nDraw::RenderTargetView* RTPostTarget (pre-UI 3D tonemapped RTV)
constexpr std::size_t kOffSceneOutputTarget     = 0x460;   // nDraw::RenderTargetView* RTScreen (DrawPass 0x10 RTV)
constexpr std::size_t kOffSceneFilterTarget     = 0x4A0;   // nDraw::RenderTargetView* RTScreen (DrawPass 0x11 / BackBuffer src RTV)
constexpr std::size_t kOffSceneScreenTex        = 0x5F8;   // nDraw::Texture* Screen copy on cRenderScene
constexpr std::size_t kOffSceneTargets          = 0x628;   // cRenderSceneTargets* on cRenderScene (0x14269d8f0)
constexpr std::size_t kOffSceneUpscaleTex       = 0x6A0;   // nDraw::Texture* UpscaleOut / FXAA output (0x1423c2620 / 0x1423c3790)
constexpr std::size_t kOffSceneAaEnabled        = 0x6A8;   // uint8_t scene AA/upscale active on cRenderScene
constexpr std::size_t kOffSceneDynamicScale     = 0x6B0;   // float dynamic resolution fraction (0..1] on cRenderScene
constexpr std::size_t kOffSceneRenderScale      = 0x6B8;   // float total render scale on cRenderScene

constexpr std::size_t kOffRenderTargetTex       = 0x58;    // nDraw::Texture* on nDraw::RenderTargetView
constexpr std::size_t kOffTargetsScreenTex      = 0x68;    // nDraw::Texture* RTScreen on cRenderSceneTargets (0x14269d8f0)
constexpr std::size_t kOffTargetsPostTex        = 0x78;    // nDraw::Texture* PostTarget on cRenderSceneTargets
constexpr std::size_t kOffTargetsRtDepth        = 0x90;    // nDraw::Texture* RTDepth (with SRV flag 0x100000) on cRenderSceneTargets
constexpr std::size_t kOffTargetsDepthCopy      = 0xA0;    // nDraw::Texture* tDepthMap #1 (R32_FLOAT copy of 3D DSV, 0x14255ce10)
constexpr std::size_t kOffTargetsDepthCopy2     = 0xA8;    // nDraw::Texture* tDepthMap #2 (R32_FLOAT copy of 3D DSV, 0x14255ce20)
constexpr std::size_t kOffTargetsTaaOut0        = 0x138;   // nDraw::Texture* TAA rwOutput[0] on cRenderSceneTargets (0x14269d8f0)
constexpr std::size_t kOffTargetsTaaOut1        = 0x140;   // nDraw::Texture* TAA rwOutput[1] on cRenderSceneTargets (0x14269d8f0)
constexpr std::size_t kOffTexMode               = 0x58;    // int32_t on nDraw::Texture (6 = alt wrapper at +0xF0)
constexpr std::size_t kOffTexWrap               = 0x60;    // low-level D3D11/D3D12 texture wrapper*
constexpr std::size_t kOffTexSrvWrap            = 0x78;    // low-level D3D11/D3D12 SRV wrapper*
constexpr std::size_t kOffTexPackedDim          = 0x88;    // uint32_t: width = & 0xFFFF, height = (>> 16) & 0x3FFF
constexpr std::size_t kOffTexWrapAlt            = 0xF0;    // alt low-level texture wrapper* when mode == 6
constexpr std::size_t kOffWrapRes11             = 0x18;    // ID3D11Resource* inside D3D11 texture wrapper
constexpr std::size_t kOffWrapRes12             = 0x20;    // ID3D12Resource* inside D3D12 texture wrapper
constexpr std::size_t kOffWrapState12           = 0x70;    // D3D12 resource state tracker* inside D3D12 wrapper
constexpr std::size_t kOffState12Bits           = 0x1A;    // uint16_t encoded D3D12_RESOURCE_STATES
constexpr std::size_t kOffSrvWrapHandle         = 0x10;    // ID3D11ShaderResourceView* (DX11) / CPU handle (DX12)

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
constexpr std::size_t kOffVpSubScenePtr       = 0x68;
constexpr std::size_t kOffVpVisible           = 0x70;
constexpr std::size_t kOffVpDisplay           = 0x75; // uint8_t display index (0 = primary, 0x14228e770)
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

// ---- sMhUnit / cUnit / cComponent / MtDTI / sInstancing Offsets ----
constexpr std::size_t kMoveLineStride          = 0xF8;
constexpr std::size_t kOffMoveLineFirstUnit    = 0x80;
constexpr std::size_t kOffMoveLineFlags        = 0x98;
constexpr std::size_t kOffMoveLineCount        = 0x3E38;
constexpr std::size_t kOffUnitSysDrawUnits     = 0x3DDE70; // cUnit** (capacity 10000)
constexpr std::size_t kOffUnitSysDrawJobs0     = 0x3DDE80; // Job* (stride 0x20, count at +0x3DDEB0)
constexpr std::size_t kOffUnitSysDrawJobs1     = 0x3DDE90; // Job* (stride 0x20, count at +0x3DDEB4)
constexpr std::size_t kOffUnitSysDrawJobs2     = 0x3DDEA0; // Job* (stride 0x20, count at +0x3DDEB8)
constexpr std::size_t kOffUnitSysDrawCnt0      = 0x3DDEB0; // uint32_t
constexpr std::size_t kOffUnitSysDrawCnt1      = 0x3DDEB4; // uint32_t
constexpr std::size_t kOffUnitSysDrawCnt2      = 0x3DDEB8; // uint32_t
constexpr std::size_t kOffUnitSysUseDrawList   = 0x3DDEBC; // bool mUseDrawList
constexpr std::size_t kOffSInstDispBake        = 0x340;    // bool mbDispBakeInstance
constexpr std::size_t kOffCDrawPassMask        = 0x152;    // uint16_t render pass bitmask (1/2 = main cam)
constexpr std::size_t kOffCDrawViewIdx         = 0x15A;    // uint16_t viewport index (0..15)
constexpr std::size_t kOffUnitAliveFlags       = 0x0C;     // int32_t (8 = active in MoveLine)
constexpr std::size_t kOffUnitDrawFlags        = 0x14;     // uint32_t ((flags & 0x12) == 0x12 -> drawable)
constexpr std::size_t kOffUnitViewportMask     = 0x1C;     // uint32_t bitmask of viewports
constexpr std::size_t kOffUnitTakeOverMode     = 0x20;     // int32_t (1 = batched via *InstanceTakeOver)
constexpr std::size_t kOffUnitMoveCount        = 0x2C;
constexpr std::size_t kOffUnitNext             = 0x30;
constexpr std::size_t kOffUnitGroup            = 0x40;     // uint64_t mUnitGroup bitmask
constexpr std::size_t kOffUnitPassMask         = 0x48;     // uint32_t mDrawPass bitmask
constexpr std::size_t kOffUnitLastDrawnMask    = 0x50;     // uint32_t mLastDrawnPassMask (checked by uGimmick::move!)
constexpr std::size_t kOffUnitCurDrawnMask     = 0x54;     // uint32_t mCurFrameDrawnPassMask (stamped by 0x1422290f0)
constexpr std::uint64_t kUnitGroupPlSubModelBit = 0x0000000000020000ULL; // Group[17]: player slinger/mantle child models (0x141fe48c0)
constexpr std::uint64_t kUnitGroupGmSubModelBit = 0x0000000100000000ULL; // Group[32]: gm025/stage gimmick child models (0x1420760c0)
constexpr std::uint64_t kUnitGroupGimmickBit    = 0x0000004000000000ULL; // Group[38]: child uMhModel of uGimmick (0x141ced660)
constexpr std::uint64_t kUnitGroupEmSubModelBit = 0x0000020000000000ULL; // Group[41]: monster held/sub models (0x141c5b010)
constexpr std::uint64_t kUnitGroupPlayerBit     = 0x0000200000000000ULL; // Group[45]: player-related models

// ---- *InstanceTakeOver UnitManager Offsets (0x1424d62c0 / 0x1423e5590 / 0x1426db810) ----
constexpr std::size_t kOffBaseModelTakeOverMgr = 0x460; // uBaseModelInstanceTakeOver + 0x460
constexpr std::size_t kOffSpeedTreeTakeOverMgr = 0x480; // uSpeedTreeInstanceTakeOver + 0x480
constexpr std::size_t kOffModelTakeOverMgr     = 0x540; // uModelInstanceTakeOver + 0x540
constexpr std::size_t kOffTakeOverUnitArray    = 0x08;  // cUnit** inside UnitManager
constexpr std::size_t kOffTakeOverUnitCount    = 0x18;  // uint32_t count inside UnitManager
constexpr std::size_t kOffUnitDeltaTime        = 0x68;
constexpr std::size_t kOffUnitDeltaSec         = 0x6C;
constexpr std::size_t kOffUnitCompMgr          = 0x70;
constexpr std::size_t kOffCompMgrHead          = 0x78;
constexpr std::size_t kOffCompNext             = 0x08;
constexpr std::size_t kOffUCoordPos            = 0x160;    // float mPos[3]
constexpr std::size_t kOffUCoordWorldPos       = 0x200;    // float mWorldMatrix.row3[3]
constexpr std::size_t kOffVftGetDti            = 0x20;
constexpr std::size_t kOffDtiName              = 0x08;
constexpr std::size_t kOffDtiParent            = 0x20;
constexpr std::size_t kOffDtiSizeFlags         = 0x30;

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

// ---- sCollision / cSbcArrayBP / rCollision (.sbc Stage Mesh) Offsets ----
constexpr std::size_t kOffSCollSbcCount        = 0x408;    // uint32_t (cSbcArrayBP @ sColl + 0x400)
constexpr std::size_t kOffSCollSbcArray        = 0x418;    // sCollision::Sbc**
constexpr std::size_t kOffSbcActive            = 0x09;     // uint8_t
constexpr std::size_t kOffSbcHasMatrix         = 0x4B;     // uint8_t
constexpr std::size_t kOffSbcMatrixPtr         = 0x50;     // const float* (4x4 row-major MtMatrix)
constexpr std::size_t kOffSbcResource          = 0x88;     // rCollision*
constexpr std::size_t kOffSbcPartsArray        = 0x90;     // sCollision::Sbc::Parts* (stride 0x78)
constexpr std::size_t kOffSbcPartsCount        = 0x98;     // uint32_t
constexpr std::size_t kSbcPartStride           = 0x78;
constexpr std::size_t kOffSbcPartEnabled       = 0x18;     // uint8_t
constexpr std::size_t kOffSbcPartHasMatrix     = 0x3B;     // uint8_t
constexpr std::size_t kOffSbcPartMatrixPtr     = 0x40;     // const float* (4x4 row-major MtMatrix)

constexpr std::size_t kOffRCollMagic           = 0xB0;     // char[4] "SBC\xFF"
constexpr std::size_t kOffRCollNumParts        = 0xC0;     // uint16_t
constexpr std::size_t kOffRCollNumTriNodes     = 0xC8;     // uint32_t
constexpr std::size_t kOffRCollNumVerts        = 0xCC;     // uint32_t
constexpr std::size_t kOffRCollAabbMin         = 0xE0;     // float[3]
constexpr std::size_t kOffRCollAabbMax         = 0xF0;     // float[3]
constexpr std::size_t kOffRCollPartsInfo       = 0x118;    // PartInfo* (stride 0x60)
constexpr std::size_t kOffRCollTriNodes        = 0x130;    // TriNode* (stride 0x20)
constexpr std::size_t kOffRCollVertices        = 0x138;    // float4* (stride 0x10)
constexpr std::size_t kRCollPartInfoStride     = 0x60;
constexpr std::size_t kOffPartInfoAabbMin      = 0x00;     // float[3]
constexpr std::size_t kOffPartInfoAabbMax      = 0x10;     // float[3]
constexpr std::size_t kOffPartInfoStartTri     = 0x40;     // uint32_t
constexpr std::size_t kOffPartInfoStartVert    = 0x48;     // uint32_t
constexpr std::size_t kRCollTriStride          = 0x20;
constexpr std::size_t kOffTriVertIndices       = 0x0C;     // uint16_t[3]

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

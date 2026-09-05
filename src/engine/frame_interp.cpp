/**
 * @file    engine/frame_interp.cpp
 * @brief   Render interpolation between the 30Hz logic ticks of the fps unlock.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "engine/frame_interp.h"

#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "engine/d2anime/anime_mouse.h"
#include "engine/d2anime/d2anime_task.h"
#include "engine/frame_clock.h"
#include "engine/guest_prim.h"
#include "engine/glyph_set.h"
#include "engine/menus/camp_settings.h"
#include "engine/menus/local_map.h"
#include "engine/mouse_cursor.h"
#include "engine/virtual_buttons.h"
#include "gpu/gpu.h"
#include "gpu/scene/native_transform_bridge.h"
#include "xr/xr_game_camera.h"

namespace {

constexpr u32 kCamViewOffset = 160; // camera view matrix
constexpr u32 kCamEyeOffset = 288;  // camera world position (vec3)
constexpr float kCutDistSq = 4.0f;  // squared eye jump => snap not lerp
// Rotation similarity floor: trace(prevR^T currR)/3 = (1+2cos(theta))/3.
// 0.90 ~= a 32-degree single-tick turn, beyond any authored pan, so cutscene
// shot cuts snap while pans still interpolate.
constexpr float kCutRotDot = 0.90f;

struct CamEntry {
  float prevView[16];
  float currView[16];
  float prevEye[3];
  float currEye[3];
  u64 lastTick = 0;
  u64 lastSeen = 0;
  bool valid = false;
};

std::unordered_map<u32, CamEntry> g_cams; // render-thread only
u64 g_camFrame = 0;
bool g_inCameraRender =
    false; // true only inside bdCameraRenderSetup (render thread)

void ReadFloats(be_f32 *p, float *out, int n) {
  for (int i = 0; i < n; ++i)
    out[i] = p[i];
}
void WriteFloats(be_f32 *p, const float *in, int n) {
  for (int i = 0; i < n; ++i)
    p[i] = in[i];
}
float EyeDistSq(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz;
}
void LerpMatrix(const float a[16], const float b[16], float t, float out[16]) {
  for (int i = 0; i < 16; ++i)
    out[i] = a[i] + (b[i] - a[i]) * t;
}

void PruneCams() {
  for (auto it = g_cams.begin(); it != g_cams.end();) {
    if (g_camFrame - it->second.lastSeen > 4)
      it = g_cams.erase(it);
    else
      ++it;
  }
}

// bdSceneNodeProcessRenderCmds uploads the current bone palette to VS reg 0x3C
// and, 0x600 bytes later in the same stack frame, the previous-frame palette to
// reg 0x9C (motion blur). Max 24 bones x 64B. Object world (reg 0x14) and its
// previous (reg 0x2C) are one 4x4 matrix each.
constexpr u32 kPalettePrevDelta = 0x600;
constexpr int kPaletteFloats = 24 * 16;
constexpr int kWorldFloats = 16;
u32 g_paletteScratch = 0; // guest scratch for the interpolated palette
u32 g_worldScratch = 0;   // guest scratch for the interpolated world

// Cut/teleport snap thresholds: translation jump over kObjCutDist in one tick,
// a rotation basis row turning past ~75 degrees, or any palette float moving
// more than kPaletteCutDelta, all beyond legitimate per-tick motion.
constexpr float kObjCutDistSq = 4.0f;
constexpr float kObjCutRotDot = 0.25f;
constexpr float kPaletteCutDelta = 1.5f;

bool WorldMatrixDiscontinuous(const be_f32 *cur, const be_f32 *prv) {
  float d2 = 0.0f;
  for (int i = 12; i < 15; ++i) {
    const float d = static_cast<float>(cur[i]) - static_cast<float>(prv[i]);
    d2 += d * d;
  }
  if (d2 > kObjCutDistSq)
    return true;
  for (int r = 0; r < 12; r += 4) {
    float dot = 0.0f, mc = 0.0f, mp = 0.0f;
    for (int i = r; i < r + 3; ++i) {
      const float c = cur[i], p = prv[i];
      dot += c * p;
      mc += c * c;
      mp += p * p;
    }
    const float denom = std::sqrt(mc * mp);
    if (denom > 1e-6f && dot / denom < kObjCutRotDot)
      return true;
  }
  return false;
}

bool PaletteDiscontinuous(const be_f32 *cur, const be_f32 *prv, int floats) {
  for (int i = 0; i < floats; ++i) {
    const float d = static_cast<float>(cur[i]) - static_cast<float>(prv[i]);
    if (d > kPaletteCutDelta || d < -kPaletteCutDelta)
      return true;
  }
  return false;
}

// Lerp 'floats' big-endian floats prev->curr into a lazily allocated guest
// scratch. Returns the scratch VA (0 on failure). Render thread only.
u32 LerpGuestFloats(u32 currVa, u32 prevVa, int floats, u32 &scratch, float a) {
  if (scratch == 0) {
    scratch = bd::gpu::HostHeap::Get().AllocGuest(floats * 4, 16);
    if (scratch == 0)
      return 0;
  }
  auto *cur = bd::mem::at<be_f32>(currVa);
  auto *prv = bd::mem::at<be_f32>(prevVa);
  auto *dst = bd::mem::at<be_f32>(scratch);
  if (!cur || !prv || !dst)
    return 0;
  for (int i = 0; i < floats; ++i) {
    const float p = prv[i], c = cur[i];
    dst[i] = p + (c - p) * a;
  }
  return scratch;
}

} // namespace

// Skip the 30Hz logic block on non-tick frames. r28=0xDEAD0000 is the sentinel
// the skipped lis would load (the render block DEAD root checks read it
// unreloaded).
bool bdLogicTickGateHook(PPCRegister &r28) {
  if (bd::engine::TickDue())
    return false;
  r28.u64 = 0xDEAD0000;
  return true;
}

// The master animation clock ticks once per present, so freeze it on
// interpolated frames. No-op at <=30Hz, where TickDue() is always true.
bool bdFrameClockGateHook() { return !bd::engine::TickDue(); }

// Render-side accumulators step once per rendered frame with no delta time.
// Only the accumulation store is skipped, the uploads and draws after it
// re-read the unchanged values.
bool bdShaderAnimGateHook() { return !bd::engine::TickDue(); }

// The ambient recovery ramp steps a fixed 0.1 per call with no delta time, and
// runs outside the 30Hz block while the logic that rewrites the darkened
// ambient runs only on ticks. Ungated, several steps accumulate between ticks
// and the player ramps bright then snaps back.
bool bdPlayerAmbientRampGateHook() { return !bd::engine::TickDue(); }

// Event camera cuts retire the outgoing shot draw-once-then-hide: the cut tick
// arms a one-shot flag and the next Draw consumes it. The consume runs at tick
// start, ahead of that tick's logic, because a cut window re-arms the hide
// every tick and a later consume kills the fresh re-arm.
constexpr u32 kIssObjectVtableEA = 0x8208AFA4;
constexpr u32 kIssActorVtableEA = 0x8208B334;
constexpr u32 kIssActorHideFnEA = 0x82410A18; // clears issActor +0x14C

namespace {
struct IssObject_t {
  u8 _pad000[0x9E0];
  be_u32 hideConsume; // Draw writes 0 to cancel the armed hide
  be_u32 hideArm;     // cut tick arms the one-shot hide
};
static_assert(offsetof(IssObject_t, hideConsume) == 0x9E0);
static_assert(offsetof(IssObject_t, hideArm) == 0x9E4);
static_assert(sizeof(IssObject_t) == 0x9E8);

// issActor__ApplySpecialModelFlags (kIssActorHideFnEA) is the consume path.
// It clears +0x14C in the guest, which is not modeled here.
struct IssActor_t {
  u8 _pad000[0x150];
  be_u32 hideArm;
};
static_assert(offsetof(IssActor_t, hideArm) == 0x150);
static_assert(sizeof(IssActor_t) == 0x154);
} // namespace

namespace {
std::unordered_set<u32> g_evtHidePending;
} // namespace

bool bdEvtShotHideDeferHook(PPCRegister &r31) {
  if (!bd::engine::InterpolationActive()) {
    g_evtHidePending.clear();
    return false;
  }
  if (g_evtHidePending.size() > 256)
    g_evtHidePending.clear();
  g_evtHidePending.insert(r31.u32);
  return true;
}

namespace {
void FlushEvtHidePending() {
  if (g_evtHidePending.empty())
    return;
  auto *dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  for (const u32 obj : g_evtHidePending) {
    const u32 vtable = bd::mem::load<u32>(obj);
    if (vtable == kIssObjectVtableEA) {
      if (auto *o = bd::mem::at<IssObject_t>(obj)) {
        if (o->hideArm != 0)
          o->hideConsume = 0;
      }
    } else if (vtable == kIssActorVtableEA) {
      if (auto *a = bd::mem::at<IssActor_t>(obj)) {
        if (a->hideArm != 0) {
          if (auto *fn = dispatcher->GetFunction(kIssActorHideFnEA))
            rex::ppc::GuestToHostFunction<void>(fn, obj);
        }
      }
    }
  }
  g_evtHidePending.clear();
}
} // namespace

// The blink arm flag is set once per logic tick and consumed by the armed
// draw, so interpolated frames find it already consumed. Track liveness here
// and force the armed path while a blink is active.
bool bdCompassBlinkHoldHook(PPCRegister &r11) {
  static bool blinkActive = false;
  if (r11.u32 != 0) {
    blinkActive = true;
    return false;
  }
  if (bd::engine::TickDue()) {
    blinkActive = false;
    return false;
  }
  return blinkActive;
}

// The prim pool is flip-recycled every rendered frame, so a quad pushed from
// the 30Hz logic side exists only on tick frames. Capture the args each tick
// and re-issue from an ungated per-frame hook in the same 2D submission
// window. A capture goes stale the moment a tick passes without vf02 re-issuing
// it, so replay stops with it.

namespace {
struct FrostPrimCapture {
  u64 tick = ~0ull;
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
  u32 color = 0;
  u32 texObjEA = 0;
};
FrostPrimCapture g_frostPrim;
} // namespace

void bdFaceFrostCaptureHook(PPCRegister &f1, PPCRegister &f2, PPCRegister &f4,
                            PPCRegister &f5, PPCRegister &r8,
                            PPCRegister &r30) {
  g_frostPrim.tick = bd::engine::TickCount();
  g_frostPrim.x = f1.f64;
  g_frostPrim.y = f2.f64;
  g_frostPrim.w = f4.f64;
  g_frostPrim.h = f5.f64;
  g_frostPrim.color = r8.u32;
  g_frostPrim.texObjEA = r30.u32;
}

// Two text prims into the same flip-recycled pool, so the same
// capture-and-replay as the frost quad above.
//
// bdPushTextPrim takes five doubles and nine integers: r3-r10 then one slot the
// SDK marshaller places at r1+0x54, read back as the text style word. Only r8
// (string), r9 (color) and r10 carry meaning here.
//
// The replayed label sits at the tick's projected position, so it steps at 30Hz
// while the camera interpolates. Re-projecting would need the world position
// and the text width centering the guest applies after it.
REX_IMPORT(__imp__Visual__method_7E60, ItemDropPushText,
           void(f64, f64, f64, f64, f64, u32, u32, u32, u32, u32, u32, u32, u32,
                u32));

namespace {

constexpr u32 kItemDropTextChars = 96;
constexpr u32 kItemDropTextPrims = 2;

struct ItemDropTextPrim {
  f64 x = 0.0, y = 0.0, z = 0.0, w = 0.0, h = 0.0;
  u32 color = 0;
  u32 mode = 0;
};

struct ItemDropTextCapture {
  u64 tick = ~0ull;
  u32 count = 0;
  u32 textEA = 0;
  ItemDropTextPrim prims[kItemDropTextPrims];
};

ItemDropTextCapture g_itemDropText;

// The guest string lives in the vf13 stack frame, which is gone by replay time.
bool CopyGuestWideString(u32 srcVa, u32 dstVa) {
  auto *src = bd::mem::try_at<const be_u16>(srcVa);
  auto *dst = bd::mem::at<be_u16>(dstVa);
  if (!src || !dst)
    return false;
  for (u32 i = 0; i + 1 < kItemDropTextChars; ++i) {
    const u16 c = src[i];
    dst[i] = c;
    if (c == 0)
      return i > 0;
  }
  dst[kItemDropTextChars - 1] = 0;
  return true;
}

} // namespace

void bdItemDropTextCaptureHook(PPCRegister &f1, PPCRegister &f2,
                               PPCRegister &f3, PPCRegister &f4,
                               PPCRegister &f5, PPCRegister &r8,
                               PPCRegister &r9, PPCRegister &r10) {
  if (!bd::engine::InterpolationActive())
    return;
  auto &cap = g_itemDropText;
  const u64 tick = bd::engine::TickCount();
  if (cap.tick != tick) {
    cap.tick = tick;
    cap.count = 0;
  }
  if (cap.count >= kItemDropTextPrims)
    return;
  if (cap.textEA == 0) {
    cap.textEA = bd::gpu::HostHeap::Get().AllocGuest(
        static_cast<u32>(kItemDropTextChars * sizeof(be_u16)), 4);
    if (cap.textEA == 0)
      return;
  }
  if (cap.count == 0 && !CopyGuestWideString(r8.u32, cap.textEA)) {
    cap.tick = ~0ull;
    return;
  }
  auto &prim = cap.prims[cap.count++];
  prim.x = f1.f64;
  prim.y = f2.f64;
  prim.z = f3.f64;
  prim.w = f4.f64;
  prim.h = f5.f64;
  prim.color = r9.u32;
  prim.mode = r10.u32;
}

// Before bdPrimFlush, so the re-issued prims join this frame's 2D pass ahead
// of the slot flip. The frost quad's own replay site exists only while a
// dialogue portrait window does.
void bdItemDropTextReplayHook() {
  if (!bd::engine::InterpolationActive() || bd::engine::TickDue())
    return;
  if (g_itemDropText.tick != bd::engine::TickCount() ||
      !g_itemDropText.textEA || g_itemDropText.count == 0)
    return;
  for (u32 i = 0; i < g_itemDropText.count; ++i) {
    const auto &prim = g_itemDropText.prims[i];
    PrimSelectTexture(0, 0);
    ItemDropPushText(prim.x, prim.y, prim.z, prim.w, prim.h, 0, 0, 0, 0, 0,
                     g_itemDropText.textEA, prim.color, prim.mode, 0);
  }
}

REX_EXTERN(__imp__FreeDfsTask__vf03);
REX_HOOK_RAW(FreeDfsTask__vf03) {
  __imp__FreeDfsTask__vf03(ctx, base);
  if (!bd::engine::InterpolationActive() || bd::engine::TickDue())
    return;
  if (g_frostPrim.tick != bd::engine::TickCount() || !g_frostPrim.texObjEA)
    return;
  PrimSelectTexture(0, g_frostPrim.texObjEA);
  PrimDrawRect2D(g_frostPrim.x, g_frostPrim.y, 1.0, g_frostPrim.w,
                      g_frostPrim.h, 0, 0, 0, 0, 0, g_frostPrim.color);
}

// A changed light already in an object's active set is what forces the
// re-score that drops a light since disabled or moved out of range, and the
// changed list is empty on interpolated frames. Hold it across a tick and
// clear it here at tick start, before the guest repopulates it.
namespace {

constexpr u32 kLightEntriesEA = 0x82E18694;      // light manager + 8
constexpr u32 kLightChangedListEA = 0x82E1DFA8;  // entries + 0x5914
constexpr u32 kLightChangedCountEA = 0x82E1E458; // entries + 0x5DC4
constexpr u32 kLightChangedFlag = 0x40;          // entry flags bit 6
constexpr u32 kLightMaxEntries = 300;
constexpr u32 kLightEntryStride = 0x4C;

void SetChangedFlags(u32 count, bool set) {
  auto *list = bd::mem::at<be_u32>(kLightChangedListEA);
  if (!list)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 entry = static_cast<u32>(list[i]);
    if (entry < kLightEntriesEA ||
        entry >= kLightEntriesEA + kLightMaxEntries * kLightEntryStride) {
      continue;
    }
    if (auto *flags = bd::mem::at<be_u32>(entry)) {
      const u32 v = *flags;
      *flags = set ? (v | kLightChangedFlag) : (v & ~kLightChangedFlag);
    }
  }
}

void ClearLightChangedList() {
  u32 count = bd::mem::load<u32>(kLightChangedCountEA);
  if (count > kLightMaxEntries)
    count = kLightMaxEntries;
  SetChangedFlags(count, false);
  bd::mem::store<u32>(kLightChangedCountEA, 0u);
}

} // namespace

REX_EXTERN(__imp__bdLightListUpdateSnapshot);
REX_HOOK_RAW(bdLightListUpdateSnapshot) {
  u32 held = bd::engine::InterpolationActive()
                 ? bd::mem::load<u32>(kLightChangedCountEA)
                 : 0;
  if (held > kLightMaxEntries)
    held = 0;

  __imp__bdLightListUpdateSnapshot(ctx, base);

  if (held == 0)
    return;
  SetChangedFlags(held, true);
  bd::mem::store<u32>(kLightChangedCountEA, held);
}

namespace bd::engine {

void OnGuestGameStep() {
  Advance();
  if (InterpolationActive() && TickDue()) {
    FlushEvtHidePending();
    ClearLightChangedList();
  }
}

} // namespace bd::engine

// Never write camera+160: the follow camera controller reads it in the
// concurrent logic phase and would feed back.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__bdCameraRenderSetup);
REX_HOOK_RAW(bdCameraRenderSetup) {
  const u32 cam = ctx.r3.u32;
  if (!bd::engine::InterpolationActive()) {
    __imp__bdCameraRenderSetup(ctx, base);
    return;
  }

  ++g_camFrame;
  PruneCams();

  float liveView[16], liveEye[3];
  ReadFloats(bd::mem::at<be_f32>(cam + kCamViewOffset), liveView, 16);
  ReadFloats(bd::mem::at<be_f32>(cam + kCamEyeOffset), liveEye, 3);

  CamEntry &e = g_cams[cam];
  e.lastSeen = g_camFrame;

  const u64 tick = bd::engine::TickCount();
  if (tick != e.lastTick) {
    if (e.valid) {
      for (int i = 0; i < 16; ++i)
        e.prevView[i] = e.currView[i];
      for (int i = 0; i < 3; ++i)
        e.prevEye[i] = e.currEye[i];
    } else { // first observation: prev = curr (no lerp yet)
      for (int i = 0; i < 16; ++i)
        e.prevView[i] = liveView[i];
      for (int i = 0; i < 3; ++i)
        e.prevEye[i] = liveEye[i];
    }
    for (int i = 0; i < 16; ++i)
      e.currView[i] = liveView[i];
    for (int i = 0; i < 3; ++i)
      e.currEye[i] = liveEye[i];
    e.lastTick = tick;
    e.valid = true;
  }

  g_inCameraRender = true;
  ctx.r3.u32 = cam;
  __imp__bdCameraRenderSetup(ctx, base);
  g_inCameraRender = false;
}

// Compose the tracked camera/XR view in native memory and feed the native
// transform producer. Its temporary compatibility path preserves the inherited
// PPC context if an unsupported engine callback still needs execution.
REX_HOOK_RAW(bdBuildViewMatrix) {
  float view[16];
  bool replaced = false;

  if (g_inCameraRender) {
    const u32 viewVa = ctx.r4.u32;
    if (viewVa > kCamViewOffset) {
      auto it = g_cams.find(viewVa - kCamViewOffset);
      if (it != g_cams.end() && it->second.valid) {
        const CamEntry &e = it->second;
        bool cut = EyeDistSq(e.prevEye, e.currEye) > kCutDistSq;
        if (!cut) {
          float rotDot = 0.0f;
          for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10})
            rotDot += e.prevView[i] * e.currView[i];
          cut = (rotDot / 3.0f) < kCutRotDot;
        }
        if (cut) {
          for (int i = 0; i < 16; ++i)
            view[i] = e.currView[i]; // hard cut: snap
        } else {
          LerpMatrix(e.prevView, e.currView, bd::engine::Alpha(), view);
        }
        replaced = true;
      }
    }
  }

  // The head pose composes on top of whatever the camera turned out to be,
  // interpolated or not - it is an offset from the shot the game framed, not a
  // replacement for it. Which is also why this runs even when interpolation is
  // off: VR does not want to depend on the frame rate setting.
  if (bd::xr::ViewOverrideActive() && ctx.r4.u32 != 0) {
    if (!replaced)
      ReadFloats(bd::mem::at<be_f32>(ctx.r4.u32), view, 16);
    float composed[16];
    if (bd::xr::ComposeView(view, composed)) {
      for (int i = 0; i < 16; ++i)
        view[i] = composed[i];
      replaced = true;
    }
  }

  bd::gpu::scene::UpdateRenderTransforms(ctx, base, replaced ? view : nullptr);
}

// Poll input at 30Hz so edge-detect and auto-repeat stay in lockstep with the
// logic.
REX_EXTERN(__imp__bdInputSystemUpdate);
REX_HOOK_RAW(bdInputSystemUpdate) {
  if (!bd::engine::TickDue())
    return;
  // Ahead of the original, so the game's own screens see the cursor write
  // already applied when they poll input this tick. A second REX_HOOK_RAW on
  // the same symbol would collide at link time.
  bd::engine::SampleButtonEdges();
  bd::engine::MenuMouse::Get().BeginFrame();
  // After BeginFrame, which publishes whether a menu owns input this frame, so
  // a look starts and stops on the same tick the menu opens.
  bd::engine::UpdateMouseLook();
  bd::engine::MouseCursorTick();
  bd::engine::Glyphs::Get().Tick();
  bd::engine::D2AnimeTask::Tick();
  bd::engine::CampSettings::Get().Tick();
  // After BeginFrame too: the bind stands down while a menu owns input.
  bd::engine::AreaMapTick();
  __imp__bdInputSystemUpdate(ctx, base);
}

// PadVibrationCore::vf03 drains the accumulated amplitude with a store, not a
// max, so it has to run at the same 30Hz that fills it.
REX_EXTERN(__imp__PadVibrationCore__vf03);
REX_HOOK_RAW(PadVibrationCore__vf03) {
  if (!bd::engine::TickDue())
    return;
  __imp__PadVibrationCore__vf03(ctx, base);
}

// r4 is the current bone palette, the previous one sits at r4 + 0x600. Redirect
// it to a scratch holding lerp(prev, curr, alpha) for the render only. The
// engine's own buffers are untouched.
void bdObjectPaletteInterpHook(PPCRegister &r4) {
  if (!bd::engine::InterpolationActive())
    return;
  const float a = bd::engine::Alpha();
  if (a <= 0.0f)
    return;
  const u32 currVa = r4.u32;
  if (!currVa)
    return;
  auto *cur = bd::mem::at<be_f32>(currVa);
  auto *prv = bd::mem::at<be_f32>(currVa + kPalettePrevDelta);
  if (!cur || !prv || PaletteDiscontinuous(cur, prv, kPaletteFloats))
    return;
  const u32 s = LerpGuestFloats(currVa, currVa + kPalettePrevDelta,
                                kPaletteFloats, g_paletteScratch, a);
  if (s)
    r4.u32 = s;
}

// r3 is the current object world matrix, r28 the previous one. Same
// render-only redirect as the palette above.
void bdObjectWorldInterpHook(PPCRegister &r3, PPCRegister &r28) {
  if (!bd::engine::InterpolationActive())
    return;
  const float a = bd::engine::Alpha();
  if (a <= 0.0f)
    return;
  const u32 currVa = r3.u32, prevVa = r28.u32;
  if (!currVa || !prevVa)
    return;
  auto *cur = bd::mem::at<be_f32>(currVa);
  auto *prv = bd::mem::at<be_f32>(prevVa);
  if (!cur || !prv || WorldMatrixDiscontinuous(cur, prv))
    return;
  const u32 s =
      LerpGuestFloats(currVa, prevVa, kWorldFloats, g_worldScratch, a);
  if (s)
    r3.u32 = s;
}

// Guest timers, the self-paced CRI movie threads included, need a stable
// real-time timebase rather than the scaled guest clock.
u32 rex_QueryPerformanceCounter_hook(u32 lpPerformanceCount) {
  if (lpPerformanceCount) {
    auto *out = bd::mem::at<be_i64>(lpPerformanceCount);
    if (out)
      *out = std::chrono::steady_clock::now().time_since_epoch().count();
  }
  return 1;
}
REX_HOOK(rex_QueryPerformanceCounter, rex_QueryPerformanceCounter_hook);

u32 rex_QueryPerformanceFrequency_hook(u32 lpFrequency) {
  if (lpFrequency) {
    constexpr i64 kFreq = std::chrono::steady_clock::period::den /
                          std::chrono::steady_clock::period::num;
    auto *out = bd::mem::at<be_i64>(lpFrequency);
    if (out)
      *out = kFreq;
  }
  return 1;
}
REX_HOOK(rex_QueryPerformanceFrequency, rex_QueryPerformanceFrequency_hook);

/**
 * @file    gpu/hooks/state.cpp
 * @brief   Guest hooks that set the draw state: targets, viewport, bindings,
 *          render state and the shader constant dirty marks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <cstring>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/foveation.h"
#include "gpu/draw_queue.h"
#include "gpu/d3d.h"
#include "gpu/constant_buffers.h"
#include "gpu/scene/host_parameter_bridge.h"
#include "gpu/device.h"
#include "gpu/shadow_fit.h"
#include "gpu/frame_stats.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/physical_buffers.h"
#include "gpu/pass_bindings.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/native_alpha_bridge.h"
#include "gpu/scene/native_blend_bridge.h"
#include "gpu/scene/native_raster_bridge.h"
#include "gpu/shaders/shader_constants.h"

namespace {

using bd::gpu::ResolveGuestBufferVa;

void D3DDevice_SetViewport_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice,
    rex::MappedPtr<bd::gpu::D3DViewport9> pViewport) {
  if (!pViewport) {
    return;
  }

  auto &s = bd::gpu::state();

  const float fx = static_cast<float>(u32(pViewport->X));
  const float fy = static_cast<float>(u32(pViewport->Y));
  const float fw = static_cast<float>(u32(pViewport->Width));
  const float fh = static_cast<float>(u32(pViewport->Height));
  const float fmin = float(pViewport->MinZ);
  const float fmax = float(pViewport->MaxZ);

  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x,
                                       fx);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y,
                                       fy);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width,
                                       fw);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.height, fh);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.minDepth, fmin);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.maxDepth, fmax);

  s.dirtyStates.scissorRect =
      s.dirtyStates.scissorRect || s.dirtyStates.viewport;

  if (pDevice) {
    pDevice->viewport.X = u32(pViewport->X);
    pDevice->viewport.Y = u32(pViewport->Y);
    pDevice->viewport.Width = u32(pViewport->Width);
    pDevice->viewport.Height = u32(pViewport->Height);
    pDevice->viewport.MinZ = fmin;
    pDevice->viewport.MaxZ = fmax;
  }
}

void D3DDevice_SetRenderTarget_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice, u32 RenderTargetIndex,
    rex::MappedPtr<bd::gpu::D3DSurface> pRenderTarget) {
  if (pDevice && RenderTargetIndex < 4)
    pDevice->renderTargetShadow[RenderTargetIndex] = pRenderTarget.guest_address();
  // Additional colour attachments are still an explicit unsupported boundary;
  // the engine's null writes must not unbind colour attachment zero.
  if (RenderTargetIndex != 0)
    return;
  auto *surface = bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(
      pRenderTarget.guest_address());
  bd::gpu::BindColorAttachment(surface);
  bd::gpu::Video::SetDefaultViewport(pDevice, surface);
}

void D3DDevice_SetDepthStencilSurface_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice,
    rex::MappedPtr<bd::gpu::D3DSurface> pZStencilSurface) {
  if (pDevice)
    pDevice->depthStencilShadow = pZStencilSurface.guest_address();
  auto *surface = bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(
      pZStencilSurface.guest_address());
  if (surface && surface->type != bd::gpu::ResourceType::DepthStencil &&
      !(surface->type == bd::gpu::ResourceType::Texture &&
        bd::gpu::IsDepthFormat(surface->format)))
    surface = nullptr;
  bd::gpu::BindDepthAttachment(surface);
  bd::gpu::Video::SetDefaultViewport(pDevice, surface);
}

void D3DDevice_SetScissorRect_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> /*pDevice*/,
    rex::MappedPtr<bd::gpu::D3DRect> pRect) {
  if (!pRect) {
    return;
  }
  // Scissor always tracks the viewport extent in FlushViewport, so the rect
  // values are unused, and only the dirty mark matters.
  bd::gpu::state().dirtyStates.scissorRect = true;
}

void D3DDevice_SetVertexShader_hook(u32 /*device*/, u32 shader_guest) {
  auto *shader =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestShader>(shader_guest);
  bd::gpu::Video::SetVertexShader(shader);
}

void D3DDevice_SetPixelShader_hook(u32 /*device*/, u32 shader_guest) {
  auto *shader =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestShader>(shader_guest);
  bd::gpu::Video::SetPixelShader(shader);
}

void D3DDevice_SetVertexDeclaration_hook(u32 /*device*/, u32 decl_guest) {
  auto *decl =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestVertexDeclaration>(
          decl_guest);

  // The recompiled VS guards its R11G11B10/SNORM decode on this bit, else it
  // asfloat()s the normal bits to garbage.
  {
    auto &s = bd::gpu::state();
    u32 spec =
        s.pipelineState.specConstants & ~bd::gpu::kSpecConstantR11G11B10Normal;
    if (decl && decl->hasR11G11B10Normal)
      spec |= bd::gpu::kSpecConstantR11G11B10Normal;
    bd::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                       s.pipelineState.specConstants, spec);
  }

  bd::gpu::Video::SetVertexDeclaration(decl);
}

void D3DDevice_SetTexture_hook(u32 /*device*/, u32 sampler, u32 texture_guest) {
  auto *tex = bd::gpu::ResolveGuestTexture(texture_guest);
  const bool unresolved = (!tex && texture_guest);
  if (unresolved) {
    // Unresolved (e.g. outside the supported native format set): green marker.
    tex = bd::gpu::GetOrCreateDebugTexture();
  }
  bd::gpu::Video::SetTexture(sampler, tex);
}

void D3DDevice_SetStreamSource_hook(u32 /*device*/, u32 stream,
                                    u32 buffer_guest, u32 offset, u32 stride) {
  auto *buf =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestBuffer>(buffer_guest);
  // Physical VBs wrap an engine-owned D3DVertexBuffer struct never
  // HostResourceHeap::Alloc'd, so FromGuest misses. Struct VA map covers VBs
  // registered eagerly (bdSceneGraphRegisterVBHook), and the lazy bootstrap
  // reads the struct's Xenos fetch constant fields for buffers from unhooked
  // paths.
  if (!buf && buffer_guest) {
    buf =
        ResolveGuestBufferVa(buffer_guest, bd::gpu::ResourceType::VertexBuffer);
  }
  if (buf && buf->hasBuffer()) {
    auto ref = buf->bufferRef(offset);
    const u32 bound_size = offset < buf->dataSize ? buf->dataSize - offset : 0;
    bd::gpu::Video::SetVertexStream(stream, ref, bound_size, stride);
  } else {
    bd::gpu::Video::SetVertexStream(stream, plume::RenderBufferReference{}, 0,
                                    0);
  }
  bd::gpu::Video::NoteStreamSource(stream, buffer_guest, offset);
}

void D3DDevice_SetIndices_hook(u32 /*device*/, u32 indices_guest) {
  auto *ib =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestBuffer>(indices_guest);
  // Physical IBs wrap an engine-owned struct FromGuest misses (same as
  // SetStreamSource). Without the struct VA bridge + lazy bootstrap, every
  // scene mesh's IB binds null, every index reads 0, scene goes black.
  if (!ib && indices_guest) {
    ib =
        ResolveGuestBufferVa(indices_guest, bd::gpu::ResourceType::IndexBuffer);
  }
  bd::gpu::Video::SetIndices(ib);
  bd::gpu::Video::NoteIndexSource(indices_guest);
}

// Integer/bool and remaining inline constant writers retain their original
// execution and dirty adapters. Float setters execute in the host bridge.
#define REBLUE_CONSTANT_DIRTY_HOOK(fn, mark)                                   \
  REX_EXTERN(__imp__##fn);                                                     \
  REX_HOOK_RAW(fn) {                                                           \
    __imp__##fn(ctx, base);                                                    \
    mark;                                                                      \
  }

// The float setters also tell the host-issued node draw which registers a
// node's interpreter run writes: (device, start register, data, count).
REX_HOOK_RAW(D3DDevice_SetVertexShaderConstantFN) {
  bd::gpu::scene::SetHostFloatParameters(ctx, base, true);
}
REX_HOOK_RAW(D3DDevice_SetPixelShaderConstantFN) {
  bd::gpu::scene::SetHostFloatParameters(ctx, base, false);
}
REBLUE_CONSTANT_DIRTY_HOOK(D3DDevice_SetVertexShaderConstantI,
                           bd::gpu::Video::MarkVSConstantsDirty())
// The bool setters also tell the host-issued node draw which bool registers
// a node's interpreter run writes: (device, start, data, count). A replay
// takes those bits from its template and the rest from the live device
// (2026-09-03: the ground pieces at the village rock set PS bools 0 and 3
// per node; taken live they were the previous node's, and the pieces drew
// without their texture path - the "cyan skirt").
REX_EXTERN(__imp__D3DDevice_SetVertexShaderConstantB);
REX_HOOK_RAW(D3DDevice_SetVertexShaderConstantB) {
  const u32 start = ctx.r4.u32;
  const u32 count = ctx.r6.u32;
  __imp__D3DDevice_SetVertexShaderConstantB(ctx, base);
  bd::gpu::Video::MarkVSConstantsDirty();
  bd::gpu::scene::NoteBoolsSet(true, start, count);
}
REBLUE_CONSTANT_DIRTY_HOOK(D3DDevice_SetPixelShaderConstantI,
                           bd::gpu::Video::MarkPSConstantsDirty())
REX_EXTERN(__imp__D3DDevice_SetPixelShaderConstantB);
REX_HOOK_RAW(D3DDevice_SetPixelShaderConstantB) {
  const u32 start = ctx.r4.u32;
  const u32 count = ctx.r6.u32;
  __imp__D3DDevice_SetPixelShaderConstantB(ctx, base);
  bd::gpu::Video::MarkPSConstantsDirty();
  bd::gpu::scene::NoteBoolsSet(false, start, count);
}

// bdSetViewportConstants writes (1/W, 1/H, 0, scale) into VS c21 (device+0x850)
// and PS c21 (device+0x1850), while Visual__DrawVerticesUP writes PS c3
// (device+0x1730) and, when its 4th arg is non-null, VS c20 (device+0x840).
// Both write the constant shadows directly via g_pD3DDevice and skip the D3D
// setter path, so the FN setter hooks never see them, so mark both stages
// dirty.
REBLUE_CONSTANT_DIRTY_HOOK(bdSetViewportConstants,
                           (bd::gpu::Video::MarkVSConstantsDirty(),
                            bd::gpu::Video::MarkPSConstantsDirty()))
// This one also brackets the 2D overlay scope. Visual__DrawVerticesUP is where
// Blue Dragon flushes its sorted 2D content - sprites, the intro credits, the
// HUD - so every draw inside it is an overlay, and the stereo path puts those
// in *both* eyes rather than once across the seam.
//
// Bracketing the guest's own function beats inspecting vertices: two
// shape-based tests were tried and both quadrupled the frame, because a
// full-screen post blit is also a four-vertex triangle strip at the sprite
// stride and must not be doubled.
REX_EXTERN(__imp__Visual__DrawVerticesUP);
REX_HOOK_RAW(Visual__DrawVerticesUP) {
  auto &s = bd::gpu::state();
  const bool outer = s.overlay2DScope;
  s.overlay2DScope = true;
  __imp__Visual__DrawVerticesUP(ctx, base);
  s.overlay2DScope = outer;
  bd::gpu::Video::MarkVSConstantsDirty();
  bd::gpu::Video::MarkPSConstantsDirty();
}
// Visual__DrawSortedQueues also writes PS c3 (device+0x1730) inline during its
// draw setup, a separate writer from Visual__DrawVerticesUP that likewise skips
// the setter path.
REBLUE_CONSTANT_DIRTY_HOOK(Visual__DrawSortedQueues,
                           (bd::gpu::Video::MarkVSConstantsDirty(),
                            bd::gpu::Video::MarkPSConstantsDirty()))

#undef REBLUE_CONSTANT_DIRTY_HOOK

} // namespace

REX_HOOK(D3DDevice_SetViewport, D3DDevice_SetViewport_hook);
REX_HOOK(D3DDevice_SetRenderTarget, D3DDevice_SetRenderTarget_hook);
REX_HOOK(D3DDevice_SetDepthStencilSurface,
         D3DDevice_SetDepthStencilSurface_hook);
REX_HOOK(D3DDevice_SetScissorRect, D3DDevice_SetScissorRect_hook);
REX_HOOK(D3DDevice_SetVertexShader, D3DDevice_SetVertexShader_hook);
REX_HOOK(D3DDevice_SetPixelShader, D3DDevice_SetPixelShader_hook);
REX_HOOK(D3DDevice_SetVertexDeclaration, D3DDevice_SetVertexDeclaration_hook);
REX_HOOK(D3DDevice_SetTexture, D3DDevice_SetTexture_hook);
REX_HOOK(D3DDevice_SetStreamSource, D3DDevice_SetStreamSource_hook);
REX_HOOK(D3DDevice_SetIndices, D3DDevice_SetIndices_hook);
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_HOOK_RAW(bdSetRenderState) {
  if (!bd::gpu::scene::UpdateAlphaImport(ctx, base) &&
      !bd::gpu::scene::UpdateBlendImport(ctx, base))
    bd::gpu::scene::UpdateRasterImport(ctx, base);
}

// Initialization seeds the render cache via SDK getters rather than the
// ordinary setter. Invalidate once after it, not by polling cache every draw.
REX_EXTERN(__imp__bdEngineInit);
REX_HOOK_RAW(bdEngineInit) {
  __imp__bdEngineInit(ctx, base);
  bd::gpu::scene::ResetRasterImport();
  bd::gpu::scene::ResetBlendImport();
  bd::gpu::scene::ResetAlphaImport();
}

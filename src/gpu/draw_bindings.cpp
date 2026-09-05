/**
 * @file    gpu/draw_bindings.cpp
 * @brief   The Set* binders: the guest device's texture, shader, stream and
 *          viewport state mirrored into VideoState for the draw path to read.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

#include <plume_render_interface.h>

#include "gpu/scene/host_draw.h"
#include "gpu/scene/native_alpha.h"
#include "gpu/shaders/shader_constants.h"

REXCVAR_DECLARE(bool, bd_debug_no_alpha_test);
REXCVAR_DECLARE(i32, bd_debug_fill_scale);

namespace bd::gpu {

// Shader cutoff binding. Live native alpha intent supplies ordinary draws;
// retained replay recipes temporarily override and restore this binding.
namespace {
std::atomic<u32> g_alpha_threshold_bits{0};
} // namespace

void Video::SetTexture(u32 index, GuestTexture *texture) {
  if (index >= 16)
    return;
  // A null SetTexture is not an unbind on Xenos: it clears a bookkeeping
  // pointer and leaves the fetch constant alone, so the slot keeps its last
  // non-null texture until a real bind replaces it.
  if (texture == nullptr)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (texture->texture) {
    BindTextureSRVLocked(s, texture);
    // SharedConstants substitution samples the source as a Texture2D, which an
    // MSAA descriptor cannot back.
    if (texture->sourceSurface && texture->sourceSurface->texture &&
        texture->sourceSurface->sampleCount ==
            plume::RenderSampleCount::COUNT_1) {
      BindTextureSRVLocked(s, texture->sourceSurface);
    }
  }
  if (s.textures[index] != texture)
    s.texture_bindings_dirty = true;
  s.textures[index] = texture;
  bd::gpu::scene::NoteTextureSet(index);
}

void Video::SetVertexShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_shader = shader;
}

void Video::SetPixelShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.pixel_shader = shader;
}

void Video::SetVertexDeclaration(GuestVertexDeclaration *decl) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_declaration = decl;
}

void Video::NoteStreamSource(u32 slot, u32 guest_va, u32 offset) {
  if (slot >= 16)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_stream_va[slot] = guest_va;
  s.vertex_stream_offset[slot] = offset;
}

void Video::NoteIndexSource(u32 guest_va) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.index_va = guest_va;
}

void Video::SetIndices(GuestBuffer *indices) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.index_buffer = indices;
  plume::RenderBufferReference new_buffer{};
  u32 new_size = 0;
  plume::RenderFormat new_format = plume::RenderFormat::R16_UINT;
  if (indices && indices->hasBuffer()) {
    new_buffer = indices->bufferRef(0);
    new_size = indices->dataSize;
    // A GuestBuffer registered through a path that never read the X360
    // Common field index format bit has none pinned. BD's default is 16-bit.
    new_format = (indices->format == plume::RenderFormat::UNKNOWN)
                     ? plume::RenderFormat::R16_UINT
                     : indices->format;
  }
  SetDirtyValue(s.dirtyStates.indices, s.index_view.buffer, new_buffer);
  SetDirtyValue(s.dirtyStates.indices, s.index_view.size, new_size);
  SetDirtyValue(s.dirtyStates.indices, s.index_view.format, new_format);
}

void Video::ScrubBufferBindings(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ScrubBufferBindingsLocked(buffer);
}

void Video::ScrubBufferBindingsLocked(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  auto &s = state();
  for (u32 i = 0; i < 16; ++i) {
    if (s.vertex_views[i].buffer.ref != buffer)
      continue;
    s.vertex_views[i].buffer = plume::RenderBufferReference{};
    s.vertex_views[i].size = 0;
    const u8 b = static_cast<u8>(i);
    if (b < s.dirtyStates.vertexStreamFirst)
      s.dirtyStates.vertexStreamFirst = b;
    if (b > s.dirtyStates.vertexStreamLast)
      s.dirtyStates.vertexStreamLast = b;
  }
  if (s.index_view.buffer.ref == buffer) {
    s.index_view.buffer = plume::RenderBufferReference{};
    s.index_view.size = 0;
    s.dirtyStates.indices = true;
  }
}

void Video::SetVertexStream(u32 slot, plume::RenderBufferReference buffer,
                            u32 size, u32 stride) {
  if (slot >= 16)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  // Expanding the dirty range only on a real change lets FlushRenderState skip
  // the per-draw 16-slot setVertexBuffers, and bind just [first, last] when
  // something did move.
  bool dirty = false;
  SetDirtyValue(dirty, s.vertex_views[slot].buffer, buffer);
  SetDirtyValue(dirty, s.vertex_views[slot].size, size);
  SetDirtyValue(dirty, s.input_slots[slot].stride, stride);
  if (dirty) {
    const u8 i = static_cast<u8>(slot);
    if (i < s.dirtyStates.vertexStreamFirst)
      s.dirtyStates.vertexStreamFirst = i;
    if (i > s.dirtyStates.vertexStreamLast)
      s.dirtyStates.vertexStreamLast = i;
  }
}

void Video::MarkVSConstantsDirty() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.dirtyStates.vertexShaderConstants = true;
}

void Video::MarkPSConstantsDirty() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.dirtyStates.pixelShaderConstants = true;
}

void Video::SetAlphaThreshold(float value) {
  u32 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  g_alpha_threshold_bits.store(bits, std::memory_order_relaxed);
}

float Video::AlphaThreshold() {
  const u32 bits = g_alpha_threshold_bits.load(std::memory_order_relaxed);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void Video::ApplyAlphaIntent(const scene::AlphaState &intent) {
  auto &s = state();
  scene::ApplyAlphaState(intent, s.pipelineState, s.dirtyStates.pipelineState,
                         s.pipelineState.sampleCount !=
                             plume::RenderSampleCount::COUNT_1,
                         REXCVAR_GET(bd_debug_no_alpha_test));
  SetAlphaThreshold(intent.threshold);
}

void Video::SetDefaultViewport(D3DDevice *device, GuestTexture *surface) {
  if (surface == nullptr)
    return;

  auto &s = state();

  const float fwidth = static_cast<float>(surface->width);
  const float fheight = static_cast<float>(surface->height);

  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width, fwidth);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.height, fheight);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.minDepth, 0.0f);
  SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.maxDepth, 1.0f);

  s.dirtyStates.scissorRect =
      s.dirtyStates.scissorRect || s.dirtyStates.viewport;

  // Device-side viewport write-back through the marshaled typed pointer.
  if (device != nullptr) {
    device->viewport.X = 0;
    device->viewport.Y = 0;
    device->viewport.Width = surface->width;
    device->viewport.Height = surface->height;
    device->viewport.MinZ = 0.0f;
    device->viewport.MaxZ = 1.0f;
  }
}

void Video::SetDesignCanvasDrain(bool on) { state().design_canvas_drain = on; }

bool Video::DesignCanvasDrain() { return state().design_canvas_drain; }

void Video::FlushViewport() {
  auto &s = state();
  if (!s.command_list)
    return;

  // While deferring, the viewport and scissor travel with the draw instead of
  // being set now - see QueuedDraw. Computed unconditionally rather than under
  // the dirty flags, because "unchanged since the last draw" stops meaning
  // anything once draws are reordered.
  if (s.deferring_draw) {
    plume::RenderViewport vp = s.viewport;
    if (vp.minDepth > vp.maxDepth)
      std::swap(vp.minDepth, vp.maxDepth);
    float sw = s.viewport.width;
    float sh = s.viewport.height;
    if (const i32 pct = REXCVAR_GET(bd_debug_fill_scale); pct < 100) {
      sw = sw * float(pct) / 100.0f;
      sh = sh * float(pct) / 100.0f;
    }
    const plume::RenderRect rc{static_cast<i32>(s.viewport.x),
                               static_cast<i32>(s.viewport.y),
                               static_cast<i32>(s.viewport.x + sw),
                               static_cast<i32>(s.viewport.y + sh)};
    // Only a viewport that would actually rasterise something. The immediate
    // path is dirty-gated and so can never push one the guest has not
    // established; recording unconditionally can, and a degenerate viewport
    // clips the draw away entirely - which looks like "the draws execute and
    // do no GPU work", exactly the symptom being chased.
    s.pending.has_viewport =
        vp.width > 0.0f && vp.height > 0.0f && rc.right > rc.left &&
        rc.bottom > rc.top;
    if (s.pending.has_viewport) {
      s.pending.viewport = vp;
      s.pending.scissor = rc;
    }
    s.dirtyStates.viewport = false;
    s.dirtyStates.scissorRect = false;
    return;
  }

  if (s.dirtyStates.viewport) {
    plume::RenderViewport vp = s.viewport;
    if (vp.minDepth > vp.maxDepth)
      std::swap(vp.minDepth, vp.maxDepth);
    s.command_list->setViewports(&vp, 1);
    s.dirtyStates.viewport = false;
  }

  if (s.dirtyStates.scissorRect) {
    // Scissor always tracks the viewport extent.
    float sw = s.viewport.width;
    float sh = s.viewport.height;
    // Diagnostic: clip fragments without touching anything else. The viewport
    // is left alone deliberately, so vertex work, draw count, pipeline state
    // and binning input are bit-identical and only the fragment count moves.
    // Every earlier attempt to measure fill by lowering the output resolution
    // was void - it resized a surface taking 21 draws while the scene surface
    // taking 2434 stayed pinned at the 1280x720 design canvas.
    if (const i32 pct = REXCVAR_GET(bd_debug_fill_scale); pct < 100) {
      sw = sw * float(pct) / 100.0f;
      sh = sh * float(pct) / 100.0f;
    }
    plume::RenderRect rc{static_cast<i32>(s.viewport.x),
                         static_cast<i32>(s.viewport.y),
                         static_cast<i32>(s.viewport.x + sw),
                         static_cast<i32>(s.viewport.y + sh)};
    s.command_list->setScissors(&rc, 1);
    s.dirtyStates.scissorRect = false;
  }
}

} // namespace bd::gpu

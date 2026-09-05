/**
 * @file    gpu/draw.cpp
 * @brief   What every draw flushes: live native render intent, the PSO
 *          lookup, and the constant buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include "gpu/frame.h"

#include <cstddef>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/vertex_pull.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/native_blend_bridge.h"
#include "gpu/scene/native_raster_bridge.h"
#include "gpu/shaders/shader_constants.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_recorder.h"

REXCVAR_DECLARE(bool, bd_blend_no_depth_write);
REXCVAR_DECLARE(bool, bd_depth_prepass);
REXCVAR_DECLARE(bool, bd_draw_instancing);
REXCVAR_DECLARE(bool, bd_draw_pull);
REXCVAR_DECLARE(bool, bd_host_draw_records);
REXCVAR_DECLARE(bool, bd_blend_off_when_opaque);
REXCVAR_DECLARE(i32, bd_debug_max_pso);

namespace bd::gpu {
namespace {

// Live host intent is independent of the last bound/replayed pipeline. Normal
// draws do not import Xenos blend words or the engine raster cache. Explicit
// correctness switches retain legacy/diagnostic imports inside the bridges.
void ApplyNativeRenderState(VideoState &s, u32 device_guest) {
  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;
  scene::ApplyBlendState(scene::CurrentBlendIntent(device_guest), ps, dirty);
  scene::ApplyRasterState(scene::CurrentRasterIntent(), ps, dirty);
  // Existing diagnostic switches remain explicit; native intent itself is not
  // mutated by a temporary per-draw override.
  if (ps.zWriteEnable && ps.alphaBlendEnable &&
      REXCVAR_GET(bd_blend_no_depth_write)) {
    Video::SetDirtyValue(dirty, ps.zWriteEnable, false);
    bd::gpu::NoteDepthWriteSuppressed();
  }
  if (ps.zWriteEnable && ps.alphaBlendEnable &&
      REXCVAR_GET(bd_blend_off_when_opaque)) {
    Video::SetDirtyValue(dirty, ps.alphaBlendEnable, false);
    Video::SetDirtyValue(dirty, ps.srcBlend, plume::RenderBlend::ONE);
    Video::SetDirtyValue(dirty, ps.destBlend, plume::RenderBlend::ZERO);
  }
}
} // namespace

// Re-base the three guest constant blocks for the next draw.
//
// The blocks live at moving offsets inside one buffer that is bound for the
// life of the device, so a draw changes an offset, never a descriptor. That is
// what a D3D12 root descriptor does, and it is why the shader can read them as
// uniform buffers instead of dereferencing a 64-bit device address per
// invocation.
static void BindGuestConstants(VideoState &s) {
#if !defined(REBLUE_D3D12)
  if (s.deferring_draw) {
    // Recorded, not bound. The three offsets are the draw's whole material -
    // transform, parameters, and every texture and sampler descriptor index -
    // so capturing them captures what the draw reads.
    s.pending.constant_offsets[0] = s.constant_dyn_offsets[0];
    s.pending.constant_offsets[1] = s.constant_dyn_offsets[1];
    s.pending.constant_offsets[2] = s.constant_dyn_offsets[2];
    return;
  }
  if (s.texture_descriptor_set && s.command_list) {
    static bool told = false;
    if (!told && (s.constant_dyn_offsets[0] || s.constant_dyn_offsets[1] ||
                  s.constant_dyn_offsets[2])) {
      told = true;
      BD_INFO("[constants] first non-zero dynamic offsets {} {} {}",
              s.constant_dyn_offsets[0], s.constant_dyn_offsets[1],
              s.constant_dyn_offsets[2]);
    }
    // The constant set holds only the three ranges, so this per-draw bind
    // copies 48 bytes in the driver, where the heap it used to share with
    // copied kilobytes.
    s.command_list->setGraphicsDescriptorSetDynamic(
        s.constant_descriptor_set.get(), kConstantDescriptorSetIndex,
        s.constant_dyn_offsets, 3);
  }
#endif
}

// Re-uploads the vertex constants with an eye skew and rebinds them, for the
// second and subsequent views of one recorded draw. FlushRenderState has
// already uploaded and bound the unskewed block by this point, so the caller
// must dirty vertexShaderConstants afterwards to put the next draw back on a
// clean one.
bool Video::BindEyeVertexConstants(u32 device_guest, float eye_skew,
                                   float eye_shift) {
  auto &s = state();
  if (!device_guest || !s.command_list)
    return false;
  const u32 *mask =
      (s.pipelineState.vertexShader &&
       s.pipelineState.vertexShader->shaderCacheEntry)
          ? s.pipelineState.vertexShader->shaderCacheEntry->constantRegisterMask
          : nullptr;
  auto alloc =
      UploadVertexShaderConstants(device_guest, eye_skew, eye_shift, mask);
  if (alloc.failed)
    return false;
  if (!alloc.size)
    return true;
#if defined(REBLUE_D3D12)
  s.command_list->setGraphicsRootDescriptor(alloc.ref, 0);
#else
  s.constant_dyn_offsets[0] = alloc.dynamicOffset;
  BindGuestConstants(s);
#endif
  return true;
}

bool Video::FlushRenderState(u32 device_guest) {
  std::lock_guard lock(state().mutex);
  return FlushRenderStateLocked(device_guest);
}

bool Video::FlushRenderStateLocked(u32 device_guest) {
  auto &s = state();
  // A confirmed device-removed event is terminal: stop recording so the render
  // thread cannot race the fatal dialog into a crash.
  if (DeviceIsLost())
    return false;
  if (!s.command_list_open)
    return false;
  if (!s.draw_framebuffer_bound)
    return false;
  // CPU zone: a GPU zone here would add two GPU timestamps per draw.
  BD_CPU_ZONE("FlushRenderState");

  // Fold the Set*-hook mirrors into the pipelineState the PSO lookup reads.
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexShader,
                s.vertex_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.pixelShader,
                s.pixel_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexDeclaration,
                s.vertex_declaration);

  for (u32 i = 0; i < 16; ++i) {
    SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexStrides[i],
                  static_cast<u8>(s.input_slots[i].stride));
  }

  // The PSO's formats must match the framebuffer BindDrawFramebuffer attached,
  // so they come from the same ResolveEffectiveTargets pair it binds.
  {
    GuestTexture *rt = nullptr;
    GuestTexture *ds = nullptr;
    ResolveEffectiveTargets(s, rt, ds);
    const auto rt_format = rt ? rt->format : plume::RenderFormat::UNKNOWN;
    const auto ds_format = ds ? ds->format : plume::RenderFormat::UNKNOWN;
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.renderTargetFormat, rt_format);
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.depthStencilFormat, ds_format);
  }

  // Restore live raster/blend intent after any retained pipeline replay.
  ApplyNativeRenderState(s, device_guest);

  // Anything missing here means the engine has not wired the pipeline up yet.
  if (!s.pipelineState.vertexShader || !s.pipelineState.vertexDeclaration) {
    u32 n;
    if (DiagShouldLog(3, s.render_target, &n)) {
      BD_WARN("[draw-diag] #{} draw dropped: vs={} decl={} ps={} rt={}x{}", n,
              static_cast<void *>(s.pipelineState.vertexShader),
              static_cast<void *>(s.pipelineState.vertexDeclaration),
              static_cast<void *>(s.pipelineState.pixelShader),
              s.render_target ? s.render_target->width : 0,
              s.render_target ? s.render_target->height : 0);
    }
    return false;
  }

  // D3D12 retains the bound pipeline across draws in a command list, so a clean
  // pipelineState can skip both the cache lookup and the bind.
  // BeginCommandList force-dirties this on every command list reset.
  // Diagnostic. A field scene binds 1121 pipelines across 2848 draws and the
  // GPU spends ~32us on a draw that moves 140 vertices - a cost that survives
  // halving the resolution, so it is per-draw and not fill. Pipeline switches
  // are the last standing explanation.
  //
  // Capping them answers it: past the cap the previously bound pipeline is
  // reused, so the scene renders with wrong materials but the draw count and
  // geometry are untouched. If the fence collapses, PSO switching is the cost.
  bool pso_capped = false;
  if (const i32 cap = REXCVAR_GET(bd_debug_max_pso); cap > 0) {
    static u32 s_frame = 0;
    static u32 s_count = 0;
    const u32 frame = FrameStatFrameCount();
    if (frame != s_frame) {
      s_frame = frame;
      s_count = 0;
    }
    if (s.dirtyStates.pipelineState) {
      if (s_count >= static_cast<u32>(cap))
        pso_capped = true;
      else
        ++s_count;
    }
  }

  if (s.dirtyStates.pipelineState && !pso_capped) {
    PipelineState lookup = s.pipelineState;
    SanitizePipelineState(lookup);
    bool built = false;
    auto *pso = GetOrCreatePipeline(lookup, &built);
    if (!pso) {
      u32 n;
      if (DiagShouldLog(4, s.render_target, &n)) {
        BD_WARN("[draw-diag] #{} draw dropped: PSO build failed (vs={} ps={} "
                "rt={}x{} fmt={})",
                n, static_cast<void *>(s.pipelineState.vertexShader),
                static_cast<void *>(s.pipelineState.pixelShader),
                s.render_target ? s.render_target->width : 0,
                s.render_target ? s.render_target->height : 0,
                u32(s.pipelineState.renderTargetFormat));
      }
      return false;
    }
    // 'built' means this draw compiled the PSO synchronously, so neither
    // residual nor predictor covered it. Warns once per pipeline, and
    // REBLUE_PSO_CAP builds also capture it for the residual/template tooling.
    RecordPipelineState(lookup, CurrentRenderPassId(), built);
    if (!s.deferring_draw)
      s.command_list->setPipeline(pso);
    NotePSOSwitch();
    s.current_pso = pso;

    // Depth-prepass variants, built beside the real pipeline and cached the
    // same way. Only for a deferred draw that writes depth with a LESS or
    // LEQUAL test and touches no stencil - anything else keeps its single
    // pass. The prepass pipeline drops colour writes and blending; the colour
    // pipeline drops the depth write and tests LEQUAL so the draw passes
    // against the depth it laid down itself. See draw_queue.cpp for why.
    s.current_prepass_pso = nullptr;
    s.current_color_pso = nullptr;
    if (s.deferring_draw && REXCVAR_GET(bd_depth_prepass) && s.depth_stencil &&
        lookup.zEnable && lookup.zWriteEnable && !lookup.stencilEnable &&
        (lookup.zFunc == plume::RenderComparisonFunction::LESS ||
         lookup.zFunc == plume::RenderComparisonFunction::LESS_EQUAL)) {
      PipelineState pre = lookup;
      pre.colorWriteEnable = 0;
      pre.alphaBlendEnable = false;
      pre.srcBlend = plume::RenderBlend::ONE;
      pre.destBlend = plume::RenderBlend::ZERO;
      pre.srcBlendAlpha = plume::RenderBlend::ONE;
      pre.destBlendAlpha = plume::RenderBlend::ZERO;
      SanitizePipelineState(pre);
      PipelineState col = lookup;
      col.zWriteEnable = false;
      col.zFunc = plume::RenderComparisonFunction::LESS_EQUAL;
      SanitizePipelineState(col);
      auto *pre_pso = GetOrCreatePipeline(pre, nullptr);
      auto *col_pso = GetOrCreatePipeline(col, nullptr);
      if (pre_pso && col_pso) {
        s.current_prepass_pso = pre_pso;
        s.current_color_pso = col_pso;
      }
    }

    // The instanced twin: the same state with the vertex shader's node
    // constants redirected into the instance record (kSpecConstantInstanced),
    // for vertex shaders the recompiler marked as carrying the redirect.
    // Built whether or not this particular draw defers, so a later deferred
    // draw on a clean pipeline state still has it. The draw queue merges
    // draws that share it, a mesh and a material into one instanced draw.
    s.current_instanced_pso = nullptr;
    s.current_pulled_pso = nullptr;
    if (REXCVAR_GET(bd_draw_instancing) && !s.current_prepass_pso &&
        lookup.vertexShader && lookup.vertexShader->shaderCacheEntry &&
        (lookup.vertexShader->shaderCacheEntry->specConstantsMask &
         kSpecConstantInstanced) &&
        InstanceRecordsReady()) {
      PipelineState inst = lookup;
      inst.specConstants |= kSpecConstantInstanced;
      SanitizePipelineState(inst);
      // Never built here: a twin the precache has not reached yet would
      // compile on the render thread (tens of milliseconds on Adreno, and the
      // Quest's frame breakdown showed the compiler inside the measurement
      // window). Ask the precache for it and draw plain until it exists.
      s.current_instanced_pso = FindPipeline(inst);
      if (!s.current_instanced_pso)
        EnqueuePipelinePriority(inst);
      // The pulled twin: the instanced state with the vertex shader pulling
      // its attributes from the record's streams (SPEC_CONSTANT_PULLED) and
      // the dummy input layout (gpu/vertex_pull.h). Same rule: never built
      // on the render thread.
      if (s.current_instanced_pso && REXCVAR_GET(bd_draw_pull) &&
          (lookup.vertexShader->shaderCacheEntry->specConstantsMask &
           kSpecConstantPulled) &&
          VertexPullReady()) {
        PipelineState pulled = inst;
        pulled.specConstants |= kSpecConstantPulled;
        pulled.vertexDeclaration = VertexPullDummyDeclaration();
        std::memset(pulled.vertexStrides, 0, sizeof(pulled.vertexStrides));
        if (pulled.vertexDeclaration) {
          SanitizePipelineState(pulled);
          s.current_pulled_pso = FindPipeline(pulled);
          if (!s.current_pulled_pso) {
            EnqueuePipelinePriority(pulled);
            VertexPullNoteTwinMissing();
          }
        }
      }
    }
  } else if (!s.current_pso) {
    // Clean dirty bits but no PSO bound: the first draw after a command list
    // reset that lost the force-dirty.
    return false;
  }

  // The Set*ShaderConstant wrappers dirty-track these, so clean means the bound
  // constants are still live and the 4 KiB byte swap upload can be skipped.
  // Vulkan push offsets 0/8/16 follow the guest PushConstants member order
  // emitted by the recompiler.
  // Whether this draw's vertex constants travel in an instance record: only
  // a deferred draw with the instanced twin, and only while the frame has
  // room for the record. Such a draw never reads the uniform window, so the
  // window is left where it is and the guest's dirty flag is kept for the
  // next plain draw (below).
  // A host-issued node draw keeps to the ordinary window unless
  // bd_host_draw_records says otherwise: both configurations in which the
  // village's rock went missing routed replayed draws through records.
  const bool constants_in_record =
      s.deferring_draw && s.current_instanced_pso != nullptr &&
      InstanceRecordsRoom() &&
      (!bd::gpu::scene::HostDrawReplaying() || REXCVAR_GET(bd_host_draw_records));
  u32 record_index = ~0u;
  bool pull_ok = false;
  bool vs_upload_kept = false;
  if (device_guest) {
    if (constants_in_record) {
      if (s.dirtyStates.vertexShaderConstants) {
        SnapshotVertexShaderConstants(device_guest);
        vs_upload_kept = true;
      }
    } else if (s.dirtyStates.vertexShaderConstants) {
      const u32 *mask =
          (s.pipelineState.vertexShader &&
           s.pipelineState.vertexShader->shaderCacheEntry)
              ? s.pipelineState.vertexShader->shaderCacheEntry
                    ->constantRegisterMask
              : nullptr;
      auto vs_alloc =
          UploadVertexShaderConstants(device_guest, 0.0f, 0.0f, mask);
      if (vs_alloc.failed)
        return false;
      if (vs_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(vs_alloc.ref, 0);
#else
        s.constant_dyn_offsets[0] = vs_alloc.dynamicOffset;
#endif
      }
    }

    if (s.dirtyStates.pixelShaderConstants) {
      auto ps_alloc = UploadPixelShaderConstants(
          device_guest,
          (s.pipelineState.pixelShader &&
           s.pipelineState.pixelShader->shaderCacheEntry)
              ? s.pipelineState.pixelShader->shaderCacheEntry
                    ->constantRegisterMask
              : nullptr);
      if (ps_alloc.failed)
        return false;
      if (ps_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(ps_alloc.ref, 1);
#else
        s.constant_dyn_offsets[1] = ps_alloc.dynamicOffset;
#endif
      }
    }

    // SharedConstants rebuilds from live guest state every draw: the sampler
    // fetch constants are written by unhooked recompiled code, so there is no
    // dirty signal. The upload is skipped internally when the built block is
    // byte-identical to the one already bound on this list.
    auto sc_alloc = UploadSharedConstants(device_guest);
    if (sc_alloc.failed)
      return false;
    if (sc_alloc.size) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(sc_alloc.ref, 2);
#else
      s.constant_dyn_offsets[2] = sc_alloc.dynamicOffset;
#endif
    }
    // After the snapshot, which is what puts this draw's block in the
    // scratch the record is copied from.
    if (constants_in_record) {
      record_index = StageInstanceRecord();
      pull_ok = VertexPullStage(record_index, s);
    }
    // One bind carries all three blocks, after every upload that could have
    // moved one. The VS and PS uploads are dirty-gated, so their offsets often
    // carry over unchanged from the previous draw.
    BindGuestConstants(s);
  }

  // Lens flare occlusion count: the counter UAV (root descriptor 3 on D3D12,
  // set 4 on Vulkan) for the sun test quad draw bracketed by D3DQuery_Issue
  // BEGIN/END. The pipeline cache swaps occlusion_count_ps in while counting.
  if (s.occlusion_counting) {
    const u32 slot = s.frame.load(std::memory_order_relaxed);
    if (s.occlusion_counter[slot]) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(
          s.occlusion_counter[slot]->at(0), 3);
#else
      s.command_list->setGraphicsDescriptorSet(
          s.occlusion_descriptor_set[slot].get(), kOcclusionDescriptorSetIndex);
#endif
    }
  }

  // Clean state is first=255, last=0, so 'first <= last' skips the call when
  // nothing changed. BeginCommandList force-dirties the full range every
  // command list reset: D3D12 IA bindings do not survive begin().
  if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
    const u32 first = s.dirtyStates.vertexStreamFirst;
    const u32 count = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
    // The union, not the latest range. The immediate path binds only what
    // changed, so the binding a draw actually sees is everything bound since
    // the command list began - which is what a deferred draw has to replay.
    const u32 last = first + count - 1u;
    if (s.bound_vertex_count == 0) {
      s.bound_vertex_first = first;
      s.bound_vertex_count = count;
    } else {
      const u32 lo = std::min(s.bound_vertex_first, first);
      const u32 hi = std::max(s.bound_vertex_first + s.bound_vertex_count - 1u,
                              last);
      s.bound_vertex_first = lo;
      s.bound_vertex_count = hi - lo + 1u;
    }
    if (!s.deferring_draw) {
      s.command_list->setVertexBuffers(first, s.vertex_views + first, count,
                                       s.input_slots + first);
    }
  }

  // Re-binds only when SetIndices changed buffer/size/format, or
  // BeginCommandList force-dirtied after a command list reset.
  if (s.deferring_draw) {
    // Unconditional, unlike the immediate path: the dirty flag says "changed
    // since the last draw", which is meaningless once draws are reordered.
    s.pending.index_view = s.index_view;
    s.pending.has_index_buffer = s.index_view.buffer.ref != nullptr;
  } else if (s.dirtyStates.indices && s.index_view.buffer.ref != nullptr) {
    s.command_list->setIndexBuffer(&s.index_view);
  }

  if (s.deferring_draw) {
    // The complete binding, not the delta. Copied by value because the guest
    // overwrites its own views between draws and a queued draw is replayed
    // long after that.
    s.pending.pipeline = s.current_pso;
    s.pending.ps_hash = (s.pixel_shader && s.pixel_shader->shaderCacheEntry)
                            ? s.pixel_shader->shaderCacheEntry->hash
                            : 0ull;
    s.pending.prepass_pipeline = s.current_prepass_pso;
    s.pending.color_pipeline = s.current_color_pso;
    s.pending.instanced_pipeline =
        record_index != ~0u ? s.current_instanced_pso : nullptr;
    s.pending.record_index = record_index;
    s.pending.pulled_pipeline =
        (record_index != ~0u && pull_ok) ? s.current_pulled_pso : nullptr;
    const u32 first = s.bound_vertex_first;
    const u32 count = s.bound_vertex_count;
    for (u32 i = first; i < first + count && i < 16u; ++i) {
      s.pending.vertex_views[i] = s.vertex_views[i];
      s.pending.input_slots[i] = s.input_slots[i];
    }
    s.pending.vertex_first = first;
    s.pending.vertex_count = count;
  }

  s.dirtyStates = DirtyStates(false);
  // The record took this draw's vertex block; the uniform window still holds
  // an older one, and the next plain draw has to upload.
  if (vs_upload_kept)
    s.dirtyStates.vertexShaderConstants = true;
  return true;
}

} // namespace bd::gpu

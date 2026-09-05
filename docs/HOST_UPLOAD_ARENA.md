# Host upload pages

`gpu/host_upload` stages host resource bytes independently of the retained
shader-register compatibility buffer. Native texture uploads call
`UploadHostData` directly, as does the native ImGui overlay; temporary guest
texture/UP-vertex adapters forward
to the same arena. A host upload carries a mapped CPU destination and ordinary
GPU buffer reference, never a guest address or uniform-buffer dynamic offset.

## Lifetime and limits

- Allocation and command recording are serialized by the renderer lock, with
  an open command list. Raw mapped destinations are write-only staging, not
  CPU source data. Callers keep any CPU data needed later separately.
- Each recording slot owns independent pages. New writes never wrap or replace
  an earlier allocation in that slot. Multiple subresources may be staged before
  their copy commands are recorded without clobbering earlier copy sources.
- `AdvanceAndWaitReused` waits for the slot's submitted command fence, then
  `ResetFrame` calls `ResetHostUploadsAfterFence`. No other runtime path rewinds
  or retires these pages. A different slot's completion cannot reclaim them.
- Ordinary pages are 4 MiB. A larger request gets a dedicated page, limited to
  64 MiB per allocation. All pages, including in-flight and retained pages,
  count against one 256 MiB payload budget across slots. Driver allocation
  metadata/alignment are outside this accounting; this is not the entire
  renderer budget or qualification of the 1.5 GB headset asset target.
- Large pages retire after the fence covering their last use. Ordinary pages
  are reusable after that fence; a whole idle slot cycle releases unused pages.
  Allocation/budget/map failure does not move existing offsets or destroy pages.
  Failure is reported and the affected import is refused.
- Rewinding a page scrubs raw vertex/index bindings; retiring it also forgets
  its vertex-pulling heap entry before destruction. Pages declare storage-buffer
  usage for vertex pulling. Cross-frame draw recipes cannot retain a transient
  upload stream: those draws still need a native dynamic-geometry producer.

The standard-library-only `UploadPageArena` helper implements the same policy
used by the renderer. `host_upload_pages` in `tools/native_texture_test` checks
alignment/overflow, non-overlap, per-slot reuse and retirement, budget and
factory failures, a 160 MiB burst with every byte verified, and one 64 MiB image.
It also verifies invalidation callbacks precede destruction and distinguish
rewinding from retirement, and that retired resources lose transient identity.
It models the fence boundary; it does not simulate a Vulkan driver or prove
that arbitrary callers obey the locking/lifetime contract.

## Remaining shader compatibility buffer

The fixed 32 MiB-per-slot buffer now contains shader constants only. Its bounds
are checked with the same wide-arithmetic range helper and it never wraps.
`ConstantAllocation::failed` distinguishes allocation refusal from a successful
content-cache hit (`size == 0` with `failed == false`). Immediate draws, the
legacy eye loop and deferred masked-record groups reject failed constant uploads
rather than bind stale values. Record-budget fallback uploads the node's own
block before a plain draw. Host passes reserve/initialize the full descriptor
window even when their payload is smaller.

This refusal policy is a visible correctness failure, not a completed frame or
a performance optimization. Full native material, frame/scene uniform layouts
and asset streaming/backpressure remain required. The host staging path does
not remove guest asset discovery, translated shader ABI, EDRAM or frame/pass
producers. See the [transition scope](HOST_RENDERER_TRANSITION.md).

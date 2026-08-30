/**
 * @file    gpu/renderdoc_capture.h
 * @brief   Drives RenderDoc's in-application API from a cvar.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

namespace bd::gpu::renderdoc {

// Loads the RenderDoc module if bd_renderdoc is set. Must run before the
// graphics API is initialised - RenderDoc hooks at load time and a module
// loaded after the VkInstance exists captures nothing.
void LoadIfRequested();

// Called once per present. Triggers a capture on the first frame at or after
// bd_renderdoc_after_s. RenderDoc delimits frames by present, so the capture
// covers the whole of the next frame.
void TriggerIfDue();

// True once a capture has been written, so a headless run can exit rather than
// sit for its remaining settle time.
bool CaptureWritten();

} // namespace bd::gpu::renderdoc

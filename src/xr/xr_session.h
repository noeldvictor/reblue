/**
 * @file    xr/xr_session.h
 * @brief   OpenXR instance, session, reference space, and per-eye views.
 *
 * Split into two phases on purpose, because OpenXR and the renderer have a
 * chicken-and-egg problem: the runtime dictates which Vulkan instance
 * extensions, device extensions and physical device must be used, but it can
 * only be asked once an XrInstance exists, and the session cannot be created
 * until the Vulkan device does.
 *
 *   1. CreateInstance()      - no Vulkan yet. Afterwards the Vulkan*() getters
 *                              describe what the renderer is obliged to do.
 *   2. CreateSession(...)    - hand back the device that was built to those
 *                              requirements.
 *
 * plume creates the Vulkan device, which is why patches/plume-openxr-seam.patch
 * exists: it lets the answers from phase 1 be threaded into that creation.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <string>
#include <vector>

#include <rex/types.h>

#include "xr/xr_math.h"

// Forward-declared rather than including vulkan.h and openxr.h here: this
// header is reached from files that have no business seeing either.
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;

namespace bd::xr {

// One eye's pose and field of view for the frame being rendered, already
// converted out of OpenXR's coordinate system.
struct EyeView {
  Pose pose;
  Fov fov;
};

// What xrWaitFrame said about the frame we are about to render.
struct FrameState {
  i64 predictedDisplayTime = 0;
  bool shouldRender = false;
  // Views are only valid when shouldRender is true and locating succeeded.
  EyeView views[2];
  u32 viewCount = 0;
};

class Session {
public:
  static Session &Get();

  // --- phase 1: before the Vulkan device exists ---

  // Creates the XrInstance and picks a system. Returns false if no runtime is
  // present, which is the normal case on a desktop with no headset and must not
  // be treated as an error - the caller falls back to the flat renderer.
  bool CreateInstance();

  bool InstanceCreated() const { return instance_ != nullptr; }

  // What the runtime requires of Vulkan. Only meaningful after CreateInstance.
  const std::vector<std::string> &VulkanInstanceExtensions() const {
    return vulkanInstanceExtensions_;
  }
  const std::vector<std::string> &VulkanDeviceExtensions() const {
    return vulkanDeviceExtensions_;
  }
  // The runtime names the adapter it can present from. On a laptop whose
  // headset is wired to the integrated GPU this will not be the fastest one,
  // and using anything else fails session creation.
  VkPhysicalDevice_T *VulkanPhysicalDevice(VkInstance_T *instance) const;
  // Minimum Vulkan API version the runtime supports, as VK_MAKE_API_VERSION.
  u32 VulkanMinApiVersion() const { return vulkanMinApiVersion_; }

  // --- phase 2: once the device exists ---

  bool CreateSession(VkInstance_T *instance, VkPhysicalDevice_T *physicalDevice,
                     VkDevice_T *device, u32 queueFamilyIndex, u32 queueIndex);

  bool SessionCreated() const { return session_ != nullptr; }

  // Pumps the event queue and tracks session state. Call once per frame before
  // BeginFrame; returns false when the runtime has asked us to exit.
  bool PollEvents();

  // True between xrBeginSession and xrEndSession. While false, do not render -
  // just keep polling, or the runtime will never bring the session up.
  bool Running() const { return running_; }

  // xrWaitFrame + xrBeginFrame, then locates the eye views. A false return
  // means the frame must be skipped entirely, not rendered blank.
  bool BeginFrame(FrameState &out);

  // xrEndFrame. Must be called for every BeginFrame that returned true, even
  // when nothing was drawn, or the runtime's frame pacing stalls.
  void EndFrame(const FrameState &state);

  // --- swapchain and layer submission ---

  // One colour swapchain the size of the game's own output. The first layer
  // this port submits is a quad - a world-locked screen - rather than a stereo
  // projection: it is genuinely immersive, it needs no per-eye rendering, and
  // it is the mode most likely to be comfortable for a fixed-camera JRPG. The
  // projection path comes later and reuses all of this.
  bool CreateSwapchain(u32 width, u32 height);

  // The runtime's images, as raw VkImage handles for the GPU layer to wrap.
  u32 SwapchainImageCount() const { return static_cast<u32>(swapchainImages_.size()); }
  void *SwapchainImage(u32 index) const;
  u32 SwapchainWidth() const { return swapchainWidth_; }
  u32 SwapchainHeight() const { return swapchainHeight_; }
  // Colour format the runtime chose, as a VkFormat.
  i64 SwapchainFormat() const { return swapchainFormat_; }

  // Acquire/wait the next image. Returns its index, or -1 if unavailable.
  i32 AcquireSwapchainImage();
  void ReleaseSwapchainImage();

  // Queues the quad for this frame's EndFrame. Size is in metres.
  void SubmitQuadLayer(f32 widthMetres, f32 heightMetres, f32 distanceMetres);

  // Places the screen in front of wherever the player was looking on the first
  // frame, once. Call after BeginFrame; does nothing thereafter.
  void AnchorQuad(const FrameState &state);

  // --- input ---

  // Creates the gameplay action set, suggests Touch bindings and
  // attaches it. Called from CreateSession; failure is not fatal.
  bool CreateActions();

  // xrSyncActions, then publishes the result through xr_pad.h for
  // the input driver. Called once per frame from BeginFrame.
  void SyncActions();

  void Destroy();

  // Recommended per-eye render target size, from the view configuration.
  u32 RecommendedWidth() const { return recommendedWidth_; }
  u32 RecommendedHeight() const { return recommendedHeight_; }

private:
  Session() = default;

  // Opaque handles, so this header stays free of openxr.h.
  void *instance_ = nullptr;
  void *session_ = nullptr;
  void *appSpace_ = nullptr;
  u64 systemId_ = 0;

  std::vector<std::string> vulkanInstanceExtensions_;
  std::vector<std::string> vulkanDeviceExtensions_;
  u32 vulkanMinApiVersion_ = 0;

  u32 recommendedWidth_ = 0;
  u32 recommendedHeight_ = 0;

  void *swapchain_ = nullptr;
  std::vector<void *> swapchainImages_;
  u32 swapchainWidth_ = 0;
  u32 swapchainHeight_ = 0;
  i64 swapchainFormat_ = 0;
  bool quadQueued_ = false;
  bool quadAnchored_ = false;
  struct { f32 x, y, z; } quadAnchorPosition_{0.0f, 0.0f, -2.0f};
  struct { f32 x, y, z, w; } quadAnchorOrientation_{0.0f, 0.0f, 0.0f, 1.0f};
  f32 quadWidth_ = 0.0f;
  f32 quadHeight_ = 0.0f;
  f32 quadDistance_ = 0.0f;

  bool running_ = false;
  bool exitRequested_ = false;
  // xrEndFrame needs the display time even on a frame that drew nothing.
  i64 frameDisplayTime_ = 0;
  bool frameBegun_ = false;
};

} // namespace bd::xr

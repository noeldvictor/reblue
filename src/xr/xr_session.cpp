/**
 * @file    xr/xr_session.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_session.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#define XR_USE_PLATFORM_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/logging.h"

namespace bd::xr {

namespace {

XrInstance AsInstance(void *p) { return static_cast<XrInstance>(p); }
XrSession AsSession(void *p) { return static_cast<XrSession>(p); }
XrSpace AsSpace(void *p) { return static_cast<XrSpace>(p); }

// The runtime hands extension lists back as one space-separated string.
std::vector<std::string> SplitExtensions(const std::string &packed) {
  std::vector<std::string> out;
  std::istringstream stream(packed);
  std::string name;
  while (stream >> name)
    out.push_back(name);
  return out;
}

bool Failed(XrResult result, const char *what) {
  if (XR_SUCCEEDED(result))
    return false;
  BD_ERROR("OpenXR: {} failed ({})", what, static_cast<int>(result));
  return true;
}

Pose FromXrPose(const XrPosef &p) {
  // Straight into game space; xr_math owns the mirror-on-Z.
  return FromOpenXRPose(Pose{{p.position.x, p.position.y, p.position.z},
                             {p.orientation.x, p.orientation.y, p.orientation.z,
                              p.orientation.w}});
}

constexpr XrViewConfigurationType kViewConfig =
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

} // namespace

Session &Session::Get() {
  static Session instance;
  return instance;
}

bool Session::CreateInstance() {
#if defined(__ANDROID__)
  // On Android the loader has to be handed the JavaVM and the Activity before
  // anything else - it reaches the runtime through a content provider and
  // cannot do that without a JNI context. Skipping this is not a soft failure:
  // xrCreateInstance returns XR_ERROR_INITIALIZATION_FAILED (-6) and there is
  // nothing in the message to suggest why.
  PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
  xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
  if (!xrInitializeLoaderKHR) {
    BD_INFO("OpenXR: loader has no xrInitializeLoaderKHR; no runtime here");
    return false;
  }

  JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  void *activity = SDL_GetAndroidActivity();
  JavaVM *vm = nullptr;
  if (env)
    env->GetJavaVM(&vm);
  if (!vm || !activity) {
    BD_INFO("OpenXR: no JNI context from SDL; staying on the flat renderer");
    return false;
  }

  XrLoaderInitInfoAndroidKHR loader_init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  loader_init.applicationVM = vm;
  loader_init.applicationContext = activity;
  if (Failed(xrInitializeLoaderKHR(
                 reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init)),
             "xrInitializeLoaderKHR"))
    return false;
#endif

  // XR_KHR_vulkan_enable rather than enable2: the older extension lets the
  // application create its own VkInstance and VkDevice, which is what plume
  // does. enable2 wants to create them for us, and plume has no seam for that.
  const char *extensions[] = {
      XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
#if defined(__ANDROID__)
      XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
#endif
  };

  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
  create.enabledExtensionNames = extensions;

#if defined(__ANDROID__)
  // The runtime wants the same pair again on the instance itself.
  XrInstanceCreateInfoAndroidKHR android_create{
      XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android_create.applicationVM = vm;
  android_create.applicationActivity = activity;
  create.next = &android_create;
#endif
  // snprintf rather than strncpy: these are fixed-size char arrays and MSVC
  // deprecates strncpy, so this keeps the build warning-free on every target.
  std::snprintf(create.applicationInfo.applicationName,
                XR_MAX_APPLICATION_NAME_SIZE, "%s", "re:Blue");
  create.applicationInfo.applicationVersion = 1;
  std::snprintf(create.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE,
                "%s", "ReXGlue");
  create.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

  XrInstance instance = XR_NULL_HANDLE;
  XrResult result = xrCreateInstance(&create, &instance);
  if (XR_FAILED(result)) {
    // Not an error. No runtime installed is the ordinary desktop case, and the
    // caller falls back to the flat renderer.
    BD_INFO("OpenXR: no usable runtime ({}), staying on the flat renderer",
            static_cast<int>(result));
    return false;
  }
  instance_ = instance;

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId systemId = XR_NULL_SYSTEM_ID;
  if (Failed(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem")) {
    Destroy();
    return false;
  }
  systemId_ = systemId;

  // Per-eye render target size, which the swapchain and the framebuffer sizing
  // both need.
  uint32_t viewCount = 0;
  xrEnumerateViewConfigurationViews(instance, systemId, kViewConfig, 0,
                                    &viewCount, nullptr);
  std::vector<XrViewConfigurationView> views(
      viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (viewCount) {
    xrEnumerateViewConfigurationViews(instance, systemId, kViewConfig, viewCount,
                                      &viewCount, views.data());
    recommendedWidth_ = views[0].recommendedImageRectWidth;
    recommendedHeight_ = views[0].recommendedImageRectHeight;
  }

  // What the runtime demands of Vulkan. These feed
  // plume::VulkanInterfaceOptions; see patches/plume-openxr-seam.patch.
  PFN_xrGetVulkanGraphicsRequirementsKHR getRequirements = nullptr;
  PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions = nullptr;
  PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
  xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements));
  xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&getInstanceExtensions));
  xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&getDeviceExtensions));

  if (getRequirements) {
    XrGraphicsRequirementsVulkanKHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (XR_SUCCEEDED(getRequirements(instance, systemId, &requirements))) {
      // XrVersion packs major/minor into the high bits; Vulkan wants its own
      // encoding, so convert rather than passing it through.
      vulkanMinApiVersion_ = VK_MAKE_API_VERSION(
          0, static_cast<uint32_t>(XR_VERSION_MAJOR(requirements.minApiVersionSupported)),
          static_cast<uint32_t>(XR_VERSION_MINOR(requirements.minApiVersionSupported)), 0);
    }
  }

  auto fetch = [](auto fn, XrInstance inst, XrSystemId sys,
                  std::vector<std::string> &out) {
    if (!fn)
      return;
    uint32_t size = 0;
    if (XR_FAILED(fn(inst, sys, 0, &size, nullptr)) || size == 0)
      return;
    std::string packed(size, '\0');
    if (XR_FAILED(fn(inst, sys, size, &size, packed.data())))
      return;
    packed.resize(std::strlen(packed.c_str()));
    out = SplitExtensions(packed);
  };
  fetch(getInstanceExtensions, instance, systemId, vulkanInstanceExtensions_);
  fetch(getDeviceExtensions, instance, systemId, vulkanDeviceExtensions_);

  BD_INFO("OpenXR: instance up, per-eye {}x{}, {} instance / {} device "
          "extensions required",
          recommendedWidth_, recommendedHeight_,
          vulkanInstanceExtensions_.size(), vulkanDeviceExtensions_.size());
  return true;
}

VkPhysicalDevice_T *Session::VulkanPhysicalDevice(VkInstance_T *instance) const {
  if (!instance_ || !instance)
    return nullptr;
  PFN_xrGetVulkanGraphicsDeviceKHR getDevice = nullptr;
  xrGetInstanceProcAddr(AsInstance(instance_), "xrGetVulkanGraphicsDeviceKHR",
                        reinterpret_cast<PFN_xrVoidFunction *>(&getDevice));
  if (!getDevice)
    return nullptr;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  if (XR_FAILED(getDevice(AsInstance(instance_), systemId_,
                          reinterpret_cast<VkInstance>(instance),
                          &physicalDevice)))
    return nullptr;
  return reinterpret_cast<VkPhysicalDevice_T *>(physicalDevice);
}

bool Session::CreateSession(VkInstance_T *instance,
                            VkPhysicalDevice_T *physicalDevice,
                            VkDevice_T *device, u32 queueFamilyIndex,
                            u32 queueIndex) {
  if (!instance_)
    return false;

  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.instance = reinterpret_cast<VkInstance>(instance);
  binding.physicalDevice = reinterpret_cast<VkPhysicalDevice>(physicalDevice);
  binding.device = reinterpret_cast<VkDevice>(device);
  binding.queueFamilyIndex = queueFamilyIndex;
  binding.queueIndex = queueIndex;

  XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
  create.next = &binding;
  create.systemId = systemId_;

  XrSession session = XR_NULL_HANDLE;
  if (Failed(xrCreateSession(AsInstance(instance_), &create, &session),
             "xrCreateSession"))
    return false;
  session_ = session;

  // STAGE would put the origin on the floor, which is what a room-scale game
  // wants. LOCAL follows the headset's own recentre and is the safer default
  // for a seated JRPG; bd_vr_eye_height covers the difference.
  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
  XrSpace space = XR_NULL_HANDLE;
  if (Failed(xrCreateReferenceSpace(session, &spaceInfo, &space),
             "xrCreateReferenceSpace")) {
    Destroy();
    return false;
  }
  appSpace_ = space;

  BD_INFO("OpenXR: session created");
  return true;
}

bool Session::PollEvents() {
  if (!instance_)
    return false;

  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (true) {
    event = {XR_TYPE_EVENT_DATA_BUFFER};
    if (xrPollEvent(AsInstance(instance_), &event) != XR_SUCCESS)
      break;

    switch (event.type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const auto &changed =
          *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
      switch (changed.state) {
      case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = kViewConfig;
        if (!Failed(xrBeginSession(AsSession(session_), &begin),
                    "xrBeginSession"))
          running_ = true;
        break;
      }
      case XR_SESSION_STATE_STOPPING:
        running_ = false;
        xrEndSession(AsSession(session_));
        break;
      case XR_SESSION_STATE_EXITING:
      case XR_SESSION_STATE_LOSS_PENDING:
        running_ = false;
        exitRequested_ = true;
        break;
      default:
        break;
      }
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      exitRequested_ = true;
      break;
    default:
      break;
    }
  }
  return !exitRequested_;
}

bool Session::BeginFrame(FrameState &out) {
  out = FrameState{};
  if (!running_ || !session_)
    return false;

  XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frameState{XR_TYPE_FRAME_STATE};
  if (Failed(xrWaitFrame(AsSession(session_), &waitInfo, &frameState),
             "xrWaitFrame"))
    return false;

  if (Failed(xrBeginFrame(AsSession(session_), nullptr), "xrBeginFrame"))
    return false;
  frameBegun_ = true;
  frameDisplayTime_ = frameState.predictedDisplayTime;

  out.predictedDisplayTime = frameState.predictedDisplayTime;
  out.shouldRender = frameState.shouldRender == XR_TRUE;
  if (!out.shouldRender)
    return true; // still has to reach EndFrame

  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
  locate.viewConfigurationType = kViewConfig;
  locate.displayTime = frameState.predictedDisplayTime;
  locate.space = AsSpace(appSpace_);

  XrViewState viewState{XR_TYPE_VIEW_STATE};
  uint32_t count = 0;
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  if (Failed(xrLocateViews(AsSession(session_), &locate, &viewState, 2, &count,
                           views),
             "xrLocateViews"))
    return true;

  // Tracking can drop out mid-session. Rendering with a stale pose is worse
  // than not rendering, so the frame goes through as a no-draw instead.
  const XrViewStateFlags needed =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if ((viewState.viewStateFlags & needed) != needed) {
    out.shouldRender = false;
    return true;
  }

  out.viewCount = count > 2 ? 2 : count;
  for (u32 i = 0; i < out.viewCount; ++i) {
    out.views[i].pose = FromXrPose(views[i].pose);
    out.views[i].fov = {views[i].fov.angleLeft, views[i].fov.angleRight,
                        views[i].fov.angleUp, views[i].fov.angleDown};
  }
  return true;
}

void Session::EndFrame(const FrameState &state) {
  if (!frameBegun_ || !session_)
    return;
  frameBegun_ = false;

  // A quad layer when one was queued this frame, otherwise none. Submitting
  // zero layers is legal and keeps the runtime's frame pacing alive, which is
  // what stops the session from being torn down as unresponsive.
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader *layers[1] = {nullptr};
  uint32_t layerCount = 0;

  if (quadQueued_ && swapchain_) {
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = AsSpace(appSpace_);
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = static_cast<XrSwapchain>(swapchain_);
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {static_cast<int32_t>(swapchainWidth_),
                                      static_cast<int32_t>(swapchainHeight_)};
    quad.subImage.imageArrayIndex = 0;
    // Straight ahead in the reference space, at eye height. -Z is forward in
    // OpenXR, which is why the distance is negative.
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = {0.0f, 0.0f, -quadDistance_};
    quad.size = {quadWidth_, quadHeight_};
    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
    layerCount = 1;
  }
  quadQueued_ = false;

  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = state.predictedDisplayTime ? state.predictedDisplayTime
                                               : frameDisplayTime_;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = layerCount;
  end.layers = layerCount ? layers : nullptr;
  xrEndFrame(AsSession(session_), &end);
}

bool Session::CreateSwapchain(u32 width, u32 height) {
  if (!session_ || swapchain_)
    return swapchain_ != nullptr;

  // Take the first format the runtime offers that we know how to present.
  // Quest offers sRGB first, which is what it wants us to use.
  uint32_t formatCount = 0;
  xrEnumerateSwapchainFormats(AsSession(session_), 0, &formatCount, nullptr);
  std::vector<int64_t> formats(formatCount);
  if (formatCount)
    xrEnumerateSwapchainFormats(AsSession(session_), formatCount, &formatCount,
                                formats.data());

  int64_t chosen = 0;
  for (int64_t format : formats) {
    if (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = format;
      break;
    }
  }
  if (!chosen && !formats.empty())
    chosen = formats[0];
  if (!chosen) {
    BD_ERROR("OpenXR: runtime offered no swapchain formats");
    return false;
  }

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format = chosen;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  if (Failed(xrCreateSwapchain(AsSession(session_), &info, &swapchain),
             "xrCreateSwapchain"))
    return false;
  swapchain_ = swapchain;
  swapchainWidth_ = width;
  swapchainHeight_ = height;
  swapchainFormat_ = chosen;

  uint32_t imageCount = 0;
  xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
  std::vector<XrSwapchainImageVulkanKHR> images(
      imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if (Failed(xrEnumerateSwapchainImages(
                 swapchain, imageCount, &imageCount,
                 reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
             "xrEnumerateSwapchainImages"))
    return false;

  swapchainImages_.clear();
  for (const auto &image : images)
    swapchainImages_.push_back(reinterpret_cast<void *>(image.image));

  BD_INFO("OpenXR: swapchain {}x{} format {} with {} images", width, height,
          chosen, swapchainImages_.size());
  return true;
}

void *Session::SwapchainImage(u32 index) const {
  return index < swapchainImages_.size() ? swapchainImages_[index] : nullptr;
}

i32 Session::AcquireSwapchainImage() {
  if (!swapchain_)
    return -1;
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(static_cast<XrSwapchain>(swapchain_),
                                        &acquire, &index)))
    return -1;

  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(static_cast<XrSwapchain>(swapchain_), &wait)))
    return -1;
  return static_cast<i32>(index);
}

void Session::ReleaseSwapchainImage() {
  if (!swapchain_)
    return;
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(static_cast<XrSwapchain>(swapchain_), &release);
}

void Session::SubmitQuadLayer(f32 widthMetres, f32 heightMetres,
                              f32 distanceMetres) {
  quadQueued_ = true;
  quadWidth_ = widthMetres;
  quadHeight_ = heightMetres;
  quadDistance_ = distanceMetres;
}

void Session::Destroy() {
  if (swapchain_) {
    xrDestroySwapchain(static_cast<XrSwapchain>(swapchain_));
    swapchain_ = nullptr;
    swapchainImages_.clear();
  }
  if (appSpace_) {
    xrDestroySpace(AsSpace(appSpace_));
    appSpace_ = nullptr;
  }
  if (session_) {
    xrDestroySession(AsSession(session_));
    session_ = nullptr;
  }
  if (instance_) {
    xrDestroyInstance(AsInstance(instance_));
    instance_ = nullptr;
  }
  running_ = false;
}

} // namespace bd::xr

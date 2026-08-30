/**
 * @file    xr/xr_session.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include <vector>
#include "xr/xr_session.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
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
#include "core/sampling_profiler.h"
#include "core/threading.h"
#include "xr/xr_game_camera.h"
#include "xr/xr_pad.h"

REXCVAR_DECLARE(bool, bd_stereo);
REXCVAR_DECLARE(f64, bd_xr_refresh_rate);

#if defined(__ANDROID__)
#include <unistd.h>
#endif

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

// Action handles. File-static rather than Session members because Session is a
// singleton, and this keeps every XR type out of xr_session.h - which is what
// lets the rest of the port include that header freely.
XrActionSet g_actionSet = XR_NULL_HANDLE;

struct Actions {
  XrAction a = XR_NULL_HANDLE;
  XrAction b = XR_NULL_HANDLE;
  XrAction x = XR_NULL_HANDLE;
  XrAction y = XR_NULL_HANDLE;
  XrAction menu = XR_NULL_HANDLE;
  XrAction leftClick = XR_NULL_HANDLE;
  XrAction rightClick = XR_NULL_HANDLE;
  XrAction leftStick = XR_NULL_HANDLE;
  XrAction rightStick = XR_NULL_HANDLE;
  XrAction leftTrigger = XR_NULL_HANDLE;
  XrAction rightTrigger = XR_NULL_HANDLE;
  XrAction leftGrip = XR_NULL_HANDLE;
  XrAction rightGrip = XR_NULL_HANDLE;
};

Actions g_act;

// The head pose and frustum this frame's projection layer claims. Kept raw and
// file-static for the same reason the actions are: no XR types in the header.
XrPosef g_headPose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
XrFovf g_layerFov{};
bool g_projectionQueued = false;

// The localized name is what a runtime shows in its own rebinding UI, and
// xrCreateAction rejects an empty one, so both names are always supplied.
bool MakeAction(XrActionSet set, XrActionType type, const char *name,
                const char *label, XrAction &out) {
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  info.actionType = type;
  std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
  std::snprintf(info.localizedActionName, sizeof(info.localizedActionName),
                "%s", label);
  return !Failed(xrCreateAction(set, &info, &out), name);
}

bool ReadBool(XrSession session, XrAction action) {
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
  get.action = action;
  XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
  if (XR_FAILED(xrGetActionStateBoolean(session, &get, &state)))
    return false;
  return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
}

f32 ReadFloat(XrSession session, XrAction action) {
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
  get.action = action;
  XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
  if (XR_FAILED(xrGetActionStateFloat(session, &get, &state)))
    return 0.0f;
  return state.isActive == XR_TRUE ? state.currentState : 0.0f;
}

void ReadStick(XrSession session, XrAction action, f32 &outX, f32 &outY) {
  outX = outY = 0.0f;
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
  get.action = action;
  XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_FAILED(xrGetActionStateVector2f(session, &get, &state)) ||
      state.isActive != XR_TRUE)
    return;
  outX = state.currentState.x;
  outY = state.currentState.y;
}

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
  // Required. An unsupported name here fails xrCreateInstance outright, which
  // is why the optional one below is checked first rather than simply listed:
  // adding it unconditionally took the whole session down on a runtime that
  // does not advertise it, and the app then died before it could log why.
  std::vector<const char *> extensions = {
      XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
#if defined(__ANDROID__)
      XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
#endif
  };

  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  // Optional, and appended rather than probed: on Android the loader must be
  // initialised before any xr call, so enumerating extensions here - before
  // xrCreateInstance - left the app launching to a splash screen with a 4MB
  // heap, no guest and no log at all. Asking for it and retrying without on
  // failure needs no call before instance creation.
  //
  // What it buys: the app can choose the display rate instead of inheriting the
  // system's and being paced to a submultiple. A Quest 2 runs 72Hz, so a frame
  // that cannot hold 13.9ms is paced to 27.8 or 41.7 - and this port measured a
  // 41.6ms frame around 20.6ms of work, a whole tier wasted. At 60Hz the tiers
  // are 16.7/33.3/50 and that work lands on 33.3ms, which is 30fps: the rate
  // Blue Dragon originally ran at.
  const size_t requiredExtensions = extensions.size();
  if (REXCVAR_GET(bd_xr_refresh_rate) > 0.0)
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  // Without these the runtime leaves the app on its default power profile and
  // paces it into a low tier no matter how little it draws: measured here at
  // 13fps with the whole scene culled away, big cores at 77% of their ceiling
  // and the prime core at 38%. They are the standard Quest setup and the port
  // had neither.
  extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
#if defined(__ANDROID__)
  extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#endif

  create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  create.enabledExtensionNames = extensions.data();

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
  if (XR_FAILED(result) && extensions.size() > requiredExtensions) {
    // The optional display-rate extension is not available here. Drop it and
    // retry rather than losing the whole session over a pacing preference.
    BD_INFO("OpenXR: no display refresh rate extension, continuing without it");
    extensions.resize(requiredExtensions);
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();
    result = xrCreateInstance(&create, &instance);
  }
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
  ApplyPerformanceHints();

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

  // Not fatal: without input the game still renders, and saying so
  // beats failing the whole session over a controller.
  if (!CreateActions())
    BD_ERROR("OpenXR: input actions unavailable; guest has no pad");

  RequestDisplayRefreshRate();
  BD_INFO("OpenXR: session created");
  return true;
}

void Session::ApplyPerformanceHints() {
  PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
  if (XR_SUCCEEDED(xrGetInstanceProcAddr(
          AsInstance(instance_), "xrPerfSettingsSetPerformanceLevelEXT",
          reinterpret_cast<PFN_xrVoidFunction *>(&setLevel))) &&
      setLevel) {
    // SUSTAINED_HIGH rather than BOOST: BOOST is documented as a short-lived
    // burst for transitions and the runtime will pull it back, where a game
    // wants the highest level it can hold for the whole session.
    const XrResult cpu =
        setLevel(AsSession(session_), XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                 XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    const XrResult gpu =
        setLevel(AsSession(session_), XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                 XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    BD_INFO("[xr] performance level SUSTAINED_HIGH: cpu={} gpu={}",
            static_cast<int>(cpu), static_cast<int>(gpu));
  } else {
    BD_INFO("[xr] XR_EXT_performance_settings unavailable; the runtime keeps "
            "the app on its default power profile");
  }
}

void Session::RegisterThread(int type_raw) {
#if defined(__ANDROID__)
  PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(
          AsInstance(instance_), "xrSetAndroidApplicationThreadKHR",
          reinterpret_cast<PFN_xrVoidFunction *>(&setThread))) ||
      !setThread)
    return;
  // Telling the runtime which thread is which is what lets it put them on the
  // big cores and raise their priority. Left unsaid, the render thread competes
  // with whatever guest worker threads happen to be runnable.
  const uint32_t tid = static_cast<uint32_t>(gettid());
  const XrResult r = setThread(
      AsSession(session_), static_cast<XrAndroidThreadTypeKHR>(type_raw), tid);
  BD_INFO("[xr] registered thread {} as type {}: {}", tid, type_raw,
          static_cast<int>(r));
#else
  (void)type_raw;
#endif
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
  // Guest threads are created and retired as scenes change, so this is a
  // periodic sweep rather than a one-off. It only issues a syscall for a thread
  // whose mask is already wrong.
  static u32 policy_tick = 0;
  if ((policy_tick++ % 120u) == 0u)
    bd::ApplyThreadPolicy();

  bd::SamplingProfilerTick();

  static bool thread_registered = false;
  if (!thread_registered && session_) {
    thread_registered = true;
    // BeginFrame runs on the thread that drives the frame loop, so gettid()
    // here names the renderer without having to plumb it from elsewhere.
    RegisterThread(3 /* XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR */);
  }

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

  // Every frame, including ones the runtime says not to render: the
  // guest still polls its pad on those, and a stick frozen at its
  // last value is worse than a still image.
  SyncActions();

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

  // The head, midway between the eyes. A mono projection layer drawn from
  // either eye's own position would swing the whole world sideways every time
  // the player turned their head, because that eye orbits the neck.
  g_headPose = views[0].pose;
  if (out.viewCount == 2) {
    g_headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
    g_headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
    g_headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
  }

  // A symmetric frustum tall enough for the headset, then widened to the
  // render target's aspect. Taking the vertical from the device and deriving
  // the horizontal - rather than the other way round - is what keeps the image
  // from being stretched: the guest renders one aspect and the layer has to
  // claim that same one, or the compositor resamples a shape that was never
  // drawn.
  f32 halfV = 0.0f;
  for (u32 i = 0; i < out.viewCount; ++i) {
    halfV = std::max(halfV, std::fabs(views[i].fov.angleUp));
    halfV = std::max(halfV, std::fabs(views[i].fov.angleDown));
  }
  const u32 w = swapchainWidth_ ? swapchainWidth_ : recommendedWidth_;
  const u32 h = swapchainHeight_ ? swapchainHeight_ : recommendedHeight_;
  if (halfV > 0.0f && w && h) {
    const f32 aspect = static_cast<f32>(w) / static_cast<f32>(h);
    const f32 halfH = std::atan(std::tan(halfV) * aspect);
    g_layerFov.angleUp = halfV;
    g_layerFov.angleDown = -halfV;
    g_layerFov.angleRight = halfH;
    g_layerFov.angleLeft = -halfH;
    SetRenderFov(halfV, aspect);
  }
  return true;
}

void Session::EndFrame(const FrameState &state) {
  if (!frameBegun_ || !session_)
    return;
  frameBegun_ = false;
  // Cleared at the end of the frame they were queued for, so a frame that
  // queues nothing submits nothing rather than re-showing the last one.
  // Latched, because the flag is set during Present and has to survive being
  // reset for the next frame. Testing g_projectionQueued below instead of this
  // - which is what the code did, having cleared it on the line above and then
  // discarded this with a (void) cast - made the projection branch dead code.
  // Every VR frame went out as a quad layer, a flat screen in space, no matter
  // what the renderer put in it. It is why the projection layer had never been
  // seen to produce an image.
  const bool hadProjection = g_projectionQueued;
  g_projectionQueued = false;

  // A quad layer when one was queued this frame, otherwise none. Submitting
  // zero layers is legal and keeps the runtime's frame pacing alive, which is
  // what stops the session from being torn down as unresponsive.
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView projViews[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  const XrCompositionLayerBaseHeader *layers[1] = {nullptr};
  uint32_t layerCount = 0;

  if (hadProjection && swapchain_) {
    // This is the seam where stereo either reaches the headset or does not.
    //
    // With bd_stereo the renderer draws both eyes side by side into one image:
    // every scene draw is submitted twice, into a left and a right half-width
    // viewport, with its own per-eye constants. So each projection view has to
    // take *its half* of that image. Handing both views the full rect - which
    // is what this did - shows the same picture to both eyes and no amount of
    // work in the renderer can make it stereo.
    //
    // Without bd_stereo the image is a single mono view and both eyes get all
    // of it. That is flat rather than wrong: an image drawn from the head and
    // shown to both eyes is comfortable, where an image drawn for one eye and
    // shown to both is a stereo mismatch.
    const bool sideBySide = REXCVAR_GET(bd_stereo);
    const int32_t fullW = static_cast<int32_t>(swapchainWidth_);
    const int32_t fullH = static_cast<int32_t>(swapchainHeight_);
    const int32_t eyeW = sideBySide ? fullW / 2 : fullW;
    for (u32 i = 0; i < 2; ++i) {
      projViews[i].pose = g_headPose;
      projViews[i].fov = g_layerFov;
      projViews[i].subImage.swapchain = static_cast<XrSwapchain>(swapchain_);
      projViews[i].subImage.imageRect.offset = {
          sideBySide ? static_cast<int32_t>(i) * eyeW : 0, 0};
      projViews[i].subImage.imageRect.extent = {eyeW, fullH};
      projViews[i].subImage.imageArrayIndex = 0;
    }
    // One-shot: what the compositor is actually handed per eye. This is the
    // only part of stereo that can be checked without wearing the headset -
    // two different offsets mean two different pictures reach the two eyes.
    {
      static bool logged = false;
      if (!logged) {
        logged = true;
        BD_INFO("[xr] projection views: eye0 rect {}x{}+{} eye1 rect {}x{}+{} "
                "(sideBySide={})",
                projViews[0].subImage.imageRect.extent.width,
                projViews[0].subImage.imageRect.extent.height,
                projViews[0].subImage.imageRect.offset.x,
                projViews[1].subImage.imageRect.extent.width,
                projViews[1].subImage.imageRect.extent.height,
                projViews[1].subImage.imageRect.offset.x, sideBySide);
      }
    }
    projection.space = AsSpace(appSpace_);
    projection.viewCount = 2;
    projection.views = projViews;
    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
    layerCount = 1;
  } else if (quadQueued_ && swapchain_) {
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = AsSpace(appSpace_);
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = static_cast<XrSwapchain>(swapchain_);
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {static_cast<int32_t>(swapchainWidth_),
                                      static_cast<int32_t>(swapchainHeight_)};
    quad.subImage.imageArrayIndex = 0;
    // Anchored where the player was looking when the first frame arrived,
    // rather than at the origin of LOCAL space - that origin is wherever the
    // headset happened to be when the session opened, which usually leaves the
    // screen off to one side. Yaw only: a screen that inherits the head's pitch
    // and roll is disorienting.
    quad.pose.orientation.x = quadAnchorOrientation_.x;
    quad.pose.orientation.y = quadAnchorOrientation_.y;
    quad.pose.orientation.z = quadAnchorOrientation_.z;
    quad.pose.orientation.w = quadAnchorOrientation_.w;
    quad.pose.position.x = quadAnchorPosition_.x;
    quad.pose.position.y = quadAnchorPosition_.y;
    quad.pose.position.z = quadAnchorPosition_.z;
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

  // UNORM strictly ahead of SRGB, in two passes. One pass with an OR takes
  // whichever the runtime happens to list first, and Quest lists SRGB first -
  // so this said UNORM-first and did the opposite.
  //
  // It matters because the frame is copied across with vkCmdCopyImage, which
  // moves raw bytes and converts nothing. The game writes display-ready,
  // already gamma-corrected pixels into a UNORM target; handing those to the
  // compositor in an SRGB image tells it to decode them a second time, which
  // comes out dark and over-saturated. Invisible on the title screen, because
  // white and magenta are near enough unchanged by it.
  int64_t chosen = 0;
  for (int64_t format : formats) {
    if (format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = format;
      break;
    }
  }
  if (!chosen) {
    for (int64_t format : formats) {
      if (format == VK_FORMAT_R8G8B8A8_SRGB) {
        chosen = format;
        break;
      }
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

// Asks the runtime for a display rate the port can actually hold.
//
// Without this the app inherits the system rate - 72Hz on a Quest 2 - and the
// compositor paces it to a submultiple when it cannot keep up: 13.9ms, then
// 27.8, then 41.7. The port measured a 41.6ms frame containing 20.6ms of work,
// so it was being held at the 24Hz tier with an entire tier of headroom unused
// and xrWait absorbing the difference.
//
// At 60Hz the tiers are 16.7/33.3/50ms and that same work lands on 33.3ms,
// which is 30fps - the rate Blue Dragon originally ran at. 72 is the panel's
// number, not the game's.
//
// bd_xr_refresh_rate picks the target; 0 leaves the runtime alone. The nearest
// supported rate at or below the request is used, because asking for a rate the
// device does not have is an error rather than a negotiation.
void Session::RequestDisplayRefreshRate() {
  const f64 wanted = REXCVAR_GET(bd_xr_refresh_rate);
  if (wanted <= 0.0 || !instance_ || !session_)
    return;

  PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
  PFN_xrRequestDisplayRefreshRateFB request = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(
          AsInstance(instance_), "xrEnumerateDisplayRefreshRatesFB",
          reinterpret_cast<PFN_xrVoidFunction *>(&enumerate))) ||
      XR_FAILED(xrGetInstanceProcAddr(
          AsInstance(instance_), "xrRequestDisplayRefreshRateFB",
          reinterpret_cast<PFN_xrVoidFunction *>(&request))) ||
      enumerate == nullptr || request == nullptr) {
    BD_INFO("OpenXR: no display refresh rate control, leaving it to the runtime");
    return;
  }

  uint32_t count = 0;
  if (XR_FAILED(enumerate(AsSession(session_), 0, &count, nullptr)) || count == 0)
    return;
  std::vector<float> rates(count, 0.0f);
  if (XR_FAILED(enumerate(AsSession(session_), count, &count, rates.data())))
    return;

  float best = 0.0f;
  for (const float r : rates)
    if (r <= float(wanted) + 0.5f && r > best)
      best = r;
  if (best <= 0.0f) {
    BD_INFO("OpenXR: no display rate at or below {:.0f}Hz, leaving it alone",
            wanted);
    return;
  }

  if (XR_FAILED(request(AsSession(session_), best))) {
    BD_WARN("OpenXR: display rate {:.0f}Hz refused", best);
    return;
  }
  BD_INFO("OpenXR: display rate set to {:.0f}Hz (asked {:.0f}, device offers "
          "{} rates)", best, wanted, count);
}

void Session::SubmitProjectionLayer() { g_projectionQueued = true; }

void Session::AnchorQuad(const FrameState &state) {
  if (quadAnchored_ || !state.shouldRender || state.viewCount == 0)
    return;

  // Done entirely in OpenXR space. The stored pose is in game space, so it goes
  // back through the mirror first - which is its own inverse - and everything
  // after that uses OpenXR's convention, where forward is -Z. Mixing the two
  // here is what put the screen 20 cm from the player's face.
  const Pose head = FromOpenXRPose(state.views[0].pose);

  Vec3 forward = Rotate(head.orientation, Vec3{0.0f, 0.0f, -1.0f});
  forward.y = 0.0f; // a screen should not tilt with the player's pitch
  forward = Normalize(forward);
  if (Length(forward) < 0.5f)
    forward = {0.0f, 0.0f, -1.0f}; // looking straight up or down

  const Vec3 anchor = head.position + forward * quadDistance_;
  quadAnchorPosition_ = {anchor.x, anchor.y, anchor.z};

  // Face the player. A quad's normal is +Z, and a yaw of t sends +Z to
  // (sin t, 0, cos t); that has to equal -forward, the direction back towards
  // the head. Note this is NOT the head's own yaw - getting that wrong leaves
  // the screen edge-on, and a player looking straight down -Z cannot tell the
  // two apart, because both are zero there.
  const f32 yaw = std::atan2(-forward.x, -forward.z);
  const f32 half = yaw * 0.5f;
  quadAnchorOrientation_ = {0.0f, std::sin(half), 0.0f, std::cos(half)};
  quadAnchored_ = true;
  BD_INFO("OpenXR: quad anchored at ({:.2f}, {:.2f}, {:.2f})",
          quadAnchorPosition_.x, quadAnchorPosition_.y, quadAnchorPosition_.z);
}

// Touch controllers reach an application only as OpenXR actions - there is no
// Android gamepad behind them - so without this the guest sees no pad at all
// and sits on its title screen forever.
//
// No subaction paths: A and B live on the right controller and X and Y on the
// left, so they are genuinely separate actions rather than one action with two
// hands, and binding the symmetric ones separately reads the same way.
bool Session::CreateActions() {
  if (!instance_ || !session_)
    return false;

  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName),
                "gameplay");
  std::snprintf(setInfo.localizedActionSetName,
                sizeof(setInfo.localizedActionSetName), "Gameplay");
  if (Failed(xrCreateActionSet(AsInstance(instance_), &setInfo, &g_actionSet),
             "xrCreateActionSet"))
    return false;

  const XrActionType kBool = XR_ACTION_TYPE_BOOLEAN_INPUT;
  const XrActionType kFloat = XR_ACTION_TYPE_FLOAT_INPUT;
  const XrActionType kVec2 = XR_ACTION_TYPE_VECTOR2F_INPUT;
  if (!MakeAction(g_actionSet, kBool, "a_click", "A", g_act.a) ||
      !MakeAction(g_actionSet, kBool, "b_click", "B", g_act.b) ||
      !MakeAction(g_actionSet, kBool, "x_click", "X", g_act.x) ||
      !MakeAction(g_actionSet, kBool, "y_click", "Y", g_act.y) ||
      !MakeAction(g_actionSet, kBool, "menu_click", "Menu", g_act.menu) ||
      !MakeAction(g_actionSet, kBool, "left_stick_click", "Left Stick Click",
                  g_act.leftClick) ||
      !MakeAction(g_actionSet, kBool, "right_stick_click", "Right Stick Click",
                  g_act.rightClick) ||
      !MakeAction(g_actionSet, kVec2, "left_stick", "Left Stick",
                  g_act.leftStick) ||
      !MakeAction(g_actionSet, kVec2, "right_stick", "Right Stick",
                  g_act.rightStick) ||
      !MakeAction(g_actionSet, kFloat, "left_trigger", "Left Trigger",
                  g_act.leftTrigger) ||
      !MakeAction(g_actionSet, kFloat, "right_trigger", "Right Trigger",
                  g_act.rightTrigger) ||
      !MakeAction(g_actionSet, kFloat, "left_grip", "Left Grip",
                  g_act.leftGrip) ||
      !MakeAction(g_actionSet, kFloat, "right_grip", "Right Grip",
                  g_act.rightGrip))
    return false;

  auto path = [&](const char *text) {
    XrPath out = XR_NULL_PATH;
    xrStringToPath(AsInstance(instance_), text, &out);
    return out;
  };

  const XrActionSuggestedBinding bindings[] = {
      {g_act.a, path("/user/hand/right/input/a/click")},
      {g_act.b, path("/user/hand/right/input/b/click")},
      {g_act.x, path("/user/hand/left/input/x/click")},
      {g_act.y, path("/user/hand/left/input/y/click")},
      {g_act.menu, path("/user/hand/left/input/menu/click")},
      {g_act.leftClick, path("/user/hand/left/input/thumbstick/click")},
      {g_act.rightClick, path("/user/hand/right/input/thumbstick/click")},
      {g_act.leftStick, path("/user/hand/left/input/thumbstick")},
      {g_act.rightStick, path("/user/hand/right/input/thumbstick")},
      {g_act.leftTrigger, path("/user/hand/left/input/trigger/value")},
      {g_act.rightTrigger, path("/user/hand/right/input/trigger/value")},
      {g_act.leftGrip, path("/user/hand/left/input/squeeze/value")},
      {g_act.rightGrip, path("/user/hand/right/input/squeeze/value")},
  };

  XrInteractionProfileSuggestedBinding suggest{
      XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  suggest.interactionProfile =
      path("/interaction_profiles/oculus/touch_controller");
  suggest.suggestedBindings = bindings;
  suggest.countSuggestedBindings = static_cast<u32>(std::size(bindings));
  if (Failed(
          xrSuggestInteractionProfileBindings(AsInstance(instance_), &suggest),
          "xrSuggestInteractionProfileBindings"))
    return false;

  XrSessionActionSetsAttachInfo attach{
      XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &g_actionSet;
  if (Failed(xrAttachSessionActionSets(AsSession(session_), &attach),
             "xrAttachSessionActionSets"))
    return false;

  BD_INFO("OpenXR: {} input actions attached", std::size(bindings));
  return true;
}

// Once per frame. Before the session reaches FOCUSED, xrSyncActions returns
// XR_SESSION_NOT_FOCUSED and every action reads inactive - which is correct,
// and not worth logging sixty times a second.
void Session::SyncActions() {
  if (!session_ || g_actionSet == XR_NULL_HANDLE)
    return;

  XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;
  if (XR_FAILED(xrSyncActions(AsSession(session_), &sync)))
    return;

  const XrSession session = AsSession(session_);
  PadState pad;
  pad.a = ReadBool(session, g_act.a);
  pad.b = ReadBool(session, g_act.b);
  pad.x = ReadBool(session, g_act.x);
  pad.y = ReadBool(session, g_act.y);
  pad.menu = ReadBool(session, g_act.menu);
  pad.leftThumbClick = ReadBool(session, g_act.leftClick);
  pad.rightThumbClick = ReadBool(session, g_act.rightClick);
  ReadStick(session, g_act.leftStick, pad.leftStickX, pad.leftStickY);
  ReadStick(session, g_act.rightStick, pad.rightStickX, pad.rightStickY);
  pad.leftTrigger = ReadFloat(session, g_act.leftTrigger);
  pad.rightTrigger = ReadFloat(session, g_act.rightTrigger);
  pad.leftGrip = ReadFloat(session, g_act.leftGrip);
  pad.rightGrip = ReadFloat(session, g_act.rightGrip);
  // Connected even while the runtime has focus on its own menu: the pad is
  // there, nobody is pressing it. Reporting it absent would make the guest
  // think the controller had been unplugged every time you check the time.
  pad.connected = true;
  SubmitPad(pad);
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

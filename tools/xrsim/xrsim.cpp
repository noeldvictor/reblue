/**
 * @file    tools/xrsim/xrsim.cpp
 * @brief   A headless OpenXR runtime, for testing VR with no headset.
 *
 * Why this exists: every other way of running the VR path without hardware is
 * shut. SteamVR's runtime will not initialise without an activated HMD, even
 * with the null driver enabled and vrserver running. The Oculus runtime wants a
 * Quest over Link. Meta XR Simulator is the right tool and its binary sits
 * behind a developer login that cannot be scripted.
 *
 * So: implement the subset of OpenXR that reblue actually calls - 32 entry
 * points - and nothing else. It reports two views, hands out real VkImages for
 * the swapchains so the renderer has somewhere to draw, and returns a head pose
 * this file makes up. It composites nothing and displays nothing.
 *
 * That is exactly what an automated check wants. The pose is deterministic and
 * scriptable, so a capture taken at frame N is the same every run - which is
 * what makes the camera modes and the character anchor testable at all, and
 * what a real headset can never give you.
 *
 * Point the loader at it per process, so nothing about the machine changes:
 *
 *   XR_RUNTIME_JSON=<build>/reblue_xrsim.json ./reblue_vk.exe
 *
 * Environment, all optional:
 *   XRSIM_YAW_RATE   radians/sec the head turns. Default 0, i.e. a fixed pose.
 *   XRSIM_HEIGHT     eye height in metres above the stage. Default 1.6.
 *   XRSIM_IPD        interpupillary distance in metres. Default 0.064.
 *   XRSIM_WIDTH      per-eye recommended width. Default 1024.
 *   XRSIM_HEIGHT_PX  per-eye recommended height. Default 1024.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */

#define XR_USE_GRAPHICS_API_VULKAN
// Deliberately NOT XR_USE_PLATFORM_WIN32: that pulls the D3D bindings into
// openxr_platform.h, which then wants IUnknown and a COM header this runtime
// has no use for. windows.h is here only for GetProcAddress.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Configuration, read once from the environment.

double EnvD(const char *name, double fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  return std::atof(v);
}

struct Config {
  double yawRate = EnvD("XRSIM_YAW_RATE", 0.0);
  double height = EnvD("XRSIM_HEIGHT", 1.6);
  double ipd = EnvD("XRSIM_IPD", 0.064);
  uint32_t width = uint32_t(EnvD("XRSIM_WIDTH", 1024));
  uint32_t height_px = uint32_t(EnvD("XRSIM_HEIGHT_PX", 1024));
};
Config &Cfg() {
  static Config c;
  return c;
}

void Log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[xrsim] ");
  std::vfprintf(stderr, fmt, args);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  va_end(args);
}

// ---------------------------------------------------------------------------
// Handles. Opaque pointers to our own structs, which is all the spec requires.

struct Swapchain;

struct Instance {
  uint64_t magic = 0x58525349u; // 'XRSI'
  std::vector<std::string> extensions;
};

struct Session {
  Instance *instance = nullptr;
  // From XrGraphicsBindingVulkanKHR. The app owns these; we only make images.
  VkInstance vkInstance = VK_NULL_HANDLE;
  VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
  VkDevice vkDevice = VK_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false;
  // The state machine is driven entirely by how many events we have posted,
  // because there is no runtime to change its mind.
  int eventsSent = 0;
  int64_t frameIndex = 0;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::vector<Swapchain *> swapchains;
};

struct Space {
  Session *session = nullptr;
  XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_STAGE;
};

struct Swapchain {
  Session *session = nullptr;
  uint32_t width = 0, height = 0, arraySize = 1;
  int64_t format = 0;
  std::vector<VkImage> images;
  std::vector<VkDeviceMemory> memory;
  uint32_t acquired = 0;
};

struct ActionSet {
  Instance *instance = nullptr;
};
struct Action {
  ActionSet *set = nullptr;
  XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
};

std::mutex g_mutex;

// The instance does not track its sessions, and reblue only ever makes one, so
// xrPollEvent finds it here rather than through a lookup that would exist only
// to serve a case that never happens.
Session *g_lastSession = nullptr;

template <typename T, typename H> T *From(H h) {
  return reinterpret_cast<T *>(h);
}
template <typename H, typename T> H To(T *p) {
  return reinterpret_cast<H>(p);
}

// ---------------------------------------------------------------------------
// Vulkan entry points, fetched from the loader the app already has loaded.

struct Vk {
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  PFN_vkCreateImage createImage = nullptr;
  PFN_vkDestroyImage destroyImage = nullptr;
  PFN_vkAllocateMemory allocateMemory = nullptr;
  PFN_vkFreeMemory freeMemory = nullptr;
  PFN_vkBindImageMemory bindImageMemory = nullptr;
  PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties =
      nullptr;
  PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
  bool ok = false;
};

Vk &VkFns() {
  static Vk vk;
  return vk;
}

bool LoadVulkan(VkInstance instance, VkDevice device) {
  Vk &vk = VkFns();
  if (vk.ok)
    return true;
#ifdef _WIN32
  HMODULE lib = GetModuleHandleA("vulkan-1.dll");
  if (!lib)
    lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) {
    Log("could not load vulkan-1.dll");
    return false;
  }
  vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void *>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
  return false;
#endif
  if (!vk.getInstanceProcAddr)
    return false;

  auto get = [&](const char *name) {
    return vk.getInstanceProcAddr(instance, name);
  };
  vk.createImage = reinterpret_cast<PFN_vkCreateImage>(get("vkCreateImage"));
  vk.destroyImage = reinterpret_cast<PFN_vkDestroyImage>(get("vkDestroyImage"));
  vk.allocateMemory =
      reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
  vk.freeMemory = reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
  vk.bindImageMemory =
      reinterpret_cast<PFN_vkBindImageMemory>(get("vkBindImageMemory"));
  vk.getImageMemoryRequirements =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          get("vkGetImageMemoryRequirements"));
  vk.getPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          get("vkGetPhysicalDeviceMemoryProperties"));
  vk.enumeratePhysicalDevices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
          get("vkEnumeratePhysicalDevices"));
  (void)device;
  vk.ok = vk.createImage && vk.allocateMemory && vk.bindImageMemory &&
          vk.getImageMemoryRequirements &&
          vk.getPhysicalDeviceMemoryProperties;
  if (!vk.ok)
    Log("failed to resolve the Vulkan entry points we need");
  return vk.ok;
}

// ---------------------------------------------------------------------------
// Pose. Deterministic by construction: a function of frame index only, never of
// wall-clock time, so two runs that reach the same frame see the same head.

XrPosef HeadPose(Session *s) {
  const Config &c = Cfg();
  const double t = double(s->frameIndex) / 60.0; // nominal 60Hz
  const double yaw = c.yawRate * t;
  XrPosef p{};
  p.orientation.x = 0.0f;
  p.orientation.y = float(std::sin(yaw * 0.5));
  p.orientation.z = 0.0f;
  p.orientation.w = float(std::cos(yaw * 0.5));
  p.position.x = 0.0f;
  p.position.y = float(c.height);
  p.position.z = 0.0f;
  return p;
}

} // namespace

// ---------------------------------------------------------------------------
// The API. Only what reblue calls.

extern "C" {

static XrResult XRAPI_CALL Sim_xrEnumerateInstanceExtensionProperties(
    const char *, uint32_t capacity, uint32_t *count,
    XrExtensionProperties *props) {
  static const char *kExts[] = {
      XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
      XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
  };
  const uint32_t n = uint32_t(sizeof(kExts) / sizeof(kExts[0]));
  *count = n;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < n)
    return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < n; ++i) {
    props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    std::snprintf(props[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s",
                  kExts[i]);
    props[i].extensionVersion = 1;
  }
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrCreateInstance(const XrInstanceCreateInfo *info,
                                                XrInstance *out) {
  auto *inst = new Instance();
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
    inst->extensions.emplace_back(info->enabledExtensionNames[i]);
  *out = To<XrInstance>(inst);
  Log("instance up, %u extension(s), headless", info->enabledExtensionCount);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroyInstance(XrInstance h) {
  delete From<Instance>(h);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetInstanceProperties(
    XrInstance, XrInstanceProperties *p) {
  p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
  std::snprintf(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "reblue xrsim");
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetSystem(XrInstance, const XrSystemGetInfo *,
                                           XrSystemId *out) {
  *out = 1;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetSystemProperties(XrInstance, XrSystemId,
                                                     XrSystemProperties *p) {
  const Config &c = Cfg();
  p->systemId = 1;
  p->vendorId = 0;
  std::snprintf(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "reblue simulated HMD");
  p->graphicsProperties.maxSwapchainImageHeight = c.height_px * 4;
  p->graphicsProperties.maxSwapchainImageWidth = c.width * 4;
  p->graphicsProperties.maxLayerCount = 16;
  p->trackingProperties.orientationTracking = XR_TRUE;
  p->trackingProperties.positionTracking = XR_TRUE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEnumerateViewConfigurations(
    XrInstance, XrSystemId, uint32_t capacity, uint32_t *count,
    XrViewConfigurationType *types) {
  *count = 1;
  if (capacity == 0)
    return XR_SUCCESS;
  types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetViewConfigurationProperties(
    XrInstance, XrSystemId, XrViewConfigurationType,
    XrViewConfigurationProperties *p) {
  p->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  p->fovMutable = XR_TRUE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEnumerateViewConfigurationViews(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
    uint32_t *count, XrViewConfigurationView *views) {
  const Config &c = Cfg();
  *count = 2;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < 2)
    return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < 2; ++i) {
    views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    views[i].next = nullptr;
    views[i].recommendedImageRectWidth = c.width;
    views[i].maxImageRectWidth = c.width * 2;
    views[i].recommendedImageRectHeight = c.height_px;
    views[i].maxImageRectHeight = c.height_px * 2;
    views[i].recommendedSwapchainSampleCount = 1;
    views[i].maxSwapchainSampleCount = 1;
  }
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEnumerateEnvironmentBlendModes(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
    uint32_t *count, XrEnvironmentBlendMode *modes) {
  *count = 1;
  if (capacity == 0)
    return XR_SUCCESS;
  modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  return XR_SUCCESS;
}

// --- XR_KHR_vulkan_enable -------------------------------------------------
//
// The app owns the Vulkan instance and device under this extension, so all the
// runtime has to do is ask for no extra extensions and name a physical device.

static XrResult XRAPI_CALL Sim_xrGetVulkanInstanceExtensionsKHR(
    XrInstance, XrSystemId, uint32_t capacity, uint32_t *count, char *buf) {
  *count = 1;
  if (capacity == 0)
    return XR_SUCCESS;
  buf[0] = '\0';
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetVulkanDeviceExtensionsKHR(
    XrInstance, XrSystemId, uint32_t capacity, uint32_t *count, char *buf) {
  *count = 1;
  if (capacity == 0)
    return XR_SUCCESS;
  buf[0] = '\0';
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetVulkanGraphicsDeviceKHR(
    XrInstance, XrSystemId, VkInstance vkInstance, VkPhysicalDevice *out) {
  if (!LoadVulkan(vkInstance, VK_NULL_HANDLE))
    return XR_ERROR_RUNTIME_FAILURE;
  uint32_t n = 0;
  VkFns().enumeratePhysicalDevices(vkInstance, &n, nullptr);
  if (n == 0)
    return XR_ERROR_RUNTIME_FAILURE;
  std::vector<VkPhysicalDevice> devices(n);
  VkFns().enumeratePhysicalDevices(vkInstance, &n, devices.data());
  // Whatever the app would have picked anyway. There is no display to match.
  *out = devices[0];
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetVulkanGraphicsRequirementsKHR(
    XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *req) {
  req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
  req->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
  req->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
  return XR_SUCCESS;
}

// --- session --------------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrCreateSession(XrInstance h,
                                               const XrSessionCreateInfo *info,
                                               XrSession *out) {
  auto *s = new Session();
  s->instance = From<Instance>(h);
  for (const XrBaseInStructure *n =
           static_cast<const XrBaseInStructure *>(info->next);
       n; n = n->next) {
    if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
      const auto *b =
          reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(n);
      s->vkInstance = b->instance;
      s->vkPhysicalDevice = b->physicalDevice;
      s->vkDevice = b->device;
    }
  }
  if (s->vkDevice == VK_NULL_HANDLE) {
    delete s;
    Log("session created with no Vulkan binding");
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  LoadVulkan(s->vkInstance, s->vkDevice);
  s->state = XR_SESSION_STATE_IDLE;
  g_lastSession = s;
  *out = To<XrSession>(s);
  Log("session up on the app's VkDevice");
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroySession(XrSession h) {
  delete From<Session>(h);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrBeginSession(XrSession h,
                                              const XrSessionBeginInfo *) {
  From<Session>(h)->running = true;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEndSession(XrSession h) {
  From<Session>(h)->running = false;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrRequestExitSession(XrSession) {
  return XR_SUCCESS;
}

// Walks IDLE -> READY -> SYNCHRONIZED -> VISIBLE -> FOCUSED, one event per
// poll, then goes quiet. An app that waits for FOCUSED before it renders - and
// reblue does - never starts if this stalls, so it must always make progress.
static XrResult XRAPI_CALL Sim_xrPollEvent(XrInstance,
                                           XrEventDataBuffer *event) {
  std::lock_guard<std::mutex> lock(g_mutex);
  static const XrSessionState kOrder[] = {
      XR_SESSION_STATE_READY, XR_SESSION_STATE_SYNCHRONIZED,
      XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED};
  if (!g_lastSession)
    return XR_EVENT_UNAVAILABLE;
  Session *tracked = g_lastSession;
  const int n = tracked->eventsSent;
  if (n >= int(sizeof(kOrder) / sizeof(kOrder[0])))
    return XR_EVENT_UNAVAILABLE;
  tracked->eventsSent = n + 1;
  auto *ev = reinterpret_cast<XrEventDataSessionStateChanged *>(event);
  ev->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
  ev->next = nullptr;
  ev->session = To<XrSession>(tracked);
  ev->state = kOrder[n];
  ev->time = 0;
  tracked->state = kOrder[n];
  Log("session state -> %d", int(kOrder[n]));
  return XR_SUCCESS;
}

// --- spaces ---------------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrCreateReferenceSpace(
    XrSession h, const XrReferenceSpaceCreateInfo *info, XrSpace *out) {
  auto *sp = new Space();
  sp->session = From<Session>(h);
  sp->type = info->referenceSpaceType;
  *out = To<XrSpace>(sp);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrCreateActionSpace(
    XrSession h, const XrActionSpaceCreateInfo *, XrSpace *out) {
  auto *sp = new Space();
  sp->session = From<Session>(h);
  *out = To<XrSpace>(sp);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroySpace(XrSpace h) {
  delete From<Space>(h);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEnumerateReferenceSpaces(
    XrSession, uint32_t capacity, uint32_t *count,
    XrReferenceSpaceType *types) {
  *count = 2;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < 2)
    return XR_ERROR_SIZE_INSUFFICIENT;
  types[0] = XR_REFERENCE_SPACE_TYPE_LOCAL;
  types[1] = XR_REFERENCE_SPACE_TYPE_STAGE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrLocateSpace(XrSpace h, XrSpace, XrTime,
                                             XrSpaceLocation *loc) {
  auto *sp = From<Space>(h);
  loc->pose = HeadPose(sp->session);
  loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                       XR_SPACE_LOCATION_POSITION_VALID_BIT |
                       XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                       XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
  return XR_SUCCESS;
}

// --- swapchains -----------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrEnumerateSwapchainFormats(
    XrSession, uint32_t capacity, uint32_t *count, int64_t *formats) {
  static const int64_t kFormats[] = {
      VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_D32_SFLOAT,
  };
  const uint32_t n = uint32_t(sizeof(kFormats) / sizeof(kFormats[0]));
  *count = n;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < n)
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::memcpy(formats, kFormats, sizeof(kFormats));
  return XR_SUCCESS;
}

static uint32_t MemoryTypeIndex(VkPhysicalDevice phys, uint32_t bits,
                                VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props{};
  VkFns().getPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & want) == want)
      return i;
  return UINT32_MAX;
}

static XrResult XRAPI_CALL Sim_xrCreateSwapchain(
    XrSession h, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
  auto *s = From<Session>(h);
  auto *sc = new Swapchain();
  sc->session = s;
  sc->width = info->width;
  sc->height = info->height;
  sc->arraySize = info->arraySize ? info->arraySize : 1;
  sc->format = info->format;

  // Three, like a real runtime, so the app's frame pacing has somewhere to go.
  const uint32_t kImages = 3;
  const bool isDepth = info->format == VK_FORMAT_D32_SFLOAT ||
                       info->format == VK_FORMAT_D24_UNORM_S8_UINT;
  for (uint32_t i = 0; i < kImages; ++i) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VkFormat(info->format);
    ci.extent = {info->width, info->height, 1};
    ci.mipLevels = info->mipCount ? info->mipCount : 1;
    ci.arrayLayers = sc->arraySize;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (VkFns().createImage(s->vkDevice, &ci, nullptr, &image) != VK_SUCCESS) {
      Log("vkCreateImage failed for a %ux%u swapchain", info->width,
          info->height);
      delete sc;
      return XR_ERROR_RUNTIME_FAILURE;
    }
    VkMemoryRequirements req{};
    VkFns().getImageMemoryRequirements(s->vkDevice, image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryTypeIndex(s->vkPhysicalDevice,
                                         req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (ai.memoryTypeIndex == UINT32_MAX ||
        VkFns().allocateMemory(s->vkDevice, &ai, nullptr, &mem) != VK_SUCCESS) {
      Log("could not allocate swapchain image memory");
      delete sc;
      return XR_ERROR_RUNTIME_FAILURE;
    }
    VkFns().bindImageMemory(s->vkDevice, image, mem, 0);
    sc->images.push_back(image);
    sc->memory.push_back(mem);
  }
  s->swapchains.push_back(sc);
  *out = To<XrSwapchain>(sc);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroySwapchain(XrSwapchain h) {
  auto *sc = From<Swapchain>(h);
  if (sc && sc->session && VkFns().ok) {
    for (size_t i = 0; i < sc->images.size(); ++i) {
      VkFns().destroyImage(sc->session->vkDevice, sc->images[i], nullptr);
      VkFns().freeMemory(sc->session->vkDevice, sc->memory[i], nullptr);
    }
  }
  delete sc;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEnumerateSwapchainImages(
    XrSwapchain h, uint32_t capacity, uint32_t *count,
    XrSwapchainImageBaseHeader *images) {
  auto *sc = From<Swapchain>(h);
  *count = uint32_t(sc->images.size());
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < sc->images.size())
    return XR_ERROR_SIZE_INSUFFICIENT;
  auto *out = reinterpret_cast<XrSwapchainImageVulkanKHR *>(images);
  for (size_t i = 0; i < sc->images.size(); ++i) {
    out[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    out[i].next = nullptr;
    out[i].image = sc->images[i];
  }
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrAcquireSwapchainImage(
    XrSwapchain h, const XrSwapchainImageAcquireInfo *, uint32_t *index) {
  auto *sc = From<Swapchain>(h);
  *index = sc->acquired;
  sc->acquired = (sc->acquired + 1) % uint32_t(sc->images.size());
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrWaitSwapchainImage(
    XrSwapchain, const XrSwapchainImageWaitInfo *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrReleaseSwapchainImage(
    XrSwapchain, const XrSwapchainImageReleaseInfo *) {
  return XR_SUCCESS;
}

// --- frame loop -----------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrWaitFrame(XrSession h, const XrFrameWaitInfo *,
                                           XrFrameState *state) {
  auto *s = From<Session>(h);
  // No compositor, so no vsync to wait for. Returning immediately makes the
  // app run as fast as it can, which is what an unattended check wants - the
  // pose is a function of frame index, not of time, so nothing drifts.
  state->type = XR_TYPE_FRAME_STATE;
  state->predictedDisplayTime = XrTime(s->frameIndex) * 16666666LL;
  state->predictedDisplayPeriod = 16666666LL;
  state->shouldRender = XR_TRUE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrBeginFrame(XrSession,
                                            const XrFrameBeginInfo *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrEndFrame(XrSession h,
                                          const XrFrameEndInfo *info) {
  auto *s = From<Session>(h);
  ++s->frameIndex;
  static int64_t reported = 0;
  if (s->frameIndex - reported >= 300) {
    reported = s->frameIndex;
    Log("frame %lld, %u layer(s)", (long long)s->frameIndex,
        info ? info->layerCount : 0u);
  }
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrLocateViews(XrSession h,
                                             const XrViewLocateInfo *,
                                             XrViewState *viewState,
                                             uint32_t capacity, uint32_t *count,
                                             XrView *views) {
  auto *s = From<Session>(h);
  *count = 2;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < 2)
    return XR_ERROR_SIZE_INSUFFICIENT;
  viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                              XR_VIEW_STATE_POSITION_VALID_BIT |
                              XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                              XR_VIEW_STATE_POSITION_TRACKED_BIT;
  const XrPosef head = HeadPose(s);
  const float halfIpd = float(Cfg().ipd * 0.5);
  for (uint32_t i = 0; i < 2; ++i) {
    views[i].type = XR_TYPE_VIEW;
    views[i].next = nullptr;
    views[i].pose = head;
    // Offset along the head's local X. The orientation is a pure yaw, so this
    // is just a rotation of the eye vector about Y - no need for a general
    // quaternion rotate.
    const float s2 = head.orientation.y, c2 = head.orientation.w;
    const float yaw = 2.0f * std::atan2(s2, c2);
    const float dx = (i == 0 ? -halfIpd : halfIpd);
    views[i].pose.position.x += dx * std::cos(yaw);
    views[i].pose.position.z += -dx * std::sin(yaw);
    // A symmetric-ish frustum with the slight inward asymmetry a real headset
    // has, so an off-centre projection bug cannot hide behind symmetry.
    views[i].fov.angleLeft = (i == 0) ? -0.90f : -0.83f;
    views[i].fov.angleRight = (i == 0) ? 0.83f : 0.90f;
    views[i].fov.angleUp = 0.86f;
    views[i].fov.angleDown = -0.86f;
  }
  return XR_SUCCESS;
}

// --- input ----------------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrStringToPath(XrInstance, const char *str,
                                              XrPath *out) {
  // A hash is a perfectly good path: nothing here ever turns one back into a
  // string, and collisions across the dozen paths reblue uses are not a risk.
  uint64_t h = 1469598103934665603ull;
  for (const char *p = str; *p; ++p) {
    h ^= uint8_t(*p);
    h *= 1099511628211ull;
  }
  *out = XrPath(h ? h : 1);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrPathToString(XrInstance, XrPath, uint32_t,
                                              uint32_t *count, char *buf) {
  *count = 1;
  if (buf)
    buf[0] = '\0';
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrCreateActionSet(XrInstance h,
                                                 const XrActionSetCreateInfo *,
                                                 XrActionSet *out) {
  auto *as = new ActionSet();
  as->instance = From<Instance>(h);
  *out = To<XrActionSet>(as);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroyActionSet(XrActionSet h) {
  delete From<ActionSet>(h);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrCreateAction(XrActionSet h,
                                              const XrActionCreateInfo *info,
                                              XrAction *out) {
  auto *a = new Action();
  a->set = From<ActionSet>(h);
  a->type = info->actionType;
  *out = To<XrAction>(a);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrDestroyAction(XrAction h) {
  delete From<Action>(h);
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrAttachSessionActionSets(
    XrSession, const XrSessionActionSetsAttachInfo *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrSyncActions(XrSession,
                                             const XrActionsSyncInfo *) {
  return XR_SUCCESS;
}

// Everything reads as untouched. reblue's autoplay drives the game through a
// synthesised pad, so the controllers do not need to do anything here - and a
// simulated stick that drifted would make captures non-reproducible.
static XrResult XRAPI_CALL Sim_xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo *, XrActionStateBoolean *state) {
  state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
  state->currentState = XR_FALSE;
  state->changedSinceLastSync = XR_FALSE;
  state->isActive = XR_TRUE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo *, XrActionStateFloat *state) {
  state->type = XR_TYPE_ACTION_STATE_FLOAT;
  state->currentState = 0.0f;
  state->changedSinceLastSync = XR_FALSE;
  state->isActive = XR_TRUE;
  return XR_SUCCESS;
}

// The thumbsticks. Missing this crashed reblue at a null PFN the moment the
// guest first polled the pad - the app caches the pointer and calls it without
// checking, which is normal for an OpenXR client because a conformant runtime
// always provides the core entry points.
static XrResult XRAPI_CALL Sim_xrGetActionStateVector2f(
    XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *state) {
  state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
  state->currentState = {0.0f, 0.0f};
  state->changedSinceLastSync = XR_FALSE;
  state->isActive = XR_TRUE;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrApplyHapticFeedback(
    XrSession, const XrHapticActionInfo *, const XrHapticBaseHeader *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrStopHapticFeedback(XrSession,
                                                    const XrHapticActionInfo *) {
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetCurrentInteractionProfile(
    XrSession, XrPath, XrInteractionProfileState *state) {
  state->type = XR_TYPE_INTERACTION_PROFILE_STATE;
  state->interactionProfile = XR_NULL_PATH;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetActionStatePose(
    XrSession, const XrActionStateGetInfo *, XrActionStatePose *state) {
  state->type = XR_TYPE_ACTION_STATE_POSE;
  state->isActive = XR_TRUE;
  return XR_SUCCESS;
}

// --- XR_FB_display_refresh_rate ------------------------------------------

static XrResult XRAPI_CALL Sim_xrEnumerateDisplayRefreshRatesFB(
    XrSession, uint32_t capacity, uint32_t *count, float *rates) {
  static const float kRates[] = {60.0f, 72.0f, 90.0f};
  const uint32_t n = 3;
  *count = n;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < n)
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::memcpy(rates, kRates, sizeof(kRates));
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrGetDisplayRefreshRateFB(XrSession,
                                                         float *rate) {
  *rate = 60.0f;
  return XR_SUCCESS;
}

static XrResult XRAPI_CALL Sim_xrRequestDisplayRefreshRateFB(XrSession, float) {
  return XR_SUCCESS;
}

// --- dispatch -------------------------------------------------------------

static XrResult XRAPI_CALL Sim_xrGetInstanceProcAddr(XrInstance instance,
                                                     const char *name,
                                                     PFN_xrVoidFunction *fn) {
#define BIND(n)                                                                \
  if (std::strcmp(name, #n) == 0) {                                            \
    *fn = reinterpret_cast<PFN_xrVoidFunction>(Sim_##n);                       \
    return XR_SUCCESS;                                                         \
  }
  BIND(xrGetInstanceProcAddr)
  BIND(xrEnumerateInstanceExtensionProperties)
  BIND(xrCreateInstance)
  BIND(xrDestroyInstance)
  BIND(xrGetInstanceProperties)
  BIND(xrGetSystem)
  BIND(xrGetSystemProperties)
  BIND(xrEnumerateViewConfigurations)
  BIND(xrGetViewConfigurationProperties)
  BIND(xrEnumerateViewConfigurationViews)
  BIND(xrEnumerateEnvironmentBlendModes)
  BIND(xrGetVulkanInstanceExtensionsKHR)
  BIND(xrGetVulkanDeviceExtensionsKHR)
  BIND(xrGetVulkanGraphicsDeviceKHR)
  BIND(xrGetVulkanGraphicsRequirementsKHR)
  BIND(xrCreateSession)
  BIND(xrDestroySession)
  BIND(xrBeginSession)
  BIND(xrEndSession)
  BIND(xrRequestExitSession)
  BIND(xrPollEvent)
  BIND(xrCreateReferenceSpace)
  BIND(xrCreateActionSpace)
  BIND(xrDestroySpace)
  BIND(xrEnumerateReferenceSpaces)
  BIND(xrLocateSpace)
  BIND(xrEnumerateSwapchainFormats)
  BIND(xrCreateSwapchain)
  BIND(xrDestroySwapchain)
  BIND(xrEnumerateSwapchainImages)
  BIND(xrAcquireSwapchainImage)
  BIND(xrWaitSwapchainImage)
  BIND(xrReleaseSwapchainImage)
  BIND(xrWaitFrame)
  BIND(xrBeginFrame)
  BIND(xrEndFrame)
  BIND(xrLocateViews)
  BIND(xrStringToPath)
  BIND(xrPathToString)
  BIND(xrCreateActionSet)
  BIND(xrDestroyActionSet)
  BIND(xrCreateAction)
  BIND(xrDestroyAction)
  BIND(xrSuggestInteractionProfileBindings)
  BIND(xrAttachSessionActionSets)
  BIND(xrSyncActions)
  BIND(xrGetActionStateBoolean)
  BIND(xrGetActionStateFloat)
  BIND(xrGetActionStatePose)
  BIND(xrGetActionStateVector2f)
  BIND(xrApplyHapticFeedback)
  BIND(xrStopHapticFeedback)
  BIND(xrGetCurrentInteractionProfile)
  BIND(xrEnumerateDisplayRefreshRatesFB)
  BIND(xrGetDisplayRefreshRateFB)
  BIND(xrRequestDisplayRefreshRateFB)
#undef BIND
  (void)instance;
  *fn = nullptr;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// The loader's entry point. Everything above hangs off this.
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
XrResult XRAPI_CALL
    xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loaderInfo,
                                      XrNegotiateRuntimeRequest *request) {
  if (!loaderInfo || !request)
    return XR_ERROR_INITIALIZATION_FAILED;
  if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
    return XR_ERROR_INITIALIZATION_FAILED;
  request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  request->runtimeApiVersion = XR_CURRENT_API_VERSION;
  request->getInstanceProcAddr = Sim_xrGetInstanceProcAddr;
  Log("negotiated with the loader");
  return XR_SUCCESS;
}

} // extern "C"

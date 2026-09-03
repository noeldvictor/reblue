/**
 * @file    gpu/renderdoc_capture.cpp
 * @brief   Drives RenderDoc's in-application API from a cvar.
 *
 * This exists because the questions left in this port are all of the form
 * "which draw wrote nothing, and into what" - and every attempt to answer one
 * by inference has lost. The open multiview bug had ten causes eliminated by
 * measurement and none of them right, three of the wrong conclusions coming
 * from bounded log counters that answered "what happened first" rather than
 * "what happens".
 *
 * A frame capture answers all of it at once and offline: which draws exist,
 * what framebuffer and render pass each targets, what it sampled, and what the
 * target held before and after. `renderdoccmd convert -f cap.rdc -c xml` turns
 * that into greppable text on the host, so the answer needs no GUI and no
 * headset.
 *
 * Triggering it from inside the app rather than through `renderdoccmd capture`
 * is deliberate: RenderDoc's command-line capture waits on a keypress, which a
 * headless autoplay run cannot supply. The in-application API is the documented
 * route for exactly this, and it keeps the whole thing a cvar - so a capture
 * costs one run and no interaction, matching bd_capture_after_s.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "gpu/renderdoc_capture.h"

#include "core/app_root.h"
#include "core/logging.h"
#include "core/settings.h"

#include <algorithm>
#include <renderdoc_app.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DECLARE(bool, bd_renderdoc);
REXCVAR_DECLARE(double, bd_renderdoc_after_s);
REXCVAR_DECLARE(i32, bd_renderdoc_frames);

namespace bd::gpu::renderdoc {

namespace {

RENDERDOC_API_1_6_0 *g_api = nullptr;
bool g_fired = false;

// RenderDoc is not on the search path of a normal install, so probe the
// documented module name first (which succeeds when the process was launched
// through RenderDoc and the module is already resident) and fall back to the
// default install location. RENDERDOC_DLL overrides both, because a dev tool
// should not need a rebuild to point somewhere else.
void *OpenModule() {
  const char *env = std::getenv("RENDERDOC_DLL");
#if defined(_WIN32)
  if (HMODULE already = GetModuleHandleA("renderdoc.dll"))
    return already;
  const char *candidates[] = {env, "renderdoc.dll",
                              "C:/Program Files/RenderDoc/renderdoc.dll"};
  for (const char *c : candidates) {
    if (!c || !*c)
      continue;
    if (HMODULE m = LoadLibraryA(c)) {
      BD_INFO("[rdoc] loaded {}", c);
      return m;
    }
  }
#else
  // RTLD_NOLOAD first: on Android the layer has already loaded it, and loading
  // a second copy would hook nothing.
  const char *names[] = {env, "librenderdoc.so",
                         "libVkLayer_GLES_RenderDoc.so"};
  for (const char *c : names) {
    if (!c || !*c)
      continue;
    if (void *already = dlopen(c, RTLD_NOW | RTLD_NOLOAD))
      return already;
  }
  for (const char *c : names) {
    if (!c || !*c)
      continue;
    if (void *m = dlopen(c, RTLD_NOW)) {
      BD_INFO("[rdoc] loaded {}", c);
      return m;
    }
  }
#endif
  return nullptr;
}

void *Symbol(void *module, const char *name) {
#if defined(_WIN32)
  return reinterpret_cast<void *>(
      GetProcAddress(static_cast<HMODULE>(module), name));
#else
  return dlsym(module, name);
#endif
}

} // namespace

void LoadIfRequested() {
  if (g_api || !REXCVAR_GET(bd_renderdoc))
    return;

  void *module = OpenModule();
  if (!module) {
    BD_WARN("[rdoc] bd_renderdoc is set but RenderDoc could not be loaded - "
            "set RENDERDOC_DLL to the module path");
    return;
  }

  auto get_api =
      reinterpret_cast<pRENDERDOC_GetAPI>(Symbol(module, "RENDERDOC_GetAPI"));
  if (!get_api) {
    BD_WARN("[rdoc] module has no RENDERDOC_GetAPI");
    return;
  }
  if (!get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&g_api)) ||
      !g_api) {
    BD_WARN("[rdoc] RENDERDOC_GetAPI(1.6.0) refused");
    g_api = nullptr;
    return;
  }

  const auto dir = bd::AppRootFolder() / "logs" / "renderdoc";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto tmpl = (dir / "reblue").string();
  g_api->SetCaptureFilePathTemplate(tmpl.c_str());

  // The on-screen overlay would otherwise land in bd_capture_after_s's
  // composited grab, which is read as evidence elsewhere.
  g_api->MaskOverlayBits(0, 0);

  int major = 0, minor = 0, patch = 0;
  g_api->GetAPIVersion(&major, &minor, &patch);
  BD_INFO("[rdoc] in-application API {}.{}.{} ready, writing to {}", major,
          minor, patch, tmpl);
}

void TriggerIfDue() {
  if (!g_api || g_fired)
    return;
  const double after = REXCVAR_GET(bd_renderdoc_after_s);
  if (after <= 0.0)
    return;

  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  if (std::chrono::duration<double>(Clock::now() - start).count() < after)
    return;

  g_fired = true;
  // Several consecutive frames when asked: an artefact that alternates
  // between frames needs the frame that carries it, and the companion
  // bd_capture_frames sequence names it (2026-09-03).
  const u32 frames = static_cast<u32>(std::max(1, REXCVAR_GET(bd_renderdoc_frames)));
  if (frames > 1)
    g_api->TriggerMultiFrameCapture(frames);
  else
    g_api->TriggerCapture();
  BD_INFO("[rdoc] capture triggered at {:.1f}s - covers the next {} frame(s)",
          after, frames);
}

bool CaptureWritten() {
  return g_api && g_fired && g_api->GetNumCaptures() > 0;
}

} // namespace bd::gpu::renderdoc

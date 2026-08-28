/**
 * @file    platform/android_main.cpp
 * @brief   Entry point for the Android build.
 *
 * The SDK ships windowed_app_main_sdl.cpp for desktop targets, but it cannot be
 * used here: on Android, rex/ui/windowed_app.h defines
 * XE_UI_WINDOWED_APPS_IN_LIBRARY, which swaps REX_DEFINE_APP from defining a
 * GetWindowedAppCreator() the template calls, to registering the app in a table
 * the library exports. Compiling the template anyway is what produces
 * "no member named GetWindowedAppCreator in namespace rex::ui".
 *
 * So this is the same flow, looking the app up by name instead. SDL's Android
 * backend calls SDL_main from its Java activity once the surface exists;
 * SDL_main.h is what renames main() to it.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#if defined(__ANDROID__)

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <android/log.h>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include "core/app_root.h"
#include "core/logging.h"

namespace {

// Must match the identifier in src/main.cpp's REX_DEFINE_APP.
constexpr const char kAppIdentifier[] = "reblue";

} // namespace

namespace {
// Android throws away stdout and stderr, and log.redirect-stdio is blocked on
// Horizon OS, so anything written before the file logger is up is invisible.
// These few lines are the difference between a diagnosable startup and a black
// window, which is exactly what happened without them.
void Say(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, "reblue", fmt, args);
  va_end(args);
}
} // namespace

int main(int argc, char **argv) {
  // Before anything else: the default app root is the APK's native library
  // directory, which is read-only, so logs and profiles silently fail to be
  // written there. SDL knows the writable external files directory.
  if (const char *external = SDL_GetAndroidExternalStoragePath()) {
    bd::SetAppRoot(external);
    Say("app root: %s", external);
  } else {
    Say("SDL_GetAndroidExternalStoragePath() returned null; "
        "logs and profiles will not be writable");
  }
  for (int i = 1; i < argc; ++i)
    Say("argv[%d]: %s", i, argv[i]);

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  Say("cvars initialised, logging up");

  rex::ui::WindowedApp::Creator creator =
      rex::ui::WindowedApp::GetCreator(kAppIdentifier);
  if (!creator) {
    // The registration is a static object in main.cpp, so this only fires if
    // the linker dropped it - which is exactly what happens if the app ever
    // ends up in a static archive instead of an object library.
    BD_ERROR("no app registered as '{}'", kAppIdentifier);
    return EXIT_FAILURE;
  }

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

    std::unique_ptr<rex::ui::WindowedApp> app = creator(app_context);

    // Android launches with no argv worth speaking of, but honour anything an
    // intent did pass so `adb shell am start ... -e args` stays usable.
    const auto &option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    const size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    Say("app created, calling OnInitialize");
    const bool initialised = app->OnInitialize();
    Say("OnInitialize -> %s", initialised ? "true" : "false");
    result = initialised ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    Say("message loop returned %d", result);
    app->InvokeOnDestroy();
  }
  return result;
}

#endif // __ANDROID__

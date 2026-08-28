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
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_main.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include "core/logging.h"

namespace {

// Must match the identifier in src/main.cpp's REX_DEFINE_APP.
constexpr const char kAppIdentifier[] = "reblue";

} // namespace

int main(int argc, char **argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

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

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    app->InvokeOnDestroy();
  }
  return result;
}

#endif // __ANDROID__

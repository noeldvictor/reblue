package com.reblue;

import org.libsdl.app.SDLActivity;

/**
 * Activity for the re:Blue Android build.
 *
 * SDL's own SDLActivity does all the real work - surface, input, audio, and
 * calling SDL_main once the window exists. It only needs telling which
 * libraries to load and which one carries the entry point.
 *
 * Load order matters: librexruntime.so holds SDL itself and the JNI bindings
 * SDLActivity calls into, so it has to be in memory before libreblue.so, which
 * links against it.
 */
public class ReblueActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "rexruntime", "reblue" };
    }

    @Override
    protected String getMainSharedObject() {
        return getContext().getApplicationInfo().nativeLibraryDir + "/libreblue.so";
    }
}

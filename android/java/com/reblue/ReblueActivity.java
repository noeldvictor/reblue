package com.reblue;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * Activity for the re:Blue Android build.
 *
 * SDL's own SDLActivity does all the real work - surface, input, audio, and
 * calling SDL_main once the window exists. It only needs telling which
 * libraries to load, which one carries the entry point, and what to pass on the
 * command line.
 */
public class ReblueActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // Load order matters: librexruntime.so holds SDL itself and the JNI
        // bindings SDLActivity calls into, so it has to be in memory before
        // libreblue.so, which links against it.
        return new String[] { "rexruntime", "reblue" };
    }

    @Override
    protected String getMainSharedObject() {
        return getContext().getApplicationInfo().nativeLibraryDir + "/libreblue.so";
    }

    @Override
    protected String[] getArguments() {
        // There is no first-run wizard here - the desktop installer wants GTK
        // dialogs and 15 GB of disc copying, and REBLUE_BUILD_INSTALLER is off
        // in the Android build. Game data is side-loaded to the app's external
        // files directory instead, and --game_data_root points the runtime at
        // it. Its parent becomes the install root, which is where profiles,
        // saves and logs are written.
        File external = getExternalFilesDir(null);
        if (external == null) {
            return new String[0];
        }
        File game = new File(external, "game");
        if (!game.isDirectory()) {
            // Launching with nothing to load is more useful than refusing to
            // start: the log says what is missing and where it was looked for.
            android.util.Log.w("reblue", "no game data at " + game.getAbsolutePath());
            return new String[0];
        }
        // user_data_root and cache_root are supplied too. Left unset, the
        // runtime calls GetUserFolder(), which on POSIX consults XDG_DATA_HOME,
        // then HOME, then getpwuid_r - none of which mean anything for an
        // Android app, whose only writable home is this directory.
        java.util.ArrayList<String> args = new java.util.ArrayList<>();
        args.add("--game_data_root");
        args.add(game.getAbsolutePath());
        args.add("--user_data_root");
        args.add(external.getAbsolutePath());
        args.add("--cache_root");
        args.add(new File(external, "cache").getAbsolutePath());
        args.addAll(readExtraArgs(external));
        return args.toArray(new String[0]);
    }

    /**
     * Extra arguments from args.txt beside the game data, one per line.
     *
     * Rebuilding and reinstalling a 62 MB APK to change a log level is a
     * miserable way to debug. This makes any cvar reachable with a single adb
     * push, which is the difference between a two minute loop and a five second
     * one.
     */
    private java.util.List<String> readExtraArgs(File external) {
        java.util.ArrayList<String> extra = new java.util.ArrayList<>();
        File file = new File(external, "args.txt");
        if (!file.isFile()) {
            return extra;
        }
        try (java.io.BufferedReader reader =
                 new java.io.BufferedReader(new java.io.FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                // '#' comments out a line, so options can be parked rather than
                // deleted.
                if (!line.isEmpty() && !line.startsWith("#")) {
                    extra.add(line);
                }
            }
            android.util.Log.i("reblue", "args.txt added " + extra.size() + " argument(s)");
        } catch (java.io.IOException e) {
            android.util.Log.w("reblue", "could not read " + file + ": " + e);
        }
        return extra;
    }
}

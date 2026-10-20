package org.gow.shell;

import org.libsdl.app.SDLActivity;

/** SDL host activity launched by the Android launcher. */
public final class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}

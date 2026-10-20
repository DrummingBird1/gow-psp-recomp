package org.gow.shell;

import org.gow.shell.SDLActivity;

/** SDL host for the native recompilation runtime. */
public final class GameActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}

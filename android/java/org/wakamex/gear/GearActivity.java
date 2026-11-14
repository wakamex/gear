package org.wakamex.gear;

import org.libsdl.app.SDLActivity;

// gear's window on Android: SDL's activity, running main() from libmain.so (src/phone), which has SDL linked in.
public class GearActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}

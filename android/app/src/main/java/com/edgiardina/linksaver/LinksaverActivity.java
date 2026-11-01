package com.edgiardina.linksaver;

import org.libsdl.app.SDLActivity;

public class LinksaverActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}

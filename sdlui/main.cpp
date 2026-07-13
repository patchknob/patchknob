//----------------------------------------------------------------------------
//  sdlui/main.cpp -- foundation demo for the SDL2 grayscale DAW frontend.
//
//  Proves the shell + toolkit + theme + font + input: a menubar strip, a theme
//  toggle (Light <-> Midnight), transport buttons, a fader, and a proto
//  piano-roll grid drawn with the themed palette.  The gate before porting the
//  real DAW views (piano roll / tracker / arrangement / mixer / patchbay).
//----------------------------------------------------------------------------
#include "gui.h"
#include <cstdio>

using namespace ui;

// Proto "piano roll" panel: exercises the themed grid drawing the real views use.
class GridDemo : public Panel {
public:
    int hzoom = 24;                 // px per beat (wheel-zoomable)
    int rows  = 24, rh = 14;
    void draw(App& app) override {
        const Theme& t = theme();
        fill_rect(app.ren, rect, t.bg);
        for (int i = 0; i < rows; ++i) {
            int y = rect.y + i*rh;
            if (i % 2) fill_rect(app.ren, SDL_Rect{rect.x, y, rect.w, rh}, t.panel);
            hline(app.ren, rect.x, rect.x+rect.w, y, (i%12==0) ? t.accent : t.dim);
        }
        for (int b = 0; ; ++b) {
            int x = rect.x + 40 + b*hzoom;
            if (x > rect.x + rect.w) break;
            vline(app.ren, x, rect.y, rect.y+rows*rh, (b%4==0) ? t.accent : t.dim);
        }
        for (int k = 0; k < 5; ++k)
            fill_rect(app.ren, SDL_Rect{rect.x+40 + k*hzoom*2, rect.y + (6+k)*rh + 2,
                                        hzoom*2-2, rh-3}, (k==2) ? t.notesel : t.note);
        frame_rect(app.ren, rect, t.dim);
    }
    bool on_wheel(App& app, int, int dy) override {
        hzoom += dy*4; if (hzoom < 6) hzoom = 6; if (hzoom > 120) hzoom = 120;
        app.request_redraw(); return true;
    }
};

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    App app;
    if (!app.init("seq24 / SDL2 frontend -- foundation")) { app.shutdown(); return 1; }

    Panel menubar;  static Color menubg; menubg = theme().panel; menubar.bg = &menubg;
    Label mFile;  mFile.text  = " File";
    Label mView;  mView.text  = "View";
    Label mTheme; mTheme.text = "Theme";
    menubar.children = { &mFile, &mView, &mTheme };

    Button bTheme; bTheme.text = "Midnight";
    bTheme.clicked = [&]{
        bool toMid = (mode()==Mode::Light);
        set_mode(toMid ? Mode::Midnight : Mode::Light);
        bTheme.text = toMid ? "Light" : "Midnight";
        menubg = theme().panel;
        app.request_redraw();
    };

    Panel transport;
    Button bPlay, bStop, bRec;
    bPlay.text="PLAY"; bStop.text="STOP"; bRec.text="REC"; bRec.toggle=true;
    Label status; status.text="SDL2 frontend  |  wheel = zoom grid  |  Theme button toggles Light/Midnight  |  Esc = quit";
    transport.children = { &bPlay, &bStop, &bRec, &status };

    GridDemo grid;
    Fader fader; fader.value = 0.7f;

    app.roots = { &menubar, &bTheme, &transport, &grid, &fader };

    app.on_layout = [&](App& a){
        menubar.rect   = { 0, 0, a.w, 26 };
        mFile.rect  = { 6,   0, 60, 26 };
        mView.rect  = { 66,  0, 60, 26 };
        mTheme.rect = { 126, 0, 60, 26 };
        bTheme.rect = { a.w-130, 2, 120, 22 };
        transport.rect = { 0, 30, a.w, 30 };
        bPlay.rect = { 8,   32, 60, 26 };
        bStop.rect = { 72,  32, 60, 26 };
        bRec.rect  = { 136, 32, 60, 26 };
        status.rect= { 210, 32, a.w-220, 26 };
        grid.rect  = { 8, 66, a.w-120, a.h-76 };
        fader.rect = { a.w-96, 66, 40, a.h-76 };
    };

    app.run();
    app.shutdown();
    return 0;
}

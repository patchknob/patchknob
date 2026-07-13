//----------------------------------------------------------------------------
//  sdlui/views/browser/test_main.cpp -- standalone harness for BrowserView.
//
//  Mounts BrowserView as a full-window root, feeds it ~12 hardcoded
//  PluginDescriptors (NO real scan), and prints the callbacks it emits so the
//  load-as-instrument / add-as-fx wiring is observable from the terminal.
//
//    Enter / double-click : load selected as instrument
//    F2 / "Add FX" button : add selected as FX
//    type                 : filter (backspace edits, matches name + vendor)
//    up/down/pgup/pgdn     : move selection   wheel: scroll
//    T                    : toggle Light / Midnight theme
//    Esc                  : quit
//
//  SEQ24_BROWSER_SHOT=<path> renders one frame to a BMP and exits (headless).
//----------------------------------------------------------------------------
#include "browser_view.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using seq24::engine::PluginDescriptor;
using seq24::engine::PluginFormat;

static PluginDescriptor mk(PluginFormat fmt, const char* name, const char* vendor,
                           bool instr, int ai, int ao) {
    PluginDescriptor d;
    d.format = fmt;
    d.name = name;
    d.vendor = vendor;
    d.path = std::string("C:/plugins/") + name + (fmt == PluginFormat::VST3 ? ".vst3" : ".dll");
    d.uid = "";
    d.isInstrument = instr;
    d.numAudioIn = ai;
    d.numAudioOut = ao;
    return d;
}

static std::vector<PluginDescriptor> sample_plugins() {
    std::vector<PluginDescriptor> v;
    v.push_back(mk(PluginFormat::VST3, "Jup-8000 V",        "Arturia",       true,  0, 2));
    v.push_back(mk(PluginFormat::VST3, "Serum",             "Xfer Records",  true,  0, 2));
    v.push_back(mk(PluginFormat::VST2, "Massive",           "Native Instr.", true,  0, 2));
    v.push_back(mk(PluginFormat::VST3, "Diva",              "u-he",          true,  0, 2));
    v.push_back(mk(PluginFormat::VST2, "Sylenth1",          "LennarDigital", true,  0, 2));
    v.push_back(mk(PluginFormat::VST3, "Pro-Q 3",           "FabFilter",     false, 2, 2));
    v.push_back(mk(PluginFormat::VST3, "Pro-C 2",           "FabFilter",     false, 2, 2));
    v.push_back(mk(PluginFormat::VST2, "ValhallaRoom",      "Valhalla DSP",  false, 2, 2));
    v.push_back(mk(PluginFormat::VST3, "ValhallaVintageVerb","Valhalla DSP", false, 2, 2));
    v.push_back(mk(PluginFormat::VST2, "OTT",               "Xfer Records",  false, 2, 2));
    v.push_back(mk(PluginFormat::VST3, "Kontakt 7",         "Native Instr.", true,  0, 16));
    v.push_back(mk(PluginFormat::VST3, "TAL-Chorus-LX",     "TAL Software",  false, 2, 2));
    return v;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    ui::set_mode(ui::Mode::Midnight);   // start on the phosphor-green theme

    ui::App app;
    if (!app.init("BrowserView test  -  VST plugin browser (SDL2)")) {
        app.shutdown();
        return 1;
    }

    ui::BrowserView browser;
    browser.populate(sample_plugins());
    browser.set_track(3);
    browser.on_load_instrument = [](const PluginDescriptor& d) {
        printf("[on_load_instrument] %s  (%s)  %s\n",
               d.name.c_str(), d.vendor.c_str(), d.path.c_str());
        fflush(stdout);
    };
    browser.on_add_fx = [](const PluginDescriptor& d) {
        printf("[on_add_fx] %s  (%s)  %s\n",
               d.name.c_str(), d.vendor.c_str(), d.path.c_str());
        fflush(stdout);
    };

    // A tiny theme-toggle root so B/W flipping is demonstrable at runtime.
    struct KeyCatcher : public ui::Widget {
        void draw(ui::App&) override {}
        bool on_key(ui::App& a, SDL_Keycode k) override {
            if (k == SDLK_t) {
                ui::set_mode(ui::mode() == ui::Mode::Light ? ui::Mode::Midnight : ui::Mode::Light);
                a.request_redraw();
                return true;
            }
            return false;
        }
    } keys;

    app.roots = { &browser, &keys };
    app.on_layout = [&](ui::App& a) {
        browser.rect = { 0, 0, a.w, a.h };
    };

    // Headless one-shot screenshot path.
    if (const char* shot = getenv("SEQ24_BROWSER_SHOT")) {
        if (app.on_layout) app.on_layout(app);
        ui::fill_rect(app.ren, SDL_Rect{0,0,app.w,app.h}, ui::theme().bg);
        browser.draw(app);
        SDL_RenderPresent(app.ren);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, app.w, app.h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s) {
            SDL_RenderReadPixels(app.ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            printf("[shot] wrote %s\n", shot);
            fflush(stdout);
        }
        app.shutdown();
        return 0;
    }

    printf("BrowserView test running. Enter=load instr, F2=add fx, type=filter, T=theme, Esc=quit\n");
    fflush(stdout);
    app.run();
    app.shutdown();
    return 0;
}

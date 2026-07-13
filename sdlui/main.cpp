//----------------------------------------------------------------------------
//  sdlui/main.cpp -- the integrated SDL2 DAW shell.
//
//  Mounts the six ported views (arrangement / piano roll / tracker / mixer /
//  modular patchbay / plugin browser) with a tab switcher, bound to the real
//  engine: a `perform` sequencer + the live MixerGraph (via audio_app).  All
//  black-&-white / green, retained + dirty-rect.
//----------------------------------------------------------------------------
#include "gui.h"
#include "audio_app.h"
#include "engine/plugin_api.h"
#include "perform.h"
#include "sequence.h"

#include "views/piano_roll/pianoroll.h"
#include "views/tracker/tracker_view.h"
#include "views/arrange/arrange_view.h"
#include "views/mixer/mixer_view.h"
#include "views/patchbay/patch_view.h"
#include "views/browser/browser_view.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ui;

static seq24::engine::PluginDescriptor vst_desc(const char* path) {
    using namespace seq24::engine;
    PluginDescriptor d; std::string p(path?path:"");
    bool v3 = p.size()>=5 && p.compare(p.size()-5,5,".vst3")==0;
    d.format = v3 ? PluginFormat::VST3 : PluginFormat::VST2;
    d.path=p; d.name=p; d.uid=""; d.isInstrument=true; d.numAudioIn=0; d.numAudioOut=2;
    return d;
}

int main(int, char**)
{
    App app;
    if (!app.init("seq24 :: SDL DAW")) { app.shutdown(); return 1; }
    bool audio_ok = seq24::app::audio_app_init();

    // --- engine model: a sequencer with a couple of demo sequences ----------
    perform perf;
    perf.init();
    perf.new_sequence(0);
    perf.new_sequence(1);
    sequence* seq0 = perf.is_active(0) ? perf.get_sequence(0) : nullptr;

    // --- the six views, bound to the real model -----------------------------
    arrange::ArrangeView    vArrange(&perf);
    ui::PianoRoll           vPiano(seq0);
    ui::TrackerView         vTracker(seq0, 0);
    mixer::MixerView        vMixer(seq24::app::audio_app_graph());
    seq24::patchbay::PatchView vPatch;
    ui::BrowserView         vBrowser;
    vBrowser.set_track(0);
    vBrowser.on_load_instrument = [](const seq24::engine::PluginDescriptor& d){
        seq24::app::audio_app_set_track_instrument(0, d); };
    vBrowser.on_add_fx = [](const seq24::engine::PluginDescriptor& d){
        seq24::app::audio_app_add_track_fx(0, d); };
    // patchbay: adding a module opens the browser (wire later); node editor too.

    Widget* views[6] = { &vArrange, &vPiano, &vTracker, &vMixer, &vPatch, &vBrowser };
    const char* tabName[6] = { "ARRANGE","PIANO","TRACKER","MIXER","PATCH","BROWSE" };
    int current = 0;

    auto show = [&](int i){
        current = i;
        for (int k=0;k<6;++k) views[k]->visible = (k==i);
        app.request_redraw();
    };

    // --- top bar: view tabs + theme + a mini transport ----------------------
    Panel topbar; static Color topbg; topbg = theme().panel; topbar.bg = &topbg;
    Button tabs[6];
    for (int i=0;i<6;++i){ tabs[i].text=tabName[i]; tabs[i].toggle=true; tabs[i].on=(i==0);
        tabs[i].clicked=[&,i]{ for(int k=0;k<6;++k) tabs[k].on=(k==i); show(i); }; }

    Button bTheme; bTheme.text="Midnight";
    bTheme.clicked=[&]{ bool m=(mode()==Mode::Light); set_mode(m?Mode::Midnight:Mode::Light);
        bTheme.text=m?"Light":"Midnight"; topbg=theme().panel; app.request_redraw(); };

    Button bSynth,bPlay,bStop;
    bSynth.text="SYNTH"; bPlay.text="PLAY"; bStop.text="STOP";
    static std::string synthPath; { const char* v=getenv("SEQ24SDL_VST");
        synthPath = v?v:"C:\\Program Files\\Common Files\\VST3\\Jup-8000 V.vst3"; }
    bSynth.clicked=[&]{ if(audio_ok) seq24::app::audio_app_set_track_instrument(0, vst_desc(synthPath.c_str())); };
    bPlay.clicked =[&]{ if(audio_ok) seq24::app::audio_app_route_midi(0,0x90,60,110); };
    bStop.clicked =[&]{ if(audio_ok) seq24::app::audio_app_route_midi(0,0x80,60,0); };

    topbar.children = { &tabs[0],&tabs[1],&tabs[2],&tabs[3],&tabs[4],&tabs[5],
                        &bSynth,&bPlay,&bStop,&bTheme };

    // roots: top bar + all views (only current visible)
    app.roots = { &topbar };
    for (int i=0;i<6;++i){ views[i]->visible=(i==current); app.roots.push_back(views[i]); }

    app.on_layout = [&](App& a){
        int th = 28;
        topbar.rect = { 0,0,a.w,th };
        int x=4; for(int i=0;i<6;++i){ tabs[i].rect={x,2,72,th-4}; x+=74; }
        x+=10; bSynth.rect={x,2,60,th-4}; x+=64; bPlay.rect={x,2,52,th-4}; x+=56;
        bStop.rect={x,2,52,th-4};
        bTheme.rect={a.w-96,2,90,th-4};
        SDL_Rect content = { 0, th, a.w, a.h-th };
        for(int i=0;i<6;++i) views[i]->rect = content;
    };

    app.run();
    seq24::app::audio_app_shutdown();
    app.shutdown();
    return 0;
}

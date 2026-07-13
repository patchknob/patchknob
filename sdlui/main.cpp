//----------------------------------------------------------------------------
//  sdlui/main.cpp -- the integrated SDL2 DAW shell.
//
//  Tabs: ARRANGE / PIANO / TRACKER / MIXER / PATCH / BROWSE / AUTO / KEYFLW /
//  WAVE, bound to a real perform sequencer + the live MixerGraph.  SAVE/LOAD
//  persist the project; PLAY drives the sequencer through hosted VSTs (with
//  automation emitted from the automation player).  All B/W or green, retained
//  + dirty-rect (idles when static).
//----------------------------------------------------------------------------
#include "gui.h"
#include "audio_app.h"
#include "engine/plugin_api.h"
#include "engine/audio/audio_engine.h"
#include "engine/host/plugin_host.h"
#include "engine/automation/automation_player.h"
#include "engine/audioclip/audio_clip.h"
#include "perform.h"
#include "sequence.h"

#include "views/piano_roll/pianoroll.h"
#include "views/tracker/tracker_view.h"
#include "views/arrange/arrange_view.h"
#include "views/mixer/mixer_view.h"
#include "views/patchbay/patch_view.h"
#include "views/browser/browser_view.h"
#include "views/automation/automation_view.h"
#include "views/waveform/waveform_view.h"
#include "views/waveform/audio_track.h"
#include "project_io.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>

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

    // --- engine model ------------------------------------------------------
    perform perf;
    perf.init();
    perf.launch_input_thread();
    perf.launch_output_thread();
    perf.new_sequence(0);
    perf.new_sequence(1);
    sequence* seq0 = perf.is_active(0) ? perf.get_sequence(0) : nullptr;
    if (seq0) {
        seq0->set_midi_bus(0); seq0->set_midi_channel(0);
        seq0->set_length(c_ppqn * 4);
        seq0->add_note(0,        c_ppqn/2, 60);
        seq0->add_note(c_ppqn,   c_ppqn/2, 64);
        seq0->add_note(c_ppqn*2, c_ppqn/2, 67);
        seq0->add_note(c_ppqn*3, c_ppqn/2, 72);
        seq0->set_playing(true); seq0->set_dirty();
    }

    seq24::engine::AutomationPlayer autoPlayer;
    waveform::AudioTrack audioTrk;
    const seq24::engine::AudioClip* demoClip = nullptr;
    if (audio_ok) {
        audioTrk.mount(seq24::app::audio_app_graph(), 2, 48000, 512);
        demoClip = audioTrk.add_test_tone(220.0, 2.0, 48000.0);
    }

    if (const char* pt = getenv("SEQ24SDL_PLAYTEST")) {
        seq24::app::audio_app_set_track_instrument(0, vst_desc(pt));
        perf.start(false); SDL_Delay(1500);
        float pk = audio_ok ? seq24::app::audio_app_engine()->masterPeak() : -1.f;
        perf.stop();
        printf("[sdl-playtest] master peak = %.4f (%s)\n", pk, pk>0.0001f?"PLAYING":"silent");
        fflush(stdout);
        seq24::app::audio_app_shutdown(); app.shutdown();
        return pk>0.0001f ? 0 : 2;
    }

    // --- views -------------------------------------------------------------
    arrange::ArrangeView       vArrange(&perf);
    ui::PianoRoll              vPiano(seq0);
    ui::TrackerView            vTracker(seq0, 0);
    mixer::MixerView           vMixer(seq24::app::audio_app_graph());
    seq24::patchbay::PatchView vPatch;
    ui::BrowserView            vBrowser;
    automation::AutomationView vAuto(seq0, 0, &autoPlayer);
    automation::KeyFollowPanel vKey(&perf);            vKey.set_tracks({0,1});
    waveform::WaveformView     vWave;                  if (demoClip) vWave.set_clip(demoClip);

    vBrowser.set_track(0);
    vBrowser.on_load_instrument = [](const seq24::engine::PluginDescriptor& d){
        seq24::app::audio_app_set_track_instrument(0, d); };
    vBrowser.on_add_fx = [](const seq24::engine::PluginDescriptor& d){
        seq24::app::audio_app_add_track_fx(0, d); };

    // background plugin scan -> populate BROWSE
    static std::vector<seq24::engine::PluginDescriptor> g_scan;
    static std::atomic<bool> g_scanDone{false};
    vBrowser.set_scanning(true);
    std::thread([&]{
        if (auto* host = seq24::app::audio_app_host()) g_scan = host->scan({});
        g_scanDone.store(true, std::memory_order_release);
        app.request_redraw();
    }).detach();

    const int NV = 9;
    Widget* views[NV] = { &vArrange,&vPiano,&vTracker,&vMixer,&vPatch,&vBrowser,&vAuto,&vKey,&vWave };
    const char* tabName[NV] = { "ARRANGE","PIANO","TRACKER","MIXER","PATCH","BROWSE","AUTO","KEYFLW","WAVE" };
    int current = 0;
    auto show = [&](int i){ current=i; for(int k=0;k<NV;++k) views[k]->visible=(k==i); app.request_redraw(); };

    // --- top bar -----------------------------------------------------------
    Panel topbar; static Color topbg; topbg = theme().panel; topbar.bg = &topbg;
    Button tabs[NV];
    for (int i=0;i<NV;++i){ tabs[i].text=tabName[i]; tabs[i].toggle=true; tabs[i].on=(i==0);
        tabs[i].clicked=[&,i]{ for(int k=0;k<NV;++k) tabs[k].on=(k==i); show(i); }; }

    Button bTheme; bTheme.text="Midnight";
    bTheme.clicked=[&]{ bool m=(mode()==Mode::Light); set_mode(m?Mode::Midnight:Mode::Light);
        bTheme.text=m?"Light":"Midnight"; topbg=theme().panel; app.request_redraw(); };

    Button bSynth,bPlay,bStop,bSave,bLoad;
    bSynth.text="SYN"; bPlay.text="PLAY"; bStop.text="STOP"; bSave.text="SAVE"; bLoad.text="LOAD";
    static std::string synthPath; { const char* v=getenv("SEQ24SDL_VST");
        synthPath = v?v:"C:\\Program Files\\Common Files\\VST3\\Jup-8000 V.vst3"; }
    bSynth.clicked=[&]{ if(audio_ok) seq24::app::audio_app_set_track_instrument(0, vst_desc(synthPath.c_str())); };
    bPlay.clicked =[&]{ perf.start(false); app.animating=true;  app.request_redraw(); };
    bStop.clicked =[&]{ perf.stop();        app.animating=false; app.request_redraw(); };
    bSave.clicked =[&]{ save_project(perf, "project.s24"); };
    bLoad.clicked =[&]{ load_project(perf, "project.s24"); app.request_redraw(); };

    topbar.children = { &tabs[0],&tabs[1],&tabs[2],&tabs[3],&tabs[4],&tabs[5],&tabs[6],&tabs[7],&tabs[8],
                        &bSynth,&bPlay,&bStop,&bSave,&bLoad,&bTheme };

    app.roots = { &topbar };
    for (int i=0;i<NV;++i){ views[i]->visible=(i==current); app.roots.push_back(views[i]); }

    app.on_layout = [&](App& a){
        // apply background scan once ready
        static bool scanApplied=false;
        if (!scanApplied && g_scanDone.load(std::memory_order_acquire)) {
            scanApplied=true; vBrowser.set_scanning(false); vBrowser.populate(g_scan);
        }
        // emit automation while playing (coarse UI-thread pump, block-granular)
        static long prevTick=0;
        if (a.animating && seq0) {
            long cur = seq0->get_last_tick();
            if (cur != prevTick) {
                autoPlayer.advance(prevTick, cur,
                    [](int trk, unsigned id, float v){ seq24::app::audio_app_route_param(trk,id,v); },
                    [](int trk, int cc, int val){ seq24::app::audio_app_route_midi(trk,0xB0,(unsigned char)cc,(unsigned char)val); });
                prevTick = cur;
            }
        }
        int th = 26, tw = 62;
        topbar.rect = { 0,0,a.w,th };
        int x=3; for(int i=0;i<NV;++i){ tabs[i].rect={x,2,tw-2,th-4}; x+=tw; }
        x+=8;
        Button* tb[5] = { &bSynth,&bPlay,&bStop,&bSave,&bLoad };
        for (int i=0;i<5;++i){ tb[i]->rect={x,2,46,th-4}; x+=48; }
        bTheme.rect={a.w-84,2,80,th-4};
        SDL_Rect content = { 0, th, a.w, a.h-th };
        for(int i=0;i<NV;++i) views[i]->rect = content;
    };

    app.run();
    seq24::app::audio_app_shutdown();
    app.shutdown();
    return 0;
}

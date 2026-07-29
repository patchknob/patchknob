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
#include "engine/patch/patch_graph.h"
#include "engine/automation/automation_player.h"
#include "engine/rack/rack_engine.h"
#include "engine/audioclip/audio_clip.h"
#include "perform.h"
#include "sequence.h"

#include "views/piano_roll/pianoroll.h"
#include "views/tracker/tracker_view.h"
#include "views/arrange/arrange_view.h"
#include "views/mixer/mixer_view.h"
#include "views/patchbay/patch_view.h"
#include "views/sample_editor/sample_editor_view.h"
#include "views/sampler_editor/sampler_editor_view.h"
#include "engine/buzz/sampler_instrument.h"   // PatchKnob::engine::SamplerZoneInfo (read-back)
#include "engine/audioclip/wav_loader.h"
#include "tab_host.h"
#include "views/browser/browser_view.h"
#include "views/automation/automation_view.h"
#include "views/waveform/waveform_view.h"
#include "views/waveform/audio_track.h"
#include "views/audio_settings/audio_settings_view.h"
#include "views/plugin_picker/plugin_picker_view.h"
#include "views/plugin_param/plugin_param_view.h"
#include "views/mixer_node/mixer_node_view.h"
#include "views/master_mixer/master_mixer_view.h"
#include "views/pd_editor/pd_editor_view.h"
#include "views/csound_editor/csound_editor_view.h"
#include "views/rack_editor/rack_editor_view.h"
#include "plugin_editor_window.h"
#include "clip_host.h"
#include "transport_bar.h"
#include "project_io.h"

#include <map>
#include <set>
#include <cstring>
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN   // keep rpcndr.h's 'byte' away from std::byte
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod (winmm, already linked)
#include <commdlg.h>
#endif

using namespace ui;

static PatchKnob::engine::PluginDescriptor vst_desc(const char* path) {
    using namespace PatchKnob::engine;
    PluginDescriptor d; std::string p(path?path:"");
    bool v3 = p.size()>=5 && p.compare(p.size()-5,5,".vst3")==0;
    d.format = v3 ? PluginFormat::VST3 : PluginFormat::VST2;
    d.path=p; d.name=p; d.uid=""; d.isInstrument=true; d.numAudioIn=0; d.numAudioOut=2;
    return d;
}

static bool choose_project_file(bool save, const std::string& current,
                                std::string& selected)
{
#ifdef _WIN32
    char path[MAX_PATH] = {};
    if (!current.empty() && current.size() < sizeof(path))
        std::memcpy(path, current.c_str(), current.size());

    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetForegroundWindow();
    dialog.lpstrFilter = "PatchKnob projects (*.s24)\0*.s24\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path);
    dialog.lpstrDefExt = "s24";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog)))
        return false;
    selected = path;
    return true;
#else
    (void)save;
    selected = current.empty() ? "project.s24" : current;
    return true;
#endif
}

// Open-file dialog for a WAV sample (sampler browser).  Returns false if cancelled.
static bool choose_wav_file(std::string& selected)
{
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetForegroundWindow();
    dialog.lpstrFilter = "WAV samples (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path);
    dialog.lpstrDefExt = "wav";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&dialog)) return false;
    selected = path;
    return true;
#else
    (void)selected; return false;
#endif
}

static std::string project_display_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

int main(int, char**)
{
#ifdef _WIN32
    // 1 ms system timer resolution for the WHOLE process.  Without this every
    // sleep/wait (sequencer pacing, MIDI drain cadence, SDL delays) quantizes
    // to the default ~15.6 ms tick -- the same magnitude as a 32nd note at
    // 500 BPM (15 ms), which audibly wrecked the clock.
    timeBeginPeriod(1);
    atexit([]{ timeEndPeriod(1); });
#endif
    App app;
    if (!app.init("PatchKnob :: SDL DAW")) { app.shutdown(); return 1; }
    bool audio_ok = PatchKnob::app::audio_app_init();

    // Headless clock acceptance test (32nds @ 500 BPM must be sample-exact):
    // PATCHKNOB_CLOCK_SELFTEST=1 runs it and exits with 0 iff perfectly steady.
    if (audio_ok && std::getenv("PATCHKNOB_CLOCK_SELFTEST")) {
        double dev = PatchKnob::app::audio_app_clock_selftest();
        PatchKnob::app::audio_app_shutdown();
        app.shutdown();
        return (dev == 0.0) ? 0 : 2;
    }
    // Headless loop acceptance test (looped bar of 32nds, sample-exact wraps).
    if (audio_ok && std::getenv("PATCHKNOB_LOOP_SELFTEST")) {
        double dev = PatchKnob::app::audio_app_loop_selftest();
        PatchKnob::app::audio_app_shutdown();
        app.shutdown();
        return (dev == 0.0) ? 0 : 2;
    }
    // Headless freeze proofs: sine capture + mixer route, then a full synth
    // voice (MIDI-CV->VCO->VCA + ADSR) playing 16 notes, all frozen offline.
    if (audio_ok && std::getenv("PATCHKNOB_FREEZE_SELFTEST")) {
        PatchKnob::app::audio_app_freeze_selftest();
        float voice = PatchKnob::app::audio_app_freeze_voice_selftest();
        PatchKnob::app::audio_app_shutdown();
        app.shutdown();
        return (voice > 0.001f) ? 0 : 2;
    }

    // --- engine model ------------------------------------------------------
    perform perf;
    perf.init();
    perf.launch_input_thread();
    perf.launch_output_thread();
    sequence* seq0 = nullptr;

    PatchKnob::engine::AutomationPlayer autoPlayer;

    if (const char* pt = getenv("PATCHKNOBSDL_PLAYTEST")) {
        PatchKnob::app::audio_app_set_track_instrument(0, vst_desc(pt));
        perf.start(false); SDL_Delay(1500);
        float pk = audio_ok ? PatchKnob::app::audio_app_engine()->masterPeak() : -1.f;
        perf.stop();
        printf("[sdl-playtest] master peak = %.4f (%s)\n", pk, pk>0.0001f?"PLAYING":"silent");
        fflush(stdout);
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return pk>0.0001f ? 0 : 2;
    }
    if (const char* pt = getenv("PATCHKNOBSDL_PATCHTEST")) {
        float pk = PatchKnob::app::audio_app_patch_selftest(pt);
        printf("[sdl-patchtest] peak=%.4f\n", pk); fflush(stdout);
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return pk>0.0001f ? 0 : 2;
    }

    // --- views -------------------------------------------------------------
    arrange::ArrangeView       vArrange(&perf);
    ui::PianoRoll              vPiano(seq0);
    ui::TrackerView            vTracker(seq0, 0);
    vPiano.set_preview(true);   // key-strip clicks preview through the selected instrument
    mixer::MixerView           vMixer(PatchKnob::app::audio_app_graph());
    PatchKnob::patchbay::PatchView vPatch;
    samped::SampleEditorView   vSampleEd;      // Ableton-style warp / sample editor
    ui::BrowserView            vBrowser;
    automation::AutomationView vAuto(seq0, 0, &autoPlayer);
    automation::KeyFollowPanel vKey(&perf);            vKey.set_tracks({});
    waveform::WaveformView     vWave;

    vBrowser.set_track(0);
    vBrowser.on_load_instrument = [](const PatchKnob::engine::PluginDescriptor& d){
        PatchKnob::app::audio_app_set_track_instrument(0, d); };
    vBrowser.on_add_fx = [](const PatchKnob::engine::PluginDescriptor& d){
        PatchKnob::app::audio_app_add_track_fx(0, d); };

    // Plugin inventory, cached on disk so we don't re-probe every boot.  Load the
    // cache if present (instant); otherwise scan once and write it.  "Rescan
    // Plugins" (View menu) forces a fresh probe + cache rewrite.
    static std::vector<PatchKnob::engine::PluginDescriptor> g_scan;
    static std::atomic<bool> g_scanDone{false};    // any inventory available yet
    static std::atomic<bool> g_scanReady{false};   // fresh data waiting to apply
    auto run_scan = [&](bool forceRescan){
        vBrowser.set_scanning(true);
        std::thread([&,forceRescan]{
            if (auto* host = PatchKnob::app::audio_app_host()) {
                host->setCachePath("PatchKnob_plugins.cache");
                host->setProbeTimeoutMs(5000);         // cap a hung probe at 5s (was 15s)
                std::vector<PatchKnob::engine::PluginDescriptor> res;
                if (!forceRescan && host->loadCache(res) && !res.empty())
                    g_scan = std::move(res);           // cache hit: no probing
                else
                    g_scan = host->scan({});           // scan() writes the cache
            }
            g_scanDone.store(true, std::memory_order_release);
            g_scanReady.store(true, std::memory_order_release);
            app.request_redraw();
        }).detach();
    };
    run_scan(false);

    const int NV = 9;
    Widget* views[NV] = { &vArrange,&vPiano,&vTracker,&vMixer,&vPatch,&vBrowser,&vAuto,&vKey,&vWave };
    enum { V_ARRANGE=0, V_PIANO=1, V_TRACKER=2, V_MIXER=3, V_PATCH=4,
           V_BROWSE=5, V_AUTO=6, V_KEY=7, V_WAVE=8 };
    // Workspace tabs.  PIANO / TRACKER are NOT tabs -- they open as clip editors
    // from the ARRANGE timeline (double-click a clip, or right-click -> Add).
    // PATCH (modular patchbay) is a bottom-docked WINDOW, not a tab; PIANO /
    // TRACKER open as floating clip-editor windows.
    struct WS { int idx; const char* name; };
    // MIXER is no longer a tab -- it's a patchable Mixer node (right-click -> Open
    // Mixer).  PIANO / TRACKER / PATCH are windows/docks too.
    static const WS ws[] = { {V_ARRANGE,"ARRANGE"},
                             {V_BROWSE,"BROWSE"},{V_AUTO,"AUTO"},{V_KEY,"KEYFLW"},{V_WAVE,"WAVE"} };
    const int NWS = (int)(sizeof(ws)/sizeof(ws[0]));
    int  current = 0;
    bool editorActive = false;                 // (kept for the back button; unused now)
    std::function<void()> toggle_patch;        // assigned once patchWin exists
    std::function<void()> open_transport_win;  // assigned once transportWin exists
    std::function<void()> open_master_mixer;   // assigned once masterMixWin exists
    auto show = [&](int i){ current=i; for(int k=0;k<NV;++k) views[k]->visible=(k==i);
        app.request_redraw(); };

    static std::string synthPath; { const char* v=getenv("PATCHKNOBSDL_VST");
        synthPath = v?v:"C:\\Program Files\\Common Files\\VST3\\Jup-8000 V.vst3"; }

    // --- realtime-bounce helper (RENDER menu item) -------------------------
    static std::atomic<bool> g_rendering{false};
    auto do_render = [&]{
        if (!audio_ok || g_rendering.load()) return;
        double secs = 4.0;
        for (int s=0;s<c_max_sequence;++s) if (perf.is_active(s)) {
            sequence* sq = perf.get_sequence(s);
            if (sq) { double b = (double)sq->get_length()/(double)c_ppqn;
                      double t = b * 60.0 / 120.0; if (t>secs) secs=t; } }
        g_rendering.store(true);
        std::thread([&,secs]{
            if (PatchKnob::app::audio_app_capture_begin(secs)) {
                perf.start(false); PatchKnob::app::audio_app_patch_set_playing(true);
                SDL_Delay((Uint32)(secs*1000.0)+200);
                perf.stop(); PatchKnob::app::audio_app_patch_set_playing(false);
                PatchKnob::app::audio_app_capture_end_wav("render.wav");
            }
            g_rendering.store(false); app.request_redraw();
        }).detach();
    };

    // --- top menu bar (File / View / Audio / Help) -------------------------
    static Color topbg; topbg = theme().panel;
    std::function<void()> refresh_loaded_project;
    std::function<void()> new_project;
    // Freeze save/restore (bound after g_frozen is declared): collect_freezes
    // snapshots g_frozen for the FRZ section; apply_loaded_freezes re-shows the
    // frozen lanes (tint + waveform) after a load and repopulates g_frozen.
    std::function<std::vector<ProjectFreezeRecord>()> collect_freezes;
    std::function<void()> apply_loaded_freezes;
    // Mirror the engine PatchGraph's connections (incl. the instrument->mixer
    // wire made by master_connect_instrument) into the patchbay UI, so the cord
    // is VISIBLE.  Forward-declared: assigned once refresh_master_ui exists.
    std::function<void()> sync_patch_connections;
    std::string projectPath;
    std::string projectStatus = "Project: Untitled";
    auto update_project_title = [&]{
        const std::string name = projectPath.empty() ? "Untitled" : project_display_name(projectPath);
        SDL_SetWindowTitle(app.window, ("PatchKnob :: SDL DAW - " + name).c_str());
    };
    auto save_to_project = [&](const std::string& path) {
        vTracker.commit_fx();   // flush the live pattern's FX edits into its sequence
        std::vector<ProjectPatchNodePosition> patchLayout;
        patchLayout.reserve(vPatch.nodes().size());
        for (const auto& node : vPatch.nodes())
            patchLayout.push_back(ProjectPatchNodePosition{(uint32_t)node.id, node.x, node.y});
        std::vector<ProjectFreezeRecord> freezes = collect_freezes ? collect_freezes()
                                                 : std::vector<ProjectFreezeRecord>{};
        if (!save_project(perf, path, patchLayout, freezes, &autoPlayer)) {
            projectStatus = "Save failed: " + std::string(project_io_last_error());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Save project failed",
                                     project_io_last_error(), app.window);
        } else {
            projectPath = path;
            projectStatus = "Saved: " + path;
            update_project_title();
        }
        app.request_redraw();
    };
    auto save_as_project = [&]{
        std::string path = projectPath;
        if (choose_project_file(true, path, path)) save_to_project(path);
    };
    auto open_project = [&]{
        std::string path = projectPath;
        if (!choose_project_file(false, path, path)) return;
        if (!load_project(perf, path, &autoPlayer)) {
            projectStatus = "Open failed: " + std::string(project_io_last_error());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open project failed",
                                     project_io_last_error(), app.window);
        } else {
            projectPath = path;
            projectStatus = "Opened: " + path;
            update_project_title();
            if (refresh_loaded_project) refresh_loaded_project();
        }
        app.request_redraw();
    };
    MenuBar menubar;
    Menu mFile; mFile.title = "File";
    mFile.items = {
        { "New",          [&]{ if (new_project) new_project(); } },
        { "Open...",      open_project },
        { "Save",         [&]{ if (projectPath.empty()) save_as_project(); else save_to_project(projectPath); } },
        { "Save As...",   save_as_project },
        { "", nullptr, true },
        { "Render to WAV...", [&]{ do_render(); } },
        { "", nullptr, true },
        { "Quit",         [&]{ app.running = false; } },
    };
    Menu mView; mView.title = "View";
    mView.items = {
        { "Light / Midnight", [&]{ bool m=(mode()==Mode::Light);
              set_mode(m?Mode::Midnight:Mode::Light); topbg=theme().panel; app.request_redraw(); } },
        { "", nullptr, true },
        { "Patchbay (bottom dock)", [&]{ if (toggle_patch) toggle_patch(); } },
        { "", nullptr, true },
        { "Rescan Plugins", [&]{ run_scan(true); } },
        { "Load Synth on Track 0", [&]{ if(audio_ok)
              PatchKnob::app::audio_app_set_track_instrument(0, vst_desc(synthPath.c_str())); } },
    };
    { MenuItem mi; mi.label = "Modular Render (patch audio)"; mi.check = true;
      mi.checked = []{ return PatchKnob::app::audio_app_modular(); };
      mi.action  = [&]{ if (audio_ok) { PatchKnob::app::audio_app_set_modular(!PatchKnob::app::audio_app_modular());
                        app.request_redraw(); } };
      mView.items.insert(mView.items.begin()+3, mi); }   // just under "Patchbay"
    { MenuItem mi; mi.label = "Multi-thread Audio (multi-core)"; mi.check = true;
      mi.checked = []{ return PatchKnob::app::audio_app_multithreaded(); };
      mi.action  = [&]{ if (audio_ok) { PatchKnob::app::audio_app_set_multithreaded(!PatchKnob::app::audio_app_multithreaded());
                        app.request_redraw(); } };
      mView.items.insert(mView.items.begin()+4, mi); }
    // Main-view switch (the old workspace tabs, moved into the menu).
    { MenuItem sep; sep.enabled=false; sep.separator=true; mView.items.push_back(sep); }
    for (int i = 0; i < NWS; ++i) {
        WS w = ws[i];
        MenuItem mi; mi.label = std::string("Show ") + w.name;
        mi.action = [&, w]{ show(w.idx); };
        mView.items.push_back(mi);
    }
    { MenuItem sep; sep.enabled=false; sep.separator=true; mView.items.push_back(sep); }
    { MenuItem mi; mi.label = "Transport (large window)";
      mi.action = [&]{ if (open_transport_win) open_transport_win(); };
      mView.items.push_back(mi); }
    { MenuItem mi; mi.label = "Master Mixer";
      mi.action = [&]{ if (open_master_mixer) open_master_mixer(); };
      mView.items.push_back(mi); }
    Menu mHelp; mHelp.title = "Help";
    { MenuItem mi; mi.label="PatchKnob DAW"; mi.enabled=false; mHelp.items.push_back(mi); }

    // --- floating window system + the Audio Settings window ----------------
    WindowManager wm;
    audioui::AudioSettingsView audioSettings; audioSettings.audio_ok = audio_ok;
    Window audioWin; audioWin.title = "Audio Settings"; audioWin.content = &audioSettings;
    audioWin.rect = { 120, 90, 360, 380 }; audioWin.visible = false;
    auto open_window = [&](Window& w, int dw, int dh){
        w.rect.w = dw; w.rect.h = dh;
        w.rect.x = (app.w - dw)/2; w.rect.y = (app.h - dh)/2;
        w.visible = true; wm.raise(&w); app.request_redraw();
    };

    Menu mAudio; mAudio.title = "Audio";
    mAudio.items = {
        { "Settings...", [&]{ open_window(audioWin, 380, 400); } },
    };
    menubar.menus = { mFile, mView, mAudio, mHelp };

    // --- transport bars (Ardour-style shaped-icon buttons + BBT clock + tempo).
    // A COMPACT strip replaces the old button row; a FULL panel opens in a window
    // from View -> Transport (large).  Both drive the kitchensink musical clock +
    // the PatchKnob sequencer through one set of callbacks.
    static bool g_recArmed = false, g_loopOn = false;
    // Live-record: toggle_record's body is assigned later (it needs the track
    // registry + master-mixer sync defined below); the REC button calls it via
    // this handle.  drain_record accumulates captured events every frame.
    std::function<void()> toggle_record;
    std::function<void()> drain_record;
    ui::TransportBar transportSmall; transportSmall.compact = true;
    ui::TransportBar transportFull;  transportFull.compact  = false;
    // SONG mode (default): playback is gated by the timeline triggers -- a clip
    // sounds ONLY where it is placed in the arrange.  LIVE mode: armed patterns
    // loop freely (classic PatchKnob jamming).  Toggled on the transport bar.
    static bool g_songMode = true;
    auto wire_transport = [&](ui::TransportBar& tb){
        tb.on_play  = [&]{ perf.start(g_songMode); PatchKnob::app::audio_app_patch_set_playing(true);
                           app.animating=true;  app.request_redraw(); };
        tb.on_stop  = [&]{ perf.stop();       PatchKnob::app::audio_app_patch_set_playing(false);
                           app.animating=false; app.request_redraw(); };
        tb.on_to_start = [&]{ perf.set_orig_ticks(0); PatchKnob::app::audio_app_transport_locate(0);
                              app.request_redraw(); };
        tb.on_rewind = [&]{ double b = PatchKnob::app::audio_app_sample_to_beats(PatchKnob::app::audio_app_transport_sample());
                            double tgt = std::floor(b/4.0)*4.0; if (tgt >= b-0.01) tgt -= 4.0; if (tgt < 0) tgt = 0;
                            PatchKnob::app::audio_app_transport_locate(PatchKnob::app::audio_app_beats_to_sample(tgt));
                            app.request_redraw(); };
        tb.on_ffwd  = [&]{ double b = PatchKnob::app::audio_app_sample_to_beats(PatchKnob::app::audio_app_transport_sample());
                            double tgt = std::floor(b/4.0)*4.0 + 4.0;
                            PatchKnob::app::audio_app_transport_locate(PatchKnob::app::audio_app_beats_to_sample(tgt));
                            app.request_redraw(); };
        tb.on_to_end = [&]{ long endt = perf.get_max_trigger();
                            double b = (double)endt / (double)c_ppqn;   // ticks -> quarter beats
                            PatchKnob::app::audio_app_transport_locate(PatchKnob::app::audio_app_beats_to_sample(b));
                            app.request_redraw(); };
        tb.on_rec   = [&]{ toggle_record(); app.request_redraw(); };  // live MIDI record -> timeline clip
        tb.on_loop  = [&]{ g_loopOn = !g_loopOn; perf.set_looping(g_loopOn); app.request_redraw(); };
        // ONE tempo write path: perform::set_bpm funnels every bpm write (UI,
        // file load, hotkeys) into the engine's kitchensink tempo map.
        // Fractional BPM survives end-to-end now.
        tb.on_tempo = [&](double bpm){ perf.set_bpm(bpm); app.request_redraw(); };
        tb.on_mode  = [&]{ g_songMode = !g_songMode; app.request_redraw(); };
        tb.is_song  = [&]{ return g_songMode; };
        tb.is_rolling = [&]{ return PatchKnob::app::audio_app_transport_rolling(); };
        tb.is_rec     = [&]{ return g_recArmed; };
        tb.is_loop    = [&]{ return g_loopOn; };
        tb.get_tempo  = [&]{ return PatchKnob::app::audio_app_tempo(); };
        tb.get_bbt    = [&](int& ba,int& be,int& ti){ PatchKnob::app::audio_app_transport_bbt(&ba,&be,&ti); };
    };
    wire_transport(transportSmall);
    wire_transport(transportFull);
    Window transportWin; transportWin.title = "Transport";
    transportWin.content = &transportFull; transportWin.visible = false;
    transportWin.rect = { 200, 120, 460, 130 };
    open_transport_win = [&]{ open_window(transportWin, 460, 130); };

    // --- unified mixer STRIP view (metered faders + mute/solo/pan) used for BOTH
    // a regular Mixer module AND the singleton Master Mixer module.  Bound to
    // `g_curMixerNode`, which the "Open Mixer" action sets.  The last strip is the
    // node's MASTER (output gain + master VU).
    static int  g_curMixerNode = 0;
    static bool g_busSolo[16] = { false }, g_busMute[16] = { false };
    auto recompute_mutes = [&]{
        int n = PatchKnob::app::audio_app_mixer_channels(g_curMixerNode); if (n > 16) n = 16;
        bool anySolo = false; for (int i = 0; i < n; ++i) if (g_busSolo[i]) anySolo = true;
        for (int i = 0; i < n; ++i)
            PatchKnob::app::audio_app_mixer_set_mute(g_curMixerNode, i, anySolo ? !g_busSolo[i] : g_busMute[i]);
    };
    // Mixer starts EMPTY (no pre-loaded channels): the master mixer node has 0
    // tracks until an instrument/audio track adds one.  bus_count grows as tracks
    // are created (refresh_master_ui / on_open_mixer sync it to the node).
    mixerui::MasterMixerView mixerStrip; mixerStrip.bus_count = 0; mixerStrip.aux_count = 2;
    {
        mixerStrip.get_label = [&](int i){ return i < mixerStrip.bus_count ? std::to_string(i+1) : std::string("MASTER"); };
        mixerStrip.get_gain  = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_gain(g_curMixerNode,i)
                                                                           : PatchKnob::app::audio_app_mixer_master_gain(g_curMixerNode); };
        mixerStrip.set_gain  = [&](int i,float v){ if (i < mixerStrip.bus_count) PatchKnob::app::audio_app_mixer_set_gain(g_curMixerNode,i,v);
                                                   else PatchKnob::app::audio_app_mixer_set_master_gain(g_curMixerNode,v); };
        mixerStrip.get_level = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_vu(g_curMixerNode,i)
                                                                           : PatchKnob::app::audio_app_mixer_master_vu(g_curMixerNode); };
        mixerStrip.get_mute  = [&](int i){ return i < mixerStrip.bus_count ? g_busMute[i] : false; };
        mixerStrip.get_solo  = [&](int i){ return i < mixerStrip.bus_count ? g_busSolo[i] : false; };
        mixerStrip.toggle_mute = [&](int i){ if (i < mixerStrip.bus_count){ g_busMute[i] = !g_busMute[i]; recompute_mutes(); }
                                             app.request_redraw(); };
        mixerStrip.toggle_solo = [&](int i){ if (i < mixerStrip.bus_count){ g_busSolo[i] = !g_busSolo[i]; recompute_mutes(); }
                                             app.request_redraw(); };
        mixerStrip.get_pan   = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_pan(g_curMixerNode,i) : 0.f; };
        mixerStrip.set_pan   = [&](int i,float p){ if (i < mixerStrip.bus_count) PatchKnob::app::audio_app_mixer_set_pan(g_curMixerNode,i,p); };
        mixerStrip.get_insert = [](int){ return std::string(); };
        mixerStrip.on_insert  = [&](int){ app.request_redraw(); };
        mixerStrip.get_send   = [](int,int){ return 0.f; };
        mixerStrip.set_send   = [](int,int,float){};
    }
    Window mixerStripWin; mixerStripWin.title = "Mixer"; mixerStripWin.content = &mixerStrip;
    mixerStripWin.visible = false; mixerStripWin.rect = { 60, 90, 700, 440 };
    auto open_mixer_for = [&](int node){
        g_curMixerNode = node;
        // Show exactly as many channels as the node has (0 = empty, only MASTER).
        int n = PatchKnob::app::audio_app_mixer_channels(node); if (n < 0) n = 0; if (n > 16) n = 16;
        mixerStrip.bus_count = n;
        for (int i = 0; i < 16; ++i) { g_busMute[i] = (i < n) && PatchKnob::app::audio_app_mixer_mute(node, i); g_busSolo[i] = false; }
        bool isMaster = (node == PatchKnob::app::audio_app_master_mixer_node());
        mixerStripWin.title = isMaster ? "Master Mixer" : "Mixer";
        open_window(mixerStripWin, 720, 460);
    };
    open_master_mixer = [&]{ open_mixer_for(PatchKnob::app::audio_app_master_mixer_node()); };

    // Clip editors live in floating windows.  Opening a clip rebinds the editor
    // to that sequence and pops (or raises) its window over the workspace.
    // Plugin editor windows: one embedded NATIVE GUI at a time (framed by an SDL
    // Window that positions the child HWND/X11 window) + a portable SDL PARAMETER
    // panel (works on a windowless framebuffer).  Both are ui::Windows.
    static void* g_guiHandle = nullptr; static int g_guiNode = -1;
    paramui::PluginParamView paramView;
    Window paramWin; paramWin.title = "Parameters"; paramWin.content = &paramView; paramWin.visible = false;
    // (the old per-node MixerNodeView is replaced by the unified strip view above)
    // Pure Data node editor: an SDL patch editor over the node's .pd file; saving
    // regenerates the .pd and reloads the live libpd instance (g_pdNode).
    static int g_pdNode = -1; static std::string g_pdPath;
    pdui::PdEditorView pdEditor;
    pdEditor.on_changed = [&]{ if (g_pdNode >= 0 && !g_pdPath.empty())
                                   PatchKnob::app::audio_app_pd_load(g_pdNode, g_pdPath.c_str()); };
    Window pdWin; pdWin.title = "Pd"; pdWin.content = &pdEditor; pdWin.visible = false;
    // VCV-Rack-style modular editor: draws + edits the RackNode's live engine.
    rackui::RackEditorView rackEditor;
    Window rackWin; rackWin.title = "Modular"; rackWin.content = &rackEditor; rackWin.visible = false;
    // Track the rack node open in the editor + its last audio-I/O signature so the
    // patcher can grow/shrink the RackNode's ports as Audio-In/Out modules load.
    static int g_rackNode = -1, g_rackIOsig = -1;
    static int g_pdIOsig = -1;   // PdNode adc~/dac~ port signature (re-mirror on change)
    // Csound CSD text editor: edits a CsoundNode's .csd; Ctrl+E recompiles + updates
    // the node's in/out ports from the CSD header (nchnls / nchnls_i).
    static int g_csoundNode = -1;
    ui::CsoundEditorView csoundEditor;
    Window csoundWin; csoundWin.title = "Csound"; csoundWin.content = &csoundEditor; csoundWin.visible = false;
    ui::SamplerEditorView vSamplerEd;
    Window samplerWin; samplerWin.title = "Sampler"; samplerWin.content = &vSamplerEd; samplerWin.visible = false;
    // Per-sampler-node multisample zones (AudioClip copies kept for display + re-push).
    static std::map<int, std::vector<ui::SamplerZone>> g_samplerZones;
    static std::map<int, ui::SamplerEnvSet> g_samplerEnv;   // per node: 5 modulation envelopes
    Window guiWin;   guiWin.title = "Plugin GUI"; guiWin.content = nullptr; guiWin.visible = false;
    guiWin.on_close = [&]{ if (g_guiHandle) { PatchKnob::hostwin::editor_close(g_guiHandle);
                           g_guiHandle = nullptr; g_guiNode = -1; } };
    // Maps an arrange sequence index -> its master-mixer track / MIDI route.
    // Keep this near the instrument picker so dropdown changes can update the
    // same source of truth used by playback, freeze, lasso, and timeline lanes.
    static std::map<int,int> g_seqToTrack;
    // The clip's instrument list = the playable nodes currently in the patcher,
    // by name: VST INSTRUMENTS plus Pure Data and Modular (Rack) patches -- all
    // honor a MIDI channel filter, so selecting one routes the clip's MIDI to it.
    auto instrument_nodes = [&]{
        std::vector<std::pair<int,std::string>> v;   // (patch node id, name)
        for (const auto& n : vPatch.nodes())
            if (n.category == "Instrument" || n.category == "Pure Data" ||
                n.category == "Modular (Rack)" || n.category == "Sampler")
                v.push_back({ (int)n.id, n.name });
        return v;
    };
    // Give a node the lowest MIDI channel (0..15) not already used by ANOTHER
    // instrument node, so every instrument is on a distinct channel and a clip
    // can address exactly one.  Single source of truth -> no channel collisions
    // (which previously made a VST and a Rack share a channel and become
    // indistinguishable in the clip instrument picker).
    auto assign_instr_channel = [&](int nodeId) -> int {
        bool used[16] = { false };
        for (const auto& p : instrument_nodes()) {
            if (p.first == nodeId) continue;
            int c = PatchKnob::app::audio_app_patch_node_channel(p.first);
            if (c >= 0 && c < 16) used[c] = true;
        }
        int ch = 0; while (ch < 16 && used[ch]) ++ch;
        if (ch >= 16) ch = 0;                 // >16 instruments: wrap (rare)
        PatchKnob::app::audio_app_patch_set_node_channel(nodeId, ch);
        return ch;
    };
    auto instr_current = [&](sequence* s) -> std::string {
        if (!s) return "-";
        int ch = s->get_midi_channel();
        for (const auto& p : instrument_nodes())
            if (PatchKnob::app::audio_app_patch_node_channel(p.first) == ch) return p.second;
        return "(none)";
    };
    // Make a selected instrument actually PLAYABLE: ensure MIDI-In -> instrument
    // (so the clip's notes reach it) and instrument -> Audio Out (so you hear it)
    // are wired.  Only adds the missing links (a user's mixer routing is kept),
    // and wiring into Out auto-enables the modular render path (see on_connect).
    // Each instrument node gets its own master-mixer AUDIO track (one source of
    // truth for its mixer channel), so its audio flows MidiIn->inst->mixer
    // inlet->master->Out and the per-track mixer strip is live.
    static std::map<int,int> g_instrTrack;   // instrument nodeId -> mixer track
    auto ensure_instr_wired = [&](int nodeId){
        if (nodeId < 0) return;
        PatchKnob::app::audio_app_set_modular(true);
        auto it = g_instrTrack.find(nodeId);
        int track;
        if (it == g_instrTrack.end()) {
            track = PatchKnob::app::audio_app_master_add_track(0);   // audio inlet/outlet
            if (track < 0) return;
            g_instrTrack[nodeId] = track;
        } else {
            track = it->second;
        }
        // Wire MidiIn -> instrument and instrument audio -> its mixer inlet
        // (idempotent-ish at the engine level; the mixer sums to Out already).
        PatchKnob::app::audio_app_master_connect_instrument(track, nodeId);
        // Reflect the new mixer inlet + the fresh wires in the patchbay UI so the
        // instrument->mixer cord is visible (forward-declared; safe once bound).
        if (sync_patch_connections)   sync_patch_connections();
    };
    auto instr_pick = [&](sequence* s, int idx){
        if (!s) return;
        auto ins = instrument_nodes();
        if (idx < 0 || idx >= (int)ins.size()) return;
        const int nodeId = ins[idx].first;
        int ch = PatchKnob::app::audio_app_patch_node_channel(nodeId);
        if (ch < 0) ch = assign_instr_channel(ins[idx].first);   // unique channel
        ensure_instr_wired(nodeId);                               // make it audible
        auto trIt = g_instrTrack.find(nodeId);
        if (trIt != g_instrTrack.end()) {
            const int track = trIt->second;
            s->set_midi_bus((char)track);
            for (int si = 0; si < c_max_sequence; ++si) {
                if (perf.is_active(si) && perf.get_sequence(si) == s) {
                    g_seqToTrack[si] = track;
                    break;
                }
            }
        }
        s->set_midi_channel((char)ch);
        s->set_dirty();
        app.request_redraw();
    };

    auto tracker_fx_label = [](const std::string& s, const char* fallback)->std::string {
        std::string out;
        for (size_t i = 0; i < s.size() && out.size() < 4; ++i) {
            char c = s[i];
            if (c > ' ' && c != '>' && c != '/' && c != '\\') out.push_back(c);
        }
        if (out.empty()) out = fallback ? fallback : "P";
        if (out.size() > 4) out.resize(4);
        return out;
    };
    vTracker.on_list_fx_targets = [&]{
        std::vector<ui::FxBinding> out;
        for (const auto& n : vPatch.nodes()) {
            const int nodeId = (int)n.id;
            if (n.category == "Instrument" || n.category == "Effect" || n.category == "Sampler") {
                PatchKnob::engine::IPluginInstance* inst = PatchKnob::app::audio_app_patch_node_instance(nodeId);
                if (!inst) continue;
                int count = inst->paramCount();
                for (int i = 0; i < count; ++i) {
                    PatchKnob::engine::ParamInfo pi = inst->paramInfo(i);
                    ui::FxBinding b;
                    b.type = ui::FX_VST_PARAM;
                    b.target = ui::FX_TARGET_PATCH_PLUGIN_PARAM;
                    b.pid = pi.id;
                    b.node = nodeId;
                    b.label = tracker_fx_label(pi.name, "P");
                    b.name = n.name + " > " + pi.name;
                    out.push_back(b);
                }
            } else if (n.category == "Modular (Rack)") {
                rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine(nodeId);
                if (!eng) continue;
                for (int mi = 0; mi < eng->moduleCount(); ++mi) {
                    rackx::RackModule* mod = eng->moduleAt(mi);
                    if (!mod || !mod->mod) continue;
                    for (int pi = 0; pi < (int)mod->mod->paramQuantities.size(); ++pi) {
                        const rack::engine::ParamQuantity& q = mod->mod->paramQuantities[pi];
                        std::string pname = q.name.empty()
                                          ? (std::string("Param ") + std::to_string(pi))
                                          : q.name;
                        ui::FxBinding b;
                        b.type = ui::FX_VST_PARAM;
                        b.target = ui::FX_TARGET_RACK_PARAM;
                        b.pid = (q.paramId >= 0) ? (unsigned int)q.paramId : (unsigned int)pi;
                        b.node = nodeId;
                        b.module = mod->id;
                        b.min_value = q.minValue;
                        b.max_value = q.maxValue;
                        b.label = tracker_fx_label(pname, "R");
                        b.name = n.name + " > " + mod->name + " > " + pname;
                        out.push_back(b);
                    }
                }
            }
        }
        return out;
    };

    // Clip windows host the editor UNDER an instrument-selector strip.
    ClipHost pianoHost, trackerHost;
    pianoHost.view = &vPiano; trackerHost.view = &vTracker;
    auto opt_names = [instrument_nodes]{ std::vector<std::string> v;
        for (const auto& p : instrument_nodes()) v.push_back(p.second); return v; };
    pianoHost.options = opt_names;  trackerHost.options = opt_names;
    pianoHost.current = [&]{ return instr_current(vPiano.get_sequence()); };
    trackerHost.current = [&]{ return instr_current(vTracker.get_sequence()); };
    pianoHost.on_select   = [&](int idx){ instr_pick(vPiano.get_sequence(), idx); };
    trackerHost.on_select = [&](int idx){ instr_pick(vTracker.get_sequence(), idx); };

    // Instrument selection is gone from the clip editors: an instrument owns a
    // mixer channel, so the editor edits the sequence directly (no strip).
    (void)pianoHost; (void)trackerHost;
    Window pianoWin;   pianoWin.title  = "Piano Roll"; pianoWin.content  = &vPiano;
    Window trackerWin; trackerWin.title= "Tracker";    trackerWin.content= &vTracker;
    pianoWin.visible = trackerWin.visible = false;
    pianoWin.rect = { 90, 80, 780, 500 }; trackerWin.rect = { 130, 110, 720, 470 };
    auto rebind_project_views = [&]{
        std::vector<int> tracks;
        sequence* primary = nullptr;
        for (int sequenceIndex = 0; sequenceIndex < c_max_sequence; ++sequenceIndex) {
            if (!perf.is_active(sequenceIndex)) continue;
            sequence* sequence = perf.get_sequence(sequenceIndex);
            if (!sequence) continue;
            tracks.push_back(sequenceIndex);
            if (!primary) primary = sequence;
        }
        seq0 = primary;
        vPiano.set_sequence(primary);
        vTracker.set_sequence(primary, primary ? primary->get_midi_bus() : -1);
        vAuto.set_target(primary, primary ? primary->get_midi_bus() : 0);
        vKey.set_tracks(tracks);
        if (!primary) {
            pianoWin.visible = false;
            trackerWin.visible = false;
        }
    };

    // Modular patchbay: a window snapped to the bottom of the screen, shown by
    // default so it's discoverable.  Its VISIBILITY is separate from whether the
    // audio actually renders through the patch graph (the "Modular Render"
    // toggle), so showing the patchbay never silences the mixer on its own.
    // The bottom dock is a TAB HOST: Patchbay | Sample Editor (warp).
    ui::TabHost bottomTabs;
    bottomTabs.add("Patchbay", &vPatch);
    bottomTabs.add("Sample Editor", &vSampleEd);
    Window patchWin; patchWin.title = "Bottom Dock"; patchWin.content = &bottomTabs;
    patchWin.visible = true;
    wm.add(&patchWin);                     // register now so it draws immediately
    toggle_patch = [&]{
        patchWin.visible = !patchWin.visible;
        if (patchWin.visible) wm.raise(&patchWin);
        app.request_redraw();
    };

    // Plugin picker window: right-click a patcher node -> "Open Editor" -> pick a
    // scanned VST for that node (choose which VST3 the instrument hosts).
    pickerui::PluginPickerView pluginPicker;
    Window pickerWin; pickerWin.title = "Choose Plugin"; pickerWin.content = &pluginPicker;
    pickerWin.visible = false;
    static int pickerTarget = -1;
    pluginPicker.on_pick = [&](const PatchKnob::engine::PluginDescriptor& d){
        if (pickerTarget >= 0 && audio_ok) {
            PatchKnob::app::audio_app_patch_set_node_plugin(pickerTarget, d);
            if (PatchKnob::patchbay::Node* n = vPatch.find_node((PatchKnob::patchbay::NodeId)pickerTarget)) {
                std::string nm = d.name;
                size_t s = nm.find_last_of("/\\"); if (s != std::string::npos) nm = nm.substr(s+1);
                size_t dot = nm.find_last_of('.'); if (dot != std::string::npos) nm = nm.substr(0,dot);
                if (!nm.empty()) n->name = nm;
            }
        }
        pickerWin.visible = false; app.request_redraw();
    };
    vPatch.on_open_editor = [&](PatchKnob::patchbay::NodeId id){
        pickerTarget = (int)id;
        pluginPicker.plugins = g_scan;
        pluginPicker.instrumentsOnly = false;   // allow effects too
        pluginPicker.status = g_scanDone.load(std::memory_order_acquire) ? std::string() : std::string("scanning plugins...");
        open_window(pickerWin, 440, 480);
    };
    // "Open GUI": embed the plugin's NATIVE editor inside an SDL Window frame.
    // If there's no native window to embed into (windowless framebuffer), fall
    // back to the portable SDL parameter panel.
    vPatch.on_open_gui = [&](PatchKnob::patchbay::NodeId id){
        if (!audio_ok) return;
        PatchKnob::engine::IPluginInstance* inst = PatchKnob::app::audio_app_patch_node_instance((int)id);
        if (!inst) return;
        if (g_guiHandle) { PatchKnob::hostwin::editor_close(g_guiHandle); g_guiHandle=nullptr; g_guiNode=-1; }
        void* h = PatchKnob::hostwin::editor_open(inst, inst->descriptor().name.c_str(), app.window);
        if (!h) {   // no native embedding available -> SDL parameter panel
            paramView.set_instance(inst);
            paramWin.title = std::string("Params: ") + inst->descriptor().name;
            open_window(paramWin, 460, 520);
            return;
        }
        g_guiHandle = h; g_guiNode = (int)id;
        int nw=480, nh=320; PatchKnob::hostwin::editor_native_size(h,&nw,&nh);
        float sc = app.scale>0 ? app.scale : 1.f;
        guiWin.title = std::string("GUI: ") + inst->descriptor().name;
        guiWin.rect = { 60, 60, (int)(nw/sc)+2, (int)(nh/sc)+guiWin.title_h+2 };
        guiWin.visible = true; wm.raise(&guiWin); app.request_redraw();
    };
    // "Parameters": the portable SDL slider panel (any target).
    vPatch.on_open_params = [&](PatchKnob::patchbay::NodeId id){
        if (!audio_ok) return;
        PatchKnob::engine::IPluginInstance* inst = PatchKnob::app::audio_app_patch_node_instance((int)id);
        if (!inst) return;
        paramView.set_instance(inst);
        paramWin.title = std::string("Params: ") + inst->descriptor().name;
        open_window(paramWin, 460, 520);
    };
    // MIDI-In node: list hardware MIDI inputs + select one to play instruments live.
    vPatch.midi_devices = []{
        std::vector<std::string> v;
        int n = PatchKnob::app::audio_app_midi_input_count();
        for (int i=0;i<n;++i){ char nm[128]={0}; int cur=0;
            if (PatchKnob::app::audio_app_midi_input_info(i,nm,sizeof(nm),&cur)) v.push_back(nm); }
        return v;
    };
    vPatch.on_select_midi = [&](int idx){ if (audio_ok) PatchKnob::app::audio_app_patch_set_midi_input(idx); };
    vPatch.midi_out_devices = []{
        std::vector<std::string> v;
        int n = PatchKnob::app::audio_app_midi_output_count();
        for (int i=0;i<n;++i){ char nm[128]={0}; int cur=0;
            if (PatchKnob::app::audio_app_midi_output_info(i,nm,sizeof(nm),&cur)) v.push_back(nm); }
        return v;
    };
    vPatch.on_open_mixer = [&](PatchKnob::patchbay::NodeId id){
        open_mixer_for((int)id);   // unified strip view (regular or master mixer)
    };
    // Open the SDL Pd editor on this node's .pd (creating a fresh patch if none).
    vPatch.on_open_pd = [&](PatchKnob::patchbay::NodeId id){
        int node = (int)id;
        char buf[512] = {0};
        bool has = PatchKnob::app::audio_app_pd_path(node, buf, sizeof(buf)) && buf[0];
        std::string path = has ? std::string(buf)
                               : ("pd_node_" + std::to_string(node) + ".pd");
        g_pdNode = node; g_pdPath = path;
        pdEditor.set_patch_path(path);            // parse if it exists, else empty
        if (!has) { pdEditor.save();              // materialize the file...
                    PatchKnob::app::audio_app_pd_load(node, path.c_str()); }  // ...and bind it
        pdWin.title = std::string("Pd: ") + path;
        open_window(pdWin, 660, 480);
    };
    // Open the SDL modular (Rack) editor bound to this node's live RackEngine.
    vPatch.on_open_rack = [&](PatchKnob::patchbay::NodeId id){
        rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine((int)id);
        rackEditor.set_engine(eng);
        g_rackNode = (int)id; g_rackIOsig = -1;   // re-mirror ports on open
        rackWin.title = "Modular";
        open_window(rackWin, 720, 520);
    };
    // --- Buzz/Unwieldy SAMPLER editor -------------------------------------
    // Push the shell's multisample zones into isolated wavetable slots.  The
    // sampler wrapper chooses the slot by key/velocity before triggering Buzz,
    // which prevents fixed-pitch zones from selecting the wrong keyrange level.
    auto sampler_push_zones = [&](int node){
        for (int slot = 1; slot <= 128; ++slot)
            PatchKnob::app::audio_app_sampler_clear(node, slot);
        auto it = g_samplerZones.find(node);
        if (it == g_samplerZones.end()) return;
        for (size_t lvl = 0; lvl < it->second.size(); ++lvl) {
            const ui::SamplerZone& z = it->second[lvl];
            const long long n = z.clip.numFrames();
            if (n <= 0 || z.clip.ch[0].empty()) continue;
            long long a = (long long)(std::max(0.f, std::min(1.f, z.start)) * (float)n);
            long long b = (long long)(std::max(0.f, std::min(1.f, z.end)) * (float)n);
            if (b <= a) { a = 0; b = n; }
            const long long frames = std::max(1LL, b - a);
            std::vector<float> il((size_t)frames * 2);
            const float* L = z.clip.ch[0].data();
            const float* R = z.clip.ch[1].empty() ? L : z.clip.ch[1].data();
            const float pan = std::max(-1.f, std::min(1.f, z.pan));
            const float panL = pan <= 0.f ? 1.f : 1.f - pan;
            const float panR = pan >= 0.f ? 1.f : 1.f + pan;
            for (long long i = 0; i < frames; ++i) {
                long long si = z.reverse ? (b - 1 - i) : (a + i);
                if (si < 0) si = 0; if (si >= n) si = n - 1;
                il[(size_t)i*2] = L[si] * z.gain * panL;
                il[(size_t)i*2+1] = R[si] * z.gain * panR;
            }
            const int loKey = std::max(0, std::min(127, std::min(z.loKey, z.hiKey)));
            const int hiKey = std::max(0, std::min(127, std::max(z.loKey, z.hiKey)));
            const int loVel = std::max(0, std::min(127, std::min(z.loVel, z.hiVel)));
            const int hiVel = std::max(0, std::min(127, std::max(z.loVel, z.hiVel)));
            const int root = std::max(0, std::min(127, z.root));
            const int sampleRate = z.clip.sampleRate > 0 ? (int)z.clip.sampleRate
                                                         : PatchKnob::app::audio_app_sample_rate();
            PatchKnob::app::audio_app_sampler_load_ex(node, 1 + (int)lvl, 0, il.data(), (int)frames, 1,
                root, sampleRate, 0, (int)frames, z.loop ? 1 : 0,
                loKey, hiKey, loVel, hiVel, z.noteOffLayer ? 1 : 0,
                z.keyToPitch ? 1 : 0, z.velToVol ? 1 : 0, z.overlapMode, z.name.c_str());
        }
    };
    // Generate dense linear points from a curved SamplerEnv and push to the engine.
    // Buzz envelope axes are word (0..65535); flags carry EIF_SUSTAIN (=1) on the
    // sustain node.  The per-segment tension is baked in by sampling env_shape().
    auto sampler_push_env = [&](int node, int envIdx){
        auto it = g_samplerEnv.find(node);
        if (it == g_samplerEnv.end() || envIdx < 0 || envIdx >= ui::ENV_COUNT) return;
        const ui::SamplerEnv& e = it->second.env[envIdx];
        std::vector<unsigned short> xs, ys; std::vector<int> flags;
        if (e.enabled && e.nodes.size() >= 2) {
            const int nseg  = (int)e.nodes.size() - 1;
            const int perSeg = std::max(2, std::min(16, 60 / std::max(1, nseg)));
            auto shape = [](float t, float c){ return std::pow(t, std::pow(2.0f, -c * 3.0f)); };
            for (int i = 0; i < (int)e.nodes.size() && (int)xs.size() < 64; ++i) {
                const ui::EnvNode& a = e.nodes[i];
                xs.push_back((unsigned short)(std::max(0.f,std::min(1.f,a.x)) * 65535.f));
                ys.push_back((unsigned short)(std::max(0.f,std::min(1.f,a.y)) * 65535.f));
                flags.push_back(i == e.sustain ? 1 /*EIF_SUSTAIN*/ : 0);
                if (i + 1 < (int)e.nodes.size()) {          // curved intermediates to next node
                    const ui::EnvNode& b = e.nodes[i + 1];
                    for (int k = 1; k < perSeg && (int)xs.size() < 64; ++k) {
                        const float t = (float)k / (float)perSeg;
                        const float y = a.y + (b.y - a.y) * shape(t, a.curve);
                        const float x = a.x + (b.x - a.x) * t;
                        xs.push_back((unsigned short)(std::max(0.f,std::min(1.f,x)) * 65535.f));
                        ys.push_back((unsigned short)(std::max(0.f,std::min(1.f,y)) * 65535.f));
                        flags.push_back(0);
                    }
                }
            }
        }
        PatchKnob::app::audio_app_sampler_set_env(node, envIdx,
            xs.empty() ? nullptr : xs.data(), ys.empty() ? nullptr : ys.data(),
            flags.empty() ? nullptr : flags.data(), (int)xs.size());
    };
    vPatch.on_open_sampler = [&, sampler_push_zones, sampler_push_env](PatchKnob::patchbay::NodeId id){
        const int node = (int)id;
        PatchKnob::engine::IPluginInstance* inst = PatchKnob::app::audio_app_patch_node_instance(node);
        // After a project LOAD the instrument holds the restored samples/zones but the
        // shell's g_samplerZones is empty.  Rebuild it from the instrument so the editor
        // shows the zones AND on_apply re-pushes them faithfully (instead of clearing the
        // slot and wiping the restored audio).
        if (g_samplerZones[node].empty()) {
            const int nz = PatchKnob::app::audio_app_sampler_zone_count(node);
            for (int zi = 0; zi < nz; ++zi) {
                PatchKnob::engine::SamplerZoneInfo zi_info;
                if (!PatchKnob::app::audio_app_sampler_get_zone(node, zi, zi_info)) continue;
                ui::SamplerZone z;
                z.root = zi_info.rootKey; z.loKey = zi_info.loKey; z.hiKey = zi_info.hiKey;
                z.loVel = zi_info.loVel; z.hiVel = zi_info.hiVel;
                z.noteOffLayer = zi_info.noteOffLayer; z.keyToPitch = zi_info.keyToPitch;
                z.velToVol = zi_info.velToVol; z.overlapMode = zi_info.overlapMode;
                z.loop = zi_info.loop; z.name = zi_info.name;
                // de-interleave the stored PCM back into an AudioClip for display/re-push
                const int ch = zi_info.stereo ? 2 : 1;
                z.clip.sampleRate = zi_info.sampleRate;
                z.clip.ch[0].assign(zi_info.numFrames, 0.f);
                z.clip.ch[1].assign(zi_info.stereo ? zi_info.numFrames : 0, 0.f);
                for (int f = 0; f < zi_info.numFrames; ++f) {
                    z.clip.ch[0][f] = zi_info.pcm[(size_t)f * ch];
                    if (zi_info.stereo) z.clip.ch[1][f] = zi_info.pcm[(size_t)f * ch + 1];
                }
                g_samplerZones[node].push_back(std::move(z));
            }
        }
        // First open on a FRESH node (no restored samples/envelopes): seed a default amp
        // ADSR.  Do NOT seed/overwrite when the instrument already carries envelopes from
        // a load, or the push below would wipe the restored envelope.
        ui::SamplerEnvSet& envs = g_samplerEnv[node];
        const bool restoredEnv = PatchKnob::app::audio_app_sampler_has_envelopes(node);
        if (envs.env[ui::ENV_AMP].nodes.empty() && !restoredEnv) {
            ui::SamplerEnv& amp = envs.env[ui::ENV_AMP];
            amp.nodes = { {0.00f, 0.0f, 0.f}, {0.08f, 1.0f, 0.f},
                          {0.30f, 0.7f, 0.f}, {1.00f, 0.0f, 0.f} };
            amp.sustain = 2; amp.enabled = true;
        }
        vSamplerEd.bind(node, &g_samplerZones[node], &envs, inst);
        vSamplerEd.on_env = [&, node, sampler_push_env](int env){ sampler_push_env(node, env); };
        auto load_sampler_path = [&, node, sampler_push_zones](const std::string& path, int level, int key){
            PatchKnob::engine::AudioClip clip;
            std::string err;
            if (!PatchKnob::engine::loadWav(path, PatchKnob::app::audio_app_sample_rate(), clip, &err) || clip.empty()) {
                projectStatus = "Sample load failed: " + err; app.request_redraw(); return;
            }
            ui::SamplerZone z; z.clip = clip;
            z.root = key >= 0 ? key : 60;
            z.loKey = key >= 0 ? key : 0;
            z.hiKey = key >= 0 ? key : 127;
            const size_t slash = path.find_last_of("/\\");
            z.name = slash == std::string::npos ? path : path.substr(slash + 1);
            auto& zones = g_samplerZones[node];
            if (level >= 0 && level < (int)zones.size()) zones[(size_t)level] = z;   // replace
            else                                         zones.push_back(z);          // append
            sampler_push_zones(node);
            app.request_redraw();
        };
        vSamplerEd.on_load = [&, load_sampler_path](int level){
            std::string path;
            if (choose_wav_file(path)) load_sampler_path(path, level, -1);
        };
        vSamplerEd.on_load_path = [&, load_sampler_path](const std::string& path, int level, int key){
            load_sampler_path(path, level, key);
        };
        vSamplerEd.on_preview_path = [&](const std::string& path, PatchKnob::engine::AudioClip& clip) -> bool {
            std::string err;
            return PatchKnob::engine::loadWav(path, PatchKnob::app::audio_app_sample_rate(), clip, &err);
        };
        vSamplerEd.on_audition_clip = [&](const PatchKnob::engine::AudioClip& clip) {
            PatchKnob::app::audio_app_preview_clip(clip, 1.0f);
        };
        vSamplerEd.on_apply = [&, node, sampler_push_zones]{ sampler_push_zones(node); };
        // Apply the shell's current env state -- but NOT when the instrument already has
        // restored envelopes and the shell copy is still empty (would wipe the restore).
        if (!restoredEnv)
            for (int ev = 0; ev < ui::ENV_COUNT; ++ev) sampler_push_env(node, ev);
        open_window(samplerWin, 660, 500);
    };
    // Right-click a Rack node -> choose its MIDI polyphony (1..16 voices).
    vPatch.on_set_rack_poly = [&](PatchKnob::patchbay::NodeId id, int voices){
        PatchKnob::app::audio_app_rack_set_poly((int)id, voices);
        app.request_redraw();
    };
    static int g_recNode = -1;    // armed record node (drained into seq0 below)
    vPatch.on_arm_record = [&](PatchKnob::patchbay::NodeId id){
        if (!audio_ok) return;
        int node = (int)id;
        bool now = !PatchKnob::app::audio_app_record_active(node);
        PatchKnob::app::audio_app_record_set(node, now);
        g_recNode = now ? node : -1;
        app.request_redraw();
    };
    vArrange.on_open_editor = [&](int seq, int kind){
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (!s) return;
        if (kind==1) { vTracker.set_sequence(s, s->get_midi_bus()); trackerWin.title="Tracker: "+instr_current(s);
                       trackerWin.visible=true; wm.raise(&trackerWin); }
        else         { vPiano.set_sequence(s); pianoWin.title="Piano Roll: "+instr_current(s);
                       pianoWin.visible=true; wm.raise(&pianoWin); }
        app.request_redraw();
    };

    // Track-header instrument dropdown: list every instrument node, show/assign
    // the one this track drives (by the sequence's MIDI channel).  Replaces the
    // removed per-clip instrument strip -- an instrument owns a mixer channel.
    vArrange.on_list_instruments = [&]{
        std::vector<std::string> v;
        for (const auto& p : instrument_nodes()) v.push_back(p.second);
        return v;
    };
    vArrange.on_track_instrument = [&](int seq) -> std::string {
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        return s ? instr_current(s) : std::string("-");
    };
    vArrange.on_pick_instrument = [&](int seq, int idx){
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (s) instr_pick(s, idx);
    };

    // --- modular patchbay  <->  engine PatchGraph coordinator --------------
    // UI node ids are kept IDENTICAL to engine PatchGraph node ids, and UI port
    // ids identical to engine port indices, so the callbacks map 1:1 with no
    // translation table.  See audio_app_patch_* in audio_app.h.
    namespace pb = PatchKnob::patchbay;
    auto pv_mirror_node = [&](int eng, const std::string& nm, const std::string& cat,
                              double x, double y){
        if (eng < 0) return;
        pb::Node n; n.id=(pb::NodeId)eng; n.name=nm; n.category=cat; n.x=x; n.y=y;
        const int np = PatchKnob::app::audio_app_patch_node_port_count(eng);
        for (int i=0;i<np;++i){ int k=0,d=0,ch=0;
            if(!PatchKnob::app::audio_app_patch_node_port(eng,i,&k,&d,&ch)) continue;
            pb::PortKind pk = (k==1)?pb::PortKind::Midi:pb::PortKind::Audio;
            const char* pnm = PatchKnob::app::audio_app_patch_node_port_name(eng,i);
            std::string plabel = (pnm && *pnm) ? std::string(pnm) : (k==1?std::string("midi"):std::string("audio"));
            pb::Port p((pb::PortId)i, pk, plabel);
            if(d==0) n.inPorts.push_back(p); else n.outPorts.push_back(p);
        }
        vPatch.add_node(n);
    };
    // Re-sync a UI node's ports to the engine (after a mixer's channel count
    // changed) and drop any UI wires that referenced now-removed ports.
    auto resync_patch_node = [&](int eng){
        pb::Node* n = vPatch.find_node((pb::NodeId)eng);
        if (!n) return;
        n->inPorts.clear(); n->outPorts.clear();
        std::set<pb::PortId> validIn, validOut;
        const int np = PatchKnob::app::audio_app_patch_node_port_count(eng);
        for (int i=0;i<np;++i){ int k=0,d=0,ch=0;
            if(!PatchKnob::app::audio_app_patch_node_port(eng,i,&k,&d,&ch)) continue;
            pb::PortKind pk = (k==1)?pb::PortKind::Midi:pb::PortKind::Audio;
            const char* pnm = PatchKnob::app::audio_app_patch_node_port_name(eng,i);
            std::string plabel = (pnm && *pnm) ? std::string(pnm) : (k==1?std::string("midi"):std::string("audio"));
            pb::Port p((pb::PortId)i, pk, plabel);
            if(d==0){ n->inPorts.push_back(p); validIn.insert((pb::PortId)i); }
            else    { n->outPorts.push_back(p); validOut.insert((pb::PortId)i); }
        }
        std::vector<pb::Connection> drop;
        for (const auto& c : vPatch.connections()) {
            if (((int)c.from.node==eng && !validOut.count(c.from.port)) ||
                ((int)c.to.node  ==eng && !validIn.count(c.to.port)))
                drop.push_back(c);
        }
        for (const auto& c : drop) vPatch.remove_connection(c);   // engine already pruned; UI catch-up
        app.request_redraw();
    };
    // --- Csound CSD editor: right-click a Csound node -> "Edit CSD" ------------
    vPatch.on_open_csound = [&](PatchKnob::patchbay::NodeId id){
        const int node = (int)id;
        g_csoundNode = node;
        csoundEditor.setText(PatchKnob::app::audio_app_patch_csound_text(node));
        const char* err = PatchKnob::app::audio_app_patch_csound_error(node);
        if (err && *err) csoundEditor.setStatus(err, true);
        else             csoundEditor.setStatus("Ctrl+E to compile", false);
        csoundWin.title = "Csound CSD";
        open_window(csoundWin, 640, 520);
    };
    // Ctrl+E in the editor: push the buffer, recompile, and remirror the node so its
    // audio in/out ports track the CSD header (nchnls / nchnls_i).
    csoundEditor.on_recompile = [&]{
        if (g_csoundNode < 0) return;
        PatchKnob::app::audio_app_patch_csound_set_text(g_csoundNode, csoundEditor.text().c_str());
        const bool ok = PatchKnob::app::audio_app_patch_csound_recompile(g_csoundNode);
        if (ok) {
            resync_patch_node(g_csoundNode);              // ports may have changed
            if (sync_patch_connections) sync_patch_connections();
            csoundEditor.setStatus("compiled OK  -  ports updated", false);
        } else {
            const char* err = PatchKnob::app::audio_app_patch_csound_error(g_csoundNode);
            csoundEditor.setStatus(err && *err ? err : "compile error", true);
        }
        app.request_redraw();
    };
    if (audio_ok) {
        // Seed the canvas with the always-present MIDI-in source, audio-out sink,
        // and the singleton Master Mixer module (8 buses + master + MIDI clock out).
        pv_mirror_node(PatchKnob::app::audio_app_patch_midi_in_node(), "Instrument out", "MIDI",  40, 70);
        pv_mirror_node(PatchKnob::app::audio_app_patch_out_node(),     "Audio Out", "Audio I/O", 620, 70);
        pv_mirror_node(PatchKnob::app::audio_app_master_mixer_node(),  "Master Mixer", "Mixer",  620, 180);
    }
    // Arrange +circle -> add a track (0=instrument,1=audio): create the sequence
    // AND auto-create its labeled module wired into the master mixer.
    auto refresh_master_ui = [&]{
        resync_patch_node(PatchKnob::app::audio_app_master_mixer_node());
        // the sequencer MidiIn node grows an "Instrument out" plug per track in
        // lockstep with the mixer -- re-mirror it so the new plugs show on canvas.
        resync_patch_node(PatchKnob::app::audio_app_patch_midi_in_node());
        if (g_curMixerNode == PatchKnob::app::audio_app_master_mixer_node())
            mixerStrip.bus_count = PatchKnob::app::audio_app_mixer_channels(g_curMixerNode);
    };
    // Pull every engine PatchGraph connection into the patchbay UI so wires made
    // in the engine (e.g. instrument audio -> master-mixer inlet, MIDI-In ->
    // instrument by master_connect_instrument) appear as real cords on canvas.
    // Resync the master-mixer node's ports FIRST so a just-added inlet exists in
    // the UI node before a connection references it.
    sync_patch_connections = [&]{
        auto* graph = PatchKnob::app::audio_app_patch_graph();
        if (!graph) return;
        resync_patch_node(PatchKnob::app::audio_app_master_mixer_node());
        resync_patch_node(PatchKnob::app::audio_app_patch_midi_in_node());   // "Instrument out" plugs
        if (g_curMixerNode == PatchKnob::app::audio_app_master_mixer_node())
            mixerStrip.bus_count = PatchKnob::app::audio_app_mixer_channels(g_curMixerNode);
        std::vector<pb::Connection> conns;
        for (const auto& c : graph->connections())
            conns.emplace_back(pb::PortRef((pb::NodeId)c.from.node, (pb::PortId)c.from.port),
                               pb::PortRef((pb::NodeId)c.to.node,   (pb::PortId)c.to.port));
        vPatch.set_connections(conns);
        app.request_redraw();
    };
    refresh_loaded_project = [&]{
        vPatch.set_nodes({});
        vPatch.set_connections({});
        g_seqToTrack.clear();
        rebind_project_views();
        if (!audio_ok) return;

        auto* graph = PatchKnob::app::audio_app_patch_graph();
        if (graph) {
            const auto ids = graph->nodeIds();
            for (size_t index = 0; index < ids.size(); ++index) {
                const auto id = ids[index];
                const auto* node = graph->node(id);
                if (!node) continue;

                const std::string type = node->typeName();
                std::string name = type;
                std::string category = "Utility";
                if (type == "PluginNode") {
                    if (auto* inst = PatchKnob::app::audio_app_patch_node_instance((int)id)) {
                        name = inst->descriptor().name;
                        // Recognize the built-in sampler so its editor menu ("Open
                        // Sampler Editor") reappears after a project reload -- else it
                        // was mis-categorized as a generic "Instrument" and could not
                        // be opened again.
                        if (PatchKnob::engine::sampler_is_sampler(inst)) category = "Sampler";
                        else category = inst->descriptor().isInstrument ? "Instrument" : "Effect";
                    } else { name = "Missing Plugin"; category = "Effect"; }
                } else if (type == "PdNode") {
                    name = "Pure Data"; category = "Pure Data";
                } else if (type == "CsoundNode") {
                    name = "Csound"; category = "Csound";
                } else if (type == "RackNode") {
                    name = "Modular Rack"; category = "Modular (Rack)";
                } else if (type == "MixerNode" || type == "MasterMixerNode") {
                    name = type == "MasterMixerNode" ? "Master Mixer" : "Mixer";
                    category = "Mixer";
                } else if (type == "MidiInNode" || type == "MidiOutNode") {
                    // The singleton sequencer MidiIn node carries the per-track
                    // "Instrument out" plugs; added hardware MIDI-in nodes stay "MIDI In".
                    if (type == "MidiInNode")
                        name = ((int)id == PatchKnob::app::audio_app_patch_midi_in_node())
                                   ? "Instrument out" : "MIDI In";
                    else name = "MIDI Out";
                    category = "MIDI";
                } else if (type == "AudioDeviceInNode" || type == "AudioDeviceOutNode") {
                    name = type == "AudioDeviceInNode" ? "Audio In" : "Audio Out";
                    category = "Audio I/O";
                } else if (type == "SineSourceNode") name = "Sine";
                else if (type == "GainNode") name = "Gain";
                else if (type == "SumNode") name = "Sum";
                else if (type == "RecordNode") name = "Record";

                pv_mirror_node((int)id, name, category,
                               40.0 + 170.0 * (index % 4),
                               70.0 + 120.0 * (index / 4));
            }

            std::vector<pb::Connection> connections;
            for (const auto& c : graph->connections()) {
                connections.emplace_back(
                    pb::PortRef((pb::NodeId)c.from.node, (pb::PortId)c.from.port),
                    pb::PortRef((pb::NodeId)c.to.node, (pb::PortId)c.to.port));
            }
            vPatch.set_connections(connections);
            for (const auto& position : project_io_loaded_patch_layout()) {
                if (pb::Node* node = vPatch.find_node((pb::NodeId)position.nodeId)) {
                    node->x = position.x;
                    node->y = position.y;
                }
            }
        }

        // Reconstruct the instrument->mixer-track and sequence->mixer-track maps
        // from the restored patch graph (a naive 1:1 seq->track mapping is wrong:
        // several patterns can share one instrument's mixer track, and there are
        // usually fewer tracks than sequences).  A node feeding master-mixer inlet
        // (2 + 2*t) owns track t; a sequence maps to the track of the instrument
        // whose MIDI channel it matches.
        g_instrTrack.clear();
        if (auto* graph = PatchKnob::app::audio_app_patch_graph()) {
            const int mm = PatchKnob::app::audio_app_master_mixer_node();
            for (const auto& c : graph->connections())
                if ((int)c.to.node == mm && (int)c.to.port >= 2)
                    g_instrTrack[(int)c.from.node] = ((int)c.to.port - 2) / 2;
        }
        for (int si = 0; si < c_max_sequence; ++si) {
            if (!perf.is_active(si)) continue;
            sequence* s = perf.get_sequence(si);
            if (!s) continue;
            const int ch = s->get_midi_channel();
            int mapped = -1;
            for (const auto& kv : g_instrTrack)
                if (PatchKnob::app::audio_app_patch_node_channel(kv.first) == ch) { mapped = kv.second; break; }
            if (mapped < 0 && !g_instrTrack.empty()) mapped = g_instrTrack.begin()->second;  // fallback
            if (mapped >= 0) { g_seqToTrack[si] = mapped; s->set_midi_bus((char)mapped); }
        }
        if (apply_loaded_freezes) apply_loaded_freezes();   // re-show frozen lanes
        refresh_master_ui();
    };

    // --- live MIDI record -> a new clip on the timeline --------------------
    // Captured events accumulate here while recording; committed as a new
    // sequence + a trigger at the record position when recording stops.
    struct RecNote { long start, end; int pitch, vel; };
    static std::vector<RecNote> g_recNotes;           // completed notes (on+off paired)
    static std::map<int,RecNote> g_recPending;        // pitch -> open note (end filled on off)
    static long g_recMaxTick = 0;
    drain_record = [&]{
        long tk[256]; unsigned char st[256], d1[256], d2[256];
        int n = PatchKnob::app::audio_app_record_drain_live(tk, st, d1, d2, 256);
        for (int i=0;i<n;++i){
            unsigned char hi = st[i] & 0xF0; int pitch = d1[i];
            if (tk[i] > g_recMaxTick) g_recMaxTick = tk[i];
            if (hi==0x90 && d2[i]>0) g_recPending[pitch] = RecNote{ tk[i], tk[i], pitch, d2[i] };
            else if (hi==0x80 || (hi==0x90 && d2[i]==0)) {
                auto it = g_recPending.find(pitch);
                if (it != g_recPending.end()) { it->second.end = tk[i];
                    g_recNotes.push_back(it->second); g_recPending.erase(it); }
            }
        }
    };
    auto commit_recording = [&]{
        drain_record();                               // flush any tail
        // close any notes still held when recording stopped
        for (auto& p : g_recPending) { p.second.end = g_recMaxTick > p.second.start
                                       ? g_recMaxTick : p.second.start + c_ppqn/4;
                                       g_recNotes.push_back(p.second); }
        g_recPending.clear();
        if (g_recNotes.empty()) { g_recMaxTick = 0; return; }
        const long bar  = c_ppqn * 4;
        long span = ((g_recMaxTick / bar) + 1) * bar; // round up to whole bars
        int idx = -1;
        for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { idx = i; break; }
        if (idx < 0) { g_recNotes.clear(); g_recMaxTick = 0; return; }
        perf.new_sequence(idx); perf.set_active(idx, true);
        sequence* s = perf.get_sequence(idx);
        if (!s) { g_recNotes.clear(); g_recMaxTick = 0; return; }
        s->set_length(span);
        s->set_name(std::string("Rec ") + std::to_string(idx));
        for (const auto& nr : g_recNotes) {
            long start = nr.start; if (start >= span) start = span - 1; if (start < 0) start = 0;
            long dur = nr.end - nr.start; if (dur < 1) dur = c_ppqn/8;
            if (start + dur >= span) dur = span - 1 - start;   // stay inside (verify_and_link prune)
            if (dur < 1) dur = 1;
            s->add_note(start, dur, nr.pitch);
        }
        s->verify_and_link();
        // place the clip at the record position on the timeline.
        long recStart = (long)PatchKnob::app::audio_app_record_start_tick();
        if (recStart < 0) recStart = 0;
        perf.push_trigger_undo();
        s->add_trigger(recStart, span, 0);
        s->set_dirty();
        // register the new track's port on the master mixer (instrument).
        if (audio_ok) { int tr = PatchKnob::app::audio_app_master_add_track(1);
                        if (tr >= 0) { g_seqToTrack[idx] = tr; s->set_midi_bus((char)tr); }
                        refresh_master_ui(); }
        if (!seq0) rebind_project_views();
        g_recNotes.clear(); g_recPending.clear(); g_recMaxTick = 0;
    };
    toggle_record = [&]{
        g_recArmed = !g_recArmed;
        if (g_recArmed) {
            g_recNotes.clear(); g_recPending.clear(); g_recMaxTick = 0;
            PatchKnob::app::audio_app_record_arm(true);
            if (!PatchKnob::app::audio_app_transport_rolling())    // rec implies roll
                { perf.start(g_songMode); PatchKnob::app::audio_app_patch_set_playing(true); app.animating=true; }
        } else {
            PatchKnob::app::audio_app_record_arm(false);
            commit_recording();
        }
    };
    new_project = [&]{
        perf.stop();
        perf.set_orig_ticks(0);
        perf.set_looping(false);
        g_loopOn = false;
        g_recArmed = false;
        g_recNode = -1;
        g_recNotes.clear();
        g_recPending.clear();
        g_recMaxTick = 0;
        autoPlayer.clear();
        for (int sequenceIndex = 0; sequenceIndex < c_max_sequence; ++sequenceIndex)
            if (perf.is_active(sequenceIndex)) perf.delete_sequence(sequenceIndex);
        if (audio_ok) {
            PatchKnob::app::audio_app_record_arm(false);
            PatchKnob::app::audio_app_patch_set_playing(false);
            PatchKnob::app::audio_app_project_clear_audio_clips();
            PatchKnob::app::audio_app_project_reset_patch();
            PatchKnob::app::audio_app_set_modular(false);
        }
        app.animating = false;
        projectPath.clear();
        projectStatus = "Project: Untitled";
        update_project_title();
        if (refresh_loaded_project) refresh_loaded_project();
        app.request_redraw();
    };
    vArrange.on_add_track = [&](int kind){
        int idx = -1;
        for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { idx = i; break; }
        if (idx < 0) return;
        perf.new_sequence(idx);
        perf.set_active(idx, true);
        sequence* s = perf.get_sequence(idx);
        if (!s) return;
        s->set_name((kind == 0 ? std::string("Inst ") : std::string("Audio ")) + std::to_string(idx));
        // Add ONLY the track's ports on the master mixer (instrument=MIDI in/out,
        // audio=AUDIO in/out) -- you patch the synth/effects yourself.
        if (audio_ok) {
            int tr = PatchKnob::app::audio_app_master_add_track(kind == 0 ? 1 : 0);
            if (tr >= 0) { g_seqToTrack[idx] = tr; s->set_midi_bus((char)tr); }
        }
        // No auto-arm: in SONG mode the timeline triggers gate playback (a clip
        // sounds only where it is placed); LIVE arming is an explicit user act.
        if (!g_songMode) perf.sequence_playing_on(idx);
        if (!seq0) rebind_project_views();
        refresh_master_ui();
        app.request_redraw();
    };
    vArrange.on_track_key = [&](int seq) -> int {
        auto it = g_seqToTrack.find(seq);
        return it != g_seqToTrack.end() ? (100000 + it->second) : seq;
    };
    vArrange.on_create_pattern = [&](int sourceSeq, long start, long length,
                                     long offset, bool copyEvents) -> int {
        if (!perf.is_active(sourceSeq)) return -1;
        sequence* src = perf.get_sequence(sourceSeq);
        if (!src) return -1;
        int idx = -1;
        for (int i = 0; i < c_max_sequence; ++i) {
            if (!perf.is_active(i)) { idx = i; break; }
        }
        if (idx < 0) return -1;

        perf.new_sequence(idx);
        perf.set_active(idx, true);
        sequence* dst = perf.get_sequence(idx);
        if (!dst) return -1;

        if (copyEvents) {
            *dst = *src;
        } else {
            dst->set_length(src->get_length(), false);
            dst->set_bpm(src->get_bpm());
            dst->set_bw(src->get_bw());
            dst->set_midi_bus(src->get_midi_bus());
            dst->set_midi_channel(src->get_midi_channel());
        }
        dst->clear_triggers();
        if (length < 1) length = dst->get_length() > 0 ? dst->get_length() : c_ppqn * 4;
        dst->add_trigger(start, length, offset, false);
        std::string base = src->get_name() ? src->get_name() : "Pattern";
        dst->set_name(base + (copyEvents ? " copy " : " pat ") + std::to_string(idx));

        auto route = g_seqToTrack.find(sourceSeq);
        if (route != g_seqToTrack.end()) {
            g_seqToTrack[idx] = route->second;
            dst->set_midi_bus((char)route->second);
        }
        if (!g_songMode) perf.sequence_playing_on(idx);
        if (!seq0) rebind_project_views();
        app.request_redraw();
        return idx;
    };
    // Freeze bookkeeping (declared BEFORE the handlers that read it, e.g.
    // on_remove_track, so removing a track can detach the freeze).
    struct FrozenRec {
        int  freezeId  = -1;    // engine freeze id (owns the rendered audio)
        bool wasMuted  = false; // source pattern's prior song-mute state
        bool isTrack   = false; // track freeze (new lane + disabled devices)?
        int  newSeq    = -1;    // the frozen audio lane's sequence (track freeze)
        int  srcTrack  = -1;    // engine track we disabled (track freeze)
    };
    static std::map<int, FrozenRec> g_frozen;   // keyed by SOURCE sequence index

    // Snapshot g_frozen into the portable FRZ records for save.
    collect_freezes = [&]() -> std::vector<ProjectFreezeRecord> {
        std::vector<ProjectFreezeRecord> v;
        for (const auto& kv : g_frozen) {
            // Only TRACK freezes persist their UI-state: their audio lives on a
            // real mixer track the display seq maps to.  A clip freeze's audio is
            // on the hidden freeze track (no seq routing), so it reloads as before
            // (audio plays via AUDI, just no tint) rather than mis-mapping.
            if (!kv.second.isTrack || kv.second.newSeq < 0) continue;
            ProjectFreezeRecord fr;
            fr.srcSeq   = kv.first;
            fr.isTrack  = kv.second.isTrack ? 1 : 0;
            fr.newSeq   = kv.second.newSeq;
            fr.srcTrack = kv.second.srcTrack;
            fr.wasMuted = kv.second.wasMuted ? 1 : 0;
            const int dispSeq = kv.second.isTrack ? kv.second.newSeq : kv.first;
            auto tr = g_seqToTrack.find(dispSeq);
            fr.audioTrack = (tr != g_seqToTrack.end()) ? tr->second : -1;
            v.push_back(fr);
        }
        return v;
    };
    // Re-establish the frozen lanes after a load: point the display seq at its
    // saved audio track, re-show the waveform + frozen tint, and repopulate
    // g_frozen with freezeId = -1 (RELOADED: the audio is a project clip, not a
    // live freeze entry -- teardown_freeze / Unfreeze handle that case).
    apply_loaded_freezes = [&]() {
        if (!audio_ok) return;
        for (const ProjectFreezeRecord& fr : project_io_loaded_freezes()) {
            if (fr.srcSeq < 0) continue;
            const int dispSeq = fr.isTrack ? fr.newSeq : fr.srcSeq;
            if (dispSeq < 0 || !perf.is_active(dispSeq)) continue;
            if (fr.audioTrack >= 0) g_seqToTrack[dispSeq] = fr.audioTrack;   // audio-lane routing
            auto tr = g_seqToTrack.find(dispSeq);
            if (tr == g_seqToTrack.end()) continue;
            const PatchKnob::engine::AudioClip* clip = PatchKnob::app::audio_app_project_clip_on_track(tr->second);
            if (!clip) continue;
            long ticks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)clip->numFrames());
            if (ticks < 1) ticks = 1;
            vArrange.set_audio_clip(dispSeq, clip, ticks);
            FrozenRec rec;
            rec.freezeId = -1;                       // reloaded -> not a live freeze entry
            rec.wasMuted = fr.wasMuted != 0; rec.isTrack = fr.isTrack != 0;
            rec.newSeq = fr.newSeq; rec.srcTrack = fr.srcTrack;
            g_frozen[fr.srcSeq] = rec;
            vArrange.set_frozen(fr.srcSeq, true);
        }
        refresh_master_ui();
    };

    vArrange.on_remove_track = [&](int seq){
        // If this seq is part of a freeze (as the SOURCE or as the rendered audio
        // lane), unfreeze it FIRST so the engine FreezeEntry + its AudioClip are
        // detached (not leaked) and its audio stops.  on_unfreeze is a std::function
        // assigned later in setup, but this lambda only RUNS at event time by which
        // point it is bound; call it on the freeze's SOURCE seq.
        if (audio_ok) {
            int frozenSrc = -1;
            if (g_frozen.count(seq)) frozenSrc = seq;
            else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { frozenSrc = kv.first; break; }
            if (frozenSrc >= 0 && vArrange.on_unfreeze) vArrange.on_unfreeze(frozenSrc);
        }
        // Drop every per-seq map entry (waveform ptr, region, fade, colour, frozen
        // tint, lane height) while the lane routing is still intact, so a recycled
        // index can't inherit stale/dangling state.
        vArrange.forget_seq(seq);
        auto it = g_seqToTrack.find(seq);
        if (it != g_seqToTrack.end() && audio_ok) {
            int tr = it->second;
            bool sharedRoute = false;
            for (const auto& kv : g_seqToTrack) {
                if (kv.first != seq && kv.second == tr) { sharedRoute = true; break; }
            }
            if (!sharedRoute)
                PatchKnob::app::audio_app_master_remove_track(tr);
            g_seqToTrack.erase(it);
            if (!sharedRoute) {
                // Lane heights follow the renumber: for each track t>tr (ascending)
                // move its height key 100000+t -> 100000+(t-1).
                std::set<int> shifted;
                for (const auto& kv : g_seqToTrack) if (kv.second > tr) shifted.insert(kv.second);
                for (int t : shifted) vArrange.move_lane_height(100000 + t, 100000 + t - 1);
                for (auto& kv : g_seqToTrack) if (kv.second > tr) --kv.second;   // higher indices shift down
                // keep freeze records' engine-track indices in step with the shift
                for (auto& kv : g_frozen) if (kv.second.srcTrack > tr) --kv.second.srcTrack;
            }
            refresh_master_ui();
        }
        if (perf.is_active(seq)) perf.delete_sequence(seq);
        rebind_project_views();
        app.request_redraw();
    };

    // ---- track / clip FREEZE ------------------------------------------------
    // Render a sequence's [startTick,endTick] span (its instrument+FX) to audio.
    auto render_seq_span = [&](int seq, int srcTrack, long startTick, long endTick,
                               PatchKnob::engine::AudioClip& outClip) -> bool {
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (!s) return false;
        const long ticks = endTick - startTick + 1;
        if (ticks < 1) return false;
        // Pump duration derived from the SAME tick->sample conversion the freeze
        // drain uses (the tempo MAP), not a bare BPM guess -- so no note falls
        // beyond the pump window when the two disagree (that dropped tail notes,
        // i.e. "only some notes froze").  +2s tail lets releases ring out.
        double sr = PatchKnob::app::audio_app_sample_rate(); if (sr < 1.0) sr = 48000.0;
        const long long endSamp = PatchKnob::app::audio_app_tick_to_sample((long long)ticks);
        const double seconds = (double)endSamp / sr + 2.0;
        const int chan = s->get_midi_channel() & 0x0F;
        long patLen = s->get_length(); if (patLen < 1) patLen = c_ppqn * 4;
        outClip.name = std::string(s->get_name()) + " (frozen)";
        // Schedule the pattern's linked notes, looping to fill the span; ticks
        // start at 0 since the freeze render seeks to 0.
        auto scheduleMidi = [&]() {
            for (long base = 0; base < ticks; base += patLen) {
                s->reset_draw_marker();
                long ts = 0, tf = 0; int note = 0, vel = 0; bool sel = false;
                draw_type dt;
                while ((dt = s->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN) {
                    if (dt != DRAW_NORMAL_LINKED || ts >= patLen) continue;
                    long onT = base + ts, offT = base + tf;
                    if (onT >= ticks) continue;
                    if (offT > ticks) offT = ticks;
                    if (offT <= onT)  offT = onT + 1;
                    PatchKnob::app::audio_app_route_midi(srcTrack, (unsigned char)(0x90 | chan),
                                                     (unsigned char)note, (unsigned char)vel, onT);
                    PatchKnob::app::audio_app_route_midi(srcTrack, (unsigned char)(0x80 | chan),
                                                     (unsigned char)note, 0, offT);
                }
            }
        };
        const bool ok = PatchKnob::app::audio_app_freeze_render(srcTrack, seconds, scheduleMidi, outClip);
        // Diagnostic: report the render result so a silent source (e.g. no
        // fixed-graph instrument on the track) is visible rather than mysterious.
        float peak = 0.f;
        for (int c = 0; c < 2; ++c)
            for (float v : outClip.ch[c]) { float a = v < 0.f ? -v : v; if (a > peak) peak = a; }
        std::fprintf(stderr, "[freeze] seq %d track %d: %lld frames, peak %.3f, ok=%d "
                             "(mode=%s)\n",
                     seq, srcTrack, (long long)outClip.numFrames(), peak, ok ? 1 : 0,
                     PatchKnob::app::audio_app_modular() ? "modular" : "fixed");
        std::fflush(stderr);
        return ok;
    };

    // CLIP freeze: render the clip's span, play it on a hidden freeze track at
    // the clip's position, song-mute the source pattern, show the waveform in
    // place on the SAME lane.
    vArrange.on_freeze_clip = [&](int seq, long st, long en){
        if (!audio_ok || g_frozen.count(seq)) { app.request_redraw(); return; }
        auto trIt = g_seqToTrack.find(seq);
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (trIt == g_seqToTrack.end() || !s) { app.request_redraw(); return; }
        PatchKnob::engine::AudioClip clip;
        if (!render_seq_span(seq, trIt->second, st, en, clip)) { app.request_redraw(); return; }
        const long long startSample = PatchKnob::app::audio_app_tick_to_sample((long long)st);
        const int id = PatchKnob::app::audio_app_freeze_attach(-1, clip, startSample, 1.0f);
        if (id < 0) { app.request_redraw(); return; }
        FrozenRec rec; rec.freezeId = id; rec.wasMuted = s->get_song_mute();
        s->set_song_mute(true);
        g_frozen[seq] = rec;
        vArrange.set_frozen(seq, true);
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
            long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)disp->numFrames());
            if (clipTicks < 1) clipTicks = en - st + 1;
            vArrange.set_audio_clip(seq, disp, clipTicks);     // real audio length (ticks)
        }
        app.request_redraw();
    };

    // TRACK freeze: render the whole lane to audio on a NEW audio track (a new
    // arrange lane), keep the source sequence, and DISABLE the source track's
    // devices (instrument + FX) to free CPU.
    vArrange.on_freeze_track = [&](int seq){
        if (!audio_ok || g_frozen.count(seq)) { app.request_redraw(); return; }
        auto trIt = g_seqToTrack.find(seq);
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (trIt == g_seqToTrack.end() || !s) { app.request_redraw(); return; }
        const int srcTrack = trIt->second;
        long maxTick = s->get_max_trigger(); if (maxTick <= 0) maxTick = s->get_length();
        const long startTick = 0, endTick = maxTick;

        PatchKnob::engine::AudioClip clip;
        if (!render_seq_span(seq, srcTrack, startTick, endTick, clip)) { app.request_redraw(); return; }

        // New audio lane (sequence + engine track).
        int newSeq = -1;
        for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { newSeq = i; break; }
        if (newSeq < 0) { app.request_redraw(); return; }
        perf.new_sequence(newSeq); perf.set_active(newSeq, true);
        sequence* ns = perf.get_sequence(newSeq);
        if (!ns) { app.request_redraw(); return; }
        ns->set_name(std::string(s->get_name()) + " (frozen)");
        const int newTrack = PatchKnob::app::audio_app_master_add_track(0);   // audio ports
        if (newTrack < 0) { perf.delete_sequence(newSeq); app.request_redraw(); return; }
        g_seqToTrack[newSeq] = newTrack;
        // Size the clip BLOCK to the captured audio's ACTUAL duration (which
        // includes the release tail past the trigger span), so the block edge ==
        // where the audio ends: no audio plays past the block, and the waveform
        // maps 1:1 to timeline ticks instead of being crammed into a short span.
        long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)clip.numFrames());
        if (clipTicks < 1) clipTicks = endTick - startTick + 1;
        ns->add_trigger(startTick, clipTicks, 0);                          // clip block

        const long long startSample = PatchKnob::app::audio_app_tick_to_sample((long long)startTick);
        const int id = PatchKnob::app::audio_app_freeze_attach(newTrack, clip, startSample, 1.0f);
        if (id < 0) {
            PatchKnob::app::audio_app_master_remove_track(newTrack);
            g_seqToTrack.erase(newSeq); perf.delete_sequence(newSeq);
            app.request_redraw(); return;
        }
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id))
            vArrange.set_audio_clip(newSeq, disp, clipTicks);              // real audio length

        PatchKnob::app::audio_app_set_track_disabled(srcTrack, true);         // free CPU
        FrozenRec rec; rec.freezeId = id; rec.wasMuted = s->get_song_mute();
        rec.isTrack = true; rec.newSeq = newSeq; rec.srcTrack = srcTrack;
        s->set_song_mute(true);
        perf.sequence_playing_off(seq);   // stop LIVE playback too, so the source
                                          // instrument can't sound under the freeze
        g_frozen[seq] = rec;
        vArrange.set_frozen(seq, true);
        rebind_project_views();
        refresh_master_ui();
        app.request_redraw();
    };

    // Shared freeze TEARDOWN.  `srcSeq` must be a g_frozen key.  unmuteSource=true
    // (Unfreeze) restores the source's prior song-mute + brings the MIDI back;
    // false (Delete) leaves the source SONG-MUTED (delete removes the audio clip
    // only -- it must NOT unfreeze).  Either way the disabled devices are
    // re-enabled (resource hygiene) and the freeze bookkeeping is dropped.
    auto teardown_freeze = [&](int srcSeq, bool unmuteSource){
        auto it = g_frozen.find(srcSeq);
        if (it == g_frozen.end()) return;
        const FrozenRec rec = it->second;
        g_frozen.erase(it);
        // If the sample editor is showing this freeze's clip, unbind it FIRST --
        // detach is about to free the AudioClip and a later draw would deref it.
        vSampleEd.forget_seq(srcSeq);
        if (rec.newSeq >= 0) vSampleEd.forget_seq(rec.newSeq);
        if (rec.freezeId >= 0) {
            PatchKnob::app::audio_app_freeze_detach(rec.freezeId);   // live freeze entry
        } else {
            // RELOADED freeze: the audio is a project clip -> stop it on its lane.
            const int dispSeq = rec.isTrack ? rec.newSeq : srcSeq;
            auto tr = g_seqToTrack.find(dispSeq);
            if (tr != g_seqToTrack.end()) PatchKnob::app::audio_app_project_clear_track(tr->second);
        }
        if (rec.isTrack && rec.srcTrack >= 0)
            PatchKnob::app::audio_app_set_track_disabled(rec.srcTrack, false);   // re-enable devices
        if (sequence* s = perf.is_active(srcSeq) ? perf.get_sequence(srcSeq) : nullptr)
            s->set_song_mute(unmuteSource ? rec.wasMuted : true);
        if (rec.isTrack) {
            vArrange.forget_seq(rec.newSeq);        // purge the deleted lane's maps
            auto nt = g_seqToTrack.find(rec.newSeq);
            if (nt != g_seqToTrack.end()) {
                const int removed = nt->second;
                // Only tear down the mixer track if no OTHER sequence still routes
                // to it (a duplicate/split shares the source lane's track).
                bool sharedRoute = false;
                for (const auto& kv : g_seqToTrack)
                    if (kv.first != rec.newSeq && kv.second == removed) { sharedRoute = true; break; }
                g_seqToTrack.erase(nt);
                if (!sharedRoute) {
                    PatchKnob::app::audio_app_master_remove_track(removed);
                    std::set<int> shifted;
                    for (const auto& kv : g_seqToTrack) if (kv.second > removed) shifted.insert(kv.second);
                    for (int t : shifted) vArrange.move_lane_height(100000 + t, 100000 + t - 1);
                    for (auto& kv : g_seqToTrack) if (kv.second > removed) --kv.second;
                    for (auto& kv : g_frozen) if (kv.second.srcTrack > removed) --kv.second.srcTrack;
                }
            }
            if (perf.is_active(rec.newSeq)) perf.delete_sequence(rec.newSeq);
            rebind_project_views();
            refresh_master_ui();
        } else {
            vArrange.set_audio_clip(srcSeq, nullptr);
        }
        vArrange.set_frozen(srcSeq, false);
    };

    vArrange.on_unfreeze = [&, teardown_freeze](int seq){
        // `seq` may be the freeze SOURCE key or the rendered audio LANE (newSeq);
        // resolve to the g_frozen key either way so "Unfreeze" on the audio lane
        // is not a silent no-op.
        auto it = g_frozen.find(seq);
        if (it == g_frozen.end())
            for (auto k = g_frozen.begin(); k != g_frozen.end(); ++k)
                if (k->second.newSeq == seq) { it = k; break; }
        if (it == g_frozen.end()) { app.request_redraw(); return; }
        const int srcSeq = it->first;
        const FrozenRec rec = it->second;
        // MIDI<->audio alignment (LIVE track freeze only): if the audio was
        // cut/rearranged, rewrite the source MIDI to match BEFORE the audio lane is
        // torn down.  Reloaded freezes (freezeId<0) don't carry per-region source
        // offsets in the display, so they unfreeze to the saved MIDI as-is.
        if (rec.isTrack && rec.newSeq >= 0 && rec.freezeId >= 0)
            vArrange.unfreeze_rebuild_midi(rec.newSeq, srcSeq);
        teardown_freeze(srcSeq, /*unmuteSource=*/true);
        app.request_redraw();
    };

    // ---- DELETE an audio clip WITH DISK-CACHED UNDO -------------------------
    // A deleted audio clip's rendered buffer + region is dumped to a raw file in
    // a temp dir (bounded ring) so Ctrl+Z can restore it without holding the
    // (possibly many-MB) audio in RAM.  Non-audio clips undo via the sequencer's
    // own trigger-undo stack (pop_trigger_undo, fired from the arrange view).
    struct ClipUndo {
        bool isTrack = false; int srcSeq = -1; int newSeq = -1;
        long posTick = 0, lenTick = 0;
        long long srcOffSamp = 0, lenSamp = 0;
        float gain = 1.f; bool muted = false, loop = false, wasMuted = false;
        int srcTrack = -1; std::string name, blob;
    };
    static std::vector<ClipUndo> g_clipUndo;
    static int g_clipUndoNextId = 0;

    auto undo_dir = []() -> std::string {
        const char* t = std::getenv("TEMP"); if (!t) t = std::getenv("TMP"); if (!t) t = ".";
        std::string d = std::string(t) + "\\PatchKnob_undo";
        CreateDirectoryA(d.c_str(), nullptr);   // ok if it already exists
        return d;
    };
    auto dump_clip = [](const PatchKnob::engine::AudioClip& c, const std::string& path) -> bool {
        FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
        const long long n = (long long)c.numFrames();
        std::fwrite(&n, sizeof(n), 1, f);
        std::fwrite(&c.sampleRate, sizeof(double), 1, f);
        std::fwrite(&c.sourceSampleRate, sizeof(double), 1, f);
        std::uint32_t nl = (std::uint32_t)c.name.size();
        std::fwrite(&nl, sizeof(nl), 1, f);
        if (nl) std::fwrite(c.name.data(), 1, nl, f);
        if (n > 0) { std::fwrite(c.ch[0].data(), sizeof(float), (size_t)n, f);
                     std::fwrite(c.ch[1].data(), sizeof(float), (size_t)n, f); }
        std::fclose(f); return true;
    };
    auto load_clip = [](const std::string& path, PatchKnob::engine::AudioClip& c) -> bool {
        FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
        long long n = 0;
        if (std::fread(&n, sizeof(n), 1, f) != 1 || n < 0 || n > (1LL<<32)) { std::fclose(f); return false; }
        std::fread(&c.sampleRate, sizeof(double), 1, f);
        std::fread(&c.sourceSampleRate, sizeof(double), 1, f);
        std::uint32_t nl = 0; std::fread(&nl, sizeof(nl), 1, f);
        c.name.resize(nl); if (nl) std::fread(&c.name[0], 1, nl, f);
        c.ch[0].resize((size_t)n); c.ch[1].resize((size_t)n);
        if (n > 0) { std::fread(c.ch[0].data(), sizeof(float), (size_t)n, f);
                     std::fread(c.ch[1].data(), sizeof(float), (size_t)n, f); }
        std::fclose(f); return true;
    };

    vArrange.on_clip_delete = [&, undo_dir, dump_clip, teardown_freeze](int seq){
        // Resolve the freeze (by source seq or by rendered audio lane).
        int srcSeq = -1;
        if (g_frozen.count(seq)) srcSeq = seq;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { srcSeq = kv.first; break; }
        if (srcSeq < 0 || !audio_ok) return;      // not a freeze -> nothing to delete
        const FrozenRec rec = g_frozen[srcSeq];
        const int dispSeq = rec.isTrack ? rec.newSeq : srcSeq;
        // Live freeze -> the clip + region come from the freeze entry; a RELOADED
        // freeze (freezeId<0) has no entry, so read the clip off its project track.
        const PatchKnob::engine::AudioClip* clip = nullptr;
        if (rec.freezeId >= 0) clip = PatchKnob::app::audio_app_freeze_clip(rec.freezeId);
        else { auto tr = g_seqToTrack.find(dispSeq);
               if (tr != g_seqToTrack.end()) clip = PatchKnob::app::audio_app_project_clip_on_track(tr->second); }
        if (clip) {
            ClipUndo u;
            u.isTrack = rec.isTrack; u.srcSeq = srcSeq; u.newSeq = rec.newSeq;
            u.wasMuted = rec.wasMuted; u.srcTrack = rec.srcTrack; u.name = clip->name;
            long long ss = 0, so = 0, ln = 0; float g = 1.f; int mu = 0, lp = 0;
            if (rec.freezeId >= 0)
                PatchKnob::app::audio_app_freeze_get_region(rec.freezeId, &ss, &so, &ln, &g, &mu, &lp);
            u.srcOffSamp = so; u.lenSamp = ln; u.gain = g; u.muted = (mu != 0); u.loop = (lp != 0);
            if (sequence* ds = perf.is_active(dispSeq) ? perf.get_sequence(dispSeq) : nullptr) {
                ds->reset_draw_trigger_marker();
                long on, off, offs; bool sel;
                if (ds->get_next_trigger(&on, &off, &sel, &offs)) { u.posTick = on; u.lenTick = off - on + 1; }
            }
            u.blob = undo_dir() + "\\clip_" + std::to_string(g_clipUndoNextId++) + ".raw";
            if (dump_clip(*clip, u.blob)) {
                g_clipUndo.push_back(u);
                while (g_clipUndo.size() > 32) {                 // bounded ring: unlink oldest
                    std::remove(g_clipUndo.front().blob.c_str());
                    g_clipUndo.erase(g_clipUndo.begin());
                }
            }
        }
        // DELETE (not unfreeze): tear the clip down but keep the source SONG-MUTED
        // -- the audio is gone, the source does NOT come back as MIDI.  Ctrl+Z
        // restores the audio from the disk cache above.
        teardown_freeze(srcSeq, /*unmuteSource=*/false);
        app.request_redraw();
    };

    vArrange.on_undo = [&, load_clip](){
        if (!audio_ok || g_clipUndo.empty()) { app.request_redraw(); return; }
        const ClipUndo u = g_clipUndo.back(); g_clipUndo.pop_back();
        PatchKnob::engine::AudioClip clip;
        if (!load_clip(u.blob, clip) || clip.empty()) { std::remove(u.blob.c_str()); app.request_redraw(); return; }
        const long long startSample = PatchKnob::app::audio_app_tick_to_sample((long long)(u.posTick < 0 ? 0 : u.posTick));

        if (u.isTrack) {
            int newSeq = (u.newSeq >= 0 && !perf.is_active(u.newSeq)) ? u.newSeq : -1;
            if (newSeq < 0) for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { newSeq = i; break; }
            if (newSeq < 0) { std::remove(u.blob.c_str()); app.request_redraw(); return; }
            perf.new_sequence(newSeq); perf.set_active(newSeq, true);
            sequence* ns = perf.get_sequence(newSeq);
            if (!ns) { std::remove(u.blob.c_str()); app.request_redraw(); return; }
            ns->set_name(u.name.empty() ? std::string("restored") : u.name);
            long lenTick = u.lenTick > 0 ? u.lenTick
                          : (long)PatchKnob::app::audio_app_sample_to_tick((long long)clip.numFrames());
            ns->add_trigger(u.posTick, lenTick, 0);
            const int newTrack = PatchKnob::app::audio_app_master_add_track(0);
            if (newTrack < 0) { perf.delete_sequence(newSeq); std::remove(u.blob.c_str()); app.request_redraw(); return; }
            g_seqToTrack[newSeq] = newTrack;
            const int id = PatchKnob::app::audio_app_freeze_attach(newTrack, clip, startSample, u.gain);
            if (id < 0) { PatchKnob::app::audio_app_master_remove_track(newTrack); g_seqToTrack.erase(newSeq);
                          perf.delete_sequence(newSeq); std::remove(u.blob.c_str()); app.request_redraw(); return; }
            PatchKnob::app::audio_app_freeze_set_region(id, startSample, u.srcOffSamp, u.lenSamp);
            PatchKnob::app::audio_app_freeze_set_muted(id, u.muted);
            PatchKnob::app::audio_app_freeze_set_loop(id, u.loop);
            if (u.srcTrack >= 0) PatchKnob::app::audio_app_set_track_disabled(u.srcTrack, true);
            if (sequence* s = perf.is_active(u.srcSeq) ? perf.get_sequence(u.srcSeq) : nullptr) s->set_song_mute(true);
            FrozenRec rec; rec.freezeId = id; rec.wasMuted = u.wasMuted; rec.isTrack = true;
            rec.newSeq = newSeq; rec.srcTrack = u.srcTrack;
            g_frozen[u.srcSeq] = rec;
            if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id))
                vArrange.set_audio_clip(newSeq, disp, lenTick);
            vArrange.set_frozen(u.srcSeq, true);
            rebind_project_views(); refresh_master_ui();
        } else {
            const int id = PatchKnob::app::audio_app_freeze_attach(-1, clip, startSample, u.gain);
            if (id < 0) { std::remove(u.blob.c_str()); app.request_redraw(); return; }
            PatchKnob::app::audio_app_freeze_set_region(id, startSample, u.srcOffSamp, u.lenSamp);
            PatchKnob::app::audio_app_freeze_set_muted(id, u.muted);
            PatchKnob::app::audio_app_freeze_set_loop(id, u.loop);
            if (sequence* s = perf.is_active(u.srcSeq) ? perf.get_sequence(u.srcSeq) : nullptr) s->set_song_mute(true);
            FrozenRec rec; rec.freezeId = id; rec.wasMuted = u.wasMuted; rec.isTrack = false;
            g_frozen[u.srcSeq] = rec;
            if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
                long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)disp->numFrames());
                if (clipTicks < 1) clipTicks = u.lenTick > 0 ? u.lenTick : 1;
                vArrange.set_audio_clip(u.srcSeq, disp, clipTicks);
            }
            vArrange.set_frozen(u.srcSeq, true);
        }
        std::remove(u.blob.c_str());
        app.request_redraw();
    };

    // Clip crossfade: the displayed audio clip's seq maps to a freeze id (clip
    // freeze keys g_frozen by seq; track freeze stores it under newSeq).
    vArrange.on_clip_fade = [&](int seq, long inTicks, long outTicks, float inK, float outK){
        int freezeId = -1;
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) freezeId = it->second.freezeId;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { freezeId = kv.second.freezeId; break; }
        if (freezeId < 0) return;
        const long long inF  = PatchKnob::app::audio_app_tick_to_sample(inTicks);
        const long long outF = PatchKnob::app::audio_app_tick_to_sample(outTicks);
        PatchKnob::app::audio_app_freeze_set_fades(freezeId, inF, outF, inK, outK);
        app.request_redraw();
    };

    // Moving/trimming an audio/frozen clip pushes its new REGION to the engine so
    // playback follows non-destructively: position, span, and start-offset into
    // the source all come from the clip's (start, length, offset) trigger.
    vArrange.on_clip_region_changed = [&](int seq, long startTick, long lengthTick, long offsetTick){
        if (!audio_ok) return;
        int freezeId = -1;
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) freezeId = it->second.freezeId;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { freezeId = kv.second.freezeId; break; }
        if (freezeId < 0) return;
        // Constant-tempo tick->sample (tick_to_sample(0)==0, so durations scale).
        const long long posS = PatchKnob::app::audio_app_tick_to_sample((long long)(startTick  < 0 ? 0 : startTick));
        const long long offS = PatchKnob::app::audio_app_tick_to_sample((long long)(offsetTick < 0 ? 0 : offsetTick));
        const long long lenS = PatchKnob::app::audio_app_tick_to_sample((long long)(lengthTick < 1 ? 1 : lengthTick));
        PatchKnob::app::audio_app_freeze_set_region(freezeId, posS, offS, lenS);
        app.request_redraw();
    };

    // Duplicating an audio clip: attach the SAME source audio to the copy's lane
    // as a NEW region so it plays (a bare pattern-clone would be a silent block).
    vArrange.on_clip_duplicated = [&](int srcSeq, int newSeq, long startTick, long srcOffTick, long lenTick){
        if (!audio_ok) return;
        int srcId = -1;
        auto it = g_frozen.find(srcSeq);
        if (it != g_frozen.end()) srcId = it->second.freezeId;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == srcSeq) { srcId = kv.second.freezeId; break; }
        if (srcId < 0) return;
        const PatchKnob::engine::AudioClip* src = PatchKnob::app::audio_app_freeze_clip(srcId);
        if (!src) return;
        // The copy's lane inherits the source's mixer track (on_create_pattern);
        // fall back to a fresh audio track if it somehow has none.
        int newTrack = -1;
        auto tr = g_seqToTrack.find(newSeq);
        if (tr != g_seqToTrack.end()) newTrack = tr->second;
        else { newTrack = PatchKnob::app::audio_app_master_add_track(0); if (newTrack >= 0) g_seqToTrack[newSeq] = newTrack; }
        if (newTrack < 0) return;
        const long long posS = PatchKnob::app::audio_app_tick_to_sample((long long)(startTick < 0 ? 0 : startTick));
        const int newId = PatchKnob::app::audio_app_freeze_attach(newTrack, *src, posS, 1.0f);
        if (newId < 0) return;
        PatchKnob::app::audio_app_freeze_set_region(newId, posS,
            PatchKnob::app::audio_app_tick_to_sample((long long)(srcOffTick < 0 ? 0 : srcOffTick)),
            PatchKnob::app::audio_app_tick_to_sample((long long)(lenTick < 1 ? 1 : lenTick)));
        FrozenRec rec; rec.freezeId = newId; rec.isTrack = true; rec.newSeq = newSeq; rec.srcTrack = -1;
        g_frozen[newSeq] = rec;
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(newId)) {
            long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)disp->numFrames());
            vArrange.set_audio_clip(newSeq, disp, clipTicks);
        }
        app.request_redraw();
    };

    // Ardour region operations -> the seq's frozen region id.
    auto freeze_id_for_seq = [&](int seq) -> int {
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) return it->second.freezeId;
        for (auto& kv : g_frozen) if (kv.second.newSeq == seq) return kv.second.freezeId;
        return -1;
    };
    vArrange.on_clip_gain = [&, freeze_id_for_seq](int seq, float gain){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if (id >= 0) { PatchKnob::app::audio_app_freeze_set_gain(id, gain); app.request_redraw(); }
    };
    vArrange.on_clip_mute = [&, freeze_id_for_seq](int seq, bool muted){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if (id >= 0) { PatchKnob::app::audio_app_freeze_set_muted(id, muted); app.request_redraw(); }
    };
    vArrange.on_clip_normalize = [&, freeze_id_for_seq](int seq){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if (id >= 0) { PatchKnob::app::audio_app_freeze_normalize(id, 0.0f); app.request_redraw(); }
    };
    vArrange.on_clip_reverse = [&, freeze_id_for_seq](int seq){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if (id >= 0) { PatchKnob::app::audio_app_freeze_reverse(id); app.request_redraw(); }
    };
    vArrange.on_clip_loop = [&, freeze_id_for_seq](int seq, bool loop){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if (id >= 0) { PatchKnob::app::audio_app_freeze_set_loop(id, loop); app.request_redraw(); }
    };

    // WARP: render the sample-editor's marker map into a new stretched clip and
    // swap it into the region, resizing the block to the warped length.
    vSampleEd.on_apply_warp = [&, freeze_id_for_seq](int seq,
            const std::vector<PatchKnob::engine::WarpMarker>& markers, double semis,
            PatchKnob::engine::WarpMode mode, double formant){
        if (!audio_ok) return;
        int id = freeze_id_for_seq(seq);
        if (id < 0) return;
        const PatchKnob::engine::AudioClip* src = PatchKnob::app::audio_app_freeze_clip(id);
        if (!src) return;
        PatchKnob::engine::AudioClip warped = PatchKnob::engine::warp_render(*src, markers, semis, mode, formant);
        if (warped.empty()) return;
        if (!PatchKnob::app::audio_app_freeze_replace_clip(id, warped)) return;
        const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id);
        if (disp) {
            long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick((long long)disp->numFrames());
            if (clipTicks < 1) clipTicks = 1;
            if (sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr) {
                const long pos = s->get_selected_trigger_start_tick();
                s->select_trigger(pos);
                s->move_selected_triggers_to(pos + clipTicks - 1, false, 1);   // resize block
            }
            vArrange.set_audio_clip(seq, disp, clipTicks);
            vSampleEd.set_clip(seq, disp, PatchKnob::app::audio_app_sample_rate(),
                               PatchKnob::app::audio_app_tempo(), c_ppqn);   // re-bind warped
        }
        app.request_redraw();
    };

    // Realtime warp preview: push the live marker map to the region's player so
    // it time-stretches during playback -- you HEAR the warp while dragging.
    vSampleEd.on_warp_live = [&, freeze_id_for_seq](int seq,
            const std::vector<PatchKnob::engine::WarpMarker>& markers){
        if (!audio_ok) return;
        int id = freeze_id_for_seq(seq);
        if (id < 0) return;
        PatchKnob::app::audio_app_freeze_set_warp(id, markers.empty() ? nullptr : markers.data(),
                                              (int)markers.size());
    };

    // Double-click an audio clip -> load it into the sample/warp editor, switch
    // the bottom dock to that tab, and show it.
    vArrange.on_open_sample_editor = [&, freeze_id_for_seq](int seq){
        if (!audio_ok) return;
        int id = freeze_id_for_seq(seq);
        if (id < 0) return;
        const PatchKnob::engine::AudioClip* c = PatchKnob::app::audio_app_freeze_clip(id);
        if (!c) return;
        vSampleEd.set_clip(seq, c, PatchKnob::app::audio_app_sample_rate(),
                           PatchKnob::app::audio_app_tempo(), c_ppqn);
        bottomTabs.active = 1;                 // Sample Editor tab
        patchWin.visible = true; wm.raise(&patchWin);
        app.request_redraw();
    };

    vPatch.on_add_module = [&](double x, double y, const std::string& cat){
        if (!audio_ok) return;
        int eng=-1; std::string nm=cat.empty()?std::string("Sine"):cat;
        if      (cat=="Instrument") { eng=PatchKnob::app::audio_app_patch_add_plugin(vst_desc(synthPath.c_str())); nm="Inst"; }
        else if (cat=="Effect")     { eng=PatchKnob::app::audio_app_patch_add_empty_plugin(); nm="FX (empty)"; }
        else if (cat=="MIDI")       { eng=PatchKnob::app::audio_app_patch_add_builtin("midiin"); nm="MIDI In"; }
        else if (cat=="Audio I/O")  { eng=PatchKnob::app::audio_app_patch_add_builtin("out");
                                      int cnt=0; for (const auto& nd : vPatch.nodes()) if (nd.category=="Audio I/O") ++cnt;
                                      nm = "Audio Out " + std::to_string(cnt+1); }   // auto-numbered
        else if (cat=="Mixer")      { eng=PatchKnob::app::audio_app_patch_add_mixer(4);          nm="Mixer"; }
        else if (cat=="Record")     { eng=PatchKnob::app::audio_app_patch_add_record();          nm="Record"; }
        else if (cat=="Pure Data")  { eng=PatchKnob::app::audio_app_patch_add_pd();              nm="Pd"; }
        else if (cat=="Modular (Rack)") { eng=PatchKnob::app::audio_app_patch_add_rack();        nm="Rack"; }
        else if (cat=="Sampler")    { eng=PatchKnob::app::audio_app_patch_add_sampler();         nm="Sampler"; }
        else if (cat=="Csound")     { eng=PatchKnob::app::audio_app_patch_add_csound();          nm="Csound"; }
        else                        { eng=PatchKnob::app::audio_app_patch_add_builtin("sine");   nm="Sine";  }
        pv_mirror_node(eng, nm, cat, x, y);
        // Instrument-like nodes (VST / Pure Data / Rack) each get a DISTINCT MIDI
        // channel up front AND auto-create + connect their own master-mixer audio
        // track (MidiIn -> inst -> mixer inlet), so inserting an instrument is
        // instantly playable through its own live mixer channel.
        if (eng >= 0 && (cat=="Instrument" || cat=="Pure Data" || cat=="Modular (Rack)" || cat=="Sampler" || cat=="Csound")) {
            assign_instr_channel(eng);
            ensure_instr_wired(eng);
            // Auto-create an ARRANGE LANE for this instrument, routed to its own
            // mixer channel + MIDI channel, so a new instrument is instantly a
            // playable/editable track (you can still reassign a track to another
            // instrument via the track-header dropdown).
            int idx = -1;
            for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { idx = i; break; }
            if (idx >= 0) {
                perf.new_sequence(idx); perf.set_active(idx, true);
                if (sequence* ns = perf.get_sequence(idx)) {
                    ns->set_name(nm + " " + std::to_string(idx));
                    int ch = PatchKnob::app::audio_app_patch_node_channel(eng);
                    if (ch >= 0) ns->set_midi_channel((char)ch);
                    auto it = g_instrTrack.find(eng);
                    if (it != g_instrTrack.end()) {
                        g_seqToTrack[idx] = it->second;  // share its mixer channel
                        ns->set_midi_bus((char)it->second);
                    }
                    if (!g_songMode) perf.sequence_playing_on(idx);
                    rebind_project_views();
                }
            }
        }
        app.request_redraw();
    };
    // PipeWire-style MIDI ports: create a virtual or hardware-bound in/out node.
    vPatch.on_add_midi_port = [&](double x, double y, int dir, int hw){
        if (!audio_ok) return;
        int eng = (dir==0) ? PatchKnob::app::audio_app_patch_add_midi_in(hw)
                           : PatchKnob::app::audio_app_patch_add_midi_out(hw);
        if (eng < 0) return;
        std::string nm;
        if (hw < 0) nm = (dir==0) ? "MIDI In" : "MIDI Out";
        else {
            char dn[128] = {0}; int cur=0;
            if (dir==0) PatchKnob::app::audio_app_midi_input_info (hw, dn, sizeof(dn), &cur);
            else        PatchKnob::app::audio_app_midi_output_info(hw, dn, sizeof(dn), &cur);
            nm = dn[0] ? std::string(dn) : ((dir==0) ? std::string("MIDI In") : std::string("MIDI Out"));
        }
        pv_mirror_node(eng, nm, "MIDI", x, y);
        app.request_redraw();
    };
    vPatch.on_connect = [&](pb::NodeId fn, pb::PortId fp, pb::NodeId tn, pb::PortId tp){
        if (!audio_ok) return;
        if (!PatchKnob::app::audio_app_patch_connect((int)fn,(int)fp,(int)tn,(int)tp)) {
            // engine refused (kind mismatch / cycle / fan-in cap): revert the
            // optimistic UI edge so the canvas matches the real graph.
            vPatch.remove_connection(pb::Connection(pb::PortRef(fn,fp), pb::PortRef(tn,tp)));
            app.request_redraw();
        } else if ((int)tn == PatchKnob::app::audio_app_patch_out_node()) {
            // Wiring into Audio Out means "I want to hear the patch" -> switch the
            // render core to the modular graph so it actually reaches the device.
            PatchKnob::app::audio_app_set_modular(true);
        }
        // Wiring a HARDWARE MIDI-in node into an instrument: re-stamp the keyboard's
        // output onto that instrument's MIDI channel so it actually plays (the
        // instrument filters incoming MIDI by channel; a keyboard rarely matches).
        if (PatchKnob::app::audio_app_patch_is_hw_midi_in((int)fn)) {
            int ch = PatchKnob::app::audio_app_patch_node_channel((int)tn);
            PatchKnob::app::audio_app_patch_midiin_set_out_channel((int)fn, ch);
        }
    };
    vPatch.on_disconnect = [&](pb::NodeId fn, pb::PortId fp, pb::NodeId tn, pb::PortId tp){
        if (audio_ok) PatchKnob::app::audio_app_patch_disconnect((int)fn,(int)fp,(int)tn,(int)tp);
    };
    vPatch.on_remove_node = [&](pb::NodeId id){
        // Singletons (Master Mixer, MIDI In, Audio Out) can't be deleted -- if the
        // patchbay optimistically dropped one, re-mirror it and keep the engine node.
        if (audio_ok) {
            if ((int)id == PatchKnob::app::audio_app_master_mixer_node())
                { pv_mirror_node((int)id, "Master Mixer", "Mixer", 620, 180); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_patch_midi_in_node())
                { pv_mirror_node((int)id, "MIDI In", "MIDI", 40, 70); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_patch_out_node())
                { pv_mirror_node((int)id, "Audio Out", "Audio I/O", 620, 70); app.request_redraw(); return; }
        }
        if ((int)id == g_guiNode && g_guiHandle) {   // close its native GUI first
            PatchKnob::hostwin::editor_close(g_guiHandle); g_guiHandle=nullptr; g_guiNode=-1; guiWin.visible=false;
        }
        // Auto-destroy this instrument's mixer channel: drop its master-mixer
        // track and compact the shared track-index space in BOTH routing maps
        // (instrument tracks and audio/sequence tracks share it, so every index
        // above the removed one shifts down by one).
        bool removedTrack = false;
        if (audio_ok) {
            auto it = g_instrTrack.find((int)id);
            if (it != g_instrTrack.end()) {
                const int removed = it->second;
                PatchKnob::app::audio_app_master_remove_track(removed);
                g_instrTrack.erase(it);
                for (auto& kv : g_instrTrack) if (kv.second > removed) --kv.second;
                for (auto& kv : g_seqToTrack) if (kv.second > removed) --kv.second;
                removedTrack = true;
            }
        }
        if (audio_ok) PatchKnob::app::audio_app_patch_remove((int)id);
        if (removedTrack && sync_patch_connections) sync_patch_connections();
    };

    if (const char* ct = getenv("PATCHKNOBSDL_COORDTEST")) {
        synthPath = ct;
        const int midiN = PatchKnob::app::audio_app_patch_midi_in_node();
        const int outN  = PatchKnob::app::audio_app_patch_out_node();
        vPatch.on_add_module(300, 70, "Instrument");     // adds engine node + UI node
        int plugN = -1;
        for (const auto& n : vPatch.nodes())
            if ((int)n.id != midiN && (int)n.id != outN) plugN = (int)n.id;
        ensure_instr_wired(plugN);
        bool c1 = false, c2 = false;
        if (auto* graph = PatchKnob::app::audio_app_patch_graph()) {
            for (const auto& c : graph->connections()) {
                if ((int)c.from.node == midiN && (int)c.to.node == plugN) c1 = true;
                if ((int)c.from.node == plugN && (int)c.to.node == outN) c2 = true;
            }
        }
        PatchKnob::app::audio_app_route_midi(0,0x90,60,110);
        SDL_Delay(700);
        float pk = audio_ok ? PatchKnob::app::audio_app_engine()->masterPeak() : -1.f;
        printf("[coordtest] plugN=%d c1=%d c2=%d peak=%.4f (%s)\n",
               plugN,(int)c1,(int)c2,pk, pk>0.0001f?"MODULAR UI OK":"silent");
        fflush(stdout);
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return pk>0.0001f?0:2;
    }

    // Headless FREEZE PROBE: load a project, freeze the first sequence that has a
    // trigger, and report the span/tempo/captured-frames/peak/note-bursts so a
    // partial capture is measurable.  PATCHKNOB_FREEZE_PROBE=<path-to-.s24>
    if (const char* fp = getenv("PATCHKNOB_FREEZE_PROBE")) {
        if (!load_project(perf, fp, &autoPlayer)) {
            fprintf(stderr, "[freeze-probe] load failed: %s\n", project_io_last_error());
        } else {
            if (refresh_loaded_project) refresh_loaded_project();
            int target = -1;
            for (int i = 0; i < c_max_sequence; ++i) {
                if (!perf.is_active(i)) continue;
                sequence* s = perf.get_sequence(i);
                if (s && s->get_max_trigger() > 0) { target = i; break; }
            }
            if (target < 0) fprintf(stderr, "[freeze-probe] no sequence with a trigger\n");
            else {
                sequence* s = perf.get_sequence(target);
                const long maxTrig = s->get_max_trigger();
                const long patLen  = s->get_length();
                const double tempo = PatchKnob::app::audio_app_tempo();
                const double sr    = PatchKnob::app::audio_app_sample_rate();
                const long long tsEnd = PatchKnob::app::audio_app_tick_to_sample((long long)maxTrig);
                fprintf(stderr, "[freeze-probe] target=%d ch=%d maxTrig=%ld patLen=%ld tempo=%.2f sr=%.0f "
                        "tick_to_sample(maxTrig)=%lld (%.2fs)\n",
                        target, s->get_midi_channel(), maxTrig, patLen, tempo, sr, tsEnd, sr>0?(double)tsEnd/sr:0.0);
                auto trIt = g_seqToTrack.find(target);
                const int srcTrack = trIt != g_seqToTrack.end() ? trIt->second : 0;
                PatchKnob::engine::AudioClip clip;
                const bool ok = render_seq_span(target, srcTrack, 0, maxTrig, clip);
                const long long nfr = clip.numFrames();
                float peak = 0.f; int bursts = 0; bool above = false; float envf = 0.f;
                float rmsMin = 1e9f, rmsMax = 0.f;
                for (long long i = 0; i < nfr; ++i) {
                    const float a = std::fabs(clip.ch[0][(size_t)i]);
                    if (a > peak) peak = a;
                    envf += (a - envf) * 0.002f;
                    if (i > 1000) { if (envf < rmsMin) rmsMin = envf; if (envf > rmsMax) rmsMax = envf; }
                    if (!above && envf > 0.03f) { above = true; ++bursts; }
                    else if (above && envf < 0.01f) above = false;
                }
                fprintf(stderr, "[freeze-probe] srcTrack=%d ok=%d frames=%lld (%.2fs) peak=%.4f bursts=%d envRange=[%.4f..%.4f]\n",
                        srcTrack, (int)ok, nfr, sr>0?(double)nfr/sr:0.0, peak, bursts, rmsMin, rmsMax);

                // PER-NOTE attack check: re-extract every scheduled note onset
                // (same loop as render_seq_span) and verify the captured audio
                // actually RISES at that sample -> counts dropped notes directly,
                // independent of release blur.
                {
                    std::vector<long> onsets;
                    long patLen = s->get_length(); if (patLen < 1) patLen = c_ppqn * 4;
                    for (long base = 0; base < maxTrig; base += patLen) {
                        s->reset_draw_marker();
                        long ts=0, tf=0; int note=0, vel=0; bool sel=false; draw_type dt;
                        while ((dt = s->get_next_note_event(&ts,&tf,&note,&sel,&vel)) != DRAW_FIN) {
                            if (dt != DRAW_NORMAL_LINKED || ts >= patLen) continue;
                            long onT = base + ts; if (onT < maxTrig) onsets.push_back(onT);
                        }
                    }
                    const long long w = PatchKnob::app::audio_app_tick_to_sample(6);   // ~pre/post window
                    int rendered = 0;
                    for (long ot : onsets) {
                        const long long os = PatchKnob::app::audio_app_tick_to_sample(ot);
                        float pre = 0.f, post = 0.f;
                        for (long long i = os - w; i < os;     ++i) if (i>=0 && i<nfr) { float a=std::fabs(clip.ch[0][(size_t)i]); if(a>pre)pre=a; }
                        for (long long i = os;     i < os + w; ++i) if (i>=0 && i<nfr) { float a=std::fabs(clip.ch[0][(size_t)i]); if(a>post)post=a; }
                        if (post > pre * 1.3f + 0.02f) ++rendered;                  // attack transient present
                    }
                    fprintf(stderr, "[freeze-probe] notes: scheduled=%d rendered=%d (%s)\n",
                            (int)onsets.size(), rendered,
                            rendered >= (int)onsets.size() ? "ALL" : "MISSING NOTES");
                }

                // Full on_freeze_track path: does it create a lane + is the frozen
                // clip AUDIBLE in modular playback?
                const int activeBefore = [&]{ int n=0; for(int i=0;i<c_max_sequence;++i) if(perf.is_active(i)) ++n; return n; }();
                if (vArrange.on_freeze_track) vArrange.on_freeze_track(target);
                const int activeAfter = [&]{ int n=0; for(int i=0;i<c_max_sequence;++i) if(perf.is_active(i)) ++n; return n; }();
                // roll + render modular blocks off sample 0, read master peak.
                float ppk = PatchKnob::app::audio_app_probe_render_peak(96);
                fprintf(stderr, "[freeze-probe] on_freeze_track: activeSeqs %d->%d, playback peak=%.4f (%s)\n",
                        activeBefore, activeAfter, ppk, ppk > 0.001f ? "PLAYS" : "SILENT");
            }
        }
        fflush(stderr);
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return 0;
    }

    app.roots = { &transportSmall };
    for (int i=0;i<NV;++i){ if (i==V_PIANO||i==V_TRACKER||i==V_PATCH||i==V_MIXER) continue;  // window content
        views[i]->visible=(i==current); app.roots.push_back(views[i]); }
    app.roots.push_back(&wm);        // floating windows: over the workspace...
    app.roots.push_back(&menubar);   // ...menu bar LAST -> on top, mouse first

    app.on_layout = [&](App& a){
        // apply scan/cache results whenever fresh (boot load or a rescan)
        if (g_scanReady.exchange(false, std::memory_order_acq_rel)) {
            vBrowser.set_scanning(false); vBrowser.populate(g_scan);
        }
        // Grow/shrink a RackNode's patch ports as Audio-In/Out modules are loaded
        // in the rack editor (poll its I/O signature; recompile + re-mirror on change).
        if (g_rackNode >= 0) {
            if (rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine(g_rackNode)) {
                const int sig = eng->audioOutCount() * 1000 + eng->audioInCount();
                if (sig != g_rackIOsig) {
                    g_rackIOsig = sig;
                    if (auto* g = PatchKnob::app::audio_app_patch_graph()) g->compileAndPublish();
                    resync_patch_node(g_rackNode);
                    if (sync_patch_connections) sync_patch_connections();
                }
            } else g_rackNode = -1;
        }
        // Same for a PdNode: as adc~/dac~ channels change (patch edit/reload), its
        // audio in/out ports grow/shrink -- re-mirror them when the signature moves.
        if (g_pdNode >= 0) {
            const int sig = PatchKnob::app::audio_app_pd_io_sig(g_pdNode);
            if (sig >= 0 && sig != g_pdIOsig) { g_pdIOsig = sig; resync_patch_node(g_pdNode);
                                                if (sync_patch_connections) sync_patch_connections(); }
        }
        // Route raw character input to the Csound editor ONLY while its window is
        // focused (single-field text edits still take precedence in the loop).  We
        // must also (re)enable SDL text input here: end_text() calls
        // SDL_StopTextInput() after any field edit, which would otherwise leave the
        // code window unable to receive SDL_TEXTINPUT ("sometimes won't let you type").
        static bool s_csoundTextOn = false;
        const bool wantCsoundText = csoundWin.visible && csoundWin.focused && !a.editing_text();
        if (wantCsoundText) {
            a.text_input_sink = [&csoundEditor](const char* s){ csoundEditor.insert(s); };
            if (!s_csoundTextOn) { SDL_StartTextInput(); s_csoundTextOn = true; }
        } else {
            if (a.text_input_sink) a.text_input_sink = nullptr;
            if (s_csoundTextOn) { if (!a.editing_text()) SDL_StopTextInput(); s_csoundTextOn = false; }
        }
        // Embedded native plugin GUI: keep the child window tracking its SDL
        // frame (physical px), pump idle (VST2), reap if the user closed it.
        if (g_guiHandle) {
            if (!PatchKnob::hostwin::editor_alive(g_guiHandle)) {
                PatchKnob::hostwin::editor_close(g_guiHandle); g_guiHandle=nullptr; g_guiNode=-1; guiWin.visible=false;
            } else if (guiWin.visible) {
                int dw=a.w, dh=a.h; SDL_GetRendererOutputSize(a.ren, &dw, &dh);
                float rx = a.w>0 ? (float)dw/a.w : 1.f, ry = a.h>0 ? (float)dh/a.h : 1.f;
                SDL_Rect b = guiWin.body();
                PatchKnob::hostwin::editor_set_bounds(g_guiHandle, (int)(b.x*rx),(int)(b.y*ry),
                                                  (int)(b.w*rx),(int)(b.h*ry));
                PatchKnob::hostwin::editor_show(g_guiHandle, true);
                PatchKnob::hostwin::editor_idle(g_guiHandle);
            } else {
                PatchKnob::hostwin::editor_show(g_guiHandle, false);
            }
        }
        // emit automation while playing (coarse UI-thread pump, block-granular).
        // Tick comes from the TRANSPORT (perform derives m_tick from the audio
        // clock) -- never from a cached sequence pointer, which dangles if that
        // sequence is deleted in the arrange view (was a use-after-free crash).
        static long prevTick=0;
        if (a.animating) {
            long cur = perf.get_tick();
            if (cur != prevTick) {
                autoPlayer.advance(prevTick, cur,
                    [](int trk, unsigned id, float v){ PatchKnob::app::audio_app_route_param(trk,id,v); },
                    [](int trk, int cc, int val){ PatchKnob::app::audio_app_route_midi(trk,0xB0,(unsigned char)cc,(unsigned char)val); });
                prevTick = cur;
            }
            if (vTracker.poll_playhead())
                a.request_redraw();
            // Engine-driven FX for EVERY OTHER active pattern (poll_playhead above
            // covers the one shown in the tracker, live).  This plays each active
            // pattern's VST-param automation regardless of which window is focused
            // and for multiple/consecutive patterns on the same track.  Row grid uses
            // the tracker's default LPB (c_ppqn/4).
            {
                static std::vector<int> s_fxRow;   // per-slot last-fired row (-1 none)
                if ((int)s_fxRow.size() < c_max_sequence) s_fxRow.assign(c_max_sequence, -1);
                sequence* shown = vTracker.get_sequence();
                const int tpr = std::max(1, c_ppqn / 4);
                for (int i = 0; i < c_max_sequence; ++i) {
                    if (!perf.is_active(i)) { s_fxRow[i] = -1; continue; }
                    sequence* s = perf.get_sequence(i);
                    if (!s || s == shown || !s->get_playing()) { s_fxRow[i] = -1; continue; }
                    const int nr  = std::max(1, (int)(s->get_length() / tpr));
                    const int cur = (int)((s->get_last_tick() / tpr) % nr);
                    if (cur != s_fxRow[i]) {
                        ui::TrackerView::play_pattern_fx(s, s_fxRow[i], cur, nr);
                        s_fxRow[i] = cur;
                    }
                }
            }
        }
        // Free deleted sequences a couple of frames after retirement (bounds the
        // graveyard instead of holding every deleted sequence until shutdown).
        perf.gc_graveyard();
        // live transport record: accumulate captured events while armed+rolling
        // (committed to a new timeline clip when REC is toggled off).
        if (g_recArmed && drain_record) drain_record();
        // drain the armed record node -> pair note on/off -> add notes to seq0
        if (g_recNode >= 0 && seq0) {
            static std::map<int,std::pair<long,int>> pending;   // pitch -> (start tick, vel)
            long ticks[256]; unsigned char st[256], d1[256], d2[256];
            int n = PatchKnob::app::audio_app_record_drain(g_recNode, ticks, st, d1, d2, 256);
            bool any = false;
            for (int i=0;i<n;++i){
                unsigned char hi = st[i] & 0xF0; int pitch = d1[i];
                if (hi==0x90 && d2[i]>0) pending[pitch] = { ticks[i], d2[i] };
                else if (hi==0x80 || (hi==0x90 && d2[i]==0)) {
                    auto it = pending.find(pitch);
                    if (it != pending.end()) {
                        long start = it->second.first, dur = ticks[i]-start; if (dur<1) dur=1;
                        seq0->add_note(start, dur, pitch); pending.erase(it); any = true;
                    }
                }
            }
            if (any) { seq0->set_dirty(); a.request_redraw(); }
        }
        const int menuH = menubar.bar_h;     // row 1: menu bar
        const int th    = 26;                // row 2: transport / tabs
        menubar.rect   = { 0, 0, a.w, a.h }; // full-window for click capture; paints bar+dropdown only
        wm.rect        = { 0, 0, a.w, a.h }; // floating-window layer (hit-tests its windows)
        wm.workspace   = { 0, menuH+th, a.w, a.h-menuH-th };  // max bounds + minimized tray
        transportSmall.rect = { 0, menuH, a.w, th };          // compact transport strip
        SDL_Rect content = { 0, menuH+th, a.w, a.h-menuH-th };
        for (int i=0;i<NV;++i){ if (i==V_PIANO||i==V_TRACKER||i==V_PATCH||i==V_MIXER) continue; views[i]->rect = content; }
        // Patchbay window snapped to the bottom (full width, lower third).
        if (patchWin.visible) { int dh = a.h/3; if (dh < 200) dh = 200;
            patchWin.rect = { 0, a.h - dh, a.w, dh }; }
    };

    // Device-loss watchdog: if the audio device vanished (unplugged / stream
    // died), try to reopen on the default device.  ~4 Hz, message thread.
    app.on_tick = [&](App& a){
        if (audio_ok && PatchKnob::app::audio_app_device_lost()) {
            if (PatchKnob::app::audio_app_try_recover())
                std::fprintf(stderr, "[audio] device lost -> recovered on default device\n");
            a.request_redraw();
        }
    };

    // CPU (DSP-load) meter, drawn on top at the far right of the menu strip.
    app.run([&](App& a){
        float load = PatchKnob::app::audio_app_cpu_load();
        int pct = (int)(load * 100.f + 0.5f); if (pct > 999) pct = 999; if (pct < 0) pct = 0;
        char buf[24]; std::snprintf(buf, sizeof(buf), "CPU %d%%", pct);
        const Theme& t = theme();
        SDL_Rect mb{ a.w - 56, 6, 48, 12 };
        fill_rect(a.ren, mb, t.keybg);
        frame_rect(a.ren, mb, t.dim);
        int fw = (int)(load * 48.f + 0.5f); if (fw > 48) fw = 48; if (fw < 0) fw = 0;
        if (fw > 0) { SDL_Rect mf{ mb.x, mb.y, fw, mb.h }; fill_rect(a.ren, mf, load < 1.0f ? t.accent : t.note); }
        int tw = a.font.text_w(buf);
        a.font.draw(a.ren, mb.x - tw - 6, (24 - a.font.ch())/2, buf, t.text);
        std::string status = projectStatus;
        const int maxStatusW = std::max(0, mb.x - tw - 224);
        while (status.size() > 3 && a.font.text_w(status) > maxStatusW)
            status = status.substr(0, status.size() - 4) + "...";
        if (!status.empty() && maxStatusW > 0)
            a.font.draw(a.ren, 218, (24 - a.font.ch())/2, status, t.dim);
    });
    if (g_guiHandle) PatchKnob::hostwin::editor_close(g_guiHandle);
    PatchKnob::app::audio_app_shutdown();
    app.shutdown();
    return 0;
}

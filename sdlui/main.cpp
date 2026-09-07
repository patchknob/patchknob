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
#include "platform/platform_ui.h"
#include "platform/mobile_ui_host.h"
#include "audio_app.h"
#include "engine/plugin_api.h"
#include "engine/audio/audio_engine.h"
#include "engine/host/plugin_host.h"
#include "engine/patch/patch_graph.h"
#include "engine/automation/automation_player.h"
#include "engine/rack/rack_engine.h"
#include "engine/audioclip/consolidate.h"
#include "engine/audioclip/audio_clip.h"
#include "perform.h"
#include "quantize.h"
#include "sequence.h"

#include "views/piano_roll/pianoroll.h"
#include "views/tracker/tracker_view.h"
#include "views/arrange/arrange_view.h"
#include "views/mixer/mixer_view.h"
#include "views/patchbay/patch_view.h"
#include "views/sample_editor/sample_editor_view.h"
#include "views/sampler_editor/sampler_editor_view.h"
#include "views/sample_slot/sample_slot_editor.h"
#include "views/sample_slot/sampler_instrument_slot.h"
#include "engine/sampler/sampler_instrument.h" // SamplerZoneInfo read-back
#include "engine/sf2/sf2_reader.h"
#include "engine/sf2/sf2_to_sampler.h"
#include "engine/sf2/sf2_import_service.h"
#include "engine/audioclip/wav_loader.h"
#include "tab_host.h"
#include "views/browser/browser_view.h"
#include "views/automation/automation_view.h"
#include "views/waveform/waveform_view.h"
#include "views/waveform/audio_track.h"
#include "views/audio_settings/audio_settings_view.h"
#include "views/video_settings/video_settings_view.h"
#include "screen_recorder.h"
#include "gpu_meter.h"
#include "folder_picker.h"
#include "views/file_dialog/file_dialog.h"
#include "views/plugin_picker/plugin_picker_view.h"
#include "views/plugin_param/plugin_param_view.h"
#include "views/master_mixer/master_mixer_view.h"
#include "views/pd_editor/pd_editor_view.h"
#include "views/csound_editor/csound_editor_view.h"
#ifdef PATCHKNOB_HAS_AI
#include "views/csound_editor/ai_settings_view.h"
#include "views/csound_editor/chat_panel.h"
#endif
#include "views/rack_editor/rack_editor_view.h"
#include "views/cdp_editor/cdp_editor_view.h"
#include "views/panel_editor/panel_editor_view.h"
#include "plugin_editor_window.h"
#include "transport_bar.h"

class VirtualMidiConfigView : public ui::Widget {
public:
    std::function<int()> get_inputs, get_outputs;
    std::function<void(int,int)> set_counts;
    void draw(ui::App& app) override {
        const ui::Theme& t=ui::theme(); ui::fill_rect(app.ren,rect,t.panel);
        const int in=get_inputs?get_inputs():1, out=get_outputs?get_outputs():1;
        app.font.draw(app.ren,rect.x+16,rect.y+18,"Virtual MIDI inputs",t.text);
        app.font.draw(app.ren,rect.x+190,rect.y+18,std::to_string(in),t.accent);
        app.font.draw(app.ren,rect.x+16,rect.y+58,"Virtual MIDI outputs",t.text);
        app.font.draw(app.ren,rect.x+190,rect.y+58,std::to_string(out),t.accent);
        for(int r=0;r<2;++r) for(int b=0;b<2;++b) {
            SDL_Rect q{rect.x+225+b*34,rect.y+12+r*40,28,26};
            ui::frame_rect(app.ren,q,t.dim);
            app.font.draw_centered(app.ren,q,b?"-":"+",t.text);
        }
        app.font.draw(app.ren,rect.x+16,rect.y+98,
            "Hardware MIDI In -> Virtual In; Virtual Out -> instrument",t.dim);
    }
    bool on_mouse(ui::App& app,const ui::MouseEv& e) override {
        if(!e.pressed||e.button!=SDL_BUTTON_LEFT) return true;
        int ins=get_inputs?get_inputs():1, outs=get_outputs?get_outputs():1;
        for(int r=0;r<2;++r) for(int b=0;b<2;++b) {
            SDL_Rect q{rect.x+225+b*34,rect.y+12+r*40,28,26};
            if(e.x>=q.x&&e.x<q.x+q.w&&e.y>=q.y&&e.y<q.y+q.h) {
                int& v=r?outs:ins; v+=b?-1:1; if(v<1)v=1;if(v>32)v=32;
                if(set_counts)set_counts(ins,outs); app.request_redraw(); return true;
            }
        }
        return true;
    }
};
#include "project_io.h"

#include <map>
#include <set>
#include <tuple>
#include <cstring>
#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <filesystem>
#include <deque>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN   // keep rpcndr.h's 'byte' away from std::byte
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod (winmm, already linked)
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

// Splits a full path into the directory to open the dialog on and the bare
// filename to pre-fill, so re-opening "Save As" starts from where the
// project/patch already lives instead of always landing on the home folder.
static void fs_path_split(const std::string& current, std::string& dir, std::string& name) {
    if (current.empty()) { dir.clear(); name.clear(); return; }
    std::filesystem::path p(current);
    dir = p.has_parent_path() ? p.parent_path().string() : std::string();
    name = p.filename().string();
}

// Project / editor / sample "open" and "save" pickers all go through the one
// in-app SDL file dialog (views/file_dialog) on every platform -- there is no
// OS window to host a native GetOpenFileName/GetSaveFileName under KMSDRM,
// and a single picker means identical behaviour on Windows/Linux/macOS
// instead of three divergent code paths.
static bool choose_project_file(ui::App& app, bool save, const std::string& current,
                                std::string& selected)
{
    ui::FileDialog::Options opt;
    opt.save = save;
    opt.title = save ? "Save Project" : "Open Project";
    opt.extensions = { "s24" };
    fs_path_split(current, opt.startDir, opt.defaultName);
    if (opt.defaultName.empty()) opt.defaultName = "project.s24";
    return ui::choose_path(app, opt, selected);
}

static bool choose_editor_file(ui::App& app, bool save, const std::string& current,
                               const char* label, const char* extension,
                               std::string& selected)
{
    ui::FileDialog::Options opt;
    opt.save = save;
    opt.title = label;
    opt.extensions = { extension };
    fs_path_split(current, opt.startDir, opt.defaultName);
    if (opt.defaultName.empty()) opt.defaultName = std::string("patch.") + extension;
    return ui::choose_path(app, opt, selected);
}

// Open-file dialog for a WAV sample (sampler browser).  Returns false if cancelled.
static bool choose_wav_file(ui::App& app, std::string& selected)
{
    ui::FileDialog::Options opt;
    opt.title = "Load Sample";
    opt.extensions = { "wav" };
    return ui::choose_path(app, opt, selected);
}

static std::string project_display_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

//============================================================================
//  PATCHKNOB_LOOPREPRO -- headless transport-LOOP regression harness.
//
//  Reproduces the user's report ("with loop on it cuts off the first note
//  sometimes, sometimes it doesn't play the clip at all") on a REAL project
//  instead of a synthetic pattern: it loads the .s24, sets the loop markers to
//  the clip's own span, rolls the engine for N loop passes, and counts every
//  byte each sequence emitted, per pass.
//
//  A correct loop emits an IDENTICAL note-on set on every pass.  The failures
//  this exists to catch are (a) the note sitting exactly on get_left_tick()
//  missing from a pass, (b) the note-on count decaying after a while as
//  m_playing_notes[] leaks and the retrigger guard starts swallowing notes, and
//  (c) a whole pass emitting nothing.
//
//  Env:  PATCHKNOB_LOOPREPRO=<project.s24>
//        PATCHKNOB_LOOPREPRO_L / _R      loop markers in ticks (default 0 ..
//                                        end of the last trigger, bar-aligned)
//        PATCHKNOB_LOOPREPRO_PASSES      loop passes to observe (default 20)
//        PATCHKNOB_LOOPREPRO_BPM         tempo override (default: project's)
//  Exit code 0 iff every observed pass matched pass 1 exactly.
//============================================================================
namespace looprepro {

struct Rec { int seq; long tick; unsigned char status, note, vel; int kind; };
static std::mutex        g_mx;
static std::vector<Rec>  g_recs;
static perform*          g_perf = nullptr;
static bool              g_capture = false;

static void tap( sequence* s, int /*bus*/, int /*chan*/, unsigned char status,
                 unsigned char note, unsigned char vel, long tick, int kind )
{
    if (!g_capture || !g_perf) return;
    int idx = -1;
    for (int i = 0; i < c_max_sequence; ++i)
        if (g_perf->is_active(i) && g_perf->get_sequence(i) == s) { idx = i; break; }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_recs.size() >= 400000) return;          // bounded: never grow forever
    g_recs.push_back(Rec{ idx, tick, status, note, vel, kind });
}

} // namespace looprepro

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
    ui::platform::MobileUiHost mobileUi;
    vPiano.set_preview(true);   // key-strip clicks preview through the selected instrument
    // TWO LOOPS, AND THEY ARE NOT THE SAME LOOP.
    //
    //   * The piano roll's ruler loop is the SEQUENCE's own loop
    //     (sequence::get_loop_start()/get_loop_end()) -- pattern-local, stored
    //     per sequence.  Every clip has its own, and different clips are
    //     expected to hold different values at the same time.
    //   * The Arrange view's loop brace is the SONG loop (perf's left/right
    //     tick + looping flag) -- one global transport range.
    //
    // This callback used to map the edited sequence's loop onto wherever that
    // pattern was first placed and write the result into the SONG loop,
    // switching song looping on as a side effect.  That collapsed the two
    // concepts onto one piece of state: opening any clip in the piano roll and
    // nudging its loop silently rewrote the song's loop markers, and with many
    // clips each holding their own loop points, whichever one was edited last
    // won.  Editing a clip must not move the transport's loop.
    //
    // The sequence's loop is already stored by set_loop_start()/set_loop_end()
    // in the piano roll itself (sdlui/views/piano_roll/pianoroll.cpp), so this
    // callback exists only to repaint the views that draw it.
    vPiano.on_loop_changed = [&]{
        // ONE LOOP, TWO CONSUMERS.  The clip's window lives on the sequence;
        // the automation player keeps its own copy on the region so the audio
        // thread never touches a sequence.  Region::id IS the sequence number,
        // so the region is the same clip -- push the window across here, at the
        // edit, rather than only while the automation editor happens to be
        // open.  They used to be seeded once from get_length() and never
        // revisited, so a 1-bar loop on a 4-bar clip repeated bar 1 of the
        // notes while the automation swept all four.
        if (sequence* s = vPiano.get_sequence()) {
            for (int i = 0; i < c_max_sequence; ++i) {
                if (!perf.is_active(i) || perf.get_sequence(i) != s) continue;
                if (auto* rg = autoPlayer.findRegion(i)) {
                    rg->loopStart  = (int64_t) s->get_loop_start();
                    rg->loopLength = (int64_t) s->get_loop_end();
                }
                break;
            }
        }
        app.request_redraw();
    };
    mixer::MixerView           vMixer(PatchKnob::app::audio_app_graph());
    PatchKnob::patchbay::PatchView vPatch;
    samped::SampleEditorView   vSampleEd;      // Ableton-style warp / sample editor
    ui::BrowserView            vBrowser;
    automation::AutomationView vAuto(seq0, 0, &autoPlayer);
    automation::KeyFollowPanel vKey(&perf);            vKey.set_tracks({});
    waveform::WaveformView     vWave;

    // --- background-worker -> UI hand-back ---------------------------------
    // `app` and `perf` are automatic objects in main().  Every worker below used
    // to capture them BY REFERENCE and was detached, so a plugin scan or a
    // realtime bounce still in flight when the run loop returned called back
    // into freed stack -- quitting within a few seconds of launch (the boot
    // scan always runs) or mid-render was a crash.  Workers now capture this
    // shared block by VALUE; main() nulls the App* under the mutex before it
    // shuts anything down, so a late callback is dropped instead of fatal.
    struct UiLink { std::mutex m; ui::App* app = nullptr; };
    auto uiLink = std::make_shared<UiLink>();
    uiLink->app = &app;

    vBrowser.set_track(0);
    vBrowser.on_load_instrument = [](const PatchKnob::engine::PluginDescriptor& d){
        PatchKnob::app::audio_app_set_track_instrument(0, d); };
    vBrowser.on_add_fx = [](const PatchKnob::engine::PluginDescriptor& d){
        PatchKnob::app::audio_app_add_track_fx(0, d); };

    // Plugin inventory, cached on disk so we don't re-probe every boot.  Load the
    // cache if present (instant); otherwise scan once and write it.  "Rescan
    // Plugins" (View menu) forces a fresh probe + cache rewrite.
    static std::vector<PatchKnob::engine::PluginDescriptor> g_scan;
    // g_scan is written by the scanner thread and read by the UI thread
    // (BROWSE repopulate, the plugin picker).  It is a std::vector, not an
    // atomic: without this lock the two threads reallocated and iterated the
    // same buffer.
    static std::mutex g_scanMutex;
    static std::atomic<bool> g_scanDone{false};    // any inventory available yet
    static std::atomic<bool> g_scanReady{false};   // fresh data waiting to apply
    // ONE scanner at a time.  "Rescan Plugins" pressed during the boot scan used
    // to start a second thread assigning the same vector.
    static std::atomic<bool> g_scanBusy{false};
    // NOT a bare std::thread: these statics are destroyed by exit(), and a
    // std::thread that is still joinable in its destructor calls
    // std::terminate().  That is exactly how the Dragonfly Reverb X-error
    // (which used to reach libX11's default exit()-ing handler) turned into a
    // SIGABRT: exit() destroyed g_scanThread while it was joinable -- the scan
    // thread stays joinable even AFTER it finishes, until someone reaps it.
    // The normal teardown paths still join (WorkerReaper below, and the
    // explicit joins at the bottom of main()); the destructor detaching is
    // strictly a last-resort so an unexpected exit() can never abort the app.
    struct ExitSafeThread {
        std::thread t;
        ~ExitSafeThread() { if (t.joinable()) t.detach(); }
        ExitSafeThread& operator=(std::thread&& o) { t = std::move(o); return *this; }
        bool joinable() const { return t.joinable(); }
        void join()   { t.join(); }
        void detach() { t.detach(); }
    };
    static ExitSafeThread g_scanThread;
    auto run_scan = [&, uiLink](bool forceRescan){
        if (g_scanBusy.exchange(true)) return;      // one already in flight
        if (g_scanThread.joinable()) g_scanThread.join();   // reap the finished one
        vBrowser.set_scanning(true);
        // Capture NOTHING by reference: this thread outlives the gesture that
        // started it and may outlive main()'s stack frame entirely.  `uiLink`
        // is a shared_ptr whose App* main() nulls (under the lock) before it
        // tears the window down, so the redraw below is either delivered to a
        // live App or dropped.
        g_scanThread = std::thread([uiLink,forceRescan]{
            std::vector<PatchKnob::engine::PluginDescriptor> res;
            bool got = false;
            if (auto* host = PatchKnob::app::audio_app_host()) {
                host->setCachePath("PatchKnob_plugins.cache");
                host->setProbeTimeoutMs(5000);         // cap a hung probe at 5s (was 15s)
                host->resetScanCancel();               // a previous Quit may have set it
                // A usable cache means no probing at all; otherwise scan()
                // probes and rewrites the cache.
                if (forceRescan || !host->loadCache(res) || res.empty())
                    res = host->scan({});
                got = true;
            }
            if (got) { std::lock_guard<std::mutex> lk(g_scanMutex); g_scan = std::move(res); }
            g_scanDone.store(true, std::memory_order_release);
            g_scanReady.store(true, std::memory_order_release);
            g_scanBusy.store(false, std::memory_order_release);   // last: makes the join cheap
            std::lock_guard<std::mutex> lk(uiLink->m);
            if (uiLink->app) uiLink->app->request_redraw();
        });
    };
    run_scan(false);

    const int NV = 9;
    // The WAVE workspace tab shipped with no binding at all, so it was
    // permanently empty.  Poll the Sample Editor's current sequence -- that is
    // the clip the user is actually looking at -- and rebind when it changes.
    vWave.clip_source = [&]() -> const PatchKnob::engine::AudioClip* {
        const int seq = vSampleEd.bound_seq();
        return seq >= 0 ? vArrange.audio_clip(seq) : nullptr;
    };
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
    std::function<void(bool)> set_arrange_visible;
    auto show = [&](int i){ current=i; for(int k=0;k<NV;++k) views[k]->visible=(k==i);
        if(set_arrange_visible)set_arrange_visible(i==V_ARRANGE);
        app.request_redraw(); };

    static std::string synthPath; { const char* v=getenv("PATCHKNOBSDL_VST");
        synthPath = v?v:""; }   // NO default VST instrument: a hardcoded path
        // that does not exist on this machine made "Add Instrument" a
        // silent no-op (audio_app_patch_add_plugin returns -1).

    // --- realtime-bounce helper (RENDER menu item) -------------------------
    static std::atomic<bool> g_rendering{false};
    // Set by the teardown path so an in-flight bounce cuts its wait short; the
    // render worker drives `perf` (an automatic object in main()) so it is
    // JOINED at teardown rather than merely guarded.
    static std::atomic<bool> g_quitting{false};
    static ExitSafeThread g_renderThread;   // see ExitSafeThread above
    auto do_render = [&, uiLink]{
        if (!audio_ok || g_rendering.load()) return;
        if (g_renderThread.joinable()) g_renderThread.join();   // reap the finished one
        double secs = 4.0;
        for (int s=0;s<c_max_sequence;++s) if (perf.is_active(s)) {
            sequence* sq = perf.get_sequence(s);
            if (sq) { double b = (double)sq->get_length()/(double)c_ppqn;
                      double t = b * 60.0 / 120.0; if (t>secs) secs=t; } }
        g_rendering.store(true);
        g_renderThread = std::thread([&perf,uiLink,secs]{
            if (PatchKnob::app::audio_app_capture_begin(secs)) {
                perf.start(false); PatchKnob::app::audio_app_patch_set_playing(true);
                // Was a single SDL_Delay of the whole bounce length, which is
                // what made joining this thread on quit unacceptable.  Poll so
                // Quit interrupts the wait and the join costs ~10 ms.
                // (auto: SDL_GetTicks is Uint32 on SDL2 and Uint64 on SDL3.)
                const auto t0 = SDL_GetTicks();
                const auto waitMs = (decltype(t0))(secs * 1000.0) + 200;
                while (!g_quitting.load(std::memory_order_acquire) &&
                       SDL_GetTicks() - t0 < waitMs)
                    SDL_Delay(10);
                perf.stop(); PatchKnob::app::audio_app_patch_set_playing(false);
                PatchKnob::app::audio_app_capture_end_wav("render.wav");
            }
            g_rendering.store(false);
            std::lock_guard<std::mutex> lk(uiLink->m);
            if (uiLink->app) uiLink->app->request_redraw();
        });
    };

    // Backstop for the headless self-test paths further down, which return from
    // main() without reaching the teardown block at the bottom: a static
    // std::thread that is still joinable when its destructor runs calls
    // std::terminate.  Declared here so it is destroyed BEFORE `perf` and `app`
    // (locals unwind in reverse declaration order), and idempotent so running
    // after the normal teardown is a no-op.
    struct WorkerReaper {
        std::shared_ptr<UiLink> link;
        ~WorkerReaper() {
            g_quitting.store(true, std::memory_order_release);
            { std::lock_guard<std::mutex> lk(link->m); link->app = nullptr; }
            if (g_renderThread.joinable()) g_renderThread.join();
            if (g_scanThread.joinable()) {
                if (auto* host = PatchKnob::app::audio_app_host()) host->cancelScan();
                for (int i = 0; i < 800 && g_scanBusy.load(std::memory_order_acquire); ++i)
                    SDL_Delay(10);
                if (g_scanBusy.load(std::memory_order_acquire)) g_scanThread.detach();
                else                                            g_scanThread.join();
            }
        }
    } workerReaper{uiLink};

    // --- top menu bar (File / View / Audio / Help) -------------------------
    static Color topbg; topbg = theme().panel;
    std::function<void()> refresh_loaded_project;
    std::function<void()> new_project;
    std::function<void()> clear_project_edit_state;
    // File>Open replaces the whole project under a LIVE shell, so it has the
    // same two obligations File>New has.  Both need state declared much further
    // down (the sub-editors and their windows, the mixer strip, the transport
    // session flags, the undo ring), so they are forward-declared here and
    // bound where that state exists.
    //   detach_project_editors  : run BEFORE load_project -- unbind every editor
    //                             that caches a raw pointer/id into the graph
    //                             load_project is about to garbage-collect, and
    //                             drop shell state that is not in the file.
    //   reset_undo_history      : run AFTER  -- a project swap is not an undo
    //                             step; the ring must start over on the new one.
    std::function<void()> detach_project_editors;
    std::function<void()> reset_undo_history;
    // Punch out (and commit) a live take if one is running.  Anything that
    // REPLACES the sequence pool under the recorder has to call this first:
    // load_project deletes every sequence, but the recorder's armed lane index,
    // its capture-open MIDI track node and its take-relative origin are all
    // plain globals that survive the load.  Without this, opening a project (or
    // an undo, which restores a whole project file) while armed left the capture
    // running against the dead project and then committed that take into
    // whatever lane happened to occupy the old index -- partitioning it, and
    // placing a clip from another song on it.  Assigned with toggle_record.
    std::function<void()> punch_out_recording;
    std::function<void()> global_undo;
    std::function<void()> global_redo;
    // Freeze save/restore (bound after g_frozen is declared): collect_freezes
    // snapshots g_frozen for the FRZ section; apply_loaded_freezes re-shows the
    // frozen lanes (tint + waveform) after a load and repopulates g_frozen.
    std::function<std::vector<ProjectFreezeRecord>()> collect_freezes;
    std::function<void()> apply_loaded_freezes;
    //  Rebinds ordinary (non-frozen) audio lanes to the view after a
    //  load; assigned next to apply_loaded_freezes, which it mirrors.
    std::function<void()> rebind_loaded_audio_lanes;
    // Mirror the engine PatchGraph's connections (incl. the instrument->mixer
    // wire made by master_connect_instrument) into the patchbay UI, so the cord
    // is VISIBLE.  Forward-declared: assigned once refresh_master_ui exists.
    std::function<void()> sync_patch_connections;
    std::string projectPath;
    std::string projectStatus = "Project: Untitled";
    auto update_project_title = [&]{
        const std::string name = projectPath.empty() ? "Untitled" : project_display_name(projectPath);
        SDL_SetWindowTitle(app.window, ("PatchKnob SDL - " + name).c_str());
    };
    update_project_title();
    auto save_to_project = [&](const std::string& path) {
        vTracker.commit_fx();   // flush the live pattern's FX edits into its sequence
        if(sequence* ts=vTracker.get_sequence())for(auto& rg:autoPlayer.regions())
            if(perf.is_active(rg.id)&&perf.get_sequence(rg.id)==ts){rg.trackerFx=ts->get_fx_blob();break;}
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
            // ch.32: the fade-settings "Session Folder" destination
            const size_t sl = path.find_last_of("/\\");
            vArrange.set_session_folder(sl == std::string::npos ? "." : path.substr(0, sl));
        }
        app.request_redraw();
    };
    auto save_as_project = [&]{
        std::string path = projectPath;
        if (choose_project_file(app, true, path, path)) save_to_project(path);
    };
    auto open_project = [&]{
        std::string path = projectPath;
        if (!choose_project_file(app, false, path, path)) return;
        // A take belongs to the project it was played into: commit it before the
        // pool it targets is deleted.
        if (punch_out_recording) punch_out_recording();
        // The editors hold non-owning sequence pointers. Detach them before
        // load_project replaces/deletes the sequence pool.
        vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
        vPiano.cancel_interaction(app); vPiano.set_sequence(nullptr);
        // The rack/panel/Pd/Csound editors and the mixer strip are bound to the
        // OLD graph.  load_project calls audio_app_project_reset_patch(), which
        // collects the RackEngine the rack editor caches as a raw pointer and
        // dereferences on every draw().  Unbind first -- New and Undo already
        // did this, Open was the one path that did not.
        if (detach_project_editors) detach_project_editors();
        if (!load_project(perf, path, &autoPlayer)) {
            projectStatus = "Open failed: " + std::string(project_io_last_error());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open project failed",
                                     project_io_last_error(), app.window);
            if(refresh_loaded_project)refresh_loaded_project();
        } else {
            if(clear_project_edit_state)clear_project_edit_state();
            vArrange.reset_project_state();
            projectPath = path;
            projectStatus = "Opened: " + path;
            {   // ch.32: the fade-settings "Session Folder" destination, and
                // the session's AutoFades value back into the preferences UI
                const size_t sl = path.find_last_of("/\\");
                vArrange.set_session_folder(sl == std::string::npos ? "." : path.substr(0, sl));
                vArrange.set_auto_fade_ms((int)PatchKnob::app::audio_app_auto_fade_ms());
            }
            update_project_title();
            if (refresh_loaded_project) refresh_loaded_project();
            // Opening a project is not an edit of the old one.  Without this the
            // mouse-up that dismissed the File menu ran on_edit_end, which
            // pushed project A onto the undo ring -- so one Ctrl+Z silently
            // reloaded the song the user had just left.
            if (reset_undo_history) reset_undo_history();
        }
        app.request_redraw();
    };
    // ---- punch record modes (Pro Tools ch.27) -----------------------------
    // The transport record MODE the shell is in, the set of punch-enabled
    // mixer tracks, and the punch-related preferences.  Declared up here so
    // the menu, the transport wiring and the arrange hooks (all defined
    // between here and the punch machinery near toggle_record) can reference
    // them; the std::functions are assigned after toggle_record exists.
    enum class RecMode { Normal = 0, QuickPunch = 1, TrackPunch = 2,
                         DestructivePunch = 3 };
    static RecMode g_recMode = RecMode::Normal;
    static std::set<int> g_punchTracks;       // punch-enabled MIXER track ids
    static bool g_recReady = false;           // transport record-armed (flashing)
    // One punch PASS = one roll of the transport with input captures open.
    // Each captured track gets its own PunchLane: the engine keeps one INPUT
    // tap per track (MixerNode::kMaxCaptureTaps of them), so several tracks
    // record simultaneously.  Punch in/out points are stamped in each lane's
    // own capture frames so marks and buffer share a clock.
    struct PunchLane {
        int track = -1, seq = -1;      // mixer track + its lane sequence
        long long startSample = 0;     // timeline sample of ITS capture frame 0
        long long inFrame = -1;        // open punch (capture frames); -1 = out
        std::vector<std::pair<long long,long long>> ranges;   // completed punches
        bool silent = false;           // inlet unpatched: records silence
    };
    static bool g_punchPassActive = false;
    static std::vector<PunchLane> g_punchLanes;        // captured this pass
    static std::set<int> g_punchRecordingTracks;       // punched-in RIGHT NOW
    static unsigned long long g_punchPassLocGen = 0;   // locate gen at pass open
    static std::map<std::string,int> g_punchPassNumber;   // lane name -> NN
    // Preferences (PT p636-637, p640).  Persisted in punch_prefs.txt next to
    // the audio prefs; defaults follow the manual (10 ms punch crossfade,
    // Audio Track RecordLock on = legacy arm-stays behaviour) EXCEPT the DP
    // file length: PT's 25-minute default is a disk file, but PatchKnob audio
    // clips live in RAM (~23 MB per stereo minute at 48 kHz), so the default
    // here is 5 minutes; 25 remains selectable.
    static int  g_prefPunchXfadeMs      = 10;
    static int  g_prefDpFileLenSec      = 5 * 60;
    static bool g_prefTransportRecLock  = false;
    static bool g_prefAudioTrackRecLock = true;
    static bool g_prefMuteArmedStopped  = false;
    auto punch_prefs_path = []() -> std::string {
        char* pref = SDL_GetPrefPath("PatchKnob", "PatchKnob");
        std::string p = pref ? std::string(pref) + "punch_prefs.txt"
                             : std::string("punch_prefs.txt");
        if (pref) SDL_free(pref);
        return p;
    };
    auto save_punch_prefs = [punch_prefs_path]{
        std::ofstream f(punch_prefs_path(), std::ios::trunc);
        f << "qp_xfade_ms=" << g_prefPunchXfadeMs << "\n"
          << "dp_file_len_sec=" << g_prefDpFileLenSec << "\n"
          << "transport_record_lock=" << (g_prefTransportRecLock ? 1 : 0) << "\n"
          << "audio_track_record_lock=" << (g_prefAudioTrackRecLock ? 1 : 0) << "\n"
          << "mute_armed_while_stopped=" << (g_prefMuteArmedStopped ? 1 : 0) << "\n";
    };
    {   // load at startup (same key=value shape audio_preferences.txt uses)
        std::ifstream f(punch_prefs_path());
        std::string line;
        while (std::getline(f, line)) {
            const size_t eq = line.find('='); if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            try {
                if      (k == "qp_xfade_ms")             g_prefPunchXfadeMs = std::stoi(v);
                else if (k == "dp_file_len_sec")         g_prefDpFileLenSec = std::stoi(v);
                else if (k == "transport_record_lock")   g_prefTransportRecLock = std::stoi(v) != 0;
                else if (k == "audio_track_record_lock") g_prefAudioTrackRecLock = std::stoi(v) != 0;
                else if (k == "mute_armed_while_stopped")g_prefMuteArmedStopped = std::stoi(v) != 0;
            } catch (...) {}
        }
        if (g_prefPunchXfadeMs < 0)  g_prefPunchXfadeMs = 0;
        if (g_prefDpFileLenSec < 10) g_prefDpFileLenSec = 10;
    }
    // Assigned after toggle_record exists (the punch machinery needs the track
    // registry + capture path defined below); every caller guards for null.
    std::function<void(int)>  set_rec_mode;         // absolute mode select
    std::function<void()>     prepare_dpe_tracks;   // Options > Prepare DPE Tracks
    std::function<bool()>     punch_record_pressed; // transport REC; true = handled
    std::function<void(int)>  punch_track_button;   // track rec-button click (all gestures)
    std::function<void()>     punch_transport_stop; // pass commit + RecordLock bookkeeping
    std::function<void()>     punch_poll;           // per-frame punch housekeeping
    std::function<int()>      punch_track_count_fn; // mode-aware punch-enabled count

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
        // Only offered when PATCHKNOBSDL_VST names a plug-in: there is no
        // default instrument to fall back on any more.
        { "Load Synth on Track 0", [&]{ if(audio_ok && !synthPath.empty())
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
    Menu mTrack; mTrack.title = "Track";
    mTrack.items = {
        { "Add Instrument Track", [&]{
              if (vArrange.on_add_track) vArrange.on_add_track(0); } },
        { "Add Audio Track", [&]{
              if (vArrange.on_add_track) vArrange.on_add_track(1); } },
        { "Add Automation Track", [&]{
              if (vArrange.on_add_track) vArrange.on_add_track(2); } },
    };
    Menu mHelp; mHelp.title = "Help";
    { MenuItem mi; mi.label="PatchKnob DAW"; mi.enabled=false; mHelp.items.push_back(mi); }

    // --- floating window system + the Audio Settings window ----------------
    WindowManager wm;
    // Arrange is a managed workspace window, not a root canvas. This gives it a
    // real client boundary above the bottom dock, so its own scrollbars can
    // never be covered by Patchbay/Sample Editor. Menu + transport stay global.
    Window arrangeWin; arrangeWin.title="Arrange"; arrangeWin.content=&vArrange;
    //  No title bar: Arrange is the full-workspace background pane, so the
    //  strip only cost vertical space above the edit-mode buttons.
    arrangeWin.titlebar = false; arrangeWin.title_h = 0;
    arrangeWin.visible=true; arrangeWin.closable=false;
    arrangeWin.resizable=false; arrangeWin.minimizable=false;
    arrangeWin.alwaysBack=true;
    wm.add(&arrangeWin);
    set_arrange_visible=[&](bool on){arrangeWin.visible=on;if(on)wm.raise(&arrangeWin);};
    audioui::AudioSettingsView audioSettings; audioSettings.audio_ok = audio_ok;
    Window audioWin; audioWin.title = "Audio Settings"; audioWin.content = &audioSettings;
    // ---- screen recorder -------------------------------------------------
    // Video is piped to ffmpeg; audio comes off the engine's master tap, so the
    // two cannot drift the way a re-recorded sound-card capture would.
    pkrec::ScreenRecorder screenRec;
    pkrec::Settings       screenRecCfg;
    ui::VideoSettingsView videoSettings;
    videoSettings.cfg = &screenRecCfg;
    videoSettings.recording   = [&]{ return screenRec.recording(); };
    videoSettings.ffmpeg_path = [&]{ return pkrec::ScreenRecorder::ffmpeg_path(); };
    videoSettings.on_pick_folder = [&](std::string& path)->bool {
        std::string p;
        if (!ui::choose_folder(app, path, p)) return false;
        path = p;
        return true;
    };
    Window videoWin; videoWin.title = "Video Settings"; videoWin.content = &videoSettings;
    videoWin.visible = false;
#ifdef PATCHKNOB_HAS_AI
    // Claude chat settings: API key (masked; machine-bound storage), model
    // and prefs.  Opened from the chat panel's SET.. button and from the A/V
    // menu; the chat wiring itself is with the Csound editor further down.
    ui::AiSettingsView aiSettings;
    Window aiSettingsWin; aiSettingsWin.title = "Claude Settings";
    aiSettingsWin.content = &aiSettings; aiSettingsWin.visible = false;
#endif
    audioWin.rect = { 120, 90, 360, 380 }; audioWin.visible = false;
    auto open_window = [&](Window& w, int dw, int dh){
        w.rect.w = dw; w.rect.h = dh;
        w.rect.x = (app.w - dw)/2; w.rect.y = (app.h - dh)/2;
        w.visible = true; wm.raise(&w); app.request_redraw();
    };

    Menu mAudio; mAudio.title = "A/V";
    mAudio.items = {
        { "Audio Settings...", [&]{ open_window(audioWin, 380, 400); } },
        { "Video Settings...", [&]{ open_window(videoWin, 520, 260); } },
    };
#ifdef PATCHKNOB_HAS_AI
    mAudio.items.push_back({ "Claude Settings...", [&]{ open_window(aiSettingsWin, 520, 560); } });
#endif
    // --- display / renderer -------------------------------------------------
    // Exclusive fullscreen and the playback redraw rate are the two settings
    // that actually move GPU load on a weak card, so they belong next to the
    // audio device choice rather than buried.  The SDL build and the backend it
    // selected are shown read-only -- see the note on the relaunch item below.
    {
        MenuItem sep; sep.separator = true; sep.enabled = false;
        mAudio.items.push_back(sep);

        MenuItem fs;
        fs.label   = "Fullscreen (exclusive)      F11";
        fs.check   = true;
        fs.checked = [&]{ return app.fullscreen; };
        fs.action  = [&]{ app.toggle_fullscreen(); };
        mAudio.items.push_back(fs);

        MenuItem vs;
        vs.label   = "VSync";
        vs.check   = true;
        vs.checked = [&]{ return app.vsync_on; };
        vs.action  = [&]{ app.set_vsync(!app.vsync_on); };
        mAudio.items.push_back(vs);

        mAudio.items.push_back(sep);

        // Playback-animation ceiling.  This only limits frames the user did not
        // ask for; clicks and drags always repaint at the full rate.
        for (int hz : {60, 30, 15}) {
            MenuItem mi;
            mi.label   = "Playback redraw " + std::to_string(hz) + " Hz";
            mi.check   = true;
            mi.checked = [&, hz]{ return app.anim_hz == hz; };
            mi.action  = [&, hz]{ app.anim_hz = hz; app.request_redraw(); };
            mAudio.items.push_back(mi);
        }

        mAudio.items.push_back(sep);

        // Read-only status.  Which SDL a binary uses is fixed at LINK time --
        // the two builds are separate executables against different libraries,
        // so there is no runtime switch to offer.  The nearest honest thing is
        // to relaunch the other build, which is what the item below does.
        MenuItem info;
        info.label   = app.sdl_version() + "  /  renderer: " + app.video_backend();
        info.enabled = false;
        mAudio.items.push_back(info);

        // Offered only when the sibling build is actually present next to this
        // one, so a shipped single-backend install simply does not show it.
        // Named by BACKEND, not by "the default build dir" -- SDL3 is the
        // default now, so keying off `build` would have pointed a SDL3 binary
        // at itself.
#ifdef PATCHKNOB_SDL3
        const char* otherName = "SDL2";
        const char* otherDir  = "build-sdl2";
#else
        const char* otherName = "SDL3";
        const char* otherDir  = "build-sdl3";
#endif
        std::error_code ec;
        std::filesystem::path here = std::filesystem::current_path(ec);
        std::filesystem::path other =
            here.parent_path() / otherDir / "PatchKnob.exe";
        if (!ec && std::filesystem::exists(other, ec)) {
            MenuItem sw;
            sw.label  = std::string("Restart with ") + otherName + "...";
            sw.action = [&app, other]{
                // Start the sibling detached, then close this one.  `start ""`
                // gives cmd an empty window title so a quoted path is not
                // mistaken for one.
                const std::string cmd =
                    "start \"\" \"" + other.string() + "\"";
                std::system(cmd.c_str());
                app.running = false;
            };
            mAudio.items.push_back(sw);
        }
    }
    if(ui::platform::mobile()) {
    std::vector<MenuItem> mobileScaleItems;
    for (float s : {1.0f,1.25f,1.5f,1.75f}) {
        MenuItem mi;
        mi.label="Mobile UI "+std::to_string((int)std::lround(s*100.f))+"%";
        mi.check=true;
        mi.checked=[&,s]{ return std::fabs(app.ui_scale-s)<0.01f; };
        mi.action=[&,s]{ app.set_ui_scale(s); };
        mobileScaleItems.push_back(std::move(mi));
    }
    { MenuItem sep; sep.separator=true; sep.enabled=false; mobileScaleItems.push_back(sep); }
    mView.items.insert(mView.items.begin(),mobileScaleItems.begin(),mobileScaleItems.end());
    }
    Menu mEdit; mEdit.title = "Edit";
    mEdit.items = {
        { "Undo        Ctrl+Z",       [&]{ if (global_undo) global_undo(); } },
        { "Redo        Ctrl+Y",       [&]{ if (global_redo) global_redo(); } },
        { "Redo    Ctrl+Shift+Z",     [&]{ if (global_redo) global_redo(); } },
    };
    // --- Options menu: record modes + punch preferences (PT ch.27) ---------
    // PatchKnob has no modal preferences dialog, so the punch preferences live
    // here as checkable items -- the same surface the record modes themselves
    // use (PT's own Options menu carries QuickPunch/TrackPunch/etc).
    Menu mOptions; mOptions.title = "Options";
    {
        auto mode_item = [&](const char* label, int m) {
            MenuItem mi; mi.label = label; mi.check = true;
            mi.checked = [m]{ return (int)g_recMode == m; };
            mi.action  = [&, m]{ if (set_rec_mode) set_rec_mode(m); };
            return mi;
        };
        mOptions.items.push_back(mode_item("Normal Record Mode", 0));
        mOptions.items.push_back(mode_item("QuickPunch          Ctrl+Shift+P", 1));
        mOptions.items.push_back(mode_item("TrackPunch          Ctrl+Shift+T", 2));
        mOptions.items.push_back(mode_item("DestructivePunch    Ctrl+Shift+D", 3));
        MenuItem sep; sep.separator = true; sep.enabled = false;
        mOptions.items.push_back(sep);
        { MenuItem mi; mi.label = "Prepare DPE Tracks";
          mi.action = [&]{ if (prepare_dpe_tracks) prepare_dpe_tracks(); };
          mOptions.items.push_back(mi); }
        mOptions.items.push_back(sep);
        // QuickPunch/TrackPunch Crossfade Length (PT p636): 0 = no written
        // punch crossfades (the 4 ms monitor-only fade always applies).
        for (int ms : { 0, 5, 10, 20, 50 }) {
            MenuItem mi; mi.check = true;
            mi.label = ms == 0 ? "QP/TP Crossfade: none"
                     : "QP/TP Crossfade: " + std::to_string(ms) + " ms"
                       + (ms == 10 ? "  (default)" : "");
            mi.checked = [ms]{ return g_prefPunchXfadeMs == ms; };
            mi.action  = [save_punch_prefs, ms]{ g_prefPunchXfadeMs = ms;
                                                 save_punch_prefs(); };
            mOptions.items.push_back(mi);
        }
        mOptions.items.push_back(sep);
        // DestructivePunch File Length (PT p636/644).
        for (int mins : { 1, 2, 5, 10, 25 }) {
            MenuItem mi; mi.check = true;
            mi.label = "DP File Length: " + std::to_string(mins) + " min"
                     + (mins == 5 ? "  (default)" : mins == 25 ? "  (PT default; ~575 MB RAM)" : "");
            mi.checked = [mins]{ return g_prefDpFileLenSec == mins * 60; };
            mi.action  = [save_punch_prefs, mins]{ g_prefDpFileLenSec = mins * 60;
                                                   save_punch_prefs(); };
            mOptions.items.push_back(mi);
        }
        mOptions.items.push_back(sep);
        { MenuItem mi; mi.label = "Transport RecordLock"; mi.check = true;
          // Auto-disabled while the destructive punch mode is on (PT p637); the
          // stored preference survives, only its effect is suspended.
          mi.checked = []{ return g_prefTransportRecLock &&
                                  g_recMode != RecMode::DestructivePunch; };
          mi.action  = [&]{
              if (g_recMode == RecMode::DestructivePunch) {
                  projectStatus = "Transport RecordLock is disabled while "
                                  "DestructivePunch is on";
                  return;
              }
              g_prefTransportRecLock = !g_prefTransportRecLock;
              save_punch_prefs();
          };
          mOptions.items.push_back(mi); }
        { MenuItem mi; mi.label = "Audio Track RecordLock"; mi.check = true;
          mi.checked = []{ return g_prefAudioTrackRecLock; };
          mi.action  = [save_punch_prefs]{
              g_prefAudioTrackRecLock = !g_prefAudioTrackRecLock;
              save_punch_prefs(); };
          mOptions.items.push_back(mi); }
        { MenuItem mi; mi.label = "Mute Record-Armed Tracks While Stopped";
          mi.check = true;
          mi.checked = []{ return g_prefMuteArmedStopped; };
          mi.action  = [save_punch_prefs]{
              g_prefMuteArmedStopped = !g_prefMuteArmedStopped;
              save_punch_prefs(); };
          mOptions.items.push_back(mi); }
    }
    menubar.menus = { mFile, mEdit, mTrack, mOptions, mView, mAudio, mHelp };

    // --- transport bars (Ardour-style shaped-icon buttons + BBT clock + tempo).
    // A COMPACT strip replaces the old button row; a FULL panel opens in a window
    // from View -> Transport (large).  Both drive the kitchensink musical clock +
    // the PatchKnob sequencer through one set of callbacks.
    static bool g_recArmed = false, g_loopOn = false;
    static bool g_countIn = false, g_countInPendingPlay = false;
    static bool g_countInPendingRecord = false;
    static bool g_countInRecordAwaitingLocate = false;
    static long long g_countInRecordOriginTick = -1;
    // Tick observed at the count-in handoff (still the count-in END, because
    // Transport::locate is applied by the audio thread) and the wall clock at
    // that moment.  Arming waits for the locate to LAND, but must not wait on an
    // exact tick match -- see the comment at the poll site.
    static long long g_countInPreLocateTick = -1;
    static unsigned  g_countInAwaitMs = 0;
    // Live-record: toggle_record's body is assigned later (it needs the track
    // registry + master-mixer sync defined below); the REC button calls it via
    // this handle.  drain_record accumulates captured events every frame.
    std::function<void()> toggle_record;
    std::function<void()> drain_record;
    int recordQuantizeTicks = 0;                    // 0=off, otherwise grid ticks
    // How far a recorded note is pulled toward its grid line: 1 = hard snap,
    // 0 = leave the performance alone.  Kept high enough to tighten a take
    // without flattening its groove.
    float recordQuantizeStrength = 1.0f;
    // Q-Range (Ardour's Quantize::_threshold, Logic's Q-Range): a note already
    // this close to its line is left EXACTLY as played.  Snapping every note is
    // what makes a take sound mechanical; correcting only what is audibly off
    // keeps the micro-timing that carries the feel.
    //
    // Held as a PERCENT of the largest deviation possible -- half a grid step --
    // NOT as absolute ticks.  An absolute Q-Range only means something relative
    // to the grid it is measured against: on a 16th grid (48 ticks) no note can
    // ever be more than 24 ticks from a line, so a fixed 24-tick Q-Range would
    // silently switch quantise off entirely.  As a percentage it keeps its
    // meaning at every grid setting.  50% on a 16th grid == a 64th (12 ticks),
    // comfortably above USB-MIDI jitter (~2 ms) so real playing is preserved
    // rather than chased.
    int recordQuantizeRangePct = 0;         // selected RQ means hard snap by default
    // 50 == straight.  Above 50 delays every other line, to 2/3 of a step at
    // 100 (the swing ceiling Ardour uses).
    int recordQuantizeSwingPct = 50;
    ui::TransportBar transportSmall; transportSmall.compact = true;
    ui::TransportBar transportFull;  transportFull.compact  = false;
    // SONG mode (default): playback is gated by the timeline triggers -- a clip
    // sounds ONLY where it is placed in the arrange.  LIVE mode: armed patterns
    // loop freely (classic PatchKnob jamming).  Toggled on the transport bar.
    static bool g_songMode = true;

    // ---- automation re-priming across transport discontinuities ---------------
    // Automation playback is a WINDOW WALK: on_frame emits everything due in
    // (prevAutomationTick, cur].  That contract only holds while the transport
    // moves FORWARD CONTINUOUSLY.  A locate breaks it in both directions, and
    // nothing used to re-prime afterwards -- prevAutomationTick was a function
    // static that the seek path never touched:
    //   * a BACKWARD locate leaves toTick <= fromTick, and both
    //     AutomationPlayer::advanceScheduled() and advanceRegionsScheduled() are
    //     documented no-ops for an empty window.  So NOTHING was emitted and every
    //     lane kept the last value it had sent.  Land on the far side of a
    //     fade-out (or a muted section) and the track stayed at gain 0 with the
    //     playhead visibly rolling, until some later breakpoint happened to be
    //     crossed -- one of the "it's silent after I move the playhead" reports.
    //   * a FORWARD locate is worse than useless: the next walk spans the WHOLE
    //     jump and enqueues every breakpoint in between STAMPED WITH ITS ORIGINAL
    //     PROJECT TICK, i.e. a flood of parameter messages due far behind the
    //     playhead -- exactly what the audio thread's unreachable-due valve is
    //     there to throw away.
    // The fix is to never walk a discontinuous window: re-prime instead.
    // emitAt() forces every lane to send its value AT the new position, which is
    // precisely the "use on locate / transport start" contract documented in
    // automation_player.h and which had ZERO call sites; advanceRegions() over a
    // one-tick window is the region-lane equivalent (regions carry no coalesce
    // memo, so there is nothing to force past).
    //
    // THREADING: emitAt()/advanceRegions() touch the coalesce memo and may
    // allocate in syncMemo(), so they are message-thread only -- the same thread
    // that already owns autoPlayer and runs the on_frame pump.  Every caller
    // below is a UI callback, so no deferral is needed.  They deliberately route
    // through the UNSCHEDULED bridge entry points (tick == -1): those land at the
    // start of the next audio block and are immune both to the schedule-epoch
    // invalidation that a locate performs and to the unreachable-due valve.
    long prevAutomationTick = 0;
    auto automation_sync_region_spans = [&]{
        // Arrange regions are authoritative for automation ownership; sequence
        // triggers remain the arranger's edit surface.  Sync their half-open
        // span/trim before evaluating anything.
        for (auto& rg : autoPlayer.regions()) {
            if (!perf.is_active(rg.id)) continue;
            sequence* s = perf.get_sequence(rg.id);
            if (!s) continue;
            long on = 0, off = 0, ofs = 0; bool sel = false;
            s->reset_draw_trigger_marker();
            if (s->get_next_trigger(&on, &off, &sel, &ofs)) {
                rg.position = on;
                rg.length   = std::max<long>(1, off - on + 1);
                rg.source   = ofs;
            }
            rg.trackerFx = s->get_fx_blob();
        }
    };
    auto automation_reprime = [&](long tick){
        if (tick < 0) tick = 0;
        automation_sync_region_spans();
        autoPlayer.emitAt(tick,
            [](int trk, unsigned id, float v){
                PatchKnob::app::audio_app_route_param(trk, id, v); },
            [](int trk, int cc, int val){
                PatchKnob::app::audio_app_route_midi(
                    trk, 0xB0, (unsigned char)cc, (unsigned char)val); },
            // Track lanes aimed at patch-graph nodes: same routing as regions.
            [](int trk, const PatchKnob::engine::LaneTarget& target, float v){
                using PatchKnob::engine::LaneTargetKind;
                if (target.kind == LaneTargetKind::PatchParam)
                    PatchKnob::app::audio_app_patch_route_param(target.node, target.id, v);
                else if (target.kind == LaneTargetKind::RackParam) {
                    rackx::RackEngine* eng =
                        PatchKnob::app::audio_app_rack_engine(target.node);
                    if (eng) eng->setParam(target.module, (int)target.id,
                        target.minValue + (target.maxValue - target.minValue) * v);
                } else if (trk >= 0)
                    PatchKnob::app::audio_app_route_param(trk, target.id, v);
            });
        autoPlayer.advanceRegions(tick, tick + 1,
            [](int trk, const PatchKnob::engine::LaneTarget& target, float v){
                using PatchKnob::engine::LaneTargetKind;
                if (target.kind == LaneTargetKind::PatchParam)
                    PatchKnob::app::audio_app_patch_route_param(target.node, target.id, v);
                else if (target.kind == LaneTargetKind::RackParam) {
                    rackx::RackEngine* eng =
                        PatchKnob::app::audio_app_rack_engine(target.node);
                    if (eng) eng->setParam(target.module, (int)target.id,
                        target.minValue + (target.maxValue - target.minValue) * v);
                } else if (target.kind == LaneTargetKind::VstParam && trk >= 0)
                    PatchKnob::app::audio_app_route_param(trk, target.id, v);
            },
            [](int trk, int cc, int val){
                if (trk >= 0) PatchKnob::app::audio_app_route_midi(
                    trk, 0xB0, (unsigned char)cc, (unsigned char)val); });
        prevAutomationTick = tick;
    };
    // Any tick delta wider than this is a locate, not transport motion: 4 beats
    // is 2 s at 120 BPM, far beyond the worst UI frame, and comfortably inside
    // the audio thread's unreachable-due margin.
    const long c_automationWalkLimit = (long)c_ppqn * 4;

    // ---- ONE seek path for every transport move -------------------------------
    // The bar's |< << >> >| buttons, the arrange ruler scrub and the keyboard all
    // come through here.  There used to be TWO paths: the ruler moved perform's
    // clock AND located the engine, while the transport buttons located ONLY the
    // engine.  perform is what the arrange view draws its playhead from, and it
    // is also what inner_start() re-locates the engine to when you press PLAY --
    // so a button seek was broken twice over: while stopped the playhead did not
    // visibly move at all, and then PLAY yanked the transport straight back to
    // the stale m_starting_tick.  That is the "rewind does nothing" report.
    auto transport_seek = [&](long tick){
        if (tick < 0) tick = 0;
        perf.set_tick(tick);            // what the arrange view DRAWS
        perf.set_starting_tick(tick);   // where PLAY will resume from
        // Sequence cursors are owned by the SCHEDULER thread while rolling.
        // Moving them from here raced it: audio_app_transport_locate() only
        // QUEUES the seek, and the scheduler polls several times per block, so
        // for the block or two before the seek landed it was still reading the
        // OLD transport sample while these cursors already pointed at the
        // target.  sequence::play() then opened a window spanning the entire
        // jump and dumped every event between the two positions into the ring
        // in one call -- the "catch-up burst" its own diagnostic warns about,
        // which floods the ring until route_midi starts admitting note-offs
        // only and drops every note-on.  Rolling, the scheduler now rewinds
        // them itself from the locate handshake, once the seek has actually
        // been applied.  Stopped, there is no scheduler thread to do it.
        if (!perf.running()) perf.set_orig_ticks(tick);
        PatchKnob::app::audio_app_transport_locate(
            PatchKnob::app::audio_app_tick_to_sample(tick));
        // Re-prime every automation lane AT the new position, AFTER the locate has
        // bumped the schedule epoch (these emits are unscheduled, so the bump
        // cannot discard them).  Without this the walk in on_frame either emitted
        // nothing at all (backward locate: empty window) or dumped the whole
        // skipped span at stale ticks (forward locate), and lanes kept whatever
        // value they last sent -- a gain lane parked at 0 stayed silent.  This is
        // also the ONLY re-prime while the transport is stopped: on_frame's
        // automation pump does not run when !animating.
        automation_reprime(tick);
        app.request_redraw();
    };
    // Where the playhead IS, in ticks, from the clock that actually owns it.
    // audio_app_transport_sample() still reports the PRE-seek position until the
    // audio thread runs a block, so two REWIND presses inside one audio block
    // both read the same spot and computed the same target -- the second press
    // did nothing.  effective_sample() answers with the queued seek if one is in
    // flight; the freeze path needed exactly this fix.
    auto transport_tick_now = [&]() -> long {
        if (!PatchKnob::app::audio_app_running())
            return std::max<long>(0, perf.get_tick());
        return (long)std::max<long long>(0,
            PatchKnob::app::audio_app_sample_to_tick(
                PatchKnob::app::audio_app_transport_effective_sample()));
    };
    // Bar-quantised transport step, in ticks.  dir<0 = previous bar line, dir>0 =
    // next.  One bar == 4 beats, the same 4/4 the transport bar's meter label and
    // its BBT clock assume.
    auto transport_step_bars = [&](int dir) -> long {
        const long bar = (long)c_ppqn * 4;
        const long cur = transport_tick_now();
        long tgt;
        if (dir < 0) {
            // Sitting exactly ON a bar line, REWIND must go to the PREVIOUS one
            // or the button visibly does nothing; anywhere else it goes to the
            // top of the bar we are inside.  The old test used a 0.01-BEAT
            // epsilon in floating point -- 5 ms at 120 BPM -- so a playhead a
            // hair past a bar line counted as "on" it and rewind skipped an
            // extra bar.  Integer ticks make the test exact.
            tgt = (cur % bar) ? (cur / bar) * bar : cur - bar;
        } else {
            tgt = (cur / bar) * bar + bar;      // always forward, never a no-op
        }
        if (tgt < 0) tgt = 0;                   // before bar 1 there is nothing
        // Respect the LOOP range while looping.  Stepping out of the loop puts
        // the playhead somewhere playback can never reach -- the engine wraps you
        // back the instant you press play, so the button looked broken.  Rewind
        // clamps to the loop start; fast-forward past the end wraps to the loop
        // start, which is where playback was going to take you anyway.
        if (g_loopOn) {
            const long l = std::max<long>(0, perf.get_left_tick());
            const long r = perf.get_right_tick();
            if (r > l && cur >= l && cur < r) {
                if (tgt <  l) tgt = l;
                if (tgt >= r) tgt = l;
            }
        }
        return tgt;
    };

    auto wire_transport = [&](ui::TransportBar& tb){
        tb.on_play  = [&]{
            // PLAY while a count-in is already counting: ignore.  It used to
            // fall straight into the branch below, which STOPS the transport and
            // starts a FRESH pre-roll -- so an impatient second press restarted
            // the countdown forever and never reached playback.
            if(g_countInPendingPlay || g_countInPendingRecord ||
               g_countInRecordAwaitingLocate) { app.request_redraw(); return; }
            // PLAY while already rolling: also ignore.  With count-in enabled the
            // old code called perf.stop() first, so pressing play DURING playback
            // killed playback and dropped you into a pre-roll bar.
            if(perf.running()) { app.animating=true; app.request_redraw(); return; }
            if(g_countIn) {
                perf.stop();
                PatchKnob::app::audio_app_metronome_start_countin();
                g_countInPendingPlay=true;
            } else {
                perf.start(g_songMode);
                PatchKnob::app::audio_app_patch_set_playing(true);
            }
            app.animating=true; app.request_redraw();
        };
        tb.on_stop  = [&]{
            // STOP is also punch-out.  Commit while the transport tick still
            // identifies the end of the capture.  The punch PASS commits first
            // (it reads the capture that is still filling), then the normal
            // MIDI/audio take; punch_transport_stop also applies the
            // Transport/Audio Track RecordLock preferences.
            if (punch_transport_stop) punch_transport_stop();
            if (g_recArmed && toggle_record) toggle_record();
            // Bailing out of a pending count-in has to put the playhead BACK
            // where the pre-roll started.  start_countin() locates the transport
            // to 0 so beat one gets the accented click, so cancelling a count-in
            // used to dump you at the top of the song instead of leaving you at
            // the punch point you had chosen.
            const bool cancelling = g_countInPendingPlay || g_countInPendingRecord ||
                                    g_countInRecordAwaitingLocate;
            const long countinOrigin = cancelling
                ? (long)std::max<long long>(0,
                      PatchKnob::app::audio_app_metronome_countin_origin_tick())
                : 0;
            g_countInPendingPlay=false;
            g_countInPendingRecord=false;
            g_countInRecordAwaitingLocate=false;
            g_countInRecordOriginTick=-1;
            g_countInPreLocateTick=-1; g_countInAwaitMs=0;
            const long stopped_at = transport_tick_now();
            perf.stop();       PatchKnob::app::audio_app_patch_set_playing(false);
            // Leave the playhead WHERE IT STOPPED and resume from there.  Two
            // separate things made stop lose your place: perform's scheduler
            // thread zeroes its own m_tick as it tears down (so the arrange
            // playhead teleported to bar 1 on every stop), and PLAY re-located
            // the engine to a stale m_starting_tick (so it then resumed at some
            // third position).  One seek at stop time pins all of it together.
            transport_seek(cancelling ? countinOrigin : stopped_at);
            app.animating=false; app.request_redraw(); };
        // Every one of these moves BOTH clocks now (see transport_seek), so the
        // playhead visibly moves while stopped and PLAY resumes from where you
        // put it.  They used to locate the engine only, which is why they looked
        // completely dead unless the transport happened to be rolling.
        tb.on_to_start = [&]{ transport_seek(0); };
        tb.on_rewind   = [&]{ transport_seek(transport_step_bars(-1)); };
        tb.on_ffwd     = [&]{ transport_seek(transport_step_bars(+1)); };
        // The real end of the ARRANGEMENT, over both models.  get_max_trigger()
        // only knows seq24 triggers and answers 0 for an audio-only project, so
        // ">|" used to jump to bar 1 on exactly the projects you'd want it for.
        tb.on_to_end   = [&]{ transport_seek(std::max<long>(0, vArrange.song_end())); };
        tb.on_rec   = [&]{
            // Punch record modes claim the button first (PT ch.27): stopped it
            // toggles Record Ready, rolling it punches in/out on the pass.  A
            // false return (Normal mode, or no audio punch target) falls
            // through to the legacy record path below.
            if (punch_record_pressed && punch_record_pressed()) {
                app.request_redraw();
                return;
            }
            // A count-in is already running for this take: ignore the press.
            // It used to restart the count-in, and start_countin() latches the
            // record origin from where the playhead reads RIGHT THEN -- which by
            // the second press is somewhere inside the pre-roll bar.  The take
            // then began a beat or two into the count-in instead of at the punch
            // point, and the clip landed there too.  STOP is the way out of a
            // pending count-in.
            if(g_countInPendingRecord || g_countInRecordAwaitingLocate) {
                app.request_redraw(); return;
            }
            // A count-in is a PRE-ROLL: it only makes sense from a standstill.
            // Punching in on the fly used to run this branch too, and its first
            // act is perf.stop() -- so hitting REC while the song played STOPPED
            // the song and started counting it back in.  Rolling means punch in
            // right here, right now.
            if(g_countIn && !g_recArmed && !perf.running()) {
                perf.stop();
                PatchKnob::app::audio_app_metronome_start_countin();
                g_countInPendingRecord=true;
                app.animating=true;
            } else if(toggle_record) toggle_record();
            app.request_redraw();
        };  // live MIDI record -> timeline clip
        tb.screen_record_available = []{ return pkrec::ScreenRecorder::available(); };
        tb.is_screen_recording     = [&]{ return screenRec.recording(); };
        tb.screen_record_elapsed   = [&]{ return screenRec.elapsed(); };
        tb.on_screen_record = [&]{
            if (screenRec.recording()) {
                screenRec.stop();
                projectStatus = screenRec.error().empty()
                              ? ("Saved " + screenRec.output_path())
                              : screenRec.error();
            } else {
                int ww = 0, wh = 0;
                SDL_GetRendererOutputSize(app.ren, &ww, &wh);
                if (!screenRec.start(app.ren, ww, wh, screenRecCfg))
                    projectStatus = "Screen record: " + screenRec.error();
                else
                    projectStatus = "Recording screen + mixer output";
            }
            app.request_redraw();
        };
        tb.get_record_quantize = [&]{ return recordQuantizeTicks; };
        tb.on_record_quantize = [&](int ticks){ recordQuantizeTicks = ticks; };
        tb.get_record_qrange = [&]{ return recordQuantizeRangePct; };
        tb.on_record_qrange  = [&](int pct){ recordQuantizeRangePct = pct; };
        tb.get_record_swing  = [&]{ return recordQuantizeSwingPct; };
        tb.on_record_swing   = [&](int pct){ recordQuantizeSwingPct = pct; };
        tb.on_loop  = [&]{ g_loopOn = !g_loopOn; perf.set_looping(g_loopOn); app.request_redraw(); };
        tb.on_metronome = [&]{
            PatchKnob::app::audio_app_metronome_enable(
                !PatchKnob::app::audio_app_metronome_enabled());
            app.request_redraw();
        };
        tb.on_count_in = [&](bool on){ g_countIn=on; app.request_redraw(); };
        // ONE tempo write path: perform::set_bpm funnels every bpm write (UI,
        // file load, hotkeys) into the engine's kitchensink tempo map.
        // Fractional BPM survives end-to-end now.
        tb.on_tempo = [&](double bpm){ perf.set_bpm(bpm); app.request_redraw(); };
        tb.on_mode  = [&]{ g_songMode = !g_songMode; app.request_redraw(); };
        tb.is_song  = [&]{ return g_songMode; };
        // Each button lights from the SAME state its press acts on.
        //
        // is_rolling used to read ONLY the engine transport while PLAY drives
        // perform.  Those diverge in both directions: with no audio device the
        // button never lit even though the sequencer was running, and during a
        // count-in the engine rolls (that is how the click is clocked) while
        // perform is stopped, so PLAY read "playing" a whole bar before a single
        // note sounded.  perform is the authority; the engine only corroborates.
        tb.is_rolling = [&]{
            return perf.running() ||
                   (PatchKnob::app::audio_app_transport_rolling() &&
                    !g_countInPendingPlay && !g_countInPendingRecord &&
                    !g_countInRecordAwaitingLocate);
        };
        // PRESSED-but-not-yet-engaged.  The count-in is transport time the user
        // asked for, so the buttons that asked for it must look engaged for all
        // of it -- otherwise REC is a switch that visibly does nothing for a
        // whole bar, which is what made it feel like a broken momentary key.
        tb.is_play_pending = [&]{ return g_countInPendingPlay || g_countInPendingRecord ||
                                         g_countInRecordAwaitingLocate; };
        tb.is_rec_pending  = [&]{ return g_countInPendingRecord ||
                                         g_countInRecordAwaitingLocate; };
        // Solid record = actually capturing: a normal take OR at least one
        // punched-in track.  Record Ready (flashing) = armed transport or an
        // open pass waiting for its first punch.
        tb.is_rec     = [&]{ return g_recArmed ||
                                    !g_punchRecordingTracks.empty(); };
        tb.is_rec_ready = [&]{ return g_recReady ||
                                      (g_punchPassActive &&
                                       g_punchRecordingTracks.empty()); };
        tb.get_record_mode = [&]{ return (int)g_recMode; };
        tb.on_record_mode  = [&](int m){ if (set_rec_mode) set_rec_mode(m); };
        // Mode-aware punch-enabled count (QuickPunch counts the record-enabled
        // audio lane; TP/DP count the punch set).  Deferred through a
        // std::function because the track registry is defined further down.
        tb.punch_track_count = [&]{
            return punch_track_count_fn ? punch_track_count_fn() : 0;
        };
        tb.is_loop    = [&]{ return g_loopOn; };
        tb.is_metronome = [&]{ return PatchKnob::app::audio_app_metronome_enabled(); };
        tb.is_count_in  = [&]{ return g_countIn; };
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
    static constexpr int kMixerUiMaxChannels=256;
    static bool g_busSolo[kMixerUiMaxChannels] = {};
    static bool g_busMute[kMixerUiMaxChannels] = {};
    auto recompute_mutes = [&]{
        int n = PatchKnob::app::audio_app_mixer_channels(g_curMixerNode);
        bool anySolo = false; for (int i = 0; i < n; ++i) if (g_busSolo[i]) anySolo = true;
        for (int i = 0; i < n; ++i)
            PatchKnob::app::audio_app_mixer_set_mute(g_curMixerNode, i, anySolo ? !g_busSolo[i] : g_busMute[i]);
    };
    // Mixer starts EMPTY (no pre-loaded channels): the master mixer node has 0
    // tracks until an instrument/audio track adds one.  bus_count grows as tracks
    // are created (refresh_master_ui / on_open_mixer sync it to the node).
    // Aux sends are REAL now: the engine grew send buses (audio_app.h "AUX
    // SENDS / AUX BUSES"), so the send hooks that were deliberately left
    // unbound while they could only be no-op stubs are bound below, and
    // aux_count is no longer a hand-set constant -- the view syncs it from
    // get_aux_count every frame, so the send knobs, the aux-return strips and
    // the "+ AUX" column appear exactly when buses exist and vanish with the
    // last one.  The old rule still holds inside the view (live_aux_count):
    // nothing send-shaped is ever drawn unless the full wiring is present.
    mixerui::MasterMixerView mixerStrip; mixerStrip.bus_count = 0; mixerStrip.aux_count = 0;
    {
        mixerStrip.get_bus_count = [&]{ return PatchKnob::app::audio_app_mixer_channels(g_curMixerNode); };
        // Without this the view's own engine calls (mute/solo resolution, pan and
        // gain defaults) fall back to audio_app_master_mixer_node() while every
        // hook above addresses g_curMixerNode -- so a NON-master Mixer window
        // silently drove the master node's mutes.
        mixerStrip.get_node  = [&]{ return g_curMixerNode; };
        mixerStrip.get_label = [&](int i){ return i < mixerStrip.bus_count ? std::to_string(i+1) : std::string("MASTER"); };
        mixerStrip.get_gain  = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_gain(g_curMixerNode,i)
                                                                           : PatchKnob::app::audio_app_mixer_master_gain(g_curMixerNode); };
        mixerStrip.set_gain  = [&](int i,float v){ if (i < mixerStrip.bus_count) PatchKnob::app::audio_app_mixer_set_gain(g_curMixerNode,i,v);
                                                   else PatchKnob::app::audio_app_mixer_set_master_gain(g_curMixerNode,v); };
        mixerStrip.get_level = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_vu(g_curMixerNode,i)
                                           : PatchKnob::app::audio_app_mixer_master_vu(g_curMixerNode); };
        mixerStrip.get_level_stereo = [&](int i,int c){
            if(i < mixerStrip.bus_count) return c==0 ? PatchKnob::app::audio_app_mixer_vu_left(g_curMixerNode,i)
                                                     : PatchKnob::app::audio_app_mixer_vu_right(g_curMixerNode,i);
            return c==0 ? PatchKnob::app::audio_app_mixer_master_vu_left(g_curMixerNode)
                        : PatchKnob::app::audio_app_mixer_master_vu_right(g_curMixerNode);
        };
        mixerStrip.get_mute  = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_mute(g_curMixerNode,i) : false; };
        mixerStrip.get_solo  = [&](int i){ return i < mixerStrip.bus_count ? g_busSolo[i] : false; };
        mixerStrip.toggle_mute = [&](int i){ if (i < mixerStrip.bus_count){
                                             g_busMute[i]=!PatchKnob::app::audio_app_mixer_mute(g_curMixerNode,i); recompute_mutes(); }
                                             app.request_redraw(); };
        mixerStrip.toggle_solo = [&](int i){ if (i < mixerStrip.bus_count){ g_busSolo[i] = !g_busSolo[i]; recompute_mutes(); }
                                             app.request_redraw(); };
        mixerStrip.get_pan   = [&](int i){ return i < mixerStrip.bus_count ? PatchKnob::app::audio_app_mixer_pan(g_curMixerNode,i) : 0.f; };
        mixerStrip.set_pan   = [&](int i,float p){ if (i < mixerStrip.bus_count) PatchKnob::app::audio_app_mixer_set_pan(g_curMixerNode,i,p); };
        // Aux sends / aux buses.  The aux API is MASTER-mixer-only while this
        // one view object also serves plain mixer-node windows (g_curMixerNode
        // is whatever "Open Mixer" targeted), so get_aux_count answers -1 for
        // any other node: the view then shows no send knobs, no aux strips and
        // no add-bus column, instead of pointing that node's strips at the
        // master's buses.  The hooks only carry the LEVEL; enable / pre-fader
        // / bus management go through the audio_app aux API inside the view,
        // exactly like the insert chains already do.
        mixerStrip.get_aux_count = [&]{
            if (g_curMixerNode != PatchKnob::app::audio_app_master_mixer_node()) return -1;
            return PatchKnob::app::audio_app_master_aux_count();
        };
        // strip index == track index for i < bus_count; the view never asks
        // for a send on an aux or master strip.
        mixerStrip.get_send = [&](int i, int a){
            return i < mixerStrip.bus_count
                 ? PatchKnob::app::audio_app_master_send_level(i, a) : 0.f;
        };
        mixerStrip.set_send = [&](int i, int a, float v){
            if (i < mixerStrip.bus_count)
                PatchKnob::app::audio_app_master_set_send_level(i, a, v);
        };
    }
    Window mixerStripWin; mixerStripWin.title = "Mixer"; mixerStripWin.content = &mixerStrip;
    mixerStripWin.visible = false; mixerStripWin.rect = { 60, 90, 700, 440 };
    auto open_mixer_for = [&](int node){
        g_curMixerNode = node;
        // Show exactly as many channels as the node has (0 = empty, only MASTER).
        int n = PatchKnob::app::audio_app_mixer_channels(node); if (n < 0) n = 0;
        mixerStrip.bus_count = n;
        for (int i = 0; i < kMixerUiMaxChannels; ++i) {
            g_busSolo[i]=false; g_busMute[i]=(i<n)&&PatchKnob::app::audio_app_mixer_mute(node,i);
        }
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
    // The panel drops its instance pointer when the plugin behind it dies; hide
    // the window rather than leave an empty "(no plugin)" panel up.
    paramView.on_instance_gone = [&]{ paramWin.visible = false; app.request_redraw(); };
    // (the old per-node MixerNodeView is replaced by the unified strip view above)
    // Pure Data node editor: an SDL patch editor over the node's .pd file; saving
    // regenerates the .pd and reloads the live libpd instance (g_pdNode).
    static int g_pdNode = -1, g_csoundNode = -1, g_rackNode = -1, g_rackIOsig = -1, g_pdIOsig = -1;
    static int g_pdRackMod = -1, g_csoundRackMod = -1;   // >=0 when an editor targets a RACK module
    static std::string g_pdPath, g_csoundPath, g_rackPath;
    pdui::PdEditorView pdEditor;
    // Structural edit: push the editor's in-memory patch text to the node (reloads
    // libpd; adc~/dac~ ports may change).  The patch lives on the node + project now.
    pdEditor.on_changed = [&]{
        if (g_pdRackMod >= 0) {                       // editing a Pd RACK module's patch
            if (auto* eng = PatchKnob::app::audio_app_rack_engine(g_rackNode))
                eng->setModuleScript(g_pdRackMod, pdEditor.patch_text());
            return;
        }
        if (g_pdNode < 0) return;
        PatchKnob::app::audio_app_pd_set_text(g_pdNode, pdEditor.patch_text().c_str());
        // re-bind every GUI atom's send symbol so feedback (a banged bang lighting up)
        // flows back to the editor (set_text dropped the old patch's subscriptions).
        PatchKnob::app::audio_app_pd_clear_gui_binds(g_pdNode);
        for (const std::string& s : pdEditor.gui_send_symbols())
            PatchKnob::app::audio_app_pd_subscribe(g_pdNode, s.c_str()); };
    // GUI value edit in RUN mode: store the text on the node WITHOUT reloading (the
    // live value already went out), so the project keeps the value + the patch runs on.
    pdEditor.on_store_text = [&](const std::string& text){
        if (g_pdRackMod >= 0) return;                 // rack modules persist via on_changed / save
        if (g_pdNode >= 0) PatchKnob::app::audio_app_pd_store_text(g_pdNode, text.c_str()); };
    // RUN-mode GUI atoms drive the running libpd instance LIVE (no reload): the value
    // is sent to the atom's receive symbol, so a toggle starts a connected [metro].
    pdEditor.on_gui_send = [&](const std::string& recv, float v){
        if (g_pdNode >= 0) PatchKnob::app::audio_app_pd_send_float(g_pdNode, recv.c_str(), v); };
    pdEditor.on_gui_bang = [&](const std::string& recv){
        if (g_pdNode >= 0) PatchKnob::app::audio_app_pd_send_bang(g_pdNode, recv.c_str()); };
    Window pdWin; pdWin.title = "Pd"; pdWin.content = &pdEditor; pdWin.visible = false;
    // VCV-Rack-style modular editor: draws + edits the RackNode's live engine.
    rackui::RackEditorView rackEditor;
    Window rackWin; rackWin.title = "Modular"; rackWin.content = &rackEditor; rackWin.visible = false;
    // Offline CDP editor: a patcher of sound transformations, opened from a
    // clip's right-click "CDP..." entry.
    cdpui::CdpEditorView cdpEditor;
    Window cdpWin; cdpWin.title = "CDP"; cdpWin.content = &cdpEditor; cdpWin.visible = false;
    cdpWin.rect = { 120, 90, 860, 560 };
    // Panel-layout editor for scripting (Pd/Csound) rack modules (drag jacks/knobs,
    // resize width); opened from a module's right-click menu.
    paneled::PanelEditorView panelEditor;
    Window panelEditorWin; panelEditorWin.title = "Module Panel";
    panelEditorWin.content = &panelEditor; panelEditorWin.visible = false;
    // (g_rackNode / g_rackIOsig / g_pdIOsig / g_csoundNode declared above with the
    //  other editor-target globals so the editor lambdas can capture them.)
    ui::CsoundEditorView csoundEditor;
    Window csoundWin; csoundWin.title = "Csound"; csoundWin.content = &csoundEditor; csoundWin.visible = false;
    // Editor-local standalone file interchange. Loading replaces the live
    // in-memory node/module source; project saves continue embedding that source.
    pdEditor.on_open_file=[&]{
        std::string p=g_pdPath;
        if(!choose_editor_file(app,false,p,"Open Pure Data Patch","pd",p))return;
        pdEditor.set_patch_path(p); g_pdPath=p; if(pdEditor.on_changed)pdEditor.on_changed();
        pdWin.title="Pd - "+project_display_name(p);
    };
    pdEditor.on_save_file_as=[&]{
        std::string p=g_pdPath;
        if(!choose_editor_file(app,true,p,"Save Pure Data Patch","pd",p))return;
        g_pdPath=p; pdEditor.set_export_path(p); pdEditor.save();
        pdWin.title="Pd - "+project_display_name(p);
    };
    pdEditor.on_save_file=[&]{if(g_pdPath.empty())pdEditor.on_save_file_as();
                              else{pdEditor.set_export_path(g_pdPath);pdEditor.save();}};
    csoundEditor.on_open_file=[&]{
        std::string p=g_csoundPath;
        if(!choose_editor_file(app,false,p,"Open Csound Document","csd",p))return;
        std::ifstream f(p,std::ios::binary); if(!f)return;
        csoundEditor.setText(std::string(std::istreambuf_iterator<char>(f),{}));g_csoundPath=p;
        if(csoundEditor.on_recompile)csoundEditor.on_recompile();
        csoundWin.title="Csound - "+project_display_name(p);
    };
    csoundEditor.on_save_file_as=[&]{
        std::string p=g_csoundPath;
        if(!choose_editor_file(app,true,p,"Save Csound Document","csd",p))return;
        std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<csoundEditor.text();g_csoundPath=p;
        csoundWin.title="Csound - "+project_display_name(p);
    };
    csoundEditor.on_save_file=[&]{if(g_csoundPath.empty())csoundEditor.on_save_file_as();
                                  else{std::ofstream f(g_csoundPath,std::ios::binary|std::ios::trunc);f<<csoundEditor.text();}};
    rackEditor.on_open_file=[&]{
        rackx::RackEngine* eng=rackEditor.engine();if(!eng)return;std::string p=g_rackPath;
        if(!choose_editor_file(app,false,p,"Open Rack Patch","pkr",p))return;
        if(load_rack_patch(*eng,p)){g_rackPath=p;g_rackIOsig=-1;rackWin.title="Modular - "+project_display_name(p);}
    };
    rackEditor.on_save_file_as=[&]{
        rackx::RackEngine* eng=rackEditor.engine();if(!eng)return;std::string p=g_rackPath;
        if(!choose_editor_file(app,true,p,"Save Rack Patch","pkr",p))return;
        if(save_rack_patch(*eng,p)){g_rackPath=p;rackWin.title="Modular - "+project_display_name(p);}
    };
    rackEditor.on_save_file=[&]{rackx::RackEngine* eng=rackEditor.engine();if(!eng)return;
        if(g_rackPath.empty())rackEditor.on_save_file_as();else save_rack_patch(*eng,g_rackPath);};
    ui::SamplerEditorView vSamplerEd;
    Window samplerWin; samplerWin.title = "Sampler"; samplerWin.content = &vSamplerEd; samplerWin.visible = false;
    // Shared waveform editor + disk browser, used by any rack module that holds
    // a sample (rackx::ISampleSlot).  One instance: it is re-bound to whichever
    // module's slot was asked for, so there is never a stale editor on a module
    // that has since been deleted.
    std::string g_lastSampleDir;          // folder the browser reopens in
    // Adapter that makes a Buzz Sampler ZONE look like an ISampleSlot, so the
    // instrument and the SMPL-1 module share one editor.
    sampleslot::SamplerInstrumentSlot vInstrSlot;
    sampleslot::SampleSlotPanel vSampleSlot;
    // Audition a browser file without loading it into the slot: decode it and
    // hand it to the engine's preview player, the same one the Buzz sampler's
    // browser uses.  Wired once here rather than per-open so it is live for both
    // the rack module and the instrument.
    vSampleSlot.on_preview = [&](const std::string& path) {
        PatchKnob::engine::AudioClip clip;
        std::string err;
        if (!PatchKnob::engine::loadWav(path, PatchKnob::app::audio_app_sample_rate(),
                                        clip, &err) || clip.empty())
            return;
        PatchKnob::app::audio_app_preview_clip(clip, 1.0f);
    };
    Window sampleSlotWin; sampleSlotWin.title = "Sample Editor";
    sampleSlotWin.content = &vSampleSlot; sampleSlotWin.visible = false;
    std::function<void()> rebuild_track_midi_routes;
    VirtualMidiConfigView virtualMidiConfig;
    virtualMidiConfig.get_inputs = []{ return PatchKnob::app::audio_app_virtual_midi_inputs(); };
    virtualMidiConfig.get_outputs=[] { return PatchKnob::app::audio_app_virtual_midi_outputs(); };
    virtualMidiConfig.set_counts = [&](int ins,int outs) {
        PatchKnob::app::audio_app_virtual_midi_set_ports(ins,outs);
        if(rebuild_track_midi_routes) rebuild_track_midi_routes();
    };
    Window virtualMidiWin; virtualMidiWin.title="Instrument Virtual MIDI Ports";
    virtualMidiWin.content=&virtualMidiConfig; virtualMidiWin.visible=false;
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
    // Maps an arrange sequence -> the STABLE id of its scheduled engine region
    // on g_seqToTrack[seq]'s player.  Region edits used to address the engine
    // by (track, clip pointer), which resolves to the FIRST region over that
    // source -- ambiguous (and silently wrong) the moment a track holds two
    // slices of one shared clip, which is exactly what a split produces.
    // Recorded at every attach site; consulted by every region-edit callback.
    static std::map<int,unsigned long long> g_seqRegion;
    // Resolve a seq's engine region id, tolerating attach paths that predate
    // the map (old projects, code paths that only know the clip pointer): an
    // unknown seq falls back to the track's sole region over that clip -- but
    // NEVER to "the first of several", which is the corruption this map ends.
    // Returns 0 when the region cannot be identified unambiguously.
    auto region_id_for_seq = [&](int seq) -> unsigned long long {
        auto rid = g_seqRegion.find(seq);
        if (rid != g_seqRegion.end()) {
            // Stale entries (region removed meanwhile) must not shadow a
            // legitimate re-resolution.
            auto tr = g_seqToTrack.find(seq);
            if (tr != g_seqToTrack.end() &&
                PatchKnob::app::audio_app_project_region_clip(tr->second, rid->second))
                return rid->second;
            g_seqRegion.erase(rid);
        }
        auto tr = g_seqToTrack.find(seq);
        const PatchKnob::engine::AudioClip* clip = vArrange.audio_clip(seq);
        if (tr == g_seqToTrack.end() || !clip) return 0;
        unsigned long long only = 0;
        const int n = PatchKnob::app::audio_app_project_region_count(tr->second);
        for (int i = 0; i < n; ++i) {
            const unsigned long long id =
                PatchKnob::app::audio_app_project_region_id_at(tr->second, i);
            if (PatchKnob::app::audio_app_project_region_clip(tr->second, id) != clip)
                continue;
            if (only) return 0;              // several slices: ambiguous, refuse
            only = id;
        }
        if (only) g_seqRegion[seq] = only;
        return only;
    };
    static int g_arrangeRecTarget = -1;
    static long g_recordTimelineStart = 0;
    static std::map<int,int> g_arrangeRecInput;
    static std::map<int,int> g_arrangeMidiOut;
    static std::map<int,int> g_arrangeAudioIn;
    static std::map<int,int> g_arrangeAudioOut;
    static std::map<int,int> g_arrangeInstrument; // sequence -> explicitly selected node
    static std::map<int,int> g_instrTrack;   // instrument nodeId -> mixer track
    static std::map<int,int> g_midiTrackNode; // mixer track -> hidden MIDI track node
    static std::set<std::tuple<int,int,int,int>> g_autoMidiEdges;
    mobileUi.set_note_callbacks([&](int note,int velocity) {
        auto it=g_seqToTrack.find(g_arrangeRecTarget);
        if(it==g_seqToTrack.end()) return;
        PatchKnob::app::audio_app_route_midi(
            it->second,0x90,(unsigned char)note,(unsigned char)velocity);
    },[&](int note) {
        auto it=g_seqToTrack.find(g_arrangeRecTarget);
        if(it==g_seqToTrack.end()) return;
        PatchKnob::app::audio_app_route_midi(
            it->second,0x80,(unsigned char)note,0);
    });
    vArrange.on_track_level = [&](int seq, int channel) {
        auto it = g_seqToTrack.find(seq);
        if (it == g_seqToTrack.end()) return 0.f;
        const int node = PatchKnob::app::audio_app_master_mixer_node();
        return channel == 0 ? PatchKnob::app::audio_app_mixer_vu_left(node, it->second)
                            : PatchKnob::app::audio_app_mixer_vu_right(node, it->second);
    };
    // ---- automation clip preview in the ARRANGE view ------------------------
    // The arrange block draws straight off the ENGINE region, so the curve, its
    // loop repeats and the live playhead dot are the automation that is played.
    // The view is told the region's geometry once per clip per frame and then
    // asks for one sampled run per lane -- it never touches the lane model.
    vArrange.on_automation_info=[&](int seq){
        arrange::ArrangeView::AutoClipInfo info;
        const auto* r=autoPlayer.findRegion(seq);
        if(!r) return info;
        // Lane INDICES are what the sampler takes, so report the real lane
        // count and point `focus` at the first lane that actually has a curve;
        // empty lanes simply sample to nothing.
        const int n=r->automation.laneCount();
        int firstUsed=-1;
        for(int i=0;i<n;++i) if(!r->automation.lane(i).empty()){firstUsed=i;break;}
        if(firstUsed<0) return info;
        info.valid=true; info.lanes=n; info.focus=firstUsed;
        info.position  =(long)r->position;
        info.length    =(long)std::max<int64_t>(1,r->length);
        info.loopLength=(long)std::max<int64_t>(1,r->loopLength);
        info.source    =(long)std::max<int64_t>(0,r->source);
        info.muted     =r->muted;
        return info;
    };
    vArrange.on_automation_sample=[&](int seq,int laneIdx,long t0,long t1,int count,
                                      std::vector<float>& out){
        const auto* r=autoPlayer.findRegion(seq);
        if(!r||laneIdx<0||laneIdx>=r->automation.laneCount()||count<1) return;
        const auto& lane=r->automation.lane(laneIdx);
        if(lane.empty()) return;
        // Exactly AutomationPlayer::advanceRegions' wrap: a region longer than
        // its loop repeats the curve, so the drawn line and the emitted value
        // cannot disagree.
        const int64_t loop=std::max<int64_t>(1,r->loopLength);
        out.reserve(out.size()+(size_t)count);
        for(int i=0;i<count;++i){
            const int64_t tk=(int64_t)t0+
                (count<2?0:(((int64_t)t1-(int64_t)t0)*i)/(count-1));
            int64_t m=tk%loop; if(m<0)m+=loop;
            out.push_back(lane.value_at(m));
        }
    };
    // Ruler click / drag-scrub.  Same helper the transport BUTTONS use, so a
    // playhead put there by dragging the ruler and one put there by pressing
    // rewind end up in identical state -- they used to differ, and PLAY resumed
    // from a different spot depending on which one you had used last.  (The
    // ruler snaps to the grid, ALT for a free position; that decision belongs to
    // the ruler, which knows the zoom, and reaches us already snapped.)
    vArrange.on_seek = [&](long tick) { transport_seek(tick); };
    // Track Record Enable display, PT p640/645 mapped onto the ONE bool this
    // hook can return (the arrange view draws lit/unlit; a richer per-state
    // hook would need an arrange_view.h change, which this round forbids):
    //   recording            -> solid lit
    //   punch-enabled only   -> solid lit
    //   punch + record armed -> flashing (wall-clock square wave)
    //   record armed only    -> flashing in punch modes, solid in Normal
    vArrange.is_track_record_armed = [&](int seq){
        const bool armed = g_arrangeRecTarget == seq;
        const auto tr = g_seqToTrack.find(seq);
        const bool punch = tr != g_seqToTrack.end() &&
                           g_punchTracks.count(tr->second) != 0;
        const bool recording =
            (g_recArmed && armed) ||
            (tr != g_seqToTrack.end() &&
             g_punchRecordingTracks.count(tr->second) != 0);
        if (recording) return true;
        const bool blink = (SDL_GetTicks() / 350u) & 1u;
        if (punch && armed) return blink;
        if (punch) return true;
        if (armed && g_recMode != RecMode::Normal) return blink;
        return armed;
    };
    vArrange.on_track_record_arm = [&](int seq) {
        // The punch machinery owns this button once assigned: it decodes the
        // modifier gestures (punch enable) and mid-pass punches, and falls
        // back to the legacy single-target arm for a plain click.
        if (punch_track_button) { punch_track_button(seq); return; }
        // A take owns one immutable destination. Reassigning the arm while the
        // audio thread is capturing made the drain switch to another node and
        // split one performance across two tracks.
        if (g_recArmed) {
            projectStatus = "Stop recording before arming a different track";
            return;
        }
        g_arrangeRecTarget = (g_arrangeRecTarget == seq) ? -1 : seq;
        if(rebuild_track_midi_routes) rebuild_track_midi_routes();
    };
    // The clip's instrument list = the playable nodes currently in the patcher,
    // by name: VST INSTRUMENTS plus Pure Data and Modular (Rack) patches -- all
    // honor a MIDI channel filter, so selecting one routes the clip's MIDI to it.
    auto instrument_nodes = [&]{
        std::vector<std::pair<int,std::string>> v;   // (patch node id, name)
        for (const auto& n : vPatch.nodes())
            if (PatchKnob::app::audio_app_patch_node_is_instrument((int)n.id))
                v.push_back({ (int)n.id, n.name });
        return v;
    };
    auto instrument_for_sequence = [&](int seq) {
        const auto it=g_arrangeInstrument.find(seq);
        return it!=g_arrangeInstrument.end()&&
               PatchKnob::app::audio_app_patch_node_is_instrument(it->second)
             ? it->second : -1;
    };
    rebuild_track_midi_routes = [&] {
        auto* graph=PatchKnob::app::audio_app_patch_graph();
        if(!graph) return;
        const int source=PatchKnob::app::audio_app_patch_midi_in_node();
        const int vm=PatchKnob::app::audio_app_virtual_midi_node();
        // Remove exactly the edges the previous automatic rebuild created.
        // Anything drawn in the patcher is user-owned and must survive.
        for(const auto& e:g_autoMidiEdges)
            PatchKnob::app::audio_app_patch_disconnect(
                std::get<0>(e),std::get<1>(e),std::get<2>(e),std::get<3>(e));
        g_autoMidiEdges.clear();
        const auto connect_auto=[&](int fn,int fp,int tn,int tp){
            bool present=false;
            for(const auto& c:graph->connections())
                if((int)c.from.node==fn&&(int)c.from.port==fp&&
                   (int)c.to.node==tn&&(int)c.to.port==tp) {present=true;break;}
            if(present||PatchKnob::app::audio_app_patch_connect(fn,fp,tn,tp))
                g_autoMidiEdges.emplace(fn,fp,tn,tp);
        };
        const auto old=graph->connections();
        const int ni=PatchKnob::app::audio_app_virtual_midi_inputs();
        const int no=PatchKnob::app::audio_app_virtual_midi_outputs();
        std::map<int,int> representative;
        for(int si=0;si<c_max_sequence;++si) if(perf.is_active(si)&&g_seqToTrack.count(si))
            if(!representative.count(g_seqToTrack[si])) representative[g_seqToTrack[si]]=si;
        for(auto it=g_midiTrackNode.begin();it!=g_midiTrackNode.end();) {
            if(!representative.count(it->first)) {
                paramView.forget_instance(PatchKnob::app::audio_app_patch_node_instance(it->second));
                PatchKnob::app::audio_app_patch_remove(it->second);it=g_midiTrackNode.erase(it);
            } else ++it;
        }
        int armedTrack=-1;
        if(g_seqToTrack.count(g_arrangeRecTarget)) armedTrack=g_seqToTrack[g_arrangeRecTarget];
        std::map<int,std::vector<int>> tracksByInput;
        std::map<int,int> trackOutput;
        for(const auto& lane:representative) {
            const int track=lane.first,si=lane.second;
            sequence* s=perf.get_sequence(si);if(!s)continue;
            const std::string nm=s->get_name()?s->get_name():"";
            if(nm.compare(0,6,"Audio ")==0)continue;
            int& tn=g_midiTrackNode[track];
            if(tn<=0) tn=PatchKnob::app::audio_app_add_midi_track_node();
            if(tn<0)continue;
            // Routing belongs to the TRACK, while clips/patterns are separate
            // sequences on that track.  Never let an arbitrary representative
            // sequence overwrite the route selected on another clip.  During a
            // take the armed lane is authoritative; otherwise use the first
            // explicitly stored choice on the track.
            int in=-1,out=-1; bool haveIn=false,haveOut=false;
            if(g_arrangeRecTarget>=0&&g_seqToTrack.count(g_arrangeRecTarget)&&
               g_seqToTrack[g_arrangeRecTarget]==track) {
                auto ri=g_arrangeRecInput.find(g_arrangeRecTarget);
                if(ri!=g_arrangeRecInput.end()){in=ri->second;haveIn=true;}
                auto ro=g_arrangeMidiOut.find(g_arrangeRecTarget);
                if(ro!=g_arrangeMidiOut.end()){out=ro->second;haveOut=true;}
            }
            for(int sj=0;sj<c_max_sequence&&(!haveIn||!haveOut);++sj)
                if(perf.is_active(sj)&&g_seqToTrack.count(sj)&&g_seqToTrack[sj]==track) {
                    auto ri=g_arrangeRecInput.find(sj);
                    if(!haveIn&&ri!=g_arrangeRecInput.end()){in=ri->second;haveIn=true;}
                    auto ro=g_arrangeMidiOut.find(sj);
                    if(!haveOut&&ro!=g_arrangeMidiOut.end()){out=ro->second;haveOut=true;}
                }
            if(in<0||in>=ni)in=-1;if(out<0||out>=no)out=-1;
            for(int sj=0;sj<c_max_sequence;++sj) if(perf.is_active(sj)&&
                g_seqToTrack.count(sj)&&g_seqToTrack[sj]==track) {
                g_arrangeRecInput[sj]=in;g_arrangeMidiOut[sj]=out;
            }
            PatchKnob::app::audio_app_midi_track_monitor(tn,track==armedTrack);
            PatchKnob::app::audio_app_midi_track_set_routing(tn,in,out);
            if(in>=0)tracksByInput[in].push_back(tn);
            if(out>=0)trackOutput[track]=out;
            // Sequencer playback is private to this lane. Live input is added
            // below only from explicit cables terminating at its selected
            // virtual input.
            connect_auto(source,track,tn,1);
            const int instrument=instrument_for_sequence(si);
            if(instrument>=0){const int midiIn=PatchKnob::app::audio_app_patch_first_port(instrument,1,0);
                if(midiIn>=0)connect_auto(tn,2,instrument,midiIn);}
        }
        // Resolve public patch cables without ever sending data through the
        // virtual node itself. One source may intentionally fan out to tracks
        // selecting the same input; unrelated inputs remain isolated.
        for(const auto& c:old) {
            if((int)c.to.node==vm&&(int)c.to.port>=0&&(int)c.to.port<ni &&
               (int)c.from.node!=vm) {
                const auto found=tracksByInput.find((int)c.to.port);
                if(found!=tracksByInput.end()) for(int tn:found->second)
                    connect_auto(
                        (int)c.from.node,(int)c.from.port,tn,0);
            }
        }
        // A drawn o-N -> i-M internal path is an explicit track-to-track
        // connection. It is metadata, resolved here as a direct node edge.
        for(const auto& src:trackOutput) {
            const int routed=PatchKnob::app::audio_app_virtual_midi_route(src.second);
            const auto dest=tracksByInput.find(routed);
            if(routed>=0&&dest!=tracksByInput.end()) {
                const auto from=g_midiTrackNode.find(src.first);
                if(from!=g_midiTrackNode.end()) for(int tn:dest->second)
                    if(tn!=from->second)
                        connect_auto(from->second,2,tn,0);
            }
        }
        // Explicit cables leaving a virtual output resolve from only the
        // tracks whose selected output matches that endpoint.
        for(const auto& c:old) if((int)c.from.node==vm&&
            (int)c.from.port>=100&&(int)c.from.port<100+no&&(int)c.to.node!=vm) {
            const int out=(int)c.from.port-100;
            for(const auto& src:trackOutput) if(src.second==out) {
                const auto from=g_midiTrackNode.find(src.first);
                if(from!=g_midiTrackNode.end())
                    connect_auto(
                        from->second,2,(int)c.to.node,(int)c.to.port);
            }
        }
        if(sync_patch_connections) sync_patch_connections();
    };
    auto virtual_port_name = [&](bool input, int index) {
        const int vm=PatchKnob::app::audio_app_virtual_midi_node();
        std::string endpoint;
        if(auto* graph=PatchKnob::app::audio_app_patch_graph()) {
            for(const auto& c:graph->connections()) {
                int node=-1;
                if(input && (int)c.to.node==vm && (int)c.to.port==index)
                    node=(int)c.from.node;
                if(!input && (int)c.from.node==vm && (int)c.from.port==100+index)
                    node=(int)c.to.node;
                if(node>=0) {
                    if(const PatchKnob::patchbay::Node* n=vPatch.find_node(
                           (PatchKnob::patchbay::NodeId)node))
                        if(endpoint.empty() ||
                           PatchKnob::app::audio_app_patch_is_hw_midi_in(node))
                            endpoint=n->name;
                    if(PatchKnob::app::audio_app_patch_is_hw_midi_in(node)) break;
                }
            }
        }
        const std::string base=std::string(input?"i":"o")+std::to_string(index+1);
        if(!input) {
            const int routed=PatchKnob::app::audio_app_virtual_midi_route(index);
            if(routed>=0) return base+">i"+std::to_string(routed+1);
        }
        std::string tag;
        if(endpoint=="Instrument out") tag="SEQ";
        else if(endpoint.find("MIDI In")!=std::string::npos) tag="HW";
        else if(endpoint.find("MIDI Out")!=std::string::npos) tag="HW";
        else for(char c:endpoint)
            if(std::isalnum((unsigned char)c)&&tag.size()<3)
                tag+=(char)std::toupper((unsigned char)c);
        return tag.empty()?base:(base+">"+tag);
    };
    // Missing means explicitly unrouted. Drawing a header must never create a
    // route as a side effect.
    auto io_choice = [](const std::map<int,int>& m, int seq) {
        auto it = m.find(seq);
        return it == m.end() ? -1 : it->second;
    };
    vArrange.on_track_record_input = [&](int seq) {
        sequence* s = perf.get_sequence(seq);
        const std::string name = s && s->get_name() ? s->get_name() : "";
        if (name.compare(0, 6, "Audio ") != 0) {
            const int choice = io_choice(g_arrangeRecInput,seq);
            return choice<0?std::string("(none)"):
                   std::string("i-") + std::to_string(choice + 1);
        }
        const int choice = g_arrangeRecInput[seq];
        if (choice <= 0) return std::string("Audio input");
        const auto ins = instrument_nodes();
        return choice <= (int)ins.size() ? std::string("Track: ") + ins[(size_t)choice - 1].second
                                          : std::string("Audio input");
    };
    vArrange.on_cycle_track_record_input = [&](int seq) {
        sequence* s = perf.get_sequence(seq);
        const std::string name = s && s->get_name() ? s->get_name() : "";
        if (name.compare(0, 6, "Audio ") != 0) {
            const int count = PatchKnob::app::audio_app_virtual_midi_inputs();
            g_arrangeRecInput[seq] =
                (io_choice(g_arrangeRecInput,seq)+2)%(count+1)-1;
            rebuild_track_midi_routes();
            return;
        }
        const int count = 1 + (int)instrument_nodes().size();
        g_arrangeRecInput[seq] = (g_arrangeRecInput[seq] + 1) % std::max(1, count);
        const int choice = g_arrangeRecInput[seq];
        auto route = g_seqToTrack.find(seq);
        const auto ins = instrument_nodes();
        if (choice > 0 && choice <= (int)ins.size() && route != g_seqToTrack.end()) {
            PatchKnob::app::audio_app_master_connect_instrument(route->second,
                                                                ins[(size_t)choice - 1].first);
            if (sync_patch_connections) sync_patch_connections();
        }
    };
    // Read a routing choice WITHOUT creating one.  These queries run from draw(),
    // once per visible header per frame, and std::map::operator[] default-inserts
    // on a miss -- so merely looking at the arrange view populated all four maps
    // with a 0 for every track.  Every track then claimed virtual MIDI out 0, the
    // "one owner per virtual output" swap in on_pick_track_io fought over those
    // phantom entries, and rebuild_track_midi_routes wired tracks nobody had
    // routed.  Only an explicit pick may insert.
    auto track_midi_in = [&](int seq) {
        return io_choice(g_arrangeRecInput, seq);
    };
    auto midi_track_for_seq = [&](int seq) {
        const auto tr=g_seqToTrack.find(seq); if(tr==g_seqToTrack.end()) return -1;
        const auto n=g_midiTrackNode.find(tr->second);
        return n==g_midiTrackNode.end()?-1:n->second;
    };
    vArrange.on_track_io_label = [&](int seq, int tab) {
        if (tab == 0)
            return track_midi_in(seq)<0?std::string("(none)"):
                   virtual_port_name(true, track_midi_in(seq));
        if (tab == 1)
            return io_choice(g_arrangeMidiOut,seq)<0?std::string("(none)"):
                   virtual_port_name(false, io_choice(g_arrangeMidiOut, seq));
        if (tab == 2) {
            const int choice = io_choice(g_arrangeAudioIn, seq);
            if (choice <= 0) return std::string("Audio input");
            const auto ins = instrument_nodes();
            return choice <= (int)ins.size()
                ? std::string("Track: ") + ins[(size_t)choice - 1].second
                : std::string("Audio input");
        }
        return std::string("Master Out ") +
               std::to_string(io_choice(g_arrangeAudioOut, seq) + 1);
    };
    vArrange.on_cycle_track_io = [&](int seq, int tab) {
        if (tab == 0) {
            const int count = PatchKnob::app::audio_app_virtual_midi_inputs();
            g_arrangeRecInput[seq] =
                (io_choice(g_arrangeRecInput,seq)+2)%(count+1)-1;
            rebuild_track_midi_routes();
            return;
        }
        if (tab == 1) {
            const int count = PatchKnob::app::audio_app_virtual_midi_outputs();
            const int oldChoice = io_choice(g_arrangeMidiOut,seq);
            const int choice = (oldChoice + 2) % (count + 1) - 1;
            g_arrangeMidiOut[seq] = choice;
            (void)oldChoice;
            rebuild_track_midi_routes();
            return;
        }
        if (tab == 2) {
            const auto ins = instrument_nodes();
            const int count = 1 + (int)ins.size();
            const int choice =
                (g_arrangeAudioIn[seq] + 1) % std::max(1, count);
            g_arrangeAudioIn[seq] = choice;
            auto route = g_seqToTrack.find(seq);
            if (choice > 0 && route != g_seqToTrack.end()) {
                PatchKnob::app::audio_app_master_route_audio_input(
                    route->second, ins[(size_t)choice - 1].first);
                if (sync_patch_connections) sync_patch_connections();
            }
            return;
        }
        g_arrangeAudioOut[seq] = 0; // currently the singleton master hardware bus
    };
    vArrange.on_list_track_io = [&](int, int route) {
        std::vector<std::string> items;
        if (route == 0) {
            items.push_back("(none)");
            const int n=PatchKnob::app::audio_app_virtual_midi_inputs();
            for(int i=0;i<n;++i) items.push_back(virtual_port_name(true,i));
        } else if (route == 1) {
            items.push_back("(none)");
            const int n=PatchKnob::app::audio_app_virtual_midi_outputs();
            for(int i=0;i<n;++i) items.push_back(virtual_port_name(false,i));
        } else if (route == 2) {
            items.push_back("Audio input");
            for(const auto& item:instrument_nodes()) items.push_back("Track: "+item.second);
        } else {
            items.push_back("Master Out 1");
        }
        return items;
    };
    vArrange.on_pick_track_io = [&](int seq, int route, int idx) {
        const int count=(int)vArrange.on_list_track_io(seq,route).size();
        if(idx<0||idx>=count) return;
        if(route==0) {
            const int endpoint=idx-1; g_arrangeRecInput[seq]=endpoint;
            // The header may currently display any clip sequence belonging to
            // this lane.  Apply the choice to the whole track immediately so a
            // later graph rebuild cannot resurrect a stale per-clip route.
            auto owner=g_seqToTrack.find(seq);
            if(owner!=g_seqToTrack.end()) for(int sj=0;sj<c_max_sequence;++sj)
                if(perf.is_active(sj)&&g_seqToTrack.count(sj)&&
                   g_seqToTrack[sj]==owner->second) g_arrangeRecInput[sj]=endpoint;
            if (endpoint>=0&&!PatchKnob::app::audio_app_virtual_midi_input_is_patched(endpoint))
                projectStatus = virtual_port_name(true, endpoint) +
                                " has nothing patched into it - patch a MIDI"
                                " source to it in the patcher to record";
        }
        else if(route==1) {
            g_arrangeMidiOut[seq]=idx-1;
            auto owner=g_seqToTrack.find(seq);
            if(owner!=g_seqToTrack.end()) for(int sj=0;sj<c_max_sequence;++sj)
                if(perf.is_active(sj)&&g_seqToTrack.count(sj)&&
                   g_seqToTrack[sj]==owner->second) g_arrangeMidiOut[sj]=idx-1;
        } else if(route==2) {
            g_arrangeAudioIn[seq]=idx;
            const auto ins=instrument_nodes();
            auto tr=g_seqToTrack.find(seq);
            if(tr!=g_seqToTrack.end()) {
                if(idx>0&&idx<=(int)ins.size())
                    PatchKnob::app::audio_app_master_route_audio_input(
                        tr->second,ins[(size_t)idx-1].first);
                else
                    // Choosing "Audio input" must UNDO an instrument route.
                    // Leaving the old one wired meant the dropdown said one
                    // thing and the graph did another.
                    PatchKnob::app::audio_app_master_clear_audio_input(tr->second);
            }
        } else g_arrangeAudioOut[seq]=idx;
        if(route<2) rebuild_track_midi_routes();
        if(sync_patch_connections) sync_patch_connections();
        app.request_redraw();
    };
    vArrange.on_track_io_index = [&](int seq,int route) {
        if(route==0) return track_midi_in(seq)+1;
        if(route==1) return io_choice(g_arrangeMidiOut, seq)+1;
        if(route==2) return io_choice(g_arrangeAudioIn, seq);
        return io_choice(g_arrangeAudioOut, seq);
    };
    // Destination ownership is the graph edge, never a MIDI-channel filter.
    auto make_instr_graph_routed = [&](int nodeId) {
        PatchKnob::app::audio_app_patch_set_node_channel(nodeId,-1);
    };
    auto instr_current = [&](sequence* s) -> std::string {
        if (!s) return "-";
        for(int si=0;si<c_max_sequence;++si)
            if(perf.is_active(si)&&perf.get_sequence(si)==s) {
                const int node=instrument_for_sequence(si);
                for(const auto& p:instrument_nodes())if(p.first==node)return p.second;
                break;
            }
        return "(none)";
    };
    // Make a selected instrument actually PLAYABLE: ensure MIDI-In -> instrument
    // (so the clip's notes reach it) and instrument -> Audio Out (so you hear it)
    // are wired.  Only adds the missing links (a user's mixer routing is kept),
    // and wiring into Out auto-enables the modular render path (see on_connect).
    // Each instrument node gets its own master-mixer AUDIO track (one source of
    // truth for its mixer channel), so its audio flows MidiIn->inst->mixer
    // inlet->master->Out and the per-track mixer strip is live.
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
        make_instr_graph_routed(nodeId);
        for (int si = 0; si < c_max_sequence; ++si) {
            if (perf.is_active(si) && perf.get_sequence(si) == s) {
                g_arrangeInstrument[si]=nodeId;
                const auto tr=g_seqToTrack.find(si);
                if(tr!=g_seqToTrack.end()) {
                    PatchKnob::app::audio_app_master_connect_instrument(tr->second,nodeId);
                    s->set_midi_bus((char)tr->second);
                }
                break;
            }
        }
        s->set_midi_channel(0); // event data only; no longer selects a destination
        s->set_dirty();
        rebuild_track_midi_routes();
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
        PatchKnob::engine::patch::PatchGraph* graph=PatchKnob::app::audio_app_patch_graph();
        if(!graph)return out;
        for (PatchKnob::engine::patch::NodeId graphId : graph->nodeIds()) {
            const int nodeId = (int)graphId;
            const auto* uiNode=vPatch.find_node((PatchKnob::patchbay::NodeId)nodeId);
            const auto* engineNode=graph->node(graphId);
            const std::string nodeBaseName=uiNode?uiNode->name:
                (engineNode?std::string(engineNode->typeName()):std::string("Node ")+std::to_string(nodeId));
            // Display names are not identities: projects may contain several
            // racks with the same label.  Qualify every hierarchy root so two
            // equal names never merge into one automation submenu.
            const std::string nodeName=nodeBaseName+" [N"+std::to_string(nodeId)+"]";
            PatchKnob::engine::IPluginInstance* inst =
                PatchKnob::app::audio_app_patch_node_instance(nodeId);
            if (inst) {
                int count = inst->paramCount();
                for (int i = 0; i < count; ++i) {
                    PatchKnob::engine::ParamInfo pi = inst->paramInfo(i);
                    ui::FxBinding b;
                    b.type = ui::FX_VST_PARAM;
                    b.target = ui::FX_TARGET_PATCH_PLUGIN_PARAM;
                    b.pid = pi.id;
                    b.node = nodeId;
                    b.label = tracker_fx_label(pi.name, "P");
                    b.name = nodeName + " > " + pi.name;
                    out.push_back(b);
                }
            } else if (rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine(nodeId)) {
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
                        // Multiple instances of the same Rack model are common.
                        // Keep each module as a distinct selectable device.
                        const std::string moduleName=mod->name+" [M"+std::to_string(mod->id)+"]";
                        b.name = nodeName + " > " + moduleName + " > " + pname;
                        out.push_back(b);
                    }
                }
            }
        }
        return out;
    };
    // The automation editor uses the exact same routable target inventory as
    // tracker FX, but preserves its hierarchy instead of flattening hundreds
    // of parameters into one menu.
    vAuto.on_list_targets = [&]{
        std::vector<automation::AutomationView::TargetChoice> out;
        const std::vector<ui::FxBinding> src = vTracker.on_list_fx_targets
            ? vTracker.on_list_fx_targets() : std::vector<ui::FxBinding>();
        for(const ui::FxBinding& b:src){
            automation::AutomationView::TargetChoice t;
            t.target.id=b.pid;t.target.node=b.node;t.target.module=b.module;
            t.target.minValue=b.min_value;t.target.maxValue=b.max_value;
            t.target.kind=b.target==ui::FX_TARGET_RACK_PARAM
                ? PatchKnob::engine::LaneTargetKind::RackParam
                : PatchKnob::engine::LaneTargetKind::PatchParam;
            std::string rest=b.name;
            const size_t a=rest.find(" > ");
            t.node=a==std::string::npos?rest:rest.substr(0,a);
            rest=a==std::string::npos?std::string():rest.substr(a+3);
            const size_t d=rest.find(" > ");
            if(d==std::string::npos)t.parameter=rest;
            else {t.device=rest.substr(0,d);t.parameter=rest.substr(d+3);}
            out.push_back(t);
        }
        return out;
    };

    // (An instrument-selector strip -- ui::ClipHost -- used to wrap these two
    // editors.  Instrument selection is gone from the clip editors: an
    // instrument owns a mixer channel, so the editor edits the sequence
    // directly and the clip windows host the view with no strip.)
    Window pianoWin;   pianoWin.title  = "Piano Roll"; pianoWin.content  = &vPiano;
    Window trackerWin; trackerWin.title= "Tracker";    trackerWin.content= &vTracker;
    Window automationWin; automationWin.title="Automation Clip"; automationWin.content=&vAuto;
    pianoWin.visible = trackerWin.visible = automationWin.visible = false;
    pianoWin.rect = { 90, 80, 780, 500 }; trackerWin.rect = { 130, 110, 720, 470 };
    automationWin.rect = { 110, 95, 860, 520 };
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
        auto still_active=[&](sequence* candidate){
            if(!candidate)return false;
            for(int i=0;i<c_max_sequence;++i)
                if(perf.is_active(i)&&perf.get_sequence(i)==candidate)return true;
            return false;
        };
        sequence* pianoSeq=still_active(vPiano.get_sequence())?vPiano.get_sequence():primary;
        sequence* trackerSeq=still_active(vTracker.get_sequence())?vTracker.get_sequence():primary;
        if(vPiano.get_sequence()!=pianoSeq)vPiano.set_sequence(pianoSeq);
        if(vTracker.get_sequence()!=trackerSeq)
            vTracker.set_sequence(trackerSeq,trackerSeq?trackerSeq->get_midi_bus():-1);
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
    Window patchWin; patchWin.title = "Patchbay"; patchWin.content = &bottomTabs;
    patchWin.visible = true;
    patchWin.alwaysBack = true;
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
    auto load_plugin_into_node = [&](int target,const PatchKnob::engine::PluginDescriptor& d){
        if(target<0||!audio_ok)return false;
        if(!PatchKnob::app::audio_app_patch_set_node_plugin(target,d))return false;
        if (PatchKnob::patchbay::Node* n = vPatch.find_node((PatchKnob::patchbay::NodeId)target)) {
            std::string nm = d.name;
            size_t s = nm.find_last_of("/\\"); if (s != std::string::npos) nm = nm.substr(s+1);
            size_t dot = nm.find_last_of('.'); if (dot != std::string::npos) nm = nm.substr(0,dot);
            if (!nm.empty()) n->name = nm;
        }
        if(sync_patch_connections)sync_patch_connections();
        return true;
    };
    pluginPicker.on_pick = [&](const PatchKnob::engine::PluginDescriptor& d){
        if (pickerTarget >= 0 && audio_ok) {
            if(!load_plugin_into_node(pickerTarget,d)) {
                // LOUD failure: a silent status line is missable, and "I picked a
                // plug-in and nothing happened" is indistinguishable from a bug.
                projectStatus="Plugin failed to load: "+d.path;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Plugin load failed",
                                         projectStatus.c_str(),app.window);
            }
        }
        pickerWin.visible = false; app.request_redraw();
    };
    vPatch.on_open_editor = [&](PatchKnob::patchbay::NodeId id){
        pickerTarget = (int)id;
        { std::lock_guard<std::mutex> lk(g_scanMutex); pluginPicker.plugins = g_scan; }
        pluginPicker.instrumentsOnly = false;   // allow effects too
        pluginPicker.status = g_scanDone.load(std::memory_order_acquire) ? std::string() : std::string("scanning plugins...");
        open_window(pickerWin, 440, 480);
    };
    vPatch.on_load_plugin_file = [&](PatchKnob::patchbay::NodeId id){
        std::string path;
        if(!choose_editor_file(app,false,path,"Load VST Plugin","dll",path)) return;
        auto* host=PatchKnob::app::audio_app_host();
        if(!host)return;
        host->setProbeTimeoutMs(5000);
        std::vector<PatchKnob::engine::PluginDescriptor> found=host->probeFile(path);
        if(found.empty()) {
            projectStatus="No loadable 64-bit VST found in: "+path;
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Plugin load failed",
                                     projectStatus.c_str(),app.window);
            return;
        }
        pickerTarget=(int)id;
        if(found.size()==1) {
            if(!load_plugin_into_node(pickerTarget,found.front()))
                projectStatus="Plugin failed to instantiate: "+path;
        } else {
            // A VST3 bundle may export several audio classes; let the user pick
            // the exact class instead of silently loading the first one.
            pluginPicker.plugins=std::move(found);
            pluginPicker.instrumentsOnly=false;
            pluginPicker.status="Choose a class from the selected plugin";
            open_window(pickerWin,440,480);
        }
        app.request_redraw();
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
            const bool nativeFx=inst->descriptor().path.rfind("builtin://",0)==0;
            open_window(paramWin, nativeFx?700:460, nativeFx?390:520);
            return;
        }
        g_guiHandle = h; g_guiNode = (int)id;
        int nw=480, nh=320; PatchKnob::hostwin::editor_native_size(h,&nw,&nh);
        float sc = app.scale>0 ? app.scale : 1.f;
        // DETACHED (Wayland session): the editor is its own OS window because
        // there is no X11 window id to embed into -- SDL is on Wayland and
        // VST3's only Linux platform type is kPlatformTypeX11EmbedWindowID.
        // The in-app frame then holds none of the GUI's pixels; SAY so in its
        // body instead of presenting an empty box, and mark the title.  The
        // frame is still load-bearing: it titles the editor, hiding it hides
        // the OS window, and closing it closes the editor.
        static ui::Label guiDetachedNote;
        if (PatchKnob::hostwin::editor_is_detached(h)) {
            guiDetachedNote.text = "Editor is open in its own window (Wayland: "
                                   "embedding needs X11). Close this frame to "
                                   "close it; hide it to hide it.";
            guiWin.content = &guiDetachedNote;
            guiWin.title = std::string("GUI: ") + inst->descriptor().name + " [floating]";
        } else {
            guiWin.content = nullptr;
            guiWin.title = std::string("GUI: ") + inst->descriptor().name;
        }
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
        const bool nativeFx=inst->descriptor().path.rfind("builtin://",0)==0;
        open_window(paramWin, nativeFx?700:460, nativeFx?390:520);
    };
    // Mixer strip processor slots: "Edit" (context menu) and Ctrl+click ask the
    // shell to open the insert's editor.  The hook was never assigned, so both
    // gestures did nothing; route them at the same target vPatch uses.
    mixerStrip.on_edit_insert = [&](int nodeId){
        if (vPatch.on_open_gui) vPatch.on_open_gui((PatchKnob::patchbay::NodeId)nodeId);
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
    // Open the SDL Pd editor on this node's IN-MEMORY patch (stored on the node and
    // saved with the project -- there is no .pd file on disk).
    vPatch.on_open_pd = [&](PatchKnob::patchbay::NodeId id){
        int node = (int)id;
        g_pdNode = node; g_pdPath.clear(); g_pdRackMod = -1;    // patch-node target
        const char* text = PatchKnob::app::audio_app_pd_get_text(node);   // node's patch text
        pdEditor.set_patch_text(text ? text : "");
        // subscribe GUI feedback for the loaded patch (e.g. after a project load).
        PatchKnob::app::audio_app_pd_clear_gui_binds(node);
        for (const std::string& s : pdEditor.gui_send_symbols())
            PatchKnob::app::audio_app_pd_subscribe(node, s.c_str());
        pdWin.title = "Pd";
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
    // Right-click a scripting (Pd/Csound) rack module -> edit its PANEL (drag/resize)
    // or its DSP (the Pd patcher / Csound text editor bound to THIS rack module).
    rackEditor.on_edit_panel = [&](int modId){
        rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine(g_rackNode);
        if (!eng) return;
        panelEditor.set_target(eng, modId);
        open_window(panelEditorWin, 540, 560);
    };
    auto open_module_dsp = [&](int modId){
        rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine(g_rackNode);
        if (!eng) return;
        const std::string kind = eng->moduleScriptKind(modId);
        const std::string text = eng->moduleScript(modId);
        if (kind == "csound") {
            g_csoundRackMod = modId; g_csoundNode = -1;
            csoundEditor.setText(text);
            csoundEditor.setStatus("Ctrl+E to compile", false);
            csoundWin.title = "Csound (rack module)";
            open_window(csoundWin, 640, 520);
        } else if (kind == "pd") {
            g_pdRackMod = modId; g_pdNode = -1; g_pdPath.clear();
            pdEditor.set_patch_text(text);
            pdWin.title = "Pd (rack module)";
            open_window(pdWin, 660, 480);
        }
    };
    rackEditor.on_edit_dsp = open_module_dsp;
    rackEditor.on_edit_sample = [&](int moduleId){
        rackx::RackEngine* eng = rackEditor.engine();
        if (!eng) return;
        rackx::RackModule* m = eng->moduleById(moduleId);
        if (!m || !m->mod) return;
        auto* slot = dynamic_cast<PatchKnob::engine::ISampleSlot*>(m->mod.get());
        if (!slot) return;
        vSampleSlot.set_sample_rate(PatchKnob::app::audio_app_sample_rate());
        vSampleSlot.set_slot(slot, m->name);
        // Point the browser at the folder the last sample came from, so the
        // window opens where you were working rather than at the cwd.
        vSampleSlot.set_dir(g_lastSampleDir);
        vSampleSlot.on_status = [&](const std::string& msg){ projectStatus = msg; };
        vSampleSlot.editor().on_pick_save_path = [&](std::string& p)->bool {
            std::string out = g_lastSampleDir;
            if (!choose_editor_file(app, true, out, "Save Sample As", "wav", out))
                return false;
            p = out; return true;
        };
        vSampleSlot.on_loaded = [&](const std::string& path){
            const size_t slash = path.find_last_of("/\\");
            if (slash != std::string::npos) g_lastSampleDir = path.substr(0, slash);
            app.request_redraw();
        };
        open_window(sampleSlotWin, 900, 420);
        app.request_redraw();
    };
    // A rack module that holds one sample (rackx::ISampleSlot) asks the shell to
    // pick a file; decoding + the realtime-safe swap happen inside the module.
    rackEditor.on_load_sample = [&](int moduleId){
        rackx::RackEngine* eng = rackEditor.engine();
        if (!eng) return;
        rackx::RackModule* m = eng->moduleById(moduleId);
        if (!m || !m->mod) return;
        auto* slot = dynamic_cast<rackx::ISampleSlot*>(m->mod.get());
        if (!slot) return;
        std::string path = g_lastSampleDir;
        if (!choose_editor_file(app, false, path, "Load Sample", "wav", path))
            return;
        std::string err;
        if (!slot->sampleLoad(path, PatchKnob::app::audio_app_sample_rate(), &err))
            projectStatus = "Sample load failed: " + err;
        else {
            const size_t slash = path.find_last_of("/\\");
            if (slash != std::string::npos) g_lastSampleDir = path.substr(0, slash);
            projectStatus = std::string("Loaded ") + slot->sampleName();
        }
        app.request_redraw();
    };
    panelEditor.on_edit_dsp = open_module_dsp;
    // --- Native keyzone SAMPLER editor ------------------------------------
    // Push ONE zone's engine-side parameters (envelope overrides, filter,
    // tuning, pan/attenuation, exclusive class, mod routing) through the
    // per-zone C API.  slot/level address the zone exactly as the load_ex
    // below does: slot = 1 + zone index, level = 0.  Cheap (no PCM upload),
    // so the editor fires it on every drag motion for live feedback.
    auto sampler_push_zone_params = [](int node, int slot, const ui::SamplerZone& z){
        auto env_of = [](const ui::SamplerZoneEnvUI& s){
            PatchKnob::engine::SamplerZoneEnv e;
            e.delay = s.delay; e.attack = s.attack; e.hold = s.hold; e.decay = s.decay;
            e.sustain = s.sustain; e.release = s.release; e.enabled = s.enabled;
            return e;
        };
        const PatchKnob::engine::SamplerZoneEnv ae = env_of(z.ampEnv);
        const PatchKnob::engine::SamplerZoneEnv me = env_of(z.modEnv);
        PatchKnob::app::audio_app_sampler_set_zone_env(node, slot, 0, 0, &ae);
        PatchKnob::app::audio_app_sampler_set_zone_env(node, slot, 0, 1, &me);
        PatchKnob::app::audio_app_sampler_set_zone_filter(node, slot, 0, z.cutoffHz, z.resonanceDb);
        PatchKnob::app::audio_app_sampler_set_zone_tuning(node, slot, 0, z.coarseTune, z.fineTune, z.scaleTuning);
        PatchKnob::app::audio_app_sampler_set_zone_level(node, slot, 0, z.pan, z.attenuationDb);
        PatchKnob::app::audio_app_sampler_set_zone_exclusive(node, slot, 0, z.exclusiveClass);
        PatchKnob::app::audio_app_sampler_set_zone_modroute(node, slot, 0, z.modEnvToPitchCents, z.modEnvToFilterCents);
    };
    // Push the shell's multisample zones into isolated wavetable slots.  The
    // sampler wrapper chooses the slot by key/velocity before triggering Buzz,
    // which prevents fixed-pitch zones from selecting the wrong keyrange level.
    auto sampler_push_zones = [&, sampler_push_zone_params](int node){
        auto it = g_samplerZones.find(node);
        if (it == g_samplerZones.end()) {
            for (int slot = 1; slot <= 128; ++slot)
                PatchKnob::app::audio_app_sampler_clear(node, slot);
            return;
        }
        const std::vector<ui::SamplerZone>& zones = it->second;
        const size_t nz = std::min<size_t>(zones.size(), 128);

        // A zone whose shell clip is EMPTY is NON-RESIDENT: the engine still
        // holds its PCM from the import, and the shell has deliberately not
        // materialised it (see sampler_fill_zone_clip -- keeping 2976 zones'
        // audio in the UI model costs 1.3 GB).  This used to clear all 128
        // slots up front and then skip such zones, so the first apply after an
        // import -- any mapping drag, delete or loop toggle -- silently deleted
        // every zone the user had not yet clicked on.  So: read the engine's
        // own mapping first.  A non-resident zone whose mapping still matches
        // keeps its slot untouched (its parameters are re-applied, which is
        // cheap and needs no PCM); one whose mapping HAS changed is re-pushed
        // from the engine's PCM rather than from a clip the shell never loaded.
        struct EngineZone { int index; PatchKnob::engine::SamplerZoneInfo meta; };
        std::map<int, EngineZone> engineBySlot;
        const int nEngine = PatchKnob::app::audio_app_sampler_zone_count(node);
        for (int i = 0; i < nEngine; ++i) {
            PatchKnob::engine::SamplerZoneInfo m;
            if (PatchKnob::app::audio_app_sampler_get_zone_meta(node, i, m) && m.level == 0)
                engineBySlot[m.slot] = EngineZone{ i, m };
        }
        auto clamp127 = [](int v){ return std::max(0, std::min(127, v)); };

        std::vector<char> keep(129, 0);
        std::vector<int>  reuseIndex(nz, -1);   // engine zone index to re-push from
        for (size_t lvl = 0; lvl < nz; ++lvl) {
            const ui::SamplerZone& z = zones[lvl];
            if (z.clip.numFrames() > 0 && !z.clip.ch[0].empty()) continue;   // resident
            auto e = engineBySlot.find(1 + (int)lvl);
            if (e == engineBySlot.end()) continue;   // nothing there to preserve
            const PatchKnob::engine::SamplerZoneInfo& m = e->second.meta;
            const bool sameMapping =
                m.loKey == clamp127(std::min(z.loKey, z.hiKey)) &&
                m.hiKey == clamp127(std::max(z.loKey, z.hiKey)) &&
                m.loVel == clamp127(std::min(z.loVel, z.hiVel)) &&
                m.hiVel == clamp127(std::max(z.loVel, z.hiVel)) &&
                m.rootKey == clamp127(z.root) &&
                m.loop == z.loop && m.noteOffLayer == z.noteOffLayer &&
                m.keyToPitch == z.keyToPitch && m.velToVol == z.velToVol &&
                m.overlapMode == z.overlapMode;
            if (sameMapping) keep[1 + (int)lvl] = 1;
            else             reuseIndex[lvl] = e->second.index;
        }
        // Only slots we are NOT re-pushing and that no zone still claims.
        for (int slot = 1; slot <= 128; ++slot)
            if (!keep[slot]) PatchKnob::app::audio_app_sampler_clear(node, slot);

        for (size_t lvl = 0; lvl < nz; ++lvl) {
            const ui::SamplerZone& z = zones[lvl];
            const int slot = 1 + (int)lvl;
            if (keep[slot]) { sampler_push_zone_params(node, slot, z); continue; }

            std::vector<float> il;
            long long frames = 0, n = 0;
            int sampleRate = 0;
            if (reuseIndex[lvl] >= 0) {
                // Non-resident, but its mapping changed.  Pull the PCM the
                // engine already has, push it back under the new mapping, and
                // let the temporary die here so the shell stays metadata-only.
                PatchKnob::engine::SamplerZoneInfo full;
                if (!PatchKnob::app::audio_app_sampler_get_zone(node, reuseIndex[lvl], full)) continue;
                frames = n = full.numFrames;
                if (frames <= 0) continue;
                if (full.stereo) il = std::move(full.pcm);
                else {
                    il.resize((size_t)frames * 2);
                    for (long long i = 0; i < frames; ++i)
                        il[(size_t)i*2] = il[(size_t)i*2+1] = full.pcm[(size_t)i];
                }
                sampleRate = full.sampleRate > 0 ? full.sampleRate
                                                 : PatchKnob::app::audio_app_sample_rate();
            } else {
                n = z.clip.numFrames();
                if (n <= 0 || z.clip.ch[0].empty()) continue;
                long long a = (long long)(std::max(0.f, std::min(1.f, z.start)) * (float)n);
                long long b = (long long)(std::max(0.f, std::min(1.f, z.end)) * (float)n);
                if (b <= a) { a = 0; b = n; }
                frames = std::max(1LL, b - a);
                il.resize((size_t)frames * 2);
                const float* L = z.clip.ch[0].data();
                const float* R = z.clip.ch[1].empty() ? L : z.clip.ch[1].data();
                // Pan is NOT baked into the PCM any more: the engine applies the
                // zone's pan via audio_app_sampler_set_zone_level below, so moving
                // the pan slider never re-uploads the sample.
                for (long long i = 0; i < frames; ++i) {
                    long long si = z.reverse ? (b - 1 - i) : (a + i);
                    if (si < 0) si = 0; if (si >= n) si = n - 1;
                    il[(size_t)i*2] = L[si] * z.gain;
                    il[(size_t)i*2+1] = R[si] * z.gain;
                }
                sampleRate = z.clip.sampleRate > 0 ? (int)z.clip.sampleRate
                                                   : PatchKnob::app::audio_app_sample_rate();
            }
            const int loKey = clamp127(std::min(z.loKey, z.hiKey));
            const int hiKey = clamp127(std::max(z.loKey, z.hiKey));
            const int loVel = clamp127(std::min(z.loVel, z.hiVel));
            const int hiVel = clamp127(std::max(z.loVel, z.hiVel));
            const int root  = clamp127(z.root);
            const float regionSpan=std::max(1e-6f,z.end-z.start);
            const float localLs=std::max(0.f,std::min(1.f,(z.loopStart-z.start)/regionSpan));
            const float localLe=std::max(localLs,std::min(1.f,(z.loopEnd-z.start)/regionSpan));
            const int loopA=std::max(0,std::min((int)frames-1,(int)(localLs*frames)));
            const int loopB=std::max(loopA+1,std::min((int)frames,(int)(localLe*frames)));
            PatchKnob::app::audio_app_sampler_load_ex(node, slot, 0, il.data(), (int)frames, 1,
                root, sampleRate, loopA, loopB, z.loop ? 1 : 0,
                loKey, hiKey, loVel, hiVel, z.noteOffLayer ? 1 : 0,
                z.keyToPitch ? 1 : 0, z.velToVol ? 1 : 0, z.overlapMode, z.name.c_str());
            // load_ex resets the slot; re-apply the per-zone parameters so an
            // undo, a duplicate or a loop edit carries them along.
            sampler_push_zone_params(node, slot, z);
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
    // Read EVERY zone back out of the engine's own zone table into the shell's
    // UI-side g_samplerZones, replacing whatever was there.  This is what a
    // project LOAD already relied on (the instrument holds restored
    // samples/zones the shell's cache does not know about yet) -- and it is
    // exactly what a soundfont import needs too, since importPresetIntoSampler
    // loads zones straight through the engine's C API, bypassing g_samplerZones
    // entirely.  Without this the sampler editor would keep showing whatever
    // zone list it had before the import.
    // Pull ONE zone's audio out of the engine and de-interleave it for display.
    // Called for the zone actually being edited, instead of for all of them --
    // the editor shows one waveform at a time, so copying every zone's PCM up
    // front bought nothing and cost a gigabyte.
    auto sampler_fill_zone_clip = [&](int node, int zoneIndex){
        auto it = g_samplerZones.find(node);
        if (it == g_samplerZones.end()) return;
        auto& zones = it->second;
        if (zoneIndex < 0 || zoneIndex >= (int)zones.size()) return;
        ui::SamplerZone& z = zones[(size_t)zoneIndex];
        if (!z.clip.ch[0].empty()) return;                  // already resident
        PatchKnob::engine::SamplerZoneInfo info;
        if (!PatchKnob::app::audio_app_sampler_get_zone(node, zoneIndex, info)) return;
        const int ch = info.stereo ? 2 : 1;
        z.clip.sampleRate = info.sampleRate;
        z.clip.ch[0].assign((size_t)info.numFrames, 0.f);
        z.clip.ch[1].assign(info.stereo ? (size_t)info.numFrames : 0, 0.f);
        for (int f = 0; f < info.numFrames; ++f) {
            z.clip.ch[0][(size_t)f] = info.pcm[(size_t)f * ch];
            if (info.stereo) z.clip.ch[1][(size_t)f] = info.pcm[(size_t)f * ch + 1];
        }
    };

    auto rebuild_zones_from_engine = [&](int node){
        auto& zones = g_samplerZones[node];
        zones.clear();
        const int nz = PatchKnob::app::audio_app_sampler_zone_count(node);
        for (int zi = 0; zi < nz; ++zi) {
            // METADATA ONLY.  audio_app_sampler_get_zone() copies the zone's
            // PCM, and this loop then de-interleaved each copy into its own
            // AudioClip -- for a 2976-zone concert grand that is ~1.3 GB
            // allocated and copied on the UI thread after every import, while
            // the engine itself holds 85 MB because its zones SHARE buffers.
            // That is the freeze: the app is memcpy-ing a gigabyte, not working.
            // The editor only ever displays the SELECTED zone's waveform, so the
            // audio is fetched for that one zone instead (below).
            PatchKnob::engine::SamplerZoneInfo zi_info;
            if (!PatchKnob::app::audio_app_sampler_get_zone_meta(node, zi, zi_info)) continue;
            ui::SamplerZone z;
            z.root = zi_info.rootKey; z.loKey = zi_info.loKey; z.hiKey = zi_info.hiKey;
            z.loVel = zi_info.loVel; z.hiVel = zi_info.hiVel;
            z.noteOffLayer = zi_info.noteOffLayer; z.keyToPitch = zi_info.keyToPitch;
            z.velToVol = zi_info.velToVol; z.overlapMode = zi_info.overlapMode;
            z.loop = zi_info.loop; z.name = zi_info.name;
            z.loopStart = zi_info.numFrames > 0 ? (float)zi_info.loopStart / zi_info.numFrames : 0.f;
            z.loopEnd = zi_info.numFrames > 0 ? (float)zi_info.loopEnd / zi_info.numFrames : 1.f;
            // per-zone engine parameters (SF2 parity) -- restored so the
            // inspector shows them and on_apply re-pushes them faithfully.
            auto env_to_ui = [](const PatchKnob::engine::SamplerZoneEnv& s){
                ui::SamplerZoneEnvUI e;
                e.delay = s.delay; e.attack = s.attack; e.hold = s.hold; e.decay = s.decay;
                e.sustain = s.sustain; e.release = s.release; e.enabled = s.enabled;
                return e;
            };
            z.ampEnv = env_to_ui(zi_info.ampEnv);
            z.modEnv = env_to_ui(zi_info.modEnv);
            z.cutoffHz = zi_info.cutoffHz;         z.resonanceDb = zi_info.resonanceDb;
            z.coarseTune = zi_info.coarseTune;     z.fineTune = zi_info.fineTune;
            z.scaleTuning = zi_info.scaleTuning;   z.pan = zi_info.pan;
            z.attenuationDb = zi_info.attenuationDb;
            z.exclusiveClass = zi_info.exclusiveClass;
            z.modEnvToPitchCents = zi_info.modEnvToPitchCents;
            z.modEnvToFilterCents = zi_info.modEnvToFilterCents;
            // The clip stays EMPTY here; sampler_fill_zone_clip() below pulls the
            // audio for one zone on demand.  numFrames/stereo are carried in the
            // metadata, so everything that only needs the zone's SHAPE (key map,
            // zone list, loop points) works without any audio at all.
            z.clip.sampleRate = zi_info.sampleRate;
            zones.push_back(std::move(z));
        }
    };
    vPatch.on_open_sampler = [&, sampler_push_zones, sampler_push_env, sampler_push_zone_params,
                              rebuild_zones_from_engine](PatchKnob::patchbay::NodeId id){
        const int node = (int)id;
        PatchKnob::engine::IPluginInstance* inst = PatchKnob::app::audio_app_patch_node_instance(node);
        // After a project LOAD the instrument holds the restored samples/zones but the
        // shell's g_samplerZones is empty.  Rebuild it from the instrument so the editor
        // shows the zones AND on_apply re-pushes them faithfully (instead of clearing the
        // slot and wiping the restored audio).
        if (g_samplerZones[node].empty()) rebuild_zones_from_engine(node);
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
            ui::SamplerZone z; z.clip = std::move(clip);   // not used again
            z.root = key >= 0 ? key : 60;
            z.loKey = key >= 0 ? key : 0;
            z.hiKey = key >= 0 ? key : 127;
            const size_t slash = path.find_last_of("/\\");
            z.name = slash == std::string::npos ? path : path.substr(slash + 1);
            auto& zones = g_samplerZones[node];
            if (level >= 0 && level < (int)zones.size()) {
                // Replacing the audio must not destroy the existing mapping. A
                // browser drop on the zone list has no target key by design.
                // Preserve every key/velocity/playback setting and replace only
                // the source audio and its display name.
                // Assign in place: the old dance copied the WHOLE existing zone
                // (PCM included) only to overwrite the audio a line later.
                zones[(size_t)level].clip = std::move(z.clip);
                zones[(size_t)level].name = std::move(z.name);
            } else {
                zones.push_back(std::move(z));
            }
            sampler_push_zones(node);
            // Loading a zone transitions directly into the real editor; do not
            // leave the user looking at the retired waveform preview box.
            const int edited = level >= 0 && level < (int)zones.size()
                             ? level : (int)zones.size() - 1;
            if (vSamplerEd.on_edit_zone && edited >= 0) vSamplerEd.on_edit_zone(edited);
            app.request_redraw();
        };
        vSamplerEd.on_load = [&, load_sampler_path](int level){
            std::string path;
            if (choose_wav_file(app, path)) load_sampler_path(path, level, -1);
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
        // Light path for the per-zone inspector / zone envelopes: push ONE
        // zone's engine-side parameters without re-uploading any PCM, so a
        // slider drag gives live feedback.
        vSamplerEd.on_zone_params = [&, node, sampler_push_zone_params](int zi){
            auto it = g_samplerZones.find(node);
            if (it == g_samplerZones.end() || zi < 0 || zi >= (int)it->second.size()) return;
            sampler_push_zone_params(node, 1 + zi, it->second[(size_t)zi]);
        };
        // The selected keyzone is edited IN PLACE inside the sampler window.
        // There is no second sample-editor window and no parallel zone model.
        vSamplerEd.on_edit_zone = [&, node](int zoneIndex){
            auto& editableZones = g_samplerZones[node];
            // Slice expansion inserts zones after the source. Reserve the full
            // supported slice count so those inserts cannot invalidate the
            // source-zone pointer held by SamplerInstrumentSlot.
            if(editableZones.capacity()<129) editableZones.reserve(129);
            auto sync_slice_zones = [&, node, zoneIndex, sampler_push_zones] {
                auto& zs=g_samplerZones[node];
                if(zoneIndex<0||zoneIndex>=(int)zs.size())return;
                ui::SamplerZone& source=zs[(size_t)zoneIndex];
                std::vector<float> cuts=source.slices;
                cuts.erase(std::remove_if(cuts.begin(),cuts.end(),[](float v){return v<=0.f||v>=1.f;}),cuts.end());
                std::sort(cuts.begin(),cuts.end());
                cuts.erase(std::unique(cuts.begin(),cuts.end(),[](float a,float b){return std::fabs(a-b)<1e-5f;}),cuts.end());
                source.slices=cuts;

                // Remove only zones previously generated by this immediately
                // preceding source. Manual zones farther down remain untouched.
                size_t at=(size_t)zoneIndex+1;
                std::vector<ui::SamplerZone> previousParts;
                while(at<zs.size()&&zs[at].sliceGenerated) {
                    previousParts.push_back(zs[at]);
                    zs.erase(zs.begin()+(ptrdiff_t)at);
                }

                if(!cuts.empty()) {
                    const ui::SamplerZone base=source;
                    source.sliceGenerated=false;source.sliceOrdinal=0;
                    source.start=0.f;source.end=cuts[0];
                    source.loKey=source.hiKey=source.root;
                    for(size_t i=0;i<cuts.size();++i) {
                        ui::SamplerZone part=base;
                        part.sliceGenerated=true;part.sliceOrdinal=(int)i+1;
                        part.slices.clear();
                        part.start=cuts[i];
                        part.end=i+1<cuts.size()?cuts[i+1]:1.f;
                        // Marker edits rebuild region boundaries, not musical
                        // mapping. Preserve each slice's independently edited
                        // root/key/velocity values by ordinal.
                        if(i<previousParts.size()) {
                            part.root=previousParts[i].root;
                            part.loKey=previousParts[i].loKey;part.hiKey=previousParts[i].hiKey;
                            part.loVel=previousParts[i].loVel;part.hiVel=previousParts[i].hiVel;
                        } else {
                            part.root=std::min(127,base.root+(int)i+1);
                            part.loKey=part.hiKey=part.root;
                        }
                        part.name=base.name+" [slice "+std::to_string(i+2)+"]";
                        zs.insert(zs.begin()+(ptrdiff_t)(at+i),std::move(part));
                    }
                } else if(source.sliceGenerated==false) {
                    source.start=0.f;source.end=1.f;
                }
                sampler_push_zones(node);app.request_redraw();
            };
            if (zoneIndex < 0 || zoneIndex >= (int)editableZones.size() ||
                !vInstrSlot.bind(node, zoneIndex, &editableZones[(size_t)zoneIndex],
                                 sync_slice_zones)) {
                projectStatus = "That zone has no sample to edit";
                return;
            }
            vSamplerEd.edit_zone_inline(&vInstrSlot,
                "Sampler zone " + std::to_string(zoneIndex + 1),
                PatchKnob::app::audio_app_sample_rate());
            vSamplerEd.inline_editor().on_status = [&](const std::string& msg){ projectStatus = msg; };
            vSamplerEd.inline_editor().on_pick_save_path = [&](std::string& p)->bool {
                std::string out = g_lastSampleDir;
                if (!choose_editor_file(app, true, out, "Save Sample As", "wav", out)) return false;
                p = out; return true;
            };
            using CA = sampleslot::SampleSlotEditor::ContextAction;
            auto zone_action = [&, node, zoneIndex, sampler_push_zones](auto fn) {
                return [&, node, zoneIndex, sampler_push_zones, fn] {
                    auto& zs = g_samplerZones[node];
                    if (zoneIndex < 0 || zoneIndex >= (int)zs.size()) return;
                    fn(zs[(size_t)zoneIndex], zs);
                    sampler_push_zones(node); app.request_redraw();
                };
            };
            vSamplerEd.inline_editor().set_context_actions({
                CA{"Reverse playback", zone_action([](ui::SamplerZone& z, auto&){ z.reverse=!z.reverse; })},
                CA{"Note-on layer", zone_action([](ui::SamplerZone& z, auto&){ z.noteOffLayer=false; })},
                CA{"Note-off layer", zone_action([](ui::SamplerZone& z, auto&){ z.noteOffLayer=true; })},
                CA{"Key follows pitch", zone_action([](ui::SamplerZone& z, auto&){ z.keyToPitch=true; })},
                CA{"Fixed pitch", zone_action([](ui::SamplerZone& z, auto&){ z.keyToPitch=false; })},
                CA{"Velocity controls volume", zone_action([](ui::SamplerZone& z, auto&){ z.velToVol=true; })},
                CA{"Fixed volume", zone_action([](ui::SamplerZone& z, auto&){ z.velToVol=false; })},
                CA{"Overlap: play all", zone_action([](ui::SamplerZone& z, auto&){ z.overlapMode=0; })},
                CA{"Overlap: cycle", zone_action([](ui::SamplerZone& z, auto&){ z.overlapMode=1; })},
                CA{"Overlap: random", zone_action([](ui::SamplerZone& z, auto&){ z.overlapMode=2; })},
                CA{"Distribute zones across keys", zone_action([](ui::SamplerZone&, auto& zs){
                    const int n=(int)zs.size(); for(int i=0;i<n;++i){zs[(size_t)i].loKey=(i*128)/n; zs[(size_t)i].hiKey=((i+1)*128)/n-1;}
                })},
                CA{"Layer zones by velocity", zone_action([](ui::SamplerZone&, auto& zs){
                    const int n=(int)zs.size(); for(int i=0;i<n;++i){zs[(size_t)i].loVel=(i*128)/n; zs[(size_t)i].hiVel=((i+1)*128)/n-1;}
                })}
            });
            vSamplerEd.on_preview_path = [&](const std::string& path, PatchKnob::engine::AudioClip& clip)->bool {
                std::string err; return PatchKnob::engine::loadWav(path,
                    PatchKnob::app::audio_app_sample_rate(), clip, &err);
            };
            app.request_redraw();
        };
        vSamplerEd.on_zone_selected = vSamplerEd.on_edit_zone;
        // Apply the shell's current env state -- but NOT when the instrument already has
        // restored envelopes and the shell copy is still empty (would wipe the restore).
        if (!restoredEnv)
            for (int ev = 0; ev < ui::ENV_COUNT; ++ev) sampler_push_env(node, ev);
        // A populated sampler opens on the full editor. KEYZONES is a sibling
        // workspace reached with the toolbar tab, not the default/basic view.
        if (!g_samplerZones[node].empty() && vSamplerEd.on_edit_zone)
            vSamplerEd.on_edit_zone(std::max(0, vSamplerEd.selected_zone()));
        open_window(samplerWin, 1000, 700);
    };
    // Asynchronous soundfont import (see import_sf2_preset below for the why).
    // sf2ImportNode/Token identify the ONE pending request: node is read at
    // click time because the user may move the sampler editor to a different
    // instrument before the prepare finishes, and the zones must still land in
    // the instrument they were requested FOR (the editor is only rebound if it
    // still shows that node).  Token != 0 means "a request is in flight", which
    // keeps on_frame repainting (live progress) until the result is taken.
    PatchKnob::engine::sf2::Sf2ImportService sf2Import;
    int      sf2ImportNode  = -1;
    uint64_t sf2ImportToken = 0;
    // Drag routing: an Sf2Preset dragged out of the disk browser (mounted in
    // the "Sample Editor" window, sampleslot::SampleSlotPanel) and released
    // over the "Sampler" window imports that ONE preset's zones into whichever
    // native Sampler instrument is currently bound there.  Both windows are
    // panes inside the SAME SDL_Window (WindowManager::m_active keeps routing
    // every event of the gesture to the browser regardless of where the
    // pointer ends up), so (x,y) here is in the one shared logical coordinate
    // space and can be hit-tested against samplerWin directly.
    // ONE import path, two ways in: a preset dragged out of the Sample Slot
    // browser, and a preset clicked in the Sampler editor's OWN browser.  There
    // are two distinct browser widgets in the app, and having only one of them
    // understand soundfonts is exactly the bug this consolidates away.
    //
    // The import is ASYNCHRONOUS.  Preparing a preset (headers + PCM decode +
    // interning) is up to ~600 ms of disk on a cold 500 MB bank -- the
    // sequential-read floor of the device, unfixable synchronously -- so
    // Sf2ImportService runs it on a worker thread and this lambda only files
    // the request.  Completion lands in on_frame's pump (below, search
    // "sf2Import.take"), which installs the prepared zones on the message
    // thread, the only thread allowed to touch sampler setters.  begin()
    // SUPERSEDES any import still in flight: clicking down a preset list
    // auditions the LAST click, and the service guarantees a stale prepare is
    // abandoned, never delivered, so its zones cannot land after the user has
    // moved on.
    auto import_sf2_preset = [&](const std::string& sf2Path, int bank, int program,
                                 const std::string& label){
        const int node = vSamplerEd.node();
        PatchKnob::engine::IPluginInstance* inst =
            node >= 0 ? PatchKnob::app::audio_app_patch_node_instance(node) : nullptr;
        if (node < 0 || !inst) {
            projectStatus = "Open a Sampler instrument before dropping a preset";
            app.request_redraw();
            return;
        }
        sf2ImportNode  = node;   // which instrument the result installs into
        sf2ImportToken = sf2Import.begin(sf2Path, bank, program, label);
        projectStatus = "Importing \"" + label + "\"...";
        app.request_redraw();
    };
    // (a) dragged out of the Sample Slot browser and dropped on the Sampler.
    vSampleSlot.on_preset_drop = [&](const sampleslot::DragPayload& payload, int x, int y){
        if (payload.kind != sampleslot::DragPayload::Sf2Preset) return;
        if (!samplerWin.visible || samplerWin.minimized || !samplerWin.hit(x, y)) {
            projectStatus = "Drop a soundfont preset on the Sampler window to import it";
            app.request_redraw();
            return;
        }
        import_sf2_preset(payload.path, payload.bank, payload.program, payload.label);
    };
    // (b) clicked in the Sampler editor's own sample library -- no drop needed,
    // that browser is already inside the sampler it would import into.
    vSamplerEd.on_load_sf2_preset = [&](const std::string& sf2Path, int bank, int program){
        char lbl[64]; std::snprintf(lbl, sizeof lbl, "[%03d:%03d]", bank, program);
        import_sf2_preset(sf2Path, bank, program, lbl);
    };
    // Right-click a Rack node -> choose its MIDI polyphony (1..16 voices).
    vPatch.on_set_rack_poly = [&](PatchKnob::patchbay::NodeId id, int voices){
        PatchKnob::app::audio_app_rack_set_poly((int)id, voices);
        app.request_redraw();
    };
    vArrange.on_open_editor = [&](int seq, int kind){
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (!s) return;
        if (kind==2) {
            const int destination = g_seqToTrack.count(seq) ? g_seqToTrack[seq] : -1;
            auto& region=autoPlayer.ensureRegion(seq);
            region.destinationTrack=destination;
            if(region.length<1)region.length=std::max<long>(1,s->get_length());
            if(region.loopLength<1)region.loopLength=std::max<long>(1,s->get_length());
            vAuto.set_region(s,seq,destination); vAuto.refresh_params();
            automationWin.title=std::string("Automation: ")+(s->get_name()?s->get_name():"clip");
            automationWin.visible=true; wm.raise(&automationWin);
        }
        else if (kind==1) {
                       auto& region=autoPlayer.ensureRegion(seq);
                       if(!region.trackerFx.empty())s->set_fx_blob(region.trackerFx);
                       vTracker.set_sequence(s, s->get_midi_bus()); trackerWin.title="Tracker: "+instr_current(s);
                       trackerWin.visible=true; wm.raise(&trackerWin); }
        else         { vPiano.set_sequence(s); pianoWin.title="Piano Roll: "+instr_current(s);
                       pianoWin.visible=true; wm.raise(&pianoWin); }
        app.request_redraw();
    };

    // Track-header instrument dropdown: list every instrument node, show/assign
    // the one this track drives (by the sequence's MIDI channel).  Replaces the
    // removed per-clip instrument strip -- an instrument owns a mixer channel.
    vArrange.on_list_instruments = [&]{
        std::vector<std::string> v{"(none)"};
        for (const auto& p : instrument_nodes()) v.push_back(p.second);
        return v;
    };
    vArrange.on_track_instrument = [&](int seq) -> std::string {
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        return s ? instr_current(s) : std::string("-");
    };
    vArrange.on_pick_instrument = [&](int seq, int idx){
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if(!s)return;
        if(idx==0) { g_arrangeInstrument.erase(seq); rebuild_track_midi_routes();
                     app.request_redraw(); return; }
        instr_pick(s, idx-1);
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
            if(!plabel.empty()&&plabel[0]=='_') continue;
            int portId=i;
            if(eng==PatchKnob::app::audio_app_virtual_midi_node()) {
                const int ins=PatchKnob::app::audio_app_virtual_midi_inputs();
                const int outs=PatchKnob::app::audio_app_virtual_midi_outputs();
                if(i>=ins&&i<ins+outs) portId=100+(i-ins);
            }
            pb::Port p((pb::PortId)portId, pk, plabel);
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
            if(!plabel.empty()&&plabel[0]=='_') continue;
            int portId=i;
            if(eng==PatchKnob::app::audio_app_virtual_midi_node()) {
                const int ins=PatchKnob::app::audio_app_virtual_midi_inputs();
                const int outs=PatchKnob::app::audio_app_virtual_midi_outputs();
                if(i>=ins&&i<ins+outs) portId=100+(i-ins);
            }
            pb::Port p((pb::PortId)portId, pk, plabel);
            if(d==0){ n->inPorts.push_back(p); validIn.insert((pb::PortId)portId); }
            else    { n->outPorts.push_back(p); validOut.insert((pb::PortId)portId); }
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
    vPatch.on_set_midi_ports = [&](PatchKnob::patchbay::NodeId id, int ports) {
        if (PatchKnob::app::audio_app_patch_set_virtual_midi_ports((int)id, ports))
            resync_patch_node((int)id);
    };
    virtualMidiConfig.set_counts = [&](int ins,int outs) {
        PatchKnob::app::audio_app_virtual_midi_set_ports(ins,outs);
        resync_patch_node(PatchKnob::app::audio_app_virtual_midi_node());
    };
    vPatch.on_config_virtual_midi = [&](PatchKnob::patchbay::NodeId) {
        open_window(virtualMidiWin, 520, 170);
    };
    // --- Csound CSD editor: right-click a Csound node -> "Edit CSD" ------------
    vPatch.on_open_csound = [&](PatchKnob::patchbay::NodeId id){
        const int node = (int)id;
        g_csoundNode = node; g_csoundRackMod = -1;    // patch-node target (not a rack module)
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
        if (g_csoundRackMod >= 0) {                   // recompile a Csound RACK module
            if (auto* eng = PatchKnob::app::audio_app_rack_engine(g_rackNode)) {
                const bool ok = eng->setModuleScript(g_csoundRackMod, csoundEditor.text());
                const std::string err = eng->moduleScriptError(g_csoundRackMod);
                csoundEditor.setStatus(ok ? (err.empty() ? std::string("compiled OK") : err)
                                          : (err.empty() ? std::string("compile error") : err), !ok);
            }
            app.request_redraw(); return;
        }
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
#ifdef PATCHKNOB_HAS_AI
    // ---- Claude chat wiring ----------------------------------------------
    // The panel's editor-side hooks (live buffer, compile errors, apply-
    // through-undo, manual dir) are wired inside CsoundEditorView::aiChat();
    // the shell only supplies the settings window and the per-frame pump
    // (App::on_frame, below -- search "pumpAi").
    csoundEditor.aiChat().open_settings = [&]{ open_window(aiSettingsWin, 520, 560); };
    aiSettings.on_changed = [&]{
        csoundEditor.aiChat().reloadSettings();
        app.request_redraw();
    };
#endif
    vPatch.virtual_midi_routes = [&](PatchKnob::patchbay::NodeId id) {
        std::vector<std::pair<int,int>> routes;
        if((int)id!=PatchKnob::app::audio_app_virtual_midi_node()) return routes;
        const int n=PatchKnob::app::audio_app_virtual_midi_outputs();
        for(int out=0;out<n;++out) {
            const int in=PatchKnob::app::audio_app_virtual_midi_route(out);
            if(in>=0) routes.push_back({in,out});
        }
        return routes;
    };
    if (audio_ok) {
        // Seed the canvas with the always-present MIDI-in source, audio-out sink,
        // and the singleton Master Mixer module (8 buses + master + MIDI clock out).
        pv_mirror_node(PatchKnob::app::audio_app_patch_midi_in_node(), "Instrument out", "MIDI",  40, 70);
        pv_mirror_node(PatchKnob::app::audio_app_default_hw_midi_in_node(), "Hardware MIDI In", "MIDI", 40, 190);
        pv_mirror_node(PatchKnob::app::audio_app_patch_out_node(),     "Audio Out", "Audio I/O", 620, 70);
        pv_mirror_node(PatchKnob::app::audio_app_master_mixer_node(),  "Master Mixer", "Mixer",  620, 180);
        pv_mirror_node(PatchKnob::app::audio_app_virtual_midi_node(), "Instrument Virtual MIDI Ports", "MIDI", 430, 180);
    }
    // Arrange +circle -> add a track (0=instrument,1=audio): create the sequence
    // AND auto-create its labeled module wired into the master mixer.
    auto refresh_master_ui = [&]{
        resync_patch_node(PatchKnob::app::audio_app_master_mixer_node());
        // the sequencer MidiIn node grows an "Instrument out" plug per track in
        // lockstep with the mixer -- re-mirror it so the new plugs show on canvas.
        resync_patch_node(PatchKnob::app::audio_app_patch_midi_in_node());
        resync_patch_node(PatchKnob::app::audio_app_default_hw_midi_in_node());
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
        resync_patch_node(PatchKnob::app::audio_app_default_hw_midi_in_node());
        if (g_curMixerNode == PatchKnob::app::audio_app_master_mixer_node())
            mixerStrip.bus_count = PatchKnob::app::audio_app_mixer_channels(g_curMixerNode);
        // The per-track MidiTrackNode is deliberately INVISIBLE in the patcher:
        // the node is skipped when mirroring and its ports are "_"-prefixed.  But
        // that is exactly what the track dropdown wires THROUGH --
        //     "Instrument N" -> trackNode:_playback in
        //     trackNode:_track out -> instrument:midi in
        // so both hops referenced a node the UI does not have and NOTHING was
        // drawn: picking an instrument silently produced no visible cable.
        // Splice each hidden hop out and publish the direct wire the user means.
        std::set<int> hidden;
        for (const auto id : graph->nodeIds()) {
            const auto* nd = graph->node(id);
            if (nd && std::string(nd->typeName()) == "MidiTrackNode")
                hidden.insert((int)id);
        }
        typedef std::pair<int,int> PortPt;                  // (node, port)
        std::map<int, std::vector<PortPt>> intoHop, outOfHop;
        std::map<int, std::vector<int>>    hopToHop;        // src hop -> dst hops
        std::vector<pb::Connection> conns;
        for (const auto& c : graph->connections()) {
            const int fn=(int)c.from.node, fp=(int)c.from.port;
            const int tn=(int)c.to.node,   tp=(int)c.to.port;
            const bool fHid = hidden.count(fn) != 0, tHid = hidden.count(tn) != 0;
            if      (!fHid && !tHid) conns.emplace_back(
                                        pb::PortRef((pb::NodeId)fn,(pb::PortId)fp),
                                        pb::PortRef((pb::NodeId)tn,(pb::PortId)tp));
            // Only splice a source into the hop's "_playback in" (port 1) --
            // that is the "Instrument N" -> trackNode -> instrument chain the
            // comment above is about, and its direct (non-hop) twin already
            // exists as a real, visible edge (audio_app_master_connect_
            // instrument wires the sequencer straight to the instrument).
            // A source feeding "_live in" (port 0) -- e.g. a virtual/hardware
            // MIDI input the user wired to this track's selected input -- must
            // NOT also be spliced through to the instrument: the track already
            // routes it there internally, so materializing a second "Hardware
            // MIDI In -> Instrument" cable on top of the cable the user
            // actually drew (source -> virtual port) is a redundant, confusing
            // duplicate, not a second real connection.
            else if (!fHid &&  tHid) { if (tp == 1) intoHop[tn].push_back(PortPt(fn,fp)); }
            else if ( fHid && !tHid) outOfHop[fn].push_back(PortPt(tn,tp));
            else                     hopToHop[fn].push_back(tn);   // track -> track
        }
        // Real sources feeding a hop, following hop->hop chains (bounded by the
        // visited set, so a cycle cannot spin here).
        std::function<void(int,std::set<int>&,std::vector<PortPt>&)> gather_src =
            [&](int hop, std::set<int>& seen, std::vector<PortPt>& out) {
                if (!seen.insert(hop).second) return;
                auto d = intoHop.find(hop);
                if (d != intoHop.end()) out.insert(out.end(), d->second.begin(), d->second.end());
                for (const auto& kv : hopToHop)
                    for (int dst : kv.second)
                        if (dst == hop) gather_src(kv.first, seen, out);
            };
        for (const auto& h : outOfHop) {
            std::set<int> seen;
            std::vector<PortPt> srcs;
            gather_src(h.first, seen, srcs);
            for (const auto& s : srcs)
                for (const auto& d : h.second)
                    conns.emplace_back(pb::PortRef((pb::NodeId)s.first,(pb::PortId)s.second),
                                       pb::PortRef((pb::NodeId)d.first,(pb::PortId)d.second));
        }
        vPatch.set_connections(conns);
        app.request_redraw();
    };
    refresh_loaded_project = [&]{
        // LOOP is part of the project file (perform saves looping + left/right),
        // but g_loopOn is the shell's own mirror that drives the toolbar lamp
        // and the rewind/ff clamps.  Re-derive it here -- the one place every
        // successful load funnels through -- so it describes the song that is
        // actually loaded rather than the one that was open before.
        g_loopOn = perf.get_looping();
        vPatch.set_nodes({});
        vPatch.set_connections({});
        g_seqToTrack.clear();
        g_seqRegion.clear();
        g_midiTrackNode.clear();
        g_autoMidiEdges.clear();
        g_arrangeRecInput.clear(); g_arrangeMidiOut.clear(); g_arrangeInstrument.clear();
        g_arrangeAudioIn.clear();  g_arrangeAudioOut.clear();
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
                if(type=="MidiTrackNode") continue;
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
                } else if (type == "VirtualMidiPortsNode") {
                    name = "Instrument Virtual MIDI Ports"; category = "MIDI";
                } else if (type == "MixerNode" || type == "MasterMixerNode") {
                    name = type == "MasterMixerNode" ? "Master Mixer" : "Mixer";
                    category = "Mixer";
                } else if (type == "MidiInNode" || type == "MidiOutNode") {
                    // The singleton sequencer MidiIn node carries the per-track
                    // "Instrument out" plugs; added hardware MIDI-in nodes stay "MIDI In".
                    if (type == "MidiInNode")
                        name = ((int)id == PatchKnob::app::audio_app_patch_midi_in_node())
                                   ? "Instrument out"
                                   : ((int)id == PatchKnob::app::audio_app_default_hw_midi_in_node()
                                      ? "Hardware MIDI In" : "MIDI In");
                    else name = "MIDI Out";
                    category = "MIDI";
                } else if (type == "AudioDeviceInNode" || type == "AudioDeviceOutNode") {
                    name = type == "AudioDeviceInNode" ? "Audio In" : "Audio Out";
                    category = "Audio I/O";
                } else if (type == "SineSourceNode") name = "Sine";
                else if (type == "GainNode") name = "Gain";
                else if (type == "SumNode") name = "Sum";

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
        // usually fewer tracks than sequences). A node feeding master-mixer inlet
        // (2 + 2*t) owns track t. The sequence's saved MIDI bus is that exact
        // track identity; MIDI channel is deliberately not used as ownership.
        g_instrTrack.clear();
        if (auto* graph = PatchKnob::app::audio_app_patch_graph()) {
            const int mm = PatchKnob::app::audio_app_master_mixer_node();
            for (const auto& c : graph->connections())
                if ((int)c.to.node == mm && (int)c.to.port >= 2)
                    g_instrTrack[(int)c.from.node] = ((int)c.to.port - 2) / 2;
        }
        for(const auto& owner:g_instrTrack) make_instr_graph_routed(owner.first);
        for (int si = 0; si < c_max_sequence; ++si) {
            if (!perf.is_active(si)) continue;
            sequence* s = perf.get_sequence(si);
            if (!s) continue;
            const int saved=(unsigned char)s->get_midi_bus();
            int mapped=-1;
            for(const auto& kv:g_instrTrack)
                if(kv.second==saved) { mapped=saved; break; }
            if (mapped >= 0) { g_seqToTrack[si] = mapped; s->set_midi_bus((char)mapped); }
        }
        // Recover track I/O selections from the saved graph cables before the
        // deterministic rebuild. This keeps dropdown status/routes stable
        // across project load without a second competing serialization format.
        if(auto* graph=PatchKnob::app::audio_app_patch_graph()) {
            const auto conns=graph->connections();
            for(const auto& owner:g_instrTrack) for(const auto& c:conns)
                if((int)c.to.node==owner.first) {
                    const auto* n=graph->node(c.from.node);
                    if(n&&std::string(n->typeName())=="MidiTrackNode")
                        g_midiTrackNode[owner.second]=(int)c.from.node;
                }
            for(const auto& kv:g_seqToTrack) {
                const int si=kv.first,track=kv.second;
                const auto tn=g_midiTrackNode.find(track);if(tn==g_midiTrackNode.end())continue;
                g_arrangeRecInput[si]=PatchKnob::app::audio_app_midi_track_input(tn->second);
                g_arrangeMidiOut[si]=PatchKnob::app::audio_app_midi_track_output(tn->second);
                for(const auto& c:conns)
                    if((int)c.from.node==tn->second&&(int)c.from.port==2&&
                       PatchKnob::app::audio_app_patch_node_is_instrument((int)c.to.node)) {
                        g_arrangeInstrument[si]=(int)c.to.node;
                        break;
                    }
            }
        }
        rebuild_track_midi_routes();
        if (apply_loaded_freezes) apply_loaded_freezes();   // re-show frozen lanes
        if (rebind_loaded_audio_lanes) rebind_loaded_audio_lanes();
        refresh_master_ui();
    };

    // --- live MIDI record -> a new clip on the timeline --------------------
    // Captured events accumulate here while recording; committed as a new
    // sequence + a trigger at the record position when recording stops.
    struct RecNote { long start, end; int pitch, vel; };
    struct RecEvent { long tick; unsigned char status,d1,d2; };
    static std::vector<RecNote> g_recNotes;           // completed notes (on+off paired)
    static std::vector<RecEvent> g_recEvents;         // CC/program/bend/pressure
    static std::map<int,RecNote> g_recPending;        // pitch -> open note (end filled on off)
    static long g_recMaxTick = 0;
    static bool g_audioTrackCapturing = false;
    // The armed lane was an AUDIO lane when this take started.
    static bool g_recTargetWasAudio = false;
    // Correction applied to incoming capture timestamps when the armed origin
    // turns out to be later than what was actually played (see drain_record).
    static PatchKnob::engine::AudioClip g_recordPreviewAudio;
    // Captured timestamps are now the time the key actually went down (the
    // hardware path undoes its one-block scheduling lookahead at the capture
    // site), so quantisation needs NO latency fudge here.  The fudge that used
    // to live here subtracted a buffer on top of a timestamp that was already a
    // buffer EARLY, which is what pushed notes onto the previous grid line.
    // Snap one absolute tick, via THE shared quantiser (src/quantize.h) that the
    // offline pattern quantise and the piano-roll grid snap also use.
    //
    // This used to floor to the PREVIOUS line, which is what made record
    // quantise feel rigid: a note played a hair AHEAD of the beat is nearest the
    // NEXT line, but flooring dragged it back a whole grid step -- the "it moved
    // my notes to the wrong 16th" symptom.  quantize::apply() rounds to the
    // nearest line, and `threshold` leaves playing that is already tight exactly
    // as performed instead of nailing every note to the grid.
    auto record_quant_params = [&] {
        PatchKnob::quantize::Params p;
        p.grid      = recordQuantizeTicks;
        p.strength  = recordQuantizeStrength;
        // Percent of the half-grid (the largest deviation there can be).
        p.threshold = (long)((double)(recordQuantizeTicks / 2)
                             * (double)recordQuantizeRangePct / 100.0);
        p.swing     = (float)recordQuantizeSwingPct / 100.0f;
        return p;
    };
    auto snap_absolute = [&](long absolute) {
        return PatchKnob::quantize::apply(absolute, record_quant_params());
    };
    // Notes struck together are ONE musical event.  Quantising each note on its
    // own splits a chord whenever its notes straddle a grid line -- half land on
    // one 16th, half on the next, which is exactly the "sometimes it moves them
    // to a wrong 16th" symptom: a chord played a hair before the line has some
    // notes rounded forward and some back.  Group near-simultaneous starts, pick
    // ONE line for the group from its mean, and move the whole chord there.
    auto chord_window_ticks = [&]() -> long {
        // Chord grouping is only for genuinely simultaneous strikes.  The old
        // 32nd-note window (62.5 ms at 120 BPM) merged fast melodies, rolls and
        // repeated 128ths into one chord and quantized them onto one tick.
        const long byGrid = recordQuantizeTicks > 0 ? recordQuantizeTicks / 8 : 0;
        const long absolute = std::max<long>(1, c_ppqn / 48); // ~10 ms at 120 BPM
        return std::max<long>(1, std::min(byGrid > 0 ? byGrid : absolute, absolute));
    };
    // Quantise a whole take at once: returns the new start for each note, index
    // for index with `notes`.
    auto quantize_take = [&](const std::vector<RecNote>& notes) {
        std::vector<long> out(notes.size());
        for (size_t i = 0; i < notes.size(); ++i) out[i] = std::max<long>(0, notes[i].start);
        if (recordQuantizeTicks <= 0 || notes.empty()) return out;
        // Work in SONG-absolute ticks: snapping clip-local values would shift the
        // grid whenever a take starts away from tick zero.
        std::vector<size_t> order(notes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return notes[a].start < notes[b].start;
        });
        const long window = chord_window_ticks();
        size_t i = 0;
        while (i < order.size()) {
            size_t j = i + 1;
            const long first = notes[order[i]].start;
            double sum = (double)first;
            while (j < order.size() && notes[order[j]].start - first <= window) {
                sum += (double)notes[order[j]].start; ++j;
            }
            const long mean = (long)std::llround(sum / (double)(j - i));
            const auto  qp   = record_quant_params();
            const long  absMean = g_recordTimelineStart + mean;
            // Q-Range: a chord that is ALREADY tight enough must be left exactly
            // as played.  quantize::apply says so by returning the input
            // unchanged -- but flattening the group onto that returned value
            // still moved every note of the chord onto the group MEAN, which is
            // the micro-timing Q-Range exists to protect.  Detect the in-range
            // case explicitly (apply()'s own test) and leave the notes alone.
            const bool inRange =
                qp.threshold > 0 &&
                std::labs(PatchKnob::quantize::nearest_line(absMean, qp) - absMean)
                    < qp.threshold;
            if (!inRange) {
                const long target = snap_absolute(absMean) - g_recordTimelineStart;
                for (size_t k = i; k < j; ++k) out[order[k]] = std::max<long>(0, target);
            }
            i = j;
        }
        return out;
    };
    // Single-note helper, kept for the live preview where notes stream in one at
    // a time and there is no complete take to group yet.
    auto quantize_record_tick = [&](long relativeTick) {
        if (recordQuantizeTicks <= 0) return std::max<long>(0, relativeTick);
        return std::max<long>(0, snap_absolute(g_recordTimelineStart + relativeTick)
                                 - g_recordTimelineStart);
    };
    // Ardour Playlist::partition semantics for PatchKnob's trigger playlists.
    // Recording never edits a source pattern.  It removes the punch interval
    // from every region on the lane, preserving left/right children and their
    // source offsets, before inserting the newly captured region.
    auto partition_lane = [&](int laneSeq, long cutStart, long cutEnd) {
        if (laneSeq < 0 || cutEnd <= cutStart) return;
        const int key = vArrange.on_track_key ? vArrange.on_track_key(laneSeq) : laneSeq;
        for (int si = 0; si < c_max_sequence; ++si) {
            if (!perf.is_active(si)) continue;
            const int otherKey = vArrange.on_track_key ? vArrange.on_track_key(si) : si;
            if (otherKey != key) continue;
            sequence* old = perf.get_sequence(si);
            if (!old) continue;
            struct Part { long pos, len, offset; };
            std::vector<Part> parts;
            long on=0, off=0, offset=0; bool selected=false;
            old->reset_draw_trigger_marker();
            while (old->get_next_trigger(&on,&off,&selected,&offset)) {
                const long end = off + 1;
                if (end <= cutStart || on >= cutEnd) {
                    parts.push_back({on,end-on,offset});
                    continue;
                }
                if (on < cutStart)
                    parts.push_back({on,cutStart-on,offset});
                if (end > cutEnd)
                    parts.push_back({cutEnd,end-cutEnd,offset+(cutEnd-on)});
            }
            old->clear_triggers();
            for (const Part& p : parts)
                if (p.len > 0) old->add_trigger(p.pos,p.len,p.offset,false);
            old->set_dirty();
        }
    };
    drain_record = [&]{
        long tk[256]; unsigned char st[256], d1[256], d2[256];
        int n=0;
        do {
          n = g_arrangeRecTarget >= 0
            ? PatchKnob::app::audio_app_midi_track_drain(
                midi_track_for_seq(g_arrangeRecTarget),tk,st,d1,d2,256):0;
          for (int i=0;i<n;++i){
            // The track recorder returns one canonical ABSOLUTE transport tick.
            // Convert to take-local time exactly once, here at the UI boundary;
            // EVERY tick past this point (g_recNotes, g_recEvents, g_recMaxTick,
            // the preview, the quantiser's input) is take-relative, and only
            // g_recordTimelineStart converts back.
            //
            // Anything stamped before the origin is pinned to 0 rather than
            // dropped.  That is a compromise, not a design: it can only happen
            // for the handful of events that slip in between capture opening and
            // the transport settling on the punch point, and losing a note
            // outright is worse than one landing a few ticks early.  (An earlier
            // version claimed to RE-ANCHOR the whole take on such an event.  It
            // never did -- and it must not, because the clip placement is
            // already committed to this origin.)
            long rel = tk[i] - g_recordTimelineStart;
            if(rel<0)rel=0;
            tk[i] = rel;
            unsigned char hi = st[i] & 0xF0; int pitch = d1[i];
            const int key = ((int)st[i] & 0x0F) * 128 + pitch;
            if (tk[i] > g_recMaxTick) g_recMaxTick = tk[i];
            if (hi==0x90 && d2[i]>0) {
                // A retrigger closes the preceding voice at this event.  Merely
                // overwriting the map entry loses its note-off and leaves stale
                // MIDI in the following take.
                auto prior = g_recPending.find(key);
                if (prior != g_recPending.end()) {
                    prior->second.end = std::max(prior->second.start + 1, tk[i]);
                    g_recNotes.push_back(prior->second);
                }
                g_recPending[key] = RecNote{ tk[i], tk[i], pitch, d2[i] };
            }
            else if (hi==0x80 || (hi==0x90 && d2[i]==0)) {
                auto it = g_recPending.find(key);
                if (it != g_recPending.end()) { it->second.end = tk[i];
                    g_recNotes.push_back(it->second); g_recPending.erase(it); }
            } else if(hi>=0xA0&&hi<=0xE0) {
                g_recEvents.push_back(RecEvent{tk[i],st[i],d1[i],d2[i]});
            }
          }
        } while(n==256);
        static Uint32 lastPreviewMs=0;
        const Uint32 nowPreviewMs=SDL_GetTicks();
        if(g_arrangeRecTarget>=0&&nowPreviewMs-lastPreviewMs>=50) {
            lastPreviewMs=nowPreviewMs;
            const long start=g_recordTimelineStart;
            const long nowTick=(long)PatchKnob::app::audio_app_sample_to_tick(
                PatchKnob::app::audio_app_transport_effective_sample());
            // The growing preview must never read BACKWARDS from its own origin:
            // a locate during a take (or a loop wrap) would otherwise produce a
            // negative span and a preview block drawn behind the record point.
            // Clamping alone left it one tick wide for the rest of a looping
            // take -- the block collapsed the moment the transport wrapped, and
            // every still-held note in it drew as a sliver.  What has actually
            // been captured (g_recMaxTick, same take-relative clock) is the
            // floor the preview can never fall below.
            const long len=std::max<long>(std::max<long>(1,g_recMaxTick),
                                          nowTick-start);
            sequence* lane=perf.is_active(g_arrangeRecTarget)
                ?perf.get_sequence(g_arrangeRecTarget):nullptr;
            const std::string name=lane&&lane->get_name()?lane->get_name():"";
            if(name.compare(0,6,"Audio ")==0&&g_audioTrackCapturing) {
                if(PatchKnob::app::audio_app_track_capture_preview(
                       g_recordPreviewAudio,4096))
                    vArrange.set_record_preview(g_arrangeRecTarget,start,len,{},
                                                &g_recordPreviewAudio);
            } else {
                std::vector<arrange::ArrangeView::RecordPreviewNote> preview;
                // Preview the take the SAME way the commit will quantise it.
                // Per-note snapping here vs chord-grouped snapping at commit
                // meant the blocks you watched while playing were not where the
                // notes actually landed -- a chord straddling a grid line drew
                // split and then committed flat.
                const std::vector<long> qs=quantize_take(g_recNotes);
                for(size_t ri=0;ri<g_recNotes.size();++ri) {
                    const auto& r=g_recNotes[ri];
                    preview.push_back({qs[ri],qs[ri]+std::max<long>(1,r.end-r.start),r.pitch});
                }
                // Still-held notes have no end yet, so there is no complete take
                // to group them with: per-note snapping is all that is available
                // until the key comes up.
                for(const auto& p:g_recPending) {
                    const long qp=quantize_record_tick(p.second.start);
                    preview.push_back({qp,std::max(qp+1,len),p.second.pitch});
                }
                vArrange.set_record_preview(g_arrangeRecTarget,start,len,preview,nullptr);
            }
            app.request_redraw();
        }
    };
    auto commit_recording = [&]{
        drain_record();                               // flush any tail
        // Ardour's CaptureInfo duration is the transport interval, not the
        // timestamp of the last MIDI event.  This keeps empty tails, held-note
        // closure, and the region end at the actual punch-out position.
        // effective_sample() so a punch-out that coincides with a locate measures
        // the span that was actually played, not the pre-seek position.
        const long stopTick = (long)PatchKnob::app::audio_app_sample_to_tick(
            PatchKnob::app::audio_app_transport_effective_sample());
        // ...but the transport interval is only an honest duration while the
        // playhead moves FORWARD.  A loop wrap -- or any locate -- during the
        // take leaves the transport BEHIND the origin, and this subtraction
        // then collapsed onto the 1-tick floor: every note still held closed one
        // tick long, the clip came out two ticks wide, and every note in it was
        // then dropped as out of range.  g_recMaxTick is the last thing actually
        // captured, measured in the SAME take-relative clock, so it is the floor
        // the duration can never legitimately fall below.
        const long capturedTicks = std::max<long>(
            std::max<long>(1, g_recMaxTick),
            stopTick - std::max<long>(0, g_recordTimelineStart));
        g_recMaxTick = capturedTicks;
        // close any notes still held when recording stopped
        for (auto& p : g_recPending) { p.second.end = capturedTicks > p.second.start
                                       ? capturedTicks : p.second.start + 1;
                                       g_recNotes.push_back(p.second); }
        g_recPending.clear();
        if (g_recNotes.empty()&&g_recEvents.empty()) {
            // A take that captured NOTHING must leave the lane alone.  Punching
            // the interval out regardless meant an accidental arm-and-stop --
            // or a take whose events were dropped -- silently deleted whatever
            // was already recorded there, with no clip to show for it.
            g_recMaxTick = 0;
            return;
        }
        sequence* target = (g_arrangeRecTarget >= 0 && perf.is_active(g_arrangeRecTarget))
                         ? perf.get_sequence(g_arrangeRecTarget) : nullptr;
        auto armedRoute = g_seqToTrack.find(g_arrangeRecTarget);

        // LOOP OVERDUB.  While the transport is looping and a lane is armed, a
        // take belongs IN that pattern, not in a new one: each pass adds to what
        // is already there at its own position within the loop, the way a drum
        // machine overdubs.  This used to allocate a fresh sequence slot on
        // EVERY commit, so a four-bar loop left behind a clip per pass and the
        // arrangement filled up with near-duplicates.
        //
        // The pattern keeps its existing length and the take wraps into it --
        // `span` therefore becomes the TARGET's length, not the captured
        // duration, so every bounds check below measures against the pattern the
        // notes are landing in.
        //
        // The engine only actually loops in SONG mode: perform.cpp publishes the
        // loop markers under `m_looping && m_playback_mode`, so with the loop
        // button lit in LIVE mode the transport rolls straight past the right
        // marker.  Treating that take as an overdub wrapped a linear performance
        // modulo the armed pattern's length and returned before placing any clip
        // at all -- record eight bars in LIVE mode with loop on and the notes
        // were scrambled into the armed pattern and nothing appeared on the
        // timeline.
        // A recording take always owns a visible arrange region.  The old
        // implicit loop-overdub branch mutated the armed lane's hidden source
        // pattern and returned without adding a trigger.  On an empty track the
        // MIDI was therefore present in memory but no clip was ever created.
        // Explicit clip/region recording is deterministic and lets partition_lane
        // replace the punched interval on every take.
        const bool overdub = false;
        // Quantise the take as a WHOLE so chords move as one event.  This has to
        // run BEFORE the clip length is chosen and before the overdub wrap:
        // both of those measure against where the notes END UP, not against
        // where they were played.
        std::vector<long> qStarts = quantize_take(g_recNotes);
        sequence* s = nullptr;
        long span = 0;
        long odPhase = 0;        // overdub: take-relative -> pattern-local shift
        int  idx  = -1;          // new-clip slot; stays -1 on the overdub path
        if (overdub) {
            s    = target;
            span = target->get_length();
            // WHERE inside the pattern a pass lands is measured from the LOOP
            // START, not from the punch point.  Wrapping take-relative ticks
            // straight modulo the pattern length only lines up when you happen
            // to punch in exactly at the top of the loop -- punch in on beat
            // three of a two-bar loop and every overdubbed note landed two beats
            // early, on every pass.
            //
            // This still assumes the pattern's clip is laid out ON the loop
            // grid.  A trigger carrying its own source offset is NOT handled
            // here and will still be off by that offset; doing it properly
            // means resolving the trigger under the punch point, which is a
            // bigger change than this one is trying to be.
            const long loopLeft = std::max<long>(0, perf.get_left_tick());
            odPhase = ((g_recordTimelineStart - loopLeft) % span + span) % span;
            // Wrap HERE, not at the add_note site.  The same-pitch separation
            // below is what stops two strikes of one voice from overlapping,
            // and it can only see an overlap if it is looking at the ticks the
            // notes will really occupy.  Wrapping afterwards meant pass one's
            // long C4 and pass two's short C4 could land on top of each other
            // untouched, and verify_and_link() then paired pass one's note-on
            // with pass two's note-off -- the exact hung/mangled voice the
            // clamp exists to prevent.
            for (size_t i = 0; i < qStarts.size(); ++i) {
                long v = (qStarts[i] + odPhase) % span;
                if (v < 0) v += span;
                qStarts[i] = v;
            }
            // An overdub mutates a pattern that already exists (and may already
            // be on the timeline), so the piano roll needs a restore point --
            // without this the pass could not be undone at all.
            s->push_undo();
        } else {
            // The clip must be long enough to CONTAIN the quantised take.
            // quantize::apply rounds to the NEAREST line, so the last note of a
            // take can move FORWARD past the punch-out; sizing the clip off the
            // raw transport interval then dropped it, because
            // sequence::add_note_velocity refuses any tick at or past
            // length-1.  Grow to fit instead: at most half a grid step of extra
            // clip is a much better trade than silently losing the note.
            long needed = capturedTicks;
            for (size_t i = 0; i < g_recNotes.size(); ++i) {
                const long dur = std::max<long>(1, g_recNotes[i].end - g_recNotes[i].start);
                needed = std::max(needed, qStarts[i] + dur + 1);
            }
            span = std::max<long>(2, needed);
            for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { idx = i; break; }
            if (idx < 0) { g_recNotes.clear();g_recEvents.clear(); g_recMaxTick = 0; return; }
            perf.new_sequence(idx); perf.set_active(idx, true);
            s = perf.get_sequence(idx);
            // A slot marked active with no sequence behind it is a null
            // dereference waiting for the next is_active() scan; hand it back.
            if (!s) { perf.set_active(idx, false);
                      g_recNotes.clear();g_recEvents.clear(); g_recMaxTick = 0; return; }
            s->set_length(span);
            // set_length CLAMPS to a musical minimum (c_ppqn/4) and is free to
            // adjust further, so the authority for every bounds test below --
            // and for the trigger placed at the end -- is what the sequence
            // actually took, not what we asked for.  A very short punch used to
            // leave span at 2 against a 48-tick pattern: every note was dropped
            // as out of range and a two-tick sliver was placed on the timeline.
            span = s->get_length();
            s->set_name(std::string("Rec ") + std::to_string(idx));
            if (target) {
                s->set_midi_channel(target->get_midi_channel());
                s->set_midi_bus(target->get_midi_bus());
            }
        }
        std::vector<long> recQEnd;          // per-note end after overlap clamping
        // Quantise moves starts LEFT, which can pull a note back over the tail
        // of the PREVIOUS note of the same pitch -- and when two starts land on
        // one grid line, exactly onto it.  Two overlapping same-pitch notes are
        // not a chord: they are one voice struck twice, and verify_and_link()
        // pairs each on with the NEXT off of that pitch, so the overlap makes
        // the first off close the sounding note and orphans the second -- a
        // hung voice.  Separate same-pitch strikes and clamp each to end where
        // the next one begins.  (sequence::put_event_on_bus also retriggers
        // defensively at playback; this keeps the RECORDED DATA honest.)
        {
            std::vector<size_t> byPitch(g_recNotes.size());
            for (size_t i = 0; i < byPitch.size(); ++i) byPitch[i] = i;
            std::sort(byPitch.begin(), byPitch.end(), [&](size_t a, size_t b) {
                if (g_recNotes[a].pitch != g_recNotes[b].pitch)
                    return g_recNotes[a].pitch < g_recNotes[b].pitch;
                if (qStarts[a] != qStarts[b]) return qStarts[a] < qStarts[b];
                return g_recNotes[a].start < g_recNotes[b].start;
            });
            // A double-strike collapsed onto one grid line keeps BOTH notes --
            // nudging the later one a tick preserves the performance, where
            // dropping it (or leaving a zero-length note) silently loses it.
            for (size_t k = 1; k < byPitch.size(); ++k) {
                const size_t prev = byPitch[k-1], cur = byPitch[k];
                if (g_recNotes[prev].pitch != g_recNotes[cur].pitch) continue;
                if (qStarts[cur] <= qStarts[prev]) qStarts[cur] = qStarts[prev] + 1;
            }
            recQEnd.assign(g_recNotes.size(), 0);
            for (size_t i = 0; i < g_recNotes.size(); ++i)
                recQEnd[i] = qStarts[i]
                           + std::max<long>(1, g_recNotes[i].end - g_recNotes[i].start);
            for (size_t k = 1; k < byPitch.size(); ++k) {
                const size_t prev = byPitch[k-1], cur = byPitch[k];
                if (g_recNotes[prev].pitch != g_recNotes[cur].pitch) continue;
                if (recQEnd[prev] > qStarts[cur]) recQEnd[prev] = qStarts[cur];
            }
        }
        for (size_t ni = 0; ni < g_recNotes.size(); ++ni) {
            const RecNote& nr = g_recNotes[ni];
            // Already pattern-local on the overdub path (wrapped above, before
            // the same-pitch separation could be defeated by the wrap).
            long start = qStarts[ni];
            if (start < 0) start = 0;
            if (start >= span-1) continue;
            long dur = recQEnd[ni] - start; if (dur < 1) dur = c_ppqn/8;
            if (start + dur >= span) dur = span - 1 - start;   // stay inside (verify_and_link prune)
            if (dur < 1) dur = 1;
            s->add_note_velocity(start, dur, nr.pitch, nr.vel);
        }
        // Controllers keep the time they were played.  Record quantise is a NOTE
        // grid: pushing a continuous stream onto it stacked every CC between two
        // grid lines onto the line itself, so a mod-wheel or filter sweep came
        // back as a staircase of one step per grid division, with a dozen
        // redundant events piled on each step.  No DAW quantises controllers
        // with the note grid, and there is nothing musical to gain by it here.
        for(RecEvent& e:g_recEvents) {
            if(e.tick<0) e.tick=0;
            if(overdub) { e.tick=(e.tick+odPhase)%span; if(e.tick<0)e.tick+=span; }
        }
        // De-dup AFTER the wrap, not before: on the overdub path several passes
        // can land the same controller value on the same pattern tick, and
        // deduping the pre-wrap ticks left every one of those copies in place.
        std::sort(g_recEvents.begin(),g_recEvents.end(),[](const RecEvent& a,const RecEvent& b){
            if(a.tick!=b.tick)return a.tick<b.tick;if(a.status!=b.status)return a.status<b.status;
            if(a.d1!=b.d1)return a.d1<b.d1;return a.d2<b.d2;});
        g_recEvents.erase(std::unique(g_recEvents.begin(),g_recEvents.end(),
            [](const RecEvent& a,const RecEvent& b){return a.tick==b.tick&&a.status==b.status&&
                a.d1==b.d1&&a.d2==b.d2;}),g_recEvents.end());
        for(const RecEvent& e:g_recEvents)
            if(e.tick>=0&&e.tick<span)s->add_event(e.tick,e.status,e.d1,e.d2,false);
        s->verify_and_link();
        // An overdub landed in a pattern that is ALREADY on the timeline, so
        // there is no clip to create or place -- doing so is what produced the
        // duplicate clips.  Everything below is new-clip placement.
        if (overdub) {
            s->set_dirty();
            g_recNotes.clear(); g_recEvents.clear(); g_recMaxTick = 0;
            app.request_redraw();
            return;
        }
        // place the clip at the record position on the timeline.
        long recStart = g_recordTimelineStart;
        if (recStart < 0) recStart = 0;
        perf.push_trigger_undo();
        vArrange.note_engine_undo("Record Take");   // one queue covers it (ch.28)
        if (target && armedRoute != g_seqToTrack.end())
            partition_lane(g_arrangeRecTarget, recStart, recStart + span);
        s->add_trigger(recStart, span, 0);
        s->set_dirty();
        // An armed instrument lane records a new piano-roll clip into that
        // lane/playlist. With no lane armed, retain the legacy new-track behavior.
        if (target && armedRoute != g_seqToTrack.end()) {
            g_seqToTrack[idx] = armedRoute->second;
        } else if (audio_ok) {
            int tr = PatchKnob::app::audio_app_master_add_track(1);
            if (tr >= 0) { g_seqToTrack[idx] = tr; s->set_midi_bus((char)tr); }
            refresh_master_ui();
        }
        rebind_project_views();
        app.request_redraw();
        g_recNotes.clear(); g_recEvents.clear(); g_recPending.clear(); g_recMaxTick = 0;
    };
    auto armed_is_audio = [&]() {
        sequence* s = (g_arrangeRecTarget >= 0 && perf.is_active(g_arrangeRecTarget))
                    ? perf.get_sequence(g_arrangeRecTarget) : nullptr;
        const std::string name = s && s->get_name() ? s->get_name() : "";
        return name.compare(0, 6, "Audio ") == 0;
    };
    auto commit_audio_recording = [&] {
        PatchKnob::engine::AudioClip clip;
        if (!PatchKnob::app::audio_app_track_capture_end(clip) || clip.empty()) return;
        // The capture buffer is a fixed 120 s.  A longer take is truncated by the
        // tap, and silently returning a short clip made it look like the recorder
        // had mangled it; say so instead.
        {
            const double sr = PatchKnob::app::audio_app_sample_rate();
            if (sr > 0.0 && (double)clip.numFrames() >= 120.0 * sr - (double)1024)
                projectStatus = "Recording hit the 120 s capture limit";
        }
        auto route = g_seqToTrack.find(g_arrangeRecTarget);
        sequence* lane = (g_arrangeRecTarget >= 0 && perf.is_active(g_arrangeRecTarget))
                       ? perf.get_sequence(g_arrangeRecTarget) : nullptr;
        // perf.get_sequence() on an INACTIVE index (the lane was deleted while the
        // take ran) handed back a stale object that was then given a trigger.
        if (route == g_seqToTrack.end() || !lane) return;
        const long startTick = g_recordTimelineStart;
        const long long startSample = PatchKnob::app::audio_app_tick_to_sample(startTick);
        const long long endSample = startSample + (long long)clip.numFrames();
        PatchKnob::app::audio_app_project_partition_track(
            route->second, startSample, endSample);
        if (!PatchKnob::app::audio_app_project_add_audio_clip(
                route->second, clip, startSample, 1.f)) return;
        const long lenTicks = std::max<long>(1,
            (long)PatchKnob::app::audio_app_sample_to_tick_ceil(endSample) - startTick);
        lane->set_length(std::max<long>(lane->get_length(), lenTicks));
        partition_lane(g_arrangeRecTarget, std::max<long>(0,startTick),
                       std::max<long>(0,startTick)+lenTicks);
        lane->add_trigger(std::max<long>(0, startTick), lenTicks, 0);
        lane->set_dirty();
        const PatchKnob::engine::AudioClip* stored =
            PatchKnob::app::audio_app_project_last_clip_on_track(route->second);
        if (stored) vArrange.set_audio_clip(g_arrangeRecTarget, stored, lenTicks);
        g_seqRegion[g_arrangeRecTarget] =
            PatchKnob::app::audio_app_project_last_region_id(route->second);
    };
    toggle_record = [&]{
        if (!g_recArmed) {
            const bool validLane = g_arrangeRecTarget >= 0 &&
                                   perf.is_active(g_arrangeRecTarget) &&
                                   g_seqToTrack.count(g_arrangeRecTarget) != 0;
            // A pre-roll origin is only good for the take it was counted in for.
            // Bailing out here without dropping it left it armed indefinitely,
            // and the NEXT take -- minutes later, count-in possibly switched off
            // -- silently punched in at the abandoned count-in's position
            // instead of at the playhead.
            if (!validLane) {
                g_countInRecordOriginTick = -1;
                projectStatus = "Arm an instrument or audio track before recording";
                return;
            }
            if (!armed_is_audio() && midi_track_for_seq(g_arrangeRecTarget) < 0) {
                g_countInRecordOriginTick = -1;
                projectStatus = "The armed track has no MIDI route node";
                return;
            }
        }
        g_recArmed = !g_recArmed;
        if (g_recArmed) {
            g_recNotes.clear(); g_recEvents.clear(); g_recPending.clear(); g_recMaxTick = 0;
            // Remember the KIND of lane this take was aimed at.  It decides WHEN
            // capture may open (below), so it has to be settled first.  It also
            // has to survive a capture that fails to start: commit_recording
            // partitions the lane, which would delete audio already there in
            // exchange for an empty pattern.
            const bool takeIsAudio = armed_is_audio();
            g_recTargetWasAudio = takeIsAudio;
            const bool wasRolling = PatchKnob::app::audio_app_transport_rolling();
            long punch = 0;
            if (!wasRolling) {
                // Stopped: the punch-in point is the arrange playhead, or the
                // pre-roll origin the count-in handed back.  Tell perform to
                // start THERE so its own locate agrees with ours.
                punch = g_countInRecordOriginTick >= 0
                    ? (long)std::max<long long>(0, g_countInRecordOriginTick)
                    : std::max<long>(0, perf.get_tick());
                perf.set_starting_tick(punch);
            }
            // Rolling already: punch in WHERE THE TRANSPORT IS.  Locating to
            // perf.get_tick() here used to drag the playhead backwards by
            // however stale that UI tick was, bumping the schedule epoch and
            // flushing the notes already queued ahead of it -- an audible drop
            // out every time you punched in on the fly.
            g_countInRecordOriginTick = -1;

            // One clock owns both capture-relative event timestamps and region
            // placement. Mixing perform::tick with the audio transport caused
            // block/lookahead-sized placement and duration errors.
            auto open_capture = [&](long origin){
                PatchKnob::app::audio_app_record_arm_at_tick(true, origin);
                g_recordTimelineStart=std::max<long>(
                    0,(long)PatchKnob::app::audio_app_record_start_tick());
                const int armIn = g_arrangeRecTarget >= 0
                                ? track_midi_in(g_arrangeRecTarget) : -1;
                PatchKnob::app::audio_app_midi_track_capture(
                    midi_track_for_seq(g_arrangeRecTarget),true);
                // Strict routing: an armed track whose virtual input has nothing
                // patched into it records nothing.  Say so rather than silently
                // producing an empty clip.
                if (armIn >= 0 &&
                    !PatchKnob::app::audio_app_virtual_midi_input_is_patched(armIn))
                    projectStatus = std::string("Armed on ") +
                        virtual_port_name(true, armIn) +
                        " but nothing is patched into it - no MIDI will record";
            };

            // A MIDI take from a STANDSTILL arms BEFORE the transport rolls.
            //
            // It used to roll first, wait out transport_settle() -- two whole
            // audio blocks plus thread scheduling -- and only THEN read the
            // position back and arm.  For that entire window the transport was
            // running and audible but not capturing, so the first notes of every
            // take were simply dropped, and the origin it finally latched was
            // later than the punch point, which slid the whole clip forward.
            // That is the "beginning of the recording is missing" report.
            //
            // Arming early is safe because inner_start()'s target is not a
            // mystery we have to measure it to discover: perform locates to
            // m_starting_tick in song mode and to a hard 0 in LIVE mode.  (That
            // asymmetry is also why the origin could NOT simply be `punch` --
            // in LIVE mode perform moves the clock out from under it, every
            // captured event stamps earlier than the origin, drain_record clamps
            // to 0 and the whole performance piles onto tick zero.)
            const bool armEarly = !wasRolling && !takeIsAudio;
            if (armEarly) open_capture(g_songMode ? punch : 0);

            if (!wasRolling) {
                perf.start(g_songMode);
                PatchKnob::app::audio_app_patch_set_playing(true);
                app.animating = true;
                // Let the seek perform just queued LAND before capture opens.
                // Re-issuing a locate of our own to achieve that would bump the
                // schedule epoch and discard the events perform has already
                // queued for the first blocks of the take.
                PatchKnob::app::audio_app_transport_settle();
            }
            // Punching in on the fly, or an AUDIO take: the origin can only be
            // read from the transport as it stands now.  An audio tap carries no
            // position of its own -- it starts filling the instant it is handed
            // over -- so the clip is placed as if ITS frame 0 were the origin,
            // and the origin therefore has to be latched right next to the tap
            // opening rather than ahead of it.
            if (!armEarly)
                open_capture((long)std::max<long long>(0,
                    PatchKnob::app::audio_app_sample_to_tick(
                        PatchKnob::app::audio_app_transport_effective_sample())));
            g_audioTrackCapturing = false;
            // The roll happens at the top of this branch now (it has to, so the
            // origin is latched against a transport that has already been moved
            // wherever perform wants it).  The mixer tap still must not open
            // before the transport is running: it fills its buffer from the
            // moment it is handed over, but the committed clip is placed as if
            // frame 0 were the record origin, so any lead-in it swallowed put
            // the take that far ahead of where it lands.
            if (g_recTargetWasAudio) {
                auto route = g_seqToTrack.find(g_arrangeRecTarget);
                if (route != g_seqToTrack.end())
                    g_audioTrackCapturing =
                        PatchKnob::app::audio_app_track_capture_begin(route->second, 120.0);
                if (!g_audioTrackCapturing)
                    projectStatus = "Audio capture could not start";
            }
        } else {
            // Close producers first, wait for the graph's RCU grace period in
            // audio_app_midi_track_capture(false), then drain every queued tail
            // event. This is the only race-free punch-out order.
            const bool wasAudio = g_recTargetWasAudio;
            PatchKnob::app::audio_app_record_arm_at_tick(false,0);
            PatchKnob::app::audio_app_midi_track_capture(
                midi_track_for_seq(g_arrangeRecTarget),false);
            if (!wasAudio && drain_record) drain_record();
            // An AUDIO take commits audio.  It must never fall through to the
            // MIDI committer: that one partitions the lane and would delete the
            // very material the punch was supposed to replace.
            if (wasAudio) { if (g_audioTrackCapturing) commit_audio_recording(); }
            else          commit_recording();
            g_audioTrackCapturing = false;
            g_recTargetWasAudio = false;
            vArrange.clear_record_preview();
            g_recordPreviewAudio = PatchKnob::engine::AudioClip();
            g_recNotes.clear(); g_recEvents.clear(); g_recPending.clear(); g_recMaxTick = 0;
        }
    };
    punch_out_recording = [&]{
        if (g_recArmed && toggle_record) toggle_record();
    };

    // ======================= punch recording machinery ======================
    // Pro Tools ch.27 (QuickPunch / TrackPunch / DestructivePunch) on the
    // modular input path.  How a punch gets its audio:
    //
    //   audio device --duplex stream--> AudioEngine::render_into (in[])
    //     --> AudioDeviceInNode (the patcher's "Audio In" module)
    //     --> whatever the user patched (directly, or via racks/FX nodes)
    //     --> the armed track's MasterMixerNode audio INLET (port 2 + 2*track)
    //     --> the PRE-FADER per-track capture tap in MasterMixerNode::process
    //
    // The tap copies the raw inlet, so a punch records exactly what is patched
    // INTO the track -- an Audio In module, an instrument, a rack -- before
    // fader/pan/mute (monitoring never bakes into the take).  A track whose
    // inlet has nothing patched into it records SILENCE, and the UI says so:
    // there is no hidden hardware side channel, the same contract the
    // virtual-MIDI side states.  The engine keeps one tap per track (8
    // simultaneous), so TrackPunch/DestructivePunch genuinely punch multiple
    // tracks at once; each captured track is a PunchLane.
    auto punch_seq_track = [&](int seq) -> int {
        auto it = g_seqToTrack.find(seq);
        return it == g_seqToTrack.end() ? -1 : it->second;
    };
    auto seq_is_audio_lane = [&](int seq) -> bool {
        sequence* s = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        if (!s) return false;
        if (s->get_track_kind() == 1) return true;
        const std::string nm = s->get_name() ? s->get_name() : "";
        return nm.compare(0, 6, "Audio ") == 0;
    };
    // Lane representative: the first audio sequence routed to `track` (several
    // clips share one lane; the first is the stable identity).
    auto lane_seq_for_track = [&](int track) -> int {
        if (track < 0) return -1;
        for (int si = 0; si < c_max_sequence; ++si)
            if (perf.is_active(si) && seq_is_audio_lane(si) &&
                punch_seq_track(si) == track)
                return si;
        return -1;
    };
    // Strip a punch suffix ("_NN-MM", then "_NN") off a lane name, so a pass
    // whose lane representative happens to BE an earlier punch clip does not
    // nest suffixes ("Audio 1_01-01_02-01").
    auto punch_base_name = [](std::string n) -> std::string {
        auto dig = [](char c){ return c >= '0' && c <= '9'; };
        size_t L = n.size();
        if (L >= 6 && n[L-6] == '_' && dig(n[L-5]) && dig(n[L-4]) &&
            n[L-3] == '-' && dig(n[L-2]) && dig(n[L-1]))
            { n.resize(L - 6); L = n.size(); }
        if (L >= 3 && n[L-3] == '_' && dig(n[L-2]) && dig(n[L-1]))
            n.resize(L - 3);
        return n.empty() ? std::string("Audio") : n;
    };
    auto lane_name_for_track = [&](int track) -> std::string {
        const int si = lane_seq_for_track(track);
        sequence* s = si >= 0 ? perf.get_sequence(si) : nullptr;
        return punch_base_name(s && s->get_name() ? s->get_name()
                                                  : std::string("Audio"));
    };
    // The mixer tracks a pass captures: QuickPunch works on the record-enabled
    // audio lane (PT p638); TrackPunch/DestructivePunch on every punch-enabled
    // track (up to the engine's 8 simultaneous input taps).
    auto punch_target_tracks = [&]() -> std::vector<int> {
        std::vector<int> v;
        if (g_recMode == RecMode::QuickPunch) {
            if (g_arrangeRecTarget >= 0 && seq_is_audio_lane(g_arrangeRecTarget)) {
                const int t = punch_seq_track(g_arrangeRecTarget);
                if (t >= 0) v.push_back(t);
            }
            return v;
        }
        for (int t : g_punchTracks) v.push_back(t);
        return v;
    };
    punch_track_count_fn = [&]() -> int {
        if (g_recMode == RecMode::Normal) return 0;
        return (int)punch_target_tracks().size();
    };
    auto dp_min_frames = [&]() -> long long {
        return (long long)((double)g_prefDpFileLenSec *
                           PatchKnob::app::audio_app_sample_rate());
    };
    auto punch_lane_for = [&](int track) -> PunchLane* {
        for (auto& pl : g_punchLanes) if (pl.track == track) return &pl;
        return nullptr;
    };
    // Open one track's input capture (at pass start, or mid-pass when a track
    // is punch-enabled late).  Latches ITS timeline origin right before its
    // tap opens, per-lane -- lanes opened mid-pass anchor where the transport
    // is then.
    auto punch_open_lane = [&](int track) -> PunchLane* {
        if (PunchLane* pl = punch_lane_for(track)) return pl;
        const int laneSeq = lane_seq_for_track(track);
        if (track < 0 || laneSeq < 0) return nullptr;
        if (g_recMode == RecMode::DestructivePunch) {
            char why[256] = {0};
            if (!PatchKnob::app::audio_app_project_dp_eligible(
                    track, dp_min_frames(), why, sizeof why)) {
                projectStatus = "DestructivePunch '" + lane_name_for_track(track)
                              + "': " + why;
                return nullptr;
            }
        }
        // Let any queued seek land so the latched origin is where playback
        // really is (same contract as toggle_record's audio path: the tap
        // fills from the moment it is handed over, and the lane is placed as
        // if its frame 0 were here).
        PatchKnob::app::audio_app_transport_settle();
        const long long origin = std::max<long long>(0,
            PatchKnob::app::audio_app_transport_effective_sample());
        if (!PatchKnob::app::audio_app_track_capture_begin(track, 120.0)) {
            projectStatus = "Punch capture could not start on '"
                          + lane_name_for_track(track)
                          + "' (all 8 simultaneous input taps in use?)";
            return nullptr;
        }
        PunchLane pl;
        pl.track = track; pl.seq = laneSeq; pl.startSample = origin;
        // Honesty about the modular routing: an armed track whose mixer inlet
        // has nothing patched into it records silence.  Warn, do not refuse --
        // the user may patch mid-pass, and printing silence is a legal take.
        pl.silent = !PatchKnob::app::audio_app_track_input_patched(track);
        if (pl.silent)
            projectStatus = "'" + lane_name_for_track(track) +
                "' has nothing patched into its mixer inlet - the punch will "
                "record silence (wire Audio In or an instrument to it)";
        g_punchLanes.push_back(pl);
        return &g_punchLanes.back();
    };
    // Open the pass: one input capture per target track.
    auto punch_pass_begin = [&]() -> bool {
        if (g_punchPassActive) return true;
        if (g_recMode == RecMode::Normal) return false;
        if (g_loopOn) {
            projectStatus = "Punch recording does not run while LOOP is on";
            return false;
        }
        const std::vector<int> targets = punch_target_tracks();
        if (targets.empty()) return false;
        g_punchLanes.clear();
        g_punchRecordingTracks.clear();
        for (int t : targets) punch_open_lane(t);
        if (g_punchLanes.empty()) return false;
        g_punchPassLocGen = PatchKnob::app::audio_app_locate_generation();
        g_punchPassActive = true;
        return true;
    };
    auto punch_in = [&](PunchLane& pl) -> bool {
        if (!g_punchPassActive || pl.inFrame >= 0) return false;
        if ((int)pl.ranges.size() >= 200) {              // PT's per-pass cap
            projectStatus = "200 punches reached on '"
                          + lane_name_for_track(pl.track) + "' this pass";
            return false;
        }
        const double sr = PatchKnob::app::audio_app_sample_rate();
        const long long f =
            PatchKnob::app::audio_app_track_capture_frames_for(pl.track);
        if (sr > 0 && (double)f >= 120.0 * sr - 4096.0) {
            projectStatus = "Punch pass hit the 120 s capture limit";
            return false;
        }
        pl.inFrame = f;
        g_punchRecordingTracks.insert(pl.track);
        // MONITOR switch only (TrackInput = input while recording): the
        // capture reads the strip INLET, which the track's own clip playback
        // never enters, so muting the scheduled clips here changes what you
        // HEAR during the punch -- input instead of the old material -- and
        // nothing about what is recorded.  Restored at punch-out: PT's
        // "instantaneous monitor switching on punch-out".
        PatchKnob::app::audio_app_project_hold_mute(pl.track, 1);
        app.request_redraw();
        return true;
    };
    auto punch_out = [&](PunchLane& pl) -> bool {
        if (!g_punchPassActive || pl.inFrame < 0) return false;
        long long f = PatchKnob::app::audio_app_track_capture_frames_for(pl.track);
        if (f <= pl.inFrame) f = pl.inFrame + 1;
        PatchKnob::app::audio_app_project_hold_mute(pl.track, 0);
        if (g_recMode == RecMode::DestructivePunch) {
            // DestructivePunch commits AT punch-out (PT p642): the punched
            // range is written straight into the track's contiguous file with
            // the fixed 10 ms linear crossfade at each end.  No clips are
            // created and the pass keeps rolling.
            PatchKnob::engine::AudioClip take;
            if (PatchKnob::app::audio_app_track_capture_read_for(
                    pl.track, pl.inFrame, f - pl.inFrame, take) && !take.empty()) {
                const long long xf = (long long)(0.010 *
                    PatchKnob::app::audio_app_sample_rate());
                if (!PatchKnob::app::audio_app_project_destructive_punch(
                        pl.track, pl.startSample + pl.inFrame,
                        take, 0, take.numFrames(), xf))
                    projectStatus = "DestructivePunch write failed on '"
                                  + lane_name_for_track(pl.track) + "'";
            } else projectStatus = "DestructivePunch: nothing captured";
        }
        pl.ranges.push_back({ pl.inFrame, f });          // DP counts them too
        pl.inFrame = -1;
        g_punchRecordingTracks.erase(pl.track);
        app.request_redraw();
        return true;
    };
    // Transport REC while rolling toggles ALL lanes together (PT p641:
    // "punch in and out on all TrackPunch-enabled tracks simultaneously").
    auto punch_toggle_all = [&]{
        if (!g_punchRecordingTracks.empty()) {
            for (auto& pl : g_punchLanes) if (pl.inFrame >= 0) punch_out(pl);
        } else {
            for (auto& pl : g_punchLanes) punch_in(pl);
        }
    };
    // QP/TP lane commit: ONE whole-file parent clip per lane per pass; every
    // punch is a region cut from it (shared source, so Trim can open the
    // head/tail to reveal what was captured in the background).  Naming per PT
    // p635: parent "Name_NN", punches "Name_NN-MM".
    auto commit_punch_lane = [&](PunchLane& lane,
                                 PatchKnob::engine::AudioClip& whole) {
        const int track = lane.track;
        const int laneSeq = lane.seq;
        sequence* laneS = perf.is_active(laneSeq) ? perf.get_sequence(laneSeq)
                                                  : nullptr;
        if (!laneS || track < 0) return;
        const double sr = PatchKnob::app::audio_app_sample_rate();
        if (sr > 0 && (double)whole.numFrames() >= 120.0 * sr - 1024.0)
            projectStatus = "Punch pass hit the 120 s capture limit";
        const std::string base =
            punch_base_name(laneS->get_name() ? laneS->get_name() : "Audio");
        const int nn = ++g_punchPassNumber[base];
        char nbuf[32]; std::snprintf(nbuf, sizeof nbuf, "_%02d", nn);
        whole.name = base + nbuf;
        perf.push_trigger_undo();
        vArrange.note_engine_undo("Punch Pass");
        // 1) partition every punched range out of the existing playlist --
        //    engine regions AND arrange triggers -- before the parent is owned
        //    (partition runs the orphan-clip GC; see audio_app.h contract).
        struct TickRange { long in, len; long long sIn, sOut; };
        std::vector<TickRange> tr;
        for (const auto& r : lane.ranges) {
            const long long sIn  = lane.startSample + r.first;
            const long long sOut = lane.startSample + r.second;
            PatchKnob::app::audio_app_project_partition_track(track, sIn, sOut);
            const long tickIn = (long)PatchKnob::app::audio_app_sample_to_tick(sIn);
            const long lenTicks = std::max<long>(1,
                (long)PatchKnob::app::audio_app_sample_to_tick_ceil(sOut) - tickIn);
            partition_lane(laneSeq, tickIn, tickIn + lenTicks);
            tr.push_back({ tickIn, lenTicks, sIn, sOut });
        }
        // 2) own the whole-file parent, then hang the punch regions off it.
        const PatchKnob::engine::AudioClip* stored =
            PatchKnob::app::audio_app_project_own_clip(track, whole);
        if (!stored) { projectStatus = "Punch commit failed (out of memory?)"; return; }
        const long fullTicks = std::max<long>(1,
            (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                (long long)stored->numFrames()));
        const long long monF = (long long)(0.004 * sr);   // 4 ms monitor fade
        const long long xfF  = (long long)((double)g_prefPunchXfadeMs * 0.001 * sr);
        int mm2 = 0, placed = 0;
        for (size_t k = 0; k < lane.ranges.size(); ++k) {
            const auto& r = lane.ranges[k];
            const TickRange& t = tr[k];
            ++mm2;
            int idx = -1;
            for (int i = 0; i < c_max_sequence; ++i)
                if (!perf.is_active(i)) { idx = i; break; }
            if (idx < 0) { projectStatus = "No free sequence slots for punch clips"; break; }
            perf.new_sequence(idx);
            perf.set_active(idx, true);
            sequence* s = perf.get_sequence(idx);
            if (!s) { perf.set_active(idx, false); break; }
            s->set_track_kind(1);
            s->set_arrange_lane_id(laneS->get_arrange_lane_id());
            s->set_midi_bus(laneS->get_midi_bus());
            s->set_midi_channel(laneS->get_midi_channel());
            s->set_length(std::max<long>(2, fullTicks));
            char pbuf[48]; std::snprintf(pbuf, sizeof pbuf, "_%02d-%02d", nn, mm2);
            // The _NN-MM suffix IS the auto-punch marker that separates these
            // from user-defined clips wherever names are listed (PT p635).
            s->set_name(base + pbuf);
            const long srcTick = std::max<long>(0,
                (long)PatchKnob::app::audio_app_sample_to_tick_ceil(r.first));
            s->clear_triggers();
            s->add_trigger(t.in, t.len, srcTick, false);
            s->set_dirty();
            g_seqToTrack[idx] = track;
            g_arrangeRecInput[idx] = -1; g_arrangeMidiOut[idx] = -1;
            // Engine region over the SHARED parent; the 4 ms monitor-equivalent
            // fades are always applied (playback-time, nothing written into the
            // audio) so entering/leaving the punch cannot click.
            PatchKnob::app::audio_app_project_add_region_shared(
                track, stored, t.sIn, r.first, r.second - r.first, 1.f, monF, monF);
            g_seqRegion[idx] = PatchKnob::app::audio_app_project_last_region_id(track);
            // QP/TP Crossfade Length (PT p636): pre-crossfade on the outgoing
            // material up to the punch-in boundary, post-crossfade on the
            // returning material after punch-out.  0 = none written.
            if (xfF > 0)
                PatchKnob::app::audio_app_project_set_boundary_fades(
                    track, t.sIn, t.sOut, xfF);
            vArrange.set_audio_clip(idx, stored, fullTicks);
            vArrange.set_audio_region(idx, t.in, t.len, srcTick);
            ++placed;
        }
        projectStatus = (g_recMode == RecMode::TrackPunch ? "TrackPunch "
                                                          : "QuickPunch ")
                      + whole.name + ": " + std::to_string(placed) + " punch clip(s)";
    };
    punch_transport_stop = [&]{
        if (g_punchPassActive) {
            int committed = 0, dpPunches = 0;
            for (auto& pl : g_punchLanes) {
                if (pl.inFrame >= 0) punch_out(pl);      // close a running punch
                PatchKnob::engine::AudioClip whole;
                const bool got =
                    PatchKnob::app::audio_app_track_capture_end_for(pl.track, whole);
                PatchKnob::app::audio_app_project_hold_mute(pl.track, 0);
                if (g_recMode == RecMode::DestructivePunch) {
                    dpPunches += (int)pl.ranges.size();   // written at punch-out
                } else if (got && !whole.empty() && !pl.ranges.empty()) {
                    commit_punch_lane(pl, whole);
                    ++committed;
                }
            }
            if (g_recMode == RecMode::DestructivePunch && dpPunches > 0)
                projectStatus = "DestructivePunch pass: "
                    + std::to_string(dpPunches) + " punch(es) written across "
                    + std::to_string((int)g_punchLanes.size()) + " track(s)";
            else if (committed > 1)
                projectStatus = (g_recMode == RecMode::TrackPunch
                                     ? "TrackPunch pass: " : "QuickPunch pass: ")
                    + std::to_string(committed) + " tracks committed";
            if (committed > 0) { rebind_project_views(); }
            g_punchPassActive = false;
            g_punchLanes.clear();
            g_punchRecordingTracks.clear();
        }
        // Transport RecordLock (PT p637): armed stays armed across a stop;
        // legacy behaviour (off) disarms.  Forced off in DestructivePunch.
        if (!(g_prefTransportRecLock && g_recMode != RecMode::DestructivePunch))
            g_recReady = false;
        // Audio Track RecordLock (PT p637): off = digital-dubber behaviour,
        // record/punch enables clear when the transport stops.
        if (!g_prefAudioTrackRecLock) {
            g_punchTracks.clear();
            if (!g_recArmed) g_arrangeRecTarget = -1;
        }
        app.request_redraw();
    };
    punch_record_pressed = [&]() -> bool {
        if (g_recMode == RecMode::Normal) return false;
        if (!perf.running()) {
            // Stopped: REC toggles Record Ready (PT: "click Record to enter
            // Record Ready mode", then Play starts the pass).
            g_recReady = !g_recReady;
            if (g_recReady && punch_target_tracks().empty())
                projectStatus = g_recMode == RecMode::QuickPunch
                    ? "QuickPunch: record enable an audio track first"
                    : "Punch enable an audio track first (Super+click or "
                      "Ctrl+click its record button)";
            return true;
        }
        // Rolling: open the pass on demand, then REC toggles the punch on all
        // captured lanes at once.  With no audio punch target (e.g. a MIDI
        // lane armed), fall through to the normal record path -- MIDI tracks
        // punch in Normal mode (PT p633) and gain nothing from these modes.
        if (!g_punchPassActive && !punch_pass_begin()) return false;
        punch_toggle_all();
        return true;
    };
    punch_track_button = [&](int seq) {
        const SDL_Keymod mod = SDL_GetModState();
        // "Start" (Windows/Super) is the manual's punch-enable modifier; on
        // Linux many window managers grab Super+click, so Ctrl+click is
        // accepted as the same gesture while a punch mode is active (Normal
        // mode keeps the plain-arm behaviour for every unmodified click).
        const bool startK = (mod & KMOD_GUI) != 0 ||
                            ((mod & KMOD_CTRL) != 0 && g_recMode != RecMode::Normal);
        const bool alt   = (mod & KMOD_ALT)   != 0;
        const bool shift = (mod & KMOD_SHIFT) != 0;
        // Toggle punch-enable on one lane, validating DP eligibility on the
        // way in (PT p644: refuse with the applicable remedy).
        auto toggle_punch = [&](int sq) {
            if (!seq_is_audio_lane(sq)) return;
            const int t = punch_seq_track(sq);
            if (t < 0) return;
            if (g_punchTracks.count(t)) {
                if (PunchLane* pl = punch_lane_for(t))
                    if (pl->inFrame >= 0) punch_out(*pl);  // disabling mid-punch = punch out
                g_punchTracks.erase(t);
            } else {
                if (g_recMode == RecMode::DestructivePunch) {
                    char why[256] = {0};
                    if (!PatchKnob::app::audio_app_project_dp_eligible(
                            t, dp_min_frames(), why, sizeof why)) {
                        projectStatus = std::string("Cannot DP-enable: ") + why;
                        return;
                    }
                }
                g_punchTracks.insert(t);
            }
        };
        // "Selected" tracks = lanes with a selected clip.  Clip selection
        // lives in the seq24 trigger model (ArrangeView::select_trigger), so
        // it is readable here without any arrange_view API.
        auto lane_selected = [&](int rep) -> bool {
            const int t = punch_seq_track(rep);
            for (int si = 0; si < c_max_sequence; ++si) {
                if (!perf.is_active(si) || punch_seq_track(si) != t) continue;
                sequence* s = perf.get_sequence(si);
                if (!s) continue;
                long on = 0, off = 0, offx = 0; bool sel = false;
                s->reset_draw_trigger_marker();
                while (s->get_next_trigger(&on, &off, &sel, &offx))
                    if (sel) return true;
            }
            return false;
        };
        auto each_audio_lane = [&](bool selectedOnly, const std::function<void(int)>& fn) {
            for (int si = 0; si < c_max_sequence; ++si) {
                if (!perf.is_active(si) || !seq_is_audio_lane(si)) continue;
                if (lane_seq_for_track(punch_seq_track(si)) != si) continue;
                if (selectedOnly && !lane_selected(si)) continue;
                fn(si);
            }
        };
        if (startK && alt) {
            // Alt+Start(+Shift)+click: toggle punch-enable on all (selected)
            // audio tracks (PT p640/645).
            each_audio_lane(shift, toggle_punch);
        } else if (startK) {
            toggle_punch(seq);                       // Start+click: this track
        } else if (alt && g_recMode != RecMode::Normal) {
            // Alt(+Shift)+click: punch-enable AND record-enable all (selected)
            // audio tracks.  Record-enable is a single target here, so the
            // clicked lane gets the arm and the rest get punch-enable.
            each_audio_lane(shift, [&](int si) {
                if (!seq_is_audio_lane(si)) return;
                const int t = punch_seq_track(si);
                if (t >= 0) g_punchTracks.insert(t);
            });
            if (!g_recArmed && seq_is_audio_lane(seq)) {
                g_arrangeRecTarget = seq;
                if (rebuild_track_midi_routes) rebuild_track_midi_routes();
            }
        } else {
            // Plain click.  TrackPunch/DestructivePunch + rolling: the track's
            // Record Enable button IS its punch switch (PT p641/645) -- each
            // track punches independently of the others.
            const int t = punch_seq_track(seq);
            if ((g_recMode == RecMode::TrackPunch ||
                 g_recMode == RecMode::DestructivePunch) && perf.running() &&
                g_punchTracks.count(t)) {
                if (!g_punchPassActive && !punch_pass_begin()) return;
                // A track punch-enabled after the pass opened gets its own
                // capture lane on first use.
                PunchLane* pl = punch_lane_for(t);
                if (!pl) pl = punch_open_lane(t);
                if (pl) { if (pl->inFrame >= 0) punch_out(*pl); else punch_in(*pl); }
                app.request_redraw();
                return;
            }
            // Legacy single-target arm.
            if (g_recArmed) {
                projectStatus = "Stop recording before arming a different track";
                return;
            }
            g_arrangeRecTarget = (g_arrangeRecTarget == seq) ? -1 : seq;
            if (rebuild_track_midi_routes) rebuild_track_midi_routes();
        }
        app.request_redraw();
    };
    set_rec_mode = [&](int m) {
        if (m < 0 || m > 3 || (int)g_recMode == m) return;
        if (g_recArmed || g_punchPassActive) {
            projectStatus = "Stop recording before changing the record mode";
            return;
        }
        g_recMode = (RecMode)m;
        static const char* kNames[4] = { "Normal", "QuickPunch", "TrackPunch",
                                         "DestructivePunch" };
        projectStatus = std::string("Record mode: ") + kNames[m] +
            (m == 3 ? "  (destructive; Transport RecordLock suspended)" : "");
        app.request_redraw();
    };
    prepare_dpe_tracks = [&]{
        if (g_recMode != RecMode::DestructivePunch) {
            projectStatus = "Enable DestructivePunch mode first (Options menu)";
            return;
        }
        if (perf.running() || g_punchPassActive || g_recArmed) {
            projectStatus = "Stop the transport before Prepare DPE Tracks";
            return;
        }
        // The tracks to prepare: the DP-enabled set when there is one.  A
        // track that FAILS the DP requirements cannot be DP-enabled (that
        // refusal is deliberate, PT p644), so with no enabled tracks the
        // command falls back to the lanes with selected clips, then to the
        // record-armed lane -- otherwise the command that fixes ineligible
        // tracks would be unreachable from exactly the state that needs it.
        std::set<int> targets = g_punchTracks;
        if (targets.empty())
            for (int si = 0; si < c_max_sequence; ++si) {
                if (!perf.is_active(si) || !seq_is_audio_lane(si)) continue;
                sequence* s2 = perf.get_sequence(si);
                if (!s2) continue;
                long on = 0, off = 0, offx = 0; bool sel = false;
                s2->reset_draw_trigger_marker();
                while (s2->get_next_trigger(&on, &off, &sel, &offx))
                    if (sel) { targets.insert(punch_seq_track(si)); break; }
            }
        if (targets.empty() && g_arrangeRecTarget >= 0 &&
            seq_is_audio_lane(g_arrangeRecTarget))
            targets.insert(punch_seq_track(g_arrangeRecTarget));
        targets.erase(-1);
        if (targets.empty()) {
            projectStatus = "Prepare DPE Tracks: DP-enable, select a clip on, "
                            "or record-arm an audio track first";
            return;
        }
        const long long lenF = dp_min_frames();
        const long lenTicks = std::max<long>(1,
            (long)PatchKnob::app::audio_app_sample_to_tick_ceil(lenF));
        perf.push_trigger_undo();
        vArrange.note_engine_undo("DestructivePunch Write");
        int done = 0;
        for (int track : targets) {
            const int laneSeq = lane_seq_for_track(track);
            sequence* lane = laneSeq >= 0 ? perf.get_sequence(laneSeq) : nullptr;
            if (!lane) continue;
            const std::string nm =
                (lane->get_name() ? lane->get_name() : "Audio") + std::string(" DPE");
            // Consolidate renders region gain + fades into the flat file and
            // schedules it at 0 with unity gain (PT p644: clip gain rendered
            // and reset to 0 dB).
            if (!PatchKnob::app::audio_app_project_consolidate_track(
                    track, lenF, nm.c_str()))
                continue;
            const PatchKnob::engine::AudioClip* stored =
                PatchKnob::app::audio_app_project_clip_on_track(track);
            // Rebuild the lane's arrange triggers as ONE contiguous clip.
            for (int si = 0; si < c_max_sequence; ++si)
                if (perf.is_active(si) && seq_is_audio_lane(si) &&
                    punch_seq_track(si) == track)
                    if (sequence* s2 = perf.get_sequence(si)) {
                        s2->clear_triggers(); s2->set_dirty();
                        vArrange.set_audio_clip(si, nullptr, 0);
                        g_seqRegion.erase(si);
                    }
            lane->set_length(std::max<long>(lane->get_length(), lenTicks));
            lane->add_trigger(0, lenTicks, 0, false);
            lane->set_dirty();
            if (stored) {
                vArrange.set_audio_clip(laneSeq, stored, lenTicks);
                vArrange.set_audio_region(laneSeq, 0, lenTicks, 0);
                g_seqRegion[laneSeq] =
                    PatchKnob::app::audio_app_project_last_region_id(track);
            }
            // The track now meets every DP requirement -- enable it, so the
            // prepare -> enable -> punch flow needs no second gesture.
            g_punchTracks.insert(track);
            ++done;
        }
        rebind_project_views();
        projectStatus = "Prepared " + std::to_string(done) + " DPE track(s) ("
                      + std::to_string(g_prefDpFileLenSec / 60) + " min file)";
        app.request_redraw();
    };
    punch_poll = [&]{
        const bool rolling = perf.running();
        // Pass lifecycle self-healing: a stop from ANY path (space bar in a
        // view, song end, remote) still commits the pass; a play started with
        // the transport armed opens one.
        if (g_punchPassActive && !rolling) punch_transport_stop();
        // A locate (or loop switched on) mid-pass breaks the frame<->timeline
        // identity the punch marks depend on: commit what exists and close.
        if (g_punchPassActive && rolling &&
            (g_loopOn || PatchKnob::app::audio_app_locate_generation() !=
                             g_punchPassLocGen)) {
            projectStatus = "Punch pass ended (transport was relocated)";
            punch_transport_stop();
        }
        // A pass opens whenever playback rolls in a punch mode with capture
        // targets -- PT starts the background file at playback start (p633), so
        // a QuickPunch REC press mid-pass can be trimmed open later to reveal
        // audio from before the punch.  ONE attempt per roll: a failed open
        // (loop on, DP track no longer eligible) must not retry -- and re-post
        // its status message -- every frame; the on-demand paths (transport
        // REC, track record buttons) can still open a pass later in the roll.
        static bool s_passTriedThisRoll = false;
        if (!rolling) s_passTriedThisRoll = false;
        if (!g_punchPassActive && rolling && g_recMode != RecMode::Normal &&
            !s_passTriedThisRoll &&
            (g_recReady || !punch_target_tracks().empty())) {
            s_passTriedThisRoll = true;
            if (punch_pass_begin() && g_recReady) {
                // Tracks both punch- and record-enabled begin recording as
                // soon as the armed transport rolls (PT p645) -- TP/DP only;
                // QuickPunch waits for the transport Record press.
                if (g_recMode != RecMode::QuickPunch &&
                    g_arrangeRecTarget >= 0)
                    if (PunchLane* pl = punch_lane_for(
                            punch_seq_track(g_arrangeRecTarget)))
                        punch_in(*pl);
            }
        }
        // Transport RecordLock in Normal mode: the armed transport re-enters
        // record when playback starts (digital-dubber behaviour).
        if (g_recReady && rolling && g_recMode == RecMode::Normal &&
            !g_recArmed && toggle_record) {
            g_recReady = false;
            toggle_record();
        }
        // Mute Record-Armed Tracks While Stopped (Foley, PT p649): silence the
        // mixer strips of record/punch-enabled tracks whenever the transport
        // is stopped; restore exactly the strips this preference muted.
        static std::set<int> s_prefMuted;
        std::set<int> want;
        if (g_prefMuteArmedStopped && !rolling) {
            if (g_arrangeRecTarget >= 0) {
                const int t = punch_seq_track(g_arrangeRecTarget);
                if (t >= 0) want.insert(t);
            }
            for (int t : g_punchTracks) want.insert(t);
        }
        if (want != s_prefMuted) {
            const int node = PatchKnob::app::audio_app_master_mixer_node();
            for (int t : s_prefMuted)
                if (!want.count(t))
                    PatchKnob::app::audio_app_mixer_set_mute(node, t, false);
            std::set<int> applied;
            for (int t : want) {
                if (s_prefMuted.count(t)) { applied.insert(t); continue; }
                // Only strips that are not already muted, so releasing the
                // hold cannot un-mute a strip the user muted themselves.
                if (PatchKnob::app::audio_app_mixer_mute(node, t)) continue;
                PatchKnob::app::audio_app_mixer_set_mute(node, t, true);
                applied.insert(t);
            }
            s_prefMuted = applied;
        }
    };
    // Everything a project SWAP has to unbind or reset, minus the engine
    // teardown (New does that itself).  Used by File>Open; File>New keeps its
    // own inline sequence because it also demolishes the patch graph.
    detach_project_editors = [&]{
        // Every floating window is an editor bound to the OLD graph -- exactly
        // the reason New closes them all.  Close the embedded native plugin GUI
        // first (its host plugin instance is about to be destroyed), then hide
        // the rest, keeping only the bottom dock and the arrange window.
        if (g_guiHandle) { PatchKnob::hostwin::editor_close(g_guiHandle);
                           g_guiHandle = nullptr; g_guiNode = -1; }
        wm.cancel_all_interactions(app);
        for (Window* w : wm.windows)
            if (w && w != &patchWin && w != &arrangeWin) w->visible = false;
        bottomTabs.active = 0;                        // back to the Patchbay tab
        // ...and drop what those editors cached.  rackEditor::engine() is a raw
        // non-owning RackEngine* read on every draw(); panelEditor holds the
        // same engine plus a module id.
        rackEditor.set_engine(nullptr); panelEditor.set_target(nullptr,-1);
        paramView.forget_all();          // every plugin instance is about to die
        g_pdNode=g_csoundNode=g_rackNode=g_pdRackMod=g_csoundRackMod=-1;
        g_rackIOsig=g_pdIOsig=-1;
        g_pdPath.clear(); g_csoundPath.clear(); g_rackPath.clear();
        vSampleEd.forget_seq(vSampleEd.bound_seq());  // release any dangling AudioClip*
        // vSamplerEd::bind() stores a pointer INTO this map, so it must not be
        // cleared while that editor is still showing (hidden just above).  It
        // rebuilds itself from the instrument the next time it is opened.
        g_samplerZones.clear(); g_samplerEnv.clear();
        // Mixer strip.  g_curMixerNode names a node in the OLD graph, and the
        // solo/mute flags are the OLD song's: recompute_mutes() reapplies the
        // whole array the next time the user touches solo, so leaving them set
        // silently muted channels of the newly opened project.
        g_curMixerNode = 0; mixerStrip.bus_count = 0;
        std::fill(std::begin(g_busSolo),std::end(g_busSolo),false);
        std::fill(std::begin(g_busMute),std::end(g_busMute),false);
        // Transport/recorder session flags that are NOT in the project file.
        // (LOOP is in the file -- refresh_loaded_project re-derives g_loopOn
        // from the loaded perform rather than forcing it here.)
        g_songMode = true;
        g_countIn = false;
        // Punch state belongs to the session being closed: cancel a running
        // pass (its capture buffers would otherwise block the next take) and
        // drop the enables/counters that name tracks in the OLD project.
        for (const auto& pl : g_punchLanes) {
            PatchKnob::engine::AudioClip discard;
            PatchKnob::app::audio_app_track_capture_end_for(pl.track, discard);
            PatchKnob::app::audio_app_project_hold_mute(pl.track, 0);
        }
        g_punchPassActive = false;
        g_punchLanes.clear(); g_punchRecordingTracks.clear();
        g_punchTracks.clear();
        g_punchPassNumber.clear(); g_recReady = false;
        recordQuantizeTicks=0; recordQuantizeStrength=1.0f;
        recordQuantizeRangePct=0; recordQuantizeSwingPct=50;
    };
    new_project = [&]{
        perf.stop();
        // Editors own non-owning sequence pointers. Flush/cancel while those
        // sequences are still alive, then detach before deleting the models.
        vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
        vPiano.cancel_interaction(app); vPiano.set_sequence(nullptr);
        perf.set_bpm(120.0);
        perf.set_scale_master(-1);
        perf.set_starting_tick(0); perf.set_orig_ticks(0);
        perf.set_left_tick(0); perf.set_right_tick(c_ppqn*16);
        perf.set_looping(false);
        g_loopOn = false;
        g_songMode = true;
        recordQuantizeTicks=0; recordQuantizeStrength=1.0f;
        recordQuantizeRangePct=0; recordQuantizeSwingPct=50;
        g_countIn=false;
        g_recArmed = false;
        g_countInPendingPlay=false; g_countInPendingRecord=false;
        g_countInRecordAwaitingLocate=false; g_countInRecordOriginTick=-1;
        g_countInPreLocateTick=-1;g_countInAwaitMs=0;
        g_recNotes.clear();
        g_recEvents.clear();
        g_recPending.clear();
        g_recMaxTick = 0;
        g_arrangeRecTarget=-1; g_recordTimelineStart=0;
        g_audioTrackCapturing=false; g_recordPreviewAudio=PatchKnob::engine::AudioClip();
        g_arrangeRecInput.clear(); g_arrangeMidiOut.clear(); g_arrangeInstrument.clear();
        g_arrangeAudioIn.clear();  g_arrangeAudioOut.clear();
        if(audio_ok)for(const auto& n:g_midiTrackNode)
            PatchKnob::app::audio_app_midi_track_capture(n.second,false);
        g_seqToTrack.clear(); g_seqRegion.clear(); g_instrTrack.clear(); g_midiTrackNode.clear();
        g_autoMidiEdges.clear();
        g_curMixerNode=0;
        std::fill(std::begin(g_busSolo),std::end(g_busSolo),false);
        std::fill(std::begin(g_busMute),std::end(g_busMute),false);
        autoPlayer.clear();
        for (int sequenceIndex = 0; sequenceIndex < c_max_sequence; ++sequenceIndex)
            if (perf.is_active(sequenceIndex)) {
                vArrange.forget_seq(sequenceIndex); // unbind AudioClip* before owners reset
                perf.delete_sequence(sequenceIndex);
            }
        if(clear_project_edit_state) clear_project_edit_state();
        if (audio_ok) {
            PatchKnob::app::audio_app_record_arm_at_tick(false,0);
            PatchKnob::app::audio_app_patch_set_playing(false);
            PatchKnob::app::audio_app_set_loop_ticks(0,c_ppqn*16,0);
            PatchKnob::app::audio_app_transport_locate(0);
            PatchKnob::app::audio_app_metronome_enable(false);
            PatchKnob::app::audio_app_project_clear_audio_clips();
            PatchKnob::app::audio_app_project_reset_patch();
            PatchKnob::app::audio_app_set_modular(false);
        }
        // Close every floating window and UNBIND the sub-editors.  New tears the
        // whole patch down, so their models are stale -- and a bound RackEngine*
        // (rack editor) or AudioClip* (sample editor) would DANGLE and get drawn.
        // Keep only the bottom dock (the patchbay), reset to its Patchbay tab.
        if (g_guiHandle) { PatchKnob::hostwin::editor_close(g_guiHandle);
                           g_guiHandle = nullptr; g_guiNode = -1; }
        wm.cancel_all_interactions(app);
        for (Window* w : wm.windows)
            if (w && w != &patchWin && w != &arrangeWin) w->visible = false;
        arrangeWin.visible=true; arrangeWin.minimized=false; arrangeWin.maximized=false;
        vArrange.reset_project_state();
        rackEditor.set_engine(nullptr); g_rackNode = -1; g_rackIOsig = -1; g_pdIOsig = -1;
        g_rackPath.clear();g_pdRackMod=-1;g_csoundRackMod=-1;
        pdEditor.set_patch_path("");    g_pdNode = -1; g_pdPath.clear();
        csoundEditor.setText("");       g_csoundNode = -1;g_csoundPath.clear();
        vSampleEd.forget_seq(vSampleEd.bound_seq());   // release any dangling AudioClip*
        g_samplerZones.clear();         g_samplerEnv.clear();
        bottomTabs.active = 0;          // back to the Patchbay tab
        app.animating = false;
        projectPath.clear();
        projectStatus = "Project: Untitled";
        update_project_title();
        if (refresh_loaded_project) refresh_loaded_project();
        // Starting a new project is not an edit of the old one.  on_edit_begin
        // already fired on the mouse-down that opened the File menu, so without
        // this the matching on_edit_end recorded the swap as an ordinary undo
        // step: one Ctrl+Z brought the previous project back, and the next edit
        // then cleared redoStates and threw the new work away.
        if (reset_undo_history) reset_undo_history();
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
        s->set_track_kind(kind);
        s->set_arrange_lane_id(idx);
        s->set_name((kind == 0 ? std::string("Inst ") :
                    (kind == 1 ? std::string("Audio ") : std::string("Automation "))) +
                    std::to_string(idx));
        // Add ONLY the track's ports on the master mixer (instrument=MIDI in/out,
        // audio=AUDIO in/out) -- you patch the synth/effects yourself.
        if (audio_ok && kind != 2) {
            int tr = PatchKnob::app::audio_app_master_add_track(kind == 0 ? 1 : 0);
            if (tr >= 0) { g_seqToTrack[idx] = tr; s->set_midi_bus((char)tr); }
        }
        g_arrangeRecInput[idx]=-1;
        g_arrangeMidiOut[idx]=-1;
        if(kind==2){
            auto& region=autoPlayer.ensureRegion(idx);
            //  AN AUTOMATION LANE INHERITS THE TRACK OF THE LANE ABOVE IT.
            //
            //  An automation lane owns no mixer track of its own (it makes no
            //  sound), so nothing ever put one in g_seqToTrack -- which left
            //  every automation clip's destinationTrack at -1.  Patch-graph
            //  targets route by node id and were fine, but MIDI-CC lanes are
            //  emitted as (track, cc) and every emit site drops them when the
            //  track is negative, and refresh_params() asks
            //  audio_app_track_param_count(-1), which is always 0 -- so the
            //  instrument-parameter list was always empty too.  The lane's CC
            //  automation simply did nothing, silently.
            //
            //  Binding to the nearest lane ABOVE is the convention every DAW
            //  with sub-lane automation uses (Live, Logic), and it is the lane
            //  the user just clicked "add automation" underneath.
            int inherited = -1;
            for (int above = idx - 1; above >= 0; --above) {
                if (!perf.is_active(above)) continue;
                sequence* as = perf.get_sequence(above);
                if (as && as->get_track_kind() == 2) continue;   // skip other automation
                auto it = g_seqToTrack.find(above);
                if (it != g_seqToTrack.end()) { inherited = it->second; break; }
            }
            if (inherited < 0)          // nothing above: take the first mapped track
                for (const auto& kv : g_seqToTrack) { inherited = kv.second; break; }
            if (inherited >= 0) g_seqToTrack[idx] = inherited;
            region.destinationTrack=inherited;region.position=0;
            region.length=std::max<long>(1,s->get_length());region.source=0;
            region.loopLength=region.length;
        }
        rebuild_track_midi_routes();
        // No auto-arm: in SONG mode the timeline triggers gate playback (a clip
        // sounds only where it is placed); LIVE arming is an explicit user act.
        if (!g_songMode) perf.sequence_playing_on(idx);
        if (!seq0) rebind_project_views();
        refresh_master_ui();
        app.request_redraw();
    };
    vArrange.on_track_key = [&](int seq) -> int {
        if(sequence* s=perf.is_active(seq)?perf.get_sequence(seq):nullptr)
            if(s->get_track_kind()==2&&s->get_arrange_lane_id()>=0)
                return 200000+s->get_arrange_lane_id();
        auto it = g_seqToTrack.find(seq);
        return it != g_seqToTrack.end() ? (100000 + it->second) : seq;
    };
    vArrange.on_create_pattern = [&](int sourceSeq, long start, long length,
                                     long offset, bool copyEvents) -> int {
        if (!perf.is_active(sourceSeq)) return -1;
        sequence* src = perf.get_sequence(sourceSeq);
        if (!src) return -1;
        // The visible tracker owns a live editable cache.  Flush it before the
        // copy takes its model snapshot or the most recent automation edits and
        // selected parameter binding remain only in the widget.
        if (vTracker.get_sequence() == src) vTracker.commit_fx();
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
            dst->set_track_kind(src->get_track_kind());
            dst->set_arrange_lane_id(src->get_arrange_lane_id());
            // The per-clip loop belongs to the pattern's SHAPE, exactly like
            // the length/meter copied above -- the copyEvents branch gets it
            // for free from sequence::operator=.  Without it an empty pattern
            // drawn on a ONE-SHOT lane came back repeating, and a lane with a
            // hand-placed loop window handed its new patterns a full-span one.
            // AFTER set_length: set_loop_end() clamps to the current length,
            // and set_loop_start() clamps to the current loop_end.
            dst->set_loop_end(src->get_loop_end());
            dst->set_loop_start(src->get_loop_start());
            dst->set_loop_enabled(src->get_loop_enabled());
        }
        dst->clear_triggers();
        if (length < 1) length = dst->get_length() > 0 ? dst->get_length() : c_ppqn * 4;
        dst->add_trigger(start, length, offset, false);
        std::string base = src->get_name() ? src->get_name() : "Pattern";
        dst->set_name(base + (copyEvents ? " copy " : " pat ") + std::to_string(idx));

        if(src->get_track_kind()==2){
            auto& dr=autoPlayer.ensureRegion(idx);
            dr.id=idx;dr.position=start;dr.length=length;dr.source=offset;
            dr.loopLength=std::max<long>(1,src->get_length());
            auto autoRoute=g_seqToTrack.find(sourceSeq);
            dr.destinationTrack=autoRoute==g_seqToTrack.end()?-1:autoRoute->second;
            if(copyEvents){
                if(const auto* sr=autoPlayer.findRegion(sourceSeq)){
                    const int keepId=dr.id; const int64_t keepPos=dr.position,keepLen=dr.length,keepSrc=dr.source;
                    dr=*sr;dr.id=keepId;dr.position=keepPos;dr.length=keepLen;dr.source=keepSrc;
                }
            }
        }
        else {
            auto& dr=autoPlayer.ensureRegion(idx);dr.id=idx;dr.position=start;dr.length=length;dr.source=offset;
            if(copyEvents)if(const auto* sr=autoPlayer.findRegion(sourceSeq))dr.trackerFx=sr->trackerFx;
        }

        auto route = g_seqToTrack.find(sourceSeq);
        if (route != g_seqToTrack.end()) {
            g_seqToTrack[idx] = route->second;
            dst->set_midi_bus((char)route->second);
        }
        // Instrument choice is shell routing state keyed by sequence, not part
        // of sequence::operator=.  A copied pattern must keep the same selected
        // instrument unless it was deliberately retargeted to another lane.
        auto instrument = g_arrangeInstrument.find(sourceSeq);
        if (instrument != g_arrangeInstrument.end())
            g_arrangeInstrument[idx] = instrument->second;
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
        long sourceOriginTick = 0; // tempo-map origin of source frame zero
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
    //  PLAIN AUDIO LANES AFTER A LOAD.
    //
    //  The project stores audio regions against a mixer TRACK, not against the
    //  sequence that owns them: only freeze records carry a seq->audio link, so
    //  apply_loaded_freezes() below rebinds frozen lanes and nothing rebound
    //  the ordinary ones.  The arrange view then had no clip for them, and --
    //  since the region-id fix -- g_seqRegion had no entry either, so an edit
    //  on such a lane could not address its region and safely did nothing.
    //  Splitting a clip in a freshly loaded project simply had no effect.
    //
    //  The association is recoverable exactly, without a format change: an
    //  audio lane's trigger starts at its region's start, because both were
    //  written from the same state.  Match on that, per track, and every slice
    //  on a track resolves individually (their starts are distinct by
    //  construction -- two regions cannot begin on the same frame of the same
    //  track and still be two clips).
    rebind_loaded_audio_lanes = [&]() {
        if (!audio_ok) return;
        for (int si = 0; si < c_max_sequence; ++si) {
            if (!perf.is_active(si)) continue;
            if (vArrange.audio_clip(si)) continue;          // already bound
            sequence* s = perf.get_sequence(si);
            if (!s || s->get_track_kind() == 2) continue;   // automation owns no audio
            auto tr = g_seqToTrack.find(si);
            if (tr == g_seqToTrack.end()) continue;
            long on = 0, off = 0, ofs = 0; bool sel = false;
            s->reset_draw_trigger_marker();
            if (!s->get_next_trigger(&on, &off, &sel, &ofs)) continue;
            const long long want = PatchKnob::app::audio_app_tick_to_sample(on);

            //  Find the region on this track that starts where the clip does.
            //  Ticks are coarser than samples, so accept the nearest region
            //  within one tick rather than demanding an exact frame match.
            const long long tol = std::max<long long>(
                1, PatchKnob::app::audio_app_tick_to_sample(1) -
                   PatchKnob::app::audio_app_tick_to_sample(0));
            unsigned long long best = 0; long long bestD = tol + 1;
            const int n = PatchKnob::app::audio_app_project_region_count(tr->second);
            for (int i = 0; i < n; ++i) {
                const unsigned long long id =
                    PatchKnob::app::audio_app_project_region_id_at(tr->second, i);
                if (!id) continue;
                long long ps = 0, so = 0, ln = 0; float g = 1.f; int mu = 0, lp = 0;
                if (!PatchKnob::app::audio_app_project_find_region_by_id(
                        tr->second, id, &ps, &so, &ln, &g, &mu, &lp)) continue;
                const long long d = ps > want ? ps - want : want - ps;
                if (d < bestD) { bestD = d; best = id; }
            }
            if (!best) continue;

            const PatchKnob::engine::AudioClip* clip =
                PatchKnob::app::audio_app_project_region_clip(tr->second, best);
            if (!clip) continue;
            long long ps = 0, so = 0, ln = 0; float gain = 1.f; int muted = 0, loop = 0;
            if (!PatchKnob::app::audio_app_project_find_region_by_id(
                    tr->second, best, &ps, &so, &ln, &gain, &muted, &loop)) continue;

            const long posTick = (long)PatchKnob::app::audio_app_sample_to_tick(ps);
            const long endTick = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(ps + ln);
            const long srcTick = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(so);
            const long fullTicks = std::max<long>(1,
                (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                    (long long)clip->numFrames()));
            vArrange.set_audio_clip(si, clip, fullTicks);
            vArrange.set_audio_region(si, posTick, std::max<long>(1, endTick - posTick),
                                      srcTick, gain, muted != 0, loop != 0, 0);
            g_seqRegion[si] = best;        // the edit path can address it now
        }
    };

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
            long long ps=0,so=0,ln=0,lpl=0; float gain=1.f; int muted=0,loop=0;
            long long fIn=0,fOut=0; float fInK=0.f,fOutK=0.f;
            int fInSh=0,fOutSh=0,fInSl=0,fOutSl=0,xLink=0;
            if (!PatchKnob::app::audio_app_project_find_region(tr->second,clip,
                    &ps,&so,&ln,&gain,&muted,&loop,&lpl,
                    &fIn,&fOut,&fInK,&fOutK,&fInSh,&fOutSh,&fInSl,&fOutSl,&xLink)) continue;
            const long posTick=(long)PatchKnob::app::audio_app_sample_to_tick(ps);
            const long endTick=(long)PatchKnob::app::audio_app_sample_to_tick_ceil(ps+ln);
            const long srcTick=(long)PatchKnob::app::audio_app_sample_to_tick_ceil(so);
            const long fullTicks=std::max<long>(1,(long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                                                   (long long)clip->numFrames()));
            const long regionTicks=std::max<long>(1,endTick-posTick);
            vArrange.set_audio_clip(dispSeq, clip, fullTicks);
            g_seqRegion[dispSeq] =
                PatchKnob::app::audio_app_project_region_id_find(tr->second, clip, ps);
            // The loop PERIOD, so the arrange view repeats the waveform on the
            // same span the player wraps on instead of stretching one pass.
            const long loopTicks = lpl > 0
                ? std::max<long>(1,(long)PatchKnob::app::audio_app_sample_to_tick_ceil(lpl)) : 0;
            vArrange.set_audio_region(dispSeq,posTick,regionTicks,srcTick,gain,muted!=0,loop!=0,
                                      loopTicks);
            if (fIn > 0 || fOut > 0) {      // ch.32: fades survive the reload
                arrange::ArrangeView::ClipFade cf;
                cf.inTicks  = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(fIn);
                cf.outTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(fOut);
                cf.inK = fInK; cf.outK = fOutK;
                cf.inShape = fInSh; cf.outShape = fOutSh;
                cf.inSlope = fInSl; cf.outSlope = fOutSl;
                cf.link = xLink;
                vArrange.set_clip_fade(dispSeq, cf);
            }
            FrozenRec rec;
            rec.freezeId = -1;                       // reloaded -> not a live freeze entry
            rec.wasMuted = fr.wasMuted != 0; rec.isTrack = fr.isTrack != 0;
            rec.newSeq = fr.newSeq; rec.srcTrack = fr.srcTrack;
            rec.sourceOriginTick = 0;
            g_frozen[fr.srcSeq] = rec;
            vArrange.set_frozen(fr.srcSeq, true);
        }
        refresh_master_ui();
    };

    vArrange.on_remove_track = [&](int seq){
        sequence* doomed=perf.is_active(seq)?perf.get_sequence(seq):nullptr;
        if(doomed&&vTracker.get_sequence()==doomed) {
            vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
            trackerWin.visible=false;
        }
        if(doomed&&vPiano.get_sequence()==doomed) {
            vPiano.cancel_interaction(app); vPiano.set_sequence(nullptr);
            pianoWin.visible=false;
        }
        if(vAuto.region_id()==seq){automationWin.visible=false;vAuto.set_target(nullptr,0);}
        autoPlayer.removeRegion(seq);
        // Deleting the lane a take is aimed at must stop and disarm it FIRST.
        // Otherwise recording stayed armed on a dead lane: the capture kept
        // running against a stale g_recordTimelineStart, and the next take
        // inherited that origin -- which is what made "delete the clip and
        // record again" start in the deleted clip's old position.
        if (g_arrangeRecTarget == seq) {
            if (g_recArmed && toggle_record) toggle_record();
            g_arrangeRecTarget = -1;
            g_recordTimelineStart = 0;
            g_recNotes.clear();g_recEvents.clear(); g_recPending.clear(); g_recMaxTick = 0;
            g_audioTrackCapturing = false; g_recTargetWasAudio = false;
            vArrange.clear_record_preview();
            g_recordPreviewAudio = PatchKnob::engine::AudioClip();
            PatchKnob::app::audio_app_midi_track_capture(
                midi_track_for_seq(seq),false);
        }
        g_arrangeRecInput.erase(seq); g_arrangeMidiOut.erase(seq);
        g_arrangeInstrument.erase(seq);
        g_arrangeAudioIn.erase(seq);  g_arrangeAudioOut.erase(seq);
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
        // The route map is purged inside the audio_ok branch above; with audio
        // disabled that branch never runs, so the entry survived and a recycled
        // sequence index inherited the dead track's routing.
        g_seqToTrack.erase(seq);
        g_seqRegion.erase(seq);
        if (perf.is_active(seq)) perf.delete_sequence(seq);
        rebuild_track_midi_routes();
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
        const long long spanStartSamp =
            PatchKnob::app::audio_app_tick_to_sample((long long)startTick);
        const long long spanEndSamp =
            PatchKnob::app::audio_app_tick_to_sample((long long)endTick + 1);
        const long long endSamp = std::max<long long>(1,spanEndSamp-spanStartSamp);
        const double seconds = (double)endSamp / sr + 2.0;
        const int chan = s->get_midi_channel() & 0x0F;
        long patLen = s->get_length(); if (patLen < 1) patLen = c_ppqn * 4;
        outClip.name = std::string(s->get_name()) + " (frozen)";
        // Capture every MIDI event, not only linked note pairs.  Tracker data,
        // imported files and live recordings can legitimately contain
        // unlinked note-ons/offs, CCs, pressure and pitch bend.  The old linked
        // note iterator silently discarded all of those and produced empty or
        // partial freezes.
        struct FreezeMidiEv { long tick; unsigned char status, d0, d1; };
        std::vector<FreezeMidiEv> midiEvents;
        std::set<std::pair<unsigned char,unsigned char>> midiKinds;
        s->reset_draw_marker();
        unsigned char kindStatus=0, kindCc=0;
        while (s->get_next_event(&kindStatus, &kindCc))
            midiKinds.insert({kindStatus,
                (unsigned char)(((kindStatus & 0xF0) == EVENT_CONTROL_CHANGE) ? kindCc : 0)});
        for (const auto& kind : midiKinds) {
            s->reset_draw_marker();
            long et=0; unsigned char d0=0,d1=0; bool selected=false;
            while (s->get_next_event(kind.first, kind.second, &et, &d0, &d1, &selected))
                midiEvents.push_back({et,kind.first,d0,d1});
        }
        // Order EXACTLY as the engine does: by tick, and within a tick by
        // event::get_rank(), which puts a note-OFF (0x080) ahead of a note-ON
        // (0x090).
        //
        // This used to be a bare std::sort on tick alone.  std::sort is not
        // stable, so events sharing a tick came out in an arbitrary -- and
        // run-dependent -- order.  Back-to-back notes put the previous note's
        // OFF on the same tick as the next note's ON, so whenever the sort
        // happened to emit the ON first, the OFF that followed killed the note
        // it had just started.  On a kick loop that reads as notes randomly
        // missing from the freeze, differing between takes: exactly the
        // "sometimes" in the report.
        auto rank = [](unsigned char status) -> int {
            switch (status & 0xF0) {
                case EVENT_NOTE_OFF:        return 0x080;
                case EVENT_NOTE_ON:         return 0x090;
                case EVENT_AFTERTOUCH:
                case EVENT_CHANNEL_PRESSURE:
                case EVENT_PITCH_WHEEL:     return 0x050;
                case EVENT_CONTROL_CHANGE:  return 0x010;
                case EVENT_PROGRAM_CHANGE:  return 0x000;
                default:                    return 0;
            }
        };
        std::stable_sort(midiEvents.begin(), midiEvents.end(),
                  [&](const FreezeMidiEv& a,const FreezeMidiEv& b){
                      if (a.tick != b.tick) return a.tick < b.tick;
                      return rank(a.status) < rank(b.status);
                  });
        // Render the PLAYLIST, not a free-running pattern. Each trigger supplies
        // its own timeline bounds and source phase/offset; gaps remain silent.
        auto scheduleMidi = [&]() {
            // Diagnostic: what the bounce ACTUALLY schedules.  Missing notes and
            // phantom notes past the material are both visible here as counts and
            // tick bounds, instead of having to be inferred from the audio.
            int nOn = 0, nOff = 0, nOther = 0, nDropped = 0;
            long minT = 0, maxT = 0; bool anyT = false;
            long trOn=0,trOff=0,trOffset=0; bool trSel=false;
            s->reset_draw_trigger_marker();
            while(s->get_next_trigger(&trOn,&trOff,&trSel,&trOffset)) {
                if(trOff<startTick||trOn>endTick) continue;
                long firstMarker=trOn-(trOn%patLen)+(trOffset%patLen)-patLen;
                for(long marker=firstMarker;marker<=trOff;marker+=patLen) {
                    for (const FreezeMidiEv& ev : midiEvents) {
                        if (ev.tick >= patLen) continue;
                        const long absTick=marker+ev.tick;
                        // A note-OFF closing a note that started inside this
                        // trigger can legitimately fall PAST the trigger's end
                        // (a note held across the boundary, or one ending on it).
                        // The bounds test dropped those, so the note-on was
                        // rendered with nothing to stop it and the voice ran on
                        // until the end-of-render CC123 -- a kick turning into a
                        // drone, or the tail of the clip smeared over.
                        // Let offs through to the render's end; a stray off for a
                        // note that is not sounding is harmless.
                        const bool isOff = ((ev.status & 0xF0) == EVENT_NOTE_OFF) ||
                                           ((ev.status & 0xF0) == EVENT_NOTE_ON && ev.d1 == 0);
                        long at = absTick;
                        if (isOff) {
                            if (at < trOn || at < startTick) continue;
                            if (at > endTick) at = endTick;
                        } else {
                            if(at<trOn||at>trOff||at<startTick||at>endTick) continue;
                        }
                        unsigned char status=ev.status;
                        if ((status & 0xF0) != 0xF0)
                            status=(unsigned char)((status & 0xF0) | chan);
                        PatchKnob::app::audio_app_route_midi(
                            srcTrack,status,ev.d0,ev.d1,at);
                    }
                }
            }
            std::fprintf(stderr,
                "[freeze-sched] patLen=%ld srcEvents=%d | scheduled on=%d off=%d "
                "other=%d dropped=%d | ticks %ld..%ld | span %ld..%ld\n",
                patLen, (int)midiEvents.size(), nOn, nOff, nOther, nDropped,
                anyT ? minT : -1, anyT ? maxT : -1, startTick, endTick);
        };
        // Freeze runs without the UI animation loop, so advance every automation
        // source explicitly at offline audio-block cadence. Timeline lanes are
        // project-time based; tracker FX are pattern-row based. Piano-roll CCs
        // are already included in midiEvents above and retain sample scheduling.
        autoPlayer.resetEmitState();
        long long lastFxTick = -1;   // tick-driven; -1 == not yet inside the clip
        // Pattern FX may TRIGGER NOTES (retrigger, arpeggio).  They were advanced
        // on every row change for the whole render -- which runs 2 s past the
        // material for release tails, and sweeps straight through the gaps
        // between clips.  So the bounce grew notes after the last clip and in
        // holes where the pattern is not playing at all.  Confine them to the
        // spans where a trigger actually exists.
        std::vector<std::pair<long,long>> fxSpans;
        {
            s->reset_draw_trigger_marker();
            long a=0,b=0,o=0; bool sel=false;
            while (s->get_next_trigger(&a,&b,&sel,&o))
                if (b >= startTick && a <= endTick) fxSpans.push_back({a,b});
        }
        auto insideClip = [&fxSpans](long long t) {
            for (size_t i = 0; i < fxSpans.size(); ++i)
                if (t >= fxSpans[i].first && t <= fxSpans[i].second) return true;
            return false;
        };
        auto advanceAutomation = [&](long long fromTick,long long toTick) {
            autoPlayer.advanceScheduled(fromTick,toTick,
                [&](int trk,unsigned id,float v,int64_t due) {
                    if (trk == srcTrack)
                        PatchKnob::app::audio_app_route_param_at_tick(trk,id,v,due);
                },
                [&](int trk,int cc,int value,int64_t due) {
                    if (trk == srcTrack)
                        PatchKnob::app::audio_app_route_midi(
                            trk,0xB0,(unsigned char)cc,(unsigned char)value,due);
                },
                // Patch-graph track lanes must be in the freeze too, or frozen
                // audio does not match what you were just listening to.
                [&](int trk,const PatchKnob::engine::LaneTarget& target,float v,int64_t due) {
                    using PatchKnob::engine::LaneTargetKind;
                    if (target.kind==LaneTargetKind::PatchParam)
                        PatchKnob::app::audio_app_patch_route_param(target.node,target.id,v);
                    else if (target.kind==LaneTargetKind::RackParam) {
                        rackx::RackEngine* eng =
                            PatchKnob::app::audio_app_rack_engine(target.node);
                        if (eng) eng->setParam(target.module,(int)target.id,
                            target.minValue+(target.maxValue-target.minValue)*v);
                    } else if (trk==srcTrack)
                        PatchKnob::app::audio_app_route_param_at_tick(trk,target.id,v,due);
                });
            // AUTOMATION CLIPS (regions) are a separate pass from track lanes.
            // Live playback runs both; the bounce ran only advance(), so every
            // automation clip -- including any pointed at a rack module -- was
            // silently absent from a freeze.  The frozen audio then did not match
            // what you had just been listening to.
            autoPlayer.advanceRegionsScheduled(fromTick,toTick,
                [&](int trk,const PatchKnob::engine::LaneTarget& target,float v,int64_t due) {
                    using PatchKnob::engine::LaneTargetKind;
                    if (target.kind==LaneTargetKind::PatchParam)
                        PatchKnob::app::audio_app_patch_route_param(target.node,target.id,v);
                    else if (target.kind==LaneTargetKind::RackParam) {
                        rackx::RackEngine* eng =
                            PatchKnob::app::audio_app_rack_engine(target.node);
                        if (eng) eng->setParam(target.module,(int)target.id,
                            target.minValue+(target.maxValue-target.minValue)*v);
                    } else if (target.kind==LaneTargetKind::VstParam && trk==srcTrack)
                        PatchKnob::app::audio_app_route_param_at_tick(trk,target.id,v,due);
                },
                [&](int trk,int cc,int value,int64_t due) {
                    if (trk == srcTrack)
                        PatchKnob::app::audio_app_route_midi(
                            trk,0xB0,(unsigned char)cc,(unsigned char)value,due);
                });
            if (toTick > (long long)endTick || !insideClip(toTick)) {
                lastFxTick = -1;         // re-enter the next clip from a clean tick
                return;
            }
            // Drive FX by TICK. This used to quantise the playhead onto a
            // hardcoded LPB-4 row grid, so a pattern authored at a finer LPB had
            // most of its automation silently dropped on the way out.
            const long long local = (long long)((toTick % patLen + patLen) % patLen);
            if (local != lastFxTick) {
                ui::TrackerView::play_pattern_fx(s, lastFxTick, local);
                lastFxTick = local;
            }
        };
        const bool ok = PatchKnob::app::audio_app_freeze_render_at(
            srcTrack, spanStartSamp, seconds, scheduleMidi, advanceAutomation, outClip);
        // Normal playback must re-emit the value at its resumed playhead rather
        // than inheriting the memo from the offline pass.
        autoPlayer.resetEmitState();
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
        rec.sourceOriginTick = st;
        s->set_song_mute(true);
        g_frozen[seq] = rec;
        vArrange.set_frozen(seq, true);
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
            long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick(
                startSample+(long long)disp->numFrames())-st;
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
        long maxTick = s->get_max_trigger();
        if (maxTick <= 0) maxTick = std::max<long>(0,s->get_length()-1);
        // Anchor the freeze at the lane's FIRST clip, not at tick 0.  Everything
        // downstream derives from startTick -- the render span, the attach sample
        // and the clip block's position -- so hard-coding 0 placed the frozen
        // lane at the start of the timeline no matter where its material began.
        // A lane whose first clip sits at bar 3 came back as a block starting at
        // bar 1: the audio was right (silent at the head) but the whole clip sat
        // bars to the LEFT of the part it was made from.
        long firstTick = -1;
        {
            s->reset_draw_trigger_marker();
            long trOn = 0, trOff = 0, trOffs = 0; bool trSel = false;
            while (s->get_next_trigger(&trOn, &trOff, &trSel, &trOffs))
                if (firstTick < 0 || trOn < firstTick) firstTick = trOn;
        }
        if (firstTick < 0) firstTick = 0;              // no triggers: whole lane
        const long startTick = firstTick, endTick = maxTick;

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
        long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick(
            PatchKnob::app::audio_app_tick_to_sample(startTick)+(long long)clip.numFrames())-startTick;
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
        rec.sourceOriginTick = startTick;
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
        // The arrange view stores a non-owning waveform pointer. Clear it BEFORE
        // detach frees the FreezeEntry's AudioClip; waiting until later leaves a
        // draw/re-entrant redraw window that dereferences released memory.
        const int dispSeq = rec.isTrack ? rec.newSeq : srcSeq;
        if (rec.isTrack) vArrange.forget_seq(dispSeq);
        else             vArrange.set_audio_clip(dispSeq, nullptr);
        // If the sample editor is showing this freeze's clip, unbind it FIRST --
        // detach is about to free the AudioClip and a later draw would deref it.
        vSampleEd.forget_seq(srcSeq);
        if (rec.newSeq >= 0) vSampleEd.forget_seq(rec.newSeq);
        if (rec.freezeId >= 0) {
            PatchKnob::app::audio_app_freeze_detach(rec.freezeId);   // live freeze entry
        } else {
            // RELOADED freeze: the audio is a project clip -> stop it on its lane.
            auto tr = g_seqToTrack.find(dispSeq);
            if (tr != g_seqToTrack.end()) PatchKnob::app::audio_app_project_clear_track(tr->second);
        }
        if (rec.isTrack && rec.srcTrack >= 0)
            PatchKnob::app::audio_app_set_track_disabled(rec.srcTrack, false);   // re-enable devices
        if (sequence* s = perf.is_active(srcSeq) ? perf.get_sequence(srcSeq) : nullptr)
            s->set_song_mute(unmuteSource ? rec.wasMuted : true);
        if (rec.isTrack) {
            vArrange.forget_seq(rec.newSeq);        // idempotent map purge
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
            if(perf.is_active(rec.newSeq)) {
                sequence* doomed=perf.get_sequence(rec.newSeq);
                if(vTracker.get_sequence()==doomed) {
                    vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
                    trackerWin.visible=false;
                }
                if(vPiano.get_sequence()==doomed) {
                    vPiano.cancel_interaction(app); vPiano.set_sequence(nullptr);
                    pianoWin.visible=false;
                }
                perf.delete_sequence(rec.newSeq);
            }
            rebind_project_views();
            refresh_master_ui();
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
    clear_project_edit_state=[&]{
        // Reclaim the PREVIOUS generation of detached freeze audio first: by now
        // every view has been rebound and redrawn, so nothing can still point at
        // it.  (Detach parks clips instead of freeing them -- see
        // audio_app_freeze_gc.)
        PatchKnob::app::audio_app_freeze_gc();
        for(auto& kv:g_frozen)
            if(kv.second.freezeId>=0) PatchKnob::app::audio_app_freeze_detach(kv.second.freezeId);
        g_frozen.clear();
        for(const ClipUndo& u:g_clipUndo) if(!u.blob.empty()) std::remove(u.blob.c_str());
        g_clipUndo.clear(); g_clipUndoNextId=0;
    };

    auto undo_dir = []() -> std::string {
        char* pref = SDL_GetPrefPath("PatchKnob", "PatchKnob");
        std::string d = pref ? std::string(pref) + "undo" : "PatchKnob_undo";
        if (pref) SDL_free(pref);
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
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
        if(!audio_ok)return;
        if(srcSeq<0) {
            // Ordinary recorded/imported audio is owned by the project player,
            // not the freeze table. The old callback simply returned here,
            // leaving both the schedule and multi-megabyte source alive.
            const auto tr=g_seqToTrack.find(seq);
            const unsigned long long rid=region_id_for_seq(seq);
            const PatchKnob::engine::AudioClip* clip=vArrange.audio_clip(seq);
            if(tr!=g_seqToTrack.end()&&rid)
                PatchKnob::app::audio_app_project_remove_region_by_id(tr->second,rid);
            else if(tr!=g_seqToTrack.end()&&clip)
                PatchKnob::app::audio_app_project_remove_audio_clip(tr->second,clip);
            g_seqRegion.erase(seq);
            if(sequence* s=perf.is_active(seq)?perf.get_sequence(seq):nullptr) {
                s->clear_triggers();
                s->clear_events();
                s->discard_edit_history();
            }
            vSampleEd.forget_seq(seq);
            vArrange.forget_seq(seq);
            app.request_redraw();
            return;
        }
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
            u.blob = (std::filesystem::path(undo_dir()) /
                      ("clip_" + std::to_string(g_clipUndoNextId++) + ".raw")).string();
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

    vArrange.on_midi_clip_delete=[&](int seq){
        sequence* s=perf.is_active(seq)?perf.get_sequence(seq):nullptr;if(!s)return;
        bool hasTrigger=false;long on=0,off=0,offset=0;bool selected=false;
        s->reset_draw_trigger_marker();
        hasTrigger=s->get_next_trigger(&on,&off,&selected,&offset);
        if(hasTrigger)return;
        autoPlayer.removeRegion(seq);
        if(vAuto.region_id()==seq){automationWin.visible=false;vAuto.set_target(nullptr,0);}
        // No region references this source anymore: destroy its event array.
        // If another sequence represents the lane, retire this sequence slot as
        // well; otherwise retain one empty sequence as the track header/model.
        s->clear_events();
        s->discard_edit_history();
        const auto route=g_seqToTrack.find(seq);bool laneHasOther=false;
        if(route!=g_seqToTrack.end())for(const auto& kv:g_seqToTrack)
            if(kv.first!=seq&&kv.second==route->second&&perf.is_active(kv.first))
                {laneHasOther=true;break;}
        if(laneHasOther) {
            if(vTracker.get_sequence()==s) {vTracker.cancel_interaction(app);
                                            vTracker.set_sequence(nullptr,-1);}
            if(vPiano.get_sequence()==s) {vPiano.cancel_interaction(app);
                                          vPiano.set_sequence(nullptr);}
            vArrange.forget_seq(seq);
            g_seqToTrack.erase(seq);g_seqRegion.erase(seq);g_arrangeRecInput.erase(seq);g_arrangeMidiOut.erase(seq);
            g_arrangeInstrument.erase(seq);g_arrangeAudioIn.erase(seq);g_arrangeAudioOut.erase(seq);
            perf.delete_sequence(seq);
        }
        app.request_redraw();
    };

    // Returns the DISPLAY seq the restored clip came back on (-1 = nothing
    // restored) so the arrange view's Multiple-Undo queue can redo the delete.
    vArrange.on_restore_deleted_clip = [&, load_clip]() -> int {
        if (!audio_ok || g_clipUndo.empty()) { app.request_redraw(); return -1; }
        const ClipUndo u = g_clipUndo.back(); g_clipUndo.pop_back();
        PatchKnob::engine::AudioClip clip;
        if (!load_clip(u.blob, clip) || clip.empty()) { std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
        const long long startSample = PatchKnob::app::audio_app_tick_to_sample((long long)(u.posTick < 0 ? 0 : u.posTick));

        int restoredSeq = -1;
        if (u.isTrack) {
            int newSeq = (u.newSeq >= 0 && !perf.is_active(u.newSeq)) ? u.newSeq : -1;
            if (newSeq < 0) for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { newSeq = i; break; }
            if (newSeq < 0) { std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
            perf.new_sequence(newSeq); perf.set_active(newSeq, true);
            sequence* ns = perf.get_sequence(newSeq);
            if (!ns) { std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
            ns->set_name(u.name.empty() ? std::string("restored") : u.name);
            long lenTick = u.lenTick > 0 ? u.lenTick
                          : (long)PatchKnob::app::audio_app_sample_to_tick((long long)clip.numFrames());
            ns->add_trigger(u.posTick, lenTick, 0);
            const int newTrack = PatchKnob::app::audio_app_master_add_track(0);
            if (newTrack < 0) { perf.delete_sequence(newSeq); std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
            g_seqToTrack[newSeq] = newTrack;
            const int id = PatchKnob::app::audio_app_freeze_attach(newTrack, clip, startSample, u.gain);
            if (id < 0) { PatchKnob::app::audio_app_master_remove_track(newTrack); g_seqToTrack.erase(newSeq);
                          perf.delete_sequence(newSeq); std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
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
            restoredSeq = newSeq;
            rebind_project_views(); refresh_master_ui();
        } else {
            const int id = PatchKnob::app::audio_app_freeze_attach(-1, clip, startSample, u.gain);
            if (id < 0) { std::remove(u.blob.c_str()); app.request_redraw(); return -1; }
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
            restoredSeq = u.srcSeq;
        }
        std::remove(u.blob.c_str());
        app.request_redraw();
        return restoredSeq;
    };

    // Clip crossfade: the displayed audio clip's seq maps to a freeze id (clip
    // freeze keys g_frozen by seq; track freeze stores it under newSeq).
    vArrange.on_clip_fade = [&](int seq, const arrange::ArrangeView::ClipFade& f){
        int freezeId = -1;
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) freezeId = it->second.freezeId;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { freezeId = kv.second.freezeId; break; }
        const long long inF  = PatchKnob::app::audio_app_tick_to_sample(f.inTicks)-
                               PatchKnob::app::audio_app_tick_to_sample(0);
        const long long outF = PatchKnob::app::audio_app_tick_to_sample(f.outTicks)-
                               PatchKnob::app::audio_app_tick_to_sample(0);
        if (freezeId >= 0) {
            PatchKnob::app::audio_app_freeze_set_fades(freezeId,inF,outF,f.inK,f.outK);
            PatchKnob::app::audio_app_freeze_set_fade_shapes(freezeId,f.inShape,f.outShape,
                                                             f.inSlope,f.outSlope,f.link);
        } else {
            auto tr=g_seqToTrack.find(seq); const unsigned long long rid=region_id_for_seq(seq);
            if(tr!=g_seqToTrack.end()&&rid) {
                PatchKnob::app::audio_app_project_set_fades_by_id(
                    tr->second,rid,inF,outF,f.inK,f.outK);
                PatchKnob::app::audio_app_project_set_fade_shapes_by_id(
                    tr->second,rid,f.inShape,f.outShape,f.inSlope,f.outSlope,f.link);
            }
        }
        app.request_redraw();
    };
    // ch.32 fade AUDITION: play the fade window through the real signal path.
    // Seek to the pre-roll point and roll the transport; the view stops it at
    // the post-roll point (audition_poll) through on_audition_stop.
    vArrange.on_audition_start = [&](long startTick, long endTick){
        (void)endTick;
        perf.stop();
        PatchKnob::app::audio_app_patch_set_playing(false);
        if (vArrange.on_seek) vArrange.on_seek(startTick);
        perf.start(g_songMode);
        PatchKnob::app::audio_app_patch_set_playing(true);
        app.animating = true;
        app.request_redraw();
    };
    vArrange.on_audition_stop = [&](){
        perf.stop();
        PatchKnob::app::audio_app_patch_set_playing(false);
        app.request_redraw();
    };
    // AutoFades preference (PT p752): push into every engine clip player.
    vArrange.on_auto_fade_changed = [&](int ms){
        PatchKnob::app::audio_app_set_auto_fade_ms((double)ms);
    };
    // ch.32 p761: which AUDIO lane an automation clip targets, so the
    // automation lane can draw that clip's fade boundaries + shapes.
    vArrange.on_automation_dest_seq = [&](int seq) -> int {
        for (const auto& rg : autoPlayer.regions())
            if (rg.id == seq) {
                for (const auto& kv : g_seqToTrack)
                    if (kv.second == rg.destinationTrack &&
                        vArrange.audio_clip(kv.first))
                        return kv.first;
                break;
            }
        return -1;
    };
    // the view loads the preference file in its constructor, before the hook
    // above exists -- apply the loaded value now
    PatchKnob::app::audio_app_set_auto_fade_ms((double)vArrange.auto_fade_ms());

    // Moving/trimming an audio/frozen clip pushes its new REGION to the engine so
    // playback follows non-destructively: position, span, and start-offset into
    // the source all come from the clip's (start, length, offset) trigger.
    vArrange.on_clip_region_changed = [&](int seq, long startTick, long lengthTick, long offsetTick){
        if(sequence* as=perf.is_active(seq)?perf.get_sequence(seq):nullptr)
            if(as->get_track_kind()==2){auto& ar=autoPlayer.ensureRegion(seq);
                ar.position=startTick;ar.length=std::max<long>(1,lengthTick);ar.source=std::max<long>(0,offsetTick);
                if(ar.loopLength<1)ar.loopLength=std::max<long>(1,as->get_length());}
        if (!audio_ok) return;
        int freezeId = -1;
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) freezeId = it->second.freezeId;
        else for (auto& kv : g_frozen) if (kv.second.newSeq == seq) { freezeId = kv.second.freezeId; break; }
        const long long posS = PatchKnob::app::audio_app_tick_to_sample((long long)(startTick  < 0 ? 0 : startTick));
        long origin=0;
        if(it!=g_frozen.end()) origin=it->second.sourceOriginTick;
        else for(auto& kv:g_frozen) if(kv.second.newSeq==seq){origin=kv.second.sourceOriginTick;break;}
        const long offTick=offsetTick<0?0:offsetTick;
        const long lenTick=lengthTick<1?1:lengthTick;
        const long long originS=PatchKnob::app::audio_app_tick_to_sample(origin);
        const long long offS=PatchKnob::app::audio_app_tick_to_sample(origin+offTick)-originS;
        const long long lenS=PatchKnob::app::audio_app_tick_to_sample(startTick+lenTick)-posS;
        if(freezeId>=0) PatchKnob::app::audio_app_freeze_set_region(freezeId,posS,offS,lenS);
        else {
            // STABLE IDENTITY: address the seq's own region by id.  The old
            // (track, clip-pointer) call resolved to the FIRST region over the
            // shared source, so once a split/duplicate put a second slice on
            // the track, edits could land on the wrong slice -- or, aimed at a
            // later slice, silently do nothing (the "cut that doesn't cut").
            auto tr=g_seqToTrack.find(seq);
            const unsigned long long rid=region_id_for_seq(seq);
            if(tr!=g_seqToTrack.end()&&rid) PatchKnob::app::audio_app_project_set_region_by_id(
                tr->second,rid,posS,offS,lenS);
        }
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
        const PatchKnob::engine::AudioClip* src = srcId>=0
            ? PatchKnob::app::audio_app_freeze_clip(srcId) : vArrange.audio_clip(srcSeq);
        if (!src) return;
        long long oldPos=0,oldOff=0,oldLen=0; float oldGain=1.f; int oldMuted=0,oldLoop=0;
        if(srcId>=0) PatchKnob::app::audio_app_freeze_get_region(srcId,&oldPos,&oldOff,&oldLen,
                                                                 &oldGain,&oldMuted,&oldLoop);
        else { auto st=g_seqToTrack.find(srcSeq); if(st!=g_seqToTrack.end()) {
            // Resolve the SOURCE seq's own region id NOW, while it is still
            // unambiguous -- the attach below adds a second region over the
            // same clip, after which a pointer lookup could hit either slice.
            const unsigned long long srid=region_id_for_seq(srcSeq);
            if(srid) PatchKnob::app::audio_app_project_find_region_by_id(
                         st->second,srid,&oldPos,&oldOff,&oldLen,
                         &oldGain,&oldMuted,&oldLoop);
            else PatchKnob::app::audio_app_project_find_region(st->second,src,&oldPos,&oldOff,&oldLen,
                                                               &oldGain,&oldMuted,&oldLoop); } }
        // The copy's lane inherits the source's mixer track (on_create_pattern);
        // fall back to a fresh audio track if it somehow has none.
        int newTrack = -1;
        auto tr = g_seqToTrack.find(newSeq);
        if (tr != g_seqToTrack.end()) newTrack = tr->second;
        else { newTrack = PatchKnob::app::audio_app_master_add_track(0); if (newTrack >= 0) g_seqToTrack[newSeq] = newTrack; }
        if (newTrack < 0) return;
        const long long posS = PatchKnob::app::audio_app_tick_to_sample((long long)(startTick < 0 ? 0 : startTick));
        // A slice is a new REGION over the same immutable source. Copying the
        // entire AudioClip here made N cuts consume N times the RAM and forced
        // the renderer/history writer to walk all those duplicate buffers.
        const long offTick=srcOffTick<0?0:srcOffTick, useLen=lenTick<1?1:lenTick;
        const long long offS=PatchKnob::app::audio_app_tick_to_sample(offTick)-
                             PatchKnob::app::audio_app_tick_to_sample(0);
        const long long lenS=PatchKnob::app::audio_app_tick_to_sample(startTick+useLen)-posS;
        const int newId = PatchKnob::app::audio_app_freeze_attach_shared(newTrack,src,posS,oldGain);
        if (newId >= 0) {
            PatchKnob::app::audio_app_freeze_set_region(newId,posS,offS,lenS);
            PatchKnob::app::audio_app_freeze_set_muted(newId,oldMuted!=0);
            PatchKnob::app::audio_app_freeze_set_loop(newId,oldLoop!=0);
            FrozenRec rec; rec.freezeId = newId; rec.isTrack = true; rec.newSeq = newSeq; rec.srcTrack = -1;
            g_frozen[newSeq] = rec;
            if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(newId)) {
                long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil((long long)disp->numFrames());
                vArrange.set_audio_clip(newSeq, disp, clipTicks);
            }
        } else if (PatchKnob::app::audio_app_project_add_region_shared(
                       newTrack, src, posS, offS, lenS, oldGain, 0, 0)) {
            // attach_shared could not adopt the source (it only owns project /
            // freeze clips): schedule a plain project region instead and hold
            // its stable id.  The old code RETURNED here, so the view showed a
            // second clip that had no engine region at all -- and its next
            // region update, addressed by (track, clip pointer), landed on the
            // ORIGINAL slice: the drawn cut never became an audible cut.
            const unsigned long long rid =
                PatchKnob::app::audio_app_project_last_region_id(newTrack);
            g_seqRegion[newSeq] = rid;
            PatchKnob::app::audio_app_project_set_muted_by_id(newTrack, rid, oldMuted!=0);
            PatchKnob::app::audio_app_project_set_loop_by_id(newTrack, rid, oldLoop!=0);
            long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil((long long)src->numFrames());
            vArrange.set_audio_clip(newSeq, src, clipTicks);
        } else {
            projectStatus = "Clip copy could not attach audio (source not project-owned)";
            return;
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
        if(id>=0) PatchKnob::app::audio_app_freeze_set_gain(id,gain);
        else { auto tr=g_seqToTrack.find(seq); const unsigned long long rid=region_id_for_seq(seq);
               if(tr!=g_seqToTrack.end()&&rid) PatchKnob::app::audio_app_project_set_gain_by_id(tr->second,rid,gain); }
        app.request_redraw();
    };
    vArrange.on_clip_mute = [&, freeze_id_for_seq](int seq, bool muted){
        if (!audio_ok) return; int id = freeze_id_for_seq(seq);
        if(id>=0) PatchKnob::app::audio_app_freeze_set_muted(id,muted);
        else { auto tr=g_seqToTrack.find(seq); const unsigned long long rid=region_id_for_seq(seq);
               if(tr!=g_seqToTrack.end()&&rid) PatchKnob::app::audio_app_project_set_muted_by_id(tr->second,rid,muted); }
        app.request_redraw();
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
        if(id>=0) PatchKnob::app::audio_app_freeze_set_loop(id,loop);
        else { auto tr=g_seqToTrack.find(seq); const unsigned long long rid=region_id_for_seq(seq);
               if(tr!=g_seqToTrack.end()&&rid) {
                   PatchKnob::app::audio_app_project_set_loop_by_id(tr->second,rid,loop);
                   // setClipLoop() captures the region's CURRENT trimmed span as
                   // the period; tell the view what it turned out to be so the
                   // waveform repeats on the same span the player wraps on.
                   long long lpl=0;
                   if(PatchKnob::app::audio_app_project_find_region_by_id(tr->second,rid,
                          nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&lpl))
                       vArrange.set_audio_region_loop_length(seq, lpl>0
                           ? std::max<long>(1,(long)PatchKnob::app::audio_app_sample_to_tick_ceil(lpl))
                           : 0);
               } }
        app.request_redraw();
    };

    // ---- ch.29/31 shell hooks: TCE stretch, loop period, scrub audition ----
    // Shared teardown of a seq's CURRENT audio attachment (freeze entry or
    // project clip) so a rendered replacement can take its place.
    auto detach_seq_audio = [&, freeze_id_for_seq](int seq){
        int oldId = -1, key = -1;
        auto it = g_frozen.find(seq);
        if (it != g_frozen.end()) { oldId = it->second.freezeId; key = it->first; }
        else for (auto& kv : g_frozen)
            if (kv.second.newSeq == seq) { oldId = kv.second.freezeId; key = kv.first; break; }
        if (oldId >= 0) {
            PatchKnob::app::audio_app_freeze_detach(oldId);
            if (key >= 0) g_frozen.erase(key);
        } else {
            // Remove exactly THIS seq's region: the pointer-based remove took
            // out the first region over the shared source, which after a split
            // could be a different slice than the one being torn down.
            auto tr = g_seqToTrack.find(seq);
            const unsigned long long rid = region_id_for_seq(seq);
            if (tr != g_seqToTrack.end() && rid)
                PatchKnob::app::audio_app_project_remove_region_by_id(tr->second, rid);
        }
        g_seqRegion.erase(seq);
    };
    // Read a seq's engine region wherever it lives (freeze entry or project).
    auto region_of_seq = [&, freeze_id_for_seq](int seq, long long* pos, long long* off,
                                                long long* len, float* gain,
                                                int* muted, int* loop) -> bool {
        const int id = freeze_id_for_seq(seq);
        if (id >= 0)
            return PatchKnob::app::audio_app_freeze_get_region(id, pos, off, len,
                                                               gain, muted, loop);
        auto tr = g_seqToTrack.find(seq);
        const unsigned long long rid = region_id_for_seq(seq);
        if (tr == g_seqToTrack.end() || !rid) return false;
        return PatchKnob::app::audio_app_project_find_region_by_id(tr->second, rid, pos,
                                                                   off, len, gain, muted, loop);
    };

    // TCE Trim / TCE-to-Timeline: render the region's source window through the
    // pitch-preserving stretcher (engine::warp_to_length) to the new length,
    // attach the render as the clip's audio and re-seat the region -- the drawn
    // span and the audio then agree, which is the whole point of TCE.
    vArrange.on_clip_tce = [&, detach_seq_audio, region_of_seq](int seq, long newLenTicks){
        if (!audio_ok || newLenTicks < 1) return;
        const PatchKnob::engine::AudioClip* src = vArrange.audio_clip(seq);
        if (!src || src->empty()) return;
        long long posS = 0, offS = 0, lenS = 0; float gain = 1.f; int muted = 0, loop = 0;
        if (!region_of_seq(seq, &posS, &offS, &lenS, &gain, &muted, &loop)) return;
        const long long nfr = (long long)src->numFrames();
        if (offS < 0) offS = 0;
        if (lenS < 1 || offS + lenS > nfr) lenS = nfr - offS;
        if (lenS < 16) return;
        // slice the region's window out of the source
        PatchKnob::engine::AudioClip slice;
        slice.sampleRate = src->sampleRate; slice.sourceSampleRate = src->sourceSampleRate;
        slice.name = src->name + ".tce";
        slice.ch[0].assign(src->ch[0].begin() + (size_t)offS,
                           src->ch[0].begin() + (size_t)(offS + lenS));
        const auto& r1 = src->ch[1].empty() ? src->ch[0] : src->ch[1];
        slice.ch[1].assign(r1.begin() + (size_t)offS, r1.begin() + (size_t)(offS + lenS));
        // stretch to the new tick span, measured at the region's own position
        const long posTick = (long)PatchKnob::app::audio_app_sample_to_tick(posS);
        const long long newLenF =
            PatchKnob::app::audio_app_tick_to_sample(posTick + newLenTicks) - posS;
        if (newLenF < 16) return;
        PatchKnob::engine::AudioClip warped =
            PatchKnob::engine::warp_to_length(slice, newLenF);
        if (warped.empty()) return;
        // swap: tear down the old attachment, attach the render as its own clip
        int track = -1;
        auto tr = g_seqToTrack.find(seq);
        if (tr != g_seqToTrack.end()) track = tr->second;
        detach_seq_audio(seq);
        if (track < 0) { track = PatchKnob::app::audio_app_master_add_track(0);
                         if (track >= 0) g_seqToTrack[seq] = track; }
        if (track < 0) return;
        const int id = PatchKnob::app::audio_app_freeze_attach(track, warped, posS, gain);
        if (id < 0) return;
        PatchKnob::app::audio_app_freeze_set_region(id, posS, 0, newLenF);
        PatchKnob::app::audio_app_freeze_set_muted(id, muted != 0);
        FrozenRec rec; rec.freezeId = id; rec.isTrack = true; rec.newSeq = seq;
        rec.srcTrack = -1;
        g_frozen[seq] = rec;
        // resize the block trigger to the new span so view + engine agree
        if (sequence* qs = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr) {
            qs->clear_triggers();
            qs->add_trigger(posTick, newLenTicks, 0);
        }
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
            vArrange.set_audio_clip(seq, disp, newLenTicks);
            vArrange.set_audio_region(seq, posTick, newLenTicks, 0, gain, muted != 0, false, 0);
        }
        app.request_redraw();
    };

    // Loop Trim (bottom half): publish the trimmed loop PERIOD to the engine
    // so the player wraps on the span the view draws.
    vArrange.on_clip_loop_length = [&, freeze_id_for_seq](int seq, long loopTicks){
        if (!audio_ok || loopTicks < 1) return;
        const long long frames = PatchKnob::app::audio_app_tick_to_sample(loopTicks) -
                                 PatchKnob::app::audio_app_tick_to_sample(0);
        const int id = freeze_id_for_seq(seq);
        if (id >= 0) PatchKnob::app::audio_app_freeze_set_loop_length(id, frames);
        else {
            auto tr = g_seqToTrack.find(seq);
            const unsigned long long rid = region_id_for_seq(seq);
            if (tr != g_seqToTrack.end() && rid)
                PatchKnob::app::audio_app_project_set_loop_length_by_id(tr->second, rid, frames);
        }
    };

    // Scrubber / Scrub-Trim audition: a short slice of the region's source at
    // the scrub point, at the region gain scaled by the track strip's fader,
    // silent when the region or the strip is muted/inaudible.  HONEST LIMIT:
    // the preview renderer mixes at the MASTER bus, so the strip's inserts do
    // not colour the audition (there is no per-track offline audition path).
    vArrange.on_scrub_audition = [&, region_of_seq](int seq, long tick){
        if (!audio_ok) return;
        static Uint32 lastMs = 0; static long lastTick = -1;
        const Uint32 now = SDL_GetTicks();
        if (tick == lastTick || now - lastMs < 60) return;   // retrigger throttle
        lastMs = now; lastTick = tick;
        const PatchKnob::engine::AudioClip* clip = vArrange.audio_clip(seq);
        if (!clip || clip->empty()) return;
        long long posS = 0, offS = 0, lenS = 0; float gain = 1.f; int muted = 0, loop = 0;
        if (!region_of_seq(seq, &posS, &offS, &lenS, &gain, &muted, &loop)) return;
        const long long t = PatchKnob::app::audio_app_tick_to_sample((long long)(tick < 0 ? 0 : tick));
        long long src = offS + (t - posS);
        const long long nfr = (long long)clip->numFrames();
        if (loop && lenS > 0 && nfr > 0 && src >= nfr) src %= nfr;
        if (src < 0 || src >= nfr) return;
        const double sr = clip->sampleRate > 0 ? clip->sampleRate : 48000.0;
        const long long n = std::min<long long>((long long)(0.09 * sr), nfr - src);
        if (n < 32) return;
        float g = muted ? 0.f : gain;
        auto tr = g_seqToTrack.find(seq);
        if (tr != g_seqToTrack.end()) {
            if (!PatchKnob::app::audio_app_master_audible(tr->second)) g = 0.f;
            g *= PatchKnob::app::audio_app_mixer_gain(
                     PatchKnob::app::audio_app_master_mixer_node(), tr->second);
        }
        PatchKnob::engine::AudioClip s;
        s.sampleRate = clip->sampleRate; s.sourceSampleRate = clip->sourceSampleRate;
        s.ch[0].assign(clip->ch[0].begin() + (size_t)src,
                       clip->ch[0].begin() + (size_t)(src + n));
        const auto& r1 = clip->ch[1].empty() ? clip->ch[0] : clip->ch[1];
        s.ch[1].assign(r1.begin() + (size_t)src, r1.begin() + (size_t)(src + n));
        PatchKnob::app::audio_app_preview_clip(s, g);
    };

    // ---- ch.31: Split Into Mono (p736) -------------------------------------
    vArrange.on_clip_split_mono = [&, region_of_seq](int seq){
        if (!audio_ok) return;
        const PatchKnob::engine::AudioClip* src = vArrange.audio_clip(seq);
        if (!src || src->empty()) return;
        if (src->ch[1].empty()) { projectStatus = "Split Into Mono: clip is already mono"; return; }
        long long posS = 0, offS = 0, lenS = 0; float gain = 1.f; int muted = 0, loop = 0;
        if (!region_of_seq(seq, &posS, &offS, &lenS, &gain, &muted, &loop)) return;
        sequence* ss = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        const std::string base = ss && ss->get_name() ? ss->get_name() : "Audio";
        const long posTick = (long)PatchKnob::app::audio_app_sample_to_tick(posS);
        const long lenTick = std::max<long>(1,
            (long)PatchKnob::app::audio_app_sample_to_tick_ceil(posS + lenS) - posTick);
        const int srcTrack = g_seqToTrack.count(seq) ? g_seqToTrack[seq] : -1;
        for (int chn = 0; chn < 2; ++chn) {
            int newSeq = -1;
            for (int i = 0; i < c_max_sequence; ++i)
                if (!perf.is_active(i)) { newSeq = i; break; }
            if (newSeq < 0) break;
            PatchKnob::engine::AudioClip mono;
            mono.sampleRate = src->sampleRate; mono.sourceSampleRate = src->sourceSampleRate;
            mono.name = base + (chn ? ".R" : ".L");
            const auto& ch = chn ? src->ch[1] : src->ch[0];
            mono.ch[0] = ch; mono.ch[1] = ch;
            perf.new_sequence(newSeq); perf.set_active(newSeq, true);
            sequence* ns = perf.get_sequence(newSeq);
            if (!ns) break;
            ns->set_name(mono.name);
            ns->add_trigger(posTick, lenTick, 0);
            const int newTrack = PatchKnob::app::audio_app_master_add_track(0);
            if (newTrack < 0) { perf.delete_sequence(newSeq); break; }
            const int id = PatchKnob::app::audio_app_freeze_attach(newTrack, mono, posS, gain);
            if (id < 0) { PatchKnob::app::audio_app_master_remove_track(newTrack);
                          perf.delete_sequence(newSeq); break; }
            PatchKnob::app::audio_app_freeze_set_region(id, posS, offS, lenS);
            PatchKnob::app::audio_app_freeze_set_muted(id, muted != 0);
            PatchKnob::app::audio_app_freeze_set_loop(id, loop != 0);
            g_seqToTrack[newSeq] = newTrack;
            // output + send assignments carry over from the source strip (p736)
            if (srcTrack >= 0) {
                PatchKnob::app::audio_app_master_strip_set_output(newTrack,
                    PatchKnob::app::audio_app_master_strip_output(srcTrack));
                const int naux = PatchKnob::app::audio_app_master_aux_count();
                for (int ax = 0; ax < naux; ++ax) {
                    PatchKnob::app::audio_app_master_set_send_level(newTrack, ax,
                        PatchKnob::app::audio_app_master_send_level(srcTrack, ax));
                    PatchKnob::app::audio_app_master_set_send_enabled(newTrack, ax,
                        PatchKnob::app::audio_app_master_send_enabled(srcTrack, ax) ? 1 : 0);
                }
            }
            FrozenRec rec; rec.freezeId = id; rec.isTrack = true; rec.newSeq = newSeq;
            rec.srcTrack = -1;
            g_frozen[newSeq] = rec;
            if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
                long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                                     (long long)disp->numFrames());
                vArrange.set_audio_clip(newSeq, disp, std::max<long>(1, clipTicks));
                vArrange.set_audio_region(newSeq, posTick, lenTick,
                    (long)PatchKnob::app::audio_app_sample_to_tick(offS),
                    gain, muted != 0, loop != 0, 0);
            }
        }
        rebind_project_views(); refresh_master_ui();
        projectStatus = "Split \"" + base + "\" into " + base + ".L / " + base + ".R";
        app.request_redraw();
    };

    // ---- ch.31: Consolidate (p705/p737) -------------------------------------
    // Shared renderer: mix exactly `seqs` over [aTick,bTick) -- region gain
    // AND the per-clip fade envelope baked in (ScheduledClip::fadeGain is the
    // same curve playback applies), gaps as silence, muted regions silent --
    // remove them through the undo queue, and attach the render as ONE new
    // whole clip covering the span.  Message thread only; the audio thread
    // just sees one schedule republish per removal + one for the new clip.
    auto consolidate_seqs = [&, region_of_seq](const std::vector<int>& seqs,
                                               long aTick, long bTick,
                                               int trackHint){
        if (!audio_ok || bTick <= aTick || seqs.empty()) return;
        const long long sA = PatchKnob::app::audio_app_tick_to_sample(aTick);
        const long long sB = PatchKnob::app::audio_app_tick_to_sample(bTick);
        const long long lenF = sB - sA;
        if (lenF < 16) return;
        int track = trackHint;
        for (int si : seqs)
            if (track < 0 && g_seqToTrack.count(si)) { track = g_seqToTrack[si]; break; }
        const double sr = PatchKnob::app::audio_app_sample_rate();
        //  Gather the pieces, then let the ENGINE mix them.  This used to be
        //  an inline loop here, which meant the one part of Consolidate that
        //  decides what you hear had no rendered-sample test -- nothing
        //  headless could link a lambda.  See engine/audioclip/consolidate.h.
        std::vector<PatchKnob::engine::ConsolidatePiece> pieces;
        std::vector<int> remove;
        for (int si : seqs) {
            const PatchKnob::engine::AudioClip* clip = vArrange.audio_clip(si);
            long long pos = 0, off = 0, len = 0; float gain = 1.f; int muted = 0, loop = 0;
            if (!clip || !region_of_seq(si, &pos, &off, &len, &gain, &muted, &loop))
                continue;
            if (pos < sA - 1024 || pos + len > sB + 1024) continue;   // outside
            remove.push_back(si);
            PatchKnob::engine::ConsolidatePiece pc;
            pc.clip = clip; pc.pos = pos; pc.off = off; pc.len = len;
            pc.gain = gain; pc.muted = muted != 0; pc.loop = loop != 0;
            //  The piece's fade envelope, from the view's per-clip fade state
            //  (the same source every playback path publishes to the engine).
            const arrange::ArrangeView::ClipFade cf = vArrange.clip_fade(si);
            const long long z = PatchKnob::app::audio_app_tick_to_sample(0);
            pc.fade.fadeInFrames  = cf.inTicks  > 0
                ? PatchKnob::app::audio_app_tick_to_sample(cf.inTicks)  - z : 0;
            pc.fade.fadeOutFrames = cf.outTicks > 0
                ? PatchKnob::app::audio_app_tick_to_sample(cf.outTicks) - z : 0;
            pc.fade.fadeInTension  = cf.inK;  pc.fade.fadeOutTension = cf.outK;
            pc.fade.fadeInShape  = (uint8_t)cf.inShape;
            pc.fade.fadeOutShape = (uint8_t)cf.outShape;
            pc.fade.fadeInSlope  = (uint8_t)cf.inSlope;
            pc.fade.fadeOutSlope = (uint8_t)cf.outSlope;
            pc.hasFade = pc.fade.fadeInFrames > 0 || pc.fade.fadeOutFrames > 0;
            pieces.push_back(pc);
        }
        PatchKnob::engine::AudioClip out =
            PatchKnob::engine::consolidateRender(pieces, sA, sB,
                                                 sr > 0 ? sr : 48000.0);
        // remove the covered clips THROUGH the queue (part of the compound the
        // view opened around this call)
        for (int si : remove) vArrange.delete_audio_clip_undoable(si);
        // attach the render on a fresh sequence sharing the lane's track
        int newSeq = -1;
        for (int i = 0; i < c_max_sequence; ++i)
            if (!perf.is_active(i)) { newSeq = i; break; }
        if (newSeq < 0) return;
        perf.new_sequence(newSeq); perf.set_active(newSeq, true);
        sequence* ns = perf.get_sequence(newSeq);
        if (!ns) return;
        ns->set_name("Consolidated");
        ns->add_trigger(aTick, bTick - aTick, 0);
        if (track < 0) track = PatchKnob::app::audio_app_master_add_track(0);
        if (track < 0) { perf.delete_sequence(newSeq); return; }
        const int id = PatchKnob::app::audio_app_freeze_attach(track, out, sA, 1.f);
        if (id < 0) { perf.delete_sequence(newSeq); return; }
        PatchKnob::app::audio_app_freeze_set_region(id, sA, 0, lenF);
        g_seqToTrack[newSeq] = track;
        FrozenRec rec; rec.freezeId = id; rec.isTrack = true; rec.newSeq = newSeq;
        rec.srcTrack = -1;
        g_frozen[newSeq] = rec;
        if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
            vArrange.set_audio_clip(newSeq, disp, bTick - aTick);
            vArrange.set_audio_region(newSeq, aTick, bTick - aTick, 0);
        }
        // The NEW clip must unwind with the rest of the compound: the deleted
        // slices already restore through their own queue entries, but the
        // attach used to survive an undo -- the consolidated audio kept
        // playing with no clip left on screen.  keep holds the render for
        // redo, so redo does not have to re-mix.
        {
            auto keep = std::make_shared<PatchKnob::engine::AudioClip>(out);
            const int fzId = id, ns2 = newSeq, trk = track;
            const long a2 = aTick, b2 = bTick;
            const long long sA2 = sA, lenF2 = lenF;
            arrange::ArrangeView::UndoOp op; op.name = "Consolidate Attach";
            op.undo.push_back([&, fzId, ns2]{
                PatchKnob::app::audio_app_freeze_detach(fzId);
                g_frozen.erase(ns2);
                vArrange.set_audio_clip(ns2, nullptr, 0);
                g_seqToTrack.erase(ns2); g_seqRegion.erase(ns2);
                if (perf.is_active(ns2)) perf.delete_sequence(ns2);
                app.request_redraw();
            });
            op.redo.push_back([&, keep, ns2, trk, a2, b2, sA2, lenF2]{
                if (!perf.is_active(ns2)) perf.new_sequence(ns2);
                perf.set_active(ns2, true);
                if (sequence* rs = perf.get_sequence(ns2)) {
                    rs->set_name("Consolidated");
                    rs->clear_triggers();
                    rs->add_trigger(a2, b2 - a2, 0);
                }
                const int rid2 = PatchKnob::app::audio_app_freeze_attach(trk, *keep, sA2, 1.f);
                if (rid2 < 0) return;
                PatchKnob::app::audio_app_freeze_set_region(rid2, sA2, 0, lenF2);
                g_seqToTrack[ns2] = trk;
                FrozenRec rr; rr.freezeId = rid2; rr.isTrack = true; rr.newSeq = ns2;
                rr.srcTrack = -1;
                g_frozen[ns2] = rr;
                if (const PatchKnob::engine::AudioClip* d2 =
                        PatchKnob::app::audio_app_freeze_clip(rid2)) {
                    vArrange.set_audio_clip(ns2, d2, b2 - a2);
                    vArrange.set_audio_region(ns2, a2, b2 - a2, 0);
                }
                app.request_redraw();
            });
            vArrange.push_undo_op(std::move(op));
        }
        rebind_project_views(); refresh_master_ui();
        projectStatus = "Consolidated selection into one clip";
        app.request_redraw();
    };
    // Range consolidate (p737): the view has already SEPARATED the range
    // boundaries, so every region on the lane is fully inside or fully outside
    // [aTick,bTick); render every inside one.
    vArrange.on_consolidate = [&, consolidate_seqs](int laneSeq, long aTick, long bTick){
        const int key = vArrange.on_track_key ? vArrange.on_track_key(laneSeq) : laneSeq;
        std::vector<int> laneSeqs;
        for (int si = 0; si < c_max_sequence; ++si)
            if (perf.is_active(si) && vArrange.audio_clip(si) &&
                (vArrange.on_track_key ? vArrange.on_track_key(si) : si) == key)
                laneSeqs.push_back(si);
        const int hint = g_seqToTrack.count(laneSeq) ? g_seqToTrack[laneSeq] : -1;
        consolidate_seqs(laneSeqs, aTick, bTick, hint);
    };
    // Clip-selection consolidate (p705): render EXACTLY the selected pieces --
    // in timeline order, the gaps between them preserved as silence -- into
    // one new clip.  Unselected material inside the span is left alone.
    vArrange.on_consolidate_clips = [&, consolidate_seqs](const std::vector<int>& seqs,
                                                          long aTick, long bTick){
        consolidate_seqs(seqs, aTick, bTick, -1);
    };

    // ---- ch.31: Compact (p737-738).  DESTRUCTIVE: the source buffer outside
    // the region's window (padded by `padMs`) is permanently discarded; the
    // session is saved right after, exactly as the manual specifies.  The
    // view cleared its undo queue before calling (cannot be undone).
    vArrange.on_clip_compact = [&, freeze_id_for_seq, region_of_seq](int seq, long padMs){
        if (!audio_ok) return;
        const PatchKnob::engine::AudioClip* src = vArrange.audio_clip(seq);
        if (!src || src->empty()) return;
        long long posS = 0, offS = 0, lenS = 0; float gain = 1.f; int muted = 0, loop = 0;
        if (!region_of_seq(seq, &posS, &offS, &lenS, &gain, &muted, &loop)) return;
        const double sr = src->sampleRate > 0 ? src->sampleRate : 48000.0;
        const long long pad = (long long)((double)padMs / 1000.0 * sr);
        const long long nfr = (long long)src->numFrames();
        long long a = std::max<long long>(0, offS - pad);
        long long b = std::min<long long>(nfr, offS + lenS + pad);
        if (b <= a) return;
        if (a == 0 && b == nfr) { projectStatus = "Compact: nothing unused to delete"; return; }
        PatchKnob::engine::AudioClip packed;
        packed.sampleRate = src->sampleRate; packed.sourceSampleRate = src->sourceSampleRate;
        packed.name = src->name;
        packed.ch[0].assign(src->ch[0].begin() + (size_t)a, src->ch[0].begin() + (size_t)b);
        const auto& r1 = src->ch[1].empty() ? src->ch[0] : src->ch[1];
        packed.ch[1].assign(r1.begin() + (size_t)a, r1.begin() + (size_t)b);
        const int id = freeze_id_for_seq(seq);
        if (id >= 0) {
            if (!PatchKnob::app::audio_app_freeze_replace_clip(id, packed)) return;
            PatchKnob::app::audio_app_freeze_set_region(id, posS, offS - a, lenS);
            if (const PatchKnob::engine::AudioClip* disp = PatchKnob::app::audio_app_freeze_clip(id)) {
                const long posTick = (long)PatchKnob::app::audio_app_sample_to_tick(posS);
                const long lenTick = std::max<long>(1,
                    (long)PatchKnob::app::audio_app_sample_to_tick_ceil(posS + lenS) - posTick);
                long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                                     (long long)disp->numFrames());
                vArrange.set_audio_clip(seq, disp, std::max<long>(1, clipTicks));
                vArrange.set_audio_region(seq, posTick, lenTick,
                    (long)PatchKnob::app::audio_app_sample_to_tick(offS - a),
                    gain, muted != 0, loop != 0, 0);
            }
        } else {
            auto tr = g_seqToTrack.find(seq);
            if (tr == g_seqToTrack.end()) return;
            const int track = tr->second;
            const unsigned long long rid = region_id_for_seq(seq);
            if (rid) PatchKnob::app::audio_app_project_remove_region_by_id(track, rid);
            else     PatchKnob::app::audio_app_project_remove_audio_clip(track, src);
            g_seqRegion.erase(seq);
            if (!PatchKnob::app::audio_app_project_add_audio_region(track, packed,
                    posS, offS - a, lenS, gain, muted, loop, 0, 0, 0.f, 0.f, 0))
                return;
            g_seqRegion[seq] = PatchKnob::app::audio_app_project_last_region_id(track);
            if (const PatchKnob::engine::AudioClip* disp =
                    PatchKnob::app::audio_app_project_last_clip_on_track(track)) {
                const long posTick = (long)PatchKnob::app::audio_app_sample_to_tick(posS);
                const long lenTick = std::max<long>(1,
                    (long)PatchKnob::app::audio_app_sample_to_tick_ceil(posS + lenS) - posTick);
                long clipTicks = (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                                     (long long)disp->numFrames());
                vArrange.set_audio_clip(seq, disp, std::max<long>(1, clipTicks));
                vArrange.set_audio_region(seq, posTick, lenTick,
                    (long)PatchKnob::app::audio_app_sample_to_tick(offS - a),
                    gain, muted != 0, loop != 0, 0);
            }
        }
        // the manual: once Compact completes, the session is saved automatically
        if (!projectPath.empty()) save_to_project(projectPath);
        else projectStatus = "Compacted (save the project to persist it)";
        app.request_redraw();
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

    // ---- CDP: clip right-click "CDP..." -------------------------------------
    // An AUDIO clip hands its buffer straight over.  A MIDI clip has no audio at
    // all, so it is FROZEN to a wav first (the same render the freeze feature
    // uses) and the result becomes the source node -- which is what "drag a MIDI
    // clip in and it auto-freezes" means in practice.
    auto cdp_buffer_from_clip = [&](const PatchKnob::engine::AudioClip* c) {
        PatchKnob::cdp::Buffer b;
        b.sampleRate = (int)PatchKnob::app::audio_app_sample_rate();
        if (!c || c->empty()) return b;
        const int chans = c->ch[1].empty() ? 1 : 2;
        b.resize(chans, c->numFrames());
        for (int ch = 0; ch < chans; ++ch)
            for (int64_t i = 0; i < c->numFrames(); ++i)
                b.ch[(size_t)ch][(size_t)i] = c->ch[(size_t)ch][(size_t)i];
        return b;
    };
    vArrange.on_open_cdp = [&, freeze_id_for_seq](int seq){
        if (!audio_ok) return;
        const PatchKnob::engine::AudioClip* c = vArrange.audio_clip(seq);
        if (!c) {                                   // MIDI clip -> freeze first
            if (vArrange.on_freeze_clip && perf.is_active(seq)) {
                sequence* s = perf.get_sequence(seq);
                if (s) {
                    s->select_trigger(0);
                    long st = s->get_selected_trigger_start_tick();
                    long en = s->get_selected_trigger_end_tick();
                    if (en <= st) { st = 0; en = std::max<long>(1, s->get_length()) - 1; }
                    vArrange.on_freeze_clip(seq, st, en);
                }
            }
            const int id = freeze_id_for_seq(seq);
            if (id >= 0) c = PatchKnob::app::audio_app_freeze_clip(id);
            if (!c) { projectStatus = "CDP: could not render that clip to audio"; return; }
        }
        PatchKnob::cdp::Buffer b = cdp_buffer_from_clip(c);
        if (b.empty()) { projectStatus = "CDP: clip has no audio"; return; }
        sequence* sq = perf.is_active(seq) ? perf.get_sequence(seq) : nullptr;
        const std::string nm = sq && sq->get_name() ? sq->get_name() : "clip";
        cdpEditor.accept_clip(b, nm, seq, 40.f, 40.f);
        cdpWin.visible = true; wm.add(&cdpWin); wm.raise(&cdpWin);
        app.request_redraw();
    };
    // "RENDER TO TRACK".  This callback was DECLARED and never assigned, and the
    // widget guards with `if (on_render)`, so the button was silently inert --
    // the whole point of the editor (get the transformed audio back into the
    // song) did nothing.  Land the finished chain on a NEW audio lane, aligned
    // to where the source clip sits, leaving the source untouched.  Modelled on
    // vArrange.on_freeze_track / commit_audio_recording, which is the existing
    // "put an AudioClip on a new lane" recipe.
    cdpEditor.on_render = [&](const PatchKnob::cdp::Buffer& out, int sourceSeq){
        if (!audio_ok)   { projectStatus = "CDP: audio engine is not running"; return; }
        if (out.empty()) { projectStatus = "CDP: chain produced no audio"; return; }
        // Anchor at the source clip's first trigger so the result lines up with
        // the material it was made from; tick 0 if that lane is gone.
        long startTick = 0;
        std::string baseName = "CDP";
        if (sourceSeq >= 0 && perf.is_active(sourceSeq)) {
            if (sequence* src = perf.get_sequence(sourceSeq)) {
                if (src->get_name() && *src->get_name()) baseName = src->get_name();
                src->reset_draw_trigger_marker();
                long on = 0, off = 0, ofs = 0; bool sel = false;
                if (src->get_next_trigger(&on, &off, &sel, &ofs)) startTick = on;
            }
        }
        int newSeq = -1;
        for (int i = 0; i < c_max_sequence; ++i) if (!perf.is_active(i)) { newSeq = i; break; }
        if (newSeq < 0) { projectStatus = "CDP: no free track for the render"; return; }
        perf.new_sequence(newSeq); perf.set_active(newSeq, true);
        sequence* ns = perf.get_sequence(newSeq);
        if (!ns) { projectStatus = "CDP: could not create a track"; return; }
        ns->set_name(baseName + " (CDP)");
        ns->set_track_kind(1);                       // audio lane
        ns->set_arrange_lane_id(newSeq);
        const int newTrack = PatchKnob::app::audio_app_master_add_track(0);   // audio ports
        if (newTrack < 0) { perf.delete_sequence(newSeq);
                            projectStatus = "CDP: could not add a mixer track"; return; }
        g_seqToTrack[newSeq] = newTrack;
        ns->set_midi_bus((char)newTrack);

        PatchKnob::engine::AudioClip clip;
        clip.name = ns->get_name() ? ns->get_name() : "CDP";
        clip.sampleRate = clip.sourceSampleRate =
            out.sampleRate > 0 ? (double)out.sampleRate
                               : PatchKnob::app::audio_app_sample_rate();
        clip.resize(out.frames());
        for (int64_t i = 0; i < out.frames(); ++i) {          // mono chains fill both sides
            clip.ch[0][(size_t)i] = out.ch[0][(size_t)i];
            clip.ch[1][(size_t)i] = out.channels() > 1 ? out.ch[1][(size_t)i]
                                                       : out.ch[0][(size_t)i];
        }
        const long long startSample = PatchKnob::app::audio_app_tick_to_sample((long long)startTick);
        if (!PatchKnob::app::audio_app_project_add_audio_clip(newTrack, clip, startSample, 1.f)) {
            PatchKnob::app::audio_app_master_remove_track(newTrack);
            g_seqToTrack.erase(newSeq); perf.delete_sequence(newSeq);
            projectStatus = "CDP: could not place the rendered audio"; return;
        }
        // Size the clip block to the audio's real duration, so the block edge is
        // where the sound ends and the waveform maps 1:1 to timeline ticks.
        const long lenTicks = std::max<long>(1,
            (long)PatchKnob::app::audio_app_sample_to_tick_ceil(
                startSample + (long long)clip.numFrames()) - startTick);
        ns->set_length(std::max<long>(ns->get_length(), lenTicks));
        ns->add_trigger(startTick, lenTicks, 0);
        ns->set_dirty();
        if (const PatchKnob::engine::AudioClip* stored =
                PatchKnob::app::audio_app_project_last_clip_on_track(newTrack))
            vArrange.set_audio_clip(newSeq, stored, lenTicks);
        g_seqRegion[newSeq] = PatchKnob::app::audio_app_project_last_region_id(newTrack);
        rebind_project_views();
        refresh_master_ui();
        projectStatus = "CDP: rendered to \"" + std::string(ns->get_name() ? ns->get_name() : "") + "\"";
        app.request_redraw();
    };
    cdpEditor.on_preview = [&](const PatchKnob::cdp::Buffer& out){
        if (!audio_ok || out.empty()) return;
        PatchKnob::engine::AudioClip c;
        c.name = "CDP preview";
        c.sampleRate = c.sourceSampleRate = out.sampleRate;
        c.resize(out.frames());
        for (int64_t i = 0; i < out.frames(); ++i) {
            c.ch[0][(size_t)i] = out.ch[0][(size_t)i];
            c.ch[1][(size_t)i] = out.channels() > 1 ? out.ch[1][(size_t)i]
                                                    : out.ch[0][(size_t)i];
        }
        PatchKnob::app::audio_app_preview_clip(c);
        projectStatus = "CDP preview";
        app.request_redraw();
    };
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

    vPatch.on_add_module = [&](double x, double y, const std::string& catIn){
        if (!audio_ok) return;
        // The view hands the category in by const reference; the Audio I/O branch
        // below rewrites it (both "Audio In" and "Audio Out" live in one canvas
        // category), so work on a local copy.
        std::string cat = catIn;
        int eng=-1; std::string nm=cat.empty()?std::string("Sine"):cat;
        // The canvas category a node is filed under; the Audio I/O submenu passes
        // a more specific label than the category it should land in.
        std::string canvasCat = cat;
        // An instrument node starts EMPTY, exactly like Add Effect below, and
        // the picker opens on it so the user CHOOSES the instrument.  This
        // used to instantiate a hardcoded default VST and silently add
        // nothing whenever that plug-in was not installed.
        if      (cat=="Instrument") { eng=PatchKnob::app::audio_app_patch_add_empty_plugin(); nm="Instrument"; }
        else if (cat=="Mono to Stereo") { eng=PatchKnob::app::audio_app_patch_add_builtin("mono2stereo"); nm="Mono > Stereo"; canvasCat="Audio I/O"; }
        else if (cat=="Effect")     { eng=PatchKnob::app::audio_app_patch_add_empty_plugin(); nm="FX (empty)"; }
        else if (cat=="MIDI")       { eng=PatchKnob::app::audio_app_patch_add_builtin("midiin"); nm="MIDI In"; }
        else if (cat=="Audio I/O" || cat=="Audio Out" || cat=="Audio In") {
            // "Audio I/O" alone used to mean OUT, which is why no input node
            // could be created.  The submenu now says which one; the bare
            // category is kept so older paths still resolve to an output.
            const bool wantIn = (cat=="Audio In");
            eng = PatchKnob::app::audio_app_patch_add_builtin(wantIn ? "in" : "out");
            int cnt=0; for (const auto& nd : vPatch.nodes()) if (nd.category=="Audio I/O") ++cnt;
            nm = (wantIn ? "Audio In " : "Audio Out ") + std::to_string(cnt+1);
            canvasCat = "Audio I/O";    // both live in the one canvas category
        }
        else if (cat=="Mixer")      { eng=PatchKnob::app::audio_app_patch_add_mixer(4);          nm="Mixer"; }
        else if (cat=="Pure Data")  { eng=PatchKnob::app::audio_app_patch_add_pd();              nm="Pd"; }
        else if (cat=="Modular (Rack)") { eng=PatchKnob::app::audio_app_patch_add_rack();        nm="Rack"; }
        else if (cat=="Sampler")    { eng=PatchKnob::app::audio_app_patch_add_sampler();         nm="Sampler"; }
        else if (cat=="Csound")     { eng=PatchKnob::app::audio_app_patch_add_csound();          nm="Csound"; }
        else                        { eng=PatchKnob::app::audio_app_patch_add_builtin("sine");   nm="Sine";  }
        pv_mirror_node(eng, nm, canvasCat, x, y);
        // Instrument-like nodes (VST / Pure Data / Rack) each get a DISTINCT MIDI
        // channel up front AND auto-create + connect their own master-mixer audio
        // track (MidiIn -> inst -> mixer inlet), so inserting an instrument is
        // instantly playable through its own live mixer channel.
        if (eng >= 0 && (cat=="Instrument" || cat=="Pure Data" || cat=="Modular (Rack)" || cat=="Sampler" || cat=="Csound")) {
            make_instr_graph_routed(eng);
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
                    ns->set_midi_channel(0);
                    auto it = g_instrTrack.find(eng);
                    if (it != g_instrTrack.end()) {
                        g_seqToTrack[idx] = it->second;  // share its mixer channel
                        ns->set_midi_bus((char)it->second);
                    }
                    g_arrangeInstrument[idx]=eng;
                    if (!g_songMode) perf.sequence_playing_on(idx);
                    rebind_project_views();
                }
            }
        }
        // Adding an instrument means CHOOSING one: the node arrives empty, so
        // open the picker on it.  With no plug-ins scanned the picker shows its
        // explanatory empty state -- which is the honest answer, and far better
        // than the silent no-op this path used to be.
        if (eng >= 0 && cat == "Instrument" && vPatch.on_open_editor) {
            pluginPicker.instrumentsOnly = true;
            vPatch.on_open_editor((pb::NodeId)eng);
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
        const int vm=PatchKnob::app::audio_app_virtual_midi_node();
        if((int)fn==vm&&(int)tn==vm&&(int)fp>=100) {
            PatchKnob::app::audio_app_virtual_midi_set_route((int)fp-100,(int)tp);
            vPatch.remove_connection(pb::Connection(pb::PortRef(fn,fp), pb::PortRef(tn,tp)));
            rebuild_track_midi_routes(); app.request_redraw(); return;
        }
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
        if((int)fn==vm||(int)tn==vm) rebuild_track_midi_routes();
    };
    vPatch.on_disconnect = [&](pb::NodeId fn, pb::PortId fp, pb::NodeId tn, pb::PortId tp){
        if(!audio_ok)return;
        const int vm=PatchKnob::app::audio_app_virtual_midi_node();
        if((int)fn==vm&&(int)tn==vm&&(int)fp>=100)
            PatchKnob::app::audio_app_virtual_midi_set_route((int)fp-100,-1);
        else PatchKnob::app::audio_app_patch_disconnect((int)fn,(int)fp,(int)tn,(int)tp);
        if((int)fn==vm||(int)tn==vm) rebuild_track_midi_routes();
    };
    vPatch.on_remove_node = [&](pb::NodeId id){
        // Singletons (Master Mixer, MIDI In, Audio Out) can't be deleted -- if the
        // patchbay optimistically dropped one, re-mirror it and keep the engine node.
        if (audio_ok) {
            if ((int)id == PatchKnob::app::audio_app_master_mixer_node())
                { pv_mirror_node((int)id, "Master Mixer", "Mixer", 620, 180); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_virtual_midi_node())
                { pv_mirror_node((int)id, "Instrument Virtual MIDI Ports", "MIDI", 430, 180); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_patch_midi_in_node())
                { pv_mirror_node((int)id, "Instrument out", "MIDI", 40, 70); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_default_hw_midi_in_node())
                { pv_mirror_node((int)id, "Hardware MIDI In", "MIDI", 40, 190); app.request_redraw(); return; }
            if ((int)id == PatchKnob::app::audio_app_patch_out_node())
                { pv_mirror_node((int)id, "Audio Out", "Audio I/O", 620, 70); app.request_redraw(); return; }
        }
        if ((int)id == g_guiNode && g_guiHandle) {   // close its native GUI first
            PatchKnob::hostwin::editor_close(g_guiHandle); g_guiHandle=nullptr; g_guiNode=-1; guiWin.visible=false;
        }
        // ...and the SDL editors bound to this node, for exactly the same
        // reason.  rackEditor caches the node's RackEngine* (non-owning) and
        // dereferences it every draw(); audio_app_patch_remove() below destroys
        // the node that owns it, so deleting a Rack node with its editor open
        // was a use-after-free on the next frame.  panelEditor holds the same
        // engine plus a module id, and the Pd/Csound editors write back to the
        // node (or to a module inside the rack) on every keystroke.
        if ((int)id == g_rackNode) {
            rackWin.visible = false; panelEditorWin.visible = false;
            rackEditor.set_engine(nullptr); panelEditor.set_target(nullptr,-1);
            g_rackNode = -1; g_rackIOsig = -1; g_rackPath.clear();
            // Editors opened ON a module of that rack die with it.
            if (g_pdRackMod     >= 0) { pdWin.visible     = false; g_pdRackMod     = -1; }
            if (g_csoundRackMod >= 0) { csoundWin.visible = false; g_csoundRackMod = -1; }
        }
        if ((int)id == g_pdNode) {
            pdWin.visible = false; g_pdNode = -1; g_pdIOsig = -1; g_pdPath.clear();
        }
        if ((int)id == g_csoundNode) {
            csoundWin.visible = false; g_csoundNode = -1; g_csoundPath.clear();
        }
        // Same class of dangling binding: the Mixer strip window addresses
        // g_curMixerNode on every frame (channel count, gains, VU).  Deleting
        // the Mixer module it points at leaves it naming a node that no longer
        // exists.  Close it and fall back to the Master Mixer, which cannot be
        // deleted (guarded at the top of this handler).
        if (audio_ok && (int)id == g_curMixerNode) {
            mixerStripWin.visible = false;
            g_curMixerNode = PatchKnob::app::audio_app_master_mixer_node();
            mixerStrip.bus_count = 0;
            for (int i = 0; i < kMixerUiMaxChannels; ++i) { g_busSolo[i] = false; g_busMute[i] = false; }
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
        // The Parameters panel holds a raw IPluginInstance* and reads
        // descriptor() every frame.  audio_app_patch_remove() destroys the
        // instance, so it must be told first; forget_instance() only clears
        // when this really is the one on show.
        if (audio_ok) paramView.forget_instance(PatchKnob::app::audio_app_patch_node_instance((int)id));
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
        // The Instrument branch no longer instantiates synthPath, so the
        // coordtest names its plug-in explicitly.
        PatchKnob::app::audio_app_patch_set_node_plugin(plugN, vst_desc(synthPath.c_str()));
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

    if(getenv("PATCHKNOB_NEW_PROJECT_PROBE")) {
        if(vArrange.on_add_track) vArrange.on_add_track(0);
        int before=0; for(int i=0;i<c_max_sequence;++i)if(perf.is_active(i))++before;
        arrangeWin.visible=false; arrangeWin.minimized=true;
        if(new_project)new_project();
        int after=0; for(int i=0;i<c_max_sequence;++i)if(perf.is_active(i))++after;
        const bool registered=std::find(wm.windows.begin(),wm.windows.end(),&arrangeWin)!=wm.windows.end();
        const bool ok=before==1&&after==0&&registered&&arrangeWin.visible&&!arrangeWin.minimized&&
                      g_arrangeRecTarget<0&&g_seqToTrack.empty();
        fprintf(stderr,"[new-project-probe] before=%d after=%d registered=%d visible=%d "
                       "minimized=%d routes=%d result=%s\n",before,after,registered?1:0,
                       arrangeWin.visible?1:0,arrangeWin.minimized?1:0,(int)g_seqToTrack.size(),
                       ok?"OK":"FAILED");
        PatchKnob::app::audio_app_shutdown(); app.shutdown(); return ok?0:3;
    }
    // Headless FREEZE PROBE: load a project, freeze the first sequence that has a
    // trigger, and report the span/tempo/captured-frames/peak/note-bursts so a
    // partial capture is measurable.  PATCHKNOB_FREEZE_PROBE=<path-to-.s24>
    if (const char* fp = getenv("PATCHKNOB_FREEZE_PROBE")) {
        const std::string probeBase = std::string(fp) + ".freeze";
        FILE* probeLog = std::freopen((probeBase + ".log").c_str(), "wb", stderr);
        (void)probeLog;
        if (!load_project(perf, fp, &autoPlayer)) {
            fprintf(stderr, "[freeze-probe] load failed: %s\n", project_io_last_error());
        } else {
            if (refresh_loaded_project) refresh_loaded_project();
            int target = -1, targetTrack = 0x7fffffff;
            for (int i = 0; i < c_max_sequence; ++i) {
                if (!perf.is_active(i)) continue;
                sequence* s = perf.get_sequence(i);
                auto mt = g_seqToTrack.find(i);
                const int tr = mt == g_seqToTrack.end() ? i : mt->second;
                if (s && s->get_max_trigger() > 0 && tr < targetTrack) {
                    target = i; targetTrack = tr;
                }
            }
            if (target < 0) fprintf(stderr, "[freeze-probe] no active sequence\n");
            else {
                sequence* s = perf.get_sequence(target);
                long maxTrig = s->get_max_trigger();
                if (maxTrig <= 0) maxTrig = std::max<long>(0, s->get_length() - 1);
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
                std::string wavError;
                const bool wroteWav = PatchKnob::engine::saveWav16(
                    probeBase + ".wav", clip, &wavError);
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
                fprintf(stderr, "[freeze-probe] wav=%s (%s)\n",
                        wroteWav ? "WRITTEN" : "FAILED",
                        wroteWav ? (probeBase + ".wav").c_str() : wavError.c_str());

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
                int alignLag=0x7fffffff;
                auto frIt=g_frozen.find(target);
                if(frIt!=g_frozen.end()&&frIt->second.freezeId>=0)
                    alignLag=PatchKnob::app::audio_app_freeze_alignment_lag(frIt->second.freezeId);
                fprintf(stderr, "[freeze-probe] on_freeze_track: activeSeqs %d->%d, playback peak=%.4f (%s)\n",
                        activeBefore, activeAfter, ppk, ppk > 0.001f ? "PLAYS" : "SILENT");
                if(alignLag!=0x7fffffff)
                    fprintf(stderr,"[freeze-probe] frozen playback correlation lag=%d samples (%s)\n",
                            alignLag,alignLag==0?"SAMPLE EXACT":"MISALIGNED");
                else if(frIt!=g_frozen.end()&&frIt->second.newSeq>=0) {
                    const int ds=frIt->second.newSeq;
                    auto rt=g_seqToTrack.find(ds); const auto* pc=vArrange.audio_clip(ds);
                    long long p0=0,o0=0,l0=0,p1=0,o1=0,l1=0;
                    bool got=rt!=g_seqToTrack.end()&&pc&&PatchKnob::app::audio_app_project_find_region(
                        rt->second,pc,&p0,&o0,&l0);
                    bool moved=got&&PatchKnob::app::audio_app_project_set_region(rt->second,pc,p0+1,o0,l0);
                    bool read=moved&&PatchKnob::app::audio_app_project_find_region(rt->second,pc,&p1,&o1,&l1);
                    bool restored=read&&p1==p0+1&&o1==o0&&l1==l0&&
                        PatchKnob::app::audio_app_project_set_region(rt->second,pc,p0,o0,l0);
                    fprintf(stderr,"[freeze-probe] persisted region position/source/length roundtrip=%s "
                                   "[%lld,%lld,%lld]\n",restored?"EXACT":"FAILED",p0,o0,l0);
                }
                if(frIt!=g_frozen.end()&&frIt->second.freezeId>=0){
                    const int fid=frIt->second.freezeId;
                    const bool r1=PatchKnob::app::audio_app_freeze_reverse(fid);
                    const bool r2=PatchKnob::app::audio_app_freeze_reverse(fid);
                    fprintf(stderr,"[freeze-probe] destructive edit reverse roundtrip=%s\n",
                            (r1&&r2)?"OK":"FAILED");
                    if(vArrange.on_unfreeze)vArrange.on_unfreeze(target);
                    fprintf(stderr,"[freeze-probe] teardown after edit=%s\n",
                            g_frozen.count(target)?"FAILED":"OK");
                }
            }
        }
        fflush(stderr);
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return 0;
    }

    app.roots = { &transportSmall };
    for (int i=0;i<NV;++i){ if (i==V_ARRANGE||i==V_PIANO||i==V_TRACKER||i==V_PATCH||i==V_MIXER) continue;  // window content
        views[i]->visible=(i==current); app.roots.push_back(views[i]); }
    app.roots.push_back(&wm);        // floating windows: over the workspace...
    if(auto* overlay=mobileUi.overlay())app.roots.push_back(overlay);
    app.roots.push_back(&menubar);   // ...menu bar LAST -> on top, mouse first
    // Punch record-mode hotkeys (PT p638/643, Windows bindings): Ctrl+Shift+P
    // QuickPunch, Ctrl+Shift+T TrackPunch, Ctrl+Shift+D DestructivePunch --
    // each toggles its mode against Normal.  Keys are dispatched to roots in
    // REVERSE order, so this zero-rect catcher (pushed after the menu bar)
    // sees them before the arrange view can eat 't' (theme toggle) or 'd'
    // (duplicate); it draws nothing and never hit-tests mouse input.
    struct PunchHotkeys : ui::Widget {
        std::function<bool(SDL_Keycode)> handler;
        void draw(ui::App&) override {}
        bool on_key(ui::App& a, SDL_Keycode k) override {
            (void)a; return handler && handler(k);
        }
    };
    static PunchHotkeys punchHotkeys;
    punchHotkeys.handler = [&](SDL_Keycode k) -> bool {
        const SDL_Keymod m = SDL_GetModState();
        if (!(m & KMOD_CTRL) || !(m & KMOD_SHIFT)) return false;
        int mode = -1;
        if      (k == SDLK_p) mode = 1;
        else if (k == SDLK_t) mode = 2;
        else if (k == SDLK_d) mode = 3;
        if (mode < 0 || !set_rec_mode) return false;
        set_rec_mode((int)g_recMode == mode ? 0 : mode);   // toggle vs Normal
        return true;
    };
    app.roots.push_back(&punchHotkeys);

    app.on_layout = [&](App& a){
        menubar.bar_h=ui::platform::menu_bar_height(a.ui_scale);
        wm.taskbar_h=ui::platform::taskbar_height(a.ui_scale);
        for (auto* win : wm.windows)
            if (win) win->title_h = win->titlebar
                   ? ui::platform::window_title_height(a.ui_scale) : 0;
        mobileUi.layout(a,menubar.bar_h);
        // apply scan/cache results whenever fresh (boot load or a rescan)
        if (g_scanReady.exchange(false, std::memory_order_acq_rel)) {
            vBrowser.set_scanning(false);
            { std::lock_guard<std::mutex> lk(g_scanMutex); vBrowser.populate(g_scan); }
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
        // Pd GUI feedback: drain messages that reached GUI atoms in the running patch
        // and reflect them onto the editor's widgets (bangs light up, sliders move as
        // wires drive them).  Keep redrawing while the editor is open so async audio-
        // thread feedback animates.
        if (g_pdNode >= 0 && pdWin.visible) {
            char recv[64]; float val = 0.f; int isBang = 0; int guard = 0;
            while (guard++ < 512 &&
                   PatchKnob::app::audio_app_pd_poll_gui(g_pdNode, recv, (int)sizeof(recv), &val, &isBang))
                pdEditor.apply_gui_feedback(recv, isBang != 0, val);
            a.request_redraw();
        }
        // Route raw character input to the Csound editor ONLY while its window is
        // focused (single-field text edits still take precedence in the loop).  We
        // must also (re)enable SDL text input here: end_text() calls
        // SDL_StopTextInput() after any field edit, which would otherwise leave the
        // code window unable to receive SDL_TEXTINPUT ("sometimes won't let you type").
        static bool s_csoundTextOn = false;
        const bool wantCsoundText = csoundWin.visible && csoundWin.focused && !a.editing_text();
        const bool wantTrackerText=ui::platform::tracker_accepts_text_input()&&
            trackerWin.visible&&trackerWin.focused&&!a.editing_text()&&!wantCsoundText;
        if (wantCsoundText) {
            a.text_input_sink = [&csoundEditor](const char* s){ csoundEditor.insert(s); };
            if (!s_csoundTextOn) { SDL_StartTextInput(); s_csoundTextOn = true; }
        } else if (wantTrackerText) {
            // Android's native keyboard emits SDL_TEXTINPUT. Feed each printable
            // character through TrackerView's existing note/hex/FX command path.
            a.text_input_sink = [&a,&vTracker](const char* s) {
                if(!s) return;
                for(const unsigned char* p=(const unsigned char*)s;*p;++p)
                    if(*p<0x80) vTracker.on_key(a,(SDL_Keycode)*p);
            };
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
            } else if (guiWin.visible && !guiWin.minimized) {
                int dw=a.w, dh=a.h; SDL_GetRendererOutputSize(a.ren, &dw, &dh);
                float rx = a.w>0 ? (float)dw/a.w : 1.f, ry = a.h>0 ? (float)dh/a.h : 1.f;
                // Follow a PLUGIN-INITIATED resize (VST3 resizeView / VST2
                // sizeWindow).  editor_native_size() is live now; it used to be
                // cached at open, so a plug-in that resized itself drew cropped
                // inside a stale frame forever.
                int enw=0, enh=0;
                if (PatchKnob::hostwin::editor_native_size(g_guiHandle,&enw,&enh) && enw>0 && enh>0) {
                    const float sc = a.scale>0 ? a.scale : 1.f;
                    const int fw=(int)(enw/sc)+2, fh=(int)(enh/sc)+guiWin.title_h+2;
                    if (fw!=guiWin.rect.w || fh!=guiWin.rect.h) {
                        guiWin.rect.w=fw; guiWin.rect.h=fh; a.request_redraw();
                    }
                }
                // A native child window ignores SDL's z-order and clipping, so
                // without this it paints over the menu bar, transport and dock.
                PatchKnob::hostwin::editor_set_clip(g_guiHandle,
                    (int)(wm.workspace.x*rx),(int)(wm.workspace.y*ry),
                    (int)(wm.workspace.w*rx),(int)(wm.workspace.h*ry));
                SDL_Rect b = guiWin.body();
                PatchKnob::hostwin::editor_set_bounds(g_guiHandle, (int)(b.x*rx),(int)(b.y*ry),
                                                  (int)(b.w*rx),(int)(b.h*ry));
                PatchKnob::hostwin::editor_show(g_guiHandle, true);
                PatchKnob::hostwin::editor_idle(g_guiHandle);
            } else {
                PatchKnob::hostwin::editor_show(g_guiHandle, false);
            }
        }
        // (The per-pattern FX pump that used to sit here has moved to on_frame:
        //  this callback is skipped on animation-only frames, so hands-off
        //  playback froze the automation for every off-screen pattern.)
        // Free deleted sequences a couple of frames after retirement (bounds the
        // graveyard instead of holding every deleted sequence until shutdown).
        perf.gc_graveyard();
        // live transport record: accumulate captured events while armed+rolling
        // (committed to a new timeline clip when REC is toggled off).
        if (g_recArmed && drain_record) drain_record();
        // punch pass lifecycle + RecordLock + Foley-mute housekeeping (PT ch.27)
        if (punch_poll) punch_poll();
        // (The second record path that drained a RecordNode straight into seq0
        //  lived here.  There is one recorder now -- see drain_record above.)
        const int menuH = menubar.bar_h;     // row 1: menu bar
        const int th    = 26;                // row 2: transport / tabs
        menubar.rect   = { 0, 0, a.w, a.h }; // full-window for click capture; paints bar+dropdown only
        wm.rect        = { 0, 0, a.w, a.h }; // floating-window layer (hit-tests its windows)
        wm.workspace   = { 0, menuH+th, a.w, a.h-menuH-th };  // max bounds + minimized tray
        transportSmall.rect = { 0, menuH, a.w, th };          // compact transport strip
        SDL_Rect content = { 0, menuH+th, a.w, a.h-menuH-th };
        for (int i=0;i<NV;++i){ if (i==V_ARRANGE||i==V_PIANO||i==V_TRACKER||i==V_PATCH||i==V_MIXER) continue; views[i]->rect = content; }
        // Patchbay window snapped to the bottom (full width, lower third).
        if (patchWin.visible) { int dh = a.h/3; if (dh < 200) dh = 200;
            patchWin.rect = { 0, a.h - dh, a.w, dh }; }
        // Arrange owns the workspace between transport and dock. Its title bar
        // and client area are wholly above the dock; bottom/right scrollbars are
        // therefore inside the visible Arrange window rather than behind it.
        if(arrangeWin.visible){
            const int top=menuH+th;
            const int bottom=patchWin.visible?patchWin.rect.y:a.h;
            arrangeWin.rect={0,top,a.w,std::max(80,bottom-top)};
        }
    };
    // Patch mixers accumulate per-block peaks on the audio thread. Publish
    // those accumulators once per painted frame, before arrange and mixer views
    // read them. This must not live in on_layout: animation frames deliberately
    // skip layout, which left both track and master meters permanently at zero.
    app.on_frame = [&](App& a){
        PatchKnob::app::audio_app_meters_latch();
#ifdef PATCHKNOB_HAS_AI
        // Claude chat pump: poll() must run once per painted frame on the
        // message thread, ALWAYS -- on_frame is the one callback that runs on
        // every painted frame (on_layout skips animation-only ones).  Hidden
        // panel/window only mutes the smooth-streaming redraws; a reply that
        // lands after the user closes the panel is still drained here and the
        // client returns to idle instead of stranding it.
        csoundEditor.pumpAi(a, csoundWin.visible && !csoundWin.minimized);
#endif
        // ---- asynchronous soundfont import pump --------------------------
        // take() delivers a finished prepare ONLY if it is still the newest
        // request -- a superseded one (the user clicked another preset) is
        // destroyed inside the service and never seen here, so stale zones
        // cannot be installed no matter how clicks interleave with the worker.
        // Installing runs here, on the message thread, per the sampler's
        // threading contract; measured at 13-27 ms for a 2976-zone preset,
        // versus the 110-520 ms the old synchronous path blocked for.
        if (sf2ImportToken != 0) {
            PatchKnob::engine::sf2::Sf2ImportService::Completed done;
            if (sf2Import.take(done)) {
                sf2ImportToken = 0;
                const int node = sf2ImportNode;
                PatchKnob::engine::IPluginInstance* inst =
                    node >= 0 ? PatchKnob::app::audio_app_patch_node_instance(node) : nullptr;
                if (!done.ok) {
                    projectStatus = "Import failed: " + done.error;
                } else if (!inst) {
                    // The instrument was deleted while the prepare ran; the
                    // decoded zones have nowhere to land.  Drop them.
                    projectStatus = "Import dropped: the Sampler instrument was closed";
                } else {
                    PatchKnob::engine::sf2::ImportOptions opts;
                    PatchKnob::engine::sf2::ImportResult result;
                    std::string err;
                    if (!PatchKnob::engine::sf2::installPreparedPreset(inst, done.prepared,
                                                                       opts, result, err)) {
                        projectStatus = "Import failed: " + err;
                    } else {
                        // The installer loaded zones straight through the
                        // engine's C API, bypassing g_samplerZones -- rebuild
                        // the shell's metadata-only copy from the engine.  The
                        // editor is rebound only if it still shows this node
                        // (the user may have moved it mid-prepare).
                        rebuild_zones_from_engine(node);
                        if (vSamplerEd.node() == node) {
                            sampler_fill_zone_clip(node, 0); // only the zone we open on
                            vSamplerEd.bind(node, &g_samplerZones[node],
                                            &g_samplerEnv[node], inst);
                            if (!g_samplerZones[node].empty() && vSamplerEd.on_edit_zone)
                                vSamplerEd.on_edit_zone(0);
                        }
                        std::string status = "Imported \"" + done.label + "\": " +
                                             std::to_string(result.zonesLoaded) + " zones, " +
                                             std::to_string(result.samplesInterned) + " samples interned";
                        if (result.stereoPairs > 0)
                            status += ", " + std::to_string(result.stereoPairs) + " stereo pairs";
                        if (result.zonesSkipped > 0)
                            status += " (" + std::to_string(result.zonesSkipped) + " skipped)";
                        if (!result.warning.empty())
                            status += "; " + result.warning;
                        projectStatus = status;
                        // Warm the page cache for the NEXT preset in this bank
                        // -- the one a user auditioning down the list clicks
                        // next.  Runs at the service's lowest priority and is
                        // preempted by any real click; measured to turn that
                        // preset's cold ~600 ms prepare into a warm one.
                        sf2Import.prefetchNext(done.fontPath, done.bank, done.program);
                    }
                }
                a.request_redraw();
            } else {
                // Still preparing: show live progress and keep frames coming.
                const auto pr = sf2Import.progress();
                if (pr.active && pr.samplesTotal > 0) {
                    char buf[192];
                    std::snprintf(buf, sizeof buf, "Importing \"%s\"... %d/%d samples (%d%%)",
                                  pr.label.c_str(), pr.samplesDone, pr.samplesTotal,
                                  (int)(100.0 * pr.samplesDone / pr.samplesTotal));
                    projectStatus = buf;
                }
                a.request_redraw();
            }
        }
        // Engine-driven FX for every active pattern other than the one shown in
        // the tracker (its live row dispatch is handled below by poll_playhead).
        // This plays each active pattern's VST-param automation regardless of
        // which window is focused and for multiple/consecutive patterns on the
        // same track.
        //
        // This lived in on_layout, which is the same mistake the meters above
        // had: on_layout only runs on frames where `dirty` was set, and
        // `animating` alone does not set it.  So with playback rolling and the
        // mouse untouched, off-screen patterns stopped emitting automation
        // entirely -- and the next incidental dirty frame fired every FX row
        // crossed since in one burst, snapping parameters instead of sweeping
        // them.  on_frame runs on EVERY frame, animation frames included.
        if (a.animating) {
            // Per-slot last-fired TICK (-1 none).  Was a row index on a
            // hardcoded LPB-4 grid, which meant any pattern authored at a
            // finer LPB had most of its FX values skipped entirely.
            static std::vector<long long> s_fxTick;
            if ((int)s_fxTick.size() < c_max_sequence) s_fxTick.assign(c_max_sequence, -1);
            sequence* shown = vTracker.get_sequence();
            for (int i = 0; i < c_max_sequence; ++i) {
                if (!perf.is_active(i)) { s_fxTick[i] = -1; continue; }
                sequence* s = perf.get_sequence(i);
                if (!s || s == shown || !s->get_playing()) { s_fxTick[i] = -1; continue; }
                const long long len = std::max(1L, (long)s->get_length());
                const long long cur = ((long long)s->get_last_tick() % len + len) % len;
                if (cur != s_fxTick[i]) {
                    ui::TrackerView::play_pattern_fx(s, s_fxTick[i], cur);
                    s_fxTick[i] = cur;
                }
            }
        }
        // Emit automation while playing (coarse UI-thread pump, block-granular).
        // Tick comes from the TRANSPORT (perform derives m_tick from the audio
        // clock) -- never from a cached sequence pointer, which dangles if that
        // sequence is deleted in the arrange view (was a use-after-free crash).
        // prevAutomationTick is NOT a function static any more: transport_seek has
        // to be able to move it, or a locate leaves the walk below starting from
        // the pre-seek position (see automation_reprime above).
        if(a.animating){
            const long cur=perf.get_tick();
            if(cur!=prevAutomationTick){
                // A window walk is only meaningful across CONTINUOUS forward
                // transport motion.  Backward motion (locate, loop wrap) makes the
                // window empty and advanceScheduled a documented no-op -- that is
                // how a lane kept a stale value forever.  A huge forward jump is a
                // locate too, and walking it would enqueue the whole skipped span
                // stamped at ticks far behind the playhead.  Re-prime at the new
                // position instead.  This also covers the rolling case where
                // perform's scheduler has not yet snapped m_tick onto the queued
                // seek: whichever order the two settle in, each discontinuous step
                // re-primes and the walk never spans a jump.
                const long span = cur - prevAutomationTick;
                if(span < 0 || span > c_automationWalkLimit){
                    automation_reprime(cur);
                } else {
                    automation_sync_region_spans();
                    // Track lanes carry a full LaneTarget too: a lane added in
                    // the automation editor may name a PATCH-GRAPH node, which
                    // the (track,id) callback cannot express.  Pass the same
                    // target-aware router the region pass below uses, or every
                    // patch-graph lane on a track silently controls nothing.
                    autoPlayer.advanceScheduled(prevAutomationTick,cur,
                        [](int trk,unsigned id,float v,int64_t due){PatchKnob::app::audio_app_route_param_at_tick(trk,id,v,due);},
                        [](int trk,int cc,int val,int64_t due){PatchKnob::app::audio_app_route_midi(
                            trk,0xB0,(unsigned char)cc,(unsigned char)val,due);},
                        [](int trk,const PatchKnob::engine::LaneTarget& target,float v,int64_t due){
                            using PatchKnob::engine::LaneTargetKind;
                            if(target.kind==LaneTargetKind::PatchParam)
                                PatchKnob::app::audio_app_patch_route_param(target.node,target.id,v);
                            else if(target.kind==LaneTargetKind::RackParam){
                                rackx::RackEngine* eng=PatchKnob::app::audio_app_rack_engine(target.node);
                                if(eng)eng->setParam(target.module,(int)target.id,
                                    target.minValue+(target.maxValue-target.minValue)*v);
                            }else if(trk>=0)
                                PatchKnob::app::audio_app_route_param_at_tick(trk,target.id,v,due);
                        });
                    autoPlayer.advanceRegionsScheduled(prevAutomationTick,cur,
                        [](int trk,const PatchKnob::engine::LaneTarget& target,float v,int64_t due){
                            using PatchKnob::engine::LaneTargetKind;
                            if(target.kind==LaneTargetKind::PatchParam)
                                PatchKnob::app::audio_app_patch_route_param(target.node,target.id,v);
                            else if(target.kind==LaneTargetKind::RackParam){
                                rackx::RackEngine* eng=PatchKnob::app::audio_app_rack_engine(target.node);
                                if(eng)eng->setParam(target.module,(int)target.id,
                                    target.minValue+(target.maxValue-target.minValue)*v);
                            }else if(target.kind==LaneTargetKind::VstParam&&trk>=0)
                                PatchKnob::app::audio_app_route_param_at_tick(trk,target.id,v,due);
                        },[](int trk,int cc,int val,int64_t due){if(trk>=0)PatchKnob::app::audio_app_route_midi(
                            trk,0xB0,(unsigned char)cc,(unsigned char)val,due);});
                }
                prevAutomationTick=cur;
            }
        } else prevAutomationTick=perf.get_tick();
        // Playback-only frames deliberately skip on_layout.  Polling the
        // tracker there made its progress highlight update only on incidental
        // dirty/full-layout frames, so it visibly stalled and jumped.  Sample
        // the sequence clock on every animated frame; request a full redraw
        // only when the displayed tracker row actually changes.
        if (a.animating && vTracker.poll_playhead())
            a.request_redraw();
    };

    // Device-loss watchdog: if the audio device vanished (unplugged / stream
    // died), try to reopen on the default device.  ~4 Hz, message thread.
    // Raise the idle refresh while the master bus is audible.  track -1 is the
    // mix bus output (post master gain) -- the same source every meter is fed
    // from.  -60 dBFS is where the meters bottom out, so below it there is
    // nothing left to animate.
    app.meters_hot = [&]() -> bool {
        if (!audio_ok) return false;
        constexpr float kFloor = 0.001f;             // -60 dBFS
        return PatchKnob::app::audio_app_master_strip_peak(-1, 0) > kFloor
            || PatchKnob::app::audio_app_master_strip_peak(-1, 1) > kFloor;
    };
    app.on_tick = [&](App& a){
        if (audio_ok && PatchKnob::app::audio_app_device_lost()) {
            if (PatchKnob::app::audio_app_try_recover())
                std::fprintf(stderr, "[audio] device lost -> recovered on default device\n");
            a.request_redraw();
        }
    };

    // ---- project-wide undo / redo ----------------------------------------
    // Every input gesture is a transaction at the App boundary.  Persist full
    // project states to a bounded disk ring: this covers heterogeneous compound
    // edits (rack/Pd/Csound/patch routing/audio regions) without duplicating
    // potentially multi-megabyte audio buffers in RAM.
    std::deque<std::string> undoStates, redoStates;
    std::string currentState;
    bool historyApplying=false, historyGesture=false;
    std::string gestureBefore;
    uint64_t historySerial=0;
    std::string historyDir;
    if (char* pref=SDL_GetPrefPath("PatchKnob","PatchKnob")) {
        historyDir=(std::filesystem::path(pref)/"project_history").string();
        SDL_free(pref);
    } else historyDir=(std::filesystem::temp_directory_path()/"PatchKnob_project_history").string();
    std::error_code historyEc;
    std::filesystem::create_directories(historyDir,historyEc);

    auto same_file=[](const std::string& a,const std::string& b)->bool {
        std::error_code ec;
        if(std::filesystem::file_size(a,ec)!=std::filesystem::file_size(b,ec)||ec) return false;
        std::ifstream fa(a,std::ios::binary),fb(b,std::ios::binary);
        char ba[65536],bb[65536];
        while(fa&&fb){ fa.read(ba,sizeof ba); fb.read(bb,sizeof bb);
            std::streamsize n=fa.gcount(); if(n!=fb.gcount())return false;
            if(n>0&&std::memcmp(ba,bb,(size_t)n)!=0)return false; }
        return true;
    };
    auto snapshot_state=[&]()->std::string {
        vTracker.commit_fx();
        std::vector<ProjectPatchNodePosition> layout;
        layout.reserve(vPatch.nodes().size());
        for(const auto& n:vPatch.nodes()) layout.push_back({(uint32_t)n.id,n.x,n.y});
        auto freezes=collect_freezes?collect_freezes():std::vector<ProjectFreezeRecord>{};
        std::string path=(std::filesystem::path(historyDir)/
            ("state_"+std::to_string(++historySerial)+".s24")).string();
        if(!save_project(perf,path,layout,freezes,&autoPlayer)) return {};
        return path;
    };
    auto trim_history=[&](std::deque<std::string>& states) {
        constexpr size_t MaxStates=32;
        while(states.size()>MaxStates){
            std::error_code ec; std::filesystem::remove(states.front(),ec); states.pop_front();
        }
    };
    auto restore_state=[&](const std::string& path)->bool {
        historyApplying=true;
        // Undo/redo restores a whole project file, which deletes the sequence
        // pool the running take is aimed at.  Punch out first (see
        // punch_out_recording) -- the alternative is a capture still open on a
        // dead lane and a commit into someone else's clip.
        if (punch_out_recording) punch_out_recording();
        // A project load rebuilds patch/rack node ownership. Close editors that
        // hold pointers/IDs into the old graph so undo can never leave a stale
        // RackEngine or script target live.
        pdWin.visible=csoundWin.visible=rackWin.visible=panelEditorWin.visible=false;
        rackEditor.set_engine(nullptr); panelEditor.set_target(nullptr,-1);
        paramView.forget_all();          // every plugin instance is about to die
        g_pdNode=g_csoundNode=g_rackNode=g_pdRackMod=g_csoundRackMod=-1;
        bool ok=load_project(perf,path,&autoPlayer);
        if(ok&&refresh_loaded_project) refresh_loaded_project();
        historyApplying=false;
        app.request_redraw();
        return ok;
    };
    currentState=snapshot_state();
    app.on_edit_begin=[&]{
        if(historyApplying||historyGesture)return;
        historyGesture=true;
        // Async recording/freeze commits happen between UI gestures. Capture
        // the real pre-edit state now so Undo Cut cannot restore a stale state
        // from before the recorded take existed.
        gestureBefore=snapshot_state();
    };
    app.on_edit_end=[&]{
        if(historyApplying||!historyGesture)return;
        historyGesture=false;
        std::string next=snapshot_state();
        if(next.empty()) {
            if(!gestureBefore.empty()){std::error_code ec;std::filesystem::remove(gestureBefore,ec);}
            gestureBefore.clear(); return;
        }
        const std::string before=!gestureBefore.empty()?gestureBefore:currentState;
        gestureBefore.clear();
        if(!before.empty()&&same_file(before,next)){
            std::error_code ec;std::filesystem::remove(next,ec);
            if(before!=currentState)std::filesystem::remove(before,ec);
            return;
        }
        if(!before.empty())undoStates.push_back(before);
        if(!currentState.empty()&&currentState!=before){std::error_code ec;std::filesystem::remove(currentState,ec);}
        currentState=next;
        for(const auto& p:redoStates){std::error_code ec;std::filesystem::remove(p,ec);}
        redoStates.clear(); trim_history(undoStates);
        projectStatus="Edited (undo available)";
    };
    global_undo=[&]{
        if(historyGesture&&app.on_edit_end)app.on_edit_end();
        if(undoStates.empty())return;
        const std::string target=undoStates.back();
        const std::string previous=currentState;
        if(restore_state(target)){
            undoStates.pop_back();
            if(!previous.empty())redoStates.push_back(previous);
            currentState=target;projectStatus="Undo";
        }
        trim_history(redoStates);
    };
    global_redo=[&]{
        if(historyGesture&&app.on_edit_end)app.on_edit_end();
        if(redoStates.empty())return;
        const std::string target=redoStates.back();
        const std::string previous=currentState;
        if(restore_state(target)){
            redoStates.pop_back();
            if(!previous.empty())undoStates.push_back(previous);
            currentState=target;projectStatus="Redo";
        }
        trim_history(undoStates);
    };
    // Swapping the project out from under the shell (File>New / File>Open) is
    // NOT an undoable edit -- there is no "undo the open", and the ring's
    // states describe a song that is no longer loaded.  Two things to do:
    //   * abandon the gesture that is still open.  on_edit_begin fires on the
    //     mouse-DOWN that opens the File menu and New/Open run synchronously
    //     inside that click, so the mouse-UP would otherwise call on_edit_end
    //     and push the OLD project as an undo step.
    //   * drop both rings (and their disk states) and re-baseline currentState
    //     on the project that is now loaded.
    reset_undo_history=[&]{
        std::error_code ec;
        historyGesture=false;
        if(!gestureBefore.empty()){ std::filesystem::remove(gestureBefore,ec); gestureBefore.clear(); }
        for(const auto& pth:undoStates) std::filesystem::remove(pth,ec);
        for(const auto& pth:redoStates) std::filesystem::remove(pth,ec);
        undoStates.clear(); redoStates.clear();
        if(!currentState.empty()) std::filesystem::remove(currentState,ec);
        currentState=snapshot_state();
    };
    app.on_global_undo=[&]{global_undo();};
    app.on_global_redo=[&]{global_redo();};

    // Headless transport-LOOP regression harness on a REAL project.  See the
    // comment block above main().
    if (const char* lrp = getenv("PATCHKNOB_LOOPREPRO")) {
        fprintf(stderr, "[looprepro] loading %s\n", lrp); fflush(stderr);
        vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
        vPiano.cancel_interaction(app);   vPiano.set_sequence(nullptr);
        bool ok = load_project(perf, lrp, &autoPlayer);
        if (ok) {
            if (clear_project_edit_state) clear_project_edit_state();
            vArrange.reset_project_state();
            if (refresh_loaded_project) refresh_loaded_project();
        }
        if (!ok) { fprintf(stderr, "[looprepro] FAIL: load failed\n");
                   PatchKnob::app::audio_app_shutdown(); app.shutdown(); return 2; }

        auto envl = [](const char* k, long d)->long {
            const char* v = getenv(k); return v ? strtol(v, nullptr, 10) : d; };

        long loopL = envl("PATCHKNOB_LOOPREPRO_L", 0);
        long loopR = envl("PATCHKNOB_LOOPREPRO_R", 0);
        if (loopR <= loopL) {                    // derive: end of the last trigger
            long mx = perf.get_max_trigger();
            const long bar = c_ppqn * 4;
            loopR = ((mx + bar) / bar) * bar;    // round up to the next bar line
            if (loopR <= loopL) loopR = loopL + bar;
        }
        const int passesWanted = (int) envl("PATCHKNOB_LOOPREPRO_PASSES", 20);
        if (const char* bp = getenv("PATCHKNOB_LOOPREPRO_BPM"))
            perf.set_bpm(atof(bp));

        fprintf(stderr, "[looprepro] loop [%ld,%ld) len=%ld ticks, %.2f BPM, "
                "%d passes\n", loopL, loopR, loopR - loopL, perf.get_bpm(),
                passesWanted);
        fprintf(stderr, "[looprepro] tick_to_sample: L=%lld R=%lld  (%.1f ms at "
                "%d Hz, block %d, lookahead ~%lld ticks)\n",
                (long long) PatchKnob::app::audio_app_tick_to_sample(loopL),
                (long long) PatchKnob::app::audio_app_tick_to_sample(loopR),
                1000.0 * (double)(PatchKnob::app::audio_app_tick_to_sample(loopR)
                                - PatchKnob::app::audio_app_tick_to_sample(loopL))
                       / (double) PatchKnob::app::audio_app_sample_rate(),
                (int) PatchKnob::app::audio_app_sample_rate(),
                (int) PatchKnob::app::audio_app_buffer_size(),
                (long long)( PatchKnob::app::audio_app_sample_to_tick(
                        2 * (long long)PatchKnob::app::audio_app_buffer_size()
                        + (long long)(0.001 * c_thread_trigger_lookahead_ms
                                      * PatchKnob::app::audio_app_sample_rate()))));
        for (int i = 0; i < c_max_sequence; ++i) if (perf.is_active(i)) {
            sequence* s = perf.get_sequence(i);
            fprintf(stderr, "[looprepro]   seq %2d \"%s\" len=%ld bus=%d "
                    "events=%d triggers=%d\n", i, s->get_name(), s->get_length(),
                    (int) s->get_midi_bus(), (int) s->event_count(),
                    (int) s->trigger_count());
        }
        fflush(stderr);

        // Roll with the loop on, in SONG (playback) mode -- the mode the wrap
        // branch of perform::output_func only ever runs in.
        perf.set_left_tick(loopL);
        perf.set_right_tick(loopR);
        perf.set_starting_tick(loopL);
        perf.set_looping(true);

        //  PATCHKNOB_LOOPREPRO_CSOUND=1 -- replace the project's instrument with
        //  a Csound sine and route every sequence to it.  The dropout was first
        //  seen on a Sampler clip; the user reports it silences Csound
        //  instruments too, so this proves the fault is in the CLIP/dispatch
        //  path and not in any one instrument.
        if (getenv("PATCHKNOB_LOOPREPRO_CSOUND")) {
            const int cn = PatchKnob::app::audio_app_patch_add_csound();
            if (cn < 0) fprintf(stderr, "[looprepro] no Csound in this build\n");
            else {
                PatchKnob::app::audio_app_patch_csound_set_text(cn,
                    "<CsoundSynthesizer>\n<CsInstruments>\n"
                    "sr = 48000\nksmps = 32\nnchnls = 2\nnchnls_i = 2\n0dbfs = 1\n"
                    "massign 0, 1\n"
                    "instr 1\n icps cpsmidi\n iamp ampmidi 0.5\n"
                    "  kenv madsr 0.002, 0.02, 0.8, 0.05\n"
                    "  a1 oscili iamp*kenv, icps\n  outs a1, a1\nendin\n"
                    "</CsInstruments>\n<CsScore>\nf 0 86400\n</CsScore>\n"
                    "</CsoundSynthesizer>\n");
                if (!PatchKnob::app::audio_app_patch_csound_recompile(cn)) {
                    fprintf(stderr, "[looprepro] csound compile failed: %s\n",
                            PatchKnob::app::audio_app_patch_csound_error(cn));
                } else {
                    PatchKnob::app::audio_app_set_modular(true);
                    PatchKnob::app::audio_app_patch_set_node_channel(cn, -1);
                    const int ct = PatchKnob::app::audio_app_master_add_track(0);
                    PatchKnob::app::audio_app_master_connect_instrument(ct, cn);
                    for (int i = 0; i < c_max_sequence; ++i)
                        if (perf.is_active(i))
                            perf.get_sequence(i)->set_midi_bus((char)ct);
                    fprintf(stderr, "[looprepro] CSOUND MODE: node %d on track %d; "
                            "every sequence routed there\n", cn, ct);
                }
            }
        }

        looprepro::g_perf    = &perf;
        looprepro::g_recs.reserve(200000);
        looprepro::g_capture = true;
        g_seq_emit_tap       = looprepro::tap;

        // Render everything, play nothing: a multi-minute loop soak must not
        // blast the speakers.  PATCHKNOB_LOOPREPRO_AUDIBLE=1 to listen.
        PatchKnob::app::audio_app_set_silent_output(
            getenv("PATCHKNOB_LOOPREPRO_AUDIBLE") == nullptr);

        g_loop_trace.clear();
        g_loop_trace.reserve(400000);
        g_loop_trace_on = true;

        perf.start(true);

        {   // transport rate sanity: the playhead must advance ONE sample per
            // sample of wall clock.  Anything else means the render callback is
            // being pumped by something other than the device.
            const long long s0 = PatchKnob::app::audio_app_transport_sample();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const long long s1 = PatchKnob::app::audio_app_transport_sample();
            fprintf(stderr, "[looprepro] transport rate: %lld samples in 500 ms "
                    "(x%.2f realtime)\n", (long long)(s1 - s0),
                    (double)(s1 - s0) / (0.5 * PatchKnob::app::audio_app_sample_rate()));
        }

        const unsigned long long gen0 =
            PatchKnob::app::audio_app_loop_wrap_generation();
        unsigned long long gen = gen0;
        const int budgetMs = 240000;
        int waited = 0, lastWrapMs = 0;
        // Log every engine wrap with the wall time since the previous one and
        // the transport tick it wrapped FROM: a wrap that arrives far too early
        // (or twice in a row) is an engine-side loop bug, not a scheduler one.
        std::vector<std::string> wrapLog;
        long long peak = 0;
        // AUDIO per pass, not just MIDI: the emit tap proves what the
        // sequencer queued, but the reported symptom is silence at the
        // SPEAKER.  Latch the meters every ~50 ms and fold the master mix-bus
        // VU into a per-pass maximum; a pass whose MIDI is present but whose
        // peak is ~0 pins the loss downstream of the sequencer.
        float passPeak = 0.f, passTrk0 = 0.f;
        int   sinceLatch = 0;
        while ((int)(gen - gen0) < passesWanted + 1 && waited < budgetMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited += 2;
            sinceLatch += 2;
            if (sinceLatch >= 50) {
                sinceLatch = 0;
                PatchKnob::app::audio_app_meters_latch();
                const float ml = PatchKnob::app::audio_app_master_strip_peak(-1, 0);
                const float mr = PatchKnob::app::audio_app_master_strip_peak(-1, 1);
                const float t0 = PatchKnob::app::audio_app_master_strip_peak(0, 0);
                if (ml > passPeak) passPeak = ml;
                if (mr > passPeak) passPeak = mr;
                if (t0 > passTrk0) passTrk0 = t0;
            }
            const long long sNow = PatchKnob::app::audio_app_transport_sample();
            if (sNow > peak) peak = sNow;
            const unsigned long long g =
                PatchKnob::app::audio_app_loop_wrap_generation();
            if (g != gen) {
                //  DISPATCH CENSUS per pass.  The MIDI table above is taken at
                //  the sequencer's emit tap and shows a perfect pass even when
                //  the audio drops out, so the deltas that matter are these:
                //  how many events were enqueued for the instrument and how
                //  many actually reached it, plus every reason one did not.
                static unsigned long long pEnq=0,pDel=0,pRF=0,pSE=0,pUR=0,pNF=0;
                unsigned long long cEnq=0,cDel=0,cRF=0,cSE=0,cUR=0,cNF=0;
                PatchKnob::app::audio_app_midi_census(&cEnq,&cDel,&cRF,&cSE,&cUR,&cNF);
                char b[420];
                snprintf(b, sizeof(b), "[looprepro] wrap #%d at %d ms (+%d ms) "
                         "wrapped from sample<=%lld (tick %lld) -> tick=%lld  "
                         "seq_last_tick=%ld  audio: master=%.4f trk0=%.4f"
                         "  midi: enq=%llu deliv=%llu | brkFuture=%llu brkEpoch=%llu "
                         "unreach=%llu brkWrapped=%llu",
                         (int)(g - gen0), waited, waited - lastWrapMs, peak,
                         (long long) PatchKnob::app::audio_app_sample_to_tick(peak),
                         (long long) PatchKnob::app::audio_app_sample_to_tick(sNow),
                         perf.get_sequence(1) ? perf.get_sequence(1)->get_last_tick() : -1L,
                         (double) passPeak, (double) passTrk0,
                         cEnq-pEnq, cDel-pDel, cRF-pRF, cSE-pSE, cUR-pUR, cNF-pNF);
                pEnq=cEnq; pDel=cDel; pRF=cRF; pSE=cSE; pUR=cUR; pNF=cNF;
                wrapLog.push_back(b);
                lastWrapMs = waited; gen = g; peak = 0;
                passPeak = 0.f; passTrk0 = 0.f;
            }
        }
        looprepro::g_capture = false;
        g_seq_emit_tap = nullptr;
        perf.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_loop_trace_on = false;

        {   long long ls=0, le=0, lt=0, rt=0; int on=0;
            PatchKnob::app::audio_app_loop_debug(&ls,&le,&lt,&rt,&on);
            fprintf(stderr, "[looprepro] engine loop state: on=%d ticks[%lld,%lld) "
                    "samples[%lld,%lld) -> %.1f ms\n", on, lt, rt, ls, le,
                    1000.0*(double)(le-ls)/(double)PatchKnob::app::audio_app_sample_rate());
        }
        fprintf(stderr, "[looprepro] engine wraps observed: %llu in %d ms\n",
                (unsigned long long)(gen - gen0), waited);
        for (const auto& w : wrapLog) fprintf(stderr, "%s\n", w.c_str());

        // Scheduler trace: print only the interesting rows -- every branch
        // change, plus the first rows of each wrap -- so the table stays legible.
        if (getenv("PATCHKNOB_LOOPREPRO_TRACE")) {
            fprintf(stderr, "\n[looprepro] scheduler trace (%d rows)\n"
                    "[looprepro]    ms | branch | transport |  horizon | "
                    "last_sched | L..R | wrapgen\n", (int) g_loop_trace.size());
            const long long t0 = g_loop_trace.empty() ? 0 : g_loop_trace[0].ms;
            int lastBranch = -99; unsigned long long lastGen = 0; int printed = 0;
            for (size_t i = 0; i < g_loop_trace.size() && printed < 400; ++i) {
                const loop_trace_rec& r = g_loop_trace[i];
                const bool interesting = (r.branch != lastBranch) ||
                                         (r.wrap_gen != lastGen);
                lastBranch = r.branch; lastGen = r.wrap_gen;
                if (!interesting) continue;
                ++printed;
                fprintf(stderr, "[looprepro] %5lld |   %d    | %9lld | %8lld | "
                        "%10lld | %lld..%lld | %llu\n",
                        r.ms - t0, r.branch, r.transport_tick, r.horizon,
                        r.last_scheduled, r.left, r.right, r.wrap_gen);
            }
        }

        // --- segment the emit log into passes ------------------------------
        // Within one pass the scheduler hands each sequence a monotonically
        // increasing tick window; the wrap is the ONLY place the tick goes
        // backwards.  Segment per sequence on that regression.  tick<0 offs
        // (panic / loop boundary) belong to the pass that is closing.
        struct PassRow { int ons=0, offs=0, panic=0, boundary=0, retrig=0;
                         long lo=-1, hi=-1;
                         std::multiset<std::pair<long,int> > onSet; };
        int failures = 0;
        for (int si = 0; si < c_max_sequence; ++si) {
            std::vector<PassRow> passes;
            long prevTick = -1; bool any = false;
            for (const auto& r : looprepro::g_recs) {
                if (r.seq != si) continue;
                if (r.tick >= 0 && (!any || r.tick < prevTick)) { passes.push_back(PassRow()); any = true; }
                if (passes.empty()) passes.push_back(PassRow());
                if (r.tick >= 0) prevTick = r.tick;
                PassRow& p = passes.back();
                if (r.tick >= 0) { if (p.lo < 0 || r.tick < p.lo) p.lo = r.tick;
                                   if (r.tick > p.hi) p.hi = r.tick; }
                if (r.kind == 2) p.panic++;
                else if (r.kind == 3) p.boundary++;
                else if (r.kind == 1) p.retrig++;
                else if ((r.status & 0xF0) == EVENT_NOTE_ON && r.vel > 0)
                     { p.ons++; p.onSet.insert(std::make_pair(r.tick,(int)r.note)); }
                else if ((r.status & 0xF0) == EVENT_NOTE_OFF ||
                         ((r.status & 0xF0) == EVENT_NOTE_ON && r.vel == 0)) p.offs++;
            }
            if (passes.empty()) continue;
            sequence* s = perf.get_sequence(si);
            // reference == the fullest pass, ignoring the first (transport
            // start) and last (truncated by stop)
            int refIdx = (passes.size() > 2) ? 1 : 0;
            for (size_t p = 1; p + 1 < passes.size(); ++p)
                if (passes[p].ons > passes[refIdx].ons) refIdx = (int) p;
            fprintf(stderr, "\n[looprepro] === seq %d \"%s\" : %d passes "
                    "(reference pass %d, %d note-ons) ===\n",
                    si, s ? s->get_name() : "?", (int) passes.size(),
                    refIdx, passes[refIdx].ons);
            fprintf(stderr, "[looprepro] pass |  ons offs retrig panic bdry | "
                    "tick span   | missing vs reference (tick:note)\n");
            for (size_t p = 0; p < passes.size(); ++p) {
                std::multiset<std::pair<long,int> > miss = passes[refIdx].onSet;
                for (auto it = passes[p].onSet.begin(); it != passes[p].onSet.end(); ++it) {
                    auto f = miss.find(*it); if (f != miss.end()) miss.erase(f);
                }
                std::string m; int shown = 0;
                for (auto it = miss.begin(); it != miss.end() && shown < 10; ++it, ++shown)
                    { m += std::to_string(it->first); m += ':';
                      m += std::to_string(it->second); m += ' '; }
                if ((int) miss.size() > shown) m += "...(+" +
                    std::to_string((int) miss.size() - shown) + ")";
                const bool interior = ((int) p > 0 && (int) p + 1 < (int) passes.size());
                const bool bad = interior && !miss.empty();
                if (bad) ++failures;
                fprintf(stderr, "[looprepro] %4d | %4d %4d %6d %5d %4d | %5ld..%-5ld | %s%s\n",
                        (int) p, passes[p].ons, passes[p].offs, passes[p].retrig,
                        passes[p].panic, passes[p].boundary, passes[p].lo, passes[p].hi,
                        m.c_str(), bad ? "  <<< MISSING" : "");
            }
        }
        fprintf(stderr, "\n[looprepro] %s (%d mismatched passes)\n",
                failures ? "FAIL" : "PASS", failures);
        fflush(stderr);
        looprepro::g_recs.clear();
        PatchKnob::app::audio_app_shutdown(); app.shutdown();
        return failures ? 2 : 0;
    }

    // Smoke harness: load a project and press play, then fall through into the
    // REAL gui loop.  Unlike the headless self-tests above this keeps the window
    // and the draw thread alive, which is the only way to catch a hang that needs
    // the GUI and the sequencer to contend -- the sequence::play() lock leak that
    // froze the arrange view was invisible to every headless harness.
    if (const char* hp = getenv("PATCHKNOB_HANGREPRO")) {
        fprintf(stderr, "[hang] loading %s\n", hp); fflush(stderr);
        vTracker.cancel_interaction(app); vTracker.set_sequence(nullptr,-1);
        vPiano.cancel_interaction(app); vPiano.set_sequence(nullptr);
        bool ok = load_project(perf, hp, &autoPlayer);
        fprintf(stderr, "[hang] load=%d\n", (int)ok); fflush(stderr);
        if (ok) { vArrange.reset_project_state();
                  if (refresh_loaded_project) refresh_loaded_project(); }
        // Maximise: the drawing cost this harness exists to measure is a
        // function of window area, so a default-sized window measures nothing.
        SDL_MaximizeWindow(app.window);
        fprintf(stderr, "[hang] PLAY\n"); fflush(stderr);
        perf.start(false);
        fprintf(stderr, "[hang] rolling -- gui loop now live\n"); fflush(stderr);
    }

    // CPU (DSP-load) meter, drawn on top at the far right of the menu strip.
    app.run([&](App& a){
        if((g_countInPendingPlay || g_countInPendingRecord) &&
           PatchKnob::app::audio_app_metronome_countin_finished()) {
            if(g_countInPendingRecord) {
                g_countInPendingRecord=false;
                // locate() is applied by the audio thread on its next block.
                // Preserve the pre-roll origin rather than sampling the still
                // one-bar-ahead transport during this handoff frame.
                g_countInRecordOriginTick =
                    PatchKnob::app::audio_app_metronome_countin_origin_tick();
                // Transport::locate is deliberately audio-thread deferred.
                // Do not arm on this UI frame: MIDI stamped here would still
                // carry the count-in end tick and appear one bar into the clip.
                g_countInRecordAwaitingLocate=true;
                g_countInPreLocateTick =
                    PatchKnob::app::audio_app_sample_to_tick(
                        PatchKnob::app::audio_app_transport_sample());
                g_countInAwaitMs = SDL_GetTicks();
            } else {
                g_countInPendingPlay=false;
                perf.start(g_songMode);
                PatchKnob::app::audio_app_patch_set_playing(true);
            }
        }
        // Count-in -> record handoff.  This used to demand that the transport
        // tick be EXACTLY the origin tick.  The UI polls at frame rate while the
        // transport moves in audio blocks, and sample->tick rounding need not
        // land on that one value, so the test could miss and recording would not
        // arm until some later frame happened to match -- the "count in, then it
        // starts recording late" bug.  Wait for the locate to have LANDED
        // instead: the transport is stopped during the handoff, so any tick at
        // or below the origin means the jump back has been applied.  A short
        // deadline arms anyway, because failing to record is worse than being a
        // block late (record_arm_at_tick anchors the capture clock to the origin
        // regardless of what the transport currently reads).
        if(g_countInRecordAwaitingLocate) {
            const long long cur = PatchKnob::app::audio_app_sample_to_tick(
                PatchKnob::app::audio_app_transport_sample());
            const bool landed = cur <= g_countInRecordOriginTick ||
                                cur <  g_countInPreLocateTick;
            const bool overdue = SDL_GetTicks() - g_countInAwaitMs > 250u;
            if(landed || overdue) {
                g_countInRecordAwaitingLocate=false;
                g_countInPreLocateTick=-1;
                if(toggle_record) toggle_record();
            }
        }
        // While STOPPED, the ENGINE transport owns the playhead and perform just
        // mirrors it.  perform's scheduler thread sets its own m_tick back to 0
        // as it tears down, so the instant you pressed STOP the arrange playhead
        // teleported to bar 1 -- and every seek that located the engine looked
        // like it had done nothing, because the arrange view draws perform's
        // tick, not the engine's.  One mirror while idle keeps the two in step
        // without giving anything a second way to seek.  Guarded on the engine
        // actually running, because with no audio device the engine transport is
        // not a clock at all and perform's tick is the only one there is.
        if(PatchKnob::app::audio_app_running() &&
           !PatchKnob::app::audio_app_transport_rolling() && !perf.running()) {
            const long t = (long)std::max<long long>(0,
                PatchKnob::app::audio_app_sample_to_tick(
                    PatchKnob::app::audio_app_transport_effective_sample()));
            if(perf.get_tick() != t) { perf.set_tick(t); a.request_redraw(); }
        }
        // CPU / GPU meter.  Same footprint as the old single CPU bar, split into
        // two half-height bars: DSP load on top, this process's GPU load below.
        // GPU is what tells you the UI is the thing eating the machine rather
        // than the audio graph -- they are different problems with different
        // fixes, and one combined number hides which you have.
        const float cpuLoad = PatchKnob::app::audio_app_cpu_load();
        const float gpuLoad = pkgpu::load();          // negative = unavailable
        const int   cpuPct  = std::max(0, std::min(999, (int)(cpuLoad * 100.f + 0.5f)));
        char buf[24];
        if (gpuLoad >= 0.f) {
            const int gpuPct = std::max(0, std::min(999, (int)(gpuLoad * 100.f + 0.5f)));
            std::snprintf(buf, sizeof(buf), "CPU/GPU %d/%d%%", cpuPct, gpuPct);
        } else {
            std::snprintf(buf, sizeof(buf), "CPU %d%%", cpuPct);
        }
        const Theme& t = theme();
        SDL_Rect mb{ a.w - 56, 6, 48, 12 };
        fill_rect(a.ren, mb, t.keybg);
        frame_rect(a.ren, mb, t.dim);
        // Top half = CPU, bottom half = GPU.  The frame is 12 px tall including
        // its 1 px border, so each bar gets 5 px of interior.
        const int barH = (mb.h - 2) / 2;
        auto bar = [&](int yOff, float v, bool over) {
            int fw = (int)(v * (mb.w - 2) + 0.5f);
            if (fw > mb.w - 2) fw = mb.w - 2;
            if (fw <= 0) return;
            SDL_Rect r{ mb.x + 1, mb.y + 1 + yOff, fw, barH };
            fill_rect(a.ren, r, over ? t.note : t.accent);
        };
        bar(0, cpuLoad, cpuLoad >= 1.0f);
        if (gpuLoad >= 0.f) bar(barH, gpuLoad, gpuLoad >= 0.90f);
        int tw = a.font.text_w(buf);
        a.font.draw(a.ren, mb.x - tw - 6, (24 - a.font.ch())/2, buf, t.text);
        // The readout ticks on its own (2 Hz GPU sample, DSP load continuously)
        // with nothing else in the frame changing, so it registers its own
        // damage: text on the left, bars on the right, one padded rect.
        a.add_damage(SDL_Rect{ mb.x - tw - 8, 4, tw + mb.w + 12, mb.h + 4 });
        // Status toast.  projectStatus was written from a dozen places and read
        // from NONE -- every message the app has ever produced ("Sample load
        // failed", "Audio capture could not start", the 120 s capture limit) went
        // nowhere.  Show it briefly when it changes, then get out of the way.
        {
            static std::string shown;
            static Uint32 shownAt = 0;
            if (projectStatus != shown) { shown = projectStatus; shownAt = SDL_GetTicks(); }
            const Uint32 age = SDL_GetTicks() - shownAt;
            if (!shown.empty() && shownAt != 0 && age < 4000u) {
                const int lh = a.mono.ch() + 6;
                SDL_Rect db{ 8, a.h - lh - 8, a.mono.text_w(shown) + 12, lh };
                fill_rect (a.ren, db, t.keybg);
                frame_rect(a.ren, db, t.accent);
                a.mono.draw(a.ren, db.x + 6, db.y + 3, shown, t.hi);
                // NO request_redraw() here.  This asked for a full-window repaint
                // on EVERY frame for the sole purpose of noticing its own 4 s
                // expiry -- and a dirty frame is treated as user-requested, so it
                // bypassed the playback frame cap and pinned the UI at the vsync
                // rate for as long as a toast was on screen.  Worse, anything
                // that keeps rewriting projectStatus (the plugin scan does) kept
                // resetting the timer, so it never expired and never stopped.
                // The idle path already forces a repaint ~4x a second, which
                // retires the toast within 250 ms of its deadline -- invisible
                // for a fading status message, and it costs nothing.
            }
        }
        // Capture last: CPU/GPU meters and status overlays above must already
        // be rendered. On Windows the recorder reads the composited HWND so
        // native VST child windows are included too.
        if (screenRec.recording()) {
            screenRec.capture_frame(a.ren);
            a.request_redraw();
        }
    });
    // --- teardown ----------------------------------------------------------
    // Retire the background workers BEFORE anything they touch goes away.
    // 1) Stop handing them the App: from here a late request_redraw() is a
    //    no-op instead of a write into a destroyed object.
    g_quitting.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(uiLink->m); uiLink->app = nullptr; }
    // 2) The bounce worker drives `perf` directly, so it must actually be gone
    //    before main() returns.  It polls g_quitting, so this costs ~10 ms.
    if (g_renderThread.joinable()) g_renderThread.join();
    // 3) The scanner only touches the plugin host and (guarded) the App, but it
    //    must be gone before audio_app_shutdown()'s `delete g_host`.  Cancel it
    //    first: scan() then stops taking new probes, so the wait is bounded by
    //    the ONE probe already in flight (5 s cap, set where the thread starts)
    //    rather than by the whole plugin directory.  Detaching remains the last
    //    resort for a probe that outlives even its own timeout.
    if (g_scanThread.joinable()) {
        if (auto* host = PatchKnob::app::audio_app_host()) host->cancelScan();
        for (int i = 0; i < 800 && g_scanBusy.load(std::memory_order_acquire); ++i)
            SDL_Delay(10);
        if (g_scanBusy.load(std::memory_order_acquire)) g_scanThread.detach();
        else                                            g_scanThread.join();
    }
    if (g_guiHandle) PatchKnob::hostwin::editor_close(g_guiHandle);
    PatchKnob::app::audio_app_shutdown();
    arrange::shutdown_cursors();
    app.shutdown();
    return 0;
}

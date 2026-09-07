//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_view.h
//
//  ArrangeView -- the PatchKnob ARRANGEMENT (song) editor ported to the SDL2
//  grayscale/green toolkit (sdlui/gui.h).  One self-contained ui::Widget that
//  draws, over a live `perform*`:
//
//    * a LEFT track-header column (one row per ACTIVE sequence): status spine,
//      INS/AUD type badge, track name + bus/ch/time-sig, a level/VU strip and
//      Mute / Solo toggle buttons                       (ports src/perfnames.cpp)
//    * a TOP time ruler with bar numbers + L / R loop markers
//                                                        (ports src/perftime.cpp)
//    * the RIGHT arrangement canvas: alternating-stripe track lanes, a bar/beat
//      grid, rounded themed clip blocks (trigger regions) carrying the pattern
//      name + a tiny note preview, and a moving playhead
//                                                        (ports src/perfroll.cpp)
//
//  Interactions mirror perfroll: left-drag on empty lane places+grows a clip
//  trigger (snapped to the pattern length); left-drag on a clip moves it, or
//  resizes it when grabbed near an edge; right-click deletes; middle-click
//  splits.  The ruler sets the L/R loop ticks.  The header M/S buttons toggle
//  song-mute / solo.  Wheel = horizontal ZOOM (vertical wheel), horizontal
//  wheel / arrow keys pan.
//
//  Strictly two-tone: every colour comes from ui::theme() so it flips with the
//  Light / Midnight mode at runtime.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_ARRANGE_VIEW_H
#define PATCHKNOB_SDLUI_ARRANGE_VIEW_H

#include "gui.h"
#include <vector>
#include <map>
#include <set>
#include <string>
#include <functional>
#include <ctime>

class perform;   // PatchKnob engine core
namespace PatchKnob { namespace engine { struct AudioClip; } }

namespace arrange {
void shutdown_cursors();

class ArrangeView : public ui::Widget {
public:
    explicit ArrangeView(perform* p);

    // ui::Widget -------------------------------------------------------------
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;
    bool on_key_up(ui::App& app, SDL_Keycode k) override;   // F-key chords
    //! Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y: the app offers the focused view first
    //! refusal.  Consumes the key while THIS view's Multiple-Undo queue has a
    //! matching entry; otherwise falls through to the shell's project-wide
    //! snapshot undo (so patch/mixer edits still undo as before).
    bool on_undo(ui::App& app, bool redo) override;
    void cancel_interaction(ui::App& app) override;

    // Timeline seek (absolute MIDI tick).  The shell moves the transport, which
    // is what perform::get_tick() then reports back -- so this is also how the
    // view moves the playhead while STOPPED.  (A second, never-assigned
    // `playhead_tick` override used to live here; nothing ever set it.)
    std::function<void(long tick)> on_seek;

    // Fired to open a clip's editor. kind: 0=piano, 1=tracker, 2=automation.
    // shell rebinds its piano/tracker editor to `seq` and shows it.
    std::function<void(int seq, int kind)> on_open_editor;
    // Fired when an AUDIO clip is double-clicked: open it in the sample/warp
    // editor (bottom dock) instead of a MIDI editor.
    std::function<void(int seq)> on_open_sample_editor;
    //! "CDP..." on a clip: open the offline CDP editor with this clip as a
    //! source.  A MIDI clip has no audio, so the shell freezes it first.
    std::function<void(int seq)> on_open_cdp;

    // ---- track-header instrument selector ----------------------------------
    // Each instrument (MIDI) track drives one instrument node; the header shows a
    // dropdown (line 2) to pick/assign it.  The shell supplies the instrument
    // list + the current name per track and applies the pick (sets the
    // sequence's MIDI channel to that instrument + wires it).  Audio tracks have
    // no instrument node, so the dropdown is hidden for them.
    std::function<std::vector<std::string>()> on_list_instruments;
    std::function<std::string(int seq)>       on_track_instrument;
    std::function<void(int seq, int idx)>     on_pick_instrument;
    // Live post-mixer level for this lane, normalized to 0..1.
    std::function<float(int seq, int channel)> on_track_level;
    // ---- automation clips ---------------------------------------------------
    // An automation clip block draws the region's CURVE, its loop repeats and a
    // live playhead dot.  The view stays independent of the automation engine:
    // the shell answers these two questions about a region instead.
    //
    // AutoClipInfo is the region's geometry.  `position` / `length` are PROJECT
    // ticks; `source` and `loopLength` are CURVE-LOCAL, exactly as
    // engine::AutomationPlayer::Region defines them -- the curve tick for a
    // project tick T is  (source + (T - position)) % loopLength, so a region
    // longer than its loop REPEATS and a trimmed / slipped one starts part-way
    // into the curve.
    struct AutoClipInfo {
        bool valid      = false;
        long position   = 0;
        long length     = 0;
        long loopLength = 1;
        long source     = 0;
        bool muted      = false;
        int  lanes      = 0;   //!< lanes carrying at least one breakpoint
        int  focus      = 0;   //!< the lane drawn brightest (0 = first)
    };
    std::function<AutoClipInfo(int seq)> on_automation_info;
    //! Sample `lane` of `seq`'s curve at `count` evenly spaced curve ticks over
    //! [t0,t1], each WRAPPED modulo the region's loop length, appending the
    //! normalized 0..1 values to `out` (never cleared, so one buffer serves a
    //! whole frame).  count == 1 samples t0 alone.  This is the same evaluation
    //! the player performs, so the line drawn is the automation played.
    std::function<void(int seq, int lane, long t0, long t1, int count,
                       std::vector<float>& out)> on_automation_sample;
    std::function<bool(int seq)>              is_track_record_armed;
    std::function<void(int seq)>              on_track_record_arm;
    std::function<std::string(int seq)>       on_track_record_input;
    std::function<void(int seq)>              on_cycle_track_record_input;
    std::function<std::string(int seq,int tab)> on_track_io_label;
    std::function<void(int seq,int tab)>        on_cycle_track_io;
    std::function<std::vector<std::string>(int seq,int route)> on_list_track_io;
    std::function<void(int seq,int route,int idx)> on_pick_track_io;
    std::function<int(int seq,int route)>           on_track_io_index;

    // Fired by the ADD-TRACK "+" affordance (bottom of the header column) and by
    // each track's "x" remove button, so the shell can create / destroy the
    // underlying sequence.  kind: 0 = instrument (MIDI) track, 1 = audio track.
    // `seq` is an ABSOLUTE sequence index.
    std::function<void(int kind)> on_add_track;      // 0=instrument, 1=audio, 2=automation
    std::function<void(int seq)>  on_remove_track;   // absolute sequence index
    // Creates an independent pattern sequence routed like `source_seq`, places a
    // trigger at start/length/offset, and returns the new absolute sequence id.
    // copy_events=false creates an empty pattern; true clones the source notes.
    std::function<int(int source_seq, long start, long length, long offset,
                      bool copy_events)> on_create_pattern;
    // Ardour-style track identity: several independent pattern sequences may
    // live on the same visible lane/playlist when they share this key.
    std::function<int(int seq)> on_track_key;

    //! Mark a track (sequence index) as an AUDIO track carrying `clip`; its clip
    //! blocks then render the audio WAVEFORM instead of the MIDI note preview.
    //! `fullTicks` is the clip's NATURAL length in ticks (the untrimmed trigger
    //! span) so a resized/trimmed clip shows only the visible slice of audio;
    //! pass 0 to fall back to the trigger's own length.  Pass nullptr to clear.
    void set_audio_clip(int seq, const PatchKnob::engine::AudioClip* clip, long fullTicks = 0);
    const PatchKnob::engine::AudioClip* audio_clip(int seq) const;
    //! `loopTicks` is the loop PERIOD of a looping region, in ticks; 0 means
    //! "the rest of the source from `source` on", which is what a region
    //! saved before the trimmed-loop period existed restores to.
    void set_audio_region(int seq, long position, long length, long source,
                          float gain = 1.0f, bool muted = false, bool loop = false,
                          long loopTicks = 0);
    //! Update just the loop period of an already-known region.  Toggling LOOP
    //! makes the player capture the region's current trimmed span as its
    //! period, so the view has to be told what that turned out to be or it
    //! falls back to repeating the whole source.
    void set_audio_region_loop_length(int seq, long loopTicks);
    struct RecordPreviewNote { long start=0,end=0; int pitch=60; };
    void set_record_preview(int seq,long timelineStart,long length,
                            const std::vector<RecordPreviewNote>& notes,
                            const PatchKnob::engine::AudioClip* audio);
    void clear_record_preview();

    //! Drop EVERY per-sequence entry for `seq` (audio ptr, source length, region,
    //! fade, colour, frozen flag, and the lane height if this was the lane's last
    //! sequence).  Call this whenever a sequence is deleted / its track removed so
    //! a later reuse of the recycled index cannot inherit stale (or freed) state.
    //! MUST run while the shell's lane routing for `seq` is still intact.
    void forget_seq(int seq);
    void reset_project_state();

    //! Move a lane's custom height from one lane-key to another (the shell calls
    //! this when it renumbers tracks on delete, so heights follow their lane
    //! instead of a lane adopting a neighbour's height).
    void move_lane_height(int fromKey, int toKey);

    //! MIDI<->audio alignment: rewrite `sourceSeq`'s notes so they follow how the
    //! frozen audio on `audioLaneSeq` was cut/moved (each region maps its source
    //! tick window to its timeline position).  Only rearranges when the audio was
    //! actually edited; a plain unfreeze leaves the source MIDI untouched.  For
    //! TRACK freezes (audio on its own lane); the shell calls this before it tears
    //! the frozen lane down on Unfreeze.
    void unfreeze_rebuild_midi(int audioLaneSeq, int sourceSeq);

    // ---- freeze (render a clip/track to audio, play that instead) -----------
    // The shell binds these; the view fires them from the right-click menu.
    // on_freeze_clip renders the clip's trigger span [startTick,endTick]; the
    // shell attaches the audio, song-mutes the source, and calls set_frozen +
    // set_audio_clip.  on_freeze_track freezes every clip on the lane.
    std::function<void(int seq, long startTick, long endTick)> on_freeze_clip;
    std::function<void(int seq)> on_freeze_track;
    std::function<void(int seq)> on_unfreeze;
    //! DELETE an audio clip (distinct from unfreeze): the shell disk-caches the
    //! rendered audio + region for undo, then tears the clip down.
    std::function<void(int seq)> on_clip_delete;
    std::function<void(int seq)> on_midi_clip_delete;
    //! Restore the most recent disk-cached audio-clip delete (re-attach audio +
    //! region + lane) and return the DISPLAY seq it came back on (-1 = nothing
    //! to restore).  Fired by the Multiple-Undo queue's audio-delete entries,
    //! never directly from Ctrl+Z any more.  (Renamed from `on_undo`: that
    //! name now belongs to the Widget VIRTUAL below, which is how the app
    //! routes Ctrl+Z / Ctrl+Shift+Z to the focused view.)
    std::function<int()> on_restore_deleted_clip;

    //------------------------------------------------------------------------
    //  Pro Tools ch.28 -- MULTIPLE UNDO (p664-666)
    //
    //  ONE queue of undoable operations (default 32 levels, settable 1..64)
    //  covering every arrange edit: the trigger-snapshot edits (which ride
    //  perform's own snapshot stack in lockstep), the disk-cached audio-clip
    //  deletes (whose closures fire on_undo / on_clip_delete), and any op the
    //  shell queues itself (record takes, punch passes).  The Undo History
    //  window lists it; Ctrl+Z / Ctrl+Shift+Z walk it.
    //------------------------------------------------------------------------
    struct UndoOp {
        std::string name;
        std::time_t when = 0;
        //! undo closures run NEWEST-first, redo closures oldest-first, so a
        //! compound op (snapshot + audio teardowns) unwinds symmetrically.
        std::vector<std::function<void()>> undo, redo;
    };
    //! Snapshot-op: push perform's trigger snapshot AND the matching history
    //! entry.  This is what every view edit calls instead of the old bare
    //! m_perf->push_trigger_undo().
    void push_undo(const char* name);
    //! The SHELL already pushed perform's trigger snapshot itself (record /
    //! punch paths): queue only the matching history entry.
    void note_engine_undo(const char* name);
    //! Queue an op with custom closures (audio-clip delete, shell compounds).
    void push_undo_op(UndoOp op);
    //! Group several pushes into ONE history entry (Delete = trigger snapshot
    //! + N audio-clip teardowns).  Re-entrant; inner groups just append.
    void begin_undo_compound(const char* name);
    void end_undo_compound();
    void do_undo();          //!< Ctrl+Z
    void do_redo();          //!< Ctrl+Shift+Z / Ctrl+Y
    void undo_all();
    void redo_all();
    //! Deleting a track invalidates queued closures (they may name dead seqs)
    //! -- the manual clears the queue on exactly that operation.
    void clear_undo_queue();
    int  undo_levels() const { return m_undo_levels; }
    void set_undo_levels(int n);
    //! Delete an audio clip THROUGH the undo queue: fires on_clip_delete +
    //! forget_seq and queues the restore/redo closures as one entry (or as
    //! part of the open compound).
    void delete_audio_clip_undoable(int seq);
    //! Shell reflects freeze state back so the menu label + lane tint track it.
    void set_frozen(int seq, bool frozen);
    bool is_frozen(int seq) const;

    // ---- clip fades + crossfades (audio/frozen clips) -- Pro Tools ch.32 ---
    // Per-clip fade-in/out lengths (ticks), curve tensions (-1..1), SHAPES
    // (0 Standard, 1 S-Curve, 2..8 the seven parabolic presets -- the same
    // encoding as engine ScheduledClip::kFadeShape*), SLOPES (0 Equal Gain,
    // 1 Equal Power) and -- on the fade-in side of a crossfade -- the LINK
    // (0 Equal Power, 1 Equal Gain, 2 None).  Drag the top-corner handle to
    // set a fade length; drag the mid-fade point to bend a Standard/S-Curve.
    // A CROSSFADE is two lane-mates whose regions OVERLAP, the left carrying a
    // fade-out and the right a fade-in over the overlap window -- the engine
    // sums the two enveloped regions, so the crossfade is heard, not just
    // drawn.  on_clip_fade fires the whole record to the shell, which converts
    // ticks to frames and updates the engine region.
    struct ClipFade { long inTicks = 0, outTicks = 0; float inK = 0.f, outK = 0.f;
                      int inShape = 0, outShape = 0;   // kFadeShape* encoding
                      int inSlope = 0, outSlope = 0;   // 0 EqGain, 1 EqPower
                      int link = 0; };                 // 0 EqPow, 1 EqGain, 2 None
    std::function<void(int seq, const ClipFade& f)> on_clip_fade;
    void set_clip_fade(int seq, const ClipFade& f);
    //! Legacy 5-argument form (shapes/slopes untouched -> Standard/Equal Gain).
    void set_clip_fade(int seq, long inTicks, long outTicks, float inK, float outK);

    //! Default fade settings (the Editing-preferences "base" settings): shapes,
    //! slopes and tensions used by the Smart tool, the default-fade command
    //! (Ctrl+Win+F), Fade To Start/End and Batch Fades.
    struct FadeSettings { int inShape = 0, outShape = 0;
                          int inSlope = 0, outSlope = 0;
                          int link = 0;
                          float inK = 0.f, outK = 0.f; };

    //! Audition hooks for the Fades dialog: play [startTick,endTick) through
    //! the real signal path (the shell seeks + starts the transport), and stop
    //! it again.  Safe unbound (the dialog's Audition button flashes a note).
    std::function<void(long startTick, long endTick)> on_audition_start;
    std::function<void()> on_audition_stop;
    //! AutoFades preference changed (0..10 ms; 0 = off) -- the shell pushes it
    //! into every engine audio clip player.  Safe unbound.
    std::function<void(int ms)> on_auto_fade_changed;
    //! Arrange seq whose AUDIO an automation clip `seq` targets (-1 = none);
    //! lets automation lanes draw that clip's fade boundaries + shapes (p761).
    std::function<int(int seq)> on_automation_dest_seq;

    // Fired when an AUDIO clip's REGION changes (move / trim / slip), carrying
    // the trigger's (startTick = timeline position, lengthTick = region span,
    // offsetTick = start-offset into the source).  The shell converts to samples
    // and updates the engine region so playback follows non-destructively.
    std::function<void(int seq, long startTick, long lengthTick, long offsetTick)> on_clip_region_changed;

    // Fired when an AUDIO clip is DUPLICATED (Ctrl+drag copy / paste): the shell
    // attaches the SAME source audio to the new lane `newSeq` as a new region at
    // (startTick, source-offset srcOffTick, length lenTick), so the copy plays.
    std::function<void(int srcSeq, int newSeq, long startTick, long srcOffTick, long lenTick)> on_clip_duplicated;

    // ---- Ardour region operations (audio clips) ----------------------------
    std::function<void(int seq, float gain)> on_clip_gain;       // set region gain
    std::function<void(int seq)>             on_clip_normalize;  // normalize to 0 dBFS
    std::function<void(int seq)>             on_clip_reverse;    // reverse region audio
    std::function<void(int seq, bool muted)> on_clip_mute;       // mute region
    std::function<void(int seq, bool loop)>  on_clip_loop;       // loop the source to fill

    // ---- Pro Tools ch.29/30 shell hooks (all safe unbound) -----------------
    //! TCE Trim: time-compress/expand `seq`'s region audio to `newLenTicks`
    //! (the shell renders through engine warp_to_length and re-attaches the
    //! clip + region).  Unbound -> the TCE gesture reports "needs shell
    //! wiring" in the drag readout and leaves the region untouched.
    std::function<void(int seq, long newLenTicks)> on_clip_tce;
    //! Loop Trim, bottom half: publish a new loop PERIOD (source-iteration
    //! length) for a looping region.  Unbound -> only the view's own
    //! m_region.loopLength updates (drawing follows; engine keeps its period).
    std::function<void(int seq, long loopTicks)> on_clip_loop_length;
    //! Scrubber / Scrub-Trim audition: the shell may route a short audition of
    //! `seq`'s track at `tick` through the mixer path.  Unbound -> the view
    //! falls back to on_seek (playhead follows, no audio while stopped).
    std::function<void(int seq, long tick)> on_scrub_audition;

    // ---- Pro Tools ch.31 shell hooks (all safe unbound) --------------------
    //! Split Into Mono (p736): the shell splits `seq`'s stereo audio into two
    //! new mono lanes named .L/.R, keeping output assignments.
    std::function<void(int seq)> on_clip_split_mono;
    //! Consolidate (p737): render `seq`'s lane over [startTick,endTick) --
    //! silence between clips included, muted clips as silence -- into ONE new
    //! whole clip replacing the range.
    std::function<void(int seq, long startTick, long endTick)> on_consolidate;
    //! Consolidate a CLIP selection (p705: select clips with the Grabber, then
    //! Edit > Consolidate): render exactly `seqs` -- in timeline order, gaps
    //! between them as silence, each piece's gain and fades baked in -- into
    //! ONE new clip spanning [startTick,endTick), replacing the selection.
    //! Unselected material inside the span is neither rendered nor removed.
    std::function<void(const std::vector<int>& seqs, long startTick,
                       long endTick)> on_consolidate_clips;
    //! Compact (p737-738): DESTRUCTIVELY drop the unused parts of `seq`'s
    //! source audio, padding the kept window by `padMs`.  Cannot be undone;
    //! the shell saves the session afterwards.
    std::function<void(int seq, long padMs)> on_clip_compact;

    //! Last tick the arrangement occupies, over BOTH models: seq24 triggers and
    //! the Ardour audio regions.  PUBLIC because the shell's transport needs it:
    //! the TO-END button called perform::get_max_trigger() directly, which knows
    //! nothing about audio regions and answers 0 for an audio-only project -- so
    //! ">|" jumped to bar 1 and looked broken on exactly the projects where you
    //! most want it.
    long song_end() const;

    // ---- test introspection (arrange_view_test --selftest) -----------------
    // Read-only peepholes so the standalone harness can assert on behaviour
    // without widening any real API.
    int  dbg_edit_mode() const { return (int)m_edit_mode; }
    int  dbg_tool()      const { return (int)m_edit_tool; }
    bool dbg_shuffle_lock() const { return m_shuffle_lock; }
    bool dbg_snap_to_grid() const { return m_snap_to_grid; }
    long dbg_sel_start() const { return m_sel_start; }
    long dbg_sel_end()   const { return m_sel_end; }
    bool dbg_link_timeline() const { return m_link_timeline; }
    double dbg_scale()   const { return m_scale_x; }
    bool dbg_universe()  const { return m_universe_on; }
    // ch.28/31 peepholes
    int  dbg_undo_len()  const { return (int)m_undo_q.size(); }
    //! Lane rows the view would draw, and their per-lane sequence buckets.
    //! Exposed so the selftest can prove the SPARSE lane-key space (the host
    //! returns 100000 + trackIndex / 200000 + laneId, not a sequence index)
    //! survives dedup and bucketing.
    std::vector<int> dbg_active_list() const { return active_list(); }
    std::vector<std::vector<int>> dbg_lane_groups() const { return lane_groups(active_list()); }
    int  dbg_undo_done() const { return (int)m_undo_done; }
    int  dbg_undo_levels() const { return m_undo_levels; }
    bool dbg_undo_open() const { return m_undo_open; }
    int  dbg_clipboard() const { return (int)m_clip_clipboard.size(); }
    int  dbg_rating(int seq) const {
        std::map<int,int>::const_iterator it = m_rating.find(seq);
        return it == m_rating.end() ? 0 : it->second;
    }
    bool dbg_layered()   const { return m_layered; }
    bool dbg_wf_power()  const { return m_wf_power; }
    bool dbg_wf_rect()   const { return m_wf_rect; }
    long dbg_nudge_ticks() const { return nudge_ticks(); }
    // ch.32 peepholes
    //! A clip's fade settings (shell accessor: consolidate bakes these into
    //! the render).  Returns a default ClipFade when the seq carries none.
    ClipFade clip_fade(int seq) const {
        std::map<int, ClipFade>::const_iterator it = m_clipFade.find(seq);
        return it != m_clipFade.end() ? it->second : ClipFade{};
    }
    //! Selftest: drive a split through the exact code path the Cut tool uses.
    void dbg_split_at(int laneSeq, long tick) { split_clip_at(laneSeq, tick); }
    //! Selftest: a clip's on-screen rect (for synthesising clicks on it).
    bool dbg_clip_rect(int seq, SDL_Rect& r) const { return clip_rect_of(seq, r); }
    int  dbg_selected_clips() const { return selected_clip_count(); }
    bool dbg_clip_selected(int seq) const {
        std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
        return it != m_region.end() && it->second.selected;
    }
    ClipFade dbg_fade(int seq) const {
        std::map<int,ClipFade>::const_iterator it = m_clipFade.find(seq);
        return it == m_clipFade.end() ? ClipFade{} : it->second;
    }
    bool dbg_xfade_pair(int seq, int& l, int& r) const { return xfade_pair(seq, l, r); }
    long dbg_region_pos(int seq) const {
        std::map<int,AudioRegion>::const_iterator it = m_region.find(seq);
        return it == m_region.end() ? -1 : it->second.position;
    }
    long dbg_region_len(int seq) const {
        std::map<int,AudioRegion>::const_iterator it = m_region.find(seq);
        return it == m_region.end() ? -1 : it->second.length;
    }
    bool dbg_fdlg_open() const { return m_fdlg_open; }
    int  dbg_fdlg_kind() const { return m_fdlg_kind; }
    bool dbg_bdlg_open() const { return m_bdlg_open; }
    bool dbg_create_crossfade(int l, int r, long a, long b) {
        return create_crossfade(l, r, a, b, m_def_xfade, true);
    }
    void dbg_remove_crossfade(int l, int r) { remove_crossfade(l, r); }
    void dbg_select(long a, long b, int lo, int hi) { set_edit_selection(a, b, lo, hi); }
    void dbg_delete_fades() { delete_fades_selection(); }
    int  dbg_fade_sel_which() const { return m_fade_sel_which; }
    int  dbg_fade_sel_seq()   const { return m_fade_sel_seq; }
    //! Bench/test hook: place the viewport exactly (scroll tick + ticks/px).
    void dbg_set_view(long scroll_ticks, double scale) {
        if (scale > 0.0) m_scale_x = scale;
        m_scroll_ticks = scroll_ticks < 0 ? 0 : scroll_ticks;
    }
    //! Bench/test hooks: view toggles + the transient detector, so the
    //! headless harness can measure them without driving menus.
    void dbg_set_wf_overlap(bool on) { m_wf_overlap = on; }
    // Clip copy/paste harness hooks.  Paste retargets onto the FOCUSED lane,
    // which is otherwise only reachable through a header/clip click, and the
    // clipboard is filled from whatever is selected ANYWHERE in the project --
    // so a test has to be able to set the one and clear the other explicitly.
    int  dbg_lane_key(int seq) const { return lane_key(seq); }
    int  dbg_focus_lane() const { return m_focus_lane; }
    void dbg_set_focus_lane(int laneKey) { m_focus_lane = laneKey; }
    void dbg_unselect_all_clips() { unselect_all_triggers(); }
    long dbg_clip_span() const { return m_clip_span; }
    void dbg_set_universe(bool on)   { m_universe_on = on; }
    long dbg_next_transient(long fromTick, bool backward) const {
        return next_transient(fromTick, backward);
    }

    // Layout knobs (pixels).  The shell just sizes `rect`; these partition it.
    int header_w = 6 * 30;   // left track-header column width  (== c_names_x)
    int ruler_h  = 42;       // Ardour-style tool strip + time ruler
    int scrollbar_h = 18;    // permanently reserved bottom song navigator
    int scrollbar_w = 18;    // permanently reserved right track navigator
    int row_h    = 80;       // per-track lane / header row height (default 2x)

private:
    perform* m_perf;

    // Zoom limits.  These used to differ per entry point (wheel and buttons
    // clamped to 512, zoom_to_fit to 4096), so FIT could land on a zoom the
    // wheel was unable to leave.
    static constexpr double kZoomMin = 0.005;
    static constexpr double kZoomMax = 4096.0;

    // horizontal mapping: ticks-per-pixel (zoom) + left-edge scroll (ticks)
    double m_scale_x;        // ticks per pixel  (default c_perf_scale_x == 32)
    long   m_scroll_ticks;   // tick at the canvas left edge
    int    m_v_offset;       // index (into the active-track list) of top row

    // grid / snap (ticks)
    long   m_snap;
    long   m_measure_len;
    long   m_beat_len;

    // interaction state (mirrors perfroll drag machine)
    bool   m_mouse_down = false;
    bool   m_drag_left  = false;   // grabbing the L loop marker on the ruler
    bool   m_drag_right = false;   // grabbing the R loop marker on the ruler
    bool   m_scroll_drag = false;
    int    m_scroll_drag_x0 = 0;
    long   m_scroll_drag_tick0 = 0;
    SDL_Rect m_scrollbar_rect {0,0,0,0};
    SDL_Rect m_scroll_thumb {0,0,0,0};
    bool   m_vscroll_drag = false;
    int    m_vscroll_drag_y0 = 0;
    int    m_vscroll_drag_offset0 = 0;
    SDL_Rect m_vscrollbar_rect {0,0,0,0};
    SDL_Rect m_vscroll_thumb {0,0,0,0};
    // The first four are the original PatchKnob tools; the Pro Tools set maps
    // onto and extends them: Grab == the Grabber, Range == the Selector,
    // Draw == the Pencil, Cut is PatchKnob's own scissors.  Zoom / Trim /
    // Scrub / Smart are new (ch.29).  Keep the first four values stable: the
    // context-menu tool strip and (EditTool)int casts index them directly.
    enum class EditTool { Grab, Range, Cut, Draw, Zoom, Trim, Scrub, Smart };
    static const int kToolCount = 8;
    EditTool m_edit_tool = EditTool::Grab;
    SDL_Rect m_tool_rect[8]{};
    // Tool picker as a right-click popup: the same four icons, with names and
    // their shortcut keys, reachable without aiming at the toolbar strip.
    bool     m_toolmenu_open = false;
    SDL_Rect m_toolmenu_rect { 0, 0, 0, 0 };
    void     draw_toolmenu (ui::App& app);
    bool     toolmenu_click(ui::App& app, int mx, int my);
    // Vector tool glyph (0 = hand/grab, 1 = range, 2 = scissors, 3 = pencil).
    void     draw_tool_icon(ui::App& app, SDL_Rect box, int tool, ui::Color c);
    const char* tool_name (int tool) const;
    const char* tool_key  (int tool) const;
    // Zoom-out / zoom-in / fit buttons on the ruler tool strip.
    SDL_Rect m_zoom_rect[3]{};
    SDL_Rect m_help_rect { 0, 0, 0, 0 };
    bool     m_help_open = false;      // keyboard-shortcut overlay ("?" / F1)
    void     draw_help(ui::App& app);
    // One-frame tooltip: hover tests set it during draw, draw() paints it last.
    std::string m_tip;
    int         m_tip_x = 0, m_tip_y = 0;
    void        tip(const std::string& s, int x, int y);
    void        draw_tooltip(ui::App& app);
    bool m_range_drag=false;
    long m_range_anchor=0;
    bool m_scrubbing=false;            // dragging the ruler seeks continuously
    bool   m_moving     = false;
    bool   m_growing    = false;
    int    m_move_seq   = -1;      // sequence whose audio clip is being dragged
    // SLIP (Ardour set_start): Alt+drag an audio clip body slides the source
    // content under the fixed timeline window.  Refs captured at press.
    bool   m_slipping   = false;
    long   m_slip_ref_tick   = 0;  // pointer tick at press
    long   m_slip_ref_source = 0;  // region source-offset at press
    // Gain line (Ardour): drag the region's horizontal gain line up/down.
    bool   m_gain_grab  = false;
    int    m_gain_seq   = -1;
    int    gain_line_y(int seq, int clipTop, int clipH) const;   // y of the gain line

    // right-click context menu (add/open/delete a clip)
    bool   m_menu_open  = false;
    int    m_menu_x = 0, m_menu_y = 0;
    int    m_menu_seq = -1;
    long   m_menu_tick = 0;
    bool   m_menu_on_clip = false;   // opened over an existing clip
    // double-click detection
    int    m_last_click_seq = -1;
    long   m_last_click_tick = 0;
    unsigned m_last_click_ms = 0;
    void   draw_menu(ui::App& app);
    bool   menu_click(ui::App& app, int mx, int my);
    //! Clamped context-menu geometry -- draw and hit-test MUST agree on it.
    SDL_Rect menu_box(ui::App& app, int& rowh, int& n) const;
    //! True while any popup (context / add / instrument / IO / tool / snap) is
    //! up.  Wheel + keys are suppressed then, so the view underneath cannot be
    //! zoomed, panned or edited out from under an open menu.
    bool   any_menu_open() const;
    //! Close every popup (Esc, project reset, a track disappearing under one).
    void   close_all_menus();
    const char* menu_label(int idx) const;   // dynamic Freeze/Unfreeze labels
    bool   menu_is_audio() const;            // clip menu is on an audio region

    // add-track chooser popup + the "+" / per-track "x" affordances -----------
    bool     m_addmenu_open = false;
    SDL_Rect m_addmenu_rect { 0, 0, 0, 0 };
    void     draw_addmenu  (ui::App& app);
    bool     addmenu_click (ui::App& app, int mx, int my);

    // track-header instrument dropdown popup ---------------------------------
    bool     m_instrmenu_open = false;
    SDL_Rect m_instrmenu_rect { 0, 0, 0, 0 };
    int      m_instrmenu_seq  = -1;
    int      m_instrmenu_scroll = 0;
    std::vector<std::string> m_instrmenu_items;
    SDL_Rect instr_box_rect (int row_y, int ch) const;   // dropdown box on line 2
    void     draw_instrmenu (ui::App& app);
    bool     instrmenu_click(ui::App& app, int mx, int my);
    bool     m_iomenu_open = false;
    SDL_Rect m_iomenu_rect { 0, 0, 0, 0 };
    int      m_iomenu_seq = -1, m_iomenu_route = -1;
    int      m_iomenu_scroll = 0;
    std::vector<std::string> m_iomenu_items;
    void     draw_iomenu(ui::App& app);
    bool     iomenu_click(ui::App& app, int mx, int my);
    // The instrument and I/O pickers were two copies of the same scrolling list
    // popup; both now draw and hit-test through these.
    void     draw_list_popup(ui::App& app, SDL_Rect& box,
                             const std::vector<std::string>& items,
                             int& scroll, int selected);
    bool     list_popup_pick(ui::App& app, const SDL_Rect& box,
                             const std::vector<std::string>& items,
                             int scroll, int mx, int my, int& outIdx) const;
    SDL_Rect add_row_rect  () const;                  // bottom "+" affordance row
    void     draw_remove_btn(ui::App& app, SDL_Rect box, bool hot);  // per-track "x"
    // live pointer (logical coords) for hover highlight -- plain motion events
    // are not delivered without a button held, so draw() polls the mouse too.
    int      m_mx = -1, m_my = -1;

    // audio tracks: seq index -> clip whose waveform draws in the lane
    std::map<int, const PatchKnob::engine::AudioClip*> m_audio;
    // seq index -> the clip's natural (untrimmed) length in ticks (the SOURCE
    // length), used to clamp the region + map the waveform window.
    std::map<int, long> m_audioLen;
    int m_recPreviewSeq=-1;
    long m_recPreviewStart=0,m_recPreviewLength=0;
    std::vector<RecordPreviewNote> m_recPreviewNotes;
    const PatchKnob::engine::AudioClip* m_recPreviewAudio=nullptr;

    // ---- Ardour AudioRegion (per audio seq, in ticks) ----------------------
    // A region is a non-destructive view onto its source clip:
    //   position = timeline placement,
    //   source   = start-offset INTO the source clip,
    //   length   = region span. Bounds are always half-open [position,end).
    // AudioRegion is authoritative; legacy Seq24 triggers are import metadata
    // only and are never consulted by audio drawing, hit-testing or editing.
    // The Ardour content laws (Region::set_position / trim_front / trim_end /
    // set_start) are applied to this on a gesture release.
    struct AudioRegion { long position = 0, source = 0, length = 0;
                         long loopLength = 0;    // 0 == to the end of the source
                         float gain = 1.0f; bool muted = false; bool loop = false;
                         bool selected = false; };
    std::map<int, AudioRegion> m_region;
    AudioRegion& region_for(int seq);              // lazy-init from the trigger
    void commit_region(int seq);        // clamp + publish the region
    // Ardour Playlist::_split_region: at splitTick, LEFT keeps {P,S,B}; RIGHT is a
    // new lane-sharing sequence with {P+B, S+B, L-B}, B = splitTick - P.
    void split_audio_clip(int seq, long splitTick);
    void split_clip_at(int laneSeq, long timelineTick);
    std::set<int> m_frozen;   // sequence indices currently frozen (menu + tint)
    std::map<int, ClipFade> m_clipFade;   // seq -> fade lengths/tensions

    // fade drag state (which handle on which clip is being dragged)
    enum class FadeGrab { None, InLen, OutLen, InCurve, OutCurve };
    FadeGrab m_fade_grab = FadeGrab::None;
    int      m_fade_seq  = -1;
    // helpers: locate a clip's on-screen rect + fade hit-testing
    bool clip_rect_of(int seq, SDL_Rect& out) const;
    //! On-screen body rect of ONE clip of `seq`, given its own tick span.
    //! clip_rect_of() can only ever answer for the FIRST span of a sequence,
    //! which is wrong the moment a lane holds several clips of the same
    //! sequence (a MIDI split makes exactly that).  Corner affordances have to
    //! hit-test the clip actually under the pointer, so they use this.
    bool clip_rect_span(int seq, long on, long endEx, SDL_Rect& out) const;
    //! Screen rect of a clip's top-right LOOP/ONE-SHOT chip (empty w when the
    //! body is too narrow to carry one).  Draw and hit-test share it so the
    //! glyph and the clickable area cannot drift apart.
    static SDL_Rect loop_chip_rect(const SDL_Rect& body);
    FadeGrab fade_at(int seq, const SDL_Rect& clip, int mx, int my) const;
    void draw_clip_fades(ui::App& app, int seq, const SDL_Rect& clip);
    void commit_fade(int seq);

    //------------------------------------------------------------------------
    //  Pro Tools ch.32 -- FADES & CROSSFADES (impl in arrange_fades.cpp)
    //------------------------------------------------------------------------
    //! Curve evaluation shared with the engine (ScheduledClip::fadeCurve), so
    //! the fade drawn is bit-for-bit the fade played.
    static float fade_curve(float u, int shape, float k, int slope);
    ClipFade fade_of(int seq) const;
    //! Snapshot fades+regions of `seqs`, run `edit`, queue ONE undo op that
    //! restores / reapplies both sides (the fade twin of the region ops).
    void fade_edit_op(const char* name, const std::vector<int>& seqs,
                      const std::function<void()>& edit);
    //! The undo/redo closure body: restore one captured fade+region pair.
    struct FadeState { int seq; bool hadFade; ClipFade fade; bool hadRegion;
                       AudioRegion region; };
    void capture_fade_state(const std::vector<int>& seqs,
                            std::vector<FadeState>& out) const;
    void restore_fade_state(const std::vector<FadeState>& st);
    //! Crossfade = two audio lane-mates whose regions overlap.  Given either
    //! contributor, find the pair; window is the overlap [a,b).
    bool xfade_pair(int seq, int& left, int& right) const;
    bool xfade_window(int left, int right, long& a, long& b) const;
    //! Source material available beyond a clip's region edges (ticks).
    long avail_before(int seq) const;   // source frames before region start
    long avail_after (int seq) const;   // source frames past region end
    //! Create/resize the crossfade between lane-mates `left`/`right` so its
    //! window becomes [a,b) (clamped to the available source material; returns
    //! false when no overlap is possible at all).  Applies `s` to both halves.
    //! Undo handling is the CALLER's (so gestures push once, not per frame).
    bool create_crossfade(int left, int right, long a, long b,
                          const FadeSettings& s, bool clampNote);
    //! Remove the crossfade pair: fades cleared, regions retracted to a butt
    //! joint at the overlap midpoint.
    void remove_crossfade(int left, int right);
    //! Re-clamp a crossfade's two fades to the pair's current overlap after a
    //! region edit (trim recalculates the crossfade; none left -> fades drop).
    void sync_xfade_after_region_edit(int seq);
    //! Ctrl+F (dialog) / Ctrl+Win+F (defaults, no dialog): fade-in at a clip
    //! start, fade-out at a clip end, crossfade across a splice, Batch Fades
    //! over several whole clips -- decided from the edit selection (p749-753).
    void create_fades_from_selection(ui::App& app, bool useDefaults);
    void fade_to_start(ui::App& app);   // Win+D (p751)
    void fade_to_end(ui::App& app);     // Win+G
    void delete_fades_selection();      // Edit > Fades > Delete
    //! Alt+Win+Left/Right: cycle every completely selected fade through
    //! Standard, S-Curve, presets 1..7 (p756).
    void cycle_fade_shape(int dir);
    //! Apply shape (0/1) or slope (0=EqPow eff. 1, ...) to selected fades from
    //! the right-click Fades submenu.  which: 0 = shape, 1 = slope.
    void set_selected_fade_shape(int which, int value);
    //! Fades covered (completely) by the edit selection; falls back to the
    //! grabber fade selection.  Each entry: seq + half (0 in, 1 out).
    void selected_fades(std::vector<std::pair<int,int>>& out) const;
    void stretch_xfades_after_nudge(int seq, long oldPos, long oldEnd);

    // grabber fade SELECTION (select a fade/crossfade as an object: Delete
    // removes it, +/- nudges it, dragging moves it within the clips)
    int  m_fade_sel_seq  = -1;
    int  m_fade_sel_which = 0;        // 0 fade-in, 1 fade-out, 2 crossfade
    bool m_fade_move = false;         // grabber drag moving the selected fade
    long m_fade_move_ref = 0;         // pointer tick at grab
    long m_fade_move_p0 = 0, m_fade_move_p1 = 0;   // captured refs at grab
    std::vector<FadeState> m_fade_move_before;     // undo capture at grab
    bool fade_hit(int seq, long tick, int& which) const;  // fade under a tick
    void move_fade_to(long tick);     // drag/nudge the selected fade
    void nudge_fade(long delta);
    void draw_fade_selection(ui::App& app);

    // ---- fade & crossfade PREFERENCES (p748, p752, p757) -------------------
    FadeSettings m_def_fadein, m_def_fadeout, m_def_xfade;
    int  m_fade_preroll_ms  = 1000;   // fade-preview pre-roll
    int  m_fade_postroll_ms = 1000;   // fade-preview post-roll
    int  m_auto_fade_ms = 0;          // AutoFades 0..10 ms (0 = off)
    bool m_preserve_fades = true;     // Preserve Fades when Editing
    bool m_smart_fade_ctrl = false;   // Smart fade-adjust needs Ctrl
    bool m_fadepref_open = false;     // preferences popup
    SDL_Rect m_fadepref_rect { 0,0,0,0 };
    void draw_fadepref(ui::App& app);
    bool fadepref_click(ui::App& app, int mx, int my);
    void load_fade_prefs();
    void save_fade_prefs() const;
    std::string fade_root_dir() const;      // SDL pref path (Root Settings)
    std::string m_session_dir;              // set by the shell on save/load
public:
    void set_session_folder(const std::string& dir) { m_session_dir = dir; }
    int  auto_fade_ms() const { return m_auto_fade_ms; }
    //! The session's AutoFades value arrived from a project load (the manual
    //! saves it with the session): mirror it in the preferences panel.
    void set_auto_fade_ms(int ms) {
        m_auto_fade_ms = ms < 0 ? 0 : (ms > 10 ? 10 : ms);
    }
private:

    // ---- the FADES dialog (Fade In / Fade Out / Crossfade; p743-747) -------
    bool m_fdlg_open = false;
    int  m_fdlg_kind = 0;             // 0 fade-in, 1 fade-out, 2 crossfade
    int  m_fdlg_seq  = -1;            // fade-in/out target (first of the set)
    int  m_fdlg_left = -1, m_fdlg_right = -1;   // crossfade contributors
    long m_fdlg_a = 0, m_fdlg_b = 0;  // fade window (ticks)
    //! One clip (or crossfade pair) the dialog's OK applies to; the first
    //! entry is the one the plot draws.  kind mirrors m_fdlg_kind per target.
    struct FadeTarget { int kind = 0; int seq = -1; int left = -1, right = -1;
                        long a = 0, b = 0; };
    std::vector<FadeTarget> m_fdlg_targets;
    ClipFade m_fdlg_fade;             // working settings
    ClipFade m_fdlg_orig;             // COMPARE snapshot (state at open)
    bool m_fdlg_compare = false;      // showing the original right now
    int  m_fdlg_view = 0;             // 0 curves, 1 separate, 2 super, 3 summed
    int  m_fdlg_trk  = 0;             // multitrack xfade: 0 both, 1 first, 2 second
    float m_fdlg_zoom = 1.f;          // waveform amplitude zoom
    int  m_fdlg_drag = 0;             // 0 none, 1 in-curve, 2 out-curve, 3 both,
                                      // 4..7 the black square endpoint handles
    int  m_fdlg_preset_menu = -1;     // preset-thumbnail popup (-1/0 in/1 out)
    bool m_fdlg_settings_menu = false;
    bool m_fdlg_auditioning = false;
    long m_fdlg_audit_end = 0;
    FadeSettings m_fade_preset[5];    // Presets 1-5 (Ctrl-click stores)
    bool m_fade_preset_set[5] = { false,false,false,false,false };
    bool m_fade_save_session = false; // Save Fade Settings To: session folder?
    SDL_Rect m_fdlg_rect { 0,0,0,0 };
    SDL_Rect m_fdlg_plot { 0,0,0,0 };
    std::vector<FadeState> m_fdlg_before;   // state at open (Cancel / undo op)
    std::string m_fdlg_buf;                 // Save-As / Import typed name
    std::string m_fade_settings_name;       // current settings file ("" = factory)
    SDL_Rect m_fdlg_btn[32] {};       // hit rects, indexed by FBtn
    void open_fade_dialog(ui::App& app, const std::vector<FadeTarget>& targets);
    void draw_fade_dialog(ui::App& app);
    bool fade_dialog_mouse(ui::App& app, const ui::MouseEv& e);
    bool fade_dialog_key(ui::App& app, SDL_Keycode k);
    void fade_dialog_apply(bool ok);
    void fade_dialog_load(const FadeSettings& s);
    FadeSettings fade_dialog_settings() const;
    void fade_dialog_live_apply();    // push the working settings to the targets
    void draw_fade_plot(ui::App& app);
    void draw_shape_thumb(ui::App& app, SDL_Rect r, int shape, bool rising,
                          float k, ui::Color c);
    void fade_settings_file(int action, ui::App& app);  // save/save-as/import/delete
    void audition_poll();             // stop the audition at its end tick

    // ---- the BATCH FADES dialog (p753) -------------------------------------
    bool m_bdlg_open = false;
    bool m_b_create[3]  = { true, true, true };    // in / xfade / out
    bool m_b_shape[3]   = { true, true, true };    // adjust existing shape&slope
    bool m_b_length[3]  = { true, true, true };    // adjust existing length
    int  m_b_place = 1;               // 0 pre-splice, 1 centered, 2 post-splice
    int  m_b_ms[3] = { 10, 10, 10 };  // lengths (ms) for in / xfade / out
    FadeSettings m_b_in, m_b_x, m_b_out;   // shapes the batch applies
    int  m_b_edit = -1;               // ms field being typed into
    SDL_Rect m_bdlg_rect { 0,0,0,0 };
    SDL_Rect m_bdlg_btn[24] {};
    void open_batch_dialog(ui::App& app);
    void draw_batch_dialog(ui::App& app);
    bool batch_dialog_mouse(ui::App& app, const ui::MouseEv& e);
    void batch_apply();
    long ms_ticks(int ms) const;      // milliseconds -> ticks at current tempo

    // right-click Fades submenu (p756-757)
    bool m_fadesmenu_open = false;
    SDL_Rect m_fadesmenu_rect { 0,0,0,0 };
    void draw_fadesmenu(ui::App& app);
    bool fadesmenu_click(ui::App& app, int mx, int my);
    // Draw the clip waveform across [bx,bx+bw) using the sample sub-range
    // [s0,s1) (s1<0 == whole clip), so a trimmed trigger shows only its slice.
    // `chan` selects one channel (0 = L, 1 = R) or both summed (-1) -- the
    // View > Clip > Display on All Channels split.  `ghost` draws the wave as
    // a translucent overlay (Overlapped Crossfades view).  The Peak/Power,
    // Rectified and Outlines options (View > Waveforms) are read from the
    // members; Peak/Normal is forced at sample zoom and while recording.
    void   draw_waveform(ui::App& app, const PatchKnob::engine::AudioClip* clip,
                         int bx, int y, int bw, int h,
                         long long s0 = 0, long long s1 = -1,
                         int chan = -1, bool ghost = false);

    // ---- AUTOMATION clip geometry, computed ONCE ---------------------------
    // The curve, the loop-repeat markers, the playhead dot and the hover
    // readout all read this one struct, so they cannot disagree about where a
    // tick sits.  Built from the BLOCK (what the user dragged) plus the
    // REGION's curve window (source / loopLength), which is the automation
    // twin of the audio waveform's [s0,s1) source mapping.
    struct AutoGeom {
        bool     ok = false;
        SDL_Rect plot { 0, 0, 0, 0 };   //!< inset curve area inside the block
        long     position = 0;          //!< block start (project ticks)
        long     length   = 1;          //!< block span   (project ticks)
        long     loop     = 1;          //!< curve period (curve-local ticks)
        long     source   = 0;          //!< curve tick under the block start
        long     tickL    = 0;          //!< project tick at plot.x
        long     tickR    = 0;          //!< project tick at plot's last column
        int      lanes    = 0;
        int      focus    = 0;
        bool     muted    = false;
        //! Curve tick for a project tick (the player's own wrap arithmetic).
        long local_at(long tick) const;
        //! Repeat index of a project tick (floor division, negatives included).
        long repeat_at(long tick) const;
    };
    //! `trig_offset` is the drawn trigger's OWN offset; it is the curve tick
    //! under this block's start.  The engine tracks one region per sequence,
    //! so info.source only ever describes the FIRST block -- passing the
    //! trigger's offset keeps a split clip's second half from redrawing the
    //! first half's window.  Negative = fall back to the region's source.
    AutoGeom auto_geom(int seq, long tick_on, long tick_off,
                       const SDL_Rect& body, long trig_offset = -1) const;
    void draw_automation(ui::App& app, int seq, const AutoGeom& g, bool washed);
    //! Publish an automation clip's block geometry (position / length / source)
    //! so the engine region follows the block a gesture just moved or trimmed.
    void commit_auto_region(int seq);
    bool   m_grow_dir   = false;   // true = drag start edge, false = end edge
    bool   m_adding     = false;   // placing+growing a fresh clip
    int    m_drop_seq   = -1;      // absolute sequence index under the press
    long   m_drop_tick  = 0;
    long   m_drop_offset = 0;      // click-tick - selected-trigger-edge

    // solo bookkeeping (ports perfnames::apply_solo)
    std::vector<char> m_solo;
    std::map<int,int> m_ioTab;
    std::vector<char> m_mute_snapshot;
    std::vector<char> m_solo_snapped;   //!< which entries m_mute_snapshot holds
    bool   m_solo_active = false;

    // ---- Qtractor-inspired additions ---------------------------------------
    bool   m_follow   = false;   // follow-playhead auto-scroll (View>Follow)
    int    m_snap_idx = 2;       // index into the snap table (default 1/4 = beat)
    SDL_Rect m_snap_rect { 0, 0, 0, 0 };   // clickable SNAP readout (cycles snap)
    bool   m_snap_menu = false;
    SDL_Rect m_snap_menu_rect { 0, 0, 0, 0 };

    //------------------------------------------------------------------------
    //  Pro Tools ch.29 -- EDIT MODES.  One of four, plus the combinable
    //  Snap-To-Grid flag, the Grid sub-mode and the two locks.  All clip
    //  gestures route their quantising through esnap(), which is what makes a
    //  mode a mode: Grid snaps, Slip is free, Shuffle packs against
    //  neighbours, Spot opens a dialog instead of dragging.
    //------------------------------------------------------------------------
    enum class EditMode { Shuffle, Slip, Spot, Grid };
    EditMode m_edit_mode  = EditMode::Grid;   // default matches the old always-snapped feel
    bool m_snap_to_grid   = false;   // Snap To Grid while in Shuffle/Slip/Spot
    bool m_grid_relative  = false;   // Grid sub-mode: false = Absolute, true = Relative
    bool m_shuffle_lock   = false;   // Ctrl-click Shuffle in another mode
    bool m_tool_lock      = false;   // Options: Edit/Tool Mode Keyboard Lock
    void set_edit_mode(EditMode m);
    const char* mode_name(int m) const;
    long esnap(long tick) const;             // mode-aware snap for clip edits
    long snap_rel(long delta) const;         // Relative-Grid delta quantise
    //! Shuffle ripple: shift every clip on `laneSeq`'s lane whose start is at
    //! or after `fromTick` by `delta` ticks (audio regions and MIDI triggers),
    //! skipping `skipSeq`.  Gaps between clips are preserved, per the manual.
    void ripple_lane(int laneSeq, long fromTick, long delta, int skipSeq = -1);
    //! Shuffle placement: pack `want` against the end of the nearest earlier
    //! clip on the lane (clips snap to each other; no overlap).
    long shuffle_pack(int laneSeq, int seq, long want) const;
    long m_trim_start0 = 0;   // grabbed clip's span at trim press (Shuffle ripple
    long m_trim_end0   = 0;   // measures the delta off these on release)

    // ---- grid CONFIGURATION (the Grid value pop-up, fig. pt-674-037) -------
    int  m_grid_scale   = 0;      // 0 Bars|Beats  1 Min:Secs  2 Samples  3 Clips/Markers
    bool m_grid_dotted  = false;
    bool m_grid_triplet = false;
    bool m_grid_follow_main = true;   // "Follow Main Time Scale"
    long grid_ticks() const;          // current grid value in ticks, all modifiers in
    long magnet_snap(long tick) const;// Clips/Markers: snap to nearby boundaries
    long sec_to_ticks(double sec) const;
    long ticks_per_sample() const;    // >= 1; Samples grid + pencil zoom gate

    // ---- SPOT dialog --------------------------------------------------------
    bool m_spot_open = false;
    int  m_spot_seq  = -1;
    int  m_spot_kind = 0;             // 0 move, 1 trim start, 2 trim end, 3/4 TCE start/end
    std::string m_spot_buf;           // typed location (bar.beat.tick | m:ss.mmm)
    std::map<int, long> m_timestamp;  // original time stamp per region (first seen)
    void open_spot(ui::App& app, int seq, int kind);
    void draw_spot(ui::App& app);
    void commit_spot();
    bool parse_time(const std::string& s, long& tick) const;

    //------------------------------------------------------------------------
    //  Pro Tools ch.29 -- TOOLS: sub-modes, chords, gestures
    //------------------------------------------------------------------------
    int m_trim_mode = 0;    // 0 Standard, 1 TCE, 2 Scrub, 3 Loop
    int m_grab_mode = 0;    // 0 Time, 1 Separation, 2 Object
    int m_zoom_mode = 0;    // 0 Normal, 1 Single (returns to the previous tool)
    EditTool m_tool_before_zoom = EditTool::Grab;
    bool m_fkey_down[6] = { false,false,false,false,false,false };  // F5..F10 held
    void select_tool(EditTool t, bool from_key);   // F-key select / sub-mode cycle
    // Zoomer gestures: rubber-band range zoom + Ctrl continuous zoom
    bool m_zoomer_band = false; int m_zoomer_x0 = 0, m_zoomer_x1 = 0;
    bool m_zoomer_cont = false; int m_zoomer_cx = 0, m_zoomer_cy = 0;
    double m_zoomer_s0 = 0.0;   int m_zoomer_h0 = 0;
    // Standard Trim trims ALL selected clips: refs captured at press.
    struct TrimRef { int seq; long pos, len, src; };
    std::vector<TrimRef> m_trim_group;
    bool m_tce_drag  = false; long m_tce_len0 = 0;   // TCE trim pending length
    bool m_scrubtrim = false; bool m_scrubtrim_left = false;
    bool m_looptrim_src = false;   // Loop Trim, bottom half (source iteration)
    bool m_tandem = false; int m_tandem_left = -1, m_tandem_right = -1;
    long m_tandem_bound0 = 0;
    //! Smart-tool zone under the pointer for a clip body:
    //! 0 selector, 1 grabber, 2 trim start, 3 trim end, 4 fade-in, 5 fade-out,
    //! 6 crossfade, 7 fade-shape, 8 scrub (Ctrl).  -1 = not over the clip.
    int smart_zone(int seq, const SDL_Rect& body, int mx, int my) const;
    static int kFadeStripH_pt();   // the clip-top fade strip height (shared)
    void draw_smart_glyph(ui::App& app);
    // crossfade drag (Smart tool, bottom edge between two adjacent clips)
    bool m_xfade_drag = false; int m_xfade_left = -1, m_xfade_right = -1;
    long m_xfade_bound = 0;
    // Shuttle Lock (numeric keypad): signed speed multiple of realtime; the
    // frame loop advances the playhead by it.  No varispeed audio: the engine
    // has no scrub path, so this is honest transport shuttling only.
    double m_shuttle = 0.0; Uint64 m_shuttle_ms = 0;
    int m_shuttle_custom = 800;    // Custom Shuttle Lock Speed, 50..800 (%)
    void set_shuttle(int digit, bool negative);
    // Pencil on audio: destructive sample redraw, only at sample zoom.
    bool m_pencil_audio = false; int m_pencil_seq = -1;
    void pencil_redraw(int seq, int px, int py);

    //------------------------------------------------------------------------
    //  Pro Tools ch.29 -- ZOOM system
    //------------------------------------------------------------------------
    float  m_wave_zoom = 1.f;      // vertical AUDIO zoom (waveform amplitude)
    float  m_midi_zoom = 1.f;      // vertical MIDI zoom (note-preview spread)
    double m_zoom_preset[5] = { 0,0,0,0,0 };   // stored m_scale_x (0 = empty)
    double m_prev_scale = 0.0; long m_prev_scroll = 0;   // previous zoom level
    void remember_zoom();          // capture current zoom before changing it
    void recall_prev_zoom();
    void zoom_at(int anchorX, double factor);   // zoom keeping anchorX pinned
    void zoom_overview();          // 256 samples/pixel (Ctrl-click the Zoomer)
    // Zoom Toggle: stores/recalls h+v zoom, lane heights and the grid setting.
    struct ZoomToggleState {
        double scale = 0; long scroll = 0; float wave = 1, midi = 1;
        int rowh = 80; std::map<int,int> trackH;
        int snap_idx = 2; bool dotted = false, triplet = false; int grid_scale = 0;
    };
    bool m_zt_on = false;          // toggled IN right now
    ZoomToggleState m_zt_out;      // the state to come back to
    ZoomToggleState m_zt_in;       // "Last Used" toggled-in state (kept across)
    bool m_zt_in_valid = false;
    int  m_zt_pref_v = 0;          // Vertical Zoom: 0 Selection, 1 Last Used
    int  m_zt_pref_h = 0;          // Horizontal Zoom: 0 Selection, 1 Last Used
    int  m_zt_pref_height = 5;     // 0 LastUsed 1 Medium 2 Large 3 Jumbo 4 Extreme 5 FitToWindow
    int  m_zt_pref_view = 0;       // Track View: PatchKnob has one view -> "No Change"
    bool m_zt_remove_range = false;   // Remove Range Selection After Zooming In
    bool m_zt_sep_grid = true;        // Separate Grid Settings When Zoomed In
    bool m_zt_follow_sel = true;      // Zoom Toggle Follows Edit Selection
    int  m_zt_lane = -1;              // lane toggled in on (auto-toggle bookkeeping)
    void zoom_toggle(bool cancel);
    void zt_capture(ZoomToggleState& s) const;
    void zt_restore(const ZoomToggleState& s);
    bool m_zt_menu = false; SDL_Rect m_zt_menu_rect { 0, 0, 0, 0 };

    //------------------------------------------------------------------------
    //  Pro Tools ch.30 -- SELECTIONS
    //------------------------------------------------------------------------
    // The EDIT selection: a tick range over a contiguous row range (indices
    // into active_list()).  -1 start = none; start == end = insertion point.
    // The TIMELINE selection is perform's left/right tick (the play range),
    // exactly as before -- linking mirrors edits into it.
    long m_sel_start = -1, m_sel_end = -1;
    int  m_sel_lo = -1, m_sel_hi = -1;
    bool m_link_timeline = true;   // Link Timeline and Edit Selection
    bool m_link_track    = true;   // Link Track and Edit Selection
    long m_lastsel[2] = { -1, -1 }; int m_lastsel_rows[2] = { -1, -1 };
    void set_edit_selection(long a, long b, int lo, int hi);
    void restore_last_selection();
    bool m_selecting = false; long m_sel_anchor = 0; int m_sel_anchor_row = -1;
    int  m_click_count = 0;        // 1/2/3 clicks (Selector double/triple)
    bool m_tl_selecting = false; long m_tl_anchor = 0;   // ruler timeline drag
    // Ruler marker drags: 1 tl-start, 2 tl-end, 3 ed-start, 4 ed-end,
    // 5 slide-timeline (Alt), 6 slide-edit (Alt).
    int  m_marker_drag = 0; long m_marker_off = 0;
    bool m_tab_transients = false;
    long next_transient(long fromTick, bool backward) const;
    long next_boundary (long fromTick, bool backward) const;
    long tab_target    (long fromTick, bool backward) const;
    void nudge_selection(int which, long delta);   // 0 range, 1 start, 2 end
    void move_selection_lane(int dir, bool extend, bool remove);
    void scroll_lane_into_view(int row);
    void selection_changed();      // link mirror + last-selection + auto-toggle
    void draw_edit_selection(ui::App& app);
    //! Rows (into active_list) the edit selection covers; falls back to the
    //! focused lane so selection-less commands still have a target.
    void sel_rows(int& lo, int& hi) const;

    // ---- counters + Edit Selection indicators (topbar row B) ---------------
    int  m_counter_edit = -1;      // -1 none, 0 Main, 1 Start, 2 End, 3 Length
    int  m_counter_sub  = 0;       // subfield: 0 bar, 1 beat, 2 tick
    long m_counter_v[3] = { 0,0,0 };
    int  m_counter_calc = 0;       // 0 off, +1 add, -1 subtract (calculator entry)
    std::string m_counter_buf;
    SDL_Rect m_counter_rect[4] {};             // main, start, end, length
    bool m_counter_scrub = false; int m_counter_scrub_y0 = 0; long m_counter_scrub_v0 = 0;
    int  m_counter_scrub_which = -1;
    void counter_begin(int which);
    void counter_commit(bool apply);
    void counter_load(int which);              // current value -> m_counter_v
    long counter_value() const;                // m_counter_v -> ticks
    void counter_bump(int dir);                // +-1 on the active subfield
    bool counter_key(ui::App& app, SDL_Keycode k);
    void counter_apply(int which, long tick);  // write an indicator back

    // ---- topbar (mode block, tool strip, zoom presets, fields) -------------
    SDL_Rect m_mode_rect[4] {};
    SDL_Rect m_preset_rect[5] {};
    SDL_Rect m_zt_rect { 0,0,0,0 };
    SDL_Rect m_link_tl_rect { 0,0,0,0 }, m_link_tr_rect { 0,0,0,0 };
    SDL_Rect m_tab_rect { 0,0,0,0 }, m_univ_btn_rect { 0,0,0,0 };
    SDL_Rect m_grid_rect { 0,0,0,0 }, m_nudge_rect { 0,0,0,0 };
    SDL_Rect m_vzoom_rect[4] {};   // audio -, audio +, midi -, midi +
    // drag up/down on an audio / MIDI zoom button = continuous vertical zoom
    int m_vzoom_drag = -1; int m_vzoom_y0 = 0; float m_vzoom_v0 = 1.f;
    bool m_grid_menu = false;  SDL_Rect m_grid_menu_rect { 0,0,0,0 };
    bool m_nudge_menu = false; SDL_Rect m_nudge_menu_rect { 0,0,0,0 };
    int  m_nudge_idx = 2;          // same table as snap_value()
    long nudge_ticks() const;
    // click-and-hold on a tool / preset button opens its pop-up menu
    Uint32 m_hold_ms = 0; int m_hold_tool = -1; int m_hold_preset = -1;
    bool m_submode_menu = false; int m_submode_tool = -1;
    SDL_Rect m_submode_rect { 0,0,0,0 };
    int  toolbar_h() const { return 44; }      // rows A + B of the topbar
    void draw_topbar(ui::App& app);
    bool topbar_click(ui::App& app, const ui::MouseEv& e);
    void draw_submode_menu(ui::App& app);
    bool submode_click(ui::App& app, int mx, int my);
    void draw_grid_menu(ui::App& app);
    bool grid_menu_click(ui::App& app, int mx, int my);
    void draw_zt_menu(ui::App& app);
    bool zt_menu_click(ui::App& app, int mx, int my);
    void draw_nudge_menu(ui::App& app);
    bool nudge_menu_click(ui::App& app, int mx, int my);
    const char* trim_mode_name(int m) const;
    const char* grab_mode_name(int m) const;

    // ---- Universe view ------------------------------------------------------
    bool m_universe_on = false; int m_universe_h = 48;
    bool m_univ_drag = false, m_univ_resize = false;
    SDL_Rect m_universe_rect { 0,0,0,0 };
    void draw_universe(ui::App& app);
    bool universe_mouse(ui::App& app, const ui::MouseEv& e);
    void universe_goto(int mx, int my);

    // Per-clip bright color (auto-assigned; clip body = colour, data = black).
    std::map<int, int> m_clipColor;        // seq -> palette index (override)
    ui::Color clip_color(int seq) const;   // bright body colour for a clip

    // Per-track LANE HEIGHT (Ableton/Ardour): drag a header's bottom edge to
    // resize its lane.  m_trackH[seq] overrides the default row_h.
    std::map<int, int> m_trackH;           // seq -> lane height px (absent = row_h)
    int  track_h(int seq) const;           // effective lane height for a track
    int  row_top(int r) const;             // screen-y top of on-screen row r (cumulative)
    // Header bottom-edge resize drag.
    bool m_hdr_resize = false;
    int  m_hdr_resize_seq = -1;
    int  m_hdr_resize_y0  = 0;             // pointer y at grab
    int  m_hdr_resize_h0  = 0;             // height at grab
    bool m_hover_resize   = false;         // pointer over a resize edge (cursor)
    bool m_hover_loop     = false;         // pointer over a clip loop corner (cursor)
    bool m_hover_extend   = false;         // pointer over a clip bottom extend edge
    bool m_hover_trim     = false;         // pointer over a clip's left/right trim zone
    void apply_resize_cursor();            // SIZENS/SIZEWE/loop cursor per hover
    // Clip bottom-edge EXTEND drag (lengthen the region, loop-filling).
    bool m_extending      = false;
    int  m_extend_seq     = -1;

    // clip drag-copy: Ctrl+DRAG duplicates the trigger instead of moving it.
    // A Ctrl+CLICK (press and release without travelling) is the universal
    // add-to-selection gesture and must NOT duplicate: the release used to
    // commit unconditionally, dropping a second, independent sequence at the
    // identical tick on the same lane -- invisible on screen, audible as the
    // part playing doubled.  The press position is kept so the release can tell
    // a click from a drag.
    bool   m_copying     = false;
    long   m_copy_len    = 0;    // duplicated trigger length (ticks)
    long   m_copy_offset = 0;    // duplicated trigger loop-offset (ticks)
    long   m_ghost_tick  = 0;    // snapped drop position of the ghost
    long   m_copy_src_tick = 0;  // the original's start tick, for the no-op test
    int    m_press_px = 0, m_press_py = 0; // pointer position at the last clip press
    //! Pixels the pointer must travel before a press counts as a drag.
    static const int kDragSlop = 4;

    // ---- multi-clip move ---------------------------------------------------
    //! One lane-sequence taking part in a group drag, with the position its
    //! selected clips had when the drag started.  A multi-selection used to be
    //! destroyed the moment you grabbed one of its clips (the press cleared the
    //! selection), so it was only ever usable for Delete and Copy.
    struct MoveGroup { int seq; long start; bool audio; };
    std::vector<MoveGroup> m_move_group;
    long m_move_anchor = 0;      //!< start tick of the clip actually grabbed
    int  m_group_click_seq  = -1;//!< clip grabbed out of a group, for a bare click
    long m_group_click_tick = 0;
    //! Rigidly shift every clip in m_move_group so the grabbed one lands on
    //! `anchor_to`.  Two passes: ask for the delta, then re-apply the largest
    //! delta EVERY lane could actually take, so a lane that hits a neighbour
    //! clamps the whole group instead of letting it shear apart.
    void move_clip_group(long anchor_to);
    //! Group start tick of `seq`'s selected clips (-1 when it has none).
    long selected_group_start(int seq) const;
    //! Is the clip under (seq, tick) already part of the selection?
    bool clip_selected_at(int seq, long tick) const;
    //! How many clips are selected across the whole project.
    int  selected_clip_count() const;

    struct ClipCopy {
        int seq = -1;
        long rel_start = 0;
        long length = 0;
        long offset = 0;
    };
    std::vector<ClipCopy> m_clip_clipboard;
    //! Span of the clipboard contents, so repeated pastes advance instead of
    //! stacking every copy on the same tick.
    long   m_clip_span  = 0;
    long   m_paste_tick = -1;      //!< where the last paste landed
    long   m_dup_origin = 0;       //!< timeline start of the copied block
    bool   m_lassoing = false;
    int    m_lasso_x0 = 0, m_lasso_y0 = 0;
    int    m_lasso_x1 = 0, m_lasso_y1 = 0;

    // The lane the last gesture touched.  Paste / keyboard edits land here, so
    // it gets a visible highlight in both the header and the canvas.
    int    m_focus_lane = -1;                        // lane KEY, not a seq index
    // Live drag feedback: a snapped guide line + a bar.beat readout by the
    // pointer while a clip is moved / trimmed / drawn.
    bool   m_guide_on   = false;
    long   m_guide_tick = 0;
    std::string m_guide_text;
    void   drag_feedback(ui::App& app);              // guide line + readout
    void   edge_autoscroll();                        // pan when dragging off-edge
    bool   dragging() const;                         // any clip gesture active
    int    count_clips_in_rect(SDL_Rect box) const;  // lasso live count

    // inline clip rename (double-click a clip -> edit its sequence name)
    std::string m_edit_name;
    int      m_edit_seq  = -1;
    SDL_Rect m_edit_rect { 0, 0, 0, 0 };

    //------------------------------------------------------------------------
    //  Pro Tools ch.28 -- MULTIPLE UNDO state (impl in arrange_edit.cpp)
    //------------------------------------------------------------------------
    std::vector<UndoOp> m_undo_q;      //!< the queue, oldest first
    size_t m_undo_done   = 0;          //!< [0,done) done (bold), [done,..) undone
    int    m_undo_levels = 32;         //!< 1..64 (Levels of Undo preference)
    UndoOp m_undo_pending;             //!< open compound collects into this
    int    m_undo_compound = 0;        //!< compound nesting depth
    bool   m_undo_open = false;        //!< Undo History window ("U")
    bool   m_undo_show_times = true;   //!< Options > Show Creation Times
    int    m_undo_scroll = 0;
    bool   m_undo_opts = false;        //!< Options pop-up open
    SDL_Rect m_undo_rect { 0,0,0,0 };       // window
    SDL_Rect m_undo_opts_btn { 0,0,0,0 };   // Options selector
    SDL_Rect m_undo_opts_rect { 0,0,0,0 };  // Options pop-up
    void draw_undo_window(ui::App& app);
    bool undo_window_mouse(ui::App& app, const ui::MouseEv& e);
    bool undo_window_wheel(int mx, int my, int dy);

    //------------------------------------------------------------------------
    //  Pro Tools ch.28 -- VIEW options (View > Waveforms / View > Clip)
    //------------------------------------------------------------------------
    bool m_wf_power    = false;   // Peak (off) vs Power/RMS (on) overview
    bool m_wf_rect     = false;   // Rectified display
    bool m_wf_outlines = true;    // waveform Outlines
    bool m_wf_overlap  = true;    // Overlapped Crossfades view
    bool m_clip_show_name = true; // View > Clip > Name
    int  m_clip_time      = 0;    // 0 NoTime 1 Current 2 OrigStamp 3 UserStamp
    bool m_clip_all_chan  = false;// Display on All Channels (stereo L/R split)
    bool m_clip_sync      = true; // Sync Points shown
    bool m_clip_shadows   = true; // Overlap Shadows
    bool m_clip_transp    = true; // Transparency (drag overlay)
    bool m_clip_overwrite = true; // Clip Overwrite Indicator
    bool m_clip_gain_info = false;// Clip Gain Info (fader glyph + dB)
    bool m_clip_rating    = false;// Rating shown on clips
    bool m_view_menu = false;
    SDL_Rect m_view_rect { 0,0,0,0 }, m_view_menu_rect { 0,0,0,0 };
    void draw_view_menu(ui::App& app);
    bool view_menu_click(ui::App& app, int mx, int my);
    //! True when this clip's waveform must fall back to Peak/Normal: sample
    //! zoom, or the clip is the live record preview (manual p656).
    bool waveform_forced_peak(const PatchKnob::engine::AudioClip* clip) const;
    std::map<int,long> m_user_stamp;   // user time stamp (orig = m_timestamp)
    std::map<int,long> m_sync_point;   // clip-RELATIVE sync point tick
    std::map<int,int>  m_rating;       // 1..5 (absent = unrated)
    std::set<int>      m_user_named;   // renamed -> user-defined clip

    //------------------------------------------------------------------------
    //  Pro Tools ch.28/31 -- EDIT commands (impl in arrange_edit.cpp)
    //------------------------------------------------------------------------
    bool m_edit_menu = false;
    SDL_Rect m_editbtn_rect { 0,0,0,0 }, m_edit_menu_rect { 0,0,0,0 };
    void draw_edit_menu(ui::App& app);
    bool edit_menu_click(ui::App& app, int mx, int my);
    //! Range selection present (start < end)?
    bool have_range() const { return m_sel_start >= 0 && m_sel_end > m_sel_start; }
    //! Lane seqs the edit selection covers (sel_rows -> active_list entries).
    void range_lanes(std::vector<int>& lanes) const;
    //! Split every clip crossing `a` and `b` on the selected lanes (the
    //! auto-created leftovers on either side of a cut/clear).
    void separate_at(long a, long b);
    //! Select exactly the clips fully inside [a,b) on the selected lanes.
    void select_range_clips(long a, long b);
    void edit_copy();               // Ctrl+C: object or range copy
    void edit_cut();                // Ctrl+X: cut (clipboard + remove)
    void edit_clear(bool force);    // Ctrl+B / Ctrl+Del: remove, no clipboard
    void edit_paste(long at);       // Ctrl+V: overwrite (or Shuffle-insert)
    void edit_delete();             // Delete key: object delete via the queue
    void edit_delete_guts();        // shared object-delete body (in a compound)
    void repeat_to_fill(ui::App& app);
    void capture_clip(ui::App& app);            // Ctrl+R
    void separate_selection(int mode, ui::App& app);  // 0 AtSel 1 OnGrid 2 AtTransients
    void separate_apply(int mode);              // after the Pre-Separate dialog
    void heal_separation();                     // Ctrl+H
    //! which: 0 ToSelection, 1 StartToInsertion, 2 EndToInsertion,
    //! 3 StartToFill, 4 EndToFill, 5 ToFillSelection,
    //! 6 ToFileStart, 7 ToFileEnd, 8 ToFileBoundaries
    void trim_cmd(int which);
    //! what: 0 move clips, 1 trim start, 2 trim end, 3 slide contents
    void nudge_clips(long delta, int what);
    long next_larger_nudge() const;
    void quantize_to_grid();                    // Ctrl+0
    void rate_selected(int r);                  // 1..5, 0 clears
    void consolidate_selection();               // Alt+Shift+3 (via hook)
    void tce_to_timeline();                     // TCE Edit to Timeline Selection
    void fit_to_selection();                    // TCE clips to the edit selection
    void begin_clip_rename(ui::App& app);       // Ctrl+Shift+R on the selection
    //! Layered Editing option (p734): ON keeps partially covered clips intact;
    //! OFF trims them to the overlapper on drop.  Fully covered clips are
    //! removed either way.
    bool m_layered = false;
    SDL_Rect m_lay_rect { 0,0,0,0 };
    void resolve_overlaps(int seq);
    //! Auto-Name Separated Clips preference (p727) + naming helper.
    bool m_auto_name_sep = true;
    void auto_name_clip(int seq, int fromSeq);
    // Pre-Separate / Compact prompt plumbing (typed ms via app.begin_text).
    std::string m_dlg_buf;
    int  m_dlg_kind = -1;          // 0/1/2 = separate modes, 3 = compact
    // gain-special clipboard (Cut/Copy/Paste Special for clip gain)
    float m_gain_clipboard = -999.f;
    std::string m_capture_name;    // Capture Clip's name -> next pasted clips
    // nudge extensions: typed custom value + follow-main
    long m_nudge_custom = 0;       // 0 = use the ladder (m_nudge_idx)
    bool m_nudge_follow = false;   // "Follow Main Time Scale" (off: ladder)
    // transient status line ("Can't Undo", heal refusals...)
    std::string m_flash; Uint32 m_flash_ms = 0;
    void flash(const std::string& s);
    void draw_flash(ui::App& app);
    //! Draw overlap shadows + sync points + times + gain info + rating for one
    //! clip body (called from draw_clips; all View > Clip options).
    void draw_clip_adornments(ui::App& app, int seq, const SDL_Rect& body,
                              long tick_on, long endEx, bool audio);

    // ---- helpers -----------------------------------------------------------
    // ---- ONE definition of where a clip is --------------------------------
    // There were four (draw_clips, count_clips_in_rect, select_clips_in_rect,
    // clip_rect_of), and they disagreed about the end tick (+1 vs exclusive),
    // so hit-testing and drawing were a pixel out of step.
    struct ClipSpan { int seq; long on; long endEx; bool selected; long offset; };
    //! Spans of every clip on `seq`'s own sequence (audio: its region; MIDI:
    //! each trigger).  Appends, so a lane can be accumulated over its sequences.
    void clip_spans(int seq, std::vector<ClipSpan>& out) const;
    //! Visit every clip in the project, already resolved to a span.
    void for_each_clip(const std::function<void(const ClipSpan&)>& fn) const;
    //! On-screen rect for a span on a lane whose top is `lane_y`.
    SDL_Rect span_rect(const ClipSpan& s, int lane_y, int lane_h) const;
    //! Visit every clip on a VISIBLE lane, with its on-screen rect.
    void for_each_visible_clip(
        const std::function<void(const ClipSpan&, const SDL_Rect&)>& fn) const;

    // ---- header column geometry, computed ONCE -----------------------------
    // draw_headers, instr_box_rect and the hit-tester each derived these
    // separately (one of them with the literal 38), so they could drift apart.
    struct HeaderGeom { int spine_w, badge_x, badge_w, name_x,
                            rm_w, rm_h, rm_x, rm_y,
                            btn_w, btn_h, btn_x, vu_w, vu_x; };
    HeaderGeom header_geom() const;

    std::vector<int> active_list() const;            // active sequence indices
    int  lane_key(int seq) const;
    bool is_automation(int seq) const;
    std::vector<int> lane_sequences(int lane_seq) const;
    //! Every active sequence grouped by lane, aligned with `act` (bucket i =
    //! the sequences sharing act[i]'s lane key, ascending).  ONE pass over the
    //! sequence table; lane_sequences() rescans all of it per call, which was
    //! paid per lane per frame (draw_canvas, draw_universe) and per CLIP per
    //! frame in the Overlapped Crossfades path.
    std::vector<std::vector<int>> lane_groups(const std::vector<int>& act) const;
    int  clip_sequence_at(int lane_seq, long tick) const;
    int  canvas_x() const { return rect.x + header_w; }
    int  canvas_y() const { return rect.y + ruler_h; }
    int  canvas_w() const { return std::max(0,rect.w - header_w - scrollbar_w); }
    int  canvas_h() const { return std::max(0,rect.h - ruler_h - scrollbar_h); }

    // song_end() is declared PUBLIC above -- perform::get_max_trigger() only
    // knows about seq24 TRIGGERS, so an audio-only project (whose material lives
    // in m_region) reported a song end of 0 and could not be scrolled, fitted or
    // navigated, and the transport could not find the end of the song either.
    //! Keep m_scroll_ticks inside [0, song end].  Several paths (arrow keys,
    //! scrollbar paging, edge autoscroll) used to scroll into empty infinity.
    void clamp_scroll();
    //! Lanes have INDIVIDUAL heights, so the visible-row count is a running sum,
    //! not canvas_h()/row_h -- that estimate was wrong on every resized lane and
    //! was duplicated at six call sites.
    int  visible_rows() const;
    int  max_v_offset() const;

    //! Height of the TOOL STRIP that occupies the top of the ruler (edit tools,
    //! zoom, LOOP chip, playhead readout, "?").  The painter and the hit-tester
    //! each hard-coded 22 in their own arithmetic, so the strip a click was
    //! tested against and the strip that was drawn could drift apart.  Below it
    //! is the TIME BAND -- the only part of the ruler that is actually a ruler,
    //! and so the only part a click may scrub the playhead from.
    //  Now the topbar (mode block + tools + counters, 2 rows) plus the Universe
    //  strip when shown; the TIME BAND is everything below it.
    int  toolstrip_h() const { return toolbar_h() + (m_universe_on ? m_universe_h : 0); }
    long scrub_tick(int px) const;
    long snap_down_bar(long tick) const;
    static bool pt_in_rect(const SDL_Rect& r, int x, int y);
    SDL_Rect m_loopchip_rect { 0, 0, 0, 0 };   // ruler chips: not scrub targets
    SDL_Rect m_readout_rect  { 0, 0, 0, 0 };

    int  tick_to_x(long tick) const;
    long x_to_tick(int px) const;
    long snap(long tick) const;
    int  row_at(int py) const;                       // on-screen row or -1
    long playhead() const;

    // ---- adaptive metric grid ----------------------------------------------
    // How dense the bar / beat / sub-division rulings may be at the current
    // zoom.  Every field is in TICKS; 0 means "no room, do not draw".  The bar
    // lines and the bar NUMBERS thin out independently in powers of two (every
    // bar, then every 2, 4, 8, 16 ...) so the ruler never jumbles.  draw_ruler()
    // and draw_canvas() share this, so the grid always lines up with the digits.
    struct Metric {
        long barStep   = 0;   // ticks between drawn bar lines
        long labelStep = 0;   // ticks between drawn bar NUMBERS (>= barStep)
        long beatStep  = 0;   // ticks between beat ticks (0 = none)
        long subStep   = 0;   // ticks between snap sub-division ticks (0 = none)
    };
    Metric metric(int cw) const;
    std::string bbt(long tick) const;       // "bar.beat.tick" readout
    std::string bars_len(long ticks) const; // "N.B" length readout
    std::string time_str(long tick) const;  // "m:ss.mmm" wall-clock readout

    void draw_ruler   (ui::App& app);
    void draw_scrollbar(ui::App& app);
    void draw_canvas  (ui::App& app, const std::vector<int>& act);
    //! `laneSeqs` = the clip's lane bucket from lane_groups(), so per-clip
    //! sibling walks (Overlapped Crossfades) need no lane rescan; null falls
    //! back to lane_sequences(seq).
    void draw_clips   (ui::App& app, int seq, int lane_y,
                       const std::vector<int>* laneSeqs = nullptr);
    void draw_headers (ui::App& app, const std::vector<int>& act);

    void press_canvas (ui::App& app, const ui::MouseEv& e, int seq);
    void drag_canvas  (ui::App& app, const ui::MouseEv& e);
    void apply_solo   ();

    // Qtractor-inspired helpers
    long        snap_value (int idx) const;          // ticks for a snap index
    const char* snap_label (int idx) const;          // ASCII name for the ruler
    void        zoom_to_fit();                        // fit 0..last-trigger to canvas
    void        zoom_to_selection();                  // shift+F : frame the selection
    void        begin_rename(ui::App& app, int seq, SDL_Rect clip_rect);
    void        draw_rename (ui::App& app);           // inline name editor overlay
    void        draw_loop_band(ui::App& app);         // shaded L..R span over lanes
    int         create_pattern(int source_seq, long start, long length,
                               long offset, bool copy_events);
    void        unselect_all_triggers();
    void        select_clips_in_rect(SDL_Rect box, bool add_to_selection);
    void        copy_selected_clips();
    void        cut_selected_clips();
    void        select_all_clips();
    void        duplicate_selected_clips();
    void        paste_clips(long start_tick);
    void        delete_selected_clips();
    bool        any_selected_clip() const;
    //! The tick an edit acts on: the playhead.  Paste, split and duplicate all
    //! used the last MOUSE PRESS instead, so moving the playhead could not
    //! retarget them.
    long        edit_tick() const { return playhead(); }
    //! Move the transport (works while stopped -- that is the point).
    void        seek_to(long tick);
    //! Faithful pattern copy (every event + its tracker column), used to paste
    //! a clip onto a DIFFERENT lane than the one it came from.
    void        clone_pattern_events(int fromSeq, int toSeq);
};

} // namespace arrange
#endif

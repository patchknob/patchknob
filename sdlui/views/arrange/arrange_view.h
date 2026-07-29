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

class perform;   // PatchKnob engine core
namespace PatchKnob { namespace engine { struct AudioClip; } }

namespace arrange {

class ArrangeView : public ui::Widget {
public:
    explicit ArrangeView(perform* p);

    // ui::Widget -------------------------------------------------------------
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;

    // Optional externally-driven playhead (in MIDI ticks).  When < 0 the view
    // reads perform::get_tick() instead.  The shell can set this to animate a
    // playhead when the transport isn't actually running.
    long playhead_tick = -1;

    // Fired to open a clip's editor.  kind: 0 = piano roll, 1 = tracker.  The
    // shell rebinds its piano/tracker editor to `seq` and shows it.
    std::function<void(int seq, int kind)> on_open_editor;
    // Fired when an AUDIO clip is double-clicked: open it in the sample/warp
    // editor (bottom dock) instead of a MIDI editor.
    std::function<void(int seq)> on_open_sample_editor;

    // ---- track-header instrument selector ----------------------------------
    // Each instrument (MIDI) track drives one instrument node; the header shows a
    // dropdown (line 2) to pick/assign it.  The shell supplies the instrument
    // list + the current name per track and applies the pick (sets the
    // sequence's MIDI channel to that instrument + wires it).  Audio tracks have
    // no instrument node, so the dropdown is hidden for them.
    std::function<std::vector<std::string>()> on_list_instruments;
    std::function<std::string(int seq)>       on_track_instrument;
    std::function<void(int seq, int idx)>     on_pick_instrument;

    // Fired by the ADD-TRACK "+" affordance (bottom of the header column) and by
    // each track's "x" remove button, so the shell can create / destroy the
    // underlying sequence.  kind: 0 = instrument (MIDI) track, 1 = audio track.
    // `seq` is an ABSOLUTE sequence index.
    std::function<void(int kind)> on_add_track;      // 0 = instrument, 1 = audio
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

    //! Drop EVERY per-sequence entry for `seq` (audio ptr, source length, region,
    //! fade, colour, frozen flag, and the lane height if this was the lane's last
    //! sequence).  Call this whenever a sequence is deleted / its track removed so
    //! a later reuse of the recycled index cannot inherit stale (or freed) state.
    //! MUST run while the shell's lane routing for `seq` is still intact.
    void forget_seq(int seq);

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
    //! rendered audio + region for undo, then tears the clip down.  Falls back to
    //! on_unfreeze when unbound.
    std::function<void(int seq)> on_clip_delete;
    //! Ctrl+Z: after the view pops its trigger-undo, the shell restores the most
    //! recent disk-cached audio-clip delete (re-attach audio + region + lane).
    std::function<void()> on_undo;
    //! Shell reflects freeze state back so the menu label + lane tint track it.
    void set_frozen(int seq, bool frozen);
    bool is_frozen(int seq) const;

    // ---- clip crossfades (audio/frozen clips) ------------------------------
    // Per-clip fade-in/out lengths (ticks) + curve tensions (-1..1).  Drag the
    // top-corner handle to set the fade length; drag the mid-fade point in/out
    // to bend the curve.  on_clip_fade fires the ticks+tensions to the shell,
    // which converts to frames and updates the engine.
    struct ClipFade { long inTicks = 0, outTicks = 0; float inK = 0.f, outK = 0.f; };
    std::function<void(int seq, long inTicks, long outTicks, float inK, float outK)> on_clip_fade;
    void set_clip_fade(int seq, long inTicks, long outTicks, float inK, float outK);

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

    // Layout knobs (pixels).  The shell just sizes `rect`; these partition it.
    int header_w = 6 * 30;   // left track-header column width  (== c_names_x)
    int ruler_h  = 20;       // top time-ruler height
    int row_h    = 80;       // per-track lane / header row height (default 2x)

private:
    perform* m_perf;

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
    std::vector<std::string> m_instrmenu_items;
    SDL_Rect instr_box_rect (int row_y, int ch) const;   // dropdown box on line 2
    void     draw_instrmenu (ui::App& app);
    bool     instrmenu_click(ui::App& app, int mx, int my);
    SDL_Rect add_row_rect  () const;                  // bottom "+" affordance row
    void     draw_add_button(ui::App& app);           // "+" ring + ADD TRACK label
    void     draw_remove_btn(ui::App& app, SDL_Rect box, bool hot);  // per-track "x"
    // live pointer (logical coords) for hover highlight -- plain motion events
    // are not delivered without a button held, so draw() polls the mouse too.
    int      m_mx = -1, m_my = -1;

    // audio tracks: seq index -> clip whose waveform draws in the lane
    std::map<int, const PatchKnob::engine::AudioClip*> m_audio;
    // seq index -> the clip's natural (untrimmed) length in ticks (the SOURCE
    // length), used to clamp the region + map the waveform window.
    std::map<int, long> m_audioLen;

    // ---- Ardour AudioRegion (per audio seq, in ticks) ----------------------
    // A region is a non-destructive view onto its source clip:
    //   position = timeline placement (mirrors the trigger's start),
    //   source   = start-offset INTO the source clip,
    //   length   = region span (mirrors the trigger's length).
    // The trigger holds position+length; `source` lives here because the
    // trigger's own loop-offset wraps modulo the pattern length (MIDI-only).
    // The Ardour content laws (Region::set_position / trim_front / trim_end /
    // set_start) are applied to this on a gesture release.
    struct AudioRegion { long position = 0, source = 0, length = 0;
                         float gain = 1.0f; bool muted = false; bool loop = false; };
    std::map<int, AudioRegion> m_region;
    AudioRegion& region_for(int seq);              // lazy-init from the trigger
    void commit_region(int seq, bool leftTrim, bool rightTrim);  // apply Ardour law
    // Ardour Playlist::_split_region: at splitTick, LEFT keeps {P,S,B}; RIGHT is a
    // new lane-sharing sequence with {P+B, S+B, L-B}, B = splitTick - P.
    void split_audio_clip(int seq, long splitTick);
    std::set<int> m_frozen;   // sequence indices currently frozen (menu + tint)
    std::map<int, ClipFade> m_clipFade;   // seq -> fade lengths/tensions

    // fade drag state (which handle on which clip is being dragged)
    enum class FadeGrab { None, InLen, OutLen, InCurve, OutCurve };
    FadeGrab m_fade_grab = FadeGrab::None;
    int      m_fade_seq  = -1;
    // helpers: locate a clip's on-screen rect + fade hit-testing
    bool clip_rect_of(int seq, SDL_Rect& out) const;
    FadeGrab fade_at(int seq, const SDL_Rect& clip, int mx, int my) const;
    void draw_clip_fades(ui::App& app, int seq, const SDL_Rect& clip);
    void commit_fade(int seq);
    // Draw the clip waveform across [bx,bx+bw) using the sample sub-range
    // [s0,s1) (s1<0 == whole clip), so a trimmed trigger shows only its slice.
    void   draw_waveform(ui::App& app, const PatchKnob::engine::AudioClip* clip,
                         int bx, int y, int bw, int h,
                         long long s0 = 0, long long s1 = -1);
    bool   m_grow_dir   = false;   // true = drag start edge, false = end edge
    bool   m_adding     = false;   // placing+growing a fresh clip
    int    m_drop_seq   = -1;      // absolute sequence index under the press
    long   m_drop_tick  = 0;
    long   m_drop_offset = 0;      // click-tick - selected-trigger-edge

    // solo bookkeeping (ports perfnames::apply_solo)
    std::vector<char> m_solo;
    std::vector<char> m_mute_snapshot;
    bool   m_solo_active = false;

    // ---- Qtractor-inspired additions ---------------------------------------
    bool   m_follow   = false;   // follow-playhead auto-scroll (View>Follow)
    int    m_snap_idx = 2;       // index into the snap table (default 1/4 = beat)
    SDL_Rect m_snap_rect { 0, 0, 0, 0 };   // clickable SNAP readout (cycles snap)

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
    void apply_resize_cursor();            // SIZENS/SIZEWE/loop cursor per hover
    // Clip bottom-edge EXTEND drag (lengthen the region, loop-filling).
    bool m_extending      = false;
    int  m_extend_seq     = -1;

    // clip drag-copy: Ctrl+drag DUPLICATES the trigger instead of moving it.
    bool   m_copying     = false;
    long   m_copy_len    = 0;    // duplicated trigger length (ticks)
    long   m_copy_offset = 0;    // duplicated trigger loop-offset (ticks)
    long   m_ghost_tick  = 0;    // snapped drop position of the ghost
    bool   m_copy_events = true; // Ctrl+drag clones pattern data, not just ref

    struct ClipCopy {
        int seq = -1;
        long rel_start = 0;
        long length = 0;
        long offset = 0;
    };
    std::vector<ClipCopy> m_clip_clipboard;
    bool   m_lassoing = false;
    int    m_lasso_x0 = 0, m_lasso_y0 = 0;
    int    m_lasso_x1 = 0, m_lasso_y1 = 0;

    // inline clip rename (double-click a clip -> edit its sequence name)
    std::string m_edit_name;
    int      m_edit_seq  = -1;
    SDL_Rect m_edit_rect { 0, 0, 0, 0 };

    // ---- helpers -----------------------------------------------------------
    std::vector<int> active_list() const;            // active sequence indices
    int  lane_key(int seq) const;
    std::vector<int> lane_sequences(int lane_seq) const;
    int  clip_sequence_at(int lane_seq, long tick) const;
    int  canvas_x() const { return rect.x + header_w; }
    int  canvas_y() const { return rect.y + ruler_h; }
    int  canvas_w() const { return rect.w - header_w; }
    int  canvas_h() const { return rect.h - ruler_h; }

    int  tick_to_x(long tick) const;
    long x_to_tick(int px) const;
    long snap(long tick) const;
    int  row_at(int py) const;                       // on-screen row or -1
    long playhead() const;

    void draw_ruler   (ui::App& app, const std::vector<int>& act);
    void draw_canvas  (ui::App& app, const std::vector<int>& act);
    void draw_clips   (ui::App& app, int seq, int lane_y);
    void draw_headers (ui::App& app, const std::vector<int>& act);

    void press_canvas (ui::App& app, const ui::MouseEv& e, int seq);
    void drag_canvas  (ui::App& app, const ui::MouseEv& e);
    void apply_solo   ();

    // Qtractor-inspired helpers
    long        snap_value (int idx) const;          // ticks for a snap index
    const char* snap_label (int idx) const;          // ASCII name for the ruler
    void        zoom_to_fit();                        // fit 0..last-trigger to canvas
    void        begin_rename(ui::App& app, int seq, SDL_Rect clip_rect);
    void        draw_rename (ui::App& app);           // inline name editor overlay
    void        draw_loop_band(ui::App& app);         // shaded L..R span over lanes
    int         create_pattern(int source_seq, long start, long length,
                               long offset, bool copy_events);
    void        unselect_all_triggers();
    void        select_clips_in_rect(SDL_Rect box, bool add_to_selection);
    void        copy_selected_clips();
    void        paste_clips(long start_tick);
    void        delete_selected_clips();
    bool        any_selected_clip() const;
};

} // namespace arrange
#endif

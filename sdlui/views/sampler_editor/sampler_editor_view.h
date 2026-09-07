//----------------------------------------------------------------------------
//  sdlui/views/sampler_editor/sampler_editor_view.h
//
//  Editor for the native keyzone "Sampler" instrument node: a disk sample
//  BROWSER (load), multisample KEYRANGES (each loaded sample lives on a note
//  range, drawn on a keyboard strip), the selected zone's WAVEFORM, and the
//  instrument PARAMETER sliders (cutoff/res/pan/envelope params exposed by the
//  machine).  The shell owns the zone data (AudioClip copies) so it can re-push
//  them to the engine and persist them; this view edits that model.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLER_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_SAMPLER_EDITOR_VIEW_H

#include "gui.h"
#include "engine/audioclip/audio_clip.h"
#include "views/sample_slot/sample_slot_editor.h"

#include <functional>
#include <string>
#include <vector>
#include <set>
#include <map>

#include "engine/sf2/sf2_reader.h"

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace ui {

//! UI-side copy of one per-zone envelope stage set (mirrors the engine's
//! PatchKnob::engine::SamplerZoneEnv field for field).  Times in SECONDS,
//! sustain is a LEVEL 0..1.  `enabled == 0` means the zone has no envelope of
//! its own and FALLS BACK to the instrument-global envelope -- the default.
struct SamplerZoneEnvUI {
    float delay = 0.f, attack = 0.f, hold = 0.f, decay = 0.f;
    float sustain = 1.f, release = 0.f;
    int   enabled = 0;
};

//! One multisample zone: a sample that sounds over [loKey,hiKey], pitched from
//! rootNote.  The AudioClip copy is kept for display + re-push to the engine.
struct SamplerZone {
    PatchKnob::engine::AudioClip clip;
    int         root  = 60;      // MIDI root note
    int         loKey = 0;
    int         hiKey = 127;
    int         loVel = 0;
    int         hiVel = 127;
    bool        noteOffLayer = false;
    bool        keyToPitch = true;
    bool        velToVol = true;
    int         overlapMode = 0; // 0 play all, 1 cycle, 2 random
    float       gain = 1.0f;
    float       pan = 0.0f;
    float       start = 0.0f;    // normalized source trim
    float       end = 1.0f;
    float       loopStart = 0.0f;
    float       loopEnd = 1.0f;
    bool        loop = false;
    bool        reverse = false;
    std::vector<float> slices;
    bool        sliceGenerated = false; // derived region owned by preceding sliced zone
    int         sliceOrdinal = 0;       // 1..N; source zone is ordinal 0
    std::string name;
    // ---- per-zone engine parameters (SF2 parity) ---------------------------
    // Pushed through the audio_app_sampler_set_zone_* surface, NOT baked into
    // the uploaded PCM, so editing them never re-uploads the sample.
    SamplerZoneEnvUI ampEnv;             // override of the instrument amp env
    SamplerZoneEnvUI modEnv;             // per-zone modulation envelope
    float       cutoffHz = 0.f;          // low-pass cutoff; 0 = no filter
    float       resonanceDb = 0.f;
    int         coarseTune = 0;          // semitones
    int         fineTune = 0;            // cents
    int         scaleTuning = 100;       // cents per key; 0 = fixed pitch (drums)
    float       attenuationDb = 0.f;     // positive = quieter
    int         exclusiveClass = 0;      // 0 = none; non-zero chokes same class
    float       modEnvToPitchCents = 0.f;
    float       modEnvToFilterCents = 0.f;
};

//! A modulation-envelope node.  x/y are 0..1; `curve` is the tension of the
//! SEGMENT starting at this node (-1 concave .. 0 linear .. +1 convex).
struct EnvNode { float x = 0.f, y = 0.f, curve = 0.f; };
//! One envelope (a small ADSR-ish node list + which node is the sustain hold).
struct SamplerEnv {
    std::vector<EnvNode> nodes;
    int                  sustain = -1;   // node index held until note-off (-1 none)
    bool                 enabled = false;
};
//! The 5 envelopes a Buzz sampler machine reads: amp/pitch/cutoff/res/pan.
enum { ENV_AMP = 0, ENV_PITCH, ENV_CUTOFF, ENV_RES, ENV_PAN, ENV_COUNT };
struct SamplerEnvSet { SamplerEnv env[ENV_COUNT]; };

class SamplerEditorView : public Widget {
public:
    //! Bind to sampler node `node`, its zone list, envelopes, and instance.
    void bind(int node, std::vector<SamplerZone>* zones, SamplerEnvSet* envs,
              PatchKnob::engine::IPluginInstance* inst);
    int  node() const { return m_node; }

    //! Fired when an envelope (index 0..4) changes: the shell regenerates dense
    //! points from the curves and pushes them to the engine.
    std::function<void(int env)> on_env;

    //! Fired to load a sample into zone slot `level` (shell opens the file dialog,
    //! loads the WAV, fills zones[level], then calls on_apply).  level<0 == append.
    std::function<void(int level)> on_load;
    //! Internal disk browser: load `path` into a zone. level<0 appends; key>=0
    //! maps a new dropped zone to that keyboard key.
    std::function<void(const std::string& path, int level, int key)> on_load_path;
    //! Read a WAV for browser preview. Returns false on failure.
    //! Import one soundfont preset into THIS sampler.  The view has no sampler
    //! instance of its own, so the shell does the work and then re-binds us.
    std::function<void(const std::string& sf2Path, int bank, int program)> on_load_sf2_preset;

    std::function<bool(const std::string& path, PatchKnob::engine::AudioClip& clip)> on_preview_path;
    //! Audition the currently selected preview clip (browser click/up/down).
    std::function<void(const PatchKnob::engine::AudioClip& clip)> on_audition_clip;
    //! Fired after any zone edit (root / keyrange / add / remove): the shell
    //! re-pushes the whole zone set to the engine wavetable.
    std::function<void()>          on_apply;
    //! Fired after a per-zone PARAMETER edit (envelopes / filter / tuning /
    //! pan / attenuation / exclusive class / mod routing): the shell pushes
    //! just that zone's parameters through audio_app_sampler_set_zone_*.
    //! Cheap enough to fire on every drag motion for live feedback.
    std::function<void(int zoneIndex)> on_zone_params;
    //! Open the SHARED waveform editor (sampleslot::SampleSlotPanel) on a zone.
    //! Same editor the SMPL-1 rack module uses -- one system, two hosts.
    std::function<void(int zoneIndex)> on_edit_zone;
    std::function<void(int zoneIndex)> on_zone_selected;

    //! Index of the zone the editor is focused on.
    int selected_zone() const { return m_sel; }
    //! Replace the keyzone detail pane with the full WAV editor in THIS window.
    void edit_zone_inline(PatchKnob::engine::ISampleSlot* slot,
                          const std::string& title, double sampleRate);
    sampleslot::SampleSlotEditor& inline_editor() { return m_inlineEditor; }

    void draw   (App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key  (App& app, SDL_Keycode k) override;
    //! Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y from the toolkit's undo route.  Undoes
    //! the SAMPLE edit when the shared waveform pane has focus, otherwise the
    //! ZONE edit; false when both local histories are empty, so the shell's
    //! project undo still gets the key.
    bool on_undo (App& app, bool redo) override;
    //! One step along the ZONE history only (what the context menu's
    //! Undo/Redo entries mean).  False when that end is empty.
    bool zone_history(App& app, bool redo);
    //! Esc / focus loss abandons a drag rather than leaving the view convinced a
    //! button is still held (a button released outside never reports back).
    void cancel_interaction(App& app) override;

private:
    int  zone_at_key(int key) const;               // topmost zone covering `key`
    //! Topmost zone covering key AND velocity.  Picking by key alone made the
    //! upper layer of a velocity stack the only one you could ever select.
    int  zone_at(int key, int vel) const;
    //! One zone-history entry.  `metaOnly` entries carry every zone field
    //! EXCEPT the PCM (their clips are empty): mapping/parameter edits never
    //! touch audio, and deep-copying 2976 clips (hundreds of MB for a big
    //! soundfont) on every key-map press measured ~250 ms per click at a
    //! 286 MB model -- the single worst interaction cost in this view.
    //! Structural edits (add/remove/replace a zone's audio) still snapshot in
    //! full, so undo across them restores the audio too.
    struct ZoneHistEntry {
        std::vector<SamplerZone> zones;
        bool metaOnly = false;
    };
    int  key_at_x(int x, const SDL_Rect& strip) const;
    int  key_left_x(const SDL_Rect& strip, int key) const;
    int  key_right_x(const SDL_Rect& strip, int key) const;
    int  key_center_x(const SDL_Rect& strip, int key) const;
    void draw_wave(App& app, const SDL_Rect& r, const PatchKnob::engine::AudioClip& c);
    void select_only(int index);
    void sanitize_selection();
    std::vector<int> selected_indices() const;
    //! Push the current zone set onto the undo stack.  `metaOnly=true` is for
    //! edits that cannot touch any zone's PCM AND cannot change the zone
    //! count (mapping drags, inspector/envelope edits, rename, spread/layer);
    //! everything else -- add, delete, duplicate, paste, replace audio --
    //! must pass false so the audio itself is recoverable.
    void zone_snapshot(bool metaOnly = false);
    void zone_restore(ZoneHistEntry entry);
    //! Copy of `z` with the PCM left out (clip empty), in O(fields) not
    //! O(samples).  Used by metaOnly snapshots and the drag origin.
    static SamplerZone zone_meta_copy(SamplerZone& z);

    // ---- key map geometry --------------------------------------------------
    //  ONE definition of the grid rect and of the velocity axis, used by draw()
    //  AND by every hit test.  They used to be re-derived from the same magic
    //  numbers in four places and the two axes disagreed (draw divided velocity
    //  by 128, the drag by 127, the press hit-test by 128 with a different
    //  rounding), so the edge you grabbed was never the edge you saw.
    SDL_Rect keymap_grid() const;
    int  vel_to_y(const SDL_Rect& g, int vel) const;
    int  y_to_vel(const SDL_Rect& g, int y) const;
    //! Paint every zone rect/frame/root/label relative to `g`: culled to the
    //! visible key window, the unselected AND selected majorities batched into
    //! a few SDL_RenderFillRects calls; only the focused zone draws
    //! individually.  (A cached-texture layer was tried and removed: SDL's
    //! software backend renders text into a target texture pathologically
    //! slowly -- seconds per render -- while this direct paint measures
    //! 1.9-3.6 ms/frame at 2976 zones.)
    void draw_keymap_zones(App& app, const SDL_Rect& g);

    void draw_env(App& app, const SDL_Rect& r);
    //! The per-zone DAHDSR editor shown by the Z-AMP / Z-MOD tabs: stage
    //! sliders + override toggle on the right, the resulting curve on the left,
    //! with an explicit "using instrument default" state when not overridden.
    void draw_zone_env(App& app, const SDL_Rect& canvas);
    //! Per-zone inspector fields: normalized get/set + display formatting.
    //! One table drives the layout, the hit test and the value mapping.
    float insp_get(const SamplerZone& z, int field) const;
    void  insp_set(SamplerZone& z, int field, float v) const;
    void  insp_default(SamplerZone& z, int field) const;
    void  insp_format(const SamplerZone& z, int field, char* out, int cap) const;
    int   env_node_at(int x, int y, const SDL_Rect& r) const;   // node index or -1
    SDL_Point env_pt(const SDL_Rect& r, const EnvNode& n) const;
    //! Where the curve handle of segment `seg` is DRAWN -- and therefore where it
    //! is grabbed.  draw() used the stepped polyline midpoint and the hit test
    //! used the analytic one, so on long segments the handle sat next to the
    //! place that actually responded.
    SDL_Point env_curve_handle(const SDL_Rect& r, int seg) const;

    int   m_node = -1;
    std::vector<SamplerZone>*        m_zones = nullptr;
    SamplerEnvSet*                   m_envs  = nullptr;
    PatchKnob::engine::IPluginInstance*  m_inst  = nullptr;
    int   m_sel = 0;                               // selected zone index
    std::set<int> m_selected;                      // multi-selection; m_sel is focus
    std::vector<ZoneHistEntry> m_zone_undo, m_zone_redo;
    std::vector<SamplerZone> m_zone_clipboard;
    bool m_move_root = true;
    bool m_move_lock = false;
    bool m_solo_selection = false;
    bool m_show_overlaps = true;
    int   m_cur_env = 0;                           // which envelope is shown (0..4)

    // interaction
    struct BrowserItem {
        std::string name;        //!< display text (carries the +/- and indent)
        std::string path;        //!< the file/dir, or the .sf2 for a preset row
        bool        dir = false;
        //! A soundfont FILE row: expandable, never auditioned as audio.
        bool        isSoundFont = false;
        //! A PRESET row inside an expanded soundfont.
        bool        isPreset = false;
        int         bank = 0, program = 0;
    };

    void scan_browser();
    //! Expand / collapse a soundfont row.  Parsing is headers-only (~8 ms even
    //! on an 800 MB bank) and cached per path, so re-expanding costs nothing.
    void toggle_sf2(const std::string& path);
    std::set<std::string> m_sf2Open;
    std::map<std::string, PatchKnob::engine::sf2::SoundFont> m_sf2Cache;
    void select_browser(int index, bool audition = false);
    bool step_browser(int delta, bool audition);
    std::vector<std::string> drive_roots() const;

    enum class Drag { None, ZoneMove, ZoneLo, ZoneHi, ZoneVelLo, ZoneVelHi, ZoneRoot, EnvNode, EnvCurve, BrowserSample };

    // ---- drag & drop out of the sample library -----------------------------
    //  ONE description of what a drop at (x,y) would do -- the target, and the
    //  rect that says so on screen.  The highlight and the load used to be
    //  derived separately: the key column under the pointer lit up while the
    //  release appended a zone somewhere else, so the drop was always a guess.
    //  drop_target_at() is the single source both read.
    struct DropTarget {
        enum Kind { None = 0, ReplaceZone, NewAtKey, AppendZone };
        Kind     kind = None;
        int      zone = -1;               //!< zone whose SAMPLE is replaced
        int      key  = -1;               //!< key a new zone is mapped to
        SDL_Rect hi{ 0, 0, 0, 0 };        //!< what lights up (same geometry)
    };
    DropTarget drop_target_at(int x, int y) const;
    //! Perform a drop.  Snapshots first, so it is undoable with Ctrl+Z, and
    //! replaces ONLY the audio of an existing zone -- its key range, root,
    //! velocity band and playback settings are the mapping and must survive.
    void       drop_sample(App& app, const std::string& path, const DropTarget& tgt);
    //! Drop target highlight + the carried file's ghost, painted LAST so the
    //! waveform editor cannot bury them at the moment the pointer crosses it.
    void       draw_drag_ghost(App& app);
    void       set_drop_status(const std::string& msg);
    //! Which handle of zone `zi` a press at (x,y) should grab -- NEAREST wins and
    //! a zone narrower than the grab zones stays movable.  First-match meant a
    //! one-key drum zone was permanently "drag its low edge": its lo and hi edges
    //! are the same 6 px, so the velocity edges and the body were unreachable.
    Drag  handle_at(const SDL_Rect& g, int zi, int x, int y) const;
    Drag  m_drag = Drag::None;
    int   m_param_drag = -1;                       // param slider being dragged
    int   m_env_node  = -1;                        // envelope node being dragged
    int   m_env_seg   = -1;                        // segment whose curve is dragged
    Uint64 m_env_last_click_ms = 0;                 // empty-canvas double click
    SDL_Point m_env_last_click{-10000,-10000};
    int   m_zoneDragKeyOffset = 0;
    int   m_zoneDragRootOffset = 0;
    //! Pre-drag zone state, METADATA ONLY (clips empty): a key-map drag never
    //! touches PCM, and the full copy this used to hold cost another ~125 ms
    //! per press on a large soundfont on top of the undo snapshot.
    std::vector<SamplerZone> m_drag_origin;
    bool  m_zone_marquee=false;
    SDL_Point m_marquee_start{0,0};
    SDL_Rect m_marquee_rect{0,0,0,0};
    int m_key_first=0, m_key_visible=128;
    bool m_key_scroll_drag=false;
    int m_key_scroll_drag_x=0, m_key_scroll_drag_first=0;
    int   m_mx = -1, m_my = -1;
    bool  m_leftDown = false, m_rightDown = false;

    SDL_Rect m_env_rect{0,0,0,0};                  // envelope canvas
    SDL_Rect m_env_tabs[ENV_COUNT + 2];            // 5 instrument envs + Z-AMP / Z-MOD
    // ---- per-zone envelope editor (Z-AMP / Z-MOD tabs) ---------------------
    SDL_Rect m_zenv_rows[6]{};                     // stage slider tracks
    SDL_Rect m_zenv_toggle{0,0,0,0};               // default <-> override button
    int      m_zenv_drag = -1;                     // stage being dragged (-1 none)
    // ---- per-zone parameter inspector --------------------------------------
    std::vector<SDL_Rect> m_insp_rows;             // slider tracks, one per field
    int      m_insp_drag = -1;                     // field being dragged (-1 none)
    SDL_Rect m_btn_zloop{0,0,0,0};                 // loop mode toggle in inspector

    // cached hit rects (recomputed each draw)
    SDL_Rect m_btn_load{0,0,0,0};
    SDL_Rect m_btn_add {0,0,0,0};
    SDL_Rect m_btn_del {0,0,0,0};
    SDL_Rect m_btn_rev {0,0,0,0};
    SDL_Rect m_btn_norm{0,0,0,0};
    SDL_Rect m_btn_loop{0,0,0,0};
    SDL_Rect m_btn_crop{0,0,0,0};
    SDL_Rect m_btn_fadein{0,0,0,0};
    SDL_Rect m_btn_fadeout{0,0,0,0};
    SDL_Rect m_btn_dc{0,0,0,0};
    SDL_Rect m_btn_zerotrim{0,0,0,0};
    SDL_Rect m_btn_note_on{0,0,0,0};
    SDL_Rect m_btn_note_off{0,0,0,0};
    SDL_Rect m_btn_drumkit{0,0,0,0};
    SDL_Rect m_btn_distribute{0,0,0,0};
    SDL_Rect m_btn_layer{0,0,0,0};
    SDL_Rect m_btn_keypitch{0,0,0,0};
    SDL_Rect m_btn_velvol{0,0,0,0};
    SDL_Rect m_btn_overlap{0,0,0,0};
    SDL_Rect m_btn_lo_dec{0,0,0,0};
    SDL_Rect m_btn_lo_inc{0,0,0,0};
    SDL_Rect m_btn_hi_dec{0,0,0,0};
    SDL_Rect m_btn_hi_inc{0,0,0,0};
    SDL_Rect m_btn_root_dec{0,0,0,0};
    SDL_Rect m_btn_root_inc{0,0,0,0};
    SDL_Rect m_btn_vel_lo_dec{0,0,0,0};
    SDL_Rect m_btn_vel_lo_inc{0,0,0,0};
    SDL_Rect m_btn_vel_hi_dec{0,0,0,0};
    SDL_Rect m_btn_vel_hi_inc{0,0,0,0};
    SDL_Rect m_zone_mapping_strip{0,0,0,0};
    SDL_Rect m_keyzone_grid{0,0,0,0};
    //! Font cell the key map was last LAID OUT with, so keymap_grid() reproduces
    //! the drawn rect exactly even when a hit test runs with a different font.
    int      m_keymap_ch = 12;
    SDL_Rect m_strip   {0,0,0,0};                  // keyboard keyrange strip
    SDL_Rect m_key_scroll_track{0,0,0,0};
    SDL_Rect m_key_scroll_thumb{0,0,0,0};
    SDL_Rect m_wave    {0,0,0,0};
    std::vector<SDL_Rect> m_zone_rows;             // left zone-list rows
    std::vector<int> m_zone_row_indices;
    //! The zone-list PANE.  A drop in the empty space under the last row lands
    //! here and appends a zone; it used to fall through and do nothing at all.
    SDL_Rect m_zone_list_rect{0,0,0,0};
    int m_zone_scroll = 0;
    std::vector<SDL_Rect> m_param_rows;            // param slider rows
    std::vector<SDL_Rect> m_browser_rows;
    SDL_Rect m_browser_list_rect{0,0,0,0};

    std::string m_browser_dir;
    //! Why the list looks the way it does ("no WAV files here" / a read error).
    //! An unreadable folder and an empty one used to paint the same blank pane.
    std::string m_browser_msg;
    std::vector<BrowserItem> m_browser;
    int         m_browser_sel = -1;
    int         m_browser_scroll = 0;
    int         m_browser_visible = 1;
    bool        m_browser_scroll_drag = false;
    int         m_browser_scroll_drag_y = 0, m_browser_scroll_drag_start = 0;
    SDL_Rect    m_browser_scroll_track{0,0,0,0}, m_browser_scroll_thumb{0,0,0,0};
    bool        m_browser_active = false;
    bool        m_drive_menu = false;
    SDL_Rect    m_drive_rect{0,0,0,0};
    std::vector<SDL_Rect> m_drive_rows;
    //! The drive strings that were actually DRAWN alongside m_drive_rows, so a
    //! click resolves against the same list the user saw.
    std::vector<std::string> m_drive_labels;
    std::string m_drag_path;
    std::string m_drag_name;            //!< leaf shown in the drag ghost
    DropTarget  m_drag_target;          //!< what the release would do, right now
    //! A press only BECOMES a drag past a threshold, so an ordinary click on a
    //! library row does not flash a ghost and cannot drop anything.
    bool        m_drag_armed = false;
    int         m_drag_press_x = 0, m_drag_press_y = 0;
    PatchKnob::engine::AudioClip m_preview_clip;
    //! Path m_preview_clip was decoded from.  A drop of THAT file reuses the
    //! decode instead of stalling the UI thread reading the same WAV twice.
    std::string m_preview_path;
    bool        m_preview_ok = false;
    //! Result of the last drop ("loaded kick.wav into zone 3"), shown in the
    //! toolbar for a few seconds.  It is a status, not a log.
    std::string m_drop_status;
    Uint32      m_drop_status_ms = 0;
    SDL_Rect    m_browser_rect{0,0,0,0};
    SDL_Rect    m_preview_rect{0,0,0,0};
    int         m_scroll_y = 0;
    int         m_scroll_max = 0;
    bool        m_scroll_drag = false;
    int         m_scroll_drag_y = 0;
    int         m_scroll_drag_start = 0;
    SDL_Rect    m_scroll_track{0,0,0,0};
    SDL_Rect    m_scroll_thumb{0,0,0,0};
    sampleslot::SampleSlotEditor m_inlineEditor;
    bool        m_inlineEdit = false;
    SDL_Rect    m_inlineRect{0,0,0,0};

    // ---- hover / tooltips ---------------------------------------------------
    //  SDL only delivers motion events while a button is HELD, so a view that
    //  tracks the pointer from on_mouse alone highlights whatever was last
    //  clicked.  draw() polls the real position instead.
    bool        m_hover_in = false;
    const char* m_tip = nullptr;
    SDL_Rect    m_tip_anchor{0,0,0,0};
    //! Which pane owns the keyboard: keys reach the shared waveform editor only
    //! when the last press landed on it, so Ctrl+Z means "undo the sample edit"
    //! there and "undo the zone edit" everywhere else.
    bool        m_wave_focus = false;

    // ---- browser preview: cached peaks + audition ---------------------------
    //  The preview used to reduce the WHOLE clip once per PIXEL COLUMN on every
    //  repaint -- a 30-second WAV re-read 1.4 M floats each time the pointer
    //  moved.  Built once per previewed file instead.
    struct PeakMip {
        const float* key = nullptr;      // identity of the buffer it was built from
        int64_t frames = 0;
        int     bucket = 256;
        std::vector<float> mn, mx, rms;
    };
    PeakMip     m_preview_peaks;
    //! Whether the preview file is mono, decided ONCE per previewed file.
    //! draw() used to answer this by comparing the two channel vectors every
    //! frame -- a full memcmp of the sample (measured ~28 ms/frame on a
    //! 10-minute WAV), which alone kept the UI from ever reaching 60 fps.
    bool        m_preview_mono = true;
    std::vector<SDL_Rect> m_col_peak, m_col_rms;
    void        rebuild_preview_peaks();
    void        preview_column(const PatchKnob::engine::AudioClip& c,
                               int64_t s0, int64_t s1, float& mn, float& mx, float& rms) const;
    //! Audition position, driven from the wall clock: the engine's one-shot
    //! preview exposes no play position, but a cursor that tracks it is the
    //! difference between "did that play?" and seeing what played.
    bool        m_prev_playing = false;
    Uint32      m_prev_ms0 = 0;
    int64_t     m_prev_from = 0;
    int         m_prev_last_x = -1;
    void        audition_preview(App& app, int64_t fromFrame);

    // ---- right-click menu ---------------------------------------------------
    //  House style: one-shot ACTIONS live in a context menu.  The key map and the
    //  zone list had none at all, and the commands that used to be buttons were
    //  laid out past the right edge of the clipped viewport -- painted nowhere
    //  and clickable nowhere.
    struct MenuRow { std::string label; int id; bool enabled; bool separator; };
    std::vector<MenuRow>  m_menu;
    std::vector<SDL_Rect> m_menu_rows;
    bool        m_menu_open = false;
    SDL_Rect    m_menu_box{0,0,0,0};
    int         m_menu_zone = -1;       // zone the menu was opened on (-1 none)
    int         m_menu_key  = -1;       // key map column it was opened on
    int         m_menu_browser = -1;    // browser row it was opened on
    void        open_menu(App& app, int x, int y, int zone, int key, int browserRow);
    void        draw_menu(App& app);
    bool        menu_click(App& app, int x, int y);
    void        run_menu(App& app, int id);
    std::string m_rename_buf;           // inline zone rename (app.begin_text)
    int         m_rename_zone = -1;
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_SAMPLER_EDITOR_VIEW_H

//----------------------------------------------------------------------------
//  sdlui/views/sampler_editor/sampler_editor_view.h
//
//  Editor for a Buzz/Unwieldy-backed "Sampler" instrument node: a disk sample
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

#include <functional>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace ui {

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
    bool        loop = false;
    bool        reverse = false;
    std::string name;
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
    std::function<bool(const std::string& path, PatchKnob::engine::AudioClip& clip)> on_preview_path;
    //! Audition the currently selected preview clip (browser click/up/down).
    std::function<void(const PatchKnob::engine::AudioClip& clip)> on_audition_clip;
    //! Fired after any zone edit (root / keyrange / add / remove): the shell
    //! re-pushes the whole zone set to the engine wavetable.
    std::function<void()>          on_apply;

    void draw   (App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key  (App& app, SDL_Keycode k) override;

private:
    int  zone_at_key(int key) const;               // topmost zone covering `key`
    int  key_at_x(int x, const SDL_Rect& strip) const;
    void draw_wave(App& app, const SDL_Rect& r, const PatchKnob::engine::AudioClip& c);

    void draw_env(App& app, const SDL_Rect& r);
    int   env_node_at(int x, int y, const SDL_Rect& r) const;   // node index or -1
    SDL_Point env_pt(const SDL_Rect& r, const EnvNode& n) const;

    int   m_node = -1;
    std::vector<SamplerZone>*        m_zones = nullptr;
    SamplerEnvSet*                   m_envs  = nullptr;
    PatchKnob::engine::IPluginInstance*  m_inst  = nullptr;
    int   m_sel = 0;                               // selected zone index
    int   m_cur_env = 0;                           // which envelope is shown (0..4)

    // interaction
    struct BrowserItem {
        std::string name;
        std::string path;
        bool        dir = false;
    };

    void scan_browser();
    void select_browser(int index, bool audition = false);
    bool step_browser(int delta, bool audition);
    std::vector<std::string> drive_roots() const;

    enum class Drag { None, ZoneMove, ZoneLo, ZoneHi, ZoneVelLo, ZoneVelHi, ZoneRoot, ZoneStart, ZoneEnd, EnvNode, EnvCurve, BrowserSample };
    Drag  m_drag = Drag::None;
    int   m_param_drag = -1;                       // param slider being dragged
    int   m_env_node  = -1;                        // envelope node being dragged
    int   m_env_seg   = -1;                        // segment whose curve is dragged
    int   m_mx = -1, m_my = -1;

    SDL_Rect m_env_rect{0,0,0,0};                  // envelope canvas
    SDL_Rect m_env_tabs[ENV_COUNT];                // env selector buttons

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
    SDL_Rect m_keyzone_grid{0,0,0,0};
    SDL_Rect m_strip   {0,0,0,0};                  // keyboard keyrange strip
    SDL_Rect m_wave    {0,0,0,0};
    std::vector<SDL_Rect> m_zone_rows;             // left zone-list rows
    std::vector<SDL_Rect> m_param_rows;            // param slider rows
    std::vector<SDL_Rect> m_browser_rows;

    std::string m_browser_dir;
    std::vector<BrowserItem> m_browser;
    int         m_browser_sel = -1;
    int         m_browser_scroll = 0;
    bool        m_browser_active = false;
    bool        m_drive_menu = false;
    SDL_Rect    m_drive_rect{0,0,0,0};
    std::vector<SDL_Rect> m_drive_rows;
    std::string m_drag_path;
    int         m_drag_hover_key = -1;
    PatchKnob::engine::AudioClip m_preview_clip;
    bool        m_preview_ok = false;
    SDL_Rect    m_browser_rect{0,0,0,0};
    SDL_Rect    m_preview_rect{0,0,0,0};
    int         m_scroll_y = 0;
    int         m_scroll_max = 0;
    bool        m_scroll_drag = false;
    int         m_scroll_drag_y = 0;
    int         m_scroll_drag_start = 0;
    SDL_Rect    m_scroll_track{0,0,0,0};
    SDL_Rect    m_scroll_thumb{0,0,0,0};
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_SAMPLER_EDITOR_VIEW_H

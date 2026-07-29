//----------------------------------------------------------------------------
//  sdlui/views/mixer/mixer_view.h -- SDL2 port of the PatchKnob master mixer.
//
//  MixerView is a single retained ui::Widget drawn over a live MixerGraph*.
//  It renders one channel strip per track (name / instrument, a stereo VU with
//  peak-hold, a gain Fader, a pan control, Mute/Solo buttons, and the insert-FX
//  list) plus a MASTER strip on the right (master stereo VU + master gain fader).
//
//  All model reads are lock-free atomic loads, so draw() may be called from the
//  UI thread while the audio thread pushes VU levels + the render loop runs.
//
//  Mounting (shell side):
//      mixer::MixerView mv(graph);          // graph = audio_app_graph()
//      app.roots.push_back(&mv);
//      app.on_layout = [&](ui::App& a){ mv.rect = { x, y, w, h }; };
//      // drive ~30 Hz redraws (a timer / the audio render loop) via
//      // a.request_redraw(); MixerView re-polls every VU meter each draw.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_MIXER_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_MIXER_VIEW_H

#include "gui.h"
#include <functional>
#include <memory>
#include <vector>

namespace PatchKnob { namespace engine { class MixerGraph; class Track; } }

namespace mixer {

// Fader throw maps linearly onto [0 .. kMaxGain]; unity (1.0) sits at 0.5.
static constexpr float kMaxGain = 2.0f;   // +6 dB at the top of the fader

// ---- a small horizontal pan control (-1 .. +1), themed two-tone ------------
class PanSlider : public ui::Widget {
public:
    float value = 0.0f;                       // -1 (L) .. +1 (R)
    std::function<void(float)> changed;
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
private:
    void apply(ui::App& app, int px);
};

// ---- the mixer view --------------------------------------------------------
class MixerView : public ui::Widget {
public:
    explicit MixerView(PatchKnob::engine::MixerGraph* graph);

    // Rebind to a (new) graph and rebuild the per-track strip widgets.
    void set_graph(PatchKnob::engine::MixerGraph* graph);

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;

private:
    struct Strip {
        std::unique_ptr<ui::Fader> fader;
        std::unique_ptr<PanSlider> pan;
        std::unique_ptr<ui::Button> mute;
        std::unique_ptr<ui::Button> solo;
        float holdL = 0.0f, holdR = 0.0f;     // peak-hold state (per meter)
    };

    void build();
    void layout(ui::App& app);                // recompute all sub-rects
    void draw_strip(ui::App& app, int i, const SDL_Rect& area);
    void draw_master(ui::App& app, const SDL_Rect& area);
    void draw_vu(ui::App& app, const SDL_Rect& bar, float level, float& hold);

    PatchKnob::engine::MixerGraph* graph_ = nullptr;
    std::vector<Strip> strips_;               // one per track
    std::unique_ptr<ui::Fader> masterFader_;
    float masterHoldL_ = 0.0f, masterHoldR_ = 0.0f;

    std::vector<ui::Widget*> children_;        // flat dispatch list (non-owning)
    ui::Widget* capture_ = nullptr;            // widget receiving the drag

    int strip_w_ = 96;
    int gap_     = 6;
};

} // namespace mixer

#endif // PATCHKNOB_SDLUI_VIEWS_MIXER_VIEW_H

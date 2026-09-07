//----------------------------------------------------------------------------
//  sdlui/views/master_mixer/master_mixer_view.cpp -- see master_mixer_view.h.
//
//  An Ardour mixer strip per engine channel.  Behaviour ported from:
//    gtk2_ardour/mixer_strip.cc    strip composition + name/input/output/comment
//    gtk2_ardour/processor_box.cc  the insert chain (add/remove/reorder/bypass,
//                                  pre- vs post-fader, context menu)
//    gtk2_ardour/gain_meter.cc     fader/meter layout, dB entry, peak readout
//    gtk2_ardour/route_ui.cc       mute/solo state semantics + isolate/safe
//    libs/pbd/pbd/control_math.h   the gain <-> fader-position taper
//    libs/ardour/ardour/dB.h       coefficient <-> dB
//    libs/ardour/ardour/logmeter.h meter deflection (fallback meter only)
//----------------------------------------------------------------------------
#include "master_mixer_view.h"

#include "audio_app.h"
#include "engine/plugin_api.h"
#include "engine/host/plugin_host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ui::App;
using ui::Color;
using ui::MouseEv;
using ui::Theme;
using ui::theme;
namespace app_ = PatchKnob::app;

namespace {

const double PI = 3.14159265358979323846;

inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int   imax(int a, int b) { return a > b ? a : b; }
inline int   imin(int a, int b) { return a < b ? a : b; }

inline bool in_rect(const SDL_Rect& q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}
inline bool near_pt(int x, int y, int cx, int cy, int rad) {
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

// Clip to `cells` monospace glyphs.  Ardour ellipsizes (Pango::ELLIPSIZE_END);
// the fixed atlas has no ellipsis glyph, so a '~' marks the truncation.
std::string clip_cells(const std::string& s, int cells) {
    if (cells <= 0) return std::string();
    if ((int)s.size() <= cells) return s;
    if (cells <= 1) return s.substr(0, (size_t)cells);
    return s.substr(0, (size_t)cells - 1) + "~";
}

void fill_disc(SDL_Renderer* r, int cx, int cy, int rad, Color c) {
    if (rad < 1) rad = 1;
    ui::set_color(r, c);
    for (int dy = -rad; dy <= rad; ++dy) {
        const int dx = int(std::floor(std::sqrt(double(rad * rad - dy * dy))));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}
void circle_ring(SDL_Renderer* r, int cx, int cy, int rad, Color c) {
    ui::set_color(r, c);
    for (int rr = rad; rr >= rad - 1 && rr > 0; --rr) {
        int x = rr, y = 0, err = 1 - rr;
        while (x >= y) {
            SDL_RenderDrawPoint(r, cx + x, cy + y); SDL_RenderDrawPoint(r, cx + y, cy + x);
            SDL_RenderDrawPoint(r, cx - y, cy + x); SDL_RenderDrawPoint(r, cx - x, cy + y);
            SDL_RenderDrawPoint(r, cx - x, cy - y); SDL_RenderDrawPoint(r, cx - y, cy - x);
            SDL_RenderDrawPoint(r, cx + y, cy - x); SDL_RenderDrawPoint(r, cx + x, cy - y);
            ++y;
            if (err < 0) err += 2 * y + 1; else { --x; err += 2 * (y - x) + 1; }
        }
    }
}

const int KNOB_R       = 8;
const int PAN_BAR_H    = 9;
const int KNOB_DRAG_PX = 110;
const int WHEEL_STEP   = 24;
const int MENU_PAD     = 4;

bool engine_up() { return app_::audio_app_running(); }

// ---- cached effect inventory (PluginHost's scan cache, no rescan) -----------
std::vector<PatchKnob::engine::PluginDescriptor>& fx_inventory() {
    static std::vector<PatchKnob::engine::PluginDescriptor> cache;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (PatchKnob::engine::PluginHost* h = app_::audio_app_host()) {
            std::vector<PatchKnob::engine::PluginDescriptor> all;
            if (h->loadCache(all))
                for (size_t i = 0; i < all.size(); ++i)
                    if (!all[i].isInstrument) cache.push_back(all[i]);
        }
    }
    return cache;
}

std::string node_name(int nodeId) {
    char buf[96] = {0};
    app_::audio_app_patch_node_name(nodeId, buf, (int)sizeof(buf));
    return std::string(buf);
}

} // anonymous namespace

namespace mixerui {

// ============================================================================
//  Ardour gain law.  pbd/control_math.h maps a [0..2] gain coefficient onto a
//  [0..1] fader position with an 8th-power curve, so unity lands ~78% up the
//  throw and the top of the fader is +6 dB.  ardour/dB.h does the dB math.
// ============================================================================
double MasterMixerView::gain_to_position(double g) {
    if (g <= 0.0) return 0.0;
    return std::pow((6.0 * std::log(g) / std::log(2.0) + 192.0) / 198.0, 8.0);
}
double MasterMixerView::position_to_gain(double pos) {
    if (pos <= 0.0) return 0.0;
    return std::exp(((std::pow(pos, 1.0 / 8.0) * 198.0) - 192.0) / 6.0 * std::log(2.0));
}
// ARDOUR::gain_to_slider_position_with_max / slider_position_to_gain_with_max
float MasterMixerView::gain_to_slider(float gain) {
    return (float)clampf((float)gain_to_position((double)gain * 2.0 / (double)kMaxGain), 0.f, 1.f);
}
float MasterMixerView::slider_to_gain(float pos) {
    return (float)(position_to_gain((double)clamp01(pos)) * (double)kMaxGain / 2.0);
}
float MasterMixerView::coefficient_to_dB(float c) {
    return c < 1e-15f ? -318.f : 20.f * std::log10(c);
}
float MasterMixerView::dB_to_coefficient(float db) {
    return db > -318.8f ? std::pow(10.f, db * 0.05f) : 0.f;
}

// ============================================================================
//  Model plumbing
// ============================================================================
int MasterMixerView::node() const {
    if (get_node) return get_node();
    return app_::audio_app_master_mixer_node();
}

// The `track` the insert API wants for this strip.  Tracks pass their index,
// the MASTER passes -1, and an AUX strip passes its STRIP CODE (-2 - aux, via
// audio_app_master_aux_strip) -- audio_app.h defines that code precisely so a
// bus's processor box can BE the ordinary insert UI over the ordinary API,
// with no second chain implementation anywhere.
int MasterMixerView::chain_id(int idx) const {
    if (idx >= 0 && idx < bus_count) return idx;
    if (is_aux(idx)) return app_::audio_app_master_aux_strip(aux_of(idx));
    return -1;   // the MASTER strip
}

MasterMixerView::StripState& MasterMixerView::state(int idx) {
    if (idx < 0) idx = 0;
    if ((int)m_strips.size() <= idx) m_strips.resize((size_t)idx + 1);
    return m_strips[(size_t)idx];
}

void MasterMixerView::sync_model(App& app) {
    if (get_bus_count) { const int n = get_bus_count(); bus_count = n < 0 ? 0 : n; }
    if (get_aux_count) {
        const int n = get_aux_count();
        m_aux_supported = n >= 0;
        aux_count = n > 0 ? n : 0;
    } else {
        m_aux_supported = false;
    }

    // Aux strips sit BETWEEN the tracks and the MASTER, so any structural
    // change (track or bus added/removed) shifts every strip index at or above
    // the change.  StripState is positional -- meters, self-mute, comments,
    // the local rename -- and letting the old MASTER's state land on a freshly
    // inserted aux strip would hand the new bus the master's mute and
    // peak-hold.  Reset the shifted region and reseed, and drop anything else
    // that holds a strip INDEX into it (selection, drag, menu, inline edit --
    // the edit must end via end_text_if, or app.text_target keeps the whole
    // keyboard aimed at a strip that no longer means what it did).
    const int auxN = live_aux_count();
    if (bus_count != m_last_bus || auxN != m_last_aux) {
        if (m_last_bus >= 0) {
            const int from = imin(bus_count, m_last_bus);
            for (int i = from; i < (int)m_strips.size(); ++i)
                m_strips[(size_t)i] = StripState();
            clear_proc_selection();
            if (m_edit_kind != E_NONE && m_edit_strip >= from) {
                app.end_text_if(&m_edit_buf);
                m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
            }
            if (m_drag != D_NONE && m_drag_strip >= from) {
                m_drag = D_NONE; m_drag_strip = -1; m_drag_aux = -1;
                m_drag_slot = -1; m_drag_moved = false;
            }
            if (m_menu.kind != M_NONE && m_menu.strip >= from) m_menu = Menu();
        }
        m_last_bus = bus_count; m_last_aux = auxN;
    }

    const int want = strip_count();
    if ((int)m_strips.size() != want) m_strips.resize((size_t)want);
    for (int i = 0; i < want; ++i) seed_strip(i);
}

// The strip's own mute/solo model is authoritative (Ardour keeps self-mute and
// muted-by-others as distinct states; the engine only stores the RESOLVED mute),
// so the self state is seeded once from whatever the engine came up with.
void MasterMixerView::seed_strip(int idx) {
    StripState& s = state(idx);
    if (s.seeded) return;
    s.seeded = true;
    // Aux strips keep no mute model of their own: a bus has no solo interplay
    // to resolve, so its mute is read from / written to the engine directly.
    if (is_aux(idx)) return;
    const bool master = is_master(idx);
    if (engine_up()) {
        s.selfMute = master ? app_::audio_app_mixer_master_mute(node())
                            : app_::audio_app_mixer_mute(node(), idx);
    } else if (get_mute) {
        s.selfMute = get_mute(idx);
    }
    if (get_solo) s.selfSolo = get_solo(idx);
}

bool MasterMixerView::any_solo() const {
    for (int i = 0; i < bus_count && i < (int)m_strips.size(); ++i)
        if (m_strips[(size_t)i].selfSolo) return true;
    return false;
}

// route_ui.cc RouteUI::mute_active_state: a strip that is not itself muted but
// is silenced because something else is soloed reads as IMPLICITLY active.
bool MasterMixerView::muted_by_others(int idx) const {
    if (idx >= bus_count) return false;
    if (idx >= (int)m_strips.size()) return false;
    const StripState& s = m_strips[(size_t)idx];
    return any_solo() && !s.selfSolo && !s.soloIso;
}

void MasterMixerView::apply_mutes() {
    const bool anySolo = any_solo();
    for (int i = 0; i < bus_count && i < (int)m_strips.size(); ++i) {
        const StripState& s = m_strips[(size_t)i];
        const bool resolved = s.selfMute || (anySolo && !s.selfSolo && !s.soloIso);
        if (engine_up()) app_::audio_app_mixer_set_mute(node(), i, resolved);
        else if (toggle_mute && get_mute && get_mute(i) != resolved) toggle_mute(i);
    }
    const int m = master_index();
    if (m < (int)m_strips.size() && engine_up())
        app_::audio_app_mixer_set_master_mute(node(), m_strips[(size_t)m].selfMute);
}

// ============================================================================
//  Geometry
// ============================================================================
int MasterMixerView::strip_x(int idx) const {
    int x = rect.x + PAD - m_scroll_x + idx * (STRIP_W + GAP);
    // the add-bus column sits between the last aux return and the MASTER
    if (aux_ui() && idx >= master_index()) x += AUX_ADD_W + GAP;
    return x;
}

// The slim "+ AUX" column: the one place a bus is created, so it lives right
// where the new strip will appear (between the returns and the MASTER).
SDL_Rect MasterMixerView::add_bus_rect() const {
    SDL_Rect a;
    a.x = rect.x + PAD - m_scroll_x + master_index() * (STRIP_W + GAP);
    a.y = rect.y + PAD;
    a.w = AUX_ADD_W;
    a.h = imax(60, rect.h - 2 * PAD);
    return a;
}

SDL_Rect MasterMixerView::strip_area(int idx) const {
    SDL_Rect a;
    a.x = strip_x(idx);
    a.y = rect.y + PAD;
    a.w = is_master(idx) ? MASTER_W : STRIP_W;
    a.h = imax(60, rect.h - 2 * PAD);
    return a;
}

int MasterMixerView::content_w() const {
    const int bc = bus_count < 0 ? 0 : bus_count;
    return 2 * PAD + (bc + live_aux_count()) * (STRIP_W + GAP)
         + (aux_ui() ? AUX_ADD_W + GAP : 0) + MASTER_W;
}

void MasterMixerView::clamp_scroll() {
    int maxsx = content_w() - rect.w;
    if (maxsx < 0) maxsx = 0;
    if (m_scroll_x > maxsx) m_scroll_x = maxsx;
    if (m_scroll_x < 0)     m_scroll_x = 0;
}

// One place computes every sub-rect so draw() and hit-testing can never drift.
// Vertical order matches MixerStrip::global_vpacker (mixer_strip.cc:335-350).
MasterMixerView::Sub MasterMixerView::layout(App& app, int idx, const SDL_Rect& area) const {
    Sub L;
    L.area = area;
    // An aux-return strip drops the blocks that would be dead controls on a
    // bus: no pan (the return has no pan stage), no solo (nothing to spotlight
    // it against -- MUTE takes the whole row instead), and no send knobs (the
    // send API is track-addressed; a bus sending to itself is a feedback
    // loop).  The dropped rects stay zeroed, so every in_rect() test on them
    // is simply never true.
    const bool auxStrip   = is_aux(idx);
    const bool trackStrip = idx >= 0 && idx < bus_count;

    const int ip  = 5;
    const int ix  = area.x + ip;
    int       iw  = area.w - 2 * ip; if (iw < 8) iw = 8;
    const int fch = app.font.ch();
    const int mch = app.mono.ch();
    const int btn = mch + 5;

    int y = area.y + 2;

    // 1. name header
    L.header = SDL_Rect{ area.x + 1, y, area.w - 2, fch + 5 };
    y += L.header.h + 3;

    // 2. input button
    L.input = SDL_Rect{ ix, y, iw, btn };
    y += btn + 3;

    // --- bottom-anchored block, so the processor box absorbs the slack -------
    int bottom = area.y + area.h - 3;

    L.comment = SDL_Rect{ ix, bottom - btn, iw, btn };  bottom -= btn + 3;
    L.output  = SDL_Rect{ ix, bottom - btn, iw, btn };  bottom -= btn + 3;

    const int entryH = mch + 4;
    L.db   = SDL_Rect{ ix, bottom - entryH, (iw - 3) / 2, entryH };
    L.peak = SDL_Rect{ ix + (iw - 3) / 2 + 3, bottom - entryH, iw - (iw - 3) / 2 - 3, entryH };
    bottom -= entryH + 3;

    // gain block: dB ruler | stereo meter | fader
    const int scaleW = 17;
    const int meterW = 13;
    const int faderW = imax(16, iw - scaleW - meterW - 4);
    const int gainH  = imax(60, imin(230, (area.h * 42) / 100));
    const int gainTop = bottom - gainH;
    L.scale = SDL_Rect{ ix, gainTop, scaleW, gainH };
    L.meter = SDL_Rect{ ix + scaleW + 2, gainTop, meterW, gainH };
    L.fader = SDL_Rect{ ix + scaleW + meterW + 4, gainTop, faderW, gainH };
    bottom = gainTop - 3;

    // mute / solo (+ isolate LED)
    const int msH  = mch + 6;
    if (auxStrip) {
        L.mute = SDL_Rect{ ix, bottom - msH, iw, msH };
    } else {
        const int isoW = 12;
        const int halfW = (iw - isoW - 6) / 2;
        L.mute = SDL_Rect{ ix, bottom - msH, halfW, msH };
        L.solo = SDL_Rect{ ix + halfW + 3, bottom - msH, halfW, msH };
        L.iso  = SDL_Rect{ ix + iw - isoW, bottom - msH + (msH - isoW) / 2, isoW, isoW };
    }
    bottom -= msH + 3;

    // pan (label + readout line, then the slider bar)
    if (!auxStrip) {
        const int panH = mch + 2 + PAN_BAR_H;
        L.pan     = SDL_Rect{ ix, bottom - panH, iw, panH };
        L.pan_bar = SDL_Rect{ ix, bottom - PAN_BAR_H, iw, PAN_BAR_H };
        bottom -= panH + 3;
    }

    // aux sends (track strips only)
    L.knob_r = KNOB_R;
    const int aux = trackStrip ? live_aux_count() : 0;
    const int cell = 2 * L.knob_r + 6;
    L.knobs_per_row = iw / (cell > 0 ? cell : 1);
    if (L.knobs_per_row < 1) L.knobs_per_row = 1;
    if (aux > 0 && L.knobs_per_row > aux) L.knobs_per_row = aux;
    const int rows = aux > 0 ? (aux + L.knobs_per_row - 1) / L.knobs_per_row : 0;
    L.send_cell_w = iw / L.knobs_per_row;
    L.send_cell_h = 2 * L.knob_r + 2 + mch;
    const int sendsH = rows * L.send_cell_h;
    L.sends = SDL_Rect{ ix, bottom - sendsH, iw, sendsH };
    bottom -= sendsH + (rows > 0 ? 3 : 0);

    // 3. processor box fills whatever is left between the input button and the
    //    bottom block (Ardour packs it with expand=true).
    L.proc_row_h = mch + 3;
    int procH = bottom - y;
    if (procH < L.proc_row_h) procH = L.proc_row_h;
    L.procs = SDL_Rect{ ix, y, iw, procH };
    L.proc_rows = imax(1, procH / L.proc_row_h);

    return L;
}

bool MasterMixerView::send_pos(const Sub& L, int aux, int& cx, int& cy, int& r) const {
    const int aux_n = live_aux_count();
    // L.sends is zero-height on the strips that draw no knobs (aux / master),
    // so positions are refused rather than computed into some other block.
    if (L.sends.h <= 0) return false;
    if (aux < 0 || aux >= aux_n || L.knobs_per_row <= 0) return false;
    const int row = aux / L.knobs_per_row;
    const int col = aux % L.knobs_per_row;
    cx = L.sends.x + col * L.send_cell_w + L.send_cell_w / 2;
    cy = L.sends.y + row * L.send_cell_h + L.knob_r + 1;
    r  = L.knob_r;
    return true;
}

// ============================================================================
//  Processor box model
//
//  ProcessorBox::setup_entry_positions walks the route's processor list and
//  flips from PreFader to PostFader when it passes the Amp, so the fader is a
//  ROW in the list.  Same here: the rows are the inserts with a fader row at
//  the pre/post boundary, and dragging across that row changes placement.
// ============================================================================
std::vector<MasterMixerView::ProcRow> MasterMixerView::proc_rows(int idx) const {
    std::vector<ProcRow> rows;
    const int chain = chain_id(idx);
    const int n = engine_up() ? app_::audio_app_master_insert_count(chain) : 0;
    int i = 0;
    for (; i < n; ++i) {
        int nodeId = -1, active = 1, pre = 1;
        if (!app_::audio_app_master_insert_info(chain, i, &nodeId, nullptr, 0, &active, &pre))
            break;
        if (!pre) break;
        ProcRow r; r.slot = i; r.pre = true; rows.push_back(r);
    }
    { ProcRow f; f.fader = true; f.slot = -1; rows.push_back(f); }
    for (; i < n; ++i) {
        ProcRow r; r.slot = i; r.pre = false; rows.push_back(r);
    }
    return rows;
}

int MasterMixerView::proc_row_at(App& app, int idx, int y) const {
    const Sub L = layout(app, idx, strip_area(idx));
    if (L.proc_row_h <= 0) return -1;
    const int rel = y - L.procs.y;
    if (rel < 0) return -1;
    return rel / L.proc_row_h;
}

void MasterMixerView::add_plugin(int idx, int pluginIndex, bool pre) {
    const std::vector<PatchKnob::engine::PluginDescriptor>& inv = fx_inventory();
    if (pluginIndex < 0 || pluginIndex >= (int)inv.size()) return;
    const int chain = chain_id(idx);
    app_::audio_app_master_insert_add(chain, inv[(size_t)pluginIndex], pre ? 1 : 0);
    // Slot indices above the insertion point all shifted; the selection is a
    // slot INDEX, so keeping it would aim Delete at a different plugin.
    clear_proc_selection();
}

// ============================================================================
//  Drawing
// ============================================================================
void MasterMixerView::draw(App& app) {
    if (!visible) return;
    sync_model(app);
    clamp_scroll();

    // METER dt IS PER FRAME.  It used to be computed inside draw_gain(), which
    // runs once per STRIP: the first strip drawn got the real frame interval
    // and every later one measured `now - m_lastTicks` against a stamp set
    // microseconds earlier in the same frame, got ~0, and fell through to a
    // fabricated 1/60 s.  Their meters then decayed 0.33 dB per repaint however
    // long the repaint actually took -- 1.4 dB/s at the idle rate against the
    // 20 dB/s strip 0 was using, so adjacent strips visibly disagreed about the
    // same signal and peak-hold took a minute to release.  mixer_view and
    // arrange_view already do it once per view draw; this matches them.
    //
    // A long gap is capped, NOT replaced: after a stall (plugin editor, file
    // dialog, project load) the meter must catch up by decaying a lot, which is
    // what ui::meter::update does with its own 1.0 s cap.  Substituting 1/60
    // there was backwards -- it froze the meters at a stale high level.
    {
        const unsigned now = SDL_GetTicks();
        double dt = m_lastTicks ? double(now - m_lastTicks) / 1000.0 : 1.0 / 60.0;
        if (dt < 0.0) dt = 0.0;
        if (dt > 1.0) dt = 1.0;
        m_meterDt   = dt;
        m_lastTicks = now;
    }

    const Theme& t = theme();
    SDL_Renderer* r = app.ren;

    ui::fill_rect(r, rect, t.bg);
    {
        ui::ScopedClip clipScope(r, rect);
        const int total = strip_count();
        for (int i = 0; i < total; ++i) {
            const SDL_Rect a = strip_area(i);
            if (a.x + a.w < rect.x || a.x > rect.x + rect.w) continue;   // cull
            draw_strip(app, i, a);
        }
        // The "+ AUX" add-bus column, between the returns and the MASTER.  It
        // only exists while the shell wired the whole aux surface (aux_ui), so
        // it is never a button that creates nothing.
        if (aux_ui()) {
            const SDL_Rect a = add_bus_rect();
            if (!(a.x + a.w < rect.x || a.x > rect.x + rect.w)) {
                int mx = 0, my = 0; ui::mouse_logical(app, mx, my);
                const bool hot = in_rect(a, mx, my);
                ui::fill_rect(r, a, hot ? t.sel : t.panel);
                ui::frame_rect(r, a, hot ? t.text : t.dim);
                static const char* glyphs = "+AUX";
                const int gh = app.mono.ch() + 2;
                int gy = a.y + imax(4, (a.h - 4 * gh) / 2);
                for (int g = 0; glyphs[g]; ++g) {
                    const std::string one(1, glyphs[g]);
                    app.mono.draw(r, a.x + (a.w - app.mono.text_w(one)) / 2, gy,
                                  one, glyphs[g] == '+' ? t.text : t.dim);
                    gy += gh;
                }
            }
        }
        draw_menu(app);
    }
    ui::frame_rect(r, rect, t.dim);
}

// Ardour's ArdourButton has three visual states; the strip uses all three:
// off, ImplicitActive (something else is causing this state) and ExplicitActive.
void MasterMixerView::draw_button(App& app, const SDL_Rect& q, const std::string& text,
                                  bool active, bool implicit, bool hot) {
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::fill_rect(r, q, active ? t.accent : (implicit ? t.sel : t.keybg));
    ui::frame_rect(r, q, hot ? t.text : t.dim);
    const int cells = (q.w - 4) / imax(1, app.mono.cw());
    app.mono.draw_centered(r, q, clip_cells(text, cells),
                           active ? t.bg : (implicit ? t.text : t.dim));
}

void MasterMixerView::draw_strip(App& app, int idx, const SDL_Rect& area) {
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    const bool master = is_master(idx);
    const bool auxStrip = is_aux(idx);
    const int auxIdx = aux_of(idx);
    const int track = track_of(idx);
    const int mcw = imax(1, app.mono.cw());
    StripState& S = state(idx);

    const Sub L = layout(app, idx, area);
    int mx = 0, my = 0; ui::mouse_logical(app, mx, my);

    ui::fill_rect(r, area, t.panel);
    ui::frame_rect(r, area, master ? t.accent : t.dim);

    // ---- 1. NAME header (mixer_strip.cc name_button) -----------------------
    {
        // aux headers get their own colour so the returns read as a block of
        // their own between the tracks and the (accent-coloured) MASTER
        ui::fill_rect(r, L.header, master ? t.accent : (auxStrip ? t.hi : t.keybg));
        std::string nm;
        if (m_edit_kind == E_NAME && m_edit_strip == idx) nm = m_edit_buf + "_";
        else if (auxStrip) {
            // the bus name lives in the ENGINE (audio_app_master_aux_name), not
            // in StripState -- aux strip indices shift as buses come and go, so
            // a positional local override would migrate to the wrong bus
            char nb[64] = {0};
            app_::audio_app_master_aux_name(auxIdx, nb, (int)sizeof(nb));
            nm = nb[0] ? std::string(nb) : ("AUX " + std::to_string(auxIdx + 1));
        }
        else if (!S.name.empty())                         nm = S.name;
        else if (get_label)                               nm = get_label(idx);
        else nm = master ? std::string("MASTER") : std::to_string(idx + 1);
        app.font.draw_centered(r, L.header, clip_cells(nm, (L.header.w - 4) / imax(1, app.font.cw())),
                               (master || auxStrip) ? t.bg : t.text);
    }

    // ---- 2. INPUT button ---------------------------------------------------
    if (auxStrip) {
        // A bus's one input IS the tracks' send taps; there is nothing to
        // choose, so this is plain text -- a button that opened no menu would
        // be exactly the kind of dead control the send knobs used to be.
        app.mono.draw_centered(r, L.input, "(sends)", t.dim);
    } else {
        std::string label = "-";
        if (engine_up()) {
            const int src = app_::audio_app_master_strip_input(track);
            if (src >= 0) label = node_name(src);
            else label = master ? std::string("mix bus") : std::string("-in-");
        }
        draw_button(app, L.input, label, false, false, in_rect(L.input, mx, my));
    }

    // ---- 3. PROCESSOR BOX --------------------------------------------------
    if (show_inserts) draw_procs(app, idx, L);
    else { ui::fill_rect(r, L.procs, t.keybg); ui::frame_rect(r, L.procs, t.dim); }

    // ---- aux sends (track strips only: the send API is track-addressed) ----
    if (track >= 0) {
        const int aux_n = live_aux_count();
        for (int a = 0; a < aux_n; ++a) {
            int cx, cy, rr;
            if (!send_pos(L, a, cx, cy, rr)) continue;
            const bool dragging = (m_drag == D_SEND && m_drag_strip == idx && m_drag_aux == a);
            const bool on  = engine_up() && app_::audio_app_master_send_enabled(track, a);
            const bool pre = engine_up() && app_::audio_app_master_send_prefader(track, a);
            const float val = get_send ? clamp01(get_send(idx, a)) : 0.f;
            fill_disc(r, cx, cy, rr, t.keybg);
            circle_ring(r, cx, cy, rr, dragging ? t.text : t.dim);
            const double ang = (-135.0 + val * 270.0) * PI / 180.0;
            const int ex = cx + (int)std::lround(std::sin(ang) * (rr - 2));
            const int ey = cy - (int)std::lround(std::cos(ang) * (rr - 2));
            // a DISABLED send keeps showing its (preserved) level, but dimmed,
            // so toggling it back on is a known quantity
            ui::set_color(r, on ? t.accent : t.dim);
            SDL_RenderDrawLine(r, cx, cy, ex, ey);
            SDL_RenderDrawLine(r, cx + 1, cy, ex + 1, ey);
            // The caption is the BUS NAME (which knob feeds which bus), except
            // while dragging, when it becomes the live value -- the knob under
            // the finger is the one whose number the user wants.  Pre-fader
            // sends caption in t.note; the right-click menu spells the state
            // out in words for anyone who does not know the colour code.
            std::string lb;
            if (dragging) {
                char b[8]; std::snprintf(b, sizeof(b), "%d%%", (int)std::lround(val * 100.f));
                lb = b;
            } else {
                char nb[64] = {0};
                app_::audio_app_master_aux_name(a, nb, (int)sizeof(nb));
                lb = nb[0] ? std::string(nb) : ("A" + std::to_string(a + 1));
            }
            lb = clip_cells(lb, imax(1, (L.send_cell_w - 2) / imax(1, app.mono.cw())));
            const Color cap = dragging ? t.text : (!on ? t.dim : (pre ? t.note : t.text));
            app.mono.draw(r, cx - app.mono.text_w(lb) / 2, cy + rr + 1, lb, cap);
        }
    }

    // ---- 4. PAN (not on aux strips: the return has no pan stage) -----------
    if (!auxStrip) {
        float pv = 0.f;
        if (get_pan)          pv = clampf(get_pan(idx), -1.f, 1.f);
        else if (engine_up() && !master) pv = clampf(app_::audio_app_mixer_pan(node(), track), -1.f, 1.f);
        char rd[8];
        if (std::fabs(pv) < 0.06f) std::snprintf(rd, sizeof(rd), "C");
        else if (pv < 0.f)         std::snprintf(rd, sizeof(rd), "L%d", (int)std::lround(-pv * 100));
        else                       std::snprintf(rd, sizeof(rd), "R%d", (int)std::lround(pv * 100));
        app.mono.draw(r, L.pan.x, L.pan.y, "PAN", t.dim);
        app.mono.draw(r, L.pan.x + L.pan.w - app.mono.text_w(rd), L.pan.y, rd, t.text);

        ui::fill_rect(r, L.pan_bar, t.keybg);
        ui::frame_rect(r, L.pan_bar, t.dim);
        ui::vline(r, L.pan_bar.x + L.pan_bar.w / 2, L.pan_bar.y + 2,
                  L.pan_bar.y + L.pan_bar.h - 3, t.dim);
        const int kx = L.pan_bar.x + 2 + (int)(clamp01(pv * 0.5f + 0.5f) * (L.pan_bar.w - 6));
        SDL_Rect knob{ kx, L.pan_bar.y + 2, 4, L.pan_bar.h - 4 };
        ui::fill_rect(r, knob, t.accent);
    }

    // ---- 5. MUTE / SOLO ----------------------------------------------------
    if (auxStrip) {
        // Bus mute is the engine's own state, read live (no solo interplay to
        // resolve, so none of the strips' self/implicit model applies) and the
        // one MUTE button spans the row -- see layout().
        const bool amute = engine_up() && app_::audio_app_master_aux_mute(auxIdx);
        draw_button(app, L.mute, "MUTE", amute, false, in_rect(L.mute, mx, my));
    } else {
        const bool implicitMute = muted_by_others(idx);
        draw_button(app, L.mute, "M", S.selfMute, implicitMute, in_rect(L.mute, mx, my));
        if (master) {
            draw_button(app, L.solo, "S", false, false, false);   // master has no solo
        } else {
            const Theme& th = theme();
            ui::fill_rect(r, L.solo, S.selfSolo ? th.note : th.keybg);
            ui::frame_rect(r, L.solo, in_rect(L.solo, mx, my) ? th.text : th.dim);
            app.mono.draw_centered(r, L.solo, "S", S.selfSolo ? th.bg : th.dim);
            // solo isolate LED (route_ui.cc solo_isolated_led)
            ui::fill_rect(r, L.iso, S.soloIso ? t.hi : t.keybg);
            ui::frame_rect(r, L.iso, S.soloSafe ? t.text : t.dim);
        }
    }

    // ---- 6. GAIN (fader + meter + dB entry + peak) -------------------------
    draw_gain(app, idx, L, master);

    // ---- 7/8. OUTPUT + COMMENTS -------------------------------------------
    {
        if (auxStrip) {
            // the return is hard-wired into the master mix at the bus's return
            // gain -- informational text, not a button with no menu behind it
            app.mono.draw_centered(r, L.output, "> master", t.dim);
        } else {
            std::string outLabel = master ? "audio out" : "master";
            if (engine_up() && !master) {
                const int d = app_::audio_app_master_strip_output(track);
                if (d >= 0) outLabel = std::string(">") + node_name(d);
            }
            draw_button(app, L.output, outLabel, false, false, in_rect(L.output, mx, my));
        }

        std::string cm;
        if (m_edit_kind == E_COMMENT && m_edit_strip == idx) cm = m_edit_buf + "_";
        else cm = S.comment.empty() ? std::string("Cmt") : S.comment;
        draw_button(app, L.comment, cm, false, !S.comment.empty(), in_rect(L.comment, mx, my));
    }
    (void)mcw;
}

void MasterMixerView::draw_procs(App& app, int idx, const Sub& L) {
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    StripState& S = state(idx);
    const int chain = chain_id(idx);
    const int cells = (L.procs.w - 6) / imax(1, app.mono.cw());

    ui::fill_rect(r, L.procs, t.keybg);

    int mx = 0, my = 0; ui::mouse_logical(app, mx, my);
    const std::vector<ProcRow> rows = proc_rows(idx);

    if (S.procScroll > (int)rows.size() - 1) S.procScroll = (int)rows.size() - 1;
    if (S.procScroll < 0) S.procScroll = 0;

    ui::ScopedClip clipScope(r, L.procs);

    const int visible = L.proc_rows;
    for (int v = 0; v < visible; ++v) {
        const int ri = v + S.procScroll;
        SDL_Rect q{ L.procs.x + 1, L.procs.y + v * L.proc_row_h, L.procs.w - 2, L.proc_row_h - 1 };
        if (ri >= (int)rows.size()) {
            // trailing empty area: double-click here adds a plugin, exactly as
            // ProcessorBox::processor_button_press_event does on empty space.
            if (ri == (int)rows.size() && in_rect(q, mx, my))
                app.mono.draw(r, q.x + 3, q.y + 1, clip_cells("+ plugin", cells), t.dim);
            continue;
        }
        const ProcRow& row = rows[(size_t)ri];

        if (row.fader) {
            // the Amp entry -- Ardour names it "Fader" and styles it apart
            ui::fill_rect(r, q, t.sel);
            app.mono.draw(r, q.x + 3, q.y + 1, clip_cells("-- FADER --", cells), t.text);
            if (m_drag == D_PROC && m_drag_strip == idx)
                ui::frame_rect(r, q, t.accent);
            continue;
        }

        char nameBuf[96] = {0};
        int nodeId = -1, active = 1, pre = 1;
        if (!app_::audio_app_master_insert_info(chain, row.slot, &nodeId, nameBuf,
                                                (int)sizeof(nameBuf), &active, &pre))
            continue;

        const bool sel  = (S.selProc == row.slot);
        const bool hot  = in_rect(q, mx, my);
        const bool drag = (m_drag == D_PROC && m_drag_strip == idx && m_drag_slot == row.slot);
        ui::fill_rect(r, q, sel ? t.sel : (row.pre ? t.keybg : t.panel));
        ui::frame_rect(r, q, drag ? t.accent : (hot ? t.text : t.dim));

        // the LED: Ardour's ProcessorEntry::led_clicked toggles the processor's
        // active state, and the LED is lit while it is enabled.
        SDL_Rect led{ q.x + 2, q.y + (q.h - 6) / 2, 6, 6 };
        ui::fill_rect(r, led, active ? t.accent : t.bg);
        ui::frame_rect(r, led, t.dim);

        app.mono.draw(r, q.x + 11, q.y + 1,
                      clip_cells(nameBuf, cells - 2),
                      active ? t.text : t.dim);
    }

    ui::frame_rect(r, L.procs, t.dim);
}

void MasterMixerView::draw_gain(App& app, int idx, const Sub& L, bool master) {
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    StripState& S = state(idx);
    const int track = track_of(idx);

    // ---- live gain ---------------------------------------------------------
    // An aux strip's fader IS the bus's RETURN GAIN -- same linear-coefficient
    // semantics (unity = 1.0), same Ardour taper, read straight from the aux
    // API.  The generic hooks are never consulted for an aux index: the shell
    // binds them by strip position (i < bus_count ? track : master) and would
    // hand an aux strip the MASTER's gain.
    float gain = 1.f;
    if (is_aux(idx)) gain = engine_up() ? app_::audio_app_master_aux_return_gain(aux_of(idx)) : 1.f;
    else if (get_gain) gain = get_gain(idx);
    else if (engine_up()) gain = master ? app_::audio_app_mixer_master_gain(node())
                                        : app_::audio_app_mixer_gain(node(), track);
    const float pos = gain_to_slider(gain);

    // ---- meter ballistics (GainMeterBase::update_meters) -------------------
    // dt is sampled once per frame in draw(); every strip in this frame shares
    // it, so they all decay at the same real rate.
    const double dt = m_meterDt;

    float lvl[2] = { 0.f, 0.f };
    for (int c = 0; c < 2; ++c) {
        if (is_aux(idx))           lvl[c] = engine_up() ? app_::audio_app_master_aux_peak(aux_of(idx), c) : 0.f;
        else if (get_level_stereo) lvl[c] = get_level_stereo(idx, c);
        else if (engine_up())      lvl[c] = app_::audio_app_master_strip_peak(track, c);
        else if (get_level)        lvl[c] = get_level(idx);
    }
    ui::meter::update(S.meterL, lvl[0], dt);
    ui::meter::update(S.meterR, lvl[1], dt);
    const float mpeak = std::max(S.meterL.peakDb, S.meterR.peakDb);
    if (mpeak > S.maxPeakDb) S.maxPeakDb = mpeak;

    // ---- dB ruler, on the meter's own scale so numbers line up -------------
    {
        static const int marks[] = { 6, 0, -6, -12, -18, -24, -30, -40, -50 };
        for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
            const float f = ui::meter::deflect(meter_type, (float)marks[i]);
            const int yy = L.scale.y + L.scale.h - 1 - (int)(f * (L.scale.h - 2));
            if (yy < L.scale.y || yy > L.scale.y + L.scale.h) continue;
            char b[8]; std::snprintf(b, sizeof(b), "%d", marks[i]);
            app.mono.draw(r, L.scale.x + L.scale.w - 4 - app.mono.text_w(b),
                          yy - app.mono.ch() / 2, b, marks[i] == 0 ? t.text : t.dim);
            ui::hline(r, L.scale.x + L.scale.w - 3, L.scale.x + L.scale.w - 1, yy, t.dim);
        }
    }

    // ---- stereo meter through ui::meter ------------------------------------
    {
        const int gapw = 1;
        const int cw = imax(2, (L.meter.w - gapw) / 2);
        ui::fill_rect(r, L.meter, t.keybg);
        SDL_Rect bl{ L.meter.x + 1, L.meter.y + 1, imax(2, cw - 1),
                     imax(2, L.meter.h - 2) };
        SDL_Rect br{ L.meter.x + cw + gapw, L.meter.y + 1,
                     imax(2, L.meter.w - cw - gapw - 1), imax(2, L.meter.h - 2) };
        // Animates every frame -> register the whole well (both bars, the
        // divider and the frame) so damage-clipped frames keep it live.
        app.add_damage(L.meter);
        ui::meter::draw(r, bl, S.meterL, meter_type, true);
        ui::meter::draw(r, br, S.meterR, meter_type, true);
        ui::vline(r, L.meter.x + cw, L.meter.y + 1,
                  L.meter.y + L.meter.h - 2, t.dim);
        ui::frame_rect(r, L.meter, t.text);
    }

    // ---- fader (Ardour taper; unity tick where gain == 1.0) ----------------
    {
        ui::fill_rect(r, L.fader, t.keybg);
        ui::frame_rect(r, L.fader, t.dim);
        const int top = L.fader.y + 4, bot = L.fader.y + L.fader.h - 4;
        const int travel = imax(1, bot - top);
        const int unityY = top + (int)((1.f - gain_to_slider(1.0f)) * travel);
        ui::hline(r, L.fader.x, L.fader.x + L.fader.w - 1, unityY, t.dim);
        const int ky = top + (int)((1.f - pos) * travel);
        SDL_Rect fill{ L.fader.x + 2, ky, L.fader.w - 4, bot - ky };
        if (fill.h > 0) ui::fill_rect(r, fill, t.accent);
        SDL_Rect handle{ L.fader.x, ky - 3, L.fader.w, 7 };
        ui::fill_rect(r, handle, t.hi);
        ui::frame_rect(r, handle, t.text);
    }

    // ---- numeric dB entry (GainMeterBase::show_gain / gain_activated) ------
    {
        char buf[24];
        if (m_edit_kind == E_GAIN && m_edit_strip == idx) {
            std::snprintf(buf, sizeof(buf), "%s_", m_edit_buf.c_str());
        } else if (gain <= 0.f) {
            std::snprintf(buf, sizeof(buf), "-inf");
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f", coefficient_to_dB(gain));
        }
        ui::fill_rect(r, L.db, t.keybg);
        ui::frame_rect(r, L.db, (m_edit_kind == E_GAIN && m_edit_strip == idx) ? t.accent : t.dim);
        app.mono.draw_centered(r, L.db, clip_cells(buf, (L.db.w - 2) / imax(1, app.mono.cw())),
                               master ? t.accent : t.text);
    }

    // ---- peak readout: max dBFS, click to reset ----------------------------
    {
        char buf[24];
        if (S.maxPeakDb <= -200.f) std::snprintf(buf, sizeof(buf), "-inf");
        else                       std::snprintf(buf, sizeof(buf), "%.1f", S.maxPeakDb);
        const bool peaking = (S.meterL.clipped || S.meterR.clipped);
        ui::fill_rect(r, L.peak, peaking ? t.active : t.keybg);
        ui::frame_rect(r, L.peak, t.dim);
        app.mono.draw_centered(r, L.peak, clip_cells(buf, (L.peak.w - 2) / imax(1, app.mono.cw())),
                               peaking ? t.bg : t.dim);
    }

}

// ============================================================================
//  Popup menus
// ============================================================================
void MasterMixerView::draw_menu(App& app) {
    if (m_menu.kind == M_NONE || m_menu.items.empty()) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    const int rowh = app.mono.ch() + 4;

    int mx = 0, my = 0; ui::mouse_logical(app, mx, my);
    m_menu.hover = -1;

    ui::fill_rect(r, m_menu.rect, t.panel);
    ui::frame_rect(r, m_menu.rect, t.accent);
    ui::ScopedClip clipScope(r, m_menu.rect);

    const int visible = imax(1, (m_menu.rect.h - 2 * MENU_PAD) / rowh);
    for (int v = 0; v < visible; ++v) {
        const int i = v + m_menu.scroll;
        if (i < 0 || i >= (int)m_menu.items.size()) break;
        const MenuItem& it = m_menu.items[(size_t)i];
        SDL_Rect q{ m_menu.rect.x + 1, m_menu.rect.y + MENU_PAD + v * rowh,
                    m_menu.rect.w - 2, rowh };
        if (it.separator) {
            ui::hline(r, q.x + 3, q.x + q.w - 4, q.y + rowh / 2, t.dim);
            continue;
        }
        const bool hot = in_rect(q, mx, my);
        if (hot) { ui::fill_rect(r, q, t.sel); m_menu.hover = i; }
        std::string label = it.label;
        if (it.check) label = (it.checked ? "* " : "  ") + label;
        app.mono.draw(r, q.x + 4, q.y + 2,
                      clip_cells(label, (q.w - 8) / imax(1, app.mono.cw())),
                      it.enabled ? (hot ? t.text : t.text) : t.dim);
    }
    if ((int)m_menu.items.size() > visible) {
        char b[24]; std::snprintf(b, sizeof(b), "%d/%d", m_menu.scroll + 1, (int)m_menu.items.size());
        app.mono.draw(r, m_menu.rect.x + 4, m_menu.rect.y + m_menu.rect.h - app.mono.ch() - 2,
                      b, t.dim);
    }
}

void MasterMixerView::open_menu(App& app, MenuKind kind, int idx, int slot, const SDL_Rect& anchor) {
    m_menu = Menu();
    m_menu.kind = kind;
    m_menu.strip = idx;
    m_menu.slot = slot;

    const int track = track_of(idx);
    const bool master = is_master(idx);
    const bool auxStrip = is_aux(idx);

    switch (kind) {
    case M_PROC: {
        // ProcessorBox::register_actions (processor_box.cc:4200-4290), trimmed
        // to the operations this engine can actually perform.
        MenuItem a; a.label = "New Plugin..."; a.id = 1; m_menu.items.push_back(a);
        MenuItem e; e.label = "Edit...";       e.id = 3; e.enabled = slot >= 0; m_menu.items.push_back(e);
        MenuItem s0; s0.separator = true; m_menu.items.push_back(s0);
        MenuItem c;  c.label = "Copy";   c.id = 4; c.enabled = slot >= 0; m_menu.items.push_back(c);
        MenuItem p;  p.label = "Paste";  p.id = 5; p.enabled = m_clip_valid; m_menu.items.push_back(p);
        MenuItem d;  d.label = "Delete"; d.id = 6; d.enabled = slot >= 0; m_menu.items.push_back(d);
        MenuItem s1; s1.separator = true; m_menu.items.push_back(s1);
        MenuItem ta; ta.label = "Toggle Active"; ta.id = 12; ta.enabled = slot >= 0; m_menu.items.push_back(ta);
        MenuItem aa; aa.label = "Activate All";   aa.id = 7; m_menu.items.push_back(aa);
        MenuItem da; da.label = "Deactivate All"; da.id = 8; m_menu.items.push_back(da);
        MenuItem s2; s2.separator = true; m_menu.items.push_back(s2);
        // Placement moves are a TRACK-strip affair: the master chain has
        // always kept its slots where they are, and an aux chain's "fader" is
        // the bus's return gain, whose pre/post split the engine owns.
        MenuItem pre;  pre.label  = "Move to Pre-Fader";  pre.id  = 13;
        pre.enabled  = slot >= 0 && !master && !auxStrip; m_menu.items.push_back(pre);
        MenuItem post; post.label = "Move to Post-Fader"; post.id = 14;
        post.enabled = slot >= 0 && !master && !auxStrip; m_menu.items.push_back(post);
        MenuItem s3; s3.separator = true; m_menu.items.push_back(s3);
        MenuItem cl;  cl.label  = "Clear";            cl.id  = 9;  m_menu.items.push_back(cl);
        MenuItem clp; clp.label = "Clear Pre-Fader";  clp.id = 10; m_menu.items.push_back(clp);
        MenuItem cls; cls.label = "Clear Post-Fader"; cls.id = 11; m_menu.items.push_back(cls);
        break; }
    case M_PLUGIN: {
        const std::vector<PatchKnob::engine::PluginDescriptor>& inv = fx_inventory();
        if (inv.empty()) {
            MenuItem it; it.label = "(no effects scanned)"; it.id = -1; it.enabled = false;
            m_menu.items.push_back(it);
        }
        for (size_t i = 0; i < inv.size(); ++i) {
            MenuItem it; it.label = inv[i].name; it.id = 1000 + (int)i;
            m_menu.items.push_back(it);
        }
        break; }
    case M_INPUT: {
        MenuItem d; d.label = "Disconnect"; d.id = 0; m_menu.items.push_back(d);
        MenuItem s; s.separator = true; m_menu.items.push_back(s);
        std::vector<int> ids(256, 0);
        const int n = imin((int)ids.size(), app_::audio_app_patch_node_ids(ids.data(), (int)ids.size()));
        for (int i = 0; i < n; ++i) {
            if (ids[(size_t)i] == app_::audio_app_master_mixer_node()) continue;
            if (app_::audio_app_patch_first_port(ids[(size_t)i], 0, 1) < 0) continue;  // needs audio out
            MenuItem it; it.label = node_name(ids[(size_t)i]); it.id = 1000 + ids[(size_t)i];
            m_menu.items.push_back(it);
        }
        break; }
    case M_OUTPUT: {
        MenuItem d; d.label = "None"; d.id = 0; m_menu.items.push_back(d);
        MenuItem s; s.separator = true; m_menu.items.push_back(s);
        std::vector<int> ids(256, 0);
        const int n = imin((int)ids.size(), app_::audio_app_patch_node_ids(ids.data(), (int)ids.size()));
        for (int i = 0; i < n; ++i) {
            if (ids[(size_t)i] == app_::audio_app_master_mixer_node()) continue;
            if (app_::audio_app_patch_first_port(ids[(size_t)i], 0, 0) < 0) continue;  // needs audio in
            MenuItem it; it.label = node_name(ids[(size_t)i]); it.id = 1000 + ids[(size_t)i];
            m_menu.items.push_back(it);
        }
        break; }
    case M_STRIP: {
        MenuItem a; a.label = "Rename...";        a.id = 1; m_menu.items.push_back(a);
        MenuItem b; b.label = "Comments...";      b.id = 2; m_menu.items.push_back(b);
        MenuItem s0; s0.separator = true; m_menu.items.push_back(s0);
        MenuItem c; c.label = "Meter Type";       c.id = 3; m_menu.items.push_back(c);
        MenuItem d; d.label = "Reset Peak";       d.id = 4; m_menu.items.push_back(d);
        MenuItem s1; s1.separator = true; m_menu.items.push_back(s1);
        if (!auxStrip) {
            // solo isolate/safe belong to the solo model; a bus has no solo
            MenuItem e; e.label = "Solo Isolate";     e.id = 5; e.check = true;
            e.checked = state(idx).soloIso; e.enabled = !master; m_menu.items.push_back(e);
            MenuItem f; f.label = "Solo Safe";        f.id = 6; f.check = true;
            f.checked = state(idx).soloSafe; f.enabled = !master; m_menu.items.push_back(f);
            MenuItem s2; s2.separator = true; m_menu.items.push_back(s2);
        }
        MenuItem g; g.label = "Clear Inserts";    g.id = 7; m_menu.items.push_back(g);
        MenuItem h; h.label = auxStrip ? "Unity Return" : "Unity Gain";
        h.id = 8; m_menu.items.push_back(h);
        if (auxStrip) {
            MenuItem s3; s3.separator = true; m_menu.items.push_back(s3);
            MenuItem rm; rm.label = "Remove Bus"; rm.id = 9; m_menu.items.push_back(rm);
        }
        break; }
    case M_SEND: {
        // Right-click on a send knob.  `slot` carries the AUX INDEX here.
        //
        // The knob itself only has room for one continuous gesture (the level
        // drag), so the two discrete states -- enabled and pre/post-fader --
        // live in this menu: a narrow strip has no space for per-send toggle
        // buttons that would still be legible, and a modifier-click can't SHOW
        // state, only change it.  The menu both shows (checkmarks) and flips
        // them, and names the destination bus so there is never a doubt which
        // send is being edited.
        char nb[64] = {0};
        app_::audio_app_master_aux_name(slot, nb, (int)sizeof(nb));
        MenuItem hd; hd.label = std::string("Send > ")
                              + (nb[0] ? std::string(nb) : ("AUX " + std::to_string(slot + 1)));
        hd.id = -1; hd.enabled = false; m_menu.items.push_back(hd);
        MenuItem s0; s0.separator = true; m_menu.items.push_back(s0);
        MenuItem en; en.label = "Enabled"; en.id = 1; en.check = true;
        en.checked = track >= 0 && engine_up() && app_::audio_app_master_send_enabled(track, slot);
        m_menu.items.push_back(en);
        MenuItem pf; pf.label = "Pre-Fader"; pf.id = 2; pf.check = true;
        pf.checked = track >= 0 && engine_up() && app_::audio_app_master_send_prefader(track, slot);
        m_menu.items.push_back(pf);
        MenuItem s1; s1.separator = true; m_menu.items.push_back(s1);
        MenuItem z; z.label = "Level 0"; z.id = 3; m_menu.items.push_back(z);
        break; }
    case M_METER: {
        static const char* names[] = { "PPM", "DIN", "Nordic", "VU", "K20", "K14", "K12", "Peak" };
        for (int i = 0; i < 8; ++i) {
            MenuItem it; it.label = names[i]; it.id = i; it.check = true;
            it.checked = ((int)meter_type == i);
            m_menu.items.push_back(it);
        }
        break; }
    default: break;
    }
    (void)track;

    // size + clamp inside the view
    const int rowh = app.mono.ch() + 4;
    int w = 0;
    for (size_t i = 0; i < m_menu.items.size(); ++i)
        w = imax(w, app.mono.text_w(m_menu.items[i].label) + 8 + 2 * app.mono.cw());
    w = imax(w, 90);
    w = imin(w, imax(90, rect.w - 8));
    int h = (int)m_menu.items.size() * rowh + 2 * MENU_PAD;
    h = imin(h, imax(rowh * 3, rect.h - 8));

    int x = anchor.x, y = anchor.y + anchor.h;
    if (x + w > rect.x + rect.w) x = rect.x + rect.w - w;
    if (x < rect.x) x = rect.x;
    if (y + h > rect.y + rect.h) y = imax(rect.y, anchor.y - h);
    if (y < rect.y) y = rect.y;
    m_menu.rect = SDL_Rect{ x, y, w, h };
    app.request_redraw();
}

void MasterMixerView::menu_pick(App& app, int id) {
    const MenuKind kind = m_menu.kind;
    const int idx = m_menu.strip;
    const int slot = m_menu.slot;
    const bool pre = m_menu.pre;
    const int track = track_of(idx);
    // insert-chain operations address chain_id (track / master / aux node)
    const int chain = chain_id(idx);
    const SDL_Rect anchor = m_menu.rect;
    m_menu = Menu();

    switch (kind) {
    case M_PROC:
        switch (id) {
        case 1:  { Menu save; (void)save;
                   open_menu(app, M_PLUGIN, idx, slot, anchor);
                   m_menu.pre = pre; return; }
        case 3:  if (slot >= 0 && on_edit_insert) {
                     const int n = app_::audio_app_master_insert_node_at(chain, slot);
                     if (n >= 0) on_edit_insert(n);
                 }
                 break;
        case 4:  if (slot >= 0) {
                     // Copy: remember which inventory entry this slot hosts, so
                     // Paste can instantiate a fresh one (Ardour copies state;
                     // this copies the plugin identity).
                     char nm[96] = {0}; int nodeId = -1, active = 1, isPre = 1;
                     if (app_::audio_app_master_insert_info(chain, slot, &nodeId, nm,
                                                            (int)sizeof(nm), &active, &isPre)) {
                         const std::vector<PatchKnob::engine::PluginDescriptor>& inv = fx_inventory();
                         m_clip_valid = false;
                         for (size_t i = 0; i < inv.size(); ++i)
                             if (inv[i].name == nm) { m_clip_plugin = (int)i; m_clip_pre = isPre != 0;
                                                      m_clip_valid = true; break; }
                     }
                 }
                 break;
        case 5:  if (m_clip_valid) add_plugin(idx, m_clip_plugin, m_clip_pre);
                 break;
        case 6:  if (slot >= 0) { app_::audio_app_master_insert_remove(chain, slot);
                                  clear_proc_selection(); }
                 break;
        case 7:  { const int n = app_::audio_app_master_insert_count(chain);
                   for (int i = 0; i < n; ++i) app_::audio_app_master_insert_set_active(chain, i, 1); }
                 break;
        case 8:  { const int n = app_::audio_app_master_insert_count(chain);
                   for (int i = 0; i < n; ++i) app_::audio_app_master_insert_set_active(chain, i, 0); }
                 break;
        case 9:  app_::audio_app_master_insert_clear(chain, -1); clear_proc_selection(); break;
        case 10: app_::audio_app_master_insert_clear(chain, 0);  clear_proc_selection(); break;
        case 11: app_::audio_app_master_insert_clear(chain, 1);  clear_proc_selection(); break;
        case 12: if (slot >= 0) {
                     int nodeId = -1, active = 1, isPre = 1;
                     if (app_::audio_app_master_insert_info(chain, slot, &nodeId, nullptr, 0,
                                                            &active, &isPre))
                         app_::audio_app_master_insert_set_active(chain, slot, active ? 0 : 1);
                 }
                 break;
        case 13: if (slot >= 0) app_::audio_app_master_insert_set_prefader(chain, slot, 1); break;
        case 14: if (slot >= 0) app_::audio_app_master_insert_set_prefader(chain, slot, 0); break;
        default: break;
        }
        break;

    case M_PLUGIN:
        if (id >= 1000) add_plugin(idx, id - 1000, pre);
        break;

    case M_INPUT:
        if (id == 0)          app_::audio_app_master_clear_audio_input(track);
        else if (id >= 1000)  app_::audio_app_master_route_audio_input(track, id - 1000);
        break;

    case M_OUTPUT:
        if (id == 0)         app_::audio_app_master_strip_set_output(track, -1);
        else if (id >= 1000) app_::audio_app_master_strip_set_output(track, id - 1000);
        break;

    case M_STRIP:
        switch (id) {
        case 1: begin_rename(app, idx); break;
        case 2: begin_comment(app, idx); break;
        case 3: open_menu(app, M_METER, idx, -1, anchor); return;
        case 4: ui::meter::reset(state(idx).meterL);
                ui::meter::reset(state(idx).meterR);
                state(idx).maxPeakDb = -318.f;
                break;
        case 5: state(idx).soloIso = !state(idx).soloIso; apply_mutes(); break;
        case 6: state(idx).soloSafe = !state(idx).soloSafe; break;
        case 7: app_::audio_app_master_insert_clear(chain, -1);
                clear_proc_selection();
                break;
        case 8: if (is_aux(idx)) { if (engine_up())
                    app_::audio_app_master_aux_set_return_gain(aux_of(idx), 1.0f); }
                else if (set_gain) set_gain(idx, 1.0f);
                else if (engine_up()) {
                    if (is_master(idx)) app_::audio_app_mixer_set_master_gain(node(), 1.0f);
                    else                app_::audio_app_mixer_set_gain(node(), track, 1.0f);
                }
                break;
        case 9: if (is_aux(idx) && engine_up()) {
                    // removing a bus shifts every aux/master strip index; the
                    // next sync_model resets the shifted StripStates, and the
                    // selection is dropped here so Delete can't act on a slot
                    // index that now belongs to a different chain
                    app_::audio_app_master_aux_remove(aux_of(idx));
                    clear_proc_selection();
                }
                break;
        default: break;
        }
        break;

    case M_SEND:
        // `slot` is the aux index; enabled/pre-fader go straight to the engine
        // (the level hooks only carry the level, and this menu only ever opens
        // from a knob that live_aux_count() said was real).
        if (track >= 0 && slot >= 0 && engine_up()) switch (id) {
        case 1: app_::audio_app_master_set_send_enabled(track, slot,
                    app_::audio_app_master_send_enabled(track, slot) ? 0 : 1);
                break;
        case 2: app_::audio_app_master_set_send_prefader(track, slot,
                    app_::audio_app_master_send_prefader(track, slot) ? 0 : 1);
                break;
        case 3: if (set_send) set_send(idx, slot, 0.f);
                break;
        default: break;
        }
        break;

    case M_METER:
        if (id >= 0 && id < 8) meter_type = (ui::meter::Type)id;
        break;

    default: break;
    }
    app.request_redraw();
}

bool MasterMixerView::menu_mouse(App& app, const MouseEv& e) {
    if (m_menu.kind == M_NONE) return false;
    if (!e.pressed) return true;                       // swallow the release
    if (!in_rect(m_menu.rect, e.x, e.y)) {             // click-away closes
        m_menu = Menu();
        app.request_redraw();
        return true;
    }
    const int rowh = app.mono.ch() + 4;
    const int v = (e.y - (m_menu.rect.y + MENU_PAD)) / imax(1, rowh);
    const int i = v + m_menu.scroll;
    if (i >= 0 && i < (int)m_menu.items.size()) {
        const MenuItem& it = m_menu.items[(size_t)i];
        if (!it.separator && it.enabled) { menu_pick(app, it.id); return true; }
    }
    return true;
}

// ============================================================================
//  Text entry (dB / rename / comment)
// ============================================================================
void MasterMixerView::begin_gain_entry(App& app, int idx) {
    float gain = 1.f;
    if (is_aux(idx)) gain = engine_up() ? app_::audio_app_master_aux_return_gain(aux_of(idx)) : 1.f;
    else if (get_gain) gain = get_gain(idx);
    else if (engine_up()) gain = is_master(idx) ? app_::audio_app_mixer_master_gain(node())
                                                : app_::audio_app_mixer_gain(node(), idx);
    char buf[24];
    if (gain <= 0.f) std::snprintf(buf, sizeof(buf), "-inf");
    else             std::snprintf(buf, sizeof(buf), "%.1f", coefficient_to_dB(gain));
    m_edit_kind = E_GAIN; m_edit_strip = idx; m_edit_buf = buf;
    app.begin_text(&m_edit_buf, nullptr, [this, &app, idx](bool ok) {
        if (ok) {
            // GainMeterBase::gain_activated: parse, clamp to the displayable
            // max, convert dB -> coefficient.
            float f = 0.f;
            if (std::sscanf(m_edit_buf.c_str(), "%f", &f) == 1) {
                const float maxDb = coefficient_to_dB(kMaxGain);
                if (f > maxDb) f = maxDb;
                const float g = dB_to_coefficient(f);
                if (is_aux(idx)) { if (engine_up())
                    app_::audio_app_master_aux_set_return_gain(aux_of(idx), g); }
                else if (set_gain) set_gain(idx, g);
                else if (engine_up()) {
                    if (is_master(idx)) app_::audio_app_mixer_set_master_gain(node(), g);
                    else                app_::audio_app_mixer_set_gain(node(), idx, g);
                }
            }
        }
        m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
        app.request_redraw();
    });
}

void MasterMixerView::begin_rename(App& app, int idx) {
    if (is_aux(idx)) {
        // The bus name is ENGINE state (audio_app_master_aux_set_name), not a
        // local override: strip indices shift when buses come and go, and the
        // name must follow the bus, not the column.  An empty commit falls
        // back to nothing -- the header shows "AUX n" for a nameless bus.
        char nb[64] = {0};
        app_::audio_app_master_aux_name(aux_of(idx), nb, (int)sizeof(nb));
        m_edit_kind = E_NAME; m_edit_strip = idx; m_edit_buf = nb;
        app.begin_text(&m_edit_buf, nullptr, [this, &app, idx](bool ok) {
            if (ok && is_aux(idx) && engine_up())
                app_::audio_app_master_aux_set_name(aux_of(idx), m_edit_buf.c_str());
            m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
            app.request_redraw();
        });
        return;
    }
    StripState& S = state(idx);
    m_edit_kind = E_NAME; m_edit_strip = idx;
    m_edit_buf = !S.name.empty() ? S.name : (get_label ? get_label(idx) : std::string());
    app.begin_text(&m_edit_buf, nullptr, [this, &app, idx](bool ok) {
        if (ok) state(idx).name = m_edit_buf;
        m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
        app.request_redraw();
    });
}

void MasterMixerView::begin_comment(App& app, int idx) {
    m_edit_kind = E_COMMENT; m_edit_strip = idx;
    m_edit_buf = state(idx).comment;
    app.begin_text(&m_edit_buf, nullptr, [this, &app, idx](bool ok) {
        if (ok) state(idx).comment = m_edit_buf;
        m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
        app.request_redraw();
    });
}

// ============================================================================
//  Input
// ============================================================================
bool MasterMixerView::on_mouse(App& app, const MouseEv& e) {
    sync_model(app);

    if (menu_mouse(app, e)) return true;

    // ---- continue an in-flight drag ---------------------------------------
    if (m_drag == D_FADER) {
        apply_fader(app, e);
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; }
        return true;
    }
    if (m_drag == D_SEND) {
        apply_send(app, e);
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; m_drag_aux = -1; }
        return true;
    }
    if (m_drag == D_PAN) {
        apply_pan(app, e);
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; }
        return true;
    }
    if (m_drag == D_PROC) {
        // ProcessorBox reorders on drop (processor_display.Reordered ->
        // ProcessorBox::reordered -> Route::reorder_processors).
        if (std::abs(e.y - m_drag_y0) > 3) m_drag_moved = true;
        if (!e.pressed) {
            if (m_drag_moved && m_drag_strip >= 0 && m_drag_slot >= 0) {
                const int chain = chain_id(m_drag_strip);
                const bool trackStrip = m_drag_strip < bus_count;
                const int row = proc_row_at(app, m_drag_strip, e.y) + state(m_drag_strip).procScroll;
                const std::vector<ProcRow> rows = proc_rows(m_drag_strip);
                int target = m_drag_slot;
                bool pre = true;
                if (row < 0) { target = 0; pre = true; }
                else if (row >= (int)rows.size()) {
                    target = app_::audio_app_master_insert_count(chain) - 1;
                    pre = !trackStrip;
                } else if (rows[(size_t)row].fader) {
                    // dropped ON the fader row: land just after it, post-fader
                    target = m_drag_slot;
                    pre = false;
                } else {
                    target = rows[(size_t)row].slot;
                    pre = rows[(size_t)row].pre;
                }
                // placement only moves across the fader on TRACK strips --
                // master and aux chains keep their current placement (same
                // rule the M_PROC menu applies)
                app_::audio_app_master_insert_move(chain, m_drag_slot, target,
                                                   trackStrip ? (pre ? 1 : 0) : -1);
                // Same reason as add_plugin: a reorder renumbers slots, so a
                // selection held over it would point at the wrong plugin and
                // Delete would remove that one instead.
                clear_proc_selection();
            }
            m_drag = D_NONE; m_drag_strip = -1; m_drag_slot = -1; m_drag_moved = false;
        }
        app.request_redraw();
        return true;
    }
    if (m_drag == D_CONSUME) {
        if (!e.pressed) m_drag = D_NONE;
        return true;
    }

    if (!e.pressed) return true;

    // the "+ AUX" column: create a bus, then go straight into naming it (the
    // strip appears where the click landed, so the edit is where the eye is)
    if (aux_ui() && in_rect(add_bus_rect(), e.x, e.y)) {
        if (e.button == SDL_BUTTON_LEFT && engine_up()) {
            char nm[32];
            std::snprintf(nm, sizeof(nm), "Aux %d", live_aux_count() + 1);
            const int nb = app_::audio_app_master_aux_add(nm);
            if (nb >= 0) {
                sync_model(app);            // pull the new count in NOW so the
                                            // new strip index exists to edit
                // only if the strip really materialised as an aux strip -- if
                // the count did not move, bus_count + nb would be the MASTER
                if (is_aux(bus_count + nb)) begin_rename(app, bus_count + nb);
            }
        }
        m_drag = D_CONSUME;
        app.request_redraw();
        return true;
    }

    for (int i = 0; i < strip_count(); ++i) {
        const SDL_Rect a = strip_area(i);
        if (in_rect(a, e.x, e.y)) { begin_press(app, i, e); return true; }
    }
    m_drag = D_CONSUME;
    return true;
}

void MasterMixerView::begin_press(App& app, int idx, const MouseEv& e) {
    const Sub L = layout(app, idx, strip_area(idx));
    const int track = track_of(idx);
    const int chain = chain_id(idx);
    const bool master = is_master(idx);
    const bool auxStrip = is_aux(idx);
    const bool rightBtn = (e.button == SDL_BUTTON_RIGHT);
    const SDL_Keymod mods = SDL_GetModState();
    StripState& S = state(idx);

    // header: DOUBLE-click renames, right-click opens the strip menu.
    //
    // A single click used to start the rename, which is both against the
    // convention every other name field in the app follows and the thing that
    // made the keyboard lockout below so easy to hit: merely clicking a strip
    // to select it captured app.text_target.  A single click now just consumes.
    if (in_rect(L.header, e.x, e.y)) {
        if (rightBtn) { open_menu(app, M_STRIP, idx, -1, L.header); m_drag = D_CONSUME; return; }
        const unsigned now = SDL_GetTicks();
        const bool dbl = (m_hdr_click_idx == idx) && (now - m_hdr_click_ms) < 400u;
        m_hdr_click_idx = idx; m_hdr_click_ms = now;
        if (dbl) { m_hdr_click_idx = -1; begin_rename(app, idx); }
        m_drag = D_CONSUME; return;
    }
    // input button (an aux strip's input is the send taps: label, no menu)
    if (in_rect(L.input, e.x, e.y)) {
        if (!master && !auxStrip) open_menu(app, M_INPUT, idx, -1, L.input);
        m_drag = D_CONSUME; return;
    }
    // processor box
    if (show_inserts && in_rect(L.procs, e.x, e.y)) {
        const int row = (e.y - L.procs.y) / imax(1, L.proc_row_h) + S.procScroll;
        const std::vector<ProcRow> rows = proc_rows(idx);
        const bool onRow = row >= 0 && row < (int)rows.size();
        const int slot = onRow ? rows[(size_t)row].slot : -1;
        const bool pre = onRow ? rows[(size_t)row].pre : (row < 0);

        if (rightBtn) {
            m_menu.pre = pre;
            open_menu(app, M_PROC, idx, slot, SDL_Rect{ L.procs.x, e.y, L.procs.w, 1 });
            m_menu.pre = pre;
            m_drag = D_CONSUME; return;
        }
        if (!onRow) {                       // empty space: add a plugin
            m_menu.pre = false;
            open_menu(app, M_PLUGIN, idx, -1, SDL_Rect{ L.procs.x, e.y, L.procs.w, 1 });
            m_menu.pre = false;
            m_drag = D_CONSUME; return;
        }
        if (rows[(size_t)row].fader) { m_drag = D_CONSUME; return; }

        // the LED column toggles active (ProcessorEntry::led_clicked)
        if (e.x <= L.procs.x + 10) {
            int nodeId = -1, active = 1, isPre = 1;
            if (app_::audio_app_master_insert_info(chain, slot, &nodeId, nullptr, 0, &active, &isPre))
                app_::audio_app_master_insert_set_active(chain, slot, active ? 0 : 1);
            m_drag = D_CONSUME; app.request_redraw(); return;
        }
        // Ardour: Delete-modifier click removes, plain click selects + arms a
        // reorder drag, double-click edits.
        if ((mods & KMOD_SHIFT) != 0) {
            app_::audio_app_master_insert_remove(chain, slot);
            clear_proc_selection(); m_drag = D_CONSUME; app.request_redraw(); return;
        }
        if ((mods & KMOD_CTRL) != 0 && on_edit_insert) {
            const int n = app_::audio_app_master_insert_node_at(chain, slot);
            if (n >= 0) on_edit_insert(n);
            m_drag = D_CONSUME; app.request_redraw(); return;
        }
        select_proc(idx, slot);
        m_drag = D_PROC; m_drag_strip = idx; m_drag_slot = slot; m_drag_row = row;
        m_drag_y0 = e.y; m_drag_x0 = e.x; m_drag_moved = false;
        app.request_redraw();
        return;
    }
    // aux sends (track strips only; send_pos refuses other strips' Subs).
    // Drag = level.  Right-click = the send menu (enable / pre-post / zero).
    // Ctrl+click = reset to 0, the same reset gesture pan and fader use.
    if (track >= 0) {
        const int aux_n = live_aux_count();
        for (int a = 0; a < aux_n; ++a) {
            int cx, cy, rr;
            if (send_pos(L, a, cx, cy, rr) && near_pt(e.x, e.y, cx, cy, rr + 3)) {
                if (rightBtn) {
                    open_menu(app, M_SEND, idx, a,
                              SDL_Rect{ cx - rr, cy + rr, 2 * rr, 1 });
                    m_drag = D_CONSUME; return;
                }
                if ((mods & KMOD_CTRL) != 0) {
                    if (set_send) set_send(idx, a, 0.f);
                    m_drag = D_CONSUME; app.request_redraw(); return;
                }
                m_drag = D_SEND; m_drag_strip = idx; m_drag_aux = a;
                m_drag_start = get_send ? clamp01(get_send(idx, a)) : 0.f;
                m_drag_y0 = e.y;
                app.request_redraw();
                return;
            }
        }
    }
    // pan
    if (in_rect(L.pan_bar, e.x, e.y)) {
        if ((mods & KMOD_CTRL) != 0) {                    // Ardour: reset to default
            if (set_pan) set_pan(idx, 0.f);
            else if (engine_up() && !master) app_::audio_app_mixer_set_pan(node(), track, 0.f);
            m_drag = D_CONSUME; app.request_redraw(); return;
        }
        m_drag = D_PAN; m_drag_strip = idx;
        apply_pan(app, e);
        return;
    }
    // mute / solo / isolate
    if (in_rect(L.mute, e.x, e.y)) {
        if (auxStrip) {
            // bus mute is engine state, no solo model involved (see layout())
            if (engine_up())
                app_::audio_app_master_aux_set_mute(
                    aux_of(idx), app_::audio_app_master_aux_mute(aux_of(idx)) ? 0 : 1);
            m_drag = D_CONSUME; app.request_redraw(); return;
        }
        S.selfMute = !S.selfMute;
        apply_mutes();
        if (toggle_mute && !engine_up()) toggle_mute(idx);
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    if (!master && in_rect(L.iso, e.x, e.y)) {
        if (rightBtn) S.soloSafe = !S.soloSafe;
        else          S.soloIso  = !S.soloIso;
        apply_mutes();
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    if (!master && in_rect(L.solo, e.x, e.y)) {
        if (S.soloSafe) { m_drag = D_CONSUME; return; }   // solo safe blocks changes
        if ((mods & (KMOD_CTRL | KMOD_ALT)) != 0) {
            // Primary-Secondary-click: exclusive solo (route_ui.cc solo_press)
            for (int i = 0; i < bus_count && i < (int)m_strips.size(); ++i)
                if (!m_strips[(size_t)i].soloSafe) m_strips[(size_t)i].selfSolo = (i == idx);
        } else {
            S.selfSolo = !S.selfSolo;
        }
        apply_mutes();
        if (toggle_solo && !engine_up()) toggle_solo(idx);
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    // fader (an aux strip's fader is the bus RETURN gain)
    if (in_rect(L.fader, e.x, e.y)) {
        if ((mods & KMOD_CTRL) != 0) {                    // reset to unity
            if (auxStrip) { if (engine_up())
                app_::audio_app_master_aux_set_return_gain(aux_of(idx), 1.0f); }
            else if (set_gain) set_gain(idx, 1.0f);
            else if (engine_up()) {
                if (master) app_::audio_app_mixer_set_master_gain(node(), 1.0f);
                else        app_::audio_app_mixer_set_gain(node(), track, 1.0f);
            }
            m_drag = D_CONSUME; app.request_redraw(); return;
        }
        m_drag = D_FADER; m_drag_strip = idx; m_drag_y0 = e.y;
        {
            float g = 1.f;
            if (auxStrip) g = engine_up() ? app_::audio_app_master_aux_return_gain(aux_of(idx)) : 1.f;
            else if (get_gain) g = get_gain(idx);
            else if (engine_up()) g = master ? app_::audio_app_mixer_master_gain(node())
                                             : app_::audio_app_mixer_gain(node(), track);
            m_drag_start = gain_to_slider(g);
        }
        apply_fader(app, e);
        return;
    }
    // dB entry / peak readout
    if (in_rect(L.db, e.x, e.y)) { begin_gain_entry(app, idx); m_drag = D_CONSUME; return; }
    if (in_rect(L.peak, e.x, e.y)) {
        // GainMeterBase::peak_button_release -> reset_peak_display
        ui::meter::reset(S.meterL); ui::meter::reset(S.meterR);
        S.maxPeakDb = -318.f;
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    // output / comments (an aux return is hard-wired to the master: no menu)
    if (in_rect(L.output, e.x, e.y)) {
        if (!master && !auxStrip) open_menu(app, M_OUTPUT, idx, -1, L.output);
        m_drag = D_CONSUME; return;
    }
    if (in_rect(L.comment, e.x, e.y)) { begin_comment(app, idx); m_drag = D_CONSUME; return; }

    if (rightBtn) { open_menu(app, M_STRIP, idx, -1, SDL_Rect{ e.x, e.y, 1, 1 }); m_drag = D_CONSUME; return; }
    m_drag = D_CONSUME;
}

void MasterMixerView::apply_fader(App& app, const MouseEv& e) {
    const int idx = m_drag_strip;
    if (idx < 0) return;
    const Sub L = layout(app, idx, strip_area(idx));
    const int top = L.fader.y + 4, bot = L.fader.y + L.fader.h - 4;
    const int travel = imax(1, bot - top);
    float pos;
    if ((SDL_GetModState() & KMOD_SHIFT) != 0) {
        // fine drag: 1/5 of the travel per pixel, relative to the grab point
        pos = m_drag_start + float(m_drag_y0 - e.y) / float(travel * 5);
    } else {
        pos = 1.f - float(e.y - top) / float(travel);
    }
    pos = clamp01(pos);
    const float g = slider_to_gain(pos);
    if (is_aux(idx)) { if (engine_up())
        app_::audio_app_master_aux_set_return_gain(aux_of(idx), g); }
    else if (set_gain) set_gain(idx, g);
    else if (engine_up()) {
        if (is_master(idx)) app_::audio_app_mixer_set_master_gain(node(), g);
        else                app_::audio_app_mixer_set_gain(node(), idx, g);
    }
    app.request_redraw();
}

void MasterMixerView::apply_send(App& app, const MouseEv& e) {
    if (m_drag_strip < 0 || m_drag_aux < 0) return;
    const float nv = clamp01(m_drag_start + float(m_drag_y0 - e.y) / float(KNOB_DRAG_PX));
    if (set_send) set_send(m_drag_strip, m_drag_aux, nv);
    app.request_redraw();
}

void MasterMixerView::apply_pan(App& app, const MouseEv& e) {
    const int idx = m_drag_strip;
    if (idx < 0) return;
    const Sub L = layout(app, idx, strip_area(idx));
    const SDL_Rect& bar = L.pan_bar;
    const int travel = imax(1, bar.w - 6);
    float v = clamp01(float(e.x - (bar.x + 2)) / float(travel)) * 2.0f - 1.0f;
    if (std::fabs(v) < 0.06f) v = 0.0f;
    if (set_pan) set_pan(idx, v);
    else if (engine_up() && idx < bus_count) app_::audio_app_mixer_set_pan(node(), idx, v);
    app.request_redraw();
}

bool MasterMixerView::on_wheel(App& app, int dx, int dy) {
    int mx = 0, my = 0; ui::mouse_logical(app, mx, my);

    if (m_menu.kind != M_NONE && in_rect(m_menu.rect, mx, my)) {
        const int rowh = app.mono.ch() + 4;
        const int visible = imax(1, (m_menu.rect.h - 2 * MENU_PAD) / rowh);
        m_menu.scroll -= dy;
        m_menu.scroll = std::max(0, std::min(m_menu.scroll, imax(0, (int)m_menu.items.size() - visible)));
        app.request_redraw();
        return true;
    }

    // wheel over a strip: fader nudge, or scroll its processor box
    for (int i = 0; i < strip_count(); ++i) {
        const SDL_Rect a = strip_area(i);
        if (!in_rect(a, mx, my)) continue;
        const Sub L = layout(app, i, a);
        if (show_inserts && in_rect(L.procs, mx, my)) {
            StripState& S = state(i);
            S.procScroll = std::max(0, S.procScroll - dy);
            app.request_redraw();
            return true;
        }
        // wheel over a send knob nudges its level, same steps as the fader
        if (i < bus_count) {
            const int aux_n = live_aux_count();
            for (int a = 0; a < aux_n; ++a) {
                int cx, cy, rr;
                if (send_pos(L, a, cx, cy, rr) && near_pt(mx, my, cx, cy, rr + 3)) {
                    const float step = ((SDL_GetModState() & KMOD_SHIFT) != 0) ? 0.005f : 0.02f;
                    const float nv = clamp01((get_send ? clamp01(get_send(i, a)) : 0.f)
                                             + step * float(dy));
                    if (set_send) set_send(i, a, nv);
                    app.request_redraw();
                    return true;
                }
            }
        }
        if (in_rect(L.fader, mx, my) || in_rect(L.meter, mx, my)) {
            float g = 1.f;
            if (is_aux(i)) g = engine_up() ? app_::audio_app_master_aux_return_gain(aux_of(i)) : 1.f;
            else if (get_gain) g = get_gain(i);
            else if (engine_up()) g = is_master(i) ? app_::audio_app_mixer_master_gain(node())
                                                   : app_::audio_app_mixer_gain(node(), i);
            const float step = ((SDL_GetModState() & KMOD_SHIFT) != 0) ? 0.005f : 0.02f;
            const float ng = slider_to_gain(clamp01(gain_to_slider(g) + step * float(dy)));
            if (is_aux(i)) { if (engine_up())
                app_::audio_app_master_aux_set_return_gain(aux_of(i), ng); }
            else if (set_gain) set_gain(i, ng);
            else if (engine_up()) {
                if (is_master(i)) app_::audio_app_mixer_set_master_gain(node(), ng);
                else              app_::audio_app_mixer_set_gain(node(), i, ng);
            }
            app.request_redraw();
            return true;
        }
        break;
    }

    if (content_w() <= rect.w) return false;
    m_scroll_x -= (dx != 0 ? dx : dy) * WHEEL_STEP;
    clamp_scroll();
    app.request_redraw();
    return true;
}

bool MasterMixerView::on_key(App& app, SDL_Keycode k) {
    if (m_menu.kind != M_NONE) {
        if (k == SDLK_ESCAPE) { m_menu = Menu(); app.request_redraw(); return true; }
        return false;
    }
    const int before = m_scroll_x;
    switch (k) {
    case SDLK_LEFT:  m_scroll_x -= WHEEL_STEP; break;
    case SDLK_RIGHT: m_scroll_x += WHEEL_STEP; break;
    case SDLK_HOME:  m_scroll_x = 0; break;
    case SDLK_END:   m_scroll_x = content_w(); break;
    case SDLK_DELETE: {
        // Delete the SELECTED processor (ProcessorBox ProcessorsDelete).
        //
        // This used to scan from strip 0 and delete the first strip that had a
        // selProc set.  selProc is per-strip and was only ever cleared for the
        // strip being acted on, so several strips could hold a live selection
        // at once and Delete then removed a plugin from a strip the user had
        // not touched in a long time -- destructive, silent, and with no undo.
        // The selection is now single and view-wide (select_proc), so Delete
        // acts on exactly the row that is drawn selected.
        const int i = m_sel_strip;
        if (i < 0 || i >= (int)m_strips.size()) return false;
        const int slot = m_strips[(size_t)i].selProc;
        if (slot < 0) { m_sel_strip = -1; return false; }
        app_::audio_app_master_insert_remove(chain_id(i), slot);
        clear_proc_selection();
        app.request_redraw();
        return true; }
    default: return false;
    }
    clamp_scroll();
    if (m_scroll_x != before) app.request_redraw();
    return true;
}

void MasterMixerView::cancel_interaction(App& app) {
    m_drag = D_NONE; m_drag_strip = -1; m_drag_aux = -1; m_drag_slot = -1;
    m_drag_moved = false;
    m_menu = Menu();
    m_hdr_click_idx = -1;
    // ...and any inline text edit (gain / rename / comment).  This is called
    // when the Mixer window is closed or minimised and when the app loses
    // focus.  Without it app.text_target stayed pointed at m_edit_buf: the
    // APP-WIDE keyboard was dead (the main loop tests text_target before it
    // dispatches keys anywhere), and Enter then renamed a strip in a window
    // that was no longer on screen.  end_text_if() only fires when OUR buffer
    // is the live one, so it can never cancel another view's edit.
    if (app.text_target == &m_edit_buf) {
        app.end_text_if(&m_edit_buf);       // drop the edit; closing != commit
        m_edit_kind = E_NONE; m_edit_strip = -1; m_edit_buf.clear();
    }
    app.request_redraw();
}

// One processor selection for the whole view.  Delete acts on it.
void MasterMixerView::clear_proc_selection() {
    for (StripState& s : m_strips) s.selProc = -1;
    m_sel_strip = -1;
}
void MasterMixerView::select_proc(int idx, int slot) {
    clear_proc_selection();
    if (idx < 0) return;
    state(idx).selProc = slot;
    m_sel_strip = slot >= 0 ? idx : -1;
}

} // namespace mixerui

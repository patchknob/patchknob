//----------------------------------------------------------------------------
//  sdlui/meter.cpp -- see meter.h for the provenance of every constant here.
//----------------------------------------------------------------------------
#include "meter.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ui {
namespace meter {

// ===========================================================================
//  libs/ardour/ardour/dB.h
// ===========================================================================
// Ardour's floor: dB_to_coefficient() returns exactly 0 below -318.8 dB, which
// is what makes -318 the "minus infinity" every meter value is initialised to.
static const float kMinDb = -318.f;

static inline float dB_to_coefficient(float dB) {
    return dB > -318.8f ? std::pow(10.0f, dB * 0.05f) : 0.0f;
}
static inline float accurate_coefficient_to_dB(float coeff) {
    if (coeff < 1e-15f) return -std::numeric_limits<float>::infinity();
    return 20.0f * std::log10(coeff);
}

// ===========================================================================
//  libs/ardour/ardour/logmeter.h -- the deflection curves, verbatim.
// ===========================================================================
static inline float log_meter(float db) {
    float def = 0.0f;                                  /* Meter deflection %age */
    if      (db < -70.0f) def = 0.0f;
    else if (db < -60.0f) def = (db + 70.0f) * 0.25f;
    else if (db < -50.0f) def = (db + 60.0f) * 0.5f  + 2.5f;
    else if (db < -40.0f) def = (db + 50.0f) * 0.75f + 7.5f;
    else if (db < -30.0f) def = (db + 40.0f) * 1.5f  + 15.0f;
    else if (db < -20.0f) def = (db + 30.0f) * 2.0f  + 30.0f;
    else if (db <   6.0f) def = (db + 20.0f) * 2.5f  + 50.0f;
    else                  def = 115.0f;
    /* 115 is the deflection %age at db=6.0; an arbitrary endpoint for the
       scaling, so the meter tops out at +6 dBFS. */
    return def / 115.0f;
}

static inline float meter_deflect_ppm(float db) {
    if (db < -30) {
        // 2.258 == ((-30 + 32.0)/ 28.0) / 10^(-30 / 20);
        return (dB_to_coefficient(db) * 2.258769757f);
    } else {
        const float rv = (db + 32.0f) / 28.0f;
        return rv < 1.0f ? rv : 1.0f;
    }
}

static inline float meter_deflect_din(float db) {
    float rv = dB_to_coefficient(db);
    rv = std::sqrt(std::sqrt(2.3676f * rv)) - 0.1803f;
    if (rv >= 1.0f) return 1.0f;
    return rv > 0 ? rv : 0.0f;
}

static inline float meter_deflect_nordic(float db) {
    if (db < -60) {
        return 0.0f;
    } else {
        const float rv = (db + 60.0f) / 54.0f;
        return rv < 1.0f ? rv : 1.0f;
    }
}

static inline float meter_deflect_vu(float db) {
    const float rv = 6.77165f * dB_to_coefficient(db);
    return rv > 1.0f ? 1.0f : rv;
}

static inline float meter_deflect_k(float db, float krange) {
    db += krange;
    if (db < -40.0f) {
        return (dB_to_coefficient(db) * 500.0f / (krange + 45.0f));
    } else {
        const float rv = (db + 45.0f) / (krange + 45.0f);
        return rv < 1.0f ? rv : 1.0f;
    }
}

float deflect(Type t, float db) {
    float v;
    switch (t) {
    case PPM:    v = meter_deflect_ppm    (db);      break;   // MeterIEC2BBC/EBU
    case DIN:    v = meter_deflect_din    (db);      break;   // MeterIEC1DIN
    case Nordic: v = meter_deflect_nordic (db);      break;   // MeterIEC1NOR
    case VU:     v = meter_deflect_vu     (db);      break;   // MeterVU
    case K20:    v = meter_deflect_k      (db, 20);  break;
    case K14:    v = meter_deflect_k      (db, 14);  break;
    case K12:    v = meter_deflect_k      (db, 12);  break;
    case Peak:
    default:     v = log_meter            (db);      break;   // MeterPeak/Krms
    }
    if (!(v > 0.f)) return 0.f;                                // also traps NaN
    return v < 1.f ? v : 1.f;
}

// gtk2_ardour/level_meter.cc::update_meters + meter_lineup()/vu_standard(),
// evaluated for Ardour's shipped defaults:
//   meter-line-up-level = MeteringLineUp18  -> meter_lineup(0)          =  0
//   meter-line-up-din   = MeteringLineUp15  -> meter_lineup_cfg(.,3.0)  =  0
//   meter-vu-standard   = MeteringVUstandard-> vu_standard()            = -6
float default_lineup_db(Type t) {
    switch (t) {
    case VU: return -6.0f;
    default: return 0.0f;
    }
}

// ===========================================================================
//  Colours -- gtk2_ardour/level_meter.cc::setup_meters()
// ===========================================================================
// The ten base colours are Ardour's "meter color0".."meter color9" from the
// default dark theme (gtk2_ardour/themes/dark-ardour.colors); setup_meters()
// then overrides some of them per meter type, which is reproduced below.
// Colours are 0xRRGGBB; the alpha byte of Ardour's 0xRRGGBBAA values is dropped
// because the meter pattern is opaque.
static const Uint32 kThemeColor[10] = {
    0x008800, 0x00aa00, 0x00ff00, 0x00ff00, 0xfff000,
    0xfff000, 0xff8800, 0xff8800, 0xff0000, 0xff0000
};

// The four gradient knees, in dB, that setup_meters() feeds to the type's
// deflection function (stp[i] = 115.0 * deflect(...), and generate_meter_pattern
// divides by 115 again, so the knee is simply the deflection).
struct TypeStyle {
    float  knee_db[4];
    Uint32 c[10];
};

static void build_style(Type t, TypeStyle& s) {
    for (int i = 0; i < 10; ++i) s.c[i] = kThemeColor[i];
    switch (t) {
    case K20:
        s.knee_db[0] = -40; s.knee_db[1] = -20; s.knee_db[2] = -18; s.knee_db[3] = -16;
        s.c[0] = s.c[1] = 0x008800; s.c[2] = s.c[3] = 0x00ff00;
        s.c[4] = s.c[5] = 0xffff00; s.c[6] = s.c[7] = 0xffff00;
        s.c[8] = s.c[9] = 0xff0000;
        break;
    case K14:
        s.knee_db[0] = -34; s.knee_db[1] = -14; s.knee_db[2] = -12; s.knee_db[3] = -10;
        s.c[0] = s.c[1] = 0x008800; s.c[2] = s.c[3] = 0x00ff00;
        s.c[4] = s.c[5] = 0xffff00; s.c[6] = s.c[7] = 0xffff00;
        s.c[8] = s.c[9] = 0xff0000;
        break;
    case K12:
        s.knee_db[0] = -32; s.knee_db[1] = -12; s.knee_db[2] = -10; s.knee_db[3] = -8;
        s.c[0] = s.c[1] = 0x008800; s.c[2] = s.c[3] = 0x00ff00;
        s.c[4] = s.c[5] = 0xffff00; s.c[6] = s.c[7] = 0xffff00;
        s.c[8] = s.c[9] = 0xff0000;
        break;
    case PPM:                                       // MeterIEC2EBU
        s.knee_db[0] = -24; s.knee_db[1] = -18; s.knee_db[2] = -9; s.knee_db[3] = 0;
        s.c[3] = s.c[2] = s.c[1];
        s.c[6] = s.c[7] = s.c[8] = s.c[9];
        break;
    case Nordic:                                    // MeterIEC1NOR
        s.knee_db[0] = -30; s.knee_db[1] = -18; s.knee_db[2] = -12; s.knee_db[3] = -9;
        s.c[0] = s.c[1] = s.c[2];                   // bright-green
        s.c[6] = s.c[7] = s.c[8] = s.c[9];
        break;
    case DIN:                                       // MeterIEC1DIN
        s.knee_db[0] = -29; s.knee_db[1] = -18; s.knee_db[2] = -15; s.knee_db[3] = -9;
        s.c[0] = s.c[2] = s.c[3] = s.c[1];
        s.c[4] = s.c[6];
        s.c[5] = s.c[7];
        break;
    case VU:
        s.knee_db[0] = -26; s.knee_db[1] = -23; s.knee_db[2] = -20; s.knee_db[3] = -18;
        s.c[0] = s.c[2] = s.c[3] = s.c[4] = s.c[5] = s.c[1];
        s.c[7] = s.c[8] = s.c[9] = s.c[6];
        break;
    case Peak:
    default:
        // stp[] = { 55.0, 77.5, 92.5, 100.0 } for MeteringLineUp18, which is
        // 115*log_meter() of -18/-9/-3/0 dBFS.
        s.knee_db[0] = -18; s.knee_db[1] = -9; s.knee_db[2] = -3; s.knee_db[3] = 0;
        break;
    }
}

// ---------------------------------------------------------------------------
//  libs/widgets/fastmeter.cc::generate_meter_pattern
// ---------------------------------------------------------------------------
// Ten colour stops in cairo space (0 = top of the widget, 1 = bottom).  Each
// knee is crossed twice, `soft` (3 px) apart, which is what turns the pairs of
// equal colours into the crisp band edges Ardour's meters have.
struct Gradient {
    float  pos[10];
    Uint32 rgb[10];
};

static void build_gradient(Type t, int height, Gradient& g) {
    TypeStyle st;
    build_style(t, st);

    const float soft =  3.0f / (float) height;
    const float offs = -1.0f / (float) height;

    float knee[4];
    for (int i = 0; i < 4; ++i) knee[i] = offs + deflect(t, st.knee_db[i]);

    g.pos[0] = 0.0f;                   g.rgb[0] = st.c[9];      // top / clip
    g.pos[1] = 1.0f - knee[3];         g.rgb[1] = st.c[8];      // 0 dB
    g.pos[2] = 1.0f - knee[3] + soft;  g.rgb[2] = st.c[7];
    g.pos[3] = 1.0f - knee[2];         g.rgb[3] = st.c[6];      // -3 dB
    g.pos[4] = 1.0f - knee[2] + soft;  g.rgb[4] = st.c[5];
    g.pos[5] = 1.0f - knee[1];         g.rgb[5] = st.c[4];      // -9 dB
    g.pos[6] = 1.0f - knee[1] + soft;  g.rgb[6] = st.c[3];
    g.pos[7] = 1.0f - knee[0];         g.rgb[7] = st.c[2];      // -18 dB
    g.pos[8] = 1.0f - knee[0] + soft;  g.rgb[8] = st.c[1];
    g.pos[9] = 1.0f;                   g.rgb[9] = st.c[0];      // bottom

    // Cairo tolerates out-of-order stops on a degenerate (very short) meter by
    // collapsing them; do the same explicitly so the lerp below stays sane.
    for (int i = 0; i < 10; ++i) {
        if (g.pos[i] < 0.f) g.pos[i] = 0.f;
        if (g.pos[i] > 1.f) g.pos[i] = 1.f;
        if (i && g.pos[i] < g.pos[i - 1]) g.pos[i] = g.pos[i - 1];
    }
}

static inline void lerp_rgb(Uint32 a, Uint32 b, float f, Uint8* out) {
    const float ar = float((a >> 16) & 0xff), ag = float((a >> 8) & 0xff), ab = float(a & 0xff);
    const float br = float((b >> 16) & 0xff), bg = float((b >> 8) & 0xff), bb = float(b & 0xff);
    out[0] = (Uint8) (ar + (br - ar) * f + 0.5f);
    out[1] = (Uint8) (ag + (bg - ag) * f + 0.5f);
    out[2] = (Uint8) (ab + (bb - ab) * f + 0.5f);
}

// Sample the stop list at cairo position p (0 = top).
static void gradient_sample(const Gradient& g, float p, Uint8* rgb) {
    if (p <= g.pos[0]) { lerp_rgb(g.rgb[0], g.rgb[0], 0.f, rgb); return; }
    for (int i = 1; i < 10; ++i) {
        if (p <= g.pos[i]) {
            const float span = g.pos[i] - g.pos[i - 1];
            const float f = span > 1e-9f ? (p - g.pos[i - 1]) / span : 1.f;
            lerp_rgb(g.rgb[i - 1], g.rgb[i], f, rgb);
            return;
        }
    }
    lerp_rgb(g.rgb[9], g.rgb[9], 0.f, rgb);
}

ui::Color color_at(float frac) {
    // Peak/RMS stops, on a nominal 128 px meter (the `soft`/`offs` terms scale
    // with the meter length, so a length has to be assumed for the free
    // function; draw() always uses the real one).
    static Gradient g;
    static bool built = false;
    if (!built) { build_gradient(Peak, 128, g); built = true; }
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    Uint8 rgb[3];
    gradient_sample(g, 1.0f - frac, rgb);           // frac is bottom-up
    return ui::Color{ rgb[0], rgb[1], rgb[2], 255 };
}

// ===========================================================================
//  Gradient texture cache -- the equivalent of FastMeter's pixbuf cache
//  (vm_pattern_cache / hm_pattern_cache, keyed on size + colours + stops).
// ===========================================================================
// Ardour's clamps: FastMeter::min/max_pattern_metric_size.
static const int kMaxMajor = 1024;
static const int kMinMajor = 4;
static const int kMinor    = 8;      // across the bar; the shade needs > 1 px
static const int kCacheN   = 24;

namespace {
struct CacheEntry {
    SDL_Renderer* ren  = nullptr;
    int           type = -1;
    int           major = 0;
    bool          vertical = true;
    SDL_Texture*  tex  = nullptr;
    Uint64        used = 0;
};
CacheEntry g_cache[kCacheN];
Uint64     g_clock = 0;
//! The ONE renderer every live entry belongs to.  The cache used to be keyed on
//! the renderer pointer alone and never flushed, so entries outlived the
//! renderer that created them: SDL_DestroyRenderer frees every texture it made,
//! leaving the cache holding dangling SDL_Texture*.  The next renderer then
//! either mismatched on the pointer and quietly wasted the whole table, or --
//! when the allocator handed back the SAME address -- matched and blitted freed
//! memory.  A pointer change now invalidates the table wholesale.
SDL_Renderer* g_cacheRen = nullptr;
// Staging buffer for a texture upload.  Static so building a gradient does not
// allocate either; 8 x 1024 x RGBA = 32 KiB.
Uint8 g_staging[kMinor * kMaxMajor * 4];
} // namespace

// Drop every entry WITHOUT touching the GPU.  For the case where the renderer
// that owned the textures is already gone: SDL_DestroyRenderer has freed them
// all, so destroying them again would be a double free.
static void forget_cache() {
    for (int i = 0; i < kCacheN; ++i) g_cache[i] = CacheEntry();
    g_clock = 0;
    g_cacheRen = nullptr;
}

void flush_cache() {
    for (int i = 0; i < kCacheN; ++i) {
        if (g_cache[i].tex) SDL_DestroyTexture(g_cache[i].tex);
        g_cache[i] = CacheEntry();
    }
    g_clock = 0;
    g_cacheRen = nullptr;
}

// The shade overlay from generate_meter_pattern (styleflags & 1): a horizontal
// wash across the bar that gives it its rounded look.  Ardour's stops are
// rgba(0,0,0,.15) at 0, rgba(1,1,1,.05) at 0.4 and rgba(0,0,0,.25) at 1.
static void shade_at(float x, float& sr, float& sg, float& sb, float& sa) {
    if (x < 0.4f) {
        const float f = x / 0.4f;
        sr = sg = sb = f;                       // black -> white
        sa = 0.15f + (0.05f - 0.15f) * f;
    } else {
        const float f = (x - 0.4f) / 0.6f;
        sr = sg = sb = 1.0f - f;                // white -> black
        sa = 0.05f + (0.25f - 0.05f) * f;
    }
}

static SDL_Texture* gradient_texture(SDL_Renderer* r, Type t, int major, bool vertical) {
    if (!r) return nullptr;
    if (major < kMinMajor) major = kMinMajor;
    if (major > kMaxMajor) major = kMaxMajor;

    // A different renderer than the cache was built against means the old one is
    // gone and took its textures with it.  Forget them rather than destroy them
    // (they are already freed) and start the table over.
    if (r != g_cacheRen) { forget_cache(); g_cacheRen = r; }

    int   oldest = 0;
    Uint64 oldestUse = ~(Uint64) 0;
    for (int i = 0; i < kCacheN; ++i) {
        CacheEntry& e = g_cache[i];
        if (e.tex && e.ren == r && e.type == (int) t && e.major == major &&
            e.vertical == vertical) {
            e.used = ++g_clock;
            return e.tex;
        }
        if (e.used < oldestUse) { oldestUse = e.used; oldest = i; }
    }

    // Miss: render the pattern.  This is the only place that touches the GPU
    // beyond a blit, and it happens once per (type, length, orientation).
    Gradient g;
    build_gradient(t, major, g);

    const int tw = vertical ? kMinor : major;
    const int th = vertical ? major  : kMinor;

    for (int j = 0; j < th; ++j) {
        for (int i = 0; i < tw; ++i) {
            // Position ALONG the bar, in cairo space (0 = top / right end).
            // generate_meter_pattern builds the vertical pattern and rotates it
            // by -90 degrees for horizontal meters, which puts the "top" colour
            // at the RIGHT end; the horizontal expose then fills left-to-right.
            const float along = vertical ? (float(j) + 0.5f) / float(th)
                                         : 1.0f - (float(i) + 0.5f) / float(tw);
            const float across = vertical ? (float(i) + 0.5f) / float(tw)
                                          : (float(j) + 0.5f) / float(th);
            Uint8 rgb[3];
            gradient_sample(g, along, rgb);

            float sr, sg, sb, sa;
            shade_at(across, sr, sg, sb, sa);
            Uint8* px = &g_staging[(j * tw + i) * 4];
            for (int k = 0; k < 3; ++k) {
                const float base = float(rgb[k]) / 255.f;
                const float s    = (k == 0 ? sr : (k == 1 ? sg : sb));
                float v = base * (1.f - sa) + s * sa;
                if (v < 0.f) v = 0.f;
                if (v > 1.f) v = 1.f;
                px[k] = (Uint8) (v * 255.f + 0.5f);
            }
            px[3] = 255;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, tw, th);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_UpdateTexture(tex, nullptr, g_staging, tw * 4);

    CacheEntry& e = g_cache[oldest];
    if (e.tex) SDL_DestroyTexture(e.tex);
    e.ren = r; e.type = (int) t; e.major = major; e.vertical = vertical;
    e.tex = tex; e.used = ++g_clock;
    return tex;
}

// ===========================================================================
//  Ballistics
// ===========================================================================
// METER_FALLOFF_MEDIUM (libs/ardour/ardour/utils.h).  The other rates in that
// table, should a caller want one: OFF 0, SLOWEST 6.6 (BBC), SLOW 8.6 (EBU),
// SLOWISH 12.0 (DIN), MODERATE 13.3 (EBU-PPM, Ardour's shipped default for the
// meter-falloff config variable), MEDIUM 20.0, FAST 32.0.
float falloff_db_per_sec = 20.0f;
// MeterHoldMedium = 100 refresh ticks (libs/ardour/ardour/types.h) and the GUI
// meter timer is Timers::super_rapid, 40 ms (gtk2_ardour/timers.cc).
int   hold_ms = 4000;

void reset(State& s) {
    s.peakDb = kMinDb;
    s.holdDb = kMinDb;
    s.holdFrames = 0;
    s.clipped = false;
}

void update(State& s, float peakLin, double dtSec) {
    if (!(dtSec > 0.0)) dtSec = 0.0;
    // Ardour's falloff runs in the audio thread, so a stalled GUI still comes
    // back to a correctly decayed level.  Here it is driven by the frame clock,
    // so cap the step: after a long stall (window hidden, a modal file dialog)
    // the meter resumes from a second ago instead of snapping to empty.
    if (dtSec > 1.0) dtSec = 1.0;

    if (!(peakLin > 0.f)) peakLin = 0.f;   // also traps NaN

    // libs/ardour/meter.cc::PeakMeter::run -- decay a fixed dB per second, then
    // take the max with this block's peak.  The attack is therefore instant and
    // the release is linear in dB; converting the rate with dtSec (rather than
    // using a fixed per-frame factor) is what makes it frame-rate independent.
    if (s.peakDb > -318.8f) s.peakDb -= float(falloff_db_per_sec * dtSec);
    if (!(s.peakDb > kMinDb)) s.peakDb = kMinDb;

    const float db = accurate_coefficient_to_dB(peakLin);
    if (db > s.peakDb) s.peakDb = db;
    if (s.peakDb > 200.f) s.peakDb = 200.f;             // absurd input guard

    // Sticky clip at >= 0 dBFS.  Ardour does this with FastMeter::set_highlight
    // driven by max_peak >= UIConfiguration::meter_peak (default 0.0 dBFS).
    if (peakLin >= 1.0f) s.clipped = true;

    // libs/widgets/fastmeter.cc::FastMeter::set -- a level at or above the held
    // peak re-arms the hold; when the hold expires the marker snaps back down
    // to the current level rather than jumping to zero.
    if (s.peakDb >= s.holdDb && s.peakDb > kMinDb) {
        s.holdDb = s.peakDb;
        s.holdFrames = hold_ms;
    }
    if (s.holdFrames > 0) {
        s.holdFrames -= (int) (dtSec * 1000.0 + 0.5);
        if (s.holdFrames <= 0) {
            s.holdFrames = 0;
            s.holdDb = s.peakDb;
        }
    }
}

// ===========================================================================
//  Drawing -- libs/widgets/fastmeter.cc::vertical_expose / horizontal_expose
// ===========================================================================
void draw(SDL_Renderer* r, const SDL_Rect& box, const State& s, Type t, bool vertical) {
    if (!r || box.w <= 2 || box.h <= 2) return;

    const Theme& th = theme();

    // Background.  Ardour paints a gradient here too, but PatchKnob is a
    // two-tone UI, so the unlit part of the meter uses the theme's well colour.
    // The clip highlight keeps Ardour's idea (FastMeter::set_highlight swaps the
    // background for the 0x991122 / 0x551111 red gradient) as a flat tint.
    const Color bg = s.clipped ? Color{ 0x77, 0x11, 0x22, 255 } : th.keybg;
    fill_rect(r, box, bg);

    const float level = deflect(t, s.peakDb);
    const float peak  = deflect(t, s.holdDb);

    const int major = vertical ? box.h : box.w;
    SDL_Texture* tex = gradient_texture(r, t, major, vertical);

    // The pattern spans the whole widget (Ardour generates it at pixheight+2);
    // the lit area is the inner box, inset by the 1 px border.
    const int texMajor = major < kMinMajor ? kMinMajor
                       : (major > kMaxMajor ? kMaxMajor : major);
    const float sc = float(texMajor) / float(major);
    const int texMinor = kMinor;

    if (vertical) {
        const int inner = box.h - 2;
        const int lit   = (int) std::floor(inner * level);
        if (tex && lit > 0) {
            const int dy = 1 + inner - lit;                 // widget-relative
            SDL_Rect src{ 0, (int) (dy * sc), texMinor,
                          (int) (lit * sc) < 1 ? 1 : (int) (lit * sc) };
            SDL_Rect dst{ box.x + 1, box.y + dy, box.w - 2, lit };
            SDL_RenderCopy(r, tex, &src, &dst);
        }
        // Peak-hold bar: a 2 px slice of the same pattern at the held level.
        if (tex && s.holdFrames > 0 && peak > 0.f) {
            int y = 1 + inner - (int) std::floor(inner * peak);
            if (y < 1) y = 1;
            int h = inner - y - 1;
            if (h > 2) h = 2;
            if (h > 0) {
                SDL_Rect src{ 0, (int) (y * sc), texMinor,
                              (int) (h * sc) < 1 ? 1 : (int) (h * sc) };
                SDL_Rect dst{ box.x + 1, box.y + y, box.w - 2, h };
                SDL_RenderCopy(r, tex, &src, &dst);
            }
        }
        if (s.clipped) {                                    // clip cap
            SDL_Rect cap{ box.x + 1, box.y + 1, box.w - 2, box.h > 8 ? 2 : 1 };
            fill_rect(r, cap, Color{ 0xff, 0x00, 0x00, 255 });
        }
    } else {
        const int inner = box.w - 2;
        const int lit   = (int) std::floor(inner * level);
        if (tex && lit > 0) {
            SDL_Rect src{ 0, 0, (int) (lit * sc) < 1 ? 1 : (int) (lit * sc), texMinor };
            SDL_Rect dst{ box.x + 1, box.y + 1, lit, box.h - 2 };
            SDL_RenderCopy(r, tex, &src, &dst);
        }
        if (tex && s.holdFrames > 0 && peak > 0.f) {
            const int xpos = (int) std::floor(inner * peak);
            int w = xpos < 2 ? xpos : 2;
            int x = 1 + (xpos - w > 0 ? xpos - w : 0);
            if (w > 0) {
                SDL_Rect src{ (int) (x * sc), 0, (int) (w * sc) < 1 ? 1 : (int) (w * sc),
                              texMinor };
                SDL_Rect dst{ box.x + x, box.y + 1, w, box.h - 2 };
                SDL_RenderCopy(r, tex, &src, &dst);
            }
        }
        if (s.clipped) {
            SDL_Rect cap{ box.x + box.w - 2 - (box.w > 8 ? 1 : 0), box.y + 1,
                          box.w > 8 ? 2 : 1, box.h - 2 };
            fill_rect(r, cap, Color{ 0xff, 0x00, 0x00, 255 });
        }
    }
}

// ===========================================================================
//  Self-check
// ===========================================================================
namespace {
int  g_fails = 0;
void check(const char* what, float got, float want, float tol = 1e-5f) {
    if (std::fabs(got - want) > tol) {
        std::fprintf(stderr, "ui::meter selftest: %s = %.7f, expected %.7f\n",
                     what, got, want);
        ++g_fails;
    }
}
} // namespace

bool selftest() {
    g_fails = 0;

    // --- log_meter (Peak).  Deflection %ages divided by the 115 endpoint.
    check("deflect(Peak,  0dB)",   deflect(Peak,   0.f), 100.0f / 115.0f);
    check("deflect(Peak, -3dB)",   deflect(Peak,  -3.f),  92.5f / 115.0f);
    check("deflect(Peak, -9dB)",   deflect(Peak,  -9.f),  77.5f / 115.0f);
    check("deflect(Peak,-18dB)",   deflect(Peak, -18.f),  55.0f / 115.0f);
    check("deflect(Peak,-20dB)",   deflect(Peak, -20.f),  50.0f / 115.0f);
    check("deflect(Peak,-30dB)",   deflect(Peak, -30.f),  30.0f / 115.0f);
    check("deflect(Peak,-70dB)",   deflect(Peak, -70.f),   0.0f);
    check("deflect(Peak, +6dB)",   deflect(Peak,   6.f),   1.0f);

    // --- PPM: linear (db+32)/28 above -30 dB, exponential below; continuous.
    check("deflect(PPM, -4dB)",    deflect(PPM,   -4.f),  1.0f);
    check("deflect(PPM,-18dB)",    deflect(PPM,  -18.f),  14.0f / 28.0f);
    check("deflect(PPM,-30dB)",    deflect(PPM,  -30.f),   2.0f / 28.0f);
    // below -30 dB the PPM scale is exponential, NOT the linear leg continued
    check("deflect(PPM,-32dB)",    deflect(PPM,  -32.f),
          std::pow(10.f, -1.6f) * 2.258769757f, 1e-6f);
    // continuity across the -30 dB breakpoint
    check("deflect(PPM,-30.001)",  deflect(PPM, -30.001f), 2.0f / 28.0f, 1e-4f);

    // --- DIN: sqrt(sqrt(2.3676 * coeff)) - 0.1803, clamped.
    check("deflect(DIN,  0dB)",    deflect(DIN,   0.f),  1.0f);
    check("deflect(DIN, -9dB)",    deflect(DIN,  -9.f),
          std::sqrt(std::sqrt(2.3676f * std::pow(10.f, -0.45f))) - 0.1803f, 1e-5f);
    check("deflect(DIN,-60dB)",    deflect(DIN, -60.f),
          std::sqrt(std::sqrt(2.3676f * std::pow(10.f, -3.0f))) - 0.1803f, 1e-5f);
    // the curve reaches zero at ~-67 dBFS and is clamped below that
    check("deflect(DIN,-70dB)",    deflect(DIN, -70.f),  0.0f);

    // --- Nordic: (db+60)/54.
    check("deflect(Nordic,-60dB)", deflect(Nordic, -60.f), 0.0f);
    check("deflect(Nordic,-18dB)", deflect(Nordic, -18.f), 42.0f / 54.0f);
    check("deflect(Nordic, -6dB)", deflect(Nordic,  -6.f), 1.0f);

    // --- VU: 6.77165 * coeff, i.e. 0 VU at -20 dBFS in the un-offset space.
    check("deflect(VU,-20dB)",     deflect(VU, -20.f), 0.677165f, 1e-5f);
    check("deflect(VU,-40dB)",     deflect(VU, -40.f), 0.0677165f, 1e-5f);
    check("deflect(VU,  0dB)",     deflect(VU,   0.f), 1.0f);

    // --- K-system: 0 on the scale sits at -krange dBFS -> 45/(krange+45).
    check("deflect(K20,-20dB)",    deflect(K20, -20.f), 45.0f / 65.0f);
    check("deflect(K14,-14dB)",    deflect(K14, -14.f), 45.0f / 59.0f);
    check("deflect(K12,-12dB)",    deflect(K12, -12.f), 45.0f / 57.0f);
    check("deflect(K20,  0dB)",    deflect(K20,   0.f), 1.0f);

    // --- ballistics: instant attack, exactly falloff_db_per_sec of release.
    {
        const float saveRate = falloff_db_per_sec;
        falloff_db_per_sec = 20.0f;
        State s;
        update(s, 1.0f, 0.0);                       // 0 dBFS
        check("attack is instantaneous", s.peakDb, 0.0f, 1e-4f);
        if (!s.clipped) { std::fprintf(stderr, "ui::meter selftest: clip did not latch\n"); ++g_fails; }
        // one second of silence at 20 dB/s
        update(s, 0.0f, 1.0);
        check("falloff 20dB/s over 1s", s.peakDb, -20.0f, 1e-3f);
        // ... and the same drop split over 100 frames must land in the same place
        State f;
        update(f, 1.0f, 0.0);
        for (int i = 0; i < 100; ++i) update(f, 0.0f, 0.01);
        check("falloff is frame-rate independent", f.peakDb, s.peakDb, 1e-2f);
        // the hold marker stays at the peak while the level falls away
        check("hold holds", f.holdDb, 0.0f, 1e-4f);
        reset(s);
        check("reset clears", s.peakDb, -318.f);
        if (s.clipped) { std::fprintf(stderr, "ui::meter selftest: reset left clip set\n"); ++g_fails; }
        falloff_db_per_sec = saveRate;
    }

    // --- gradient: green at the bottom, red at the top.
    {
        const Color lo = color_at(0.05f), hi = color_at(1.0f);
        if (!(lo.g > lo.r && lo.g > lo.b)) {
            std::fprintf(stderr, "ui::meter selftest: bottom of the gradient is not green\n");
            ++g_fails;
        }
        if (!(hi.r > 200 && hi.g < 80)) {
            std::fprintf(stderr, "ui::meter selftest: top of the gradient is not red\n");
            ++g_fails;
        }
    }

    return g_fails == 0;
}

} // namespace meter
} // namespace ui

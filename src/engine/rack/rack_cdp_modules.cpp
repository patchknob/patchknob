//----------------------------------------------------------------------------
//  src/engine/rack/rack_cdp_modules.cpp
//
//  Every STREAMABLE ported CDP process, exposed as a rack module.
//
//  These are generated from the CDP registry rather than hand-written: one
//  module type per process whose `streamable` flag is set, with a knob per
//  declared parameter.  A new port therefore becomes a playable rack module the
//  moment it lands, with no work here.
//
//  ON LATENCY -- the honest part.  A CDP process transforms a whole buffer; it
//  has no block-wise entry point.  So this module accumulates `kChunk` frames,
//  runs the process over that chunk, and plays the result out.  That costs one
//  chunk of delay, on top of whatever the algorithm itself reports through
//  Process::latencyFrames (a waveset lookahead, a PVOC analysis window).  Both
//  are summed and published through getLatency(), so the host can delay
//  compensate instead of the module quietly sitting late in the mix.
//
//  Processes that are NOT streamable are deliberately absent: an algorithm that
//  has to scan the whole file, or that reorders material across it, cannot be
//  made to work on a chunk without lying about what it does.  Those stay in the
//  offline editor where they belong.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "cdp_process.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;
namespace cdp = PatchKnob::cdp;

// One chunk of delay is the price of hosting a whole-buffer transform in a
// realtime graph.  4096 frames is ~85 ms at 48k: long enough that the waveset
// families see whole cycles, short enough to stay usable.
constexpr int kChunk = 4096;

struct CdpModule final : Module {
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    std::string slug;
    const cdp::Process* proc = nullptr;

    // in[] fills until it holds kChunk frames; then the process runs and its
    // result is appended to out[], which drains one frame per sample.
    std::vector<float> inBuf, outBuf;
    size_t outPos = 0;
    std::vector<double> lastParams;
    float rate = 48000.f;
    // Zero-latency path: processes that are genuinely sample-by-sample hand us
    // a live Stream instead, and we run it straight through with no buffering.
    std::unique_ptr<cdp::Stream> stream;
    bool  paramsDirty = true;

    explicit CdpModule(const cdp::Process& p) : slug(p.slug), proc(&p) {
        config((int)p.params.size(), NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (size_t i = 0; i < p.params.size(); ++i) {
            const cdp::ParamSpec& s = p.params[i];
            auto* q = configParam((int)i, (float)s.min, (float)s.max, (float)s.def,
                                  s.name, s.unit);
            if (q && s.integer) q->snapEnabled = true;
        }
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
        inBuf.reserve(kChunk);
        lastParams.assign(p.params.size(), 0.0);
    }

    //! Zero when the process streams; otherwise a chunk plus its own delay.
    int64_t latencyFrames() const {
        if (proc && proc->makeStream) {
            std::vector<double> pr(params.size());
            for (size_t i = 0; i < params.size(); ++i) pr[i] = params[i].getValue();
            return proc->latencyFrames ? proc->latencyFrames((int)rate, pr) : 0;
        }
        int64_t own = 0;
        if (proc && proc->latencyFrames) {
            std::vector<double> pr(params.size());
            for (size_t i = 0; i < params.size(); ++i) pr[i] = params[i].getValue();
            own = proc->latencyFrames((int)rate, pr);
        }
        return (int64_t)kChunk + own;
    }

    void onReset() override {
        inBuf.clear(); outBuf.clear(); outPos = 0;
        if (stream) stream->reset();
        paramsDirty = true;
    }

    void process(const ProcessArgs& args) override {
        rate = args.sampleRate;

        // ---- streaming path: no added latency at all ----------------------
        if (proc && proc->makeStream) {
            std::vector<double> pr(params.size());
            bool changed = !stream || paramsDirty || lastParams.size() != pr.size();
            for (size_t i = 0; i < params.size(); ++i) {
                pr[i] = params[i].getValue();
                if (!changed && pr[i] != lastParams[i]) changed = true;
            }
            if (changed) {
                // Rebuilding allocates, so it happens only when a knob actually
                // moved -- not per sample.  State is carried by the new
                // instance's reset(), which is what CDP does on a coefficient
                // change too.
                stream = proc->makeStream((int)args.sampleRate, pr);
                lastParams = pr;
                paramsDirty = false;
            }
            float v = inputs[IN_INPUT].getVoltage() * 0.2f;
            if (stream) { float* chp[1] = { &v }; stream->process(chp, 1, 1); }
            if (!std::isfinite(v)) v = 0.f;
            outputs[OUT_OUTPUT].setVoltage(rack::clamp(v * 5.f, -10.f, 10.f));
            outputs[OUT_OUTPUT].channels = 1;
            return;
        }

        inBuf.push_back(inputs[IN_INPUT].getVoltage() * 0.2f);   // +/-5V -> +/-1
        if ((int)inBuf.size() >= kChunk) {
            std::vector<double> pr(params.size());
            for (size_t i = 0; i < params.size(); ++i) pr[i] = params[i].getValue();

            cdp::Buffer in;
            in.sampleRate = (int)args.sampleRate;
            in.ch.assign(1, inBuf);

            cdp::Buffer res; std::string err;
            const bool ok = proc && proc->run && proc->run({in}, pr, res, err, {});
            // A refusal must not kill the audio: pass the chunk through dry
            // rather than dropping to silence or repeating stale output.
            const std::vector<float>& src =
                (ok && !res.empty()) ? res.ch[0] : inBuf;

            // Drop what has already been played, keep the rest queued.
            if (outPos > 0 && outPos <= outBuf.size())
                outBuf.erase(outBuf.begin(), outBuf.begin() + (ptrdiff_t)outPos);
            outPos = 0;
            outBuf.insert(outBuf.end(), src.begin(), src.end());
            // A length-changing process could grow this without bound; cap the
            // queue at a few chunks so a bad parameter cannot eat memory.
            if (outBuf.size() > (size_t)kChunk * 8)
                outBuf.erase(outBuf.begin(),
                             outBuf.begin() + (ptrdiff_t)(outBuf.size() - (size_t)kChunk * 8));
            inBuf.clear();
        }

        float v = 0.f;
        if (outPos < outBuf.size()) v = outBuf[outPos++];
        if (!std::isfinite(v)) v = 0.f;
        outputs[OUT_OUTPUT].setVoltage(rack::clamp(v * 5.f, -10.f, 10.f));
        outputs[OUT_OUTPUT].channels = 1;
    }
};

rackx::PanelElement el(int id, float x, float y, float r,
                       rackx::PanelControlStyle style, const std::string& label,
                       rackx::PanelLabelPlacement place = rackx::PanelLabelPlacement::Below) {
    rackx::PanelElement e;
    e.id = id; e.x = x; e.y = y; e.radius = r;
    e.style = style; e.label = label; e.labelPlacement = place;
    return e;
}

//! Panel sized to the parameter count: a knob column plus the two jacks.
//! EVERY parameter must get a control -- a fixed row pitch silently dropped the
//! last knobs of a six-parameter process, leaving them unreachable.  The pitch
//! and knob size shrink to fit instead, and two columns are used once one would
//! be too cramped.
rackx::PanelSpec cdpPanel(const cdp::Process& p) {
    const int rows = (int)p.params.size();
    const int cols = rows > 6 ? 2 : 1;
    const int perCol = cols > 1 ? (rows + 1) / 2 : rows;
    const int hp = cols > 1 ? 12 : (rows > 4 ? 10 : 8);
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(hp);

    const float top = 58.f, bottom = 300.f;
    const float pitch = perCol > 1
        ? std::min(52.f, (bottom - top) / (float)(perCol - 1)) : 0.f;
    const float radius = std::min(14.f, std::max(7.f, pitch * 0.26f));
    for (int i = 0; i < rows; ++i) {
        const int col = cols > 1 ? i / perCol : 0;
        const int row = cols > 1 ? i % perCol : i;
        const float cx = cols > 1 ? panel.width * (col == 0 ? 0.28f : 0.72f)
                                  : panel.width * 0.5f;
        panel.params.push_back(el(i, cx, top + pitch * (float)row, radius,
                                  rackx::PanelControlStyle::Knob,
                                  p.params[(size_t)i].name));
    }
    const float cx = panel.width * 0.5f;
    panel.inputs  = { el(CdpModule::IN_INPUT,  cx - 26.f, 330.f, 9.f,
                         rackx::PanelControlStyle::Knob, "IN") };
    panel.outputs = { el(CdpModule::OUT_OUTPUT, cx + 26.f, 330.f, 9.f,
                         rackx::PanelControlStyle::Knob, "OUT") };
    return panel;
}

} // namespace

namespace rackx {

void registerCdpModules() {
    for (const cdp::Process& p : cdp::registry()) {
        if (!p.streamable) continue;          // offline-only stays offline
        const cdp::Process* ptr = &p;
        addType("CDP." + p.slug, p.name, "CDP / " + p.group, Role::Normal,
                [ptr] { return std::make_unique<CdpModule>(*ptr); }, cdpPanel(p));
    }
}

} // namespace rackx

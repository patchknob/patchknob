//----------------------------------------------------------------------------
//  src/engine/rack/rack_engine.cpp
//----------------------------------------------------------------------------
#include "rack_engine.h"

#include <algorithm>
#include <cmath>

namespace rackx {

using rack::engine::Module;

RackEngine::RackEngine() { registerBuiltins(); }
RackEngine::~RackEngine() = default;

// ---- helpers ---------------------------------------------------------------
RackModule* RackEngine::moduleById(int id) {
    for (auto& m : mods_) if (m.id == id) return &m;
    return nullptr;
}

void RackEngine::refreshRoles() {
    // Resolve role modules to Module* ONCE per edit; the audio path then reads
    // cached pointers with zero per-sample search.  Also rebuild the dense
    // process list so stepSample iterates plain pointers (no null checks, no
    // RackModule indirection) -- both matter at ~500 modules.
    audioOutMods_.clear(); audioInMods_.clear(); midiCvMods_.clear();
    procMods_.clear();
    procMods_.reserve(mods_.size());
    for (auto& m : mods_) {
        if (!m.mod) continue;
        procMods_.push_back(m.mod.get());
        switch (m.role) {
            case Role::AudioOut: audioOutMods_.push_back(m.mod.get()); break;
            case Role::AudioIn:  audioInMods_.push_back(m.mod.get());  break;
            case Role::MidiCV:   midiCvMods_.push_back(m.mod.get());   break;
            default: break;
        }
    }
}

int RackEngine::audioChannelIndex(int moduleId) const {
    // Mirror refreshRoles' ordering: 1-based position among same-role audio modules
    // in mods_ order == the host node's port order for that module.
    Role want = Role::Normal;
    for (const auto& m : mods_)
        if (m.id == moduleId) { want = m.role; break; }
    if (want != Role::AudioOut && want != Role::AudioIn) return 0;
    int idx = 0;
    for (const auto& m : mods_)
        if (m.role == want) { ++idx; if (m.id == moduleId) return idx; }
    return 0;
}

void RackEngine::rebuildCables() {
    // Resolve every cable's endpoints to port pointers.  O(cables x modules)
    // is fine here -- this runs on the GUI thread once per edit, never on the
    // audio thread.  Out-of-range/dangling cables are simply not compiled.
    compiledCables_.clear();
    compiledCables_.reserve(cables_.size());
    for (auto& module : mods_) {
        if (!module.mod) continue;
        // Clear the SIGNAL as well as the cable flag.  isConnected() is
        // `connected || channels > 0`, so an input whose source module was just
        // deleted (removeModule, setModuleScript's cable pruning, clear())
        // still read as connected and kept returning the dead module's LAST
        // sample forever -- delete an envelope mid-performance and the VCA it
        // fed stayed latched at whatever gain it happened to be at.  Only
        // removeCable() was doing this, and only for its one endpoint.
        // Inputs that still have a cable are refilled by the very next
        // stepSample(), so this costs nothing for live connections.
        for (auto& input : module.mod->inputs) { input.connected = false; input.setChannels(0); }
        for (auto& output : module.mod->outputs) output.connected = false;
    }
    for (const auto& c : cables_) {
        RackModule* s = nullptr; RackModule* d = nullptr;
        for (auto& m : mods_) { if (m.id == c.fromMod) s = &m; if (m.id == c.toMod) d = &m; }
        if (!s || !d || !s->mod || !d->mod) continue;
        if (c.outPort < 0 || c.outPort >= (int)s->mod->outputs.size()) continue;
        if (c.inPort  < 0 || c.inPort  >= (int)d->mod->inputs.size())  continue;
        CompiledCable cc;
        cc.out = &s->mod->outputs[c.outPort];
        cc.in  = &d->mod->inputs[c.inPort];
        cc.out->connected = true;
        cc.in->connected = true;
        compiledCables_.push_back(cc);
    }
}

int RackEngine::inputCableCount(int moduleId, int inPort) const {
    int n = 0;
    for (const auto& c : cables_) if (c.toMod == moduleId && c.inPort == inPort) ++n;
    return n;
}

// ---- lifecycle -------------------------------------------------------------
void RackEngine::setSampleRate(double sr) {
    std::lock_guard<std::mutex> lk(mtx_);
    sr_ = sr;
    for (auto& m : mods_) if (m.mod) m.mod->onSampleRateChange((float)sr);
}

int RackEngine::ensureDefaultIO() {
    for (auto& m : mods_) if (m.role == Role::AudioOut) return m.id;
    return addModule("AudioOut", 380.f, 40.f);
}

// ---- structural edits ------------------------------------------------------
// Build a default editable panel from a module's ports: knobs in rows near the top,
// input jacks down the left, output jacks down the right.  Used for scripting
// modules (Pd/Csound), which have no fixed factory panel.
static void build_default_panel(rack::engine::Module* d, PanelSpec& panel) {
    if (!d) return;
    const int nP = (int)d->params.size(), nI = (int)d->inputs.size(), nO = (int)d->outputs.size();
    int cols = nP; if (nI > cols) cols = nI; if (nO > cols) cols = nO;
    int hp = 6 + (cols > 3 ? (cols - 3) * 2 : 0); if (hp > 30) hp = 30;
    panel = PanelSpec::fromHp(hp);                     // width = hp*HP, height = 380
    const float W = panel.width;
    auto mk = [](int id, float x, float y, float r, PanelControlStyle st, std::string lbl) {
        PanelElement e; e.id = id; e.x = x; e.y = y; e.radius = r; e.style = st;
        e.label = std::move(lbl); e.labelPlacement = PanelLabelPlacement::Below; return e;
    };
    const int perRow = (int)(W / 42.f) < 1 ? 1 : (int)(W / 42.f);
    for (int i = 0; i < nP; ++i) {
        const int row = i / perRow, col = i % perRow;
        float x = 24.f + col * 42.f; if (x > W - 18.f) x = W - 18.f;
        panel.params.push_back(mk(i, x, 60.f + row * 62.f, 14.f, PanelControlStyle::Knob,
            i < (int)d->paramQuantities.size() ? d->paramQuantities[i].name : std::string()));
    }
    for (int i = 0; i < nI; ++i) {
        float y = 250.f + i * 34.f; if (y > 360.f) y = 360.f;
        panel.inputs.push_back(mk(i, 22.f, y, 9.f, PanelControlStyle::Knob,
            i < (int)d->inputInfos.size() ? d->inputInfos[i] : std::string()));
    }
    for (int i = 0; i < nO; ++i) {
        float y = 250.f + i * 34.f; if (y > 360.f) y = 360.f;
        panel.outputs.push_back(mk(i, W - 22.f, y, 9.f, PanelControlStyle::Knob,
            i < (int)d->outputInfos.size() ? d->outputInfos[i] : std::string()));
    }
}

int RackEngine::addModule(const std::string& slug, float x, float y) {
    const ModuleType* t = findType(slug);
    if (!t || !t->make) return -1;
    auto dsp = t->make();
    if (!dsp) return -1;
    dsp->onSampleRateChange((float)sr_);

    std::lock_guard<std::mutex> lk(mtx_);
    RackModule rm;
    rm.id = nextId_++;
    rm.slug = t->slug; rm.name = t->name; rm.category = t->category; rm.role = t->role;
    rm.x = x; rm.y = y; rm.mod = std::move(dsp);
    // scripting modules (Pd/Csound) get an editable per-instance panel from their ports
    if (auto* sm = dynamic_cast<IScriptModule*>(rm.mod.get())) {
        build_default_panel(rm.mod.get(), rm.panel);
        sm->setPolyphony(polyphony_);
    }
    int id = rm.id;
    mods_.push_back(std::move(rm));
    refreshRoles();
    return id;
}

void RackEngine::removeModule(int moduleId) {
    std::lock_guard<std::mutex> lk(mtx_);
    // drop cables that touch it
    cables_.erase(std::remove_if(cables_.begin(), cables_.end(),
                  [&](const RackCable& c){ return c.fromMod == moduleId || c.toMod == moduleId; }),
                  cables_.end());
    mods_.erase(std::remove_if(mods_.begin(), mods_.end(),
                [&](const RackModule& m){ return m.id == moduleId; }), mods_.end());
    refreshRoles();
    rebuildCables();
}

void RackEngine::moveModule(int moduleId, float x, float y) {
    // position only affects the editor, not audio -> no lock needed.
    if (RackModule* m = moduleById(moduleId)) { m->x = x; m->y = y; }
}

// ---- scripting modules (Pd / Csound) ---------------------------------------
// The panel is GUI-thread-only (drawn by the editor, never touched by process()),
// so these need no lock; setModuleScript does, since it reshapes the port vectors.
PanelSpec* RackEngine::modulePanel(int moduleId) {
    RackModule* m = moduleById(moduleId);
    return (m && m->panel.valid()) ? &m->panel : nullptr;
}
bool RackEngine::isScriptModule(int moduleId) const {
    for (const auto& m : mods_) if (m.id == moduleId)
        return m.mod && dynamic_cast<const IScriptModule*>(m.mod.get()) != nullptr;
    return false;
}
std::string RackEngine::moduleScript(int moduleId) const {
    for (const auto& m : mods_) if (m.id == moduleId && m.mod)
        if (auto* sm = dynamic_cast<const IScriptModule*>(m.mod.get())) return sm->script();
    return std::string();
}
const char* RackEngine::moduleScriptKind(int moduleId) const {
    for (const auto& m : mods_) if (m.id == moduleId && m.mod)
        if (auto* sm = dynamic_cast<const IScriptModule*>(m.mod.get())) return sm->scriptKind();
    return "";
}
std::string RackEngine::moduleScriptError(int moduleId) const {
    for (const auto& m : mods_) if (m.id == moduleId && m.mod)
        if (auto* sm = dynamic_cast<const IScriptModule*>(m.mod.get())) return sm->lastError();
    return std::string();
}
bool RackEngine::setModuleScript(int moduleId, const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    RackModule* m = moduleById(moduleId);
    if (!m || !m->mod) return false;
    auto* sm = dynamic_cast<IScriptModule*>(m->mod.get());
    if (!sm) return false;
    // setScript() may resize Module::inputs/outputs. CompiledCable stores direct
    // pointers into those vectors, so invalidate it before any resize rather
    // than leaving dangling endpoints during compilation/reconfiguration.
    compiledCables_.clear();
    sm->setScript(text);                                  // re-scans; port vectors may resize
    const int nI = (int)m->mod->inputs.size(), nO = (int)m->mod->outputs.size();
    cables_.erase(std::remove_if(cables_.begin(), cables_.end(), [&](const RackCable& c){
        return (c.fromMod == moduleId && c.outPort >= nO) ||
               (c.toMod   == moduleId && c.inPort  >= nI); }), cables_.end());
    build_default_panel(m->mod.get(), m->panel);          // fresh layout for the new ports
    refreshRoles();
    rebuildCables();
    return true;
}

int RackEngine::addCable(int fromMod, int outPort, int toMod, int inPort) {
    std::lock_guard<std::mutex> lk(mtx_);
    RackModule* s = nullptr; RackModule* d = nullptr;
    for (auto& m : mods_) { if (m.id == fromMod) s = &m; if (m.id == toMod) d = &m; }
    if (!s || !d || !s->mod || !d->mod) return -1;
    if (outPort < 0 || outPort >= (int)s->mod->outputs.size()) return -1;
    if (inPort  < 0 || inPort  >= (int)d->mod->inputs.size())  return -1;

    // one cable per input: remove any existing cable feeding (toMod,inPort)
    cables_.erase(std::remove_if(cables_.begin(), cables_.end(),
                  [&](const RackCable& c){ return c.toMod == toMod && c.inPort == inPort; }),
                  cables_.end());
    RackCable c;
    c.id = nextId_++; c.fromMod = fromMod; c.outPort = outPort; c.toMod = toMod; c.inPort = inPort;
    int id = c.id;
    cables_.push_back(c);
    rebuildCables();
    return id;
}

void RackEngine::removeCable(int cableId) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = cables_.begin(); it != cables_.end(); ++it) {
        if (it->id == cableId) {
            // clear the (now unfed) destination input
            if (RackModule* d = moduleById(it->toMod))
                if (d->mod && it->inPort < (int)d->mod->inputs.size())
                    d->mod->inputs[it->inPort].setChannels(0);
            cables_.erase(it);
            rebuildCables();
            return;
        }
    }
}

void RackEngine::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    cables_.clear(); mods_.clear(); refreshRoles(); rebuildCables();
}

// ---- param write (lock-free; benign float race with the audio reader) ------
void RackEngine::setParam(int moduleId, int paramId, float value) {
    if (RackModule* m = moduleById(moduleId))
        if (m->mod && paramId >= 0 && paramId < (int)m->mod->params.size())
        {
            if (paramId < (int)m->mod->paramQuantities.size()) {
                const rack::engine::ParamQuantity& quantity = m->mod->paramQuantities[paramId];
                value = rack::clamp(value, quantity.minValue, quantity.maxValue);
                if (quantity.snapEnabled) value = std::round(value);
            }
            m->mod->params[paramId].value = value;
        }
}
float RackEngine::getParam(int moduleId, int paramId) const {
    for (const auto& m : mods_)
        if (m.id == moduleId && m.mod && paramId >= 0 && paramId < (int)m.mod->params.size())
            return m.mod->params[paramId].value;
    return 0.f;
}
int RackEngine::moduleParamCount(int moduleId) const {
    for (const auto& m : mods_)
        if (m.id == moduleId && m.mod) return (int)m.mod->params.size();
    return 0;
}

bool RackEngine::resetModule(int moduleId) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (RackModule& module : mods_) {
        if (module.id != moduleId || !module.mod) continue;
        module.mod->onReset();
        return true;
    }
    return false;
}

// ---- realtime --------------------------------------------------------------
// ---- polyphony / voice allocation ------------------------------------------
void RackEngine::setPolyphony(int n) {
    n = (n < 1) ? 1 : (n > 16 ? 16 : n);
    std::lock_guard<std::mutex> lk(mtx_);
    polyphony_ = n;
    for (auto& m : mods_)
        if (m.mod)
            if (auto* sm = dynamic_cast<IScriptModule*>(m.mod.get())) sm->setPolyphony(n);
    if (rrNext_ >= polyphony_) rrNext_ = 0;
    for (int i = polyphony_; i < 16; ++i) voices_[i] = Voice{};   // silence dropped voices
}

int RackEngine::allocVoice() {
    // Self-defending clamp: even if polyphony_ was corrupted past the setter,
    // never index voices_[] outside its 16 slots.
    const int poly = polyphony_ < 1 ? 1 : (polyphony_ > 16 ? 16 : polyphony_);
    if (rrNext_ >= poly) rrNext_ = 0;
    // Prefer a released voice; otherwise steal round-robin (oldest reuse).
    for (int i = 0; i < poly; ++i) {
        int c = (rrNext_ + i) % poly;
        if (voices_[c].gateV < 5.f) { rrNext_ = (c + 1) % poly; return c; }
    }
    int c = rrNext_; rrNext_ = (rrNext_ + 1) % poly; return c;
}

void RackEngine::pushHeld(int note, int vel) {
    // A repeated note-on for an already-held note (no note-off arrived in
    // between) updates velocity in place rather than pushing a duplicate, so
    // the stack stays one entry per physically-down key.
    for (int i = 0; i < heldCount_; ++i)
        if (heldNote_[i] == note) { heldVel_[i] = vel; return; }
    if (heldCount_ < kMaxHeld) { heldNote_[heldCount_] = note; heldVel_[heldCount_] = vel; ++heldCount_; }
}

void RackEngine::popHeld(int note) {
    for (int i = 0; i < heldCount_; ++i)
        if (heldNote_[i] == note) {
            for (int j = i; j + 1 < heldCount_; ++j) { heldNote_[j] = heldNote_[j + 1]; heldVel_[j] = heldVel_[j + 1]; }
            --heldCount_;
            return;
        }
}

bool RackEngine::noteHasVoice(int note) const {
    const int poly = polyphony_ < 1 ? 1 : (polyphony_ > 16 ? 16 : polyphony_);
    for (int c = 0; c < poly; ++c)
        if (voices_[c].note == note && voices_[c].gateV > 0.f) return true;
    return false;
}

void RackEngine::noteOn(int note, int vel) {
    pushHeld(note, vel);
    int c = allocVoice();
    voices_[c].note   = note;
    voices_[c].pitchV = (note - 60) / 12.f;        // 1V/oct, 0V == C4
    voices_[c].velV   = (vel / 127.f) * 10.f;
    voices_[c].gateV  = 10.f;
}

void RackEngine::noteOff(int note) {
    popHeld(note);
    const int poly = polyphony_ < 1 ? 1 : (polyphony_ > 16 ? 16 : polyphony_);
    for (int c = 0; c < poly; ++c) {
        if (voices_[c].note != note || voices_[c].gateV <= 0.f) continue;
        voices_[c].gateV = 0.f;
        // This voice just freed.  If an earlier note lost its voice to
        // stealing and is still physically held, re-sound it now (last-note-
        // priority recovery) instead of leaving it silent until some
        // unrelated voice happens to free up.
        for (int i = heldCount_ - 1; i >= 0; --i) {
            if (noteHasVoice(heldNote_[i])) continue;
            voices_[c].note   = heldNote_[i];
            voices_[c].pitchV = (heldNote_[i] - 60) / 12.f;
            voices_[c].velV   = (heldVel_[i] / 127.f) * 10.f;
            voices_[c].gateV  = 10.f;
            break;
        }
    }
}

void RackEngine::stepSample(const Module::ProcessArgs& args) {
    // AudioIn/MidiCV module outputs were already driven for this sample by the
    // caller (process(), which has the per-sample inL/inR + MIDI voice).
    // 1. propagate cables (source output -> dest input).  Module->module signals
    //    carry the source's PREVIOUS sample (1-sample delay -> feedback-stable);
    //    I/O sources (AudioIn/MidiCV) were set THIS sample just above/in process.
    //    Endpoints were resolved at edit time (rebuildCables), so this is pure
    //    pointer copies -- no by-id module scan in the hot loop.
    for (const CompiledCable& c : compiledCables_) {
        const auto& o = *c.out;
        auto& in = *c.in;
        int ch = o.channels ? o.channels : 1;
        // Port::channels is a public uint8_t a module can set past 16 without
        // going through setChannels; clamp here so a corrupt count can never
        // index outside the 16-slot voltages[] on either port.
        if (ch > rack::engine::PORT_MAX_CHANNELS) ch = rack::engine::PORT_MAX_CHANNELS;
        in.channels = (uint8_t)ch;
        for (int k = 0; k < ch; ++k) in.voltages[k] = o.voltages[k];
    }
    // 2. process every module (dense pointer list, rebuilt per edit).
    for (Module* m : procMods_) m->process(args);
}

void RackEngine::process(int nframes,
                         const float* inL, const float* inR,
                         float* outL, float* outR,
                         const PatchKnob::engine::MidiEvent* midi, int numMidi,
                         float hostTempoBpm, bool hostPlaying,
                         int64_t hostPlayPositionSamples) {
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) {                    // a structural edit is in flight
        if (outL) std::fill(outL, outL + nframes, 0.f);
        if (outR) std::fill(outR, outR + nframes, 0.f);
        return;
    }

    Module::ProcessArgs args;
    args.sampleRate = (float)sr_;
    args.sampleTime = (float)(1.0 / sr_);
    args.tempoBpm = hostTempoBpm;
    args.isPlaying = hostPlaying;

    // Clamp BOTH ends at the read site (voices_[] has exactly 16 slots): the
    // audio path stays in bounds even if polyphony_ was set without the setter.
    const int poly = polyphony_ < 1 ? 1 : (polyphony_ > 16 ? 16 : polyphony_);
    int ei = 0;
    for (int i = 0; i < nframes; ++i) {
        args.frame = hostPlayPositionSamples >= 0
                   ? hostPlayPositionSamples + i : frame_;
        // apply MIDI events landing at/behind this sample -> poly voices
        while (ei < numMidi && midi && midi[ei].sampleOffset <= i) {
            const auto& e = midi[ei++];
            const uint8_t type = e.status & 0xF0;
            if (type == 0x90 && e.data2 > 0)                          noteOn(e.data1, e.data2);
            else if ((type == 0x80) || (type == 0x90 && e.data2 == 0)) noteOff(e.data1);
            else if (type == 0xB0 && (e.data1 == 120 || e.data1 == 123))
                for (int n=0;n<128;++n) noteOff(n);
        }

        // drive AudioIn + MidiCV module outputs for THIS sample (role modules
        // were resolved to pointers in refreshRoles -- no per-sample search)
        const float l = inL ? inL[i] : 0.f, r = inR ? inR[i] : 0.f;
        for (Module* m : audioInMods_) {
            auto& o = m->outputs;
            if (o.size() > 0) { o[0].setVoltage(l * 5.f); o[0].channels = 1; }
            if (o.size() > 1) { o[1].setVoltage(r * 5.f); o[1].channels = 1; }
        }
        for (Module* m : midiCvMods_) {
            auto& o = m->outputs;
            for (int c = 0; c < poly; ++c) {                     // per-voice poly channels
                if (o.size() > 0) o[0].voltages[c] = voices_[c].pitchV;
                if (o.size() > 1) o[1].voltages[c] = voices_[c].gateV;
                if (o.size() > 2) o[2].voltages[c] = voices_[c].velV;
            }
            if (o.size() > 0) o[0].channels = (uint8_t)poly;
            if (o.size() > 1) o[1].channels = (uint8_t)poly;
            if (o.size() > 2) o[2].channels = (uint8_t)poly;
        }

        stepSample(args);

        // read AudioOut inputs -> node stereo out (V -> +/-1).  getVoltageSum()
        // folds a polyphonic input (all voices) down to the stereo bus.
        float L = 0.f, R = 0.f;
        for (Module* m : audioOutMods_) {
            auto& in = m->inputs;
            float ml = (in.size() > 0) ? in[0].getVoltageSum() : 0.f;
            float mr = (in.size() > 1) ? in[1].getVoltageSum() : ml;
            L += ml * 0.2f; R += mr * 0.2f;
        }
        if (outL) outL[i] = L;
        if (outR) outR[i] = R;
        ++frame_;
    }
}

void RackEngine::processMulti(int nframes,
                              const float* const* insL, const float* const* insR, int numIns,
                              float* const* outsL, float* const* outsR, int numOuts,
                              const PatchKnob::engine::MidiEvent* midi, int numMidi,
                              float hostTempoBpm, bool hostPlaying,
                              int64_t hostPlayPositionSamples) {
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) {                    // structural edit in flight -> silence
        for (int k = 0; k < numOuts; ++k) {
            if (outsL && outsL[k]) std::fill(outsL[k], outsL[k] + nframes, 0.f);
            if (outsR && outsR[k]) std::fill(outsR[k], outsR[k] + nframes, 0.f);
        }
        return;
    }

    Module::ProcessArgs args;
    args.sampleRate = (float)sr_;
    args.sampleTime = (float)(1.0 / sr_);
    args.tempoBpm = hostTempoBpm;
    args.isPlaying = hostPlaying;

    const int poly = polyphony_ < 1 ? 1 : (polyphony_ > 16 ? 16 : polyphony_);
    int ei = 0;
    for (int i = 0; i < nframes; ++i) {
        args.frame = hostPlayPositionSamples >= 0
                   ? hostPlayPositionSamples + i : frame_;
        while (ei < numMidi && midi && midi[ei].sampleOffset <= i) {
            const auto& e = midi[ei++];
            const uint8_t type = e.status & 0xF0;
            if (type == 0x90 && e.data2 > 0)                          noteOn(e.data1, e.data2);
            else if ((type == 0x80) || (type == 0x90 && e.data2 == 0)) noteOff(e.data1);
            else if (type == 0xB0 && (e.data1 == 120 || e.data1 == 123))
                for (int n=0;n<128;++n) noteOff(n);
        }

        // each AudioIn module gets its OWN stereo input port
        for (size_t k = 0; k < audioInMods_.size(); ++k) {
            const float l = ((int)k < numIns && insL && insL[k]) ? insL[k][i] : 0.f;
            const float r = ((int)k < numIns && insR && insR[k]) ? insR[k][i] : l;
            auto& o = audioInMods_[k]->outputs;
            if (o.size() > 0) { o[0].setVoltage(l * 5.f); o[0].channels = 1; }
            if (o.size() > 1) { o[1].setVoltage(r * 5.f); o[1].channels = 1; }
        }
        for (Module* m : midiCvMods_) {
            auto& o = m->outputs;
            for (int c = 0; c < poly; ++c) {
                if (o.size() > 0) o[0].voltages[c] = voices_[c].pitchV;
                if (o.size() > 1) o[1].voltages[c] = voices_[c].gateV;
                if (o.size() > 2) o[2].voltages[c] = voices_[c].velV;
            }
            if (o.size() > 0) o[0].channels = (uint8_t)poly;
            if (o.size() > 1) o[1].channels = (uint8_t)poly;
            if (o.size() > 2) o[2].channels = (uint8_t)poly;
        }

        stepSample(args);

        // each AudioOut module writes its OWN stereo output port
        for (size_t k = 0; k < audioOutMods_.size(); ++k) {
            auto& in = audioOutMods_[k]->inputs;
            const float ml = (in.size() > 0) ? in[0].getVoltageSum() : 0.f;
            const float mr = (in.size() > 1) ? in[1].getVoltageSum() : ml;
            if ((int)k < numOuts) {
                if (outsL && outsL[k]) outsL[k][i] = ml * 0.2f;
                if (outsR && outsR[k]) outsR[k][i] = mr * 0.2f;
            }
        }
        ++frame_;
    }
}

} // namespace rackx

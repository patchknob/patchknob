//----------------------------------------------------------------------------
//  src/engine/patch/rack_ext_modules.cpp
//
//  Two extra VCV-Rack modules that host a DSP scripting engine, so you can drop a
//  Pd patch or a Csound orchestra straight into the modular rack and wire it to
//  Fundamental modules:
//
//      * "Csound" -- runs a .csd.  Ports/knobs are declared in the orchestra by
//        the channel names it reads/writes:  chnget "jack-N" -> an INPUT jack,
//        chnset ... "jack-N" -> an OUTPUT jack, chnget "knob-N" -> a KNOB.
//      * "Pd"     -- runs a .pd (libpd).  adc~/dac~ channels -> jacks, and
//        [r knob-N] receives -> knobs.  (added alongside Csound.)
//
//  Rack ports are +/-5 V; Pd/Csound work in +/-1.0, so audio/CV is scaled x0.2
//  on the way in and x5 on the way out -- they interoperate with the rest of the
//  rack automatically.  The rack calls process() per sample; we bridge to the
//  engine's block size (Csound ksmps / libpd 64) with a one-block ring.
//
//  Registered via addType() so both the module PICKER and the rack right-click
//  ADD menu list them (both enumerate the same registry()).
//----------------------------------------------------------------------------
#include "rack.hpp"
#include "rack_factory.h"
#include "rack_script_module.h"

#include "z_libpd.h"                         // libpd (always linked into PatchKnob_patch)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef PATCHKNOB_HAVE_CSOUND
#if __has_include(<csound/csound.h>)
#include <csound/csound.h>
#else
#include <csound.h>
#endif
#endif

namespace rackx {

// ---- shared: scan a script for jack-N / knob-N channel declarations ---------
namespace {

struct ChannelScan {
    std::vector<int> inJacks;    // chnget "jack-N"  -> rack input ports
    std::vector<int> outJacks;   // chnset "jack-N"  -> rack output ports
    std::vector<int> knobs;      // chnget "knob-N"  -> rack knob params
    std::vector<char> inRates;   // a = audio, k/i = control
    std::vector<char> outRates;
};

// Add n to v keeping it a sorted, duplicate-free set.
static void add_unique(std::vector<int>& v, int n) {
    for (int x : v) if (x == n) return;
    v.push_back(n);
    for (size_t i = v.size() - 1; i > 0 && v[i] < v[i - 1]; --i) std::swap(v[i], v[i - 1]);
}

// Parse the integer that follows a `"<tag>-` occurrence at position p (p points at
// the opening quote).  Returns -1 if it isn't `"<tag>-<digits>"`.
static int parse_tag_index(const std::string& s, size_t quote, const char* tag) {
    const size_t tl = std::strlen(tag);
    size_t i = quote + 1;
    if (s.compare(i, tl, tag) != 0) return -1;
    i += tl;
    if (i >= s.size() || s[i] != '-') return -1;
    ++i;
    int n = 0; bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { n = n * 10 + (s[i] - '0'); ++i; any = true; }
    if (!any || i >= s.size() || s[i] != '"') return -1;
    return n;
}

// Scan CSD-style text: on each line, "jack-N"/"knob-N" strings are classified by
// whether chnset (write = output) or chnget (read = input) appears on the line.
static ChannelScan scan_csound(const std::string& csd) {
    ChannelScan sc;
    bool blockComment = false;
    size_t line0 = 0;
    while (line0 <= csd.size()) {
        size_t line1 = csd.find('\n', line0);
        if (line1 == std::string::npos) line1 = csd.size();
        std::string line = csd.substr(line0, line1 - line0);
        // Csound comments are documentation, never port declarations.
        std::string code;
        for (size_t i = 0; i < line.size();) {
            if (blockComment) {
                const size_t end = line.find("*/", i);
                if (end == std::string::npos) { i = line.size(); continue; }
                blockComment = false; i = end + 2; continue;
            }
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
                blockComment = true; i += 2; continue;
            }
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') break;
            code += line[i++];
        }
        line.swap(code);
        const size_t comment = line.find(';');
        if (comment != std::string::npos) line.erase(comment);
        const bool hasSet = line.find("chnset") != std::string::npos;
        const size_t op = line.find(hasSet ? "chnset" : "chnget");
        char rate = 0;
        // Explicit typed opcode forms (chnget:a / chnset:k) take precedence.
        if (op != std::string::npos) {
            const size_t colon = op + 6;
            if (colon + 1 < line.size() && line[colon] == ':')
                rate = (char)std::tolower((unsigned char)line[colon + 1]);
        }
        // Otherwise Csound's output/input variable prefix carries the rate.
        if (!rate && op != std::string::npos) {
            if (!hasSet) {
                size_t b = line.find_first_not_of(" \t");
                if (b < op && b < line.size()) rate = (char)std::tolower((unsigned char)line[b]);
            } else {
                size_t b = op + 6;
                b = line.find_first_not_of(" \t:", b);
                if (b < line.size()) {
                    rate = (char)std::tolower((unsigned char)line[b]);
                    // Global Csound variables use a two-letter prefix: ga/gk/gi.
                    if (rate == 'g' && b + 1 < line.size())
                        rate = (char)std::tolower((unsigned char)line[b + 1]);
                }
            }
        }
        if (rate != 'a') rate = 'k'; // i-rate and untyped numeric channels use control storage
        for (size_t q = line.find('"'); q != std::string::npos; q = line.find('"', q + 1)) {
            int j = parse_tag_index(line, q, "jack");
            if (j >= 0) {
                std::vector<int>& ids = hasSet ? sc.outJacks : sc.inJacks;
                std::vector<char>& rates = hasSet ? sc.outRates : sc.inRates;
                size_t n = 0; while (n < ids.size() && ids[n] < j) ++n;
                if (n == ids.size() || ids[n] != j) {
                    ids.insert(ids.begin() + n, j);
                    rates.insert(rates.begin() + n, rate);
                } else if (rate == 'a') rates[n] = 'a';
                continue;
            }
            int k = parse_tag_index(line, q, "knob");
            if (k >= 0) add_unique(sc.knobs, k);
        }
        if (line1 >= csd.size()) break;
        line0 = line1 + 1;
    }
    return sc;
}

static std::string make_default_csd() {
    return
        "<CsoundSynthesizer>\n<CsInstruments>\nksmps = 32\nnchnls = 1\n0dbfs = 1\n"
        "giSine ftgen 0, 0, 16384, 10, 1\n"
        "gaMix init 0\n"
        "; Rack ports: jack-1 pitch, jack-2 gate/amplitude, jack-3 audio out\n\n"
        "; PatchKnob native poly voice: every tagged i-event creates another instr 1 instance.\n"
        "instr 1\n"
        "  kRackPitch chnget \"jack-1\"\n"
        "  kRackAmp chnget \"jack-2\"\n"
        "  ivoct = p4\n"
        "  iamp init 0\n"
        "  if p5 > 0 then\n"
        "    iamp = 0.1\n"
        "  endif\n"
        "  icps = 261.625565 * (2 ^ ivoct)\n"
        "  gaMix += oscili(iamp, icps, giSine)\n"
        "endin\n\n"
        "instr 999\n"
        "  chnset gaMix, \"jack-3\"\n"
        "  gaMix = 0\n"
        "endin\n"
        "</CsInstruments>\n<CsScore>\n"
        "i 1 0 604800 0 0\n"
        "i 999 0 604800\n"
        "</CsScore>\n</CsoundSynthesizer>\n";
}

#ifdef PATCHKNOB_HAVE_CSOUND
// Keep the user's CSD simple while giving native-poly instrument 1 a separate
// channel bus for every rack allocator lane. The editor/source remains untouched;
// only the text handed to the runtime is expanded. Fractional p1 tags are assigned
// by the host as 1.001, 1.002, ... and survive voice stealing.
static std::string csound_poly_runtime_csd(const std::string& source) {
    if (source.find("PatchKnob native poly voice") == std::string::npos)
        return source;
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    bool voiceInstr = false;
    std::vector<int> declared;
    while (std::getline(input, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.compare(0, 7, "instr 1") == 0 &&
            (trimmed.size() == 7 || std::isspace((unsigned char)trimmed[7])))
            voiceInstr = true;
        if (voiceInstr && line.find("chnget") != std::string::npos) {
            for (size_t q = line.find('"'); q != std::string::npos; q = line.find('"', q + 1)) {
                const int jack = parse_tag_index(line, q, "jack");
                if (jack < 0) continue;
                const size_t close = line.find('"', q + 1);
                if (close == std::string::npos) break;
                const std::string var = "SpkJack" + std::to_string(jack);
                if (std::find(declared.begin(), declared.end(), jack) == declared.end()) {
                    output << "  " << var
                           << " sprintf \"jack-" << jack
                           << "-v%d\", int(frac(p1) * 1000 + 0.5)\n";
                    declared.push_back(jack);
                }
                line.replace(q, close - q + 1, var);
                break;
            }
        }
        output << line << '\n';
        if (voiceInstr && trimmed == "endin") voiceInstr = false;
    }
    return output.str();
}
#endif

// ---- Pd scan: adc~/dac~ channel counts (jacks) + knob-N receives (knobs) ----
static std::string pd_temp_dir() {
    const char* t = std::getenv("TEMP"); if (!t || !*t) t = std::getenv("TMP");
    if (!t || !*t) t = std::getenv("TMPDIR"); if (!t || !*t) t = ".";
    return std::string(t);
}

struct PdScan {
    int inCh = 0, outCh = 0;
    std::vector<int> knobs;
    std::vector<int> controlIn, controlOut;
    std::vector<int> audioIn, audioOut;
};

static void scan_pd_named_jacks(const std::string& pd, PdScan& s) {
    std::istringstream lines(pd);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string hash, kind, xs, ys, object, symbol;
        if (!(words >> hash >> kind >> xs >> ys >> object >> symbol) ||
            hash != "#X" || kind != "obj") continue;
        if (!symbol.empty() && symbol.back() == ';') symbol.pop_back();
        if (symbol.compare(0, 5, "jack-") != 0) continue;
        char* end = nullptr;
        const long n = std::strtol(symbol.c_str() + 5, &end, 10);
        if (!end || *end || n < 0 || n > 9999) continue;
        if      (object == "r"  || object == "receive")  add_unique(s.controlIn,  (int)n);
        else if (object == "r~" || object == "receive~") add_unique(s.audioIn,    (int)n);
        else if (object == "s"  || object == "send")     add_unique(s.controlOut, (int)n);
        else if (object == "s~" || object == "send~")    add_unique(s.audioOut,   (int)n);
    }
}

static int pd_max_channel(const std::string& pd, const char* tok) {
    int mx = 0; const size_t tl = std::strlen(tok); size_t p = 0;
    while ((p = pd.find(tok, p)) != std::string::npos) {
        const bool okpre = (p == 0) || pd[p - 1] == ' ' || pd[p - 1] == '\n';
        const size_t aft = p + tl; p = aft;
        const bool okpost = (aft >= pd.size()) || pd[aft] == ' ' || pd[aft] == ';' ||
                            pd[aft] == '\n' || pd[aft] == ',';
        if (!okpre || !okpost) continue;
        std::string args; size_t i = aft;
        while (i < pd.size() && pd[i] != ';' && pd[i] != '\n' && pd[i] != ',') args += pd[i++];
        std::istringstream ss(args); std::string a; std::vector<std::string> av;
        while (ss >> a) av.push_back(a);
        int localMax = 0;
        if (!av.empty() && av[0] == "-m") {                 // adc~ -m <count> <start>
            int cnt = av.size() > 1 ? std::atoi(av[1].c_str()) : 2;
            int st  = av.size() > 2 ? std::atoi(av[2].c_str()) : 1;
            if (cnt < 1) cnt = 1; if (st < 1) st = 1; localMax = st + cnt - 1;
        } else {
            for (const std::string& x : av) { char* e = nullptr; long v = std::strtol(x.c_str(), &e, 10);
                if (e && *e == '\0' && v > 0 && v < 256 && (int)v > localMax) localMax = (int)v; }
        }
        if (localMax == 0) localMax = 2;                    // bare adc~/dac~ == 2 channels
        if (localMax > mx) mx = localMax;
    }
    return mx;
}

static PdScan scan_pd(const std::string& pd) {
    PdScan s;
    s.inCh  = pd_max_channel(pd, "adc~");
    s.outCh = pd_max_channel(pd, "dac~");
    scan_pd_named_jacks(pd, s);
    for (size_t q = pd.find("knob-"); q != std::string::npos; q = pd.find("knob-", q + 1)) {
        size_t i = q + 5; int n = 0; bool any = false;
        while (i < pd.size() && pd[i] >= '0' && pd[i] <= '9') { n = n * 10 + (pd[i] - '0'); ++i; any = true; }
        if (any) add_unique(s.knobs, n);
    }
    return s;
}

const char* kDefaultPd =
    "#N canvas 0 0 450 300 12;\n"
    "#X obj 40 30 r jack-1;\n"
    "#X obj 40 65 expr 261.625565 * pow(2\\, $f1);\n"
    "#X text 190 65 V/oct to Hz (0 V = C4);\n"
    "#X obj 40 105 osc~;\n"
    "#X obj 160 105 r knob-1;\n"
    "#X obj 40 145 *~;\n"
    "#X obj 40 205 dac~ 1;\n"
    "#X connect 0 0 1 0;\n"
    "#X connect 1 0 3 0;\n"
    "#X connect 3 0 5 0;\n"
    "#X connect 4 0 5 1;\n"
    "#X connect 5 0 6 0;\n";

} // namespace

// ============================================================================
//  Csound rack module
// ============================================================================
#ifdef PATCHKNOB_HAVE_CSOUND
struct RackCsoundModule : rack::engine::Module, public rackx::IScriptModule {
    std::string          csd_ = make_default_csd();
    ChannelScan          scan_;
    CSOUND*              cs_ = nullptr;
    int                  ksmps_ = 32;
    bool                 started_ = false, failed_ = false, finished_ = false;
    double               sr_ = 0.0, srHint_ = 44100.0;
    std::string          err_;                          // last compile messages
    int                  blockPos_ = 0;
    std::vector<MYFLT*>  inPtr_, outPtr_, knobPtr_;     // csound channel buffers
    std::vector<MYFLT>   channelBlock_;                  // reusable channel copy scratch
    std::vector<char>    inAudio_, outAudio_;           // 1 = a-rate channel, 0 = k-rate
    std::vector<std::vector<float>> inBuf_, outBuf_;    // per-jack one-block ring
    std::vector<std::string> inName_, outName_, knobName_;
    int                  polyphony_ = 1;
    bool                 gateHigh_[16] = {};
    float                voicePitch_[16] = {};

    RackCsoundModule() { reconfigure(); }
    ~RackCsoundModule() override { if (cs_) { csoundDestroy(cs_); cs_ = nullptr; } }

    // (re)derive ports/knobs from the current CSD + declare them to the rack.
    void reconfigure() {
        scan_ = scan_csound(csd_);
        knobName_.clear(); inName_.clear(); outName_.clear();
        for (int k : scan_.knobs)    knobName_.push_back("knob-" + std::to_string(k));
        for (int j : scan_.inJacks)  inName_.push_back ("jack-" + std::to_string(j));
        for (int j : scan_.outJacks) outName_.push_back("jack-" + std::to_string(j));
        config((int)knobName_.size(), (int)inName_.size(), (int)outName_.size(), 0);
        for (size_t i = 0; i < knobName_.size(); ++i)
            configParam((int)i, 0.f, 1.f, 0.f, knobName_[i]);
        for (size_t i = 0; i < inName_.size(); ++i)  configInput((int)i, inName_[i]);
        for (size_t i = 0; i < outName_.size(); ++i) configOutput((int)i, outName_[i]);
    }
    // --- IScriptModule ---
    const char* scriptKind() const override { return "csound"; }
    std::string script() const override { return csd_; }
    void setScript(const std::string& t) override {
        csd_ = t;
        // Project data is authoritative. Never infer that a user's orchestra is
        // an obsolete starter from comments or code fragments: doing so replaced
        // valid saved CSDs with make_default_csd() during every project reload.
        reconfigure();
        compile(srHint_);          // compile NOW (GUI thread, under the rack edit lock) so
    }                              // the editor gets the error text immediately in err_
    int knobCount()   const override { return (int)knobName_.size(); }
    int inJackCount() const override { return (int)inName_.size(); }
    int outJackCount() const override { return (int)outName_.size(); }
    std::string lastError() const override { return err_; }
    void setPolyphony(int voices) override {
        const int next = voices < 1 ? 1 : (voices > 16 ? 16 : voices);
        if (next == polyphony_) return;
        polyphony_ = next;
        // The generated starter score contains one native Csound instrument
        // instance per rack voice, so a voice-count edit requires one recompile.
        if (started_ || cs_) compile(srHint_);
    }
    void onSampleRateChange(float sr) override {
        if (sr > 0.f) srHint_ = sr;
    }
    // Csound routes compile/runtime messages here; accumulate them so the editor can
    // show the FULL error text in its status bar.
    static void msgCb(CSOUND* cs, int, const char* fmt, va_list args) {
        RackCsoundModule* self = (RackCsoundModule*)csoundGetHostData(cs);
        if (!self || !fmt) return;
        char buf[1024]; std::vsnprintf(buf, sizeof(buf), fmt, args);
        self->err_ += buf;
    }

    void compile(double sr) {
        sr_ = sr; failed_ = true; started_ = false; err_.clear();
        if (cs_) { csoundDestroy(cs_); cs_ = nullptr; }
        // Pass host data at construction. Csound 6 initializes several internal
        // realtime/circular-buffer structures during csoundCreate(); installing
        // host data only afterward can leave that initialization path invalid.
        cs_ = csoundCreate(this);
        if (!cs_) { err_ = "csoundCreate failed"; return; }
        csoundSetHostData(cs_, this);
        // Keep Csound's own message path. Some Csound 6 builds reuse the callback
        // va_list internally; consuming it in a host callback corrupts the
        // realtime message circular buffer before the first perform call.
        csoundSetOption(cs_, "-n");                     // no realtime audio device
        csoundSetOption(cs_, "-d");                     // no display
        char opt[64]; std::snprintf(opt, sizeof(opt), "--sample-rate=%d", (int)(sr > 0 ? sr : 44100));
        csoundSetOption(cs_, opt);
        const std::string runtimeCsd = csound_poly_runtime_csd(csd_);
        if (csoundCompileCsdText(cs_, runtimeCsd.c_str()) != 0) return;
        if (csoundStart(cs_) != 0) return;
        ksmps_ = csoundGetKsmps(cs_); if (ksmps_ < 1) ksmps_ = 1;
        // Record channel rates. We intentionally use Csound's copying channel
        // API during processing instead of retaining raw engine-owned pointers.
        inPtr_.assign(inName_.size(), nullptr);
        outPtr_.assign(outName_.size(), nullptr);
        knobPtr_.assign(knobName_.size(), nullptr);
        inAudio_ = scan_.inRates;
        outAudio_ = scan_.outRates;
        // The source scan is needed before compilation to build the rack panel,
        // but Csound itself is the authority on the compiled channel rate. This
        // catches every valid spelling/expression form of chnget/chnset rather
        // than relying on the variable prefix parser alone.
        controlChannelInfo_t* channelList = nullptr;
        const int channelCount = csoundListChannels(cs_, &channelList);
        auto compiledAudioRate = [&](const std::string& name, bool fallback) {
            for (int c = 0; c < channelCount; ++c)
                if (channelList[c].name && name == channelList[c].name)
                    return (channelList[c].type & CSOUND_CHANNEL_TYPE_MASK) ==
                           CSOUND_AUDIO_CHANNEL;
            return fallback;
        };
        for (size_t i = 0; i < inName_.size(); ++i) {
            inAudio_[i] = compiledAudioRate(inName_[i], inAudio_[i] == 'a');
        }
        for (size_t i = 0; i < outName_.size(); ++i) {
            outAudio_[i] = compiledAudioRate(outName_[i], outAudio_[i] == 'a');
        }
        if (channelList) csoundDeleteChannelList(cs_, channelList);
        const bool nativePoly = csd_.find("PatchKnob native poly voice") != std::string::npos;
        const int inputLanes = nativePoly ? polyphony_ : 1;
        inBuf_.assign(inName_.size(),  std::vector<float>((size_t)ksmps_ * inputLanes, 0.f));
        outBuf_.assign(outName_.size(), std::vector<float>(ksmps_, 0.f));
        channelBlock_.assign((size_t)ksmps_, (MYFLT)0);
        blockPos_ = 0; started_ = true; failed_ = false; finished_ = false;
        for (int v = 0; v < 16; ++v) {
            gateHigh_[v] = false;
            voicePitch_[v] = 0.f;
        }
    }

    void process(const ProcessArgs& args) override {
        if (!started_ && !failed_) compile(args.sampleRate);   // compile ONCE (or on setScript)
        if (!started_ || finished_) { for (auto& o : outputs) o.setVoltage(0.f); return; }

        const bool nativePoly = csd_.find("PatchKnob native poly voice") != std::string::npos;
        // Preserve allocator lanes for the native-poly runtime. Mono sources use
        // Rack's normal broadcast behavior; poly ADSR/VCA sources remain isolated.
        for (size_t i = 0; i < inBuf_.size(); ++i) {
            const int lanes = nativePoly ? polyphony_ : 1;
            for (int v = 0; v < lanes; ++v) {
                float value = inputs[i].getPolyVoltage(v);
                if (inAudio_[i]) value *= 0.2f; // rack +/-5 V audio -> Csound +/-1
                inBuf_[i][(size_t)v * ksmps_ + blockPos_] = value;
            }
        }
        // emit last block's outputs (Csound +/-1 -> rack +/-5V)
        for (size_t i = 0; i < outBuf_.size(); ++i)
            outputs[i].setVoltage(outBuf_[i][blockPos_] *
                (outAudio_[i] ? 5.f : 1.f));

        if (++blockPos_ >= ksmps_) {
            blockPos_ = 0;
            for (size_t i = 0; i < inName_.size(); ++i) {
                const int lanes = nativePoly ? polyphony_ : 1;
                for (int v = 0; v < lanes; ++v) {
                    const std::string channel = nativePoly
                        ? inName_[i] + "-v" + std::to_string(v + 1) : inName_[i];
                    const size_t base = (size_t)v * ksmps_;
                    if (inAudio_[i]) {
                        for (int s = 0; s < ksmps_; ++s)
                            channelBlock_[(size_t)s] = (MYFLT)inBuf_[i][base + s];
                        csoundSetAudioChannel(cs_, channel.c_str(), channelBlock_.data());
                    } else {
                        csoundSetControlChannel(cs_, channel.c_str(),
                                                (MYFLT)inBuf_[i][base + ksmps_ - 1]);
                    }
                }
            }
            for (size_t i = 0; i < knobName_.size(); ++i)
                csoundSetControlChannel(cs_, knobName_[i].c_str(), (MYFLT)params[i].getValue());
            // The starter's score line `i 1 0 z` is the voice template. Each
            // rack gate edge retriggers that SAME instrument using a fractional
            // p1 tag, which is Csound's native instance/voice mechanism.
            if (csd_.find("PatchKnob native poly voice") != std::string::npos) {
                int pitchPort = -1, gatePort = -1;
                for (size_t i = 0; i < inName_.size(); ++i) {
                    if (inName_[i] == "jack-1") pitchPort = (int)i;
                    if (inName_[i] == "jack-2") gatePort = (int)i;
                }
                if (pitchPort >= 0 && gatePort >= 0) {
                    const int pitchCh = inputs[pitchPort].getChannels();
                    const int gateCh = inputs[gatePort].getChannels();
                    for (int v = 0; v < polyphony_; ++v) {
                        const float gate = v < gateCh ? inputs[gatePort].voltages[v] : 0.f;
                        const bool high = gate > 0.0001f;
                        const float voct = v < pitchCh ? inputs[pitchPort].voltages[v] : 0.f;
                        // A stolen allocator lane commonly remains high while its
                        // pitch/note identity changes. Treat that as an explicit
                        // off+on retrigger of the same fractional p1 tag; otherwise
                        // the old Csound instance sticks and repeats its old note.
                        const bool reassigned = high && gateHigh_[v] &&
                            std::fabs(voct - voicePitch_[v]) > 0.00001f;
                        if (high != gateHigh_[v] || reassigned) {
                            char evt[128];
                            if (!high || reassigned) {
                                std::snprintf(evt, sizeof(evt), "i -1.%03d 0 0", v + 1);
                                csoundInputMessage(cs_, evt);
                            }
                            if (high) {
                                std::snprintf(evt, sizeof(evt), "i 1.%03d 0 -1 %.9g %.9g",
                                              v + 1, voct, gate);
                                csoundInputMessage(cs_, evt);
                            }
                            gateHigh_[v] = high;
                            voicePitch_[v] = voct;
                        }
                    }
                }
            }
            if (csoundPerformKsmps(cs_) != 0) { finished_ = true; return; }   // score ended: STOP,
                                                                              // no auto-recompile loop
            for (size_t i = 0; i < outName_.size(); ++i) {
                if (outAudio_[i]) {
                    csoundGetAudioChannel(cs_, outName_[i].c_str(), channelBlock_.data());
                    for (int s = 0; s < ksmps_; ++s) outBuf_[i][s] = (float)channelBlock_[(size_t)s];
                } else {
                    int err = 0;
                    const float value = (float)csoundGetControlChannel(cs_, outName_[i].c_str(), &err);
                    for (int s = 0; s < ksmps_; ++s) outBuf_[i][s] = err ? 0.f : value;
                }
            }
        }
    }
};
#endif // PATCHKNOB_HAVE_CSOUND

// ============================================================================
//  Pd rack module (libpd)
// ============================================================================
static std::string pd_voice_namespace(const std::string& source, int voice) {
    std::string out = source;
    size_t p = 0;
    while ((p = out.find("jack-", p)) != std::string::npos) {
        size_t e = p + 5;
        while (e < out.size() && out[e] >= '0' && out[e] <= '9') ++e;
        if (e == p + 5 || (e < out.size() && out[e] == '-')) { p = e; continue; }
        const std::string suffix = "-v" + std::to_string(voice);
        out.insert(e, suffix);
        p = e + suffix.size();
    }
    return out;
}

struct RackPdModule : rack::engine::Module, public rackx::IScriptModule {
    std::string          pd_ = kDefaultPd;
    PdScan               scan_;
    t_pdinstance*        inst_ = nullptr;
    std::vector<void*>   files_;
    bool                 started_ = false, failed_ = false;
    int                  block_ = 64, blockPos_ = 0;
    int                  inCh_ = 0, outCh_ = 0;
    std::vector<float>   in_, out_;                     // interleaved libpd scratch (one tick)
    std::vector<std::string> knobName_, inName_, outName_;
    int                  polyphony_ = 1;
    bool                 nativePoly_ = false;

    RackPdModule() { reconfigure(); }
    ~RackPdModule() override {
        closeRuntime();
    }
    void closeRuntime() {
        if (!inst_) { files_.clear(); return; }
        libpd_set_instance(inst_);
        for (void* f : files_) if (f) libpd_closefile(f);
        files_.clear();
        libpd_free_instance(inst_); inst_ = nullptr;
    }
    void reconfigure() {
        scan_ = scan_pd(pd_);
        nativePoly_ = pd_.find(" clone ") != std::string::npos ||
                      pd_.find(" poly ")  != std::string::npos;
        inCh_ = scan_.inCh + (int)scan_.audioIn.size();
        outCh_ = scan_.outCh + (int)scan_.audioOut.size() + (int)scan_.controlOut.size();
        knobName_.clear(); inName_.clear(); outName_.clear();
        for (int k : scan_.knobs) knobName_.push_back("knob-" + std::to_string(k));
        for (int i = 0; i < scan_.inCh; ++i) inName_.push_back("in " + std::to_string(i + 1));
        for (int j : scan_.controlIn) inName_.push_back("jack-" + std::to_string(j));
        for (int j : scan_.audioIn)   inName_.push_back("jack-" + std::to_string(j));
        for (int i = 0; i < scan_.outCh; ++i) outName_.push_back("out " + std::to_string(i + 1));
        for (int j : scan_.controlOut) outName_.push_back("jack-" + std::to_string(j));
        for (int j : scan_.audioOut)   outName_.push_back("jack-" + std::to_string(j));
        config((int)knobName_.size(), (int)inName_.size(), (int)outName_.size(), 0);
        for (size_t i = 0; i < knobName_.size(); ++i) configParam((int)i, 0.f, 1.f, 0.f, knobName_[i]);
        for (size_t i = 0; i < inName_.size();  ++i) configInput ((int)i, inName_[i]);
        for (size_t i = 0; i < outName_.size(); ++i) configOutput((int)i, outName_[i]);
    }
    // --- IScriptModule ---
    const char* scriptKind() const override { return "pd"; }
    std::string script() const override { return pd_; }
    void setScript(const std::string& t) override {
        pd_ = t; reconfigure();
        closeRuntime();
        started_ = false; failed_ = false;              // reload on next process()
    }
    int knobCount()   const override { return (int)knobName_.size(); }
    int inJackCount() const override { return (int)inName_.size(); }
    int outJackCount() const override { return (int)outName_.size(); }
    void setPolyphony(int voices) override {
        const int next = voices < 1 ? 1 : (voices > 16 ? 16 : voices);
        if (next == polyphony_) return;
        polyphony_ = next;
        if (inst_) closeRuntime();
        started_ = false; failed_ = false;
    }
    void start(double sr) {
        failed_ = true; started_ = false;
        libpd_init();                                    // idempotent (self-guarded)
        inst_ = libpd_new_instance();
        if (!inst_) return;
        libpd_set_instance(inst_);
        block_ = libpd_blocksize(); if (block_ < 1) block_ = 64;
        const int ic = inCh_ > 0 ? inCh_ : 1, oc = outCh_ > 0 ? outCh_ : 1;
        in_.assign((size_t)block_ * ic, 0.f);
        out_.assign((size_t)block_ * oc, 0.f);
        libpd_init_audio(ic, oc, (int)(sr > 0 ? sr : 44100));
        // libpd opens from a FILE: materialise a transient temp .pd, open, delete.
        static std::atomic<unsigned long long> seq{0};
        const std::string dir  = pd_temp_dir();
        files_.clear();
        // Open one namespaced canvas per voice INSIDE this single libpd
        // instance. Pd shares the interpreter and DSP scheduler, while each
        // canvas retains independent oscillator/filter/envelope state.
        const int copies = nativePoly_ ? 1 : polyphony_;
        for (int v = 1; v <= copies; ++v) {
            const std::string name = "pkrackpd_" +
                std::to_string(((unsigned long long)(uintptr_t)this) ^ seq.fetch_add(1)) +
                "_v" + std::to_string(v) + ".pd";
            const std::string full = dir + "/" + name;
            std::string runtimePd = nativePoly_ ? pd_ : pd_voice_namespace(pd_, v);
            int bridge = 0;
            auto addBridge = [&](const std::string& body) {
                runtimePd += "#N canvas 0 0 220 120 pkjack" + std::to_string(bridge) + " 0;\n";
                runtimePd += body;
                runtimePd += "#X restore 900 " + std::to_string(20 + bridge++ * 24) + " pd pkjack;\n";
            };
            for (size_t i = 0; i < scan_.audioIn.size(); ++i) {
                const int ch = scan_.inCh + (int)i + 1;
                addBridge("#X obj 10 10 adc~ " + std::to_string(ch) +
                    ";\n#X obj 10 50 s~ jack-" + std::to_string(scan_.audioIn[i]) +
                    "-v" + std::to_string(v) + ";\n#X connect 0 0 1 0;\n");
            }
            int outExtra = scan_.outCh;
            for (int j : scan_.controlOut) {
                ++outExtra;
                addBridge("#X obj 10 10 r jack-" + std::to_string(j) + "-v" +
                    std::to_string(v) + ";\n#X obj 10 40 sig~;\n#X obj 10 70 dac~ " +
                    std::to_string(outExtra) +
                    ";\n#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n");
            }
            for (int j : scan_.audioOut) {
                ++outExtra;
                addBridge("#X obj 10 10 r~ jack-" + std::to_string(j) + "-v" +
                    std::to_string(v) + ";\n#X obj 10 60 dac~ " +
                    std::to_string(outExtra) + ";\n#X connect 0 0 1 0;\n");
            }
            { std::ofstream o(full.c_str(), std::ios::binary | std::ios::trunc);
              if (o) o << runtimePd; }
            void* f = libpd_openfile(name.c_str(), dir.c_str());
            std::remove(full.c_str());
            if (f) files_.push_back(f);
        }
        blockPos_ = 0;
        started_ = ((int)files_.size() == copies);
        failed_ = !started_;
        if (!started_) {
            closeRuntime();                       // don't retain a half-loaded voice bank
            return;
        }
        // Enable DSP only after every canvas has loaded, so loadbang/setup runs
        // against a stable graph instead of processing a partially built bank.
        libpd_set_instance(inst_);
        libpd_start_message(1); libpd_add_float(1); libpd_finish_message("pd", "dsp");
    }
    void process(const ProcessArgs& args) override {
        if (!started_ && !failed_) start(args.sampleRate);
        if (!started_) { for (auto& o : outputs) o.setVoltage(0.f); return; }
        const int ic = inCh_ > 0 ? inCh_ : 1, oc = outCh_ > 0 ? outCh_ : 1;
        for (int c = 0; c < scan_.inCh; ++c)
            in_[(size_t)blockPos_ * ic + c] = inputs[c].getVoltage() * 0.2f;
        const int controlBase = scan_.inCh;
        const int audioBase = controlBase + (int)scan_.controlIn.size();
        for (size_t i = 0; i < scan_.audioIn.size(); ++i)
            in_[(size_t)blockPos_ * ic + scan_.inCh + (int)i] =
                inputs[audioBase + (int)i].getVoltage() * 0.2f;
        const float voiceGain = nativePoly_ || polyphony_ <= 1
            ? 1.f : 1.f / std::sqrt((float)polyphony_);
        for (size_t i = 0; i < outName_.size(); ++i) {
            const bool control = i >= (size_t)scan_.outCh &&
                i < (size_t)scan_.outCh + scan_.controlOut.size();
            outputs[i].setVoltage(out_[(size_t)blockPos_ * oc + (int)i] *
                                  (control ? 1.f : 5.f * voiceGain));
        }
        if (++blockPos_ >= block_) {
            blockPos_ = 0;
            libpd_set_instance(inst_);
            for (size_t i = 0; i < knobName_.size(); ++i)
                libpd_float(knobName_[i].c_str(), (float)params[i].getValue());   // knob -> [r knob-N]
            libpd_float("polyphony", (float)polyphony_);
            for (size_t i = 0; i < scan_.controlIn.size(); ++i)
            {
                const std::string recv = "jack-" + std::to_string(scan_.controlIn[i]);
                const rack::engine::Input& input = inputs[controlBase + (int)i];
                if (nativePoly_) {
                    libpd_float(recv.c_str(), input.getChannels() > 0 ? input.voltages[0] : 0.f);
                } else {
                    // Do not use getPolyVoltage(): its mono-broadcast semantics
                    // would trigger every copied canvas from one note.
                    const int channels = input.getChannels();
                    for (int v = 0; v < polyphony_; ++v)
                        libpd_float((recv + "-v" + std::to_string(v + 1)).c_str(),
                            v < channels ? input.voltages[v] : 0.f);
                }
                // [r jack-N-poly] receives a list with one CV value per rack
                // voice, ready for [list prepend]/[clone] routing in Pd.
                if (libpd_start_message(polyphony_) == 0) {
                    for (int v = 0; v < polyphony_; ++v)
                        libpd_add_float(v < input.getChannels() ? input.voltages[v] : 0.f);
                    libpd_finish_list((recv + "-poly").c_str());
                }
            }
            // libpd mixes every canvas into this buffer; clear the destination
            // explicitly so a backend that accumulates cannot retain the prior tick.
            std::fill(out_.begin(), out_.end(), 0.f);
            libpd_process_float(1, in_.data(), out_.data());
        }
    }
};

// ============================================================================
//  Registration (idempotent) -- call once at startup.
// ============================================================================
void registerRackExtModules() {
    registerBuiltins();                                 // ensure the base set is present first
#ifdef PATCHKNOB_HAVE_CSOUND
    if (!findType("PKCsound"))
        addType("PKCsound", "Csound", "Scripting", Role::Normal,
                [] { return ModuleHandle(std::unique_ptr<RackCsoundModule>(new RackCsoundModule())); });
#endif
    if (!findType("PKPd"))
        addType("PKPd", "Pd", "Scripting", Role::Normal,
                [] { return ModuleHandle(std::unique_ptr<RackPdModule>(new RackPdModule())); });
}

} // namespace rackx

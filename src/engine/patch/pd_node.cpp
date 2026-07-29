//----------------------------------------------------------------------------
//  src/engine/patch/pd_node.cpp -- embedded Pure Data (libpd) patch node.
//----------------------------------------------------------------------------
#include "pd_node.h"

#include "z_libpd.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace PatchKnob { namespace engine { namespace patch {

// libpd_init() sets up the global Pd system exactly once (message thread).
static std::once_flag g_pdInitOnce;
static void pd_global_init() { std::call_once(g_pdInitOnce, [] { libpd_init(); }); }

// Scan a .pd file for the highest adc~ / dac~ channel referenced so the node can
// size its audio in/out ports to match (bare "adc~"/"dac~" == 2 channels).  A
// simple text scan -- libpd itself resolves the actual routing.
static void scan_pd_channels(const std::string& path, int& inCh, int& outCh) {
    inCh = 2; outCh = 2;
    std::ifstream f(path.c_str());
    if (!f) return;
    int maxIn = 0, maxOut = 0;
    auto scan1 = [](const std::string& line, const char* tok, int& mx) {
        const size_t tl = std::strlen(tok);
        size_t p = 0;
        while ((p = line.find(tok, p)) != std::string::npos) {
            const bool okpre  = (p == 0) || line[p - 1] == ' ';
            const size_t aft  = p + tl;
            const bool okpost = (aft >= line.size()) || line[aft] == ' ' ||
                                line[aft] == ';' || line[aft] == ',';
            p = aft;
            if (!okpre || !okpost) continue;
            // Collect the creation args up to the record terminator.
            std::istringstream ss(line.substr(aft));
            std::string t; std::vector<std::string> args;
            while (ss >> t) { if (t == ";" || t == ",") break; args.push_back(t); }
            int localMax = 0;
            if (!args.empty() && args[0] == "-m") {
                // multichannel: [adc~ -m <count> <start>] -> channels start..start+count-1
                int count = args.size() > 1 ? std::atoi(args[1].c_str()) : 2;
                int start = args.size() > 2 ? std::atoi(args[2].c_str()) : 1;
                if (count < 1) count = 1;
                if (start < 1) start = 1;
                localMax = start + count - 1;
            } else {
                // explicit channel list: [adc~ 3 4] -> channels 3 and 4
                for (const std::string& a : args) {
                    char* e = nullptr; long v = std::strtol(a.c_str(), &e, 10);
                    if (e && *e == '\0' && v > 0 && v < 256 && (int)v > localMax) localMax = (int)v;
                }
            }
            if (localMax == 0) localMax = 2;         // bare adc~ / dac~ == 2 channels
            if (localMax > mx) mx = localMax;
        }
    };
    std::string line;
    while (std::getline(f, line)) { scan1(line, "adc~", maxIn); scan1(line, "dac~", maxOut); }
    inCh  = maxIn  > 0 ? std::max(2, maxIn)  : 2;
    outCh = maxOut > 0 ? std::max(2, maxOut) : 2;
}

static void pd_dsp_on(int on) {
    libpd_start_message(1);
    libpd_add_float(on ? 1.0f : 0.0f);
    libpd_finish_message("pd", "dsp");
}

PdNode::PdNode() {
    pd_global_init();
    pd_ = libpd_new_instance();
}

PdNode::~PdNode() {
    release();
    if (pd_) { libpd_free_instance(pd_); pd_ = nullptr; }
}

bool PdNode::prepare(double sampleRate, int maxBlock) {
    sr_    = sampleRate > 0 ? sampleRate : 48000.0;
    block_ = maxBlock > 0 ? maxBlock : 512;
    if (!pd_) return false;
    libpd_set_instance(pd_);
    pdBlock_ = libpd_blocksize();               // 64
    in_.assign((size_t)block_ * (size_t)std::max(1, audioInCh_), 0.0f);
    out_.assign((size_t)block_ * (size_t)std::max(1, audioOutCh_), 0.0f);
    libpd_init_audio(audioInCh_, audioOutCh_, (int)sr_);
    pd_dsp_on(1);
    return true;
}

void PdNode::release() {
    if (pd_ && file_) { libpd_set_instance(pd_); libpd_closefile(file_); file_ = nullptr; }
}

bool PdNode::loadPatch(const std::string& path) {
    if (!pd_) return false;
    libpd_set_instance(pd_);
    if (file_) { libpd_closefile(file_); file_ = nullptr; }
    // Size the node's audio ports to the patch's adc~/dac~ channels, then re-init
    // libpd's audio to that width so multichannel adc~/dac~ route to the ports.
    int nin = 2, nout = 2; scan_pd_channels(path, nin, nout);
    if (nin != audioInCh_ || nout != audioOutCh_) {
        audioInCh_ = nin; audioOutCh_ = nout;
        in_.assign((size_t)block_ * (size_t)std::max(1, audioInCh_), 0.0f);
        out_.assign((size_t)block_ * (size_t)std::max(1, audioOutCh_), 0.0f);
        libpd_init_audio(audioInCh_, audioOutCh_, (int)sr_);
    }
    size_t s = path.find_last_of("/\\");
    std::string dir  = (s == std::string::npos) ? std::string(".") : path.substr(0, s);
    std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
    file_ = libpd_openfile(name.c_str(), dir.c_str());
    patchPath_ = path;
    pd_dsp_on(1);
    return file_ != nullptr;
}

void PdNode::process(const NodeProcessContext& ctx) {
    const int n = ctx.nframes;
    for (int b = 0; b < ctx.numAudioOut; ++b)            // clear every out port
        for (int c = 0; c < ctx.audioOut[b].channels; ++c)
            std::memset(ctx.audioOut[b].chans[c], 0, sizeof(float) * (size_t)n);
    if (!pd_) return;

    libpd_set_instance(pd_);

    // --- feed MIDI into the patch ([notein]/[ctlin]/...) --------------------
    if (ctx.numMidiIn > 0) {
        const MidiBuffer& mb = ctx.midiIn[0];
        const int filt = midiChannel_.load(std::memory_order_relaxed);
        for (int i = 0; i < mb.count; ++i) {
            const MidiEvent& m = mb.ev[i];
            const unsigned char hi = m.status & 0xF0u;
            const int ch = m.status & 0x0Fu;
            // channel filter: when assigned to a clip's channel, ignore others
            if (filt >= 0 && hi >= 0x80u && hi < 0xF0u && ch != filt) continue;
            switch (hi) {
                case 0x90: libpd_noteon(ch, m.data1, m.data2); break;      // note on (vel 0 == off)
                case 0x80: libpd_noteon(ch, m.data1, 0); break;            // note off
                case 0xB0: libpd_controlchange(ch, m.data1, m.data2); break;
                case 0xC0: libpd_programchange(ch, m.data1); break;
                case 0xD0: libpd_aftertouch(ch, m.data1); break;
                case 0xE0: libpd_pitchbend(ch, ((m.data2 << 7) | m.data1) - 8192); break;
                default: break;
            }
        }
    }

    // --- run libpd in 64-sample ticks; interleave <-> planar ----------------
    const int ticks = (pdBlock_ > 0) ? (n / pdBlock_) : 0;
    const int nn = ticks * pdBlock_;            // frames processed (multiple of 64)
    if (nn <= 0) return;
    const int inCh  = std::max(1, audioInCh_);
    const int outCh = std::max(1, audioOutCh_);

    // Interleave the node's stereo audio-IN ports into libpd's input: adc~ channel
    // c comes from in-port (c/2), lane (c&1).
    for (int i = 0; i < nn; ++i)
        for (int c = 0; c < inCh; ++c) {
            float v = 0.f;
            const int bus = c / 2, lane = c & 1;
            if (bus < ctx.numAudioIn) {
                const AudioBus& ai = ctx.audioIn[bus];
                const int lc = (lane < ai.channels) ? lane : ai.channels - 1;
                if (lc >= 0) v = ai.chans[lc][i];
            }
            in_[(size_t)i * inCh + c] = v;
        }

    libpd_process_float(ticks, in_.data(), out_.data());

    // De-interleave libpd's output to the stereo audio-OUT ports: dac~ channel c
    // goes to out-port (c/2), lane (c&1).
    for (int b = 0; b < ctx.numAudioOut; ++b) {
        AudioBus& ao = ctx.audioOut[b];
        for (int lane = 0; lane < ao.channels; ++lane) {
            const int c = b * 2 + lane;
            if (c >= outCh) continue;
            for (int i = 0; i < nn && i < n; ++i)
                ao.chans[lane][i] = out_[(size_t)i * outCh + c];
        }
    }
}

}}} // namespace PatchKnob::engine::patch

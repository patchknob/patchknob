//----------------------------------------------------------------------------
//  src/engine/patch/csound_node.cpp -- embedded Csound (.csd) patch node.
//
//  Real functionality is compiled only when PATCHKNOB_HAVE_CSOUND is defined (the
//  build found a Csound library).  Without it the node still exists but is inert
//  (compile() fails cleanly, process() emits silence) so the app always links.
//----------------------------------------------------------------------------
#include "csound_node.h"

#ifdef PATCHKNOB_HAVE_CSOUND
#include <csound/csound.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace PatchKnob { namespace engine { namespace patch {

// Bytes carried by a raw MIDI status (so we feed Csound the right count).
static int midi_len(unsigned char status) {
    unsigned char hi = status & 0xF0u;
    if (status >= 0xF8u)                    return 1;   // system realtime
    if (status == 0xF1u || status == 0xF3u) return 2;
    if (status == 0xF2u)                    return 3;
    if (hi == 0xC0u || hi == 0xD0u)         return 2;   // program change / chan pressure
    return 3;                                            // note / cc / pitch-bend
}

#ifdef PATCHKNOB_HAVE_CSOUND
// ---- host-implemented MIDI input: Csound pulls the block's bytes from us -----
// (perform runs on the audio thread INSIDE process(), which stages the bytes just
// before, so this is single-threaded w.r.t. the queue.)
static int cs_midi_open (CSOUND* cs, void** ud, const char*) { *ud = csoundGetHostData(cs); return 0; }
static int cs_midi_close(CSOUND*,   void*) { return 0; }
static int cs_midi_read (CSOUND*, void* ud, unsigned char* buf, int nBytes) {
    CsoundNode* self = static_cast<CsoundNode*>(ud);
    return self ? self->drainMidi(buf, nBytes) : 0;
}
#endif

CsoundNode::CsoundNode() {
    // Blank starter CSD: ksmps 32, 2 ins + 2 outs, one empty instrument.  Valid and
    // stereo so the node's ports come up 2-in / 2-out immediately; the long `f 0`
    // score keeps performance alive so a MIDI/effect instrument can be filled in.
    csd_ =
        "<CsoundSynthesizer>\n"
        "<CsInstruments>\n"
        "sr = 48000\n"
        "ksmps = 32\n"
        "nchnls = 2\n"
        "nchnls_i = 2\n"
        "0dbfs = 1\n"
        "\n"
        "instr 1\n"
        "endin\n"
        "\n"
        "</CsInstruments>\n"
        "<CsScore>\n"
        "f 0 86400\n"
        "</CsScore>\n"
        "</CsoundSynthesizer>\n";
}

CsoundNode::~CsoundNode() {
    if (CSOUND* cs = cs_.exchange(nullptr)) destroyInstance(cs);
}

void CsoundNode::destroyInstance(CSOUND* cs) {
#ifdef PATCHKNOB_HAVE_CSOUND
    if (cs) { csoundCleanup(cs); csoundDestroy(cs); }
#else
    (void)cs;
#endif
}

bool CsoundNode::prepare(double sampleRate, int maxBlock) {
    sr_    = sampleRate > 0 ? sampleRate : 48000.0;
    block_ = maxBlock > 0 ? maxBlock : 512;
    return compile(csd_);   // so a freshly-added node makes sound without an edit
}

void CsoundNode::release() {
    if (CSOUND* cs = cs_.exchange(nullptr)) destroyInstance(cs);
}

bool CsoundNode::compile(const std::string& csdText) {
    csd_ = csdText;
#ifndef PATCHKNOB_HAVE_CSOUND
    err_ = "Csound support was not built into this binary";
    return false;
#else
    CSOUND* ncs = csoundCreate(this);
    if (!ncs) { err_ = "csoundCreate failed"; return false; }

    csoundSetHostData(ncs, this);
    csoundSetHostImplementedMIDIIO(ncs, 1);
    csoundSetExternalMidiInOpenCallback (ncs, &cs_midi_open);
    csoundSetExternalMidiReadCallback   (ncs, &cs_midi_read);
    csoundSetExternalMidiInCloseCallback(ncs, &cs_midi_close);

    csoundSetOption(ncs, "-n");    // no audio device -- we pull spout ourselves
    csoundSetOption(ncs, "-d");    // no display windows
    csoundSetOption(ncs, "-M0");   // realtime MIDI -> our callbacks
    char sropt[48];
    std::snprintf(sropt, sizeof(sropt), "--sample-rate=%d", (int)(sr_ + 0.5));
    csoundSetOption(ncs, sropt);   // force engine sample rate for correct pitch

    int r = csoundCompileCsdText(ncs, csd_.c_str());
    if (r != 0) { char b[64]; std::snprintf(b, sizeof(b), "CSD compile failed (%d)", r);
                  err_ = b; destroyInstance(ncs); return false; }
    r = csoundStart(ncs);
    if (r != 0) { char b[64]; std::snprintf(b, sizeof(b), "csoundStart failed (%d)", r);
                  err_ = b; destroyInstance(ncs); return false; }

    const int nch = (int)csoundGetNchnls(ncs);
    const int nin = (int)csoundGetNchnlsInput(ncs);
    const int kk  = (int)csoundGetKsmps(ncs);

    CSOUND* old = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);   // brief swap only (compile was off-lock)
        old = cs_.exchange(ncs, std::memory_order_acq_rel);
        nchnls_   = nch > 0 ? nch : 2;
        nchnlsIn_ = nin > 0 ? nin : 0;
        ksmps_    = kk  > 0 ? kk  : 32;
        leftover_.assign((size_t)ksmps_ * (size_t)std::max(1, nchnls_), 0.f);
        leftoverCount_ = 0;
        leftoverPos_   = 0;
        finished_      = false;
    }
    if (old) destroyInstance(old);
    err_.clear();
    return true;
#endif
}

void CsoundNode::process(const NodeProcessContext& ctx) {
    const int n = ctx.nframes;
    for (int b = 0; b < ctx.numAudioOut; ++b)          // clear every out port
        for (int c = 0; c < ctx.audioOut[b].channels; ++c)
            std::memset(ctx.audioOut[b].chans[c], 0, sizeof(float) * (size_t)n);

    // Never block the audio thread on a compile: skip (silence) this block if a
    // recompile holds the lock, or if there's no compiled engine yet.
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return;
    CSOUND* cs = cs_.load(std::memory_order_acquire);
    if (!cs || finished_) return;

#ifdef PATCHKNOB_HAVE_CSOUND
    const int outCh = std::max(1, nchnls_);
    if ((int)blockOut_.size() < n * outCh) blockOut_.assign((size_t)n * outCh, 0.f);

    // stage this block's MIDI bytes for the read callback (drained during perform).
    midiBytes_.clear(); midiPos_ = 0;
    if (ctx.numMidiIn > 0) {
        const MidiBuffer& mb = ctx.midiIn[0];
        const int filt = midiChannel_.load(std::memory_order_relaxed);
        for (int i = 0; i < mb.count; ++i) {
            const MidiEvent& m = mb.ev[i];
            const unsigned char hi = m.status & 0xF0u;
            const int ch = m.status & 0x0Fu;
            if (filt >= 0 && hi >= 0x80u && hi < 0xF0u && ch != filt) continue;
            const int len = midi_len(m.status);
            midiBytes_.push_back(m.status);
            if (len > 1) midiBytes_.push_back(m.data1);
            if (len > 2) midiBytes_.push_back(m.data2);
        }
    }

    MYFLT* spout = csoundGetSpout(cs);
    MYFLT* spin  = (nchnlsIn_ > 0) ? csoundGetSpin(cs) : nullptr;
    // audio-in ports arrive first among audio-in buses; each port is one stereo pair
    // -> Csound input channel = port*2 + laneWithinPort.
    const int nInBuses  = ctx.numAudioIn;

    int produced = 0;
    while (produced < n) {
        if (leftoverCount_ == 0) {
            if (spin && nchnlsIn_ > 0) {           // feed engine audio-in ports -> spin
                for (int j = 0; j < ksmps_; ++j) {
                    const int idx = produced + j;
                    for (int c = 0; c < nchnlsIn_; ++c) {
                        float v = 0.f;
                        const int bus = c / 2, lane = c & 1;
                        if (idx < n && bus < nInBuses) {
                            const AudioBus& ai = ctx.audioIn[bus];
                            const int lc = (lane < ai.channels) ? lane : ai.channels - 1;
                            if (lc >= 0) v = ai.chans[lc][idx];
                        }
                        spin[(size_t)j * nchnlsIn_ + c] = (MYFLT)v;
                    }
                }
            }
            if (csoundPerformKsmps(cs) != 0) { finished_ = true; return; }  // score ended
            for (int j = 0; j < ksmps_; ++j)
                for (int c = 0; c < outCh; ++c)
                    leftover_[(size_t)j * outCh + c] = (float)spout[(size_t)j * outCh + c];
            leftoverCount_ = ksmps_;
            leftoverPos_   = 0;
        }
        const int take = std::min(leftoverCount_, n - produced);
        std::memcpy(blockOut_.data() + (size_t)produced * outCh,
                    leftover_.data() + (size_t)leftoverPos_ * outCh,
                    sizeof(float) * (size_t)take * outCh);
        produced      += take;
        leftoverPos_  += take;
        leftoverCount_ -= take;
    }

    // Distribute Csound channels to the stereo out ports: out port b -> Csound
    // channels [b*2, b*2+1].
    for (int b = 0; b < ctx.numAudioOut; ++b) {
        AudioBus& ao = ctx.audioOut[b];
        for (int lane = 0; lane < ao.channels; ++lane) {
            const int csCh = b * 2 + lane;
            if (csCh >= outCh) continue;               // (already zero-cleared)
            float* dst = ao.chans[lane];
            for (int i = 0; i < n; ++i) dst[i] = blockOut_[(size_t)i * outCh + csCh];
        }
    }
#endif // PATCHKNOB_HAVE_CSOUND
}

int CsoundNode::drainMidi(unsigned char* buf, int nBytes) {
    int avail = (int)midiBytes_.size() - midiPos_;
    if (avail <= 0 || nBytes <= 0) return 0;
    int take = std::min(avail, nBytes);
    std::memcpy(buf, midiBytes_.data() + midiPos_, (size_t)take);
    midiPos_ += take;
    return take;
}

}}} // namespace PatchKnob::engine::patch

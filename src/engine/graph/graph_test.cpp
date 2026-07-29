//----------------------------------------------------------------------------
//  PatchKnob — graph module self-test.
//
//  Defines FAKE IPluginInstance implementations (no real plugins required):
//    * SineSynth : an instrument that starts a sine oscillator on note-on and
//                  stops it on note-off, writing into both stereo out channels.
//    * GainFx    : an effect that multiplies its audio input by a fixed gain.
//
//  Then builds 3 Tracks in a MixerGraph, feeds MIDI, renders ~1 s of audio in
//  blocks, and asserts:
//    * master output is non-silent,
//    * per-track and master VU values are sane,
//    * mute and solo behave correctly.
//----------------------------------------------------------------------------
#include "mixer_graph.h"
#include "track.h"
#include "vu_meter.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::engine;

static const double kSr    = 48000.0;
static const int    kBlock = 256;

// ---------------------------------------------------------------------------
// Minimal IPluginInstance scaffolding (everything not needed is a no-op).
// ---------------------------------------------------------------------------
struct FakeBase : public IPluginInstance {
    PluginDescriptor desc;
    const PluginDescriptor& descriptor() const override { return desc; }
    bool prepare(double, int) override { return true; }
    void setActive(bool) override {}
    void release() override {}
    int       paramCount() const override { return 0; }
    ParamInfo paramInfo(int) const override { return ParamInfo{}; }
    float     getParamNormalized(uint32_t) const override { return 0.0f; }
    void      setParamNormalized(uint32_t, float) override {}
    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    void loadState(const std::vector<uint8_t>&) override {}
};

// A simple sine-oscillator "synth": note-on (0x90, vel>0) starts a tone at the
// note's frequency; note-off (0x80 or 0x90 vel 0) stops it.
struct SineSynth : public FakeBase {
    double sr      = kSr;
    double phase   = 0.0;
    double freq    = 0.0;
    float  amp     = 0.0f;   // current target amplitude (0 when silent)

    SineSynth() {
        desc.name         = "FakeSineSynth";
        desc.isInstrument = true;
        desc.numAudioIn   = 0;
        desc.numAudioOut  = 2;
    }

    bool prepare(double sampleRate, int) override { sr = sampleRate; return true; }

    static double noteToHz(int n) {
        return 440.0 * std::pow(2.0, (n - 69) / 12.0);
    }

    void process(const ProcessBlock& blk) override {
        // Apply MIDI events at their sample offsets (simple: latch latest state).
        int evt = 0;
        for (int i = 0; i < blk.nframes; ++i) {
            while (evt < blk.numMidiIn && blk.midiIn[evt].sampleOffset == i) {
                const MidiEvent& m = blk.midiIn[evt];
                const uint8_t st = m.status & 0xF0;
                if (st == 0x90 && m.data2 > 0) {
                    freq = noteToHz(m.data1);
                    amp  = m.data2 / 127.0f * 0.8f;
                    phase = 0.0;
                } else if (st == 0x80 || (st == 0x90 && m.data2 == 0)) {
                    amp = 0.0f;
                }
                ++evt;
            }
            float s = 0.0f;
            if (amp > 0.0f && freq > 0.0) {
                s = amp * (float)std::sin(phase);
                phase += 2.0 * 3.14159265358979323846 * freq / sr;
                if (phase > 2.0 * 3.14159265358979323846)
                    phase -= 2.0 * 3.14159265358979323846;
            }
            blk.audioOut[0][i] = s;
            if (blk.numMidiIn >= 0) blk.audioOut[1][i] = s; // stereo (same)
        }
    }
};

// A simple gain effect: out = in * gain.
struct GainFx : public FakeBase {
    float gain = 1.0f;
    explicit GainFx(float g) : gain(g) {
        desc.name        = "FakeGainFx";
        desc.numAudioIn  = 2;
        desc.numAudioOut = 2;
    }
    void process(const ProcessBlock& blk) override {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blk.nframes; ++i)
                blk.audioOut[c][i] = blk.audioIn[c][i] * gain;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int gFail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++gFail; } \
    else         { std::printf("  ok  : %s\n", msg); } \
} while (0)

// Render `seconds` of audio through the graph, optionally feeding a note-on at
// the start. Returns the peak absolute master sample observed.
static float renderSeconds(MixerGraph& g, double seconds,
                           const std::vector<TrackBlockInput>& inputsFirst,
                           const std::vector<TrackBlockInput>& inputsRest) {
    std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
    float* out[2] = { L.data(), R.data() };
    const int totalFrames = (int)(seconds * kSr);
    int done = 0;
    float maxAbs = 0.0f;
    bool first = true;
    while (done < totalFrames) {
        const int n = std::min(kBlock, totalFrames - done);
        const auto& in = first ? inputsFirst : inputsRest;
        g.renderBlock(out, 2, n, in.empty() ? nullptr : in.data(), (int)in.size());
        for (int i = 0; i < n; ++i) {
            maxAbs = std::max(maxAbs, std::fabs(L[i]));
            maxAbs = std::max(maxAbs, std::fabs(R[i]));
        }
        done += n;
        first = false;
    }
    return maxAbs;
}

int main() {
    std::printf("=== PatchKnob graph_test ===\n");
    std::printf("sampleRate=%.0f block=%d\n\n", kSr, kBlock);

    // ---- Build 3 tracks, each with a sine synth; tracks 0/1 also have a gain FX.
    MixerGraph graph;
    graph.setTrackCount(3);

    SineSynth synth0, synth1, synth2;
    GainFx    fx0(0.5f);   // -6 dB on track 0
    GainFx    fx1(2.0f);   // +6 dB on track 1

    graph.track(0)->setInstrument(&synth0);
    graph.track(1)->setInstrument(&synth1);
    graph.track(2)->setInstrument(&synth2);

    CHECK(graph.track(0)->addFx(&fx0), "track0 addFx (gain 0.5)");
    CHECK(graph.track(1)->addFx(&fx1), "track1 addFx (gain 2.0)");

    graph.track(0)->setGain(1.0f);
    graph.track(1)->setGain(1.0f);
    graph.track(2)->setGain(1.0f);
    graph.track(0)->setPan(0.0f);
    graph.track(1)->setPan(0.0f);
    graph.track(2)->setPan(0.0f);
    graph.setMasterGain(1.0f);

    CHECK(graph.prepare(kSr, kBlock), "graph.prepare()");
    graph.setTransport(120.0, 0, true);

    // ---- Build per-track MIDI: note-on for all 3 tracks at offset 0 -----------
    MidiEvent noteOn0{0, 0x90, 60, 100};  // C4
    MidiEvent noteOn1{0, 0x90, 64, 100};  // E4
    MidiEvent noteOn2{0, 0x90, 67, 100};  // G4

    std::vector<TrackBlockInput> inFirst(3), inRest(3);
    inFirst[0] = { &noteOn0, 1, nullptr, 0 };
    inFirst[1] = { &noteOn1, 1, nullptr, 0 };
    inFirst[2] = { &noteOn2, 1, nullptr, 0 };
    // subsequent blocks: no new events (note sustains)

    // =====================================================================
    std::printf("\n[1] All tracks audible -- master should be non-silent\n");
    float peak = renderSeconds(graph, 0.25, inFirst, inRest);
    std::printf("    master peak (rendered)        = %.4f\n", peak);
    std::printf("    track0 VU  peakL=%.4f rmsL=%.4f\n",
                graph.track(0)->vuLeft().peak(), graph.track(0)->vuLeft().rms());
    std::printf("    track1 VU  peakL=%.4f rmsL=%.4f\n",
                graph.track(1)->vuLeft().peak(), graph.track(1)->vuLeft().rms());
    std::printf("    track2 VU  peakL=%.4f rmsL=%.4f\n",
                graph.track(2)->vuLeft().peak(), graph.track(2)->vuLeft().rms());
    std::printf("    master VU  peakL=%.4f rmsL=%.4f  peakR=%.4f\n",
                graph.masterVuLeft().peak(), graph.masterVuLeft().rms(),
                graph.masterVuRight().peak());

    CHECK(peak > 0.01f, "master output non-silent");
    CHECK(graph.track(0)->vuLeft().peak() > 0.0f, "track0 VU non-zero");
    CHECK(graph.track(1)->vuLeft().peak() > 0.0f, "track1 VU non-zero");
    CHECK(graph.track(2)->vuLeft().peak() > 0.0f, "track2 VU non-zero");
    CHECK(graph.masterVuLeft().peak() > 0.0f, "master VU non-zero");
    // VU sanity: peak >= rms, values in a sane range (< ~4).
    CHECK(graph.masterVuLeft().peak() >= graph.masterVuLeft().rms(),
          "master peak >= rms");
    CHECK(graph.masterVuLeft().peak() < 4.0f, "master peak in sane range");
    // track1 had +6 dB FX, track0 -6 dB FX => track1 VU should exceed track0.
    CHECK(graph.track(1)->vuLeft().peak() > graph.track(0)->vuLeft().peak(),
          "track1 (+6dB FX) louder than track0 (-6dB FX)");

    // =====================================================================
    std::printf("\n[2] MUTE track1 -- its contribution drops out of master\n");
    // Capture master peak with all audible, then with track1 muted, same notes.
    graph.track(0)->setMute(false);
    graph.track(1)->setMute(false);
    graph.track(2)->setMute(false);
    float peakAll = renderSeconds(graph, 0.20, inFirst, inRest);

    graph.track(1)->setMute(true);
    float vuT1BeforeMute = graph.track(1)->vuLeft().peak();
    float peakMuted = renderSeconds(graph, 0.20, inFirst, inRest);
    float vuT1AfterMute = graph.track(1)->vuLeft().peak();
    std::printf("    master peak all=%.4f  track1-muted=%.4f\n", peakAll, peakMuted);
    std::printf("    track1 VU before mute=%.4f after 0.2s muted=%.4f (out zeroed, VU decaying)\n",
                vuT1BeforeMute, vuT1AfterMute);
    CHECK(peakMuted < peakAll, "muting track1 lowers master peak");
    // Muted track's output is hard-zeroed, so its VU only sees zeros and must
    // decay (not climb). Confirm it dropped while muted.
    CHECK(vuT1AfterMute < vuT1BeforeMute,
          "muted track1 VU decays (own output zeroed)");
    graph.track(1)->setMute(false);

    // =====================================================================
    std::printf("\n[3] SOLO track2 -- only track2 reaches master\n");
    graph.track(2)->setSolo(true);
    float peakSolo = renderSeconds(graph, 0.20, inFirst, inRest);
    // Render track2 alone (no solo, others muted) for comparison.
    graph.track(2)->setSolo(false);
    graph.track(0)->setMute(true);
    graph.track(1)->setMute(true);
    float peakT2Only = renderSeconds(graph, 0.20, inFirst, inRest);
    graph.track(0)->setMute(false);
    graph.track(1)->setMute(false);

    std::printf("    master peak solo(track2)=%.4f  track2-only=%.4f\n",
                peakSolo, peakT2Only);
    std::printf("    solo: track0 VU=%.4f track1 VU=%.4f track2 VU=%.4f\n",
                graph.track(0)->vuLeft().peak(),
                graph.track(1)->vuLeft().peak(),
                graph.track(2)->vuLeft().peak());
    CHECK(std::fabs(peakSolo - peakT2Only) < 1e-3f,
          "solo(track2) == track2-only master peak");
    CHECK(peakSolo < peakAll, "solo master quieter than full mix");

    // =====================================================================
    std::printf("\n[4] FX hot-swap during processing (add/remove/reorder)\n");
    // Solo track2 so only its signal reaches master; we then watch track2's
    // own VU before vs after inserting a 0.25x gain FX mid-stream.
    graph.track(2)->setSolo(true);
    renderSeconds(graph, 0.15, inFirst, inRest);   // settle with no extra FX
    float t2VuNoExtra = graph.track(2)->vuLeft().peak();

    GainFx fxExtra(0.25f);
    fxExtra.prepare(kSr, kBlock);
    CHECK(graph.track(2)->addFx(&fxExtra), "addFx to track2 (hot-swap)");
    CHECK(graph.track(2)->fxCount() == 1, "track2 fxCount==1 after add");
    renderSeconds(graph, 0.15, inFirst, inRest);   // settle with 0.25x FX live
    float t2VuWithExtra = graph.track(2)->vuLeft().peak();
    CHECK(graph.track(2)->removeFx(0), "removeFx from track2 (hot-swap)");
    CHECK(graph.track(2)->fxCount() == 0, "track2 fxCount==0 after remove");
    graph.track(2)->setSolo(false);
    std::printf("    track2 VU  no-extra=%.4f  with-0.25x-FX=%.4f\n",
                t2VuNoExtra, t2VuWithExtra);
    CHECK(t2VuWithExtra > 0.0f && t2VuWithExtra < t2VuNoExtra,
          "hot-swapped 0.25x FX attenuated track2 VU");

    // reorder test (two FX on a fresh track-ish check using track0)
    GainFx a(1.0f), b(1.0f);
    a.prepare(kSr, kBlock); b.prepare(kSr, kBlock);
    Track rt;
    rt.setInstrument(&synth0);
    rt.prepare(kSr, kBlock);
    CHECK(rt.addFx(&a) && rt.addFx(&b), "reorder: add two FX");
    CHECK(rt.fxAt(0) == &a && rt.fxAt(1) == &b, "reorder: initial order a,b");
    CHECK(rt.moveFx(0, 1), "reorder: moveFx(0,1)");
    CHECK(rt.fxAt(0) == &b && rt.fxAt(1) == &a, "reorder: now b,a");

    // =====================================================================
    std::printf("\n[5] VU release ballistics decay toward 0 in silence\n");
    // stop notes
    MidiEvent off0{0,0x80,60,0}, off1{0,0x80,64,0}, off2{0,0x80,67,0};
    std::vector<TrackBlockInput> stopFirst(3);
    stopFirst[0] = {&off0,1,nullptr,0};
    stopFirst[1] = {&off1,1,nullptr,0};
    stopFirst[2] = {&off2,1,nullptr,0};
    std::vector<TrackBlockInput> none;
    float beforeSilence = graph.masterVuLeft().peak();
    renderSeconds(graph, 1.0, stopFirst, none);  // 1s of silence after note-off
    float afterSilence = graph.masterVuLeft().peak();
    std::printf("    master VU before silence=%.4f after 1s silence=%.6f\n",
                beforeSilence, afterSilence);
    CHECK(afterSilence < beforeSilence, "VU decays during silence");
    // With a 300 ms release time constant, 1 s is ~3.3 tau => ~e^-3.3 (~3.7%)
    // of the pre-silence level remains. Assert it has fallen well below 10%.
    CHECK(afterSilence < beforeSilence * 0.10f, "VU fell below 10% after 1s silence");

    // =====================================================================
    std::printf("\n=== %s (%d failure%s) ===\n",
                gFail == 0 ? "PASS" : "FAIL", gFail, gFail == 1 ? "" : "s");
    return gFail == 0 ? 0 : 1;
}

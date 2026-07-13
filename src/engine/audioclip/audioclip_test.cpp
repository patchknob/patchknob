//----------------------------------------------------------------------------
//  seq24 Windows port — audioclip module self-test.
//
//  Exercises the built-in audio-track engine end to end, through the same
//  IPluginInstance surface the graph Track uses:
//
//    [1] Scheduled playback: synth two sine clips, place them at different
//        startSamples on the timeline, render process() block-by-block across
//        the whole timeline and assert the output is NON-SILENT exactly during
//        each clip's scheduled window and SILENT everywhere else. Also checks
//        the region where the two clips overlap sums both.
//    [2] Record mode: arm the recorder, feed known audio via captureBlock() in
//        blocks, stop, and assert the resulting AudioClip captured every frame
//        exactly.
//    [3] WAV round-trip: write a 16-bit stereo WAV at 44100, load it back
//        targeting 48000, and assert the loader + linear resampler produced a
//        correctly-lengthed, non-silent clip.
//    [4] Transport gating: with isPlaying=false the player emits silence.
//----------------------------------------------------------------------------
#include "audio_clip.h"
#include "audio_clip_player.h"
#include "wav_loader.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace seq24::engine;

static const double kSr    = 48000.0;
static const int    kBlock = 256;

static int gFail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++gFail; } \
    else         { std::printf("  ok  : %s\n", msg); } \
} while (0)

// Peak absolute value across both channels over one block.
static float blockPeak(const float* l, const float* r, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; ++i) {
        m = std::max(m, std::fabs(l[i]));
        m = std::max(m, std::fabs(r[i]));
    }
    return m;
}

int main() {
    std::printf("=== seq24 audioclip_test ===\n");
    std::printf("sampleRate=%.0f block=%d\n\n", kSr, kBlock);

    // =====================================================================
    // [1] Scheduled playback across a timeline.
    // =====================================================================
    std::printf("[1] Scheduled clip playback\n");

    // Two 0.10 s sine clips at different pitches.
    AudioClip clipA = AudioClip::synth_sine(440.0, 0.10, kSr, 0.8f, "A440");
    AudioClip clipB = AudioClip::synth_sine(660.0, 0.10, kSr, 0.5f, "B660");
    const int64_t lenA = clipA.numFrames();     // 4800 frames
    const int64_t lenB = clipB.numFrames();     // 4800 frames
    CHECK(lenA == 4800 && lenB == 4800, "synth_sine produced 0.10s clips");
    CHECK(!clipA.empty() && clipA.ch[0].size() == clipA.ch[1].size(),
          "clip is stereo, both channels equal length");

    // Timeline placement:
    //   clipA : [ 2400 ,  7200 )
    //   clipB : [ 6000 , 10800 )   -> overlaps A on [6000,7200)
    const int64_t startA = 2400;
    const int64_t startB = 6000;

    AudioClipPlayer player;
    CHECK(player.prepare(kSr, kBlock), "player.prepare()");
    player.setActive(true);
    CHECK(player.addClip(&clipA, startA, 1.0f), "addClip A @2400");
    CHECK(player.addClip(&clipB, startB, 1.0f), "addClip B @6000");
    CHECK(player.clipCount() == 2, "clipCount == 2");
    CHECK(!player.addClip(nullptr, 0), "addClip(null) rejected");

    // Expected non-silent union window on the timeline: [2400, 10800).
    const int64_t soundStart = startA;                       // 2400
    const int64_t soundEnd   = startB + lenB;                // 10800

    // Render the whole timeline in blocks; classify each sample as silent or not.
    const int64_t timelineLen = 14000;  // past the end of both clips
    std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
    float* out[2] = { L.data(), R.data() };

    // Build a sample-accurate reference: what the mix SHOULD be at every
    // timeline sample (sum of overlapping clips). Compare the player's block
    // output against it exactly — this proves placement to the sample without
    // being fooled by a sine's legitimately-zero value at its zero crossings
    // (e.g. clip frame 0 is sin(0)=0, yet the clip IS scheduled there).
    auto refAt = [&](int64_t t, float& l, float& r) {
        l = 0.0f; r = 0.0f;
        if (t >= startA && t < startA + lenA) {
            l += clipA.ch[0][(size_t)(t - startA)];
            r += clipA.ch[1][(size_t)(t - startA)];
        }
        if (t >= startB && t < startB + lenB) {
            l += clipB.ch[0][(size_t)(t - startB)];
            r += clipB.ch[1][(size_t)(t - startB)];
        }
    };

    float  maxErr = 0.0f;                 // max |rendered - reference|
    bool   silenceBefore = true, silenceAfter = true;
    double energyInWindow = 0.0;          // energy inside [soundStart, soundEnd)
    float  overlapPeak = 0.0f, aOnlyPeak = 0.0f;
    int64_t pos = 0;
    while (pos < timelineLen) {
        const int n = (int)std::min<int64_t>(kBlock, timelineLen - pos);
        ProcessBlock blk{};
        const float* inNull[2] = { nullptr, nullptr };
        float*       outPtr[2] = { out[0], out[1] };
        blk.audioIn             = inNull;
        blk.audioOut            = outPtr;
        blk.nframes             = n;
        blk.midiIn = nullptr; blk.numMidiIn = 0;
        blk.paramIn = nullptr; blk.numParamIn = 0;
        blk.tempoBpm            = 120.0;
        blk.playPositionSamples = pos;
        blk.isPlaying           = true;
        player.process(blk);

        for (int i = 0; i < n; ++i) {
            const int64_t t = pos + i;
            float el, er; refAt(t, el, er);
            maxErr = std::max(maxErr, std::fabs(L[(size_t)i] - el));
            maxErr = std::max(maxErr, std::fabs(R[(size_t)i] - er));

            const float mag = std::max(std::fabs(L[(size_t)i]), std::fabs(R[(size_t)i]));
            // Outside the union window the output must be exactly zero.
            if (t < soundStart && mag != 0.0f) silenceBefore = false;
            if (t >= soundEnd  && mag != 0.0f) silenceAfter  = false;
            if (t >= soundStart && t < soundEnd) energyInWindow += (double)mag * mag;
            // Peaks: region where only A plays vs the A+B overlap.
            if (t >= 3000 && t < 3200) aOnlyPeak = std::max(aOnlyPeak, mag);
            if (t >= 6000 && t < 7200) overlapPeak = std::max(overlapPeak, mag);
        }
        pos += n;
    }

    std::printf("    max sample error vs reference = %.2e\n", maxErr);
    std::printf("    energy in [%lld,%lld)=%.1f  A-only peak=%.4f  overlap peak=%.4f\n",
                (long long)soundStart, (long long)soundEnd,
                energyInWindow, aOnlyPeak, overlapPeak);

    CHECK(maxErr < 1e-6f, "block output is sample-accurate vs scheduled reference");
    CHECK(silenceBefore, "silent before first clip (exact zero)");
    CHECK(silenceAfter,  "silent after last clip (exact zero)");
    CHECK(energyInWindow > 1.0, "scheduled window carries audio energy (non-silent)");
    // In the overlap the two sines sum, so the peak should exceed A-alone's
    // amplitude (0.8) at least somewhere (0.8 + up-to-0.5).
    CHECK(aOnlyPeak > 0.5f && aOnlyPeak <= 0.8001f, "A-only region ~amp 0.8");
    CHECK(overlapPeak > 0.8001f, "overlap region sums both clips (>0.8)");

    // Remove one clip and re-render a block inside B-only-formerly region.
    CHECK(player.removeClip(0), "removeClip(0)");
    CHECK(player.clipCount() == 1, "clipCount == 1 after remove");
    {
        ProcessBlock blk{};
        const float* inNull[2] = { nullptr, nullptr };
        float*       outPtr[2] = { out[0], out[1] };
        blk.audioIn = inNull; blk.audioOut = outPtr; blk.nframes = kBlock;
        blk.midiIn=nullptr; blk.numMidiIn=0; blk.paramIn=nullptr; blk.numParamIn=0;
        blk.tempoBpm = 120.0; blk.playPositionSamples = 3000; blk.isPlaying = true;
        player.process(blk);
        CHECK(blockPeak(L.data(), R.data(), kBlock) < 1e-6f,
              "after removing A, region @3000 is silent (only B remains)");
    }
    player.clearClips();
    CHECK(player.clipCount() == 0, "clearClips empties schedule");

    // =====================================================================
    // [2] Record mode captures fed audio exactly.
    // =====================================================================
    std::printf("\n[2] Record capture\n");
    AudioClipPlayer rec;
    rec.prepare(kSr, kBlock);
    rec.startRecord(5.0);   // reserve 5 s
    CHECK(rec.isRecording(), "isRecording() true after startRecord");

    // Feed 10 blocks of a known ramp so we can verify every sample.
    const int nBlocks = 10;
    int64_t fed = 0;
    for (int b = 0; b < nBlocks; ++b) {
        std::vector<float> cl((size_t)kBlock), cr((size_t)kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float v = (float)(fed + i);
            cl[(size_t)i] = v;             // left  = absolute sample index
            cr[(size_t)i] = -v;            // right = negated
        }
        const float* in[2] = { cl.data(), cr.data() };
        rec.captureBlock(in, kBlock);
        fed += kBlock;
    }
    CHECK(rec.recordedFrames() == fed, "recordedFrames == fed frames");

    auto recClip = rec.stopRecord("take1");
    CHECK(!rec.isRecording(), "isRecording() false after stopRecord");
    CHECK(recClip && recClip->numFrames() == fed, "recorded clip length == fed");

    bool sampleMatch = (recClip->numFrames() == fed);
    for (int64_t i = 0; i < recClip->numFrames() && sampleMatch; ++i) {
        if (recClip->ch[0][(size_t)i] != (float)i)  sampleMatch = false;
        if (recClip->ch[1][(size_t)i] != -(float)i) sampleMatch = false;
    }
    CHECK(sampleMatch, "every recorded sample matches the fed ramp (L & R)");

    // Record via process(): armed player captures blk.audioIn automatically.
    AudioClipPlayer recP;
    recP.prepare(kSr, kBlock);
    recP.startRecord(1.0);
    {
        std::vector<float> cl((size_t)kBlock, 0.25f), cr((size_t)kBlock, -0.25f);
        std::vector<float> ol((size_t)kBlock), orr((size_t)kBlock);
        const float* in[2]  = { cl.data(), cr.data() };
        float*       op[2]  = { ol.data(), orr.data() };
        ProcessBlock blk{};
        blk.audioIn = in; blk.audioOut = op; blk.nframes = kBlock;
        blk.midiIn=nullptr; blk.numMidiIn=0; blk.paramIn=nullptr; blk.numParamIn=0;
        blk.tempoBpm=120.0; blk.playPositionSamples=0; blk.isPlaying=true;
        recP.process(blk);
    }
    auto recPClip = recP.stopRecord("via_process");
    CHECK(recPClip && recPClip->numFrames() == kBlock,
          "process() captured its audioIn while armed");
    CHECK(recPClip && recPClip->numFrames() > 0 &&
          std::fabs(recPClip->ch[0][0] - 0.25f) < 1e-6f,
          "process()-captured samples match fed input");

    // =====================================================================
    // [3] WAV round-trip: write 44100 stereo, load @48000 (resample).
    // =====================================================================
    std::printf("\n[3] WAV load + resample\n");
    const double fileRate = 44100.0;
    AudioClip src = AudioClip::synth_sine(1000.0, 0.20, fileRate, 0.7f, "src");
    const std::string wavPath = "audioclip_roundtrip.wav";
    std::string werr;
    CHECK(saveWav16(wavPath, src, &werr), "saveWav16 wrote 44100 stereo file");

    AudioClip loaded;
    std::string lerr;
    bool ok = loadWav(wavPath, kSr, loaded, &lerr);
    if (!ok) std::printf("    loadWav error: %s\n", lerr.c_str());
    CHECK(ok, "loadWav succeeded");
    CHECK(std::fabs(loaded.sourceSampleRate - fileRate) < 1.0,
          "sourceSampleRate metadata preserved (44100)");
    CHECK(std::fabs(loaded.sampleRate - kSr) < 1.0, "clip sampleRate == engine rate");
    // 0.20 s at 48000 = 9600 frames (within a couple frames of resample rounding).
    const int64_t expectFrames = (int64_t)(0.20 * kSr + 0.5);
    std::printf("    loaded frames=%lld (expected ~%lld)\n",
                (long long)loaded.numFrames(), (long long)expectFrames);
    CHECK(std::llabs((long long)loaded.numFrames() - (long long)expectFrames) <= 4,
          "resampled length ~ 0.20s @48000");
    float lpk = 0.0f;
    for (size_t i = 0; i < loaded.ch[0].size(); ++i)
        lpk = std::max(lpk, std::fabs(loaded.ch[0][i]));
    std::printf("    loaded peak=%.4f (source amp 0.7)\n", lpk);
    CHECK(lpk > 0.5f && lpk < 0.8f, "loaded audio non-silent, amplitude preserved");
    std::remove(wavPath.c_str());

    // =====================================================================
    // [4] Transport gating: not playing -> silence.
    // =====================================================================
    std::printf("\n[4] Transport gating\n");
    AudioClipPlayer p2;
    p2.prepare(kSr, kBlock);
    p2.addClip(&clipA, 0, 1.0f);
    {
        std::vector<float> ol((size_t)kBlock), orr((size_t)kBlock);
        const float* inNull[2] = { nullptr, nullptr };
        float*       op[2] = { ol.data(), orr.data() };
        ProcessBlock blk{};
        blk.audioIn=inNull; blk.audioOut=op; blk.nframes=kBlock;
        blk.midiIn=nullptr; blk.numMidiIn=0; blk.paramIn=nullptr; blk.numParamIn=0;
        blk.tempoBpm=120.0; blk.playPositionSamples=0; blk.isPlaying=false;
        p2.process(blk);
        CHECK(blockPeak(ol.data(), orr.data(), kBlock) < 1e-6f,
              "isPlaying=false yields silence");
        blk.isPlaying = true;
        p2.process(blk);
        CHECK(blockPeak(ol.data(), orr.data(), kBlock) > 0.1f,
              "isPlaying=true yields audio at clip start");
    }

    // =====================================================================
    std::printf("\n=== %s (%d failure%s) ===\n",
                gFail == 0 ? "PASS" : "FAIL", gFail, gFail == 1 ? "" : "s");
    return gFail == 0 ? 0 : 1;
}

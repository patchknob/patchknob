//----------------------------------------------------------------------------
//  PatchKnob — audioclip module self-test.
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
//    [5] DECLICK: no region edge, loop wrap or transport stop may step faster
//        than the material itself does.
//    [6] LOOP PERIOD: a looped region repeats its own trimmed span, not the
//        whole rest of the source.
//    [7] REALTIME WARP: survives schedule edits, and clearWarp(nullptr) really
//        purges (so a new clip cannot inherit a dead one's stretch).
//    [8] RESAMPLER: band-limited, so 44.1k imports keep their top octave and a
//        96k import does not fold its ultrasonics down into the audio band.
//    [9] RECORD teardown: stopping while the audio thread is still capturing.
//   [10] WARP under contention: losing the warp lock holds the stretch instead
//        of swapping in raw (differently pitched, differently timed) playback.
//----------------------------------------------------------------------------
#include "audio_clip.h"
#include "audio_clip_player.h"
#include "wav_loader.h"

#include <atomic>
#include <chrono>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "warp_stretch.h"

using namespace PatchKnob::engine;

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
    std::printf("=== PatchKnob audioclip_test ===\n");
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
    // Regions with no fades of their own still get the player's ~2 ms declick
    // ramp at each edge (see [5]); the reference applies the very same curve
    // through the public helper, so this stays an exact sample-for-sample check.
    const ScheduledClip refScA = player.clipAt(0), refScB = player.clipAt(1);
    const int64_t declickN = player.declickFrames();
    auto refAt = [&](int64_t t, float& l, float& r) {
        l = 0.0f; r = 0.0f;
        if (t >= startA && t < startA + lenA) {
            const int64_t ri = t - startA;
            const float g = refScA.edgeDeclickGain(ri, lenA, declickN);
            l += clipA.ch[0][(size_t)ri] * g;
            r += clipA.ch[1][(size_t)ri] * g;
        }
        if (t >= startB && t < startB + lenB) {
            const int64_t ri = t - startB;
            const float g = refScB.edgeDeclickGain(ri, lenB, declickN);
            l += clipB.ch[0][(size_t)ri] * g;
            r += clipB.ch[1][(size_t)ri] * g;
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
    // [5] DECLICK: region edges, loop wraps and the transport stop.
    //
    // The yardstick is the material's OWN maximum sample-to-sample step: a
    // splice that jumps many times further than the waveform ever does is a
    // click.  1000 frames of 137 Hz is deliberately not a whole number of
    // cycles -- exactly what a hand-trimmed one-bar loop looks like.
    // =====================================================================
    std::printf("\n[5] Declick (region edges / loop wrap / transport stop)\n");
    {
        const double kHz = 137.0;
        AudioClip lp = AudioClip::synth_sine(kHz, 1000.0 / kSr, kSr, 0.8f, "loop");
        const int64_t N = lp.numFrames();
        const float slew = (float)(2.0 * 3.14159265358979 * kHz / kSr) * 0.8f;

        std::vector<float> l((size_t)kBlock), r((size_t)kBlock);
        float* op[2] = { l.data(), r.data() };
        auto renderInto = [&](AudioClipPlayer& p, int64_t from, int64_t to,
                              bool playing, std::vector<float>& acc) {
            for (int64_t q = from; q < to; q += kBlock) {
                ProcessBlock b{};
                b.nframes = kBlock; b.audioOut = op; b.numAudioOut = 2;
                b.isPlaying = playing; b.playPositionSamples = q;
                p.process(b);
                for (int i = 0; i < kBlock; ++i) acc.push_back(l[(size_t)i]);
            }
        };
        auto maxJump = [](const std::vector<float>& v) {
            float m = 0.0f;
            for (size_t i = 1; i < v.size(); ++i) m = std::max(m, std::fabs(v[i] - v[i - 1]));
            return m;
        };

        {   // looped region: six passes over the same 1000 frames
            AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
            p.addClip(&lp, 0, 1.0f);
            p.setClipLoop(0, true);
            p.setClipRegion(0, 0, 0, N * 6);
            std::vector<float> v; renderInto(p, 0, N * 6, true, v);
            const float j = maxJump(v);
            std::printf("    loop wrap  max step %.5f (%.1fx the material's own %.5f)\n",
                        j, j / slew, slew);
            CHECK(j < slew * 2.0f, "looped region wraps without a click");
        }
        {   // trimmed + slipped region: starts and ends mid-waveform
            AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
            p.addClip(&lp, 512, 1.0f);
            p.setClipRegion(0, 512, 300, 400);
            std::vector<float> v; renderInto(p, 0, 2048, true, v);
            const float j = maxJump(v);
            std::printf("    region edge max step %.5f (%.1fx)\n", j, j / slew);
            CHECK(j < slew * 2.0f, "trimmed region starts/ends without a click");
        }
        {   // transport stop mid-clip
            AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
            p.addClip(&lp, 0, 1.0f);
            p.setClipRegion(0, 0, 0, N);
            std::vector<float> v;
            renderInto(p, 0, kBlock * 2, true,  v);
            renderInto(p, kBlock * 2, kBlock * 4, false, v);
            const float j = maxJump(v);
            std::printf("    stop       max step %.5f (%.1fx)\n", j, j / slew);
            CHECK(j < slew * 2.0f, "transport stop ramps out instead of cutting");
            bool endsSilent = true;
            for (size_t i = v.size() - kBlock; i < v.size(); ++i)
                if (std::fabs(v[i]) > 1e-6f) endsSilent = false;
            CHECK(endsSilent, "stop tail is over within one declick window");
        }
        {   // a player that never rolled must be exactly silent when stopped
            AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
            p.addClip(&lp, 0, 1.0f);
            std::vector<float> v; renderInto(p, 0, kBlock, false, v);
            float pk = 0.0f; for (float x : v) pk = std::max(pk, std::fabs(x));
            CHECK(pk == 0.0f, "stopped-from-the-start player emits exact silence");
        }
    }

    // =====================================================================
    // [6] LOOP PERIOD is the region's own trimmed span.
    //
    // Trim one 1000-frame bar out of a 4000-frame source, switch looping on,
    // then drag the region out to four bars: every bar must be that FIRST bar
    // again, not frames 1000.. of the file.
    // =====================================================================
    std::printf("\n[6] Looped region repeats its trimmed span\n");
    {
        AudioClip src; src.resize(4000);
        for (int64_t i = 0; i < 4000; ++i) {          // each 1000-frame bar has its own level
            const float v = 0.2f * (float)(1 + i / 1000);
            src.ch[0][(size_t)i] = v; src.ch[1][(size_t)i] = v;
        }
        AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
        p.addClip(&src, 0, 1.0f);
        p.setClipRegion(0, 0, 0, 1000);               // trim to the FIRST bar
        p.setClipLoop(0, true);                       // captures the 1000-frame period
        p.setClipRegion(0, 0, 0, 4000);               // extend to four bars

        std::vector<float> l((size_t)kBlock), r((size_t)kBlock);
        float* op[2] = { l.data(), r.data() };
        std::vector<float> v;
        for (int64_t q = 0; q < 4000; q += kBlock) {
            ProcessBlock b{};
            b.nframes = kBlock; b.audioOut = op; b.numAudioOut = 2;
            b.isPlaying = true; b.playPositionSamples = q;
            p.process(b);
            for (int i = 0; i < kBlock; ++i) v.push_back(l[(size_t)i]);
        }
        // Sample the middle of each bar, clear of every declick window.
        float bar[4];
        for (int k = 0; k < 4; ++k) bar[k] = v[(size_t)(k * 1000 + 500)];
        std::printf("    bar levels: %.3f %.3f %.3f %.3f (source bars are 0.2/0.4/0.6/0.8)\n",
                    bar[0], bar[1], bar[2], bar[3]);
        CHECK(std::fabs(bar[0] - 0.2f) < 1e-4f && std::fabs(bar[1] - 0.2f) < 1e-4f &&
              std::fabs(bar[2] - 0.2f) < 1e-4f && std::fabs(bar[3] - 0.2f) < 1e-4f,
              "every loop pass replays the trimmed bar (not the rest of the file)");

        // An UNtrimmed looped region still wraps the whole source from its
        // offset, which is the historical behaviour.
        AudioClipPlayer p2; p2.prepare(kSr, kBlock); p2.setActive(true);
        p2.addClip(&src, 0, 1.0f);
        p2.setClipLoop(0, true);
        p2.setClipRegion(0, 0, 0, 8000);
        std::vector<float> v2;
        for (int64_t q = 0; q < 8000; q += kBlock) {
            ProcessBlock b{};
            b.nframes = kBlock; b.audioOut = op; b.numAudioOut = 2;
            b.isPlaying = true; b.playPositionSamples = q;
            p2.process(b);
            for (int i = 0; i < kBlock; ++i) v2.push_back(l[(size_t)i]);
        }
        CHECK(std::fabs(v2[(size_t)1500] - 0.4f) < 1e-4f &&
              std::fabs(v2[(size_t)5500] - 0.4f) < 1e-4f,
              "an untrimmed looped region still wraps the whole source");
    }

    // =====================================================================
    // [7] Realtime warp: schedule edits, and clearWarp(nullptr).
    // =====================================================================
    std::printf("\n[7] Realtime warp survives editing\n");
    {
        AudioClip w = AudioClip::synth_sine(1000.0, 0.25, kSr, 0.8f, "warped");
        const int64_t N = w.numFrames();            // 12000
        const int64_t DST = N * 4;                  // stretched 4x

        AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
        p.addClip(&w, 0, 1.0f);
        p.setClipRegion(0, 0, 0, DST);
        std::vector<WarpMarker> markers;
        WarpMarker m0; m0.srcSample = 0; m0.dstSample = 0;
        WarpMarker m1; m1.srcSample = N; m1.dstSample = DST;
        markers.push_back(m0); markers.push_back(m1);
        p.setWarp(&w, markers);

        std::vector<float> l((size_t)kBlock), r((size_t)kBlock);
        float* op[2] = { l.data(), r.data() };
        // Peak inside a window that ONLY a working stretch can fill: it lies
        // past the end of the raw source.
        auto peakPastSource = [&](AudioClipPlayer& pl) {
            float pk = 0.0f;
            for (int64_t q = 0; q < DST; q += kBlock) {
                ProcessBlock b{};
                b.nframes = kBlock; b.audioOut = op; b.numAudioOut = 2;
                b.isPlaying = true; b.playPositionSamples = q;
                pl.process(b);
                for (int i = 0; i < kBlock; ++i) {
                    const int64_t t = q + i;
                    if (t >= N + 4000 && t < N + 8000) pk = std::max(pk, std::fabs(l[(size_t)i]));
                }
            }
            return pk;
        };
        const float before = peakPastSource(p);
        p.setClipGain(0, 1.0f);                 // any ordinary schedule edit
        const float after  = peakPastSource(p);
        std::printf("    warped peak past the source: before edit %.4f, after %.4f\n",
                    before, after);
        CHECK(before > 0.1f, "realtime warp fills the stretched region");
        CHECK(after  > 0.1f, "warp still runs after a schedule edit (owner is the region id)");

        // clearWarp(nullptr) must purge EVERY entry: warp state is keyed on a
        // raw AudioClip*, and a recycled address inherited a dead clip's map.
        p.clearWarp(nullptr);
        const float cleared = peakPastSource(p);
        std::printf("    after clearWarp(nullptr): %.4f (raw region, must be silent there)\n",
                    cleared);
        CHECK(cleared < 1e-6f, "clearWarp(nullptr) purges every warp entry");
    }

    // =====================================================================
    // [8] Resampler is band-limited (no aliasing, no dulling).
    // =====================================================================
    std::printf("\n[8] Band-limited resampling\n");
    {
        // Amplitude of frequency f in x, by direct correlation.
        auto ampAt = [](const std::vector<float>& x, double f, double sr) {
            double re = 0.0, im = 0.0;
            const size_t n = x.size();
            for (size_t i = 0; i < n; ++i) {
                const double a = 2.0 * 3.14159265358979 * f * (double)i / sr;
                re += x[i] * std::cos(a); im -= x[i] * std::sin(a);
            }
            return n ? 2.0 * std::sqrt(re * re + im * im) / (double)n : 0.0;
        };
        auto through = [&](double rate, double hz, AudioClip& out) {
            AudioClip t = AudioClip::synth_sine(hz, 0.5, rate, 0.8f, "t");
            std::string e;
            const std::string path = "audioclip_rs.wav";
            bool okw = saveWav16(path, t, &e);
            bool okr = okw && loadWav(path, kSr, out, &e);
            std::remove(path.c_str());
            return okr;
        };
        AudioClip hi, top, ultra;
        CHECK(through(44100.0, 10000.0, hi),  "44.1k 10 kHz tone loaded");
        CHECK(through(44100.0, 15000.0, top), "44.1k 15 kHz tone loaded");
        CHECK(through(96000.0, 30000.0, ultra), "96k 30 kHz tone loaded");
        const double a10 = ampAt(hi.ch[0], 10000.0, kSr);
        const double a15 = ampAt(top.ch[0], 15000.0, kSr);
        const double alias = ampAt(ultra.ch[0], 18000.0, kSr);
        std::printf("    44.1k->48k: 10 kHz %+.2f dB, 15 kHz %+.2f dB;  96k->48k 30 kHz"
                    " folds to %.1f dBFS at 18 kHz\n",
                    20.0 * std::log10(a10 / 0.8), 20.0 * std::log10(a15 / 0.8),
                    20.0 * std::log10(alias + 1e-12));
        CHECK(a10 > 0.8 * 0.9 && a10 < 0.8 * 1.1, "10 kHz survives a 44.1k import (was -1.50 dB)");
        CHECK(a15 > 0.8 * 0.9 && a15 < 0.8 * 1.1, "15 kHz survives a 44.1k import (was -3.44 dB)");
        CHECK(alias < 0.01, "96k -> 48k does not alias its ultrasonics down (was 0.80)");
    }

    // =====================================================================
    // [9] Record teardown while the audio thread is still capturing.
    // =====================================================================
    std::printf("\n[9] Record teardown keep-alive\n");
    {
        AudioClipPlayer p;
        p.prepare(kSr, kBlock);
        p.startRecord(2.0);
        std::atomic<bool> stop{ false };
        std::vector<float> cl((size_t)kBlock, 0.5f), cr((size_t)kBlock, -0.5f);
        const float* in[2] = { cl.data(), cr.data() };
        // A stand-in audio thread hammering captureBlock across the teardown:
        // stopRecord() FREES the storage those writes land in, so without the
        // keep-alive handshake this is a write to freed memory.
        std::thread audio([&] {
            while (!stop.load(std::memory_order_acquire)) p.captureBlock(in, kBlock, 2);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto take = p.stopRecord("racy");
        const bool sane = take && take->numFrames() >= 0 &&
                          take->numFrames() <= (int64_t)(2.0 * kSr) &&
                          take->ch[0].size() == take->ch[1].size();
        // Keep hammering for a moment AFTER the free: a capture that slipped
        // through would be writing into released storage right now.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop.store(true, std::memory_order_release);
        audio.join();
        CHECK(!p.isRecording(), "stopRecord disarms");
        CHECK(sane, "stopRecord returns a well-formed take taken across live captures");
    }

    // =====================================================================
    // [10] Warp under lock contention holds, it does not fall back to raw.
    //
    // The source is silent for its first three quarters and loud in the last;
    // warped 4x, that loud quarter belongs at the END of the region.  RAW
    // playback would put it at [0.75N, N) -- inside a window that must stay
    // silent -- so any block that swapped rendering shows up there.
    // =====================================================================
    std::printf("\n[10] Warp holds when the warp lock is contended\n");
    {
        AudioClip c = AudioClip::synth_sine(1000.0, 0.5, kSr, 0.8f, "src");
        const int64_t N = c.numFrames();
        for (int64_t i = 0; i < (N * 3) / 4; ++i) { c.ch[0][(size_t)i] = 0.f; c.ch[1][(size_t)i] = 0.f; }
        const int64_t DST = N * 4;

        AudioClipPlayer p; p.prepare(kSr, kBlock); p.setActive(true);
        p.addClip(&c, 0, 1.0f);
        p.setClipRegion(0, 0, 0, DST);
        std::vector<WarpMarker> mk(2);
        mk[0].srcSample = 0; mk[0].dstSample = 0;
        mk[1].srcSample = N; mk[1].dstSample = DST;
        p.setWarp(&c, mk);

        std::atomic<bool> stop{ false };
        std::atomic<long> edits{ 0 };
        // The message thread dragging warp markers, i.e. the editing the
        // realtime warp exists for.  Each setWarp() holds the warp mutex for
        // about a millisecond, so the audio thread genuinely loses try_locks.
        std::thread editor([&] {
            while (!stop.load(std::memory_order_acquire)) {
                p.setWarp(&c, mk);
                edits.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });

        std::vector<float> l((size_t)kBlock), r((size_t)kBlock);
        float* op[2] = { l.data(), r.data() };
        float rawPeak = 0.0f, warpPeak = 0.0f;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < deadline) {
            for (int64_t q = 0; q < DST; q += kBlock) {
                ProcessBlock b{};
                b.nframes = kBlock; b.audioOut = op; b.numAudioOut = 2;
                b.isPlaying = true; b.playPositionSamples = q;
                p.process(b);
                for (int i = 0; i < kBlock; ++i) {
                    const int64_t t = q + i;
                    if (t >= (N * 3) / 4 + 200 && t < N - 200)
                        rawPeak = std::max(rawPeak, std::fabs(l[(size_t)i]));
                    if (t >= N * 3 + 2000 && t < DST - 2000)
                        warpPeak = std::max(warpPeak, std::fabs(l[(size_t)i]));
                }
            }
        }
        stop.store(true, std::memory_order_release);
        editor.join();
        std::printf("    %ld concurrent setWarp() calls; raw-only window %.6f, warped window %.4f\n",
                    edits.load(), rawPeak, warpPeak);
        CHECK(rawPeak < 1e-6f, "a lost warp try_lock never swaps in raw playback");
        CHECK(warpPeak > 0.1f, "warped audio still renders between the edits (test not vacuous)");
    }

    // =====================================================================
    std::printf("\n=== %s (%d failure%s) ===\n",
                gFail == 0 ? "PASS" : "FAIL", gFail, gFail == 1 ? "" : "s");
    return gFail == 0 ? 0 : 1;
}

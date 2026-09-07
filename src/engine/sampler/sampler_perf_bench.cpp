//----------------------------------------------------------------------------
//  src/engine/sampler/sampler_perf_bench.cpp
//
//  Headless PERFORMANCE benchmark for the native sampler with a large
//  multisampled instrument loaded -- the "2976-zone concert grand" shape from
//  the large-soundfont regression report, synthesised so the bench does not
//  depend on any real SoundFont file.
//
//  Instrument shape (mirrors the measured real bank):
//     2976 zones  =  93 key positions x 32 velocity layers
//      192 distinct shared PCM buffers (~15.5 zones per sample)
//
//  Phases timed separately, because they hit different code paths:
//    1. zone load        -- 2976 x sampler_load_sample_shared()
//    2. per-zone setters -- 7 x 2976 sampler_set_zone_* calls (the import tail)
//    3. note-on lookup   -- zoneForMidi() cost in isolation (note that matches
//                           no zone: full scan, no voice started)
//    4. note pattern     -- realistic render with notes starting/stopping
//    5. steady state     -- 32 sustained voices, per-sample render cost
//    6. editor read-back -- sampler_get_zone_meta vs sampler_get_zone sweeps
//    7. save state       -- full project-save serialisation
//
//  Phases 6 and 7 additionally run a POLLER thread that repeatedly takes the
//  instrument mutex (via sampler_zone_count) and records the longest single
//  acquisition. That number is the worst case the AUDIO thread would stall
//  waiting for process()'s lock while the message thread does the same work --
//  the direct "does the UI/save glitch the audio" metric.
//
//  Build: configure src/engine/sampler with -DPATCHKNOB_SAMPLER_TESTS=ON, or
//     g++ -std=c++17 -O2 -pthread -I.. sampler_perf_bench.cpp samplerinstrument.cpp -o bench
//----------------------------------------------------------------------------
#include "sampler_instrument.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace PatchKnob::engine;
using Clock = std::chrono::steady_clock;

static const double kSR    = 48000.0;
static const int    kBlock = 128;

static const int kZones      = 2976;
static const int kKeys       = 93;      // key positions (rootKey 18..110)
static const int kLayers     = 32;      // velocity layers, 4 wide each
static const int kSamples    = 192;     // distinct shared PCM buffers
static const int kFrames     = 48000;   // 1 s per sample (192 x 1 s ~= 36 MB)

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

//----------------------------------------------------------------------------
// Synthesised source material: decaying two-partial tone, looped.
//----------------------------------------------------------------------------
static SharedPcm makeSample(int seed) {
    auto v = std::make_shared<std::vector<float>>((size_t)kFrames);
    const double f = 110.0 * std::pow(2.0, (seed % 48) / 12.0);
    for (int i = 0; i < kFrames; ++i) {
        const double t = (double)i / kSR;
        const double a = std::exp(-t * 1.5);
        (*v)[(size_t)i] = (float)(a * (0.6 * std::sin(6.283185307 * f * t)
                                     + 0.3 * std::sin(6.283185307 * 2.0 * f * t)));
    }
    return SharedPcm(v);
}

struct Bench {
    IPluginInstance* inst = nullptr;
    std::vector<SharedPcm> pcm;

    // -- lock-stall poller ---------------------------------------------------
    std::atomic<bool>   pollRun{false};
    std::atomic<double> pollMaxMs{0.0};
    std::thread         poller;
    void startPoll() {
        pollMaxMs = 0.0; pollRun = true;
        poller = std::thread([this] {
            while (pollRun.load(std::memory_order_relaxed)) {
                const auto t0 = Clock::now();
                (void)sampler_zone_count(inst);      // takes the instrument mutex
                const double d = ms(t0, Clock::now());
                double cur = pollMaxMs.load(std::memory_order_relaxed);
                while (d > cur &&
                       !pollMaxMs.compare_exchange_weak(cur, d)) {}
                std::this_thread::yield();
            }
        });
    }
    double stopPoll() {
        pollRun = false;
        if (poller.joinable()) poller.join();
        return pollMaxMs.load();
    }

    // -- phase 1+2: build the instrument -------------------------------------
    void build() {
        inst = create_sampler_instrument();
        if (!inst) { std::printf("FATAL: no sampler instrument\n"); std::exit(2); }
        inst->prepare(kSR, kBlock);

        pcm.reserve(kSamples);
        auto t0 = Clock::now();
        for (int s = 0; s < kSamples; ++s) pcm.push_back(makeSample(s));
        auto t1 = Clock::now();
        std::printf("synthesize %d samples (%d frames ea)   : %9.2f ms\n",
                    kSamples, kFrames, ms(t0, t1));

        t0 = Clock::now();
        for (int i = 0; i < kZones; ++i) {
            const int key   = i % kKeys;            // 93 key positions
            const int layer = i / kKeys;            // 32 velocity layers
            const int root  = 18 + key;
            const int loVel = layer * 4;
            const int hiVel = layer == kLayers - 1 ? 127 : layer * 4 + 3;
            const int sIdx  = (int)((long long)i * kSamples / kZones);
            char name[32]; std::snprintf(name, sizeof name, "z%04d", i);
            sampler_load_sample_shared(inst, /*slot*/ key + 1, /*level*/ layer,
                                       pcm[(size_t)sIdx], kFrames, false, root,
                                       (int)kSR, 0, kFrames, true,
                                       root, root, loVel, hiVel,
                                       false, true, true, 0, name);
        }
        t1 = Clock::now();
        const double loadMs = ms(t0, t1);
        std::printf("load %d zones (shared pcm)             : %9.2f ms  (%.1f us/zone)\n",
                    kZones, loadMs, loadMs * 1000.0 / kZones);

        // The import tail: the seven per-zone setters, per zone.
        t0 = Clock::now();
        SamplerZoneEnv env; env.enabled = 1; env.attack = 0.002f;
        env.decay = 8.f; env.sustain = 0.f; env.release = 0.35f;
        for (int i = 0; i < kZones; ++i) {
            const int slot = (i % kKeys) + 1, level = i / kKeys;
            sampler_set_zone_env(inst, slot, level, 0, &env);
            sampler_set_zone_env(inst, slot, level, 1, nullptr);
            sampler_set_zone_filter(inst, slot, level, 8000.f, 0.f);
            sampler_set_zone_tuning(inst, slot, level, 0, (i % 21) - 10, 100);
            sampler_set_zone_level(inst, slot, level, 0.f, (float)(i % 8));
            sampler_set_zone_exclusive(inst, slot, level, 0);
            sampler_set_zone_modroute(inst, slot, level, 0.f, 0.f);
        }
        t1 = Clock::now();
        const double setMs = ms(t0, t1);
        std::printf("7 x %d per-zone setters (%d calls)    : %9.2f ms  (%.2f us/call)\n",
                    kZones, 7 * kZones, setMs, setMs * 1000.0 / (7 * kZones));
    }

    // -- render helpers -------------------------------------------------------
    void renderBlock(const MidiEvent* ev, int nev, float* L, float* R) {
        float* outs[2] = { L, R };
        ProcessBlock blk{};
        blk.audioOut = outs; blk.numAudioOut = 2; blk.nframes = kBlock;
        blk.midiIn = ev; blk.numMidiIn = nev;
        blk.tempoBpm = 120.0; blk.isPlaying = true;
        inst->process(blk);
    }

    // -- phase 3: zoneForMidi in isolation ------------------------------------
    // Note 5 is below every zone's key range: the scan runs over all 2976
    // zones, matches nothing, and no voice starts -- so the time measured is
    // (almost) pure lookup.
    void benchLookup() {
        std::vector<MidiEvent> ev((size_t)kBlock);
        for (int i = 0; i < kBlock; ++i) {
            ev[(size_t)i] = MidiEvent{};
            ev[(size_t)i].sampleOffset = i;
            ev[(size_t)i].status = 0x90; ev[(size_t)i].data1 = 5; ev[(size_t)i].data2 = 100;
        }
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        const int blocks = 200;                    // 25,600 note-ons total
        // warmup
        renderBlock(ev.data(), kBlock, L.data(), R.data());
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(ev.data(), kBlock, L.data(), R.data());
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        std::printf("note-on zone lookup (no match, %d zones): %8.3f us/note-on  [%d note-ons in %.1f ms]\n",
                    kZones, total * 1000.0 / (blocks * kBlock), blocks * kBlock, total);
    }

    // -- phase 4: realistic note pattern --------------------------------------
    void benchPattern() {
        const double seconds = 10.0;
        const int totalBlocks = (int)(seconds * kSR / kBlock);
        const int notePeriod = 250;                 // new note every ~5.2 ms
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        std::vector<MidiEvent> ev; ev.reserve(8);
        long long samplePos = 0; int noteIdx = 0, notes = 0;
        auto t0 = Clock::now();
        for (int b = 0; b < totalBlocks; ++b) {
            ev.clear();
            for (int i = 0; i < kBlock; ++i) {
                const long long s = samplePos + i;
                if (s % notePeriod == 0) {
                    MidiEvent m{}; m.sampleOffset = i;
                    m.status = 0x90; m.data1 = (unsigned char)(21 + (noteIdx % 88));
                    m.data2 = (unsigned char)(20 + (noteIdx * 13) % 107);
                    ev.push_back(m); ++notes;
                    MidiEvent off{}; off.sampleOffset = i;
                    off.status = 0x80;
                    off.data1 = (unsigned char)(21 + ((noteIdx + 86) % 88)); // release 2 notes ago
                    ev.push_back(off);
                    ++noteIdx;
                }
            }
            renderBlock(ev.data(), (int)ev.size(), L.data(), R.data());
            samplePos += kBlock;
        }
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        std::printf("note pattern %4.1f s audio, %5d notes   : %9.2f ms  (%.1fx realtime)\n",
                    seconds, notes, total, seconds * 1000.0 / total);
    }

    // -- phase 5: steady-state render, 32 held voices -------------------------
    void benchSteady() {
        // silence + reset
        {   std::vector<MidiEvent> offs;
            for (int n = 0; n < 128; ++n) {
                MidiEvent m{}; m.sampleOffset = 0; m.status = 0x80; m.data1 = (unsigned char)n;
                offs.push_back(m);
            }
            std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
            for (int i = 0; i < 400; ++i)
                renderBlock(i ? nullptr : offs.data(), i ? 0 : (int)offs.size(), L.data(), R.data());
        }
        std::vector<MidiEvent> ons;
        for (int v = 0; v < 32; ++v) {
            MidiEvent m{}; m.sampleOffset = 0; m.status = 0x90;
            m.data1 = (unsigned char)(24 + v * 2); m.data2 = 100;
            ons.push_back(m);
        }
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        renderBlock(ons.data(), (int)ons.size(), L.data(), R.data());
        const double seconds = 5.0;
        const int blocks = (int)(seconds * kSR / kBlock);
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(nullptr, 0, L.data(), R.data());
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        const double nsPerSample = total * 1.0e6 / ((double)blocks * kBlock);
        std::printf("steady state 32 voices, %3.1f s audio    : %9.2f ms  (%.1f ns/sample, %.1fx realtime)\n",
                    seconds, total, nsPerSample, seconds * 1000.0 / total);
    }

    // -- phase 5b: steady state again with a DENSE hand-drawn amp envelope ----
    // The editor approximates its curves with dense linear points; EnvelopeF::
    // eval() walks the point list per sample per voice, so envelope density is
    // a render cost multiplier worth measuring on its own.
    void benchDenseEnvelope() {
        const int nPts = 128;
        std::vector<unsigned short> xs((size_t)nPts), ys((size_t)nPts);
        std::vector<int> flags((size_t)nPts, 0);
        for (int i = 0; i < nPts; ++i) {
            xs[(size_t)i] = (unsigned short)((i * 65535) / (nPts - 1));
            const double t = (double)i / (nPts - 1);
            ys[(size_t)i] = (unsigned short)(65535.0 * (0.3 + 0.7 * std::sin(3.14159 * t)));
        }
        flags[(size_t)(nPts - 8)] = 1;                      // sustain near the end
        // Drain every sounding voice BEFORE touching the envelopes: clearing a
        // zone envelope out from under a held voice leaves it playing the
        // instrument fallback, which is not the configuration being measured.
        {   std::vector<MidiEvent> offs;
            for (int n = 0; n < 128; ++n) {
                MidiEvent m{}; m.sampleOffset = 0; m.status = 0x80; m.data1 = (unsigned char)n;
                offs.push_back(m);
            }
            std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
            for (int i = 0; i < 400; ++i)
                renderBlock(i ? nullptr : offs.data(), i ? 0 : (int)offs.size(), L.data(), R.data());
        }
        // Per-zone envelopes shadow the instrument-global one; clear them so the
        // voices actually evaluate the dense global envelope.
        for (int i = 0; i < kZones; ++i)
            sampler_set_zone_env(inst, (i % kKeys) + 1, i / kKeys, 0, nullptr);
        sampler_set_envelope(inst, 0, xs.data(), ys.data(), flags.data(), nPts);
        benchSteadyInner("steady state 32 voices, dense 128-pt env");
        sampler_set_envelope(inst, 0, nullptr, nullptr, nullptr, 0);   // clear
    }

    void benchSteadyInner(const char* label) {
        {   std::vector<MidiEvent> offs;
            for (int n = 0; n < 128; ++n) {
                MidiEvent m{}; m.sampleOffset = 0; m.status = 0x80; m.data1 = (unsigned char)n;
                offs.push_back(m);
            }
            std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
            for (int i = 0; i < 400; ++i)
                renderBlock(i ? nullptr : offs.data(), i ? 0 : (int)offs.size(), L.data(), R.data());
        }
        std::vector<MidiEvent> ons;
        for (int v = 0; v < 32; ++v) {
            MidiEvent m{}; m.sampleOffset = 0; m.status = 0x90;
            m.data1 = (unsigned char)(24 + v * 2); m.data2 = 100;
            ons.push_back(m);
        }
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        renderBlock(ons.data(), (int)ons.size(), L.data(), R.data());
        const double seconds = 5.0;
        const int blocks = (int)(seconds * kSR / kBlock);
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(nullptr, 0, L.data(), R.data());
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        const double nsPerSample = total * 1.0e6 / ((double)blocks * kBlock);
        std::printf("%s: %9.2f ms  (%.1f ns/sample, %.1fx realtime)\n",
                    label, total, nsPerSample, seconds * 1000.0 / total);
    }


    // -- phase 5c: NOTE-ON STORM at full polyphony with a dense envelope ------
    // Every note-on at full polyphony runs stealVoice(), and stealVoice() runs
    // voiceLevel() -> EnvelopeF::eval() for all 32 voices.  eval() is O(points),
    // so a dense hand-drawn envelope multiplies the cost of every steal.
    void benchStealStorm() {
        const int nPts = 128;
        std::vector<unsigned short> xs((size_t)nPts), ys((size_t)nPts);
        std::vector<int> flags((size_t)nPts, 0);
        for (int i = 0; i < nPts; ++i) {
            xs[(size_t)i] = (unsigned short)((i * 65535) / (nPts - 1));
            ys[(size_t)i] = (unsigned short)(20000 + (i * 331) % 30000);
        }
        flags[(size_t)(nPts - 8)] = 1;
        sampler_set_envelope(inst, 0, xs.data(), ys.data(), flags.data(), nPts);
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        // fill polyphony and let the phases spread out
        std::vector<MidiEvent> ons;
        for (int v = 0; v < 32; ++v) {
            MidiEvent m{}; m.sampleOffset = (v * 4) % kBlock; m.status = 0x90;
            m.data1 = (unsigned char)(24 + (v * 3) % 80); m.data2 = 100;
            ons.push_back(m);
        }
        renderBlock(ons.data(), (int)ons.size(), L.data(), R.data());
        for (int b = 0; b < 40; ++b) renderBlock(nullptr, 0, L.data(), R.data());
        // storm: 64 note-ons per block, every one a steal
        std::vector<MidiEvent> ev((size_t)64);
        for (int i = 0; i < 64; ++i) {
            ev[(size_t)i] = MidiEvent{};
            ev[(size_t)i].sampleOffset = i * 2;
            ev[(size_t)i].status = 0x90;
            ev[(size_t)i].data1 = (unsigned char)(24 + (i * 5) % 80);
            ev[(size_t)i].data2 = 100;
        }
        const int blocks = 400;                     // 25,600 stolen note-ons
        renderBlock(ev.data(), 64, L.data(), R.data());   // warmup
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(ev.data(), 64, L.data(), R.data());
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        // subtract nothing: report the whole block cost per note-on alongside
        // a no-storm render of the same length for context
        auto t2 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(nullptr, 0, L.data(), R.data());
        auto t3 = Clock::now();
        const double base = ms(t2, t3);
        std::printf("steal storm, dense env (%d note-ons)  : %9.2f ms render+steal, %9.2f ms render-only -> %.3f us/steal\n",
                    blocks * 64, total, base, (total - base) * 1000.0 / (blocks * 64));
        sampler_set_envelope(inst, 0, nullptr, nullptr, nullptr, 0);
        // drain
        std::vector<MidiEvent> offs;
        for (int n = 0; n < 128; ++n) {
            MidiEvent m{}; m.sampleOffset = 0; m.status = 0x80; m.data1 = (unsigned char)n;
            offs.push_back(m);
        }
        for (int i = 0; i < 500; ++i)
            renderBlock(i ? nullptr : offs.data(), i ? 0 : (int)offs.size(), L.data(), R.data());
    }

    // -- phase 6: editor read-back sweeps -------------------------------------
    void benchReadback() {
        const int n = sampler_zone_count(inst);
        SamplerZoneInfo zi;
        auto t0 = Clock::now();
        for (int i = 0; i < n; ++i) sampler_get_zone_meta(inst, i, zi);
        auto t1 = Clock::now();
        std::printf("get_zone_meta sweep (%d zones)         : %9.2f ms\n", n, ms(t0, t1));

        startPoll();
        t0 = Clock::now();
        for (int i = 0; i < n; ++i) sampler_get_zone(inst, i, zi);
        t1 = Clock::now();
        const double stall = stopPoll();
        std::printf("get_zone FULL sweep (%d zones, w/ pcm) : %9.2f ms  [max mutex stall %.2f ms]\n",
                    n, ms(t0, t1), stall);
    }

    // -- phase 8: clearing the whole patch (what a re-import does first) ------
    void benchClear() {
        // per-slot clearing, the way the importer used to do it
        auto t0 = Clock::now();
        for (int s = 1; s <= kKeys; ++s) sampler_clear_slot(inst, s);
        auto t1 = Clock::now();
        std::printf("clear per-slot (%d clearSlot calls)      : %9.2f ms\n",
                    kKeys, ms(t0, t1));
        // reload, then clear in one call
        for (int i = 0; i < kZones; ++i) {
            const int key = i % kKeys, layer = i / kKeys, root = 18 + key;
            const int sIdx = (int)((long long)i * kSamples / kZones);
            sampler_load_sample_shared(inst, key + 1, layer, pcm[(size_t)sIdx],
                                       kFrames, false, root, (int)kSR, 0, kFrames, true,
                                       root, root, layer * 4,
                                       layer == kLayers - 1 ? 127 : layer * 4 + 3,
                                       false, true, true, 0, "z");
        }
        t0 = Clock::now();
        sampler_clear_all_zones(inst);
        t1 = Clock::now();
        std::printf("clear ALL zones (one call)               : %9.3f ms\n", ms(t0, t1));

        // B's real-import shape: one SLOT per zone, cleared slot by slot --
        // the O(N^2) path measured at ~458 ms on the actual importer.
        for (int i = 0; i < kZones; ++i)
            sampler_load_sample_shared(inst, i + 1, 0, pcm[(size_t)(i % kSamples)],
                                       kFrames, false, 60, (int)kSR, 0, kFrames, true,
                                       0, 127, 0, 127, false, true, true, 0, "z");
        t0 = Clock::now();
        for (int i = 0; i < kZones; ++i) sampler_clear_slot(inst, i + 1);
        t1 = Clock::now();
        std::printf("clear per-slot, slot-per-zone (%d calls): %9.2f ms\n", kZones, ms(t0, t1));
    }


    // -- phase 9: IDLE instrument -- no zones, no voices ----------------------
    // What every silent sampler track in a project pays per sample: output
    // zeroing, syncGlobalParams, and the always-on applyNativeFx tail.
    void benchIdle() {
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        const int blocks = 40000;                   // ~106 s of audio
        for (int b = 0; b < 100; ++b) renderBlock(nullptr, 0, L.data(), R.data());
        auto t0 = Clock::now();
        for (int b = 0; b < blocks; ++b)
            renderBlock(nullptr, 0, L.data(), R.data());
        auto t1 = Clock::now();
        const double total = ms(t0, t1);
        std::printf("idle instrument (0 voices)               : %9.2f ns/sample  (%.2f%% of 48k budget)\n",
                    total * 1.0e6 / ((double)blocks * kBlock),
                    total * 1.0e6 / ((double)blocks * kBlock) / 20833.3 * 100.0);
    }

    // -- phase 7: saveState ---------------------------------------------------
    void benchSave() {
        startPoll();
        auto t0 = Clock::now();
        const std::vector<uint8_t> blob = inst->saveState();
        auto t1 = Clock::now();
        const double stall = stopPoll();
        std::printf("saveState (%.1f MB blob)                : %9.2f ms  [max mutex stall %.2f ms]\n",
                    (double)blob.size() / (1024.0 * 1024.0), ms(t0, t1), stall);
        // loadState of the same blob (message thread; only the final zone-table
        // swap happens under the lock).
        t0 = Clock::now();
        inst->loadState(blob);
        t1 = Clock::now();
        std::printf("loadState (same blob)                   : %9.2f ms\n", ms(t0, t1));
    }
};

int main() {
    std::printf("== sampler_perf_bench: %d zones / %d samples / %d frames each ==\n",
                kZones, kSamples, kFrames);
    Bench b;
    b.build();
    b.benchLookup();
    b.benchPattern();
    b.benchSteady();
    b.benchDenseEnvelope();
    b.benchStealStorm();
    b.benchReadback();
    b.benchSave();
    b.benchClear();
    b.benchIdle();
    b.inst->release();
    delete b.inst;
    return 0;
}

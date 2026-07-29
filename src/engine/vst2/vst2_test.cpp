//----------------------------------------------------------------------------
//  PatchKnob VST2 host test harness.
//
//  Default run: headless hardening tests against in-process fake AEffects
//  (no plugin DLL needed) — the SEH guard self-test, the channel clamp
//  against a 16-out plugin on a stereo bus, fault containment in
//  processReplacing and the dispatcher, load-time count validation, the
//  unsigned param-index guards, the null-midiIn guard, and the teardown gate.
//
//  With --dll: additionally finds a 64-bit VST2 instrument under
//  C:\Program Files\VstPlugins, loads it through Vst2PluginInstance, prepares
//  at 48000/512, sends a C4 (vel 100) Note-On, processes ~50 blocks, asserts
//  the output is non-silent, and writes vst2_test_out.wav.
//----------------------------------------------------------------------------
#include "vst2_host.h"

#include "aeffectx.h"
#include "seh_guard.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace PatchKnob::engine;

// ---------------------------------------------------------------------------
// Headless hardening tests — in-process fake AEffects, no DLL involved.
// ---------------------------------------------------------------------------

static int g_testFails = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond))                                                           \
        {                                                                      \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_testFails;                                                     \
        }                                                                      \
    } while (0)

// Per-fake-plugin state; stashed in AEffect::user (the host never touches
// that field — it maps AEffect* -> instance through its own registry).
struct FakeState {
    int  processCalls  = 0;
    int  setParamCalls = 0;
    int  processEvents = 0;
    bool faultInProcess = false;    // processReplacing writes through null
    bool faultInEvents  = false;    // effProcessEvents writes through null
    int  numOuts        = 2;
};

static FakeState* stateOf(AEffect* e) { return (FakeState*)e->user; }

static intptr_t VST_CALL_CONV fakeDispatcher(AEffect* e, int32_t opcode,
                                             int32_t, intptr_t, void*, float)
{
    FakeState* st = stateOf(e);
    if (opcode == effProcessEvents)
    {
        ++st->processEvents;
        if (st->faultInEvents)
        {
            volatile int* p = nullptr;
            *p = 1;
        }
    }
    return 0;
}

static void VST_CALL_CONV fakeSetParameter(AEffect* e, int32_t, float)
{
    ++stateOf(e)->setParamCalls;
}

static float VST_CALL_CONV fakeGetParameter(AEffect*, int32_t)
{
    return 0.5f;
}

// Writes a channel-tagged value into EVERY output channel the fake claims.
// If the host handed it a garbage pointer for any channel (i.e. read past the
// caller's audioOut array), the write faults, the guard latches dead_, and
// the clamp test's !isDead() assertion fails.
static void VST_CALL_CONV fakeProcessReplacing(AEffect* e, float**,
                                               float** outs, int n)
{
    FakeState* st = stateOf(e);
    ++st->processCalls;
    if (st->faultInProcess)
    {
        volatile int* p = nullptr;
        *p = 42;
    }
    for (int c = 0; c < st->numOuts; ++c)
        for (int i = 0; i < n; ++i)
            outs[c][i] = 1.0f + (float)c;
}

static void VST_CALL_CONV fakeProcess(AEffect* e, float** ins, float** outs, int32_t n)
{
    fakeProcessReplacing(e, ins, outs, (int)n);
}

static void initFakeEffect(AEffect& e, FakeState& st, int numIn, int numOut,
                           int numParams)
{
    std::memset(&e, 0, sizeof(e));
    e.magic            = kEffectMagic;
    e.dispatcher       = &fakeDispatcher;
    e.setParameter     = &fakeSetParameter;
    e.getParameter     = &fakeGetParameter;
    e.processReplacing = &fakeProcessReplacing;
    e.numInputs        = numIn;
    e.numOutputs       = numOut;
    e.numParams        = numParams;
    e.flags            = effFlagsIsSynth | effFlagsCanReplacing;
    e.user             = &st;
    st.numOuts         = numOut;
}

// (a) A 16-out plugin processed against a stereo bus: the host must never
// read blk.audioOut past numAudioOut (the poison slots stay untouched) and
// must hand the plugin valid scratch for channels 2..15 (no fault => !dead).
static void testChannelClamp()
{
    std::printf("[clamp] 16-out plugin vs numAudioOut=2\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 16, 8);

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 512));

    // Two real stereo buffers, then poison pointer slots: if the host reads
    // audioOut[2..] and passes them on, the plugin's write faults and dead_
    // latches (caught below); if the host itself dereferenced them it would
    // crash the test outright.
    std::vector<float> ch0(512, -9.0f), ch1(512, -9.0f);
    float* outArr[8] = { ch0.data(), ch1.data(),
                         (float*)0x1, (float*)0x1, (float*)0x1,
                         (float*)0x1, (float*)0x1, (float*)0x1 };

    ProcessBlock pb{};
    pb.audioOut    = outArr;
    pb.nframes     = 512;
    pb.numAudioIn  = 0;
    pb.numAudioOut = 2;
    inst.process(pb);

    CHECK(st.processCalls == 1);
    CHECK(!inst.isDead());                    // channels 2..15 got valid scratch
    CHECK(ch0[0] == 1.0f && ch0[511] == 1.0f);// caller channels were written
    CHECK(ch1[0] == 2.0f && ch1[511] == 2.0f);
    CHECK(outArr[2] == (float*)0x1);          // poison slots never dereferenced
    inst.release();
}

static void testMonoOutputUpmix()
{
    std::printf("[output] mono plugin upmixes to stereo\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 1, 4);

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 128));

    std::vector<float> ch0(128, -1.0f), ch1(128, -1.0f);
    float* outArr[2] = { ch0.data(), ch1.data() };
    ProcessBlock pb{};
    pb.audioOut = outArr;
    pb.nframes = 128;
    pb.numAudioOut = 2;
    inst.process(pb);

    CHECK(st.processCalls == 1);
    CHECK(ch0[0] == 1.0f && ch0[127] == 1.0f);
    CHECK(ch1[0] == 1.0f && ch1[127] == 1.0f);
    inst.release();
}

static void testLegacyProcess()
{
    std::printf("[output] legacy VST2 process callback\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 2, 4);
    eff.processReplacing = nullptr;
    eff.process = &fakeProcess;

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 128));

    std::vector<float> ch0(128, -1.0f), ch1(128, -1.0f);
    float* outArr[2] = { ch0.data(), ch1.data() };
    ProcessBlock pb{};
    pb.audioOut = outArr;
    pb.nframes = 128;
    pb.numAudioOut = 2;
    inst.process(pb);

    CHECK(st.processCalls == 1);
    CHECK(ch0[0] == 1.0f && ch1[0] == 2.0f);
    inst.release();
}

// (b) processReplacing writes through null: process() must return, the
// caller's outputs must be zeroed, the instance is dead, and the next
// process() never re-enters the plugin.
static void testProcessFaultContained()
{
    std::printf("[fault] processReplacing null write is contained\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 2, 4);
    st.faultInProcess = true;

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 256));

    std::vector<float> ch0(256, 0.5f), ch1(256, 0.5f);
    float* outArr[2] = { ch0.data(), ch1.data() };
    ProcessBlock pb{};
    pb.audioOut    = outArr;
    pb.nframes     = 256;
    pb.numAudioOut = 2;

    inst.process(pb);                          // faults inside the plugin
    CHECK(inst.isDead());
    CHECK(st.processCalls == 1);
    bool zeroed = true;
    for (int i = 0; i < 256; ++i)
        if (ch0[i] != 0.0f || ch1[i] != 0.0f) zeroed = false;
    CHECK(zeroed);                             // outputs handed back silent

    // Later blocks: no-op that still silences the caller's buffers.
    std::fill(ch0.begin(), ch0.end(), 0.7f);
    std::fill(ch1.begin(), ch1.end(), 0.7f);
    inst.process(pb);
    CHECK(st.processCalls == 1);               // plugin never re-entered
    zeroed = true;
    for (int i = 0; i < 256; ++i)
        if (ch0[i] != 0.0f || ch1[i] != 0.0f) zeroed = false;
    CHECK(zeroed);
    inst.release();
}

// A fault inside the dispatcher (effProcessEvents, the MIDI path) must also
// latch dead_ and keep processReplacing from being called on the crashed
// plugin; the caller still gets silence.
static void testDispatcherFaultContained()
{
    std::printf("[fault] effProcessEvents null write is contained\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 2, 4);
    st.faultInEvents = true;

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 256));

    std::vector<float> ch0(256, 0.5f), ch1(256, 0.5f);
    float* outArr[2] = { ch0.data(), ch1.data() };
    MidiEvent ev; ev.sampleOffset = 0; ev.status = 0x90; ev.data1 = 60; ev.data2 = 100;
    ProcessBlock pb{};
    pb.audioOut    = outArr;
    pb.nframes     = 256;
    pb.numAudioOut = 2;
    pb.midiIn      = &ev;
    pb.numMidiIn   = 1;

    inst.process(pb);
    CHECK(inst.isDead());
    CHECK(st.processEvents == 1);
    CHECK(st.processCalls == 0);               // never reached processReplacing
    bool zeroed = true;
    for (int i = 0; i < 256; ++i)
        if (ch0[i] != 0.0f || ch1[i] != 0.0f) zeroed = false;
    CHECK(zeroed);

    inst.process(pb);                          // dead: dispatcher not re-entered
    CHECK(st.processEvents == 1);
    inst.release();
}

// Load-time validation: insane self-reported counts are refused before they
// can size any buffer.
static void testLoadValidation()
{
    std::printf("[load] insane AEffect counts are refused\n");
    AEffect eff; FakeState st;

    initFakeEffect(eff, st, -1, 2, 4);         // negative inputs
    { Vst2PluginInstance i; CHECK(!i.adoptEffectForTest(&eff)); }

    initFakeEffect(eff, st, 0, 100000, 4);     // absurd outputs
    { Vst2PluginInstance i; CHECK(!i.adoptEffectForTest(&eff)); }

    initFakeEffect(eff, st, 0, 2, 200000);     // absurd param count
    { Vst2PluginInstance i; CHECK(!i.adoptEffectForTest(&eff)); }

    initFakeEffect(eff, st, 2, 2, 16);         // sane counts still accepted
    { Vst2PluginInstance i; CHECK(i.adoptEffectForTest(&eff)); i.release(); }
}

// Index/pointer guards: huge uint32 param ids must not cast negative past the
// bound check, and numMidiIn>0 with a null midiIn must be ignored.
static void testIndexAndNullGuards()
{
    std::printf("[guards] unsigned param ids + null midiIn\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 2, 4);

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 256));

    inst.setParamNormalized(0xFFFFFFFFu, 0.5f);   // would index -1 if signed
    CHECK(st.setParamCalls == 0);
    CHECK(inst.getParamNormalized(0xFFFFFFFFu) == 0.0f);
    inst.setParamNormalized(3, 0.5f);             // in range: goes through
    CHECK(st.setParamCalls == 1);

    std::vector<float> ch0(256, 0.0f), ch1(256, 0.0f);
    float* outArr[2] = { ch0.data(), ch1.data() };
    ParamChange pc; pc.id = 0xFFFFFFFFu; pc.sampleOffset = 0; pc.value = 1.0f;
    ProcessBlock pb{};
    pb.audioOut    = outArr;
    pb.nframes     = 256;
    pb.numAudioOut = 2;
    pb.paramIn     = &pc;
    pb.numParamIn  = 1;
    pb.midiIn      = nullptr;
    pb.numMidiIn   = 3;                           // lies: no events supplied
    inst.process(pb);

    CHECK(st.setParamCalls == 1);                 // huge id was rejected
    CHECK(st.processEvents == 0);                 // null midiIn never dispatched
    CHECK(st.processCalls == 1);                  // block still processed
    CHECK(!inst.isDead());
    inst.release();
}

// Teardown gate: release() while an audio thread hammers process() must not
// crash or free buffers under the in-flight block; post-release process()
// calls are no-ops.
static void testTeardownGate()
{
    std::printf("[gate] release() vs concurrent process()\n");
    AEffect eff; FakeState st;
    initFakeEffect(eff, st, 0, 2, 4);

    Vst2PluginInstance inst;
    CHECK(inst.adoptEffectForTest(&eff));
    CHECK(inst.prepare(48000.0, 256));

    std::atomic<bool> stop{false};
    std::thread audio([&]() {
        std::vector<float> ch0(256, 0.0f), ch1(256, 0.0f);
        float* outArr[2] = { ch0.data(), ch1.data() };
        ProcessBlock pb{};
        pb.audioOut    = outArr;
        pb.nframes     = 256;
        pb.numAudioOut = 2;
        while (!stop.load(std::memory_order_relaxed))
            inst.process(pb);
    });

    Sleep(50);                     // let the "audio thread" run hot
    inst.release();                // must quiesce, then tear down safely
    Sleep(10);                     // post-release process() calls: no-ops
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    CHECK(st.processCalls > 0);
}

static int runHardeningTests()
{
    g_testFails = 0;

    std::printf("[seh] guard self-test\n");
    CHECK(seh_guard_self_test());

    testChannelClamp();
    testMonoOutputUpmix();
    testLegacyProcess();
    testProcessFaultContained();
    testDispatcherFaultContained();
    testLoadValidation();
    testIndexAndNullGuards();
    testTeardownGate();

    std::printf("\nhardening tests: %s (%d failure%s)\n",
                g_testFails == 0 ? "ALL PASS" : "FAILED",
                g_testFails, g_testFails == 1 ? "" : "s");
    return g_testFails;
}

// --- minimal 16-bit PCM WAV writer -----------------------------------------
static void writeWav(const std::string& path, const std::vector<float>& interleaved,
                     int channels, int sampleRate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("WAV: could not open %s\n", path.c_str()); return; }

    uint32_t numSamples = (uint32_t)interleaved.size();        // total samples
    uint32_t dataBytes  = numSamples * 2;                       // 16-bit
    uint32_t byteRate   = (uint32_t)(sampleRate * channels * 2);
    uint16_t blockAlign = (uint16_t)(channels * 2);
    uint32_t riffSize   = 36 + dataBytes;

    auto w32 = [&](uint32_t v){ std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); w32(riffSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)channels);
    w32((uint32_t)sampleRate); w32(byteRate); w16(blockAlign); w16(16);
    std::fwrite("data", 1, 4, f); w32(dataBytes);

    for (uint32_t i = 0; i < numSamples; ++i)
    {
        float s = interleaved[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)std::lround(s * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// --- objdump-free 64-bit check via PE header --------------------------------
static bool isPe64(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t dos[64];
    if (std::fread(dos, 1, 64, f) == 64 && dos[0] == 'M' && dos[1] == 'Z')
    {
        uint32_t peOff = *(uint32_t*)(dos + 0x3C);
        if (std::fseek(f, (long)peOff, SEEK_SET) == 0)
        {
            uint8_t sig[6];
            if (std::fread(sig, 1, 6, f) == 6 &&
                sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0)
            {
                uint16_t machine = (uint16_t)(sig[4] | (sig[5] << 8));
                ok = (machine == 0x8664); // IMAGE_FILE_MACHINE_AMD64
            }
        }
    }
    std::fclose(f);
    return ok;
}

// --- recursively collect candidate .dll paths ------------------------------
static void collectDlls(const std::string& dir, std::vector<std::string>& out,
                        int depth)
{
    if (depth < 0) return;
    WIN32_FIND_DATAA fd;
    std::string pat = dir + "\\*";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            collectDlls(full, out, depth - 1);
        else
        {
            size_t dot = name.rfind('.');
            if (dot != std::string::npos)
            {
                std::string ext = name.substr(dot);
                for (auto& c : ext) c = (char)tolower(c);
                if (ext == ".dll") out.push_back(full);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Render a short probe (Note-On C4) and return the peak, to find an instrument
// that actually produces audio with its default preset. (Some synths, e.g.
// sample-based drum machines, ship a silent/empty default program.)
static float probePeak(Vst2PluginInstance* inst, int numOut, int sr, int block)
{
    (void)sr;
    std::vector<std::vector<float>> bufs((size_t)numOut,
                                         std::vector<float>((size_t)block, 0.0f));
    std::vector<float*> ptrs((size_t)numOut);
    float peak = 0.0f; int64_t pos = 0;
    for (int b = 0; b < 40; ++b)
    {
        for (int c = 0; c < numOut; ++c)
        {
            std::fill(bufs[(size_t)c].begin(), bufs[(size_t)c].end(), 0.0f);
            ptrs[(size_t)c] = bufs[(size_t)c].data();
        }
        MidiEvent ev; ProcessBlock pb{};
        pb.audioOut = ptrs.data(); pb.nframes = block;
        pb.numAudioIn = 0; pb.numAudioOut = numOut;  // we supply ALL channels
        pb.tempoBpm = 120.0; pb.isPlaying = true; pb.playPositionSamples = pos;
        if (b == 0)
        {
            ev.sampleOffset = 0; ev.status = 0x90; ev.data1 = 60; ev.data2 = 110;
            pb.midiIn = &ev; pb.numMidiIn = 1;
        }
        inst->process(pb);
        for (int c = 0; c < numOut; ++c)
            for (int i = 0; i < block; ++i)
            {
                float a = std::fabs(bufs[(size_t)c][(size_t)i]);
                if (a > peak) peak = a;
            }
        pos += block;
    }
    return peak;
}

// ---------------------------------------------------------------------------
// Real-DLL smoke test (opt-in via --dll): load an installed instrument, play
// a note, assert non-silent output.
// ---------------------------------------------------------------------------
static int runDllTest()
{
    const char* root = "C:\\Program Files\\VstPlugins";
    const int sr = 48000;
    const int block = 512;

    // Preferred candidates first (known-audible 64-bit synths), then a scan.
    // 808 Machine is listed but ships a silent default kit, so it is only used
    // as a last resort — the probe below skips silent instruments.
    std::vector<std::string> candidates = {
        std::string(root) + "\\Arturia\\Jup-8000 V.dll",
        std::string(root) + "\\GForce\\impOSCar3.dll",
        std::string(root) + "\\TubeSynth_x64.dll",
        std::string(root) + "\\808 Machine x64.dll",
    };
    std::vector<std::string> scanned;
    collectDlls(root, scanned, 2);
    for (auto& s : scanned) candidates.push_back(s);

    Vst2PluginInstance* inst = nullptr;
    PluginDescriptor desc;
    std::string chosen;
    // Fallback: first loadable instrument even if its default preset is silent.
    Vst2PluginInstance* fallback = nullptr;
    PluginDescriptor fbDesc; std::string fbPath;

    for (const auto& path : candidates)
    {
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (!isPe64(path))
        {
            std::printf("skip (not 64-bit): %s\n", path.c_str());
            continue;
        }
        PluginDescriptor d;
        d.format = PluginFormat::VST2;
        d.path   = path;
        Vst2PluginInstance* candidate = new Vst2PluginInstance();
        if (!candidate->load(d))
        {
            std::printf("load failed: %s\n", path.c_str());
            delete candidate;
            continue;
        }
        const PluginDescriptor& got = candidate->descriptor();
        std::printf("loaded: %-24s (synth=%s, in=%d, out=%d, params=%d)\n",
                    got.name.c_str(), got.isInstrument ? "yes" : "no",
                    got.numAudioIn, got.numAudioOut, candidate->paramCount());

        if (!got.isInstrument || got.numAudioOut <= 0)
        {
            std::printf("  -> not a usable instrument, trying next\n");
            candidate->release(); delete candidate;
            continue;
        }

        // Probe for audible output with the default preset.
        if (!candidate->prepare(sr, block))
        {
            std::printf("  -> prepare() failed, trying next\n");
            candidate->release(); delete candidate;
            continue;
        }
        float pk = probePeak(candidate, got.numAudioOut, sr, block);
        std::printf("  -> probe peak %.6f\n", pk);
        if (pk > 0.0f)
        {
            inst = candidate; desc = got; chosen = path;
            break; // already prepared; reuse for the full render
        }
        // Keep the first usable-but-silent one as a fallback, discard the rest.
        if (!fallback)
        {
            fallback = candidate; fbDesc = got; fbPath = path;
        }
        else
        {
            candidate->release(); delete candidate;
        }
    }

    if (!inst && fallback)
    {
        std::printf("\nNOTE: no instrument was audible with its default preset; "
                    "using first loadable instrument (output may be silent).\n");
        inst = fallback; desc = fbDesc; chosen = fbPath;
    }
    else if (fallback && fallback != inst)
    {
        fallback->release(); delete fallback;
    }

    if (!inst)
    {
        std::printf("FAIL: no usable 64-bit VST2 instrument found under %s\n", root);
        return 1;
    }

    std::printf("\n== testing: %s ==\n", chosen.c_str());
    std::printf("name      : %s\n", desc.name.c_str());
    std::printf("vendor    : %s\n", desc.vendor.c_str());
    std::printf("isSynth   : %s\n", desc.isInstrument ? "yes" : "no");
    std::printf("audio in  : %d\n", desc.numAudioIn);
    std::printf("audio out : %d\n", desc.numAudioOut);
    std::printf("params    : %d\n", inst->paramCount());

    if (!inst->prepare(sr, block))
    {
        std::printf("FAIL: prepare() returned false\n");
        inst->release(); delete inst; return 1;
    }

    const int numOut = desc.numAudioOut;

    // Per-channel non-interleaved output buffers.
    std::vector<std::vector<float>> outBufs((size_t)numOut,
                                            std::vector<float>((size_t)block, 0.0f));
    std::vector<float*> outPtrs((size_t)numOut);

    // Interleaved capture for the WAV (cap channels at 2 for the file).
    int wavCh = numOut >= 2 ? 2 : 1;
    std::vector<float> wavData;

    const int numBlocks = 50;
    float peak = 0.0f;
    int64_t playPos = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int c = 0; c < numOut; ++c)
        {
            std::fill(outBufs[(size_t)c].begin(), outBufs[(size_t)c].end(), 0.0f);
            outPtrs[(size_t)c] = outBufs[(size_t)c].data();
        }

        // Note-On C4 (MIDI 60) vel 100 at the very first block, sample 0.
        MidiEvent ev;
        ProcessBlock pb{};
        pb.audioIn   = nullptr;
        pb.audioOut  = outPtrs.data();
        pb.nframes   = block;
        pb.numAudioIn  = 0;
        pb.numAudioOut = numOut;    // this harness supplies ALL plugin channels
        pb.tempoBpm  = 120.0;
        pb.playPositionSamples = playPos;
        pb.isPlaying = true;

        if (b == 0)
        {
            ev.sampleOffset = 0;
            ev.status = 0x90;   // Note On, channel 0
            ev.data1  = 60;     // C4
            ev.data2  = 100;    // velocity
            pb.midiIn = &ev;
            pb.numMidiIn = 1;
        }
        else
        {
            pb.midiIn = nullptr;
            pb.numMidiIn = 0;
        }

        inst->process(pb);

        // Peak + WAV capture.
        for (int i = 0; i < block; ++i)
        {
            for (int c = 0; c < wavCh; ++c)
            {
                float s = (c < numOut) ? outBufs[(size_t)c][(size_t)i] : 0.0f;
                wavData.push_back(s);
            }
            for (int c = 0; c < numOut; ++c)
            {
                float a = std::fabs(outBufs[(size_t)c][(size_t)i]);
                if (a > peak) peak = a;
            }
        }
        playPos += block;
    }

    std::printf("\nprocessed %d blocks of %d frames @ %d Hz\n",
                numBlocks, block, sr);
    std::printf("peak level: %.6f\n", peak);

    writeWav("vst2_test_out.wav", wavData, wavCh, sr);
    std::printf("wrote vst2_test_out.wav (%d ch, %zu samples)\n",
                wavCh, wavData.size());

    bool nonSilent = peak > 0.0f;
    std::printf("\nRESULT: output is %s (peak %s 0)\n",
                nonSilent ? "NON-SILENT" : "SILENT",
                nonSilent ? ">" : "==");

    inst->setActive(false);
    inst->release();
    delete inst;

    return nonSilent ? 0 : 2;
}

int main(int argc, char** argv)
{
    int fails = runHardeningTests();

    // The DLL smoke test needs real plugins installed, so it is opt-in.
    if (argc > 1 && std::strcmp(argv[1], "--dll") == 0)
    {
        std::printf("\n== real-DLL smoke test ==\n");
        int rc = runDllTest();
        if (rc != 0)
        {
            std::printf("dll smoke test: FAILED (rc=%d)\n", rc);
            ++fails;
        }
    }

    return fails == 0 ? 0 : 1;
}

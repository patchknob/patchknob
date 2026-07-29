//----------------------------------------------------------------------------
//  buzz_test.cpp -- headless self-test for the Buzz host backend.
//  Verifies the SDK/MDK/dsplib/host all compile+link and the wavetable +
//  multisample keyrange lookup + oscillator tables work.
//----------------------------------------------------------------------------
#include "buzz_host.h"
#include "buzz_machine_host.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::buzz;

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: survive a crash-at-exit
    int fails = 0;
    BuzzHost host;

    // 1) empty wavetable
    if (host.GetWaveLevel(1, 0) != nullptr) { std::printf("FAIL: wave 1 not empty\n"); ++fails; }

    // 2) load two keyrange levels into wave slot 1 (a low + a high sample)
    std::vector<short> lo(1000), hi(1000);
    for (int i = 0; i < 1000; ++i) { lo[i] = (short)(3000 * std::sin(i * 0.05)); hi[i] = (short)(3000 * std::sin(i * 0.2)); }
    host.setWaveLevel(1, 0, lo.data(), 1000, false, 48, 44100, 0, 1000, false, 0,  59, "lo");   // C3, keys 0..59
    host.setWaveLevel(1, 1, hi.data(), 1000, false, 72, 44100, 0, 1000, false, 60, 127, "hi");  // C5, keys 60..127

    const CWaveInfo* wi = host.GetWave(1);
    if (!wi) { std::printf("FAIL: GetWave(1) null\n"); ++fails; }

    // 3) keyrange selection: a low note -> the lo level (root 48); a high note -> hi (root 72)
    const CWaveLevel* a = host.GetNearestWaveLevel(1, 40);
    const CWaveLevel* b = host.GetNearestWaveLevel(1, 90);
    if (!a || a->RootNote != 48) { std::printf("FAIL: low note picked wrong level\n"); ++fails; }
    if (!b || b->RootNote != 72) { std::printf("FAIL: high note picked wrong level\n"); ++fails; }
    std::printf("keyrange: note40 -> root %d, note90 -> root %d\n", a ? a->RootNote : -1, b ? b->RootNote : -1);

    // 4) oscillator table callback returns non-null band-limited data
    const short* osc = host.GetOscillatorTable(OWF_SINE);
    if (!osc) { std::printf("FAIL: GetOscillatorTable null\n"); ++fails; }

    // 5) compiled-in machines self-register (Unwieldy) and can be instantiated
    register_builtin_machines();
    std::printf("registered machines: %d\n", (int)registered_machines().size());
    for (const auto& m : registered_machines()) std::printf("  - %s\n", m.name.c_str());
    if (registered_machines().empty()) { std::printf("FAIL: no machine registered\n"); ++fails; }
    else {
        CMachineInfo const* info = nullptr;
        CMachineInterface* mac = create_machine(registered_machines()[0].name, &info);
        if (!mac || !info) { std::printf("FAIL: could not instantiate machine\n"); ++fails; }
        else {
            std::printf("instantiated '%s' by %s: type %d, %d global + %d track params, tracks %d..%d\n",
                        info->Name, info->Author ? info->Author : "?", info->Type,
                        info->numGlobalParameters, info->numTrackParameters, info->minTracks, info->maxTracks);
            delete mac;
        }
    }

    // 6) DRIVE THE MACHINE: load a sample, play a note, pull audio.
    {
        const int sr = 44100;
        BuzzMachineHost mh;
        if (!mh.create("Fuzzpilz UnwieldyTracker", sr, 1)) {
            std::printf("FAIL: could not create machine host\n"); ++fails;
        } else {
            const int ng = mh.numGlobalParams();
            const int noteIdx = mh.findTrackParam("Note");   // 5
            const int smpIdx  = mh.findTrackParam("Sample");  // 6
            const int volIdx  = mh.findTrackParam("Volume");  // 7
            std::printf("param idx: Note=%d Sample=%d Volume=%d (global=%d, track=%d)\n",
                        noteIdx, smpIdx, volIdx, ng, mh.numTrackParams());

            // a 1-second 220 Hz sine at buzz root note 0x41 (C-4), loaded into slot 1.
            const int rootNote = 0x41;
            std::vector<short> smp(sr);
            for (int i = 0; i < sr; ++i) smp[i] = (short)(9000 * std::sin(2*3.14159265*220.0*i/sr));
            mh.host().setWaveLevel(1, 0, smp.data(), sr, false, rootNote, sr, 0, sr, false, 0, 127, "sine");

            const int volMax = mh.info()->Parameters[ng + (volIdx < 0 ? 7 : volIdx)]->MaxValue;
            if (smpIdx >= 0) mh.setTrackParam(0, smpIdx, 1);          // wavetable slot 1
            if (volIdx >= 0) mh.setTrackParam(0, volIdx, volMax);     // full volume
            if (noteIdx >= 0) mh.setTrackParam(0, noteIdx, rootNote); // trigger the note
            mh.tick();

            std::vector<float> buf(4096 * 2);
            double pk = 0; bool anyWork = false;
            for (int b = 0; b < 12; ++b) {
                bool r = mh.work(buf.data(), 4096);
                anyWork = anyWork || r;
                for (float v : buf) { double a = std::fabs(v); if (a > pk) pk = a; }
            }
            std::printf("machine audio: peak %.4f (work returned %s)\n", pk, anyWork ? "true" : "false");
            if (pk < 1e-4) { std::printf("FAIL: machine produced no audio\n"); ++fails; }
        }
    }

    std::printf(fails ? "\nBUZZ HOST SELFTEST: %d FAILURES\n" : "\nBUZZ HOST SELFTEST: OK\n", fails);
    return fails ? 1 : 0;
}

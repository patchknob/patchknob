// spike_midi.cpp
//
// Minimal standalone RtMidi spike for the seq24 Windows (WinMM) port.
//
// What it does:
//   1. Enumerates all available MIDI OUTPUT ports by name.
//   2. Opens the first one (or reports that none exist and exits cleanly).
//   3. Sends a middle-C (note 60) Note-On, waits ~500ms, then a Note-Off.
//
// It is intentionally self-contained: it must build and run even with zero
// MIDI ports present on the system.
//
// Build (MINGW64):
//   g++ -std=c++11 -D__WINDOWS_MM__ spike_midi.cpp RtMidi.cpp -o spike_midi.exe -lwinmm
//
// Run:
//   ./spike_midi.exe

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdlib>

#include "RtMidi.h"

int main()
{
    RtMidiOut *midiout = 0;

    // RtMidi constructors throw RtMidiError on failure; never let it escape.
    try {
        midiout = new RtMidiOut(RtMidi::WINDOWS_MM, "seq24 spike");
    }
    catch (RtMidiError &error) {
        std::cout << "Failed to create RtMidiOut: ";
        error.printMessage();
        return 0; // still a clean exit for the spike's purposes
    }

    // (a) Enumerate output ports.
    unsigned int nPorts = 0;
    try {
        nPorts = midiout->getPortCount();
    }
    catch (RtMidiError &error) {
        error.printMessage();
        delete midiout;
        return 0;
    }

    std::cout << "MIDI output ports found: " << nPorts << std::endl;
    for (unsigned int i = 0; i < nPorts; ++i) {
        std::string name;
        try { name = midiout->getPortName(i); }
        catch (RtMidiError &error) { name = "<error reading name>"; }
        std::cout << "  [" << i << "] " << name << std::endl;
    }

    if (nPorts == 0) {
        std::cout << "No MIDI output ports available. Nothing to send. "
                     "(Spike exits cleanly.)" << std::endl;
        delete midiout;
        return 0;
    }

    // (b) Open the first port.
    try {
        midiout->openPort(0, "seq24 spike out");
        std::cout << "Opened port [0]: " << midiout->getPortName(0) << std::endl;
    }
    catch (RtMidiError &error) {
        std::cout << "Failed to open port 0: ";
        error.printMessage();
        delete midiout;
        return 0;
    }

    // (c) Send a raw MIDI message: middle-C Note-On, then Note-Off ~500ms later.
    std::vector<unsigned char> msg(3);

    // Note-On, channel 0, note 60 (middle C), velocity 100.
    msg[0] = 0x90;
    msg[1] = 60;
    msg[2] = 100;
    midiout->sendMessage(&msg);
    std::cout << "Sent Note-On  (0x90 60 100)" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Note-Off, channel 0, note 60, velocity 0.
    msg[0] = 0x80;
    msg[1] = 60;
    msg[2] = 0;
    midiout->sendMessage(&msg);
    std::cout << "Sent Note-Off (0x80 60 0)" << std::endl;

    delete midiout;
    std::cout << "Done." << std::endl;
    return 0;
}

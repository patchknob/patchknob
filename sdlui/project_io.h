//----------------------------------------------------------------------------
//  sdlui/project_io.h  --  save / load the whole DAW project to one file.
//
//  A single self-contained, chunked binary container that persists everything
//  the SDL DAW shell needs to reopen a session:
//
//    * per active sequence : name, midi bus + channel, length, beats/bw,
//                            playing (mute) state, ALL note + CC + other events,
//                            and the scale-follow flags (is_scale_master /
//                            follows_master / master key + scale).
//    * per track           : mixer gain / pan / mute / solo  AND the assigned
//                            VST instrument (format + path + uid + the plugin's
//                            own saveState() blob), read from the live
//                            MixerGraph via audio_app_graph().
//    * globals             : perform tempo (BPM) and the UI theme mode.
//
//  Track / instrument state is only written when the audio engine is running
//  (audio_app_running()); otherwise those sections are simply omitted and load
//  restores everything that does not need the audio graph.  This lets the round
//  trip be exercised head-less (no audio device) while still persisting the full
//  engine state in the real app.
//
//  Wire the two free functions straight into File > Save / File > Open.
//
//  ---- file format (all integers little-endian) ----------------------------
//    char  magic[8]   = "S24DAWPJ"
//    u32   version    = 1
//    then a sequence of sections until "END " / EOF, each:
//        char tag[4]
//        u32  payloadLen
//        u8   payload[payloadLen]
//    sections:
//        "GLOB"  u8 themeMode(0=Light,1=Midnight)  i32 bpm
//        "SEQ "  (repeatable, one per active sequence)
//                u32 slot
//                str name                       (u32 len + bytes)
//                i8  midiBus   u8 midiChannel
//                i32 length    i32 beatsPerMeasure  i32 beatWidth
//                u8  playing
//                u8  isScaleMaster  u8 followsMaster
//                i32 masterScale    i32 masterKey
//                u32 numEvents
//                  numEvents * { i32 tick, u8 status, u8 d0, u8 d1 }
//        "TRAK"  (repeatable, one per non-default / instrumented track)
//                u32 track
//                f32 gain  f32 pan  u8 mute  u8 solo
//                u8  hasInstrument
//                  if hasInstrument:
//                    u8  format(0=VST2,1=VST3)
//                    str path   str uid
//                    blob state (u32 len + bytes)
//        "END "  u32 0
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_PROJECT_IO_H
#define SEQ24_SDLUI_PROJECT_IO_H

#include <string>

class perform;

// Serialize the full project rooted at `p` to `path`.  Returns false on any I/O
// or engine error (see project_io_last_error()).  Message thread only.
bool save_project(perform& p, const std::string& path);

// Replace the contents of `p` with the project stored in `path`.  Existing
// sequences in `p` are cleared first.  Returns false on parse / I/O error.
// Message thread only.
bool load_project(perform& p, const std::string& path);

// Human-readable description of the last save/load failure ("" if none).
const char* project_io_last_error();

#endif // SEQ24_SDLUI_PROJECT_IO_H

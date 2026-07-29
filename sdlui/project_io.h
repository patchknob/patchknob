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
//    u32   version    = 3
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
//                u32 numTriggers
//                  numTriggers * { i32 start, i32 length, i32 offset }
//        "TRAK"  (repeatable, one per non-default / instrumented track)
//                u32 track
//                f32 gain  f32 pan  u8 mute  u8 solo
//                instrument descriptor/state and all insert FX descriptors/states
//        "PTCH"  modular graph: built-in, VST, Pd, and Rack nodes, their
//                parameters, graph wiring, mixer routing, and Rack modules/cables
//        "AUDI"  embedded raw stereo clip data and scheduled clip playback
//        "PUI "  patcher node coordinates, keyed by patch-graph node id
//        "END "  u32 0
//
//  Versions 1 and 2 files remain readable.  Pd source is embedded in the project and
//  extracted to a sidecar .pd file when opened so libpd can instantiate it.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PROJECT_IO_H
#define PATCHKNOB_SDLUI_PROJECT_IO_H

#include <cstdint>
#include <string>
#include <vector>

class perform;
namespace PatchKnob { namespace engine { class AutomationPlayer; } }

struct ProjectPatchNodePosition {
    uint32_t nodeId = 0;
    double x = 0.0;
    double y = 0.0;
};

// A freeze relationship (v4 "FRZ" section): enough shell bookkeeping to re-show a
// frozen lane's tint + let it be unfrozen after a reload.  The rendered AUDIO
// itself rides in the normal "AUDI" section (so it always loads + plays even if
// this metadata is dropped).
struct ProjectFreezeRecord {
    int srcSeq     = -1;  // the source (MIDI) sequence, kept song-muted
    int isTrack    = 0;   // track freeze (own audio lane) vs clip freeze (overlay)
    int newSeq     = -1;  // the frozen audio lane (track freeze)
    int srcTrack   = -1;  // engine track disabled during the freeze
    int wasMuted   = 0;   // source's prior song-mute (restored on unfreeze)
    int audioTrack = -1;  // mixer track the frozen audio plays on (display lane)
};

// Serialize the full project rooted at `p` to `path`.  Returns false on any I/O
// or engine error (see project_io_last_error()).  Message thread only.
bool save_project(perform& p, const std::string& path,
                  const std::vector<ProjectPatchNodePosition>& patchLayout = {},
                  const std::vector<ProjectFreezeRecord>& freezes = {},
                  const PatchKnob::engine::AutomationPlayer* autoPlayer = nullptr);

// Replace the contents of `p` with the project stored in `path`.  Existing
// sequences in `p` are cleared first.  Returns false on parse / I/O error.
// Message thread only.
bool load_project(perform& p, const std::string& path,
                  PatchKnob::engine::AutomationPlayer* autoPlayer = nullptr);

// Patch-canvas coordinates restored by the most recent successful load.
const std::vector<ProjectPatchNodePosition>& project_io_loaded_patch_layout();

// Freeze relationships restored by the most recent successful load (empty for
// pre-v4 projects).  The shell re-applies these after it rebuilds g_seqToTrack.
const std::vector<ProjectFreezeRecord>& project_io_loaded_freezes();

// Human-readable description of the last save/load failure ("" if none).
const char* project_io_last_error();

#endif // PATCHKNOB_SDLUI_PROJECT_IO_H

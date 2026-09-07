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
//  The engine-owned sections ("TRAK", "PTCH", "AUDI") can only be GENERATED
//  from a running audio engine (audio_app_running()), and the shell keeps
//  running with no engine at all when the device is busy.  A save in that state
//  therefore carries whatever those sections held in the copy already on disk
//  through byte for byte, instead of dropping them: omitting them silently
//  emptied a project of its patch, rack, embedded audio and plugin state while
//  still reporting success.  Their layout is gated on the container version, so
//  if the file being overwritten is an OLDER format the save is refused rather
//  than completed without them.  A head-less round trip of a project that never
//  had those sections is unaffected.
//
//  A load NEVER destroys the open session on a bad file: the whole container is
//  parsed and validated first, and the live perform / audio graph is only
//  replaced once the file has been proven readable end to end.
//
//  Wire the two free functions straight into File > Save / File > Open.
//
//  ---- file format (all integers little-endian) ----------------------------
//    char  magic[8]   = "S24DAWPJ"
//    u32   version    (current: 18; v8 and up are readable)
//    then a sequence of sections until "END " / EOF, each:
//        char tag[4]
//        u32  payloadLen
//        u8   payload[payloadLen]
//    sections:
//        "GLOB"  u8 themeMode, f64 bpm, loop/range, i32 virtualMidiIns/Outs (v7)
//        "SEQ "  (repeatable, one per active sequence)
//                u32 slot
//                str name                       (u32 len + bytes)
//                i8  midiBus   u8 midiChannel
//                i32 length    i32 beatsPerMeasure  i32 beatWidth
//                u8  playing
//                u8  isScaleMaster  u8 followsMaster
//                i32 masterScale    i32 masterKey
//                u8  songMute (v4)  str trackerFxBlob (v4)  u8 midiThru (v5)
//                u8  trackKind (v11)  i32 arrangeLaneId (v11)
//                i32 loopStart (v14)  i32 loopEnd (v15)  i32 loopEnabled (v16)
//                  The per-clip loop window [loopStart, loopEnd) is INDEPENDENT
//                  of `length` (the END marker) and may legally extend past it;
//                  loopEnabled == 0 is a ONE-SHOT clip.  Absent fields default
//                  to the historical behaviour: window == the whole pattern,
//                  loop enabled.
//                u32 numEvents
//                  numEvents * { i32 tick, u8 status, u8 d0, u8 d1,
//                                u8 trackerColumn (v9, 0xFF == untagged) }
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
//  Every section is parsed through a reader scoped to its own declared payload
//  length, so a corrupt count inside one section fails that file instead of
//  wandering into the next section and returning plausible garbage.
//
//  v8 and every later version remain readable; v1-v7 are rejected outright (the
//  accepted set is the closed interval [8, version], NOT an enumerated list --
//  enumerating it is how v15 briefly became unopenable).  Ticks in pre-v13 files
//  are in the old 192-PPQN unit and are scaled on load.  Pd source is embedded in
//  the project and extracted to a sidecar .pd file when opened so libpd can
//  instantiate it.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PROJECT_IO_H
#define PATCHKNOB_SDLUI_PROJECT_IO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace rackx { class RackEngine; }

class perform;
namespace PatchKnob { namespace engine { class AutomationPlayer; class Track; } }

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

// The state a project load leaves an engine track in when the incoming file
// carries no "TRAK" section for that track index.  load_project applies this to
// EVERY track before it commits, so a track the new project never mentions can
// never inherit the outgoing session's instrument, inserts, mix -- or its
// FREEZE (disabled) flag, which is invisible in the mixer and silences the
// track.  Exposed so those defaults stay testable with no audio device.
void project_io_reset_track_defaults(PatchKnob::engine::Track& track);

// TEST HOOK: validate a "PTCH" section PAYLOAD (the bytes after the tag+length)
// exactly as a load would, in DRY mode -- no engine is touched and nothing is
// created.  load_project only parses PTCH when an audio graph is up, so this is
// the only way a head-less test can pin the modular-patch layout down; it is
// what the aux-bus / version-migration checks in project_io_test use.
bool project_io_parse_patch_section(const void* payload, size_t len, unsigned version);

// Human-readable description of the last save/load failure ("" if none).
const char* project_io_last_error();

// Standalone modular-rack interchange. This is the same lossless rack payload
// embedded in .s24 (modules, params, cables, polyphony, Pd/Csound scripts and
// per-instance panels), wrapped in its own versioned .pkr file.
bool save_rack_patch(rackx::RackEngine& rack, const std::string& path);
bool load_rack_patch(rackx::RackEngine& rack, const std::string& path);

#endif // PATCHKNOB_SDLUI_PROJECT_IO_H

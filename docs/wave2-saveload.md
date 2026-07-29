# Wave 2 — Persisting the full DAW state (save/load spec)

Status: design only. This document specifies how to persist the new DAW state added
in Wave 1 (hosted VST instruments, insert-FX chains, mixer state, keyfollow,
automation, audio clips, per-pattern editor choice) **without breaking existing
`.mid` files** written by stock PatchKnob or by this port so far.

All anchors are `path:line` into the tree as of this writing.

---

## 1. How PatchKnob persists state today

PatchKnob saves the whole live set as a **Standard MIDI File, format 1** (`MThd`/`MTrk`)
with two distinct proprietary-data mechanisms layered on top. Both are driven from
`midifile` (`src/midifile.h:29`, `src/midifile.cpp`). There is no separate project
file — everything lives in the one `.mid`.

### 1a. Per-sequence "SeqSpec" chunks (inside each MTrk)

Per-pattern extras are smuggled into each track chunk as a **MIDI meta event of type
`0x7F` (sequencer-specific)**, tagged with a 4-byte magic:

```
FF 7F <var-len> <TAG:4 bytes big-endian> <payload...>
```

Write path — `sequence::fill_list()` (`src/sequence.cpp:3316`). After the note
events it emits, in order:

- `c_triggers_new` — song triggers (`src/sequence.cpp:3395`)
- `c_midibus`      — output bus, 1 byte (`src/sequence.cpp:3416`)
- `c_timesig`      — beats/measure + beat-width, 2 bytes (`src/sequence.cpp:3424`)
- `c_midich`       — MIDI channel, 1 byte (`src/sequence.cpp:3437`)
- then the `FF 2F 00` end-of-track meta (`src/sequence.cpp:3443`)

The exact idiom for a fixed-size SeqSpec (see the `c_midibus` block,
`src/sequence.cpp:3416`):

```cpp
addListVar( a_list, 0 );          // delta-time 0
a_list->push_front( 0xFF );       // meta
a_list->push_front( 0x7F );       // sequencer-specific
a_list->push_front( 0x05 );       // meta length = 4 (tag) + 1 (payload)
addLongList( a_list, c_midibus ); // 4-byte tag, big-endian
a_list->push_front( m_bus );      // 1-byte payload
```

`addListVar` writes a MIDI variable-length quantity (`src/sequence.cpp:3281`);
`addLongList` writes a 4-byte big-endian long (`src/sequence.cpp:3306`).

Read path — `midifile::parse()`, meta-type `0x7f` case
(`src/midifile.cpp:308`). It reads the tag with `read_long()` (`:314`), subtracts 4
from `len`, then matches known tags: `c_midibus` (`:318`), `c_midich` (`:324`),
`c_timesig` (`:330`), `c_triggers` (`:337`), `c_triggers_new` (`:356`).

**The forward-compat property that makes this safe:** after the known-tag `if`
blocks, whatever bytes remain are discarded with `m_pos += len`
(`src/midifile.cpp:381-382`). So a reader that does **not** recognise a tag simply
eats its payload and moves on. Old PatchKnob will silently skip any new SeqSpec we add.
This is the extension seam for small, genuinely per-pattern state.

### 1b. Global trailer chunks (after all MTrks)

Set-wide data is appended **after** the last MTrk as bare `<TAG:4><...>` records (not
wrapped in MIDI meta). Write path — `midifile::write()` (`src/midifile.cpp:617`):

- `c_midictrl`   — MIDI-control bindings (`src/midifile.cpp:691`)
- `c_midiclocks` — per-bus clock on/off (`src/midifile.cpp:696`)
- `c_notes`      — 32 screen-set notepad strings (`src/midifile.cpp:701`)
- `c_bpmtag`     — song BPM (`src/midifile.cpp:717`)

Read path — `src/midifile.cpp:480-585`, with the marker comment
`// *** ADD NEW TAGS AT END` at `src/midifile.cpp:590`.

**Caveat that constrains us:** the trailer reader is **positional, not a tag loop**.
Each block does `if ((file_size - m_pos) > sizeof(unsigned long))` then
`ID = read_long()` then `if (ID == c_xxx)` (e.g. `:480`, `:521`, `:541`, `:577`).
There is no generic "unknown tag → skip length" fallback at file scope. Consequences:

1. New global tags **must be appended strictly after `c_bpmtag`**, in a fixed order,
   each guarded by its own `(file_size - m_pos) > sizeof(unsigned long)` EOF check so
   that an **old file lacking them still loads** (the check fails → we stop).
2. Old PatchKnob stops after `c_bpmtag` and ignores anything past it → **forward
   compatible** (new file still opens in old PatchKnob, minus the new state).

### 1c. The write-path memory model (decisive constraint)

`midifile::write()` accumulates the **entire file** into a `std::list<unsigned
char> m_l` via `push_front` (`src/midifile.h:40`, `src/midifile.cpp:683`,`:712`),
and `sequence::fill_list()` builds each track into a `std::list<char>` one byte per
`push_front` (`src/sequence.cpp:3316`+). Each list node is ~24–32 bytes on 64-bit.

Therefore embedding large binaries in-band is pathological:

- A 100 KB VST state blob → ~2.5–3 MB of list nodes. Across 32 tracks with FX,
  tens of MB transiently. Borderline but survivable.
- Audio PCM (an audio clip is raw `std::vector<float>` per channel,
  `src/engine/audioclip/audio_clip.h:43`) at, say, 50 MB → ~1.5 GB of list nodes.
  **Dealbreaker.**

**Conclusion:** audio, and ideally the larger VST state blobs, must not go through
the `.mid` write path.

### 1d. Where save/load is invoked

- Save: `mainwnd::file_save_dialog` (`src/mainwnd.cpp:257`, calls `f.write` at `:261`)
  and `file_saveas_dialog` (`src/mainwnd.cpp:278`, `f.write` at `:292`).
- Autosave: `src/mainwnd.cpp:482-483` (`autosave.mid`).
- Open: `mainwnd::file_open_dialog` (`src/mainwnd.cpp:324`, `f.parse` at `:339`).
- Import (per-pattern MIDI merge, offset into a screen-set): `src/mainwnd.cpp:403-404`.
  Import must remain MIDI-only and must **not** touch the sidecar.

---

## 2. New state to persist, and its natural scope

| State | Lives in (code) | Scope | Home (recommended) |
|---|---|---|---|
| keyfollow: is-scale-master, follows-master, master key, master scale | `sequence` fields `src/sequence.h:113-119`; accessors `src/sequence.cpp:2796-2860` (getters `src/sequence.h:266-274`) | per **sequence** | in-band SeqSpec |
| clip-view (piano-roll vs tracker) | ephemeral today — dialog each open, `enum clip_view_e` `src/seqmenu.cpp:31`, chosen `src/seqmenu.cpp:261-271`. Needs a new `sequence` field. | per **sequence** | in-band SeqSpec |
| track type (MIDI/instrument, Audio, Bus) | implicit today (an "audio track" = a `Track` whose instrument is an `AudioClipPlayer`, `src/engine/audioclip/audio_clip_player.h:6-9`). Needs an explicit enum. | per **engine track** (0..31) | sidecar |
| instrument: VST format + path + uid | `IPluginInstance::descriptor()` → `PluginDescriptor{format,path,uid,...}` (`src/engine/plugin_api.h:35-44`,`:93`) | per **engine track** | sidecar |
| instrument state blob | `IPluginInstance::saveState()` → `std::vector<uint8_t>` (`src/engine/plugin_api.h:117`); restore via `loadState()` (`:118`) | per **engine track** | sidecar |
| insert FX chain (ordered) | `Track::fxCount/fxAt` (`src/engine/graph/track.h:94-99`); each fx has `descriptor()`+`saveState()` | per **engine track** | sidecar |
| gain / pan / mute / solo | `Track` atomics `src/engine/graph/track.h:103-113` | per **engine track** | sidecar |
| master gain | `MixerGraph::masterGain` `src/engine/graph/mixer_graph.h:62-63` | project | sidecar |
| automation lanes | `AutomationTrack` / `AutomationLane` (`src/engine/automation/automation_track.h`, `automation_lane.h`); target kind+id (`automation_lane.h:44-64`), interp (`:50-54`), breakpoints (`:67-70`) | per **engine track** | sidecar |
| audio clips (placement + audio) | `AudioClipPlayer` schedule: `ScheduledClip{clip*,startSample,gain}` (`src/engine/audioclip/audio_clip.h:84-93`), enumerated by `clipCount/clipAt` (`audio_clip_player.h:75-81`); underlying `AudioClip{name,sampleRate,sourceSampleRate,ch[2]}` (`audio_clip.h:39-43`) | per **engine track** + audio blobs | sidecar `.mid` + WAV bundle |

**Index/scope mapping.** Sequences are `0..c_max_sequence-1` (1024;
`src/globals.h:41`). Engine tracks are `0..31` (`AUDIO_APP_MAX_TRACKS == c_maxBuses
== 32`, `src/audio_app.h:30`, `src/globals.h:46`). A sequence's output **bus**
(`sequence::m_bus`, persisted via `c_midibus`) maps 1:1 to a `MixerGraph` track
(`src/audio_app.h:11-12`). Automation is likewise keyed by track index because it is
emitted per track (`audio_app_route_param(track, paramId, value)`,
`src/audio_app.h:59`, `src/audio_app.cpp:248`).

So: **per-sequence** state attaches to the MTrk it belongs to; **per-engine-track**
state is a flat `[0..31]` array in the sidecar.

---

## 3. Recommendation — a HYBRID split

**Recommended:** keep tiny, genuinely per-pattern flags **in-band as new SeqSpec
chunks**, and put everything track/engine-scoped, binary, or large in a **sidecar**
written alongside the `.mid`.

Split rule:

- **In-band SeqSpec (in the `.mid`):** keyfollow flags, clip-view choice.
  Rationale: they are literally fields on `sequence`, they are 1–4 bytes, and they
  must travel with a pattern through single-pattern export/import
  (`src/mainwnd.cpp:403-404`). The SeqSpec skip-unknown behaviour
  (`src/midifile.cpp:381-382`) makes them invisible to old PatchKnob.

- **Sidecar (new file next to the `.mid`):** track type, instrument
  (format/path/uid/state), FX chain, gain/pan/mute/solo, master gain, automation
  lanes, and audio-clip placements; audio PCM as WAV files in a bundle directory.

### Why not "everything in-band" (extended trailer chunks)?

Rejected primarily on the write-path memory model (§1c): the `std::list<unsigned
char>` accumulator makes multi-MB (audio) or even multi-hundred-KB (VST state ×32
tracks) payloads slow and memory-abusive. Secondary reasons: it bloats what other
tools still see as a MIDI file; the positional trailer reader (§1b) is brittle to
extend with heterogeneous binary; and audio PCM inside a "MIDI" file is a
maintenance liability.

### Why not "everything in a sidecar"?

The per-pattern keyfollow/clip-view genuinely belong to a `sequence` and should
survive per-pattern import/export and copy/paste of a `.mid`. Putting them in-band
(where PatchKnob already carries `bus`/`channel`/`timesig` per pattern) is the
consistent, low-risk choice and costs only two new SeqSpec tags.

### Sidecar packaging

Primary (least disruption to existing plumbing): a **companion file alongside the
`.mid`**, derived from the same path that `global_filename`/the file dialogs already
carry (`src/globals.h:171`, `src/mainwnd.cpp:261`):

```
song.mid            <- unchanged Standard MIDI File (still opens in old PatchKnob / other DAWs)
song.mid.s24        <- sidecar: DAW state (chunked binary, see §5)
song.mid.audio/     <- bundle dir: one WAV per AudioClip (see §5c)
    clip_0001.wav
    clip_0002.wav
```

This keeps `global_filename` as the `.mid` path; the sidecar path is a pure suffix
derivation, so no dialog/plumbing changes beyond one helper. (A future option is a
true bundle directory `song.s24proj/` containing `project.mid` + `daw.s24` +
`audio/`; the format below is identical either way, so this is a packaging choice
only.)

**Missing-sidecar rule:** opening a `.mid` with no sidecar must load exactly as
today (plain PatchKnob set, empty engine state). Opening a sidecar whose `.mid` moved is
an error surfaced to the user; never silently discard MIDI.

---

## 4. New constants to allocate

PatchKnob's tags are `0x2424xxxx` (`src/globals.h:108-117`). Currently used:
`0x24240001`..`0x24240008` and `0x24240010`. Allocate:

```cpp
// src/globals.h — new SeqSpec (per-sequence, in-band) tags
const unsigned long c_keyfollow = 0x24240009;  // key-follow flags block
const unsigned long c_clipview  = 0x2424000A;  // editor view choice

// sidecar container magic + version (NOT a 0x2424 tag; sidecar is its own file)
// 'S','2','4','D' ; bump kDawStateVersion on any incompatible layout change.
static const uint32_t kDawStateMagic   = 0x53323444; // "S24D"
static const uint16_t kDawStateVersion = 1;
```

Reserve `0x2424000B`..`0x2424000F` and `0x24240011`+ for future in-band needs.

---

## 5. On-disk formats

### 5a. New in-band SeqSpec chunks

Written in `sequence::fill_list()` immediately after the `c_midich` block
(`src/sequence.cpp:3437-3438`), before the end-of-track meta (`:3443`), using the
same `addListVar`/`push_front`/`addLongList` idiom.

**`c_keyfollow`** — fixed 4-byte payload, meta length = 8 (4 tag + 4 payload):

| offset | bytes | field | source |
|---|---|---|---|
| 0 | 1 | is_scale_master (0/1) | `sequence::get_scale_master()` `src/sequence.cpp:2803` |
| 1 | 1 | follows_master (0/1) | `sequence::get_follows_master()` `:2820` |
| 2 | 1 | master_key (0..11) | `sequence::get_master_key()` (decl `src/sequence.h:274`) |
| 3 | 1 | master_scale (0..`c_scale_size`-1) | `sequence::get_master_scale()` (decl `src/sequence.h:272`) |

**`c_clipview`** — fixed 1-byte payload, meta length = 5:

| offset | bytes | field | values |
|---|---|---|---|
| 0 | 1 | clip_view | 0 = `CLIP_PIANO_ROLL`, 1 = `CLIP_TRACKER` (mirror `src/seqmenu.cpp:31`) |

Read them in `midifile::parse()` alongside the existing tag matches
(`src/midifile.cpp:318-379`), e.g.:

```cpp
if (proprietary == c_keyfollow) {
    seq->set_scale_master ( m_d[m_pos++] != 0 ); len--;
    seq->set_follows_master( m_d[m_pos++] != 0 ); len--;
    seq->set_master_key    ( m_d[m_pos++] );      len--;
    seq->set_master_scale  ( m_d[m_pos++] );      len--;
}
if (proprietary == c_clipview) {
    seq->set_clip_view( m_d[m_pos++] ); len--;
}
```

Any leftover is still eaten at `src/midifile.cpp:381-382`, so ordering vs. other
tags is unconstrained. Old PatchKnob ignores both tags (skip-unknown).

> Note: today `fill_list` does **not** persist the scale-master fields at all, so
> this is net-new per-pattern data, not a change to existing bytes.

### 5b. Sidecar container (`song.mid.s24`)

A self-describing chunked binary. Little-endian scalars (choose one convention and
keep it; the in-band `.mid` stays big-endian as MIDI requires — the sidecar is
independent). **Do not** build this with the `std::list` idiom; stream it straight to
a `std::ofstream` (this is why it is a separate writer, not `midifile`).

Header:

```
uint32  magic   = kDawStateMagic ("S24D")
uint16  version = kDawStateVersion
uint16  flags   = 0
uint32  midiFileNameLen ; bytes  midiFileName   // basename of the paired .mid, for integrity check
```

Then a sequence of chunks, each `uint32 id; uint32 byteLen; <payload>`. Unknown
chunk ids are skipped via `byteLen` (a real tag loop — unlike the `.mid` trailer, so
the sidecar is trivially forward-extensible). Chunk ids:

- `'MSTR'` — project/master:
  - `float masterGain`  (`src/engine/graph/mixer_graph.h:62`)
  - `uint32 trackCount` (engine tracks present; `MixerGraph::trackCount()` `:55`)
  - `double engineSampleRate` (context for resampling clips on load)

- `'TRKS'` — one record per engine track index `0..trackCount-1`:
  ```
  uint32 trackIndex
  uint8  trackType        // 0=MidiInstrument, 1=Audio, 2=Bus/Aux (see §6)
  float  gain             // Track::gain()  track.h:104
  float  pan              // Track::pan()   track.h:107  (-1..1)
  uint8  mute             // Track::mute()  track.h:110
  uint8  solo             // Track::solo()  track.h:113

  // --- instrument (present iff trackType==MidiInstrument) ---
  uint8  hasInstrument
  if hasInstrument:
     uint8  format        // 0=VST2,1=VST3  (PluginFormat, plugin_api.h:30)
     str    path          // PluginDescriptor.path  plugin_api.h:39
     str    uid           // PluginDescriptor.uid   plugin_api.h:40
     blob   state         // IPluginInstance::saveState()  plugin_api.h:117

  // --- FX chain (ordered) ---
  uint32 fxCount          // Track::fxCount()  track.h:94
  repeat fxCount:
     uint8  format
     str    path
     str    uid
     blob   state

  // --- automation lanes ---
  uint32 laneCount        // AutomationTrack::laneCount()  automation_track.h:28
  repeat laneCount:
     uint8  targetKind    // 0=VstParam,1=MidiCC   automation_lane.h:44
     uint32 targetId      // paramId or CC number  automation_lane.h:59
     uint8  interp        // 0=Linear,1=Step,2=Hold automation_lane.h:50
     uint32 bpCount
     repeat bpCount: { int64 tick; float value }   // Breakpoint  automation_lane.h:67

  // --- audio-clip placements (present for trackType==Audio) ---
  uint32 clipCount        // AudioClipPlayer::clipCount()  audio_clip_player.h:75
  repeat clipCount:
     str    wavName       // filename inside song.mid.audio/  (see §5c)
     int64  startSample   // ScheduledClip.startSample  audio_clip.h:86
     float  gain          // ScheduledClip.gain         audio_clip.h:87
     double sourceSampleRate // AudioClip.sourceSampleRate audio_clip.h:42 (metadata)
  ```

Encoding primitives: `str` = `uint32 len` + `len` bytes (no NUL); `blob` = `uint32
len` + `len` bytes.

### 5c. Audio: consolidate to WAV, do not embed PCM

`AudioClip` holds raw float PCM (`src/engine/audioclip/audio_clip.h:43`) and has
**no source-path field**; `ScheduledClip` is a **non-owning** `const AudioClip*`
(`audio_clip.h:84-93`). Recorded clips (`AudioClipPlayer::stopRecord`,
`src/engine/audioclip/audio_clip_player.h:99`) exist only in memory. So "store a
path" is not sufficient by itself — some clips have no file.

**Recommendation: consolidate.** On save, materialise **every** referenced
`AudioClip` as a 16-bit WAV in `song.mid.audio/` via the existing
`saveWav16()` (`src/engine/audioclip/wav_loader.h:41`), name it deterministically
(`clip_%04d.wav`), and store only that filename + placement in the sidecar (§5b). On
load, read each WAV back with `loadWav()` (resamples to engine rate,
`wav_loader.h:33`) into an owned pool, then re-schedule.

Benefits: the project is self-contained and portable; no giant PCM through the
`.mid` list writer (§1c); reuses code that already exists. Optionally add
`AudioClip::sourcePath` (a new field) to remember external provenance and skip
re-copying unchanged files — a size optimization, not required for correctness.

**Ownership gap to close (blocking):** nothing currently owns `AudioClip` objects for
the app lifetime — `audio_app`'s `g_owned` holds only `IPluginInstance*`
(`src/audio_app.cpp:32`), and schedules are non-owning. The save/load layer must
introduce an **AudioClip pool** (e.g. `std::vector<std::shared_ptr<AudioClip>>` in
`audio_app`) that owns every clip a player references, so loaded clips outlive the
schedule. Specify this pool alongside the loader.

---

## 6. Engine seams needed (and Wave-1 gaps to close)

These are the accessors the sidecar reader/writer needs. Add them to `audio_app`
(the existing engine glue, `src/audio_app.h`) so the save/load module never includes
engine headers directly (same containment rationale as `rackapp.h:5-11`).

**Save side (read from live engine):**
- Instrument descriptor for a track: `g_graph->track(t)->instrument()->descriptor()`
  gives format/path/uid (`plugin_api.h:93`,`:35-44`). No stored descriptor needed —
  `audio_app_set_track_instrument` currently discards `desc` (`src/audio_app.cpp:190`),
  but the concrete host retains it and `descriptor()` returns it.
- Instrument/FX state blob: `instrument()->saveState()` / `fxAt(i)->saveState()`
  (`plugin_api.h:117`, `track.h:97`).
- Mix: `Track::gain/pan/mute/solo` (`track.h:104-113`); master `MixerGraph::masterGain`
  (`mixer_graph.h:63`).
- Add `audio_app_track_type(int)` returning the enum below.

**Track type derivation (save):** there is no stored type today. Derive it:
- instrument is an `AudioClipPlayer` → `Audio`. Detect via a cheap type tag —
  `AudioClipPlayer::descriptor()` returns an empty desc and `saveState()` returns
  `{}` (`audio_clip_player.h:108`,`:134`), so add an explicit marker (e.g. a virtual
  `isAudioClipPlayer()` or a reserved descriptor name) rather than sniffing.
- instrument non-null VST → `MidiInstrument`.
- instrument null → `Bus/Aux` (or MIDI-only pass-through).
Persist the explicit enum so load is unambiguous.

**Load side (reconstruct):**
- `audio_app_load_track_instrument(track, format, path, uid, const uint8_t* state,
  size_t n)` — build a `PluginDescriptor` (format/path/uid), `g_host->instantiate`
  (`src/audio_app.cpp:195`), `prepare`+`setActive`, then `loadState(state)`
  (`plugin_api.h:118`), then `setInstrument` (mirrors `src/audio_app.cpp:190-207`).
- FX equivalent that also `loadState`s (mirrors `audio_app_add_track_fx`,
  `src/audio_app.cpp:209-225`).
- `audio_app_set_track_type_audio(track)` — create an `AudioClipPlayer`, set it as
  the instrument, own it.
- `audio_app_track_add_clip(track, AudioClip* owned, startSample, gain)` — after the
  pool owns the clip, `AudioClipPlayer::addClip` (`audio_clip_player.h:66`).
- Mix setters already exist through the mixer path
  (`setGain/Pan/Mute/Solo`, `track.h:103-113`); expose thin `audio_app_*` wrappers.

**Automation wiring gap (flag):** `AutomationTrack`/`AutomationLane` are headers only;
grep shows no `AutomationPlayer` instantiated in `audio_app.cpp`. The **persistence
format** (§5b) is defined against the data model, but the runtime that owns one
`AutomationTrack` per track and drives `audio_app_route_param` is not yet wired. The
save/load layer needs an owner of `AutomationTrack[0..31]` (put it in `audio_app`
next to the graph). If that owner is deferred, still write/read the `laneCount`/lanes
so files are format-stable now and playback lights up when the player lands.

---

## 7. Ordered implementation checklist

**Phase 0 — constants & scaffolding**
1. Add `c_keyfollow`, `c_clipview`, `kDawStateMagic`, `kDawStateVersion` to
   `src/globals.h` (after `:117`). Reserve the ranges in §4.

**Phase 1 — in-band per-sequence flags (backward-safe, self-contained)**
2. Add `m_clip_view` field + `set_clip_view/get_clip_view` to `sequence`
   (`src/sequence.h` near the scale-master block `:113-119` and accessors
   `:265-275`; default `CLIP_PIANO_ROLL`).
3. Write `c_keyfollow` and `c_clipview` SeqSpec chunks in `sequence::fill_list()`
   after the `c_midich` block (`src/sequence.cpp:3437`), using the §5a layout and the
   `c_midibus` idiom (`:3416`).
4. Parse both tags in `midifile::parse()` next to the existing matches
   (`src/midifile.cpp:318-379`); rely on the skip-unknown tail (`:381`).
5. Use the persisted view in `seqmenu::seq_edit_or_tracker`: honour
   `get_clip_view()` and only fall back to `choose_clip_view()`
   (`src/seqmenu.cpp:261-271`) when unset / user forces a chooser; write the chosen
   view back to the sequence so it round-trips.
6. **Checkpoint:** save a set, reload — key-follow + view survive; open the same
   `.mid` in stock PatchKnob (or `git stash` these) and confirm it still loads (unknown
   SeqSpec skipped).

**Phase 2 — sidecar writer/reader skeleton**
7. New flat files `src/dawstate.h` / `src/dawstate.cpp` (flat in `src/` so the
   top-level `src/*.cpp` glob compiles them, per `rackapp.h:5-11`). Implement the
   §5b container primitives (`str`/`blob`/scalars) over `std::ofstream`/`ifstream`.
   Provide `dawstate::save(midiPath, perform*, MixerGraph*)` and
   `dawstate::load(midiPath, perform*, MixerGraph*)`, plus a `sidecar_path()` /
   `audio_dir()` suffix helper off the `.mid` path.

**Phase 3 — engine seams (§6)**
8. Add the `audio_app_*` save-side getters (track type, instrument descriptor, state
   blobs, FX enumeration) and load-side reconstructors, in `src/audio_app.h` /
   `src/audio_app.cpp` (mirror `:190-225`).
9. Add the explicit `TrackType` enum + `AudioClipPlayer` self-identification marker
   (avoid RTTI sniffing).
10. Introduce the **AudioClip pool** owner in `audio_app` (§5c) and an
    `AutomationTrack[0..31]` owner (§6), with accessors.

**Phase 4 — audio consolidation (§5c)**
11. On save: for each audio track, iterate `clipCount/clipAt`
    (`audio_clip_player.h:75-81`), `saveWav16` each unique `AudioClip` into
    `…​.audio/clip_%04d.wav` (`wav_loader.h:41`), record filename+placement.
12. On load: `loadWav` each referenced file into the pool
    (`wav_loader.h:33`), re-`addClip` at its `startSample`/`gain`.

**Phase 5 — wire into the file menu**
13. In `mainwnd::file_save_dialog` / `file_saveas_dialog`, after the successful
    `f.write` (`src/mainwnd.cpp:261`,`:292`), call `dawstate::save(path, m_mainperf,
    audio_app_graph())`. Report a combined success/fail.
14. In `mainwnd::file_open_dialog`, after `f.parse` (`src/mainwnd.cpp:339`), call
    `dawstate::load(path, …)`; if no sidecar exists, load clean (missing-sidecar
    rule, §3). Do **not** call it from the Import path (`:403-404`).
15. Mirror into autosave (`src/mainwnd.cpp:482-483`) — write `autosave.mid.s24` too.

**Phase 6 — versioning, robustness, tests (§8, §9)**
16. Version-gate the sidecar reader; unknown chunk ids skipped by `byteLen`.
17. Tests per §8.

---

## 8. Test plan

- **Backward compat:** open pre-Wave-2 `.mid` (no sidecar) → clean load, no engine
  state, no crash (missing-sidecar rule).
- **Forward compat:** open a Wave-2 `.mid` in code with the new SeqSpec parsing
  removed → still loads (proves skip-unknown at `src/midifile.cpp:381`). Confirm the
  `.mid` is a valid SMF (loads in another tool).
- **Round-trip per-sequence:** set scale-master/follower/key/scale + tracker view on
  several patterns, save, reload → identical (`get_*` match).
- **Round-trip engine:** assign a VST instrument + 2 FX + non-default
  gain/pan/mute/solo + master gain, tweak a plugin param so `saveState` differs from
  default, save, reload → descriptor path/uid re-instantiated and `loadState`
  restores the tweak; mix values match.
- **Automation:** lanes (one VstParam, one MidiCC) with several breakpoints and each
  interp mode → survive round-trip byte-identical.
- **Audio:** a file-loaded clip and a recorded clip on an audio track → both WAVs
  land in the bundle, reload places them at the same `startSample`/`gain`; verify
  audio plays (self-test style peak check, cf. `src/audio_app.cpp:289`).
- **Move/copy:** moving `song.mid` + `song.mid.s24` + `song.mid.audio/` together
  reloads fully; moving only the `.mid` surfaces a clear "sidecar not found" (not a
  silent MIDI-only load that loses engine state without warning).

---

## 9. Versioning, migration, failure handling

- Bump `kDawStateVersion` on any incompatible sidecar layout change; the reader
  refuses (or migrates) higher versions and warns rather than misparsing.
- Sidecar chunk loop skips unknown `id` via `byteLen` → new chunk types are additive
  and old readers ignore them.
- In-band tags are additive and skip-safe by construction (§1a).
- Integrity: store the paired `.mid` basename in the sidecar header (§5b) and warn if
  it does not match the file being opened (renamed/mismatched sidecar).
- Never let a sidecar failure discard successfully-parsed MIDI: on sidecar
  read/parse error, load the MIDI set and report the engine state could not be
  restored.

---

## 10. Decisions to confirm before coding

1. **Packaging:** companion files next to the `.mid` (recommended, minimal plumbing)
   vs. a true bundle directory. Format is identical; pick the UX.
2. **Automation scope:** confirmed here as per **engine track** (0..31) because it
   emits via `audio_app_route_param(track,…)` (`src/audio_app.h:59`). If product
   intent is per-**pattern** automation, move the lane chunks in-band per MTrk
   instead — this is the one scope that could plausibly flip.
3. **Audio provenance:** always consolidate to WAV in the bundle (recommended) vs.
   also remembering the external source path to avoid re-copying unchanged files
   (requires adding `AudioClip::sourcePath`).
4. **Sidecar endianness/format:** chunked **binary** (recommended — matches the
   codebase's tag idiom, no new deps, holds binary blobs natively) vs. JSON + sibling
   blob files (more debuggable, needs a JSON dep and out-of-line blobs).

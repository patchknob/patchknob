# Wave 2 — Track Types, Audio Clips, and Bounce/Record

Implementation spec for adding **track types** (INSTRUMENT / AUDIO), **audio clips**,
and **bounce-to-audio-track (record)** to the PatchKnob Windows-port DAW.

This is a **design document**. It describes the minimal data-model and wiring
changes and gives an ordered, file-anchored implementation checklist. No source
is changed by this document.

All paths are relative to `src/` unless noted. Line numbers are anchors against
the tree as read on 2026-07-12; treat them as "look here", not literal offsets.

---

## 0. What already exists (the substrate we build on)

### The bus == track == graph-node identity
PatchKnob's output "bus" index (0..31) is the single spine that ties the MIDI
sequencer to the audio engine:

- A `sequence` targets exactly one bus via `m_bus` (`sequence.h:97`, init `sequence.cpp:45`).
  It dumps its events with `m_masterbus->play( m_bus, &e, ch )`
  (`sequence.cpp:567`, `:3036`, `:3060`).
- `mastermidibus::play` (`midibus.cpp:676-693`) forwards each channel-voice
  message to `PatchKnob::app::audio_app_route_midi( (int)a_bus, ... )` (`midibus.cpp:691`).
- `audio_app_route_midi` (`audio_app.cpp:234-246`) enqueues onto a lock-free ring;
  the audio callback `audio_render` (`audio_app.cpp:59-110`) drains it into
  per-track `TrackBlockInput`s and calls `g_graph->renderBlock(...)` (`audio_app.cpp:109`).
- `MixerGraph` owns 32 `Track`s (`audio_app.cpp:140`, `AUDIO_APP_MAX_TRACKS==c_maxBuses==32`,
  `audio_app.h:30`, `globals.h:46`). `MixerGraph::track(i)` (`mixer_graph.h:56-58`)
  is the graph node for bus `i`.
- Each `Track` runs `instrument -> FX chain -> gain/pan -> VU -> out`
  (`track.cpp:117-204`). The instrument is any `IPluginInstance`
  (`track.h:79-80`), fed MIDI+automation+transport each block.

So **"bus N" and "MixerGraph Track N" are the same track.** Track type is a
property of that node, not of any one sequence.

### The audio-clip engine (built by the parallel agent, assume present)
`engine/audioclip/` provides, against the stable `plugin_api.h` contract:

- `AudioClip` (`audio_clip.h:39-79`): in-memory planar stereo buffer + metadata;
  `numFrames()` at `:46`.
- `ScheduledClip` (`audio_clip.h:84-93`): `{ const AudioClip*; int64 startSample; float gain }`;
  a non-owning placement on the sample timeline.
- `AudioClipPlayer : public IPluginInstance` (`audio_clip_player.h:50`):
  **already a drop-in Track instrument.** Key surface:
  - `addClip(clip, startSample, gain)` (`:66`), `removeClip` (`:69`), `clearClips` (`:72`).
  - `startRecord(maxSeconds)` (`:88`), `stopRecord(name) -> shared_ptr<AudioClip>` (`:99`),
    `captureBlock(in, nframes)` (`:104`) — **the bounce integration entry point.**
  - `process()` (`:115`) mixes scheduled clips **only while `blk.isPlaying`**, using
    `blk.playPositionSamples` as the block window start (`audio_clip_player.cpp:159-188`),
    and *also* appends `blk.audioIn` to the capture buffer when armed
    (`audio_clip_player.cpp:191-193`) — see the double-capture note in §4.
  - descriptor advertises `isInstrument=true`, `uid="builtin.audioclip"`,
    `numAudioIn=2`, `numAudioOut=2` (`audio_clip_player.cpp:11-26`).
- `wav_loader.h`: `loadWav(path, engineRate, out)` (`:33`), `saveWav16` (`:41`).

**Consequence:** the audio graph *already* renders an audio track. An audio track
is just `Track` whose `instrument()` is an `AudioClipPlayer`; `Track::processBlock`
runs it exactly like a VST synth (`track.cpp:129-148`). No render-loop change is
needed for audio-clip *playback* — **except that the transport play position is
never advanced today** (see §3, the one true blocker).

### The gap in one sentence
`audio_app_init` calls `g_graph->setTransport(120.0, 0, true)` exactly once
(`audio_app.cpp:153`) and nothing ever moves `playPos` again. The audio thread
reads it into `RenderContext` every block (`mixer_graph.cpp:62-66`), so every
block sees `playPositionSamples == 0`. Audio clips can only ever play the window
`[0, nframes)` and never advance. **Fixing transport sync (§3) is the prerequisite
for both audio playback and bounce.**

---

## 1. Track types

### 1.1 Where the track type lives — authoritative on `engine::Track`
Add an explicit type to the graph node (the bus). This is the single source of
truth; the UI and the bounce coordinator read it from here.

`engine/graph/track.h`, near the instrument accessors (`track.h:79-80`):

```cpp
enum class TrackType { Instrument, Audio };

void      setType(TrackType t) { type_ = t; }   // message thread
TrackType type() const         { return type_; }
```

Add member (with the other message-thread config): `TrackType type_ = TrackType::Instrument;`.

Rationale for putting it here and not on `sequence`:
- Type is per-bus, and many sequences can share one bus. Storing it on the
  sequence would let two clips on the same bus disagree.
- The audio thread does **not** need it — `Track::processBlock` already does the
  right thing for whatever `instrument()` is set. Type is a message-thread /
  routing concept only, so a plain (non-atomic) member is fine.

Do **not** try to infer type by `dynamic_cast`/uid at runtime; keep it explicit.
(You *may* assert consistency: an `Audio` track's instrument should be an
`AudioClipPlayer`.)

### 1.2 Making an audio track — glue factory
Model on `audio_app_set_track_instrument` (`audio_app.cpp:190-207`). Add to
`audio_app.h`/`.cpp`:

```cpp
bool audio_app_make_audio_track(int track);   // instrument := new AudioClipPlayer, type := Audio
int  audio_app_track_type(int track);         // 0=Instrument, 1=Audio, -1=bad
PatchKnob::engine::AudioClipPlayer* audio_app_track_player(int track); // player or null
```

`audio_app_make_audio_track(t)`:
1. bounds-check `t` against `AUDIO_APP_MAX_TRACKS`.
2. `auto* p = new AudioClipPlayer(); p->prepare(g_sr, g_block); p->setActive(true);`
   (mirrors instrument prep at `audio_app.cpp:198-199`).
3. `Track* trk = g_graph->track(t); trk->setInstrument(p); trk->setType(TrackType::Audio);`
4. `g_owned.push_back(p);` so shutdown releases it (`audio_app.cpp:173-177`).

`audio_app_track_player(t)` returns the instrument cast to `AudioClipPlayer*`
only when `trk->type()==Audio` (safe because we set both together).

An INSTRUMENT track needs no new factory — it is the existing default and is
populated by `audio_app_set_track_instrument` via the rack UI
(`rackapp.cpp:71-74`).

---

## 2. Clip types

There are three clip kinds, but only **two storage regimes**:

| Clip kind    | Lives on         | Storage                                   | Playback path                          |
|--------------|------------------|-------------------------------------------|----------------------------------------|
| Piano-roll   | INSTRUMENT track | `sequence` (MIDI events)                  | per-tick MIDI dump -> VST (existing)   |
| Tracker      | INSTRUMENT track | `sequence` (MIDI events + param automation)| per-tick MIDI/param dump -> VST (existing) |
| Audio        | AUDIO track      | `AudioClip` samples, scheduled by sample  | `AudioClipPlayer` reads sample transport|

**Piano-roll vs tracker is a per-sequence *view*, not a different data model.**
Both are ordinary `sequence`s that emit MIDI/param to the bus's hosted VST. The
only thing that differs is which editor opens (piano roll vs the tracker grid /
FX-command columns referenced in `engine/automation/automation_lane.h`). This is
a UI switch, not an engine change.

**Audio clips are not MIDI**, so they are not stored as events. They are
`AudioClip` sample buffers scheduled into the AUDIO track's `AudioClipPlayer`.

### 2.1 Minimal `sequence` additions (the "clip unit")
`sequence` is PatchKnob's clip object (it already carries `m_bus`, `m_length`
`sequence.h:164`, and the timeline `m_list_trigger` `sequence.h:82`, whose
`trigger.m_tick_start` `sequence.h:49` is the placement). Extend it minimally so
the same object can represent an audio clip placement without dragging engine
headers into the core:

`sequence.h`, near `m_bus` (`:97`):

```cpp
enum clip_type_e { CLIP_PIANOROLL = 0, CLIP_TRACKER = 1, CLIP_AUDIO = 2 };
clip_type_e m_clip_type;   // default CLIP_PIANOROLL
int         m_audio_clip_id; // index into the engine clip pool; -1 == none
```

Accessors near `get/set_midi_bus` (`sequence.h:349-350`). Initialise in the ctor
next to `m_bus = 0;` (`sequence.cpp:45`): `m_clip_type = CLIP_PIANOROLL; m_audio_clip_id = -1;`.
Copy both in `operator=` next to `m_bus` (`sequence.cpp:2634`). If persisted, add
them to `fill_list` alongside `m_bus` (`sequence.cpp:3421`) and the loader.

Why an `int` id and not a `shared_ptr<AudioClip>` on the sequence:
- Keeps `sequence.h` free of `engine/audioclip/*` includes (the core stays light,
  same discipline as `audio_app.h` forward-declaring engine types, `audio_app.h:20-26`).
- The actual sample buffers are owned by an **engine-side clip pool** in the glue
  (§2.2), whose lifetime matches the audio device, and which the realtime
  `ScheduledClip` non-owning pointers require anyway (`audio_clip.h:84-86`).
- `-1` cleanly means "MIDI clip / no audio".

A `sequence` with `m_clip_type == CLIP_AUDIO` has an empty `m_list_event`, so its
per-tick `sequence::play` (`sequence.cpp` `play`) dumps no MIDI — it is inert on
the existing output path, which is exactly what we want (audio scheduling is
handled out-of-band, §2.3). No change to `perform::play` (`perform.cpp:676-723`)
is required for audio clips to coexist.

### 2.2 The engine-side clip pool (glue)
Audio-clip sample data must outlive any `ScheduledClip` that references it and be
shared across schedule/record. Own it in `audio_app.cpp`:

```cpp
std::vector<std::shared_ptr<AudioClip>> g_clipPool;   // message-thread owned
int  audio_app_pool_add(std::shared_ptr<AudioClip>);  // returns clip id
const AudioClip* audio_app_pool_get(int id);          // stable ptr for schedules
```

`audio_app_pool_add` pushes and returns the index used as `sequence::m_audio_clip_id`.
Because `shared_ptr` keeps the object at a stable address, `pool_get(id)` yields a
pointer valid for the audio thread's `ScheduledClip` (same contract as the
FX-chain snapshot). Never erase pool entries while the transport is live.

Loading a WAV onto an audio track (message thread):
1. `AudioClip c; loadWav(path, g_sr, c)` (`wav_loader.h:33`).
2. `int id = audio_app_pool_add(std::make_shared<AudioClip>(std::move(c)));`
3. `seq->set_clip_type(CLIP_AUDIO); seq->set_audio_clip_id(id); seq->set_midi_bus(audioTrack);`
4. schedule it (§2.3).

### 2.3 Attaching an audio clip to the timeline
An audio clip's timeline position comes from the `sequence`'s placement (its
trigger start tick, or `m_starting_tick`-relative song position). Translate that
tick to a sample and hand it to the AUDIO track's player:

```cpp
int64_t startSample = audio_app_tick_to_samples(startTick);   // §3.3
AudioClipPlayer* p = audio_app_track_player(audioTrack);
p->addClip(audio_app_pool_get(seq->get_audio_clip_id()), startSample, gain);
```

Provide a glue helper `audio_app_schedule_audio_clip(int track, int clipId,
long startTick, float gain)` that does exactly this. Rescheduling on edit =
`p->clearClips()` then re-add for every audio sequence on that bus. Because the
player publishes its schedule via RCU (`audio_clip_player.cpp:58-70`), edits are
realtime-safe while the transport rolls.

Playback then needs nothing further: the audio thread renders the player against
`playPositionSamples` (`audio_clip_player.cpp:159-188`) — once §3 advances it.

---

## 3. Transport: tick <-> sample sync (the enabling change)

Playback of audio clips and the timing of bounce both require the graph's
`playPositionSamples` to advance in lockstep with PatchKnob's tick clock and to be
gated by the real run state.

### 3.1 The mapping
PatchKnob ticks are `c_ppqn = 192` pulses per quarter note (`globals.h:44`); tempo is
`bpm = m_master_bus.get_bpm()` (used in the output loop at `perform.cpp:1265`).
The audio engine runs at `g_sr` (48000, `audio_app.cpp:29`,`:149`).

```
samples_per_tick(bpm) = g_sr * 60.0 / (bpm * c_ppqn)
tick_to_samples(tick) = llround(tick * samples_per_tick(bpm))
samples_to_tick(s)    = llround(s    / samples_per_tick(bpm))
```

This is exact for constant tempo. Variable/ramped tempo would need integration
across the change; out of scope for v1 (note it as a limitation).

### 3.2 Who advances the cursor — advance on the audio thread, seed on control
Do **not** try to write `playPos` from the output thread every iteration (it runs
in bursty prebuffer chunks and would make the clip window stutter). Instead:

- The **audio thread** owns a sample-accurate cursor and advances it by `nframes`
  each block while playing. This gives sample-continuous clip playback.
- The **message/control thread** only *seeds* the cursor on start/stop/seek/loop.

Add to `audio_app.cpp` (file-static atomics next to `g_graph` `audio_app.cpp:25-29`):

```cpp
std::atomic<int64_t> g_xportPos{0};      // audio-thread advanced play cursor (samples)
std::atomic<int64_t> g_xportSeek{-1};    // control thread posts a target; -1 == none
std::atomic<double>  g_xportTempo{120.0};
std::atomic<bool>    g_xportPlaying{false};
```

New message-thread API (`audio_app.h`, near the transport-free accessors `:38-40`):

```cpp
void audio_app_transport_set_tempo(double bpm);
void audio_app_transport_set_playing(bool playing);
void audio_app_transport_seek_samples(int64_t sample);   // e.g. on start/loop
void audio_app_transport_seek_ticks(long tick);          // uses §3.1 with current bpm
int64_t audio_app_tick_to_samples(long tick);
```

`seek_*` write `g_xportSeek`; `set_playing`/`set_tempo` write their atomics.

Rework `audio_render` (`audio_app.cpp:59-110`) so that *before* `renderBlock`
(currently the single call at `:109`) it establishes the block's transport:

```cpp
int64_t seek = g_xportSeek.exchange(-1, std::memory_order_acq_rel);
int64_t pos  = (seek >= 0) ? seek : g_xportPos.load(std::memory_order_relaxed);
bool    play = g_xportPlaying.load(std::memory_order_relaxed);
double  bpm  = g_xportTempo.load(std::memory_order_relaxed);

g_graph->setTransport(bpm, pos, play);       // mixer_graph.h:70-74
g_graph->renderBlock(out, numChannels, nframes, s_inputs, NT);   // existing :109

g_xportPos.store(play ? pos + nframes : pos, std::memory_order_relaxed);
```

`MixerGraph::setTransport` already stores into the atomics that
`renderBlock` reads into `RenderContext` (`mixer_graph.cpp:62-66`), which
`Track::processBlock` forwards into `ProcessBlock` (`track.cpp:145-147`), which
the `AudioClipPlayer` consumes (`audio_clip_player.cpp:159-166`). The chain is
already wired — we are only making `pos` move.

Remove the one-shot `g_graph->setTransport(120.0, 0, true)` at `audio_app.cpp:153`
(or leave it as the initial seed; it becomes redundant once the cursor advances).

### 3.3 Hook points in `perform`
- `perform::inner_start` (`perform.cpp:926-946`), where `set_running(true)` is
  called (`:937`): seed and roll —
  `audio_app_transport_seek_ticks(m_playback_mode ? m_starting_tick : 0);`
  `audio_app_transport_set_playing(true);`
  (`m_starting_tick` is the song offset already used at `perform.cpp:1222`.)
- `perform::inner_stop` (`perform.cpp:951-956`): `audio_app_transport_set_playing(false);`
  (optionally `audio_app_transport_seek_samples(0)` to mirror `m_tick = 0` at
  `perform.cpp:1595`).
- Loop wrap in `output_func` (`perform.cpp:1437-1448`), right after
  `reset_sequences()` and `set_orig_ticks(get_left_tick())` (`:1444-1446`):
  `audio_app_transport_seek_ticks(get_left_tick());` so audio clips loop with the
  song.
- `perform::set_bpm` (`perform.cpp:546-554`), after `m_master_bus.set_bpm`
  (`:552`): `audio_app_transport_set_tempo(a_bpm);`.

No change to the tick math in `output_func` (`perform.cpp:1417-1419`,`:1452`) —
the MIDI clock is untouched; we only mirror run-state/tempo/seeks to the audio
transport.

Accuracy note: MIDI events are already block-granular (`sampleOffset = 0`,
`audio_app.cpp:76`), and clip windows are block-granular here too. Sub-block
sample-accuracy (deriving the cursor from the RtAudio callback count and
distributing MIDI at true sample offsets) is a later refinement.

---

## 4. Bounce: instrument track post-fx -> audio track record

Goal: capture an INSTRUMENT track's rendered stereo output into an AUDIO track's
`AudioClipPlayer` record buffer, then place the result as an audio clip.

### 4.1 The tap point
`Track::processBlock` writes the track's final post-gain/pan stereo into `out`
(`track.cpp:196-199`) and updates its VU (`:202-203`); this `out` is the
per-track scratch `trkOut` in the graph (`mixer_graph.cpp:67`,`:86`) that gets
summed into master (`:88-93`). **That buffer is exactly "what this track sounds
like".** Tapping it there is the clean bounce source. (This is post-fader; if a
pre-fader bounce is ever wanted, tap `cur[]` right before the gain stage at
`track.cpp:185`, but post-fader is the correct default and is what §4 specifies.)

### 4.2 A tiny sink interface (decouples the graph from audioclip)
`Track` only ever talks to `IPluginInstance` today (`track.h:12-13`). Keep that
discipline: add a one-method sink interface rather than including the audioclip
header into the graph.

New in `plugin_api.h` (or a small `engine/audio_sink.h`):

```cpp
class IAudioSink {
public:
    virtual ~IAudioSink() {}
    // Realtime: append nframes of planar stereo (in[1] may be null == mono).
    virtual void captureBlock(const float* const* in, int nframes) = 0;
};
```

`AudioClipPlayer` already declares `captureBlock(const float* const*, int)`
(`audio_clip_player.h:104`) with this exact signature — just add the base:
`class AudioClipPlayer : public IPluginInstance, public IAudioSink`.

`engine/graph/track.h`: add an atomic tap (message thread sets, audio thread
reads; same lifetime discipline as the instrument pointer):

```cpp
void       setCaptureTap(IAudioSink* s) { captureTap_.store(s, std::memory_order_release); }
IAudioSink* captureTap() const          { return captureTap_.load(std::memory_order_acquire); }
// member:
std::atomic<IAudioSink*> captureTap_{nullptr};
```

`engine/graph/mixer_graph.cpp`, in `renderBlock` immediately after
`t->processBlock(...)` (`mixer_graph.cpp:86`), before the audible-sum block
(`:88`):

```cpp
if (IAudioSink* tap = t->captureTap())
    tap->captureBlock(trkOut, nframes);   // trkOut == this track's post-fader stereo
```

`captureBlock` only appends when the sink is armed (`audio_clip_player.cpp:109-127`),
so leaving a tap connected while disarmed is harmless.

### 4.3 Avoiding double-capture (must-fix)
`AudioClipPlayer::process` *also* appends `blk.audioIn` when armed
(`audio_clip_player.cpp:191-193`). In the graph, the destination audio track's
own `Track::processBlock` passes its (zeroed) instrument-input buffers as
`blk.audioIn` (`track.cpp:136-138`, `inPtrs = { cur[0], cur[1] }` after the
zero-fill at `:132-133`). If the destination player is armed, it would append a
block of **silence** via `process()` *in addition* to the real audio appended by
the §4.2 tap — corrupting the take (double length, interleaved silence).

Fix (one line, correct for every instrument): in `Track::processBlock`, feed the
instrument stage `blk.audioIn = nullptr` instead of the zeroed `cur[]`
(`track.cpp:136`,`:138`). Synth/instrument plugins generate from MIDI and ignore
audio input; the graph never has a live audio source feeding an instrument's
input anyway (it zeros `cur` first). Nulling it makes the explicit `captureBlock`
tap the **single** capture path and disables the player's silent auto-append. The
FX stage's `audioIn` (`track.cpp:164`) is unaffected.

(Alternative if you prefer not to touch `track.cpp`: give the player an
`setAutoCaptureFromInput(bool)` gate and clear it in graph context. The null-input
fix is simpler and strictly better here.)

### 4.4 The bounce operation (message-thread coordinator, in the glue)
Add `audio_app_route_bounce(int srcTrack, int dstTrack)` and a driver
`audio_app_bounce(int srcTrack, int dstTrack, long startTick, long endTick)`:

1. Validate: `dstTrack` is an AUDIO track; `p = audio_app_track_player(dstTrack)`
   (§1.2) non-null. `srcTrack` is an INSTRUMENT track with an instrument
   (`audio_app_track_has_instrument`, `audio_app.cpp:227-232`).
2. `double sec = (endTick - startTick) * samples_per_tick / g_sr;`
   `p->startRecord(sec + guard);` (`audio_clip_player.h:88`) — reserves the buffer
   on the message thread so the audio thread never allocates.
3. `g_graph->track(srcTrack)->setCaptureTap(p);` (§4.2) — arm the tap.
4. Drive transport over the region: `audio_app_transport_seek_ticks(startTick);
   audio_app_transport_set_playing(true);` (§3). Real-time bounce: poll
   `p->recordedFrames()` (`audio_clip_player.h:94`) until it reaches
   `(endTick-startTick)` samples, or run a Glib timeout for `sec`.
5. Stop: `audio_app_transport_set_playing(false);
   g_graph->track(srcTrack)->setCaptureTap(nullptr);`
6. `auto clip = p->stopRecord("bounce");` (`audio_clip_player.h:99`).
7. Place it: `int id = audio_app_pool_add(clip);` then
   `p->addClip(audio_app_pool_get(id), tick_to_samples(startTick));` (§2.3), and
   create/point an audio `sequence` on `dstTrack` at `id` (§2.1) so it persists
   and is visible in the song editor.

Notes:
- Because the tap reads `srcTrack`'s post-fader output and the destination player
  records via the tap only (§4.3), ordering of src vs dst in the render loop does
  not matter for the *capture* (the tap fires when the source renders). The dst
  player's own scheduled playback is independent.
- v1 is **real-time** bounce (audio flows through the live RtAudio device).
  Offline / faster-than-real-time rendering would require driving
  `MixerGraph::renderBlock` from a non-device clock; out of scope, note as future.
- The tap is a general audio send; the same mechanism can later feed sub-mix/aux
  buses.

---

## 5. Ordered implementation checklist

Do the transport first — nothing audible works without it.

**Phase A — Transport sync (unblocks audio playback + bounce timing)**
1. `audio_app.cpp:25-29` — add `g_xportPos/g_xportSeek/g_xportTempo/g_xportPlaying`
   atomics. (§3.2)
2. `audio_app.h:38-59` region — declare `audio_app_transport_set_tempo /
   set_playing / seek_samples / seek_ticks`, `audio_app_tick_to_samples`. (§3.2)
3. `audio_app.cpp` — implement them; add `samples_per_tick` helper using
   `c_ppqn` (`globals.h:44`) and `g_sr` (`audio_app.cpp:149`). (§3.1)
4. `audio_app.cpp:59-110` — rework `audio_render` to seed/advance the cursor and
   call `g_graph->setTransport(...)` before `renderBlock` (`:109`); drop/keep the
   one-shot at `:153`. (§3.2)
5. `perform.cpp:937` (inner_start), `:953` (inner_stop), `:1444-1446` (loop wrap),
   `:552` (set_bpm) — call the new transport API. (§3.3)
6. Verify: schedule a clip on a track via a temporary hook and confirm it plays
   through and advances (peak via `AudioEngine::masterPeak`, `audio_engine.h:134`).

**Phase B — Track types + audio-track factory**
7. `engine/graph/track.h:79` — add `enum class TrackType`, `type_`, `setType/type`. (§1.1)
8. `audio_app.h` / `audio_app.cpp:190` neighbourhood — add
   `audio_app_make_audio_track`, `audio_app_track_type`, `audio_app_track_player`. (§1.2)

**Phase C — Audio clips + clip pool + scheduling**
9. `audio_app.cpp` — add `g_clipPool`, `audio_app_pool_add/get`. (§2.2)
10. `audio_app.*` — add `audio_app_schedule_audio_clip(track, clipId, startTick, gain)`
    and a WAV-load helper wrapping `loadWav` (`wav_loader.h:33`). (§2.2/§2.3)
11. `sequence.h:97` / `:349` — add `clip_type_e m_clip_type`, `int m_audio_clip_id`
    + accessors; init at `sequence.cpp:45`, copy at `sequence.cpp:2634`, persist at
    `sequence.cpp:3421` (+ loader) if saved to file. (§2.1)

**Phase D — Bounce/record**
12. `plugin_api.h` (or `engine/audio_sink.h`) — add `IAudioSink`. (§4.2)
13. `engine/audioclip/audio_clip_player.h:50` — also inherit `IAudioSink`
    (signature already matches at `:104`). (§4.2)
14. `engine/graph/track.h` — add atomic `captureTap_` + `setCaptureTap/captureTap`. (§4.2)
15. `engine/graph/mixer_graph.cpp:86` — after `processBlock`, call
    `tap->captureBlock(trkOut, nframes)`. (§4.2)
16. `engine/graph/track.cpp:136,138` — set instrument-stage `blk.audioIn = nullptr`
    (kills double-capture). (§4.3)
17. `audio_app.*` — add `audio_app_route_bounce` and the `audio_app_bounce(...)`
    driver (arm, tap, roll region, stop, finalize, place). (§4.4)

**Phase E — UI wiring (thin, on top of the glue)**
18. Track header: track-type selector -> `audio_app_make_audio_track` /
    `audio_app_set_track_instrument` (existing rack path, `rackapp.cpp:71-74`).
19. Clip editors: open piano-roll vs tracker per `sequence::m_clip_type`; for
    `CLIP_AUDIO` open a waveform/clip view. (View-only; no engine change.)
20. "Bounce" menu action on an instrument track -> pick/target audio track ->
    `audio_app_bounce(src, dst, leftTick, rightTick)` using the song's
    left/right ticks (`perform::get_left_tick`/`get_right_tick`, `perform.h:189`,`:195`).

---

## 6. Risks / invariants to preserve

- **Realtime rules** (`plugin_api.h:100-101` contract): everything the audio
  thread touches — the new tap call (`mixer_graph.cpp:86`), the cursor
  advance/seek (`audio_app.cpp`), `captureBlock` (`audio_clip_player.cpp:109-127`,
  appends within `startRecord`-reserved capacity) — is allocation-free and
  lock-free. Keep it that way; never `pool_add`/`stopRecord`/`clearClips` from the
  audio thread.
- **Clip lifetime**: `ScheduledClip`/tap hold non-owning pointers
  (`audio_clip.h:84-86`). The `g_clipPool` `shared_ptr`s must outlive every
  schedule; do not erase pool entries or drop a recorded clip while the transport
  is live.
- **Instrument-input null change** (`track.cpp:136`): audited safe — the graph
  feeds instruments only silence today; VST synths ignore audio input. Re-check if
  a live audio-input-monitor feature is added later (it would then route through
  the FX-stage input or a dedicated input node, not the instrument stage).
- **bus/track count**: `AUDIO_APP_MAX_TRACKS == c_maxBuses == 32`
  (`audio_app.h:30`, `globals.h:46`); all new per-track arrays/loops must respect
  it, matching `audio_render`'s `NT` (`audio_app.cpp:61`).
- **Tempo changes** invalidate previously computed `startSample`s (§3.1 is
  bpm-dependent). For v1 assume constant tempo during a bounce and while audio
  clips are scheduled; reschedule audio clips on tempo change if that assumption
  is relaxed.

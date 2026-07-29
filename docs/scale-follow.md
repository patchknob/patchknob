# Scale Master / Scale Follow — Design Document

## 0. Goal

Designate ONE currently-playing pattern (`sequence`) as the **scale master**. It
carries a musical scale: a **root** (pitch-class 0–11, C..B) and a **scale type**
(`c_scale_major`, `c_scale_minor`, ...). Every OTHER pattern that is playing at the
same time and has its **follow** toggle on will have each emitted note snapped to
the nearest in-scale pitch, **in real time** and **non-destructively** — the stored
`event` data is never modified; only the byte sent to the MIDI bus is altered.

All file:line references below are against the PatchKnob-0.8.7 tree as read.

---

## 1. Where to snap (the emit call-site)

### The playback path

`perform::play(long a_tick)` (perform.cpp:639) runs on the **output thread**
(`perform::output_func` → `perform::play`). It loops over every active sequence and
calls `sequence::play(a_tick, m_playback_mode)` (perform.cpp:660, and the queued
case at perform.cpp:656).

`sequence::play()` (sequence.cpp:251) walks `m_list_event`, and for every event
whose timestamp falls in the current tick slice it calls:

```
put_event_on_bus( &(*e) );      // sequence.cpp:358
```

`sequence::put_event_on_bus(event *a_e)` (sequence.cpp:2873) is the **single,
authoritative emit point**. It reads the note, maintains the note-on/off reference
count in `m_playing_notes[]`, and finally calls:

```
m_masterbus->play( m_bus, a_e, m_midi_channel );   // sequence.cpp:2895
```

### The chosen snap site: `put_event_on_bus` (sequence.cpp:2873)

**Snap inside `put_event_on_bus`, before the bus call at sequence.cpp:2895.**

Reasons:

1. It is the *only* place note data leaves a follower sequence for live playback.
   (`off_playing_notes()` at sequence.cpp:2905 and `play_note_on/off` at
   sequence.cpp:1582/1600 are separate paths — see §4 stuck-note handling and §6.)
2. It already owns the note-on/off reference count `m_playing_notes[note]`
   (sequence.cpp:2877–2892), which is exactly the bookkeeping we must keep
   consistent across the snap (§4).
3. Snapping here is non-destructive by construction: `a_e` points at the stored
   `event` in `m_list_event`, so we must **NOT** mutate `*a_e`. Instead, compute a
   snapped note number and play a *temporary copy*, or pass an explicit note byte to
   the bus. (See §4 — the cleanest approach is a local `event` copy with
   `set_note()` applied to the copy only.)

Do **not** snap inside `sequence::play()` directly — keeping the logic in
`put_event_on_bus` means every emit path that already funnels through it is covered,
and the `m_playing_notes[]` accounting stays in one place.

> Note: `put_event_on_bus` uses `a_e->get_note()` (unsigned char) and indexes
> `m_playing_notes[note]` (array size `c_midi_notes = 256`, globals.h:100), so the
> note index is always valid even after snapping.

---

## 2. Data model & thread-safety

### Per-sequence members (sequence.h, private section near line 96–151)

```cpp
bool m_is_scale_master;   // this sequence carries the master scale
bool m_follows_master;    // this sequence snaps its notes to the master scale
```

with public accessors (mirroring existing `set_playing/get_playing` style,
sequence.h:238–240):

```cpp
void set_scale_master( bool );   bool get_scale_master();
void set_follows_master( bool ); bool get_follows_master();
```

The master's **(root, scale)** itself does NOT need new sequence members for the
MVP: PatchKnob already has a per-pattern scale/key concept used only for the piano-roll
(`seqedit::m_scale`, `seqedit::m_key` — seqedit.h:187,190). For the master we add an
explicit pair of members so the value is owned by the model, not the editor window:

```cpp
int m_master_scale;   // index into c_scales_* tables: c_scale_major, c_scale_minor...
int m_master_key;     // root pitch-class 0..11 (C..B), see c_key_text[] globals.h:270
```

with `set_master_scale/get_master_scale`, `set_master_key/get_master_key`.

### Perform-level master pointer (perform.h, private, near line 74)

`perform` owns all sequences via `m_seqs[c_max_sequence]` (perform.h:74) and
`m_seqs_active[]` (perform.h:76). Add:

```cpp
int m_scale_master_seq;   // index into m_seqs[], or -1 == none
```

initialized to `-1` in `perform()` / `perform::init()`. Public API:

```cpp
void set_scale_master( int a_seq );   // enforce single master; -1 clears
int  get_scale_master( void );
```

`set_scale_master(n)` clears the previous master's `m_is_scale_master` flag (so only
one master exists — §4), sets the new one, and stores `m_scale_master_seq = n`.

### How a follower reads the master scale without races

The snap runs on the **output thread** inside `sequence::put_event_on_bus` (which
holds the *follower's own* `m_mutex` via `lock()` at sequence.cpp:2875). It must read
three values that belong to a *different* object (the master sequence) and to
`perform`:

- `m_scale_master_seq` (in `perform`)
- the master's `m_master_key`, `m_master_scale`, and whether the master is still
  playing (`get_playing()`).

PatchKnob's locking primitive is a per-object recursive-style `mutex` (mutex.h:27;
`sequence::m_mutex` at sequence.h:150; `sequence::lock/unlock` at sequence.h:165).
`perform` does **not** itself hold a mutex member, so we cannot take "the perform
lock."

Recommended approach — **pass a resolved snapshot down, computed under perform's
control on the same output thread**, avoiding cross-object lock ordering entirely:

1. In `perform::play()` (perform.cpp:639), once per tick, before the sequence loop,
   resolve the active master into three plain scalars:

   ```cpp
   bool master_on = false; int master_key = 0, master_scale = c_scale_off;
   int m = m_scale_master_seq;
   if ( m >= 0 && is_active(m) && m_seqs[m]->get_playing()
        && m_seqs[m]->get_scale_master() ) {
       master_on    = true;
       master_key   = m_seqs[m]->get_master_key();
       master_scale = m_seqs[m]->get_master_scale();
   }
   ```

   These getters each take/release the master sequence's own `m_mutex`; they are
   short and there is no nested locking because we read them *before* entering the
   follower's `play()`/`lock()`.

2. Push the snapshot into each follower for this tick. Two clean options:

   - **(a) Plumb through play():** extend
     `sequence::play(long a_tick, bool a_playback_mode, bool master_on,
     int master_key, int master_scale)`. The follower stashes them in members read
     by `put_event_on_bus`. Touches the `play()` signature and the two call sites
     (perform.cpp:656,660).

   - **(b) Setter before play():** add
     `sequence::set_master_scale_context(bool on, int key, int scale)` that the
     follower stores in members (set under its own lock), called by `perform::play`
     just before `m_seqs[i]->play(...)`. Less intrusive to the `play()` signature.

   Either way the follower ends up with three plain-old-data members
   (`m_have_master`, `m_follow_key`, `m_follow_scale`) that `put_event_on_bus` reads
   while already holding the follower's own lock. Because they are written and read
   on the **same output thread**, there is no data race for the snap itself; the only
   cross-thread writers are the GUI thread setting the master's key/scale/flags,
   which is serialized by the master sequence's own `m_mutex` in the getters used in
   step 1.

This keeps the existing single-lock-per-object discipline (no nested locks, no new
global lock, no lock-order inversion).

---

## 3. Snap algorithm using the existing tables

### Table format (globals.h:204–243)

- `c_scales_policy[scale][pc]` (globals.h:204): `bool`, true if pitch-class `pc`
  (0..11, relative to **C**) is IN the scale.
- `c_scales_transpose_up[scale][pc]` (globals.h:218): how many semitones UP to reach
  the next higher in-scale note. **It is `0` for pitch-classes that ARE in the
  scale** — i.e. the value is the gap to the *next* scale degree, and the
  off-scale entries hold the distance up to the nearest in-scale note above. Used by
  `sequence::transpose_notes` (sequence.cpp:2999–3025) exactly this way.
- `c_scales_transpose_dn[scale][pc]` (globals.h:233): negative semitone offset DOWN
  to the nearest in-scale note below.

Note the tables are **C-relative**. The master may be rooted elsewhere, so we rotate
the incoming pitch by `-root` before indexing, then the result is already an absolute
MIDI note number because `transpose_*` deltas are root-agnostic (they only depend on
the pitch-class shape, which rotates with the root).

### Pseudocode

```cpp
// Returns a snapped absolute MIDI note (0..127). Non-destructive: caller copies.
int snap_to_scale( int note, int master_key /*0..11*/, int master_scale )
{
    if ( master_scale == c_scale_off )        // chromatic: nothing to snap
        return note;

    // pitch-class relative to the master root
    int pc = ( (note % 12) - master_key + 12 ) % 12;

    if ( c_scales_policy[master_scale][pc] )  // already in scale -> pass through
        return note;

    // distance to nearest in-scale note above and below (semitones)
    int up = c_scales_transpose_up[master_scale][pc];   // > 0 for off-scale pc
    int dn = c_scales_transpose_dn[master_scale][pc];   // < 0 for off-scale pc

    // choose nearest; tie -> snap down (musical convention, deterministic)
    int delta = ( up <= -dn ) ? up : dn;

    int snapped = note + delta;
    if ( snapped < 0 )   snapped += 12;       // clamp into MIDI range, keep pc
    if ( snapped > 127 ) snapped -= 12;
    return snapped;
}
```

Important detail re the `transpose_up`/`dn` semantics: for an **off-scale** pitch
class, `c_scales_transpose_up[scale][pc]` is the number of semitones up to the next
in-scale note (1 or 2 in these tables), and `c_scales_transpose_dn` is the negative
distance down. Both are non-zero for off-scale pcs (verify against the major/minor
rows at globals.h:222–227 / 239–241), so the nearest-neighbour comparison
`up <= -dn` is well defined. (For in-scale pcs `up` is 0 and we never reach the
comparison because of the `c_scales_policy` early return.)

Because `master_key` rotates the index, a C-table entry applied to a pitch already
expressed in absolute semitones gives the correct absolute snapped note for any
root — no separate re-rooting of the result is needed.

---

## 4. Edge cases

### 4.1 Note-off must snap to the SAME pitch as its note-on (stuck-note safety)

This is the critical correctness issue. A NOTE ON for pitch `p` is snapped to `p'`
and sent. The matching NOTE OFF is a *separate* `event` in `m_list_event` carrying
the original pitch `p`. If we recompute the snap for the note-off and the master
scale/root has changed (or the master stopped) between on and off, the off could
land on a different pitch `p'' != p'`, leaving `p'` sounding forever.

**PatchKnob already pairs on/off by reference count, not by linkage, at emit time:**
`put_event_on_bus` does `m_playing_notes[note]++` on note-on and `--` (with an
underflow `skip`) on note-off (sequence.cpp:2877–2892). `off_playing_notes()`
(sequence.cpp:2905) later flushes any note whose count is still > 0. So correctness
reduces to: **the index used to increment must equal the index used to decrement.**

Two consistent ways to do it; pick ONE and apply it uniformly:

- **Option A — snap both, key bookkeeping by the snapped note (recommended).**
  Compute `snapped = snap_to_scale(orig, key, scale)` for both note-on and note-off,
  and index `m_playing_notes[snapped]` (not `[orig]`). Send a temporary copy of the
  event whose note is `snapped`. To guarantee on/off agree even if the master
  changes mid-note, **latch the active note mapping**: maintain a small
  `int m_note_remap[c_midi_notes]` (or reuse a parallel array) that records, on
  note-on, `m_note_remap[orig] = snapped`; on note-off, look up
  `snapped = m_note_remap[orig]` and clear it. This makes the off always match the
  on regardless of intervening scale changes — the cleanest stuck-note guarantee.

- **Option B — snap on-the-fly both directions with no latch.** Simpler but only
  safe if the master scale/root is *stable* for the duration of a note. Not
  recommended because live scale changes are an explicit feature goal.

Either way: **do not mutate `*a_e`.** Build a local `event tmp = *a_e;
tmp.set_note(snapped);` and pass `&tmp` to `m_masterbus->play(...)`. `event` is a
value type (event.h:41) and `set_note` exists (event.h:104).

`off_playing_notes()` (sequence.cpp:2905) iterates `m_playing_notes[x] > 0` and emits
raw note-offs for pitch `x`. With Option A the counts are kept against snapped
indices, so these flush note-offs are automatically on the correct (snapped) pitch —
no extra work, no stuck notes when a follower is muted/stopped.

### 4.2 Master not playing / stops mid-loop

Resolved each tick in `perform::play` (§2 step 1): if the master index is `-1`, not
active, not flagged master, or `get_playing()` is false, `master_on = false` and
followers pass notes through unsnapped. If the master stops *between* a follower's
note-on and note-off, Option A's latch (4.1) still routes the off to the pitch that
was actually sounded, so no stuck note. New notes after the master stops play
unsnapped.

### 4.3 Multiple masters — disallowed

Enforced centrally in `perform::set_scale_master(n)` (§2): it clears the previous
master's `m_is_scale_master` before setting the new one and stores a single
`m_scale_master_seq`. The per-sequence `m_is_scale_master` flag is only ever set
through this perform method, so two simultaneous masters cannot exist.

### 4.4 Master following itself

In `perform::play`, when iterating to the master's own index, the master must not
snap to itself. Guard: a follower only snaps if `i != m_scale_master_seq` AND
`m_seqs[i]->get_follows_master()`. Equivalently, never set both `m_is_scale_master`
and the follow behaviour on one sequence; the index check is the robust guard.

### 4.5 Notes already in scale — pass through

Handled by the `c_scales_policy` early return in `snap_to_scale` (§3): in-scale
pitches return unchanged, so they pass through `put_event_on_bus` exactly as today.

### 4.6 Non-note events

`put_event_on_bus` also carries CC, pitch-wheel, etc. Snapping must apply ONLY when
`a_e->is_note_on() || a_e->is_note_off()` (event.h:136,137). Guard the snap with that
test; everything else passes through untouched.

---

## 5. Phased implementation plan

**Phase 1 — Data model (no behaviour change).**
- Add `m_is_scale_master`, `m_follows_master`, `m_master_key`, `m_master_scale` to
  `sequence` with getters/setters (sequence.h ~line 96–240, sequence.cpp accessors).
- Initialize them in the `sequence()` constructor (sequence.cpp:26).
- Add `m_scale_master_seq` (init `-1`) and `set/get_scale_master` to `perform`
  (perform.h ~74, perform.cpp init/`init()`), enforcing single-master.

**Phase 2 — Snap primitive.**
- Add free function or static helper `snap_to_scale(note, key, scale)` per §3
  (place near `transpose_notes`, sequence.cpp ~2984, or a small inline in a shared
  header). Unit-test against known cases (C major: C#→C, F#→G tie-down, etc.).

**Phase 3 — Plumb the master context into followers.**
- In `perform::play` (perform.cpp:639) resolve the master snapshot once per tick
  (§2 step 1) and deliver it to each follower (setter option (b) is least invasive:
  `set_master_scale_context(on,key,scale)` before each `m_seqs[i]->play(...)` at
  perform.cpp:656,660), guarding `i != m_scale_master_seq`.

**Phase 4 — Apply snap at emit with stuck-note safety.**
- In `sequence::put_event_on_bus` (sequence.cpp:2873), if this sequence follows the
  master and has context: gate on `is_note_on/off`, compute snapped note using the
  latch array (Option A, §4.1), update `m_playing_notes[snapped]`, and send a
  temporary copy `tmp` with `tmp.set_note(snapped)` to `m_masterbus->play` at
  sequence.cpp:2895. Leave `*a_e` untouched.
- Add the `m_note_remap[c_midi_notes]` latch member to `sequence` and clear it in the
  constructor and `zero_markers()` (sequence.cpp:398) / when playing stops.

**Phase 5 — UI.**
- Per-pattern "scale master" designation and "follow" toggle (and master root/scale
  pickers, reusing `c_key_text`/`c_scales_text`, globals.h:263,270) in the seqedit /
  main-window context menu. UI calls `perform::set_scale_master` and the per-seq
  setters. (Out of scope for the audio-correctness core; Phases 1–4 are the engine.)

---

## 6. Summary of the two critical points

- **Exact snap call-site:** inside `sequence::put_event_on_bus`
  (sequence.cpp:2873), immediately before `m_masterbus->play(m_bus, a_e,
  m_midi_channel)` at **sequence.cpp:2895**. Send a temporary copy with the snapped
  note; never mutate the stored `*a_e` (keeps it non-destructive). This is the single
  emit funnel reached from `sequence::play` (sequence.cpp:358) which is driven by
  `perform::play` (perform.cpp:660) on the output thread.

- **Stuck-note handling:** PatchKnob pairs note-on/off by a reference count
  `m_playing_notes[note]` in `put_event_on_bus` (sequence.cpp:2877–2892), and
  `off_playing_notes()` (sequence.cpp:2905) flushes any count left > 0. To stay
  consistent, snap the note-off to the **same** pitch as its note-on by latching the
  per-note mapping at note-on time (`m_note_remap[orig] = snapped`) and reusing it at
  note-off, and by indexing `m_playing_notes[]` with the *snapped* note. Then even if
  the master's scale/root changes or the master stops between the on and the off, the
  off (and any later flush) targets exactly the pitch that is actually sounding — no
  stuck notes.

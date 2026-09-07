# Transport loop and live MIDI bug report

Scope: PortAudio cycle playback, Seq24 trigger scheduling, fixed/modular MIDI
routing, live hardware MIDI, audio regions, automation regions, and editor
mutation visibility.

## Confirmed defects and corrections

1. Loop-head scheduling was latched to the lookahead horizon instead of the
   actual transport wrap. Short loops could therefore skip whole passes.
2. The latch was never reset when the lookahead exceeded an entire loop.
3. Loop-tail note-offs were emitted immediately while scheduling ahead, cutting
   sustained notes one or more buffers early.
4. Moving those releases to `right-1` cancelled legitimate last-tick note-ons.
5. Tick `right` is outside the half-open cycle and can never be rendered as a
   conventional queued event.
6. Loop releases and next-pass tick-zero note-ons lacked an explicit ordering
   contract.
7. The corrected boundary release path initially risked releasing live hardware
   notes as well as sequence-owned notes; ownership is now retained per track,
   channel, and pitch.
8. Sequence voice counters were reset before the audio engine had performed the
   corresponding boundary releases.
9. Boundary-generated note-offs were not reflected in the global held-note
   ledger, leaving phantom held state for later panic/routing operations.
10. Boundary release masks survived transport-loop disable and could affect a
    later loop session.
11. Boundary release masks survived live schedule invalidation and could refer
    to pre-edit notes.
12. Hardware MIDI used absolute sample timestamps during backward cycle jumps,
    despite comments claiming looped input was delivered immediately.
13. A hardware event captured near loop end could consequently remain future
    dated after wrap.
14. Repeated unreachable hardware events accumulated until the MIDI input
    backlog safety valve forced them out at the wrong time.
15. A stranded hardware note-off could leave a monitored input note held.
16. Already-queued sequencer lookahead was not invalidated by live note edits.
17. A note inserted inside the queued window could remain inaudible until the
    next cycle.
18. Editor and scheduler sequence cursors had no content-revision handshake.
19. A live edit could only be incorporated by a transport locate, which also
    killed sounding notes and was not suitable for normal editing.
20. MIDI and parameter messages from the old edit revision could coexist with
    newly scheduled data.
21. Loop markers changed in ticks while engine boundaries live in samples; the
    active tempo map is now the single conversion authority.
22. Audio blocks crossing loop end must be split; block-edge wrapping loses the
    last or first portion when the boundary is not buffer aligned.
23. Fixed and modular render paths must receive the same split transport window.
24. Audio regions use half-open `[start,end)` spans; treating trigger ends as
    inclusive duplicates or removes a boundary sample.
25. Automation regions require the same wrapped local-time rule as MIDI/audio
    regions rather than clamping at arranged length.
26. The scheduler inferred a wrap only when two successive sample-position
    polls observed a decrease. If the audio callback wrapped and advanced past
    the earlier position between polls, the wrap was invisible.
27. A missed wrap left the loop-head `tail_done` latch permanently armed on
    short/high-lookahead cycles, silencing all later passes until transport was
    stopped and restarted. Wrap detection now uses an audio-callback-owned,
    monotonic generation counter, so scheduler stalls cannot lose a boundary.

## Current contract

- The audio callback owns the exact sample-domain wrap.
- The scheduler may prepare tail and head data ahead of time, but cannot emit
  audible boundary releases early.
- At wrap: render tail, seek to loop start, emit sequence-owned note-offs, drain
  tick-zero note-ons, render head.
- Hardware MIDI is delivered in the current block while cycle playback is on;
  no wall-clock absolute timestamp is allowed to cross a backward wrap.
- Live edits bump a content revision, invalidate only queued future messages,
  rewind scheduling cursors to the audible position, and refill the horizon
  without seeking or panicking active voices.

## Acceptance checks

- Four complete one-bar passes at 500 BPM with 32nd-note events.
- Loop boundary deliberately not aligned to the 128-frame device block.
- Required result: zero misplaced event samples and no missing pass.
- Automation lane regression suite remains passing.

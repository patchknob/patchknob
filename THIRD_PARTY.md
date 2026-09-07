# Third-party components

PatchKnob is licensed under the **GNU General Public License, version 3 or
later** — see [`LICENSE`](LICENSE).

It vendors the libraries below in `vendor/`, so a build needs no manual
dependency hunting. Each keeps its own licence; the file named in the last
column is the authoritative text and ships with the source.

| Component | Purpose in PatchKnob | Licence | Licence file |
|---|---|---|---|
| Cardinal (DISTRHO) | modular rack / VCV-style bridge modules | **GPL-3.0** | `vendor/Cardinal/LICENSE` |
| Ardour (partial) | reference DSP + session concepts | **GPL-2.0** | `vendor/ardour/COPYING` |
| Csound | `Csound` patch nodes and the assistant's compile oracle | **LGPL-2.1** | `vendor/csound/COPYING` |
| CDP (cdp8) | CDP spectral/time-domain processes | **LGPL** | `vendor/cdp8/LICENSE` |
| libsndfile | audio file import/export | **LGPL-2.1** | `vendor/libsndfile/COPYING` |
| libpd | Pure Data patch nodes | BSD-3-Clause | `vendor/libpd/LICENSE.txt` |
| Pure Data | libpd's DSP core | BSD-3-Clause | `vendor/pure-data/LICENSE.txt` |
| SDL2 | windowing, input, audio backend | Zlib | `vendor/SDL2/LICENSE.txt` |
| SDL2_ttf | text rendering | Zlib | `vendor/SDL2_ttf/LICENSE.txt` |
| PortAudio | audio device backend | MIT | `vendor/portaudio/LICENSE.txt` |
| RtAudio | audio device backend | MIT-like | `vendor/rtaudio/LICENSE` |
| RtMidi | MIDI device I/O | MIT-like | `vendor/rtmidi/LICENSE` |
| litehtml | Csound manual rendering in the help drawer | BSD-3-Clause | `vendor/litehtml/LICENSE` |
| libpng | PNG decoding | PNG Reference Library Licence | `vendor/libpng/LICENSE` |
| zlib | compression | Zlib | `vendor/zlib/LICENSE` |
| Signalsmith Stretch | time-stretch / pitch-shift | see header | `third_party/signalsmith/` |
| Csound Reference Manual | the in-app manual and the opcode audit's source of truth | FDL / see upstream | `vendor/csound-manual/` |

## Built-in effects

`PatchKnob Compressor`, `PatchKnob Transient Shaper` and `PatchKnob Drum Bus`
are original PatchKnob code under GPL-3.0
(`src/engine/native/native_effects.cpp`). The compressor follows the standard
feed-forward design published in Giannoulis, Massberg & Reiss, *"Digital
Dynamic Range Compressor Design: A Tutorial and Analysis"* (JAES 60(6), 2012) —
a published method, implemented here from scratch.

An earlier compressor derived its DSP from an **AGPL-3.0** project. AGPL is
stricter than GPL and would have set the licence for all of PatchKnob, so that
code was replaced rather than carried. Nothing of it remains, and the reference
tree is deliberately excluded from this repository — redistributing it would
re-impose AGPL even though no code derives from it. Projects saved against the
old effect id still load: it is aliased onto the new compressor.

`Classic Master Limiter RE01` is adapted from AnClark's GPL-3.0 plug-in of the
same name and stays GPL-3.0.

## Why PatchKnob is GPL-3.0

Cardinal is GPL-3.0. PatchKnob links it, so the combined work is GPL-3.0 as
well — that is not a preference, it is the licence's requirement, and it is why
this project cannot be distributed under weaker terms while Cardinal is part of
it. The LGPL components (Csound, CDP, libsndfile) are compatible with that, and
the BSD/MIT/Zlib components impose only attribution.

## If you redistribute a build

The GPL asks you to pass on what you received. In practice:

- Ship or link the corresponding source for the version you distributed.
- Keep `LICENSE` and this file with it.
- Preserve the vendored licence files listed above; do not strip them.

## Not bundled

The VST2 and VST3 SDK headers are **not** included. VST is a Steinberg
trademark and its SDK carries its own licence, so plug-in hosting is built
against headers you supply. PatchKnob builds and runs without them; the VST
hosting targets are simply skipped.

Anthropic API access in the Csound assistant is optional, off by default, and
uses either your own API key or a local Claude Code installation. No key is
compiled into the binary, and none is included here.

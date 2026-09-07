#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  PatchKnob — verify the in-tree third-party sources are present.
#
#  IMPORTANT: this script does NOT fetch anything from upstream.
#
#  The trees under vendor/ are FORKS.  They carry substantial PatchKnob changes
#  (hundreds of modified files in Cardinal, Pure Data, libpd, RtMidi and
#  RtAudio), so re-cloning them from upstream would silently destroy that work.
#  An earlier version of this script did exactly that, pinned by commit, on the
#  assumption they were pristine checkouts.  They are not.  Everything needed
#  is committed to this repository; there is nothing to download.
#
#  All this does now is tell you early and clearly if a checkout is incomplete,
#  instead of letting CMake fail a hundred lines into a build.
#-----------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")/.."

missing=0
need() {   # path, what it is for
    if [ -e "$1" ]; then
        printf "  ok       %s\n" "$1"
    else
        printf "  MISSING  %-46s (%s)\n" "$1" "$2"
        missing=1
    fi
}

echo "==> in-tree third-party sources"
need vendor/csound                       "Csound patch nodes + the assistant's compile oracle"
need vendor/csound-manual                "in-app manual + the opcode audit's source of truth"
need vendor/cdp8                         "CDP processes"
need vendor/libpd                        "Pure Data patch nodes"
need vendor/pure-data                    "libpd's DSP core"
need vendor/libsndfile                   "audio file import/export"
need vendor/litehtml                     "help-drawer HTML rendering"
need vendor/rtaudio                      "audio device backend"
need vendor/rtmidi                       "MIDI device I/O"
need vendor/portaudio                    "audio device backend"
need vendor/zlib                         "compression"
need vendor/libpng                       "PNG decoding"
need vendor/Cardinal/plugins/Fundamental "rack modules"
need vendor/Cardinal/plugins/GoodSheperd "rack modules"
need vendor/Cardinal/src/Rack/include    "rack headers"
need vendor/Cardinal/src/Rack/dep/simde  "SIMD compatibility headers"
need third_party/signalsmith             "time-stretch / pitch-shift"

if [ "$missing" != "0" ]; then
    cat <<MSG

Some third-party sources are missing.  They are committed to this repository,
so this usually means a partial clone or a checkout interrupted part-way.
Try:  git checkout -- . && git submodule update --init --recursive
MSG
    exit 1
fi
echo "==> all present"

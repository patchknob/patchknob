#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT/build-linux"}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
PORTAUDIO_DIR="${PORTAUDIO_DIR:-"$ROOT/vendor/portaudio"}"

usage() {
  cat <<'EOF'
Usage: tools/build_linux.sh [--clean] [--debug]

Environment:
  BUILD_DIR       Build directory, default: ./build-linux
  BUILD_TYPE      CMake build type, default: Release
  JOBS            Parallel build jobs, default: nproc
  PORTAUDIO_DIR   PortAudio source directory, default: ./vendor/portaudio

Ubuntu/Debian packages:
  sudo apt install build-essential cmake pkg-config python3 libsdl3-dev \
    libsdl3-ttf-dev libpng-dev librsvg2-dev libcairo2-dev libfontconfig1-dev \
    libasound2-dev libjack-jackd2-dev libpulse-dev

Fedora packages:
  sudo dnf install gcc-c++ cmake pkgconf-pkg-config python3 SDL3-devel \
    SDL3_ttf-devel libpng-devel librsvg2-devel cairo-devel fontconfig-devel \
    alsa-lib-devel jack-audio-connection-kit-devel pulseaudio-libs-devel

Arch/Omarchy packages:
  sudo pacman -S --needed base-devel cmake pkgconf python sdl3 sdl3_ttf \
    libpng librsvg cairo fontconfig alsa-lib jack2 libpulse

(python3 generates the Fundamental rack modules' DSP source on non-Windows;
Csound and libsndfile build from vendored source automatically -- no extra
packages needed for those. VST2/VST3 hosting needs nothing beyond the above:
both load native Linux .so/.vst3 plugins the same way the Windows build
loads .dll/.vst3 ones.)

Pass -DPATCHKNOB_SDL3=OFF to cmake (or set PATCHKNOB_SDL3=OFF and pass it
through CMAKE_ARGS) to build against SDL2/SDL2_ttf instead.

Running under KMS/DRM (no X11/Wayland session) needs an SDL3 built with its
kmsdrm backend (most distro packages already have it -- verify with
SDL_VIDEODRIVER=kmsdrm ./build-linux/PatchKnob) and either 'video' group
membership or a seatd/logind session; see the README for details.
EOF
}

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    --debug) BUILD_TYPE=Debug ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $arg" >&2; usage; exit 2 ;;
  esac
done

if [[ "$CLEAN" == "1" ]]; then
  rm -rf "$BUILD_DIR"
fi

if [[ ! -f "$PORTAUDIO_DIR/build/libportaudio.a" ]]; then
  if [[ ! -d "$PORTAUDIO_DIR" ]]; then
    echo "PortAudio source not found at $PORTAUDIO_DIR" >&2
    echo "Set PORTAUDIO_DIR or place PortAudio under vendor/portaudio." >&2
    exit 1
  fi
  cmake -S "$PORTAUDIO_DIR" -B "$PORTAUDIO_DIR/build" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_SHARED_LIBS=OFF \
    -DPA_BUILD_EXAMPLES=OFF \
    -DPA_BUILD_TESTS=OFF \
    -DPA_USE_SNDIO=OFF
  # PA_USE_SNDIO=OFF: PortAudio auto-enables its sndio backend whenever
  # libsndio's headers happen to be installed (pulled in as some other
  # package's dependency, not because anything here uses it), but its
  # CMake never propagates the resulting -lsndio to static-lib consumers --
  # so an opportunistic auto-detect turns into a link failure in THIS
  # binary. ALSA/PulseAudio/JACK (the backends this app actually documents)
  # are unaffected.
  cmake --build "$PORTAUDIO_DIR/build" --parallel "$JOBS"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPORTAUDIO_DIR="$PORTAUDIO_DIR"

cmake --build "$BUILD_DIR" --parallel "$JOBS"

cat <<EOF

PatchKnob Linux build complete:
  $BUILD_DIR/PatchKnob

Run:
  "$BUILD_DIR/PatchKnob"
EOF

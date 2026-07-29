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
  sudo apt install build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev \
    libpng-dev librsvg2-dev libcairo2-dev libasound2-dev libjack-jackd2-dev \
    libpulse-dev

Fedora packages:
  sudo dnf install gcc-c++ cmake pkgconf-pkg-config SDL2-devel SDL2_ttf-devel \
    libpng-devel librsvg2-devel cairo-devel alsa-lib-devel jack-audio-connection-kit-devel \
    pulseaudio-libs-devel
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
    -DPA_BUILD_TESTS=OFF
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

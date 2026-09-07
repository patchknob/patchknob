#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  Build a .deb for PatchKnob.
#
#  Deliberately does NOT require dpkg-dev: a .deb is just an `ar` archive of
#  debian-binary, control.tar.gz and data.tar.xz, so this builds one with tools
#  present on any Linux.  That means the package can be produced from a
#  non-Debian machine, which is the normal case here.
#
#  Usage:  ./packaging/build-deb.sh [--build-dir DIR] [--out DIR]
#-----------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

BUILD_DIR="build-linux"
OUT="dist"
VERSION="$(sed -n 's/.*project(PatchKnob_root VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "$VERSION" ] || VERSION="0.0.0"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out)       OUT="$2"; shift 2 ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  DEB_ARCH=amd64 ;;
    aarch64) DEB_ARCH=arm64 ;;
    armv7l)  DEB_ARCH=armhf ;;
    *)       DEB_ARCH="$ARCH" ;;
esac

[ -x "$BUILD_DIR/PatchKnob" ] || { echo "error: $BUILD_DIR/PatchKnob not built.  Run ./scripts/build.sh"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "==> staging"
DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null

mkdir -p "$STAGE/DEBIAN"
INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

#  Runtime dependencies.  Everything else PatchKnob carries itself; these are
#  the shared libraries it links against on a normal desktop.
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: patchknob
Version: $VERSION
Section: sound
Priority: optional
Architecture: $DEB_ARCH
Maintainer: PatchKnob <https://patchknob.com>
Installed-Size: $INSTALLED_KB
Depends: libc6, libstdc++6, libasound2, libsdl2-2.0-0, libsdl2-ttf-2.0-0, libx11-6, libgl1, libcurl4, libpng16-16, libfreetype6, libfontconfig1
Recommends: jackd2
Homepage: https://patchknob.com
Description: Tracker, timeline and modular synthesis DAW
 PatchKnob is a digital audio workstation built around tracker, piano-roll and
 timeline sequencing, a sampler with keyzones and per-sample editing, VST2/VST3
 hosting, a modular rack, and Pure Data and Csound patch nodes.
 .
 It ships an in-editor Csound assistant that validates generated instruments
 against the Csound manual and compiles them before offering them.
CTRL

echo "==> building package"
mkdir -p "$OUT"
( cd "$STAGE" && tar --owner=root --group=root -czf "$STAGE/control.tar.gz" -C "$STAGE/DEBIAN" . )
( cd "$STAGE" && tar --owner=root --group=root -cJf "$STAGE/data.tar.xz" --exclude=./DEBIAN ./usr )
echo "2.0" > "$STAGE/debian-binary"

DEB="$ROOT/$OUT/patchknob_${VERSION}_${DEB_ARCH}.deb"
rm -f "$DEB"
( cd "$STAGE" && ar rc "$DEB" debian-binary control.tar.gz data.tar.xz )

echo "Built: $DEB"
ls -lh "$DEB" | awk '{print "  size:", $5}'

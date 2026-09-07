#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  Build an Arch package (.pkg.tar.zst) for PatchKnob.
#
#  Built directly from the staged install rather than through makepkg.  makepkg
#  wants to copy the whole source tree into its own build root, and this tree is
#  several gigabytes of vendored source -- the copy alone can exhaust a disk
#  quota, and it recompiles what is already built.  An Arch package is a
#  zstd-compressed tar carrying .PKGINFO and .MTREE, so it is assembled here
#  from the same `cmake --install` staging the .deb uses.  One source of truth
#  for what lands on disk, two container formats.
#
#  Usage:  ./packaging/build-arch.sh [--build-dir DIR] [--out DIR]
#-----------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

BUILD_DIR="build-linux"
OUT="dist"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out)       OUT="$2"; shift 2 ;;
        *) echo "unknown option: $1"; exit 1 ;;
    esac
done

VERSION="$(sed -n 's/.*project(PatchKnob_root VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "$VERSION" ] || VERSION="0.0.0"
PKGREL=1
ARCH="$(uname -m)"

[ -x "$BUILD_DIR/PatchKnob" ] || { echo "error: $BUILD_DIR/PatchKnob not built.  Run ./scripts/build.sh"; exit 1; }
command -v bsdtar >/dev/null 2>&1 || { echo "error: bsdtar (libarchive) is required"; exit 1; }

STAGE="$ROOT/$OUT/.arch-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
trap 'rm -rf "$STAGE"' EXIT

echo "==> staging"
DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null

SIZE="$(du -sb "$STAGE" | cut -f1)"
BUILDDATE="$(date +%s)"

cat > "$STAGE/.PKGINFO" <<INFO
pkgname = patchknob
pkgbase = patchknob
pkgver = ${VERSION}-${PKGREL}
pkgdesc = Tracker, timeline and modular synthesis DAW with VST hosting, Csound and Pure Data nodes
url = https://patchknob.com
builddate = ${BUILDDATE}
packager = PatchKnob <https://patchknob.com>
size = ${SIZE}
arch = ${ARCH}
license = GPL3
depend = alsa-lib
depend = sdl2
depend = sdl2_ttf
depend = libx11
depend = libgl
depend = curl
depend = libpng
depend = freetype2
depend = fontconfig
optdepend = jack2: JACK audio backend
optdepend = pipewire-jack: JACK via PipeWire
INFO

echo "==> building package"
mkdir -p "$ROOT/$OUT"
PKG="$ROOT/$OUT/patchknob-${VERSION}-${PKGREL}-${ARCH}.pkg.tar.zst"
rm -f "$PKG"

#  .MTREE is pacman's file manifest; generate it over the payload, then pack
#  metadata first so pacman can read it without decompressing everything.
( cd "$STAGE" && bsdtar -czf .MTREE --format=mtree \
    --options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
    .PKGINFO usr )

( cd "$STAGE" && bsdtar --zstd -cf "$PKG" .PKGINFO .MTREE usr )

echo
echo "Built: $PKG"
ls -lh "$PKG" | awk '{print "  size:", $5}'

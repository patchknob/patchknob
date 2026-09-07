#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  PatchKnob — install the system packages the build needs.
#
#  Detects the distro from /etc/os-release and maps to that family's package
#  names.  Run it once; it is idempotent.  If your distro is not listed the
#  script prints the generic dependency list so you can translate it yourself
#  rather than failing silently.
#
#  Usage:  ./scripts/install-deps.sh            # install
#          ./scripts/install-deps.sh --dry-run  # just print the command
#-----------------------------------------------------------------------------
set -euo pipefail

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

#  What PatchKnob actually needs, in plain terms.  Everything else it carries
#  in vendor/ and builds itself.
#    build tools : cmake, a C++17 compiler, make/ninja, pkg-config, git
#    audio       : ALSA (required), JACK (optional), PulseAudio (optional)
#    ui          : SDL2 (or SDL3) + SDL_ttf, X11, OpenGL, fontconfig, freetype
#    graphics    : libpng, cairo + librsvg (Cardinal's SVG panels)
#    net         : libcurl (the assistant's only network dependency)
#    csound      : bison + flex (Csound's parser is generated at build time)
GENERIC="cmake g++ make ninja pkg-config git alsa-lib jack sdl2 sdl2_ttf \
libx11 mesa/opengl fontconfig freetype libpng cairo librsvg curl bison flex libsndfile"

detect() {
    if [ -r /etc/os-release ]; then . /etc/os-release; echo "${ID_LIKE:-$ID}"; else echo unknown; fi
}
FAMILY="$(detect)"

run() {
    echo "==> $*"
    [ "$DRY" = "1" ] && return 0
    if [ "$(id -u)" = "0" ]; then "$@"; else sudo "$@"; fi
}

case " $FAMILY " in
  *debian*|*ubuntu*)
      run apt-get update
      run apt-get install -y \
          build-essential cmake ninja-build pkg-config git \
          libasound2-dev libjack-jackd2-dev libpulse-dev \
          libsdl2-dev libsdl2-ttf-dev \
          libx11-dev libgl1-mesa-dev libglu1-dev \
          libfontconfig1-dev libfreetype6-dev \
          libpng-dev libcairo2-dev librsvg2-dev \
          libcurl4-openssl-dev bison flex libsndfile1-dev zlib1g-dev
      ;;
  *fedora*|*rhel*|*centos*)
      run dnf install -y \
          gcc-c++ cmake ninja-build pkgconf-pkg-config git \
          alsa-lib-devel jack-audio-connection-kit-devel pulseaudio-libs-devel \
          SDL2-devel SDL2_ttf-devel \
          libX11-devel mesa-libGL-devel mesa-libGLU-devel \
          fontconfig-devel freetype-devel \
          libpng-devel cairo-devel librsvg2-devel \
          libcurl-devel bison flex libsndfile-devel zlib-devel
      ;;
  *arch*)
      #  --needed makes it idempotent; Arch ships headers with the library.
      run pacman -S --needed --noconfirm \
          base-devel cmake ninja pkgconf git \
          alsa-lib jack2 libpulse \
          sdl2 sdl2_ttf \
          libx11 mesa glu \
          fontconfig freetype2 \
          libpng cairo librsvg \
          curl bison flex libsndfile zlib
      ;;
  *suse*|*opensuse*)
      run zypper install -y \
          gcc-c++ cmake ninja pkg-config git \
          alsa-devel libjack-devel libpulse-devel \
          libSDL2-devel libSDL2_ttf-devel \
          libX11-devel Mesa-libGL-devel glu-devel \
          fontconfig-devel freetype2-devel \
          libpng16-devel cairo-devel librsvg-devel \
          libcurl-devel bison flex libsndfile-devel zlib-devel
      ;;
  *alpine*)
      run apk add --no-cache \
          build-base cmake samurai pkgconf git \
          alsa-lib-dev jack-dev pulseaudio-dev \
          sdl2-dev sdl2_ttf-dev \
          libx11-dev mesa-dev glu-dev \
          fontconfig-dev freetype-dev \
          libpng-dev cairo-dev librsvg-dev \
          curl-dev bison flex libsndfile-dev zlib-dev
      ;;
  *)
      cat <<MSG
Could not recognise this distribution (ID_LIKE/ID = "$FAMILY").

Install the equivalent of these, then run ./scripts/build.sh:

    $GENERIC

Everything else PatchKnob builds from vendor/ itself.
MSG
      exit 1
      ;;
esac

echo
echo "Dependencies installed.  Next:  ./scripts/build.sh"

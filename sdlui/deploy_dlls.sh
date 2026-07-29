#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# deploy_dlls.sh <path-to-PatchKnob.exe>
#
# Copy the FULL transitive mingw64 DLL closure of the executable (and the
# probe_vst2/3 helpers next to it) into the executable's directory, so the app
# runs SELF-CONTAINED -- no need for /mingw64/bin on PATH.  Without this, a
# detached launch (double-click / Start-Process / shipped build) loads whatever
# stale SDL2 / libstdc++ / libwinpthread it finds elsewhere on PATH; a version
# missing an export (e.g. clock_gettime) aborts the process with
# STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139).  The exe directory has highest DLL
# search priority, so bundling the correct versions here always wins.
#
# Invoked from CMake POST_BUILD; also runnable by hand.
#-----------------------------------------------------------------------------
set -u
exe="${1:?usage: deploy_dlls.sh <exe>}"
if command -v cygpath >/dev/null 2>&1; then
  exe="$(cygpath -u "$exe")"
fi
dir="$(dirname "$exe")"
export PATH="/c/msys64/usr/bin:/c/msys64/mingw64/bin:$PATH"

n=0
{
  for e in "$exe" "$dir/probe_vst2.exe" "$dir/probe_vst3.exe"; do
    [ -f "$e" ] && ldd "$e" 2>/dev/null | awk '/mingw64/ {print $3}'
  done
} | sort -u | while read -r dll; do
  if [ -f "$dll" ]; then cp -u "$dll" "$dir/" && n=$((n+1)); fi
done

echo "deploy_dlls: bundled mingw64 DLL closure into $dir"
exit 0

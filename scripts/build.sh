#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  PatchKnob — configure and build.
#
#  Usage:
#      ./scripts/build.sh                 # release build into build-linux/
#      ./scripts/build.sh --debug         # debug build
#      ./scripts/build.sh --jobs 4        # limit parallelism (low-RAM machines)
#      ./scripts/build.sh --clean         # wipe the build dir first
#      ./scripts/build.sh --tests         # also build and run the test suites
#
#  The binary lands at build-linux/PatchKnob.
#-----------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

BUILD_DIR="build-linux"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
TESTS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)  BUILD_TYPE="Debug"; shift ;;
        --clean)  CLEAN=1; shift ;;
        --tests)  TESTS=1; shift ;;
        --jobs)   JOBS="$2"; shift 2 ;;
        --dir)    BUILD_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)"; exit 1 ;;
    esac
done

#  A parallel C++ build of this size is memory-bound, not CPU-bound: each
#  compiler process can take ~1 GB on the heavier translation units.  Cap the
#  job count so a machine with plenty of cores and modest RAM does not start
#  swapping or get processes OOM-killed halfway through.
if command -v free >/dev/null 2>&1; then
    MEM_GB=$(( $(free -m | awk '/^Mem:/{print $2}') / 1024 ))
    MAX_BY_MEM=$(( MEM_GB > 1 ? MEM_GB : 1 ))
    if [ "$JOBS" -gt "$MAX_BY_MEM" ]; then
        echo "note: limiting to $MAX_BY_MEM jobs (${MEM_GB} GB RAM); override with --jobs"
        JOBS="$MAX_BY_MEM"
    fi
fi

for tool in cmake git pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: '$tool' not found.  Run ./scripts/install-deps.sh first."; exit 1; }
done

#  Vendored sources the build compiles into the binary.  bootstrap-vendor.sh
#  fetches any that are missing, so a fresh clone needs no manual dependency
#  hunting -- that is the whole point of vendoring them.
if [ -x scripts/bootstrap-vendor.sh ]; then
    ./scripts/bootstrap-vendor.sh
fi

[ "$CLEAN" = "1" ] && rm -rf "$BUILD_DIR"

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

echo "==> configuring ($BUILD_TYPE) in $BUILD_DIR"
# shellcheck disable=SC2086
cmake -S . -B "$BUILD_DIR" $GEN -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "==> building with $JOBS job(s)"
cmake --build "$BUILD_DIR" --target PatchKnob -j "$JOBS"

echo
echo "Built: $ROOT/$BUILD_DIR/PatchKnob"

if [ "$TESTS" = "1" ]; then
    echo
    echo "==> test suites"
    fail=0
    run_suite() {   # name, source dir, extra cmake args, binary, run args
        local name="$1" src="$2" args="$3" bin="$4" runargs="${5:-}"
        local dir="$BUILD_DIR/tests/$name"
        # shellcheck disable=SC2086
        cmake -S "$src" -B "$dir" -DCMAKE_BUILD_TYPE=Release $args >/dev/null 2>&1 || {
            echo "  SKIP $name (configure failed)"; return 0; }
        cmake --build "$dir" -j "$JOBS" >/dev/null 2>&1 || {
            echo "  SKIP $name (build failed)"; return 0; }
        local exe; exe="$(find "$dir" -maxdepth 1 -type f -name "$bin" | head -1)"
        [ -n "$exe" ] || { echo "  SKIP $name (no binary)"; return 0; }
        # shellcheck disable=SC2086
        if SDL_VIDEODRIVER=dummy "$exe" $runargs >/dev/null 2>&1; then
            echo "  PASS $name"
        else
            echo "  FAIL $name"; fail=1
        fi
    }
    run_suite arrange   sdlui/views/arrange        ""                              arrange_view_test --selftest
    run_suite sampler   src/engine/sampler         "-DPATCHKNOB_SAMPLER_TESTS=ON"  PatchKnob_sampler_test
    run_suite sf2       src/engine/sf2             "-DPATCHKNOB_SF2_TESTS=ON"      PatchKnob_sf2_reader_test
    run_suite ai        src/engine/ai              "-DPATCHKNOB_AI_TESTS=ON"       ai_test
    run_suite regions   src/engine/audioclip/region_harness ""                     region_harness_test
    [ "$fail" = "0" ] && echo "All suites passed." || { echo "Some suites FAILED."; exit 1; }
fi

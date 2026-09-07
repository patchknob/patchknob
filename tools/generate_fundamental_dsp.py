#!/usr/bin/env python3
"""
Cross-platform replacement for generate_fundamental_dsp.ps1.

Extracts the DSP-only portion of each vendored VCV Rack "Fundamental" module
source (stripping its widget/UI code) and emits one merged translation unit
of rackx::fundamental::make*() factories -- see generate_fundamental_dsp.ps1
for the original PowerShell implementation this mirrors line-for-line.
PowerShell is Windows-only in practice (an AUR/third-party package on every
Linux distro), so the Linux/macOS build path uses this instead; both are
kept so the Windows CMake path is unchanged.
"""
import argparse
import os
import re
import sys

SOURCES = [
    "VCO.cpp", "VCF.cpp", "VCA-1.cpp", "ADSR.cpp", "LFO.cpp", "Mixer.cpp",
    "8vert.cpp", "Merge.cpp", "MidSide.cpp", "Octave.cpp", "Split.cpp", "Sum.cpp",
    "VCA.cpp", "VCMixer.cpp", "Mutes.cpp", "Pulses.cpp", "Random.cpp", "SEQ3.cpp",
    "SequentialSwitch.cpp",
]

FACTORIES = [
    ("VCO", "makeVCO"), ("VCF", "makeVCF"), ("VCA_1", "makeVCA1"),
    ("ADSR", "makeADSR"), ("LFO", "makeLFO"), ("Mixer", "makeMixer"),
    ("_8vert", "make8vert"), ("Merge", "makeMerge"), ("MidSide", "makeMidSide"),
    ("Octave", "makeOctave"), ("Split", "makeSplit"), ("Sum", "makeSum"),
    ("VCA", "makeVCA2"), ("VCMixer", "makeVCMixer"), ("Mutes", "makeMutes"),
    ("Pulses", "makePulses"), ("Random", "makeRandom"), ("SEQ3", "makeSEQ3"),
    ("SequentialSwitch<1, 4>", "makeSequentialSwitch1"),
    ("SequentialSwitch<4, 1>", "makeSequentialSwitch2"),
]

BOUNDARY = re.compile(
    r"^struct\s+[A-Za-z_][A-Za-z0-9_]*(Widget|Display|Knob|Port|Button|Meter)\b",
    re.MULTILINE)
BOUNDARY_SEQSWITCH = re.compile(r"^struct\s+LightButtonTriSwitch\b", re.MULTILINE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--stable16-source", default="")
    args = ap.parse_args()

    entries = [(name, os.path.join(args.source_root, name)) for name in SOURCES]
    factories = list(FACTORIES)
    if args.stable16_source:
        entries.append(("Stable16.cpp", args.stable16_source))
        factories.append(("Stable16", "makeStable16"))

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as out:
        out.write('#include "fundamental_bridge.h"\n')
        out.write('#include "rack_sdk_compat.hpp"\n')
        out.write("#include <memory>\n")
        out.write("using namespace rack;\n")
        for name, path in entries:
            with open(path, "r", encoding="utf-8") as f:
                content = f.read()
            content = re.sub(r"^#include[^\r\n]*\r?\n?", "", content, flags=re.MULTILINE)
            content = re.sub(
                r"(ParamQuantity\*\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*)paramQuantities\[([^\]]+)\]",
                r"\1&paramQuantities[\2]", content)
            content = re.sub(r"paramQuantities\[([^\]]+)\]->", r"paramQuantities[\1].", content)
            content = re.sub(r"^\s*DEBUG\([^\r\n]*\);\s*\r?\n?", "", content, flags=re.MULTILINE)
            boundary = BOUNDARY_SEQSWITCH if name == "SequentialSwitch.cpp" else BOUNDARY
            m = boundary.search(content)
            if not m:
                sys.exit(f"No DSP/UI boundary found in {name}")
            out.write(content[:m.start()])
        out.write("namespace rackx { namespace fundamental {\n")
        for type_name, fn in factories:
            out.write(f"std::unique_ptr<rack::engine::Module> {fn}() "
                      f"{{ return std::make_unique<{type_name}>(); }}\n")
        out.write("} }\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# How much of the safety function the Unit Design Document actually describes.
#
#   tools/udd-coverage.py <supervisor dir> <doxygen output dir>
#
# Called by tools/udd.sh, which is where the document is built.
#
# WHY THIS EXISTS. doxygen says nothing about a file that carries no
# documentation comments at all: with EXTRACT_ALL off it simply leaves it out.
# So a clean run proves the comments that exist are well formed, and proves
# nothing whatever about the ones that do not. "0 warnings" on a document
# describing a tenth of the component is exactly the vacuous pass this project
# refuses for coverage, for MC/DC and for the target static analysis, and the
# refusal should not stop at documentation.
#
# The declarations are counted from the HEADERS rather than from doxygen's own
# output, deliberately: a unit that is missing from the document has to be
# missing from a list built independently of the document, or it is invisible
# twice.
import glob
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# A public function or a public constant, which between them are what a unit
# design has to cover: what can be called, and the thresholds it decides with.
DECL = re.compile(r"^(?:constexpr\s+\w+\s+(\w+)\s*=|"
                  r"(?:void|bool|uint\d+_t|int\d+_t|size_t)\s+(\w+)\s*\()")


def documented_names(out_dir):
    """Every member doxygen produced a non-empty brief description for."""
    names = set()
    for f in glob.glob(str(pathlib.Path(out_dir) / "xml" / "*.xml")):
        try:
            root = ET.parse(f).getroot()
        except ET.ParseError:
            # doxygen emits one index file this parser will not take; the
            # member definitions are in the per-file documents either way.
            continue
        for m in root.iter("memberdef"):
            brief = m.find("briefdescription")
            if brief is not None and "".join(brief.itertext()).strip():
                names.add(m.findtext("name"))
    return names


def declared(sup_dir):
    """Public functions and constants, per header, read from the source."""
    sup = pathlib.Path(sup_dir)
    headers = sorted(sup.glob("core/include/sup/*.h"))
    headers.append(sup / "protocol/include/sup_proto.h")
    per_file = {}
    for h in headers:
        if not h.is_file():
            continue
        names = set()
        for line in h.read_text().splitlines():
            m = DECL.match(line.strip())
            if m:
                names.add(m.group(1) or m.group(2))
        per_file[h.relative_to(sup)] = names
    return per_file


def main(argv):
    if len(argv) != 3:
        print("usage: udd-coverage.py <supervisor dir> <doxygen output dir>",
              file=sys.stderr)
        return 2

    have = documented_names(argv[2])
    per_file = declared(argv[1])
    public = set().union(*per_file.values()) if per_file else set()
    missing = sorted(public - have)

    print()
    print(f"Unit design coverage: {len(public) - len(missing)} of {len(public)} "
          f"public functions and constants in core/ and protocol/")
    for h, names in per_file.items():
        gap = sorted(names - have)
        state = "complete" if not gap else f"{len(names) - len(gap)} of {len(names)}"
        print(f"  {str(h):<34} {state}")
    if missing:
        print()
        print("  Not in the document yet: " + ", ".join(missing[:12])
              + (" ..." if len(missing) > 12 else ""))
        print("  Tasklist R13.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

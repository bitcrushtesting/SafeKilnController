#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generate the supervisor's Unit Design Document (V-model level 5).
#
#   tools/udd.sh            build it, fail on any documentation warning
#   tools/udd.sh --open     build it and open it in a browser
#   tools/udd.sh --lenient  build it and report warnings without failing
#
# The document is firmware/supervisor/Doxyfile's output and the reasoning for
# generating rather than writing it is in that file's header.
#
# WARNINGS ARE THE GATE, and that is the whole point of running this in CI: an
# undocumented unit in the safety function is a hole in the unit design, and
# WARN_AS_ERROR turns it into a failed build rather than a document with a gap
# in it that nobody reads to the end of.  --lenient exists for working on the
# documentation, not for CI.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here/.."
sup="$root/firmware/supervisor"
out="$sup/build-udd"

lenient=0
open_after=0
for arg in "$@"; do
    case "$arg" in
    --lenient) lenient=1 ;;
    --open)    open_after=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# Named in the output because the version matters and has bitten: Ubuntu's
# doxygen 1.9 treats a bare \dot in prose as the start of a dot block where
# 1.18 does not, so the same sources passed here and failed in CI. A log that
# does not say which tool produced it cannot show that.
if command -v doxygen >/dev/null 2>&1; then
    echo "doxygen:  $(doxygen --version)"
fi

if ! command -v doxygen >/dev/null 2>&1; then
    echo "doxygen is not installed." >&2
    echo "  macOS:  brew install doxygen" >&2
    echo "  Debian: apt-get install doxygen" >&2
    exit 1
fi

# Graphviz is REQUIRED, and it used to be optional. The unit design now carries
# \dot blocks of its own: the trip logic's state machine and the cycle's
# program-sequence graph, which are the two pictures an assessment of a safety
# function actually asks for. Without dot those are not missing decoration,
# they are missing content, and doxygen says so loudly enough to fail the
# build. Better to say it here, in one sentence, than as a documentation
# warning somebody has to interpret.
if ! command -v dot >/dev/null 2>&1; then
    echo "graphviz is not installed, and the unit design needs it: the state" >&2
    echo "machine and the program-sequence diagram are \dot blocks in the" >&2
    echo "headers." >&2
    echo "  macOS:  brew install graphviz" >&2
    echo "  Debian: apt-get install graphviz" >&2
    exit 1
fi
dot_flag="YES"

cd "$sup"
rm -rf "$out"
mkdir -p "$out"

# The overrides are appended to the configuration rather than edited into it,
# which is doxygen's documented way to do this and keeps the Doxyfile the one
# statement of what the document is.
{
    cat Doxyfile
    echo "HAVE_DOT = $dot_flag"
    if [ "$dot_flag" = "YES" ]; then
        echo "CALL_GRAPH = YES"
        echo "CALLER_GRAPH = YES"
        echo "INCLUDE_GRAPH = YES"
    fi
    if [ "$lenient" = "1" ]; then
        echo "WARN_AS_ERROR = NO"
    fi
} | doxygen - || status=$?
status=${status:-0}

warnings="$out/doxygen-warnings.txt"
if [ -s "$warnings" ]; then
    echo
    echo "--- documentation warnings ---"
    cat "$warnings"
    echo "--- $(grep -c . "$warnings") line(s) ---"
fi

if [ "$status" -ne 0 ] && [ "$lenient" = "0" ]; then
    echo >&2
    echo "The unit design is incomplete: every warning above is a unit, a" >&2
    echo "parameter or a return value with nothing said about it. Document it" >&2
    echo "or, if it genuinely belongs outside the document, exclude it in the" >&2
    echo "Doxyfile with the reason written next to it." >&2
    exit "$status"
fi

# --- coverage, because zero warnings is not the same as a complete document --
#
# doxygen is silent about a file that carries no documentation comments at all:
# with EXTRACT_ALL off it simply omits it, so "0 warnings" can mean "nothing is
# documented" as easily as "everything is".  That is the vacuous pass this
# project refuses elsewhere, so the number is computed and printed every time.
if [ "$lenient" = "1" ]; then
    python3 "$here/udd-coverage.py" "$sup" "$out"
else
    python3 "$here/udd-coverage.py" --require-complete "$sup" "$out"
fi

echo
echo "Unit Design Document: $out/html/index.html"
[ -d "$out/xml" ] && echo "Machine-readable:     $out/xml/"

if [ "$open_after" = "1" ]; then
    if command -v open >/dev/null 2>&1; then open "$out/html/index.html"
    elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$out/html/index.html"
    fi
fi

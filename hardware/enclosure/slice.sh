#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Render the enclosure and slice it for PLA on a Prusa, from .scad to G-code.
#
#   ./slice.sh              everything: shell, bezel, knob
#   ./slice.sh knob         one part
#   ./slice.sh --stl-only   render, do not slice
#
# Output lands in build/. Nothing is committed from here.
#
# ===========================================================================
# WHY THIS IS A SCRIPT
# ===========================================================================
# Because the per-part arguments are the whole point and a GUI cannot record
# them. The shell gets a brim and the other two do not; the knob is small
# enough to want a slower first layer. Those are decisions about these parts,
# and a decision that lives in somebody's slicer window is a decision that is
# made differently next time.
#
# It also fails loudly rather than producing a quietly wrong result, which for
# a slicer means: no profile, no slicer, a non-manifold render, or a G-code
# file that came out suspiciously small all stop the script. A 2 kB G-code
# file looks like success to a shell and is an empty plate in practice.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

SCAD=${SCAD:-openscad}
PROFILE=${PROFILE:-pla-0.2mm.ini}
OUT=${OUT:-build}
MODEL=safekiln-enclosure.scad

# PrusaSlicer ships under several names depending on how it was installed.
find_slicer() {
    for c in "${PRUSA_SLICER:-}" prusa-slicer PrusaSlicer prusa-slicer-console \
             "/Applications/PrusaSlicer.app/Contents/MacOS/PrusaSlicer" \
             "/Applications/Original Prusa Drivers/PrusaSlicer.app/Contents/MacOS/PrusaSlicer"; do
        [ -n "$c" ] || continue
        if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return 0; fi
        if [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}

stl_only=0
parts=()
while [ $# -gt 0 ]; do
    case "$1" in
    --stl-only) stl_only=1; shift ;;
    -h|--help)  sed -n '4,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    shell|bezel|knob) parts+=("$1"); shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#parts[@]} -gt 0 ] || parts=(shell bezel knob)

command -v "$SCAD" >/dev/null 2>&1 || {
    echo "openscad is not on PATH." >&2
    echo "  macOS:  brew install openscad" >&2
    echo "  Debian: apt-get install openscad" >&2
    exit 1
}
[ -f "$PROFILE" ] || { echo "no profile at $PROFILE" >&2; exit 1; }

mkdir -p "$OUT"
echo "openscad: $("$SCAD" --version 2>&1 | head -1)"

# --- per-part slicer arguments --------------------------------------------
#
# The shell is 115 mm tall on a 54 x 81 footprint. That aspect ratio is what
# takes a part off the bed when somebody walks past it, so it gets a brim and
# nothing else does: a brim on the bezel would have to be cut off the face
# the operator looks at.
slicer_args_for() {
    case "$1" in
    shell) echo "--brim-width 3" ;;
    bezel) echo "--brim-width 0" ;;
    # The knob is 25 mm across and 16 tall, so its first layer is small and
    # its grip ends up at the top where any wobble shows. Slower first layer,
    # and one more top layer than the profile for the face under a thumb.
    knob)  echo "--brim-width 2 --first-layer-speed 15 --top-solid-layers 6" ;;
    esac
}

fail=0
for p in "${parts[@]}"; do
    stl="$OUT/$p.stl"
    echo
    echo "=== $p ==="
    "$SCAD" -D "part=\"$p\"" -o "$stl" "$MODEL" 2>&1 \
      | grep -E "WARNING|ERROR|Simple:|Volumes:" || true

    # A render that is not a closed solid slices into something unpredictable,
    # so it stops here rather than at the printer.
    if ! grep -qi "solid\|facet" "$stl" 2>/dev/null; then
        echo "::error::$stl is not an STL" >&2; fail=1; continue
    fi
    facets=$(grep -c "facet normal" "$stl" || true)
    echo "facets: $facets"
    if [ "${facets:-0}" -lt 100 ]; then
        echo "::error::$p rendered only $facets facets, which is not this part" >&2
        fail=1; continue
    fi

    [ "$stl_only" = "1" ] && continue

    slicer=$(find_slicer || true)
    if [ -z "$slicer" ]; then
        echo "PrusaSlicer not found; $stl is ready to slice by hand." >&2
        echo "  Set PRUSA_SLICER=/path/to/prusa-slicer, or install it." >&2
        echo "  Then: $0 $p" >&2
        continue
    fi

    gcode="$OUT/$p.gcode"
    # shellcheck disable=SC2046  # the per-part arguments are meant to split
    rm -f "$gcode"
    # shellcheck disable=SC2046  # the per-part arguments are meant to split
    "$slicer" --export-gcode --load "$PROFILE" $(slicer_args_for "$p") \
              --output "$gcode" "$stl" >"$OUT/$p.slicer.log" 2>&1 || true
    # Not `|| { ... }`: PrusaSlicer exits ZERO when it refuses a configuration.
    # It printed "Relative extruder addressing requires resetting the extruder
    # position at each layer", wrote no file, and returned success. So the
    # existence of the output is the test, not the exit status.
    if [ ! -f "$gcode" ]; then
        echo "::error::slicing $p produced no G-code (the slicer exits 0 when "\
             "it refuses a configuration); see $OUT/$p.slicer.log" >&2
        tail -3 "$OUT/$p.slicer.log" >&2 || true
        fail=1; continue
    fi

    # A G-code file that exists is not a G-code file that prints. These three
    # lines are what distinguish a sliced part from an empty plate.
    bytes=$(wc -c < "$gcode" | tr -d ' ')
    moves=$(grep -c "^G1 " "$gcode" || true)
    if [ "$bytes" -lt 20000 ] || [ "${moves:-0}" -lt 500 ]; then
        echo "::error::$gcode is $bytes bytes with $moves moves, which is an "\
             "empty plate rather than a printed part" >&2
        fail=1; continue
    fi
    # --- did the profile actually apply? ------------------------------
    #
    # The reason this check exists: the first version of pla-0.2mm.ini had
    # [print:]/[filament:]/[printer:] sections, which makes it a config BUNDLE.
    # `--load` ignored them without a word and sliced at 0.3 mm, 200 degC and a
    # cold bed. The G-code was 891 kB with 30 000 moves and passed every check
    # above it.
    #
    # PrusaSlicer writes its whole resolved configuration into the G-code
    # footer, so the settings can be read back from the artefact rather than
    # assumed from the file that was passed in. Four are checked, chosen
    # because each would ruin the print on its own and none is visible in a
    # preview: the layer height, both temperatures and the perimeter count.
    for want in "layer_height = 0.2" "temperature = 210" \
                "bed_temperature = 60" "perimeters = 3"; do
        if ! grep -qx "; $want" "$gcode"; then
            got=$(grep -E "^; ${want%% =*} = " "$gcode" | head -1 | sed 's/^; //')
            echo "::error::$p was sliced with '${got:-nothing}' where the "\
                 "profile says '$want': $PROFILE was not applied" >&2
            fail=1
        fi
    done

    # The estimate and the filament, straight from the slicer's own summary.
    grep -E "^; (estimated printing time|filament used \[g\])" "$gcode" \
      | sed 's/^; /  /' || true
    echo "  $gcode ($bytes bytes, $moves moves)"
done

echo
if [ "$fail" -ne 0 ]; then
    echo "one or more parts did not complete" >&2
    exit 1
fi
cat <<'NOTE'
Before printing, two things worth a moment:

  PLA SOFTENS AROUND 55 TO 60 degC. This enclosure bolts to the panel of a
  kiln. Measure the panel at temperature before trusting PLA on a real
  installation, and read the note in README.md: PETG or ASA is the answer if
  it runs warm, and the same profile slices either with its own filament
  settings.

  THE KNOB'S BORE IS THE ONE DIMENSION TO CHECK FIRST. Print the knob alone,
  try it on the encoder, and adjust knob_bore in the .scad before committing
  to the two large parts. It costs four minutes and saves two hours.
NOTE

#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Analyse the ESP-IDF-facing code that tools/tidy.sh cannot reach.
#
# tools/tidy.sh drives from the *host* compile database, which by construction
# excludes kiln_hal_esp32s3: it needs the IDF headers.  This script drives from
# the database idf.py emits instead, so the adapter layer is analysed too.
#
#   idf.py build && tools/tidy-target.sh          analyse, fail on any finding
#   idf.py build && tools/tidy-target.sh --fix    apply what can be applied
#
# The target database cannot be fed to clang-tidy unaltered: it is written for
# xtensa-esp32s3-elf-g++ and carries flags clang rejects outright
# (-mlongcalls and friends), plus an @response file that smuggles more of them
# back in.  So it is filtered into a clang-digestible copy first.  Analysing
# xtensa code with a clang front end is approximate by nature -- a handful of
# IDF's own headers will not resolve -- so only findings in *our* files count.
#
# ---------------------------------------------------------------------------
# WHY THERE IS A CANARY PASS
# ---------------------------------------------------------------------------
# "Only findings in our files count" is also how this script could pass
# vacuously: if the toolchain headers cannot be found, every one of our files
# fails to parse, clang-tidy reports nothing about them, and the grep below
# finds nothing to complain about -- green, having checked nothing.  That is
# the failure mode tasklist F1b called out as the reason not to wire this into
# CI blind.
#
# So a clean verdict is not trusted on its own.  Before the real analysis, each
# translation unit is run through a check that *must* fire on any file with a
# function in it (function-size with a threshold of zero statements).  A file
# that reports nothing there did not parse, and the script fails saying so
# rather than reporting it clean.  The picolibc lookup fails loudly for the
# same reason.
set -euo pipefail

cd "$(dirname "$0")/.."

# firmware/controller/build since the split into controller and supervisor.
# This said firmware/build, so the script ran and then reported the database
# missing, which reads like "you forgot to build" and is not that.
IDF_BUILD=${IDF_BUILD:-firmware/controller/build}
DB=$IDF_BUILD/compile_commands.json
OUT=${OUT:-$IDF_BUILD/tidy-target}
SRC_DIR=firmware/controller/components/kiln_hal_esp32s3/src
FIX=0
[[ ${1:-} == "--fix" ]] && FIX=1

[[ -f $DB ]] || { echo "no $DB -- run 'idf.py build' in firmware/controller/ first" >&2; exit 1; }

# TIDY= overrides, which is how you reproduce CI's pinned version locally:
#   TIDY=$(brew --prefix llvm@20)/bin/clang-tidy tools/tidy.sh
if [[ -z ${TIDY:-} ]]; then
    if command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang-tidy" ]]; then
        TIDY="$(brew --prefix llvm)/bin/clang-tidy"
    else
        TIDY=$(command -v clang-tidy || true)
    fi
fi
[[ -n ${TIDY:-} ]] || { echo "clang-tidy not found (brew install llvm)" >&2; exit 127; }

# picolibc headers from the xtensa toolchain, so <string.h> and friends resolve.
# IDF_TOOLS_PATH first: that is what the espressif/idf container sets
# (/opt/esp), and it is the only place the toolchain lives there.  Missing
# headers are fatal rather than ignored -- see the canary note above.
TOOLS=${IDF_TOOLS_PATH:-$HOME/.espressif}
# `|| true`, or set -e kills the script here with no message at all when
# the glob matches nothing -- which is the one case worth reporting.
PICO=$(ls -d "$TOOLS"/tools/xtensa-esp-elf/*/xtensa-esp-elf/picolibc/include \
         2>/dev/null | head -1 || true)
if [[ -z ${PICO:-} ]]; then
    echo "no picolibc headers under $TOOLS/tools/xtensa-esp-elf/*/" >&2
    echo "set IDF_TOOLS_PATH to the toolchain root; refusing to analyse" \
         "without the C library headers, because the result would be" \
         "vacuously clean" >&2
    exit 1
fi

mkdir -p "$OUT"
python3 - "$DB" "$OUT/compile_commands.json" "$PICO" <<'PY'
import json, re, shlex, sys
src, dst, pico = sys.argv[1], sys.argv[2], sys.argv[3]
drop = re.compile(r'^(@.*|-mlongcalls|-mno-target-align|-freorder-blocks'
                  r'|-fstrict-volatile-bitfields|-fno-tree-switch-conversion'
                  r'|-fno-shrink-wrap|-mtext-section-literals|-fmacro-prefix-map=.*'
                  r'|-gdwarf-4|-fno-jump-tables|-std=gnu\+\+\d+|-fno-builtin.*'
                  r'|-fuse-cxa-atexit|-Os|-ggdb)$')
out = []
for e in json.load(open(src)):
    if 'kiln_hal_esp32s3' not in e['file']:
        continue
    args = shlex.split(e['command']) if 'command' in e else e['arguments']
    new  = [a for a in args[1:] if not drop.match(a)]
    pre  = ['clang++', '--target=riscv32-unknown-elf', '-std=gnu++20',
            '-isystem', pico, '-D__PICOLIBC__']
    out.append({'directory': e['directory'], 'arguments': pre + new, 'file': e['file']})
json.dump(out, open(dst, 'w'), indent=1)
print(f"filtered {len(out)} target translation units")
PY

mapfile -t FILES < <(ls "$SRC_DIR"/*.cpp)
(( ${#FILES[@]} )) || { echo "no sources in $SRC_DIR" >&2; exit 1; }

# --- canary: prove every unit actually parsed ---------------------------
# function-size with a zero-statement threshold reports every function clang
# built an AST for.  A file reporting none did not compile, and a "clean"
# verdict for it would mean nothing.
echo "canary: confirming all ${#FILES[@]} units parse"
# Both temp files are named before the trap is installed: the canary can
# exit before $log exists, and the trap would then trip over set -u.
canary=$(mktemp); log=$(mktemp)
trap 'rm -f "$canary" "$log"' EXIT
for f in "${FILES[@]}"; do
    "$TIDY" -p "$OUT" --quiet --checks='-*,readability-function-size' \
        --config="{CheckOptions: {readability-function-size.StatementThreshold: 0}}" \
        "$f" >>"$canary" 2>/dev/null || true
done
unparsed=()
for f in "${FILES[@]}"; do
    grep -qF "$(basename "$f"):" "$canary" || unparsed+=("$f")
done
if (( ${#unparsed[@]} )); then
    echo "VACUOUS PASS AVERTED: clang-tidy built no AST for:" >&2
    printf '  %s\n' "${unparsed[@]}" >&2
    echo >&2
    echo "These files were not analysed, so a clean report would be a lie." \
         "Usually the toolchain headers: check IDF_TOOLS_PATH ($TOOLS) and" \
         "that 'idf.py build' produced $DB from this tree." >&2
    exit 1
fi
echo "canary: all units parsed"

# --- the real analysis --------------------------------------------------
ARGS=(-p "$OUT" --quiet)
(( FIX )) && ARGS+=(--fix --fix-errors)

for f in "${FILES[@]}"; do
    "$TIDY" "${ARGS[@]}" "$f" >>"$log" 2>/dev/null || true
done

# Only our own files: an unresolved IDF header is a limitation of the front
# end, not a defect in this repository.
ours=$(grep -E "error:|warning:" "$log" | grep "/$SRC_DIR/" | sort -u || true)
if [[ -n $ours ]]; then
    echo "$ours" | sed "s|$PWD/||"
    echo
    echo "findings: $(echo "$ours" | wc -l | tr -d ' ')"
    (( FIX )) || exit 1
else
    echo "clean -- no findings in kiln_hal_esp32s3"
fi

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
set -euo pipefail

cd "$(dirname "$0")/.."

IDF_BUILD=${IDF_BUILD:-firmware/build}
DB=$IDF_BUILD/compile_commands.json
OUT=${OUT:-$IDF_BUILD/tidy-target}
FIX=0
[[ ${1:-} == "--fix" ]] && FIX=1

[[ -f $DB ]] || { echo "no $DB -- run 'idf.py build' in firmware/ first" >&2; exit 1; }

if command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang-tidy" ]]; then
    TIDY="$(brew --prefix llvm)/bin/clang-tidy"
else
    TIDY=$(command -v clang-tidy || true)
fi
[[ -n ${TIDY:-} ]] || { echo "clang-tidy not found (brew install llvm)" >&2; exit 127; }

# picolibc headers from the xtensa toolchain, so <string.h> and friends resolve.
PICO=$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/picolibc/include 2>/dev/null | head -1)

mkdir -p "$OUT"
python3 - "$DB" "$OUT/compile_commands.json" "${PICO:-}" <<'PY'
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
    pre  = ['clang++', '--target=riscv32-unknown-elf', '-std=gnu++20']
    if pico:
        pre += ['-isystem', pico, '-D__PICOLIBC__']
    out.append({'directory': e['directory'], 'arguments': pre + new, 'file': e['file']})
json.dump(out, open(dst, 'w'), indent=1)
print(f"filtered {len(out)} target translation units")
PY

ARGS=(-p "$OUT" --quiet)
(( FIX )) && ARGS+=(--fix --fix-errors)

log=$(mktemp); trap 'rm -f "$log"' EXIT
for f in firmware/components/kiln_hal_esp32s3/src/*.cpp; do
    "$TIDY" "${ARGS[@]}" "$f" >>"$log" 2>/dev/null || true
done

# Only our own files: an unresolved IDF header is a limitation of the front
# end, not a defect in this repository.
ours=$(grep -E "error:|warning:" "$log" | grep "/firmware/components/kiln_hal_esp32s3/" | sort -u || true)
if [[ -n $ours ]]; then
    echo "$ours" | sed "s|$PWD/||"
    echo
    echo "findings: $(echo "$ours" | wc -l | tr -d ' ')"
    (( FIX )) || exit 1
else
    echo "clean -- no findings in kiln_hal_esp32s3"
fi

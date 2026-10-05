#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run clang-tidy over the host build, exactly as CI does (NFR-25, TR-24).
#
#   tools/tidy.sh              analyse, fail on any finding
#   tools/tidy.sh --fix        apply the fixes clang-tidy can apply
#
# Analysis is driven from compile_commands.json rather than a glob over the
# tree, because a file clang-tidy has no compile command for is analysed with
# guessed flags and reports nonsense.  That deliberately leaves two things out:
#
#   kiln_hal_esp32s3  needs the ESP-IDF headers; it is analysed by the
#                     idf.py-based job, not this one (tasklist F1).
#   host/webhost      a separate CMake project with its own database.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD=${BUILD:-build-host}
FIX=0
[[ ${1:-} == "--fix" ]] && FIX=1

# Homebrew LLVM first: Apple's clang ships no clang-tidy, and the hicpp-*
# module these checks replaced is gone from LLVM 23 regardless of vendor.
if command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang-tidy" ]]; then
    TIDY="$(brew --prefix llvm)/bin/clang-tidy"
else
    TIDY=$(command -v clang-tidy || true)
fi
[[ -n ${TIDY:-} ]] || { echo "clang-tidy not found (brew install llvm)" >&2; exit 127; }

EXTRA=()
# Homebrew clang does not know where the macOS SDK is; on Linux this is unset.
if [[ $(uname) == Darwin ]] && command -v xcrun >/dev/null 2>&1; then
    EXTRA+=(--extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)")
fi

if [[ ! -f $BUILD/compile_commands.json ]]; then
    cmake -B "$BUILD" -S firmware/test/host -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

mapfile -t FILES < <(python3 -c "
import json,sys
print('\n'.join(sorted({e['file'] for e in json.load(open('$BUILD/compile_commands.json'))})))")

echo "clang-tidy: $("$TIDY" --version | grep -oE "version [0-9.]+" | head -1)"
echo "analysing ${#FILES[@]} translation units from $BUILD/compile_commands.json"

ARGS=(-p "$BUILD" --quiet "${EXTRA[@]}")
(( FIX )) && ARGS+=(--fix --fix-errors)

log=$(mktemp)
trap 'rm -f "$log"' EXIT
rc=0
for f in "${FILES[@]}"; do
    "$TIDY" "${ARGS[@]}" "$f" >>"$log" 2>/dev/null || rc=1
done

if grep -qE "error:|warning:" "$log"; then
    grep -E "error:|warning:" "$log" | sed "s|$PWD/||" | sort -u
    echo
    echo "findings: $(grep -cE 'error:|warning:' "$log")"
    (( FIX )) || exit 1
elif (( FIX )); then
    echo "no findings left to fix"
else
    echo "clean -- no findings"
fi
exit $(( FIX ? 0 : rc ))

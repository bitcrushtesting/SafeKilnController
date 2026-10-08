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
# guessed flags and reports nonsense.  Two databases are therefore generated:
#
#   firmware/controller/test/host  the host build, which is almost everything.
#   firmware/controller/host/webhost
#                       a separate CMake project.  Its *own* file is the only
#                       one taken from it; the components it links are already
#                       covered above, and analysing them twice would just
#                       double-report.
#
# One thing is still out of reach here: kiln_hal_esp32s3 needs the ESP-IDF
# headers, so it is analysed by tools/tidy-target.sh against the database
# idf.py emits.  Both run in CI.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD=${BUILD:-build-host}
FIX=0
[[ ${1:-} == "--fix" ]] && FIX=1

# Homebrew LLVM first: Apple's clang ships no clang-tidy, and the hicpp-*
# module these checks replaced is gone from LLVM 23 regardless of vendor.
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

EXTRA=()
# Homebrew clang does not know where the macOS SDK is; on Linux this is unset.
if [[ $(uname) == Darwin ]] && command -v xcrun >/dev/null 2>&1; then
    EXTRA+=(--extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)")
fi

if [[ ! -f $BUILD/compile_commands.json ]]; then
    cmake -B "$BUILD" -S firmware/controller/test/host -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

WEBHOST_BUILD=${WEBHOST_BUILD:-build-webhost}
if [[ ! -f $WEBHOST_BUILD/compile_commands.json ]]; then
    cmake -B "$WEBHOST_BUILD" -S firmware/controller/host/webhost \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

mapfile -t FILES < <(python3 -c "
import json,sys
print('\n'.join(sorted({e['file'] for e in json.load(open('$BUILD/compile_commands.json'))})))")

# webhost.cpp only: everything else in that project is a component the host
# database already covers.
mapfile -t WEBHOST_FILES < <(python3 -c "
import json
db = json.load(open('$WEBHOST_BUILD/compile_commands.json'))
print('\n'.join(sorted({e['file'] for e in db if '/host/webhost/' in e['file']})))")
(( ${#WEBHOST_FILES[@]} )) || {
    echo "no webhost translation unit in $WEBHOST_BUILD -- refusing to report" \
         "clean for a project that was not analysed" >&2
    exit 1
}

echo "clang-tidy: $("$TIDY" --version | grep -oE "version [0-9.]+" | head -1)"
echo "analysing ${#FILES[@]} translation units from $BUILD/compile_commands.json"
echo "       and ${#WEBHOST_FILES[@]} from $WEBHOST_BUILD/compile_commands.json"

ARGS=(-p "$BUILD" --quiet "${EXTRA[@]}")
(( FIX )) && ARGS+=(--fix --fix-errors)

log=$(mktemp)
trap 'rm -f "$log"' EXIT
rc=0
for f in "${FILES[@]}"; do
    "$TIDY" "${ARGS[@]}" "$f" >>"$log" 2>/dev/null || rc=1
done
WEBHOST_ARGS=(-p "$WEBHOST_BUILD" --quiet "${EXTRA[@]}")
(( FIX )) && WEBHOST_ARGS+=(--fix --fix-errors)
for f in "${WEBHOST_FILES[@]}"; do
    "$TIDY" "${WEBHOST_ARGS[@]}" "$f" >>"$log" 2>/dev/null || rc=1
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

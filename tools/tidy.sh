#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run clang-tidy over the host build, exactly as CI does (SWR-NFR-25, SWR-TST-24).
#
#   tools/tidy.sh              analyse, fail on any finding
#   tools/tidy.sh --fix        apply the fixes clang-tidy can apply
#
# Analysis is driven from compile_commands.json rather than a glob over the
# tree, because a file clang-tidy has no compile command for is analysed with
# guessed flags and reports nonsense.  Four databases are therefore generated:
#
#   firmware/controller/test/host  the host build, which is almost everything.
#   firmware/controller/host/webhost
#                       a separate CMake project.  Its *own* file is the only
#                       one taken from it; the components it links are already
#                       covered above, and analysing them twice would just
#                       double-report.
#   firmware/supervisor/test/host
#                       the supervisor's core and protocol, plus its suites.
#                       Same filter logic: kiln_check.cpp is linked from the
#                       controller tree and is already covered above.
#   synthesised         the supervisor's two target-only units, src/main.cpp
#                       and board/startup.cpp, which no host project compiles.
#                       See THE SUPERVISOR'S BOARD LAYER below.
#
# One thing is still out of reach here: kiln_hal_esp32s3 needs the ESP-IDF
# headers, so it is analysed by tools/tidy-target.sh against the database
# idf.py emits.  Both run in CI.
#
# ---------------------------------------------------------------------------
# THE SUPERVISOR'S BOARD LAYER
# ---------------------------------------------------------------------------
# src/main.cpp and board/startup.cpp are built only by the ARM target project,
# and STM32CubeCLT is not in CI.  They are analysed against a *synthesised*
# database instead, with clang's own bare-metal target: both files include
# nothing but <stdint.h> and the project's own headers, so -ffreestanding
# -nostdinc++ resolves everything without a sysroot and without arm-none-eabi
# being installed at all.
#
# The flags mirror firmware/supervisor/CMakeLists.txt rather than approximating
# it, because -Wconversion and -fno-exceptions change what the front end sees.
# They are listed in one place, SUP_TARGET_FLAGS, so the two cannot drift
# silently -- and if they do drift, the compiler is still the authority: this
# database only ever feeds the analyser.
#
# A synthesised database brings the vacuous-pass risk tools/tidy-target.sh
# documents at length: if the headers do not resolve, the files do not parse,
# clang-tidy reports nothing, and the run is green having checked nothing.  So
# the same canary runs here, over the synthesised units only.  The three real
# databases come from a CMake configure of a project that builds, and a parse
# failure in those shows up as a finding rather than as silence.
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
# Host databases only: handing -isysroot to the bare-metal parse below would
# put the macOS SDK on a thumbv6m include path.
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

SUP_BUILD=${SUP_BUILD:-build-sup-tidy}
if [[ ! -f $SUP_BUILD/compile_commands.json ]]; then
    cmake -B "$SUP_BUILD" -S firmware/supervisor/test/host \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

# --- the synthesised database for the supervisor's board layer -------------
SUP_TARGET_BUILD=${SUP_TARGET_BUILD:-build-sup-tidy-target}
SUP_TARGET_UNITS=(firmware/supervisor/src/main.cpp firmware/supervisor/board/startup.cpp)
SUP_TARGET_FLAGS=(
    --target=thumbv6m-none-eabi -mcpu=cortex-m0plus
    -ffreestanding -nostdinc++ -std=gnu++20
    -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -Ifirmware/supervisor/board
    -Ifirmware/supervisor/core/include
    -Ifirmware/supervisor/protocol/include)

mkdir -p "$SUP_TARGET_BUILD"
python3 - "$SUP_TARGET_BUILD/compile_commands.json" "$PWD" \
         "${#SUP_TARGET_UNITS[@]}" "${SUP_TARGET_UNITS[@]}" "${SUP_TARGET_FLAGS[@]}" <<'PY'
import json, sys
dst, root, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
units = sys.argv[4:4 + n]
flags = sys.argv[4 + n:]
db = [{'directory': root,
       'arguments': ['clang++', *flags, '-c', f],
       'file': f} for f in units]
json.dump(db, open(dst, 'w'), indent=1)
PY

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

# Supervisor sources only: the suites link the controller's kiln_check.cpp,
# which the first database already covers.
mapfile -t SUP_FILES < <(python3 -c "
import json
db = json.load(open('$SUP_BUILD/compile_commands.json'))
print('\n'.join(sorted({e['file'] for e in db if '/firmware/supervisor/' in e['file']})))")
(( ${#SUP_FILES[@]} )) || {
    echo "no supervisor translation unit in $SUP_BUILD -- refusing to report" \
         "clean for a project that was not analysed" >&2
    exit 1
}

echo "clang-tidy: $("$TIDY" --version | grep -oE "version [0-9.]+" | head -1)"
echo "analysing ${#FILES[@]} translation units from $BUILD/compile_commands.json"
echo "       and ${#WEBHOST_FILES[@]} from $WEBHOST_BUILD/compile_commands.json"
echo "       and ${#SUP_FILES[@]} from $SUP_BUILD/compile_commands.json"
echo "       and ${#SUP_TARGET_UNITS[@]} from $SUP_TARGET_BUILD/compile_commands.json (synthesised)"

log=$(mktemp)
canary=$(mktemp)
trap 'rm -f "$log" "$canary"' EXIT

# --- canary: prove the synthesised units actually parsed -------------------
# function-size with a zero-statement threshold reports every function clang
# built an AST for.  A file reporting none did not compile, and a "clean"
# verdict for it would mean nothing.  Only the synthesised database needs
# this; see the header.
for f in "${SUP_TARGET_UNITS[@]}"; do
    "$TIDY" -p "$SUP_TARGET_BUILD" --quiet --checks='-*,readability-function-size' \
        --config="{CheckOptions: {readability-function-size.StatementThreshold: 0}}" \
        "$f" >>"$canary" 2>/dev/null || true
done
unparsed=()
for f in "${SUP_TARGET_UNITS[@]}"; do
    grep -qF "$(basename "$f"):" "$canary" || unparsed+=("$f")
done
if (( ${#unparsed[@]} )); then
    echo "VACUOUS PASS AVERTED: clang-tidy built no AST for:" >&2
    printf '  %s\n' "${unparsed[@]}" >&2
    echo >&2
    echo "These files were not analysed, so a clean report would be a lie." \
         "The synthesised flags in SUP_TARGET_FLAGS no longer resolve this" \
         "code: check that clang still knows the thumbv6m-none-eabi target" \
         "and that nothing new is included from a hosted C library." >&2
    exit 1
fi

# --- the real analysis -----------------------------------------------------
rc=0
analyse() {
    local db=$1; shift
    local -a args=(-p "$db" --quiet)
    # Host databases get the macOS sysroot; the bare-metal one must not.
    [[ $db == "$SUP_TARGET_BUILD" ]] || args+=("${EXTRA[@]}")
    (( FIX )) && args+=(--fix --fix-errors)
    local f
    for f in "$@"; do
        "$TIDY" "${args[@]}" "$f" >>"$log" 2>/dev/null || rc=1
    done
}

analyse "$BUILD"            "${FILES[@]}"
analyse "$WEBHOST_BUILD"    "${WEBHOST_FILES[@]}"
analyse "$SUP_BUILD"        "${SUP_FILES[@]}"
analyse "$SUP_TARGET_BUILD" "${SUP_TARGET_UNITS[@]}"

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

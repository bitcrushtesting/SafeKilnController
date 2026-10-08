#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Modified condition/decision coverage for the supervisor's host tests.
#
#   tools/mcdc.sh              measure, fail below the floor
#   tools/mcdc.sh --show       also print every decision and its test vectors
#   MCDC_FLOOR=90 tools/mcdc.sh
#
# ---------------------------------------------------------------------------
# WHY MC/DC AND NOT BRANCH COVERAGE
# ---------------------------------------------------------------------------
# The supervisor's permit decision is a five-term conjunction:
#
#     permit = selftest_ok && !tripped && chamber_valid
#           && fault_bits == 0 && chamber_c <= SUP_OVERTEMP_C
#
# One test with everything true and one with everything false gives 100 % line
# and 100 % branch coverage of that line, and proves almost nothing: it does not
# show that `chamber_valid` alone can withhold heat, which is the property the
# whole component exists for.  MC/DC requires each condition to be shown to
# change the outcome *independently*, so a five-term decision needs at least six
# cases that differ in the right way.
#
# gcov cannot measure it.  Clang can, with -fcoverage-mcdc, from Clang 18.
#
# This covers the supervisor only.  Its trip logic is roughly 120 lines and is
# the one place where a missed condition means heat that should have stopped;
# holding the whole project to the same bar would be a different and much larger
# argument.
set -euo pipefail

cd "$(dirname "$0")/.."

FLOOR=${MCDC_FLOOR:-80}
OUT=${OUT:-build-mcdc}
SHOW=0
[[ ${1:-} == "--show" ]] && SHOW=1

# Apple's clang ships no llvm-profdata/llvm-cov pair that matches it, and
# -fcoverage-mcdc needs Clang 18 or newer.  CLANGXX= overrides.
if [[ -z ${CLANGXX:-} ]]; then
    if command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang++" ]]; then
        BIN="$(brew --prefix llvm)/bin"
    else
        BIN=$(dirname "$(command -v clang++ 2>/dev/null || echo /nonexistent/x)")
    fi
    CLANGXX="$BIN/clang++"
    LLVM_PROFDATA=${LLVM_PROFDATA:-$BIN/llvm-profdata}
    LLVM_COV=${LLVM_COV:-$BIN/llvm-cov}
fi
LLVM_PROFDATA=${LLVM_PROFDATA:-llvm-profdata}
LLVM_COV=${LLVM_COV:-llvm-cov}

for t in "$CLANGXX" "$LLVM_PROFDATA" "$LLVM_COV"; do
    command -v "$t" >/dev/null 2>&1 || [[ -x $t ]] || {
        echo "$t not found; need Clang 18+ with matching llvm-cov" >&2; exit 127; }
done

# A clang that cannot do MC/DC must not quietly produce a report without it.
if ! echo 'int f(int a,int b){return a&&b;}' \
     | "$CLANGXX" -x c++ -fprofile-instr-generate -fcoverage-mapping \
                  -fcoverage-mcdc -c -o /dev/null - 2>/dev/null; then
    echo "$CLANGXX does not support -fcoverage-mcdc (needs Clang 18+)" >&2
    exit 1
fi

rm -rf "$OUT"; mkdir -p "$OUT"

FLAGS=(-std=c++20 -fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
       -O0 -g -Wall -Wextra -Werror
       -Ifirmware/supervisor/core/include -Ifirmware/supervisor/protocol/include
       -Ifirmware/controller/test/host/support)
# The host test harness needs the macOS SDK when the compiler is not Apple's.
if [[ $(uname) == Darwin ]] && command -v xcrun >/dev/null 2>&1; then
    FLAGS+=(-isysroot "$(xcrun --show-sdk-path)")
fi

UNDER=(firmware/supervisor/core/src/trip.cpp firmware/supervisor/core/src/max31856.cpp
       firmware/supervisor/core/src/selfcheck.cpp
       firmware/supervisor/protocol/src/sup_proto.cpp)
for suite in test_trip test_proto test_max31856 test_selfcheck; do
    "$CLANGXX" "${FLAGS[@]}" "${UNDER[@]}" \
        firmware/controller/test/host/support/kiln_check.cpp \
        "firmware/supervisor/test/host/$suite.cpp" -o "$OUT/$suite"
    ( cd "$OUT" && LLVM_PROFILE_FILE="$suite.profraw" "./$suite" >/dev/null )
done

"$LLVM_PROFDATA" merge -sparse "$OUT"/*.profraw -o "$OUT/sup.profdata"

# llvm-cov takes the first binary positionally and the rest with -object.
REPORT=$("$LLVM_COV" report "$OUT/test_trip" -object "$OUT/test_proto" \
         -instr-profile="$OUT/sup.profdata" --show-mcdc-summary \
         --ignore-filename-regex='(kiln_check|/test_)')
echo "$REPORT"

if (( SHOW )); then
    "$LLVM_COV" show "$OUT/test_trip" -object "$OUT/test_proto" \
        -instr-profile="$OUT/sup.profdata" --show-mcdc \
        --ignore-filename-regex='(kiln_check|/test_)'
fi

# The TOTAL row's last percentage is MC/DC.
PCT=$(echo "$REPORT" | awk '/^TOTAL/{gsub(/%/,"",$NF); print $NF}')
[[ -n $PCT ]] || { echo "could not read the MC/DC total from llvm-cov" >&2; exit 1; }

printf '\nMC/DC %s%%, floor %s%%\n' "$PCT" "$FLOOR"
if awk -v p="$PCT" -v f="$FLOOR" 'BEGIN{exit !(p+0 < f+0)}'; then
    echo
    echo "below the floor.  The condition pairs that are not covered:"
    "$LLVM_COV" show "$OUT/test_trip" -object "$OUT/test_proto" \
        -instr-profile="$OUT/sup.profdata" --show-mcdc \
        --ignore-filename-regex='(kiln_check|/test_)' 2>/dev/null \
      | awk '/MC\/DC Decision Region/{d=$0} /C[0-9]+-Pair: not covered/{print "  "d; print "     "$0}'
    exit 1
fi
echo "ok"

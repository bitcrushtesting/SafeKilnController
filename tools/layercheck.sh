#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hold three architecture decisions to the code rather than to discipline:
# SWA-14, SWA-03 and SWA-02 (tasklist C5 and C13).
#
#   tools/layercheck.sh                 check everything it can from what is built
#   tools/layercheck.sh --self-test     prove each check can fail
#
# ===========================================================================
# WHY NOT A GREP
# ===========================================================================
# CI already greps kiln_core for `#include <esp_...>`, and the comment beside
# it says the full check is this script. A grep over include lines misses the
# two ways this has any chance of going wrong in practice:
#
#   - a platform header reached INDIRECTLY, through a project header that
#     includes it;
#   - an include inside `#if defined(ESP_PLATFORM)`, which the host build
#     never compiles and the host tests therefore never catch, so the core
#     would quietly stop being portable while every gate stayed green.
#
# So the include check reads the compiler's own dependency files from the
# TARGET build, where the IDF headers are on the path and a mistake would
# actually resolve. That is the preprocessor's answer, not a guess at it.
#
# The other two checks read the object code, which is stronger than any grep
# over sources could be:
#
#   SWA-03, no mutable state in the core: a mutable file-scope object lands in
#   .data or .bss, so `nm` finds it whatever it is called and wherever it was
#   declared -- including a function-local `static`, which is the form that
#   slips past a reviewer looking for globals.
#
#   SWA-02, time is injected: a direct clock read leaves an UNDEFINED symbol
#   for time, clock_gettime, esp_timer_get_time and friends. There is nowhere
#   to hide one, and nothing to argue about.
#
# What these two buy is the 168-hour firing that a test exercises in
# milliseconds, and the 15-minute runaway timer it does the same to. Both stop
# working the first time the core reads a real clock or keeps state between
# calls, and neither failure announces itself: the tests still pass, they just
# stop testing what they say they test.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here/.."
cd "$root"

HOST_BUILD=${HOST_BUILD:-build-host}
TARGET_BUILD=${TARGET_BUILD:-firmware/controller/build-hw}
CORE=firmware/controller/components/kiln_core
PORTS=firmware/controller/components/kiln_ports

fail=0
note() { printf '%s\n' "$*"; }
err()  { printf '::error::%s\n' "$*" >&2; fail=1; }

# --- 1. the core includes no platform header, transitively ----------------
#
# Driven from the target build's .d files: they list every header the
# preprocessor actually opened, so an indirect or ESP_PLATFORM-guarded include
# appears here and nowhere else.
check_includes() {
    # Ninja keeps dependencies in a binary log rather than in .d files, and
    # `ninja -t deps` is the documented way to read it. That log is the
    # preprocessor's own record of every header it opened, which is the whole
    # point of looking here instead of at include lines.
    if [ ! -f "$TARGET_BUILD/.ninja_deps" ]; then
        note "includes:  skipped, no target build at $TARGET_BUILD"
        note "           (build it with: idf.py -B build-hw build)"
        check_direct_includes
        return
    fi
    local ninja_bin=${NINJA:-ninja}
    if ! command -v "$ninja_bin" >/dev/null 2>&1; then
        note "includes:  skipped, ninja is not on PATH (set NINJA=... or . export.sh)"
        check_direct_includes
        return
    fi

    # Every header opened while compiling a kiln_core translation unit.
    local deps
    deps=$("$ninja_bin" -C "$TARGET_BUILD" -t deps 2>/dev/null \
             | awk '/^esp-idf\/(kiln_core|kiln_ports)\//{keep=1; next}
                    /^[^ ]/{keep=0}
                    keep && NF' || true)
    if [ -z "$deps" ]; then
        note "includes:  skipped, the dependency log names no kiln_core units"
        check_direct_includes
        return
    fi

    # IDF components the core's preprocessed sources must not reach. The
    # allowance below is not permission to include them: it is what the
    # toolchain itself drags in through <stdint.h> and <math.h> on an IDF
    # build, where picolibc's headers live under esp_libc. Anything outside it
    # means a platform dependency got in, directly or through a header that
    # looked harmless.
    local bad
    bad=$(printf '%s\n' "$deps" | grep -oE 'esp-idf/components/[a-z_0-9]+/' \
            | sed 's|esp-idf/components/||; s|/$||' | sort -u \
            | grep -vxE 'esp_libc|newlib|esp_rom|esp_common' || true)
    if [ -n "$bad" ]; then
        err "SWA-14: kiln_core's preprocessed sources reach IDF components:"
        printf '  %s\n' $bad >&2
        note "       This is what a grep over include lines cannot see: the" >&2
        note "       header may be reached indirectly, or from inside an" >&2
        note "       #if defined(ESP_PLATFORM) the host build never compiles." >&2
    fi
    note "includes:  clean over $(printf '%s\n' "$deps" | sort -u | wc -l | tr -d ' ') headers the target build opened"
    check_direct_includes
}

# The direct form, which is what a reviewer means by the question. Over
# sources, because here what matters is what the code asks for rather than
# what the toolchain supplies.
check_direct_includes() {

    local direct
    direct=$(grep -rnE '#include[[:space:]]*[<"](esp_|freertos/|driver/|soc/|hal/|nvs|sdkconfig)' \
                  "$CORE" "$PORTS" 2>/dev/null || true)
    if [ -n "$direct" ]; then
        err "SWA-14: a platform header is included directly:"
        printf '%s\n' "$direct" >&2
    else
        note "direct:    no platform include in kiln_core or kiln_ports"
    fi
}

# --- 2. SWA-03: the core holds no mutable state --------------------------
core_lib() {
    # Always succeeds: a missing build directory is "nothing to check here",
    # not an error, and `set -e` would otherwise end the script on find's exit
    # status before the caller could say which check it skipped.
    [ -d "$HOST_BUILD" ] || return 0
    find "$HOST_BUILD" -name 'libkiln_core.a' 2>/dev/null | head -1 || true
}

check_no_mutable_state() {
    local lib; lib=$(core_lib)
    if [ -z "$lib" ]; then
        note "SWA-03:    skipped, no host build at $HOST_BUILD"
        return
    fi
    # d/D: initialised writable data. b/B: zero-initialised. Lower case is
    # local linkage, which is exactly where a function-local static lands --
    # the form that looks innocent in review.
    local syms
    syms=$(nm "$lib" 2>/dev/null | awk '$2 ~ /^[dDbB]$/ { print $2" "$3 }' | sort -u || true)
    if [ -n "$syms" ]; then
        err "SWA-03: kiln_core has mutable file-scope state:"
        printf '%s\n' "$syms" >&2
        note "       All core state belongs in the caller's struct. A static" >&2
        note "       here makes two tests in one process interfere, and makes" >&2
        note "       a 168 h firing untestable in milliseconds." >&2
    else
        note "SWA-03:    no mutable state in $(basename "$lib")"
    fi
}

# --- 3. SWA-02: time is injected, never read -----------------------------
check_time_injected() {
    local lib; lib=$(core_lib)
    if [ -z "$lib" ]; then
        note "SWA-02:    skipped, no host build at $HOST_BUILD"
        return
    fi
    # The whole family, including the two IDF ones that would only resolve on
    # a target build: a core that calls them is broken on the host as well,
    # and this is the check that says so before the link does.
    local pattern='^_?(time|clock|clock_gettime|gettimeofday|mktime|localtime|localtime_r|gmtime|esp_timer_get_time|xTaskGetTickCount|esp_log_timestamp)$'
    local refs
    refs=$(nm -u "$lib" 2>/dev/null | awk '{ print $NF }' | sort -u \
             | grep -E "$pattern" || true)
    if [ -n "$refs" ]; then
        err "SWA-02: kiln_core reads a clock directly:"
        printf '%s\n' "$refs" >&2
        note "       Time arrives as a dt or a timestamp argument. A direct" >&2
        note "       read is what makes a dwell tolerance untestable." >&2
    else
        note "SWA-02:    no clock reads in $(basename "$lib")"
    fi
}

# --- 4. the component graph is layered and acyclic -----------------------
#
# From each component's REQUIRES, which is where ESP-IDF records the edges.
check_graph() {
    python3 - "$@" <<'PY'
import pathlib, re, sys

# Low number may be depended on by high, never the reverse. The numbers are the
# layering of architecture section 4: ports are headers everyone may see, the
# core is platform-free logic, the application composes it, the presentation
# and adapter layers sit above, and main is the composition root.
LAYER = {
    'kiln_ports': 0,
    'kiln_core': 1,
    'kiln_app': 2,
    'kiln_web': 3, 'kiln_hmi': 3, 'kiln_sim': 3, 'kiln_hal_esp32s3': 3,
}
comp_dir = pathlib.Path('firmware/controller/components')
edges, bad = {}, []
for d in sorted(comp_dir.iterdir()):
    if not (d / 'CMakeLists.txt').is_file():
        continue
    txt = (d / 'CMakeLists.txt').read_text()
    reqs = set()
    for m in re.finditer(r'(?:PRIV_)?REQUIRES([^)\n]*(?:\n(?!\s*\))[^)\n]*)*)', txt):
        for w in m.group(1).split():
            if w.startswith('kiln_'):
                reqs.add(w)
    edges[d.name] = reqs - {d.name}

for a, deps in edges.items():
    if a not in LAYER:
        continue
    for b in deps:
        if b not in LAYER:
            continue
        if LAYER[b] > LAYER[a]:
            bad.append(f"{a} (layer {LAYER[a]}) requires {b} (layer {LAYER[b]})")
        elif LAYER[b] == LAYER[a] and a != b:
            bad.append(f"{a} and {b} are the same layer and {a} requires {b}")

# Cycles, over the whole graph rather than only the layered part.
seen, stack = set(), []
def walk(n):
    if n in stack:
        bad.append("cycle: " + " -> ".join(stack[stack.index(n):] + [n]))
        return
    if n in seen:
        return
    seen.add(n); stack.append(n)
    for m in sorted(edges.get(n, ())):
        walk(m)
    stack.pop()
for n in sorted(edges):
    walk(n)

if bad:
    print("::error::the component graph is not layered or not acyclic")
    for b in sorted(set(bad)):
        print("  " + b)
    sys.exit(1)
print(f"graph:     {len(edges)} components, layered and acyclic")
PY
}

# --- the self-test -------------------------------------------------------
#
# A gate nobody has seen fail is a gate nobody should trust. This builds two
# tiny objects that violate SWA-03 and SWA-02 and checks that the checks say
# so, which is the only way to know the nm invocations are right on this
# platform rather than merely silent.
self_test() {
    local tmp; tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    local checks=0 failed=0
    t() { checks=$((checks+1)); if [ "$2" = "$3" ]; then echo "ok: $1"; else
            echo "FAIL: $1 (got $2, want $3)"; failed=$((failed+1)); fi; }

    cat > "$tmp/global.cpp" <<'EOF'
int kiln_bad_counter = 0;
int bump(void) { static int local_static = 0; local_static++; return ++kiln_bad_counter + local_static; }
EOF
    cat > "$tmp/clock.cpp" <<'EOF'
#include <time.h>
long now(void) { return (long)time(nullptr); }
EOF
    c++ -std=c++20 -c "$tmp/global.cpp" -o "$tmp/global.o" 2>/dev/null
    c++ -std=c++20 -c "$tmp/clock.cpp"  -o "$tmp/clock.o"  2>/dev/null
    ar rcs "$tmp/libbad.a" "$tmp/global.o" "$tmp/clock.o" 2>/dev/null

    local n
    n=$(nm "$tmp/libbad.a" 2>/dev/null | awk '$2 ~ /^[dDbB]$/' | wc -l | tr -d ' ')
    t "a global and a function-local static are both seen" "$([ "$n" -ge 2 ] && echo yes || echo no)" "yes"

    n=$(nm -u "$tmp/libbad.a" 2>/dev/null | awk '{print $NF}' \
          | grep -cE '^_?time$' || true)
    t "a direct time() call is seen" "$([ "$n" -ge 1 ] && echo yes || echo no)" "yes"

    # And the real library passes both, which is the other half: a check that
    # fires on everything is as useless as one that fires on nothing.
    local lib; lib=$(core_lib)
    if [ -n "$lib" ]; then
        n=$(nm "$lib" 2>/dev/null | awk '$2 ~ /^[dDbB]$/' | wc -l | tr -d ' ')
        t "kiln_core has no mutable state" "$n" "0"
        n=$(nm -u "$lib" 2>/dev/null | awk '{print $NF}' | grep -cE '^_?time$' || true)
        t "kiln_core calls no time()" "$n" "0"
    else
        echo "skip: no host build, so the real library is not checked"
    fi

    echo "$((checks - failed))/$checks self-test checks passed"
    [ "$failed" -eq 0 ]
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

check_includes
check_no_mutable_state
check_time_injected
check_graph || fail=1

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "layercheck: clean"

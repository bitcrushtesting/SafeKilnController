#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boot the firmware under QEMU and assert that it fires.
#
#   tools/qemu-smoke.sh              build, run for 180 s, assert
#   tools/qemu-smoke.sh --seconds 40 a shorter run, for iterating locally
#   tools/qemu-smoke.sh --log FILE   assert against an existing log, no run
#
# Needs ESP-IDF exported and qemu-xtensa installed:
#     . "$IDF_PATH/export.sh"
#     python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
#
# ===========================================================================
# WHY THIS IS A SCRIPT AND NOT A FEW LINES IN ci.yml
# ===========================================================================
# It was a few lines in ci.yml, and they were run by the IDF container action
# as `bash -c '<the whole block>'`. Everything in the block therefore lives
# inside a single-quoted string, so one apostrophe in a comment or one
# `echo 'text'` ends the quoting and bash exits 2 on a parse error before a
# single line of it runs. That is what happened, and the symptom was a job that
# failed with no output at all, which reads exactly like the job that had been
# failing for a different reason the week before.
#
# A file has no such hazard, it can be run on a developer machine, and the
# assertions can be tested against a recorded log with --log. tools/tidy.sh and
# tools/tidy-target.sh are in the repository for the same reason.
#
# ---------------------------------------------------------------------------
# WHAT IT ASSERTS, AND WHY EACH ONE
# ---------------------------------------------------------------------------
# Booting is not the test. A controller that boots and sits at ambient would
# satisfy every "did it start" check ever written, so the last two assertions
# are the ones that matter: the simulated kiln has to pass 100 degC, and it has
# to get there without latching a fault. A rule that stops a working kiln is
# worse than no rule, because it is the one that gets switched off.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here/.."
fw="$root/firmware/controller"

seconds=180
log=""
run=1

while [ $# -gt 0 ]; do
    case "$1" in
    --seconds) seconds="$2"; shift 2 ;;
    --log)     log="$2"; run=0; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- locate the tools -----------------------------------------------------
#
# `idf.py` is a command on PATH inside the IDF container and a shell ALIAS on a
# developer machine that used the installer, and an alias does not survive into
# a script. Calling the interpreter and the script directly works in both, so
# every invocation below goes through this.
# IDF records the exact interpreter a build directory was configured with and
# refuses to reuse it from another, so this prefers IDF's own virtualenv rather
# than whatever python3 resolves to.
idf_python() {
    if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
        echo "$IDF_PYTHON_ENV_PATH/bin/python"
    else
        echo python3
    fi
}

idf() {
    if command -v idf.py >/dev/null 2>&1; then
        idf.py "$@"
    elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf.py" ]; then
        "$(idf_python)" "$IDF_PATH/tools/idf.py" "$@"
    else
        echo "neither idf.py nor \$IDF_PATH/tools/idf.py is available." >&2
        echo "Run '. \$IDF_PATH/export.sh' first." >&2
        exit 1
    fi
}

# --- locate the emulator --------------------------------------------------
#
# `. export.sh` does NOT put qemu-xtensa on PATH in the IDF container image,
# which is what made `idf.py qemu` exit 127 with "command not found" while the
# tool was installed the whole time. Three ways to find it, cheapest first.
find_qemu() {
    if command -v qemu-system-xtensa >/dev/null 2>&1; then
        command -v qemu-system-xtensa
        return 0
    fi
    # idf_tools knows where it put things; this is the documented route and it
    # does not depend on any layout this script guesses at.
    if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf_tools.py" ]; then
        local line
        line="$(python3 "$IDF_PATH/tools/idf_tools.py" export --format key-value 2>/dev/null \
                  | sed -n 's/^PATH=//p' | head -1)" || true
        if [ -n "$line" ]; then
            local expanded
            expanded="$(eval "echo \"$line\"")"
            local saved="$PATH"
            PATH="$expanded"
            if command -v qemu-system-xtensa >/dev/null 2>&1; then
                command -v qemu-system-xtensa
                PATH="$saved"
                return 0
            fi
            PATH="$saved"
        fi
    fi
    # Last resort: look where idf_tools installs. Reported, not silent, because
    # needing this means one of the two routes above should have worked.
    local found
    found="$(find "${IDF_TOOLS_PATH:-$HOME/.espressif}" -name qemu-system-xtensa \
               -type f -perm -u+x 2>/dev/null | head -1)"
    [ -n "$found" ] && { echo "$found"; return 0; }
    return 1
}

if [ "$run" = "1" ]; then
    qemu="$(find_qemu || true)"
    if [ -z "$qemu" ]; then
        echo "qemu-system-xtensa not found." >&2
        echo "  IDF_PATH=${IDF_PATH:-unset}" >&2
        echo "  IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-unset}" >&2
        echo "Install it with:" >&2
        echo "  python \"\$IDF_PATH/tools/idf_tools.py\" install qemu-xtensa" >&2
        exit 1
    fi
    export PATH="$(dirname "$qemu"):$PATH"
    echo "qemu:    $qemu"
    qemu-system-xtensa --version 2>&1 | head -1 || true

    cd "$fw"
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"
    idf build

    log="$fw/qemu.log"
    echo "running: idf.py qemu for ${seconds}s"

    # The timeout is in python because `timeout` is coreutils and this script
    # also runs on a developer machine that may not have it. SIGINT first so
    # QEMU can exit cleanly, SIGKILL after a grace, and the process group so
    # the emulator goes with the wrapper that started it.
    if command -v idf.py >/dev/null 2>&1; then
        qemu_cmd="idf.py"
    else
        qemu_cmd="$(idf_python) $IDF_PATH/tools/idf.py"
    fi
    python3 - "$seconds" "$log" "$qemu_cmd" <<'PY'
import os, signal, shlex, subprocess, sys, time
seconds, logpath, cmd = int(sys.argv[1]), sys.argv[2], shlex.split(sys.argv[3])
with open(logpath, "wb") as f:
    p = subprocess.Popen(cmd + ["qemu"], stdout=f, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    deadline = time.time() + seconds
    while time.time() < deadline and p.poll() is None:
        time.sleep(1)
    if p.poll() is None:
        os.killpg(os.getpgid(p.pid), signal.SIGINT)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    elif p.returncode != 0:
        # It stopped on its own, which for `idf.py qemu` means it failed.
        print(f"idf.py qemu exited {p.returncode} on its own", file=sys.stderr)
        sys.exit(p.returncode)
PY
    echo "log:     $log ($(wc -c < "$log") bytes)"
fi

# --- assertions -----------------------------------------------------------
[ -s "$log" ] || { echo "::error::QEMU produced no output"; exit 1; }

fail=0
want() {
    if grep -qE "$1" "$log"; then
        echo "ok: $2"
    else
        echo "::error::$2 -- expected /$1/ in the QEMU log"
        fail=1
    fi
}

# Matched against what main.cpp actually logs. An earlier version of this check
# looked for 'Safe Kiln Controller starting', which the firmware has never
# printed, so it could only ever have passed vacuously or failed.
want 'kiln: Safe Kiln Controller .* on esp32s3' 'the composition root ran'
want 'SIMULATED PLANT'                 'the simulated plant was bound'
want 'started "'                       'a program was accepted and started'
want '^\[RUN'                          'the run controller reached Running'
want 'HEAT'                            'the safety supervisor granted heat authority'

# The kiln has to actually get somewhere. The state is padded inside its
# brackets, so strip the whole bracketed field before reading the temperature
# rather than counting on where it lands.
if awk '/^\[/ { gsub(/\[[^]]*\]/, ""); if ($1+0 > 100) found = 1 }
        END { exit !found }' "$log"; then
    echo "ok: the simulated kiln passed 100 degC"
else
    echo "::error::the simulated kiln never passed 100 degC"
    fail=1
fi

# And it has to get there without tripping a rule.
if grep -qE '^\s+FAULT [0-9]+' "$log"; then
    echo "::error::a fault latched during an unperturbed firing"
    grep -E '^\s+FAULT [0-9]+' "$log" | head -5
    fail=1
else
    echo "ok: no fault latched"
fi

exit $fail

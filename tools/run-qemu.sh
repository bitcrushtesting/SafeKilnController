#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the firmware under QEMU with the kiln process simulated.
#
# Usage:  tools/run-qemu.sh [build|run|clean]     (default: build then run)
#
# Requires ESP-IDF 5.2 or newer with QEMU support installed:
#     . $IDF_PATH/export.sh
#     idf.py --preview install-qemu     # or: python -m idf_tools install qemu-xtensa
#
# What this exercises, and what it does not, is set out in firmware/controller/sdkconfig.qemu.
# In short: the real control and safety path against a plant that responds, on
# the target's own compiler and scheduler -- but no drivers and no charge pump.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$here/../firmware"
action="${1:-all}"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set. Run '. \$IDF_PATH/export.sh' first." >&2
    exit 1
fi

cd "$fw"

case "$action" in
clean)
    rm -rf build sdkconfig
    ;;
build|all)
    # The overlay order matters: sdkconfig.qemu wins where the two disagree.
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"
    idf.py set-target esp32s3
    idf.py build
    ;;
esac

case "$action" in
run|all)
    cat <<'BANNER'

--------------------------------------------------------------------
 Safe Kiln Controller under QEMU -- simulated plant, no hardware driven.

 The firing starts by itself.  Keys (press 'h' for the full list):
   s start   a abort   p pause   r resume   c clear fault   i idle
   1 relay fail-on (SWR-SAF-25/SWR-SAF-27)     2 relay fail-off (SWR-SAF-26)
   3 welded contactor (SWR-SAF-27)        4 partial element loss (SWR-SAF-28)
   5 over-current (SWR-SAF-29)            6 CT disconnected (SWR-CUR-11)
   7 SSR shorted (SWR-SAF-08/SWR-SAF-25)       8 thermocouple open (SWR-SAF-04)
   9 thermocouple stuck (SWR-SAF-06)      0 lid open (SWR-SAF-07)
   x clear all injections

 Simulated time runs 60x by default (CONFIG_KILN_SIM_TIME_ACCEL),
 so a four-hour schedule completes in about four minutes.

 Ctrl-] to leave the monitor.
--------------------------------------------------------------------

BANNER
    idf.py qemu monitor
    ;;
esac

#!/usr/bin/env bash
# Host tests for the kilncontrol-fixture fixture firmware. No board, no SDK, no USB.
#
# The protocol core is deliberately free of SDK headers so it can be built against the mock HAL
# and exercised here, which is the only place the JSON layer and the channel tables get checked
# before they reach hardware.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
out="${TMPDIR:-/tmp}/kilncontrol-fixture-host-tests"

mkdir -p "$out"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I "$root/include" -I "$here" \
    -o "$out/test_core" \
    "$here/test_core.c" \
    "$here/mock_hal.c" \
    "$root/src/fixture_core.c" \
    "$root/src/fixture_config.c"

"$out/test_core" "$@"

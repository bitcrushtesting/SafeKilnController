<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Running Safe Kiln Controller against a simulated kiln

The firmware can be run with the kiln process simulated instead of driven, in two
places: on a development host, where the host test suite does it, and on the
target under **QEMU**, where you can watch a firing and inject faults from the
console.

Both work for the same reason: [`AD-01`](architecture.md#3-key-decisions) puts
every hardware access behind an interface header in `kiln_ports`, so the plant
substitutes at the *port* boundary. The code being exercised is the real control
and safety path, not a mock of it.

---

## 1. Host tests

No ESP-IDF needed. Plain CMake, seconds to build, and the whole suite runs in
about four seconds.

```sh
cmake -B build-host -S firmware/controller/test/host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Options, per [architecture §14.2](architecture.md#142-dual-target-build):

| Option | Effect |
|---|---|
| `-DENABLE_ASAN=ON` | AddressSanitizer and UBSan (`TR-20`) |
| `-DENABLE_COVERAGE=ON` | gcov instrumentation (`TR-19`) |

A single suite can be run directly, and takes a substring filter:

```sh
./build-host/test_safety_current sr27
```

`AD-02`'s injected clock is what makes the timings practical: a 168 h firing, a
15 min runaway window and a 30 s fail-off window are all simulated in
milliseconds of wall time, because no core function ever reads a clock.

---

## 2. On the target, under QEMU

```sh
. $IDF_PATH/export.sh
idf.py --preview install-qemu        # once
tools/run-qemu.sh
```

Or by hand:

```sh
cd firmware
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"
idf.py set-target esp32s3
idf.py build
idf.py qemu monitor
```

The firing starts on its own and the console prints a line a second:

```
[RUN   ]   412.8 degC  sp   415.0  rate  +178.3 degC/h  duty  612  I 24.81 A C     ref 29.95  seg 2/5  HEAT  [contactor closed]
```

The `I` column is the gated current measurement and the letter after it is the
flag that says what the measurement *means*
([`FR-CUR-04`](requirements.sdoc)): `C`
conduction, `L` leakage, `S` skipped because the window was too short, `~` stale,
`!` transformer fault. Without that flag a reading of `0.00 A` is uninterpretable,
which is why [`AD-18`](architecture.md#3-key-decisions) put it in the log record
too.

### Keys

| | |
|---|---|
| `s` `a` `p` `r` | start, abort, pause, resume |
| `c` `m` `i` | clear the latched fault, manual 50 %, idle |
| `R` `l` | list stored run records, erase the sample log |
| `h` | help |

Fault injection, each toggling:

> `0` and `d` are deliberately separate. `0` is the plant, an open lid losing
> heat, which is what `SR-07` infers from duty and rate of rise. `d` is the
> interlock *switch*. Keeping them apart is what lets you exercise a switch that
> has failed open on a shut door, or a genuinely open door on a kiln with no
> interlock. A realistic "operator opened the door" is both at once.

| Key | Injection | Rule it exercises |
|---|---|---|
| `1` | Relay fail-on, current with heating commanded off | `SR-25`, then `SR-27` |
| `2` | Relay fail-off, no current with heating commanded on | `SR-26` |
| `3` | Welded contactor, current persists after the contactor opens | `SR-27` |
| `4` | Partial element loss, one group of three | `SR-28` |
| `5` | Over-current | `SR-29` |
| `6` | Current transformer disconnected | `FR-CUR-11`, `FR-CUR-12` |
| `7` | SSR shorted | `SR-08` and `SR-25` together |
| `8` | Thermocouple open | `SR-04` |
| `9` | Thermocouple stuck | `SR-06` |
| `0` | Lid opened mid-firing, the *thermal* model: heat loss, no switch | `SR-07` |
| `d` | Door interlock switch reads open | `SR-31` |
| `D` | No door interlock fitted | warning 113 |
| `x` | Clear every injection | |

Try `3` then `7` together during a firing: the contactor is welded shut *and* the
SSR is passing current, so `SR-25` sees current in an off-window, withholds heat,
opens the contactor, and the current does not stop. That is the discrimination
sequence of `SR-27`, and it latches fault 22 with the instruction to isolate the
kiln at its supply. Press `7` alone and the same symptom resolves to fault 21: the
contactor opened, so it is only the SSR that has failed.

### Time acceleration

A kiln's time constant is tens of minutes, so `CONFIG_KILN_SIM_TIME_ACCEL`
(default 60) multiplies both the plant integration and the `dt` handed to the
acquisition, control and safety cycles, keeping the two consistent. A four-hour
schedule completes in about four minutes.

The 10 ms output window and the current-measurement settle and burst timings are
deliberately **not** accelerated, those are real hardware timings, and leaving
them real is what makes the gating of
[`AD-17`](architecture.md#3-key-decisions) meaningful here.

---

## 3. What QEMU does and does not prove

QEMU's `esp32s3` model provides a CPU, memory, flash and a UART. It does not
model a MAX31856 on SPI, an SSD1306 on I²C, the ADC, or PCNT, so there is
nothing for a thermocouple driver to talk to and nothing for the CT front end to
sample. Substituting the plant at the port boundary is what makes the exercise
possible at all.

**Exercised:** the task structure and its periods, the real `kiln_core` logic end
to end against a plant that responds, the state machine, the window and
current-gating timing, every fault injection, all on the target's own compiler,
scheduler and single-precision FPU.

**Storage is not simulated.** QEMU emulates the flash, so NVS, the `kilnlog`
partition and the `kilnfs` file store are all real: the configuration persists,
the sample log is a real ring on real sectors, programs and run records survive a
reboot, and `SR-17`'s latched fault survives a restart. Press `R` to list stored
run records and `l` to erase the log and watch the ring start over. The simulated
firmware runs exactly the store the board runs ([`AD-21`](architecture.md#3-key-decisions)),
so a program saved under QEMU is still there after a reset.

**Not exercised:** the device drivers; the charge-pump release of
[`AD-05`](architecture.md#3-key-decisions), which is the central safety property
of the design and can only be verified by halting the safety task on real
hardware (`TR-17`, `SR-02`); flash endurance; WiFi; and anything else that needs
a pin. Those are target and HIL work.

The simulator does model the charge pump as a *decay timeout*, so the firmware's
side of the property, that nothing refreshes the pump when the safety task stops,
and the contactor releases, is tested (`ad05_a_stopped_safety_cycle_releases_the_contactor`
in the integration suite). Whether the real circuit reaches the contactor's
drop-out voltage inside [`NFR-04`](requirements.sdoc)'s
one second is a question about resistors, and is open as tasklist item A7.

> **Never flash a `CONFIG_KILN_PLANT_SIM` build to a kiln.** The SSR and contactor
> outputs are not driven at all, and the temperatures on the display are invented.

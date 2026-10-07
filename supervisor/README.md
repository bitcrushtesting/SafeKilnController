<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# The independent safety supervisor

Firmware for the second microcontroller of [`AD-22`](../docs/architecture.sdoc).
It reads the chamber thermocouple, the front end's fault output and the lid
switch, holds one series element in the contactor coil, and reports to the
ESP32 over a one-way serial link. It takes no instruction from anything.

The design, the failure analysis and the open questions are in
[`docs/safety-supervisor.md`](../docs/safety-supervisor.md).

## The part: STM32G031K8T6

| | |
|---|---|
| Part | STM32G031K8T6 |
| Core | Cortex-M0+ at 16 MHz from HSI16, no crystal |
| Memory | 64 KB flash (3.5 KB used), 8 KB RAM |
| Package | LQFP32, 0.8 mm pitch |
| Uses | SPI1, USART2, IWDG, 8 GPIO |

### Pins

Taken from ST's own data: the pinout and alternate functions from CubeMX's MCU
database for this part, the register and bit positions from the CMSIS-SVD.
Neither is a build-time fetch, both ship with ST's tools.

| Pin | Pos | Function | Mode |
|---|---|---|---|
| `PA0` | 7 | clear button | input, pull-up |
| `PA2` | 9 | UART TX to the ESP32 | AF1, `USART2_TX` |
| `PA3` | 10 | MAX31856 `~FAULT` | input, pull-up |
| `PA4` | 11 | MAX31856 `~CS` | output, idle high |
| `PA5` | 12 | SPI1 `SCK` | AF0 |
| `PA6` | 13 | SPI1 `MISO` | AF0 |
| `PA7` | 14 | SPI1 `MOSI` | AF0 |
| `PA8` | 18 | coil permit | output, low opens the coil |
| `PA13` | 24 | `SWDIO` | reserved |
| `PA14` | 25 | `SWCLK` | reserved |
| `PF2` | 6 | `NRST` | |

Three properties of this assignment are deliberate and should survive any
change to it.

**Everything is on port A.** `startup.cpp` drives the permit line low before
`.data` is copied, and enables exactly one GPIO clock to do it. A function
moved to port B would be driven before its port had a clock.

**`PA9` to `PA12` are avoided entirely.** On this package `PA11`/`PA12` can be
remapped to act as `PA9`/`PA10` through `SYSCFG_CFGR1`, and positions 19 and 21
are listed as NC-or-`PA9`/`PA10` depending on bonding. A supervisor should not
depend on a remap bit being right.

**The UART's receive pin is never configured.** `USART2_RX` would be `PA3`, and
`PA3` is the `~FAULT` input instead, so the simplex link is unwired rather than
merely unused. `RE` is left clear in `CR1` for the same reason.

`PA5`/`PA6`/`PA7` are adjacent and all AF0, putting the whole SPI bus on three
neighbouring pins beside the chip select. Free for later: `PA1`, `PA15`,
`PB0`–`PB9`, `PC6`, `PC14`, `PC15`.

One requirement this places on the hardware: the permit line is active high and
every GPIO is high-impedance between reset and the first instruction, so the
series element must be held off by an **external pull-down**, not by this pin.
Without it there is a window at every reset where the element's state is
whatever the board leaks to.

Why this one:

- **The `IWDG` is clocked from the LSI, not the system clock.** This is the
  property that decided the family. A supervisor whose watchdog stops when its
  clock stops is not a supervisor, and on the G0 the independent watchdog runs
  from its own oscillator and cannot be disabled in software once started.
- **LQFP32 rather than QFN.** Hand-solderable, reworkable, and every pin can be
  probed, which matters for a part whose whole argument is that it can be
  verified independently. `HR-19` already requires top-side test points for the
  same reason.
- **64 KB of flash against a firmware that currently uses 3.5 KB.** The
  headroom is not for features. It is for self-tests, and for the fact that a
  part running at 5 % of its flash will still build in ten years.
- Cheap, long-lived and widely stocked, which for a safety part matters more
  than the last few cents. The G0 line also has ST functional-safety material
  available, which is worth having if a claim is ever made formally.

It needs **SWD brought out to test points**, not optional: the independence
argument rests on this firmware being reviewable and flashable without going
through the ESP32.

## Layout

```
supervisor/
  protocol/     the wire format, shared with the ESP32 adapter
  core/         the trip logic: the whole safety function, host-tested
  board/        STM32G031 registers, startup, linker script, pin map
  src/main.cpp  composition root: bring-up and the 10 Hz loop
  test/host/    24 tests, no ARM toolchain needed
```

The split is the one [`AD-01`](../docs/architecture.sdoc) makes on the other
side of the link, for the same reason: everything that *decides* is
platform-free and exercised on a development host, and what needs the silicon
is thin enough to read.

`core/` shares no code with the ESP32's safety rules. That is the point of
`AD-22`; any resemblance is a resemblance and not reuse. The one file both
projects include is `protocol/include/sup_proto.h`, which is declarative, and
the host test harness, which is macros.

## Building

Host tests, which is where the safety logic is verified:

```sh
cmake -B build-sup -S supervisor/test/host
cmake --build build-sup -j
ctest --test-dir build-sup --output-on-failure
```

Target firmware. The compiler ships with STM32CubeCLT, so nothing is fetched at
build time (`CON-04`):

```sh
cmake -B build-sup-target -S supervisor \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/supervisor/arm-none-eabi.cmake
cmake --build build-sup-target -j
```

`board/stm32g031.h` is generated from ST's own CMSIS-SVD by
[`tools/gen-stm32g031-header.py`](../tools/gen-stm32g031-header.py). Regenerate
it rather than editing it: the reason it is generated is that then no address in
it is anybody's recollection.

## What is done, and what is not

**Done and tested:** the trip logic (14 tests) and the wire format (10 tests).
Between them that is the entire safety function and the entire interface, and
neither needs hardware to exercise.

**Written but never run on silicon:** the peripheral bring-up in `main.cpp` and
the alternate-function numbers in `board/pins.h`. The register addresses are
ST's, but the AF mappings live in the datasheet rather than the SVD and are
marked `(confirm)` where they are assumed. The MAX31856 driver is a stub that
returns "not configured", which makes the start-up self-test fail closed, which
is the correct behaviour for a supervisor that cannot read its sensor.

In other words: this will build, flash and refuse to permit heat. That is the
right order to be incomplete in.

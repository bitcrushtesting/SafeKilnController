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
| Core | Cortex-M0+, 64 KB flash, 8 KB RAM |
| Package | LQFP32, 0.8 mm pitch |
| Uses | SPI1, USART2, IWDG, 9 GPIO |

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

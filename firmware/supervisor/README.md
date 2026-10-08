<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# The independent safety supervisor

Firmware for the second microcontroller of [`SWA-22`](../docs/04_software_arch.sdoc).
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
  verified independently. `SYS-HW-19` already requires top-side test points for the
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
firmware/supervisor/
  protocol/     the wire format, shared with the ESP32 adapter
  core/         the trip logic: the whole safety function, host-tested
  board/        STM32G031 registers, startup, linker script, pin map
  src/main.cpp  composition root: bring-up and the 10 Hz loop
  test/host/    24 tests, no ARM toolchain needed
```

The split is the one [`SWA-01`](../docs/04_software_arch.sdoc) makes on the other
side of the link, for the same reason: everything that *decides* is
platform-free and exercised on a development host, and what needs the silicon
is thin enough to read.

`core/` shares no code with the ESP32's safety rules. That is the point of
`SWA-22`; any resemblance is a resemblance and not reuse. The one file both
projects include is `protocol/include/sup_proto.h`, which is declarative, and
the host test harness, which is macros.

## Building

Host tests, which is where the safety logic is verified:

```sh
cmake -B build-sup -S firmware/supervisor/test/host
cmake --build build-sup -j
ctest --test-dir build-sup --output-on-failure
```

Target firmware. The compiler ships with STM32CubeCLT, so nothing is fetched at
build time (`UR-CON-04`):

```sh
cmake -B build-sup-target -S supervisor \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/supervisor/arm-none-eabi.cmake
cmake --build build-sup-target -j
```

`board/stm32g031.h` is generated from ST's own CMSIS-SVD by
[`tools/gen-stm32g031-header.py`](../tools/gen-stm32g031-header.py). Regenerate
it rather than editing it: the reason it is generated is that then no address in
it is anybody's recollection.

## What is done, and what is not

**Done and tested:** the trip logic, the wire format, the MAX31856 register
decode and the self-diagnostics: 68 host tests over four suites, with MC/DC at
100 % over all of it against an 80 % floor.

One coverage gap is worth naming rather than leaving to be found. The two
failure returns inside the RAM pattern test are unreachable from a host test,
because host RAM cannot be made to fail on demand. That is why the comparison
they depend on, `sup_ram_word_ok`, is a separate function with its own tests for
a bit stuck high, a bit stuck low and both polarities of a wrong word. The loop
around it is four lines; the judgement in it is tested. Between them that is the entire safety
function, the entire interface and the arithmetic that turns six register bytes
into a temperature, and none of it needs hardware to exercise. MC/DC is measured
over all three and is at 100 % against an 80 % floor (`tools/mcdc.sh`).

The decode is in `sup_core` rather than beside the SPI code on purpose. Two's
complement reassembled across three registers and shifted back with a
sign-extending shift is not something to get right by inspection: a logical
shift there reads -1 degC as +524287, which is above the backstop, so a cold
kiln would trip. Its test vectors were produced by encoding known temperatures
from the datasheet's register layout, not by recording what the decoder
returned.

**Written but never run on silicon:** the peripheral bring-up in `main.cpp` and
the alternate-function numbers in `board/pins.h`. The register addresses are
ST's, but the AF mappings live in the datasheet rather than the SVD and are
marked `(confirm)` where they are assumed. What is unverified is now the
*transport* rather than the logic: the SPI configuration, the chip-select
timing, the SysTick period and the USART bring-up. Each is a small separate
function so it can be brought up and checked one at a time on a bench.

### Self-diagnostics

Three measures, added because an assessment against EN IEC 60730-1 Annex H or
EN ISO 13849-1 asks for them and a single-channel supervisor can only claim
diagnostic coverage for what it actually observes.

**Clock integrity** (`sup_clock_ok`). The clock switch status must agree with the
selection, HSI must be ready, and the PLL must be off. Every check is phrased as
an invariant that holds whatever the `SWS` encoding turns out to be, because this
part's SVD carries no enumerated values for it and its `RCC_CR` reset value
disagrees with the reference manual. Guessing a bit pattern in a safety check is
not worth the brevity.

**Program memory integrity** (`sup_flash_ok`). CRC-32 over the whole image from
the vector table to `_sup_crc_region_end`, compared against a digest stamped into
`.sup_crc` after linking by `tools/sup-crc.py`. The digest sits immediately after
the region because a digest cannot cover itself, and the tool checks that
invariant rather than trusting the linker script. An **unstamped** image fails:
`SUP_CRC_UNPROGRAMMED` is a failure, not "no expectation", because an image that
reached a board without being stamped is exactly the one whose integrity is
unknown. The stamp is a build step so it cannot be forgotten.

**Working memory** (`SWR-SAF-33`). A pattern test that drives every bit of every
tested word to both states, plus an **address-dependent** pattern. The second one
is required rather than decorative: two words that alias because an address line
is stuck pass every uniform pattern, since both hold the same value and both read
back what was written. They fail only when asked to hold two different values at
once.

The whole free region is tested once before heat can be permitted, and then
walked 64 bytes a cycle for ever. The region walked is the memory between the
stack guard and the current stack pointer, which nothing is using, so no save and
restore is needed and it is exactly the memory the stack will grow into. When the
stack has grown too close to leave a safe window, the test declines that cycle
rather than reporting a failure: a nuisance trip is worse than a missed cycle of
a test that runs ten times a second.

**Stack overflow** (`SWR-SAF-34`). 256 words of guard below the stack, filled at
start-up and checked in full every cycle. Every word, not a sample, because the
stack arrives at the top of the region and a sampled check would miss the shallow
overflow, which is the one still worth catching.

**Program sequence** (`SWR-SAF-35`). The four stages of the cycle announce
themselves, and at the end the supervisor checks that each ran exactly once and in
order. This catches what a watchdog cannot: a cycle that finished on time having
skipped the stage that drives the permit line, or one a corrupted branch entered
halfway through. Both produce a cycle of the right duration doing the wrong work.
The verdict necessarily lands in the following cycle, since a sequence can only be
judged once it has finished, which costs 100 ms against `SWR-NFR-04`'s 500 ms.

**A failed diagnostic is not clearable** (`SWR-SAF-36`). Any diagnostic failure,
at start-up or running, revokes the self-test rather than merely tripping. That
makes it unclearable by the local button and keeps the permit false for good,
because the permit is conjunctive on the self-test. The distinction is deliberate:
the button exists so an operator can acknowledge a condition they can see and have
dealt with, and a supervisor whose memory, stack, clock, image or program sequence
has failed is not in that category. A button that returned it to service would be
overriding the diagnostic rather than the fault.

The diagnostics reach the trip logic through `diag_ok` on the input snapshot,
which is **positive logic** so that a zero-initialised input withholds heat. That
is the convention `chamber_valid` already uses, and for the same reason: the safe
state has to be the one you get by forgetting to set a field. One test does
nothing but check that.

**A windowed watchdog**, which is the clock cross-check and is easy to mistake
for an ordinary watchdog. The IWDG counts from the LSI, an oscillator the system
clock cannot influence. Feeding late resets, as before. Feeding **early**, while
the counter is still above `IWDG_WINDOW`, now also resets, so the 100 ms cycle is
measured against an independent oscillator from both sides. That makes three
previously silent failures loud:

| failure | before | now |
|---|---|---|
| system clock running fast | undetected | early feed resets |
| LSI stopped | **watchdog never fires** | counter never leaves the reload, first feed resets |
| system clock slow, or loop hung | timeout | timeout, unchanged |

The LSI one matters most: a dead LSI meant a watchdog that could never fire, so
the protection the design leans on would have been absent with no indication.

What it does **not** catch is small drift, and the limit is the LSI's own
tolerance rather than a design choice. At 32 kHz ±10 % a 100 ms cycle is 45 to 55
ticks, putting the counter at 120 to 130 when the feed arrives, so the window has
to sit above that or a healthy board resets itself. 140 leaves ten ticks for loop
jitter and detects a system clock roughly 1.3× fast or worse. That is
gross-failure detection. A 2 % drift is not caught and does not need to be: it
does not threaten the safety function, whereas a nuisance reset of the supervisor
would be worse than the fault.

Every feed in the firmware sits immediately after `wait_for_cycle_end()`, which
is the one place a full cycle is known to have elapsed. With a window, that is no
longer a style preference.

The start-up self-test is real rather than a stub returning "not configured". It
writes MASK, CR1 and CR0, reads CR1 back to tell a configured part from a dead
bus, and then waits for a conversion to actually complete before the trip logic
is allowed to run at all. That last step is not ceremony: before the first
conversion the temperature registers read zero, and zero decodes as a
plausible, in-range, fault-free 0 degC. It is the one reading that would permit
heat on a kiln whose temperature is not yet known.

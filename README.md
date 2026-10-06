<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# KilnControl

An open-source PID controller for electric ceramic and glass kilns, built on the
**ESP32-S3** with **ESP-IDF**.

- **Simple local display**: a 128×64 OLED showing current and target temperature at a glance, plus state, segment progress and rate of rise.
- **Web interface**: served by the device itself. Live dashboard, charts of the logged data, power and energy, and a firing-curve editor. **Observation only**: nothing reachable over the network can start a firing, heat the kiln or change its configuration. Those live on the device.
- **Three-phase aware**: a strap selects 1 or 3 phase, with one current transformer per phase, so a failed element on any phase is caught electrically in seconds rather than inferred from temperature.
- **PID with automatic tuning**: relay (Åström–Hägglund) autotune on the real kiln; no manual gain hunting.
- **Safety first**: thermal runaway, thermocouple failure, shorted-SSR, over-temperature and door-interlock detection, with a safety supervisor that has sole authority over a heat-enable line that decays unless actively refreshed.
- **Self-contained**: no SD card, no external database, no cloud. Logs live in a circular partition on internal flash; web assets are embedded in the firmware.
- **Designed for testability**: all decision logic is hardware-free C++ that runs on a development host against a simulated kiln.

## Status

**In development.** The firmware is C++20. The control, safety and web logic is
implemented and tested; the hardware adapters and the local display are not,
and there is currently **no field update path** (see below).

347 host tests pass plain and under AddressSanitizer/UBSan, `clang-tidy` is
clean on host and target, the `esp32s3` image builds with zero warnings at
237 kB (89 % of the OTA slot free), and QEMU boots that image and fires it.

| Area | State |
|---|---|
| `kiln_core`: PID, setpoint generator, program model, safety supervisor (including the current-based relay rules), autotune, heater-current measurement, configuration model, run state, log codec | Implemented, host-tested |
| `kiln_ports`: the interface headers everything hardware goes through | Complete for the above |
| `kiln_sim`: plant simulator with heater current and electrical fault injection | Implemented |
| `kiln_app`: task orchestration, mode state machine, heat authority, logging, persistence, power-loss recovery | Implemented, host-tested |
| `kiln_hal_esp32s3`: log partition, NVS, clock, reset cause, watchdog | Implemented, builds for esp32s3 |
| `kiln_hal_esp32s3`: MAX31856, SSD1306, encoder, SSR outputs, CT front end, LittleFS | **Not started** |
| `kiln_web`: REST API, JSON, log streaming, read-only enforcement | Implemented, host-tested |
| `kiln_hmi`: the local display and encoder | **Not started**, and now the only way to start a firing |
| Firmware update | **Removed from the network** (FR-UPD-01); no local path specified yet |

You can watch a complete firing, and break it in a dozen ways, without any
hardware at all, see [`docs/simulation.md`](docs/simulation.md):

```sh
tools/run-qemu.sh          # the firmware on an emulated ESP32-S3, kiln simulated
```

or run the logic tests directly:

```sh
cmake -B build-host -S firmware/test/host && cmake --build build-host
ctest --test-dir build-host
```

CI runs the layering and licence checks, the host suites plain and under
AddressSanitizer/UBSan, a coverage gate, the `esp32s3` build with a size report,
and a QEMU job that boots the real image and watches it fire. Tagging `v*`
builds a release.

| Document | Contents |
|---|---|
| [`docs/requirements.md`](docs/requirements.md) | Requirements specification, functional, safety, non-functional, hardware-interface and testability requirements, each with an identifier and a verification method. |
| [`docs/safety.md`](docs/safety.md) | Safety concept, hazard analysis, safety goals, the layered protection concept and the independence claimed between layers, detection coverage and timing, residual risk, and the obligations on the installer and on anyone changing the design. |
| [`docs/security.md`](docs/security.md) | Security concept, assets, attack surface and trust boundaries, threats and the controls against them (marked built / specified / absent), why a security compromise here is a safety event, and the residual risk. |
| [`docs/architecture.md`](docs/architecture.md) | Software architecture, key decisions, component decomposition, task and timing design, control and safety algorithms, persistence and flash-endurance design, REST API, and the build and test architecture. |
| [`docs/simulation.md`](docs/simulation.md) | Running the firmware against a simulated kiln, on the host and under QEMU, including fault injection. |
| [`tasklist.md`](tasklist.md) | Outstanding work, by priority. |

Start with [`docs/requirements.md`](docs/requirements.md); the architecture
document cites it throughout.

## Repository layout

```
docs/       requirements and architecture
firmware/   ESP-IDF application (not yet implemented)
hardware/   schematic, PCB, pin map
housing/    enclosure
```

## Hardware at a glance

| Part | Choice |
|---|---|
| MCU | ESP32-S3, ≥ 8 MB flash, no PSRAM required |
| Temperature | 2 × MAX31856 (chamber + enclosure), type-K by default |
| Display | 128×64 monochrome OLED, I²C |
| Input | Rotary encoder with push button |
| Output | Zero-cross SSR in series with a safety contactor |
| Phases | Strap-selected 1 or 3 phase, one current transformer per phase |
| Door interlock | Optional normally-closed switch, stops the heater immediately when the door opens |
| Connectivity | WiFi station with access-point fallback, `kiln.local` via mDNS |

Details and rationale are in
[requirements §6](docs/requirements.md#6-hardware-interface-requirements).

## Credit

Functionally inspired by [**PIDKiln**](https://github.com/Saur0o0n/PIDKiln) by
Adrian Siemieniak, which showed that a low-cost ESP32 can run a real kiln well.
KilnControl borrows its ideas, segment-based programs, dual local/web control,
on-device storage, a redundant SSR + contactor output stage, and rebuilds them
on ESP-IDF with a host-testable core and automatic PID tuning. No PIDKiln source
code is used;
[requirements §1.6](docs/requirements.md#16-relationship-to-pidkiln) records what
was adopted and what deliberately differs.

## Safety

KilnControl is **not** a safety-certified device. A kiln is a multi-kilowatt mains
heater reaching temperatures at which its own wiring and the surrounding building
are at risk.

- An **independent hardware over-temperature cutout** is required in the safety chain, in addition to this controller.
- Mains wiring must be carried out by a competent person in accordance with local regulation.
- Do not fire unattended.

The hazards, the layered protection concept and the risk that remains are
set out in [`docs/safety.md`](docs/safety.md); the requirements it derives from
are [requirements §5](docs/requirements.md#5-safety-requirements).

KilnControl is designed for a **trusted local network** and must not be exposed
to the internet: there is no transport encryption, and an attacker who can issue
commands can start a multi-kilowatt heater. The threat model, the controls and
what is still only specified are in [`docs/security.md`](docs/security.md).

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

    Copyright (C) 2026 Bitcrush Testing

    This program is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
    more details.

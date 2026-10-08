<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Safe Kiln Controller

[![CI](https://img.shields.io/github/actions/workflow/status/bitcrushtesting/SafeKilnController/ci.yml?branch=main&label=CI&style=flat-square)](https://github.com/bitcrushtesting/SafeKilnController/actions/workflows/ci.yml)
[![Coverage gate](https://img.shields.io/badge/coverage%20gate-%E2%89%A5%2090%25%20lines-blue?style=flat-square)](.github/workflows/ci.yml)
[![MC/DC gate](https://img.shields.io/badge/MC%2FDC%20gate-%E2%89%A5%2080%25-blue?style=flat-square)](tools/mcdc.sh)
[![clang-tidy](https://img.shields.io/badge/clang--tidy-host%20%2B%20target-blue?style=flat-square)](.clang-tidy)
[![Requirements](https://img.shields.io/badge/requirements-StrictDoc%20validated-blue?style=flat-square)](docs/03_software_req.sdoc)
[![Licence](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue?style=flat-square)](LICENSE)

An open-source PID controller for electric ceramic and glass kilns, built on the
**ESP32-S3** with **ESP-IDF**.

- **Simple local display**: a 128×64 OLED showing current and target temperature at a glance, plus state, segment progress and rate of rise.
- **Web interface**: served by the device itself. Live dashboard, charts of the logged data, power and energy, and a firing-curve editor. **Observation only**: nothing reachable over the network can start a firing, heat the kiln or change its configuration. Those live on the device.
- **PID with automatic tuning**: relay (Åström–Hägglund) autotune on the real kiln; no manual gain hunting.
- **Safety first**: thermal runaway, thermocouple failure, shorted-SSR, over-temperature and door-interlock detection, with a safety supervisor that has sole authority over a heat-enable line that decays unless actively refreshed.
- **Current monitoring**: a current transformer turns relay and element failures from slow thermal inferences into fast electrical facts, with the thermal rules retained as an independent backstop.
- **Self-contained**: no SD card, no external database, no cloud, no filesystem. Logs live in a circular partition on internal flash and programs in a fixed-slot one, each built so a power cut cannot tear a record; web assets are embedded in the firmware.
- **Designed for testability**: all decision logic is hardware-free C++ that runs on a development host against a simulated kiln.

## Status

**In development.** The firmware is C++20. The control, safety, storage and web
logic is implemented and tested, and the hardware adapters are written but have
not yet been run against a board. The browser UI is not served yet, its assets
are not embedded in the image, and there is currently **no field update path**
(see below).

398 host tests pass plain and under AddressSanitizer/UBSan, `clang-tidy` is
clean on host and target, the `esp32s3` image builds with zero warnings at
866 kB (58 % of the OTA slot free), and QEMU boots that image and fires it.

| Area | State |
|---|---|
| `kiln_core`: PID, setpoint generator, program model, safety supervisor (including the current-based relay rules), autotune, heater-current measurement, configuration model, run state, log codec, log ring, file store | Implemented, host-tested |
| `kiln_ports`: the interface headers everything hardware goes through | Complete for the above |
| `kiln_sim`: plant simulator with heater current and electrical fault injection | Implemented |
| `kiln_app`: task orchestration, mode state machine, heat authority, logging, persistence, power-loss recovery | Implemented, host-tested |
| `kiln_hal_esp32s3`: log partition, file store, NVS, clock, reset cause, watchdog | Implemented, builds for esp32s3 |
| `kiln_hal_esp32s3`: MAX31856, SSD1306, encoder, SSR outputs, CT front end, WiFi | Implemented, **not yet run against hardware** |
| `kiln_web`: REST API, JSON, log streaming, read-only enforcement | Implemented, host-tested; HTTP transport on target, **browser assets not yet embedded** |
| `kiln_hmi`: the local display and encoder | Implemented and host-tested, **not yet run against hardware**; the only way to start a firing |
| Firmware update | **Removed from the network** (SWR-UPD-01); no local path specified yet |

You can watch a complete firing, and break it in a dozen ways, without any
hardware at all, see [`docs/simulation.md`](docs/simulation.md):

```sh
tools/run-qemu.sh          # the firmware on an emulated ESP32-S3, kiln simulated
```

or run the logic tests directly:

```sh
cmake -B build-host -S firmware/controller/test/host && cmake --build build-host
ctest --test-dir build-host
```

CI runs the layering and licence checks, the host suites plain and under
AddressSanitizer/UBSan, a coverage gate, the `esp32s3` build with a size report,
and a QEMU job that boots the real image and watches it fire. Tagging `v*`
builds a release.

## Putting a unit into production

Two tools, in this order, because the order is not interchangeable:

```sh
tools/prod-data.py flash --serial SK1-2026-000042   # identity, a plain write
tools/secure-boot.py provision --key keys/prod.pem  # dry run; add --commit
```

The production data block first, while the device is still freely writable.
Then signing, flashing and lockdown.

The two differ in what they do by default, and the difference is the point.
`prod-data.py` writes unless told `--dry-run`, because its write is a plain
flash write that a reflash undoes. `secure-boot.py` does *nothing* unless told
`--commit`, because most of what it does cannot be undone at all. Both carry a
`--self-test` that needs neither a board nor ESP-IDF.

**`secure-boot.py` burns eFuses, and three of its four steps cannot be undone.**
It refuses to touch a board until its pre-flight passes, demands a differently
worded confirmation for each irreversible step, and sequences them so the device
is left bootable at every point. Read the header of
[`firmware/controller/sdkconfig.secure`](firmware/controller/sdkconfig.secure) before the first run;
the short version is that **losing the signing key means the unit can never run
new firmware again**, and that secure boot stops hostile code running but does
nothing about secrets being read off the flash (`SRR-05`).

It deliberately does not burn `DIS_DOWNLOAD_MODE`. `SRR-11` records that this
device has no field update path, so serial download is the only route a security
fix can take; closing it as well would make a unit permanently unfixable.

| Document | Contents |
|---|---|
| [`docs/v-model-process.md`](docs/v-model-process.md) | The development process: the V-model levels, which document holds each, the relations between them, and an honest record of which levels exist and which do not. |
| [`docs/01_user_req.sdoc`](docs/01_user_req.sdoc) | User requirements: the target markets (the European Union), the regulatory framework that follows from them with instrument numbers and editions, the user needs with their acceptance criteria, and the project constraints. |
| [`docs/02_system_req.sdoc`](docs/02_system_req.sdoc) | System requirements: the hardware interface, the four safety requirements that hardware discharges, and the assumptions about the environment the system is placed in. |
| [`docs/03_software_req.sdoc`](docs/03_software_req.sdoc) | Software requirements: the functional, safety, non-functional and testability requirements, each with an identifier and a verification method. |
| [`docs/04_software_arch.sdoc`](docs/04_software_arch.sdoc) | Software architecture: the 22 decisions the design rests on, each with the requirement that drove it and the cost it carries. |
| [`docs/safety.sdoc`](docs/safety.sdoc) | Hazards, safety goals and residual risks, each with an identifier, and the chain between them as checked relations: which hazards a goal mitigates, which requirements realise it, and which risk its layers leave. |
| [`docs/safety.md`](docs/safety.md) | Safety concept: the system boundary, the layered protection concept and the independence claimed between layers, detection coverage and timing, the reaction and recovery sequence, and the obligations on the installer and on anyone changing the design. |
| [`docs/security.sdoc`](docs/security.sdoc) | Assets, adversaries, threats, security goals, residual risks and open questions, each with an identifier, and the chain between them as checked relations: which asset a threat is aimed at, which safety hazard it reaches, which goal answers it, and what each goal's implementation status actually is. |
| [`docs/security.md`](docs/security.md) | Security concept: scope, the attack surface and trust boundaries, why a security compromise here is a safety event, what the implementation already gets right, the obligations on the owner and on whoever implements the HTTP transport, and the verification status. |
| [`docs/architecture.md`](docs/architecture.md) | Architecture prose: component decomposition, task and timing design, control and safety algorithms, persistence and flash-endurance design, REST API, and the build and test architecture. |
| [`docs/test-concept.md`](docs/test-concept.md) | How the product is verified: unit, integration, system and hardware-in-the-loop, what each level can and cannot prove, the HIL fixture design, and an honest status against every testability requirement. |
| [`docs/safety-supervisor.md`](docs/safety-supervisor.md) | The independent safety supervisor (`SWA-22`): a second microcontroller holding the absolute over-temperature, thermocouple-fault and lid trips, what moves and what stays, the link, the failure modes, and the questions still open. |
| [`docs/bom-optimisation.md`](docs/bom-optimisation.md) | Parts the design might do without or do more cheaply, each worked through to a recommendation with the requirements a change would touch. Candidates, not decisions. |
| [`docs/simulation.md`](docs/simulation.md) | Running the firmware against a simulated kiln, on the host and under QEMU, including fault injection. |
| [`tasklist.md`](tasklist.md) | Outstanding work, by priority. |

Start with [`docs/03_software_req.sdoc`](docs/03_software_req.sdoc); the architecture
document cites it throughout.

## Repository layout

```
docs/       requirements and architecture
firmware/controller/   ESP-IDF application (not yet implemented)
hardware/   schematic, PCB, pin map
housing/    enclosure
```

## Scope

Safe Kiln Controller supports **single-phase kilns only**. A three-phase kiln can be
monitored on one representative phase, but a fault confined to one of the other
two would be caught only by the thermal rules, slowly, and the power and energy
figures would cover a third of the load. Neither is a safe basis for firing a
three-phase kiln, so it is out of scope rather than partially supported.

## Hardware at a glance

| Part | Choice |
|---|---|
| MCU | ESP32-S3, ≥ 8 MB flash, no PSRAM required |
| Temperature | 2 × MAX31856 (chamber + enclosure), type-K by default |
| Display | 128×64 monochrome OLED, I²C |
| Input | Rotary encoder with push button |
| Output | Zero-cross SSR in series with a safety contactor |
| Supply | **Single phase only.** One current transformer on the heater conductor |
| Door interlock | Optional normally-closed switch, stops the heater immediately when the door opens |
| Connectivity | WiFi station with access-point fallback, `kiln.local` via mDNS |

Details and rationale are in
[requirements §6](docs/03_software_req.sdoc).

## Credit

Functionally inspired by [**PIDKiln**](https://github.com/Saur0o0n/PIDKiln) by
Adrian Siemieniak, which showed that a low-cost ESP32 can run a real kiln well.
Safe Kiln Controller borrows its ideas, segment-based programs, dual local/web control,
on-device storage, a redundant SSR + contactor output stage, and rebuilds them
on ESP-IDF with a host-testable core and automatic PID tuning. No PIDKiln source
code is used

## Safety

Safe Kiln Controller is **not** a safety-certified device. A kiln is a multi-kilowatt mains
heater reaching temperatures at which its own wiring and the surrounding building
are at risk.

Mains wiring must be carried out by a competent person in accordance with local regulation.

The hazards, the layered protection concept and the risk that remains are
set out in [`docs/safety.md`](docs/safety.md); the requirements it derives from
are [requirements §5](docs/03_software_req.sdoc).

Safe Kiln Controller is designed for a **trusted local network** and must not be exposed
to the internet. The threat model, the controls and what is still only specified are in [`docs/security.md`](docs/security.md).

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

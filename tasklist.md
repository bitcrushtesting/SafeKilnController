<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Safe Kiln Controller Task List

**Outstanding work only.** An item leaves this file when it is done; what was
done and why is in the commit that did it. Item IDs are stable and cited from
commit messages, so gaps in the numbering are closed items.

Priorities: **P1** safety-relevant or blocking, before the next board revision
and before any firing. **P2** needed for a release that claims the
requirements are met. **P3** polish.

## State, 2026-10-10

| | |
|---|---|
| **Firmware logic** | Complete and tested. 26 host suites green plain and under ASan/UBSan, `kiln_core` lines 91.8 %, clang-tidy clean on host and target, both IDF configurations build. QEMU boots and fires the simulated image with seven assertions. |
| **Firmware on hardware** | 15 of 16 ports wired; only `update` has nothing behind it (`H2`). **None of it has run against hardware** (`L2`, `O3`, `P4`, `M3`). |
| **Schematic** | ERC clean apart from one known `SDO` false positive. Carries the lid interlock; does not carry the supervisor (`R6`). |
| **PCB** | Placement started, **not routed at all** (`A23`). |
| **Documents** | Requirements, architecture, safety, security and the supervisor's unit design all build in CI and gate it. |

---

## A. Schematic and PCB

The board-level specification is now
[`docs/03_hardware_req.sdoc`](docs/03_hardware_req.sdoc) (`HWR-01` to
`HWR-23`), which carries the numbers and the reasoning. These are the open
changes against it.

- [ ] **A1. Pull up the MAX31856 `~DRDY` outputs.** Open-drain, read as
  asserted when floating. `HWR-06`.
- [ ] **A3. Bypass `VBIAS`.** R24/R25 present ~5 kΩ with no local
  capacitance. `HWR-08`.
- [ ] **A4. Fix the CT anti-alias filter.** Corner ≈ 723 Hz sits above a
  1 kHz rate's Nyquist. Pick the sample rate first. `HWR-10`.
- [ ] **A5. Rescale the CT front end.** A 30 A/1 V CT clips across the upper
  half of `SWR-CUR-02`'s 0–60 A. `HWR-09`.
- [ ] **A6. Define `CURR_SENSE` with no CT fitted.** Open input drifts, so
  "disconnected" and "zero current" are indistinguishable. `HWR-11`.
- [ ] **A7. Work out the charge-pump release threshold. P1.** The circuit the
  whole `SYS-SAF-02` property rests on: pumped level ≈ 2.7 V against an
  AO3400A characterised at 4.5 V, decay τ ≈ 155 ms. `HWR-01`.
- [ ] **A8. Budget the +5 V rail against coil inrush.** `HWR-14`.
- [ ] **A9. Confirm the 5 V contactor coil is a real part choice.** Mains
  contactors with 5 V coils are uncommon; a 12 V or 24 V coil changes the rail.
- [ ] **A10. Decouple `VBUS`.** `HWR-17`.
- [ ] **A11. Record the ERC exclusion for the shared SPI `SDO`.** `HWR-22`.
- [ ] **A12. ESD protection on the USB data pair.** `HWR-17`.
- [ ] **A13. Clamp the thermocouple inputs.** `HWR-08`.
- [ ] **A14. Pull up `TC1_CS` and `TC2_CS`.** Both float through reset.
  `HWR-07`.
- [ ] **A15. Pull up `MCU_IO0`.** SW2 pulls it low with nothing holding it
  otherwise. `HWR-18`.
- [ ] **A16. Check the AMS1117's dropout and thermal margin.** `HWR-15`.
- [ ] **A17. Transient protection on `V_IN`.** `HWR-16`.
- [ ] **A18. Resolve the "(opt)" annotations.** `HWR-13`.
- [ ] **A19. Put the CT specification on the schematic as a note.** `HWR-13`.
- [ ] **A20. Review the encoder input network.** `HWR-18`.
- [ ] **A21. One generated pin map.** Assignments live in three places that
  can disagree. `HWR-21`.
- [ ] **A23a. Finish the KiCad rename to `safekiln`.** Files are renamed;
  check no stale references remain.
- [ ] **A23. Route the board, and review the mains-adjacent nets. P1.** No
  tracks and no vias exist yet. `HWR-23`.

---

## C. Build, test and CI infrastructure

- [ ] **C6. Publish the requirement-to-test matrix.** Use StrictDoc's own
  traceability rather than building `tools/trace`; have the `requirements` job
  publish it. Today 147 of 268 requirements appear in no test file.
- [ ] **C10. Gate branch coverage on the safety decisions.** Lines are gated
  at 90 %; `SWR-TST-19`'s 100 % of safety branches is not. Last measured 81 %.
- [ ] **C11. `main.cpp` and `httpd.cpp` are analysed by nothing.** Both
  scripts miss them; the set should be computed as "in the target build, not in
  the host build" so it cannot drift. 102 findings, 54 of them `ESP_LOGx` and
  `ESP_ERROR_CHECK` wanting the component-scoped exemption `kiln_hal_esp32s3`
  already has.
- [ ] **C12. Build the HIL fixture. P1.** Designed in section 5 of
  [`docs/test-concept.md`](docs/test-concept.md). The thermocouple simulator
  has to subtract the cold-junction compensation the MAX31856 adds, and the
  current side needs a fixture-controlled bypass across the load rather than a
  signal generator, because `SWR-SAF-27` discriminates a shorted SSR from a
  welded contactor by dropping the contactor and re-measuring. The scenario it
  exists for: hold the ESP32 in reset, drive the chamber above 1350 °C, confirm
  the contactor opens.

---

## D. Documentation and open questions

- [ ] **D5. Write the manuals `SWR-NFR-26` lists:** assembly and wiring, mains
  safety, commissioning, autotuning, program authoring, the REST API, the log
  format, CT fitting and calibration.
- [ ] **D6. Close `OQ-01`** (cone-based targets), **`OQ-03`** (whole-life run
  retention), **`OQ-05`** (3-zone variant, which decides whether the control
  path is written for one zone or N), **`OQ-07`** (element temperature
  coefficient measured or entered).

---

## F. Static analysis

Both are decisions rather than tasks: the work is understood, the question is
whether it is worth its cost.

- [ ] **F3. `cppcoreguidelines-use-enum-class`.** 31 enums, 223 enumerators,
  2 530 references. The C-callability objection is settled (nothing is
  `extern "C"` but `app_main`), so what remains is the project's vocabulary:
  `KILN_FAULT_DOOR_OPEN` is greppable from `SWR-SAF-31`, and scoping renames
  all 223. Two bit-position enums would get worse, gaining a `static_cast` at
  every mask site.
- [ ] **F9. `kiln_run_record_t` wastes 9 padding bytes.** Reordering
  invalidates every run record on every device, so it can only move with a
  record-format version bump, if at all.

---

## G. Door interlock, SWR-SAF-31

- [ ] **G2. HIL: confirm the series contact breaks the coil. P1.** Open the
  door with the safety task halted and watch the contactor. Cannot be settled
  in simulation.
- [ ] **G3. Decide whether the interlock is mandatory.** `SYS-HW-21` is a
  *should* because many kilns have no door furniture to take a switch; a
  *shall* makes the controller unfittable to them. The consequence is `RR-10`,
  and warning 113 makes the gap visible rather than silent. Revisit after the
  first real installations.

---

## H. Field update and local control

- [ ] **H2. The update path is specified and unbuilt. P2, blocks release
  (`SRR-11`).** `SWR-UPD-09` to `SWR-UPD-16`, with
  [`docs/architecture.md` §13.5](docs/architecture.md) the sequence. In
  dependency order:

  1. **Generate the manifest signing key** on a machine it can then leave.
     `tools/update-manifest.py keygen` exists; the key does not, so
     `update_pubkey.h` is uncommitted and nothing verifies anything.
  2. **The verifier, host-tested first:** signature, semver against
     `kiln_fw_info_t::version`, target, and refusing anything not strictly
     newer. Pure logic, and the suite should beat on a tampered manifest, a
     wrong key, a truncated image and a replayed release.
  3. **The adapter:** `esp_https_ota` behind `check` and `install`, streaming
     into the inactive slot. The partitions already exist.
  4. **The screen** (`SWR-HMI-16`), the only place an install is authorised,
     so part of the security design rather than of the interface.
  5. **The late `confirm_running()`** of `SWR-UPD-15`, called once the
     supervisor link, both couples, the config and the display have proved
     themselves. Calling it at start-up is the defect the requirement exists
     to prevent.
  6. **`update.check_enabled` and `update.url`,** with the opt-out reachable
     from the display.
  7. **`tools/update.py`** for `SWR-UPD-14`'s USB recovery: write `ota_0`,
     erase `otadata`, leave `nvs`, `kilnfs`, `kilnlog` and `prod` alone.

  Not firmware, and not discharged by any: the release channel with an
  advisory per security release, the declared support period (`N3`), and the
  CRA Article 14 reporting route (`N2`), live since **11 September 2026**.

  The security events of `SWR-LOG-16` that carry a version and a refusal
  reason belong here: neither fits a 20-byte record.

- [ ] **H3. Local control is the only control, and nobody has used it.**
  Starting, pausing, aborting and acknowledging are reachable only through
  `kiln_hmi`, which is verified by pixel counts (`M3`) with an encoder
  direction that is a guess (`M5`).

---

## J. German translation, SWR-NFR-23

- [ ] **J3. Config descriptions are English.** `/api/config` keys are an API
  contract and stay; the human-readable descriptions beside them are not
  translated.
- [ ] **J4. No native-speaker review.** `VERSCHWEISST` for a welded contactor
  and `KEINE WAERME` for no heat are the two to check first, being the most
  safety-critical messages a German operator would act on.

---

## K. Hardware interlock chain

Superseded by the supervisor: `SYS-HW-24` is rewritten rather than deleted,
and the chain is not built. What is left is unrelated to it.

- [ ] **K3. `D7` is an unwired LED.** Pre-existing: the coil indicator's
  cathode is on `COIL_DRV` and its anode goes nowhere. Taking it from
  `LID_SWITCH` rather than `+5V` makes it indicate "coil energised" rather
  than "the MCU asked".
- [ ] **K4. Depopulate the chain's parts from the board.** `J9`, `Q5`, `Q6`
  and `R28`–`R31` are placed for an interlock the design dropped. The larger
  half of this is `R6`, the supervisor the board does not carry.

---

## L. Target adapters

- [ ] **L2. None of this has touched hardware. P1.** Every adapter compiles
  and passes analysis, and that is the whole of the evidence. QEMU emulates
  none of SPI, I2C, PCNT or the ADC, so the MAX31856 register decode, the
  SSD1306 init sequence, the quadrature decoding and the ADC scaling are all
  unverified against a real part. The largest untested surface in the project.

---

## M. kiln_hmi, the local interface

- [ ] **M3. Nobody has seen a glyph on real glass. P1.** The init sequence,
  the page addressing and the 5x7 font are unverified against hardware, and
  layout judgements, whether 15x21 is legible at two metres, cannot be made
  from a test.
- [ ] **M5. The encoder direction is a guess.** If the knob turns the menu the
  wrong way, swap the two `pcnt_channel_set_edge_action` pairs. A one-line fix
  that will be wrong half the time until somebody turns a real knob.

---

## N. Security obligations and the Cyber Resilience Act

Inventory in [`docs/security.md` §13](docs/security.md).

- [ ] **N2. The Article 14 procedure, and the mailbox. P2.** `SECURITY.md`
  names `security@bitcrushtesting.com` as the Article 13(17) contact; **that
  mailbox has to exist and be read by a person.** Behind it: a written
  procedure for an actively exploited vulnerability: ENISA and the relevant
  CSIRT within 24 hours, notification within 72, final report within 14 days,
  users told without undue delay. Live from **11 September 2026**, and it does
  not wait for firmware. `SRR-14`.
- [ ] **N3. Choose the support period and declare it. P2.** At least five
  years (`SWR-UPD-16`); the service life of a kiln controller is well beyond
  that. The number gates what users are told, how long the SBOM is kept, and
  how long the update host and its signing key must exist (`SRR-12`). Half of
  `OQ-R5`; the other half is the conformity route, where a kiln controller
  appears in neither Annex list, so self-assessment should be available, but
  record the conclusion rather than assume it.
- [ ] **N4. Two loose ends on the SBOM.** Retention for the support period
  needs `N3` answered. `--require-licences` can be turned on once four
  ESP-IDF v6.0.1 components resolve: `cmock`, `esp_netif_stack`,
  `http_parser` and `protobuf-c` carry no SPDX tag, three ship a licence file
  and `esp_netif_stack` ships neither, which is worth raising upstream. The
  flag stays off by default rather than gating a release on a vendor tree's
  tagging habits.

  Also unowned, and process rather than requirements: somebody has to read
  ESP-IDF and mbedTLS advisories, and the Annex II information for users has
  to be written.
- [ ] **N5. Two decisions that are now requirements. P2.** `OQ-S3`, flash
  encryption: the Act requires stored data to be protected by
  state-of-the-art means and `net.wifi_pass` is plaintext in NVS. And secure
  boot, which exists as an opt-in provisioning step, should be **on by default
  for a sold unit**, a production-line decision nobody has recorded. Both may
  legitimately differ between the two routes to market; what cannot happen is
  either staying open while units ship.

  Framing worth keeping: `security.md` §13.1 marks six Annex I Part I items as
  **evidence rather than work**, because the read-only interface, the absence
  of `malloc`, the isolation of control from the network and the test
  apparatus already satisfy them. The job is citing them in a conformity file.

---

## O. WiFi, FR-NET

- [ ] **O3. Untested against a radio. P1.** Nobody has watched it associate,
  recover from a dropped connection, or sync time. The WiFi setup matters more
  than the rest because it is now the **only** way credentials reach the
  device: a scan that returns nothing, or a join that silently fails, leaves a
  kiln with no network and no second route to one.

  First hardware session, in order: scan in a crowded band, join WPA2 with a
  passphrase containing symbols, power-cycle and confirm it comes back, then
  join a different network to confirm it replaces rather than accumulates.

---

## P. HTTP transport and the API

- [ ] **P4. `esp_http_server` has never served a request. P1.** The API layer
  has been driven over real sockets by the development harness; the transport
  has not. Unverified: URI splitting in its parser, `httpd_resp_send_chunk`
  against a real client, the socket limit with two sessions parked on streams,
  the async handler's lifetime when a tab closes mid-push, and whether a slow
  client can hold the shared 4 kB response buffer long enough to matter.

---

## R. The independent safety supervisor, SWA-22

The firmware, the requirement deltas and the link tests are done. What is left
is hardware.

- [ ] **R6. Draw the supervisor. P1.** Part and pin map are settled:
  STM32G031K8T6, LQFP32, assignment in
  [`firmware/supervisor/README.md`](firmware/supervisor/README.md). Three
  things the pin map asks of the board: the permit line needs an external
  pull-down (`HWR-03`), SWD must come out to test points (`HWR-19`), and on the
  ESP32 side the link lands on one receive-only pin, and `UART0` is the console
  and must not be used.
- [ ] **R11. The clear button on the panel.** The firmware implements it
  (edge triggered, held 0.5 s, a line stuck low never arms). What is left is
  physical: the button, its position relative to the HMI, and whether it is
  labelled as clearing the *supervisor* or clearing *a fault*, which are not the same
  thing, and the operator cannot see the difference. There are two
  acknowledgements and the panel must not imply there is one.

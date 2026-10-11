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
| **Schematic** | ERC clean apart from one known `SDO` false positive. Carries the lid interlock, the supervisor (`U5`, STM32G031K8Tx), both MAX31856 and the SWD tag-connect. |
| **PCB** | 182 tracks and 9 vias placed, so routing is started and not finished. 14 of 125 footprints sit outside the outline (`A24`), and there are no mounting holes (`A25`). |
| **Documents** | Requirements, architecture, safety, security and the supervisor's unit design all build in CI and gate it. |

---

## A. Schematic and PCB

The board-level specification is
[`docs/03_hardware_req.sdoc`](docs/03_hardware_req.sdoc) (`HWR-01` to
`HWR-25`), which carries the numbers and the reasoning. These are the open
changes against it.

Reviewed against the exported netlist (102 nets, 343 nodes) on 2026-10-11.
ERC is clean at `--severity-all` with no exclusions, which is worth stating
because every item below passed it: none of this is the kind of mistake an
ERC finds.

### The permit chain contradicts its own requirement

- [ ] **A26. `Q5`'s gate is pulled up, so a dead supervisor permits heat. P1.**
  `R29` (10k) pulls `/COIL_PERMIT` to +3.3 V, and `Q5` (AO3400A, source to
  GND) is the series element in the coil chain. So the one state the design
  must treat as unsafe, a supervisor that is in reset, unprogrammed or not
  driving, closes the chain. The contrast is one FET away: `Q3`, the
  controller's leg, has `R20` (47k) pulling its gate *down*, so the
  controller fails safe and the supervisor, which is the leg that has to hold
  under every condition, does not. `HWR-03` and `SYS-HW-26` require a
  pull-down. Move `R29` to GND.
- [ ] **A27. One button labelled RESET also clears the supervisor's latch. P1.**
  `D4` is a BAT54C, so pin 3 is the common cathode, and it is on `SW1.2` with
  `SW1.1` on GND. The two anodes are `/ESP32_EN` and `/SUPERVISOR_RESET`,
  which is `U5.PA0`, which the firmware reads as `SUP_PIN_CLEAR` (active low,
  internal pull-up). Press `SW1` and both lines go to about 0.3 V, so a press
  held past `SUP_CLEAR_HOLD_MS` (500 ms) reboots the controller and runs
  `sup_clear()`. The diodes are doing their job of keeping the two lines
  apart; the problem is that one button drives both. `SYS-HW-28` wants the
  clear button wired to the supervisor and to nothing else, and an operator
  reaching for RESET to reboot a hung display should not re-arm the heater
  path. Split them: `SW1` to `/ESP32_EN` alone, and delete `D4`.
- [ ] **A28. There is then no clear button. P1.** `SW1` is RESET, `SW2` is
  BOOT, `SW3` is the encoder, and no connector brings one in, so once A27
  separates them the only path to `PA0` is gone and `SYS-HW-28`'s button has
  to be added: a momentary switch from `PA0` to GND, which the internal
  pull-up already suits. `PB0` to `PB3`, `PB9`, `PC6` and `PA15` are free if
  `PA0` is wanted elsewhere. Decided 2026-10-11: **not** one of the SMD
  tactiles from `A38`. It is what an operator presses after a trip, held
  500 ms, so it wants a panel button on a block or a through-hole part with a
  stem reaching the bezel, not a 1.5 mm button inside a closed box.
- [ ] **A30. `J4.3` (`~RESET`) goes to `PA0`, and `PF2` is unconnected.** The
  tag-connect's reset line is on the clear input rather than on the MCU's
  reset, so a probe asserting reset for 500 ms clears the latch and never
  resets the part. `U5.6` is in `unconnected-`. Move it there.

### The second thermocouple is not the one the firmware expects

- [ ] **A31. No enclosure thermocouple channel exists. P1.** Both MAX31856
  belong to the supervisor, on separate buses (`TC1_*` on `PA4` to `PA7`,
  `TC2_*` on `PB4` to `PB8`), on `J3` (TC1) and `J7` (TC2). Neither touches
  `U1`. But the controller calls
  `kiln_hal_tc_init(1, &s_case_tc_port)` and `SWR-SAF-11` raises a fault on
  enclosure over-temperature. Two chamber couples on the supervisor is a
  defensible choice, since that is where the redundancy earns its keep, but
  then the enclosure sensor and `SWR-SAF-11` have to go, or a third channel
  has to appear on the controller. `HWR-05` currently describes neither board.

### Parts and 3D models

Added 2026-10-11 while filling in the LCSC fields. The board now carries a
part number on all 90 BOM lines, 34 distinct, with the 34 test points and
`J4` excluded as bare pads. The three models KiCad never shipped are
generated by [`tools/fetch3d.py`](tools/fetch3d.py) into `hardware/3dmodels/`
and all 19 model references resolve.

- [ ] **A32. `SW3`'s part does not fit `SW3`'s footprint. P1.** `C7275025` is
  a Bourns PEC11R-4215K-S0024, not the Alps EC11E the footprint is named
  for. The five terminals line up exactly, but the part's mounting lugs are
  13.99 mm apart against the footprint's 11.20 mm `MP` holes, so it will not
  go in. Either move to an EC11E that JLCPCB stocks or change the footprint
  to the PEC11R. It reaches the enclosure too: the knob's 6.15 mm splined
  bore in `safekiln-enclosure.scad` was cut for an EC11E shaft, so `B`'s
  knob needs re-checking against whichever part wins.
- [ ] **A33. No hardware gate in CI.** There is no ERC, no DRC and no BOM
  check, which is why `R6` and `R7` could carry a 100R part number
  unchallenged until somebody read the BOM by hand. ERC is clean today and
  `tools/fetch3d.py --check` verifies the models, so a job that runs both
  plus "every BOM line has an LCSC" and "no part number appears against two
  values" would hold the line at no cost.
- [ ] **A34. The 2P blocks are drawn as a MaiXu and ordered as a Kefa.**
  Footprint `TerminalBlock_MaiXu_MX126-5.0-02P`, part `C474881` =
  KF301-5.0-2P. Same 5.00 mm pitch and the pins land exactly, so this
  assembles, but the bodies differ about 0.2 mm in depth and the `F.Fab`
  outline is the wrong part. Swap the footprint to a KF301 or the part to a
  MaiXu, so the drawing and the order agree.
- [x] **A35. `R6` and `R7` carried a 100R part number.** Both were
  `C25076` (UNI-ROYAL 0402WGF1000TCE, 100Ω) while `R6` is 220R and `R7` is
  10k. `R7` is the pull-up in the supervisor's TX network, so 100Ω would have
  drawn 33 mA and left `R6` no longer limiting. Now `C25091` and `C25744`.
- [x] **A36. `J7` was labelled TC1.** Copied from `J3` with its value, so
  both thermocouple blocks read TC1 on the BOM and the silkscreen. Now TC2.
- [x] **A37. The 34 test points were in the BOM.** `(in_bom yes)` on bare
  pads, which a JLCPCB BOM cannot fulfil. Now excluded, as `J4` already was.
- [x] **A38. SW1, SW2 and BZ1 are surface mount.** `SW1`/`SW2` to the XKB
  TS-1187A (`C318884`, 5.1 x 5.1 mm, 1.6 N), whose footprint KiCad already
  ships; neither switch net was routed, so the swap cost no rework. `BZ1` to
  the FST-9650B-5V (`C391033`), which keeps `SYS-HW-09` as written: active
  with built-in drive, magnetic, 5 V, and the same 30 mA, so `HWR-14`'s rail
  budget is untouched and the firmware still just writes a level. 83 dB
  against the old 85. Pin 1 stays positive. The remaining through-hole parts
  are the seven screw blocks, two headers, the encoder and the USB-C's shield
  posts, all of which have to be through-hole.
- [ ] **A39. A footprint this project owns now exists.**
  `hardware/safekiln.pretty` plus `hardware/fp-lib-table`, holding the
  buzzer's land pattern because KiCad ships none for a 9.6 x 9.6 SMD buzzer.
  Drawn from LCSC's own footprint for the part number, not from the
  datasheet, whose text is CID-encoded and will not extract. It is checked
  against the part's 3D mesh instead, which is the better check: the leads
  run x 3.05 to 5.00 by y +/-0.75, a 10.00 mm lead span exactly as the
  package name claims, inside pads covering 3.25 to 5.75 by +/-0.90. Worth a
  second opinion against the real drawing before fabrication.

### Corrections to earlier items

- [ ] **A1. Decide what `~DRDY` is for.** The pins are unconnected, not
  un-pulled-up as this item said: `U3.7` and `U4.7` are both in
  `unconnected-`. The firmware polls, so unconnected is defensible; record it
  in `HWR-06` or wire them if conversion-complete ever becomes an interrupt.
- [ ] **A14. Pull up `TC1_CS` and `TC2_CS`.** These belong to `U5`, not the
  controller, so the window is the supervisor's own reset and the unprogrammed
  board. `HWR-07`.
- [x] **A11.** Nothing to exclude. ERC is clean at `--severity-all` with
  `erc_exclusions: []`, and the shared `SDO` is not flagged because the two
  MAX31856 are on separate buses. `HWR-22` should say so.

### Analogue and power

- [ ] **A3. Bypass `VBIAS`.** `R24`/`R25` present ~5 kΩ with no local
  capacitance, confirmed: the net holds `J10.2`, `R24.2`, `R25.1` and nothing
  else. `HWR-08`.
- [ ] **A4. Fix the CT anti-alias filter.** Corner ≈ 723 Hz sits above a
  1 kHz rate's Nyquist. Pick the sample rate first. `HWR-10`.
- [ ] **A5. Rescale the CT front end.** A 30 A/1 V CT clips across the upper
  half of `SWR-CUR-02`'s 0 to 60 A. `HWR-09`.
- [ ] **A6. Define `CURR_SENSE` with no CT fitted.** Open input drifts, so
  "disconnected" and "zero current" are indistinguishable. `HWR-11`.
- [ ] **A7. Work out the charge-pump release threshold. P1.** The circuit the
  whole `SYS-SAF-02` property rests on: pumped level ≈ 2.7 V against an
  AO3400A characterised at 4.5 V, decay τ ≈ 155 ms. `HWR-01`.
- [ ] **A8. Budget the +5 V rail against coil inrush.** `HWR-14`.
- [ ] **A9. Confirm the 5 V contactor coil is a real part choice.** Mains
  contactors with 5 V coils are uncommon; a 12 V or 24 V coil changes the rail.
- [ ] **A10. Decouple `VBUS`.** `HWR-17`.
- [ ] **A12. ESD protection on the USB data pair.** `HWR-17`.
- [ ] **A13. Clamp the thermocouple inputs.** `HWR-08`. The CT's own clamp is
  right: `D9` is a BAT54S with the junction on `/CURR_SENSE`, anode to GND,
  cathode to +3.3 V.
- [ ] **A15. Pull up `MCU_IO0`.** `SW2` pulls it low with nothing holding it
  otherwise. `HWR-18`.
- [ ] **A16. Check the AMS1117's dropout and thermal margin.** `HWR-15`.
- [ ] **A17. Transient protection on `V_IN`.** `HWR-16`.

### Drawing and layout

- [ ] **A18. Resolve the "(opt)" annotations.** `HWR-13`.
- [ ] **A19. Put the CT specification on the schematic as a note.** `HWR-13`.
- [ ] **A20. Review the encoder input network.** `HWR-18`.
- [ ] **A21. One generated pin map.** Assignments live in three places that
  can disagree, and A1, A14 and A31 are all cases of the documentation
  describing a different board from the schematic. `HWR-21`.
- [ ] **A23a. Finish the KiCad rename to `safekiln`.** Files are renamed;
  check no stale references remain.
- [ ] **A23. Finish routing, and review the mains-adjacent nets. P1.** 182
  tracks and 9 vias exist, so this is unfinished rather than unstarted.
  `HWR-23`.
- [ ] **A24. 14 of 125 footprints sit outside the outline.** `U4`, the second
  MAX31856, is at y = 117.9 against an outline ending at 110, and `J7`, the
  second thermocouple terminal, at y = 73.2 against 70. A part outside the
  outline is not placed: it will not be fabricated where it appears.
  `HWR-25`.
- [ ] **A25. The board has no mounting holes. P1.** Nothing fixes it to
  anything. The enclosure proposes four M3 at 4 mm in from each corner;
  `HWR-24`.

What the review confirmed as correct, so that it is not re-checked: the coil
chain, which is `+5V`, `J9` lid, `J8` coil, then `Q3` and `Q5` in series on
the low side through `/COIL_RTN1`, so both legs must conduct; both dividers
into the supervisor, `/COIL_FEEDBACK` 10k/18k on `PA1` and `/LID_SENSE`
10k/18k; the CT clamp, `D9` a BAT54S with its junction on `/CURR_SENSE`; and
decoupling, where every supply pin has local capacitance.

---

## B. Enclosure

Designed in [`hardware/enclosure`](hardware/enclosure/README.md): panel-mount,
three printed parts, every dimension read from the PCB. Renders and slices, and
nothing has been printed.

- [ ] **B1. Print the knob and check its bore.** `knob_bore` is 6.15 mm for a
  splined EC11E shaft, but the ordered part is a Bourns PEC11R (`A32`), so
  settle the encoder first or the bore is cut for a shaft nobody is fitting.
  One 27-minute print decides it, and it gates the other two.
- [ ] **B2. Check the OLED module against the three `CHECK` parameters.** The
  window, the module outline and the 4 × M2 grid are the common 0.96″ variant
  and modules differ.
- [ ] **B3. Decide the material.** PLA softens at 55 to 60 °C and this bolts
  to a kiln's panel, which `SWR-SAF-11` exists because of. Measure the panel
  at temperature; PETG or ASA slices from the same profile.
- [ ] **B4. Check the SWD probe clearance.** The shell's opening is positioned
  from `J4`, but a TC2030 needs the probe square to the pads (`HWR-19`).
- [ ] **B6. `h_below` is 1 mm short, and it sets the box width.** It says
  2.5 mm of lead protrusion under the board; the terminal blocks' pins stick
  out 3.50 mm, measured off the part's own 3D mesh and matching the KF301's
  stated pin length. `pcb_stack` and so `in_w` are built from it, so the real
  board sits 1 mm closer to the wall than the model allows. Correcting it
  widens the shell by 1 mm and moves the panel cut-out, which is why the
  number is annotated rather than changed: it is a reprint, not an edit.
- [x] **B5. The buzzer height is now the SMD part's.** `h_buzzer` 9.5 to 5.5
  for the FST-9650B. Documentation only as it turns out, since `pcb_stack`
  is built from `h_terminal` at 11.0, so the box does not change. The
  sound-hole grid is still positioned from `BZ1`, which has not moved.

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

- [ ] **R6. Finish the supervisor's schematic. P1.** It is drawn, not
  finished: `U5` (STM32G031K8Tx, LQFP32), both MAX31856 and the SWD
  tag-connect `J4` are placed. What the pin map still asks of the board is the
  permit line's external pull-down (`HWR-03`), SWD out to test points
  (`HWR-19`), and the link landing on one receive-only ESP32 pin, since
  `UART0` is the console and must not be used. `U4` is also outside the
  outline (`A24`).
- [ ] **R11. The clear button on the panel.** The firmware implements it
  (edge triggered, held 0.5 s, a line stuck low never arms). What is left is
  physical: the button, its position relative to the HMI, and whether it is
  labelled as clearing the *supervisor* or clearing *a fault*, which are not the same
  thing, and the operator cannot see the difference. There are two
  acknowledgements and the panel must not imply there is one.

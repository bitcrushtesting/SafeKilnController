<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Safe Kiln Controller Task List

**Outstanding work only.** An item leaves this file when it is done; what was
done and why is in the commit that did it, not here. Checked against
[`docs/03_software_req.sdoc`](docs/03_software_req.sdoc),
[`docs/architecture.md`](docs/architecture.md), [`docs/safety.md`](docs/safety.md)
and [`docs/security.md`](docs/security.md).

Item IDs are stable and referenced from commit messages, so a letter is never
reused: gaps in the numbering are items that have been closed.

| | Meaning |
|---|---|
| **P1** | Safety-relevant or blocking. Do before the next board revision, and before any firing. |
| **P2** | Required for a release that claims the requirements are met. |
| **P3** | Correctness polish, consistency, cleanup. |

## State, 2026-10-06

| | |
|---|---|
| **Firmware logic** | Complete and tested. C++20, 398 host tests green plain and under ASan/UBSan, `clang-tidy` clean on host, target and webhost with no suppressions baseline, `esp32s3` builds with zero warnings at 243 kB (88 % of the OTA slot free), QEMU boots the image and fires it. |
| **Firmware on hardware** | 15 of 16 ports are wired on the target. Only `update` has nothing behind it (`H2`). **None of it has been run against real hardware** (`L2`, `O3`, `P4`, `M3`). |
| **Schematic** | ERC clean apart from one known `SDO` false positive (`A11`). Carries the lid interlock and the thermocouple-fault interlock. Does not carry the phase strap (`HR-22`) or the second and third CT inputs (`HR-23`). |
| **PCB** | Updated from the schematic and placement started. **Not routed at all** (`A23`, `K4`). |
| **Blocking release** | No field update path (`H2`, `SRR-11`). The web interface is read-only by design, so local control is the only control (`H3`). |

## Where the open items are

| | | Open |
|---|---|---|
| [A](#a-schematic-and-pcb) | Schematic and PCB | 22 |
| [C](#c-build-test-and-ci-infrastructure) | Build, test and CI | 8 |
| [D](#d-documentation-and-open-questions) | Documentation and open questions | 2 |
| [F](#f-static-analysis) | Static analysis | 2 |
| [G](#g-door-interlock-sr-31) | Door interlock | 3 |
| [H](#h-field-update-and-local-control) | Field update and local control | 2 |
| [I](#i-current-measurement) | Current measurement | 1 |
| [J](#j-german-translation-nfr-23) | German translation | 4 |
| [K](#k-hardware-interlock-chain) | Hardware interlock chain | 5 |
| [L](#l-target-adapters) | Target adapters | 2 |
| [M](#m-kiln_hmi-the-local-interface) | `kiln_hmi` | 3 |
| [O](#o-wifi-fr-net) | WiFi | 3 |
| [P](#p-http-transport-and-the-api) | HTTP transport and the API | 4 |
| [Q](#q-the-file-store-ad-21) | The file store | 5 |
| [R](#r-the-independent-safety-supervisor-ad-22) | Independent safety supervisor | 7 |

---

## A. Schematic and PCB

### A.1 Blocking electrical defects (P1)

- [ ] **A1. Add pull-ups to the MAX31856 `~DRDY` outputs.** The `~FAULT` outputs got theirs in section K (`R28`, `R29`); `~DRDY` still has none, confirmed by query. Original note: All four
  are open-drain and have no pull-up anywhere in the netlist. `TC1_DRDY`
  (U3.7 → U1 IO14) and `TC2_DRDY` (U4.7 → U1 IO21) therefore float between
  assertions, so acquisition timing is undefined. Add 10 k to +3V3 on
  `TC1_DRDY`, `TC2_DRDY`, `TC1_FAULT`, `TC2_FAULT`. Affects SWR-ACQ-03,
  SWR-ACQ-10, SWR-SAF-04.

- [ ] **A3. Bypass `VBIAS`.** R24/R25 (10 k/10 k off +3V3) present ~5 kΩ of
  source impedance, and the CT secondary returns into that node through J10.2.
  The "mid-rail" reference is therefore modulated by the signal it is supposed
  to reference. Add 10 µF ∥ 100 nF from `VBIAS` to GND. SYS-HW-17.

- [ ] **A4. Fix the CT anti-alias filter.** R26 (1 k) + C25 (220 nF) gives
  f<sub>c</sub> ≈ 723 Hz. SWR-CUR-03 specifies sampling at ≥ 1 kHz, whose Nyquist
  frequency is 500 Hz, the filter corner sits *above* Nyquist and does not
  anti-alias. Either raise the sample rate (4–8 kHz) and keep a corner near
  300 Hz, or lower the corner to ~200 Hz for a 1 kHz rate. Pick the sample rate
  first, then the filter. SYS-HW-17, SWR-CUR-03.

- [ ] **A5. Rescale the CT front end for the specified range.** J10 is annotated
  "CT 30A/1V" but SWR-CUR-02 requires 0–60 A. At 60 A a 30 A/1 V CT delivers
  2 V<sub>rms</sub> = ±2.83 V<sub>pk</sub> about the 1.65 V bias, which D9 clamps , 
  the reading saturates across the whole upper half of the required range. Even
  at 30 A the swing is 0.24 V to 3.06 V, in the region where the ESP32-S3 ADC is
  least linear. Specify a CT ratio (or add an attenuator) that puts full-scale
  current at roughly 0.5 V<sub>rms</sub>, and record the resulting LSB against
  SWR-CUR-02's 0.1 A resolution.

- [ ] **A6. Define `CURR_SENSE` when the CT is absent.** With J10 open there is
  no DC path to the node; it is held only by C25 and diode leakage, so it
  drifts. SWR-CUR-11 requires distinguishing "CT disconnected" from "genuinely
  zero current", and SWR-CUR-12 requires refusing to start a run when monitoring
  is unavailable. Add a defined bias (e.g. a high-value resistor to a level the
  CT winding cannot produce) so an open input is electrically recognisable, and
  write down the detection rule the firmware will use.

- [ ] **A7. Work out the charge-pump release threshold properly.** This is the
  circuit the whole SYS-SAF-02 / SYS-HW-07 safety property rests on, and architecture
  §16 already flags it as the design's most fragile idea.
  `HEAT_EN` → R19 → C23 (10 µF) → D5 (BAT54S) → `HEAT_EN_DC` → C24 (3.3 µF) ∥
  R20 (47 k) → Q3 gate. Two things need numbers rather than intent:
  - Pumped level is roughly 3.3 V − 2·V<sub>f</sub> ≈ 2.7 V, which is below the
    4.5 V at which the AO3400A's R<sub>DS(on)</sub> is characterised.
  - Decay is τ = 47 k × 3.3 µF ≈ 155 ms through a MOSFET threshold, so release
    is a slow slide through the linear region, not an edge. Establish that the
    contactor's drop-out voltage is reached inside SWR-NFR-04's 1 s, that Q3 does not
    dissipate meaningfully on the way through, and that the contactor cannot sit
    partially closed.

  If the margin is thin, a comparator or a retriggerable monostable gives a crisp
  threshold for a few cents. Verify on the HIL jig by halting the safety task
  (SWR-TST-17, SYS-SAF-02, SWR-NFR-04).

- [ ] **A8. Budget the +5 V rail and protect the MCU from coil inrush.** J8 feeds
  the contactor coil from the same +5 V node as the LDO, behind the 1.5 A PTC
  (F1). Add up ESP32-S3 WiFi peaks, the LDO's input current, the coil's holding
  and inrush current, both SSR inputs and the buzzer, then confirm F1 and the
  external supply (SYS-HW-14 requires MCU + display + coil simultaneously). C1 is
  only 100 µF; coil energisation will sag the rail that the MCU brownout detector
  watches. Consider feeding J8 from `V_FUSED` ahead of D1 so coil transients
  cannot pull the MCU rail down. SYS-HW-14, SWR-NFR-15, SWR-SAF-15.

- [ ] **A9. Confirm the 5 V contactor coil is a real part choice.** Mains
  contactors are commonly 24 V AC/DC or 230 V AC coils; pinning J8 to 5 V narrows
  the field sharply. Either name a specific 5 V-coil contactor in the BOM, or
  change the interface to drive an intermediate relay / a higher coil voltage.
  SYS-HW-07, SYS-SAF-03.

### A.2 Required before fabrication (P2)

- [ ] **A10. Add VBUS decoupling.** The design review flags `VBUS` as having no
  decoupling at all. Add 1 µF (USB spec caps bulk VBUS capacitance at 10 µF)
  plus 100 nF near J2.

- [ ] **A11. Record an ERC exclusion for the shared SPI `SDO`.** ERC reports one
  error: U3.11 and U4.11 (`SDO`, both Output) are connected. This is correct for
  a shared SPI bus, the MAX31856 tri-states `SDO` when `~CS` is high, but the
  project currently carries zero ERC exclusions, so the schematic cannot be
  gated on a clean ERC. Add the exclusion with a comment, and make a clean ERC a
  CI gate.

- [ ] **A12. ESD protection on the USB data pair.** `USB_DP`/`USB_DM` go straight
  from J2 to the ESP32-S3 native USB pins with no clamping. Add a low-capacitance
  array (USBLC6-2SC6 class) at the connector.

- [ ] **A13. Clamp the thermocouple inputs.** SYS-HW-15 requires the TC inputs to be
  *filtered and protected*. The filtering is there (R4/R5 100 R, C8 10 nF
  differential, C9/C10 100 pF common-mode; same for TC2) but there is no
  clamping. A thermocouple is a multi-metre unshielded pair routed beside a
  switching multi-kilowatt load, add TVS or clamp diodes to the rails on all
  four TC terminals.

- [ ] **A14. Pull up `TC1_CS` and `TC2_CS`.** Both float during reset and boot
  while the MAX31856s are already powered, so a spurious chip select can
  misconfigure a front end. 10 k to +3V3 each.

- [ ] **A15. Pull up `MCU_IO0`.** SW2 pulls IO0 to GND with no external pull-up;
  the design relies on the module's internal pull-up alone, and the BOOT button's
  wiring is an unterminated stub. Add 10 k to +3V3 (most ESP32-S3 reference
  designs also add 100 nF).

- [ ] **A16. Check the AMS1117's dropout and thermal margin.** Worst case the LDO
  sees USB VBUS at 4.75 V minus D2's forward drop ≈ 4.3 V, against a 1.1–1.3 V
  dropout at ESP32-S3 WiFi peaks, close to falling out of regulation.
  Dissipation is ~0.5–0.7 W in SOT-223, roughly +40 °C over ambient; with SWR-SAF-11
  permitting a 70 °C enclosure, the junction has little headroom and the LDO sits
  on the same board as the cold-junction reference. Either move to a low-dropout
  part with better thermals or a small buck, or document the measured rise and
  the ambient limit it implies.

- [ ] **A17. Add input transient protection on `V_IN`.** F1 and the series D1
  cover overcurrent and reverse polarity, but nothing clamps a surge. The board
  shares an enclosure with a switching contactor coil. Add a TVS / MOV across
  `V_IN`.

- [ ] **A18. Resolve the "(opt)" annotations.** J10 is labelled "CT 30A/1V (opt)",
  but SYS-HW-11 makes the current transformer mandatory and SWR-CUR-12 makes a run
  refuse to start without it. Re-label, and keep J7 "SSR2 CTRL (opt)" which
  genuinely is optional (SYS-HW-12).

- [ ] **A19. Put SYS-HW-16 on the schematic as a note.** The CT must be a
  voltage-output type with an integral burden resistor; a current-output CT whose
  burden can be disconnected develops dangerous voltages on an open secondary.
  That is a hard BOM constraint and a safety one, it belongs on the sheet next
  to J10, not only in the requirements. Same for SYS-HW-18: state the required CT
  insulation rating, since the CT is the only galvanic isolation in the design.

- [ ] **A20. Review the encoder input network.** R10–R12 (10 k) with C20–C22
  (100 nF) gives a ~1 ms edge into non-Schmitt ESP32-S3 GPIOs feeding the pulse
  counter (SYS-HW-05). Slow edges dwelling near V<sub>IH</sub> can double-count.
  Either lower to ~4.7 k/10 nF and lean on the PCNT glitch filter, or verify the
  current values on hardware before committing.

- [ ] **A21. Create the single pin-map artefact SYS-HW-10 requires.** Pin assignments
  exist only as schematic net names; there is no `hardware/pinmap.*` and no
  firmware header. One file per board variant, referenced by both.

### A.3 Cleanup and layout follow-up (P3)

- [ ] **A23a. Rename the KiCad project to `safekiln`.** The project was renamed
  to Safe Kiln Controller everywhere except `hardware/`, which was left alone
  for two reasons: the files were being edited at the time, and the name is not
  only a filename there. `kilncontrol` appears in several hundred internal
  references, as `(project "kilncontrol")` in every symbol instance of the
  schematic and as `(sheetfile "kilncontrol.kicad_sch")` in every footprint of
  the PCB. Do it with KiCad's own Save As / rename rather than `git mv` plus a
  substitution, so the cross-references are rewritten by the tool that owns
  them. `board_pins.h` cites the schematic filename and will follow.

- [ ] **A23. PCB review is still outstanding.** `hardware/safekiln.kicad_pcb`
  has placement but no copper: zero tracks, zero vias, sixteen zones untouched.
  Creepage and clearance for the mains section, the coil interrupt chain (`K4`),
  star-grounding of the analogue front end, and the thermal path of the LDO
  (`A16`) all want checking against the layout rather than the schematic, and
  none of that can be judged until the board is routed.

---

## C. Build, test and CI infrastructure

- [ ] **C5. `tools/layercheck`.** CI-blocking check that `kiln_core` references no
  IDF or RTOS header and that the component graph is acyclic and layered
  (SWR-TST-01, SWR-TST-07, SWA-14).

- [ ] **C6. Requirement-to-test traceability.** *Superseded in approach:* see
  section 7 of [`docs/test-concept.md`](docs/test-concept.md). The recommendation
  is not to build `tools/trace` but to use StrictDoc source traceability, which
  now owns the requirements, and have the existing `requirements` CI job publish
  the matrix. Today 28 of 268 requirements appear in a test name and 147 appear
  in no test file at all. Original item follows.

  **`tools/trace`.** Requirement-ID traceability from `docs/` to test
  names; fails on an untraced mandatory requirement, on a test naming a
  nonexistent ID, and on any `SR-*` without an automated test
  (SWR-TST-22, SWR-TST-23, SWR-TST-26).

- [ ] **C7. `tools/logdump`.** Decode a log partition dump to CSV, also the
  cross-check for the `logrec` codec.

- [ ] **C9. Vendor the web assets into the firmware.** `web/` is no longer empty:
  it carries the UI, read-only and translated. What is missing is the build step
  that gzips it into `kiln_web/assets` so `SWA-11` holds and the assets ship
  inside the image, within the budget of architecture §12.4.

- [ ] **C11. `main.cpp` and `httpd.cpp` are analysed by nothing.** Found while
  starting section P. `tools/tidy.sh` drives from the host compile database,
  which cannot reach either file; `tools/tidy-target.sh` hardcodes
  `kiln_hal_esp32s3/src`, so it does not either. Subtracting the host database
  from the target one gives 14 target-only translation units, and two of them
  are outside every gate in the project.

  Widening the script is three lines -- the set should be *computed* as "in the
  target build, not in the host build", which also cannot drift when the next
  target-only file appears -- but it is **102 findings**: 20 in `httpd.cpp`
  and 82 in `main.cpp`. 54 of those are inside `ESP_LOGx` and
  `ESP_ERROR_CHECK` and want the same component-scoped exemption
  `kiln_hal_esp32s3` already carries, which for `main.cpp` is a clean
  `firmware/controller/main/.clang-tidy` but for `httpd.cpp` cannot be, because
  `kiln_web/src` also holds the host-analysed `api.cpp` and `json.cpp` and must
  keep those checks. The remaining ~48 are the ordinary `F2`/`F4`/`F5`/`F6`
  passes that never ran on these two files.

  Nothing here is hard; it is the size of the diff that makes it its own
  commit rather than a detour inside another one.

- [ ] **C12. Build the HIL fixture.** Designed in section 5 of
  [`docs/test-concept.md`](docs/test-concept.md), which closes `SWR-TST-17` and
  `SWR-TST-28` on paper and nothing in hardware. The shape: the plant model is
  `kiln_sim` running on a host, so an L4 scenario can be the same scenario as
  an L2 test with the same seed, and a disagreement isolates to the adapters
  and the electricals. The I/O board is close to dumb but timestamps in
  hardware, because USB latency is fine for a 10 Hz thermal model and useless
  for measuring a 10 ms SSR window.

  Two parts are not obvious and are worked through there. The thermocouple
  simulator has to *subtract* the cold-junction compensation the MAX31856 adds,
  and the loop closes itself because the supervisor already reports `cj_c` ten
  times a second. And the current side needs a fixture-controlled bypass across
  the load, not a signal generator, because `SWR-SAF-27` discriminates a shorted SSR
  from a welded contactor by dropping the contactor and re-measuring, and that
  needs real contacts behaving both ways.

  The single most valuable scenario it enables: hold the ESP32 in reset, drive
  the chamber above 1350 degC, confirm the contactor opens. That is the one
  experiment that distinguishes this design from the one it replaced, and it
  exists at no other level.

- [ ] **C13. Make three architecture decisions structural.** Each holds today
  by discipline alone and is cheap to enforce, per section 6 of the test
  concept. `SWA-18`, the 20-byte log record, has no `static_assert` anywhere,
  and it guards a persisted format. `SWA-03`, no globals in the core, is true
  (zero mutable file-scope objects in `kiln_core`) with
  `cppcoreguidelines-avoid-non-const-global-variables` switched off. `SWA-02`,
  time is injected, is true (zero direct clock reads in the core) and is a
  grep. Three decisions that are currently claims.

- [ ] **C10. Coverage gate.** 90 % lines on control, safety, setpoint, program and
  autotune is enforced; 100 % of safety decision branches (`SWR-TST-19`) is not.
  Branch coverage was around 83 % when last measured.

---

## D. Documentation and open questions

- [ ] **D5. Write the documentation SWR-NFR-26 lists:** assembly and wiring, mains
  safety, commissioning, autotuning, program authoring, the REST API, the log
  record format, and current-transformer fitting and calibration. The CT
  commissioning procedure is called out in architecture §16 as the mitigation for
  a CT fitted to the wrong conductor.

- [ ] **D6. Remaining open questions to close:** OQ-01 (cone-based targets),
  OQ-03 (whole-life run-summary retention), OQ-05 (3-zone variant, affects
  whether the control path is written for one zone or N), OQ-07 (element
  temperature coefficient measured or entered).

---

## F. Static analysis

Both of these are decisions rather than tasks: the work is understood and the
question is whether it is worth its cost.

- [ ] **F3. `cppcoreguidelines-use-enum-class`: 31 enums, 223 enumerators,
  2 530 references across 85 files.** Still deferred, and now with the blocking
  question answered rather than open.

  **The C-callability half is settled: the port layer is not C-callable.**
  There is no `extern "C"` anywhere in the firmware except `app_main`, which
  ESP-IDF requires. So `SWA-01`'s boundary is a C++ boundary already, and that
  is no longer a reason not to do this.

  What remains is not a technical blocker but a cost. The enumerator names are
  the project's vocabulary: `KILN_FAULT_DOOR_OPEN` is greppable from `SWR-SAF-31`
  in the requirements, and `tools/trace` parses test names on the same
  convention (`SWR-TST-22`, `SWR-TST-23`). Scoping renames all 223 of them. The
  transform is compiler-verified -- every unconverted site is a hard error --
  but the naming choice (`kiln_fault_t::DOOR_OPEN`, idiomatic but breaks the
  greps, versus `kiln_fault_t::KILN_FAULT_DOOR_OPEN`, redundant but traceable)
  is a decision about the project's vocabulary rather than about the code.

  Two enums would also get worse: `kiln_warn_bit_t` and `kiln_inject_t` are bit
  *positions*, and scoping them puts a `static_cast` at every mask site --
  precisely the cast noise the signed-bitwise and C-cast passes removed.

- [ ] **F9. `kiln_run_record_t` carries 9 padding bytes where 1 is optimal**
  (`clang-analyzer-optin.performance.Padding`, disabled). Reordering would
  invalidate every run record already on a device, so it can only change
  alongside a record-format version bump, if at all.

---

## G. Door interlock, SWR-SAF-31

- [ ] **G2. HIL: confirm the series contact actually breaks the coil.** As with
  the charge pump (M4), the claim that matters is a hardware one and cannot be
  verified in simulation. Open the door on the bench jig with the safety task
  halted, and observe the contactor.

- [ ] **G3. Decide whether the interlock should be mandatory.** `SYS-HW-21` is a
  *should* because many existing kilns have no door furniture to take a switch,
  and making it a *shall* would make the controller unfittable to them. The
  consequence is RR-10: a kiln without one has nothing at all against HZ-13.
  Warning 113 makes the gap visible rather than silent, which is the compromise
 , revisit if the first real installations suggest otherwise.

- [ ] **G4. Expose the door state in the API and on the display.** The
  supervisor knows; `/api/status` and the OLED do not yet say. Wanted for
  SWR-WEB-04 and the default screen, and it is the cheap half of making
  warning 113 actually visible.

---

## H. Field update and local control

- [ ] **H2. There is no field update path at all.** `SWR-UPD-01` was inverted:
  no firmware image is accepted over the network. That closes TH-04 completely
  and leaves no way to ship a fix without physical access, including a
  security fix. `OQ-08` asks what replaces it (USB/serial via `esptool`, or an
  image staged over the network but applied only after a physical confirmation
  at the kiln). **This blocks release**, and is tracked as `SRR-11`.

- [ ] **H3. Local control is now the only control, and it has never been
  operated.** The web interface is read-only by design, so starting, pausing,
  aborting and acknowledging are reachable *only* through `kiln_hmi`. That
  component now exists, carries those five actions, passes 21 tests and is
  wired into `main`. What keeps this open is that no one has ever used it: it
  is verified by pixel counts and nothing else (`M3`), its German is not
  length-checked against the screen (`M4`), and the encoder direction is a
  guess (`M5`). Until those close on real hardware, **the only path to
  starting a firing is unexercised.**

---

## I. Current measurement

- [ ] **I3. Real power, not apparent.** `SWR-CUR-07` says apparent power and
  assumes a resistive load (`SYS-ASM-09`), which for a kiln element is very nearly
  true. Measuring real power would need a voltage channel, which the design
  does not have and probably should not grow. Worth closing explicitly rather
  than leaving as an implied limitation.

---

## J. German translation, SWR-NFR-23

- [ ] **J1. The OLED font and encoding are unverified.** The labels avoid
  umlauts for that reason, but nobody has yet confirmed what the SSD1306 font
  in `kiln_hmi` will carry. If it does handle Latin-1 or a UTF-8 subset, the
  labels can use proper German and the test's 16-character bound should be
  re-checked against the real glyph widths rather than character count.

- [ ] **J2. The long causes are not shown on the display yet.** `kiln_hmi` does
  not exist, so only the web renders them. When the fault screen lands it will
  need to wrap German text, which runs roughly 15 per cent longer than English.

- [ ] **J3. Config item names and units are still English.** `/api/config`
  returns keys like `safety.max_temp_c` with English descriptions. The keys
  are an API contract and should stay, but the human-readable descriptions
  beside them in the settings screen are untranslated.

- [ ] **J4. No German review by a native speaker.** The translations are
  careful but unreviewed. `VERSCHWEISST` for a welded contactor and
  `KEINE WAERME` for no heat are the two worth checking first, being the most
  safety-critical messages a German-speaking operator would act on.

---

## K. Hardware interlock chain

- [ ] **K1. The hardware TC interlock is not firmware-independent, and the
  documentation now says so.** The `FAULT` outputs are open-drain: an unpowered
  or absent front end leaves the path closed. The MAX31856 also detects an open
  circuit only once its fault mask is configured, so out of reset the interlock
  does not act. It covers faults the device actively reports; `SWR-SAF-04` in
  firmware remains the cover for a dead or unconfigured front end. The lid
  contact has no such caveat.

- [ ] **K2. Should an enclosure thermocouple fault really stop the kiln?**
  Both front ends are in the chain, because the request said fault pins. But
  `SWR-SAF-11` (enclosure over-temperature) is a backstop, and a failed enclosure
  probe killing a firing mid-glaze is a nuisance trip, which `HZ-10` says is
  how protections get disabled. Consider a fitted-by-default `0R` in `Q5`'s
  drain so the enclosure branch can be depopulated without cutting a track.

  Decide this together with the parts analysis that used to live in
  `docs/bom-optimisation.md` (removed from the repository; see its history),
  which reached the same question from the parts end: if this branch is depopulatable then the
  MAX31856 on the enclosure channel has nothing left to justify it, since it is
  bought for its `FAULT` pin rather than for measuring 40 to 90 degC. That
  document's preferred option removes the channel entirely, using the chamber
  front end's own cold-junction reading for `SWR-SAF-11` and a bimetallic cutout in
  the coil for a hardware trip.

- [ ] **K3. `D7` is an unwired LED.** Pre-existing, not from this change: the
  coil indicator's cathode is on `COIL_DRV` and its anode goes nowhere. It
  needs a series resistor to the coil supply. Note that taking it from
  `LID_SWITCH` rather than `+5V` makes it indicate "coil actually energised"
  rather than "the MCU asked".

- [ ] **K4. The PCB carries section K, but is not routed.**
  `update_pcb_from_schematic` has been run and the interlock chain's parts are
  on the board (`J9`, `Q5`, `Q6`, `R28`–`R31`); placement is started, with 74
  footprints positioned. **There are no tracks and no vias on the board at
  all**, and the coil interrupt is a mains-adjacent net, so routing it is not
  a formality: see `A23`.

- [ ] **K5. HIL: verify each interrupt separately.** Four elements in series
  means four tests: open the lid, pull each `FAULT` low, and halt the safety
  task. Each should drop the contactor on its own. This is the same class of
  claim as `G2` and cannot be settled in simulation.

---

## L. Target adapters

- [ ] **L2. None of this has touched hardware.** Every adapter compiles and
  passes analysis, and that is the whole of the evidence. QEMU does not
  emulate SPI, I2C, PCNT or the ADC in any way that would exercise them, so
  the MAX31856 register decode, the SSD1306 init sequence, the quadrature
  decoding and the ADC scaling are all unverified against a real part. This is
  the same class of claim as `G2` and `K5`, and it is the largest untested
  surface in the project.

- [ ] **L6. The hardware build is not in CI.** CI builds the simulated
  configuration only. The hardware one is a second `idf.py` invocation with a
  different `SDKCONFIG_DEFAULTS`, and it is the configuration that matters for
  a release.

---

## M. kiln_hmi, the local interface

- [ ] **M3. Nobody has looked at it.** Every screen is verified by pixel counts
  and state assertions on the host. No one has seen a glyph on a real SSD1306,
  and the init sequence, the page addressing and the 5x7 font are all
  unverified against glass. Layout judgements -- whether 15x21 really is
  legible at two metres, whether the fault cause wraps readably -- cannot be
  made from a test. Same class as `L2`.

- [ ] **M4. German text is not length-checked against the screen.** The fault
  *labels* are held to 16 characters by a test (`J1`), but the long *causes*
  that the fault screen wraps are not, and German runs roughly 15 per cent
  longer than English. A cause that overflows the panel loses its last line,
  which on a fault screen is where the instruction tends to be.

- [ ] **M5. The encoder direction is a guess.** The quadrature channel actions
  in `hal_input.cpp` assume one wiring of A and B. If the knob turns the menu
  the wrong way on real hardware, swap the two `pcnt_channel_set_edge_action`
  pairs; it is a one-line fix and not a design error, but it will be wrong half
  the time until somebody turns a real knob.

---

## O. WiFi, FR-NET

- [ ] **O1. `SWR-NET-04` (mDNS, `kiln.local`) is not implemented.** mDNS left
  the ESP-IDF tree for the component manager, and `UR-CON-04` forbids a
  build-time fetch from an unpinned source. `E7` met the same constraint over
  the file store and resolved it by not needing the dependency at all; mDNS has
  no such escape, since the protocol is the feature. Adding a managed
  dependency for a convenience feature would be the wrong trade against a
  constraint the project applies everywhere else, so the device is reachable by
  IP until mDNS is vendored deliberately. The address is on the HMI network screen, which is
  where an operator would look anyway.

- [ ] **O2. The image grew by 540 kB.** WiFi takes it from 292 kB to 831 kB,
  which is 60.4 % of the OTA slot still free, so `SWR-NFR-13` holds comfortably.
  Worth watching once the web assets are embedded (`C9`): that budget is
  architecture 12.4's and it has not been measured against a real asset build.

- [ ] **O3. Untested against a radio.** Same class as `L2`: it compiles and
  passes analysis. Nobody has watched it associate, fall back to the AP,
  recover from a dropped connection, or sync time.

---

## P. HTTP transport and the API

- [ ] **P1. The UI is not served.** The transport answers `/api/*` and nothing
  else: `C9`'s asset embedding is not done, so there is no `index.html` in the
  image and a browser at the device's address gets a 404. The API is usable
  with `curl` today. `SWA-11` wants the assets gzipped into the image, which
  also needs the budget of architecture 12.4 measured for the first time.

- [ ] **P2. Server-Sent Events are not implemented.** `SWR-WEB-05` wants live
  values pushed at least once a second, and `kiln_api_telemetry_event()` is
  written and tested for exactly that, but the transport has no `/api/events`
  handler. The UI falls back to nothing: it reads `/api/status` on a timer
  already, so this is a refinement rather than a gap in function.

- [ ] **P3. Where do firing programs come from now?** Recorded as `OQ-09`.
  Authoring is gone from the web and there is no local editor, so a user
  cannot create a curve of their own: they get the seeded examples of
  `SWR-PRG-09` and nothing else. This is a real functional gap and the most
  likely thing to make somebody reverse the read-only decision. A file import
  at provisioning time is probably the cheapest answer.

- [ ] **P4. Untested against a client.** It compiles and passes analysis. No
  request has been made of it. The host API suite covers every route's
  behaviour, so what is unverified is specifically the socket half: URI
  splitting, chunked streaming, the four-session limit, and whether a slow
  client can hold a buffer long enough to matter.

---

## Q. The file store, SWA-21

- [ ] **Q1. The store has never seen a worn sector.** Every test runs on a fake
  that writes what it is told. A real NOR sector near the end of its life fails
  a program or reads back something it was not given, and the store's answer to
  that is the CRC, which rejects the copy and falls back to the other one. That
  is the designed behaviour and it is tested, but it has been tested against
  simulated corruption rather than against a worn part. There is no bad-region
  retirement: a region whose both sectors have failed will keep being chosen
  and keep failing, where a filesystem would have remapped it. At 40 regions of
  two sectors each, written single-digit times a year against a 100 000 cycle
  rating, that is a defensible trade rather than an oversight, but it is a trade
  and this is where it is recorded.

- [ ] **Q2. The index is built by reading every region at every boot.** 64
  regions, two copies each, each validated by streaming its payload through the
  CRC. That is up to 128 sector reads and about 320 kB of CRC at startup,
  measured at nothing in particular because it has only ever run under QEMU and
  on the host. If it turns out to cost real time on a board it can be made lazy,
  since the only thing mount actually needs eagerly is which copy of each region
  wins. Worth measuring before it is worth optimising.

- [ ] **Q3. `exists` and `list` are implemented and have no caller.** They are
  in the port contract, so the store provides them and the tests cover them, but
  `program_store` and `run_index` use neither. They are the natural way a local
  program editor would enumerate what is there, which is `P3`, so they are kept
  rather than removed.

- [ ] **Q4. A name longer than 63 bytes is refused rather than truncated.**
  `KILN_PATH_MAX` is 64 including the terminator and the name field is exactly
  that, so a longer path returns `KILN_ERR_INVALID_ARG`. Nothing generates one
  today, since both callers produce five-character slot paths, and refusing is
  the right answer rather than silently storing a different file than the caller
  asked for. Noted because a future caller with user-supplied names will meet it.

- [ ] **Q5. The run index rewrites more than it needs to.** `kiln_run_index_append`
  reads every slot to find the oldest, then writes one. On this store that is 20
  reads served from the in-RAM index plus one region write, which is cheap. But
  `kiln_run_index_mark_truncated` and `kiln_run_index_baseline` both walk all 20
  slots too, and the walk is now the only reason `run_index` reads at all. Not a
  problem, just the place where the store being an array rather than a directory
  would let the index get simpler if it were ever revisited.

---

## R. The independent safety supervisor, SWA-22

Designed in [`docs/safety-supervisor.md`](docs/safety-supervisor.md). Nothing
below can start until `R1` to `R4` are answered, because each of them changes
either the supervisor's pin count or a requirement.

- [ ] **R9. Retire `sense.tc_type` from the chamber channel.** The reason is
  now in the code: `kiln_suplink`'s `configure` returns
  `KILN_ERR_UNSUPPORTED`, because the ESP32 no longer owns that front end.
  The config item is therefore dead on this side. `SWR-ACQ-02` now
  fixes the chamber couple as type K, and after `SWA-22` the ESP32 does not
  configure that front end at all, so the config item is meaningless on this
  side. Removing it is a schema change (`KILN_CFG_SCHEMA_VERSION`,
  `SWR-CFG-05` migration), so do it in the same commit as the rest of the
  supervisor's firmware work rather than bumping the schema twice.

- [ ] **R5. Write the requirement deltas.** Six new requirements and nine
  changed ones, listed in section 11 of the design. `SWR-SAF-23` and `SWR-ACQ-02` are
  done; still to do are `SWR-ACQ-01`, `SYS-HW-02`, `SYS-HW-24` superseded, `SWA-04`,
  `SWA-05`, `SG-01`, `SG-03` and `safety.md` sections 5 and 6.

  With `R3` and `R4` settled, §6's independence table is now writable and says
  three specific things. A new row for the supervisor against the ESP32 that
  genuinely says **yes**: separate silicon, firmware, clock and watchdog,
  sharing only the 3V3 rail. A new row, or an amendment to the L1-vs-L2 one,
  recording that the chamber couple is shared by *both* MCUs, so the
  independence is against software and not against a plausible-but-wrong
  reading. And the existing "thermal rules vs. current rules" row promoted from
  defence in depth to **load-bearing**, because with one couple the CT is the
  only physically independent detection channel left in the system.

  `SWR-SAF-17` also needs a sentence: the supervisor's latch does not survive a
  power cycle by design, and the system-level obligation is met by the ESP32's
  persisted fault. Section 3 of the design states the gap that leaves.

- [ ] **R11. The clear button on the panel.** `R4` decided a local button and
  the firmware implements it (edge triggered, held 0.5 s, and a line stuck low
  never arms, so a short fails towards the latch holding). What is left is
  physical: the button itself, its position relative to the HMI, and whether it
  is labelled as clearing the *supervisor* or clearing *a fault*, which are not
  the same thing and the operator cannot see the difference. The ESP32's own
  latched fault still needs its own acknowledgement (`SWR-SAF-17`), so there are two
  acknowledgements and the panel should not imply there is one.

- [ ] **R6. Draw the supervisor.** The part and the pin map are settled:
  STM32G031K8T6, LQFP32, and the assignment is in
  [`firmware/supervisor/README.md`](firmware/supervisor/README.md), taken from ST's own pinout
  and alternate-function database rather than assumed. What is left is the
  schematic.

  Three things the pin map asks of the hardware. The permit line is active
  high and every GPIO is high-impedance between reset and the first
  instruction, so **the series element needs an external pull-down** or there
  is a window at every reset where its state is whatever the board leaks to.
  SWD on `PA13`/`PA14` must come out to test points, not be left as pads,
  because the independence argument rests on this firmware being reviewable and
  flashable on its own. And on the ESP32 side the link lands on
  `KILN_PIN_EXP_IO2` or `KILN_PIN_EXP_IO42`, one pin, receive only; `UART0` is
  the console and must not be used.

- [ ] **R7. Remove what the supervisor supersedes.** `Q5`, `Q6` and `R28` to
  `R31` of section K, and `SYS-HW-24` rewritten rather than deleted, because the
  property it reached for is now delivered differently. Keep the lid's series
  contact (`SYS-HW-21`): it depends on no firmware at all and costs nothing. `K1`
  is answered by this change and `K2` dissolves, the enclosure channel not
  being in the supervisor's remit.

  The lid is likewise not in it. Its switch breaks the coil in hardware, which
  is already safe without firmware, and `SWR-SAF-31`'s latch is gated on a heating
  state only the ESP32 knows; a supervisor latching on it regardless would trip
  on every cold load. So `SWR-SAF-31` and `SYS-HW-21` are unchanged, the lid sense stays
  on the ESP32, and `Q5`'s branch is the only one section K loses.

- [x] **R10. The supervisor is integer-only.** Done. The internal unit is q7,
  1/128 degC in an `int32_t`, which is the MAX31856's own LSB, so the decode is
  a shift and nothing else; the wire stays integer tenths and the one scaling
  step is `sup_q7_to_dc`, which has its own tests. `nm` on the linked image
  finds no `__aeabi_f*` symbol and no `lroundf`, and the text segment went from
  6 736 to 3 488 bytes: the soft-float helpers were 2.7 kB of a 6.7 kB image.

  Three things worth carrying forward. **Every NaN case is gone rather than
  passing**, because an `int32_t` has no NaN, which also removed the `dt`
  sign guard in `sup_step` (a `uint32_t` interval cannot be negative) and two
  tests that existed only to pin NaN behaviour. **Timers saturate rather than
  wrap**, via `add_ms`, because a latch timer that wrapped would fall back below
  its threshold and un-arm a live condition. And `disagreeing` now takes its
  magnitude in unsigned arithmetic, so neither a signed overflow nor negating
  `INT32_MIN` is reachable even for inputs the decode cannot produce.

  `--specs=nano.specs` **stays**, and the reason changed: the only remaining
  C library dependency is `memcpy` and `memset`, which the compiler emits for
  the zero-initialised structs rather than anything anyone wrote. Verified by
  trying `-nostdlib`, which now fails on exactly those two symbols and nothing
  else. Supplying them locally is a decision with its own review, not a flag
  change, so it is not done here.

- [x] **R12. The supervisor's tests run under ASan and UBSan**, and the
  controller's sanitiser job was found to be reporting findings while passing.
  `SWR-TST-20` only ever said the build shall *support* the sanitisers, and the
  supervisor's did not; it does now, through the same `ENABLE_ASAN` option name
  the controller's host build uses, plus a second `ctest` step in the
  `supervisor` CI job.

  The part worth recording is what adding it uncovered. **UBSan's default is to
  print a runtime error and carry on**, leaving the exit code at zero, so
  `ctest` reports PASSED for a test that triggered real undefined behaviour.
  The controller's `asan+ubsan` matrix leg had been green on exactly those
  terms since it was written, over three genuine findings. Both builds now pass
  `-fno-sanitize-recover=undefined`.

  Those three findings turned out to be one class and not defects: `test_faults`
  casting 999 to a fault code, `test_safety_current` casting 200, and
  `test_suplink` feeding 200 as a wire trip reason, each deliberately, to prove
  an out-of-range value from outside is rejected. It is the same behaviour
  `firmware/controller/test/.clang-tidy` already exempts `EnumCastOutOfRange`
  for, so UBSan's `enum` check is excluded in both CMakeLists with that reason
  written down. Everything else UBSan checks now aborts.

  Verified the way the tidy canary is: reintroducing the signed `hi - lo` that
  `disagreeing` avoids makes the sanitised step fail with "signed integer
  overflow" and leaves the plain step passing. A gate that has not been shown
  to fail is not known to be a gate.

- [x] **R11. The supervisor is inside the static-analysis gate.** Found while
  writing `docs/coding-standard.md`: `SWR-NFR-25` says code shall pass the
  project's static analysis configuration, and `firmware/supervisor` passed
  through no analyser at all. `.clang-tidy`'s `HeaderFilterRegex` named only
  `firmware/controller/...`, `tools/tidy.sh` built databases only from the
  controller's two host projects, and `tools/tidy-target.sh` hardcodes
  `kiln_hal_esp32s3`. C11 records the same hole for `main.cpp` and `httpd.cpp`
  and did not mention the supervisor, so the one component whose argument is
  independent reviewability was the one component clang-tidy had never seen.

  `tools/tidy.sh` now drives four databases, 65 translation units. The
  supervisor's two target-only units need no ARM toolchain: they include
  nothing but `<stdint.h>` and the project's own headers, so they are analysed
  against a synthesised database with clang's `thumbv6m-none-eabi` target and
  `-ffreestanding`, with the same vacuous-pass canary `tidy-target.sh` carries.

  It reported 207 findings, all in the supervisor, and the split is the useful
  part. 46 were `macro-to-enum` in the core headers and were **fixed**, every
  object-like macro in `core/` and `protocol/` becoming `constexpr`. 7 were
  `const-correctness` in the suites and were fixed. 3 were narrow false
  positives and carry a `NOLINTNEXTLINE` with the reason. The remaining ~150
  were one class, memory-mapped register access, in `board/` and `src/` only,
  and are exempted by two directory-scoped `.clang-tidy` files on the same
  grounds `kiln_hal_esp32s3` already has one. **Nothing is relaxed for
  `core/` or `protocol/`**, which is the half that matters: the safety function
  is clean under the unmodified root profile.

- [ ] **R8. The supervisor's firmware, and its own test strategy.** Small
  enough to read in one sitting, which is a design constraint and not an
  aspiration. It needs its own host-testable core on the same argument as
  `SWA-01`, and the link needs a test that proves the ESP32 withholds heat on
  silence and on a stale sequence number, which are different failures.

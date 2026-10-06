<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# KilnControl Task List

Outstanding work against [`docs/requirements.md`](docs/requirements.md),
[`docs/architecture.md`](docs/architecture.md), [`docs/safety.md`](docs/safety.md)
and [`docs/security.md`](docs/security.md). Started from a review of
`hardware/kilncontrol.kicad_sch` (120 symbols, 58 nets) on 2026-10-01 and kept
current since.

Priorities:

| | Meaning |
|---|---|
| **P1** | Safety-relevant or blocking. Do before the next board revision, and before any firing. |
| **P2** | Required for a release that claims the requirements are met. |
| **P3** | Correctness polish, consistency, cleanup. |

## State, 2026-10-06

| | |
|---|---|
| **Firmware logic** | Complete and tested. C++20, 360 host tests green plain and under ASan/UBSan, clang-tidy clean on host and target, `esp32s3` builds with zero warnings at 243 kB (88.4 % of the OTA slot free), QEMU boots the image and fires it. |
| **Firmware on hardware** | **4 of 16 ports have a target adapter** (clock, flash, kvstore, system). The twelve missing ones include temperature, heat output and current, so the image cannot drive a real kiln at all. The target build is simulated-plant only. |
| **Schematic** | ERC clean apart from one known `SDO` false positive. Carries the lid interlock and the thermocouple-fault interlock (section K). Does **not** carry the phase strap (`HR-22`) or the second and third CT inputs (`HR-23`). |
| **PCB** | Not updated for section K. `update_pcb_from_schematic` has not been run. |
| **Scope** | **Single-phase kilns only**, decided 2026-10-06 and now fully removed from the requirements and the code (section N). |
| **Blocking release** | No field update path at all (`H2`, `SRR-11`). Local control is the only control and `kiln_hmi` does not exist (`H3`). |

## Firmware critical path

The logic is done; the adapters are not. Nothing below is about algorithms, and
that is the point: every remaining firmware task is a driver.

Ordered by what unblocks the most. The first three are the whole of "can this
thing fire a kiln".

Rows 1 to 9 are done: the adapters in section L, `kiln_hmi` in M, WiFi in O,
the HTTP transport in P and the file store in E. What remains is row 10's
decision about `port_update` and the asset embedding of `C9`, which is what
stands between a working REST API and a browser that can reach it.

| | Port / component | Why it is where it is |
|---|---|---|
| **1** | ~~`port_tc`, MAX31856~~ **done** | No temperature, no anything. Every control and safety rule consumes it. `SR-04`'s fault decode and the `FR-ACQ-12` grace period both live in the adapter's interpretation of the fault register. |
| **2** | ~~`port_heat`, SSR and charge pump~~ **done** | No output. And the charge pump is `AD-05`, the central safety property: the toggle must stay a software-generated square wave and must never be handed to a hardware PWM peripheral, or the property is silently gone. |
| **3** | ~~`port_current`, CT front end~~ **done** | `FR-CUR-12` **refuses to start a run** when current monitoring is unavailable unless it is explicitly disabled, so without this adapter a kiln cannot be started at all except by switching off the electrical cover. |
| **4** | ~~`port_door`, `port_phase`~~ **done** | Small, new, and the features that depend on them (`SR-31`, `FR-CUR-15`) are already written and tested. Cheap to finish. |
| **5** | ~~`port_alarm`, `port_counters`~~ **done** | Small. `port_counters` unblocks `SR-30`, which can never fire today because the counts restart at every boot (`E8`). |
| **6** | ~~`kiln_hmi`, `port_display`, `port_input`~~ **done**, see section M | Now the **only** way to start a firing, because `FR-WEB-26` withdrew run control from the network. Until this exists the only control path is the simulator console. |
| **7** | ~~HTTP transport over `esp_http_server`~~ **done**, see section P |
| **8** | ~~`port_filestore`, the file store~~ **done**, see `E7` | Was the last thing standing between the HMI and a program to run. A fixed-slot raw partition rather than LittleFS (`AD-21`). |
| **9** | ~~`port_net`, WiFi~~ **done**, see section O |
| **10** | `port_update` | **Probably delete it.** `FR-UPD-01` was inverted: no image is accepted over the network, so the port has no caller. Decide this rather than leaving a dead interface (`H2`, `OQ-08`). |

### Where the open items are

| Section | Area |
|---|---|
| [A](#a-schematic) | Schematic and board |
| [B](#b-firmware) | Firmware defects and gaps |
| [C](#c-build-test-and-ci-infrastructure) | Build, test and CI |
| [D](#d-documentation-and-open-questions) | Documentation and open questions |
| [E](#e-milestone-m5-persist-done-2026-10-04) | Milestone M5, persistence |
| [F](#f-c20-migration-and-static-analysis-2026-10-05) | C++20 and static analysis |
| [G](#g-door-interlock-sr-31-2026-10-05) | Door interlock |
| [H](#h-read-only-web-interface-fr-web-26-2026-10-05) | Read-only web interface |
| [I](#i-three-phase-measurement-fr-cur-15-2026-10-05) | Three-phase measurement |
| [J](#j-german-translation-nfr-23-2026-10-05) | German translation |
| [K](#k-hardware-interlock-chain-in-the-coil-circuit-2026-10-06) | Coil interlock chain |
| [L](#l-target-adapters-m2-and-m4b-2026-10-06) | Target adapters |
| [M](#m-kiln_hmi-the-local-interface-2026-10-06) | Local interface |
| [N](#n-single-phase-only-2026-10-06) | Single-phase scope change |
| [O](#o-wifi-fr-net-2026-10-06) | WiFi |
| [P](#p-the-http-transport-and-a-fully-read-only-api-2026-10-06) | HTTP transport |
| [Q](#q-the-file-store-ad-21-2026-10-06) | The file store |

---

## A. Schematic

### A.1 Blocking electrical defects (P1)

- [ ] **A1. Add pull-ups to the MAX31856 `~DRDY` outputs.** The `~FAULT` outputs got theirs in section K (`R28`, `R29`); `~DRDY` still has none, confirmed by query. Original note: All four
  are open-drain and have no pull-up anywhere in the netlist. `TC1_DRDY`
  (U3.7 → U1 IO14) and `TC2_DRDY` (U4.7 → U1 IO21) therefore float between
  assertions, so acquisition timing is undefined. Add 10 k to +3V3 on
  `TC1_DRDY`, `TC2_DRDY`, `TC1_FAULT`, `TC2_FAULT`. Affects FR-ACQ-03,
  FR-ACQ-10, SR-04.

- [x] **A2. Decide what `~FAULT` is for.** Decided 2026-10-06, section K: each `FAULT` output interrupts the contactor coil through its own series MOSFET (`HR-24`). Original note: `TC1_FAULT` and `TC2_FAULT` currently
  reach only test points TP13/TP14, the MCU cannot read them, so SR-04 depends
  entirely on polling the fault register over SPI. Either route both to spare
  GPIOs (IO35–IO41 are unconnected) for interrupt-driven fault detection, or
  drop the test points and record in `hardware/` that fault detection is
  register-polled by design.

- [ ] **A3. Bypass `VBIAS`.** R24/R25 (10 k/10 k off +3V3) present ~5 kΩ of
  source impedance, and the CT secondary returns into that node through J10.2.
  The "mid-rail" reference is therefore modulated by the signal it is supposed
  to reference. Add 10 µF ∥ 100 nF from `VBIAS` to GND. HR-17.

- [ ] **A4. Fix the CT anti-alias filter.** R26 (1 k) + C25 (220 nF) gives
  f<sub>c</sub> ≈ 723 Hz. FR-CUR-03 specifies sampling at ≥ 1 kHz, whose Nyquist
  frequency is 500 Hz, the filter corner sits *above* Nyquist and does not
  anti-alias. Either raise the sample rate (4–8 kHz) and keep a corner near
  300 Hz, or lower the corner to ~200 Hz for a 1 kHz rate. Pick the sample rate
  first, then the filter. HR-17, FR-CUR-03.

- [ ] **A5. Rescale the CT front end for the specified range.** J10 is annotated
  "CT 30A/1V" but FR-CUR-02 requires 0–60 A. At 60 A a 30 A/1 V CT delivers
  2 V<sub>rms</sub> = ±2.83 V<sub>pk</sub> about the 1.65 V bias, which D9 clamps , 
  the reading saturates across the whole upper half of the required range. Even
  at 30 A the swing is 0.24 V to 3.06 V, in the region where the ESP32-S3 ADC is
  least linear. Specify a CT ratio (or add an attenuator) that puts full-scale
  current at roughly 0.5 V<sub>rms</sub>, and record the resulting LSB against
  FR-CUR-02's 0.1 A resolution.

- [ ] **A6. Define `CURR_SENSE` when the CT is absent.** With J10 open there is
  no DC path to the node; it is held only by C25 and diode leakage, so it
  drifts. FR-CUR-11 requires distinguishing "CT disconnected" from "genuinely
  zero current", and FR-CUR-12 requires refusing to start a run when monitoring
  is unavailable. Add a defined bias (e.g. a high-value resistor to a level the
  CT winding cannot produce) so an open input is electrically recognisable, and
  write down the detection rule the firmware will use.

- [ ] **A7. Work out the charge-pump release threshold properly.** This is the
  circuit the whole SR-02 / HR-07 safety property rests on, and architecture
  §16 already flags it as the design's most fragile idea.
  `HEAT_EN` → R19 → C23 (10 µF) → D5 (BAT54S) → `HEAT_EN_DC` → C24 (3.3 µF) ∥
  R20 (47 k) → Q3 gate. Two things need numbers rather than intent:
  - Pumped level is roughly 3.3 V − 2·V<sub>f</sub> ≈ 2.7 V, which is below the
    4.5 V at which the AO3400A's R<sub>DS(on)</sub> is characterised.
  - Decay is τ = 47 k × 3.3 µF ≈ 155 ms through a MOSFET threshold, so release
    is a slow slide through the linear region, not an edge. Establish that the
    contactor's drop-out voltage is reached inside NFR-04's 1 s, that Q3 does not
    dissipate meaningfully on the way through, and that the contactor cannot sit
    partially closed.

  If the margin is thin, a comparator or a retriggerable monostable gives a crisp
  threshold for a few cents. Verify on the HIL jig by halting the safety task
  (TR-17, SR-02, NFR-04).

- [ ] **A8. Budget the +5 V rail and protect the MCU from coil inrush.** J8 feeds
  the contactor coil from the same +5 V node as the LDO, behind the 1.5 A PTC
  (F1). Add up ESP32-S3 WiFi peaks, the LDO's input current, the coil's holding
  and inrush current, both SSR inputs and the buzzer, then confirm F1 and the
  external supply (HR-14 requires MCU + display + coil simultaneously). C1 is
  only 100 µF; coil energisation will sag the rail that the MCU brownout detector
  watches. Consider feeding J8 from `V_FUSED` ahead of D1 so coil transients
  cannot pull the MCU rail down. HR-14, NFR-15, SR-15.

- [ ] **A9. Confirm the 5 V contactor coil is a real part choice.** Mains
  contactors are commonly 24 V AC/DC or 230 V AC coils; pinning J8 to 5 V narrows
  the field sharply. Either name a specific 5 V-coil contactor in the BOM, or
  change the interface to drive an intermediate relay / a higher coil voltage.
  HR-07, SR-03.

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

- [ ] **A13. Clamp the thermocouple inputs.** HR-15 requires the TC inputs to be
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
  Dissipation is ~0.5–0.7 W in SOT-223, roughly +40 °C over ambient; with SR-11
  permitting a 70 °C enclosure, the junction has little headroom and the LDO sits
  on the same board as the cold-junction reference. Either move to a low-dropout
  part with better thermals or a small buck, or document the measured rise and
  the ambient limit it implies.

- [ ] **A17. Add input transient protection on `V_IN`.** F1 and the series D1
  cover overcurrent and reverse polarity, but nothing clamps a surge. The board
  shares an enclosure with a switching contactor coil. Add a TVS / MOV across
  `V_IN`.

- [ ] **A18. Resolve the "(opt)" annotations.** J10 is labelled "CT 30A/1V (opt)",
  but HR-11 makes the current transformer mandatory and FR-CUR-12 makes a run
  refuse to start without it. Re-label, and keep J7 "SSR2 CTRL (opt)" which
  genuinely is optional (HR-12).

- [ ] **A19. Put HR-16 on the schematic as a note.** The CT must be a
  voltage-output type with an integral burden resistor; a current-output CT whose
  burden can be disconnected develops dangerous voltages on an open secondary.
  That is a hard BOM constraint and a safety one, it belongs on the sheet next
  to J10, not only in the requirements. Same for HR-18: state the required CT
  insulation rating, since the CT is the only galvanic isolation in the design.

- [ ] **A20. Review the encoder input network.** R10–R12 (10 k) with C20–C22
  (100 nF) gives a ~1 ms edge into non-Schmitt ESP32-S3 GPIOs feeding the pulse
  counter (HR-05). Slow edges dwelling near V<sub>IH</sub> can double-count.
  Either lower to ~4.7 k/10 nF and lean on the PCNT glitch filter, or verify the
  current values on hardware before committing.

- [ ] **A21. Create the single pin-map artefact HR-10 requires.** Pin assignments
  exist only as schematic net names; there is no `hardware/pinmap.*` and no
  firmware header. One file per board variant, referenced by both.

### A.3 Cleanup and layout follow-up (P3)

- [ ] **A22. Close the J9 designator gap.** Connectors run J1–J8, J10, J11.
  Harmless, but re-annotate or note why.

- [ ] **A23. PCB review is still outstanding.** `hardware/kilncontrol.kicad_pcb`
  has large uncommitted changes and no board was supplied to the design review,
  so DRC, parity and DFM were not run. Separate items once the layout settles:
  verify HR-19/HR-20 (every test point a 1.5 mm square top-layer pad with no
  drill), creepage and clearance around J10 and the SSR/contactor terminals
  (HR-18), thermal isolation between the LDO / contactor driver and the TC
  terminal blocks + MAX31856 cold junctions, and an analogue/digital ground plan
  for the CT and TC front ends.

---

## B. Firmware

Implemented today: `kiln_core` (`pid`, `window`, `tempfilt`, `setpoint`,
`profile`, `safety`, `autotune`, `logrec`, `faults`, `err`) and the
`kiln_ports` interface headers. `kiln_app`, `kiln_hal_esp32s3`, `kiln_hmi`,
`kiln_sim`, `kiln_web`, `main/` and `test/` are empty directories.

### B.1 Safety gaps in existing code (P1)

- [x] **B1. Implement the current subsystem, SR-25 … SR-30 and FR-CUR are
  entirely absent.** The fault codes (21–26) and warnings (109–112) exist in
  `kiln/types.h` and the operator text exists in `faults.c`, but there is no rule
  anywhere that can raise them. Specifically missing:
  - `kiln_ports/include/kiln_ports/port_current.h`: gated RMS acquisition
    (FR-CUR-03/04/05), CT fault status (FR-CUR-11).
  - `kiln_core/current.{c,h}`: RMS over whole mains cycles, settle delay,
    window gating, calibration (FR-CUR-02/06), reference current (FR-CUR-08),
    apparent power and energy (FR-CUR-07).
  - Current fields on `kiln_safety_input_t`: `current_a`, `current_ref_a`,
    `current_flags`, commanded on/off window state. The struct currently has
    none, so the rules cannot be written without changing it.
  - Rules: SR-25 (fail-on), SR-26 (fail-off), SR-27 (weld discrimination, the
    de-assert / wait / re-measure sequence, NFR-27's 1 s + 3 s budget), SR-28
    (deviation vs. reference), SR-29 (over-current), SR-30 (wear and
    intermittent-mismatch warnings).
  - Switching-operation counters (FR-CUR-13), persisted.

  `kiln_core/include/kiln_core/safety.h` documents itself as covering
  "SR-04..SR-13", so the omission is deliberate scoping, but requirements §5.2
  makes the current rules the *primary* detection of relay and element failure,
  with the thermal rules as backstop. This is architecture milestone **M4b**.

- [x] **B2. Invert the default in `kiln_safety_can_clear`.**
  `firmware/components/kiln_core/src/safety.c` ends its switch with
  `default: return true`, so any fault not explicitly listed is clearable.
  `KILN_FAULT_CONTACTOR_WELDED` (code 22) falls into that default, the one fault
  whose operator instruction is "isolate the kiln at its supply, the controller
  can no longer interrupt the current" (SR-27) is one acknowledgement away from
  being cleared. A fail-safe decision must not fail open. Make the default
  `false` with an explicit allow-list, or enumerate every code and add a
  compile-time exhaustiveness check. SR-17, SR-18, SR-27.

- [x] **B3. Stop the temperature rules consuming an invalid reading.**
  `kiln_snapshot_t` carries `kiln_valid` / `case_valid` ("false while inside the
  FR-ACQ-12 grace period") but `kiln_safety_input_t` does not. During the 5 s
  grace window `rule_reversed`, `rule_stuck`, `rule_runaway`, `rule_excursion`
  and `rule_uncommanded` all read `in->kiln_c` regardless. Add the validity flags
  to the input struct and decide per rule whether to hold its timer or reset it.
  SR-04, FR-ACQ-12.

- [x] **B4. Clamp the autotune setpoint to the configured maximum.**
  `kiln_autotune_start` clamps `setpoint_c` to `KILN_TEMP_CEILING_C` (1350 °C)
  only. SR-23 requires *every* temperature setpoint, program target and **tuning
  setpoint** to be clamped to the configured maximum. `autotune.h` pushes this to
  the caller by comment; SR-23 wants it enforced where it cannot be forgotten.
  Pass `max_temp_c` into `kiln_tune_cfg_t` and clamp there.

- [x] **B5. There are no tests.** `firmware/test/host` and `firmware/test/target`
  are empty. TR-13 (host unit tests for every logic component), TR-19 (90 % lines
  / 100 % safety decision branches), TR-23 (every SR covered by an *automated*
  test) and TR-24 (CI blocking) are all unmet, and the safety rules above are
  exactly the code that must not ship untested. See §C.

### B.2 Correctness and timing (P2)

- [x] **B6. Replace the forward-simulation time estimate.**
  `predict_s` in `setpoint.c` copies the whole `kiln_setpoint_t` (~440 bytes,
  including an embedded `kiln_program_t`) onto the caller's stack and then calls
  `kiln_setpoint_tick` up to `PREDICT_MAX_STEPS` = 720 000 times at 1 s steps.
  `kiln_setpoint_remaining_s` and `kiln_setpoint_segment_remaining_s` are
  display/API calls (FR-PRG-06, FR-RUN-05), on the HMI or web task this is a
  multi-millisecond-to-worse blocking loop and a large stack spike, against
  NFR-02's 50 ms ceiling for non-safety activity. The arithmetic is closed-form:
  compute it directly, or compute once per segment transition and cache. Keep the
  current implementation as the test oracle.

- [x] **B7. Recompute the rate regression only when the history changes.**
  `kiln_tempfilt_push` calls `regress_rate_per_h` on every push (≥ 4 Hz per
  FR-ACQ-03), which walks up to 300 samples in `double` arithmetic, software
  emulated on the ESP32-S3's single-precision FPU. The decimated history only
  changes at 1 Hz, so three of every four passes are pure waste. Move the call
  inside the decimation branch, and consider running sums or `float`. NFR-01,
  NFR-02.

- [x] **B8. Make warning bits non-latching.** `kiln_safety_t.warnings` is
  described as "sticky within a run" and is only cleared by
  `kiln_safety_begin_run`. Appendix A defines warnings as non-latching, and
  `KILN_WARN_DUTY_SATURATED` in particular is a live condition (FR-CTL-15) that
  should clear when duty comes off the ceiling. Recompute the mask each
  evaluation, keeping only the warnings that are genuinely episodic
  (`KILN_WARN_INSULATION`) latched.

- [x] **B9. Fix the clamping inconsistency in `kiln_setpoint_heat_allowed`.**
  `kiln_setpoint_tick` compares against `target = clampf(s->target_c, 0,
  cfg.max_temp_c)`, but `heat_allowed` compares the *unclamped* `s->target_c`
  against `seg_start_c`. With a segment target above the configured maximum, the
  setpoint ramps downward (a cooling ramp) while `heat_allowed` still reports
  true, so FR-CTL-13's passive cooling does not happen. Clamp in both places , 
  ideally via one shared helper.

- [x] **B10. Validate string termination in `kiln_profile_validate`.**
  `kiln_program_t.name` and `.description` are fixed arrays that arrive from the
  network and from flash. Validation checks `name[0] != '\0'` but never that
  either field is NUL-terminated, and the struct is `memcpy`'d wholesale into
  `kiln_setpoint_t`. NFR-19 requires every request body to be treated as
  untrusted. Force termination (or reject) during validation.

- [x] **B11. Handle `dt_s > 1 s` in the decimation ring.** The
  `while (decim_accum_s >= 1.0f)` loop in `kiln_tempfilt_push` pushes the *same*
  `filt_c` value several times when a cycle overruns, inserting duplicate points
  at distinct x-positions and flattening the regressed rate, which SR-07 then
  reads as "not rising". Either interpolate, or push once and record the gap.

- [x] **B12. Derive the autotune settle test from the filtered rate.**
  `kiln_autotune_tick` computes `rate_per_h` from a single-sample difference:
  `(pv_c - last_pv_c) / dt_s * 3600`. At a 0.25 s cycle, 0.5 °C of sensor noise
  is 7200 °C/h, against a `settle_rate_c_per_h` threshold of 30. The rate test
  can essentially never pass, so SETTLE always falls through on
  `settle_max_s` (30 min) instead. Feed in `kiln_tempfilt_rate()`, which exists
  for exactly this. FR-TUN.

- [x] **B13. Decouple peak detection from the relay hysteresis.**
  `track_extremes` confirms an extreme only once the PV has reversed by
  `cfg.hysteresis_c`: the same value that sets the relay band. The confirmed
  extreme is therefore the true peak, but the measured half-amplitude is biased
  low by up to `h`, which biases `Ku = 4d/(πa)` **high** and the resulting gains
  with it. Give peak detection its own threshold, and quantify the residual bias
  against the simulator.

- [x] **B14. Harden the logic components against non-finite input.** `pid`,
  `tempfilt` and `autotune` all propagate a NaN `pv_c` straight into persistent
  state (`integral_pct`, `filt_c`, `extreme_c`), and `kiln_clampf` returns NaN
  for a NaN input because both comparisons are false. A NaN must not be able to
  produce a non-zero duty. Gate at the acquisition boundary and assert in the
  core. SR-01, NFR-17.

- [x] **B15. Stop swallowing caller bugs.** `kiln_pid_update` returns the previous
  output on `dt_s <= 0`; `kiln_setpoint_tick` and `kiln_autotune_tick` return
  silently; `kiln_window_init`/`recompute` and `clamp_cfg` quietly rewrite
  invalid configuration. NFR-17 calls an unchecked failure in control or safety
  code a defect. Decide the contract per function, assert, or return
  `kiln_err_t`: and make the config-correcting paths report what they changed.

- [x] **B16. Finish decimation for an unbounded range.**
  `kiln_decimator_push`'s `bucket_ms == 0` path fills buckets sequentially and
  then silently drops everything once full, so an open-ended query returns the
  *first* N samples rather than a downsampled view of the run, the comment in
  `logrec.c` admits this is out of scope. Either implement pair-folding or make
  an explicit range mandatory at the API boundary and reject the open form.
  FR-LOG-10, FR-LOG-11.

- [x] **B17. NULL-check `kiln_decimator_init`.** It `memset`s `d` before any
  validation while `kiln_decimator_push` checks all three of its pointers.

- [x] **B18. Add the missing ports.** Beyond `port_current.h` (B1):
  OTA / firmware update (FR-UPD), system and reset-cause reporting (NFR-15,
  SR-14, SR-15), a second SSR channel on `port_heat` (HR-12, the schematic
  already wires `SSR2` to IO5 and J7, but `set_duty` is single-channel), and the
  relay switching counters of FR-CUR-13.

- [x] **B19. Build the core components the architecture names but that do not
  exist yet:** `core/configmodel` (FR-CFG validation), `core/runstate` (FR-RUN
  state machine, power-loss recovery policy FR-RUN-08), `core/current` (B1).
  Architecture §14.4 lists unit tests for all three.

- [x] **B20. Validate the built-in example programs in a test.** The three
  `k_examples` in `profile.c` are never run through `kiln_profile_validate`.
  "Glass fuse (full)" segment 3 uses `rate_c_per_h = 999` and segment 2 uses
  `rate = 0`; peak target across the examples is 1222 °C against a default
  `max_temp_c` of 1280 °C. All appear valid, assert it, so an edit cannot ship a
  built-in program the validator rejects. FR-PRG-09.

### B.3 Polish (P3)

- [x] **B21. Remove or implement `KILN_TUNE_IDENTIFY`.** The phase is declared,
  has a label string, and is never entered, qualification happens inline in
  `KILN_TUNE_RELAY`.

- [x] **B22. Document the limits of `kiln_pid_bumpless`.** It back-calculates
  `I := u − P` and clamps to `[0, duty_max]`. When the required integral falls
  outside that range the transfer is *not* bumpless. Either say so in the header
  or handle the case.

- [x] **B23. Reject a window period that is not a multiple of the tick.**
  `ticks_per_window = window_ms / tick_ms` truncates, so e.g. 2500 ms / 10 ms is
  fine but 2505 ms silently becomes a 2500 ms window. Validate instead.
  FR-CTL-07.

- [x] **B24. Note the interaction between duty quantisation and current gating.**
  `kiln_window_quantise` promotes a duty whose off-time is shorter than
  `min_off_ms` to a full `KILN_DUTY_MAX`. That removes the commanded-off
  intervals that FR-CUR-04 needs for a leakage measurement and SR-25 needs to
  detect a stuck relay. Decide whether a minimum off-window must be preserved
  while current monitoring is enabled.

- [x] **B25. Align `kiln_logrec_decode` with its documentation.** The header says
  it "returns `KILN_ERR_CORRUPT` on a bad CRC or an all-0xFF (erased, or torn)
  slot"; the code returns `KILN_ERR_NOT_FOUND` for erased. The distinction is
  useful, fix the comment, not the code.

- [x] **B26. Fix the stale record size in `port_logstore.h`.** The file header
  says "fixed 16 byte records"; `KILN_LOG_RECORD_BYTES` is 20 (AD-18, after
  FR-CUR-09 added current). The derived `KILN_LOG_RECS_PER_SECTOR` = 204 is
  correct.

- [x] **B27. Tidy small inconsistencies.**
  - `kiln_pid_init` accepts `duty_max_permille` up to `KILN_DUTY_MAX` and treats
    0 as "default", while `kiln_pid_set_duty_max` enforces a floor of 100. Pick
    one range (`pid.h` documents 100..1000).
  - The `dt_s <= 0` early return in `kiln_pid_update` scales by `10.0f` without
    the `+ 0.5f` the normal path uses.
  - `kiln_profile_validate` returns `KILN_PROG_ERR_NO_SEGMENTS` for a NULL
    program.
  - `kiln_setpoint_replace_remaining`'s comment says "keep name/description"
    while the code assigns the whole updated program.
  - `kiln_safety_eval` reports `KILN_FAULT_SAFETY_DEADLINE` for a NULL argument
    or negative `dt_s`, conflating an invalid argument with SR-13's missed
    deadline (fault code 14).
  - `kiln_safety_band_duty` dereferences `s` unchecked while its neighbours
    validate.
  - `kiln_tempfilt`'s `clamp_cfg` bounds `rate_window_s` (seconds) by
    `KILN_RATE_MAX_POINTS` (samples); they are equal only because decimation is
    1 Hz. Introduce a seconds constant.

### B.4 Implementation notes (2026-10-04)

All of §B is implemented. `kiln_core` gains `current`, `configmodel` and
`runstate`; `kiln_ports` gains `port_current`, `port_counters`, `port_system` and
`port_update`, and `port_heat` gains a second channel and a pin-level write;
`kiln_sim` and a minimal `kiln_app` exist, and the ESP-IDF project root runs the
whole thing under QEMU against the simulated plant
([`docs/simulation.md`](docs/simulation.md)). 15 host suites, green under
ASan/UBSan.

Four things turned up that were not in the brief and changed what was built.
They are recorded here rather than silently absorbed.

- **FR-CUR-08's reference was a hole straight through SR-26.** "The median
  conduction current measured while the elements are cold and fully on" is
  *exactly* the condition a kiln with a failed SSR, an open contactor or dead
  elements is also in, so the reference learned the fault current, SR-26's
  threshold of 20 % of it sat below the noise floor, and the rule compared no
  current against a reference of no current and concluded all was well. Measured
  in the integration rig: SR-26 never fired. `kiln_current` now rejects a median
  outside 0.25–2.00 × the configured nominal, leaving `ref_valid` false so SR-26
  falls back to its absolute nominal-derived floor. Covered by
  `sr26_relay_fail_off_is_caught_long_before_the_thermal_backstop`.

- **SR-05 false-tripped on a healthy kiln.** The rule latched on an
  *instantaneous* 5 °C fall from a running maximum while duty was above 50 %. A
  plant with transport lag and imperfect gains overshoots and then coasts down
  several degrees while the controller is already pushing duty back up, which the
  rule read as a reversed probe, it aborted an otherwise clean firing in the
  integration suite. Added `reversed_confirm_s` (default 30 s): the drop must
  persist. A genuinely reversed couple falls monotonically and does not come back,
  so the confirmation costs it nothing. **Worth reviewing against SR-05's intent.**

- **B13's stated rationale is backwards.** The tasklist says confirming an extreme
  on a reversal of `hysteresis_c` biases the half-amplitude low and therefore `Ku`
  high. Measured: the value recorded is the extreme itself, not the value that
  confirmed it, so on a clean oscillation there is *no* bias at any threshold , 
  `Ku` comes out exact. What the threshold actually buys is noise immunity, and it
  is not optional: at 0.5 °C of sensor noise a 0.25 °C threshold lets noise
  manufacture extrema and the procedure fails on its timeout having learned
  nothing. Noise that does get through inflates the measured amplitude and biases
  `Ku` **low** (≈9 %), which is the conservative direction. Peak detection now has
  its own threshold as B13 asks, but the default is 1.0 °C, not the finer value the
  stated reasoning would imply.

- **Two latent defects found by the new tests.** `kiln_decimator_init` never
  zeroed its caller-supplied storage, so every bucket's min/max came from
  whatever was on the stack (found by the decimation fuzz sweep).
  `kiln_logrec_decode` returned `rec[14] * 5`, up to 1275 per mille, for a record
  that satisfied its CRC but came from a corruption or a future encoder (found by
  the decode sweep).

Still open and deliberately not done here: `FR-CUR-13`'s counters are accumulated
and exposed but not yet *persisted*, because that needs the `port_counters`
adapter from §C; and `OQ-06` (one CT or three) is still open, `kiln_current_t` is
one channel so a second and third are additive, per architecture §16, but D1
should settle it before the component is frozen.

### B.5 Found while building the CI (2026-10-04)

Driving the real image end to end (§C8) turned up two more defects, both now
fixed, and one open question.

- **The charge-pump decay must not be accelerated.** `AD-05`'s pump is hardware
  with a real 1 s time constant, but the simulator integrates in accelerated
  seconds, so under `CONFIG_KILN_SIM_TIME_ACCEL` it expired between two safety
  refreshes 100 ms apart and dropped the contactor continuously, which then
  reads, entirely correctly, as no heater current and latches `SR-26` within
  seconds of starting a firing. `main.c` now scales it. The general rule, worth
  remembering for anything else added to the simulator: a time constant that
  belongs to the *hardware* does not accelerate with the plant.

- **`main.c`'s demonstration gains were four times too high** for the default
  simulated plant and its 90 s of transport lag, which produced 24 °C of
  tracking error and tripped `SR-08` on residual heat soak. Now derived for that
  plant and checked against it: 5.7 °C worst error through the cone 6 example.

  A related observation that needed no change: `SR-08`'s `uncommanded_settle_s`
  has to exceed the plant's transport lag, or residual soak after a duty-zero
  stretch reads as a shorted SSR. The 60 s default is adequate with a controller
  that is not bang-banging, and was left alone on that basis rather than moved
  on thin evidence.

- **`SR-26` and `SR-28` now require a run of consecutive qualifying measurements
  as well as their elapsed window.** `SR-25` counts windows and is the more
  robust rule for it; a rule decided purely on a timer can be tipped over by one
  unrepresentative measurement that happens to be the last before the window
  expires, the first on-window of a run, say, caught while the contactor is
  still closing and reading near zero through no fault of the kiln. Three
  windows by default, which is a fraction of a second. This was *not* the cause
  of the failure above, which was the charge pump; it is a separate robustness
  improvement made while looking at it.

- **`FR-CUR-08` as written may never establish a reference on a real firing,
  which disarms `SR-28`.** The requirement says "the median conduction current
  measured while the elements are cold **and fully on**", implemented literally:
  duty at maximum, below `ref_cold_max_c`. But a program ramping from cold at a
  modest rate never commands full duty, the cone 6 example settles around
  10–25 %, so no reference is learned and `SR-28`, having nothing to compare
  against, never arms. `SR-26` is unaffected: it falls back to the
  nominal-derived floor.

  There is a defensible broader reading. During *any* conduction window the
  elements are by definition fully on, and RMS conduction current does not depend
  on duty, so any valid cold conduction measurement is the same physical
  quantity, and `FR-CUR-05` already discards windows too short to measure.
  Adopting it would make `SR-28` work on every firing. That is a requirements
  interpretation rather than an implementation choice, so it has been left as it
  stands and flagged here: **decide this alongside OQ-07.**


---

## C. Build, test and CI infrastructure

Nothing in architecture §14 exists yet. `kiln_ports/CMakeLists.txt` is the only
build file in the repository.

- [x] **C1. `firmware/CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`,
  `main/`**: the ESP-IDF project root and composition root (CON-01, ESP-IDF
  5.x). Partition table must realise architecture §10.1: a 2 MB OTA pair
  (NFR-13), the circular log partition, the `kilnfs` file store and NVS.

- [x] **C2. `kiln_core/CMakeLists.txt`.** `kiln_core` has ten source files and no
  build file at all, so nothing currently compiles. Needs the dual-target form of
  architecture §14.2, `idf_component_register` under `ESP_PLATFORM`, plain
  `add_library` otherwise.

- [x] **C3. `firmware/test/host/` CMake project.** Plain CMake, no IDF, with
  `ENABLE_COVERAGE` and `ENABLE_ASAN`/UBSan options (TR-20), driven by `ctest`.
  First tests: one per safety rule (TR-23), the PID and autotune arithmetic
  against published values, `logrec` round-trip plus a decode fuzz target over
  arbitrary bytes, and program validation.

- [x] **C4. `kiln_sim` plant simulator.** FOPDT per architecture §14.3 with
  configurable K, τ, dead time, ambient, high-temperature loss and
  seeded-deterministic noise (TR-11, TR-12), **plus the heater-current model and
  the electrical fault injections of TR-27**: relay fail-on, fail-off, welded
  contactor, partial element failure, over-current, CT disconnected. B1's rules
  cannot be tested without it.

- [ ] **C5. `tools/layercheck`.** CI-blocking check that `kiln_core` references no
  IDF or RTOS header and that the component graph is acyclic and layered
  (TR-01, TR-07, AD-14).

- [ ] **C6. `tools/trace`.** Requirement-ID traceability from `docs/` to test
  names; fails on an untraced mandatory requirement, on a test naming a
  nonexistent ID, and on any `SR-*` without an automated test
  (TR-22, TR-23, TR-26).

- [ ] **C7. `tools/logdump`.** Decode a log partition dump to CSV, also the
  cross-check for the `logrec` codec.

- [x] **C8. `.github/workflows/`.** `ci.yml` runs five of the six jobs of
  architecture §14.6, all merge-blocking: the layering and SPDX checks; host
  tests plain and under ASan/UBSan; coverage with TR-19's line floor enforced at
  90 % (measured 97.5 %); the `esp32s3` build with a size report against NFR-13;
  and a QEMU job that boots the real image and asserts it reaches Running, is
  granted heat authority, passes 100 °C and latches no fault. `release.yml`
  builds and publishes on a `v*` tag.

  Still outstanding: clang-tidy and cppcheck; web lint and the asset budget,
  which need `web/` (C9); the API suite, which needs `kiln_web`; TR-19's 100 %
  safety-branch floor (C10, branch coverage is ~83 %, reported but not gated);
  and the KiCAD ERC gate of A11.

---

- [ ] **C9. Vendor the web assets into the firmware.** `web/` is no longer empty:
  it carries the UI, read-only and translated. What is missing is the build step
  that gzips it into `kiln_web/assets` so `AD-11` holds and the assets ship
  inside the image, within the budget of architecture §12.4.

- [ ] **C10. Coverage gate.** 90 % lines on control, safety, setpoint, program and
  autotune is enforced; 100 % of safety decision branches (`TR-19`) is not.
  Branch coverage was around 83 % when last measured.

## D. Documentation and open questions

- [x] **D1. Resolve OQ-06 (one CT or three).** Resolved 2026-10-05 in favour of
  three, see section I. Original note: before freezing the current
  component.** It determines the ADC channel count, so it gates A4, A5 and A6 on
  the hardware side and B1's rule structure on the firmware side. Requirements
  §11 already says it "should be settled before the current component is
  frozen", and that point is now.

- [x] **D2. Document `uncommanded_settle_s` in architecture §8.2.** The
  implementation added a 60 s settle delay before SR-08 arms (so heat soaking
  inward after a high-duty spell is not read as a shorted SSR). The rule table
  lists only "+5 °C over 3 min at 0 % duty". The parameter also means SR-08 is
  effectively inactive during normal firing, when duty is rarely zero for a full
  minute, worth stating explicitly, since it is the reason SR-25 is the primary
  detection.

- [x] **D3. Update the README status table.** It says "no firmware has been
  implemented yet"; `kiln_core` and `kiln_ports` are in place.

- [x] **D4. Note the optimism in the remaining-time estimate.**
  `kiln_profile_duration_s` charges a cooling segment at its stated rate, but
  FR-CTL-13 makes cooling ramps passive, a real kiln cools as fast as it cools.
  FR-PRG-06 / FR-RUN-05 estimates will run short through a cooling segment. Say
  so in the API docs and the UI.

- [ ] **D5. Write the documentation NFR-26 lists:** assembly and wiring, mains
  safety, commissioning, autotuning, program authoring, the REST API, the log
  record format, and current-transformer fitting and calibration. The CT
  commissioning procedure is called out in architecture §16 as the mitigation for
  a CT fitted to the wrong conductor.

- [ ] **D6. Remaining open questions to close:** OQ-01 (cone-based targets),
  OQ-03 (whole-life run-summary retention), OQ-05 (3-zone variant, affects
  whether the control path is written for one zone or N), OQ-07 (element
  temperature coefficient measured or entered).

---

## E. Milestone M5: Persist (done 2026-10-04)

Architecture §17's M5: log ring, run index, programs, configuration,
power-loss recovery. Exit criterion was "`FR-LOG`, `FR-CFG`, `FR-RUN-08` pass;
endurance analysis confirmed by measurement".

- [x] **E1. The circular log ring**, as `kiln_core/logring` over a new
  `port_flash` rather than inside the adapter, recorded as **AD-19**, because it
  revises architecture §5.2. Head discovery, wrap and erase ordering, torn-record
  handling and run selection are all host-tested through a flash fake that
  enforces NOR semantics and can cut power mid-write.

- [x] **E2. Endurance confirmed by measurement**, which was the part of the exit
  criterion that needed evidence rather than arithmetic: 150 h of logging costs
  265 sector erases, one per 34 minutes of running, matching §10.4, the figure
  the whole ten-year `NFR-14` argument rests on. Capacity measures 104 448
  records, 290 h at the default interval against `FR-LOG-07`'s 150 h.

  Measured against the *algorithm*, not the device. On-device measurement is
  still M8 work.

- [x] **E3. `FR-LOG-04`'s out-of-band records**, using the byte AD-18's layout was
  already carrying as reserved. The log now reads as a narrative, run start,
  state change, fault, warning, configuration change, operator action, rather
  than as a temperature series with unexplained steps in it.

- [x] **E4. `app/settings`, `app/program_store`, `app/run_index`** (architecture
  §5.3), plus the logging queue of §10.3 and `SR-17`'s latched fault written with
  a snapshot and committed before the alarm sounds.

- [x] **E5. `FR-RUN-08` power-loss recovery** from the log tail (`AD-09`), with a
  test that counts NVS writes across two minutes of firing and finds none, which
  is the whole point of the decision.

- [x] **E6. The esp32s3 adapters that have no logic left in them**: log
  partition, NVS, clock, reset cause, watchdog. Verified by building: ESP-IDF
  6.0.1, 236 kB image, 89 % of the OTA slot free.

### E.1 Found while building M5

- **`kiln_app_boot` decided `FR-RUN-08`'s band test against an unmeasured
  temperature.** The rule compares the interrupted setpoint against the present
  temperature, and at boot nothing has been acquired, so `kiln_c` still held its
  initialiser of 20 °C. Every kiln more than the band above ambient was refused , 
  which is every kiln worth resuming. It now acquires once before deciding.

- **A host-only component cannot live under `firmware/components/`.** ESP-IDF
  treats everything there as part of the image, so the host fakes moved to
  `firmware/host/`. Structural, rather than an `EXCLUDE_COMPONENTS` list someone
  has to maintain.

- **ESP-IDF 6.0 split the monolithic `driver` component per peripheral.** The CI
  pin moved from the speculative `v5.3` to the `v6.0.1` the build is actually
  verified against.

### E.2 Still outstanding for M5's neighbours

- [x] **E7. The file store.** Written 2026-10-06 as
  `kiln_core/fileslots` over `port_flash`, **not LittleFS**. Recorded as
  `AD-21`, which revises `AD-10`.

  Two things decided it. `CON-04` forbids a build-time fetch and LittleFS is
  not in the ESP-IDF tree, so it was a choice between several thousand vendored
  lines and the in-tree alternatives, and SPIFFS and FAT are neither of them
  power-fail safe. And nothing above the port wanted a filesystem: the two
  callers address `/p/00` to `/p/19` and `/r/00` upward, and between them use
  read, write_atomic, remove and usage, while `list` and `exists` have no
  caller at all. That is a fixed array, which is what `AD-08` already concluded
  for the log.

  One file to a region of two erase sectors, alternating, with the twelve byte
  commit header written after the payload so a cut before it leaves the
  previous copy intact and a cut inside it fails the sequence check or the CRC.
  64 regions in 512 kB against the 40 that `FR-PRG-04` and `FR-LOG-09` need.
  Layout in architecture §10.6.

  24 tests: 16 in `test_fileslots` for the store, 8 in `test_stores_on_flash`
  driving the real `program_store` and `run_index` over it, including power cut
  mid-save and mid-append. The partition is `kilnfs`, custom type 0x40 subtype
  0x01, so the simulated build uses it too and a program saved under QEMU
  survives a reset.

- [x] **E8. Persist `FR-CUR-13`'s switching counters.** Done in section L: the
  `port_counters` adapter keeps them in NVS, so `SR-30`'s wear warning can now
  reach its threshold instead of restarting at zero every boot.

- [x] **E9. The remaining M2/M4b adapters**: MAX31856, SSD1306, encoder, SSR
  outputs and the CT front end. All written, see section L. None has been run
  against a board yet, which is `L2`.


---

## F. C++20 migration and static analysis (2026-10-05)

The firmware moved from C99 to C++20 and `clang-tidy` now gates merge
(`NFR-25`, `TR-24`, closing the static-analysis half of C8). The migration
itself was small, 9 compile errors across 22 597 lines, all `= {0}` on a
struct whose first member is an enum, because the codebase already had no
heap, no VLAs, no `restrict` and explicit context structs.

**Not MISRA, and no part of this may be described as MISRA.** `clang-tidy`
implements no MISRA checks in any release, and the `hicpp-*` module that
approximated High Integrity C++ has been removed from LLVM (absent in 23.x);
its content now lives in `cppcoreguidelines-*`, `bugprone-*` and `cert-*`,
which is what `.clang-tidy` enables. Free MISRA tooling is cppcheck's addon,
which is MISRA **C** 2012 only and needs non-redistributable rule texts; real
MISRA C++:2023 checking is commercial. See `.clang-tidy` and
[`docs/safety.md`](docs/safety.md) section 9.3.

Verified end to end on 2026-10-05 against ESP-IDF v6.0.1: 335 host tests green
plain and under ASan/UBSan, the `esp32s3` image builds with **zero warnings**,
and QEMU boots it and fires to 632 degC across two segments with no fault
latched. The image is 235 920 bytes, 88.8 % of the OTA slot free (`NFR-13`) --
**no size penalty against the C build**, which is what no exceptions, no RTTI,
no STL containers and no heap buys.

The target toolchain (GCC 15.2) caught six sites clang had not: partial
designated initialisers, which C zero-fills silently and C++ reports under
`-Wmissing-field-initializers`, plus one designator written out of declaration
order. All six are now value-initialised and then assigned, which keeps that
warning switched on -- it is what makes every site reconsider itself when a
config struct gains a field.

1 408 findings were fixed automatically and verified. The gate is green with
**no suppressions baseline**:
every remaining check is switched off in `.clang-tidy` with a written reason.
The ones below are deferred work rather than permanent policy, each with the
finding count measured on 2026-10-05.

**Update 2026-10-06: F1b and F2 to F8 are done, and F10 is dropped.** Eight more
checks now gate merge (`macro-to-enum`, `signed-bitwise`, `pro-type-cstyle-cast`,
`const-correctness`, `use-anonymous-namespace`, `use-internal-linkage`,
`incorrect-roundings`, `suspicious-memory-comparison`, `not-null-terminated-result`,
`err33-c`), the target adapters and `host/webhost` are analysed in CI as well as
locally, and the counts below were all measured against the host database only,
which is why several of them were wrong. What the pass actually found is recorded
per item. Verified at each step: 24 host suites green plain and under ASan/UBSan,
`clang-tidy` clean on host, target and webhost under both LLVM 23 and the LLVM 20
CI pins, `esp32s3` builds with zero warnings, and the image came out **288 bytes
smaller** at 243 104 bytes.

- [x] **F1. Analyse `kiln_hal_esp32s3`.** Done: `tools/tidy-target.sh` drives
  from the database `idf.py` emits, filtered into something a clang front end
  will accept (the xtensa flags and the `@response` file have to come out, and
  the toolchain's picolibc headers go in). Found and fixed 42 findings the host
  job could never see, plus one deliberate exception now carried as a narrow
  `NOLINTNEXTLINE`: partition type `0x40` is not a named `esp_partition_type_t`
  because IDF reserves `0x40..0xFE` for application-defined types
  (architecture 10.1). `pro-type-union-access` is disabled for this component
  only -- all five hits were inside `ESP_LOGx` expanding to IDF's own union.

- [x] **F1b. Run `tools/tidy-target.sh` in CI, and cover `host/webhost`.**
  Done, and the vacuous-pass risk this item was really about is now something
  the script refuses to let happen. Before analysing anything it runs every
  translation unit through a check that must fire on any file with a function
  in it, and fails saying `VACUOUS PASS AVERTED` if clang-tidy built no AST for
  one -- so "clean" now means "analysed and clean" rather than "reported
  nothing". The missing picolibc path is fatal rather than ignored, and the
  lookup goes through `IDF_TOOLS_PATH` so it resolves inside the Espressif
  container (`/opt/esp`) as well as locally. A new `tidy-target` CI job builds
  and analyses in that container, installing `clang-tidy` there at the same
  pinned `LLVM_VERSION` the host job uses.

  Three bugs turned up in the wiring, all of which would have shown as a green
  build: `set -e` killed the script silently when the picolibc glob missed, the
  `EXIT` trap tripped over `set -u` on a variable the canary path never
  reached, and `host/webhost` -- its own CMake project, never analysed by
  anything -- carried **28 findings** across exactly the checks this section
  enabled. `tools/tidy.sh` now generates and analyses that database too.

  Both scripts take `TIDY=` so CI's pinned version can be reproduced locally,
  which is how the LLVM 20 / 23 difference below was caught.

- [x] **F2. `cppcoreguidelines-macro-to-enum`: 119 macros, not 585.** The 585
  counted the same header once per translation unit. Converted to `constexpr`
  rather than to the enums the check names, because an unscoped enum is signed
  and that collides head-on with F5: every `KILN_A | KILN_B` would become a
  signed bitwise operation. Each constant is typed deliberately -- bit masks
  `uint32_t`, counts and lengths `size_t`, magics and versions at their
  persisted width, the framebuffer geometry `int` because its coordinate space
  is signed on purpose (`kiln_fb_pixel` clips on `x < 0` rather than faulting).

  Masks are deliberately *wider* than the fields they live in, which is noted
  at the definitions: a `uint8_t` constant promotes to `int` before a bitwise
  operator, so the narrow type would reintroduce the signed-mask problem at
  every site that composes one. The field's own width still bounds what is
  stored.

  The log record format is unchanged: `KILN_LOG_RECORD_BYTES` is still 20, the
  magics and the CRC seed are the same values, and the persistence and fuzz
  suites pass. What the conversion did surface is **eleven loosely-typed
  loops** that `-Wsign-compare` could not see while the bounds were untyped
  macros.

- [ ] **F3. `cppcoreguidelines-use-enum-class`: 31 enums, 223 enumerators,
  2 530 references across 85 files.** Still deferred, and now with the blocking
  question answered rather than open.

  **The C-callability half is settled: the port layer is not C-callable.**
  There is no `extern "C"` anywhere in the firmware except `app_main`, which
  ESP-IDF requires. So `AD-01`'s boundary is a C++ boundary already, and that
  is no longer a reason not to do this.

  What remains is not a technical blocker but a cost. The enumerator names are
  the project's vocabulary: `KILN_FAULT_DOOR_OPEN` is greppable from `SR-31`
  in the requirements, and `tools/trace` parses test names on the same
  convention (`TR-22`, `TR-23`). Scoping renames all 223 of them. The
  transform is compiler-verified -- every unconverted site is a hard error --
  but the naming choice (`kiln_fault_t::DOOR_OPEN`, idiomatic but breaks the
  greps, versus `kiln_fault_t::KILN_FAULT_DOOR_OPEN`, redundant but traceable)
  is a decision about the project's vocabulary rather than about the code.

  Two enums would also get worse: `kiln_warn_bit_t` and `kiln_inject_t` are bit
  *positions*, and scoping them puts a `static_cast` at every mask site --
  precisely the cast noise F5 and F6 just finished removing.

- [x] **F4. `misc-use-anonymous-namespace`: 1 147 findings, not 267.** The 267
  was production only. Production is done: every file-local `static` in
  `firmware/components`, `firmware/main` and `firmware/host` is now in an
  anonymous namespace, along with the file-local struct types
  `misc-use-internal-linkage` wanted moved. 100 groups across 39 files, applied
  in place with no reordering, so the diff is the namespace braces and the
  dropped `static` keywords and nothing else.

  The other 890 are in `firmware/test`, and **796 of them are the two functions
  `KILN_TEST()` expands to**. Those cannot be fixed at the macro -- wrapping
  them means opening a namespace the caller's `{ body }` would have to close,
  and a macro has no way to emit the closing brace -- nor suppressed with a
  `NOLINT`, because the diagnostic lands on the expansion site. Both checks are
  therefore switched off for `firmware/test` only, with that reasoning written
  into `firmware/test/.clang-tidy`. The remaining 94 are hand-written fixtures
  where file-scope `static` is correct C++ and the check is expressing a style.

- [x] **F5. `bugprone-signed-bitwise`: 283 findings, and one real defect.**
  263 of them were one pattern: in `(1u << 3)` the *shift amount* is a signed
  literal, so every flag definition in the tree tripped it. 125 shift amounts
  are now unsigned, which cost nothing and removed the noise hiding the rest.

  The remaining 20 were integer promotion -- `uint8_t` and `uint16_t` promote
  to *int* before they are shifted -- in the CRC-8 and CRC-16 rounds, the
  little-endian readers in five store and codec files, and the framebuffer.
  Each now carries its accumulator in `uint32_t` and masks back, which produces
  the identical value in defined arithmetic.

  **The defect is in the MAX31856 decode** (`hal_tc.cpp`): the hot junction was
  assembled as `((int32_t)r[2] << 24) | ...`, and `r[2] << 24` overflows
  `int32_t` -- undefined behaviour -- for exactly the byte that means "below
  zero". It is assembled in `uint32_t` and converted now. The two deliberate
  *arithmetic* right shifts beside it are kept and carry a `NOLINT` each: they
  sign-extend, and `SR-05` detects a reversed couple by the reading falling, so
  a logical shift would read -1 degC as +524 287.

  `kiln_inject_t` got an explicit `uint32_t` underlying type for the same
  reason -- an unscoped enum whose values fit in `int` gets a *signed* one.
  In `kiln_hal_esp32s3` the check is switched off and only there: 24 findings,
  every one inside `ESP_LOGx`, `WIFI_INIT_CONFIG_DEFAULT` or IDF's own
  `MALLOC_CAP_INTERNAL`, none reachable from this side. That component's own
  bitwise code was audited by hand under the check before it was switched off,
  which is what found the decode above.

- [x] **F6. `cppcoreguidelines-pro-type-cstyle-cast`: 93 on the host, 36 more
  on the target.** The check offers no fix-it, so the pointer casts were
  rewritten mechanically and the compiler was left to arbitrate: a
  `static_cast` between unrelated object pointers is a hard error, so the four
  sites that genuinely needed `reinterpret_cast` identified themselves rather
  than being guessed at.

  `cppcoreguidelines-pro-type-reinterpret-cast` then fires on the replacements,
  and it is left **on**. Byte-level reads go through a named `kiln_bytes_of()`
  in `kiln/types.h` -- the intent is "read these bytes", not "this is secretly
  another type" -- and the three sites that really do reinterpret (the
  `offsetof` arithmetic in `configmodel.cpp`, IDF's `IP2STR`, and `bind()`'s
  `struct sockaddr`) carry a `NOLINT` and a sentence saying why. So a new
  `reinterpret_cast` still has to be argued for.

  On the target this also replaced a DMA frame aliased through a struct pointer
  with a `memcpy`: `adc_continuous_read` does not promise the alignment that
  cast assumed.

- [x] **F7. `misc-const-correctness`: 74 findings, and the port boundary is
  configured out rather than worked around.** The reason applying this broke
  the build is that it analyses *parameters*, so it demands `const void *ctx`
  on every read-only port adapter -- and `void *ctx` is the one signature every
  slot in a vtable shares, whether it mutates or not (`AD-01`). It also wants
  `const char **argv` on `main()`, which is not a signature `main()` may have.

  `misc-const-correctness.AnalyzeParameters: false` says exactly that:
  signatures are the author's call, locals are the check's. All 46 remaining
  findings are locals and all are fixed, pointees included. The auto-fix emits
  east-const (`T const *`) against a tree written west-const, so 31 of those
  were normalised afterwards.

  One trap for whoever bumps `LLVM_VERSION`: that option does not exist before
  LLVM 21, and neither does the pointee analysis it governs. Under the pinned
  20 it is ignored and the check reports values only -- CI is a subset of a
  newer local run, which is the safe direction but does mean local can be
  stricter than the gate. Both scripts take `TIDY=` so the pinned version can
  be reproduced.

- [x] **F8. The real findings, all four fixed, all four checks now gating.**
  - `clamp_cfg()` in `safety.cpp` and `current.cpp` no longer answers "was
    anything clamped?" with a `memcmp` over a padded struct. Each field reports
    for itself through a new `kiln_clampf_moved()`, so a false "clamped" in
    safety code is no longer possible rather than merely unlikely.
  - `configmodel.cpp` writes its NUL explicitly at the copy site. Redundant
    against the `memset` three lines above, which is the point: the
    termination is now provable where the copy happens.
  - The four `(int)(x + 0.5f)` sites are `lroundf`/`llround`. Only `enc_temp`
    could see a negative, and it was already handling it correctly by hand --
    but reading it turned up something the check was not looking for: its
    saturation was two naive comparisons, **both false for a NaN**, so a
    non-finite reading reached a cast that is undefined for it. That is the
    exact failure `kiln_clampf`'s comment in `types.h` warns about. Both the
    temperature and the current paths in the log encoder go through the
    sanctioned clamp now.
  - `cert-err33-c`: 2 124 findings, not 26. The 26 was production only; the
    rest were four `fprintf` calls in the `KILN_TEST` macros, fixed at the
    macro rather than suppressed across 2 100 expansion sites. The 25
    production sites each got a check or an explicit `(void)` with a sentence
    saying why truncation is unreachable, which is the handling `NFR-17` asks
    for.

- [ ] **F9. `kiln_run_record_t` carries 9 padding bytes where 1 is optimal**
  (`clang-analyzer-optin.performance.Padding`, disabled). Reordering would
  invalidate every run record already on a device, so it can only change
  alongside a record-format version bump, if at all.

- [x] **F11. CI's QEMU assertion was stale.** It asserted the firmware logs
  `KilnControl starting`; the firmware has never printed that, at HEAD or
  before, so that line could only fail. Now matched against what `main.cpp`
  actually logs. Worth noting as a reminder that an assertion nobody has seen
  pass is not evidence of anything.

**F10, cppcheck as a second opinion: dropped 2026-10-06.** Decided against
rather than left open, which is what the item itself asked for. Its value here
would have been the MISRA C 2012 addon, and that does not apply to a C++
firmware; the rest of its analysis overlaps `clang-analyzer-*` heavily, and
that is now running over three compile databases rather than one (F1b). Not
worth a second toolchain in CI for the overlap.

---

## G. Door interlock, SR-31 (2026-10-05)

Added on request: a door safety switch input that stops the heater the moment
the door opens. `SR-31` and `HR-21` are new; fault **27**, warning **113**.

The rule has **two tiers**, which is the part worth not losing in a later
refactor. Heat is withheld and the contactor dropped on the **first** open
sample, with no timer at all; that is what "immediately" has to mean for a
door. Only the *latch* waits for `door_confirm_s` (default 200 ms), so a single
sample corrupted by the switching noise of a multi-kilowatt load costs a
fraction of a second of duty instead of stopping a healthy firing (HZ-10). An
implementation that waited 200 ms before dropping the heater would pass a
latch-only test and miss the requirement, so the tiers are tested separately.

It is evaluated **first**, ahead even of `SR-13`: a door that is open is a fact,
where every other rule is an inference from a measurement.

Done: `port_door`, the rule and its clamps, `SR-18` clearability, the fault and
warning rows, `kiln_app` wiring, simulator injection (`d`, `D`) and console
keys, 12 host tests, and the requirement, architecture, safety and simulation
docs. Verified: 347 tests green plain and under ASan/UBSan, clang-tidy clean on
host and target, esp32s3 builds with zero warnings at 236 640 bytes (+720 for
the whole feature).

- [x] **G1. The interlock is only as good as its wiring.** Done 2026-10-06, section K: the lid contacts are in series with the coil and `LID_SENSE` is sensed on IO38. `HR-21` remains a *should*, so `RR-10` still stands for a kiln with no switch fitted. Original note: The software rule is the weaker half by design: `HR-21` wants the
  same normally-closed switch in series with the contactor coil, so the heater
  drops whether or not this firmware is working, the argument `AD-05` makes for
  the charge pump, applied to a second input. The schematic does not yet have
  the input or the series contact. Until it does, SR-31 is software-only and
  RR-10 stands.

- [ ] **G2. HIL: confirm the series contact actually breaks the coil.** As with
  the charge pump (M4), the claim that matters is a hardware one and cannot be
  verified in simulation. Open the door on the bench jig with the safety task
  halted, and observe the contactor.

- [ ] **G3. Decide whether the interlock should be mandatory.** `HR-21` is a
  *should* because many existing kilns have no door furniture to take a switch,
  and making it a *shall* would make the controller unfittable to them. The
  consequence is RR-10: a kiln without one has nothing at all against HZ-13.
  Warning 113 makes the gap visible rather than silent, which is the compromise
 , revisit if the first real installations suggest otherwise.

- [ ] **G4. Expose the door state in the API and on the display.** The
  supervisor knows; `/api/status` and the OLED do not yet say. Wanted for
  FR-WEB-04 and the default screen, and it is the cheap half of making
  warning 113 actually visible.

---

## H. Read-only web interface, FR-WEB-26 (2026-10-05)

The web interface is now an observation surface. No route reachable over the
network can put heat into the kiln, write configuration or clear a latched
fault. Done in the API layer and its tests; the requirement, architecture,
safety and security documents follow it.

Withdrawn from the network: `/api/run*` (start, pause, resume, abort, segment
edits), `/api/manual`, `/api/tune` writes (start, cancel, accept),
`/api/config` writes and `/api/config/defaults`, `/api/fault/ack`, and
`/api/current/calibrate`. Each returns **403 `read_only`** naming where the
control actually is, not 405, which would imply a different verb might work.

The handlers are **deleted, not disabled**. The firmware cannot start a firing
over HTTP because the code to do it is not in the image, which is a stronger
claim than a flag somebody could flip back.

**Program authoring stays**, and that is a capability judgement rather than a
read/write one: a stored curve cannot heat anything until somebody starts it at
the kiln, and authoring a five-segment curve is the one task a rotary encoder
and a 128×64 OLED are genuinely bad at. So the interface is *not* literally
read-only, and the documents say so rather than overclaiming.

Security effect, recorded in `security.md`: threats TH-01 (unauthenticated
command execution), TH-02 (loosening the safety configuration), TH-03 (remote
fault clear) and TH-04 (hostile firmware) are **closed**, and TH-05 (CSRF) is
reduced to editing a stored program. SEC-00 is the one control in that document
that does not depend on the unwritten HTTP transport, because it is enforced in
the API layer that *is* written.

- [x] **H1. The UI client still offers the controls.** Done 2026-10-05: the withdrawn routes are gone from `web/app.js`, the settings and tuning screens are displays, and power, energy and a language selector were added. Original note: `web/app.js` calls every
  withdrawn route. The API refuses them, so the buttons would simply error , 
  which is worse than not being there. The client needs the controls removed
  and the settings and tune screens turned into displays. Being done together
  with the power/energy work (section I) so the UI is reshaped once.

- [ ] **H2. There is no field update path at all.** `FR-UPD-01` was inverted:
  no firmware image is accepted over the network. That closes TH-04 completely
  and leaves no way to ship a fix without physical access, including a
  security fix. `OQ-08` asks what replaces it (USB/serial via `esptool`, or an
  image staged over the network but applied only after a physical confirmation
  at the kiln). **This blocks release**, and is tracked as `SRR-11`.

- [ ] **H3. Local control is now the only control, and the HMI is Not Started.**
  Everything withdrawn from the web is reachable only through `kiln_hmi`, which
  does not exist yet. Until it does, the simulated console is the only way to
  start a firing. M2 and the default screen become blocking rather than merely
  next.


---

## I. Three-phase measurement, FR-CUR-15 (2026-10-05, REVERSED 2026-10-06)

> **Withdrawn and removed.** The project supports single-phase kilns only,
> decided 2026-10-06. The code and requirements described below were taken out
> the same day; section N records the removal. Kept as the record of a
> decision that was made and then reversed, which is worth being able to read
> back.

A phase strap (`HR-22`) and three current transformers (`HR-23`), so every
phase is measured and the power and energy figures cover the whole kiln.
Resolves `OQ-06` in favour of per-phase monitoring and supersedes `ASM-10`.

The safety rules were **not** rewritten. `SR-25` to `SR-30` still take one set
of numbers, now fed the worst phase: the highest reading for fail-on and
over-current, the lowest conduction for fail-off, and the largest departure
from its own reference for deviation. Rewriting eighteen tested safety rules to
iterate phases would have been a large change to the most safety-critical code
in the project for no gain, because the worst phase is what each rule actually
wants. Each channel keeps its own reference, because losing one element group
of three is a step change on one phase and barely visible in a total.

`mains_v` is the **phase** voltage, line to neutral on a three-phase star, so
the total is a plain sum of per-phase VA and not a sqrt(3) line-voltage form.
The two differ by 73 per cent, so every place the figure appears says which it
is.

Done: `KILN_CUR_CHANNELS`, `port_phase`, per-phase state in `kiln_app`,
aggregation helpers, the per-phase burst cycle, warning 114 for a strap that
disagrees with the transformers fitted, per-phase and total reporting on
`/api/current`, simulator support with per-phase element loss, console key `p`,
and seven host tests.

- [x] ~~**I1. The board has one CT input and no strap.**~~ Moot: one CT input is now the specification, not a shortfall. `HR-22` and `HR-23` are
  requirements with no schematic behind them yet. Three conditioned CT inputs
  (`HR-17`, `HR-18` each) and a non-strapping input for the phase select. Until
  the board exists this is verified in simulation only.

- [x] ~~**I2. Delta-connected kilns are not addressed.**~~ Moot with single phase. The power sum assumes a
  star connection with `mains_v` as the line-to-neutral voltage. A delta-wired
  kiln measures line current, and the arithmetic differs. Either detect it,
  configure it, or state the restriction in the commissioning documentation.
  Currently it is stated only in `FR-CUR-17` and in the code comments.

- [ ] **I3. Real power, not apparent.** `FR-CUR-07` says apparent power and
  assumes a resistive load (`ASM-09`), which for a kiln element is very nearly
  true. Measuring real power would need a voltage channel, which the design
  does not have and probably should not grow. Worth closing explicitly rather
  than leaving as an implied limitation.

- [x] ~~**I4. Per-phase energy is not in the run record.**~~ Moot with single phase. `kiln_run_record_t`
  carries one `energy_wh`, now the total across phases. Per-phase energy would
  show which phase is doing the work, but the record is a persisted layout
  (`F9`) and cannot grow without a format version bump.

---

## J. German translation, NFR-23 (2026-10-05)

English and German, selected by `hmi.language`. `NFR-23` always asked for
operator text in a single resource location so it could be translated later;
this is later, and the requirement is now a **M** rather than a **C**.

The single resource location is `kiln_core/faults`: its tables are indexed by
`kiln_lang_t`, and adding a language is a column there and nowhere else. The
firmware serves text already translated rather than sending English for the
browser to re-translate, because the device has to say the same thing on its
own 128x64 display with no browser involved, and two copies of those sentences
would drift. The web interface translates only its own chrome.

Three deliberate splits:

- **The unsuffixed `kiln_fault_label()` and friends stay English.** Diagnostics,
  the event log and requirement identifiers read the same whoever filed the
  report. `_in(code, lang)` is what talks to the operator.
- **Short labels are ASCII transliterations** (`UEBERTEMP`, `TUER OFFEN`,
  `GEHAEUSE HEISS`), because the OLED font is not guaranteed to carry umlauts.
  The long causes use proper German, since those are read in the browser.
  A test holds every German label to the 16-character display width.
- **The browser can override the device.** A workshop with one German-speaking
  potter and one English-speaking one needs that; the device setting is only
  the default.

Done: `kiln_lang_t`, bilingual fault, warning and state tables with the
compile-time index assertions intact, `_in()` accessors with English fallback,
the `hmi.language` config item, `language` on `/api/info`, localised fault and
warning text in the API payloads, `data-i18n` chrome in the web UI with a
language selector, and six host tests including a check that the German column
is not a copy of the English one.

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

## K. Hardware interlock chain in the coil circuit (2026-10-06)

The contactor coil is now a series chain of four independent interrupts, three
of which need no firmware:

```
+5V -[J9 lid, NC]- J8 [COIL] -COIL_DRV- Q3 -COIL_RTN1- Q6 -COIL_RTN2- Q5 - GND
                                     charge pump    TC1 FAULT    TC2 FAULT
```

Added `Q5` and `Q6` (AO3400A) with gate pull-ups `R28`/`R29` (10k to +3V3), and
inserted J9's lid contacts between +5V and the coil so they carry coil current
directly. `LID_SENSE` brings the node back to IO38 through an `R30`/`R31`
10k/18k divider (5 V to 3.21 V) so `SR-31` can still see the door.

Each `FAULT` output drives **its own** MOSFET rather than being wire-ANDed onto
one. Wire-ANDing would have merged `TC1_FAULT` and `TC2_FAULT` into a single
net and cost the per-device test points; a MOSFET each keeps both names, both
probe points, and gives two independent interrupts for the price of one part.

Two things found while doing it, both worth knowing:

- **The flyback diode was on the wrong side.** `D6`'s cathode went to raw `+5V`,
  which the new lid contact bypasses, so opening the lid would have had no
  freewheel path and the inductive kick would have arced across the switch
  contacts. Moved to the `LID_SWITCH` node so the loop is across the coil.
  Recorded as `HR-25` so it cannot be undone by accident.
- **`Q4` already existed** at (76.2, 287.02). The first placement created a
  duplicate reference, caught and removed by UUID. Worth remembering that
  Konnect's `reference` override does not check for collisions.

ERC is unchanged at one error, the known `U3` `SDO` output-output false
positive. No new violations.

- [ ] **K1. The hardware TC interlock is not firmware-independent, and the
  documentation now says so.** The `FAULT` outputs are open-drain: an unpowered
  or absent front end leaves the path closed. The MAX31856 also detects an open
  circuit only once its fault mask is configured, so out of reset the interlock
  does not act. It covers faults the device actively reports; `SR-04` in
  firmware remains the cover for a dead or unconfigured front end. The lid
  contact has no such caveat.

- [ ] **K2. Should an enclosure thermocouple fault really stop the kiln?**
  Both front ends are in the chain, because the request said fault pins. But
  `SR-11` (enclosure over-temperature) is a backstop, and a failed enclosure
  probe killing a firing mid-glaze is a nuisance trip, which `HZ-10` says is
  how protections get disabled. Consider a fitted-by-default `0R` in `Q5`'s
  drain so the enclosure branch can be depopulated without cutting a track.

- [ ] **K3. `D7` is an unwired LED.** Pre-existing, not from this change: the
  coil indicator's cathode is on `COIL_DRV` and its anode goes nowhere. It
  needs a series resistor to the coil supply. Note that taking it from
  `LID_SWITCH` rather than `+5V` makes it indicate "coil actually energised"
  rather than "the MCU asked".

- [ ] **K4. The PCB has not been updated.** This is a schematic-only change;
  `update_pcb_from_schematic` has not been run, so the board still has neither
  the two MOSFETs, the four resistors, nor the rerouted coil path.

- [ ] **K5. HIL: verify each interrupt separately.** Four elements in series
  means four tests: open the lid, pull each `FAULT` low, and halt the safety
  task. Each should drop the contactor on its own. This is the same class of
  claim as `G2` and cannot be settled in simulation.

---

## L. Target adapters, M2 and M4b (2026-10-06)

The firmware can now be built for real hardware. Before this, the `esp32s3`
image bound the simulator and `#error`ed without it; there is now a second
configuration that binds the board, and both build with zero warnings.

Twelve of sixteen ports have a target adapter, up from four:

| File | Ports | Notes |
|---|---|---|
| `board_pins.h` | n/a | The single pin map `HR-10` requires, closing `A21`. Nothing else in the firmware names a GPIO. |
| `hal_tc.cpp` | `port_tc` | Two MAX31856 on one SPI bus, separate chip selects (`HR-02`, `HR-03`). |
| `hal_heat.cpp` | `port_heat` | Two SSR channels and the `AD-05` charge pump. |
| `hal_current.cpp` | `port_current` | CT on ADC1 through the DMA continuous driver. |
| `hal_io.cpp` | `port_door`, `port_phase`, `port_alarm` | Lid sense, phase strap, buzzer with the two `SR-20` patterns. |
| `hal_counters.cpp` | `port_counters` | NVS-backed, RAM accumulate and coarse flush, so `SR-30` can finally fire. |
| `hal_display.cpp` | `port_display` | SSD1306 over I2C. |
| `hal_input.cpp` | `port_input` | Encoder on the pulse counter unit, as `HR-05` requires. |

Three decisions worth keeping:

- **The charge pump has a banner above it.** `enable_refresh()` toggles one
  edge per call and must be called from the safety task. Three changes would
  each leave the board working on the bench and `SR-02` silently gone: setting
  a level instead of toggling, handing the toggle to LEDC or MCPWM, or calling
  it from a timer callback. The third is the subtle one, since an `esp_timer`
  callback runs from a task a hung safety task does not block, so the contactor
  would stay closed while the supervisor was dead.
- **Open-circuit detection is enabled explicitly, and the fault mask cleared.**
  Both default the wrong way out of reset on the MAX31856: `OCFAULT` off and
  `MASK` at `0xFF`. A build that omitted either would have a kiln that cannot
  detect a disconnected probe **and** an `HR-24` interlock that never opens,
  with nothing visibly wrong.
- **The current adapter reports one channel, not three.** See `L1`.

- [x] ~~**L1. `HR-23`'s second and third CT inputs have nowhere to go.**~~ Resolved 2026-10-06 by dropping `HR-23`: single phase needs one ADC channel and the board has exactly that. The constraint itself is real and still documented in `board_pins.h`, because it will bite anyone who later wants a second analogue input for anything at all. Original note: On the
  ESP32-S3 the only ADC usable alongside WiFi is ADC1 (GPIO1 to GPIO10), and
  every one of those pins on this board is taken: `CURR_SENSE`, `EXP_IO2`, a
  strapping pin, `SSR1`, `SSR2`, `HEAT_EN`, `ALARM`, `I2C_SDA`, `TC2_CS`,
  `TC1_CS`. ADC2 is GPIO11 to GPIO20 and is unusable while WiFi is active.
  So three-phase current measurement, which the core and application already
  support, **cannot reach real hardware** as the board stands. Three ways out,
  none free: move `SSR1`, `SSR2`, `ALARM` or `I2C_SDA` above IO20 and free the
  ADC1 channels; add an external SPI ADC on the existing thermocouple bus,
  which also buys simultaneous sampling the internal ADC cannot give; or drop
  `HR-23`. Written out in `board_pins.h` where whoever next touches the pins
  will see it.

- [ ] **L2. None of this has touched hardware.** Every adapter compiles and
  passes analysis, and that is the whole of the evidence. QEMU does not
  emulate SPI, I2C, PCNT or the ADC in any way that would exercise them, so
  the MAX31856 register decode, the SSD1306 init sequence, the quadrature
  decoding and the ADC scaling are all unverified against a real part. This is
  the same class of claim as `G2` and `K5`, and it is the largest untested
  surface in the project.

- [x] **L3. `kiln_hmi` does not exist.** Written 2026-10-06, section M.

- [x] **L4. The HTTP transport does not exist.** Written 2026-10-06, section P. `SRR-02` was settled by removing the password entirely.

- [x] **L5. Every port now has an adapter, or deliberately has none.**
  `port_filestore` was the last one, done in `E7`; WiFi was done in section O.
  `port_logstore` and `port_filestore` are both provided by `kiln_core` over
  `port_flash` rather than by an adapter (`AD-19`, `AD-21`), which is the right
  place for logic a host test needs to cut power on. `port_update` is the one
  genuinely open case and probably wants deleting rather than implementing, now
  that `FR-UPD-01` accepts no network image.

- [ ] **L6. The hardware build is not in CI.** CI builds the simulated
  configuration only. The hardware one is a second `idf.py` invocation with a
  different `SDKCONFIG_DEFAULTS`, and it is the configuration that matters for
  a release.

---

## M. kiln_hmi, the local interface (2026-10-06)

The screens, the menus and the confirmation flows (`FR-HMI-02` to
`FR-HMI-15`). With this and section L, an operator standing at the kiln can
select a stored program, start it, pause, resume, abort and acknowledge a
fault, which is `FR-HMI-10` and is the whole reason this was urgent: `FR-WEB-26`
withdrew all of that from the network, so until now a real board had no way to
start a firing at all.

**The component commands nothing.** `kiln_hmi_update()` takes a view and an
encoder event, renders a frame, and returns an *action* for the caller to carry
out. It never calls `kiln_app`. That is what lets all 21 tests run with no
display, no application and no kiln, and it keeps the decision to start a
firing in `kiln_app` where the state machine and its guards already are. The
HMI asks; the application decides, and a refusal is logged rather than
swallowed.

One font, scaled. `FR-HMI-03` wants the chamber temperature legible at two
metres, which is the 5x7 glyph set drawn at scale 3 rather than a second large
font to get wrong: one table, one place to fix a glyph, about 300 bytes saved.

Screens: default (`FR-HMI-02`, `FR-HMI-04`), fault with precedence
(`FR-HMI-06`), menu, program selection, confirmation (`FR-HMI-11`), network
(`FR-HMI-07`), diagnostics and info (`FR-HMI-08`).

Three behaviours the tests pin down because they are easy to get wrong:

- **The confirmation defaults to NO.** A confirmation is only worth having if
  the lazy answer is the safe one, so pressing straight through the menu does
  not start a kiln.
- **A fault takes the screen mid-menu and cannot be dismissed** while the
  condition holds (`FR-HMI-06`); it leaves on its own when the fault clears.
- **The dim timeout is suspended while a fault is up** (`FR-HMI-12`). A kiln
  that blanked its own fault screen would be worse than one with no screen.

Rendering is checked by counting lit pixels in a region rather than by
comparing golden images. A golden image fails on every deliberate layout change
and says nothing about why; "the big number occupies the top left and is bigger
than everything else" survives a nudge and still catches the regression that
matters.

- [x] **M1. The HMI now has programs to offer.** Closed by `E7`: the file store
  is real on target, `kiln_program_store_seed` puts the built-in examples in it
  on first boot, and they survive a reboot. `FR-HMI-10` lists them and can
  start one, so the local control path is complete end to end. It has still not
  been run against a display and an encoder, which is `L2` and `M5`.

- [x] **M2. The network screen has something to show.** Closed by section O:
  `port_net` has a WiFi adapter, so `FR-HMI-07`'s screen shows the address
  instead of "no wifi adapter". That address is how the device is reached while
  `FR-NET-04` is unmet (`O1`).

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

## N. Single-phase only (2026-10-06)

The project supports **single-phase kilns only**. This reverses the decision of
section I, taken the previous day, and reopens `OQ-06` as closed the other way:
one current transformer on one conductor, which is what `HR-11` always asked
for and what the board actually has.

Why it is a clean decision rather than a retreat: a three-phase kiln monitored
on one phase is worse than not supporting three-phase at all. A fault confined
to an unmonitored phase would be caught only by the thermal rules, slowly, and
the power and energy figures would cover a third of the load while looking
exactly like a whole-kiln number. Partial support here is the kind that gets
trusted. The README now says so under **Scope**.

The firmware for it is written, tested and working, so it has to come out
deliberately rather than by deleting whatever mentions a phase. **Nothing below
is done yet**; the tree still builds and passes as it stands, three-phase code
and all.

- [x] **N1. Remove the phase strap.** Done 2026-10-06. `port_phase.h`, the HAL init, the pin, the simulator strap and console key `P`, and `kiln_app_phases()` are all gone.

  Original note: `port_phase.h`, `hal_io.cpp`'s
  `kiln_hal_phase_init`, `KILN_PIN_PHASE_SEL` in `board_pins.h`, the
  `kiln_sim` strap and its console key `P`, and `kiln_app_phases()`. The strap
  was never on the schematic (`I1`), so nothing physical has to change.

- [x] **N2. Collapse `KILN_CUR_CHANNELS` back to one.** Done 2026-10-06. The constant is removed entirely, `kiln_app_t::cur` is a single `kiln_current_t` again, the burst loop is single-channel, and the aggregation helpers are gone. The safety inputs were reconnected to `kiln_current_amps/ref/flags` directly and the suites re-run, which is the part that needed care.

  Original note: The constant, the
  `kiln_current_t cur[]` array in `kiln_app_t`, the per-channel burst loop in
  `kiln_app_window_tick`, and the aggregation helpers `cur_worst_amps`,
  `cur_ref_of_worst`, `cur_flags_any`. Take care here: the aggregation is what
  currently feeds `SR-25` to `SR-30`, so the single-channel path has to be
  reconnected to those inputs and the safety tests re-run, not just deleted
  around.

- [x] **N3. Remove warning 114 and its row.** Done 2026-10-06, both languages, and code 114 is out of appendix A. The compile-time index assertions passed, which is what they are for.

  Original note: `KILN_WARN_PHASE_MISMATCH` in
  `types.h`, the row in `faults.cpp` (both languages), the raise in
  `app.cpp`, and code 114 in requirements appendix A. The compile-time index
  assertions in `faults.cpp` will catch a half-done job, which is what they
  are for.

- [x] **N4. Revert the API payload.** Done 2026-10-06. `/api/current` no longer reports `phases`, `channels` or `per_phase`, and `power_basis` no longer claims to be summed over phases.

  Original note: `/api/current` currently reports
  `phases`, `channels` and a `per_phase` array, and `kiln_app_apparent_va` and
  `kiln_app_energy_wh` sum across channels. Single phase makes all of that one
  number again. The web UI reads `apparent_va` and `energy_wh` and will not
  notice; the `power_basis` string should stop saying "summed over measured
  phases".

- [x] **N5. Retire the requirements.** Done 2026-10-06. `HR-22`, `HR-23`, `FR-CUR-15`, `FR-CUR-16` and `FR-CUR-17` are gone, `OQ-06` is resolved the other way, `ASM-10` is reinstated as "the kiln is single phase", and three-phase is named in requirements §12 as out of scope.

  Original note: `HR-22` (strap), `HR-23` (three CT
  inputs), `FR-CUR-15` (one CT per phase), `FR-CUR-16` (strap read at boot)
  and `FR-CUR-17` (summed power). `FR-CUR-07` stays and reverts to a single
  measurement. `OQ-06` is resolved the other way, and **`ASM-10` comes back**:
  a three-phase kiln is out of scope, so the assumption is no longer that one
  phase is representative but that the kiln has one.

- [x] **N6. Revert the safety and architecture notes.** Done 2026-10-06. `RR-02` is reopened and now reads as what it is: a three-phase kiln cannot be fired safely with this controller, carried by the installer who must not fit it to one. The architecture risk row follows.

  Original note: `safety.md`'s `RR-02`
  was closed on the strength of per-phase monitoring and has to reopen in its
  original form, and the architecture risk table entry with it. This matters
  more than the code: `RR-02` is a statement about what the design does not
  catch, and leaving it marked closed would be a false claim in the safety
  concept.

- [x] **N7. Delete `test_phases.cpp`.** Done 2026-10-06. Seven tests removed; 374 remain.

  Original note: Seven tests, of which
  `frcur15_an_element_lost_on_one_phase_is_seen_electrically` was the one that
  justified the whole feature. Worth reading once before deleting, because the
  rig it builds is a compact example of driving the app against the simulator
  and may be worth keeping in another form.

- [x] **N8. Decide what the simulator keeps.** Done 2026-10-06. `KILN_INJ_ELEMENT_PARTIAL` scales the whole heater again, so `SR-28`'s deviation test exercises something the hardware can actually produce.

  Original note: `KILN_INJ_ELEMENT_PARTIAL`
  applies the loss to phase 0 only, which was written for the three-phase
  case. On a single-phase kiln it should go back to scaling the whole heater,
  or `SR-28`'s deviation test is exercising something the hardware can no
  longer produce.

---

## O. WiFi, FR-NET (2026-10-06)

Station with access-point fallback, exponential-backoff reconnection, SNTP and
the diagnostics of `FR-NET-09`. Thirteen of sixteen ports now have a target
adapter.

`FR-NET-07` shaped the whole file: losing the network must not interrupt,
pause or otherwise alter a running firing. So nothing blocks, nothing is on
the control path, and the radio is started **after** `kiln_app_init` so the
kiln is already fully operational before it is touched. Reconnection is
event-driven with a 1 s to 60 s backoff, because an adapter that retried in a
tight loop would satisfy "connects to WiFi" and quietly violate `FR-NET-07`.

The port is not in `kiln_app_ports_t` and that is deliberate: the application
has no use for the network. It is read by the HMI's network screen and by the
web API, neither of which is a control path.

Two things the compiler caught that were worth fixing rather than silencing:

- **The configuration strings are longer than the radio's fields.** An SSID cut
  at 32 characters simply never associates, leaving an operator looking at a
  kiln that will not join a network it can see. `copy_checked()` refuses and
  logs which item is too long, and unusable credentials fall through to the
  provisioning AP, which is how the operator gets to correct them.
- **An open access point is never started.** `FR-NET-05` wants at least eight
  characters; where none is configured the adapter derives a device-unique
  passphrase from the MAC rather than leaving the provisioning page open to
  anyone in radio range.

- [ ] **O1. `FR-NET-04` (mDNS, `kiln.local`) is not implemented.** mDNS left
  the ESP-IDF tree for the component manager, and `CON-04` forbids a
  build-time fetch from an unpinned source. `E7` met the same constraint over
  the file store and resolved it by not needing the dependency at all; mDNS has
  no such escape, since the protocol is the feature. Adding a managed
  dependency for a convenience feature would be the wrong trade against a
  constraint the project applies everywhere else, so the device is reachable by
  IP until mDNS is vendored deliberately. The address is on the HMI network screen, which is
  where an operator would look anyway.

- [ ] **O2. The image grew by 540 kB.** WiFi takes it from 292 kB to 831 kB,
  which is 60.4 % of the OTA slot still free, so `NFR-13` holds comfortably.
  Worth watching once the web assets are embedded (`C9`): that budget is
  architecture 12.4's and it has not been measured against a real asset build.

- [ ] **O3. Untested against a radio.** Same class as `L2`: it compiles and
  passes analysis. Nobody has watched it associate, fall back to the AP,
  recover from a dropped connection, or sync time.

---

## P. The HTTP transport, and a fully read-only API (2026-10-06)

`kiln_web` is reachable on the device. Writing the transport turned up a
blocker first, and the answer changed the shape of the interface.

**The web password could not be set by anything.** `FR-WEB-26` had already
withdrawn configuration writes from the network, the local interface has no
text entry, and nothing else in the firmware wrote
`security.web_password`. So `FR-WEB-23`'s optional password protection was
permanently off, and writing authentication would have meant writing a gate
that could never be armed. An authentication gate that cannot be armed is
worse than none, because it reads as protection.

The decision was to make the interface **fully read-only**, which dissolved
the problem rather than working around it:

- Program authoring is withdrawn. `/api/programs` reads; it no longer creates,
  edits, deletes or copies. `FR-WEB-12` becomes a viewer and `FR-WEB-13` is
  withdrawn outright, since there is no save to re-validate.
- `FR-WEB-23` is withdrawn and `security.web_password` is gone from the
  configuration schema. `SRR-02` (salted hash against stored plaintext) and
  `OQ-S1` are both closed by there being no password.
- `SEC-01` and `SEC-04` dissolve in `security.md`: no client may change state,
  authenticated or not. `TH-05` (CSRF and DNS rebinding) **closes** without
  any code, because a forged request can only read. That is the one threat the
  decision closed for free.
- The web UI's editor became a viewer and now makes zero write calls.

The transport itself is small as a result. It registers **GET and nothing
else**, so a write does not reach a handler at all, which is a second
independent enforcement of `FR-WEB-26` alongside api.cpp's own refusal.
Neither is load-bearing on its own and both are cheap.

Three decisions in the transport worth keeping:

- **An over-long URI is refused, not truncated.** A path cut at the buffer
  length could match a *different* route from the one the client asked for.
- **A truncated response is a 500, not a partial body.** A client cannot tell
  a cut JSON document from a corrupt one, and serving the first 4 kB of a
  response would be the worst of both. The log route streams precisely so that
  it never hits this.
- **One shared response buffer, not one per session.** The handlers are
  serialised by the server's single task, so four sessions do not cost four
  buffers, which is what keeps this inside architecture 13.4's 40 kB.

- [ ] **P1. The UI is not served.** The transport answers `/api/*` and nothing
  else: `C9`'s asset embedding is not done, so there is no `index.html` in the
  image and a browser at the device's address gets a 404. The API is usable
  with `curl` today. `AD-11` wants the assets gzipped into the image, which
  also needs the budget of architecture 12.4 measured for the first time.

- [ ] **P2. Server-Sent Events are not implemented.** `FR-WEB-05` wants live
  values pushed at least once a second, and `kiln_api_telemetry_event()` is
  written and tested for exactly that, but the transport has no `/api/events`
  handler. The UI falls back to nothing: it reads `/api/status` on a timer
  already, so this is a refinement rather than a gap in function.

- [ ] **P3. Where do firing programs come from now?** Recorded as `OQ-09`.
  Authoring is gone from the web and there is no local editor, so a user
  cannot create a curve of their own: they get the seeded examples of
  `FR-PRG-09` and nothing else. This is a real functional gap and the most
  likely thing to make somebody reverse the read-only decision. A file import
  at provisioning time is probably the cheapest answer.

- [ ] **P4. Untested against a client.** It compiles and passes analysis. No
  request has been made of it. The host API suite covers every route's
  behaviour, so what is unverified is specifically the socket half: URI
  splitting, chunked streaming, the four-session limit, and whether a slow
  client can hold a buffer long enough to matter.

---

## Q. The file store (`AD-21`, 2026-10-06)

`port_filestore` was the last port without an implementation and the last thing
between the HMI and a program it could actually run. It is now
`kiln_core/fileslots` over a `port_flash`, a fixed array of two-sector regions
in a raw `kilnfs` partition, and **not** LittleFS as `AD-10` originally said.
The reasoning and the layout are in `E7` and architecture §10.6.

What this closed: `E7`, `E8`, `E9`, `L5`, `M1`, `M2`, and critical path rows 8
and 9. `M1` is the one that mattered: the local control path now has programs
at both ends of it.

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

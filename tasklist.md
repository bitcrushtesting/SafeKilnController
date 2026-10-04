<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# KilnControl — Task List

Derived from a review of `hardware/kilncontrol.kicad_sch` (120 symbols, 58 nets)
and `firmware/` as of 2026-10-01, against
[`docs/requirements.md`](docs/requirements.md) and
[`docs/architecture.md`](docs/architecture.md).

Priorities:

| | Meaning |
|---|---|
| **P1** | Safety-relevant or blocking. Do before the next board revision / before any firing. |
| **P2** | Required for a release that claims the requirements are met. |
| **P3** | Correctness polish, consistency, cleanup. |

Review state: schematic ERC clean apart from one known false positive; design
review reports 1 error, 3 info. Firmware is `kiln_core` logic plus `kiln_ports`
headers only — no build system, no tests, no HAL, no app, no web.

---

## A. Schematic

### A.1 — P1 Blocking electrical defects

- [ ] **A1. Add pull-ups to the MAX31856 `~DRDY` and `~FAULT` outputs.** All four
  are open-drain and have no pull-up anywhere in the netlist. `TC1_DRDY`
  (U3.7 → U1 IO14) and `TC2_DRDY` (U4.7 → U1 IO21) therefore float between
  assertions, so acquisition timing is undefined. Add 10 k to +3V3 on
  `TC1_DRDY`, `TC2_DRDY`, `TC1_FAULT`, `TC2_FAULT`. Affects FR-ACQ-03,
  FR-ACQ-10, SR-04.

- [ ] **A2. Decide what `~FAULT` is for.** `TC1_FAULT` and `TC2_FAULT` currently
  reach only test points TP13/TP14 — the MCU cannot read them, so SR-04 depends
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
  frequency is 500 Hz — the filter corner sits *above* Nyquist and does not
  anti-alias. Either raise the sample rate (4–8 kHz) and keep a corner near
  300 Hz, or lower the corner to ~200 Hz for a 1 kHz rate. Pick the sample rate
  first, then the filter. HR-17, FR-CUR-03.

- [ ] **A5. Rescale the CT front end for the specified range.** J10 is annotated
  "CT 30A/1V" but FR-CUR-02 requires 0–60 A. At 60 A a 30 A/1 V CT delivers
  2 V<sub>rms</sub> = ±2.83 V<sub>pk</sub> about the 1.65 V bias, which D9 clamps —
  the reading saturates across the whole upper half of the required range. Even
  at 30 A the swing is 0.24 V to 3.06 V, in the region where the ESP32-S3 ADC is
  least linear. Specify a CT ratio (or add an attenuator) that puts full-scale
  current at roughly 0.5 V<sub>rms</sub>, and record the resulting LSB against
  FR-CUR-02's 0.1 A resolution.

- [ ] **A6. Define `CURR_SENSE` when the CT is absent.** With J10 open there is
  no DC path to the node — it is held only by C25 and diode leakage, so it
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

### A.2 — P2 Required before fabrication

- [ ] **A10. Add VBUS decoupling.** The design review flags `VBUS` as having no
  decoupling at all. Add 1 µF (USB spec caps bulk VBUS capacitance at 10 µF)
  plus 100 nF near J2.

- [ ] **A11. Record an ERC exclusion for the shared SPI `SDO`.** ERC reports one
  error: U3.11 and U4.11 (`SDO`, both Output) are connected. This is correct for
  a shared SPI bus — the MAX31856 tri-states `SDO` when `~CS` is high — but the
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
  switching multi-kilowatt load — add TVS or clamp diodes to the rails on all
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
  dropout at ESP32-S3 WiFi peaks — close to falling out of regulation.
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
  That is a hard BOM constraint and a safety one — it belongs on the sheet next
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

### A.3 — P3 Cleanup and layout follow-up

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

### B.1 — P1 Safety gaps in existing code

- [x] **B1. Implement the current subsystem — SR-25 … SR-30 and FR-CUR are
  entirely absent.** The fault codes (21–26) and warnings (109–112) exist in
  `kiln/types.h` and the operator text exists in `faults.c`, but there is no rule
  anywhere that can raise them. Specifically missing:
  - `kiln_ports/include/kiln_ports/port_current.h` — gated RMS acquisition
    (FR-CUR-03/04/05), CT fault status (FR-CUR-11).
  - `kiln_core/current.{c,h}` — RMS over whole mains cycles, settle delay,
    window gating, calibration (FR-CUR-02/06), reference current (FR-CUR-08),
    apparent power and energy (FR-CUR-07).
  - Current fields on `kiln_safety_input_t`: `current_a`, `current_ref_a`,
    `current_flags`, commanded on/off window state. The struct currently has
    none, so the rules cannot be written without changing it.
  - Rules: SR-25 (fail-on), SR-26 (fail-off), SR-27 (weld discrimination — the
    de-assert / wait / re-measure sequence, NFR-27's 1 s + 3 s budget), SR-28
    (deviation vs. reference), SR-29 (over-current), SR-30 (wear and
    intermittent-mismatch warnings).
  - Switching-operation counters (FR-CUR-13), persisted.

  `kiln_core/include/kiln_core/safety.h` documents itself as covering
  "SR-04..SR-13", so the omission is deliberate scoping — but requirements §5.2
  makes the current rules the *primary* detection of relay and element failure,
  with the thermal rules as backstop. This is architecture milestone **M4b**.

- [x] **B2. Invert the default in `kiln_safety_can_clear`.**
  `firmware/components/kiln_core/src/safety.c` ends its switch with
  `default: return true`, so any fault not explicitly listed is clearable.
  `KILN_FAULT_CONTACTOR_WELDED` (code 22) falls into that default — the one fault
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

### B.2 — P2 Correctness and timing

- [x] **B6. Replace the forward-simulation time estimate.**
  `predict_s` in `setpoint.c` copies the whole `kiln_setpoint_t` (~440 bytes,
  including an embedded `kiln_program_t`) onto the caller's stack and then calls
  `kiln_setpoint_tick` up to `PREDICT_MAX_STEPS` = 720 000 times at 1 s steps.
  `kiln_setpoint_remaining_s` and `kiln_setpoint_segment_remaining_s` are
  display/API calls (FR-PRG-06, FR-RUN-05) — on the HMI or web task this is a
  multi-millisecond-to-worse blocking loop and a large stack spike, against
  NFR-02's 50 ms ceiling for non-safety activity. The arithmetic is closed-form:
  compute it directly, or compute once per segment transition and cache. Keep the
  current implementation as the test oracle.

- [x] **B7. Recompute the rate regression only when the history changes.**
  `kiln_tempfilt_push` calls `regress_rate_per_h` on every push (≥ 4 Hz per
  FR-ACQ-03), which walks up to 300 samples in `double` arithmetic — software
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
  true, so FR-CTL-13's passive cooling does not happen. Clamp in both places —
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
  at distinct x-positions and flattening the regressed rate — which SR-07 then
  reads as "not rising". Either interpolate, or push once and record the gap.

- [x] **B12. Derive the autotune settle test from the filtered rate.**
  `kiln_autotune_tick` computes `rate_per_h` from a single-sample difference:
  `(pv_c - last_pv_c) / dt_s * 3600`. At a 0.25 s cycle, 0.5 °C of sensor noise
  is 7200 °C/h — against a `settle_rate_c_per_h` threshold of 30. The rate test
  can essentially never pass, so SETTLE always falls through on
  `settle_max_s` (30 min) instead. Feed in `kiln_tempfilt_rate()`, which exists
  for exactly this. FR-TUN.

- [x] **B13. Decouple peak detection from the relay hysteresis.**
  `track_extremes` confirms an extreme only once the PV has reversed by
  `cfg.hysteresis_c` — the same value that sets the relay band. The confirmed
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
  code a defect. Decide the contract per function — assert, or return
  `kiln_err_t` — and make the config-correcting paths report what they changed.

- [x] **B16. Finish decimation for an unbounded range.**
  `kiln_decimator_push`'s `bucket_ms == 0` path fills buckets sequentially and
  then silently drops everything once full, so an open-ended query returns the
  *first* N samples rather than a downsampled view of the run — the comment in
  `logrec.c` admits this is out of scope. Either implement pair-folding or make
  an explicit range mandatory at the API boundary and reject the open form.
  FR-LOG-10, FR-LOG-11.

- [x] **B17. NULL-check `kiln_decimator_init`.** It `memset`s `d` before any
  validation while `kiln_decimator_push` checks all three of its pointers.

- [x] **B18. Add the missing ports.** Beyond `port_current.h` (B1):
  OTA / firmware update (FR-UPD), system and reset-cause reporting (NFR-15,
  SR-14, SR-15), a second SSR channel on `port_heat` (HR-12 — the schematic
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
  `max_temp_c` of 1280 °C. All appear valid — assert it, so an edit cannot ship a
  built-in program the validator rejects. FR-PRG-09.

### B.3 — P3 Polish

- [x] **B21. Remove or implement `KILN_TUNE_IDENTIFY`.** The phase is declared,
  has a label string, and is never entered — qualification happens inline in
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
  useful — fix the comment, not the code.

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

### B.4 — Implementation notes, 2026-10-04

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
  elements is also in — so the reference learned the fault current, SR-26's
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
  rule read as a reversed probe — it aborted an otherwise clean firing in the
  integration suite. Added `reversed_confirm_s` (default 30 s): the drop must
  persist. A genuinely reversed couple falls monotonically and does not come back,
  so the confirmation costs it nothing. **Worth reviewing against SR-05's intent.**

- **B13's stated rationale is backwards.** The tasklist says confirming an extreme
  on a reversal of `hysteresis_c` biases the half-amplitude low and therefore `Ku`
  high. Measured: the value recorded is the extreme itself, not the value that
  confirmed it, so on a clean oscillation there is *no* bias at any threshold —
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
adapter from §C; and `OQ-06` (one CT or three) is still open — `kiln_current_t` is
one channel so a second and third are additive, per architecture §16, but D1
should settle it before the component is frozen.

### B.5 — Found while building the CI, 2026-10-04

Driving the real image end to end (§C8) turned up two more defects, both now
fixed, and one open question.

- **The charge-pump decay must not be accelerated.** `AD-05`'s pump is hardware
  with a real 1 s time constant, but the simulator integrates in accelerated
  seconds, so under `CONFIG_KILN_SIM_TIME_ACCEL` it expired between two safety
  refreshes 100 ms apart and dropped the contactor continuously — which then
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
  expires — the first on-window of a run, say, caught while the contactor is
  still closing and reading near zero through no fault of the kiln. Three
  windows by default, which is a fraction of a second. This was *not* the cause
  of the failure above, which was the charge pump; it is a separate robustness
  improvement made while looking at it.

- **`FR-CUR-08` as written may never establish a reference on a real firing,
  which disarms `SR-28`.** The requirement says "the median conduction current
  measured while the elements are cold **and fully on**", implemented literally:
  duty at maximum, below `ref_cold_max_c`. But a program ramping from cold at a
  modest rate never commands full duty — the cone 6 example settles around
  10–25 % — so no reference is learned and `SR-28`, having nothing to compare
  against, never arms. `SR-26` is unaffected: it falls back to the
  nominal-derived floor.

  There is a defensible broader reading. During *any* conduction window the
  elements are by definition fully on, and RMS conduction current does not depend
  on duty, so any valid cold conduction measurement is the same physical
  quantity — and `FR-CUR-05` already discards windows too short to measure.
  Adopting it would make `SR-28` work on every firing. That is a requirements
  interpretation rather than an implementation choice, so it has been left as it
  stands and flagged here: **decide this alongside OQ-07.**


---

## C. Build, test and CI infrastructure

Nothing in architecture §14 exists yet. `kiln_ports/CMakeLists.txt` is the only
build file in the repository.

- [x] **C1. `firmware/CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`,
  `main/`** — the ESP-IDF project root and composition root (CON-01, ESP-IDF
  5.x). Partition table must realise architecture §10.1: a 2 MB OTA pair
  (NFR-13), the circular log partition, NVS and LittleFS.

- [x] **C2. `kiln_core/CMakeLists.txt`.** `kiln_core` has ten source files and no
  build file at all, so nothing currently compiles. Needs the dual-target form of
  architecture §14.2 — `idf_component_register` under `ESP_PLATFORM`, plain
  `add_library` otherwise.

- [x] **C3. `firmware/test/host/` CMake project.** Plain CMake, no IDF, with
  `ENABLE_COVERAGE` and `ENABLE_ASAN`/UBSan options (TR-20), driven by `ctest`.
  First tests: one per safety rule (TR-23), the PID and autotune arithmetic
  against published values, `logrec` round-trip plus a decode fuzz target over
  arbitrary bytes, and program validation.

- [x] **C4. `kiln_sim` plant simulator.** FOPDT per architecture §14.3 with
  configurable K, τ, dead time, ambient, high-temperature loss and
  seeded-deterministic noise (TR-11, TR-12), **plus the heater-current model and
  the electrical fault injections of TR-27** — relay fail-on, fail-off, welded
  contactor, partial element failure, over-current, CT disconnected. B1's rules
  cannot be tested without it.

- [ ] **C5. `tools/layercheck`.** CI-blocking check that `kiln_core` references no
  IDF or RTOS header and that the component graph is acyclic and layered
  (TR-01, TR-07, AD-14).

- [ ] **C6. `tools/trace`.** Requirement-ID traceability from `docs/` to test
  names; fails on an untraced mandatory requirement, on a test naming a
  nonexistent ID, and on any `SR-*` without an automated test
  (TR-22, TR-23, TR-26).

- [ ] **C7. `tools/logdump`.** Decode a log partition dump to CSV — also the
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
  safety-branch floor (C10 — branch coverage is ~83 %, reported but not gated);
  and the KiCAD ERC gate of A11.

---

## E. Milestone M5 — Persist (done 2026-10-04)

Architecture §17's M5: log ring, run index, programs, configuration,
power-loss recovery. Exit criterion was "`FR-LOG`, `FR-CFG`, `FR-RUN-08` pass;
endurance analysis confirmed by measurement".

- [x] **E1. The circular log ring**, as `kiln_core/logring` over a new
  `port_flash` rather than inside the adapter — recorded as **AD-19**, because it
  revises architecture §5.2. Head discovery, wrap and erase ordering, torn-record
  handling and run selection are all host-tested through a flash fake that
  enforces NOR semantics and can cut power mid-write.

- [x] **E2. Endurance confirmed by measurement**, which was the part of the exit
  criterion that needed evidence rather than arithmetic: 150 h of logging costs
  265 sector erases, one per 34 minutes of running, matching §10.4 — the figure
  the whole ten-year `NFR-14` argument rests on. Capacity measures 104 448
  records, 290 h at the default interval against `FR-LOG-07`'s 150 h.

  Measured against the *algorithm*, not the device. On-device measurement is
  still M8 work.

- [x] **E3. `FR-LOG-04`'s out-of-band records**, using the byte AD-18's layout was
  already carrying as reserved. The log now reads as a narrative — run start,
  state change, fault, warning, configuration change, operator action — rather
  than as a temperature series with unexplained steps in it.

- [x] **E4. `app/settings`, `app/program_store`, `app/run_index`** (architecture
  §5.3), plus the logging queue of §10.3 and `SR-17`'s latched fault written with
  a snapshot and committed before the alarm sounds.

- [x] **E5. `FR-RUN-08` power-loss recovery** from the log tail (`AD-09`), with a
  test that counts NVS writes across two minutes of firing and finds none — which
  is the whole point of the decision.

- [x] **E6. The esp32s3 adapters that have no logic left in them**: log
  partition, NVS, clock, reset cause, watchdog. Verified by building: ESP-IDF
  6.0.1, 236 kB image, 89 % of the OTA slot free.

### E.1 — Found while building M5

- **`kiln_app_boot` decided `FR-RUN-08`'s band test against an unmeasured
  temperature.** The rule compares the interrupted setpoint against the present
  temperature, and at boot nothing has been acquired, so `kiln_c` still held its
  initialiser of 20 °C. Every kiln more than the band above ambient was refused —
  which is every kiln worth resuming. It now acquires once before deciding.

- **A host-only component cannot live under `firmware/components/`.** ESP-IDF
  treats everything there as part of the image, so the host fakes moved to
  `firmware/host/`. Structural, rather than an `EXCLUDE_COMPONENTS` list someone
  has to maintain.

- **ESP-IDF 6.0 split the monolithic `driver` component per peripheral.** The CI
  pin moved from the speculative `v5.3` to the `v6.0.1` the build is actually
  verified against.

### E.2 — Still outstanding for M5's neighbours

- [ ] **E7. Vendor LittleFS and write the file-store adapter.** `AD-10` puts
  programs and run records in LittleFS, which is not in the ESP-IDF tree, and
  `CON-04` forbids a build-time fetch — so it has to be vendored rather than
  pulled as a managed component. Until then the simulated build uses a RAM file
  store in `kiln_sim`, so the path is exercised but nothing survives a reboot.

- [ ] **E8. Persist `FR-CUR-13`'s switching counters.** They are accumulated and
  exposed and feed `SR-30`, but the `port_counters` adapter does not exist, so
  they restart at zero on every boot and the wear warning can never fire on a
  real kiln.

- [ ] **E9. The remaining M2/M4b adapters**: MAX31856, SSD1306, encoder, SSR
  outputs and the CT front end. These are what stand between the current state
  and a kiln that can actually be fired.


- [ ] **C9. `web/`.** Empty. FR-WEB and CON-06 need UI sources with a build output
  vendored into `kiln_web/assets`, inside the asset budget of architecture §12.4.

- [ ] **C10. Coverage gate.** 90 % lines on control, safety, setpoint, program and
  autotune; 100 % of safety decision branches (TR-19).

---

## D. Documentation and open questions

- [ ] **D1. Resolve OQ-06 (one CT or three) before freezing the current
  component.** It determines the ADC channel count, so it gates A4, A5 and A6 on
  the hardware side and B1's rule structure on the firmware side. Requirements
  §11 already says it "should be settled before the current component is
  frozen", and that point is now.

- [x] **D2. Document `uncommanded_settle_s` in architecture §8.2.** The
  implementation added a 60 s settle delay before SR-08 arms (so heat soaking
  inward after a high-duty spell is not read as a shorted SSR). The rule table
  lists only "+5 °C over 3 min at 0 % duty". The parameter also means SR-08 is
  effectively inactive during normal firing, when duty is rarely zero for a full
  minute — worth stating explicitly, since it is the reason SR-25 is the primary
  detection.

- [x] **D3. Update the README status table.** It says "no firmware has been
  implemented yet"; `kiln_core` and `kiln_ports` are in place.

- [x] **D4. Note the optimism in the remaining-time estimate.**
  `kiln_profile_duration_s` charges a cooling segment at its stated rate, but
  FR-CTL-13 makes cooling ramps passive — a real kiln cools as fast as it cools.
  FR-PRG-06 / FR-RUN-05 estimates will run short through a cooling segment. Say
  so in the API docs and the UI.

- [ ] **D5. Write the documentation NFR-26 lists:** assembly and wiring, mains
  safety, commissioning, autotuning, program authoring, the REST API, the log
  record format, and current-transformer fitting and calibration. The CT
  commissioning procedure is called out in architecture §16 as the mitigation for
  a CT fitted to the wrong conductor.

- [ ] **D6. Remaining open questions to close:** OQ-01 (cone-based targets),
  OQ-03 (whole-life run-summary retention), OQ-05 (3-zone variant — affects
  whether the control path is written for one zone or N), OQ-07 (element
  temperature coefficient measured or entered).

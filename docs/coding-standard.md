<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Coding standard

| | |
|---|---|
| **Document** | Coding standard |
| **Project** | Safe Kiln Controller, PID kiln controller |
| **Version** | 0.1 (draft) |
| **Date** | 2026-10-08 |
| **Status** | For review |
| **Derives from** | [`UR-CON-05`](01_user_req.sdoc), [`SWR-NFR-25`](03_software_req.sdoc), [`SWA-20`](04_software_arch.sdoc), [`UR-REG-010`](01_user_req.sdoc), [`UR-REG-011`](01_user_req.sdoc) |
| **License** | GPL-3.0-or-later |

[`v-model-process.md` §5](v-model-process.md) names "coding guidelines" as an
input to the implementation phase, and until now there was no such document:
`.clang-format` and `.clang-tidy` were the de facto standard, with their
reasoning in comments and nothing stating what the rules were for. This is that
statement. It is also what an assessment under
[`UR-REG-010`](01_user_req.sdoc) or [`UR-REG-011`](01_user_req.sdoc) asks for by
name, and the reason those two standards are the frame for everything below.

> **Nothing in this repository is MISRA-compliant, and this document does not
> change that.** Section 3 says what the claim actually is and section 10 says
> what it would take to make the stronger one. The distinction matters because
> "MISRA" is the word people reach for when they mean "written carefully", and
> an assessor will ask for the compliance artefacts rather than the adjective.

## 1. Which standards ask for this, and what they ask for

Two of the project's regulatory items reach the source code, and they ask for
different things. Confusing them is the easiest way to do the wrong work.

| Instrument | What it asks of the code | Where it is answered |
|---|---|---|
| **EN IEC 60730-1 Annex H**, with part 2-9 ([`UR-REG-010`](01_user_req.sdoc)) | A declared software class, and the fault and error detection measures that go with it: invariable memory, variable memory, CPU registers, program counter and sequence, clock, I/O. Annex H prescribes **no coding standard and no language subset.** | [`safety-supervisor.md`](safety-supervisor.md) and `core/selfcheck.cpp`. Not this document. |
| **EN ISO 13849-1:2023 cl. 4.6** ([`UR-REG-011`](01_user_req.sdoc)) | For safety-related embedded software at PL c or d: a documented **language subset and coding guideline**, defensive programming, modular design, code review, static analysis, and separation of safety from non-safety functions. PL e defers to IEC 61508-3. | **This document**, plus [`SWA-22`](04_software_arch.sdoc) for the separation. |
| **IEC 61508-3**, the equivalent route [`UR-REG-011`](01_user_req.sdoc) permits | Table B.1: a coding standard is highly recommended from SIL 2 up, with no dynamic objects, no dynamic variables, limited pointers, no recursion and a strongly typed language. §7.4.4 adds offline **tool qualification**. | This document for the subset; section 10 for the tools, which are not qualified. |

So: 60730 gives the diagnostics, and 13849 or 61508 are why a coding standard
exists at all. Neither names a standard to adopt. They require that one be
adopted, documented, and shown to be followed with deviations recorded.

Clause and table numbers shift between editions, and `OQ-R3` is open on which
editions apply. The obligations above are stable across the editions this was
written against; the citations will need fixing when `OQ-R3` closes.

## 2. Scope

This governs every line of C++ in `firmware/`. It does **not** govern the web
assets in `web/`, the Python in `tools/`, the OpenSCAD in `housing/` or the
fixture firmware in `fixture/`, none of which is in the safety function or in
the shipped image.

Within `firmware/` there are two components and they are not held to the same
standard, deliberately:

| | `firmware/supervisor` | `firmware/controller` |
|---|---|---|
| Role | The whole of one safety function ([`SWA-22`](04_software_arch.sdoc)) | Everything else, including four of the five protection layers |
| Language | **C++17** (section 4) | C++20 ([`SWA-20`](04_software_arch.sdoc)) |
| Size | 3.5 kB of image, four core source files | 22 597 lines |
| Target standard | MISRA C++:2023, section 3 | The high-integrity profile, section 3 |
| Floating point | **None** (section 5) | Permitted; the ESP32-S3 has an FPU |

The supervisor is held tighter because its argument is that it can be read and
verified on its own, and because it is small enough that the tighter rules are
affordable. A rule the controller cannot afford and the supervisor can is
stated for the supervisor rather than being dropped from both.

## 3. What the standard is, and what it is not

**Today: a high-integrity C++ profile, enforced by `clang-tidy`.** That is
`SWA-20`'s claim and it is the honest one. The content is
`cppcoreguidelines-*`, `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`,
`performance-*`, `portability-*` and part of `readability-*`, which is the same
material the removed `hicpp-*` module approximated, reached by its current
names. `WarningsAsErrors` is `'*'`, there is no suppressions baseline, and CI
blocks merge on any finding.

**Not MISRA, and the word must not be used.** `clang-tidy` implements no MISRA
checks in any release. The `hicpp-*` module that approximated High Integrity
C++ has been removed from LLVM entirely. Cppcheck's free addon is MISRA **C**
2012 only and needs rule texts nobody may redistribute. Genuine MISRA C++:2023
checking is commercial. `.clang-tidy` says all of this at length and
[`safety.md` §9.3](safety.md#93-obligations-on-anyone-changing-this-design)
makes it an obligation on anyone changing the design.

**Target for the supervisor: MISRA C++:2023.** It is the current subset, it
absorbed AUTOSAR C++14 and MISRA C++:2008, and it is the thing an assessor
under 13849 or 61508 recognises as the "suitable language subset" those clauses
ask for. The alternatives were considered and are worse: MISRA C:2012 would
mean rewriting in C; CERT C++ is a security standard and not accepted as a
functional-safety subset, though its checks are enabled already; AUTOSAR C++14
is withdrawn; JSF++ is C++03; HICPP is gone from the tooling.

Adopting it is a **tooling purchase and a paperwork exercise, not a rewrite.**
Section 10 lists what is outstanding.

## 4. Language and dialect

**The supervisor is C++17.** MISRA C++:2023 is written against C++17, so
building the supervisor at C++17 makes the standard applicable as written
rather than as adapted. This costs nothing: no C++20 construct appears anywhere
in `core/`, `protocol/`, `board/` or `src/`, and all four compile clean at
`-std=c++17`. `SWA-20`'s reason for C++20, the 152 designated initialisers that
name the controller's safety defaults, is a controller concern; the supervisor
has none.

**The build sets it as of 2026-10-09**, in both
`firmware/supervisor/CMakeLists.txt` and the suites' own, so the code the tests
translate is the code that ships. It cost nothing twice over: the four suites
pass unchanged, and the linked image is **byte-identical** to the C++20 one,
same 3 488 bytes and the same `0x6b9284a1` program-memory digest. The language
level moved; not one instruction did.

**The controller stays C++20**, per `SWA-20`.

Both halves are built at a `-std=gnu++20` floor today. Neither uses a GNU
extension deliberately; `gnu++` rather than `c++` is inherited from ESP-IDF.

### The subset, as enforced by the compiler

These are not style preferences. Each removes a language feature whose failure
mode is hard to see in review, and each is a flag so that a violation is a
build error rather than a review finding. `firmware/supervisor/CMakeLists.txt`
is the authority; the controller's build carries the first three.

| Flag | What it forbids, and why |
|---|---|
| `-Wall -Wextra -Werror` | The baseline. `SWR-NFR-25` requires it. |
| `-Wshadow` | A name shadowing an outer one is how an assignment reaches the wrong variable while reading correctly. |
| `-Wconversion` | An implicit narrowing is the arithmetic error that compiles. On a safety path that is a silently truncated temperature. |
| `-Wdouble-promotion` | Supervisor only, and now mostly moot: there is no floating point left to promote (section 5). Kept so that reintroducing one is loud. |
| `-fno-exceptions` | No hidden control flow and no unwinder. A throw across the trip logic would be a path no test took. |
| `-fno-rtti` | No dynamic type information, and therefore no `dynamic_cast`. |
| `-fno-threadsafe-statics` | No hidden guard variable and no hidden lock on a function-local static. |
| `-fno-use-cxa-atexit` | No registration of destructors to run at an exit that never comes. |
| `-fno-common` | A duplicate tentative definition is a link error rather than a silent merge. |
| `-fstack-usage` | Emits per-function stack usage. Not yet consumed; section 10. |

### Rules the flags cannot express

- **No heap.** Zero `malloc`, `free`, `new` or `delete` across the firmware, by
  design and not by accident: `cppcoreguidelines-no-malloc` and
  `-owning-memory` are enabled, and there is no allocator on either target.
- **No variable-length arrays**, and no recursion in the supervisor. The
  controller's JSON parser recurses by design and boundedly, which is why
  `misc-no-recursion` is off project-wide; the supervisor contains no recursion
  and no function pointer at all, which is what makes a static stack bound
  possible once section 10 is done.
- **No object-like macros in the supervisor's `core/` or `protocol/`.** They
  are `constexpr`, so the compiler has a type to check. The two vendor headers
  `board/pins.h` and `board/cortex_m0plus.h` are the exception, stated in
  `board/.clang-tidy`: a pin number must read exactly as ST's datasheet spells
  it.
- **Positive logic for every safety predicate.** A field named for the healthy
  state, so that a zero-initialised struct withholds heat rather than granting
  it. `chamber_valid` and `diag_ok` are both this, and `sup_input_t` documents
  why at each one. This is the single most load-bearing convention in the
  supervisor and it has a test that does nothing else.
- **Layer purity.** The deciding code takes no hardware, RTOS or ESP-IDF
  header, so every rule is host-testable (`SWR-TST-01`, `SWA-01`, `SWA-14`).

## 5. Floating point, and the C library

This section is referenced from `sup_proto.h`, `trip.h` and
`protocol/src/sup_proto.cpp`, because it is the reasoning behind a convention
those files would otherwise just assert.

**The supervisor contains no floating point.** Not discouraged: absent, and
checked by the fact that `nm` on the linked image finds no `__aeabi_f*` symbol.

The Cortex-M0+ has no FPU. Every float compare, add and multiply was a call
into a libgcc soft-float helper, and `lroundf` came from newlib-nano, which is
why the link needed `--specs=nano.specs` rather than `-nostdlib`. Three things
followed from that, and only the first is the one people notice:

1. **It was 2.7 kB of the image**, out of 6.7 kB. Removing it took the
   supervisor from 6 736 to 3 488 bytes of text.
2. **A safety function depended on a library the project does not build, does
   not test and does not qualify.** Under 61508-3 that library is in scope for
   the argument; under 60730 Annex H it is code in the declared software class.
   The cheapest way to discharge that is not to link it.
3. **Every comparison had a NaN case.** `effective_c`, `over_temp`,
   `disagreeing` and `enc_temp` each carried a comment explaining that a
   comparison against a NaN is false whichever way it is written, and one test
   existed purely to pin that. That is correct, subtle, and exactly the kind of
   reasoning that gets "simplified" by the next reader. An `int32_t` has no NaN,
   so those cases are **unrepresentable rather than handled**, which is a
   stronger property than a passing test.

The same three apply to the controller with the first two reversed: the
ESP32-S3 has hardware floating point and the libraries are already linked, so
float there costs nothing and is permitted.

**Rules:**

- No floating-point type in `firmware/supervisor`, at all. Reintroducing one
  needs an entry in this section saying why, not just a passing build.
- Conversions between a sensor's unit and a human unit happen at **one** named,
  tested function, never inline at a call site. In the supervisor that is
  `sup_q7_to_dc`.
- An integer conversion function must be **total**: defined for every value of
  its argument type, including values no sensor can produce, because the struct
  it reads is public and the next thing to fill it may not be the decode.
  `sup_q7_to_dc` clamps before it multiplies for this reason, and
  `disagreeing` takes its magnitude in unsigned arithmetic so that neither a
  signed overflow nor negating `INT32_MIN` is reachable.
- An accumulating safety timer **saturates, never wraps**. A latch timer that
  wrapped would fall back below its threshold and un-arm a condition that is
  still present, which is the one arithmetic failure in the trip logic that
  would cost protection rather than cause a nuisance trip. `add_ms` is four
  lines and has a test.

### The fixed-point convention

| Unit | Where | Why |
|---|---|---|
| **q7**, 1/128 degC, `int32_t` | Inside the supervisor: the decode, the trip logic, the thresholds | It is the MAX31856's own LSB, so the decode is a shift and nothing else. There is no scaling step in which a reading can be rounded or truncated. |
| **decidegrees**, 0.1 degC, `int16_t` | The link frame and `sup_report_t` | What the wire already carried, and what the log record uses, so the two never need converting between each other. |
| **degrees**, `float` | The controller, from its port boundary inwards | An FPU, and a port contract already expressed in degrees. |

The two integer units differ by a factor of 10/128, which is 5/64 exactly, so
the conversion is a multiply and a shift with no division. The unit is named
once, in `sup_proto.h`, because a unit named in two places eventually means two
things.

## 6. Formatting

`.clang-format`, and it is not negotiable per file: `clang-tidy --fix` consults
it, so a mismatch silently reformats every region a fix touches. Four-space
indent, 100 columns, Stroustrup braces, pointer bound to the name. `SortIncludes`
is off because the include order is sometimes load-bearing.

Comments are expected to say **why**, not what. The tree is written this way
throughout and it is the single thing that makes an independent review of the
supervisor possible in an afternoon. A comment restating the code is noise; a
comment recording the failure the line prevents is the review.

## 7. How a deviation is recorded

There is no suppressions baseline and there will not be one: a baseline records
violations as accepted and then stops being read. There are exactly three ways
to deviate, in order of preference, and all three are visible in a diff.

1. **Fix it.** The default, and what happened to 46 macro constants in the
   supervisor's core headers when the gate first reached them.
2. **`NOLINTNEXTLINE(check)` with the reason in the comment above it**, for a
   false positive on one line. Used for the two linker-symbol subtractions in
   `main.cpp`, where the analyser cannot see the linker script that places the
   two symbols adjacently, and for the `extern` array declaration in
   `selfcheck.h` that `bugprone-dynamic-static-initializers` reports despite
   having no initialiser.
3. **A directory-scoped `.clang-tidy` with `InheritParentConfig: true`**, for a
   finding class that is inherent to a boundary. Each disabled check carries a
   written reason in that file. There are four such files and each states what
   it does *not* relax:

| File | Relaxes | Because |
|---|---|---|
| `firmware/supervisor/board/.clang-tidy` | int-to-pointer casts, C-style and reinterpret casts, macro-to-enum, internal linkage | A memory-mapped peripheral is an integer address turned into a pointer. The addresses are generated from ST's CMSIS-SVD, which is a stronger control than a cast style. |
| `firmware/supervisor/src/.clang-tidy` | the same five | `main.cpp` touches the same registers. Deliberately identical, so the two cannot drift. |
| `firmware/supervisor/test/.clang-tidy` | two checks, both on the `KILN_TEST()` macro expansion | The diagnostic lands on the expansion site, so no `NOLINT` can reach it. |
| `firmware/controller/test/.clang-tidy`, `.../kiln_hal_esp32s3/.clang-tidy` | negative-test and ESP-IDF-macro findings | Pre-existing; their own files carry the reasoning. |

Nothing is relaxed for `firmware/supervisor/core`,
`firmware/supervisor/protocol`, `firmware/controller/components` or
`firmware/controller/main`. The supervisor's safety function is gated by the
unmodified root profile and is clean under it.

A new entry in any of these files is a change to this document.

## 8. What is enforced, and by what

Every row is CI-blocking unless it says otherwise.

| | Gate | Covers |
|---|---|---|
| Compiler | `-Wall -Wextra -Werror` and the subset flags of section 4 | Every target |
| `clang-tidy` | `tools/tidy.sh`, four compile databases, 65 translation units | The controller host build, `host/webhost`, the supervisor's core, protocol and suites, and the supervisor's two target-only units against a synthesised bare-metal database |
| `clang-tidy` | `tools/tidy-target.sh` | `kiln_hal_esp32s3`, which needs the ESP-IDF headers |
| Unit tests | `ctest`, plain and under ASan and UBSan | The controller: 25 suites |
| Unit tests | `ctest`, plain and under ASan and UBSan | The supervisor: 4 suites, 81 tests |
| Coverage | `gcovr`, 90 % line floor | `kiln_core` |
| MC/DC | `tools/mcdc.sh`, 80 % floor, measured at 100 % | The supervisor's trip logic, decode, self-checks and wire format |
| Layer purity | `tools/layercheck` | **Does not exist.** `safety.md` §9.3 cites it. Section 10 |
| Traceability | `tools/trace` | Not yet written, tasklist C6 |

Two of those deserve naming because they are easy to get wrong.

**A clean report must be provable.** `tools/tidy.sh` and
`tools/tidy-target.sh` both refuse to report clean unless they can show
`clang-tidy` built an AST for every unit they claim to have analysed. The
failure mode being guarded is not a red build, it is a green one that parsed
nothing: a cross-target database whose headers do not resolve produces no
findings and looks like success. Any future script that analyses code it did
not build needs the same canary.

The sanitiser runs had the same defect and it went unnoticed for longer.
**UBSan's default is to print a runtime error and continue**, which leaves the
exit code at zero, so `ctest` reports PASSED for a test that triggered real
undefined behaviour. The controller's ASan job had been green on those terms
over three findings for as long as it had existed. Both builds now pass
`-fno-sanitize-recover=undefined`, so the first finding aborts.

`enum` is the one UBSan check excluded, with its reason in both CMakeLists:
three controller suites cast an out-of-range value to an enum **on purpose**,
to prove the code rejects a value that arrived from outside, which is the same
deliberate behaviour `firmware/controller/test/.clang-tidy` already exempts
`EnumCastOutOfRange` for. Excluding a check that fires on a negative test is
not the same as recovering from every finding, which is why the two were
separated rather than solved with one flag.

A gate that prints its findings and goes green is worse than no gate, because
it looks like one. Any new gate in this project has to be shown to fail: the
sanitiser step was verified by reintroducing the signed subtraction that
`disagreeing` deliberately avoids, confirming that UBSan reports it, that the
step fails, and that the plain step passes.

**MC/DC rather than branch coverage, for the permit decision.** The permit is a
five-term conjunction. One test with everything true and one with everything
false gives 100 % line and branch coverage of it and proves close to nothing;
MC/DC requires each term to be shown to change the outcome on its own. That is
the level 61508-3 expects at SIL 3 and is above what 13849 PL d asks for, and
it is the one place in the project where a missed condition means heat that
should have stopped.

## 9. Obligations on a change

In addition to [`safety.md` §9.3](safety.md#93-obligations-on-anyone-changing-this-design),
which stands:

| | Obligation |
|---|---|
| A change to a safety rule needs a host test that fails before it and passes after (`SWR-TST-25`) | Existing |
| A new fault code needs an explicit clearability decision; it does not inherit one | Existing |
| No floating point may enter `firmware/supervisor` without an entry in section 5 | New |
| A new `.clang-tidy` exemption, or a new `NOLINT`, needs its reason in the file and a row in section 7 | New |
| The supervisor's firmware must stay small enough to read in one sitting. If it grows a scheduler, a parser or a configuration model, the independence claim starts to rot ([`safety-supervisor.md` §10](safety-supervisor.md)) | Existing |

## 10. What is not met yet

Stated plainly, because a coding standard that lists only what is already done
is a description rather than a standard.

| | Gap | What closing it takes |
|---|---|---|
| 1 | **MISRA C++:2023 is a target, not a claim.** No commercial checker has been run. | A licence for one checker (Cppcheck Premium, QAC, LDRA, Polyspace, Parasoft) pointed at `firmware/supervisor` only, which is four core files and 3.5 kB. Then the MISRA Compliance:2020 artefacts: a guideline enforcement plan, a re-categorisation plan, deviation records and a compliance summary. The deviation discipline of section 7 is already that in substance. |
| 2 | ~~The supervisor builds at C++20, which MISRA C++:2023 is not written against.~~ **Done 2026-10-09.** Both the target and the suites build at C++17, the suites pass, and the image is byte-identical to the one C++20 produced. | |
| 3 | **No tool qualification argument.** 61508-3 §7.4.4 puts `arm-none-eabi-g++`, `tools/sup-crc.py` and `tools/gen-stm32g031-header.py` in class T3: each transforms or generates what ends up in the image. The CRC stamper is the sharp one, because a silently wrong stamp is exactly what the firmware then trusts. | Version pinning, plus an independent read-back of the stamp rather than trusting the writer. |
| 4 | **Worst-case stack is not bounded at build time.** `-fstack-usage` is set and nothing consumes the `.su` files. The 256-word guard catches an overflow at run time, which is the second line of defence, not the first. | A script summing the `.su` files along the call graph. Cheap here specifically: the supervisor has no recursion and no function pointer. |
| 5 | ~~The supervisor's tests do not run under a sanitizer.~~ **Done.** `ENABLE_ASAN` on the supervisor's host build and a second `ctest` step in its CI job, mirroring the controller's option name and sanitiser pair. Finding it also fixed the controller's: both had been recovering from UBSan findings and reporting PASSED. | |
| 6 | **`tools/layercheck` does not exist**, though `safety.md` §9.3 cites it as enforcing the host-testability rule. | Write it, or stop citing it. |
| 7 | **`main.cpp` and `httpd.cpp` on the controller side are analysed by nothing**, tasklist C11, 102 findings. Unrelated to the supervisor and tracked there. | Compute the target-only unit set as "in the target build, not in the host build", then the ordinary passes. |

Items 2 and 4 are each an afternoon. Item 1 is a purchase. Item 3 is the one
that needs a decision rather than work.

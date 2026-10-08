<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# V-Model Development Process with StrictDoc

## 1. Purpose

This document defines the development process along the V-Model and how each
level is represented and traced in [StrictDoc](https://strictdoc.readthedocs.io).
Every artifact on the left side (specification) has a counterpart on the right
side (verification). Traceability is maintained in StrictDoc from user
requirement down to source code and back up to acceptance test.

## 2. Overview

```
User Requirements  ────────────────────────────────────────  Acceptance Tests
   System Requirements  ────────────────────────────────  System Tests
      Software Requirements  ───────────────────────  Integration Tests
         Software Architecture  ───────────────  Software Tests
            Unit Design  ───────────────────  Unit Tests
                       Implementation
```

- **Downward (left):** each level refines its parent level.
- **Horizontal:** each test level verifies the specification level opposite to it.
- **Upward (right):** test levels are executed bottom-up; a level starts only
  when the level below has passed.

## 3. Levels at a Glance

| # | Level                 | Document               | UID prefix | Refines (Parent) | Verified by       |
|---|-----------------------|------------------------|------------|------------------|-------------------|
| 1 | User Requirements     | `01_user_req.sdoc`     | `UR-`      | –                | Acceptance Tests  |
| 2 | System Requirements   | `02_system_req.sdoc`   | `SYS-`     | `UR-`            | System Tests      |
| 3 | Software Requirements | `03_software_req.sdoc` | `SWR-`     | `SYS-`           | Integration Tests |
| 4 | Software Architecture | `04_software_arch.sdoc`| `SWA-`     | `SWR-`           | Software Tests    |
| 5 | Unit Design           | `05_unit_design.sdoc`  | `SWD-`     | `SWA-`           | Unit Tests        |
| 6 | Implementation        | `src/`                 | –          | `SWD-`           | –                 |

| #  | Test level        | Document                    | UID prefix | Verifies |
|----|-------------------|-----------------------------|------------|----------|
| 7  | Unit Tests        | `07_unit_tests.sdoc`        | `UT-`      | `SWD-`   |
| 8  | Software Tests    | `08_software_tests.sdoc`    | `SWT-`     | `SWA-`   |
| 9  | Integration Tests | `09_integration_tests.sdoc` | `IT-`      | `SWR-`   |
| 10 | System Tests      | `10_system_tests.sdoc`      | `ST-`      | `SYS-`   |
| 11 | Acceptance Tests  | `11_acceptance_tests.sdoc`  | `AT-`      | `UR-`    |

## 4. Specification Phases (Left Side)

### 4.1 User Requirements (`UR-`)

- **Purpose:** capture what the user or customer needs, in their language.
- **Input:** customer specification, use cases, standards, stakeholder interviews.
- **Content:** needs, operating scenarios, constraints. No solution details.
- **Target markets:** the countries or regions in which the product will be
  placed on the market or operated. Each is stated as its own requirement, so
  that market-specific requirements can be traced to it.
- **Regulatory framework:** the regulations, directives and standards that
  apply as a consequence of the target markets and the intended use. Each is
  stated as its own requirement with:
  - identifier and edition/date (e.g. "ISO 13849-1:2023"),
  - type (regulation, directive, harmonised standard, customer standard),
  - the target markets it applies to,
  - scope of applicability (whole product or specific functions).
- **Exit criteria:** every requirement is uniquely identified, agreed with the
  stakeholder and has an acceptance criterion; target markets are complete and
  confirmed by the stakeholder; for every target market the applicable
  regulations, directives and standards are identified with their edition.

Recommended structure of `01_user_req.sdoc`:

| Section                         | UID prefix | Content                                   |
|---------------------------------|------------|-------------------------------------------|
| Target markets                  | `UR-MKT-`  | one item per country or region            |
| Regulations, directives, standards | `UR-REG-` | one item per document, linked to `UR-MKT-` |
| User needs                      | `UR-`      | functional and operational needs          |

Example:

```
[REQUIREMENT]
UID: UR-MKT-001
TITLE: Target market European Union
STATEMENT: >>>
The product shall be suitable for placing on the market in all member states
of the European Union.
<<<

[REQUIREMENT]
UID: UR-REG-003
TITLE: Machinery Regulation
STATEMENT: >>>
The product shall comply with Regulation (EU) 2023/1230.
<<<
RELATIONS:
- TYPE: Parent
  VALUE: UR-MKT-001
```

`UR-REG-` items are refined like any other user requirement: the system
requirements derived from a regulation or standard reference it as parent, so
compliance can be shown through the traceability matrix. A change of target
markets or a new edition of a standard triggers the impact analysis of rule 6
in section 8.

### 4.2 System Requirements (`SYS-`)

- **Purpose:** translate user needs into verifiable technical requirements of
  the whole system (hardware + software + mechanics).
- **Input:** user requirements.
- **Content:** functional and non-functional requirements, external interfaces,
  environmental and safety requirements.
- **Exit criteria:** every `UR-` is covered by at least one `SYS-`; every
  `SYS-` has a parent or a documented rationale; each is testable.

### 4.3 Software Requirements (`SWR-`)

- **Purpose:** define what the software must do, as allocated from the system.
- **Input:** system requirements, hardware/software interface.
- **Content:** software functions, timing, resource limits, interfaces to
  hardware, error handling.
- **Exit criteria:** every software-relevant `SYS-` is covered; no `SWR-`
  without parent.

### 4.4 Software Architecture (`SWA-`)

- **Purpose:** define the static and dynamic structure of the software.
- **Input:** software requirements.
- **Content:** components, their responsibilities, interfaces, data flow,
  scheduling, resource budgets.
- **Exit criteria:** every `SWR-` is allocated to at least one component;
  every interface is specified on both sides.

### 4.5 Unit Design (`SWD-`)

- **Purpose:** specify each unit in enough detail to implement and test it.
- **Input:** software architecture.
- **Content:** per unit: functions, parameters, value ranges, algorithms,
  state machines, error behaviour.
- **Exit criteria:** every `SWA-` component is broken down into units; every
  unit design element is testable in isolation.

## 5. Implementation

- **Purpose:** realise the unit design in source code.
- **Input:** unit design, coding guidelines.
- **Traceability:** source code references the `SWD-` UID it implements using
  a StrictDoc relation marker (see 7.3).
- **Exit criteria:** code compiles without warnings, static analysis is clean,
  review is done, every unit design element is linked to code.

## 6. Verification Phases (Right Side)

### 6.1 Unit Tests (`UT-`) – verify Unit Design

- **Scope:** a single unit in isolation; dependencies are stubbed or mocked.
- **Methods:** requirement-based tests, boundary values, equivalence classes,
  structural coverage measurement.
- **Exit criteria:** every `SWD-` has at least one passing `UT-`; coverage
  target reached.

### 6.2 Software Tests (`SWT-`) – verify Software Architecture

- **Scope:** units integrated into components and components into the complete
  software; typically on host or simulator.
- **Methods:** interface tests, data and control flow tests, resource usage.
- **Exit criteria:** every `SWA-` interface and component has a passing `SWT-`.

### 6.3 Integration Tests (`IT-`) – verify Software Requirements

- **Scope:** integrated software running on the target hardware.
- **Methods:** requirement-based tests, hardware/software interface tests,
  timing and fault injection tests.
- **Exit criteria:** every `SWR-` has at least one passing `IT-`.

### 6.4 System Tests (`ST-`) – verify System Requirements

- **Scope:** complete system in a representative environment (e.g. HIL).
- **Methods:** functional, performance, environmental, robustness tests.
- **Exit criteria:** every `SYS-` has at least one passing `ST-`.

### 6.5 Acceptance Tests (`AT-`) – validate User Requirements

- **Scope:** complete system in its intended use, with or by the customer.
- **Methods:** scenario-based tests against the acceptance criteria.
- **Exit criteria:** every `UR-` has at least one passing `AT-`; customer
  sign-off.

## 7. Realisation in StrictDoc

### 7.1 Repository Layout

```
project/
├── strictdoc.toml
├── docs/
│   ├── 01_user_req.sdoc
│   ├── 02_system_req.sdoc
│   ├── 03_software_req.sdoc
│   ├── 04_software_arch.sdoc
│   ├── 05_unit_design.sdoc
│   ├── 07_unit_tests.sdoc
│   ├── 08_software_tests.sdoc
│   ├── 09_integration_tests.sdoc
│   ├── 10_system_tests.sdoc
│   └── 11_acceptance_tests.sdoc
├── src/
└── tests/
```

### 7.2 Relations

Two relation types are sufficient:

| Relation             | From → To                      | Meaning                    |
|----------------------|--------------------------------|----------------------------|
| `Parent`             | lower spec level → upper level | "refines"                  |
| `Parent` / `Verifies`| test case → specification item | "verifies"                 |
| `File`               | spec or test item → file       | "implemented in / run by"  |

Specification item:

```
[DOCUMENT]
TITLE: Software Requirements
UID: SWR
VERSION: 0.1

[REQUIREMENT]
UID: SWR-012
STATUS: Approved
TITLE: Output shutdown on overcurrent
STATEMENT: >>>
The software shall switch off the output within 10 ms after an overcurrent
condition is detected.
<<<
RATIONALE: >>>
Protects the output stage.
<<<
RELATIONS:
- TYPE: Parent
  VALUE: SYS-007
```

Test case (the role `Verifies` must be declared in the document grammar):

```
[REQUIREMENT]
UID: IT-031
TITLE: Overcurrent shutdown time
STATEMENT: >>>
Inject an overcurrent on the output and measure the time until switch-off.
Pass: switch-off time <= 10 ms.
<<<
RELATIONS:
- TYPE: Parent
  VALUE: SWR-012
  ROLE: Verifies
- TYPE: File
  VALUE: tests/integration/test_overcurrent.py
```

### 7.3 Source Code Traceability

Enable the feature in `strictdoc.toml`:

```toml
[project]
title = "My Project"
features = [
  "REQUIREMENT_TO_SOURCE_TRACEABILITY",
]
```

Mark the implementing code with the unit design UID:

```c
/**
 * @relation(SWD-045, scope=function)
 */
void output_shutdown(void)
{
    ...
}
```

Unit test code is marked the same way with the `UT-` UID.

### 7.4 Generating and Checking

```
strictdoc export .      # generate HTML incl. traceability views
strictdoc server .      # edit and browse in the web UI
```

`strictdoc export` runs in CI. A broken relation (unknown UID) fails the
build. The traceability matrix and the deep-traceability view are used as
review evidence.

## 8. Process Rules

1. **Unique UIDs:** every item has a stable UID with the level prefix; UIDs
   are never reused.
2. **No orphans:** every item below `UR-` has at least one parent; every
   specification item has at least one child or a rationale why not.
3. **Full coverage:** every specification item is verified by at least one
   test case on the opposite level.
4. **Tests are specified early:** the test specification of a level is written
   when the opposite specification level is approved, not after implementation.
5. **Status workflow:** `Draft → In Review → Approved`. Only approved items
   may be refined or implemented.
6. **Change handling:** a change to an item triggers an impact analysis along
   its relations: children, implementing code and verifying tests are
   re-reviewed.
7. **Baselines:** documents and code are versioned together in Git; a release
   is a tag for which the StrictDoc export is archived together with the test
   results.

## 9. Phase Gates

| Gate                 | Condition                                                        |
|----------------------|------------------------------------------------------------------|
| Requirements freeze  | `UR-`, `SYS-`, `SWR-` approved; upward coverage complete         |
| Design freeze        | `SWA-`, `SWD-` approved; all `SWR-` allocated                    |
| Code complete        | all `SWD-` linked to source; static analysis and review passed   |
| Software release     | `UT-`, `SWT-`, `IT-` passed; coverage targets met                |
| System release       | `ST-` passed; open issues assessed                               |
| Acceptance           | `AT-` passed; customer sign-off                                  |

---

## 10. How this project realises the process

Added when the existing requirements and architecture documents were restructured
onto the levels above. This section records what was done, where it departs from
the sections above, and what is not done yet, so that the gap between the process
and its realisation is visible rather than assumed away.

### 10.1 Documents

| Level | Document | Prefix | State |
|---|---|---|---|
| 1 User requirements | [`01_user_req.sdoc`](01_user_req.sdoc) | `UR-`, `UR-MKT-`, `UR-REG-`, `UR-CON-` | written |
| 2 System requirements | [`02_system_req.sdoc`](02_system_req.sdoc) | `SYS-HW-`, `SYS-SAF-`, `SYS-ASM-` | written |
| 3 Software requirements | [`03_software_req.sdoc`](03_software_req.sdoc) | `SWR-` + area | written |
| 4 Software architecture | [`04_software_arch.sdoc`](04_software_arch.sdoc) | `SWA-` | written |
| 5 Unit design | `05_unit_design.sdoc` | `SWD-` | **not written** |
| 7 Unit tests | `07_unit_tests.sdoc` | `UT-` | **not written** |
| 8 Software tests | `08_software_tests.sdoc` | `SWT-` | **not written** |
| 9 Integration tests | `09_integration_tests.sdoc` | `IT-` | **not written** |
| 10 System tests | `10_system_tests.sdoc` | `ST-` | **not written** |
| 11 Acceptance tests | `11_acceptance_tests.sdoc` | `AT-` | **not written** |

Two concept documents sit alongside rather than inside the levels:
[`safety.sdoc`](safety.sdoc) and [`security.sdoc`](security.sdoc). They are
analyses, not specification levels: hazards, safety goals, threats and residual
risks, each relating down into the requirements that discharge them. Keeping them
beside the V rather than on it is deliberate; a hazard is not a refinement of a
user requirement.

The 467 automated tests that already exist are not yet specified as `UT-` and
`IT-` items. Until they are, rule 3 of section 8 is **not met**: coverage runs
from requirement to test by naming convention and by `@relation` markers, not by
a test specification. That is the largest remaining gap and it is specification
work rather than test work, because the tests themselves are written and passing.

### 10.2 Deviations from the sections above

**Sub-prefixes within a level.** Section 3 shows flat numbering (`SWR-012`).
This project keeps the functional area in the identifier: `SWR-CTL-07`,
`SYS-HW-13`, `SWR-SAF-25`. With 169 software requirements, `SWR-CTL-07` says
where it belongs and `SWR-112` does not, and the areas were already the
document's structure. Section 4.1 already uses sub-prefixes for `UR-MKT-` and
`UR-REG-`, so the pattern is the guide's own.

**Numbering is not compacted.** `SWR-SAF-` runs 04 to 31 with 01, 02, 03 and 24
missing, because those four are system level and are `SYS-SAF-01/02/03/24`.
`SRR-` in the security concept has no 10. The gaps are kept on purpose: these
identifiers are cited from the firmware, from the tests, from the two concept
documents and from commit messages, and closing a numbering gap would break every
one of those citations to buy nothing.

**Section titles keep their old numbers.** `02_system_req.sdoc` contains sections
titled "6. Hardware interface requirements" and "9. Assumptions" even though it
has three sections, because a great deal of prose across the repository refers to
"§6" and "§9" by those numbers. Renumbering is a separate tidy.

**Statuses are not yet used as section 8 rule 5 describes.** The workflow
`Draft → In Review → Approved` is not in force; items carry `Active`,
`Withdrawn`, `Superseded`, `Reversed` or `Resolved`, which record what happened
to a requirement rather than how far it has got through review. The phase gates
of section 9 therefore cannot be evaluated yet.

### 10.3 Target markets

The target market is the **European Union**, with the EEA EFTA states and
Switzerland as secondary ([`UR-MKT-001`](01_user_req.sdoc),
[`UR-MKT-002`](01_user_req.sdoc)). Fifteen `UR-REG-` items record the legislation
and standards that follow, each with its instrument number, edition, type and the
route it applies to.

The project has **two routes to market** and they carry different obligations:
finished units placed on the market by Bitcrush Testing, and a published design
built by somebody else who then becomes the manufacturer of the finished machine.
`UR-REG-` items carry a `ROUTE` field for this reason.

One finding from writing that section deserves to be read by anyone planning a
release: **the Cyber Resilience Act requires security updates to be available,
and this device has no field update path at all.** That is `UR-REG-004` against
`SRR-11`, and it means `OQ-08` is now a regulatory deadline rather than an
engineering preference. Two further questions, `OQ-R1` on whether the controller
is a machinery safety component and `OQ-R2` on the radio conformity route, decide
how expensive the sold-units route is. None of section 2 is a conformity
assessment, and `SYS-SAF-24` already requires the documentation to say what this
device is not.

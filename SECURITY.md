<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Security policy

> **A security report about this project may be a safety report.** Safe Kiln
> Controller switches several kilowatts into a kiln. If you have found something
> that lets an attacker start or prolong heating, say so in the first line and we
> will treat it as the safety matter it is, not as a web bug.

## Reporting a vulnerability

**Email `security@bitcrushtesting.com`.** That address is the single point of
contact required by Article 13(17) of the Cyber Resilience Act, and it is read by
a person rather than a ticket queue.

Please do not open a public issue for something exploitable. Public issues are
the right place for everything else, including hardening suggestions and the
"this looks wrong but I cannot exploit it" category.

What helps, in rough order of usefulness:

- the firmware version (`GET /api/info`, or the local display's info screen) and
  the board revision from the label;
- what you had access to: the LAN, the board, a debugger, the update service;
- what the device did, and what you expected;
- whether heating was involved, and whether the independent cutout of
  `SYS-HW-13` was fitted on the unit you tested.

You do not need a proof of concept. A clear description of the mechanism is
worth more than an exploit we have to reverse.

### What happens next

| When | What |
|---|---|
| Within **3 working days** | We acknowledge the report and say whether we can reproduce it |
| Within **14 days** | An assessment: severity, whether it is exploitable in the documented deployment, and a target date |
| Target **90 days** | A fix released through the update path, with an advisory |

If a fix will take longer than 90 days, we will say so and why rather than let
the date pass quietly. If you intend to publish, tell us when; we would rather
coordinate than be surprised, and we will not ask you to delay indefinitely.

Credit is given in the advisory unless you ask us not to. There is no bug
bounty: this is a small project and we would rather be honest about that than
imply a payment that is not coming.

### Our own reporting obligations

Separately from this policy, and not at the reporter's discretion: an **actively
exploited** vulnerability or a severe incident is reported to ENISA and the
relevant CSIRT within 24 hours as an early warning, with a notification within 72
hours and a final report within 14 days, under Article 14 of the Cyber Resilience
Act. Those obligations apply from **11 September 2026**. Users are informed
without undue delay in the same cases.

## Supported versions

| | |
|---|---|
| Security fixes | The latest release on the `stable` channel |
| Support period | **To be declared** before the first unit is placed on the market, and at least five years (`SWR-UPD-16`, Article 13(8) CRA) |

Security fixes are released **separately** from functional changes wherever that
is feasible, marked as security releases in the update manifest, and accompanied
by an advisory (`SWR-UPD-16`).

## How a fix reaches a device

The device checks `update.bitcrushtesting.com` once a day, verifies a signed
manifest against a key compiled into its own firmware, and announces an
available release **on the local display**. It installs only when somebody
confirms there; it never installs by itself, and nothing on the network can
initiate, authorise or cancel an install.

That last property is deliberate and is the reason an update may sit unapplied:
rebooting a kiln controller into untried firmware unattended, with a load inside,
is not an acceptable trade for promptness
([`docs/security.md` §6.2](docs/security.md), `SWR-UPD-12`). If you own one of
these, installing an offered update is **your** step, and
[`docs/security.md` §10.1](docs/security.md) is the rest of what the design asks
of you.

A local update over USB is the recovery path when the service is unreachable or
an image will not confirm itself (`SWR-UPD-14`).

## Scope

### In scope

- The firmware in this repository, both microcontrollers.
- The REST API and the browser assets.
- The update path, the manifest format and
  [`tools/update-manifest.py`](tools/update-manifest.py).
- The provisioning tools, [`tools/secure-boot.py`](tools/secure-boot.py) and
  [`tools/prod-data.py`](tools/prod-data.py).
- The safety supervisor's trip logic, where a defect in it is a safety matter
  whether or not anybody can trigger it remotely.

### Known and documented, so not a finding on its own

Each of these is written down, argued, and carried as a named residual risk in
[`docs/security.sdoc`](docs/security.sdoc). A report that restates one of them
adds nothing; a report that shows one is **worse than documented**, or combines
with something else into an attack, is very welcome.

| | |
|---|---|
| No transport encryption; the device is for a trusted LAN only | `SRR-03`, `SEC-11`, `SWR-NFR-20` |
| The dashboard is readable by anyone on the LAN. The interface is read-only by design, so there is nothing to authenticate | `SRR-07`, `SEC-00` |
| Secrets are plaintext in flash; anyone with the board reads the WiFi password | `SRR-05`, `OQ-S3` |
| Physical access is treated as equivalent to knowing the WiFi password | `SRR-05`, `ADV-3` |
| The supervisor's SWD port is deliberately left open | [`docs/security.md` §6.1](docs/security.md) |
| mDNS makes the device discoverable, on purpose | `TH-13` |
| Nearly every network control is a **specification**: there is no HTTP transport on the target yet | `SRR-01` |

That last row matters for anyone testing: a vulnerability in code that has never
been built for a device is a real finding about the design, but please say which
you mean.

### Out of scope

- The user's network, router, browser or WiFi infrastructure, except where this
  project depends on them and says so.
- Denial of service that requires being on the LAN already: the control loop and
  the safety supervisor are isolated from the network by construction
  (`SEC-08`), so the worst case is a lost web interface, not a lost kiln. If you
  can show otherwise, that is in scope and important.
- Social engineering, and anything requiring the operator to confirm a firmware
  install they were not expecting. We would still like to hear about the latter
  if the prompt is misleading.

## For the curious

The threat model is not a paragraph in this file. It is
[`docs/security.md`](docs/security.md) with its nodes in
[`docs/security.sdoc`](docs/security.sdoc): seven assets, six adversaries,
fifteen threats, fifteen security goals and thirteen residual risks, each with
who carries it. §7 is the part worth reading first, on why a security compromise
of this device is a safety event.

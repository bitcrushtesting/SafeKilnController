<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Safe Kiln Controller, Security Concept

| | |
|---|---|
| **Document** | Security Concept |
| **Project** | Safe Kiln Controller, PID kiln controller |
| **Version** | 0.1 (draft) |
| **Date** | 2026-10-05 |
| **Status** | For review |
| **Derives from** | [`03_software_req.sdoc`](03_software_req.sdoc) v0.1, [`architecture.md`](architecture.md) v0.1, [`safety.md`](safety.md) and [`safety.sdoc`](safety.sdoc) v0.1 |
| **License** | GPL-3.0-or-later |

---

> **Safe Kiln Controller is designed for a trusted local network and nothing else**
> ([`SWR-NFR-20`](03_software_req.sdoc),
> [`SYS-ASM-05`](02_system_req.sdoc)). It must not be exposed to the
> internet, port-forwarded, or placed on a network it shares with untrusted
> devices. There is **no transport encryption**: everything, the web password
> included, crosses the LAN in cleartext.
>
> This matters more here than on an ordinary appliance, because **a security
> compromise of this device is a safety event**. An attacker who can issue
> commands can start a multi-kilowatt heater. [§7](#7-security-as-a-safety-concern)
> is the part of this document that distinguishes it from a generic threat
> model, and it should be read before the controls.

## 1. Purpose and scope

[`safety.md`](safety.md) analyses what happens when the equipment *fails*. This
document analyses what happens when somebody *attacks* it, and the two meet at
one hazard: [`HZ-12`](safety.sdoc), a remote command putting the
kiln into a dangerous state.

It covers the firmware's network-facing surfaces, its stored secrets, its
update path, and physical access to the board. It does not cover the security of
the user's own network, their browser, or their WiFi infrastructure, except to
say where Safe Kiln Controller depends on them.

The split between this document and [`security.sdoc`](security.sdoc) follows the
one between [`safety.md`](safety.md) and [`safety.sdoc`](safety.sdoc). The
assets, adversaries, threats, security goals, residual risks and open questions
are nodes in `security.sdoc`, because they have identity and a chain between
them that is now checked when the document is built. The reasoning is here: the
scope, the trust boundaries, section 7 on why a security compromise is a safety
event, what the implementation already gets right, the obligations and the
verification status.

Identifiers introduced by this concept extend the scheme of
[requirements §1.4](03_software_req.sdoc) and of
[`safety.md` §1](safety.md#1-purpose-and-scope), and are defined as nodes in
[`security.sdoc`](security.sdoc):

| Prefix | Meaning |
|---|---|
| `A` | Asset |
| `ADV` | Adversary |
| `TH` | Threat |
| `SEC` | Security goal |
| `SRR` | Security residual risk |
| `OQ-S` | Open question, security |

The trust boundaries `B-1` to `B-6` are **not** nodes, and that is deliberate:
they are the legend of the diagram in [§3](#3-attack-surface-and-trust-boundaries)
and carry no chain through them, so splitting them from the picture would help
nobody.

Note for anyone reading `03_software_req.sdoc` alongside this: its section anchors
used to be `SEC-n`, which collided with the security goals here once both became
StrictDoc documents. They are `SECT-n` now. `SEC` means security goal.

**Status honesty.** Every control below is marked with what is actually true
today, and the marks are not decoration, most of this is **not built yet**:

| Mark | Meaning |
|---|---|
| **[built]** | Implemented and covered by an automated test |
| **[partial]** | Implemented in the host-testable core, but the piece that would enforce it on a device does not exist |
| **[spec]** | Required or designed, no implementation at all |
| **[absent]** | Not required, not designed, not present, a gap, named as one |

At the time of writing there is **no HTTP transport on the target**. The REST
API ([`kiln_web`](../firmware/components/kiln_web)) is complete and host-tested,
but the `esp_http_server` adapter that would terminate TCP, parse headers, check
a password and apply rate limiting **is not started**
([README status table](../README.md#status)). Nearly every network control in
this document therefore reads `[spec]`, and that is the single most important
fact in it.

## 2. Assets

The seven assets live in [`security.sdoc`](security.sdoc), not here, each with
what it is and why it matters. `A-1`, control of the heater, is the asset;
everything else is a means to it.

## 3. Attack surface and trust boundaries

```mermaid
flowchart TB
    subgraph UNTRUSTED["Untrusted"]
        INET["The internet<br/>(must never reach the device)"]
    end
    subgraph LAN["Trusted by assumption, SYS-ASM-05"]
        BROWSER["Operator's browser"]
        OTHER["Other LAN devices<br/>(IoT, guests, malware)"]
    end
    subgraph DEVICE["Safe Kiln Controller"]
        HTTP["HTTP server<br/>**not implemented**"]
        API["REST API, kiln_web<br/>built, host-tested"]
        APP["kiln_app, commands"]
        SAFE["safety supervisor"]
        NVS["NVS: WiFi + web password<br/>**plaintext**"]
        OTA["OTA update path<br/>**not implemented**"]
    end
    subgraph PHYS["Physical access"]
        UART["UART console"]
        JTAG["JTAG / SWD"]
        FLASH["SPI flash readout"]
    end
    INET -. "must not be routed" .-x HTTP
    BROWSER --> HTTP
    OTHER --> HTTP
    HTTP --> API --> APP --> SAFE
    APP --> NVS
    HTTP --> OTA --> NVS
    UART --> APP
    JTAG --> NVS
    FLASH --> NVS
```

The trust boundaries, named:

| # | Boundary | Crossed by | Enforced by |
|---|---|---|---|
| **B-1** | Internet → LAN | Nothing, by assumption | The user's router. Safe Kiln Controller has no control here and no defence if it is wrong. |
| **B-2** | LAN → device | Every HTTP request | The HTTP transport, **which does not exist**. |
| **B-3** | Unauthenticated → authenticated | State-changing requests | [`kiln_api_needs_auth`](../firmware/components/kiln_web/include/kiln_web/api.h) decides *which*; the transport would decide *whether*. |
| **B-4** | Request data → parser | Bodies, query strings, JSON | Bounded buffers in `kiln_web` **[built]**. |
| **B-5** | API → control | Commands | `kiln_app`; the safety supervisor holds heat authority regardless ([`SWA-04`](architecture.md#3-key-decisions)). |
| **B-6** | Physical → device | UART, JTAG, flash | **Nothing.** No secure boot, no flash encryption. |

## 4. Adversaries

The six adversaries live in [`security.sdoc`](security.sdoc), each with its
capability and an honest answer to whether it is realistic. `ADV-1`, another
device on the same LAN, is the normal case and the one `SYS-ASM-05` quietly assumes
away.

## 5. Threats

The thirteen threats live in [`security.sdoc`](security.sdoc), rated by
consequence rather than likelihood. Each names the asset it is aimed at, the
adversary who can mount it, and, where it gets there, the **safety hazard** it
reaches: that last relation is the claim this document exists to make, and it is
now a link to a node in [`safety.sdoc`](safety.sdoc) rather than a sentence.

Four of them, `TH-01` to `TH-04`, are `Resolved`, closed outright by the
read-only decision of 2026-10-06. They are kept rather than deleted, because why
a threat is closed is the most useful thing about it.

## 6. Security goals and the controls behind them

The twelve security goals live in [`security.sdoc`](security.sdoc), each with the
threats it addresses, the requirements that realise it, and an
`IMPLEMENTATION` field carrying what used to be a `[built]` / `[partial]` /
`[spec]` / `[absent]` mark in this table.

That field is the one to read first. At the time of writing it stands at **6
built, 1 partial, 2 spec, 2 absent, 1 not needed**, and the reason is that there
is still **no HTTP transport on the target**: the REST API is complete and
host-tested, but the adapter that would terminate TCP, parse headers and apply
rate limiting is not started. Nearly every network control is therefore a
specification, and that remains the single most important fact in this document.

`SEC-01` and `SEC-04` are `Withdrawn`, dissolved on 2026-10-06 along with the
password itself.

## 7. Security as a safety concern

This is the part that makes a kiln controller different from a thermostat.

[`safety.md` §5](safety.md#5-the-protection-layers) layers the protections by how
little each depends on software being correct. An attacker does not defeat those
layers by breaking them, **they defeat them by using them as designed**, from
the wrong side of the trust boundary:

| Protection layer | What an attacker does to it |
|---|---|
| **L1 control**: setpoint clamped to the configured maximum | Raise the configured maximum (TH-02). Still bounded by `SWR-SAF-23`'s compile-time 1350 °C ceiling, which no configuration and therefore no attacker can raise. |
| **L2 safety supervisor**: 18 detection rules | Widen the thresholds (TH-02). `SWR-SAF-22` bounds every range and forbids disabling a detection outright, so this degrades detection rather than removing it, a bound that was written for a careless operator and happens to hold against a hostile one. |
| **L3 charge pump and contactor** | **Nothing.** It is hardware, and it does not care who asked. An attacker commanding heat gets heat, but a hung or crashed MCU still drops the contactor. |
| **L4 watchdogs, brownout** | Nothing. |
| **L5 independent hardware cutout** | **Nothing, and this is the point.** It has its own sensor and its own contacts, and it is reachable from no network. Against TH-01 to TH-05 it is the only layer that holds unconditionally. |
| **L6 operator attendance** | Everything, if the operator is not there. `SYS-ASM-06` requires attendance during firing; an attack that starts a firing *when nobody expects one* removes L6 by surprise rather than by negligence. |

Three consequences worth stating plainly:

1. **`SYS-HW-13`'s independent cutout is a security control, not only a safety one.**
   It is the one protection in the entire design that no network attacker can
   reach, degrade, or reason about. Every argument for it in `safety.md` is
   strengthened, not duplicated, here.

2. **The bounded ranges of `SWR-SAF-22` and the ceiling of `SWR-SAF-23` do real security
   work.** They were written so an operator could not disable a detection; the
   effect is that TH-02 degrades the safety envelope within known limits instead
   of removing it. A future change that relaxes a bound "because the operator
   knows what they are doing" would also be widening an attack.

3. **`SWR-SAF-18` blunts TH-03.** A latched fault cannot be cleared while its
   triggering condition is still observably true, and the check reuses the
   detector's own rule function. An attacker can clear a *stale* fault; they
   cannot clear a *live* one and then heat.

## 8. What the implementation already gets right

Recorded because a security document that lists only gaps misrepresents the
design, and because each of these is a property worth not losing.

- **Auth by method, not by list.** `kiln_api_needs_auth` protects every non-`GET`
  route. A new state-changing endpoint is protected the moment it exists; the
  failure mode of the usual approach, an enumerated allow-list somebody forgets
  to update, cannot occur.
- **Secrets cannot be read back.** `SWR-CFG-07` is implemented and tested, and the
  config endpoint is generated from the same table that declares the flag, so a
  new secret field is redacted by construction rather than by a second edit.
- **No allocation proportional to input, anywhere.** The whole firmware contains
  zero `malloc`/`free`. An attacker cannot exhaust a heap that does not exist.
- **The dev harness binds to loopback only.** `host/webhost` listens on
  `INADDR_LOOPBACK`, not `INADDR_ANY`, and sets `authenticated = true`
  unconditionally, which is safe *precisely because* it is unreachable from the
  network. Worth never "fixing" into `INADDR_ANY` for convenience.
- **Fault injection is compile-gated.** The UART console that can inject relay
  and sensor faults exists only under `CONFIG_KILN_PLANT_SIM`, not in a
  production image.
- **Control is isolated from the network by construction** (SEC-08), which means
  the worst realistic DoS is a lost web interface, not a lost kiln.

## 9. Residual risk

The ten security residual risks live in [`security.sdoc`](security.sdoc), each
with why it stands, **who carries it**, and what mitigation exists today. Two
are worth naming here: `SRR-01`, that every network control is a specification
and **nothing in this concept may be cited as evidence of a deployed control**;
and `SRR-11`, that removing OTA left the device with no way to receive a
security fix without a physical visit, which **blocks release**.

There is no `SRR-10`. The numbering has a gap, and keeping it costs less than
renumbering identifiers other documents cite.

## 10. Obligations

### 10.1 On the owner and installer

| | Requirement |
|---|---|
| Do not expose the device to the internet; do not port-forward to it | `SWR-NFR-20`, SRR-03 |
| Prefer an isolated network segment or VLAN over a flat home LAN | SRR-09 |
| Set a web password; it is optional (`SWR-WEB-23`) and the device ships without one | SEC-01 |
| Set an AP passphrase before relying on the provisioning fallback | SRR-08 |
| Treat physical access to the controller as equivalent to knowing the WiFi password | SRR-05 |
| **Erase flash before disposing of or selling a device** | SRR-05, ADV-4 |
| Fit the independent hardware over-temperature cutout; it is the only protection no attacker can reach | `SYS-HW-13`, [§7](#7-security-as-a-safety-concern) |

### 10.2 On anyone implementing the HTTP transport

This is the component that turns most of this document from specification into
fact. These are not suggestions.

| | Constraint |
|---|---|
| Resolve [SRR-02](#9-residual-risk) **first**: store a salted hash, never the password. Decide it before writing the comparison, not during. | `SWR-NFR-19` |
| Constant-time comparison; increasing delay after repeated failures. | `SWR-NFR-19`, SEC-04 |
| Validate `Origin`/`Host` on every state-changing request. The cost is trivial and SRR-06 is otherwise permanent. | SRR-06 |
| Set `req.authenticated` **only** after a successful check. It is an input the API trusts absolutely. | SEC-01 |
| Never add a back channel for the UI. `SWA-16` is a security property, not a tidiness preference. | `SWR-WEB-19`, `SWA-16` |
| Enforce the declared body maximum *before* parsing, not during. | `SWR-NFR-19`, SEC-03 |
| Keep the server on core 0. Heat authority stays with the safety supervisor. | `SWA-15`, `SWA-04`, SEC-08 |
| Do not bind the dev harness to anything but loopback. | [§8](#8-what-the-implementation-already-gets-right) |

### 10.3 On the project

| | Constraint |
|---|---|
| A new state-changing route must not bypass `kiln_api_needs_auth`'s method rule. | SEC-01 |
| A new configuration field holding a credential must carry `KILN_CFG_F_SECRET`. | `SWR-CFG-07`, SEC-02 |
| No dependency may be fetched at build time; assets stay vendored. | `UR-CON-04`, ADV-6 |
| No telemetry, no analytics, no outbound connection, ever. | `SWR-NFR-21`, SEC-06 |
| Relaxing a bound in `SWR-SAF-22` or the ceiling in `SWR-SAF-23` widens an attack surface, not just a safety envelope. | [§7](#7-security-as-a-safety-concern) |

## 11. Verification

| Claim | How it is verified | Status |
|---|---|---|
| Secrets are never serialised outward | Host unit test over every `SECRET`-flagged field, plus the API suite | **Verified** |
| Parsers survive hostile input | `test_fuzz_logrec`, the JSON and API suites, all under ASan/UBSan in CI | **Verified** for the codec and API; the HTTP layer does not exist to fuzz |
| No allocation proportional to input | Structural, zero `malloc`/`free` in the firmware; `tools/tidy.sh` enforces the check set | **Verified** |
| Control is unaffected by web load | `SWR-NFR-02` timing tests | **Not performed**: needs the transport and on-target instrumentation |
| Authentication is constant-time and rate-limited |, | **Not performed**: nothing to test |
| OTA rejects an invalid or unauthenticated image |, | **Not performed**: nothing to test |
| CSRF defences |, | **None designed** |

The pattern is the same one `safety.md` §10 reports: what exists in the
host-testable core is tested well, and what needs a device is untested because
it is unbuilt.

## 12. Open questions

The five open questions live in [`security.sdoc`](security.sdoc), each related to
the residual risk it would settle, so a question cannot drift away from the risk
that motivated it. `OQ-S1` and `OQ-S5` are `Resolved`, answered by the read-only
decision of 2026-10-06.

The traceability tables that used to sit below them are gone. They were a threat
to security-goal to requirement grid and a goal to residual-risk grid, both
maintained by hand, and both had fallen out of step with the body: they still
routed threats through `SEC-01` and `SEC-04` after those were dissolved, and
carried no row for `SEC-00`, the strongest control in the document. Those
mappings are relations in [`security.sdoc`](security.sdoc) now, generated in both
directions from the relations themselves, and a goal citing a requirement that
does not exist is an error in the `requirements` CI job rather than a stale row.

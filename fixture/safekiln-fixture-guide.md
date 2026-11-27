<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

## The guide plate

`36` holes at **1.78 mm**, unplated, with no copper anywhere on the board. It is a
drilled sheet: order it in the same panel as the probe plate and it costs almost nothing.

Its only job is to hold each 0985-0-15-20-71-14-11-0 pin perpendicular near its tip. A pin leaning one
degree over 10 mm of exposed length has moved 0.17 mm where it lands, which is most of the
placement budget on a small pad -- and the symptom is an intermittent channel, not something
that looks mechanical.

| | |
|---|---|
| Hole | 1.78 mm |
| Pin | ? mm |
| Clearance | 0.08 mm |
| Plate | 1.6 mm |
| Travel to leave above it | at least 2.4 mm |

**The standoffs between the two plates set the exposed length.** The pin has to stand at least
its own travel above this plate, plus however far the DUT's standoffs hold the board off, or it
bottoms out before the contact is made. 1.4 mm of travel is the figure for this
probe.

**The fit is a sliding one, deliberately.** The pin travels its whole stroke through the hole,
so the clearance is 0.08 mm rather than a press fit. Drill it tighter and the
pin binds, which reads on the bench as a dead channel.

**Nobody has checked these dimensions against a supplier drawing.** The hole is the pin diameter plus a clearance, and if the pin diameter is wrong the plate either binds every probe or guides none of them. Measure one pin with a micrometer before ordering: it is a minute of work against a board that cannot be salvaged.

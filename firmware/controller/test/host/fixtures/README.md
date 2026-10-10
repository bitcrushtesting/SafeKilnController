<!-- SPDX-FileCopyrightText: 2026 Bitcrush Testing
     SPDX-License-Identifier: GPL-3.0-or-later -->

# Wire-format fixtures

Binary files that pin a persisted format, checked in so that two
implementations can be compared against one artefact rather than against each
other's mood.

## `logring.bin`

Two 4 kB log sectors, produced by `kiln_logrec_encode` and
`kiln_logrec_encode_hdr` (`SWA-18`). Three records, chosen for the cases a
decoder gets wrong: a negative temperature, the segment sentinel, every flag
set at once, a non-sample event, and each field at the edge of its scale. The
sectors are deliberately **out of address order** — sector 0 holds the newer
data — so that anything reading the ring by address rather than by sequence
number interleaves two firings and is caught.

Two things read it, and that is the point:

- `test_logrec`, which re-encodes the same samples and compares bytes. The
  firmware cannot change the format without this failing.
- [`tools/logdump.py`](../../../../../tools/logdump.py), whose decoder is
  written from the format's documentation rather than from `kiln_core`. A log
  is the only evidence of what a kiln did before it failed, and "did the
  firmware record this correctly" cannot be answered by the firmware's own
  decoder: a codec that encodes and decodes with the same wrong idea round
  trips perfectly and proves nothing.

### When it fails

Ask which side moved.

- **The format changed deliberately.** Then `KILN_LOG_FORMAT_VERSION` in the
  sector header changes with it, the decoder learns the new version, and this
  file is regenerated. Logs already on devices stay readable because the
  version says which layout they are.
- **It did not.** Then the encoder has a bug that would have made every log on
  every device unreadable by the tooling, and the fixture has just caught it
  before a release did not.

Regenerating it is deliberate work rather than a command to run on a whim,
which is why there is no script for it here: write a short program against
`kiln_logrec_encode`, produce the same three records, and check the diff is
the change you meant.

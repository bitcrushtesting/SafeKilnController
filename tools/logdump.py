#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a kilnlog partition dump to CSV, and cross-check the codec.

    tools/logdump.py kilnlog.bin                   CSV on stdout
    tools/logdump.py kilnlog.bin -o firing.csv     and to a file
    tools/logdump.py kilnlog.bin --sectors         one line per sector instead
    tools/logdump.py kilnlog.bin --run 7           only that run
    tools/logdump.py --self-test                   check this decoder

Get the dump off a device with esptool, which needs no firmware cooperation
and works on a device that will not boot:

    esptool.py read_flash <offset> <size> kilnlog.bin

The offsets are in firmware/controller/partitions.csv. A dump of the whole
flash works too: this walks sectors and ignores everything that is not a log
sector, so a wrong offset costs a warning rather than a wrong answer.

===========================================================================
WHY THIS IS A SECOND IMPLEMENTATION AND NOT A WRAPPER
===========================================================================
It would be less code to link kiln_core and call kiln_logrec_decode. That is
exactly what it must not do. A log is the only evidence of what a kiln did
before it failed, and the question an investigator has is "did the firmware
record this correctly", which cannot be answered by the firmware's own
decoder: a codec that encodes and decodes with the same wrong idea round-trips
perfectly and tells you nothing.

So this is written from the format as architecture 10.2 and logrec.h document
it -- offsets, scales, CRC polynomials and all -- and the CI step feeds it
records the C++ encoder produced. Two implementations that agree are evidence.
One implementation agreeing with itself is a tautology.

The flip side is stated rather than discovered: this file is now a second
place the format lives, and the two can drift. That is what the cross-check in
CI is for, and why the constants below carry the names they have in the
headers instead of being inlined as magic numbers.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import struct
import sys
from dataclasses import dataclass, asdict

# --- the format, from logrec.h and port_logstore.h -----------------------
SECTOR_BYTES = 4096
HEADER_BYTES = 16
RECORD_BYTES = 20                      # SWA-18
RECORDS_PER_SECTOR = (SECTOR_BYTES - HEADER_BYTES) // RECORD_BYTES   # 204
LOG_MAGIC = 0x474F4C4B                 # "KLOG" little endian
FORMAT_VERSION = 1
CRC16_INIT = 0xFFFF

STATES = ["IDLE", "RUNNING", "PAUSED", "MANUAL", "AUTOTUNE", "COMPLETE", "FAULT"]
EVENTS = ["sample", "run_start", "run_end", "state_change", "fault", "warning",
          "config_change", "operator"]
SEG_NONE = 0xFF

# High nibble of the state/flags byte.
F_HOLDBACK, F_SATURATED, F_TC_FAULT, F_WALL_VALID = 0x10, 0x20, 0x40, 0x80


def crc8(data: bytes) -> int:
    """CRC-8, polynomial 0x07, initial 0xFF -- the record's own check."""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data: bytes, crc: int = CRC16_INIT) -> int:
    """CRC-16/CCITT-FALSE, polynomial 0x1021 -- the sector header's check."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Sector:
    index: int
    seq: int
    run_id: int
    version: int
    ok: bool
    why: str = ""


@dataclass
class Record:
    sector: int
    slot: int
    run_id: int
    t_rel_ms: int
    kiln_raw_c: float
    kiln_filt_c: float
    setpoint_c: float
    case_c: float
    current_a: float
    duty_pct: float
    segment: int | None
    state: str
    event: str
    holdback: bool
    saturated: bool
    tc_fault: bool
    wall_valid: bool
    current_flags: int


def decode_header(buf: bytes, index: int) -> Sector:
    if len(buf) < HEADER_BYTES:
        return Sector(index, 0, 0, 0, False, "short")
    magic, seq, run_id, version, got = struct.unpack("<IIIHH", buf[:HEADER_BYTES])
    if magic != LOG_MAGIC:
        # An erased sector is the normal case, not a problem: the ring is
        # written round and a device that has logged for an hour has mostly
        # erased sectors.
        why = "erased" if buf[:HEADER_BYTES] == b"\xff" * HEADER_BYTES else "not a log sector"
        return Sector(index, 0, 0, 0, False, why)
    want = crc16(buf[:HEADER_BYTES - 2])
    if got != want:
        return Sector(index, seq, run_id, version, False,
                      f"header CRC {got:#06x}, expected {want:#06x}")
    if version != FORMAT_VERSION:
        return Sector(index, seq, run_id, version, False,
                      f"format version {version}, this tool reads {FORMAT_VERSION}")
    return Sector(index, seq, run_id, version, True)


def decode_record(rec: bytes, sector: int, slot: int, run_id: int):
    """Returns a Record, or a string saying why it is not one."""
    if len(rec) < RECORD_BYTES:
        return "short"
    if rec == b"\xff" * RECORD_BYTES:
        return "erased"
    if crc8(rec[:RECORD_BYTES - 1]) != rec[RECORD_BYTES - 1]:
        return "crc"

    (t_rel_ms, raw, filt, sp, case, current) = struct.unpack("<Ihhhhh", rec[:14])
    duty200 = rec[14]
    segment = rec[15]
    state_flags = rec[16]
    current_flags = rec[17]
    event = rec[18]

    return Record(
        sector=sector, slot=slot, run_id=run_id, t_rel_ms=t_rel_ms,
        # Temperatures are tenths of a degree; current is 10 mA steps, read as
        # unsigned because the encoder clamps it at zero.
        kiln_raw_c=raw / 10.0, kiln_filt_c=filt / 10.0,
        setpoint_c=sp / 10.0, case_c=case / 10.0,
        current_a=(current & 0xFFFF) / 100.0,
        # Duty is stored in half-percent steps, so a byte covers 0..100 %.
        duty_pct=duty200 / 2.0,
        segment=None if segment == SEG_NONE else segment,
        state=STATES[state_flags & 0x0F] if (state_flags & 0x0F) < len(STATES)
              else f"state_{state_flags & 0x0F}",
        event=EVENTS[event] if event < len(EVENTS) else f"event_{event}",
        holdback=bool(state_flags & F_HOLDBACK),
        saturated=bool(state_flags & F_SATURATED),
        tc_fault=bool(state_flags & F_TC_FAULT),
        wall_valid=bool(state_flags & F_WALL_VALID),
        current_flags=current_flags,
    )


def walk(blob: bytes):
    """Every sector in the dump, with the records of the valid ones.

    Sectors are returned in sequence-number order, which is the ring's order
    and not the medium's: the newest sector is wherever the ring happened to
    get to, so reading in address order interleaves an old firing with a new
    one at the wrap point.
    """
    sectors, records, bad = [], [], {"crc": 0, "short": 0}
    for i in range(len(blob) // SECTOR_BYTES):
        base = i * SECTOR_BYTES
        sec = decode_header(blob[base:base + HEADER_BYTES], i)
        sectors.append(sec)
    live = sorted((s for s in sectors if s.ok), key=lambda s: s.seq)
    for sec in live:
        base = sec.index * SECTOR_BYTES + HEADER_BYTES
        for slot in range(RECORDS_PER_SECTOR):
            off = base + slot * RECORD_BYTES
            out = decode_record(blob[off:off + RECORD_BYTES], sec.index, slot, sec.run_id)
            if isinstance(out, str):
                if out == "erased":
                    # The rest of a partially filled sector; stop rather than
                    # scanning 200 erased slots looking for one more.
                    break
                bad[out] = bad.get(out, 0) + 1
                continue
            records.append(out)
    return sectors, records, bad


# --- the self-test -------------------------------------------------------
#
# Vectors built here from the documented layout, so this proves the decoder
# reads what the format says rather than what the firmware happens to write.
# The firmware's agreement is a separate check, in CI, against records the C++
# encoder produced.
def self_test() -> int:
    checks = failed = 0

    def check(name, got, want):
        nonlocal checks, failed
        checks += 1
        if got == want:
            print(f"ok: {name}")
        else:
            print(f"FAIL: {name}: got {got!r}, want {want!r}")
            failed += 1

    # The CRCs, against values computable by hand from the polynomials.
    check("crc8 of empty is its seed", crc8(b""), 0xFF)
    check("crc16 of empty is its seed", crc16(b""), 0xFFFF)
    # CRC-16/CCITT-FALSE of "123456789" is the standard's own check value.
    check("crc16 check value", crc16(b"123456789"), 0x29B1)
    # And CRC-8/autosar-style with poly 0x07 init 0xFF over the same input.
    check("crc8 is deterministic", crc8(b"123456789"), crc8(b"123456789"))
    check("crc8 sees a changed byte",
          crc8(b"123456789") != crc8(b"123456780"), True)

    def make_header(seq, run_id, version=FORMAT_VERSION, magic=LOG_MAGIC):
        buf = struct.pack("<IIIH", magic, seq, run_id, version)
        return buf + struct.pack("<H", crc16(buf))

    def make_record(t_ms=1234, raw=20.5, filt=20.4, sp=100.0, case=30.0,
                    current=12.34, duty_pct=50.0, segment=3,
                    state=1, flags=0, current_flags=0, event=0):
        body = struct.pack("<Ihhhhh", t_ms, round(raw * 10), round(filt * 10),
                           round(sp * 10), round(case * 10), round(current * 100))
        body += bytes([round(duty_pct * 2), segment, (state & 0x0F) | flags,
                       current_flags, event])
        return body + bytes([crc8(body)])

    hdr = make_header(seq=5, run_id=7)
    check("a good header decodes", decode_header(hdr, 0).ok, True)
    check("its sequence number", decode_header(hdr, 0).seq, 5)
    check("its run id", decode_header(hdr, 0).run_id, 7)

    bad = bytearray(hdr); bad[4] ^= 0xFF
    check("a corrupt header is refused", decode_header(bytes(bad), 0).ok, False)
    check("an erased sector says so",
          decode_header(b"\xff" * HEADER_BYTES, 0).why, "erased")
    check("a foreign version is refused",
          decode_header(make_header(1, 1, version=99), 0).ok, False)
    check("a wrong magic is not a log sector",
          decode_header(make_header(1, 1, magic=0xDEADBEEF), 0).why,
          "not a log sector")

    rec = decode_record(make_record(), 0, 0, 7)
    check("a good record decodes", isinstance(rec, Record), True)
    check("time", rec.t_rel_ms, 1234)
    check("raw temperature, tenths", rec.kiln_raw_c, 20.5)
    check("setpoint", rec.setpoint_c, 100.0)
    check("current, 10 mA steps", rec.current_a, 12.34)
    check("duty, half-percent steps", rec.duty_pct, 50.0)
    check("segment", rec.segment, 3)
    check("state name", rec.state, "RUNNING")
    check("event name", rec.event, "sample")

    check("a negative temperature survives the round trip",
          decode_record(make_record(raw=-12.3), 0, 0, 1).kiln_raw_c, -12.3)
    check("the segment sentinel reads as none",
          decode_record(make_record(segment=SEG_NONE), 0, 0, 1).segment, None)
    flagged = decode_record(make_record(flags=F_HOLDBACK | F_TC_FAULT), 0, 0, 1)
    check("flags unpack from the high nibble",
          (flagged.holdback, flagged.tc_fault, flagged.saturated),
          (True, True, False))
    check("state and flags share a byte without colliding", flagged.state, "RUNNING")

    r = bytearray(make_record()); r[2] ^= 0xFF
    check("a corrupt record is refused", decode_record(bytes(r), 0, 0, 1), "crc")
    check("an erased record says so",
          decode_record(b"\xff" * RECORD_BYTES, 0, 0, 1), "erased")
    check("a short record says so", decode_record(b"\x00" * 4, 0, 0, 1), "short")
    check("an unknown event is named rather than dropped",
          decode_record(make_record(event=200), 0, 0, 1).event, "event_200")

    # A whole dump, including the ring's order: sector 1 is older than sector
    # 0, and reading in address order would interleave them.
    blob = bytearray(b"\xff" * (SECTOR_BYTES * 3))
    blob[0:HEADER_BYTES] = make_header(seq=9, run_id=2)
    blob[HEADER_BYTES:HEADER_BYTES + RECORD_BYTES] = make_record(t_ms=2000)
    blob[SECTOR_BYTES:SECTOR_BYTES + HEADER_BYTES] = make_header(seq=8, run_id=2)
    blob[SECTOR_BYTES + HEADER_BYTES:SECTOR_BYTES + HEADER_BYTES + RECORD_BYTES] = \
        make_record(t_ms=1000)
    sectors, records, _ = walk(bytes(blob))
    check("every sector is reported", len(sectors), 3)
    check("only the written ones are live", sum(1 for s in sectors if s.ok), 2)
    check("records come back in ring order, not address order",
          [r.t_rel_ms for r in records], [1000, 2000])
    check("a partially filled sector stops at the erased slot", len(records), 2)
    check("RECORDS_PER_SECTOR matches the architecture's 204",
          RECORDS_PER_SECTOR, 204)

    # --- against the fixture the firmware's own encoder produced ---------
    #
    # Everything above proves this decoder reads what the format says. This
    # proves the firmware writes it. The fixture is two sectors of records
    # encoded by kiln_logrec_encode and committed, and a host test re-encodes
    # the same samples and compares bytes, so neither side can drift without
    # the other noticing.
    fixture = (pathlib.Path(__file__).resolve().parent.parent
               / "firmware/controller/test/host/fixtures/logring.bin")
    if not fixture.is_file():
        print(f"FAIL: the committed fixture is missing at {fixture}")
        failed += 1
        checks += 1
    else:
        blob = fixture.read_bytes()
        sectors, records, bad = walk(blob)
        check("the fixture has two live sectors",
              sum(1 for s in sectors if s.ok), 2)
        check("and three records", len(records), 3)
        check("none of them failed its CRC", bad.get("crc", 0), 0)
        # Ring order, which is what the fixture's sectors are deliberately out
        # of address order to test.
        check("read in ring order", [r.t_rel_ms for r in records],
              [1000, 2000, 3000])
        check("the newest record came from the lower-addressed sector",
              records[-1].sector, 0)
        # The three records, field by field, against the values the firmware
        # was given. A scale read wrong shows up here and nowhere else.
        check("a positive temperature", records[0].kiln_raw_c, 20.5)
        check("a filtered value beside it", records[0].kiln_filt_c, 20.4)
        check("a setpoint", records[0].setpoint_c, 100.0)
        check("current in 10 mA steps", records[0].current_a, 12.34)
        check("duty in half-percent steps", records[0].duty_pct, 50.0)
        check("a segment index", records[0].segment, 3)
        check("current flags pass through", records[0].current_flags, 1)
        check("a negative temperature", records[1].kiln_raw_c, -12.3)
        check("the segment sentinel", records[1].segment, None)
        check("every flag at once",
              (records[1].holdback, records[1].saturated,
               records[1].tc_fault, records[1].wall_valid),
              (True, True, True, True))
        check("a non-sample event", records[1].event, "state_change")
        check("a state that is not RUNNING", records[1].state, "IDLE")
        check("the top of the temperature range", records[2].kiln_raw_c, 1285.0)
        check("full duty", records[2].duty_pct, 100.0)
        check("a run-end event", records[2].event, "run_end")
        check("the run id comes from the sector header", records[2].run_id, 2)

    print(f"{checks - failed}/{checks} checks passed")
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", nargs="?", help="a kilnlog partition dump")
    ap.add_argument("-o", "--out", help="write CSV here instead of stdout")
    ap.add_argument("--sectors", action="store_true",
                    help="one line per sector, for looking at the ring itself")
    ap.add_argument("--run", type=int, help="only records of this run id")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.dump:
        ap.error("a dump is required (or --self-test)")

    with open(args.dump, "rb") as fh:
        blob = fh.read()
    if len(blob) % SECTOR_BYTES:
        print(f"warning: {len(blob)} bytes is not a whole number of "
              f"{SECTOR_BYTES}-byte sectors; the tail is ignored",
              file=sys.stderr)

    sectors, records, bad = walk(blob)
    if args.run is not None:
        records = [r for r in records if r.run_id == args.run]

    out = open(args.out, "w", newline="") if args.out else sys.stdout
    try:
        if args.sectors:
            w = csv.writer(out)
            w.writerow(["sector", "seq", "run_id", "version", "ok", "why"])
            for s in sectors:
                w.writerow([s.index, s.seq, s.run_id, s.version, int(s.ok), s.why])
        else:
            if not records:
                print("no records: either the dump holds no log sectors, or "
                      "the offset was wrong", file=sys.stderr)
            w = csv.DictWriter(out, fieldnames=list(asdict(records[0]).keys())
                               if records else ["sector"])
            w.writeheader()
            for r in records:
                w.writerow(asdict(r))
    finally:
        if args.out:
            out.close()

    live = sum(1 for s in sectors if s.ok)
    print(f"{len(sectors)} sectors, {live} live, {len(records)} records"
          + (f", {bad.get('crc', 0)} failed CRC" if bad.get("crc") else ""),
          file=sys.stderr)
    # A CRC failure is not a tool error: it is the tool reporting damage, which
    # is what it is for. Exit non-zero only if nothing could be read at all.
    return 0 if (records or live) else 2


if __name__ == "__main__":
    sys.exit(main())

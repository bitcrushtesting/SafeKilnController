#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and flash the production data block (SWR-PROD-01).
#
#   tools/prod-data.py generate --serial SK1-2026-000042 -o prod.bin
#   tools/prod-data.py flash    --serial SK1-2026-000042 --port /dev/ttyUSB0
#   tools/prod-data.py read     --port /dev/ttyUSB0
#   tools/prod-data.py --self-test
#
# The block holds manufacturer, device model, board revision, serial number and
# production date, written once at manufacture into the `prod` NVS partition and
# never written again: the firmware opens that partition NVS_READONLY.
#
# Two things this tool refuses to guess:
#
#   The partition offset and size are read from firmware/controller/partitions.csv, not
#   typed here. Writing a production block over `kilnlog` because two files
#   disagreed about an offset is exactly the failure that a second copy of a
#   constant invites.
#
#   The serial number has no default. Everything else can sensibly default for a
#   given product; a serial number that defaults is a serial number that gets
#   duplicated across a batch.
#
# esptool and the NVS partition generator both ship with ESP-IDF, so this is not
# a network fetch (UR-CON-04). `generate` needs only the generator; `flash` and
# `read` need esptool and a board.
import argparse
import csv
import datetime
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PARTITIONS = ROOT / "firmware" / "controller" / "partitions.csv"

PARTITION_LABEL = "prod"      # must match partitions.csv
NAMESPACE = "prod"            # must match hal_prod.cpp

# Field name -> (NVS key, capacity in kiln_prod_info_t including the NUL).
# The capacities are the firmware's, so a value that would be truncated on the
# device is rejected here instead, where it is still a typo rather than a wrong
# serial number on a shipped unit.
FIELDS = {
    "manufacturer":    ("manufacturer", 32),
    "model":           ("model", 24),
    "revision":        ("revision", 16),
    "serial":          ("serial", 24),
    "production_date": ("prod_date", 11),
}

DEFAULTS = {
    "manufacturer": "Bitcrush Testing",
    "model": "SafeKiln-1",
    "revision": "rev-C",
}

# A serial number is scanned, typed and read aloud over a phone, so it is
# restricted to characters that survive all three.
SERIAL_RE = re.compile(r"^[A-Z0-9][A-Z0-9-]{2,22}$")
REVISION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.\-_]{0,14}$")


class Error(Exception):
    """A problem worth a clean message rather than a traceback."""


# --- the partition table, as the single source of truth ------------------

def read_partition(path=PARTITIONS, label=PARTITION_LABEL, require_nvs=True):
    """Return (offset, size) in bytes for `label` from an IDF partitions CSV.

    Raises if the partition is absent, so a firmware built without the `prod`
    partition cannot be programmed as though it had one.

    `require_nvs` is on by default and is the guard that matters: it is what
    stops an NVS image being written over a partition that holds something else.
    Pass it False only to ask where some other partition lies, which the
    self-test does to prove `prod` does not overlap `kilnlog`.
    """
    text = pathlib.Path(path).read_text()
    rows = csv.reader(io.StringIO(text))
    for row in rows:
        if not row or row[0].strip().startswith("#"):
            continue
        cells = [c.strip() for c in row]
        if len(cells) < 5 or cells[0] != label:
            continue
        p_type, subtype, offset, size = cells[1], cells[2], cells[3], cells[4]
        if require_nvs and (p_type != "data" or subtype != "nvs"):
            raise Error(
                f"partition `{label}` is {p_type}/{subtype}, not data/nvs; "
                "refusing to write an NVS image into it"
            )
        return _parse_size(offset), _parse_size(size)
    raise Error(
        f"no `{label}` partition in {path}. The firmware must be built with it "
        "before a unit can be programmed."
    )


def _parse_size(s):
    """IDF sizes: 0x1000, 4096, 16K, 2M."""
    s = s.strip()
    if not s:
        raise Error("empty size or offset in the partition table")
    mult = 1
    if s[-1] in "kK":
        mult, s = 1024, s[:-1]
    elif s[-1] in "mM":
        mult, s = 1024 * 1024, s[:-1]
    try:
        return int(s, 0) * mult
    except ValueError as exc:
        raise Error(f"cannot parse size or offset {s!r}") from exc


# --- validation ----------------------------------------------------------

def validate(values):
    """Check every field, and return them in NVS-key form.

    Every rule here mirrors one the firmware or the operator depends on, and
    every failure is raised rather than trimmed: this is the last point at which
    a bad value is cheap to fix.
    """
    out = {}
    for field, (key, cap) in FIELDS.items():
        v = values.get(field)
        if v is None or v == "":
            raise Error(f"{field} is required")
        if len(v.encode()) > cap - 1:
            raise Error(
                f"{field} is {len(v.encode())} bytes, but the firmware's field "
                f"holds {cap - 1} plus a NUL; shorten it rather than let the "
                "device truncate it"
            )
        out[key] = v

    if not SERIAL_RE.match(values["serial"]):
        raise Error(
            f"serial {values['serial']!r} is not of the form the production "
            "process expects: upper-case letters, digits and hyphens, 3 to 23 "
            "characters, not starting with a hyphen"
        )
    if not REVISION_RE.match(values["revision"]):
        raise Error(f"revision {values['revision']!r} has characters that do not belong in one")

    d = values["production_date"]
    try:
        parsed = datetime.date.fromisoformat(d)
    except ValueError as exc:
        raise Error(f"production_date {d!r} is not an ISO 8601 YYYY-MM-DD date") from exc
    if parsed.isoformat() != d:
        raise Error(f"production_date {d!r} must be written exactly as YYYY-MM-DD")
    return out


# --- the NVS image -------------------------------------------------------

def nvs_csv(keys):
    """The CSV that nvs_partition_gen.py consumes.

    One namespace, five string entries. `type` is `data` and `encoding` is
    `string` so that the firmware reads them with nvs_get_str.
    """
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["key", "type", "encoding", "value"])
    w.writerow([NAMESPACE, "namespace", "", ""])
    for key, value in keys.items():
        w.writerow([key, "data", "string", value])
    return buf.getvalue()


def find_nvs_generator():
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise Error(
            "IDF_PATH is not set: run `. $IDF_PATH/export.sh` first. The NVS "
            "partition generator ships with ESP-IDF."
        )
    gen = pathlib.Path(idf) / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
    if not gen.is_file():
        raise Error(f"the NVS partition generator is not at {gen}")
    return gen


def generate(keys, size, out_path, quiet=False):
    """Write an NVS image of exactly `size` bytes to `out_path`."""
    gen = find_nvs_generator()
    with tempfile.TemporaryDirectory() as td:
        csv_path = pathlib.Path(td) / "prod.csv"
        csv_path.write_text(nvs_csv(keys))
        cmd = [sys.executable, str(gen), "generate",
               str(csv_path), str(out_path), str(size)]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise Error("nvs_partition_gen failed:\n" + (proc.stderr or proc.stdout))
    produced = pathlib.Path(out_path).stat().st_size
    if produced != size:
        raise Error(
            f"the generator produced {produced} bytes for a {size} byte "
            "partition; refusing to flash a size the table does not describe"
        )
    if not quiet:
        print(f"wrote {out_path} ({produced} bytes) for namespace `{NAMESPACE}`")
    return out_path


# --- esptool -------------------------------------------------------------

def esptool_cmd():
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    if shutil.which("esptool"):
        return ["esptool"]
    return [sys.executable, "-m", "esptool"]


def run_esptool(args, dry_run):
    cmd = esptool_cmd() + args
    printable = " ".join(cmd)
    if dry_run:
        print(f"would run: {printable}")
        return
    print(f"running: {printable}")
    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0:
        raise Error(f"esptool exited {proc.returncode}")


# --- commands ------------------------------------------------------------

def collect(args):
    return {
        "manufacturer": args.manufacturer,
        "model": args.model,
        "revision": args.revision,
        "serial": args.serial,
        "production_date": args.date,
    }


def cmd_generate(args):
    _, size = read_partition()
    keys = validate(collect(args))
    generate(keys, size, args.output)
    return 0


def cmd_flash(args):
    offset, size = read_partition()
    keys = validate(collect(args))
    print(f"partition `{PARTITION_LABEL}` at {offset:#x}, {size} bytes "
          f"(from {PARTITIONS.relative_to(ROOT)})")
    for key, value in keys.items():
        print(f"  {key:13} {value}")

    out = args.output
    with tempfile.TemporaryDirectory() as td:
        if out is None:
            out = str(pathlib.Path(td) / "prod.bin")
        generate(keys, size, out, quiet=True)
        flash_args = []
        if args.port:
            flash_args += ["--port", args.port]
        if args.baud:
            flash_args += ["--baud", str(args.baud)]
        flash_args += ["--chip", "esp32s3", "write_flash", hex(offset), out]
        run_esptool(flash_args, args.dry_run)
    if not args.dry_run:
        print("programmed. Power-cycle the board and check the boot log for "
              "`unit ... serial ...`.")
    return 0


def cmd_read(args):
    """Read the partition back off a board, so the written block can be
    confirmed without trusting that the write worked."""
    offset, size = read_partition()
    out = args.output or "prod-readback.bin"
    flash_args = []
    if args.port:
        flash_args += ["--port", args.port]
    flash_args += ["--chip", "esp32s3", "read_flash", hex(offset), hex(size), out]
    run_esptool(flash_args, args.dry_run)
    if args.dry_run:
        return 0
    found = strings_in(pathlib.Path(out).read_bytes())
    print(f"strings in {out}:")
    for s in found:
        print(f"  {s}")
    return 0


def strings_in(blob, minlen=3):
    """Printable ASCII runs. NVS stores string values verbatim, so this is
    enough to confirm a block by eye without reimplementing the NVS format."""
    out, cur = [], bytearray()
    for b in blob:
        if 0x20 <= b < 0x7F:
            cur.append(b)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode())
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode())
    return out


# --- self-test -----------------------------------------------------------

def self_test():
    """Everything that does not need ESP-IDF or a board.

    The offset arithmetic and the validation are the two places where a mistake
    is expensive and silent, so they are the two that get tested.
    """
    checks, failed = 0, 0

    def check(what, got, want):
        nonlocal checks, failed
        checks += 1
        if got != want:
            failed += 1
            print(f"  FAIL {what}: got {got!r}, want {want!r}")

    def raises(what, fn):
        nonlocal checks, failed
        checks += 1
        try:
            fn()
        except Error:
            return
        except Exception as exc:                       # noqa: BLE001
            failed += 1
            print(f"  FAIL {what}: raised {type(exc).__name__}, want Error")
            return
        failed += 1
        print(f"  FAIL {what}: did not raise")

    # sizes
    check("0x4000", _parse_size("0x4000"), 16384)
    check("16K", _parse_size("16K"), 16384)
    check("2M", _parse_size("2M"), 2 * 1024 * 1024)
    check("4096", _parse_size("4096"), 4096)
    raises("nonsense size", lambda: _parse_size("banana"))

    # the real partition table, which is the whole point of reading it
    offset, size = read_partition()
    check("prod offset", offset, 0x6A0000)
    check("prod size", size, 16 * 1024)
    # It must not overlap the partition before it.
    log_off, log_size = read_partition(label="kilnlog", require_nvs=False)
    check("prod starts where kilnlog ends", offset, log_off + log_size)

    raises("absent partition", lambda: read_partition(label="nosuchpart"))
    raises("wrong partition type", lambda: read_partition(label="kilnlog"))

    good = {
        "manufacturer": "Bitcrush Testing",
        "model": "SafeKiln-1",
        "revision": "rev-C",
        "serial": "SK1-2026-000042",
        "production_date": "2026-10-07",
    }
    keys = validate(dict(good))
    check("serial maps to its NVS key", keys["serial"], "SK1-2026-000042")
    check("date uses the short NVS key", keys["prod_date"], "2026-10-07")
    check("five entries", len(keys), 5)

    raises("missing serial", lambda: validate({**good, "serial": ""}))
    raises("lower-case serial", lambda: validate({**good, "serial": "sk1-2026-42"}))
    raises("serial with a space", lambda: validate({**good, "serial": "SK1 2026"}))
    raises("serial starting with a hyphen", lambda: validate({**good, "serial": "-SK12026"}))
    raises("over-long serial", lambda: validate({**good, "serial": "S" + "1" * 23}))
    raises("over-long model", lambda: validate({**good, "model": "M" * 24}))
    raises("date with slashes", lambda: validate({**good, "production_date": "2026/10/07"}))
    raises("date without padding", lambda: validate({**good, "production_date": "2026-1-7"}))
    raises("impossible date", lambda: validate({**good, "production_date": "2026-02-31"}))

    # The 11 byte date field holds exactly YYYY-MM-DD plus its NUL, so a
    # correct date must never be the thing that trips the capacity check.
    check("date is exactly the field width", len("2026-10-07"), FIELDS["production_date"][1] - 1)

    # the generated CSV
    text = nvs_csv(keys)
    lines = text.strip().split("\n")
    check("CSV header", lines[0], "key,type,encoding,value")
    check("namespace row first", lines[1], f"{NAMESPACE},namespace,,")
    check("one row per field plus header and namespace", len(lines), 7)
    check("values are strings", all(",data,string," in ln for ln in lines[2:]), True)

    # strings_in, which is what `read` reports a block with
    check("strings_in finds a value",
          "SK1-2026-000042" in strings_in(b"\x00\xffSK1-2026-000042\x00\x00"), True)

    print(f"{checks} checks, {failed} failed")
    return 1 if failed else 0


# --- argument parsing ----------------------------------------------------

def add_fields(p, require_serial=True):
    p.add_argument("--manufacturer", default=DEFAULTS["manufacturer"])
    p.add_argument("--model", default=DEFAULTS["model"])
    p.add_argument("--revision", default=DEFAULTS["revision"],
                   help="board revision, e.g. rev-C")
    p.add_argument("--serial", required=require_serial,
                   help="unit serial number; deliberately has no default")
    p.add_argument("--date", default=datetime.date.today().isoformat(),
                   metavar="YYYY-MM-DD", help="production date (default: today)")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Create and flash the ESP32 production data block.")
    ap.add_argument("--self-test", action="store_true",
                    help="check the offset arithmetic and validation, then exit")
    sub = ap.add_subparsers(dest="cmd")

    g = sub.add_parser("generate", help="write the NVS image to a file")
    add_fields(g)
    g.add_argument("-o", "--output", default="prod.bin")
    g.set_defaults(fn=cmd_generate)

    f = sub.add_parser("flash", help="generate and write it to a board")
    add_fields(f)
    f.add_argument("--port", help="serial port; esptool autodetects if omitted")
    f.add_argument("--baud", type=int, default=460800)
    f.add_argument("-o", "--output", help="also keep the generated image here")
    f.add_argument("--dry-run", action="store_true",
                   help="print the esptool command instead of running it")
    f.set_defaults(fn=cmd_flash)

    r = sub.add_parser("read", help="read the block back off a board")
    r.add_argument("--port")
    r.add_argument("-o", "--output")
    r.add_argument("--dry-run", action="store_true")
    r.set_defaults(fn=cmd_read)

    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    try:
        return args.fn(args)
    except Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

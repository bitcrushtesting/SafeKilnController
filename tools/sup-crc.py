#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stamp the supervisor image with the CRC of its own program memory.
#
#   tools/sup-crc.py build-sup-target/supervisor.elf
#   tools/sup-crc.py --self-test
#
# The supervisor checks this at boot (sup_flash_ok) and refuses to permit heat
# if it does not match, so an image that has not been through this step does not
# run. That is deliberate: SUP_CRC_UNPROGRAMMED is treated as a failure rather
# than as "no expectation", because an image that reached a board without being
# stamped is exactly the one whose integrity is unknown.
#
# Three addresses come out of the ELF rather than being written down here:
#
#   _sup_crc_region_start   first byte covered
#   _sup_crc_region_end     one past the last byte covered
#   sup_expected_crc        where the 4-byte digest goes, immediately after the
#                           region, because a digest cannot cover itself
#
# Reading them from the ELF is the point. The linker decides the layout, and a
# tool that hard-coded an offset would silently stamp the wrong word the first
# time somebody added a function.
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import zlib

FLASH_ORIGIN = 0x08000000      # STM32G031, and asserted against the ELF below

REGION_START = "_sup_crc_region_start"
REGION_END = "_sup_crc_region_end"
DIGEST = "sup_expected_crc"
UNPROGRAMMED = 0xFFFFFFFF


class Error(Exception):
    """A problem worth a clean message rather than a traceback."""


def nm_tool():
    for c in ("arm-none-eabi-nm", "llvm-nm", "nm"):
        if shutil.which(c):
            return c
    raise Error("no nm found; put arm-none-eabi-nm on PATH")


def objcopy_tool():
    for c in ("arm-none-eabi-objcopy", "llvm-objcopy", "objcopy"):
        if shutil.which(c):
            return c
    raise Error("no objcopy found; put arm-none-eabi-objcopy on PATH")


def symbols(elf):
    """Address of each symbol this tool needs, from the ELF's own table."""
    out = subprocess.run([nm_tool(), str(elf)], capture_output=True, text=True,
                         check=False)
    if out.returncode != 0:
        raise Error(f"nm failed on {elf}:\n{out.stderr}")
    found = {}
    for line in out.stdout.splitlines():
        m = re.match(r'^([0-9a-fA-F]+)\s+\S\s+(\S+)$', line.strip())
        if m and m.group(2) in (REGION_START, REGION_END, DIGEST):
            found[m.group(2)] = int(m.group(1), 16)
    for want in (REGION_START, REGION_END, DIGEST):
        if want not in found:
            raise Error(
                f"{elf} has no {want} symbol. Either it is not a supervisor "
                "image, or the linker script no longer marks the region."
            )
    return found


def check_layout(sym):
    """The invariants that make the digest meaningful.

    Checked rather than assumed, because every one of them failing produces an
    image that boots and quietly checks the wrong bytes.
    """
    start, end, digest = sym[REGION_START], sym[REGION_END], sym[DIGEST]
    if start < FLASH_ORIGIN:
        raise Error(f"{REGION_START} {start:#x} is below the flash origin")
    if end <= start:
        raise Error(f"empty or inverted region: {start:#x}..{end:#x}")
    if digest < end:
        raise Error(
            f"the digest at {digest:#x} lies INSIDE the region it covers "
            f"({start:#x}..{end:#x}). A digest cannot cover itself; the "
            "linker script has to place .sup_crc after _sup_crc_region_end."
        )
    if digest % 4:
        raise Error(f"the digest at {digest:#x} is not word aligned")
    return start, end, digest


def to_bin(elf, out):
    r = subprocess.run([objcopy_tool(), "-O", "binary", str(elf), str(out)],
                       capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise Error(f"objcopy failed:\n{r.stderr}")
    return pathlib.Path(out)


def stamp(elf, out_bin=None, quiet=False):
    elf = pathlib.Path(elf)
    if not elf.is_file():
        raise Error(f"no such image: {elf}")
    sym = symbols(elf)
    start, end, digest = check_layout(sym)

    out = pathlib.Path(out_bin) if out_bin else elf.with_suffix(".bin")
    to_bin(elf, out)
    image = bytearray(out.read_bytes())

    s, e, d = start - FLASH_ORIGIN, end - FLASH_ORIGIN, digest - FLASH_ORIGIN
    if d + 4 > len(image):
        raise Error(
            f"the digest slot at {digest:#x} is past the end of the {len(image)} "
            "byte image; the .sup_crc section was stripped from the binary"
        )
    placeholder = int.from_bytes(image[d:d + 4], "little")
    if placeholder != UNPROGRAMMED:
        raise Error(
            f"the digest slot already holds {placeholder:#010x}, not the "
            f"{UNPROGRAMMED:#010x} placeholder. This image looks stamped "
            "already; stamping twice would hash a changed image."
        )

    crc = zlib.crc32(bytes(image[s:e])) & 0xFFFFFFFF
    image[d:d + 4] = crc.to_bytes(4, "little")
    out.write_bytes(bytes(image))

    if not quiet:
        print(f"region  {start:#010x}..{end:#010x}  ({e - s} bytes)")
        print(f"digest  {digest:#010x}  = {crc:#010x}")
        print(f"wrote   {out} ({len(image)} bytes)")
    return crc


def self_test():
    """Everything that needs no toolchain and no image.

    The layout invariants are the point: each of them failing gives a firmware
    that boots and checks the wrong bytes, which is worse than one that fails
    to boot.
    """
    checks = failed = 0

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
        except Exception as exc:                      # noqa: BLE001
            failed += 1
            print(f"  FAIL {what}: raised {type(exc).__name__}, want Error")
            return
        failed += 1
        print(f"  FAIL {what}: did not raise")

    ok = {REGION_START: 0x08000000, REGION_END: 0x08001194, DIGEST: 0x08001194}
    check("a good layout is accepted", check_layout(ok),
          (0x08000000, 0x08001194, 0x08001194))

    raises("a digest inside its own region",
           lambda: check_layout({**ok, DIGEST: 0x08001000}))
    raises("an inverted region",
           lambda: check_layout({**ok, REGION_END: 0x08000000}))
    raises("an empty region",
           lambda: check_layout({REGION_START: 0x08001000,
                                 REGION_END: 0x08001000, DIGEST: 0x08001004}))
    raises("a region below the flash origin",
           lambda: check_layout({**ok, REGION_START: 0x00000000}))
    raises("a misaligned digest",
           lambda: check_layout({**ok, DIGEST: 0x08001195}))

    # The CRC must be the one the firmware computes. sup_crc32 is tested
    # against these same zlib vectors on the host, so the two agree by
    # construction rather than by hope.
    check("zlib crc of '123456789'", zlib.crc32(b"123456789") & 0xFFFFFFFF,
          0xCBF43926)
    check("zlib crc of empty", zlib.crc32(b"") & 0xFFFFFFFF, 0x00000000)

    # little-endian round trip, which is the byte order the Cortex-M reads
    v = 0xDEADBEEF
    check("little-endian round trip",
          int.from_bytes(v.to_bytes(4, "little"), "little"), v)
    check("placeholder is all ones", UNPROGRAMMED, 0xFFFFFFFF)

    print(f"{checks} checks, {failed} failed")
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Stamp the supervisor image with the CRC of its program memory.")
    ap.add_argument("elf", nargs="?", help="the linked supervisor.elf")
    ap.add_argument("-o", "--output", help="binary to write (default: <elf>.bin)")
    ap.add_argument("--self-test", action="store_true",
                    help="check the layout invariants and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.elf:
        ap.print_help()
        return 2
    try:
        stamp(args.elf, args.output)
        return 0
    except Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

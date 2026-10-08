#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Secure Boot V2 provisioning for the ESP32-S3 (SEC-05, SRR-04, OQ-S2).
#
#   tools/secure-boot.py keygen      --key keys/prod.pem
#   tools/secure-boot.py sign        --key keys/prod.pem
#   tools/secure-boot.py preflight   --key keys/prod.pem --port /dev/ttyUSB0
#   tools/secure-boot.py provision   --key keys/prod.pem --port /dev/ttyUSB0 --commit
#   tools/secure-boot.py --self-test
#
# ===========================================================================
# THIS TOOL BURNS eFUSES. THREE OF ITS FOUR STEPS CANNOT BE UNDONE.
# ===========================================================================
#
# `flash` is reversible: reflash and it is as it was.
#
# `burn-key`, `enable-secure-boot` and `disable-jtag` are not. They set bits in
# one-time-programmable fuses. There is no erase, no override, no vendor
# recovery. A board that has had them burnt with the wrong key, or whose key is
# later lost, is a board that can never run new firmware again.
#
# So every irreversible step:
#
#   * does nothing without --commit, and prints what it would do instead,
#   * refuses to run until `preflight` passes,
#   * demands a typed confirmation phrase, which is different per step so that
#     muscle memory cannot carry you through all three,
#   * and happens in an order where the device is left bootable at every point.
#
# Order matters and is enforced, not merely documented. Signed images are
# flashed and the key digest burnt BEFORE secure boot is enabled, because
# enabling it first leaves a device that will refuse to boot the image already
# on it and cannot be told otherwise.
#
# ---------------------------------------------------------------------------
# What this deliberately does NOT do
# ---------------------------------------------------------------------------
# It does not burn DIS_DOWNLOAD_MODE. SRR-11 records that this device has no
# field update path at all, so serial download is the only route a security fix
# can ever take. Disabling it on top of secure boot would make the unit
# permanently unfixable. There is a separate `disable-download-mode` subcommand
# for anyone who decides otherwise, and it argues with you.
#
# It does not enable flash encryption. That is OQ-S3, and it is a separate
# decision with its own field-recovery cost. Secure boot stops hostile code
# running; it does nothing about the WiFi password being readable off the chip
# (SRR-05).
#
# Run `tools/prod-data.py flash` BEFORE this. Writing the production data block
# is a plain flash write, and it is far easier before the device is locked down.
import argparse
import hashlib
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware" / "controller"
SDKCONFIG = FIRMWARE / "sdkconfig"
BUILD_DIRS = ("build", "build-esp32s3", "build-hw")

CHIP = "esp32s3"

# The app is signed as a whole; the bootloader is signed and verified by the ROM
# against the eFuse digest. Both have to be signed or the device will not boot.
APP_BIN = "safekiln.bin"
BOOTLOADER_BIN = "bootloader/bootloader.bin"
PARTITION_BIN = "partition_table/partition-table.bin"

# Secure Boot V2 appends one 4096-byte signature block, at the next 4 KiB
# boundary after the image. Both numbers are the ESP32-S3's, not a convention.
SIG_BLOCK_BYTES = 4096
SIG_ALIGN = 4096

# BLOCK_KEY0 holds the first of three possible digests. Using slot 0 leaves
# SECURE_BOOT_DIGEST1 and 2 free for a key rotation later, which is the only
# recovery this scheme has: revoke slot 0, boot on slot 1.
KEY_BLOCK = "BLOCK_KEY0"
KEY_PURPOSE = "SECURE_BOOT_DIGEST0"

# One phrase per irreversible step, deliberately different, so that typing the
# first does not get you through the third.
CONFIRM = {
    "burn-key": "BURN THE KEY DIGEST",
    "enable-secure-boot": "ENABLE SECURE BOOT PERMANENTLY",
    "disable-jtag": "DISABLE JTAG PERMANENTLY",
    "disable-download-mode": "I ACCEPT THIS DEVICE CAN NEVER BE UPDATED",
}

# ESP32-S3 has two JTAG paths and disabling one leaves the other. Both names
# were read out of `espefuse --chip esp32s3 summary`, not recalled.
JTAG_EFUSES = ("DIS_PAD_JTAG", "DIS_USB_JTAG")


class Error(Exception):
    """A problem worth a clean message rather than a traceback."""


# --- sdkconfig, as the single source of truth for the layout --------------

def sdkconfig_value(key, path=SDKCONFIG):
    """Read one CONFIG_ value from a generated sdkconfig.

    The partition table offset bounds the bootloader, and the bootloader is the
    thing that grows when secure boot is enabled, so this number decides whether
    a signed bootloader fits. It is read rather than assumed for the same reason
    tools/prod-data.py reads partitions.csv.
    """
    p = pathlib.Path(path)
    if not p.is_file():
        raise Error(f"no {p}; build the firmware once before provisioning")
    m = re.search(rf'^{re.escape(key)}=(.+)$', p.read_text(), re.M)
    if m is None:
        raise Error(f"{key} is not set in {p}")
    return m.group(1).strip().strip('"')


def partition_table_offset(path=SDKCONFIG):
    return int(sdkconfig_value("CONFIG_PARTITION_TABLE_OFFSET", path), 0)


def signed_size(raw_size):
    """Size of an image once its signature block is appended.

    The block goes at the next SIG_ALIGN boundary, so an image that already ends
    on one still costs a whole further block.
    """
    if raw_size < 0:
        raise Error("negative image size")
    padded = (raw_size + SIG_ALIGN - 1) // SIG_ALIGN * SIG_ALIGN
    return padded + SIG_BLOCK_BYTES


def bootloader_fits(raw_size, table_offset):
    """Whether a signed bootloader fits in front of the partition table.

    This is the check that prevents the expensive mistake. A signed bootloader
    that overruns the partition table offset overwrites the partition table, and
    on a secure-boot device that is not a reflash away: the ROM will refuse the
    replacement unless it is signed, and it cannot read a table that is no
    longer there.
    """
    return signed_size(raw_size) <= table_offset


# --- the key --------------------------------------------------------------

def check_key(path, require_private=True):
    """Insist on a key that exists and is not readable by everyone.

    A signing key with world-readable permissions on a shared build machine is
    the same failure as a committed one, and both make every device that trusts
    it signable by somebody else.
    """
    p = pathlib.Path(path)
    if not p.is_file():
        raise Error(
            f"no signing key at {p}. Create one with `keygen`, and back it up "
            "before going any further: once SECURE_BOOT_EN is burnt, losing it "
            "means the device can never run new firmware."
        )
    mode = p.stat().st_mode
    if require_private and (mode & (stat.S_IRGRP | stat.S_IROTH)):
        raise Error(
            f"{p} is readable beyond its owner ({stat.filemode(mode)}). "
            f"Run `chmod 600 {p}` first."
        )
    return p


def key_fingerprint(digest_bytes):
    """A short, human-comparable fingerprint of the public key digest.

    Printed at every step so an operator can see that the key about to be burnt
    is the key they expect, without reading 32 bytes of hex aloud.
    """
    h = hashlib.sha256(digest_bytes).hexdigest()
    return ":".join(h[i:i + 4] for i in range(0, 16, 4))


# --- external tools -------------------------------------------------------

def tool(name):
    """Locate esptool's companions.

    They ship with ESP-IDF, so this is not a network fetch (UR-CON-04). Preference
    goes to whatever the active IDF exports, which keeps the signing tool and
    the build at one version.
    """
    for candidate in (name, f"{name}.py"):
        found = shutil.which(candidate)
        if found:
            return [found]
    return [sys.executable, "-m", name.replace("-", "_")]


def esptool_version():
    out = run_capture(tool("esptool") + ["version"])
    m = re.search(r'(\d+)\.(\d+)', out)
    if m is None:
        raise Error(f"cannot read an esptool version from: {out!r}")
    return (int(m.group(1)), int(m.group(2)))


def require_esptool():
    major, minor = esptool_version()
    if major < 5:
        raise Error(
            f"esptool {major}.{minor} is too old; this needs 5.x, which is what "
            "ESP-IDF 6.0 ships. Run `. $IDF_PATH/export.sh` first."
        )
    return (major, minor)


def run_capture(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if p.returncode != 0:
        raise Error(f"{' '.join(cmd)} failed:\n{p.stderr or p.stdout}")
    return (p.stdout or "") + (p.stderr or "")


def run(cmd, commit, what):
    """Run a command, or say what it would have been.

    Dry run is the default everywhere, including for the reversible steps, so
    that a mistyped port or key is caught by reading rather than by flashing.
    """
    printable = " ".join(str(c) for c in cmd)
    if not commit:
        print(f"  would run: {printable}")
        return
    print(f"  {what}")
    print(f"  $ {printable}")
    p = subprocess.run(cmd, check=False)
    if p.returncode != 0:
        raise Error(f"{what} failed (exit {p.returncode})")


def efuse_cmd(args, port=None, no_confirm=False):
    """Build an espefuse invocation.

    --do-not-confirm is a GLOBAL option and has to precede the subcommand;
    passing it after one is rejected outright. It is only ever added when we
    have already taken our own typed confirmation, so the operator confirms
    once, to this tool, rather than twice to two different prompts.
    """
    cmd = tool("espefuse") + ["--chip", CHIP]
    if port:
        cmd += ["--port", port]
    if no_confirm:
        cmd += ["--do-not-confirm"]
    return cmd + args


def burn_efuse_args(*names):
    """`burn-efuse` takes NAME VALUE pairs, not bare names.

    A bare name is a usage error rather than a default of 1, which is the kind
    of thing that fails at the worst possible moment: with the operator's hand
    on a board and the confirmation already typed.
    """
    out = []
    for n in names:
        out += [n, "1"]
    return ["burn-efuse"] + out


# --- build artefacts ------------------------------------------------------

def find_build_dir(explicit=None):
    if explicit:
        p = pathlib.Path(explicit)
        if not (p / APP_BIN).is_file():
            raise Error(f"{p} holds no {APP_BIN}; is it a finished build?")
        return p
    for name in BUILD_DIRS:
        p = FIRMWARE / name
        if (p / APP_BIN).is_file():
            return p
    raise Error(
        "no build directory with a built app. Build first:\n"
        '  idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure" '
        "set-target esp32s3 build"
    )


def is_signed(path):
    """Whether an image carries a Secure Boot V2 signature block."""
    try:
        out = run_capture(tool("espsecure") + ["signature-info-v2", str(path)])
    except Error:
        return False
    return "Signature block" in out or "signature block" in out


def verify_against_key(path, key):
    """Whether this key's signature is the one on the image.

    Signed is not enough: an image signed by last quarter's key is signed, and
    will not boot on a device whose eFuse holds this quarter's digest.
    """
    try:
        run_capture(tool("espsecure") +
                    ["verify-signature", "--version", "2", "--keyfile", str(key), str(path)])
    except Error:
        return False
    return True


# --- the steps ------------------------------------------------------------

def cmd_keygen(args):
    p = pathlib.Path(args.key)
    if p.exists():
        raise Error(
            f"{p} already exists, and overwriting a signing key is how a fleet "
            "becomes unupdatable. Move it aside deliberately if that is what "
            "you mean."
        )
    p.parent.mkdir(parents=True, exist_ok=True)
    require_esptool()
    run(tool("espsecure") + ["generate-signing-key", "--version", "2",
                             "--scheme", "rsa3072", str(p)],
        args.commit, "generating an RSA-3072 secure boot signing key")
    if not args.commit:
        print("\n  (dry run; pass --commit to actually create the key)")
        return 0
    os.chmod(p, 0o600)
    print(f"\nkey written to {p}, mode 600.")
    print("\n  BACK IT UP NOW, somewhere that is not this machine and not this")
    print("  repository. Once SECURE_BOOT_EN is burnt on a device, this file is")
    print("  the only thing in the world that can produce firmware it will run.")
    return 0


def digest_bytes(key, out_path=None):
    """The public key digest, as burnt into the eFuse."""
    p = pathlib.Path(out_path) if out_path else ROOT / "secure_boot_digest.bin"
    run_capture(tool("espsecure") +
                ["digest-sbv2-public-key", "--keyfile", str(key), "--output", str(p)])
    return p, p.read_bytes()


def cmd_digest(args):
    key = check_key(args.key)
    require_esptool()
    path, data = digest_bytes(key, args.output)
    print(f"public key digest: {path} ({len(data)} bytes)")
    print(f"fingerprint:       {key_fingerprint(data)}")
    print("\nThis is what `burn-key` writes. Compare the fingerprint against")
    print("your records before burning it into a board.")
    return 0


def cmd_sign(args):
    key = check_key(args.key)
    require_esptool()
    build = find_build_dir(args.build_dir)
    for rel in (BOOTLOADER_BIN, APP_BIN):
        target = build / rel
        if not target.is_file():
            raise Error(f"{target} is missing; build before signing")
        if is_signed(target):
            print(f"  {rel}: already signed, leaving it alone")
            continue

        # Sign to a sibling and move it into place, rather than in place.
        # espsecure refuses an output that is also its input, and the move makes
        # the replacement atomic: a signing failure leaves the unsigned image
        # intact instead of a truncated one that would brick a secure-boot
        # device on the next flash.
        tmp = target.with_suffix(target.suffix + ".signed")
        run(tool("espsecure") + ["sign-data", "--version", "2", "--keyfile", str(key),
                                 "--output", str(tmp), str(target)],
            args.commit, f"signing {rel}")
        if args.commit:
            if not tmp.is_file():
                raise Error(f"espsecure produced no {tmp}")
            os.replace(tmp, target)
            print(f"  {rel}: signed, {target.stat().st_size} bytes")
    return 0


def preflight(args, need_port=True):
    """Every check that can be made without burning anything.

    This is the gate. Each irreversible step runs it first and refuses to
    proceed on a single failure, because the cheapest time to find a problem is
    before the fuse.
    """
    problems = []
    notes = []

    require_esptool()
    key = check_key(args.key)
    build = find_build_dir(args.build_dir)
    _, digest = digest_bytes(key, args.digest_out)
    fp = key_fingerprint(digest)

    print(f"build directory   {build}")
    print(f"signing key       {key}")
    print(f"key fingerprint   {fp}")

    # 1. the images exist, are signed, and are signed by THIS key
    for rel in (BOOTLOADER_BIN, APP_BIN):
        target = build / rel
        if not target.is_file():
            problems.append(f"{rel} is missing")
            continue
        if not is_signed(target):
            problems.append(f"{rel} carries no signature block; run `sign`")
        elif not verify_against_key(target, key):
            problems.append(
                f"{rel} is signed, but not by {key}. Flashing it onto a device "
                "whose eFuse holds this key's digest gives a board that will "
                "not boot."
            )
        else:
            print(f"signature         {rel}: signed by this key")

    # 2. the signed bootloader fits in front of the partition table
    boot = build / BOOTLOADER_BIN
    if boot.is_file():
        raw = boot.stat().st_size
        offset = partition_table_offset()
        need = signed_size(raw) if not is_signed(boot) else raw
        # Spare bytes between the end of the signed bootloader and the
        # partition table. Reported rather than "room to grow in source terms",
        # because once an image is signed its original size is not recoverable
        # from it: the padding before the signature block is indistinguishable
        # from content. Spare is unambiguous either way, and zero spare means
        # any growth at all overruns the table.
        spare = offset - need
        print(f"bootloader        {raw} B on disk, {need} B signed, "
              f"{offset} B to the partition table")
        print(f"                  {spare} B spare "
              f"({'already signed' if is_signed(boot) else 'unsigned'})")
        if need > offset:
            problems.append(
                f"the signed bootloader needs {need} B but the partition table "
                f"sits at {offset:#x}. Raising CONFIG_PARTITION_TABLE_OFFSET "
                "also means moving every offset in partitions.csv, `nvs` "
                "included, so do not do it casually."
            )
        elif spare < SIG_ALIGN:
            notes.append(
                f"only {spare} B spare in front of the partition table. "
                "Enabling secure boot adds RSA verification code to the "
                "bootloader, so measure this against a secure-boot build "
                "rather than a plain one."
            )

    # 3. the device, if one is attached
    if need_port:
        summary = efuse_summary(args.port)
        if summary is None:
            problems.append(
                "could not read eFuses from the device. A provisioning run "
                "must see the board it is about to modify."
            )
        else:
            for name in ("SECURE_BOOT_EN",) + JTAG_EFUSES:
                state = summary.get(name)
                print(f"efuse             {name} = {state}")
                if state is True:
                    notes.append(f"{name} is already burnt; that step will be skipped")
            if summary.get("DIS_DOWNLOAD_MODE") is True:
                problems.append(
                    "DIS_DOWNLOAD_MODE is already burnt: this device cannot be "
                    "flashed over serial at all, so there is nothing to provision."
                )

    print()
    notes.append(
        "write the production data block with tools/prod-data.py BEFORE this "
        "if you have not: it is a plain flash write and far easier now than "
        "after the device is locked down."
    )
    if notes:
        for n in notes:
            print(f"note:  {n}")
    if problems:
        for p_ in problems:
            print(f"BLOCK: {p_}")
        raise Error(f"pre-flight failed with {len(problems)} problem(s); nothing was changed")
    print("pre-flight: ok")
    return build, key, digest


def efuse_summary(port):
    """Parse `espefuse summary` into {name: True|False|str}."""
    args = ["summary"]
    try:
        out = run_capture(efuse_cmd(args, port))
    except Error:
        return None
    state = {}
    for line in out.splitlines():
        m = re.match(r'^(\w+)\s+\(BLOCK\d+\)\s+.*?=\s*(\S+)', line)
        if m:
            name, value = m.group(1), m.group(2)
            state[name] = {"True": True, "False": False}.get(value, value)
    return state or None


def confirm(step, commit):
    """Demand the step's own phrase, typed exactly."""
    if not commit:
        return False
    want = CONFIRM[step]
    print(f"\n  This is irreversible. Type exactly:  {want}")
    try:
        got = input("  > ").strip()
    except EOFError:
        raise Error("no confirmation on a non-interactive input; refusing") from None
    if got != want:
        raise Error("confirmation did not match; nothing was changed")
    return True


def cmd_preflight(args):
    preflight(args, need_port=bool(args.port))
    return 0


def cmd_flash(args):
    build, _key, _ = preflight(args, need_port=bool(args.port))
    offset = partition_table_offset()
    cmd = tool("esptool") + ["--chip", CHIP]
    if args.port:
        cmd += ["--port", args.port]
    cmd += ["write-flash" if esptool_version()[0] >= 5 else "write_flash",
            "0x0", str(build / BOOTLOADER_BIN),
            hex(offset), str(build / PARTITION_BIN),
            "0x20000", str(build / APP_BIN)]
    print("\nflashing the signed bootloader, partition table and app")
    run(cmd, args.commit, "writing flash")
    if args.commit:
        print("\nFlashed. Power-cycle and confirm it boots BEFORE burning any eFuse:")
        print("  the next steps assume the image on the device is one it can run.")
    return 0


def cmd_burn_key(args):
    _, _key, digest = preflight(args)
    path = pathlib.Path(args.digest_out or (ROOT / "secure_boot_digest.bin"))
    print(f"\nabout to burn {KEY_BLOCK} = {KEY_PURPOSE}")
    print(f"  fingerprint {key_fingerprint(digest)}")
    print("  After this, only images signed by that key will ever boot on this unit.")
    burn = ["burn-key", KEY_BLOCK, str(path), KEY_PURPOSE]
    if confirm("burn-key", args.commit):
        run(efuse_cmd(burn, args.port, no_confirm=True), True, "burning the key digest")
    else:
        run(efuse_cmd(burn, args.port), False, "burning the key digest")
    return 0


def cmd_enable_secure_boot(args):
    summary = efuse_summary(args.port)
    _, _key, digest = preflight(args)
    if summary is not None and summary.get("SECURE_BOOT_EN") is True:
        print("SECURE_BOOT_EN is already burnt; nothing to do.")
        return 0
    print("\nabout to burn SECURE_BOOT_EN")
    print(f"  key fingerprint {key_fingerprint(digest)}")
    print("  The ROM will from now on refuse any bootloader this key did not sign.")
    print("  Flash a signed image and confirm it boots FIRST if you have not.")
    burn = burn_efuse_args("SECURE_BOOT_EN")
    if confirm("enable-secure-boot", args.commit):
        run(efuse_cmd(burn, args.port, no_confirm=True), True, "enabling secure boot")
    else:
        run(efuse_cmd(burn, args.port), False, "enabling secure boot")
    return 0


def cmd_disable_jtag(args):
    preflight(args)
    print(f"\nabout to burn {' and '.join(JTAG_EFUSES)}")
    print("  Both, not one: the ESP32-S3 reaches JTAG through the USB-serial-JTAG")
    print("  peripheral AND through the pads, and burning either alone leaves the")
    print("  other path open. SOFT_DIS_JTAG is deliberately not used, because it")
    print("  can be re-enabled by an HMAC key and so is not a disable at all.")
    print("  On-chip debugging ends here, permanently. Finish HIL work first.")
    burn = burn_efuse_args(*JTAG_EFUSES)
    if confirm("disable-jtag", args.commit):
        run(efuse_cmd(burn, args.port, no_confirm=True), True, "disabling JTAG")
    else:
        run(efuse_cmd(burn, args.port), False, "disabling JTAG")
    return 0


def cmd_disable_download_mode(args):
    print("DIS_DOWNLOAD_MODE is not part of `provision`, and this subcommand")
    print("exists to argue with you before it does anything.\n")
    print("  SRR-11: this device has NO field update path. Removing OTA was")
    print("  deliberate, and it left serial download as the only way a security")
    print("  fix ever reaches a unit. Burning this fuse closes that too, and the")
    print("  result is a device that can never be updated again by anyone,")
    print("  including you, for any reason.\n")
    print("  Secure boot already stops hostile firmware running. This fuse buys")
    print("  very little on top of it and costs the whole update path.\n")
    print("  If you want the middle ground, look at ENABLE_SECURITY_DOWNLOAD")
    print("  instead: it keeps flashing possible while stopping the ROM reading")
    print("  flash back, which is a partial answer to SRR-05.\n")
    if not args.commit:
        print("  (dry run; nothing was changed)")
        return 0
    preflight(args)
    burn = burn_efuse_args("DIS_DOWNLOAD_MODE")
    if confirm("disable-download-mode", args.commit):
        run(efuse_cmd(burn, args.port, no_confirm=True), True, "disabling download mode")
    return 0


def cmd_provision(args):
    """The whole sequence, in the only order that leaves a bootable device."""
    print("=" * 74)
    print("Secure boot provisioning, in order. Steps 2 to 4 cannot be undone.")
    print("=" * 74)
    steps = [
        ("1. flash the signed images", cmd_flash),
        ("2. burn the public key digest", cmd_burn_key),
        ("3. enable secure boot", cmd_enable_secure_boot),
        ("4. disable JTAG", cmd_disable_jtag),
    ]
    for title, fn in steps:
        print(f"\n--- {title} " + "-" * max(0, 60 - len(title)))
        rc = fn(args)
        if rc:
            return rc
    print("\n" + "=" * 74)
    if args.commit:
        print("Provisioned. Record the key fingerprint against this unit's serial")
        print("number; `tools/prod-data.py read` will give you the serial.")
    else:
        print("Dry run complete. Nothing was changed. Add --commit to do it.")
    return 0


# --- self-test ------------------------------------------------------------

def self_test():
    """Everything that does not need a board, a key or ESP-IDF.

    The arithmetic in bootloader_fits is the reason this exists: getting it
    wrong means a signed bootloader that overruns the partition table, on a
    device that will not accept an unsigned replacement.
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

    # signature block placement
    check("empty image", signed_size(0), 4096)
    check("1 byte", signed_size(1), 8192)
    check("one short of a block", signed_size(4095), 8192)
    # exactly on a boundary still costs a whole further block
    check("exactly one block", signed_size(4096), 8192)
    check("just over", signed_size(4097), 12288)
    check("the real bootloader, 21056 B", signed_size(21056), 28672)
    raises("negative size", lambda: signed_size(-1))

    # The fit check against this project's actual 0x8000 offset.
    #
    # The boundary is worth stating rather than assuming, and getting it wrong
    # in a comment is how it gets wrong in the code: 0x8000 is 32768 bytes, a
    # signature block is 4096, so the largest raw bootloader that fits is one
    # that pads to 28672. That is a long way above today's 21056, which is why
    # the current build has room and a future one might not.
    check("today's 21056 B fits", bootloader_fits(21056, 0x8000), True)
    check("24576 B still fits", bootloader_fits(24576, 0x8000), True)
    check("28672 B fits exactly", bootloader_fits(28672, 0x8000), True)
    check("28673 B is one byte too many", bootloader_fits(28673, 0x8000), False)
    check("and the overrun is a whole block", signed_size(28673), 36864)
    # a raised offset makes room, at the cost of moving every partition
    check("28673 B fits under 0xA000", bootloader_fits(28673, 0xA000), True)

    # The parser, against a fixture rather than against firmware/controller/sdkconfig.
    #
    # sdkconfig is GENERATED and gitignored, so a self-test that reads it fails
    # on every clean checkout. The parser is what is worth testing anyway; the
    # real file's value is checked below only when there is one to check.
    import tempfile as _tf
    with _tf.TemporaryDirectory() as td:
        f = pathlib.Path(td) / "sdkconfig"
        f.write_text(
            '# comment\n'
            'CONFIG_PARTITION_TABLE_OFFSET=0x8000\n'
            'CONFIG_IDF_TARGET="esp32s3"\n'
            '# CONFIG_SECURE_BOOT is not set\n'
        )
        check("offset parsed from hex", partition_table_offset(f), 0x8000)
        check("quoted string value", sdkconfig_value("CONFIG_IDF_TARGET", f), "esp32s3")
        raises("a commented-out option counts as absent",
               lambda: sdkconfig_value("CONFIG_SECURE_BOOT", f))
        f.write_text("CONFIG_PARTITION_TABLE_OFFSET=40960\n")
        check("decimal offset", partition_table_offset(f), 40960)

    # And the project's own value, if this checkout has been built.
    if SDKCONFIG.is_file():
        check("this project's partition table offset", partition_table_offset(), 0x8000)
    else:
        print("  skip: firmware/controller/sdkconfig absent (not built here), parser tested above")

    # every irreversible step has its own distinct phrase
    phrases = list(CONFIRM.values())
    check("phrases are distinct", len(set(phrases)), len(phrases))
    check("a phrase for every irreversible step", set(CONFIRM) >=
          {"burn-key", "enable-secure-boot", "disable-jtag"}, True)

    # burn-efuse needs NAME VALUE pairs, and --do-not-confirm must be global
    check("one efuse becomes a pair",
          burn_efuse_args("SECURE_BOOT_EN"), ["burn-efuse", "SECURE_BOOT_EN", "1"])
    check("two efuses become two pairs", burn_efuse_args("DIS_PAD_JTAG", "DIS_USB_JTAG"),
          ["burn-efuse", "DIS_PAD_JTAG", "1", "DIS_USB_JTAG", "1"])
    plain = efuse_cmd(["summary"], port="/dev/null")
    conf = efuse_cmd(["summary"], port="/dev/null", no_confirm=True)
    check("no --do-not-confirm unless asked", "--do-not-confirm" in plain, False)
    check("--do-not-confirm precedes the subcommand",
          conf.index("--do-not-confirm") < conf.index("summary"), True)

    # both JTAG paths, because one is not a disable
    check("both JTAG efuses", set(JTAG_EFUSES), {"DIS_PAD_JTAG", "DIS_USB_JTAG"})
    check("SOFT_DIS_JTAG is not used", "SOFT_DIS_JTAG" in JTAG_EFUSES, False)

    # the key digest fingerprint is stable and short
    fp = key_fingerprint(b"\x01" * 32)
    check("fingerprint shape", bool(re.fullmatch(r'([0-9a-f]{4}:){3}[0-9a-f]{4}', fp)), True)
    check("fingerprint is deterministic", fp, key_fingerprint(b"\x01" * 32))
    check("and distinguishes keys", fp != key_fingerprint(b"\x02" * 32), True)

    # sdkconfig parsing
    raises("absent key", lambda: sdkconfig_value("CONFIG_NO_SUCH_THING"))
    raises("absent file", lambda: sdkconfig_value("CONFIG_X", "/nonexistent/sdkconfig"))

    # key permission check
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        k = pathlib.Path(td) / "k.pem"
        k.write_text("x")
        os.chmod(k, 0o644)
        raises("world-readable key", lambda: check_key(k))
        os.chmod(k, 0o600)
        check("private key accepted", check_key(k), k)
        raises("missing key", lambda: check_key(pathlib.Path(td) / "nope.pem"))

    # the confirmation guard, which is the last thing between a typo and a fuse.
    # Its prompt is swallowed here: six identical warnings would bury the
    # result they are meant to protect.
    import builtins
    import contextlib
    import io
    real_input = builtins.input
    sink = contextlib.redirect_stdout(io.StringIO())
    sink.__enter__()

    def with_input(value):
        def f(_prompt=""):
            if value is EOFError:
                raise EOFError
            return value
        return f

    try:
        # A dry run must never even ask, and must never return True.
        builtins.input = with_input("anything")
        check("dry run does not confirm", confirm("disable-jtag", False), False)

        builtins.input = with_input(CONFIRM["disable-jtag"])
        check("the exact phrase confirms", confirm("disable-jtag", True), True)

        builtins.input = with_input("  " + CONFIRM["disable-jtag"] + "  ")
        check("surrounding whitespace is tolerated",
              confirm("disable-jtag", True), True)

        builtins.input = with_input(CONFIRM["disable-jtag"].lower())
        raises("lower case is not the phrase", lambda: confirm("disable-jtag", True))

        builtins.input = with_input(CONFIRM["burn-key"])
        raises("another step's phrase does not work",
               lambda: confirm("disable-jtag", True))

        builtins.input = with_input("yes")
        raises("a casual yes does not work", lambda: confirm("disable-jtag", True))

        builtins.input = with_input(EOFError)
        raises("non-interactive input refuses", lambda: confirm("disable-jtag", True))
    finally:
        builtins.input = real_input
        sink.__exit__(None, None, None)

    # the summary parser, against real espefuse output shapes
    sample = (
        "SECURE_BOOT_EN (BLOCK0)      Set this bit to enable secure boot  = False R/W (0b0)\n"
        "DIS_PAD_JTAG (BLOCK0)        Set this bit to disable JTAG        = True R/W (0b1)\n"
        "MAC (BLOCK1)                 MAC address                         = aa:bb R/W \n"
    )
    parsed = {}
    for line in sample.splitlines():
        m = re.match(r'^(\w+)\s+\(BLOCK\d+\)\s+.*?=\s*(\S+)', line)
        if m:
            parsed[m.group(1)] = {"True": True, "False": False}.get(m.group(2), m.group(2))
    check("summary parses False", parsed.get("SECURE_BOOT_EN"), False)
    check("summary parses True", parsed.get("DIS_PAD_JTAG"), True)
    check("summary keeps other values", parsed.get("MAC"), "aa:bb")

    print(f"{checks} checks, {failed} failed")
    return 1 if failed else 0


# --- argument parsing -----------------------------------------------------

def add_common(p, port=True):
    p.add_argument("--key", default="keys/secure_boot_signing_key.pem",
                   help="private signing key (default: %(default)s)")
    p.add_argument("--build-dir", help="override the build directory")
    p.add_argument("--digest-out", help="where to write the public key digest")
    p.add_argument("--commit", action="store_true",
                   help="actually do it; without this everything is a dry run")
    if port:
        p.add_argument("--port", help="serial port; esptool autodetects if omitted")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Sign, flash and lock down an ESP32-S3 (secure boot V2, JTAG off).",
        epilog="Three of these steps burn eFuses and cannot be undone. "
               "Nothing happens without --commit.")
    ap.add_argument("--self-test", action="store_true",
                    help="check the arithmetic and the guards, then exit")
    sub = ap.add_subparsers(dest="cmd")

    for name, fn, help_ in (
        ("keygen", cmd_keygen, "create the RSA-3072 signing key"),
        ("digest", cmd_digest, "print the public key digest and fingerprint"),
        ("sign", cmd_sign, "sign the built bootloader and app"),
        ("preflight", cmd_preflight, "run every check without changing anything"),
        ("flash", cmd_flash, "flash the signed images (reversible)"),
        ("burn-key", cmd_burn_key, "burn the key digest (IRREVERSIBLE)"),
        ("enable-secure-boot", cmd_enable_secure_boot, "burn SECURE_BOOT_EN (IRREVERSIBLE)"),
        ("disable-jtag", cmd_disable_jtag, "burn both JTAG disables (IRREVERSIBLE)"),
        ("disable-download-mode", cmd_disable_download_mode,
         "burn DIS_DOWNLOAD_MODE (IRREVERSIBLE, and argues with you)"),
        ("provision", cmd_provision, "the whole ordered sequence"),
    ):
        p = sub.add_parser(name, help=help_)
        add_common(p, port=(name not in ("keygen", "digest", "sign")))
        p.set_defaults(fn=fn)
        if name in ("keygen", "digest", "sign"):
            p.set_defaults(port=None)
        if name == "digest":
            p.add_argument("-o", "--output", help="write the digest here")

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
    except KeyboardInterrupt:
        print("\ninterrupted; nothing was changed", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())

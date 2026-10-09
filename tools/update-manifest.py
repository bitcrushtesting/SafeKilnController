#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The update manifest, and the key that signs it (SWR-UPD-09 to SWR-UPD-16, SEC-12).
#
#   tools/update-manifest.py keygen  --key keys/manifest.pem
#   tools/update-manifest.py pubkey  --key keys/manifest.pem --header firmware/controller/main/update_pubkey.h
#   tools/update-manifest.py sign    --key keys/manifest.pem --image build-esp32s3/safekiln.bin \
#                                    --version 1.4.2 --security --advisory advisories/2026-002.html
#   tools/update-manifest.py verify  --manifest dist/update/stable.json --pubkey keys/manifest.pub.pem
#   tools/update-manifest.py show    --manifest dist/update/stable.json
#   tools/update-manifest.py --self-test
#
# ===========================================================================
# WHAT THIS TOOL IS FOR
# ===========================================================================
# The device fetches one static file per channel, verifies its signature against
# a key compiled into its own firmware, and only then believes a word of it
# (SWR-UPD-11).  This tool makes that file.  Nothing here touches a board: the
# output is a manifest and an image to put on a web server.
#
# The direction of the update path is the whole design and is argued in
# docs/security.md section 6.2.  Two of its consequences land in this file:
#
#   THE MANIFEST IS STATIC AND IDENTICAL FOR EVERY DEVICE (SWR-UPD-10).  There is
#   no per-device URL, no query string and no installed version in the request,
#   so the comparison against the running version happens on the device.  That is
#   what keeps the daily check out of the telemetry category (SEC-06), and it is a
#   constraint on the service as much as on the client: the moment a manifest is
#   generated per unit, the check becomes a tracker.
#
#   EVERY PATH IN IT IS RELATIVE (to the manifest's own directory).  `update.url`
#   is configurable so that a site can mirror the service rather than let the
#   controller out to the internet (SRR-13), and a mirror is then a directory
#   copy.  An absolute URL in the manifest would send a mirrored device back to
#   the origin and defeat the point.
#
# ---------------------------------------------------------------------------
# THE SIGNING KEY IS NOT THE SECURE BOOT KEY
# ---------------------------------------------------------------------------
# Two keys, two blast radii (SRR-12).  The secure boot key decides what a
# provisioned board will BOOT and its digest is burnt into eFuses; this key
# decides what a board will be OFFERED.  Signing manifests with the secure boot
# key would mean the key that can never be rotated is also the key used weekly.
# `keygen` and `sign` both refuse a key that is the configured secure boot key.
#
# Keep it offline.  An attacker who holds it still has to get somebody to press
# confirm at the kiln (SWR-UPD-12), which is the whole reason the install is
# manual, but that is the last line and not a reason to be relaxed about the
# first.
#
# ---------------------------------------------------------------------------
# THE FORMAT, AND WHY IT IS AN ENVELOPE
# ---------------------------------------------------------------------------
# A manifest is a JSON envelope whose `payload` is the base64 of the exact bytes
# that were signed:
#
#   {
#     "alg": "RSA3072-PSS-SHA256",
#     "key_id": "3f2a91c4",
#     "payload": "eyJzY2hlbWEiOiAxLCAicHJvZHVjdCI6ICJzYWZla2lsbiIsIC4uLg==",
#     "sig": "Base64 of 384 bytes"
#   }
#
# Signing the bytes rather than the object is the point.  A device that re-
# serialised the parsed JSON to check a signature would have to agree with this
# tool about key order, spacing and number formatting for ever; the usual way
# that goes wrong is a verifier that passes on the manifests it was tested with
# and fails on the one that matters.  Here the device base64-decodes, verifies
# over those bytes, and parses afterwards.  Canonicalisation stops being a
# problem anybody has to have an opinion about.
#
# The payload, once decoded:
#
#   schema    1, so a future format change is detectable rather than confusing
#   product   "safekiln"
#   channel   "stable" (the file is <channel>.json)
#   target    "esp32s3", cross-checked against the image's own chip id
#   version   a RELEASE semantic version: 1.4.2, never 1.4.2+7.gabc1234.dirty
#   released  ISO date, for the operator and the advisory, not for logic
#   security  true for a security release (SWR-UPD-16), which then needs an advisory
#   advisory  relative path to the advisory, required when security is true
#   image     relative path to the image, resolved against the manifest's directory
#   size      bytes, so the device can refuse an image that does not match
#   sha256    of the image, checked before it is marked bootable (SWR-UPD-11)
#
# What is deliberately NOT in it: release notes.  The display is 128x64 pixels
# (SWR-HMI-16 shows a version and whether it is a security release), and a field
# that only a browser can usefully render would end up being the reason somebody
# adds a browser.  The advisory is a URL for a human on another device.
#
# ---------------------------------------------------------------------------
# WHY openssl AND NOT A PYTHON CRYPTO LIBRARY
# ---------------------------------------------------------------------------
# Every tool in this directory is standard library plus a subprocess to a tool
# that is already on the machine, because UR-CON-04 forbids fetching a dependency
# to build or release this project.  `cryptography` would be a pip install in a
# release path; openssl is on every developer machine and every CI runner.  The
# self-test is stdlib only and runs the sign/verify round trip as well when
# openssl is present, so the arithmetic and the parsing are checked even where
# the crypto cannot be.
import argparse
import base64
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware" / "controller"
SDKCONFIG_SECURE = FIRMWARE / "sdkconfig.secure"

SCHEMA = 1
PRODUCT = "safekiln"
DEFAULT_CHANNEL = "stable"
DEFAULT_TARGET = "esp32s3"
ALG = "RSA3072-PSS-SHA256"

# RSA-3072 to match the scheme the ESP32-S3 ROM verifies for secure boot, so the
# release process has one key size and one hash to think about rather than two.
# PSS rather than PKCS#1 v1.5 for the same reason Secure Boot V2 uses it.
KEY_BITS = 3072
SIG_BYTES = KEY_BITS // 8          # 384

# Signature block of Secure Boot V2: 4096 bytes at the next 4 KiB boundary after
# the image, beginning with this magic byte.  Both numbers and the magic are the
# ESP32-S3's, and tools/secure-boot.py works from the same ones.  The check here
# is structural: it says a block is present, not that it verifies.  `espsecure
# signature-info-v2` is the authority on the latter and is used when available.
SIG_BLOCK_BYTES = 4096
SIG_ALIGN = 4096
SIG_BLOCK_MAGIC = 0xE7

# esp_image_header_t: magic at 0, chip id little-endian at 12.  Read from the
# IDF header layout rather than recalled, because a wrong offset here would pass
# a manifest naming an image for another chip, which is exactly the mistake
# SWR-UPD-03 asks the device to refuse.
IMAGE_MAGIC = 0xE9
IMAGE_CHIP_ID_OFFSET = 12
CHIP_IDS = {
    "esp32": 0x0000,
    "esp32s2": 0x0002,
    "esp32c3": 0x0005,
    "esp32s3": 0x0009,
}

# A release version and nothing else.  SWR-UPD-06 allows build metadata on a
# build that is not on a tag ("1.4.2+7.gabc1234.dirty"), and SWR-UPD-13 compares
# versions to decide whether to install; publishing a version with build
# metadata would put a thing that is not a release into a channel.
RELEASE_VERSION = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")

CHANNEL_NAME = re.compile(r"^[a-z][a-z0-9-]{0,15}$")


class Error(Exception):
    """A problem worth a clean message rather than a traceback."""


# --- small helpers --------------------------------------------------------

def run_capture(cmd, stdin_bytes=None):
    """Run a command and return stdout, raising Error on failure."""
    try:
        p = subprocess.run(cmd, input=stdin_bytes, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, check=False)
    except FileNotFoundError:
        raise Error(f"{cmd[0]} not found") from None
    if p.returncode != 0:
        msg = p.stderr.decode(errors="replace").strip() or f"exit {p.returncode}"
        raise Error(f"{' '.join(cmd[:3])}...: {msg}")
    return p.stdout


def have(prog):
    return shutil.which(prog) is not None


def require_openssl():
    if not have("openssl"):
        raise Error(
            "openssl not found. This tool shells out to it rather than adding a "
            "Python crypto dependency (UR-CON-04); install openssl, or sign on a "
            "machine that has it."
        )


def b64(data):
    return base64.b64encode(data).decode("ascii")


def unb64(text, what):
    try:
        return base64.b64decode(text, validate=True)
    except Exception:
        raise Error(f"{what} is not valid base64") from None


# --- version comparison, the device's rule --------------------------------

def parse_release_version(text):
    """(major, minor, patch) for a release version, or Error.

    Rejects build metadata on purpose: see RELEASE_VERSION.
    """
    if not isinstance(text, str):
        raise Error("version is not a string")
    m = RELEASE_VERSION.match(text)
    if m is None:
        raise Error(
            f"{text!r} is not a release version. Expected major.minor.patch with "
            "no build metadata and no leading zeros; a build off a tag carries "
            "'+count.rev' and is not a release (SWR-UPD-06)."
        )
    return tuple(int(g) for g in m.groups())


def is_newer(offered, running):
    """SWR-UPD-13: strictly greater, or the device refuses it.

    The running version may carry build metadata, because a device can be
    running a build that was never a release; the comparison ignores it, so
    1.4.2+7.gabc is treated as 1.4.2 and is NOT upgraded to 1.4.2.  That is the
    conservative direction: it refuses rather than reinstalls.
    """
    base = running.split("+", 1)[0]
    return parse_release_version(offered) > parse_release_version(base)


# --- the image ------------------------------------------------------------

def image_chip_id(data):
    if len(data) <= IMAGE_CHIP_ID_OFFSET + 1:
        raise Error("image is too short to hold an ESP image header")
    if data[0] != IMAGE_MAGIC:
        raise Error(
            f"image does not start with the ESP image magic {IMAGE_MAGIC:#04x}; "
            "is it an application binary?"
        )
    return int.from_bytes(data[IMAGE_CHIP_ID_OFFSET:IMAGE_CHIP_ID_OFFSET + 2], "little")


def check_image_target(data, target):
    want = CHIP_IDS.get(target)
    if want is None:
        raise Error(f"unknown target {target!r}; known: {', '.join(sorted(CHIP_IDS))}")
    got = image_chip_id(data)
    if got != want:
        names = {v: k for k, v in CHIP_IDS.items()}
        raise Error(
            f"image is for chip id {got:#06x} ({names.get(got, 'unknown')}), "
            f"not {target} ({want:#06x}). Publishing it would offer every device "
            "an image it must refuse."
        )


def sig_block_offset(raw_size):
    """Where Secure Boot V2's appended block starts for an image of this size.

    An image that already ends on a boundary still has the block at the next
    one, which is the arithmetic tools/secure-boot.py::signed_size encodes from
    the other direction.
    """
    if raw_size < 0:
        raise Error("negative image size")
    return (raw_size + SIG_ALIGN - 1) // SIG_ALIGN * SIG_ALIGN


def image_has_sig_block(data):
    """Whether a signature block looks present at the aligned offset.

    Structural only.  A signed image is `payload | padding | 4096-byte block`,
    so the block is the last SIG_BLOCK_BYTES and starts on an alignment boundary
    with its magic byte.  This catches the common mistake, publishing an
    unsigned build, and does not pretend to be verification.
    """
    if len(data) < SIG_BLOCK_BYTES * 2:
        return False
    if len(data) % SIG_ALIGN != 0:
        return False
    block = data[-SIG_BLOCK_BYTES:]
    return block[0] == SIG_BLOCK_MAGIC and any(block)


def espsecure_says_signed(path):
    """None when espsecure is unavailable, else True/False."""
    if not have("espsecure.py") and not have("espsecure"):
        return None
    prog = "espsecure.py" if have("espsecure.py") else "espsecure"
    try:
        out = run_capture([prog, "signature-info-v2", str(path)]).decode(errors="replace")
    except Error:
        return False
    return "ignature block" in out


# --- relative paths -------------------------------------------------------

def check_relative(value, what):
    """Every path in a manifest is relative to the manifest's own directory."""
    if not isinstance(value, str) or not value:
        raise Error(f"{what} is missing")
    if "://" in value or value.startswith("//"):
        raise Error(
            f"{what} is an absolute URL. Manifest paths are relative so that a "
            "mirror is a directory copy and `update.url` works (SRR-13)."
        )
    if value.startswith("/"):
        raise Error(f"{what} must not start with '/'; it is relative to the manifest")
    parts = pathlib.PurePosixPath(value).parts
    if ".." in parts:
        raise Error(f"{what} must not contain '..'")
    if value != pathlib.PurePosixPath(value).as_posix():
        raise Error(f"{what} is not a clean POSIX relative path")
    return value


# --- keys -----------------------------------------------------------------

def secure_boot_key_path():
    """The secure boot signing key this project configures, if any.

    Read from sdkconfig.secure rather than assumed, so that moving the key moves
    this check with it.
    """
    if not SDKCONFIG_SECURE.is_file():
        return None
    m = re.search(r'^CONFIG_SECURE_BOOT_SIGNING_KEY="(.+)"$',
                  SDKCONFIG_SECURE.read_text(), re.M)
    if m is None:
        return None
    return (FIRMWARE / m.group(1)).resolve()


def refuse_secure_boot_key(key):
    """SRR-12: two keys, two blast radii."""
    sb = secure_boot_key_path()
    key = pathlib.Path(key).resolve()
    if sb is not None and key == sb:
        raise Error(
            f"{key} is this project's SECURE BOOT signing key.\n"
            "The manifest key must be a different key: the secure boot key "
            "decides what a provisioned board will boot and its digest is burnt "
            "into eFuses, so it can never be rotated, while this one is used on "
            "every release. See SRR-12 and docs/security.md section 6.2."
        )
    if sb is not None and sb.is_file() and key.is_file():
        if sb.read_bytes() == key.read_bytes():
            raise Error(
                f"{key} is byte-identical to the secure boot key at {sb}, which "
                "is the same mistake under another name (SRR-12)."
            )


def public_der(key=None, pubkey=None):
    """DER SubjectPublicKeyInfo of the public key, from either half of the pair.

    DER because that is what mbedtls_pk_parse_public_key wants on the device; a
    PEM would mean shipping a base64 decoder to read a key.
    """
    require_openssl()
    if key is not None:
        return run_capture(["openssl", "rsa", "-in", str(key), "-pubout",
                            "-outform", "DER"])
    if pubkey is not None:
        return run_capture(["openssl", "rsa", "-pubin", "-in", str(pubkey),
                            "-pubout", "-outform", "DER"])
    raise Error("need --key or --pubkey")


def key_id(der):
    """First four bytes of the SHA-256 of the DER public key, as hex.

    Short on purpose: it exists so a device can say "this manifest was signed
    with a key I do not have" before attempting a verification, and so a key
    rotation is visible in a file somebody is reading.  It is not a security
    property; the signature is.
    """
    return hashlib.sha256(der).hexdigest()[:8]


# --- sign and verify ------------------------------------------------------

def sign_bytes(payload, key):
    require_openssl()
    with tempfile.TemporaryDirectory() as tmp:
        pf = pathlib.Path(tmp) / "payload"
        sf = pathlib.Path(tmp) / "sig"
        pf.write_bytes(payload)
        run_capture(["openssl", "dgst", "-sha256",
                     "-sigopt", "rsa_padding_mode:pss",
                     "-sigopt", "rsa_pss_saltlen:digest",
                     "-sign", str(key), "-out", str(sf), str(pf)])
        sig = sf.read_bytes()
    if len(sig) != SIG_BYTES:
        raise Error(
            f"signature is {len(sig)} bytes, expected {SIG_BYTES}: is {key} an "
            f"RSA-{KEY_BITS} key?"
        )
    return sig


def verify_bytes(payload, sig, key=None, pubkey=None):
    require_openssl()
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        (d / "payload").write_bytes(payload)
        (d / "sig").write_bytes(sig)
        pub = d / "pub.pem"
        if pubkey is not None:
            pub = pathlib.Path(pubkey)
        else:
            if key is None:
                raise Error("need --key or --pubkey")
            pub.write_bytes(run_capture(["openssl", "rsa", "-in", str(key),
                                         "-pubout"]))
        try:
            run_capture(["openssl", "dgst", "-sha256",
                         "-sigopt", "rsa_padding_mode:pss",
                         "-sigopt", "rsa_pss_saltlen:digest",
                         "-verify", str(pub), "-signature", str(d / "sig"),
                         str(d / "payload")])
        except Error:
            return False
    return True


# --- the payload and the envelope ----------------------------------------

def build_payload(version, channel, target, image_rel, size, digest,
                  security, advisory, released):
    """The object that gets signed, validated before it is.

    Validation lives here rather than at the argument parser so that `verify`
    applies exactly the same rules to a manifest somebody else produced.
    """
    parse_release_version(version)
    if not CHANNEL_NAME.match(channel or ""):
        raise Error(f"channel {channel!r} must be lowercase letters, digits and dashes")
    if target not in CHIP_IDS:
        raise Error(f"unknown target {target!r}")
    check_relative(image_rel, "image")
    if size <= 0:
        raise Error("size must be positive")
    if not re.fullmatch(r"[0-9a-f]{64}", digest or ""):
        raise Error("sha256 must be 64 lowercase hex characters")
    if security and not advisory:
        raise Error(
            "a security release needs an advisory (SWR-UPD-16): the obligation is "
            "to say what was fixed, not only to ship the fix."
        )
    if advisory:
        check_relative(advisory, "advisory")
    try:
        datetime.date.fromisoformat(released)
    except ValueError:
        raise Error(f"released {released!r} is not an ISO date") from None

    payload = {
        "schema": SCHEMA,
        "product": PRODUCT,
        "channel": channel,
        "target": target,
        "version": version,
        "released": released,
        "security": bool(security),
        "image": image_rel,
        "size": int(size),
        "sha256": digest,
    }
    if advisory:
        payload["advisory"] = advisory
    return payload


def payload_bytes(payload):
    """The exact bytes that are signed.

    Sorted keys and a trailing newline: not because the device cares, it verifies
    over whatever these bytes are, but because a human diffing two manifests
    should see the release change and nothing else.
    """
    return (json.dumps(payload, sort_keys=True, indent=2) + "\n").encode()


def envelope(payload, sig, kid):
    return {
        "alg": ALG,
        "key_id": kid,
        "payload": b64(payload_bytes(payload) if isinstance(payload, dict) else payload),
        "sig": b64(sig),
    }


def read_envelope(path):
    p = pathlib.Path(path)
    if not p.is_file():
        raise Error(f"no manifest at {p}")
    try:
        env = json.loads(p.read_text())
    except json.JSONDecodeError as exc:
        raise Error(f"{p} is not valid JSON: {exc}") from None
    if not isinstance(env, dict):
        raise Error(f"{p} is not a JSON object")
    for field in ("alg", "key_id", "payload", "sig"):
        if field not in env:
            raise Error(f"{p} has no {field!r}")
    if env["alg"] != ALG:
        raise Error(f"{p} is {env['alg']!r}, this tool signs {ALG!r}")
    raw = unb64(env["payload"], "payload")
    sig = unb64(env["sig"], "sig")
    if len(sig) != SIG_BYTES:
        raise Error(f"signature is {len(sig)} bytes, expected {SIG_BYTES}")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise Error(f"the signed payload in {p} is not valid JSON: {exc}") from None
    return env, raw, sig, payload


def check_payload_fields(payload):
    """Re-apply build_payload's rules to a payload we did not build."""
    if payload.get("schema") != SCHEMA:
        raise Error(f"schema is {payload.get('schema')!r}, this tool knows {SCHEMA}")
    if payload.get("product") != PRODUCT:
        raise Error(f"product is {payload.get('product')!r}, expected {PRODUCT!r}")
    build_payload(payload.get("version"), payload.get("channel"),
                  payload.get("target"), payload.get("image"),
                  payload.get("size", 0), payload.get("sha256"),
                  payload.get("security", False), payload.get("advisory"),
                  payload.get("released", ""))


# --- subcommands ----------------------------------------------------------

def cmd_keygen(args):
    p = pathlib.Path(args.key)
    if p.exists():
        raise Error(
            f"{p} already exists. Overwriting a manifest signing key orphans "
            "every device that trusts it until they are reflashed locally; move "
            "it aside deliberately if that is what you mean."
        )
    refuse_secure_boot_key(p)
    require_openssl()
    p.parent.mkdir(parents=True, exist_ok=True)
    der = run_capture(["openssl", "genpkey", "-algorithm", "RSA",
                       "-pkeyopt", f"rsa_keygen_bits:{KEY_BITS}",
                       "-outform", "PEM"])
    p.write_bytes(der)
    os.chmod(p, stat.S_IRUSR | stat.S_IWUSR)
    kid = key_id(public_der(key=p))
    print(f"wrote {p} (mode 0600), key_id {kid}")
    print()
    print("Three things to do with it now, and one not to:")
    print(f"  1. Move it OFF this machine. It signs what every unit is offered;")
    print( "     it has no business on a laptop that browses the web (SRR-12).")
    print( "  2. Back it up somewhere you will still have in five years. The")
    print( "     support period outlasts most of the decisions in this project.")
    print(f"  3. Emit the public half into the firmware:")
    print(f"       tools/update-manifest.py pubkey --key {p} --header <path>")
    print( "  Do NOT reuse the secure boot key for this, or this one for that.")


def cmd_pubkey(args):
    if args.key:
        refuse_secure_boot_key(args.key)
    der = public_der(key=args.key, pubkey=args.pubkey)
    kid = key_id(der)
    print(f"key_id {kid}, {len(der)} bytes of DER SubjectPublicKeyInfo")

    if args.pem:
        require_openssl()
        src = ["-in", str(args.key)] if args.key else ["-pubin", "-in", str(args.pubkey)]
        pem = run_capture(["openssl", "rsa", *src, "-pubout"])
        pathlib.Path(args.pem).write_bytes(pem)
        print(f"wrote {args.pem}")

    if args.header:
        lines = []
        for i in range(0, len(der), 12):
            chunk = der[i:i + 12]
            lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
        body = "\n".join(lines)
        text = f"""/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED by tools/update-manifest.py pubkey.  Do not edit.
 *
 * The public half of the update manifest signing key (SWR-UPD-11, SEC-12), as
 * DER SubjectPublicKeyInfo so that mbedtls_pk_parse_public_key can take it
 * directly.  Replacing this key means every device carrying the old one stops
 * believing the service, so a rotation is a release, not a config change.
 *
 * key_id {kid}: the first four bytes of the SHA-256 of these bytes.
 */
#ifndef KILN_UPDATE_PUBKEY_H
#define KILN_UPDATE_PUBKEY_H

#include <stddef.h>
#include <stdint.h>

#define KILN_UPDATE_KEY_ID "{kid}"

static const uint8_t kiln_update_pubkey_der[] = {{
{body}
}};

static const size_t kiln_update_pubkey_der_len = sizeof kiln_update_pubkey_der;

#endif /* KILN_UPDATE_PUBKEY_H */
"""
        out = pathlib.Path(args.header)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"wrote {out}")


def cmd_sign(args):
    refuse_secure_boot_key(args.key)
    image = pathlib.Path(args.image)
    if not image.is_file():
        raise Error(f"no image at {image}")
    data = image.read_bytes()

    check_image_target(data, args.target)

    signed = espsecure_says_signed(image)
    if signed is False or (signed is None and not image_has_sig_block(data)):
        how = "espsecure" if signed is False else "the appended block check"
        if not args.allow_unsigned_image:
            raise Error(
                f"{image} carries no Secure Boot V2 signature ({how}).\n"
                "SWR-UPD-11 has the device verify the image's own signature as "
                "well as the digest in the manifest, so an unsigned image is one "
                "a provisioned unit will refuse after downloading it.\n"
                "Sign it first:\n"
                "  tools/secure-boot.py sign --key keys/prod.pem\n"
                "or pass --allow-unsigned-image if you are deliberately making a "
                "manifest for an unprovisioned test board."
            )
        print(f"WARNING: {image} is not signed ({how}); a provisioned unit will "
              "refuse it.")

    digest = hashlib.sha256(data).hexdigest()
    image_rel = args.image_path or f"images/{PRODUCT}-{args.version}-{args.target}.bin"
    released = args.released or datetime.date.today().isoformat()

    payload = build_payload(args.version, args.channel, args.target, image_rel,
                            len(data), digest, args.security, args.advisory,
                            released)
    raw = payload_bytes(payload)
    sig = sign_bytes(raw, args.key)
    kid = key_id(public_der(key=args.key))
    if not verify_bytes(raw, sig, key=args.key):
        raise Error(
            "the signature this tool just made does not verify against its own "
            "key. Something is wrong with the openssl invocation; do not publish."
        )

    out = pathlib.Path(args.out or (ROOT / "dist" / "update" / f"{args.channel}.json"))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(envelope(payload, sig, kid), indent=2) + "\n")

    print(f"wrote {out}")
    print()
    print(raw.decode().rstrip())
    print()
    print(f"signed with key_id {kid}, verified against its own public half")
    print()
    print("Serve it at this layout, where the manifest's directory is the root")
    print("that every relative path resolves against:")
    print()
    print(f"  https://update.bitcrushtesting.com/{PRODUCT}/{args.channel}.json")
    print(f"  https://update.bitcrushtesting.com/{PRODUCT}/{image_rel}")
    if args.advisory:
        print(f"  https://update.bitcrushtesting.com/{PRODUCT}/{args.advisory}")
    print()
    print(f"Upload the image BEFORE the manifest. A manifest naming an image that")
    print(f"is not there yet makes every device that checks report a failed")
    print(f"update, which is a support call rather than a fix (SWR-UPD-09).")


def cmd_verify(args):
    env, raw, sig, payload = read_envelope(args.manifest)

    # Signature FIRST, fields second, in that order and not the other way round.
    # It is the order SWR-UPD-11 puts on the device, which discards an unverified
    # manifest without looking further, and a tool that checked the fields first
    # would report "bad version" for a forgery and teach whoever read that the
    # wrong thing about what had just happened.
    der = public_der(key=args.key, pubkey=args.pubkey)
    kid = key_id(der)
    if kid != env["key_id"]:
        print(f"key_id mismatch: manifest says {env['key_id']}, this key is {kid}")
    if not verify_bytes(raw, sig, key=args.key, pubkey=args.pubkey):
        raise Error(
            "SIGNATURE DOES NOT VERIFY. A device would discard this manifest "
            "without parsing it further and without telling the operator "
            "anything was offered (SWR-UPD-11)."
        )
    print(f"signature verifies against key_id {kid}")
    check_payload_fields(payload)
    print(raw.decode().rstrip())

    if args.image:
        image = pathlib.Path(args.image)
        if not image.is_file():
            raise Error(f"no image at {image}")
        data = image.read_bytes()
        if len(data) != payload["size"]:
            raise Error(f"size mismatch: manifest {payload['size']}, image {len(data)}")
        got = hashlib.sha256(data).hexdigest()
        if got != payload["sha256"]:
            raise Error(f"sha256 mismatch:\n  manifest {payload['sha256']}\n  image    {got}")
        check_image_target(data, payload["target"])
        print(f"{image} matches: {len(data)} bytes, sha256 and chip id agree")

    if args.running_version:
        verdict = ("would install" if is_newer(payload["version"], args.running_version)
                   else "would be refused")
        print(f"a device running {args.running_version}: {verdict} "
              f"{payload['version']} (SWR-UPD-13)")


def cmd_show(args):
    _, raw, _, payload = read_envelope(args.manifest)
    print(raw.decode().rstrip())
    print()
    print("NOT VERIFIED. `show` decodes and does not check the signature, which "
          "is the one thing that decides whether any of the above is true. Use "
          "`verify --pubkey ...` before believing it.")
    try:
        check_payload_fields(payload)
    except Error as exc:
        print(f"and the payload would be rejected anyway: {exc}")


# --- self-test ------------------------------------------------------------

def self_test():
    """Everything that needs no key and no network.

    The round trip through openssl is included when openssl is present, because
    the failure this tool must never have is producing a manifest whose signature
    does not verify, and the only way to know is to verify one.
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

    # --- versions: the device's acceptance rule ---------------------------
    check("plain release", parse_release_version("1.4.2"), (1, 4, 2))
    check("zeros", parse_release_version("0.0.0"), (0, 0, 0))
    raises("build metadata", lambda: parse_release_version("1.4.2+7.gabc1234"))
    raises("dirty build", lambda: parse_release_version("1.4.2+7.gabc1234.dirty"))
    raises("leading zero", lambda: parse_release_version("1.04.2"))
    raises("two components", lambda: parse_release_version("1.4"))
    raises("v prefix", lambda: parse_release_version("v1.4.2"))
    raises("not a string", lambda: parse_release_version(None))

    check("newer patch", is_newer("1.4.3", "1.4.2"), True)
    check("newer minor", is_newer("1.5.0", "1.4.9"), True)
    check("newer major", is_newer("2.0.0", "1.99.99"), True)
    check("same version", is_newer("1.4.2", "1.4.2"), False)
    check("older", is_newer("1.4.1", "1.4.2"), False)
    # 10 > 9 numerically, and "10" < "9" as text: the comparison a string
    # compare gets wrong, which is why it is tested rather than assumed.
    check("double digits", is_newer("1.10.0", "1.9.0"), True)
    check("not a downgrade to 1.9", is_newer("1.9.0", "1.10.0"), False)
    # A device on a non-release build is not offered the release it was built
    # from: conservative, refuses rather than reinstalls.
    check("running a dirty build of the same version",
          is_newer("1.4.2", "1.4.2+7.gabc1234.dirty"), False)
    check("running a dirty build, newer release offered",
          is_newer("1.4.3", "1.4.2+7.gabc1234.dirty"), True)

    # --- image header -----------------------------------------------------
    def image(chip_id, magic=IMAGE_MAGIC, extra=0):
        hdr = bytearray(32 + extra)
        hdr[0] = magic
        hdr[IMAGE_CHIP_ID_OFFSET:IMAGE_CHIP_ID_OFFSET + 2] = chip_id.to_bytes(2, "little")
        return bytes(hdr)

    check("s3 chip id", image_chip_id(image(0x0009)), 9)
    raises("wrong magic", lambda: image_chip_id(image(0x0009, magic=0x00)))
    raises("too short", lambda: image_chip_id(b"\xe9\x00"))
    check("s3 image for s3", check_image_target(image(0x0009), "esp32s3"), None)
    raises("c3 image for s3", lambda: check_image_target(image(0x0005), "esp32s3"))
    raises("unknown target", lambda: check_image_target(image(0x0009), "esp32h9"))

    # --- signature block placement, the same arithmetic as secure-boot.py -
    check("empty", sig_block_offset(0), 0)
    check("one byte", sig_block_offset(1), 4096)
    check("one short", sig_block_offset(4095), 4096)
    check("exactly one block", sig_block_offset(4096), 4096)
    check("just over", sig_block_offset(4097), 8192)
    raises("negative", lambda: sig_block_offset(-1))

    signed = bytes([IMAGE_MAGIC]) + bytes(4095) + bytes([SIG_BLOCK_MAGIC]) + bytes(4095)
    check("signed-looking image", image_has_sig_block(signed), True)
    check("unpadded image", image_has_sig_block(bytes(5000)), False)
    check("zero block", image_has_sig_block(bytes(8192)), False)
    check("too small to hold a block", image_has_sig_block(bytes(4096)), False)

    # --- relative paths ---------------------------------------------------
    check("plain path", check_relative("images/a.bin", "image"), "images/a.bin")
    raises("absolute URL", lambda: check_relative("https://x/y.bin", "image"))
    raises("scheme relative", lambda: check_relative("//x/y.bin", "image"))
    raises("rooted", lambda: check_relative("/images/a.bin", "image"))
    raises("parent", lambda: check_relative("../a.bin", "image"))
    raises("parent inside", lambda: check_relative("images/../../a.bin", "image"))
    raises("empty", lambda: check_relative("", "image"))

    # --- the payload ------------------------------------------------------
    good = dict(version="1.4.2", channel="stable", target="esp32s3",
                image_rel="images/safekiln-1.4.2-esp32s3.bin", size=1234,
                digest="a" * 64, security=False, advisory=None,
                released="2026-10-09")
    p = build_payload(**good)
    check("payload keys", sorted(p), ["channel", "image", "product", "released",
                                      "schema", "security", "sha256", "size",
                                      "target", "version"])
    check("no advisory when not security", "advisory" in p, False)
    check("security release carries one",
          "advisory" in build_payload(**{**good, "security": True,
                                         "advisory": "advisories/1.html"}), True)
    raises("security without advisory",
           lambda: build_payload(**{**good, "security": True}))
    raises("bad digest", lambda: build_payload(**{**good, "digest": "nope"}))
    raises("uppercase digest", lambda: build_payload(**{**good, "digest": "A" * 64}))
    raises("zero size", lambda: build_payload(**{**good, "size": 0}))
    raises("bad channel", lambda: build_payload(**{**good, "channel": "Stable!"}))
    raises("bad date", lambda: build_payload(**{**good, "released": "09.10.2026"}))

    # Byte-for-byte stability: the payload is what gets signed, so a change in
    # how it serialises is a change in what a device verifies.
    raw = payload_bytes(p)
    check("payload ends with a newline", raw.endswith(b"\n"), True)
    check("payload is sorted", raw.index(b'"channel"') < raw.index(b'"version"'), True)
    check("payload round trips", json.loads(raw), p)
    check("payload is stable", payload_bytes(json.loads(raw)), raw)

    # --- the envelope -----------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        env = envelope(p, bytes(SIG_BYTES), "deadbeef")
        mf = d / "stable.json"
        mf.write_text(json.dumps(env, indent=2) + "\n")
        _, raw2, sig2, pl2 = read_envelope(mf)
        check("envelope round trip, payload", pl2, p)
        check("envelope round trip, bytes", raw2, raw)
        check("envelope round trip, sig length", len(sig2), SIG_BYTES)

        (d / "notjson.json").write_text("{")
        raises("not JSON", lambda: read_envelope(d / "notjson.json"))
        (d / "noalg.json").write_text('{"key_id":"x","payload":"","sig":""}')
        raises("no alg", lambda: read_envelope(d / "noalg.json"))
        (d / "wrongalg.json").write_text(json.dumps({**env, "alg": "ED25519"}))
        raises("wrong alg", lambda: read_envelope(d / "wrongalg.json"))
        (d / "shortsig.json").write_text(json.dumps({**env, "sig": b64(b"\x00" * 8)}))
        raises("short signature", lambda: read_envelope(d / "shortsig.json"))
        (d / "badb64.json").write_text(json.dumps({**env, "payload": "!!!"}))
        raises("payload not base64", lambda: read_envelope(d / "badb64.json"))
        (d / "payloadnotjson.json").write_text(json.dumps({**env, "payload": b64(b"{")}))
        raises("payload not JSON", lambda: read_envelope(d / "payloadnotjson.json"))
        raises("no such manifest", lambda: read_envelope(d / "nothing.json"))

        # A payload we did not build is held to the same rules.
        check("our payload passes the re-check", check_payload_fields(p), None)
        raises("future schema",
               lambda: check_payload_fields({**p, "schema": 2}))
        raises("another product",
               lambda: check_payload_fields({**p, "product": "something"}))
        raises("absolute image URL",
               lambda: check_payload_fields({**p, "image": "https://x/y.bin"}))

    # --- the secure boot key guard ---------------------------------------
    sb = secure_boot_key_path()
    checks += 1
    if sb is None:
        failed += 1
        print("  FAIL secure boot key path: sdkconfig.secure names no key, so the "
              "SRR-12 guard would never fire")
    else:
        raises("signing with the secure boot key", lambda: refuse_secure_boot_key(sb))

    # --- the round trip, where openssl exists ----------------------------
    if not have("openssl"):
        print("  SKIP sign/verify round trip: no openssl on this machine")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            key = d / "test.pem"
            key.write_bytes(run_capture(
                ["openssl", "genpkey", "-algorithm", "RSA",
                 "-pkeyopt", f"rsa_keygen_bits:{KEY_BITS}", "-outform", "PEM"]))
            other = d / "other.pem"
            other.write_bytes(run_capture(
                ["openssl", "genpkey", "-algorithm", "RSA",
                 "-pkeyopt", f"rsa_keygen_bits:{KEY_BITS}", "-outform", "PEM"]))

            sig = sign_bytes(raw, key)
            check("signature length", len(sig), SIG_BYTES)
            check("verifies with its own key", verify_bytes(raw, sig, key=key), True)
            check("fails with another key", verify_bytes(raw, sig, key=other), False)
            check("fails on a tampered payload",
                  verify_bytes(raw[:-2] + b"X\n", sig, key=key), False)
            check("fails on a tampered signature",
                  verify_bytes(raw, sig[:-1] + bytes([sig[-1] ^ 0xFF]), key=key), False)

            der = public_der(key=key)
            check("DER is a SubjectPublicKeyInfo", der[0], 0x30)
            check("key_id is eight hex", len(key_id(der)), 8)
            check("key_id is stable", key_id(der), key_id(public_der(key=key)))
            checks += 1
            if key_id(der) == key_id(public_der(key=other)):
                failed += 1
                print("  FAIL key_id: two different keys share one")

            # PSS is randomised, so two signatures over the same bytes differ
            # and both verify. Worth pinning: a verifier that compared
            # signatures rather than verifying them would pass with v1.5 and
            # fail here, and this is the scheme we have chosen.
            check("PSS is randomised", sign_bytes(raw, key) != sig, True)
            check("and the second one verifies",
                  verify_bytes(raw, sign_bytes(raw, key), key=key), True)

    print(f"{checks - failed}/{checks} checks passed")
    return 1 if failed else 0


# --- main -----------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Build and sign the update manifest (SWR-UPD-09 to SWR-UPD-16).",
        epilog="The signing key is NOT the secure boot key, and belongs offline "
               "(SRR-12). docs/security.md section 6.2 has the reasoning.",
    )
    ap.add_argument("--self-test", action="store_true",
                    help="run the checks that need no key, no board and no network")
    sub = ap.add_subparsers(dest="cmd")

    k = sub.add_parser("keygen", help="generate the manifest signing key")
    k.add_argument("--key", required=True)
    k.set_defaults(fn=cmd_keygen)

    pk = sub.add_parser("pubkey", help="print the key id; emit the public half")
    pk.add_argument("--key", help="the private key")
    pk.add_argument("--pubkey", help="or the public key, if that is all you have")
    pk.add_argument("--header", help="write a C header for the firmware")
    pk.add_argument("--pem", help="write the public key as PEM")
    pk.set_defaults(fn=cmd_pubkey)

    s = sub.add_parser("sign", help="build and sign a manifest for an image")
    s.add_argument("--key", required=True)
    s.add_argument("--image", required=True, help="the signed application binary")
    s.add_argument("--version", required=True, help="release version, e.g. 1.4.2")
    s.add_argument("--channel", default=DEFAULT_CHANNEL)
    s.add_argument("--target", default=DEFAULT_TARGET)
    s.add_argument("--security", action="store_true",
                   help="mark it a security release (SWR-UPD-16); needs --advisory")
    s.add_argument("--advisory", help="relative path to the advisory")
    s.add_argument("--released", help="ISO date, default today")
    s.add_argument("--image-path", help="relative path the manifest names, "
                                        "default images/<product>-<version>-<target>.bin")
    s.add_argument("--out", help="default dist/update/<channel>.json")
    s.add_argument("--allow-unsigned-image", action="store_true",
                   help="publish an image with no Secure Boot V2 signature, for a "
                        "test board that was never provisioned")
    s.set_defaults(fn=cmd_sign)

    v = sub.add_parser("verify", help="verify a manifest, and optionally its image")
    v.add_argument("--manifest", required=True)
    v.add_argument("--key", help="the private key")
    v.add_argument("--pubkey", help="or the public key, which is what a device has")
    v.add_argument("--image", help="check size, digest and chip id against it")
    v.add_argument("--running-version",
                   help="report whether a device on this version would install it")
    v.set_defaults(fn=cmd_verify)

    sh = sub.add_parser("show", help="decode a manifest WITHOUT verifying it")
    sh.add_argument("--manifest", required=True)
    sh.set_defaults(fn=cmd_show)

    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    try:
        args.fn(args)
    except Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

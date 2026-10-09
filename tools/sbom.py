#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The software bill of materials (SWR-NFR-28, UR-REG-004, SRR-14).
#
#   tools/sbom.py                                     # CycloneDX, from the default build dir
#   tools/sbom.py --format both --strict
#   tools/sbom.py --build-dir firmware/controller/build \
#                 --image firmware/controller/build/safekiln.bin \
#                 --out-dir dist --format both
#   tools/sbom.py --self-test
#
# ===========================================================================
# WHY THIS EXISTS, AND WHAT IT HAS TO BE RIGHT ABOUT
# ===========================================================================
# The Cyber Resilience Act requires a machine-readable bill of materials
# covering at least the top-level dependencies, kept for the support period.
# The question it has to answer, under time pressure, is:
#
#     "An advisory landed this morning against mbedTLS 3.6.0.  Did we ship it?"
#
# A project that cannot answer that has to assume the worst about every unit it
# ever sold.  So the one property that matters here is that the inventory is
# GENERATED FROM THE BUILD and not maintained by hand: a hand-written list of
# dependencies is wrong within one release of being written, and wrong in the
# direction that costs the most, which is omission.
#
# The source of truth is `build/project_description.json`, which the IDF build
# writes and which names the components that were ACTUALLY LINKED rather than
# the ones the project might use.  Nothing in this tool guesses at that list.
#
# ---------------------------------------------------------------------------
# CYCLONEDX AND SPDX, AND WHY BOTH
# ---------------------------------------------------------------------------
# CycloneDX 1.6 JSON is the default: its model fits a firmware image (a product
# component with a dependency graph and file-level detail where that helps), and
# it is what vulnerability tooling consumes most readily.  SPDX 2.3 JSON is
# available with `--format spdx` or `both`, because an assessor or a customer may
# ask for SPDX specifically and "we have the other one" is a bad answer to a
# question with a deadline attached.
#
# Both are emitted from one inventory, so they cannot disagree about what
# shipped.  They differ in exactly one respect, deliberately: the web assets
# carry per-file hashes in the CycloneDX output, where nested components are part
# of the model, and stay a single package in the SPDX output, where file-level
# listing would pull in `filesAnalyzed` and a package verification code for no
# regulatory gain.  That asymmetry is documented rather than silent.
#
# ---------------------------------------------------------------------------
# LICENCES ARE SCANNED, NOT ASSUMED
# ---------------------------------------------------------------------------
# ESP-IDF publishes no per-component licence manifest, and a curated map in this
# file would be the hand-maintained inventory this tool exists to avoid.  So the
# licence of each component is taken from the `SPDX-License-Identifier:` tags in
# its own sources, which is the vendor's own statement about its own code, in the
# same spirit as tools/gen-stm32g031-header.py taking register addresses from
# ST's SVD rather than from anybody's recollection.
#
# Two honest limits on that:
#
#   A SCAN IS EVIDENCE, NOT A LEGAL CONCLUSION.  It reports what the files say.
#   A component whose sources carry no tag comes out as NOASSERTION rather than
#   as a guess, and `--strict` makes that a failure so a release cannot quietly
#   ship an unknown.
#
#   IT SEES ONLY WHAT IS ON DISK.  Run it where $IDF_PATH is, which for CI means
#   inside the IDF container, not in a step afterwards.  Without the IDF tree it
#   still produces a valid SBOM, with the component list intact and the licences
#   unresolved, and says so loudly.
#
# ---------------------------------------------------------------------------
# WHAT IS FIRST-PARTY, WHICH IS MORE THAN YOU WOULD EXPECT
# ---------------------------------------------------------------------------
# There are no vendored third-party browser assets.  `web/chart.js` is
# hand-written for exactly this reason (OQ-04: "a dependency would cost more than
# the entire asset budget and bring a licence to audit with it"), and the SBOM
# records that as a fact with hashes behind it rather than as an absence.  An
# assessor asking "what JavaScript libraries does this ship" gets "none, and
# here are the four files it does ship".
#
# ---------------------------------------------------------------------------
# REPRODUCIBILITY
# ---------------------------------------------------------------------------
# Same build, same SBOM bytes.  Components are sorted, the serial number is
# derived from a digest of the inventory rather than randomly generated, and the
# timestamp honours SOURCE_DATE_EPOCH.  That matters because the SBOM is a
# release artefact kept for five years: a diff between two of them should show
# what changed in the product, not what time it was.
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware" / "controller"
DEFAULT_BUILD_DIRS = ("build", "build-esp32s3", "build-hw", "build-qemu")

SUPPLIER = "Bitcrush Testing"
PROJECT_LICENCE = "GPL-3.0-or-later"
PROJECT_URL = "https://github.com/Boernsman/kilncontrol"

TOOL_NAME = "tools/sbom.py"
TOOL_VERSION = "1"

CYCLONEDX_SPEC = "1.6"
SPDX_VERSION = "SPDX-2.3"
NOASSERTION = "NOASSERTION"

# The release tag glob and the version rules of firmware/controller/CMakeLists.txt.
# Reimplemented here rather than parsed out of the image, because the SBOM has to
# name a version even when no image is given, and because `--match` is not
# optional: this repository carries a non-release tag that a bare
# `git describe --tags` would happily name a release after.
TAG_GLOB = "v[0-9]*.[0-9]*.[0-9]*"

# Files worth opening when looking for an SPDX tag, and the caps that keep a scan
# of a large component tree to a fraction of a second.  A tag lives in a header
# comment, so the first 2 KiB is where it is or is not.
SCAN_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".S", ".s", ".py",
                 ".ld", ".cmake", ".txt", ".yml", ".yaml", ".json", ".rst", ".md"}
SCAN_MAX_FILES = 400
SCAN_HEAD_BYTES = 2048
SCAN_SKIP_DIRS = {".git", "test", "tests", "examples", "docs", "doc", "build"}

# Test and example code does not ship, and in ESP-IDF it is licensed
# differently from the component it sits inside: the component is Apache-2.0 and
# its `test_apps/` carry CC0-1.0 or "Unlicense OR CC0-1.0", which is Espressif's
# convention for code they want people to copy freely.  Scanning those in made
# every second component look dual-licensed in a way no shipped byte is.  Found
# by running this against the real v6.0.1 tree rather than by reading about it,
# which is why the pattern is this specific: `test_apps`, `i2c_test_apps`,
# `heap_tests`, `pthread_unity_tests`, `host_test`.
SCAN_SKIP_DIR_RE = re.compile(r"(^|_)(test_apps?|tests?|unity_tests|host_test)$")

SPDX_TAG = re.compile(rb"SPDX-License-Identifier:\s*([^\r\n*/]+)")

WEB_SUFFIXES = {".html", ".js", ".css"}

# Where a licence lives when it is not in an SPDX tag.  Four of ESP-IDF v6.0.1's
# own components are in that position (cmock, esp_netif_stack, http_parser,
# protobuf-c), three of them with a licence FILE and no tags, often one
# directory down inside the vendored submodule: `CMock/LICENSE.txt`,
# `protobuf-c/LICENSE`.  Finding those does not let this tool assert a licence,
# and it deliberately does not try: reading licence prose and concluding an SPDX
# id is the kind of guess this file exists to avoid.  What it does is say where
# the answer is, so the person resolving the five has somewhere to start.
LICENCE_FILE_RE = re.compile(r"^(licen[sc]e|copying|notice)", re.I)
LICENCE_FILE_DEPTH = 3
LICENCE_FILE_MAX = 4


class Error(Exception):
    """A problem worth a clean message rather than a traceback."""


# --- small helpers --------------------------------------------------------

def run_capture(cmd, cwd=None):
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, check=False)
    except (FileNotFoundError, NotADirectoryError):
        return None
    if p.returncode != 0:
        return None
    return p.stdout.decode(errors="replace").strip()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def timestamp():
    """UTC, to the second, honouring SOURCE_DATE_EPOCH."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            when = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
        except ValueError:
            raise Error(f"SOURCE_DATE_EPOCH={epoch!r} is not an integer") from None
    else:
        when = datetime.datetime.now(datetime.timezone.utc)
    return when.replace(microsecond=0).isoformat().replace("+00:00", "Z")


# --- the project version, by the same rules as the build ------------------

def semver(tag, ahead, rev, dirty):
    """The version string firmware/controller/CMakeLists.txt would produce.

    The cases are its documented ones:

        on the tag, clean    1.4.0
        on the tag, dirty    1.4.0+dirty
        12 commits past it   1.4.0+12.a1b2c3d
        ... and dirty        1.4.0+12.a1b2c3d.dirty
        no tag yet           0.0.0+a1b2c3d
        not a git checkout   0.0.0
    """
    suffix = ".dirty" if dirty else ""
    if not tag:
        if not rev:
            return "0.0.0"
        return f"0.0.0+{rev}{suffix}"
    base = tag[1:] if tag.startswith("v") else tag
    if not ahead:
        return base if not dirty else f"{base}+dirty"
    return f"{base}+{ahead}.{rev}{suffix}"


def project_version(repo=ROOT):
    """Derived from git, and NOT from the build, on purpose.

    The build's own `project_version` is authoritative when a build exists and is
    preferred by build_inventory; this is the fallback that lets the tool produce
    an SBOM of the sources alone, which is what a `--self-test` or a dry run in a
    tree with no build directory needs.
    """
    if shutil.which("git") is None:
        return "0.0.0"
    rev = run_capture(["git", "rev-parse", "--short=7", "HEAD"], cwd=repo)
    if rev is None:
        return "0.0.0"
    dirty = bool(run_capture(["git", "status", "--porcelain", "--untracked-files=no"],
                             cwd=repo))
    tag = run_capture(["git", "describe", "--tags", "--abbrev=0", "--match", TAG_GLOB],
                      cwd=repo)
    ahead = None
    if tag:
        count = run_capture(["git", "rev-list", f"{tag}..HEAD", "--count"], cwd=repo)
        ahead = None if count in (None, "", "0") else count
    return semver(tag, ahead, rev, dirty)


# --- the build's own description -----------------------------------------

def find_build_dir(explicit=None):
    if explicit is not None:
        p = pathlib.Path(explicit)
        if not (p / "project_description.json").is_file():
            raise Error(f"{p} holds no project_description.json; is it an IDF build dir?")
        return p
    for name in DEFAULT_BUILD_DIRS:
        p = FIRMWARE / name
        if (p / "project_description.json").is_file():
            return p
    raise Error(
        "no IDF build directory found. Build the firmware first:\n"
        "  idf.py -C firmware/controller build\n"
        "or pass --build-dir. The component list comes from the build rather "
        "than from a list in this tool, so there is nothing to report without one."
    )


def read_project_description(build_dir):
    """Normalise the fields this tool needs out of project_description.json.

    Key names have moved between IDF versions, so each one is looked for under
    the spellings it has had.  A missing component map is an error rather than an
    empty SBOM: an inventory that silently lists nothing is worse than no
    inventory, because it looks like an answer.
    """
    p = pathlib.Path(build_dir) / "project_description.json"
    try:
        d = json.loads(p.read_text())
    except json.JSONDecodeError as exc:
        raise Error(f"{p} is not valid JSON: {exc}") from None

    info = d.get("build_component_info") or d.get("component_info") or {}
    paths = d.get("build_component_paths") or []
    names = d.get("build_components") or list(info)

    components = {}
    for i, name in enumerate(names):
        # ESP-IDF v6.0.1 puts one empty string in `build_components`.  Found by
        # running this against a real build, where it produced a component
        # called "esp-idf-" with no version and no licence: an SBOM entry for
        # nothing, which is worse than a missing one because it looks like data.
        if not name or not name.strip():
            continue
        entry = info.get(name) or {}
        directory = entry.get("dir") or (paths[i] if i < len(paths) else None)
        if directory:
            components[name] = directory
        else:
            components[name] = None

    if not components:
        raise Error(
            f"{p} names no components (looked for build_components, "
            "build_component_info and component_info). Either the file is from "
            "an IDF this tool has not seen, or the build did not finish."
        )

    return {
        "project_name": d.get("project_name") or "safekiln",
        "project_version": d.get("project_version"),
        "target": d.get("target") or "esp32s3",
        "idf_path": d.get("idf_path"),
        "components": components,
        "toolprefix": d.get("monitor_toolprefix") or "",
        "c_compiler": d.get("c_compiler") or "",
    }


def idf_version(idf_path):
    if not idf_path:
        return os.environ.get("IDF_VER") or NOASSERTION
    p = pathlib.Path(idf_path)
    vf = p / "version.txt"
    if vf.is_file():
        text = vf.read_text().strip()
        if text:
            return text
    described = run_capture(["git", "describe", "--tags", "--dirty"], cwd=p)
    if described:
        return described
    return os.environ.get("IDF_VER") or NOASSERTION


def mbedtls_version(idf_path):
    """From mbedTLS's own header, which is the only place that cannot be stale.

    Called out separately from the component scan because mbedTLS is the
    dependency most likely to be the subject of the advisory this SBOM exists to
    answer, and because its version is its submodule's, not ESP-IDF's.
    """
    if not idf_path:
        return None
    base = pathlib.Path(idf_path) / "components" / "mbedtls" / "mbedtls" / "include" / "mbedtls"
    for name in ("build_info.h", "version.h"):
        f = base / name
        if not f.is_file():
            continue
        m = re.search(r'#define\s+MBEDTLS_VERSION_STRING\s+"([^"]+)"', f.read_text(errors="replace"))
        if m:
            return m.group(1)
    return None


def compiler_version(desc):
    """Version of the C compiler that actually built the image, if reachable."""
    cc = desc.get("c_compiler") or ""
    if cc and pathlib.Path(cc).is_file():
        prog = cc
    else:
        prefix = desc.get("toolprefix") or ""
        prog = shutil.which(prefix + "gcc") if prefix else None
        if prog is None:
            prog = shutil.which("xtensa-esp32s3-elf-gcc")
    if prog is None:
        return None, None
    out = run_capture([prog, "-dumpversion"])
    return pathlib.Path(prog).name, (out or None)


# --- licences, from the sources themselves -------------------------------

def find_licence_files(directory):
    """Relative paths of licence files, for a component whose sources carry no tag."""
    if not directory:
        return []
    d = pathlib.Path(directory)
    if not d.is_dir():
        return []
    found = []
    for path in sorted(d.rglob("*")):
        if len(found) >= LICENCE_FILE_MAX:
            break
        rel = path.relative_to(d)
        if len(rel.parts) > LICENCE_FILE_DEPTH or not path.is_file():
            continue
        if LICENCE_FILE_RE.match(path.name):
            found.append(rel.as_posix())
    return found


def scan_licenses(directory):
    """SPDX expressions found in a component's own sources, sorted and deduped.

    Bounded by SCAN_MAX_FILES and SCAN_HEAD_BYTES so that scanning the whole of
    ESP-IDF stays cheap; tests and examples are skipped because a test fixture's
    licence is not the component's.
    """
    if not directory:
        return []
    d = pathlib.Path(directory)
    if not d.is_dir():
        return []
    found = set()
    seen = 0
    for path in sorted(d.rglob("*")):
        if seen >= SCAN_MAX_FILES:
            break
        if not path.is_file() or path.suffix not in SCAN_SUFFIXES:
            continue
        if any(part in SCAN_SKIP_DIRS or SCAN_SKIP_DIR_RE.search(part)
               for part in path.relative_to(d).parts[:-1]):
            continue
        seen += 1
        try:
            with open(path, "rb") as f:
                head = f.read(SCAN_HEAD_BYTES)
        except OSError:
            continue
        for m in SPDX_TAG.finditer(head):
            expr = m.group(1).decode(errors="replace").strip().strip("*").strip()
            # A tag inside this tool's own regex, or a placeholder in a template,
            # is not a licence statement.
            if expr and not expr.startswith("<") and len(expr) < 120:
                found.add(expr)
    return sorted(found)


# --- the inventory --------------------------------------------------------

def spdxid(text):
    """SPDX ids allow letters, digits, '.' and '-' and nothing else."""
    return re.sub(r"[^A-Za-z0-9.\-]", "-", text)


def check_unique(ids, what):
    """Refuse to emit a document with a repeated identifier.

    Nothing produces one today, and the way one would appear is quiet:
    `spdxid()` maps every character outside its allowed set to a dash, so a
    component named `foo_bar` and one named `foo-bar` become the same package.
    A consumer reading that document silently loses one of them, which is worse
    than an error, so this is an error.
    """
    seen = set()
    for i in ids:
        if i in seen:
            raise Error(
                f"two components share the {what} {i!r}. That would make one of "
                "them invisible to whoever reads this document; rename one or "
                "widen the identifier scheme before publishing."
            )
        seen.add(i)


def make_component(name, version, licenses, ctype="library", purl=None,
                   description=None, hashes=None, supplier=None, nested=None):
    return {
        "name": name,
        "version": version or NOASSERTION,
        "licenses": licenses or [],
        "type": ctype,
        "purl": purl,
        "description": description,
        "hashes": hashes or {},
        "supplier": supplier,
        "nested": nested or [],
    }


def managed_component(name, directory):
    """A component the component manager fetched, described as what it is.

    These are the only third-party sources in the image that did not arrive with
    ESP-IDF, so they are the only ones whose version an advisory can be matched
    against. Describing one as `esp-idf-x` at the IDF version, which is what the
    generic path below would do, states the wrong version for the one entry where
    the version is the whole point: UR-CON-04 requires the exact pin and the
    committed lockfile precisely so that a CRA Article 13(1) question about a
    known vulnerability can be answered from the repository.

    Returns None when the directory is not a managed component, so the caller
    falls through to treating it as part of IDF.
    """
    if "managed_components" not in pathlib.Path(directory).parts:
        return None
    d = pathlib.Path(directory)
    meta = {}
    manifest = d / "idf_component.yml"
    if manifest.is_file():
        # Read by hand rather than with a YAML parser: this is two flat keys out
        # of a file the component manager wrote, and pulling in PyYAML to get
        # them would make a build-time gate depend on a package that may not be
        # in the CI container's python.
        lines = manifest.read_text(errors="replace").splitlines()
        for i, line in enumerate(lines):
            for key in ("version", "description"):
                prefix = key + ":"
                if not line.startswith(prefix) or key in meta:
                    continue
                value = line[len(prefix):].strip().strip("'\"")
                # The manager writes long values folded, so the rest of the
                # sentence sits on the following indented lines. Without this
                # the description ends mid-word, which reads as a bug in the
                # SBOM rather than as what it is.
                for cont in lines[i + 1:]:
                    if not cont[:1].isspace() or not cont.strip():
                        break
                    value += " " + cont.strip()
                meta[key] = value
    # `espressif__mdns` is the directory name the manager uses for `espressif/mdns`.
    registry_name = name.replace("__", "/", 1)
    chash = ""
    hashfile = d / ".component_hash"
    if hashfile.is_file():
        chash = hashfile.read_text().strip()
    version = meta.get("version", "")
    description = (
        f"Managed component `{registry_name}` {version or 'at an unknown version'}, "
        f"fetched from the ESP-IDF component registry and linked into the image. "
        f"Pinned exactly and locked per UR-CON-04")
    if chash:
        description += f"; registry content hash {chash}"
    if meta.get("description"):
        description += f". Upstream describes it as: {meta['description']}"
    licences = scan_licenses(d)
    if not licences:
        licences = [NOASSERTION]
    return make_component(
        registry_name, version, licences,
        purl=f"pkg:idf/{registry_name}@{version}" if version else None,
        description=description,
        supplier="Espressif Systems",
    )


def web_assets(version=None):
    """The browser assets, with hashes, and the point they make by being listed.

    Not a dependency inventory: an evidence item. Every one of these is
    first-party, which is the resolution of OQ-04 and is the answer to "which
    JavaScript libraries does this ship".
    """
    web = ROOT / "web"
    if not web.is_dir():
        return None
    files = sorted(p for p in web.iterdir()
                   if p.is_file() and p.suffix in WEB_SUFFIXES)
    if not files:
        return None
    # Versioned with the product rather than left NOASSERTION: these files are
    # part of this project and ship inside the image, so the image's version is
    # the only version they have.
    nested = [
        make_component(f"web/{p.name}", version, [PROJECT_LICENCE], ctype="file",
                       hashes={"SHA-256": sha256_file(p)}, supplier=SUPPLIER)
        for p in files
    ]
    return make_component(
        "safekiln-web-assets", version, [PROJECT_LICENCE], ctype="library",
        supplier=SUPPLIER,
        description=("The browser interface, served from the image. Every file is "
                     "first-party: there are no vendored third-party browser "
                     "libraries, which is the resolution of OQ-04 and is why this "
                     "entry exists at all."),
        nested=nested,
    )


def build_inventory(build_dir=None, image=None, supervisor_elf=None, repo=ROOT):
    """Everything that ships, from the build that made it.

    Returns (product, components, warnings), where each warning is a
    (kind, text) pair.  The kinds are separated because they are not equally the
    project's fault:

      "structural"  something about OUR build is missing: no image digest, no
                    build directory, no toolchain.  A release must not ship with
                    one of these, and --strict refuses.

      "licence"     a third-party component's own sources carry no SPDX tag.
                    Worth reporting every time and worth resolving, but gating a
                    release on the tagging habits of a vendor's tree would stop
                    releases for something the project cannot fix in the moment.
                    --require-licences is there for whoever decides otherwise.
    """
    warnings = []

    desc = None
    if build_dir is not None or any(
            (FIRMWARE / n / "project_description.json").is_file() for n in DEFAULT_BUILD_DIRS):
        desc = read_project_description(find_build_dir(build_dir))

    version = (desc or {}).get("project_version") or project_version(repo)
    target = (desc or {}).get("target") or "esp32s3"

    hashes = {}
    if image is not None:
        p = pathlib.Path(image)
        if not p.is_file():
            raise Error(f"no image at {p}")
        hashes["SHA-256"] = sha256_file(p)
    else:
        warnings.append((
            "structural",
            "no --image given, so the product carries no digest: an SBOM that "
            "cannot be tied to a binary cannot answer which units are affected"))

    product = make_component(
        "safekiln", version, [PROJECT_LICENCE], ctype="firmware",
        purl=f"pkg:generic/safekiln@{version}?target={target}",
        description=f"Safe Kiln Controller firmware for {target}",
        hashes=hashes, supplier=SUPPLIER,
    )

    components = []

    if desc is None:
        warnings.append((
            "structural",
            "no IDF build directory: the SBOM covers this project's own sources "
            "and nothing it links against. Build first, or pass --build-dir"))
    else:
        idf_path = desc["idf_path"]
        if not idf_path or not pathlib.Path(idf_path).is_dir():
            warnings.append((
                "structural",
                "the ESP-IDF tree is not on this machine, so every component's "
                "licence is NOASSERTION. Run this where $IDF_PATH is, which for "
                "CI means inside the IDF container rather than a step after it"))

        idfver = idf_version(idf_path)
        components.append(make_component(
            "esp-idf", idfver, scan_licenses(pathlib.Path(idf_path) / "components" / "esp_common")
            if idf_path else [],
            purl=f"pkg:github/espressif/esp-idf@{idfver}",
            description="The framework and SDK the controller image is built on",
            supplier="Espressif Systems",
        ))

        mbed = mbedtls_version(idf_path)
        if mbed:
            components.append(make_component(
                "mbedtls", mbed,
                scan_licenses(pathlib.Path(idf_path) / "components" / "mbedtls" / "mbedtls"),
                purl=f"pkg:generic/mbedtls@{mbed}",
                description=("TLS and the crypto behind secure boot and the update "
                             "manifest check (SWR-UPD-11). Versioned by its own "
                             "submodule rather than by ESP-IDF, which is why it is "
                             "named separately"),
                supplier="TrustedFirmware",
            ))

        for name, directory in sorted(desc["components"].items()):
            managed = managed_component(name, directory)
            if managed is not None:
                if managed["licenses"] == [NOASSERTION]:
                    warnings.append((
                        "licence",
                        f"managed component {managed['name']} carries no SPDX tag, "
                        "and UR-CON-04 allows a fetched component only when its "
                        "licence is recorded"))
                components.append(managed)
                continue
            licences = scan_licenses(directory)
            description = f"ESP-IDF component `{name}`, linked into the image"
            if not licences:
                licences = [NOASSERTION]
                files = find_licence_files(directory)
                description += (
                    ". Its sources carry no SPDX tag, so no licence is asserted"
                    + (f"; a licence file ships at {', '.join(files)}" if files
                       else " and it ships no licence file either, which is worth "
                            "raising upstream"))
            components.append(make_component(
                f"esp-idf-{name}", idfver, licences,
                description=description,
                supplier="Espressif Systems",
            ))

        ccname, ccver = compiler_version(desc)
        if ccname:
            components.append(make_component(
                ccname, ccver, [], ctype="application",
                description=("The compiler that produced the controller image. A "
                             "build input rather than a shipped component: nothing "
                             "of it is distributed in the product, which is why no "
                             "licence is asserted for it"),
                supplier="Espressif Systems",
            ))
        else:
            warnings.append((
                "structural",
                "the cross compiler is not reachable, so the toolchain is not in "
                "the inventory; SWR-NFR-28 asks for it by name"))

    if supervisor_elf is not None:
        p = pathlib.Path(supervisor_elf)
        if not p.is_file():
            raise Error(f"no supervisor image at {p}")
        components.append(make_component(
            "safekiln-supervisor", version, [PROJECT_LICENCE], ctype="firmware",
            hashes={"SHA-256": sha256_file(p)}, supplier=SUPPLIER,
            description=("The independent safety supervisor (SWA-22), STM32G031K8T6. "
                         "Links no third-party source: the register definitions are "
                         "generated from ST's SVD and the trip logic is this "
                         "project's own"),
        ))
        armcc = shutil.which("arm-none-eabi-gcc")
        if armcc:
            components.append(make_component(
                "arm-none-eabi-gcc", run_capture([armcc, "-dumpversion"]), [],
                ctype="application",
                description=("The compiler that produced the supervisor image. A "
                             "build input rather than a shipped component"),
            ))

    web = web_assets(version)
    if web is not None:
        components.append(web)

    unresolved = [c["name"] for c in components if c["licenses"] == [NOASSERTION]]
    if unresolved:
        warnings.append((
            "licence",
            f"{len(unresolved)} component(s) carry no SPDX tag in their sources and "
            f"are NOASSERTION: {', '.join(unresolved[:6])}"
            + (" ..." if len(unresolved) > 6 else "")))

    components.sort(key=lambda c: c["name"])
    return product, components, warnings


def serial_uuid(product, components):
    """A stable serial number: same inventory, same urn:uuid.

    Derived rather than random so that two SBOMs of the same build are
    byte-identical and a diff between releases shows the product changing.  The
    version and variant bits are set so it is a well-formed UUID; it is a digest
    wearing a UUID's clothes, which is what RFC 4122 version 5 is.
    """
    payload = json.dumps([product, components], sort_keys=True).encode()
    h = bytearray(hashlib.sha256(payload).digest()[:16])
    h[6] = (h[6] & 0x0F) | 0x50
    h[8] = (h[8] & 0x3F) | 0x80
    hexed = h.hex()
    return f"{hexed[:8]}-{hexed[8:12]}-{hexed[12:16]}-{hexed[16:20]}-{hexed[20:32]}"


# --- CycloneDX 1.6 --------------------------------------------------------

def cdx_component(c, ref_prefix=""):
    ref = f"{ref_prefix}{c['name']}@{c['version']}"
    out = {
        "bom-ref": ref,
        "type": c["type"],
        "name": c["name"],
        "version": c["version"],
    }
    if c["supplier"]:
        out["supplier"] = {"name": c["supplier"]}
    if c["description"]:
        out["description"] = c["description"]
    if c["licenses"]:
        out["licenses"] = [
            {"license": {"id": lic}} if re.fullmatch(r"[A-Za-z0-9.\-+]+", lic)
            else {"expression": lic}
            for lic in c["licenses"] if lic != NOASSERTION
        ]
        if not out["licenses"]:
            del out["licenses"]
    if c["purl"]:
        out["purl"] = c["purl"]
    if c["hashes"]:
        out["hashes"] = [{"alg": alg, "content": val}
                         for alg, val in sorted(c["hashes"].items())]
    if c["nested"]:
        out["components"] = [cdx_component(n) for n in c["nested"]]
    return out


def emit_cyclonedx(product, components, when):
    prod = cdx_component(product)
    comps = [cdx_component(c) for c in components]
    check_unique([prod["bom-ref"], *(c["bom-ref"] for c in comps)], "bom-ref")
    return {
        "bomFormat": "CycloneDX",
        "specVersion": CYCLONEDX_SPEC,
        "serialNumber": f"urn:uuid:{serial_uuid(product, components)}",
        "version": 1,
        "metadata": {
            "timestamp": when,
            "tools": {"components": [{
                "type": "application",
                "name": TOOL_NAME,
                "version": TOOL_VERSION,
                "supplier": {"name": SUPPLIER},
            }]},
            "component": prod,
            "supplier": {"name": SUPPLIER, "url": [PROJECT_URL]},
            "licenses": [{"license": {"id": PROJECT_LICENCE}}],
        },
        "components": comps,
        "dependencies": [
            {"ref": prod["bom-ref"], "dependsOn": [c["bom-ref"] for c in comps]},
            *({"ref": c["bom-ref"], "dependsOn": []} for c in comps),
        ],
    }


# --- SPDX 2.3 -------------------------------------------------------------

def spdx_package(c, is_product=False):
    pkg = {
        "SPDXID": f"SPDXRef-{spdxid(c['name'])}",
        "name": c["name"],
        "versionInfo": c["version"],
        "downloadLocation": PROJECT_URL if is_product else NOASSERTION,
        "filesAnalyzed": False,
        "licenseConcluded": NOASSERTION,
        "licenseDeclared": (" AND ".join(c["licenses"]) if c["licenses"]
                            and c["licenses"] != [NOASSERTION] else NOASSERTION),
        "copyrightText": NOASSERTION,
        "supplier": f"Organization: {c['supplier']}" if c["supplier"] else NOASSERTION,
    }
    if is_product:
        pkg["licenseConcluded"] = PROJECT_LICENCE
    if c["description"]:
        pkg["description"] = c["description"]
    if c["purl"]:
        pkg["externalRefs"] = [{
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": c["purl"],
        }]
    if c["hashes"]:
        pkg["checksums"] = [{"algorithm": alg.replace("-", ""), "checksumValue": val}
                            for alg, val in sorted(c["hashes"].items())]
    return pkg


def emit_spdx(product, components, when):
    prod = spdx_package(product, is_product=True)
    pkgs = [spdx_package(c) for c in components]
    check_unique([prod["SPDXID"], *(p["SPDXID"] for p in pkgs)], "SPDXID")
    uuid = serial_uuid(product, components)
    return {
        "spdxVersion": SPDX_VERSION,
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{product['name']}-{product['version']}",
        "documentNamespace": f"{PROJECT_URL}/sbom/{product['name']}-{product['version']}-{uuid}",
        "creationInfo": {
            "created": when,
            "creators": [f"Tool: {TOOL_NAME}-{TOOL_VERSION}",
                         f"Organization: {SUPPLIER}"],
        },
        "packages": [prod, *pkgs],
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT",
             "relationshipType": "DESCRIBES",
             "relatedSpdxElement": prod["SPDXID"]},
            *({"spdxElementId": prod["SPDXID"],
               "relationshipType": "DEPENDS_ON",
               "relatedSpdxElement": p["SPDXID"]} for p in pkgs),
        ],
    }


# --- self-test ------------------------------------------------------------

def self_test():
    """Everything that needs no build, no ESP-IDF and no network.

    The project_description.json shapes are synthesised rather than taken from a
    real build, because the point of those checks is the key names that have
    moved between IDF versions, and a real file only ever has one of them.
    """
    import tempfile
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

    # --- the version rules, against CMakeLists' own documented cases -----
    check("on the tag, clean", semver("v1.4.0", None, "a1b2c3d", False), "1.4.0")
    check("on the tag, dirty", semver("v1.4.0", None, "a1b2c3d", True), "1.4.0+dirty")
    check("12 past it", semver("v1.4.0", "12", "a1b2c3d", False), "1.4.0+12.a1b2c3d")
    check("12 past it, dirty", semver("v1.4.0", "12", "a1b2c3d", True),
          "1.4.0+12.a1b2c3d.dirty")
    check("no tag yet", semver(None, None, "a1b2c3d", False), "0.0.0+a1b2c3d")
    check("no tag, dirty", semver(None, None, "a1b2c3d", True), "0.0.0+a1b2c3d.dirty")
    check("not a checkout", semver(None, None, None, False), "0.0.0")
    check("tag without the v", semver("1.4.0", None, "a1b2c3d", False), "1.4.0")

    # --- SPDX id sanitisation --------------------------------------------
    check("plain name", spdxid("mbedtls"), "mbedtls")
    check("slashes and underscores", spdxid("esp-idf-wpa_supplicant"),
          "esp-idf-wpa-supplicant")
    check("version characters survive", spdxid("gcc-12.2.0"), "gcc-12.2.0")
    check("path separators go", spdxid("web/app.js"), "web-app.js")

    # --- the licence scan ------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        (d / "a.c").write_bytes(b"/* SPDX-License-Identifier: Apache-2.0 */\n")
        (d / "b.c").write_bytes(b"// SPDX-License-Identifier: MIT\n")
        (d / "dual.c").write_bytes(
            b"/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */\n")
        (d / "untagged.c").write_bytes(b"int main(void){return 0;}\n")
        (d / "notsource.bin").write_bytes(b"SPDX-License-Identifier: WTFPL\n")
        (d / "test").mkdir()
        (d / "test" / "t.c").write_bytes(b"/* SPDX-License-Identifier: Unlicense */\n")
        # The shapes the real ESP-IDF v6.0.1 tree uses for code that does not
        # ship, each of which was polluting the licence set before it was
        # skipped: component/test_apps/x/main/y.c, i2c_test_apps, heap_tests,
        # pthread_unity_tests, host_test.
        for sub_dir in ("test_apps/app/main", "i2c_test_apps", "heap_tests",
                        "pthread_unity_tests", "host_test"):
            nested = d / sub_dir
            nested.mkdir(parents=True)
            (nested / "t.c").write_bytes(b"/* SPDX-License-Identifier: CC0-1.0 */\n")
        got = scan_licenses(d)
        check("scan finds the tags it should",
              got, ["Apache-2.0", "Apache-2.0 OR GPL-2.0-or-later", "MIT"])
        check("a binary is not scanned", "WTFPL" in " ".join(got), False)
        check("a test fixture's licence is not the component's",
              "Unlicense" in " ".join(got), False)
        check("nor a test app's, however it is spelled",
              "CC0-1.0" in " ".join(got), False)
        check("no directory, no licences", scan_licenses(None), [])
        check("missing directory, no licences", scan_licenses(d / "nope"), [])

        # Licence files, in the shapes ESP-IDF's untagged components use.
        lf = d / "vendored"
        (lf / "sub" / "deep" / "deeper").mkdir(parents=True)
        (lf / "LICENSE.txt").write_text("MIT-ish prose")
        (lf / "sub" / "COPYING").write_text("prose")
        (lf / "sub" / "deep" / "deeper" / "LICENSE").write_text("too deep to count")
        (lf / "readme.md").write_text("not a licence")
        check("licence files are found, nearest first",
              find_licence_files(lf), ["LICENSE.txt", "sub/COPYING"])
        check("and nothing is asserted from them", scan_licenses(lf), [])
        check("no directory, no licence files", find_licence_files(None), [])

        # --- project_description.json, in both of its shapes -------------
        modern = {
            "project_name": "safekiln", "project_version": "1.4.0",
            "target": "esp32s3", "idf_path": "/opt/esp/idf",
            "monitor_toolprefix": "xtensa-esp32s3-elf-",
            "build_components": ["freertos", "mbedtls"],
            "build_component_info": {
                "freertos": {"dir": "/opt/esp/idf/components/freertos"},
                "mbedtls": {"dir": "/opt/esp/idf/components/mbedtls"},
            },
        }
        bd = d / "build"
        bd.mkdir()
        (bd / "project_description.json").write_text(json.dumps(modern))
        desc = read_project_description(bd)
        check("project version", desc["project_version"], "1.4.0")
        check("components, modern shape", sorted(desc["components"]),
              ["freertos", "mbedtls"])
        check("component dir", desc["components"]["freertos"],
              "/opt/esp/idf/components/freertos")

        older = {
            "project_name": "safekiln", "target": "esp32s3",
            "build_components": ["lwip", "newlib"],
            "build_component_paths": ["/idf/components/lwip", "/idf/components/newlib"],
        }
        (bd / "project_description.json").write_text(json.dumps(older))
        desc2 = read_project_description(bd)
        check("components, path-list shape", sorted(desc2["components"]),
              ["lwip", "newlib"])
        check("dir from the parallel list", desc2["components"]["newlib"],
              "/idf/components/newlib")
        check("target defaulted", desc2["target"], "esp32s3")

        blank = dict(modern)
        blank["build_components"] = ["freertos", "", "mbedtls"]
        (bd / "project_description.json").write_text(json.dumps(blank))
        check("a blank component name is dropped, not published as 'esp-idf-'",
              sorted(read_project_description(bd)["components"]),
              ["freertos", "mbedtls"])

        (bd / "project_description.json").write_text('{"project_name":"x"}')
        raises("a description naming no components",
               lambda: read_project_description(bd))
        (bd / "project_description.json").write_text("{not json")
        raises("a description that is not JSON",
               lambda: read_project_description(bd))
        raises("a build dir with no description",
               lambda: find_build_dir(d))

    # --- managed components, whose version is the point -------------------
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp) / "managed_components" / "espressif__mdns"
        d.mkdir(parents=True)
        (d / "idf_component.yml").write_text(
            "dependencies:\n  idf:\n    version: '>=5.0'\n"
            "description: Multicast UDP service used to provide local network\n"
            "  service and host discovery.\n"
            "version: 1.14.0\n")
        (d / ".component_hash").write_text("b5b3002\n")
        (d / "mdns.c").write_bytes(b"/* SPDX-License-Identifier: Apache-2.0 */\n")
        c = managed_component("espressif__mdns", str(d))
        check("a managed component is recognised", c is not None, True)
        check("named as the registry names it", c["name"], "espressif/mdns")
        check("its own version, not IDF's", c["version"], "1.14.0")
        check("licence from its sources", c["licenses"], ["Apache-2.0"])
        check("purl identifies it for an advisory match",
              c["purl"], "pkg:idf/espressif/mdns@1.14.0")
        check("the locked hash is recorded",
              "b5b3002" in c["description"], True)
        check("and the upstream description, unfolded rather than cut mid-line",
              c["description"].endswith(
                  "Multicast UDP service used to provide local network service "
                  "and host discovery."), True)
        # The version key of a *dependency* is indented and must not be mistaken
        # for the component's own, which is the one an advisory is matched on.
        check("an indented version key is not the component's",
              c["version"], "1.14.0")
        plain = pathlib.Path(tmp) / "components" / "freertos"
        plain.mkdir(parents=True)
        check("a component inside IDF is not treated as managed",
              managed_component("freertos", str(plain)), None)
        bare = pathlib.Path(tmp) / "managed_components" / "someone__thing"
        bare.mkdir(parents=True)
        b = managed_component("someone__thing", str(bare))
        check("no manifest, no version asserted", b["version"], NOASSERTION)
        check("and no purl rather than a wrong one", b["purl"], None)
        check("an untagged managed component is NOASSERTION, not silent",
              b["licenses"], [NOASSERTION])

    # --- the documents ---------------------------------------------------
    product = make_component(
        "safekiln", "1.4.0", [PROJECT_LICENCE], ctype="firmware",
        purl="pkg:generic/safekiln@1.4.0", hashes={"SHA-256": "ab" * 32},
        supplier=SUPPLIER)
    comps = [
        make_component("esp-idf", "v6.0.1", ["Apache-2.0"],
                       purl="pkg:github/espressif/esp-idf@v6.0.1",
                       supplier="Espressif Systems"),
        make_component("mbedtls", "3.6.0",
                       ["Apache-2.0 OR GPL-2.0-or-later"], supplier="TrustedFirmware"),
        make_component("esp-idf-opaque", "v6.0.1", [NOASSERTION]),
        make_component("safekiln-web-assets", None, [PROJECT_LICENCE],
                       nested=[make_component("web/app.js", None, [PROJECT_LICENCE],
                                              ctype="file",
                                              hashes={"SHA-256": "cd" * 32})]),
    ]
    when = "2026-10-09T00:00:00Z"

    cdx = emit_cyclonedx(product, comps, when)
    check("cyclonedx format", cdx["bomFormat"], "CycloneDX")
    check("cyclonedx spec", cdx["specVersion"], CYCLONEDX_SPEC)
    check("serial is a urn:uuid", cdx["serialNumber"].startswith("urn:uuid:"), True)
    check("product is the metadata component", cdx["metadata"]["component"]["name"],
          "safekiln")
    check("product carries its digest",
          cdx["metadata"]["component"]["hashes"], [{"alg": "SHA-256", "content": "ab" * 32}])
    check("every component is listed", len(cdx["components"]), 4)
    check("the dependency graph names the product",
          cdx["dependencies"][0]["ref"], "safekiln@1.4.0")
    check("and depends on all of them", len(cdx["dependencies"][0]["dependsOn"]), 4)
    check("a graph entry per component", len(cdx["dependencies"]), 5)
    byname = {c["name"]: c for c in cdx["components"]}
    check("a simple licence is an id",
          byname["esp-idf"]["licenses"], [{"license": {"id": "Apache-2.0"}}])
    check("a dual licence is an expression",
          byname["mbedtls"]["licenses"],
          [{"expression": "Apache-2.0 OR GPL-2.0-or-later"}])
    check("NOASSERTION is omitted rather than asserted",
          "licenses" in byname["esp-idf-opaque"], False)
    check("nested files are nested",
          [n["name"] for n in byname["safekiln-web-assets"]["components"]],
          ["web/app.js"])
    check("a nested file carries its hash",
          byname["safekiln-web-assets"]["components"][0]["hashes"],
          [{"alg": "SHA-256", "content": "cd" * 32}])

    spdx = emit_spdx(product, comps, when)
    check("spdx version", spdx["spdxVersion"], SPDX_VERSION)
    check("data licence", spdx["dataLicense"], "CC0-1.0")
    check("document id", spdx["SPDXID"], "SPDXRef-DOCUMENT")
    check("the product plus every component", len(spdx["packages"]), 5)
    check("the document describes the product",
          spdx["relationships"][0], {"spdxElementId": "SPDXRef-DOCUMENT",
                                     "relationshipType": "DESCRIBES",
                                     "relatedSpdxElement": "SPDXRef-safekiln"})
    check("one DEPENDS_ON per component",
          sum(1 for r in spdx["relationships"] if r["relationshipType"] == "DEPENDS_ON"),
          4)
    spkgs = {p["name"]: p for p in spdx["packages"]}
    check("declared licence of a dual-licensed package",
          spkgs["mbedtls"]["licenseDeclared"], "Apache-2.0 OR GPL-2.0-or-later")
    check("an unknown licence is NOASSERTION, not a guess",
          spkgs["esp-idf-opaque"]["licenseDeclared"], NOASSERTION)
    check("the product's concluded licence is this project's",
          spkgs["safekiln"]["licenseConcluded"], PROJECT_LICENCE)
    check("checksum algorithm is SPDX's spelling",
          spkgs["safekiln"]["checksums"], [{"algorithm": "SHA256",
                                            "checksumValue": "ab" * 32}])
    check("purl travels as an external ref",
          spkgs["esp-idf"]["externalRefs"][0]["referenceLocator"],
          "pkg:github/espressif/esp-idf@v6.0.1")
    check("every SPDXID is well formed",
          all(re.fullmatch(r"SPDXRef-[A-Za-z0-9.\-]+", p["SPDXID"])
              for p in spdx["packages"]), True)
    check("no file-level listing in the SPDX output", "files" in spdx, False)

    # The collision that sanitisation would otherwise hide.
    collide = [make_component("foo_bar", "1", []), make_component("foo-bar", "1", [])]
    raises("two components that sanitise to one SPDXID",
           lambda: emit_spdx(product, collide, when))
    twice = [make_component("same", "1", []), make_component("same", "1", [])]
    raises("two components with one bom-ref",
           lambda: emit_cyclonedx(product, twice, when))

    # --- reproducibility --------------------------------------------------
    check("the serial is derived, not random",
          serial_uuid(product, comps), serial_uuid(product, comps))
    check("and it is a well-formed v5-shaped uuid",
          bool(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
                            serial_uuid(product, comps))), True)
    moved = [dict(c) for c in comps]
    moved[0] = dict(moved[0], version="v6.0.2")
    checks += 1
    if serial_uuid(product, comps) == serial_uuid(product, moved):
        failed += 1
        print("  FAIL the serial ignores a version change")
    check("same inventory, same bytes",
          json.dumps(emit_cyclonedx(product, comps, when), sort_keys=True),
          json.dumps(emit_cyclonedx(product, comps, when), sort_keys=True))

    # --- SOURCE_DATE_EPOCH ------------------------------------------------
    old = os.environ.get("SOURCE_DATE_EPOCH")
    try:
        os.environ["SOURCE_DATE_EPOCH"] = "1760000000"
        # 1760000000 = 20370 whole days plus 32000 s, so 08:53:20 UTC.
        check("a fixed epoch gives a fixed timestamp", timestamp(), "2025-10-09T08:53:20Z")
        os.environ["SOURCE_DATE_EPOCH"] = "not a number"
        raises("a nonsense epoch", timestamp)
    finally:
        if old is None:
            os.environ.pop("SOURCE_DATE_EPOCH", None)
        else:
            os.environ["SOURCE_DATE_EPOCH"] = old

    # --- the web assets, which are the evidence item ----------------------
    web = web_assets()
    checks += 1
    if web is None:
        failed += 1
        print("  FAIL web/ holds no assets, so the no-third-party-JS evidence is missing")
    else:
        check("every web asset is first-party",
              {lic for n in web["nested"] for lic in n["licenses"]}, {PROJECT_LICENCE})
        check("and each carries a digest",
              all(n["hashes"].get("SHA-256") for n in web["nested"]), True)

    # --- an inventory of the sources alone, which must still be valid -----
    product2, comps2, warnings = build_inventory(image=None)
    check("the product is named", product2["name"], "safekiln")
    check("its licence is this project's", product2["licenses"], [PROJECT_LICENCE])
    checks += 1
    if not any(kind == "structural" and "no --image" in text
               for kind, text in warnings):
        failed += 1
        print("  FAIL an SBOM with no image digest does not warn about it")
    cdx2 = emit_cyclonedx(product2, comps2, when)
    check("and it is still a valid document", cdx2["bomFormat"], "CycloneDX")

    print(f"{checks - failed}/{checks} checks passed")
    return 1 if failed else 0


# --- main -----------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate the software bill of materials (SWR-NFR-28).",
        epilog="Run it where $IDF_PATH is, which for CI means inside the IDF "
               "container: the component licences are scanned from the sources "
               "on disk and cannot be recovered afterwards.",
    )
    ap.add_argument("--build-dir", help="IDF build directory; default: the first of "
                                        + ", ".join(DEFAULT_BUILD_DIRS))
    ap.add_argument("--image", help="the application binary, for the product digest")
    ap.add_argument("--supervisor-elf", help="the supervisor image, to cover both MCUs")
    ap.add_argument("--out-dir", default=str(ROOT / "dist"),
                    help="where to write; default dist/")
    ap.add_argument("--format", choices=("cyclonedx", "spdx", "both"),
                    default="cyclonedx")
    ap.add_argument("--strict", action="store_true",
                    help="fail on a gap in OUR build: no image digest, no build "
                         "directory, no toolchain. What a release should use")
    ap.add_argument("--require-licences", action="store_true",
                    help="additionally fail when a third-party component's own "
                         "sources carry no SPDX tag. Not on by default: it would "
                         "gate a release on a vendor tree's tagging habits")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    try:
        product, components, warnings = build_inventory(
            build_dir=args.build_dir, image=args.image,
            supervisor_elf=args.supervisor_elf)
        when = timestamp()
        out = pathlib.Path(args.out_dir)
        out.mkdir(parents=True, exist_ok=True)
        stem = f"{product['name']}-{product['version']}"

        written = []
        if args.format in ("cyclonedx", "both"):
            p = out / f"{stem}.cdx.json"
            p.write_text(json.dumps(emit_cyclonedx(product, components, when),
                                    indent=2) + "\n")
            written.append(p)
        if args.format in ("spdx", "both"):
            p = out / f"{stem}.spdx.json"
            p.write_text(json.dumps(emit_spdx(product, components, when),
                                    indent=2) + "\n")
            written.append(p)
    except Error as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for p in written:
        print(f"wrote {p}")
    print(f"{product['name']} {product['version']}, {len(components)} components")

    structural = [text for kind, text in warnings if kind == "structural"]
    licence = [text for kind, text in warnings if kind == "licence"]

    if warnings:
        print()
        print("Gaps, each of which makes this SBOM less able to answer the question")
        print("it exists for:")
        for text in structural:
            print(f"  - [build] {text}")
        for text in licence:
            print(f"  - [licence] {text}")

    if structural and args.strict:
        print()
        print("error: --strict, and the build gaps above stand", file=sys.stderr)
        return 1
    if licence and args.require_licences:
        print()
        print("error: --require-licences, and a component's licence is unresolved",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Host-side IDN checks: fixture schema, Python sender tests, native C tests.

  python tools/test_idn.py                  # everything that runs on this host
  python tools/test_idn.py --no-native      # fixtures + Python only
  python tools/test_idn.py --generator "MinGW Makefiles" --cc gcc
  python tools/test_idn.py --emit-c-header out/idn_fixtures.h

Steps, each reported separately:

  1. fixtures  validate tests/idn/fixtures/*.json: schema, hex, segment
               offsets and literal lengths. No production code is used.
  2. python    unittest discovery in tests/idn (standard library only).
  3. native    every tests/idn/native/<name>/CMakeLists.txt is configured,
               built and run with ctest, in its own build directory under
               build/idn-tests/ (outside the source tree). Each target gets
                 -DIDN_FIXTURES_DIR=<repo>/tests/idn/fixtures
                 -DIDN_FIXTURE_HEADER_DIR=<build>/generated
               where generated/idn_fixtures.h holds every fixture as a C array.

Nothing here touches the network or hardware, and nothing writes into the
source tree. Host-native tests do not prove that the ESP32-S3 / RP2354
firmware builds; those builds (tools/build_all.py) and hardware runs
(tools/hwtest.py) are separate checks. Exit status is non-zero if any step fails.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE_DIR = os.path.join(ROOT, "tests", "idn", "fixtures")
PY_TEST_DIR = os.path.join(ROOT, "tests", "idn")
NATIVE_DIR = os.path.join(ROOT, "tests", "idn", "native")
DEFAULT_BUILD_DIR = os.path.join(ROOT, "build", "idn-tests")

SCHEMA = "idn-fixtures/1"
KINDS = {"datagram", "sequence", "sender-sequence", "table"}
DIRECTIONS = {"client-to-server", "server-to-client"}
INTERPRETATIONS = {"normative", "project"}
HEX_RE = re.compile(r"^(?:[0-9a-f]{2})+$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")


# -- 1. fixtures ---------------------------------------------------------------

def _check_segments(where: str, obj: dict, errors: list) -> bytes:
    segs = obj.get("segments")
    if not isinstance(segs, list) or not segs:
        errors.append(f"{where}: needs a non-empty 'segments' list")
        return b""
    data = bytearray()
    for k, s in enumerate(segs):
        w = f"{where}.segments[{k}]"
        for key in ("offset", "field", "hex", "value", "ref"):
            if key not in s:
                errors.append(f"{w}: missing '{key}'")
        hx = s.get("hex", "")
        if not isinstance(hx, str) or not HEX_RE.match(hx):
            errors.append(f"{w}: 'hex' must be lower-case hex octets, got {hx!r}")
            continue
        if s.get("offset") != len(data):
            errors.append(f"{w} ({s.get('field')}): offset {s.get('offset')} but previous segments end at {len(data)}")
        data += bytes.fromhex(hx)
    if obj.get("length") != len(data):
        errors.append(f"{where}: 'length' {obj.get('length')} but segments hold {len(data)} octets")
    dl = obj.get("datagram_length")
    if dl is not None and (not isinstance(dl, int) or dl < len(data)):
        errors.append(f"{where}: 'datagram_length' {dl} is shorter than the literal prefix ({len(data)})")
    return bytes(data)


def load_fixtures(fixture_dir: str = FIXTURE_DIR):
    """Parse and validate every fixture file. Returns (docs, datagrams, errors).

    datagrams maps a C-safe name to (bytes, family, outcome) for every literal
    datagram, sequence step and sender packet.
    """
    errors: list = []
    docs = {}
    datagrams = {}
    ids = {}
    paths = sorted(glob.glob(os.path.join(fixture_dir, "*.json")))
    if not paths:
        errors.append(f"no fixture files in {fixture_dir}")
    for path in paths:
        name = os.path.basename(path)
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as exc:
            errors.append(f"{name}: {exc}")
            continue
        docs[name] = doc
        if doc.get("schema") != SCHEMA:
            errors.append(f"{name}: schema must be {SCHEMA!r}")
        for key in ("family", "description", "sources", "fixtures"):
            if key not in doc:
                errors.append(f"{name}: missing '{key}'")
        sources = doc.get("sources", {})
        for sk, sv in sources.items():
            for key in ("title", "identifier", "revision"):
                if key not in sv:
                    errors.append(f"{name}: sources.{sk} missing '{key}'")
        family = doc.get("family", "?")
        for fx in doc.get("fixtures", []):
            fid = fx.get("id", "?")
            where = f"{name}:{fid}"
            if not ID_RE.match(str(fid)):
                errors.append(f"{where}: id must be lower-case words joined by '-'")
            if fid in ids:
                errors.append(f"{where}: duplicate id (also in {ids[fid]})")
            ids[fid] = name
            kind = fx.get("kind")
            if kind not in KINDS:
                errors.append(f"{where}: kind {kind!r} not in {sorted(KINDS)}")
                continue
            for key in ("summary", "refs", "interpretation"):
                if key not in fx:
                    errors.append(f"{where}: missing '{key}'")
            if fx.get("interpretation") not in INTERPRETATIONS:
                errors.append(f"{where}: interpretation must be one of {sorted(INTERPRETATIONS)}")
            if fx.get("interpretation") == "project" and not fx.get("note") and kind != "sender-sequence":
                errors.append(f"{where}: project interpretations need a 'note' explaining the choice")
            if not isinstance(fx.get("refs"), list) or not fx.get("refs"):
                errors.append(f"{where}: 'refs' must list at least one section reference")
            if kind != "table" and not isinstance(fx.get("expect"), dict):
                errors.append(f"{where}: needs an 'expect' object")
            outcome = (fx.get("expect") or {}).get("outcome", "")
            cname = fid.replace("-", "_")
            if kind == "datagram":
                if fx.get("direction") not in DIRECTIONS:
                    errors.append(f"{where}: direction must be one of {sorted(DIRECTIONS)}")
                if not outcome:
                    errors.append(f"{where}: expect.outcome is required")
                datagrams[cname] = (_check_segments(where, fx, errors), family, outcome)
            elif kind == "sequence":
                steps = fx.get("steps")
                if not isinstance(steps, list) or not steps:
                    errors.append(f"{where}: needs 'steps'")
                    continue
                for k, st in enumerate(steps):
                    if "source" not in st or "at_us" not in st:
                        errors.append(f"{where}.steps[{k}]: needs 'source' and 'at_us'")
                    datagrams[f"{cname}__{k}"] = (_check_segments(f"{where}.steps[{k}]", st, errors), family, "step")
            elif kind == "sender-sequence":
                if not isinstance(fx.get("sender"), dict):
                    errors.append(f"{where}: needs a 'sender' parameter object")
                for k, p in enumerate(fx.get("packets") or []):
                    if "ref" in p:
                        continue
                    _check_segments(f"{where}.packets[{k}]", p, errors)
            elif kind == "table":
                if not isinstance(fx.get("cases"), list) or not fx.get("cases"):
                    errors.append(f"{where}: needs 'cases'")
    for name, doc in docs.items():
        for fx in doc.get("fixtures", []):
            refs = [p["ref"] for p in fx.get("packets", []) if "ref" in p]
            if fx.get("variation_of"):
                refs.append(fx["variation_of"])
            for r in refs:
                if r not in ids:
                    errors.append(f"{name}:{fx.get('id')}: refers to unknown fixture {r!r}")
    return docs, datagrams, errors


def c_header(datagrams: dict) -> str:
    lines = ["/* Generated by tools/test_idn.py from tests/idn/fixtures/*.json. Do not edit. */",
             "#ifndef IDN_FIXTURES_H", "#define IDN_FIXTURES_H", "", "#include <stddef.h>", "",
             "typedef struct {", "    const char *name;     /* fixture id, '-' -> '_'; sequence steps get __<n> */",
             "    const char *family;", "    const char *outcome;  /* expect.outcome, or \"step\" */",
             "    const unsigned char *data;", "    size_t len;", "} idn_fixture_t;", ""]
    for name, (data, _, _) in sorted(datagrams.items()):
        body = ", ".join(f"0x{b:02x}" for b in data) or "0"
        lines.append(f"static const unsigned char idn_fx_{name}[] = {{{body}}};")
        lines.append(f"#define IDN_FX_{name.upper()}_LEN {len(data)}u")
    lines += ["", "static const idn_fixture_t idn_fixtures[] = {"]
    for name, (data, family, outcome) in sorted(datagrams.items()):
        lines.append(f'    {{"{name}", "{family}", "{outcome}", idn_fx_{name}, {len(data)}u}},')
    lines += ["};", "#define IDN_FIXTURE_COUNT (sizeof(idn_fixtures) / sizeof(idn_fixtures[0]))", "",
              "#endif", ""]
    return "\n".join(lines)


def step_fixtures() -> tuple:
    docs, datagrams, errors = load_fixtures()
    for e in errors:
        print(f"  FAIL {e}")
    n = sum(len(d.get("fixtures", [])) for d in docs.values())
    if errors:
        return False, f"{len(errors)} problem(s) in {len(docs)} files", datagrams
    return True, f"{n} fixtures in {len(docs)} files, {len(datagrams)} literal datagrams", datagrams


# -- 2. python -----------------------------------------------------------------

def step_python(verbosity: int) -> tuple:
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    suite = unittest.defaultTestLoader.discover(PY_TEST_DIR, pattern="test_*.py", top_level_dir=PY_TEST_DIR)
    result = unittest.TextTestRunner(verbosity=verbosity, stream=sys.stdout).run(suite)
    ok = result.wasSuccessful() and result.testsRun > 0
    detail = (f"{result.testsRun} tests, {len(result.failures)} failures, {len(result.errors)} errors, "
              f"{len(result.skipped)} skipped")
    return ok, detail


# -- 3. native -----------------------------------------------------------------

def pick_generator(requested):
    if requested:
        return requested
    env = os.environ.get("IDN_CMAKE_GENERATOR")
    if env:
        return env
    if shutil.which("ninja"):
        return "Ninja"
    if os.name == "nt" and shutil.which("mingw32-make"):
        return "MinGW Makefiles"
    return None


def probe_sanitizers(cc: str) -> tuple:
    """Whether `cc` links -fsanitize=address,undefined. Never presented as a pass."""
    if not shutil.which(cc):
        return False, f"compiler {cc!r} not found for the sanitizer probe"
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "p.c")
        with open(src, "w") as f:
            f.write("int main(void){return 0;}\n")
        exe = os.path.join(d, "p.exe" if os.name == "nt" else "p")
        r = subprocess.run([cc, "-fsanitize=address,undefined", src, "-o", exe], capture_output=True, text=True)
        if r.returncode != 0:
            return False, (r.stderr.strip().splitlines() or ["link failed"])[-1]
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode != 0:
            return False, "sanitized probe program did not run"
    return True, "address,undefined"


def run(cmd, log) -> bool:
    log.append("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if out:
        log.append(out)
    return r.returncode == 0


def step_native(args, datagrams) -> list:
    """Returns a list of (name, ok, detail) per native target."""
    lists = sorted(glob.glob(os.path.join(args.native_root, "*", "CMakeLists.txt")))
    if not lists:
        return []
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    if not cmake or not ctest:
        return [("(all)", False, f"{len(lists)} native target(s) found but cmake/ctest is not on PATH")]
    gen = pick_generator(args.generator)
    build_root = os.path.abspath(args.build_dir)
    gen_dir = os.path.join(build_root, "generated")
    os.makedirs(gen_dir, exist_ok=True)
    with open(os.path.join(gen_dir, "idn_fixtures.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(c_header(datagrams))
    extra_flags = []
    if args.sanitize:
        ok, why = probe_sanitizers(args.cc or "gcc")
        if ok:
            extra_flags.append("-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer")
            print(f"  sanitizers: enabled ({why})")
            args.sanitizer_note = (True, f"enabled ({why})")
        else:
            print(f"  sanitizers: UNSUPPORTED by this compiler ({why}); native tests run WITHOUT sanitizers")
            args.sanitizer_note = (None, f"UNSUPPORTED by this compiler ({why}); native tests ran without them")
    results = []
    for cml in lists:
        src = os.path.dirname(cml)
        name = os.path.basename(src)
        bdir = os.path.join(build_root, name)
        if args.clean and os.path.isdir(bdir):
            shutil.rmtree(bdir)
        log: list = []
        cfg = [cmake, "-S", src, "-B", bdir, "--no-warn-unused-cli",
               f"-DIDN_FIXTURES_DIR={FIXTURE_DIR}", f"-DIDN_FIXTURE_HEADER_DIR={gen_dir}",
               f"-DIDN_REPO_ROOT={ROOT}"] + extra_flags
        if gen:
            cfg += ["-G", gen]
        if args.cc:
            cfg.append(f"-DCMAKE_C_COMPILER={args.cc}")
        if not run(cfg, log):
            results.append((name, False, "configure failed", log))
            continue
        if not run([cmake, "--build", bdir], log):
            results.append((name, False, "build failed", log))
            continue
        ok = run([ctest, "--test-dir", bdir, "--output-on-failure", "--no-tests=error"], log)
        summary = next((l for l in reversed("\n".join(log).splitlines()) if "tests passed" in l), "")
        results.append((name, ok, summary or ("ctest passed" if ok else "ctest failed"), log))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-python", action="store_true", help="skip the Python unittest step")
    ap.add_argument("--no-native", action="store_true", help="skip native CMake targets")
    ap.add_argument("--generator", default=None,
                    help="CMake generator (default: $IDN_CMAKE_GENERATOR, else Ninja, else MinGW Makefiles on Windows)")
    ap.add_argument("--cc", default=None, help="C compiler for native targets (passed as CMAKE_C_COMPILER)")
    ap.add_argument("--build-dir", default=DEFAULT_BUILD_DIR, help="native build root (default: build/idn-tests)")
    ap.add_argument("--native-root", default=NATIVE_DIR, help="directory whose */CMakeLists.txt are native targets "
                                                              "(default: tests/idn/native)")
    ap.add_argument("--clean", action="store_true", help="delete each native build directory first")
    ap.add_argument("--sanitize", action="store_true", help="try ASan/UBSan; reported as unsupported if unavailable")
    ap.add_argument("--emit-c-header", metavar="PATH", help="write the fixture C header to PATH and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    report = []
    print("== fixtures")
    ok, detail, datagrams = step_fixtures()
    report.append(("fixtures", ok, detail))
    if args.emit_c_header:
        if not ok:
            print("fixture validation failed; header not written")
            return 1
        with open(args.emit_c_header, "w", encoding="utf-8", newline="\n") as f:
            f.write(c_header(datagrams))
        print(f"wrote {args.emit_c_header}")
        return 0

    if args.no_python:
        report.append(("python", None, "skipped (--no-python)"))
    else:
        print("\n== python (tests/idn)")
        report.append(("python",) + step_python(2 if args.verbose else 1))

    if args.no_native:
        report.append(("native", None, "skipped (--no-native)"))
    else:
        print("\n== native (tests/idn/native/*)")
        results = step_native(args, datagrams)
        if not results:
            print("  no native targets found")
            report.append(("native", None, "no native targets found (tests/idn/native/*/CMakeLists.txt)"))
        for r in results:
            name, ok_n, det = r[0], r[1], r[2]
            log = r[3] if len(r) > 3 else []
            if args.verbose or not ok_n:
                print("\n".join("    " + l for l in "\n".join(log).splitlines()))
            report.append((f"native/{name}", ok_n, det))
        if getattr(args, "sanitizer_note", None):
            report.append(("sanitizers",) + args.sanitizer_note)

    print("\n== summary")
    failed = False
    for name, ok, detail in report:
        tag = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
        failed |= ok is False
        print(f"  [{tag}] {name}: {detail}")
    print("  (not run here: ESP32-S3/RP2354 firmware builds, hardware tests, independent-receiver checks)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

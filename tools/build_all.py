#!/usr/bin/env python3
"""Build every firmware target from a scratch copy of the working tree.

Catches API drift between the boards: the ESP32-S3 for V1 (BOARD_VERSION=1,
where the ESP32-S3 drives the DAC itself through rp_manager_v1.c) and for V2,
and the RP2354. Each build runs in a temporary copy, the ESP32-S3 ones with
only sdkconfig.defaults as a fresh clone would have, so the local sdkconfig,
build folders and dependencies.lock are never touched.

  python3 tools/build_all.py              # esp-v1, esp-v2, rp2354
  python3 tools/build_all.py esp-v1       # one target
  python3 tools/build_all.py --keep       # keep the scratch copies

Logs go to logs/build-<target>-YYYYMMDD-HHMMSS.log. The exit status is 0 only
if every target built with no compiler warnings (--allow-warnings relaxes the
warning rule).

Toolchains, overridable from the environment:
  IDF_PATH             ~/.espressif/v6.1/esp-idf
  PICO_SDK_PATH        ~/.pico-sdk/sdk/2.2.0
  PICO_TOOLCHAIN_PATH  ~/.pico-sdk/toolchain/14_2_Rel1
  PICO_CMAKE           ~/.pico-sdk/cmake/v4.3.4/bin/cmake
  PICO_NINJA           ~/.pico-sdk/ninja/v1.13.2/ninja
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HOME = Path.home()

TARGETS = ("esp-v1", "esp-v2", "rp2354")

ESP_SKIP = {"build", "sdkconfig", "sdkconfig.old"}
RP_SKIP = {"build"}


def env_path(name: str, default: Path) -> str:
    return os.environ.get(name, str(default))


def copy_tree(src: Path, dst: Path, skip: set[str]) -> None:
    shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names
                                                       if Path(d) == src and n in skip])


def run(cmd: list[str], cwd: Path, log, env: dict | None = None) -> int:
    log.write(f"$ {' '.join(cmd)}\n")
    log.flush()
    return subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                          env=env).returncode


def build_esp(board: int, work: Path, log) -> int:
    proj = work / "ESP32S3"
    copy_tree(REPO / "firmware" / "ESP32S3", proj, ESP_SKIP)
    idf = env_path("IDF_PATH", HOME / ".espressif" / "v6.1" / "esp-idf")
    script = (f'export IDF_PATH="{idf}" && . "$IDF_PATH/export.sh" >/dev/null && '
              f"idf.py -DBOARD_VERSION={board} build")
    return run(["bash", "-c", script], proj, log)


def build_rp(work: Path, log) -> int:
    proj = work / "RP2354"
    copy_tree(REPO / "firmware" / "RP2354", proj, RP_SKIP)
    env = dict(os.environ)
    env["PICO_SDK_PATH"] = env_path("PICO_SDK_PATH", HOME / ".pico-sdk" / "sdk" / "2.2.0")
    env["PICO_TOOLCHAIN_PATH"] = env_path("PICO_TOOLCHAIN_PATH",
                                          HOME / ".pico-sdk" / "toolchain" / "14_2_Rel1")
    cmake = env_path("PICO_CMAKE", HOME / ".pico-sdk" / "cmake" / "v4.3.4" / "bin" / "cmake")
    ninja = env_path("PICO_NINJA", HOME / ".pico-sdk" / "ninja" / "v1.13.2" / "ninja")
    rc = run([cmake, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DPICO_BOARD=pico2",
              "-B", "build", "."], proj, log, env)
    if rc != 0:
        return rc
    return run([ninja, "-C", "build"], proj, log, env)


def count_warnings(log_path: Path) -> int:
    with open(log_path, errors="replace") as f:
        return sum(1 for line in f if "warning:" in line)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("targets", nargs="*", help=f"any of {', '.join(TARGETS)}; default: all")
    ap.add_argument("--keep", action="store_true", help="keep the scratch copies")
    ap.add_argument("--allow-warnings", action="store_true",
                    help="pass a target that builds with warnings")
    args = ap.parse_args()
    unknown = [t for t in args.targets if t not in TARGETS]
    if unknown:
        ap.error(f"unknown target {', '.join(unknown)}; choose from {', '.join(TARGETS)}")
    targets = args.targets or list(TARGETS)

    logs = REPO / "logs"
    logs.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    scratch = Path(tempfile.mkdtemp(prefix="ildawave-build-"))

    results = []
    for target in targets:
        log_path = logs / f"build-{target}-{stamp}.log"
        work = scratch / target
        work.mkdir()
        t0 = time.monotonic()
        with open(log_path, "w") as log:
            if target == "esp-v1":
                rc = build_esp(1, work, log)
            elif target == "esp-v2":
                rc = build_esp(2, work, log)
            else:
                rc = build_rp(work, log)
        warnings = count_warnings(log_path)
        ok = rc == 0 and (warnings == 0 or args.allow_warnings)
        results.append((target, rc, warnings, time.monotonic() - t0, ok, log_path))

    if args.keep:
        print(f"scratch copies kept in {scratch}")
    else:
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"{'target':<8} {'result':<7} {'warnings':>8} {'seconds':>8}  log")
    for target, rc, warnings, secs, ok, log_path in results:
        result = "ok" if ok else ("FAILED" if rc else "WARN")
        print(f"{target:<8} {result:<7} {warnings:>8} {secs:>8.0f}  {log_path.relative_to(REPO)}")

    return 0 if all(r[4] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())

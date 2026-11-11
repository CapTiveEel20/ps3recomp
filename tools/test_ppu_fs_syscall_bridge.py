#!/usr/bin/env python3
"""One fd space for cellFs and the raw sys_fs syscalls.

On a PS3, cellFsOpen and syscall 801 share the kernel's fd table, so a title
may open a file with one and read it with the other. Civilization Revolution
opens its movies with cellFsOpen and hands the fd to Bink, which seeks and
reads with syscalls 818/802; runtime/syscalls/sys_fs.c only knew its own
table and answered EBADF. This builds runtime/ppu/tests/test_fs_syscall_bridge.cpp
(which includes ppu_fs.cpp) plus sys_fs.c and checks lseek/read/fstat on a
cellFsOpen'd fd, and that the two open paths never hand out the same number.

It needs a host C/C++ compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_ppu_fs_syscall_bridge.py [--exe-out PATH] [--build-only]
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_syscall_bridge.cpp")
SYS_FS = os.path.join(ROOT, "runtime", "syscalls", "sys_fs.c")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "."]


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("g++")
            or shutil.which("clang++"))


def _stub_header():
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-out", help="keep the built test program at this path")
    ap.add_argument("--build-only", action="store_true",
                    help="build (for another machine, e.g. a Windows target) and stop")
    args = ap.parse_args()

    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            print("FAIL: no C++ compiler found")
            return 1
        print("skip test_ppu_fs_syscall_bridge: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        incs = ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        # sys_fs.c is C: compile it on its own (the C++ driver, told -x c).
        obj = os.path.join(td, "sys_fs.o")
        r = subprocess.run([cxx, "-x", "c", "-std=gnu11", "-w", "-c", SYS_FS, "-o", obj] + incs,
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: sys_fs.c did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        exe = args.exe_out or os.path.join(td, "test_fs_syscall_bridge")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td] + incs
        cmd += ["-o", exe, FIXTURE, obj, "-pthread"]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0
        r = subprocess.run([exe, td], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())

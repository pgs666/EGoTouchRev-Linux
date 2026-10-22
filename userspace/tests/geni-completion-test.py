#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Apply the real controller patch and compile its callback regression."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

compiler, test, fixture, patch = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="himax-geni-completion-") as directory:
    root = Path(directory)
    target = root / "drivers" / "spi" / "spi-geni-qcom.c"
    target.parent.mkdir(parents=True)
    shutil.copyfile(fixture, target)
    shutil.copyfile(fixture, root / "original.c")
    # Line-number offsets are expected for the small callback fixture, but no
    # fuzz or rejected hunks may silently weaken the actual shipped patch.
    result = subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch.resolve())],
                            cwd=root, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    shutil.copyfile(target, root / "patched.c")
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(root), str(test.resolve()), "-o", str(root / "test")],
                   check=True, timeout=30)
    subprocess.run([str(root / "test")], check=True, timeout=10)

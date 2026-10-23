#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproduce GPI lifetime failure and apply the actual shipped fix offline."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import tempfile

compiler, test, fixture, patch = map(Path, sys.argv[1:])
fixture_bytes = fixture.read_bytes()
expected = "18061aedb9d6ac24c4c5b9103275e2d566fe7204a29110dcd98708d65d3237c8"
assert hashlib.sha256(fixture_bytes).hexdigest() == expected, "review original GPI fixture drift"
with tempfile.TemporaryDirectory(prefix="himax-gpi-lifetime-") as directory:
    root = Path(directory)
    target = root / "drivers" / "dma" / "qcom" / "gpi.c"
    target.parent.mkdir(parents=True)
    target.write_bytes(fixture_bytes)

    def build(name):
        shutil.copyfile(target, root / "gpi-source.inc")
        binary = root / name
        subprocess.run(
            [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(root),
             str(test.resolve()), "-o", str(binary)],
            check=True, timeout=30,
        )
        return binary

    original = build("original")
    subprocess.run([str(original), "retained"], check=True, timeout=10)
    result = subprocess.run([str(original), "released"], capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and "AddressSanitizer" in result.stderr
    assert "double-free" in result.stderr and "gpi_peripheral_config" in result.stderr
    assert "gpi_free_chan_resources" in result.stderr, result.stderr
    print("Original GPI released/reconfigured channel fails ASan at freed config, as expected")
    result = subprocess.run(
        ["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch.resolve())],
        cwd=root, capture_output=True, text=True, timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert target.read_bytes().replace(b"\tgchan->config = NULL;\n", b"") == fixture_bytes
    fixed = build("fixed")
    subprocess.run([str(fixed), "retained"], check=True, timeout=10)
    subprocess.run([str(fixed), "released"], check=True, timeout=10)

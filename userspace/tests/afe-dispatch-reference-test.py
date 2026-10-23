#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify production dispatch/original Windows extraction, not copied models."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, native, module = sys.argv[1:]
original = (Path(upstream) / "HimaxAfe.cpp").read_bytes()
native_bytes = Path(native).read_bytes()
with tempfile.TemporaryDirectory(prefix="himax-afe-dispatch-") as directory:
    root = Path(directory)
    source = root / "HimaxAfe.cpp"
    source.write_bytes(original)
    native_source = root / "native.c"
    native_source.write_bytes(native_bytes)
    script = root / "prepare.cmake"
    script.write_text(f'include("{Path(module).as_posix()}")\n'
                     f'himax_prepare_afe_dispatch("{root.as_posix()}" '
                     f'"{native_source.as_posix()}" "{root.as_posix()}")\n')

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stderr
    windows = root / "afe-dispatch-windows.cpp"
    output = root / "afe-dispatch-native.c"
    generated = windows.read_text()
    native_generated = output.read_text()
    text = original.decode()
    assert generated.count(text[text.index("ChipResult<> AfeController::EnterIdle("):
                                text.index("// ── 手写笔生命周期")]) == 1
    text = native_bytes.decode()
    assert native_generated.count(text[text.index("int himax_afe_command("):
                                       text.index("#else\nint himax_restore_afe_runtime(")]) == 1
    for path in (windows, output):
        os.utime(path, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert all(path.stat().st_mtime_ns == 1_000_000_000 for path in (windows, output))
    source.write_bytes(original + b"\n// drift\n")
    result = prepare()
    assert result.returncode != 0 and "needs review" in result.stderr
    source.unlink()
    result = prepare()
    assert result.returncode != 0 and "Missing upstream" in result.stderr
    assert windows.read_text() == generated
    source.write_bytes(original)
    native_source.write_bytes(native_bytes.replace(b"0x06, 0x0c, 0x0e, 0x0a, 0x01",
                                                   b"0x06, 0x0d, 0x0e, 0x0a, 0x01"))
    assert prepare().returncode == 0
    assert output.read_text() != native_generated
print("Exact Windows/native AFE dispatch extraction, drift/missing guards, stable generation and native refresh passed")

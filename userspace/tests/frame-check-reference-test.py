#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guard the unchanged pinned CheckFrame oracle and its constants."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, module = sys.argv[1:]
original = (Path(upstream) / "HimaxChipFrame.cpp").read_bytes()
with tempfile.TemporaryDirectory(prefix="himax-frame-check-") as directory:
    root = Path(directory)
    source = root / "HimaxChipFrame.cpp"
    source.write_bytes(original)
    output = root / "reference.cpp"
    script = root / "prepare.cmake"
    script.write_text(f'include("{Path(module).as_posix()}")\n'
                      f'himax_prepare_frame_check("{root.as_posix()}" "{output.as_posix()}")\n')

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stderr
    text = original.decode()
    generated = output.read_text()
    body = text[text.index("enum class FrameCheck"):text.index("// 无效的 slave")]
    assert generated.count(body) == 1
    for name in ("kChecksumOffset", "kBusFiller"):
        line = next(line for line in text.splitlines() if line.startswith("constexpr") and name in line)
        assert generated.count(line) == 1
    os.utime(output, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0 and output.stat().st_mtime_ns == 1_000_000_000
    source.write_bytes(original + b"\n// source drift\n")
    result = prepare()
    assert result.returncode != 0 and "needs review" in result.stderr
    assert output.read_text() == generated
    source.unlink()
    result = prepare()
    assert result.returncode != 0 and "Missing upstream" in result.stderr
print("Pinned frame-check function/constants, source drift/missing guards and stable generation passed")

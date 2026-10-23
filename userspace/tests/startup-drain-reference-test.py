#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the startup oracle compiles current pinned source, not copied logic."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, module = sys.argv[1:]
original = {name: (Path(upstream) / name).read_bytes()
            for name in ("HimaxChipLifecycle.cpp", "HimaxChipFrame.cpp")}
with tempfile.TemporaryDirectory(prefix="himax-startup-drain-") as directory:
    root = Path(directory)
    for name, contents in original.items():
        (root / name).write_bytes(contents)
    output = root / "startup-drain-windows.cpp"
    script = root / "prepare.cmake"
    script.write_text(f'include("{Path(module).as_posix()}")\n'
                      f'himax_prepare_startup_drain("{root}" "{output}")\n')

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stderr
    generated = output.read_text()
    for name, start, end in (
        ("HimaxChipLifecycle.cpp", "    if (auto res = m_master->IntOpen(); !res)",
         '    m_lastInitFailStep = "";'),
        ("HimaxChipFrame.cpp", "ChipResult<> Chip::ApplyFrameReadPolicy()",
         "void Chip::ResetFrameReadState()"),
    ):
        text = original[name].decode()
        assert generated.count(text[text.index(start):text.index(end)]) == 1
    os.utime(output, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0 and output.stat().st_mtime_ns == 1_000_000_000
    for name, contents in original.items():
        source = root / name
        source.write_bytes(contents + b"\n// drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        assert output.read_text() == generated
        source.unlink()
        result = prepare()
        assert result.returncode != 0 and "Missing upstream" in result.stderr
        source.write_bytes(contents)
print("Exact Windows startup drain/read-policy extraction, stable generation and drift/missing guards passed")

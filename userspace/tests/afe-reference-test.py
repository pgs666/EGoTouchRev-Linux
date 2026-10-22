#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify test-only Windows reference extraction without modifying upstream."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, module = sys.argv[1:]
original = Path(upstream).read_bytes()

with tempfile.TemporaryDirectory(prefix="himax-afe-reference-") as directory:
    root = Path(directory)
    source = root / "HimaxProtocol.cpp"
    output = root / "nested" / "afe-reference.cpp"
    source.write_bytes(original)
    script = root / "prepare.cmake"
    script.write_text(
        f'include("{Path(module).as_posix()}")\n'
        f'himax_prepare_afe_reference("{source.as_posix()}" "{output.as_posix()}")\n'
    )

    def prepare():
        return subprocess.run(
            [cmake, "-P", str(script)], capture_output=True, text=True, timeout=10
        )

    result = prepare()
    assert result.returncode == 0, result.stdout + result.stderr
    assert source.read_bytes() == original, "upstream source must remain unchanged"
    contents = original.decode()
    start = contents.index("    void HimaxProtocol::build_command_packet(")
    end = contents.index("    ChipResult<> HimaxProtocol::write_and_verify(")
    generated = output.read_text()
    assert contents[start:end] in generated, "reference must use the exact upstream builder"
    assert 'extern "C" void hx_afe_reference_packet' in generated
    assert "ChipResult" not in generated, "Windows I/O must not enter the host test"
    # Give an unnecessary rewrite a distinct timestamp without a blocking wait.
    os.utime(output, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert output.stat().st_mtime_ns == 1_000_000_000, "no-op configure rewrote reference"

    source.write_bytes(original + b"\n// upstream drift\n")
    result = prepare()
    assert result.returncode != 0 and "needs review" in result.stderr
    assert output.read_text() == generated, "drift must not replace reviewed reference"
    source.unlink()
    result = prepare()
    assert result.returncode != 0 and "Missing upstream AFE source" in result.stderr

print("Exact AFE reference, source preservation, no-op configure and drift rejection passed")

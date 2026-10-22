#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the real CMake source integration and reject upstream drift."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import time

cmake, upstream, module = sys.argv[1:]
sources = ("TsaCoreSolver.cpp", "src/App/Sys.cpp",
           "src/Rejection/TouchReport/TouchReport.cpp", "src/Alg/TouchZone/Contour.cpp")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix="himax-overlay-") as directory:
    root = Path(directory)
    source, output = root / "source", root / "output"
    originals = {}
    for name in sources:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((Path(upstream) / name).read_bytes())
        originals[name] = sha(path)
    script = root / "prepare.cmake"
    script.write_text(
        f'include("{Path(module).as_posix()}")\n'
        f'himax_prepare_upstream("{source.as_posix()}" "{output.as_posix()}")\n'
    )

    def prepare():
        return subprocess.run(
            [cmake, "-P", str(script)], capture_output=True, text=True, timeout=10
        )

    result = prepare()
    assert result.returncode == 0, result.stdout + result.stderr
    for name in sources:
        assert sha(source / name) == originals[name], "submodule source was modified"
        text = (output / name).read_text()
        if name == sources[3]:
            assert text.startswith('#line 1 "')
            assert text.count(
                "if (first + k < 0 || first + k >= len) return std::clamp(first + k, 0, len - 1);"
            ) == 1
            continue
        hook, header = (
            ("::himax::RecoverEdgeOnset(curIdx, prevIdx, report);", "edge-onset.h")
            if name == sources[2] else ("::himax::ApplyHostScreenRotation();", "host-screen.h")
        )
        assert text.count(hook) == 1
        assert text.startswith(f'#include "{header}"\n#line 1 "')
    assert "ApplyPenSession();\n    ::himax::ApplyHostScreenRotation();" in (
        output / sources[0]
    ).read_text()
    assert "s.screenRotation = 0;\n    ::himax::ApplyHostScreenRotation();" in (
        output / sources[1]
    ).read_text()
    assert "if (replayed) u.pos = orig;\n    ::himax::RecoverEdgeOnset" in (
        output / sources[2]
    ).read_text()
    stamps = {name: (output / name).stat().st_mtime_ns for name in sources}
    time.sleep(0.02)
    assert prepare().returncode == 0
    assert all((output / name).stat().st_mtime_ns == stamps[name] for name in sources), (
        "unchanged configure must not force recompilation"
    )
    for name in sources:
        path = source / name
        original = path.read_bytes()
        path.write_bytes(original + b"\n// upstream drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        path.write_bytes(original)
    (source / sources[0]).unlink()
    result = prepare()
    assert result.returncode != 0 and "Missing upstream source" in result.stderr

print("Checked upstream integration, drift rejection and no-op configure passed")

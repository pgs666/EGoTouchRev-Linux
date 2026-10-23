#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check native lifecycle extraction and pinned Windows backoff source guards."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, runtime, chip, native, module = sys.argv[1:]
original = (Path(runtime) / "DeviceRuntime.cpp").read_bytes()
native_bytes = Path(native).read_bytes()
with tempfile.TemporaryDirectory(prefix="himax-start-reference-") as directory:
    root = Path(directory)
    source = root / "DeviceRuntime.cpp"
    source.write_bytes(original)
    for name in ("HimaxChip.cpp", "HimaxChip.h"):
        (root / name).write_bytes((Path(chip) / name).read_bytes())
    native_source = root / "native.c"
    native_source.write_bytes(native_bytes)
    script = root / "prepare.cmake"
    script.write_text(f'include("{Path(module).as_posix()}")\n'
                     f'himax_prepare_afe_start("{root.as_posix()}" '
                     f'"{root.as_posix()}" '
                     f'"{native_source.as_posix()}" "{root.as_posix()}")\n')

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stderr
    output = root / "afe-start-native.c"
    windows = root / "afe-start-windows.cpp"
    generated, reference = output.read_text(), windows.read_text()
    text = original.decode()
    assert reference.count(text[text.index("std::chrono::milliseconds BringUpBackoff("):
                                text.index("const char *StandbyReason(")]) == 1
    text = native_bytes.decode()
    startup = text[text.index("static void himax_start_or_retry("):text.index("static void himax_return_buffers(")]
    assert startup.index("hx_raw_can_start") < startup.index("himax_spi_read(ts, 0x13, &bus_value, 1)")
    assert startup.index("himax_spi_read(ts, 0x13") < startup.index("himax_start_afe(ts)")
    assert startup.index("ts->start_test_failures") < startup.index("himax_spi_read(ts, 0x13")
    for name, first, last in (
            ("HimaxChip.cpp", "Chip::BusProbe Chip::ProbeBus()", "void Chip::CancelPendingFrameRead()"),
            ("HimaxChip.h", "            struct BusProbe {", "            BusProbe ProbeBus();")):
        source_text = (Path(chip) / name).read_text()
        assert reference.count(source_text[source_text.index(first):source_text.index(last)]) == 1
    assert "module_param_named(afe_start_failures, himax_afe_start_failures, uint, 0400)" in text
    probe = text[text.index("static int himax_raw_probe("):text.index("static void himax_raw_remove(")]
    assert probe.index("himax_afe_start_failures > 8") < probe.index("kzalloc_obj")
    assert "ts->start_test_failures = himax_afe_start_failures" in probe
    for first, last in (("static int himax_start_afe(", "static void himax_return_buffers("),
                        ("static int himax_stream_start(", "static const struct vb2_ops")):
        assert generated.count(text[text.index(first):text.index(last)]) == 1
    for first, last in (("static int himax_panel_disabling(", "static int himax_panel_unpreparing("),
                        ("static int himax_raw_suspend(", "static int himax_raw_resume("),
                        ("static void himax_raw_shutdown(", "static void himax_v4l2_release(")):
        body = text[text.index(first):text.index(last)]
        assert body.index("hx_raw_retry_reset(&ts->retry)") < body.index("himax_unlock(ts)")
        assert body.index("himax_unlock(ts)") < body.index("cancel_delayed_work_sync")
    for path in (output, windows):
        os.utime(path, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert all(path.stat().st_mtime_ns == 1_000_000_000 for path in (output, windows))
    source.write_bytes(original + b"\n// drift\n")
    result = prepare()
    assert result.returncode != 0 and "needs review" in result.stderr
    source.unlink()
    result = prepare()
    assert result.returncode != 0 and "Missing upstream" in result.stderr
    assert windows.read_text() == reference
    source.write_bytes(original)
    native_source.write_bytes(native_bytes.replace(b"int i, ret;", b"int i, ret = 0;"))
    assert prepare().returncode == 0 and output.read_text() != generated
    for name in ("HimaxChip.cpp", "HimaxChip.h"):
        vendor = (root / name).read_bytes()
        (root / name).write_bytes(vendor + b"\n// drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        (root / name).write_bytes(vendor)
print("Exact native startup/STREAMON/STREAMOFF, pinned Windows backoff, stop wiring, drift and refresh passed")

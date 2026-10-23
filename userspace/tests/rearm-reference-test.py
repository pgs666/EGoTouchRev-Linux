#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify exact producer extraction, ioctl lock wiring and Windows hash guard."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, native, module = sys.argv[1:]
windows = (Path(upstream) / "HimaxChipFrame.cpp").read_bytes()
production = Path(native).read_bytes()
daemon = (Path(native).parent.parent / "userspace" / "touchd.cpp").read_text()
assert "afeStarted && readMode.IdleRetry(), experimentalRearm)" in daemon
assert daemon.index("const auto error = himax::ToThpMaster(") < daemon.index("producerRecovery.Frame(error ==")
assert daemon.index("producerRecovery.Frame(error ==") < daemon.index("frameHealth.Observe(error, fromRearm)")
assert daemon.index("const auto error = himax::ToThpMaster(") < daemon.index("readMode.Observe(error, thp, fromRearm);")
assert daemon.index("readMode.Observe(error, thp, fromRearm);") < daemon.index("frameHealth.Observe(error, fromRearm)")
assert daemon.index("const bool ok = !observe && capture.Command(") < daemon.index("readMode.Action(actionType, ok);")
assert daemon.index("readMode.Action(actionType, ok);") < daemon.index("solver->OnAfeActionExecuted(request, ok);")
state_reset = daemon[daemon.index("if (!state.running || state.epoch != epoch)"):
                     daemon.index("if (!capture.Error().empty())")]
assert "readMode.Reset();" in state_reset
epoch_reset = daemon[daemon.index("if (packet->epoch != epoch || !afeStarted)"):
                     daemon.index("solver->SetChargerPlugged(")]
assert "readMode.Reset();" in epoch_reset
assert "if (emptyRearm) producerRecovery.EmptyRearm(stream);" in daemon
assert "bool experimentalRearm = false;" in daemon
assert 'arg == "--experimental-rearm"' in daemon
assert "lastFrameValid" not in daemon
capture_source = (Path(native).parent.parent / "userspace" / "v4l2-capture.cpp").read_text()
assert "return ioctl(m_fd, VIDIOC_HIMAX_REARM_TAGGED, &request) == 0;" in capture_source
assert "const bool fromRearm = packet->flags & HX_RAW_FRAME_REARM;" in daemon
with tempfile.TemporaryDirectory(prefix="himax-rearm-reference-") as directory:
    root = Path(directory)
    (root / "HimaxChipFrame.cpp").write_bytes(windows)
    source = root / "native.c"
    source.write_bytes(production)
    script = root / "prepare.cmake"
    script.write_text(f'include("{Path(module).as_posix()}")\n'
                      f'himax_prepare_rearm("{source.as_posix()}" "{root}/native-out.c")\n'
                      f'himax_prepare_rearm_policy("{root}" "{root}/windows-out.cpp")\n'
                      f'himax_prepare_read_mode("{root}" "{root}/read-mode-out.cpp")\n')

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stdout + result.stderr
    generated = (root / "native-out.c").read_text()
    text = production.decode()
    for first, last in (("struct himax_video_buffer {", "static void himax_state_event("),
                        ("static int himax_capture_frame(", "static int himax_queue_setup(")):
        assert generated.count(text[text.index(first):text.index(last)]) == 1
    ioctl = text[text.index("static long himax_private_ioctl("):text.index("static const struct v4l2_ioctl_ops")]
    assert ioctl.index("mutex_lock(&ts->op_lock)") < ioctl.index("cmd == VIDIOC_HIMAX_REARM")
    assert "ret = himax_rearm(ts, file, arg, cmd == VIDIOC_HIMAX_REARM_TAGGED);" in ioctl
    assert ioctl.index("cmd == VIDIOC_HIMAX_REARM") < ioctl.index("cmd != VIDIOC_HIMAX_COMMAND")
    assert ioctl.index("himax_rearm(ts, file, arg,") < ioctl.index("mutex_unlock(&ts->op_lock)")
    expected = windows.decode()
    exact = expected[expected.index("    enum class Read :"):expected.index("    // ── slave（笔）──")]
    exact = exact.replace("std::this_thread::sleep_for", "TestSleep")
    assert (root / "windows-out.cpp").read_text().count(exact) == 1
    exact_mode = expected[expected.index("        const uint16_t idleFlag = suffix.idleFlag();"):
                          expected.index("        const THP_AFE_MODE mode =")]
    assert (root / "read-mode-out.cpp").read_text().count(exact_mode) == 1
    for path in (root / "native-out.c", root / "windows-out.cpp", root / "read-mode-out.cpp"):
        os.utime(path, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert all((root / name).stat().st_mtime_ns == 1_000_000_000
               for name in ("native-out.c", "windows-out.cpp", "read-mode-out.cpp"))
    source.write_bytes(production.replace(b"now <= ts->last_frame_ns", b"now < ts->last_frame_ns"))
    assert prepare().returncode == 0 and (root / "native-out.c").read_text() != generated
    source.write_bytes(production.replace(b"static int himax_rearm(", b"static int renamed_rearm("))
    # Capture extraction still contains the changed real body, not an old fixture.
    assert prepare().returncode == 0 and "renamed_rearm(" in (root / "native-out.c").read_text()
    source.write_bytes(production.replace(b"static int himax_capture_frame(", b"static int renamed_capture("))
    assert prepare().returncode != 0
    source.write_bytes(production)
    (root / "HimaxChipFrame.cpp").write_bytes(windows + b"\n// drift\n")
    result = prepare()
    assert result.returncode != 0 and "needs review" in result.stderr
print("Exact producer/IRQ extraction, locked ioctl wiring, refresh and pinned Windows policy guards passed")

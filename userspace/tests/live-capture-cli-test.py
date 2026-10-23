#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate arguments and fail-closed signal helper without touching a device."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

daemon, helper = sys.argv[1:]
result = subprocess.run([daemon, "--help"], capture_output=True, text=True, timeout=10)
assert result.returncode == 0 and "--live-capture-dir" in result.stderr
assert "--experimental-rearm" in result.stderr and "default off" in result.stderr
invalid = [["--live-capture-seconds", value] for value in
           ("0", "121", "-1", "1x", "999999999999999999999999", "1")]
invalid += [["--info", "--live-capture-dir", "/tmp"],
            ["--info", "--experimental-rearm"], ["--observe", "--experimental-rearm"],
            ["--record", "/tmp/not-created.hxst", "--live-capture-dir", "/tmp"],
            ["--live-capture-dir"], ["--live-capture-seconds"]]
for arguments in invalid:
    result = subprocess.run([daemon, *arguments], capture_output=True, text=True, timeout=10)
    assert result.returncode == 2, (arguments, result.stderr)

with tempfile.TemporaryDirectory(prefix="himax-live-cli-") as directory:
    root = Path(directory)
    mock = root / "mock.py"
    mock.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ["LIVE_MOCK_ROOT"])
if name == "sudo":
    assert args.pop(0) == "-n"
    os.execvp(args[0], args)
if name == "cat":
    assert args == ["/proc/4242/cmdline"]
    values = ["/usr/local/bin/himax-touchd"]
    if os.environ.get("LIVE_CONFIGURED") == "1":
        values += ["--live-capture-dir", "/var/lib/himax-touchd-captures"]
    sys.stdout.buffer.write(b"\\0".join(v.encode() for v in values) + b"\\0")
elif name == "systemctl":
    if args == ["is-active", "--quiet", "himax-touchd.service"]:
        sys.exit(0 if os.environ.get("LIVE_ACTIVE") == "1" else 1)
    if args == ["show", "--value", "-p", "MainPID", "himax-touchd.service"]:
        counter = root / "pid-reads"
        count = int(counter.read_text()) if counter.exists() else 0
        counter.write_text(str(count + 1))
        print("4343" if count and os.environ.get("LIVE_CHANGED") == "1" else "4242")
    elif args == ["kill", "--kill-whom=main", "--signal=USR2", "himax-touchd.service"]:
        (root / "signal").write_text("USR2")
    else:
        raise AssertionError(args)
else:
    raise AssertionError(name)
''')
    mock.chmod(0o755)
    for name in ("sudo", "cat", "systemctl"):
        (root / name).symlink_to(mock)
    for active, configured, changed, expected in ((0, 0, 0, 1), (1, 0, 0, 1),
                                                 (1, 1, 1, 1), (1, 1, 0, 0)):
        for name in ("signal", "pid-reads"):
            (root / name).unlink(missing_ok=True)
        env = {**os.environ, "PATH": str(root) + ":" + os.environ["PATH"],
               "LIVE_MOCK_ROOT": directory, "LIVE_ACTIVE": str(active),
               "LIVE_CONFIGURED": str(configured), "LIVE_CHANGED": str(changed)}
        result = subprocess.run(["bash", helper], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == expected, result.stderr
        assert (root / "signal").exists() == (expected == 0)
print("Live capture CLI bounds and no-signal gates for inactive/old/replaced services passed")

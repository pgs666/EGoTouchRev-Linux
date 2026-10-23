#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise actual source-only deployment gates with mocked kernel evidence."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

library = Path(sys.argv[1]).resolve()
cases = {
    "healthy": (0, 0, "normal transfer completed", True),
    "oot-only": (4096, 4096, "normal transfer completed", True),
    "existing-warning": (512, 512, "normal transfer completed", True),
    "existing-oops": (128, 128, "", False),
    "existing-bad-page": (32, 32, "", False),
    "new-oops-taint": (4096, 4224, "printk lost", False),
    "new-warn-taint": (0, 512, "printk lost", False),
    "new-bad-page": (0, 32, "printk lost", False),
    "oops-log-old-warn-taint": (512, 512, "Internal error: Oops: 96000004 [#1] SMP", False),
    "kasan": (0, 0, "BUG: KASAN: use-after-free in memcpy", False),
    "bad-address": (0, 0, "Unable to handle kernel paging request", False),
    "lockup": (0, 0, "watchdog: BUG: soft lockup - CPU#0 stuck", False),
    "rcu": (0, 0, "rcu: INFO: rcu_preempt detected stalls on CPUs/tasks", False),
    "warning": (0, 0, "WARNING: CPU: 0 PID: 1 at gpi_free_chan_resources", False),
    "unrelated-normal-dma": (0, 0, "DMA: preallocated pool initialized", True),
    "no-cursor": (0, 0, "", False),
    "journal-start-error": (0, 0, "", False),
    "journal-after-error": (0, 0, "", False),
    "boot-changed": (0, 0, "", False),
    "invalid-taint": ("bad", 0, "", False),
}
for name, (before, after, log, success) in cases.items():
    with tempfile.TemporaryDirectory(prefix="himax-kernel-health-") as directory:
        # The real library runs unchanged; mocks replace only proc/journal I/O.
        script = r'''
set -Eeuo pipefail
source "$1"
state=$2
fixture_stage=before
himax_kernel_boot_id() {
    if [[ "$CASE" == boot-changed && "$fixture_stage" == after ]]; then
        echo new-boot
    else echo old-boot; fi
}
himax_kernel_taint() {
    if [[ "$fixture_stage" == before ]]; then echo "$BEFORE"; else echo "$AFTER"; fi
}
himax_kernel_journal_start() {
    [[ "$CASE" != journal-start-error ]] || return 1
    echo "old message outside test window"
    if [[ "$CASE" != no-cursor ]]; then echo "-- cursor: test-cursor"; fi
}
himax_kernel_journal_after() {
    [[ "$1" == test-cursor ]] || return 99
    [[ "$CASE" != journal-after-error ]] || return 1
    echo "$LOG"
}
himax_kernel_health_start "$state"
fixture_stage=after
himax_kernel_health_check "$state" final
'''
        environment = dict(os.environ, CASE=name, BEFORE=str(before), AFTER=str(after), LOG=log)
        result = subprocess.run(["bash", "-c", script, "health-test", str(library), directory],
                                env=environment, capture_output=True, text=True, timeout=10)
        assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
        if success:
            assert (Path(directory) / "kernel-final.log").is_file(), name
        if name == "oops-log-old-warn-taint":
            assert "Oops" in (Path(directory) / "kernel-final.faults").read_text()
print(f"Kernel health gate: {len(cases)} cursor/taint/log/boot/read-failure cases passed")

# Run the actual EXIT handler, replacing only health checks and restoration.
# A successful trial must include both restoration and its final health check.
deployment = library.parent.parent / "deploy-v4l2-local.sh"
source = deployment.read_text()
finish = source[source.index("finish() {"):source.index("\ntrap finish EXIT")]
finish_cases = [
    ("install", 0, 0, "", 0, True),
    ("trial", 0, 1, "", 0, True),
    ("failed-command", 2, 1, "", 0, False),
    ("finish-health", 0, 1, "finish", 0, False),
    ("restore-health", 0, 1, "restored", 0, False),
    ("restore-failed", 0, 1, "", 1, False),
]
for name, initial_status, changed, failed_phase, restore_status, success in finish_cases:
    script = r'''
set -Eeuo pipefail
state_dir=mock-checkpoint
kernel_checkpoint_ready=1
kernel_unhealthy=0
changed=$CHANGED
success_message=DEPLOYMENT_SUCCESS
himax_kernel_health_check() {
    echo "check:$2"
    [[ "$2" != "$FAILED_PHASE" ]]
}
restore() { echo restore; return "$RESTORE_STATUS"; }
''' + finish + r'''
trap finish EXIT
exit "$INITIAL_STATUS"
'''
    environment = dict(os.environ, CHANGED=str(changed), FAILED_PHASE=failed_phase,
                       RESTORE_STATUS=str(restore_status), INITIAL_STATUS=str(initial_status))
    result = subprocess.run(["bash", "-c", script], env=environment,
                            capture_output=True, text=True, timeout=10)
    assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
    assert ("DEPLOYMENT_SUCCESS" in result.stdout) == success, (name, result.stdout)
    if name == "trial":
        assert result.stdout.splitlines() == [
            "check:finish", "restore", "check:restored", "DEPLOYMENT_SUCCESS"
        ], result.stdout
    if name == "finish-health":
        assert "restore" not in result.stdout, result.stdout
print(f"Deployment EXIT handler: {len(finish_cases)} success/failure/restoration cases passed")

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare real raw->solver->mapper entry events with/without the report hook."""
import re
import subprocess
import sys

fixed, reference = sys.argv[1:]


def trace(binary, rotation, edge, speed, drift, initial=-1):
    result = subprocess.run(
        [binary, str(rotation), str(edge), str(speed), str(drift), str(initial)],
        text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 0, result.stderr
    events = []
    for line in result.stdout.splitlines():
        if not line.startswith("f="):
            continue
        frame = int(re.match(r"f=(\d+)", line)[1])
        for event, x, y in re.findall(r"OUT\((\d+)\)=([\d.]+),([\d.]+)", line):
            ids = re.findall(r"\{id=(\d+) ev=", line)
            assert len(ids) == 1, line
            events.append((frame, int(event), int(ids[0]), float(x), float(y)))
    assert events and events[0][1] == 2, result.stdout
    return events


repaired = 0
for rotation in range(4):
    for edge in range(4):
        for speed in (1.5, 2, 2.5, 3, 4):
            for drift in (0, 0.15, -0.15):
                old = trace(reference, rotation, edge, speed, drift)
                new = trace(fixed, rotation, edge, speed, drift)
                assert [v[:3] for v in old] == [v[:3] for v in new], (
                    "correction changed admission/release frame or ID", rotation, edge, speed, drift
                )
                x, y = new[0][3:]
                distance = (x, 2559.9 - x, y, 1599.9 - y)[edge]
                assert -0.1 <= distance <= 1, (rotation, edge, speed, drift, new[0])
                ox, oy = old[0][3:]
                old_distance = (ox, 2559.9 - ox, oy, 1599.9 - oy)[edge]
                repaired += old_distance > 10
assert repaired >= 32, "control must reproduce missed edge recovery"
old = trace(reference, 3, 2, 3, 0)
new = trace(fixed, 3, 2, 3, 0)
assert old[0][4] > 200 and new[0][4] <= 1, "previously uncovered narrow-band miss"

# Same narrow-band miss as the bug, but the raw starts INSIDE: there is no
# preceding boundary pulse. The complete trajectory must stay upstream.
for rotation in range(4):
    for initial in (2, 3, 4):
        old = trace(reference, rotation, 2, 3, 0, initial)
        new = trace(fixed, rotation, 2, 3, 0, initial)
        assert new == old and new[0][4] > 200, (rotation, initial, old, new)

print("240 edge sequences: onset frame, release and ID unchanged; edge coordinates recovered")
print("12 interior starts outside the narrow band: full upstream trajectory preserved")

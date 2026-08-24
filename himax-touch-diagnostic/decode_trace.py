#!/usr/bin/env python3
"""Decode the binary event_trace snapshots exported by himax-spi."""

import argparse
import csv
import struct
import sys
from pathlib import Path


MAGIC = 0x52545848
HEADER = struct.Struct("<IHHIIIIQQ")
PREFIX = struct.Struct("<QIIIiIi h 8B")
PEAK_V1 = struct.Struct("<BBhHBB")
TRACK_V1 = struct.Struct("<BBBB9i")
PEAK_V2 = struct.Struct("<BBhHBBBB")
TRACK_V2 = struct.Struct("<BBBBBB9i")
PEAK_COUNT = 20
TRACK_COUNT = 10
PROCESSED_PIXELS = 2400
EVENT_STACK_SIZE = 5132

FLAG_READ_OK = 1 << 0
FLAG_MASTER_VALID = 1 << 1
FLAG_RETRY = 1 << 2
FLAG_HAS_FINGER = 1 << 3
FLAG_NOISE_HOLD = 1 << 4


def decode(path: Path):
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("file is shorter than the trace header")
    (magic, version, header_size, record_size, capacity, count, _reserved,
     snapshot_ns, reset_generation) = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    if version == 1:
        peak_struct, track_struct = PEAK_V1, TRACK_V1
    elif version in (2, 3):
        peak_struct, track_struct = PEAK_V2, TRACK_V2
    else:
        raise ValueError(f"unsupported format version={version}")
    expected = (PREFIX.size + PEAK_COUNT * peak_struct.size +
                TRACK_COUNT * track_struct.size + PROCESSED_PIXELS * 2 +
                EVENT_STACK_SIZE)
    if header_size != HEADER.size or record_size != expected:
        raise ValueError(
            f"unsupported format version={version} header={header_size} "
            f"record={record_size}, expected record={expected}")
    required = header_size + count * record_size
    if len(data) < required:
        raise ValueError(f"truncated snapshot: {len(data)} < {required}")

    rows = []
    for index in range(count):
        base = header_size + index * record_size
        values = PREFIX.unpack_from(data, base)
        (boottime_ns, irq_sequence, reset_gen, flags, read_error,
         algo_frame, common_diff, frame_max, has_signal, zones, peaks,
         contacts_pre, contacts_post, active, reported, _reserved) = values

        peak_offset = base + PREFIX.size
        peak_items = []
        for peak_index in range(min(peaks, PEAK_COUNT)):
            peak_values = peak_struct.unpack_from(
                data, peak_offset + peak_index * peak_struct.size)
            row, col, signal, area, zone_index, peak_flags = peak_values[:6]
            peak_id, peak_age = peak_values[6:8] if version >= 2 else (0, 0)
            peak_items.append(
                f"{row}:{col}:{signal}:{area}:{zone_index}:{peak_flags}:"
                f"{peak_id}:{peak_age}")

        track_offset = peak_offset + PEAK_COUNT * peak_struct.size
        track_items = []
        for slot in range(TRACK_COUNT):
            track = track_struct.unpack_from(
                data, track_offset + slot * track_struct.size)
            track_flags, age, missed, debounce = track[:4]
            if version >= 2:
                source_peak_id, source_peak_age = track[4:6]
                values_offset = 6
            else:
                source_peak_id, source_peak_age = 0, 0
                values_offset = 4
            if not track_flags:
                continue
            x, y, vx, vy, signal_sum = track[values_offset:values_offset + 5]
            track_items.append(
                f"{slot}:{track_flags}:{x}:{y}:{vx}:{vy}:"
                f"{signal_sum}:{age}:{missed}:{debounce}:"
                f"{source_peak_id}:{source_peak_age}")

        rows.append({
            "source": path.name,
            "index": index,
            "boottime_ns": boottime_ns,
            "irq_sequence": irq_sequence,
            "reset_generation": reset_gen,
            "read_ok": int(bool(flags & FLAG_READ_OK)),
            "master_valid": int(bool(flags & FLAG_MASTER_VALID)),
            "retry": int(bool(flags & FLAG_RETRY)),
            "has_finger": int(bool(flags & FLAG_HAS_FINGER)),
            "noise_hold": int(bool(flags & FLAG_NOISE_HOLD)),
            "read_error": read_error,
            "algo_frame": algo_frame,
            "common_diff": common_diff,
            "frame_max": frame_max,
            "has_signal": has_signal,
            "zones": zones,
            "peaks": peaks,
            "contacts_pre": contacts_pre,
            "contacts_post": contacts_post,
            "active_tracks": active,
            "reported_tracks": reported,
            "peak_details": ";".join(peak_items),
            "track_details": ";".join(track_items),
        })
    return rows, capacity, snapshot_ns, reset_generation


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert one or more Himax event_trace snapshots to CSV")
    parser.add_argument("trace", nargs="+", type=Path)
    parser.add_argument("-o", "--output", type=Path,
                        help="CSV output (default: stdout)")
    args = parser.parse_args()

    all_rows = []
    for path in args.trace:
        try:
            rows, capacity, snapshot_ns, reset_generation = decode(path)
        except (OSError, ValueError) as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            return 2
        all_rows.extend(rows)
        print(f"{path}: records={len(rows)}/{capacity} "
              f"snapshot_ns={snapshot_ns} reset_generation={reset_generation}",
              file=sys.stderr)

    if not all_rows:
        print("no trace records", file=sys.stderr)
        return 0

    output = args.output.open("w", newline="", encoding="utf-8") \
        if args.output else sys.stdout
    try:
        writer = csv.DictWriter(output, fieldnames=list(all_rows[0].keys()))
        writer.writeheader()
        writer.writerows(all_rows)
    finally:
        if args.output:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

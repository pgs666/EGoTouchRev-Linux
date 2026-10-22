#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Publish KDE's internal-panel rotation; never run inside the touch loop."""

import argparse
import json
import signal
import socket
import subprocess
import sys
import threading


def panel_rotation(config, connector):
    if not isinstance(config, dict):
        raise ValueError("KScreen configuration is not an object")
    outputs = config.get("outputs", [])
    if not isinstance(outputs, list):
        raise ValueError("KScreen outputs is not a list")
    panels = [
        output for output in outputs
        if isinstance(output, dict) and output.get("name") == connector
        and output.get("enabled") is True and output.get("connected") is True
    ]
    if len(panels) != 1:
        raise ValueError(f"expected one enabled, connected {connector}")
    rotation = panels[0].get("rotation")
    if type(rotation) is not int or rotation not in (1, 2, 4, 8):
        raise ValueError("unsupported KScreen rotation/reflection")
    return rotation


def publish(rotation, address="\0himax-touchd-screen-state"):
    # The kernel attaches real sender credentials; the root service accepts
    # only root or the user currently owning seat0.
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as connection:
        connection.settimeout(0.1)
        connection.sendto(f"kscreen={rotation}\n".encode("ascii"), address)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connector", default="DSI-1")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    stopped = threading.Event()
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, lambda *_: stopped.set())
    last_message = None
    while not stopped.is_set():
        try:
            result = subprocess.run(
                ["kscreen-doctor", "--json"], check=True, capture_output=True,
                text=True, timeout=2,
            )
            rotation = panel_rotation(json.loads(result.stdout), args.connector)
            # Repeat unchanged state so a restarted daemon learns it too.
            publish(rotation)
            message = f"{args.connector}: kscreen={rotation}"
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            message = f"screen orientation unavailable: {error}"
            if message != last_message:
                print(message, file=sys.stderr, flush=True)
            last_message = message
            if args.once:
                return 1
        else:
            if message != last_message:
                print(message, flush=True)
            last_message = message
            if args.once:
                return 0
        stopped.wait(1)
    return 0


if __name__ == "__main__":
    sys.exit(main())

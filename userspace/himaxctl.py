#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Operator commands handled by the active touchscreen daemon."""
import argparse
import os
import secrets
import socket
import sys


def main():
    parser = argparse.ArgumentParser(description="Control the running Himax touch daemon")
    parser.add_argument("command", choices=("status", "calibrate", "reset"))
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Run with sudo; the control socket is root-only")
    # An abstract return address avoids creating or cleaning client files.
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
        client.bind("\0himaxctl-" + secrets.token_hex(12))
        client.settimeout(3)
        try:
            client.connect("/run/himax-touchd/control.sock")
            client.send(args.command.encode("ascii"))
            response = client.recv(4096).decode("utf-8")
        except (OSError, UnicodeError) as error:
            print(f"himaxctl: {error}", file=sys.stderr)
            return 1
    print(response, end="" if response.endswith("\n") else "\n")
    return 0 if response.startswith("OK ") else 1


if __name__ == "__main__":
    sys.exit(main())

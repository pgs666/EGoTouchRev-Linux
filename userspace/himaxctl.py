#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Operator commands handled by the active touchscreen daemon."""
import argparse
import os
import secrets
import socket
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="Control the running Himax touch daemon")
    parser.add_argument("command", choices=("status", "calibrate", "reset"))
    parser.add_argument("--wait", action="store_true",
                        help="wait for a calibration completion marker or a new running reset epoch")
    args = parser.parse_args()
    if args.wait and args.command == "status":
        parser.error("--wait applies to calibrate or reset")
    if os.geteuid() != 0:
        parser.error("Run with sudo; the control socket is root-only")
    # An abstract return address avoids creating or cleaning client files.
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
        client.bind("\0himaxctl-" + secrets.token_hex(12))
        client.settimeout(3)
        try:
            client.connect("/run/himax-touchd/control.sock")
            def request(command):
                client.send(command.encode("ascii"))
                return client.recv(4096).decode("utf-8")

            def fields(response):
                return dict(item.split("=", 1) for item in response.split() if "=" in item)

            prior_epoch = None
            if args.wait and args.command == "reset":
                status = request("status")
                if not status.startswith("OK "):
                    print(status, file=sys.stderr)
                    return 1
                prior_epoch = int(fields(status)["epoch"])
            response = request(args.command)
            if args.wait and response.startswith("OK "):
                print(response, flush=True)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    time.sleep(0.2)
                    response = request("status")
                    if not response.startswith("OK "):
                        break
                    state = fields(response)
                    if args.command == "calibrate":
                        if state.get("calibration") == "done":
                            print("OK calibration completion marker observed")
                            return 0
                        if state.get("calibration") in ("timeout", "interrupted", "failed"):
                            print("ERR calibration " + state["calibration"], file=sys.stderr)
                            return 1
                    elif (state.get("running") == "1" and int(state["epoch"]) > prior_epoch
                          and state.get("valid_epoch") == state["epoch"]
                          and int(state["valid_sequence"]) > 0):
                        print("OK new AFE epoch received data: " + response.removeprefix("OK "))
                        return 0
                else:
                    print("ERR no confirmed result before ten-second deadline", file=sys.stderr)
                    return 1
        except (OSError, UnicodeError, KeyError, ValueError) as error:
            print(f"himaxctl: {error}", file=sys.stderr)
            return 1
    print(response, end="" if response.endswith("\n") else "\n")
    return 0 if response.startswith("OK ") else 1


if __name__ == "__main__":
    sys.exit(main())

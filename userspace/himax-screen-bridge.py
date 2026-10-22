#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Publish KDE's internal-panel rotation; never run inside the touch loop."""

import argparse
import array
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time


ROTATION_ADDRESS = "\0himax-touchd-screen-state"
REQUEST_PREFIX = "\0himax-screen-bridge-"
REQUEST = b"HXSR 1\n"
CACHE_MAX_AGE = 3.0
CREDENTIALS = struct.Struct("iII")


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


def publish(rotation, address=ROTATION_ADDRESS):
    # The kernel attaches real sender credentials; the root service accepts
    # only root or the user currently owning seat0.
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as connection:
        connection.settimeout(0.1)
        connection.sendto(f"kscreen={rotation}\n".encode("ascii"), address)


class RotationCache:
    """Only reuse a successful, recent desktop query; serialize publications."""

    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self._lock = threading.Lock()
        self._rotation = None
        self._updated = 0.0

    def update(self, rotation):
        with self._lock:
            self._rotation = rotation
            self._updated = self._clock()

    def clear(self):
        with self._lock:
            self._rotation = None

    def publish(self, address=ROTATION_ADDRESS):
        # Keep the lock through the bounded send so a cached response cannot
        # overtake a newer periodic publication and restore an old direction.
        with self._lock:
            age = self._clock() - self._updated
            if self._rotation is None or not 0 <= age <= CACHE_MAX_AGE:
                return False
            publish(self._rotation, address)
            return True


class RequestServer:
    """Answer authenticated startup requests from cache, never by querying KDE."""

    def __init__(self, address=None, receiver=ROTATION_ADDRESS):
        if address is None:
            address = REQUEST_PREFIX + str(os.getuid())
        self.receiver = receiver
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            self.connection.setblocking(False)
            self.connection.bind(address)
        except BaseException:
            self.connection.close()
            raise

    def close(self):
        self.connection.close()

    def respond_batch(self, cache):
        replies = 0
        for _ in range(16):
            try:
                data, ancillary, flags, peer = self.connection.recvmsg(
                    64, socket.CMSG_SPACE(CREDENTIALS.size) + socket.CMSG_SPACE(16),
                    socket.MSG_DONTWAIT | socket.MSG_CMSG_CLOEXEC,
                )
            except BlockingIOError:
                break
            uid = None
            unexpected = False
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    # recvmsg installs descriptors even for a rejected packet.
                    descriptors = array.array("i")
                    count = len(payload) // descriptors.itemsize
                    descriptors.frombytes(payload[:count * descriptors.itemsize])
                    for descriptor in descriptors:
                        os.close(descriptor)
                    unexpected = True
                elif (level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS
                      and len(payload) == CREDENTIALS.size):
                    _, uid, _ = CREDENTIALS.unpack(payload)
                else:
                    unexpected = True
            if (unexpected or flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
                    or data != REQUEST or uid not in (0, os.getuid())
                    or not isinstance(peer, (str, bytes))
                    or os.fsencode(peer) != os.fsencode(self.receiver)):
                continue
            try:
                replies += cache.publish(self.receiver)
            except OSError:
                # The daemon may have exited between request and response.
                pass
        return replies


def query_rotation(connector):
    result = subprocess.run(
        ["kscreen-doctor", "--json"], check=True, capture_output=True,
        text=True, timeout=2,
    )
    return panel_rotation(json.loads(result.stdout), connector)


def poll_updates(connector, cache, stopped, *, once=False, receiver=ROTATION_ADDRESS):
    last_message = None
    while not stopped.is_set():
        try:
            rotation = query_rotation(connector)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            cache.clear()
            message = f"screen orientation unavailable: {error}"
            succeeded = False
        else:
            cache.update(rotation)
            # Repeat unchanged state so a restarted daemon learns it too.
            # A missing receiver must not invalidate a successful desktop query.
            try:
                succeeded = cache.publish(receiver)
                message = f"{connector}: kscreen={rotation}"
            except OSError as error:
                message = f"screen orientation unavailable: {error}"
                succeeded = False
        if message != last_message:
            print(message, file=sys.stdout if succeeded else sys.stderr, flush=True)
        last_message = message
        if once:
            return 0 if succeeded else 1
        stopped.wait(1)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connector", default="DSI-1")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    stopped = threading.Event()
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, lambda *_: stopped.set())
    cache = RotationCache()
    if args.once:
        return poll_updates(args.connector, cache, stopped, once=True)
    # Bind before the worker's first potentially slow desktop query. Requests
    # only read the cache; no D-Bus or subprocess runs on this receiving thread.
    server = RequestServer()
    worker = threading.Thread(
        target=poll_updates, args=(args.connector, cache, stopped), daemon=True,
    )
    worker.start()
    try:
        while not stopped.is_set():
            if not worker.is_alive():
                raise RuntimeError("desktop query worker exited")
            if select.select([server.connection], [], [], 0.25)[0]:
                server.respond_batch(cache)
                # Bound processing even under a continuous datagram flood.
                stopped.wait(0.25)
    finally:
        stopped.set()
        server.close()
        worker.join(timeout=3)
    return 0


if __name__ == "__main__":
    sys.exit(main())

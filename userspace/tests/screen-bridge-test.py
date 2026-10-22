# SPDX-License-Identifier: MIT
import array
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "screen_bridge", Path(__file__).resolve().parents[1] / "himax-screen-bridge.py"
)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


@contextlib.contextmanager
def endpoints(*, abstract=False):
    with tempfile.TemporaryDirectory(prefix="himax-screen-") as directory:
        receiver = str(Path(directory) / "receiver")
        request = str(Path(directory) / "request")
        if abstract:
            receiver = "\0" + receiver
            request = "\0" + request
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
            client.bind(receiver)
            client.settimeout(0.05)
            server = bridge.RequestServer(request, receiver)
            try:
                yield client, server, request, receiver
            finally:
                server.close()


class ScreenBridgeTest(unittest.TestCase):
    def config(self, rotation=8):
        return {
            "outputs": [
                {"name": "DP-1", "enabled": True, "connected": True, "rotation": 2},
                {"name": "DSI-1", "enabled": True, "connected": True, "rotation": rotation},
            ]
        }

    def test_four_rotations(self):
        for rotation in (1, 2, 4, 8):
            config = json.loads(json.dumps(self.config(rotation)))
            self.assertEqual(bridge.panel_rotation(config, "DSI-1"), rotation)

    def test_reject_ambiguous_or_absent_panel(self):
        for config in (None, [], 8, "bad", {}, {"outputs": None}, {"outputs": []}):
            with self.assertRaises(ValueError):
                bridge.panel_rotation(config, "DSI-1")
        config = self.config()
        config["outputs"].append(dict(config["outputs"][1]))
        with self.assertRaises(ValueError):
            bridge.panel_rotation(config, "DSI-1")
        for field in ("enabled", "connected"):
            config = self.config()
            config["outputs"][1][field] = False
            with self.assertRaises(ValueError):
                bridge.panel_rotation(config, "DSI-1")

    def test_reject_reflections_or_bad_values(self):
        for rotation in (0, 3, 16, 32, 64, 128, 8.0, True, None, "8"):
            with self.assertRaises(ValueError):
                bridge.panel_rotation(self.config(rotation), "DSI-1")

    def test_datagram_publish(self):
        with tempfile.TemporaryDirectory(prefix="himax-screen-") as directory:
            address = str(Path(directory) / "socket")
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as server:
                server.bind(address)
                server.settimeout(1)
                bridge.publish(8, address)
                bridge.publish(2, address)
                self.assertEqual(server.recv(64), b"kscreen=8\n")
                self.assertEqual(server.recv(64), b"kscreen=2\n")

    def test_request_uses_fresh_cache_and_both_address_forms(self):
        for abstract in (False, True):
            with self.subTest(abstract=abstract), endpoints(abstract=abstract) as pair:
                client, server, request, receiver = pair
                now = [10.0]
                cache = bridge.RotationCache(clock=lambda: now[0])
                client.sendto(bridge.REQUEST, request)
                self.assertEqual(server.respond_batch(cache), 0)
                with self.assertRaises(socket.timeout):
                    client.recv(64)
                cache.update(8)
                client.sendto(bridge.REQUEST, request)
                self.assertEqual(server.respond_batch(cache), 1)
                self.assertEqual(client.recv(64), b"kscreen=8\n")
                now[0] += bridge.CACHE_MAX_AGE + 0.01
                client.sendto(bridge.REQUEST, request)
                self.assertEqual(server.respond_batch(cache), 0)
                cache.update(2)
                self.assertTrue(cache.publish(receiver))
                self.assertEqual(client.recv(64), b"kscreen=2\n")
                now[0] -= 1
                self.assertFalse(cache.publish(receiver), "clock rollback rejected")
                cache.update(4)
                cache.clear()
                self.assertFalse(cache.publish(receiver), "failed query clears cache")

    def test_bad_request_and_wrong_source_rejected(self):
        with endpoints(abstract=True) as pair:
            client, server, request, _ = pair
            cache = bridge.RotationCache()
            cache.update(8)
            for payload in (b"", b"HXSR 2\n", b"HXSR 1\njunk", b"X" * 128):
                client.sendto(payload, request)
                self.assertEqual(server.respond_batch(cache), 0)
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as other:
                # Even root/self must use the configured receiver as source.
                other.sendto(bridge.REQUEST, request)
                self.assertEqual(server.respond_batch(cache), 0)
                other.bind(request + "-wrong-source")
                other.sendto(bridge.REQUEST, request)
                self.assertEqual(server.respond_batch(cache), 0)
            with self.assertRaises(socket.timeout):
                client.recv(64)

    def test_received_descriptors_closed_and_rejected(self):
        with endpoints(abstract=True) as pair, open("/dev/null", "rb") as descriptor:
            client, server, request, _ = pair
            cache = bridge.RotationCache()
            cache.update(8)
            for count in (1, 16):
                before = len(list(Path("/proc/self/fd").iterdir()))
                client.sendmsg(
                    [bridge.REQUEST],
                    [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                      array.array("i", [descriptor.fileno()] * count))], 0, request,
                )
                self.assertEqual(server.respond_batch(cache), 0)
                self.assertEqual(len(list(Path("/proc/self/fd").iterdir())), before)
            with self.assertRaises(socket.timeout):
                client.recv(64)

    @unittest.skipUnless(os.getuid() == 0, "genuine foreign-UID sender requires root")
    def test_kernel_credentials_reject_other_uid(self):
        with endpoints(abstract=True) as pair:
            client, server, request, _ = pair
            cache = bridge.RotationCache()
            cache.update(8)
            pid = os.fork()
            if pid == 0:
                try:
                    os.setgid(65534)
                    os.setuid(65534)
                    client.sendto(bridge.REQUEST, request)
                except BaseException:
                    os._exit(2)
                os._exit(0)
            _, status = os.waitpid(pid, 0)
            self.assertEqual(os.waitstatus_to_exitcode(status), 0)
            self.assertEqual(server.respond_batch(cache), 0)
            with self.assertRaises(socket.timeout):
                client.recv(64)

    def test_query_failure_clears_cache_but_absent_daemon_does_not(self):
        with endpoints() as pair:
            client, _, _, receiver = pair
            cache = bridge.RotationCache()
            stopped = threading.Event()
            with mock.patch.object(bridge, "query_rotation", return_value=8), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(bridge.poll_updates(
                    "DSI-1", cache, stopped, once=True, receiver=receiver + "-missing",
                ), 1)
            self.assertTrue(cache.publish(receiver))
            self.assertEqual(client.recv(64), b"kscreen=8\n")
            with mock.patch.object(bridge, "query_rotation", side_effect=ValueError("offline")), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(bridge.poll_updates(
                    "DSI-1", cache, stopped, once=True, receiver=receiver,
                ), 1)
            self.assertFalse(cache.publish(receiver))

    def test_cached_response_does_not_wait_for_query(self):
        entered = threading.Event()
        release = threading.Event()
        stopped = threading.Event()

        def slow_query(_):
            entered.set()
            if not release.wait(2):
                raise RuntimeError("test did not release desktop query")
            return 4

        with endpoints(abstract=True) as pair:
            client, server, request, receiver = pair
            cache = bridge.RotationCache()
            cache.update(8)
            with mock.patch.object(bridge, "query_rotation", side_effect=slow_query) as query, \
                    contextlib.redirect_stdout(io.StringIO()):
                worker = threading.Thread(
                    target=bridge.poll_updates, args=("DSI-1", cache, stopped),
                    kwargs={"once": True, "receiver": receiver},
                )
                worker.start()
                try:
                    self.assertTrue(entered.wait(1))
                    client.sendto(bridge.REQUEST, request)
                    self.assertEqual(server.respond_batch(cache), 1)
                    self.assertEqual(client.recv(64), b"kscreen=8\n")
                    self.assertEqual(query.call_count, 1)
                    self.assertFalse(release.is_set())
                finally:
                    stopped.set()
                    release.set()
                    worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(client.recv(64), b"kscreen=4\n")

    def test_once_does_not_bind_request_endpoint(self):
        with mock.patch.object(bridge.sys, "argv", ["bridge", "--once"]), \
                mock.patch.object(bridge.signal, "signal"), \
                mock.patch.object(bridge, "query_rotation", return_value=2), \
                mock.patch.object(bridge, "publish") as publish, \
                mock.patch.object(bridge, "RequestServer",
                                  side_effect=AssertionError("once must not bind")), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bridge.main(), 0)
            publish.assert_called_once_with(2, bridge.ROTATION_ADDRESS)
        with mock.patch.object(bridge.sys, "argv", ["bridge", "--once"]), \
                mock.patch.object(bridge.signal, "signal"), \
                mock.patch.object(bridge, "query_rotation", side_effect=ValueError("bad")), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bridge.main(), 1)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "screen_bridge", Path(__file__).resolve().parents[1] / "himax-screen-bridge.py"
)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


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


if __name__ == "__main__":
    unittest.main()

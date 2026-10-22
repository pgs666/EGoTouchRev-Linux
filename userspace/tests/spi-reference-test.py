#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify exact Windows/native bodies, source guards and stable generation."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, native, module = sys.argv[1:]
originals = {name: (Path(upstream) / name).read_bytes()
             for name in ("HimaxProtocol.cpp", "HimaxProtocol.h")}
native_bytes = Path(native).read_bytes()

with tempfile.TemporaryDirectory(prefix="himax-spi-reference-") as directory:
    root = Path(directory)
    source = root / "source"
    source.mkdir()
    for name, contents in originals.items():
        (source / name).write_bytes(contents)
    native_path = root / "native.c"
    native_path.write_bytes(native_bytes)
    output = root / "generated"
    script = root / "prepare.cmake"
    script.write_text(
        f'include("{Path(module).as_posix()}")\n'
        f'himax_prepare_spi_reference("{source.as_posix()}" '
        f'"{native_path.as_posix()}" "{output.as_posix()}")\n'
    )

    def prepare():
        return subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True, timeout=10)

    result = prepare()
    assert result.returncode == 0, result.stdout + result.stderr
    generated = (output / "spi-reference.cpp").read_text()
    native_generated = (output / "spi-native.c").read_text()
    text = originals["HimaxProtocol.cpp"].decode()
    for first, last in (
        ("    ChipResult<> HalDevice::IoctlOnce(",
         "    /**\n     * @brief 从设备读取原始数据"),
        ("    ChipResult<> HalDevice::ReadBus(",
         "    /**\n     * @brief 读取 ACPI 配置数据"),
    ):
        assert generated.count(text[text.index(first):text.index(last)]) == 1
    text = native_bytes.decode()
    for first, last in (
        ("int himax_spi_read(",
         "#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2\nstatic int himax_ahb_write("),
        ("static int himax_ahb_write(", "static int himax_ahb_read("),
    ):
        assert native_generated.count(text[text.index(first):text.index(last)]) == 1
    for path in output.iterdir():
        os.utime(path, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert all(path.stat().st_mtime_ns == 1_000_000_000 for path in output.iterdir())
    for name, contents in originals.items():
        path = source / name
        path.write_bytes(contents + b"\n// drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        assert (output / "spi-reference.cpp").read_text() == generated
        path.unlink()
        result = prepare()
        assert result.returncode != 0 and "Missing upstream" in result.stderr
        path.write_bytes(contents)
    # Native changes must be re-extracted, not tested against a stale snapshot.
    native_path.write_bytes(native_bytes.replace(b"return -EMSGSIZE;", b"return -EINVAL;"))
    assert prepare().returncode == 0
    assert (output / "spi-native.c").read_text() != native_generated
    assert all((source / name).read_bytes() == contents for name, contents in originals.items())

print("Exact native/Windows SPI extraction, two upstream source hashes, drift/missing "
      "guards, stable generation and native-source refresh passed")

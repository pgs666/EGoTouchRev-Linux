#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check exact Windows master startup extraction and hash drift guards."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, module = sys.argv[1:]
names = ("HimaxChipMode.cpp", "HimaxChipLifecycle.cpp", "HimaxProtocol.cpp",
         "HimaxRegisters.cpp", "HimaxByteUtils.h")
originals = {name: (Path(upstream) / name).read_bytes() for name in names}

with tempfile.TemporaryDirectory(prefix="himax-raw-reference-") as directory:
    root = Path(directory)
    source, output = root / "source", root / "nested" / "reference.cpp"
    source.mkdir()
    for name, contents in originals.items():
        (source / name).write_bytes(contents)
    script = root / "prepare.cmake"
    script.write_text(
        f'include("{Path(module).as_posix()}")\n'
        f'himax_prepare_raw_reference("{source.as_posix()}" "{output.as_posix()}")\n'
    )

    def prepare():
        return subprocess.run(
            [cmake, "-P", str(script)], capture_output=True, text=True, timeout=10
        )

    result = prepare()
    assert result.returncode == 0, result.stdout + result.stderr
    generated = output.read_text()
    assert all((source / name).read_bytes() == contents for name, contents in originals.items())
    for name, first, last in (
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::himax_mcu_reload_disable(",
         "ChipResult<> Chip::hx_is_reload_done_ahb("),
        ("HimaxChipMode.cpp", "ChipResult<> Chip::himax_mcu_assign_sorting_mode(",
         "ChipResult<> Chip::himax_switch_data_type("),
        ("HimaxChipMode.cpp", "ChipResult<> Chip::hx_set_N_frame(",
         "ChipResult<> Chip::himax_switch_mode_inspection("),
        ("HimaxChipMode.cpp", "ChipResult<> Chip::himax_switch_mode_inspection(",
         "\n} // namespace Himax"),
        ("HimaxProtocol.cpp", "    ChipResult<> HimaxProtocol::write_and_verify(",
         "    ChipResult<> HimaxProtocol::safeModeSetRaw("),
        ("HimaxChipMode.cpp", "ChipResult<> Chip::himax_switch_data_type(",
         "ChipResult<> Chip::hx_set_N_frame("),
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::himax_mcu_read_FW_status(",
         "ChipResult<> Chip::himax_mcu_interface_on("),
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::himax_mcu_power_on_init(",
         "\nnamespace {"),
        ("HimaxChipLifecycle.cpp",
         "    std::array<uint8_t, 4> tmp_data = {0xA5, 0x5A, 0x00, 0x00};",
         "    if (auto res = himax_switch_data_type(DeviceType::Slave"),
    ):
        text = originals[name].decode()
        body = text[text.index(first):text.index(last)]
        assert generated.count(body) == 1, f"must compile the exact original body: {first}"
    assert 'extern "C" int hx_raw_prepare_reference' in generated
    assert 'extern "C" int hx_raw_start_reference' in generated
    assert "#include <Windows.h>" not in generated
    os.utime(output, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert output.stat().st_mtime_ns == 1_000_000_000, "no-op configure rewrote reference"
    for name, contents in originals.items():
        path = source / name
        path.write_bytes(contents + b"\n// drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        assert output.read_text() == generated, "drift must not replace reviewed reference"
        path.write_bytes(contents)
        path.unlink()
        result = prepare()
        assert result.returncode != 0 and "Missing upstream raw preparation source" in result.stderr
        path.write_bytes(contents)

print("Nine exact startup source sections, register/hash guards, no-op configure "
      "and missing-source rejection passed")

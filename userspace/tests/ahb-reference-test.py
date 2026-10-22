#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check exact upstream AHB extraction, register constants and source guards."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

cmake, upstream, module = sys.argv[1:]
names = ("HimaxProtocol.cpp", "HimaxChipLifecycle.cpp",
         "HimaxRegisters.cpp", "HimaxByteUtils.h")
originals = {name: (Path(upstream) / name).read_bytes() for name in names}

with tempfile.TemporaryDirectory(prefix="himax-ahb-reference-") as directory:
    root = Path(directory)
    source, output = root / "source", root / "nested" / "reference.cpp"
    source.mkdir()
    for name, contents in originals.items():
        (source / name).write_bytes(contents)
    script = root / "prepare.cmake"
    script.write_text(
        f'include("{Path(module).as_posix()}")\n'
        f'himax_prepare_ahb_reference("{source.as_posix()}" "{output.as_posix()}")\n'
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
        ("HimaxProtocol.cpp", "    ChipResult<> HimaxProtocol::burst_enable(",
         "    void HimaxProtocol::build_command_packet("),
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::himax_mcu_interface_on(",
         "ChipResult<> Chip::hx_sense_on("),
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::hx_hw_reset_ahb_intf(",
         "ChipResult<> Chip::hx_sw_reset_ahb_intf("),
        ("HimaxChipLifecycle.cpp", "ChipResult<> Chip::hx_sense_on(",
         "ChipResult<> Chip::hx_sense_off("),
    ):
        text = originals[name].decode()
        body = text[text.index(first):text.index(last)]
        assert generated.count(body) == 1, f"must compile exact original section: {first}"
    for constant in ("addr_ahb_rdata_byte_0 = 0x08", "addr_conti = 0x13",
                     "addr_incr4 = 0x0D", "data_conti = 0x31", "data_incr4 = 0x12",
                     "adr_i2c_psw_lb = 0x31", "addr_ctrl_fw_isr = 0x9000005C",
                     "addr_fw_define_2nd_flash_reload = 0x100072C0", "data_clear = 0x00000000"):
        assert constant in generated, constant
    assert 'extern "C" int hx_ahb_reference' in generated
    assert "#include <windows.h>" not in generated
    os.utime(output, ns=(1_000_000_000, 1_000_000_000))
    assert prepare().returncode == 0
    assert output.stat().st_mtime_ns == 1_000_000_000, "no-op configure rewrote reference"
    for name, contents in originals.items():
        path = source / name
        path.write_bytes(contents + b"\n// drift\n")
        result = prepare()
        assert result.returncode != 0 and "needs review" in result.stderr
        assert output.read_text() == generated, "drift replaced reviewed reference"
        path.write_bytes(contents)
        path.unlink()
        result = prepare()
        assert result.returncode != 0 and "Missing upstream" in result.stderr
        path.write_bytes(contents)

print("Exact AHB/interface source, constants, four-file hashes, no-op configure "
      "and missing-source guards passed")

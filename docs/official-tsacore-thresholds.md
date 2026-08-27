# Official TSACore SafeBaseline thresholds

The Gaokun Windows build exposes the SafeBaseline reset tables as exported
globals in `TSACore.dll`.  The values below were extracted from:

`/home/user/Project/HuaweiThpService/TSACore.dll`

SHA-256: `e9597b2ce3370a6dc976c3163d2a65479c61206a1eca73f55e7de6e20567f840`

The PE image maps these RVAs into `.rdata`; they are not fields in the
`GaokunHimaxBOE.bin` panel parameter blob.

| Symbol | RVA | Element type | Values |
| --- | ---: | --- | --- |
| `g_timeAbnormalThold` | `0x117d40` | `u16[10]` | `500,500,500,500,500,500,500,500,2000,500` |
| `g_triggerAbnormalThold` | `0x117d54` | `u8[10]` | `5,5,5,5,5,5,50,5,5,5` |
| `g_pushAbnormalThold` | `0x117d5e` | `u8[10]` | `1,1,1,1,1,1,1,0,0,0` |
| `g_timeScreenOnThold` | `0x117d80` | `u32[10]` | `300000,300000,300000,300000,300000,300000,300000,600000,600000,600000` |
| `g_timeAbnormalTholdSide` | `0x117fa0` | `u16[10]` | `500,500,500,500,500,500,500,2000,2000,500` |
| `g_triggerAbnormalTholdSide` | `0x117fb4` | `u8[10]` | `5,5,5,5,5,5,50,5,5,5` |
| `g_pushAbnormalTholdSide` | `0x117fbe` | `u8[10]` | `1,1,1,1,1,1,1,0,0,0` |
| `g_timeScreenOnTholdSide` | `0x117fe0` | `u32[10]` | `300000,300000,300000,300000,300000,300000,300000,600000,600000,600000` |

The reset-statistics thresholds are accumulated using the formatted touch
frame interval.  Linux stores `frame_interval_ms` (8 ms by default for the
120 Hz path) and converts frame-sequence deltas to milliseconds before applying
these tables.  The BLSM touch-release transition uses a separate dynamic
parameter at `g_tsaPrmtDynamic + 0x54`; Gaokun initializes it to `0x198`
(408 ms).

## SS/CMF boundary

The Gaokun `TSACore.dll` export `SS_IsValidForCMF()` returns constant `false`.
Consequently `CMF_ProcessDim()` does not add the `SS_DiffCMFProcessDimUnit`
correction on this build.  `SS_Process()` maintains a separate self-sense
 baseline using buffers whose length is `TX + RX`; those buffers are not present
 in the HX83121A master event-stack frame used by this Linux driver.  The Linux
 port therefore deliberately omits SS correction instead of fabricating a
 self-sense grid from mutual-capacitance pixels.

The live original Windows service additionally reports
`g_tsaPrmtDynamic[0x24] = 0`, so the entire SS path is disabled for Gaokun;
bytes `0x25 = 2` and `0x26 = 3` are subordinate masks and do not enable it.

`BLIIR_DoUpdate()` does have an unambiguous source selection: it consumes
`g_tsaBufPreCMFRaw` when panel parameter byte `0x69` is nonzero, otherwise
`g_tsaBufRaw`.  The Linux `bliir_use_pre_cmf_raw` default is enabled because
the Gaokun parameter blob contains `flash[0x69] = 1`.

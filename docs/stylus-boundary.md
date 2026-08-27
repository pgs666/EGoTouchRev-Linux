# Stylus implementation boundary

The current HX83121A Linux driver implements finger touch only.  It does not
register a pen input device, parse a stylus packet, report pressure/tilt/
buttons, or feed stylus state into the finger tracker or baseline state
machines.

The reverse-engineered Windows frame structure contains an optional stylus
frame pointer at `frame + 0x40`.  That pointer and the TSACore pen algorithms
are deliberately outside the current baseline port.  In particular:

- SafeBaseline, BLIIR, BLSM, BLReset, BLRecal and SHB consume only the mutual
  finger-capacitance grid and finger tracks;
- a non-null Windows stylus pointer or its flags must not be translated into a
  finger/baseline signal;
- no stylus code may be inserted into `hx_algo_process_frame_state()`, the
  finger Hungarian tracker, or the finger input-report loop;
- a future stylus implementation must use its own source/header, state object,
  packet parser, input device and build-time option, disabled by default;
- the only permitted integration points are a top-level event demultiplexer
  and controller power/reset notifications.  Baseline sharing requires new
  evidence from the official driver and must not be inferred.

This is an architectural boundary, not a claim that stylus support exists.
There is currently no stylus implementation to compile or enable. The
Makefile reserves `HIMAX_STYLUS=1` for a future stand-alone `hx-stylus.c`
translation unit; it is `0` by default and normal builds define
`HIMAX_STYLUS_DISABLED`. Enabling it today only links no-op lifecycle hooks;
it does not create a pen input device or alter finger processing.

# SafeBaseline port status

This document records which parts of the Gaokun Windows `TSACore.dll`
baseline path have a Linux implementation and which parts are still being
ported.  “Present in TSACore” is not the same as “enabled for Gaokun”; the
runtime project parameters are authoritative.

## Gaokun SS configuration boundary

A live Windows capture of the original service and DLL read the exported
`g_tsaPrmtDynamic` array as:

```text
[0x24] SS dimension-valid switch  = 0
[0x25] SS baseline dimension mask = 2
[0x26] SS touch dimension mask    = 3
```

`SS_Process()` first checks byte `0x24`, so it returns without calling
`SS_IsRawValid()` when this byte is zero.  The same capture observed
`frame+0x18 == NULL`, `SS_Process` entry calls, and zero `SS_IsRawValid`
calls.  Consequently the Gaokun configuration disables SS/self-sense even
though the generic TSACore binary contains the implementation.  Linux must
not fabricate or enable SS data to claim parity.

## Implemented from the reference binary

- `BaselineQueue`: five-entry chronological normal queue and five-entry
  temporary queue, cross-screen confirmation, validity and promotion.
- `SafeBaseline_GetPrpt`: center/inner/all/left/right min/max/count
  aggregates, before and after CMF.
- `SafeBaseline_JudgeCurrentBaseline` equivalent and runtime comparison.
- `SafeBaseline_ResetStatisticsUnit`, normal and side threshold tables,
  millisecond time accumulation, push and trigger quotas.
- Held-in-hand wake protection and the official
  `latestSafeBL - SafeBL2BLDif + SafeBL2BLDifCMF` reconstruction.
- `BLIIR_DoUpdate` source selection, bounded fixed-step update, and
  `BLIIR_Reset` fixed value (`0x7fff` for the Gaokun blob).
- Per-cell `BLIIR_DoUpdate` arithmetic is owned by
  `hx-baseline-update.c`; preprocessing only selects the source and step.
- BLSM stage retention and no-touch qualification ordering.
- The exact ten numeric `BLSM_UpdateStage` states and the Gaokun transition
  table, including the 408 ms touch-release transition timer.
- `BLSM_ProcessStage` action layer is now isolated in
  `hx-baseline-update.c`: stage 0/post-touch bounded BLIIR updates, stage 1
  snapshot reset, explicit hold stages, and one-shot reset consumption.
- Frame-flow health watchdog is kept in the SPI owner: O(1) invalid-master and
  retry streaks are held without advancing baseline or tracker state;
  persistent streaks trigger full hardware reinitialization.  A byte-identical
  frame counter is intentionally not enabled yet because the exposed frame
  format has no verified sequence field and a stationary finger can be truly
  identical.
- `BLSM_GetProperty` priority and property-bit mapping are isolated in
  `hx-baseline-property.c`; the frame preprocessor no longer embeds this
  policy alongside per-cell baseline arithmetic.
- Signal-level `SafeBaseline_CheckWithSignal` predicates are isolated in
  `hx-safe-baseline-signal.c`, including the vendor-style dominant-negative
  and broad-noise checks for mutual-cap data.
- `SideBaseline_IsVeryNegative` is represented by an edge-ring predicate in
  `hx-baseline-state.c`: it applies the recovered `4 * max < -2 * min` and
  dynamic-threshold tests without inventing the unavailable private side
  buffer.
- The four Gaokun project IDs are represented in the versioned
  `hx-panel-profile.c` registry with confirmed geometry and scalar metadata;
  selection currently falls back to `DFLT000000` until an OTP project-ID
  reader is wired to the SPI hardware path.  No unverified profile value is
  applied to the IRQ hot path yet.
- The DDReg byte-address transform from `hx_ddreg_read_byte` is now isolated
  as `himax_ddreg_read_byte()` in the hardware layer.  It is a read-only
  building block and is not called during probe until the complete OTP enter/
  exit sequence is available.
- The matching DDReg write path (`himax_ddreg_write()`) is now isolated as
  well, using the vendor's 12-bit register-prefix encoding and bounded by the
  transfer buffer size.  It is intentionally not invoked by normal resume.
- `himax_get_project_id_otp()` now performs the vendor three-address probe
  (`0xf18`, `0xf3a`, `0xf5c`) over the isolated OTP transaction, validates the
  returned 10-byte ID against the profile registry, and returns `-ENODATA` on
  unknown IDs.  It remains an explicit caller API; probe does not invoke it.
- Shared raw-grid max/min, range, spatial-noise and operational-noise
  observation is isolated in `hx-baseline-observe.c`, keeping the frame
  preprocessor focused on sequencing and per-cell BLIIR updates.
- Initial `BLReset_CheckAbnormal*` mutual-cap classification is isolated in
  `hx-blreset.c`; it reports noisy, negative, sensor, ghost and all-touch
  conditions to the existing debounced policy without performing reset side
  effects in the frame classifier.
- `BLRecal_Process` is isolated in `hx-blrecal.c` and uses the actual Gaokun
  flash values (`center=0x8000`, `span=8000`, `signal threshold=600`) with the
  vendor 5-frame guard, 10-frame vote, and three-vote request threshold.
- `BLSM_ShbProcess` is isolated in `hx-blsm-shb.c`, including raw snapshot
  change invalidation and both 5-second hold windows.
- SHB's second-window notification is now consumed once by BLSM property
  selection; the settled state remains available for diagnostics without
  pinning every later frame in the noisy stage.
- `BLSM_ShbProcess` now follows the per-frame action-word semantics exactly:
  bit 0 re-captures pre-CMF raw and restarts the timestamp at the end of each
  five-second window, while bit 1 is the second-window notification.  The
  second comparison is therefore made against the midpoint capture rather
  than the original ten-second-old frame.
- `BLRecal` requests are latched until an explicit `hx_blrecal_ack()` from the
  hardware owner, so a request cannot be lost on the next frame or repeatedly
  re-enter the weak-signal stage.
- The SPI owner now consumes a latched BLRecal request through the existing
  AFE command queue (`START_CALIBRATION`) and acknowledges it only after the
  command slot readback succeeds.  Diagnostics distinguish transport failure
  from success.  HX83121A exposes no calibration-done register in the
  recovered protocol, so the acknowledgement deliberately means “command
  accepted by the transport”, not “AFE convergence proven”.
- BLReset frame flags are now published before BLSM stage selection, so
  `BLSM_GetProperty`-equivalent stage decisions and SafeBaseline reset policy
  observe one consistent classification for each raw frame.
- `BLSM_UpdateForRawUnstable` is implemented in the stage-action module as a
  bounded 17-count correction, with the vendor signal-limit gate.
- The selected BLSM action is now exposed as a distinct per-frame value
  (`hold`, `bounded update`, `snapshot reset`, or `raw-unstable correction`)
  with diagnostic counters, making stage behavior auditable in sysfs.
- The PE jump table for `BLSM_ProcessStage` was disassembled directly: stages
  1/2 call reset, stage 3 restores the previous stage semantics (Linux keeps
  the numeric stage for diagnostics while suppressing its action), stages
  4/5/6 hold, stages 7/8 use after-touch BLIIR, and stage 9 uses forced BLIIR.
- Stage 2 now executes the same BLSM reset action as stage 1 (with the
  no-touch precondition made explicit for the Linux predicate adapter).
  Stages 7/8 use the official dynamic `threshold/64` after-touch step and
  symmetric `threshold-1` limit.  Stage 9 implements
  `BLIIR_Update(1, -minSig, min(-minSig/16, threshold/4))`, including the
  force-bit rule that bypasses only the negative bound.
- Raw-derived reset flags can no longer outrank a current protected/real
  touch.  This preserves the official `BLSM_GetProperty` control-flow
  precondition: `BLReset_Process` is evaluated only in the no-touch branch.
- `BLReset_CheckNumOverNoise` now keeps a consecutive over-noise frame
  counter and promotes the condition only after a five-frame vote; isolated
  noisy frames remain non-destructive.
- `BLReset_IsDirtyBaseline` is now represented for the mutual-cap path:
  simultaneous strong negative/common shifts plus excessive negative nodes are
  promoted to the sensor/negative reset reasons.
- `BLReset_Process` now has an isolated temporal gate in `hx-blreset.c`:
  abnormal reasons must persist for the vendor-sized 500 ms debounce window
  before one trigger is emitted; a clean interval rearms the gate.  The
  trigger is consumed by SafeBaseline policy as a working-grid recovery when
  a confirmed safe slot exists, and its state/reason/timer/counters are
  exposed through diagnostics sysfs.
- The BLReset gate now consults the existing per-reason time table before its
  generic fallback, matching `SafeBaseline_ResetStatisticsUnit` when a panel
  supplies reason-specific thresholds.
- New baseline arithmetic keeps all configurable signed thresholds clamped
  before fixed-point multiplication, avoiding undefined signed left shifts in
  kernel builds.
- `BLSM_SaveNormalBl` has a separate normal-baseline snapshot updated only by
  accepted stage-0 no-touch frames.
- SafeBaseline signal predicates are now evaluated on every normal
  preprocessing frame, before BLSM property/stage selection.  This keeps
  very-negative, very-noisy, ghost and silent-ghost evidence from being
  confined to the optional runtime comparator path.
- The BLSM reset action is guarded by the same frame-quality gate: a reset
  stage no longer snapshots a frame containing touch evidence or operational
  negative/line noise.  Such frames are held until a clean no-touch frame can
  safely seed BLIIR.
- SafeBaseline current-grid judging now considers all five spatial property
  groups (center, inner, all, left and right) when scalar BL2BL/BL2RAW scores
  are inconclusive, with a regression test for distributed regional failure.
- `SafeBaseline_IsDoubleChecked` is represented separately from the stronger
  queue-confidence predicate: replacement requires two valid entries from
  different screen epochs, matching the vendor check, while reset/recovery
  decisions may still require the stricter trustability gate.
- Current-baseline comparison now evaluates every mutual-cap grid cell,
  including index zero, before allowing an aggregate `BOTH_VALID` result;
  distributed corruption that does not move the global extrema is therefore
  still eligible for safe-baseline recovery.
- Shared raw-grid observation likewise includes index zero, so edge-cell
  excursions participate in SafeBaseline, BLSM and BLReset noise statistics.
- Silent-ghost classification now applies the vendor-like weak-maximum gate
  before marking a compact positive island; ordinary low-amplitude contact
  candidates are no longer rejected solely because they occupy a few cells.
- Baseline state, abnormal-reset, recalibration, queue compatibility, and
  safe-diff paths now use the complete mutual-cap grid (including cell zero)
  consistently for extrema, jumps, and compatibility checks.
- `SafeBaseline_IsTouchToBeProtected` now follows the vendor's previous-touch
  accounting: an unmatched previously valid touch remains protected even when
  the current solver has no active track, preventing that frame from becoming
  a baseline-learning sample.
- `SafeBaseline_CheckTouch`/`BLReset_CheckAbnormalTouch` now consume resolved
  contacts (the Linux equivalent of `Peak_IsTouchValid`) rather than every raw
  local maximum, preventing noise lobes from inflating all-touch-abnormal
  counters.
- `BLReset`'s compact-ghost path now requires both an abnormal sensor island
  and a weak reported maximum, matching the vendor's
  `CheckGhostSensorByMinSigOff` plus `CheckAbnormalMax` conjunction.
- `SafeBaseline_IsVeryNoisy` now uses the recovered 0x33 abnormal-node
  qualification and a positive signal gate, keeping the learning guard
  distinct from the larger runtime reset quota.
- `SafeBaseline_CheckSafeDiff` qualification is now restricted to an idle
  frame with a completed stable comparison and fewer than 50 abnormal
  signal nodes, matching the vendor's no-touch/small-signal gate.
- `BLReset_BaselineStateMachine` was recovered directly from the PE jump
  table and isolated in `hx-blreset-state.c`.  States 0/1/2/4/8/16, the six-
  frame bootstrap, 333 ms release window, 100 ms clean window, activity
  thresholds 30/1, normal-baseline snapshot, and the explicit no-op states
  3/5/6/7 now match the vendor control flow.
- `BLReset_IsTriggered2AfeWakeUp` now has an independent Linux temporal gate
  in `hx-blreset-state.c`: abnormal-touch/baseline evidence accumulates for
  the vendor-sized 160 ms window, emits one request, and decays/re-arms on
  clean frames instead of sharing the normal 500 ms reset timer.
- `BLReset_CheckCleanBaseline` is now implemented against the captured
  normal-baseline grid: fewer than four cells may differ by more than 100
  counts.  A no-touch frame that fails this check contributes a sensor-bad
  reset reason instead of being learned by BLIIR.
- `BLReset_IsCurBaselineDirty` now has its own 300 ms timer.  It accumulates
  only when over-noise persists, the current BL is abnormal, raw still
  matches the captured clean baseline, and no touch is protected; every
  disqualifying frame clears the timer as in the vendor implementation.
- `BLReset_IsBaselineAbnormalType1/2/3` now have an isolated mutual-cap
  classifier in `hx-blreset-abnormal.c`.  Negative-dominant, broad-noise and
  strict-negative predicates maintain independent 300 ms votes, suppress
  protected/active-touch frames, and publish flags/counters through
  diagnostics sysfs.  SS-only portions remain disabled for Gaokun.

## Disabled by the official Gaokun configuration

- `SS_Process` self-sense baseline and SS touch processing.  They are generic
  TSACore capabilities, but `g_tsaPrmtDynamic[0x24] == 0` disables them for
  this product.
- `SS_DiffCMFProcessDimUnit` correction.  The current Gaokun binary's
  `SS_IsValidForCMF()` returns constant false, so the correction is disabled
  in the official build as well.
- Side self-sense baseline.  No Gaokun runtime frame supplies `ss_raw`; edge
  statistics used by the mutual path are separate and remain applicable.

## Deliberately disabled and separated

- Stylus/active-pen processing.  The Linux driver currently has no stylus
  packet parser or pen input device, and none of the baseline modules consume
  the Windows `frame+0x40` stylus pointer or its flags.  Any future support
  must remain a separately compiled, default-off subsystem as specified in
  `docs/stylus-boundary.md`.

## Still incomplete

- Full `BLReset_Check` and raw-jump predicates.  The baseline state machine
  and mutual-cap portions of abnormal Type1/2/3 are translated; several
  activity-count and protected-touch fields remain operational-node/tracker
  equivalents because TSACore's private touch-property structures are not
  exported by the firmware frame.
- Exact toe/side-area threshold switching within stages 7/8 remains
  incomplete.  The Gaokun mutual-cap main-panel action and SHB action-word
  behavior are translated; the separate SideTouch consumer still depends on
  private side-area structures not present in this driver.
- Exact SafeBaseline abnormal touch/peak/sensor predicates and reset reasons.
  Current implementations retain conservative Linux guards where the vendor
  predicate has not yet been translated function-for-function.
- SafeBaseline screen-on replacement is present but disabled by default,
  matching Gaokun flash byte `0x1743 == 0`.
- The downstream vendor exception/report action after AFE calibration and the
  consumer of SHB bit 2 remain incomplete.  Linux now sends the calibration
  command with transport-level acknowledgement, but cannot claim completion
  without a firmware status handshake that is absent from the protocol.

## Deliberately not approximated

- Full `CMF_ProcessDimUnit` parameter behavior.  It depends on the vendor
  CMF parameter tables and runtime `TSAStatic` state; the Linux driver
  currently uses its own validated row/column CMF path rather than inventing
  values for untranslated inputs.  SS correction is not one of those inputs
  on Gaokun because it is disabled as described above.

These boundaries are not hidden behind names that claim vendor equivalence.
Completion requires translating and testing the enabled mutual-capacitance
paths above; obtaining a nonexistent Gaokun SS stream is no longer considered
a prerequisite.

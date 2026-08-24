package org.egotouch.tuner;

/** Builds the root-side collector used by the former standalone diagnostic app. */
final class DiagnosticScript {
    static String make(String directory, String stamp, boolean reset) {
        String d = RootShell.quote(directory);
        return "set +e\n" +
            "D=" + d + "; mkdir -p \"$D/post-window\" || { echo CAPTURE_STATUS=FAILED_directory; exit 10; }; STATUS=\"$D/status.txt\"; : >\"$STATUS\"; failed=''\n" +
            "echo 'timestamp=" + stamp + "' >\"$D/README.txt\"\n" +
            "core(){ label=$1; shift; \"$@\" >\"$D/$label\" 2>\"$D/$label.err\"; rc=$?; echo core_$label=$rc >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed $label\"; }\n" +
            "optional(){ label=$1; shift; \"$@\" >\"$D/$label\" 2>\"$D/$label.err\"; echo optional_$label=$? >>\"$STATUS\"; }\n" +
            "named(){ found=1; for p in $(find /sys/devices -name \"$1\" -type f 2>/dev/null); do found=0; echo ==== $p ====; cat \"$p\"; echo; done; return $found; }\n" +
            "paths(){ found=1; for p in $(find /sys/devices -path \"$1\" -type f 2>/dev/null); do case $p in */event_stack|*/event_trace) continue;; esac; found=0; echo ==== $p ====; cat \"$p\"; echo; done; return $found; }\n" +
            "DP=$(find /sys/devices -name diagnostics -type f 2>/dev/null|head -1); BP=$(find /sys/devices -name event_stack -type f 2>/dev/null|head -1); HP=$(find /sys/devices -name event_stack_hex -type f 2>/dev/null|head -1); TP=$(find /sys/devices -name event_trace -type f 2>/dev/null|head -1)\n" +
            "printf 'diagnostics_node=%s\\nevent_stack_node=%s\\nevent_stack_hex_node=%s\\nevent_trace_node=%s\\n' \"${DP:-<none>}\" \"${BP:-<none>}\" \"${HP:-<none>}\" \"${TP:-<none>}\" >\"$D/nodes.txt\"\n" +
            "if [ -n \"$TP\" ]; then cat \"$TP\" >\"$D/event-trace-before.bin\" 2>\"$D/event-trace-before.bin.err\"; rc=$?; else rc=2; fi; echo core_event-trace-before.bin=$rc >>\"$STATUS\"; [ $rc = 0 ] || failed=\"$failed event-trace-before\"\n" +
            "core dmesg-before dmesg -T; core diagnostics.txt named diagnostics; core event-stack-hex.txt named event_stack_hex\n" +
            "if [ -n \"$BP\" ]; then cat \"$BP\" >\"$D/event-stack.bin\" 2>\"$D/event-stack.bin.err\"; rc=$?; else rc=2; fi; echo core_event-stack.bin=$rc >>\"$STATUS\"; [ $rc = 0 ] || failed=\"$failed event-stack\"\n" +
            "EVENT_DEV=''; for n in /sys/class/input/event*/device/name; do [ -f \"$n\" ] || continue; case $(cat \"$n\" 2>/dev/null) in *Himax*Touch*) EVENT_DEV=/dev/input/$(basename $(dirname $(dirname \"$n\"))); break;; esac; done; echo input_event=${EVENT_DEV:-<none>} >\"$D/input-event-node.txt\"\n" +
            "GPID=''; if [ -n \"$EVENT_DEV\" ] && command -v getevent >/dev/null; then getevent -lt \"$EVENT_DEV\" >\"$D/getevent-post-window.txt\" 2>\"$D/getevent-post-window.err\" & GPID=$!; fi\n" +
            "STOP=\"$D/post-window/.stop\"; rm -f \"$STOP\"; (sleep 5; : >\"$STOP\") & SPID=$!; if [ -n \"$TP\" ]; then (sleep 2; cat \"$TP\" >\"$D/event-trace-mid.bin\" 2>\"$D/event-trace-mid.bin.err\") & MPID=$!; else MPID=''; fi\n" +
            "started=$(cut -d' ' -f1 /proc/uptime); i=0; post_failed=0; while [ ! -e \"$STOP\" ] && [ $i -lt 250 ]; do n=$(printf %03d $i); if [ -n \"$DP\" ]; then cat \"$DP\" >\"$D/post-window/diagnostics-$n.txt\" 2>\"$D/post-window/diagnostics-$n.err\" || post_failed=1; else post_failed=1; fi; if [ -n \"$BP\" ]; then cat \"$BP\" >\"$D/post-window/event-stack-$n.bin\" 2>\"$D/post-window/event-stack-$n.err\" || post_failed=1; else post_failed=1; fi; i=$((i+1)); sleep 0.02; done; wait $SPID 2>/dev/null; ended=$(cut -d' ' -f1 /proc/uptime); rm -f \"$STOP\"; echo samples=$i requested_interval_ms=20 requested_duration_seconds=5 started_uptime=$started ended_uptime=$ended >\"$D/post-window/status.txt\"; echo core_post-window=$post_failed >>\"$STATUS\"; [ $post_failed = 0 ] || failed=\"$failed post-window\"\n" +
            "[ -n \"$MPID\" ] && wait $MPID; [ -n \"$GPID\" ] && { kill $GPID 2>/dev/null; wait $GPID 2>/dev/null; }; [ -s \"$D/getevent-post-window.txt\" ]; rc=$?; echo core_getevent-post-window=$rc >>\"$STATUS\"; [ $rc = 0 ] || failed=\"$failed getevent\"\n" +
            "if [ -n \"$TP\" ]; then cat \"$TP\" >\"$D/event-trace-after.bin\" 2>\"$D/event-trace-after.bin.err\"; rc=$?; else rc=2; fi; echo core_event-trace-after.bin=$rc >>\"$STATUS\"; [ $rc = 0 ] || failed=\"$failed event-trace-after\"\n" +
            "core dmesg-post-window dmesg -T; core id id; core uname uname -a; core cmdline cat /proc/cmdline; core getprop getprop; core interrupts cat /proc/interrupts; core logcat logcat -b all -d -v threadtime; core dumpsys-input dumpsys input; optional spi-sysfs paths '*/spi0.0/*'; optional algo paths '*/spi0.0/algo/*'\n" +
            (reset ? "P=$(find /sys/devices -name inplace_reset -type f 2>/dev/null|head -1); echo node=${P:-<none>} >\"$D/reset.txt\"; if [ -n \"$P\" ] && echo 1 >\"$P\" 2>\"$D/reset.err\"; then echo reset_succeeded >>\"$D/reset.txt\"; sleep 2; [ -n \"$TP\" ] && cat \"$TP\" >\"$D/event-trace-after-reset.bin\"; core dmesg-after dmesg -T; else echo reset_failed >>\"$D/reset.txt\"; failed=\"$failed reset\"; fi\n" : "") +
            "du -ah \"$D\" >\"$D/files.txt\" 2>\"$D/files.err\"; if [ -n \"$failed\" ]; then echo CAPTURE_STATUS=FAILED$failed; else echo CAPTURE_STATUS=SUCCESS; fi\n";
    }
    private DiagnosticScript() {}
}

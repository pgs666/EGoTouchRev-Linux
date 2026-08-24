package org.egotouch.diagnostic;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.Configuration;
import android.view.View;
import android.widget.*;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;

/** Root-assisted, deliberately manual Himax evidence collector. */
public final class MainActivity extends Activity {
    private TextView status;
    private volatile String lastCaptureDir;
    private volatile String statusMessage = "等待操作";
    private static final String BASE = "/sdcard/HimaxTouchDiag";
    private static final int POST_CAPTURE_SECONDS = 5;
    private static final int POST_CAPTURE_INTERVAL_MS = 20;

    @Override public void onCreate(Bundle b) { super.onCreate(b); buildUi(); }

    @Override public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        buildUi();
    }

    private void buildUi() {
        boolean landscape = getResources().getConfiguration().orientation ==
                Configuration.ORIENTATION_LANDSCAPE;
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 32, 32, 24);
        TextView title = new TextView(this); title.setText("Himax HX83121A 断触诊断"); title.setTextSize(22); title.setTextColor(Color.BLACK);
        root.addView(title, new LinearLayout.LayoutParams(-1, -2));
        TextView hint = new TextView(this); hint.setText("断触发生后保持现场，点击“抓取证据”，随后继续滑动 5 秒。新版驱动会保存点击前约 4 秒，以及窗口中间和结束时的全速追踪；普通采集不会重置芯片。\n\n数据保存在 " + BASE + "，需要 root（KernelSU/Magisk 授权）。"); hint.setPadding(0, 18, 0, 18);
        root.addView(hint);
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        Button collect = new Button(this); collect.setText("抓取证据（推荐）");
        Button reset = new Button(this); reset.setText("手动硬件重置并记录前后日志");
        Button copy = new Button(this); copy.setText("复制最近一次目录路径");
        if (landscape) {
            actions.addView(collect, new LinearLayout.LayoutParams(0, -2, 1));
            actions.addView(reset, new LinearLayout.LayoutParams(0, -2, 1));
            actions.addView(copy, new LinearLayout.LayoutParams(0, -2, 1));
        } else {
            actions.addView(collect, new LinearLayout.LayoutParams(-1, -2));
            actions.addView(reset, new LinearLayout.LayoutParams(-1, -2));
            actions.addView(copy, new LinearLayout.LayoutParams(-1, -2));
        }
        root.addView(actions, new LinearLayout.LayoutParams(-1, -2));
        status = new TextView(this); status.setText(statusMessage); status.setTextIsSelectable(true); status.setPadding(0, 22, 0, 0); root.addView(status);
        collect.setOnClickListener(v -> runCapture(false));
        reset.setOnClickListener(v -> runCapture(true));
        copy.setOnClickListener(v -> { String target = (lastCaptureDir == null || lastCaptureDir.isEmpty()) ? BASE : lastCaptureDir; ClipboardManager c=(ClipboardManager)getSystemService(CLIPBOARD_SERVICE); c.setPrimaryClip(ClipData.newPlainText("Himax诊断目录", target)); setStatus("已复制：" + target); });
        scroll.addView(root, new ScrollView.LayoutParams(-1, -2));
        setContentView(scroll);
    }

    private void setStatus(String message) {
        statusMessage = message;
        if (status != null) status.setText(message);
    }

    private void runCapture(boolean doReset) {
        setStatus("正在请求 root，并采集点击后 " + POST_CAPTURE_SECONDS + " 秒窗口…");
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        String dir = BASE + "/" + stamp;
        lastCaptureDir = dir;
        new Thread(() -> {
            String script = "set +e\n" +
                "D='" + dir + "'\n" +
                "if ! mkdir -p \"$D\"; then echo 'CAPTURE_STATUS=FAILED directory_create'; exit 10; fi\n" +
                "STATUS=\"$D/status.txt\"; : > \"$STATUS\"; failed=\"\"\n" +
                "echo 'timestamp=" + stamp + "' > \"$D/README.txt\"\n" +
                "run_core() { label=\"$1\"; shift; \"$@\" >\"$D/$label\" 2>\"$D/$label.err\"; rc=$?; echo \"core_$label=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed $label\"; }\n" +
                "run_optional() { label=\"$1\"; shift; \"$@\" >\"$D/$label\" 2>\"$D/$label.err\"; rc=$?; echo \"optional_$label=$rc\" >>\"$STATUS\"; }\n" +
                "collect_named_nodes() { pattern=\"$1\"; found=0; node_failed=0; for p in $(find /sys/devices -name \"$pattern\" -type f 2>/dev/null); do found=1; echo \"==== $p ====\"; cat \"$p\" || { rc=$?; echo \"read_rc=$rc path=$p\" >&2; node_failed=1; }; echo; done; [ $found -eq 1 ] && [ $node_failed -eq 0 ]; }\n" +
                "collect_path_nodes() { pattern=\"$1\"; found=0; node_failed=0; for p in $(find /sys/devices -path \"$pattern\" -type f 2>/dev/null); do case \"$p\" in */event_stack|*/event_trace) continue;; esac; found=1; echo \"==== $p ====\"; cat \"$p\" || { rc=$?; echo \"read_rc=$rc path=$p\" >&2; node_failed=1; }; echo; done; [ $found -eq 1 ] && [ $node_failed -eq 0 ]; }\n" +
                "DP=$(find /sys/devices -name diagnostics -type f 2>/dev/null | head -1); BP=$(find /sys/devices -name event_stack -type f 2>/dev/null | head -1); HP=$(find /sys/devices -name event_stack_hex -type f 2>/dev/null | head -1); TP=$(find /sys/devices -name event_trace -type f 2>/dev/null | head -1)\n" +
                "echo \"diagnostics_node=${DP:-<none>}\" >\"$D/nodes.txt\"; echo \"event_stack_node=${BP:-<none>}\" >>\"$D/nodes.txt\"; echo \"event_stack_hex_node=${HP:-<none>}\" >>\"$D/nodes.txt\"; echo \"event_trace_node=${TP:-<none>}\" >>\"$D/nodes.txt\"\n" +
                "if [ -n \"$TP\" ]; then cat \"$TP\" >\"$D/event-trace-before.bin\" 2>\"$D/event-trace-before.bin.err\"; rc=$?; else rc=2; echo missing_node >\"$D/event-trace-before.bin.err\"; fi; echo \"core_event-trace-before.bin=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed event-trace-before.bin\"\n" +
                "run_core dmesg-before dmesg -T\n" +
                "run_core diagnostics.txt collect_named_nodes diagnostics\n" +
                "run_core event-stack-hex.txt collect_named_nodes event_stack_hex\n" +
                "BP=$(find /sys/devices -name event_stack -type f 2>/dev/null | head -1); echo \"event_stack_node=${BP:-<none>}\" >\"$D/event-stack-node.txt\"; if [ -n \"$BP\" ]; then cat \"$BP\" >\"$D/event-stack.bin\" 2>\"$D/event-stack.bin.err\"; rc=$?; else rc=2; echo missing_node >\"$D/event-stack.bin.err\"; fi; echo \"core_event-stack.bin=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed event-stack.bin\"\n" +
                "EVENT_DEV=; for n in /sys/class/input/event*/device/name; do [ -f \"$n\" ] || continue; input_name=$(cat \"$n\" 2>/dev/null); case \"$input_name\" in *Himax*Touch*) event_name=$(basename \"$(dirname \"$(dirname \"$n\")\")\"); EVENT_DEV=\"/dev/input/$event_name\"; break;; esac; done; echo \"input_event=${EVENT_DEV:-<none>}\" >\"$D/input-event-node.txt\"; GPID=; if [ -n \"$EVENT_DEV\" ] && command -v getevent >/dev/null 2>&1; then getevent -lt \"$EVENT_DEV\" >\"$D/getevent-post-window.txt\" 2>\"$D/getevent-post-window.err\" & GPID=$!; else echo missing_himax_input_or_getevent >\"$D/getevent-post-window.err\"; fi\n" +
                "mkdir -p \"$D/post-window\"; echo \"diagnostics_node=${DP:-<none>}\" >\"$D/post-window/README.txt\"; echo \"event_stack_node=${BP:-<none>}\" >>\"$D/post-window/README.txt\"; echo \"duration_seconds=" + POST_CAPTURE_SECONDS + "\" >>\"$D/post-window/README.txt\"; STOP=\"$D/post-window/.capture-stop\"; rm -f \"$STOP\"; ( sleep " + POST_CAPTURE_SECONDS + "; : >\"$STOP\" ) & SPID=$!; if [ -n \"$TP\" ]; then ( sleep 2; cat \"$TP\" >\"$D/event-trace-mid.bin\" 2>\"$D/event-trace-mid.bin.err\" ) & MPID=$!; else MPID=; echo missing_node >\"$D/event-trace-mid.bin.err\"; fi; started=$(cut -d' ' -f1 /proc/uptime); i=0; post_failed=0; while [ ! -e \"$STOP\" ] && [ $i -lt 250 ]; do n=$(printf '%03d' $i); if [ -n \"$DP\" ]; then cat \"$DP\" >\"$D/post-window/diagnostics-$n.txt\" 2>\"$D/post-window/diagnostics-$n.err\" || post_failed=1; else echo missing_node >\"$D/post-window/diagnostics-$n.err\"; post_failed=1; fi; if [ -n \"$BP\" ]; then cat \"$BP\" >\"$D/post-window/event-stack-$n.bin\" 2>\"$D/post-window/event-stack-$n.err\" || post_failed=1; else echo missing_node >\"$D/post-window/event-stack-$n.err\"; post_failed=1; fi; i=$((i + 1)); sleep 0.02; done; wait \"$SPID\" 2>/dev/null; ended=$(cut -d' ' -f1 /proc/uptime); rm -f \"$STOP\"; echo \"samples=$i requested_interval_ms=" + POST_CAPTURE_INTERVAL_MS + " requested_duration_seconds=" + POST_CAPTURE_SECONDS + " started_uptime=$started ended_uptime=$ended\" >\"$D/post-window/status.txt\"; echo \"core_post-window=$post_failed\" >>\"$STATUS\"; [ $post_failed -eq 0 ] || failed=\"$failed post-window\"; if [ -n \"$MPID\" ]; then wait \"$MPID\"; rc=$?; else rc=2; fi; echo \"core_event-trace-mid.bin=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed event-trace-mid.bin\"; if [ -n \"$GPID\" ]; then kill \"$GPID\" 2>/dev/null; wait \"$GPID\" 2>/dev/null; fi; if [ -s \"$D/getevent-post-window.txt\" ]; then rc=0; else rc=2; fi; echo \"core_getevent-post-window=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed getevent-post-window\"\n" +
                "if [ -n \"$TP\" ]; then cat \"$TP\" >\"$D/event-trace-after.bin\" 2>\"$D/event-trace-after.bin.err\"; rc=$?; else rc=2; echo missing_node >\"$D/event-trace-after.bin.err\"; fi; echo \"core_event-trace-after.bin=$rc\" >>\"$STATUS\"; [ $rc -eq 0 ] || failed=\"$failed event-trace-after.bin\"; run_core dmesg-post-window dmesg -T\n" +
                "run_core id id\n" +
                "run_core uname uname -a\n" +
                "run_core cmdline cat /proc/cmdline\n" +
                "run_core getprop getprop\n" +
                "run_core interrupts cat /proc/interrupts\n" +
                "run_core logcat logcat -b all -d -v threadtime\n" +
                "run_core dumpsys-input dumpsys input\n" +
                "run_optional spi-sysfs collect_path_nodes '*/spi0.0/*'\n" +
                "run_optional algo collect_path_nodes '*/spi0.0/algo/*'\n" +
                (doReset ?
                    "P=$(find /sys/devices -name inplace_reset -type f | head -1); echo \"node=${P:-<none>}\" >\"$D/reset.txt\"; if [ -z \"$P\" ]; then echo 'reset_failed=no_inplace_reset_node' >>\"$D/reset.txt\"; echo 'core_reset=missing_node' >>\"$STATUS\"; failed=\"$failed reset\"; else echo 1 >\"$P\" 2>\"$D/reset.err\"; rc=$?; echo \"write_rc=$rc\" >>\"$D/reset.txt\"; if [ $rc -eq 0 ]; then echo reset_succeeded >>\"$D/reset.txt\"; echo core_reset=0 >>\"$STATUS\"; else echo reset_failed=write_error >>\"$D/reset.txt\"; echo core_reset=$rc >>\"$STATUS\"; failed=\"$failed reset\"; fi; fi; sleep 2; if [ -n \"$TP\" ]; then cat \"$TP\" >\"$D/event-trace-after-reset.bin\" 2>\"$D/event-trace-after-reset.bin.err\"; fi; run_core dmesg-after dmesg -T\n" : "") +
                "du -ah \"$D\" >\"$D/files.txt\" 2>\"$D/files.err\"\n" +
                "if [ -n \"$failed\" ]; then echo \"CAPTURE_STATUS=FAILED$failed\"; else echo CAPTURE_STATUS=SUCCESS; fi\n";
            String out = execRoot(script);
            runOnUiThread(() -> setStatus((out.contains("CAPTURE_STATUS=SUCCESS") ? "采集完成：\n" : "采集失败或不完整：\n") + dir + "\n\n" + out));
        }).start();
    }

    private String execRoot(String command) {
        try { Process p = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
            ByteArrayOutputStream b = new ByteArrayOutputStream(); InputStream in=p.getInputStream(); byte[] x=new byte[1024]; int n; while((n=in.read(x))>=0)b.write(x,0,n); p.waitFor(); return b.toString("UTF-8");
        } catch(Exception e) { return e.toString(); }
    }
}

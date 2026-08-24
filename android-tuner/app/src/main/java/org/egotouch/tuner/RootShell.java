package org.egotouch.tuner;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

final class RootShell {
    static String run(String command) throws Exception {
        Process process = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        InputStream input = process.getInputStream();
        byte[] buffer = new byte[4096];
        for (int count; (count = input.read(buffer)) >= 0; ) output.write(buffer, 0, count);
        int result = process.waitFor();
        String text = output.toString(StandardCharsets.UTF_8.name()).trim();
        if (result != 0) throw new IllegalStateException(text.isEmpty() ? "root 命令失败：" + result : text);
        return text;
    }

    static String quote(String value) { return "'" + value.replace("'", "'\\''") + "'"; }
    private RootShell() {}
}

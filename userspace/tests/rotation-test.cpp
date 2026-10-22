// SPDX-License-Identifier: MIT
#include "screen-rotation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

static void Check(bool pass, const char* message) {
    if (!pass) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    for (int rotation = 0; rotation < 4; ++rotation) {
        Check(himax::ParseScreenRotation(std::to_string(rotation)) == rotation, "explicit states");
        Check(himax::ParseScreenRotation(std::to_string(rotation * 90)) == rotation, "degrees");
        Check(himax::ParseKScreenRotation(1 << rotation) == rotation, "KScreen bit values");
    }
    Check(himax::ParseScreenRotation(" \nrotation=270\t") == 3, "whitespace");
    for (const auto* invalid : {"", "4", "8", "-1", "360", "90 degrees", "rotation=NaN",
                                "1\n2", "999999999999999999999999999999999"})
        Check(!himax::ParseScreenRotation(invalid), "malformed explicit values rejected");
    for (int mask : {0, 3, 16, 32, 64, 128})
        Check(!himax::ParseKScreenRotation(mask), "reflected/invalid KScreen value rejected");

    char directory[] = "/tmp/himax-rotation-XXXXXX";
    Check(mkdtemp(directory), "private temporary directory");
    const auto file = std::filesystem::path(directory) / "rotation";
    const auto write = [&](const char* contents) { std::ofstream(file) << contents; };
    himax::ScreenRotationProvider provider(file);
    Check(!provider.Poll(), "absent explicit source does not silently use framebuffer");
    write("kscreen=8\n");
    auto sample = provider.Poll();
    Check(sample && sample->value == 3 && sample->source == file, "desktop bridge source");
    write("rotation=90\n");
    sample = provider.Poll();
    Check(sample && sample->value == 1, "explicit source changes observed");
    write("kscreen=16\n");
    Check(!provider.Poll(), "reflected source rejected");
    write("0");
    provider.SetFixed(2);
    sample = provider.Poll();
    Check(sample && sample->value == 2 && sample->source == "fixed", "fixed override wins");
    provider.SetFixed({});
    write("999999999999999999999999999999999999999999999999999999999999999999999999999");
    Check(!provider.Poll(), "oversized source rejected");
    std::filesystem::remove(file);
    Check(!mkfifo(file.c_str(), 0600), "create FIFO fixture");
    Check(!provider.Poll(), "FIFO rejected without blocking");
    std::filesystem::remove(file);
    const auto target = std::filesystem::path(directory) / "target";
    std::ofstream(target) << "3";
    std::filesystem::create_symlink(target, file);
    Check(!provider.Poll(), "symlink rejected");
    std::filesystem::remove(file);
    std::filesystem::remove(target);
    std::filesystem::remove(directory);
    std::cout << "screen rotation source validation passed\n";
}

#pragma once
#include <string>

// Startup/crash log written to <user data dir>/satlight_log.txt (NEW-2). Exists so a bug
// report from a stranger's machine carries something diagnosable — build id, GPU, and
// whatever init step failed — instead of just "it doesn't work."
namespace Log
{
    // Truncates and opens satlight_log.txt in the user data directory, writes a header
    // (app version/commit/build date). Call once, as early as possible in main() — before
    // Vulkan init — so an instance/device creation failure still lands in the file.
    void init();

    // Appends a timestamped (UTC) line, reopening/flushing each call so a crash immediately
    // after this call still leaves the line on disk.
    void line(const std::string &msg);

    // Absolute path to the log file, for showing in a message box / toast. Empty if init()
    // hasn't been called or the file could not be created.
    const std::string &path();
}

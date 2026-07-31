#pragma once
#include <string>

// Filesystem locations shared across the app (main.cpp for the startup log,
// SatelliteSim for settings/perf snapshots). Kept in one place so the
// exe-relative vs. per-user split (NEW-4) has a single source of truth.
namespace Paths
{
    // Directory containing the running executable. Read-only game data
    // (constellations.json, assets/, shaders/) always lives here.
    std::string exeDir();

    // Per-user writable directory for settings/logs/perf data:
    //   Windows: %APPDATA%/SatLightSim
    //   Linux:   $XDG_DATA_HOME/SatLightSim or ~/.local/share/SatLightSim
    //   macOS:   ~/Library/Application Support/SatLightSim
    // If a file named "portable.txt" exists next to the executable, returns
    // exeDir() instead so the whole install (incl. settings) stays self-
    // contained and movable — e.g. for a USB-stick / zip-and-run distribution.
    // Creates the directory if it does not already exist.
    std::string userDataDir();
}

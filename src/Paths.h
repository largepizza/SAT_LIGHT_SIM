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

    // Per-user writable directory for settings/logs/perf data. Portable by default:
    // returns exeDir() whenever that directory is actually writable (the common case for a
    // zip-and-run/dev build), so settings/log/session-lock/perf-snapshot files sit right next
    // to the exe instead of a separate per-user profile directory.
    // Falls back to the OS per-user directory only when exeDir() is NOT writable (e.g.
    // installed under Program Files without elevation, or any other read-only install
    // location) so settings still persist rather than silently failing every write:
    //   Windows: %APPDATA%/SatLightSim
    //   Linux:   $XDG_DATA_HOME/SatLightSim or ~/.local/share/SatLightSim
    //   macOS:   ~/Library/Application Support/SatLightSim
    // Creates the directory if it does not already exist.
    std::string userDataDir();
}

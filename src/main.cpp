#include "App.h"
#include "Log.h"
#include "simulations/GameOfLife.h"
#include "simulations/Particles.h"
#include "simulations/Scene3DDemo.h"
#include "simulations/SatelliteSim.h"
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int main()
{
    // As early as possible — before any Vulkan call — so an instance/device creation
    // failure (missing driver, unsupported GPU) still lands in the log file (NEW-2).
    Log::init();

    try
    {
        // ── Pick a simulation ─────────────────────────────────────────────
        // Comment/uncomment to switch. Rebuild to apply.
        // App app(std::make_unique<GameOfLife>());
        // App app(std::make_unique<Particles>());
        // App app(std::make_unique<Scene3DDemo>());
        App app(std::make_unique<SatelliteSim>());

        app.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal: " << e.what() << "\n";
        Log::line(std::string("FATAL: ") + e.what());

#if defined(_WIN32)
        std::string msg = std::string(e.what()) +
                           "\n\nMake sure your GPU drivers are up to date and support Vulkan.\n\n"
                           "Details were written to:\n" + Log::path();
        MessageBoxA(nullptr, msg.c_str(), "Sat Light Sim — failed to start", MB_OK | MB_ICONERROR);
#endif
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

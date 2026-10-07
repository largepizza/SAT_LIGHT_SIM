# The weak-hardware sky tiers' size guard (CLAUDE.md "Weak-Hardware Sky Tiers"): fails the build when a
# low-tier sky shader's SPIR-V grows past its budget. Run right after the shader compiles:
#   cmake -DSPV=<file.spv> -DMAX_BYTES=<n> -DTIER=<name> -P cmake/CheckSpvSize.cmake
# The file is deleted on failure, so the next build compiles (and checks) it again.
#
# Why: sat_sky_lite.frag.spv (Planetarium) grew 142 KB (v1.1.0) -> 567 KB and sat_sky_minimal.frag.spv
# (Potato) took on terrain v2's water code, without anyone noticing, because every new feature was gated by
# a RUNTIME knockout or `if (false)` — which still compiles every call into the variant. On a GCN 1.0 / MoltenVK
# GPU the compiled size is what collapses fragment occupancy (~490 ms a frame for the full shader). A new
# feature must be #if'd out of SKY_LITE / left out of sat_sky_minimal.frag, and skipped on the CPU.
if(NOT EXISTS "${SPV}")
    message(FATAL_ERROR "CheckSpvSize: ${SPV} does not exist")
endif()
file(SIZE "${SPV}" bytes)
math(EXPR kb "${bytes} / 1024")
math(EXPR maxKb "${MAX_BYTES} / 1024")
if(bytes GREATER MAX_BYTES)
    file(REMOVE "${SPV}")
    message(FATAL_ERROR
        "${TIER} sky shader is ${kb} KB of SPIR-V, over its ${maxKb} KB budget (${SPV}).\n"
        "A v1.2-style feature reached a weak-hardware tier. Gate it out of the variant at COMPILE time "
        "(#ifndef SKY_LITE in sat_sky.frag; nothing new in sat_sky_minimal.frag) and skip its CPU work for that "
        "preset. Raise the budget in CMakeLists.txt only with a measurement on the target hardware.")
endif()
message(STATUS "${TIER} sky shader: ${kb} KB of SPIR-V (budget ${maxKb} KB)")

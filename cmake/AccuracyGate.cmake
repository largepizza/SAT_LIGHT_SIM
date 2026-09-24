# AccuracyGate.cmake — run in script mode (cmake -P) by the `accuracy-gate` target
# (benchmarking milestone M11, design page "Satellite Brightness Benchmarking").
#
# The SINGLE list of photometric accuracy checks. CI (.github/workflows/release.yml, Linux job)
# and a local run (`cmake --build <dir> --target accuracy-gate`) both drive this script, so the
# two can't drift. Any failing check fails the target, and in CI the PR:
#   1. every data/satellite_models/*.json through SatModelTool --selftest (known-value geometry,
#      earthshine, atmosphere, posed lobes vs brute force, occlusion vs ray-cast reference, the
#      GPU-form occlusion, trace and bulk-export round trips);
#   2. every data/benchmarks/*.json through --benchmark (the transcription reproduces the paper);
#   3. every data/benchmarks/*.json through --run-benchmark with the logged configuration
#      (SAMPLES, SEED): each compared metric must be within its tolerance.
#
# Required -D arguments (absolute paths):
#   TOOL      the SatModelTool executable
#   SRC_DIR   project source root
#   OUT_DIR   where reports, sample CSVs and OBJ exports go
# Optional:
#   SAMPLES (5000), SEED (1), SELFTEST_N (400)

cmake_minimum_required(VERSION 3.20)

foreach(arg TOOL SRC_DIR OUT_DIR)
    if(NOT DEFINED ${arg})
        message(FATAL_ERROR "AccuracyGate.cmake: -D${arg} is required")
    endif()
endforeach()
if(NOT DEFINED SAMPLES)
    set(SAMPLES 5000)
endif()
if(NOT DEFINED SEED)
    set(SEED 1)
endif()
if(NOT DEFINED SELFTEST_N)
    set(SELFTEST_N 400)
endif()
if(NOT EXISTS "${TOOL}")
    message(FATAL_ERROR "AccuracyGate.cmake: SatModelTool not found: ${TOOL}")
endif()

file(GLOB MODELS "${SRC_DIR}/data/satellite_models/*.json")
file(GLOB BENCHMARKS "${SRC_DIR}/data/benchmarks/*.json")
list(SORT MODELS)
list(SORT BENCHMARKS)
file(MAKE_DIRECTORY "${OUT_DIR}")

set(FAILED "")
# Runs one SatModelTool invocation from the source root (models are found relative to it), its
# output going straight to the console; records the step's name if it exits non-zero.
function(gate_step name)
    message(STATUS "── ${name}")
    execute_process(COMMAND "${TOOL}" ${ARGN}
                    WORKING_DIRECTORY "${SRC_DIR}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        set(FAILED "${FAILED};${name}" PARENT_SCOPE)
    endif()
endfunction()

gate_step("model selftests" ${MODELS} --selftest ${SELFTEST_N} --out "${OUT_DIR}/obj")
foreach(b IN LISTS BENCHMARKS)
    get_filename_component(id "${b}" NAME_WE)
    gate_step("transcription ${id}" --benchmark "${b}")
endforeach()
foreach(b IN LISTS BENCHMARKS)
    get_filename_component(id "${b}" NAME_WE)
    gate_step("run ${id}" --run-benchmark "${b}" --samples ${SAMPLES} --seed ${SEED}
              --report-dir "${OUT_DIR}/benchmark_runs" --models-dir "${SRC_DIR}/data/satellite_models")
endforeach()

list(REMOVE_ITEM FAILED "")
if(FAILED)
    list(JOIN FAILED ", " failedList)
    message(FATAL_ERROR "accuracy gate FAILED: ${failedList}\nReports: ${OUT_DIR}/benchmark_runs")
endif()
list(LENGTH MODELS nModels)
list(LENGTH BENCHMARKS nBench)
message(STATUS "accuracy gate passed: ${nModels} models self-tested, ${nBench} benchmarks transcribed and run "
               "(${SAMPLES} samples, seed ${SEED}). Reports: ${OUT_DIR}/benchmark_runs")

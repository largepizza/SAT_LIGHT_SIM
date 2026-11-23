# Writes the build stamp (git commit + build date) into ${OUT} at BUILD time, not configure time.
# Run as a script: cmake -DGIT=<git> -DSRC_DIR=<repo> -DOUT=<generated/build_stamp.cpp> -P BuildStamp.cmake
# The file is rewritten only when its text changes, so an unchanged commit recompiles nothing and a new
# commit recompiles only this one small source (the stamp lives here, not in version.h, which the big
# translation units include). Until 2026-11-22 the stamp was set at configure time and every issue report
# named the commit CMake last ran at (issue reports said 4988683a for builds hours newer).
set(commit "unknown")
if(GIT AND EXISTS "${SRC_DIR}/.git")
    execute_process(COMMAND "${GIT}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${SRC_DIR}" OUTPUT_VARIABLE out RESULT_VARIABLE res
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(res EQUAL 0 AND NOT out STREQUAL "")
        set(commit "${out}")
        # Tracked files changed since that commit: say so, the build is not that commit.
        execute_process(COMMAND "${GIT}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SRC_DIR}" OUTPUT_VARIABLE dirty RESULT_VARIABLE res2
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(res2 EQUAL 0 AND NOT dirty STREQUAL "")
            set(commit "${commit}-dirty")
        endif()
    endif()
endif()
string(TIMESTAMP date "%Y-%m-%d" UTC)
set(text "// Generated at build time by cmake/BuildStamp.cmake: do not edit.\n")
string(APPEND text "#define APP_GIT_COMMIT_STR \"${commit}\"\n")
string(APPEND text "#define APP_BUILD_DATE_STR \"${date}\"\n")
string(APPEND text "const char *satBuildCommit() { return APP_GIT_COMMIT_STR; }\n")
string(APPEND text "const char *satBuildDate() { return APP_BUILD_DATE_STR; }\n")
set(have "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" have)
endif()
if(NOT have STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()

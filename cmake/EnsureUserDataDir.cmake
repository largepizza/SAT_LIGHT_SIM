# EnsureUserDataDir.cmake — run in script mode (cmake -P) by the `<tgt>_userdata` target that
# CMakeLists.txt wires up through sat_ensure_user_data_dir().
#
# Keeps the app's user data (settings.json, reports/, perf_profiles/, bookmarks/, cinematics/, ...)
# OUT of the build tree. By default the app writes all of it next to the exe (Paths::userDataDir()),
# i.e. into build-win-release/Release — and release.bat deletes that tree before every release build.
# That took a day's settings and perf snapshots with it on 2026-10-13.
#
# This writes <exe dir>/user_data_dir.txt holding DATA_DIR; Paths::userDataDir() reads it and uses
# that folder instead. Every build tree's exe (build/, build-win-release/) then shares one set of
# user data, and wiping a tree loses nothing. Harness runs are unaffected (their own override wins),
# and cmake/PackageRelease.cmake is a whitelist, so the file never ships.
#
# The first time a tree gets its redirect, user data already sitting next to that exe is COPIED into
# DATA_DIR where DATA_DIR has no copy or an older one (the originals stay; they are no longer read).
#
# Required -D arguments:
#   RUNTIME_DIR   directory the executable lands in ($<TARGET_FILE_DIR:tgt>)
#   DATA_DIR      the stable user data folder (outside the build tree)

if(NOT DEFINED RUNTIME_DIR OR NOT DEFINED DATA_DIR)
    message(FATAL_ERROR "EnsureUserDataDir.cmake: -DRUNTIME_DIR and -DDATA_DIR are required")
endif()

set(REDIRECT "${RUNTIME_DIR}/user_data_dir.txt")

if(EXISTS "${REDIRECT}")
    file(READ "${REDIRECT}" _points_at)
    string(STRIP "${_points_at}" _points_at)
    if(_points_at STREQUAL "${DATA_DIR}")
        return() # the case on every build after the first: one file read, no output
    endif()
endif()

file(MAKE_DIRECTORY "${RUNTIME_DIR}")
file(MAKE_DIRECTORY "${DATA_DIR}")

# What the app keeps under its user data folder that is worth carrying over. Logs, the crash
# sentinel and the debug dumps (satellite_models_debug/, satellite_types_resolved.json) are
# rewritten every launch and are left behind.
set(_migrated 0)
foreach(_item settings.json bookmarks cinematics reports report_exports perf_profiles traces exports music_analysis)
    set(_src "${RUNTIME_DIR}/${_item}")
    if(IS_DIRECTORY "${_src}")
        file(GLOB_RECURSE _files RELATIVE "${_src}" "${_src}/*")
        foreach(_rel IN LISTS _files)
            if("${_src}/${_rel}" IS_NEWER_THAN "${DATA_DIR}/${_item}/${_rel}")
                get_filename_component(_dst_dir "${DATA_DIR}/${_item}/${_rel}" DIRECTORY)
                file(COPY "${_src}/${_rel}" DESTINATION "${_dst_dir}")
                math(EXPR _migrated "${_migrated} + 1")
            endif()
        endforeach()
    elseif(EXISTS "${_src}")
        if("${_src}" IS_NEWER_THAN "${DATA_DIR}/${_item}")
            file(COPY "${_src}" DESTINATION "${DATA_DIR}")
            math(EXPR _migrated "${_migrated} + 1")
        endif()
    endif()
endforeach()

file(WRITE "${REDIRECT}" "${DATA_DIR}\n")
message(STATUS "[user data] ${RUNTIME_DIR} -> ${DATA_DIR} (${_migrated} newer files copied over)")

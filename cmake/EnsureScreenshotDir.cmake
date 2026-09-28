# EnsureScreenshotDir.cmake — run in script mode (cmake -P) by the `<tgt>_screenshots` target
# that CMakeLists.txt wires up through sat_ensure_screenshot_dir().
#
# Keeps the app's screenshots/ output directory alive across build-tree wipes.
#
# SatelliteSim::requestScreenshot() writes to <exe dir>/screenshots, and <exe dir> is inside the
# build tree — build-win-release/Release for the windows-release preset. Anything that throws that
# tree away takes the screenshots with it: release.bat runs `rmdir /s /q build-win-release` before
# every release build, and so does a hand "delete the build dir and reconfigure".
#
# So instead of a real directory, <exe dir>/screenshots is kept as a *link* to SHOT_DIR — a stable
# directory outside the build tree — and this script re-creates that link whenever the build tree is
# regenerated:
#   Windows:   a directory junction (`mklink /J`) — unlike a symlink it needs neither elevation nor
#              Developer Mode, and it is transparent to the app: std::filesystem::create_directories()
#              on it just reports "already exists", which is what requestScreenshot() expects.
#   Elsewhere: a plain directory symlink.
# If neither can be created the app still works — a real screenshots/ directory is made instead — but
# a warning says the images will not survive the next build-tree wipe, and the next build retries.
#
# Required -D arguments (CMakeLists.txt's sat_ensure_screenshot_dir() supplies both):
#   RUNTIME_DIR   directory the executable lands in ($<TARGET_FILE_DIR:tgt>); its screenshots/ is
#                 what gets linked
#   SHOT_DIR      the stable directory the link points at (must be outside the build tree)
#
# Idempotent and cheap once set up: it reads a marker file through the link and returns, so running
# it on every build costs one file read and prints nothing.

if(NOT DEFINED RUNTIME_DIR OR NOT DEFINED SHOT_DIR)
    message(FATAL_ERROR "EnsureScreenshotDir.cmake: -DRUNTIME_DIR and -DSHOT_DIR are required")
endif()

# Written into SHOT_DIR and read back *through* the link: that read is what detects an
# already-correct link, and it works the same for a junction and a symlink (CMake's IS_SYMLINK
# happens to report junctions as symlinks, but the marker also catches a link left pointing
# somewhere stale, e.g. after SHOT_DIR is moved). Its content is the path it is supposed to point at.
set(MARKER_NAME ".satlight_link_target")
set(LINK "${RUNTIME_DIR}/screenshots")

file(MAKE_DIRECTORY "${RUNTIME_DIR}") # this target runs before the link step, so it may not exist yet

if(EXISTS "${LINK}/${MARKER_NAME}")
    file(READ "${LINK}/${MARKER_NAME}" _points_at)
    string(STRIP "${_points_at}" _points_at)
    if(_points_at STREQUAL "${SHOT_DIR}")
        return() # already linked to the right place — the case on every build after the first
    endif()
endif()

file(MAKE_DIRECTORY "${SHOT_DIR}")
file(WRITE "${SHOT_DIR}/${MARKER_NAME}" "${SHOT_DIR}\n")

# Anything already at LINK that is not our link: a real screenshots/ directory from before this
# existed (its images must not be lost, so they are moved into SHOT_DIR first), or a stale/foreign
# link, which is removed without touching what it points at — file(REMOVE_RECURSE) on a reparse
# point removes only the link, verified against a junction on Windows.
if(IS_SYMLINK "${LINK}")
    file(REMOVE "${LINK}")
elseif(IS_DIRECTORY "${LINK}")
    message(STATUS "[screenshots] Migrating existing ${LINK} into ${SHOT_DIR}")
    file(GLOB _existing "${LINK}/*")
    foreach(_file IN LISTS _existing)
        get_filename_component(_name "${_file}" NAME)
        if(NOT EXISTS "${SHOT_DIR}/${_name}")
            file(RENAME "${_file}" "${SHOT_DIR}/${_name}")
        else()
            message(STATUS "[screenshots] ${_name} is already in ${SHOT_DIR}; keeping that copy")
        endif()
    endforeach()
    file(REMOVE_RECURSE "${LINK}")
endif()

if(WIN32)
    file(TO_NATIVE_PATH "${LINK}" _link_native)
    file(TO_NATIVE_PATH "${SHOT_DIR}" _shot_native)
    execute_process(
        COMMAND cmd /c mklink /J "${_link_native}" "${_shot_native}"
        RESULT_VARIABLE _link_result
        OUTPUT_VARIABLE _link_out
        ERROR_VARIABLE _link_err)
else()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${SHOT_DIR}" "${LINK}"
        RESULT_VARIABLE _link_result
        OUTPUT_VARIABLE _link_out
        ERROR_VARIABLE _link_err)
endif()

# A parallel build can have SatLightSimFresh create the same link first — that is a success, not a
# failure, so re-read the marker before treating a non-zero result as one.
if(NOT _link_result EQUAL 0 AND EXISTS "${LINK}/${MARKER_NAME}")
    file(READ "${LINK}/${MARKER_NAME}" _points_at)
    string(STRIP "${_points_at}" _points_at)
    if(_points_at STREQUAL "${SHOT_DIR}")
        set(_link_result 0)
    endif()
endif()

if(_link_result EQUAL 0)
    message(STATUS "[screenshots] ${LINK} -> ${SHOT_DIR}")
else()
    message(WARNING
        "[screenshots] Could not link ${LINK} to ${SHOT_DIR} (${_link_err}). Screenshots will land "
        "in a normal directory there and will NOT survive deleting the build tree; the next build "
        "retries.")
    file(MAKE_DIRECTORY "${LINK}")
endif()

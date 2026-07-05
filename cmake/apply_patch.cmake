# Idempotently apply a patch to a FetchContent source tree.
#
# Usage:
#   cmake -DPATCH_FILE=<patch> -DSOURCE_DIR=<dir> -P apply_patch.cmake
#
# If the patch is already applied (reverse-check succeeds) the script is a
# no-op, so FetchContent may re-run the patch step safely on reconfigure.

if(NOT DEFINED PATCH_FILE OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "apply_patch.cmake requires -DPATCH_FILE=<patch> and -DSOURCE_DIR=<dir>")
endif()

find_package(Git QUIET REQUIRED)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE PAGECORE_PATCH_ALREADY_APPLIED
    OUTPUT_QUIET
    ERROR_QUIET
)
if(PAGECORE_PATCH_ALREADY_APPLIED EQUAL 0)
    message(STATUS "Patch already applied, skipping: ${PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE PAGECORE_PATCH_RESULT
    ERROR_VARIABLE PAGECORE_PATCH_ERROR
)
if(NOT PAGECORE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to apply ${PATCH_FILE} to ${SOURCE_DIR}:\n${PAGECORE_PATCH_ERROR}\n"
        "The fetched sources may contain conflicting local changes. "
        "Remove them (e.g. `rm -rf build/_deps/litehtml-*`) and re-run CMake.")
endif()
message(STATUS "Applied patch: ${PATCH_FILE}")

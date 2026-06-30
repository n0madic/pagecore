if(NOT CONFIG MATCHES "^(Release|MinSizeRel)$")
    return()
endif()

if(NOT DEFINED TARGET_FILE OR TARGET_FILE STREQUAL "" OR NOT EXISTS "${TARGET_FILE}")
    return()
endif()

if(NOT DEFINED STRIP_TOOL OR STRIP_TOOL STREQUAL "")
    return()
endif()

message(STATUS "Stripping ${TARGET_FILE}")
execute_process(
    COMMAND "${STRIP_TOOL}" "${TARGET_FILE}"
    RESULT_VARIABLE strip_result
    OUTPUT_VARIABLE strip_output
    ERROR_VARIABLE strip_error
)

if(NOT strip_result EQUAL 0)
    string(STRIP "${strip_output}" strip_output)
    string(STRIP "${strip_error}" strip_error)
    message(FATAL_ERROR "Failed to strip ${TARGET_FILE}: ${strip_output} ${strip_error}")
endif()

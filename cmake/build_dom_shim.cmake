if(NOT DEFINED INPUTS)
    message(FATAL_ERROR "INPUTS is required")
endif()

if(NOT DEFINED CONCAT_OUTPUT)
    message(FATAL_ERROR "CONCAT_OUTPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED MINIFY)
    set(MINIFY OFF)
endif()

get_filename_component(CONCAT_OUTPUT_DIR "${CONCAT_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${CONCAT_OUTPUT_DIR}")

set(CONTENT "")
foreach(INPUT IN LISTS INPUTS)
    if(NOT EXISTS "${INPUT}")
        message(FATAL_ERROR "DOM shim input does not exist: ${INPUT}")
    endif()

    file(READ "${INPUT}" PART)
    string(APPEND CONTENT "${PART}")
    if(NOT PART MATCHES "\n$")
        string(APPEND CONTENT "\n")
    endif()
endforeach()

file(WRITE "${CONCAT_OUTPUT}" "${CONTENT}")

if(NOT MINIFY)
    if(NOT "${OUTPUT}" STREQUAL "${CONCAT_OUTPUT}")
        get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
        file(MAKE_DIRECTORY "${OUTPUT_DIR}")
        file(COPY_FILE "${CONCAT_OUTPUT}" "${OUTPUT}")
    endif()
    return()
endif()

if(NOT DEFINED MINIFIER OR NOT DEFINED MINIFIER_EXECUTABLE)
    message(FATAL_ERROR "MINIFIER and MINIFIER_EXECUTABLE are required when MINIFY is enabled")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

if(MINIFIER STREQUAL "terser")
    execute_process(
        COMMAND
            "${MINIFIER_EXECUTABLE}"
            "${CONCAT_OUTPUT}"
            --compress
            --mangle
            --output
            "${OUTPUT}"
        RESULT_VARIABLE MINIFIER_RESULT
        OUTPUT_VARIABLE MINIFIER_STDOUT
        ERROR_VARIABLE MINIFIER_STDERR
    )
elseif(MINIFIER STREQUAL "esbuild")
    execute_process(
        COMMAND
            "${MINIFIER_EXECUTABLE}"
            "${CONCAT_OUTPUT}"
            --minify
            "--outfile=${OUTPUT}"
            --log-level=warning
        RESULT_VARIABLE MINIFIER_RESULT
        OUTPUT_VARIABLE MINIFIER_STDOUT
        ERROR_VARIABLE MINIFIER_STDERR
    )
else()
    message(FATAL_ERROR "Unsupported DOM shim minifier: ${MINIFIER}")
endif()

if(NOT MINIFIER_RESULT EQUAL 0)
    message(FATAL_ERROR
        "DOM shim minification failed with ${MINIFIER}:\n"
        "${MINIFIER_STDOUT}\n"
        "${MINIFIER_STDERR}"
    )
endif()

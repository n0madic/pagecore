cmake_minimum_required(VERSION 3.24)

project(PageCore
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(PAGECORE_DEFAULT_BUILD_EXTRAS ON)
if(NOT PROJECT_IS_TOP_LEVEL)
    set(PAGECORE_DEFAULT_BUILD_EXTRAS OFF)
endif()

option(PAGECORE_BUILD_TESTS "Build PageCore tests" ${PAGECORE_DEFAULT_BUILD_EXTRAS})
option(PAGECORE_BUILD_TOOLS "Build PageCore command-line tools" ${PAGECORE_DEFAULT_BUILD_EXTRAS})
option(PAGECORE_BUILD_EXAMPLES "Build PageCore examples" OFF)
set(PAGECORE_DEFAULT_ENABLE_RENDERING ${PAGECORE_BUILD_TOOLS})
option(PAGECORE_ENABLE_RENDERING "Build the litehtml layout adapter and Cairo/Pango rendering pipeline" ${PAGECORE_DEFAULT_ENABLE_RENDERING})
option(PAGECORE_ENABLE_WEBP "Build WebP image decoding support" ON)
option(PAGECORE_ENABLE_SVG "Build SVG image rasterization support" ON)
set(PAGECORE_IMAGE_DECODER "system" CACHE STRING "Raster image decoder backend: system or stb")
set_property(CACHE PAGECORE_IMAGE_DECODER PROPERTY STRINGS system stb)
set(PAGECORE_IMAGE_DECODER_CHOICES system stb)
string(TOLOWER "${PAGECORE_IMAGE_DECODER}" PAGECORE_IMAGE_DECODER_NORMALIZED)
if(NOT PAGECORE_IMAGE_DECODER_NORMALIZED IN_LIST PAGECORE_IMAGE_DECODER_CHOICES)
    message(FATAL_ERROR "PAGECORE_IMAGE_DECODER must be one of: system, stb")
endif()
set(PAGECORE_IMAGE_DECODER "${PAGECORE_IMAGE_DECODER_NORMALIZED}" CACHE STRING "Raster image decoder backend: system or stb" FORCE)
option(PAGECORE_OPTIMIZE_SIZE "Enable size-focused release flags, linker dead-code elimination, LTO when available, and executable stripping" OFF)
option(PAGECORE_STRIP_RELEASE_BINARIES "Strip Release/MinSizeRel executables when PAGECORE_OPTIMIZE_SIZE is enabled" ON)
option(PAGECORE_MINIFY_DOM_SHIM "Minify the generated DOM shim before embedding it" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PAGECORE_OPTIMIZE_SIZE)
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT pagecore_ipo_supported
        OUTPUT pagecore_ipo_output
        LANGUAGES C CXX
    )
    if(pagecore_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "PAGECORE_OPTIMIZE_SIZE requested, but IPO/LTO is not supported: ${pagecore_ipo_output}")
    endif()

    if(MSVC)
        add_compile_options(
            "$<$<CONFIG:Release>:/Gy>"
            "$<$<CONFIG:Release>:/Gw>"
            "$<$<CONFIG:MinSizeRel>:/Gy>"
            "$<$<CONFIG:MinSizeRel>:/Gw>"
        )
        add_link_options(
            "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:/OPT:ICF>"
            "$<$<CONFIG:MinSizeRel>:/OPT:REF>"
            "$<$<CONFIG:MinSizeRel>:/OPT:ICF>"
        )
    else()
        add_compile_options(
            "$<$<CONFIG:Release>:-ffunction-sections>"
            "$<$<CONFIG:Release>:-fdata-sections>"
            "$<$<CONFIG:MinSizeRel>:-ffunction-sections>"
            "$<$<CONFIG:MinSizeRel>:-fdata-sections>"
        )

        if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            add_compile_options("$<$<CONFIG:MinSizeRel>:-Oz>")
        endif()

        if(APPLE)
            add_link_options(
                "$<$<CONFIG:Release>:LINKER:-dead_strip>"
                "$<$<CONFIG:MinSizeRel>:LINKER:-dead_strip>"
            )
        else()
            add_link_options(
                "$<$<CONFIG:Release>:LINKER:--gc-sections>"
                "$<$<CONFIG:MinSizeRel>:LINKER:--gc-sections>"
            )
        endif()
    endif()
endif()

include(FetchContent)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

set(PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL EXCLUDE_FROM_ALL)
endif()

function(pagecore_strip_release_binary target)
    if(NOT PAGECORE_OPTIMIZE_SIZE OR NOT PAGECORE_STRIP_RELEASE_BINARIES)
        return()
    endif()

    if(NOT CMAKE_STRIP)
        message(WARNING "PAGECORE_STRIP_RELEASE_BINARIES requested, but CMAKE_STRIP is not set")
        return()
    endif()

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND}
            "-DSTRIP_TOOL=${CMAKE_STRIP}"
            "-DTARGET_FILE=$<TARGET_FILE:${target}>"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/strip_release_binary.cmake"
        VERBATIM
    )
endfunction()

function(pagecore_pkg_config_static_first target)
    add_library(${target} INTERFACE)
    set(modules ${ARGN})

    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --exists ${modules}
        RESULT_VARIABLE pkg_exists
    )
    if(NOT pkg_exists EQUAL 0)
        message(FATAL_ERROR "Required pkg-config modules not found: ${modules}")
    endif()

    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags ${modules}
        OUTPUT_VARIABLE pkg_cflags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    separate_arguments(pkg_cflags_list UNIX_COMMAND "${pkg_cflags}")
    foreach(flag IN LISTS pkg_cflags_list)
        if(flag MATCHES "^-I(.+)")
            target_include_directories(${target} INTERFACE "${CMAKE_MATCH_1}")
        elseif(flag MATCHES "^-D(.+)")
            target_compile_definitions(${target} INTERFACE "${CMAKE_MATCH_1}")
        elseif(flag STREQUAL "-pthread")
            target_compile_options(${target} INTERFACE "${flag}")
        elseif(NOT flag STREQUAL "")
            target_compile_options(${target} INTERFACE "${flag}")
        endif()
    endforeach()

    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --libs --static ${modules}
        OUTPUT_VARIABLE pkg_libs
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    separate_arguments(pkg_libs_list UNIX_COMMAND "${pkg_libs}")

    set(pkg_lib_dirs)
    foreach(flag IN LISTS pkg_libs_list)
        if(flag MATCHES "^-L(.+)")
            list(APPEND pkg_lib_dirs "${CMAKE_MATCH_1}")
            target_link_directories(${target} INTERFACE "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(APPLE)
        # Homebrew installs static libraries outside the pkg-config -L search
        # paths; include the standard Homebrew prefixes so the static-first
        # resolution below can find them. Guarded to Apple so these non-portable
        # paths never pull a stray system .a into non-macOS builds.
        list(APPEND pkg_lib_dirs "/opt/homebrew/lib" "/usr/local/lib")
    endif()

    # Fontconfig has process-global state used by Cairo/Pango. When Cairo is
    # dynamic, mixing it with a static libfontconfig copy leaves two independent
    # registries in one process and can trigger spurious FcInit warnings.
    set(pkg_dynamic_state_libs fontconfig)

    list(LENGTH pkg_libs_list pkg_libs_len)
    set(i 0)
    while(i LESS pkg_libs_len)
        list(GET pkg_libs_list ${i} flag)

        if(flag MATCHES "^-L")
            # Library search paths are consumed while resolving -lfoo to libfoo.a.
        elseif(flag STREQUAL "-framework")
            math(EXPR i "${i} + 1")
            if(i LESS pkg_libs_len)
                list(GET pkg_libs_list ${i} framework_name)
                target_link_options(${target} INTERFACE "SHELL:-framework ${framework_name}")
            endif()
        elseif(flag MATCHES "^-l(.+)")
            set(lib_name "${CMAKE_MATCH_1}")
            set(found_static_lib "")
            if(NOT lib_name IN_LIST pkg_dynamic_state_libs)
                foreach(lib_dir IN LISTS pkg_lib_dirs)
                    if(EXISTS "${lib_dir}/lib${lib_name}.a")
                        set(found_static_lib "${lib_dir}/lib${lib_name}.a")
                        break()
                    endif()
                endforeach()
            endif()

            if(NOT found_static_lib STREQUAL "")
                target_link_libraries(${target} INTERFACE "${found_static_lib}")
            else()
                target_link_libraries(${target} INTERFACE "${flag}")
            endif()
        elseif(flag STREQUAL "-pthread")
            target_compile_options(${target} INTERFACE "${flag}")
            target_link_options(${target} INTERFACE "${flag}")
        elseif(NOT flag STREQUAL "")
            target_link_options(${target} INTERFACE "${flag}")
        endif()

        math(EXPR i "${i} + 1")
    endwhile()
endfunction()

function(pagecore_fetchcontent_make_available_excluded dependency)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        FetchContent_MakeAvailable(${dependency})
        return()
    endif()

    string(TOLOWER "${dependency}" dependency_lc)
    FetchContent_GetProperties(${dependency})
    if(NOT ${dependency_lc}_POPULATED)
        FetchContent_Populate(${dependency})
        add_subdirectory(
            "${${dependency_lc}_SOURCE_DIR}"
            "${${dependency_lc}_BINARY_DIR}"
            EXCLUDE_FROM_ALL
        )
    endif()
endfunction()

if(PAGECORE_ENABLE_RENDERING)
    set(PAGECORE_RENDERING_PKG_MODULES
        cairo
        pangocairo
        pangoft2
        fontconfig
        libbrotlidec
    )
    if(PAGECORE_IMAGE_DECODER STREQUAL "system")
        list(APPEND PAGECORE_RENDERING_PKG_MODULES libturbojpeg)
    endif()
    if(PAGECORE_ENABLE_WEBP)
        list(APPEND PAGECORE_RENDERING_PKG_MODULES libwebp libwebpdecoder)
    endif()

    pagecore_pkg_config_static_first(
        pagecore_static_pkg_deps
        ${PAGECORE_RENDERING_PKG_MODULES}
    )

    if(PAGECORE_IMAGE_DECODER STREQUAL "system")
        find_path(GIFLIB_INCLUDE_DIR NAMES gif_lib.h)
        set(PAGECORE_SAVED_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX} ${CMAKE_FIND_LIBRARY_SUFFIXES})
        find_library(GIFLIB_STATIC_LIBRARY NAMES gif)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${PAGECORE_SAVED_FIND_LIBRARY_SUFFIXES})
        find_library(GIFLIB_LIBRARY NAMES gif)
        if(GIFLIB_STATIC_LIBRARY)
            set(GIFLIB_LIBRARY "${GIFLIB_STATIC_LIBRARY}")
        endif()
        if(NOT GIFLIB_INCLUDE_DIR OR NOT GIFLIB_LIBRARY)
            message(FATAL_ERROR "giflib is required for GIF first-frame decoding")
        endif()
        target_include_directories(pagecore_static_pkg_deps INTERFACE "${GIFLIB_INCLUDE_DIR}")
        target_link_libraries(pagecore_static_pkg_deps INTERFACE "${GIFLIB_LIBRARY}")
    endif()
else()
    add_library(pagecore_static_pkg_deps INTERFACE)
endif()

set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS_CPP OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    lexbor
    GIT_REPOSITORY https://github.com/lexbor/lexbor.git
    GIT_TAG f1ba992ba33e7cf7d39936a9fd43444d7b31b600
    ${PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL}
)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    quickjs_ng
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG 377a25e0e646356670eef3d3f03d9c4839b23d6d
    ${PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL}
)

pagecore_fetchcontent_make_available_excluded(lexbor)
pagecore_fetchcontent_make_available_excluded(quickjs_ng)

# zlib is required separately below and linked transitively through CURL::libcurl.
# Requesting it here as a CURL "component" is fragile: in CONFIG mode
# (CURLConfig.cmake) "libz" is a feature string that may not be exposed as a
# component, causing a spurious hard configure failure.
find_package(CURL 7.64.0 REQUIRED)
find_package(ZLIB REQUIRED)

find_program(PAGECORE_NODE_EXECUTABLE NAMES node)

set(PAGECORE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PAGECORE_DOM_SHIM_PARTS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/00_runtime.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/10_core.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/20_events.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/30_dom.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/40_web.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/45_forms.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/50_streams.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/60_compat.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/90_install.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dom_shim/99_boot.js"
)
set(PAGECORE_DOM_SHIM_SOURCE "${PAGECORE_GENERATED_DIR}/dom_shim.js")
set(PAGECORE_DOM_SHIM_EMBED_INPUT "${PAGECORE_DOM_SHIM_SOURCE}")
set(PAGECORE_DOM_SHIM_BUILD_OUTPUTS "${PAGECORE_DOM_SHIM_SOURCE}")
set(PAGECORE_DOM_SHIM_BUILD_ARGS -DMINIFY=OFF)

if(PAGECORE_MINIFY_DOM_SHIM)
    find_program(PAGECORE_TERSER_EXECUTABLE NAMES terser)
    find_program(PAGECORE_ESBUILD_EXECUTABLE NAMES esbuild)

    if(PAGECORE_TERSER_EXECUTABLE)
        set(PAGECORE_DOM_SHIM_MINIFIER terser)
        set(PAGECORE_DOM_SHIM_MINIFIER_EXECUTABLE "${PAGECORE_TERSER_EXECUTABLE}")
    elseif(PAGECORE_ESBUILD_EXECUTABLE)
        set(PAGECORE_DOM_SHIM_MINIFIER esbuild)
        set(PAGECORE_DOM_SHIM_MINIFIER_EXECUTABLE "${PAGECORE_ESBUILD_EXECUTABLE}")
    else()
        message(FATAL_ERROR "PAGECORE_MINIFY_DOM_SHIM=ON requires terser or esbuild in PATH")
    endif()

    set(PAGECORE_DOM_SHIM_EMBED_INPUT "${PAGECORE_GENERATED_DIR}/dom_shim.min.js")
    list(APPEND PAGECORE_DOM_SHIM_BUILD_OUTPUTS "${PAGECORE_DOM_SHIM_EMBED_INPUT}")
    set(PAGECORE_DOM_SHIM_BUILD_ARGS
        -DMINIFY=ON
        "-DMINIFIER=${PAGECORE_DOM_SHIM_MINIFIER}"
        "-DMINIFIER_EXECUTABLE=${PAGECORE_DOM_SHIM_MINIFIER_EXECUTABLE}"
    )
endif()

# Strip comments from the concatenated shim before embedding it. Requires node
# (a tokenizer-aware strip; a regex would corrupt strings/regex literals). When
# node is absent the shim is embedded with comments intact.
set(PAGECORE_DOM_SHIM_STRIPPER "${CMAKE_CURRENT_SOURCE_DIR}/cmake/strip_js_comments.js")
option(PAGECORE_STRIP_DOM_SHIM_COMMENTS "Strip comments from the generated DOM shim" ON)
if(PAGECORE_STRIP_DOM_SHIM_COMMENTS AND PAGECORE_NODE_EXECUTABLE)
    list(APPEND PAGECORE_DOM_SHIM_BUILD_ARGS
        -DSTRIP_COMMENTS=ON
        "-DNODE_EXECUTABLE=${PAGECORE_NODE_EXECUTABLE}"
        "-DCOMMENT_STRIPPER=${PAGECORE_DOM_SHIM_STRIPPER}"
    )
endif()

add_custom_command(
    OUTPUT ${PAGECORE_DOM_SHIM_BUILD_OUTPUTS}
    COMMAND
        ${CMAKE_COMMAND}
        "-DINPUTS=${PAGECORE_DOM_SHIM_PARTS}"
        "-DCONCAT_OUTPUT=${PAGECORE_DOM_SHIM_SOURCE}"
        "-DOUTPUT=${PAGECORE_DOM_SHIM_EMBED_INPUT}"
        ${PAGECORE_DOM_SHIM_BUILD_ARGS}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_dom_shim.cmake
    DEPENDS
        ${PAGECORE_DOM_SHIM_PARTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_dom_shim.cmake"
        "${PAGECORE_DOM_SHIM_STRIPPER}"
    VERBATIM
)

set(PAGECORE_DOM_SHIM_HEADER "${PAGECORE_GENERATED_DIR}/dom_shim.hpp")
add_custom_command(
    OUTPUT "${PAGECORE_DOM_SHIM_HEADER}"
    COMMAND
        ${CMAKE_COMMAND}
        -DINPUT=${PAGECORE_DOM_SHIM_EMBED_INPUT}
        -DOUTPUT=${PAGECORE_DOM_SHIM_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS
        "${PAGECORE_DOM_SHIM_EMBED_INPUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
    VERBATIM
)
add_custom_target(pagecore_dom_shim DEPENDS "${PAGECORE_DOM_SHIM_HEADER}")

if(PAGECORE_NODE_EXECUTABLE)
    add_custom_target(pagecore_dom_shim_check
        COMMAND
            "${PAGECORE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dom_shim_modules.js"
            ${PAGECORE_DOM_SHIM_PARTS}
        DEPENDS
            ${PAGECORE_DOM_SHIM_PARTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dom_shim_modules.js"
        VERBATIM
    )
    add_custom_target(pagecore_dom_shim_unit_tests
        COMMAND
            "${PAGECORE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/dom_shim/dom_shim_unit_tests.js"
        DEPENDS
            ${PAGECORE_DOM_SHIM_PARTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/dom_shim/dom_shim_unit_tests.js"
        VERBATIM
    )
endif()

if(PAGECORE_ENABLE_RENDERING)
    set(PAGECORE_SAVED_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    set(PAGECORE_SAVED_POLICY_VERSION_MINIMUM "${CMAKE_POLICY_VERSION_MINIMUM}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(
        woff2
        GIT_REPOSITORY https://github.com/google/woff2.git
        GIT_TAG fb9c3379f2605b10f3e8f1d9636664ab5576775c
        ${PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL}
    )
    pagecore_fetchcontent_make_available_excluded(woff2)
    set(BUILD_SHARED_LIBS "${PAGECORE_SAVED_BUILD_SHARED_LIBS}" CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM "${PAGECORE_SAVED_POLICY_VERSION_MINIMUM}" CACHE STRING "" FORCE)

    set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        litehtml
        GIT_REPOSITORY https://github.com/litehtml/litehtml.git
        GIT_TAG 932439c91afb04dbce30903673292e3bf2da01dc
        ${PAGECORE_FETCHCONTENT_EXCLUDE_FROM_ALL}
    )
    pagecore_fetchcontent_make_available_excluded(litehtml)
endif()

add_library(pagecore
    "${PAGECORE_DOM_SHIM_HEADER}"
    src/base64_codec.cpp
    src/cookie_jar.cpp
    src/display_list_dump.cpp
    src/dom_document.cpp
    src/image_io.cpp
    src/js_runtime.cpp
    src/page.cpp
    src/page_activity_tracker.cpp
    src/resource_loader.cpp
)
add_dependencies(pagecore pagecore_dom_shim)
add_library(PageCore::pagecore ALIAS pagecore)

if(PAGECORE_ENABLE_RENDERING)
    target_sources(pagecore PRIVATE
        src/cairo_raster.cpp
        src/image_decoder.cpp
        src/litehtml_layout.cpp
        src/web_fonts.cpp
    )
else()
    target_sources(pagecore PRIVATE src/rendering_disabled.cpp)
endif()

target_include_directories(pagecore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/base64/include
        ${PAGECORE_GENERATED_DIR}
)

if(PAGECORE_ENABLE_RENDERING)
    target_include_directories(pagecore PRIVATE "${woff2_SOURCE_DIR}/include")
    if(PAGECORE_IMAGE_DECODER STREQUAL "stb")
        target_include_directories(pagecore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
        target_compile_definitions(pagecore PRIVATE PAGECORE_IMAGE_DECODER_STB=1)
    else()
        target_compile_definitions(pagecore PRIVATE PAGECORE_IMAGE_DECODER_SYSTEM=1)
    endif()
endif()

if(PAGECORE_ENABLE_WEBP)
    target_compile_definitions(pagecore PUBLIC PAGECORE_ENABLE_WEBP=1)
else()
    target_compile_definitions(pagecore PUBLIC PAGECORE_ENABLE_WEBP=0)
endif()

if(PAGECORE_ENABLE_SVG)
    target_compile_definitions(pagecore PUBLIC PAGECORE_ENABLE_SVG=1)
else()
    target_compile_definitions(pagecore PUBLIC PAGECORE_ENABLE_SVG=0)
endif()

target_link_libraries(pagecore
    PRIVATE
        $<BUILD_INTERFACE:lexbor_static>
        $<BUILD_INTERFACE:qjs>
        $<BUILD_INTERFACE:CURL::libcurl>
        $<BUILD_INTERFACE:ZLIB::ZLIB>
        $<BUILD_INTERFACE:pagecore_static_pkg_deps>
    INTERFACE
        $<INSTALL_INTERFACE:PageCore::lexbor_static>
        $<INSTALL_INTERFACE:PageCore::qjs>
        $<INSTALL_INTERFACE:CURL::libcurl>
        $<INSTALL_INTERFACE:ZLIB::ZLIB>
        $<INSTALL_INTERFACE:PageCore::pagecore_static_pkg_deps>
)

target_compile_features(pagecore PUBLIC cxx_std_20)

if(PAGECORE_ENABLE_RENDERING)
    target_compile_definitions(pagecore PUBLIC PAGECORE_ENABLE_RENDERING=1)
    target_link_libraries(pagecore
        PRIVATE
            $<BUILD_INTERFACE:litehtml>
            $<BUILD_INTERFACE:woff2dec>
            $<BUILD_INTERFACE:woff2common>
        INTERFACE
            $<INSTALL_INTERFACE:PageCore::litehtml>
            $<INSTALL_INTERFACE:PageCore::woff2dec>
            $<INSTALL_INTERFACE:PageCore::woff2common>
    )
endif()

if(PAGECORE_BUILD_TOOLS)
    add_executable(pagecore_cli tools/pagecore_cli.cpp)
    target_link_libraries(pagecore_cli PRIVATE pagecore)
    pagecore_strip_release_binary(pagecore_cli)
endif()

if(PAGECORE_BUILD_EXAMPLES)
    add_executable(pagecore_embed_minimal examples/embed_minimal.cpp)
    target_link_libraries(pagecore_embed_minimal PRIVATE pagecore)
endif()

if(PAGECORE_BUILD_TESTS)
    enable_testing()
    if(PAGECORE_NODE_EXECUTABLE)
        add_test(
            NAME pagecore_dom_shim_modules
            COMMAND
                "${PAGECORE_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dom_shim_modules.js"
                ${PAGECORE_DOM_SHIM_PARTS}
        )
        add_test(
            NAME pagecore_dom_shim_unit_tests
            COMMAND
                "${PAGECORE_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/dom_shim/dom_shim_unit_tests.js"
        )
    endif()
    add_executable(pagecore_tests tests/pagecore_tests.cpp)
    target_compile_definitions(
        pagecore_tests
        PRIVATE
            PAGECORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            PAGECORE_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )
    target_include_directories(pagecore_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(pagecore_tests PRIVATE pagecore pagecore_static_pkg_deps)
    pagecore_strip_release_binary(pagecore_tests)
    add_test(NAME pagecore_tests COMMAND pagecore_tests)
    # Cap the suite so a hung loopback-server test (blocking accept()) fails the
    # run instead of stalling CI indefinitely.
    set_tests_properties(pagecore_tests PROPERTIES TIMEOUT 300)
    if(PAGECORE_ENABLE_RENDERING AND PAGECORE_BUILD_TOOLS)
        add_test(
            NAME pagecore_cli_screenshot
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body><div style='font-size:24px;color:#000;background:#fff'>Hello</div></body></html>"
                --format png
                --output "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_test.png"
                --viewport 64x48
        )
        add_test(
            NAME pagecore_cli_pdf_output
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body><div style='font-size:24px;color:#000;background:#fff'>Hello</div></body></html>"
                --format pdf
                --output "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_test.pdf"
                --viewport 64x48
        )
        add_test(
            NAME pagecore_cli_display_list_dump
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body><div style='width:20px;height:10px;background:#f00'></div></body></html>"
                --dump-display-list "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_display_list_test.json"
                --viewport 64x48
        )
        add_test(
            NAME pagecore_cli_full_page
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body style='margin:0'><div style='width:40px;height:300px;background:#f00'></div></body></html>"
                --format png
                --output "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_full_page_test.png"
                --viewport 40x10
                --full-page
        )
        add_test(
            NAME pagecore_cli_full_page_viewport_dependent_height
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body style='margin:0'><div style='height:100vh;background:#f00'></div><div style='height:300px;background:#0f0'></div></body></html>"
                --dump-display-list -
                --viewport 40x10
                --full-page
        )
        set_tests_properties(
            pagecore_cli_full_page_viewport_dependent_height
            PROPERTIES
                PASS_REGULAR_EXPRESSION "\"viewport\":\\{\"width\":40,\"height\":310"
        )
        add_test(
            NAME pagecore_cli_full_page_png_perf_trace_raster
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html><body style='margin:0'><div style='width:40px;height:300px;background:#f00'></div></body></html>"
                --format png
                --output "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_full_page_trace_test.png"
                --viewport 40x10
                --full-page
                --perf-trace -
        )
        set_tests_properties(
            pagecore_cli_full_page_png_perf_trace_raster
            PROPERTIES
                PASS_REGULAR_EXPRESSION "\"phase\":\"raster\""
        )

        # Negative argument-parsing cases must exit non-zero with a clear error.
        add_test(
            NAME pagecore_cli_rejects_viewport_overflow
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --viewport 99999999999x10
        )
        add_test(
            NAME pagecore_cli_rejects_viewport_over_max
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --viewport 200000x200000
        )
        add_test(
            NAME pagecore_cli_rejects_bad_scale
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --scale 1e30
        )
        add_test(
            NAME pagecore_cli_rejects_missing_value
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --viewport
        )
        add_test(
            NAME pagecore_cli_rejects_missing_output
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --format pdf
        )
        add_test(
            NAME pagecore_cli_rejects_conflicting_sources
            COMMAND $<TARGET_FILE:pagecore_cli> --html "<html></html>" --url "https://example.test/"
        )
        add_test(
            NAME pagecore_cli_rejects_scaled_dimension_overflow
            COMMAND
                $<TARGET_FILE:pagecore_cli>
                --html "<html></html>"
                --format png
                --output "${CMAKE_CURRENT_BINARY_DIR}/pagecore_cli_overflow.png"
                --viewport 16000x16000
                --scale 2
        )
        set_tests_properties(
            pagecore_cli_rejects_viewport_overflow
            pagecore_cli_rejects_viewport_over_max
            pagecore_cli_rejects_bad_scale
            pagecore_cli_rejects_missing_value
            pagecore_cli_rejects_missing_output
            pagecore_cli_rejects_conflicting_sources
            pagecore_cli_rejects_scaled_dimension_overflow
            PROPERTIES WILL_FAIL TRUE
        )
    endif()
endif()

set(PAGECORE_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/PageCore"
    CACHE STRING
    "Install location for PageCore CMake package files"
)
set(PAGECORE_INSTALL_DEPSDIR "${CMAKE_INSTALL_LIBDIR}/pagecore/deps")
set(PAGECORE_LEXBOR_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}lexbor_static${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAGECORE_QJS_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}qjs${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAGECORE_LITEHTML_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}litehtml${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAGECORE_GUMBO_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}gumbo${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAGECORE_WOFF2COMMON_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}woff2common${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAGECORE_WOFF2DEC_STATIC_FILE "${CMAKE_STATIC_LIBRARY_PREFIX}woff2dec${CMAKE_STATIC_LIBRARY_SUFFIX}")

install(
    TARGETS pagecore pagecore_static_pkg_deps
    EXPORT PageCoreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "$<TARGET_FILE:lexbor_static>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_LEXBOR_STATIC_FILE}")
install(FILES "$<TARGET_FILE:qjs>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_QJS_STATIC_FILE}")

if(PAGECORE_ENABLE_RENDERING)
    install(FILES "$<TARGET_FILE:litehtml>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_LITEHTML_STATIC_FILE}")
    install(FILES "$<TARGET_FILE:woff2common>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_WOFF2COMMON_STATIC_FILE}")
    install(FILES "$<TARGET_FILE:woff2dec>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_WOFF2DEC_STATIC_FILE}")
    if(TARGET gumbo)
        install(FILES "$<TARGET_FILE:gumbo>" DESTINATION "${PAGECORE_INSTALL_DEPSDIR}" RENAME "${PAGECORE_GUMBO_STATIC_FILE}")
    endif()
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/PageCoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PageCoreConfig.cmake"
    INSTALL_DESTINATION "${PAGECORE_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PageCoreConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    EXPORT PageCoreTargets
    NAMESPACE PageCore::
    DESTINATION "${PAGECORE_INSTALL_CMAKEDIR}"
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/PageCoreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/PageCoreConfigVersion.cmake"
    DESTINATION "${PAGECORE_INSTALL_CMAKEDIR}"
)

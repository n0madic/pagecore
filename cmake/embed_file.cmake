file(READ "${INPUT}" CONTENT)

# Guard against the embedded content prematurely terminating the C++ raw string
# literal (which would inject the trailing bytes as C++ tokens).
string(FIND "${CONTENT}" ")PAGECOREJS\"" _delimiter_collision)
if(NOT _delimiter_collision EQUAL -1)
    message(FATAL_ERROR
        "embed_file: ${INPUT} contains the raw-string delimiter )PAGECOREJS\"; choose a different delimiter")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT}" "#pragma once

#include <string_view>

namespace pagecore {
inline constexpr std::string_view kDomShim = R\"PAGECOREJS(
${CONTENT}
)PAGECOREJS\";
} // namespace pagecore
")

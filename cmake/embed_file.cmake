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

#include <cstddef>
#include <string_view>

namespace pagecore {
// Stored as a char array and viewed with an explicit length. Constructing the
// string_view from the raw literal directly (its single-argument, length-scanning
// constructor) makes GCC constexpr-evaluate char_traits::length over the whole
// ~200 KB blob, which ICEs cc1plus; sizeof avoids that scan entirely.
inline constexpr char kDomShimData[] = R\"PAGECOREJS(
${CONTENT}
)PAGECOREJS\";
inline constexpr std::string_view kDomShim{kDomShimData, sizeof(kDomShimData) - 1};
} // namespace pagecore
")

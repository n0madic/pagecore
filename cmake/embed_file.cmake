file(READ "${INPUT}" CONTENT)
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

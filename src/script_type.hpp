#pragma once

#include <optional>
#include <string>

namespace pagecore {

// Classifies the `type` attribute of a <script> element per the HTML spec's
// "classic vs module" rules. A missing or empty type, or any recognised
// JavaScript MIME type, counts as executable JavaScript.
bool is_javascript_script_type(const std::optional<std::string>& type);

// Returns true only when the `type` attribute names an ES module (`module`).
bool is_module_script_type(const std::optional<std::string>& type);

} // namespace pagecore

#pragma once

#include "pagecore/render.hpp"

#include <cairo.h>
#include <pango/pango.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pagecore {

struct CssFontFace {
    std::string family;
    std::vector<std::string> sources;
    int weight = 400;
    bool italic = false;
};

struct WebFontSource {
    std::string css_family;
    std::string url;
    std::string body;
    int weight = 400;
    bool italic = false;
};

std::vector<CssFontFace> extract_font_faces(std::string_view css);
std::shared_ptr<const FontEnvironment> create_font_environment(const std::vector<WebFontSource>& fonts);
PangoLayout* create_pango_layout_for_cairo(cairo_t* cr, const std::shared_ptr<const FontEnvironment>& font_environment);

} // namespace pagecore

#include "pagecore/render.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <variant>

namespace pagecore {
namespace {

std::string escaped(std::string_view value)
{
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            const auto uch = static_cast<unsigned char>(ch);
            if (uch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(uch);
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

const char* repeat_name(ImageRepeat repeat)
{
    switch (repeat) {
    case ImageRepeat::Repeat:
        return "repeat";
    case ImageRepeat::RepeatX:
        return "repeat-x";
    case ImageRepeat::RepeatY:
        return "repeat-y";
    case ImageRepeat::NoRepeat:
        return "no-repeat";
    }
    return "unknown";
}

const char* border_style_name(BorderStyle style)
{
    switch (style) {
    case BorderStyle::None:
        return "none";
    case BorderStyle::Solid:
        return "solid";
    case BorderStyle::Dotted:
        return "dotted";
    case BorderStyle::Dashed:
        return "dashed";
    case BorderStyle::Double:
        return "double";
    case BorderStyle::Other:
        return "other";
    }
    return "unknown";
}

void write_rect(std::ostream& out, const Rect& rect)
{
    out << "{\"x\":" << rect.x
        << ",\"y\":" << rect.y
        << ",\"width\":" << rect.width
        << ",\"height\":" << rect.height
        << '}';
}

void write_point(std::ostream& out, const Point& point)
{
    out << "{\"x\":" << point.x << ",\"y\":" << point.y << '}';
}

void write_color(std::ostream& out, const Color& color)
{
    out << "{\"r\":" << static_cast<int>(color.r)
        << ",\"g\":" << static_cast<int>(color.g)
        << ",\"b\":" << static_cast<int>(color.b)
        << ",\"a\":" << static_cast<int>(color.a)
        << '}';
}

void write_corner(std::ostream& out, const CornerRadii& radii)
{
    out << "{\"x\":" << radii.x << ",\"y\":" << radii.y << '}';
}

void write_radii(std::ostream& out, const BorderRadii& radii)
{
    out << "{\"topLeft\":";
    write_corner(out, radii.top_left);
    out << ",\"topRight\":";
    write_corner(out, radii.top_right);
    out << ",\"bottomRight\":";
    write_corner(out, radii.bottom_right);
    out << ",\"bottomLeft\":";
    write_corner(out, radii.bottom_left);
    out << '}';
}

void write_border_side(std::ostream& out, const BorderSide& side)
{
    out << "{\"width\":" << side.width
        << ",\"style\":\"" << border_style_name(side.style)
        << "\",\"color\":";
    write_color(out, side.color);
    out << '}';
}

void write_command(std::ostream& out, const TextCommand& command)
{
    out << "{\"type\":\"text\",\"text\":\"" << escaped(command.text) << "\",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"color\":";
    write_color(out, command.color);
    out << ",\"font\":{\"family\":\"" << escaped(command.font.family)
        << "\",\"sizePx\":" << command.font.size_px
        << ",\"weight\":" << command.font.weight
        << ",\"italic\":" << (command.font.italic ? "true" : "false")
        << "}}";
}

void write_command(std::ostream& out, const SolidFillCommand& command)
{
    out << "{\"type\":\"solidFill\",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"color\":";
    write_color(out, command.color);
    out << ",\"isRoot\":" << (command.is_root ? "true" : "false") << ",\"radii\":";
    write_radii(out, command.radii);
    out << '}';
}

void write_command(std::ostream& out, const BorderCommand& command)
{
    out << "{\"type\":\"border\",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"left\":";
    write_border_side(out, command.left);
    out << ",\"top\":";
    write_border_side(out, command.top);
    out << ",\"right\":";
    write_border_side(out, command.right);
    out << ",\"bottom\":";
    write_border_side(out, command.bottom);
    out << ",\"isRoot\":" << (command.is_root ? "true" : "false") << ",\"radii\":";
    write_radii(out, command.radii);
    out << '}';
}

void write_command(std::ostream& out, const ImageCommand& command)
{
    out << "{\"type\":\"image\",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"clip\":";
    write_rect(out, command.clip);
    out << ",\"tile\":";
    write_rect(out, command.tile);
    out << ",\"repeat\":\"" << repeat_name(command.repeat)
        << "\",\"url\":\"" << escaped(command.url)
        << "\",\"baseUrl\":\"" << escaped(command.base_url)
        << "\",\"decoded\":";
    if (command.image) {
        out << "{\"width\":" << command.image->width << ",\"height\":" << command.image->height << '}';
    } else {
        out << "null";
    }
    out << ",\"radii\":";
    write_radii(out, command.radii);
    out << '}';
}

void write_command(std::ostream& out, const LinearGradientCommand& command)
{
    out << "{\"type\":\"linearGradient\",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"clip\":";
    write_rect(out, command.clip);
    out << ",\"start\":";
    write_point(out, command.start);
    out << ",\"end\":";
    write_point(out, command.end);
    out << ",\"stops\":[";
    for (std::size_t index = 0; index < command.stops.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << "{\"offset\":" << command.stops[index].offset << ",\"color\":";
        write_color(out, command.stops[index].color);
        out << '}';
    }
    out << "],\"radii\":";
    write_radii(out, command.radii);
    out << '}';
}

void write_command(std::ostream& out, const ClipCommand& command)
{
    out << "{\"type\":\"clip\",\"push\":" << (command.push ? "true" : "false") << ",\"rect\":";
    write_rect(out, command.rect);
    out << ",\"radii\":";
    write_radii(out, command.radii);
    out << '}';
}

} // namespace

std::string display_list_to_json(const DisplayList& display_list)
{
    std::ostringstream out;
    out << std::setprecision(6);
    out << "{\"viewport\":{\"width\":" << display_list.viewport.width
        << ",\"height\":" << display_list.viewport.height
        << ",\"deviceScaleFactor\":" << display_list.viewport.device_scale_factor
        << "},\"contentWidth\":" << display_list.content_width
        << ",\"contentHeight\":" << display_list.content_height
        << ",\"commands\":[";
    for (std::size_t index = 0; index < display_list.commands.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        std::visit([&](const auto& command) { write_command(out, command); }, display_list.commands[index]);
    }
    out << "]}";
    return out.str();
}

} // namespace pagecore

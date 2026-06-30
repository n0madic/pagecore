#include "pagecore/page.hpp"
#include "pagecore/image_io.hpp"
#include "pagecore/render.hpp"

#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string read_stdin()
{
    std::ostringstream out;
    out << std::cin.rdbuf();
    return out.str();
}

std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open input file: " + path);
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void usage(const char* argv0)
{
    std::cerr
        << "usage:\n"
        << "  " << argv0 << " --url URL [--eval JS] [--screenshot PATH.png] [--dump-display-list PATH|-]\n"
        << "  " << argv0 << " --file PATH [--eval JS] [--screenshot PATH.png] [--dump-display-list PATH|-]\n"
        << "  " << argv0 << " --html HTML [--eval JS] [--screenshot PATH.png] [--dump-display-list PATH|-]\n"
        << "  echo HTML | " << argv0 << " --stdin [--eval JS] [--screenshot PATH.png] [--dump-display-list PATH|-]\n"
        << "options:\n"
        << "  --viewport WIDTHxHEIGHT  screenshot viewport, default 1280x720\n"
        << "  --scale NUMBER           screenshot device scale factor, default 1\n"
        << "  --wait-ms NUMBER         async timer/XHR/fetch wait budget, default 5000\n"
        << "  --js-memory-mb NUMBER    QuickJS heap limit, default 256\n";
}

int parse_positive_int(const std::string& value, const std::string& name)
{
    std::size_t parsed = 0;
    const int result = std::stoi(value, &parsed);
    if (parsed != value.size() || result <= 0) {
        throw std::runtime_error(name + " must be a positive integer");
    }
    return result;
}

int parse_nonnegative_int(const std::string& value, const std::string& name)
{
    std::size_t parsed = 0;
    const int result = std::stoi(value, &parsed);
    if (parsed != value.size() || result < 0) {
        throw std::runtime_error(name + " must be a non-negative integer");
    }
    return result;
}

pagecore::Viewport parse_viewport(const std::string& value)
{
    const auto separator = value.find('x');
    if (separator == std::string::npos || separator == 0 || separator + 1 == value.size()) {
        throw std::runtime_error("viewport must be formatted as WIDTHxHEIGHT");
    }

    pagecore::Viewport viewport;
    viewport.width = parse_positive_int(value.substr(0, separator), "viewport width");
    viewport.height = parse_positive_int(value.substr(separator + 1), "viewport height");
    return viewport;
}

float parse_positive_float(const std::string& value, const std::string& name)
{
    std::size_t parsed = 0;
    const float result = std::stof(value, &parsed);
    if (parsed != value.size() || !std::isfinite(result) || result <= 0.0f) {
        throw std::runtime_error(name + " must be a positive number");
    }
    return result;
}

void write_text_file(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open output file: " + path);
    }
    out << text;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        std::string url;
        std::string html;
        std::string file;
        std::string eval;
        std::string screenshot;
        std::string display_list_dump;
        pagecore::RenderOptions render_options;
        pagecore::LoadOptions load_options;
        bool stdin_input = false;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error("missing value for " + arg);
                }
                return argv[++i];
            };

            if (arg == "--url") url = next();
            else if (arg == "--html") html = next();
            else if (arg == "--file") file = next();
            else if (arg == "--eval") eval = next();
            else if (arg == "--screenshot") screenshot = next();
            else if (arg == "--dump-display-list") display_list_dump = next();
            else if (arg == "--viewport") {
                const float scale = render_options.viewport.device_scale_factor;
                render_options.viewport = parse_viewport(next());
                render_options.viewport.device_scale_factor = scale;
            }
            else if (arg == "--scale") render_options.viewport.device_scale_factor = parse_positive_float(next(), "scale");
            else if (arg == "--wait-ms") load_options.wait_time = std::chrono::milliseconds(parse_nonnegative_int(next(), "wait-ms"));
            else if (arg == "--js-memory-mb") {
                load_options.js_memory_limit_bytes = static_cast<std::size_t>(parse_positive_int(next(), "js-memory-mb")) * 1024 * 1024;
            }
            else if (arg == "--stdin") stdin_input = true;
            else if (arg == "--help" || arg == "-h") {
                usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + arg);
            }
        }

        pagecore::Page page(load_options);
        if (!url.empty()) {
            page.load_url(url);
        } else if (!file.empty()) {
            page.load_html(read_file(file), "file://" + file);
        } else if (!html.empty()) {
            page.load_html(html);
        } else if (stdin_input) {
            page.load_html(read_stdin());
        } else {
            usage(argv[0]);
            return 2;
        }

        if (!screenshot.empty()) {
            pagecore::write_png_rgba(page.render(render_options), screenshot);
        }

        if (!display_list_dump.empty()) {
            const std::string dump = pagecore::display_list_to_json(page.display_list(render_options));
            if (display_list_dump == "-") {
                std::cout << dump << "\n";
            } else {
                write_text_file(display_list_dump, dump);
            }
        }

        if (!eval.empty()) {
            std::cout << page.eval(eval) << "\n";
        } else if (!screenshot.empty()) {
            std::cout << screenshot << "\n";
        } else if (!display_list_dump.empty()) {
            if (display_list_dump != "-") {
                std::cout << display_list_dump << "\n";
            }
        } else {
            std::cout << page.serialize_html() << "\n";
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pagecore_cli: " << error.what() << "\n";
        return 1;
    }
}

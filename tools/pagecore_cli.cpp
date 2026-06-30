#include "pagecore/page.hpp"
#include "pagecore/image_io.hpp"
#include "pagecore/render.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

enum class OutputFormat {
    Html,
    Png,
    Pdf,
};

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
        << "  " << argv0 << " --url URL [--eval JS] [--format html|png|pdf] [--output PATH] [--dump-display-list PATH|-]\n"
        << "  " << argv0 << " --file PATH [--eval JS] [--format html|png|pdf] [--output PATH] [--dump-display-list PATH|-]\n"
        << "  " << argv0 << " --html HTML [--eval JS] [--format html|png|pdf] [--output PATH] [--dump-display-list PATH|-]\n"
        << "  echo HTML | " << argv0 << " --stdin [--eval JS] [--format html|png|pdf] [--output PATH] [--dump-display-list PATH|-]\n"
        << "options:\n"
        << "  --format FORMAT          output format: html, png, or pdf; default html\n"
        << "  --output PATH            output path; required for png/pdf, optional for html\n"
        << "  --viewport WIDTHxHEIGHT  render viewport, default 1280x720\n"
        << "  --full-page              expand the viewport height to the full page content height before rendering\n"
        << "  --scale NUMBER           render scale factor, default 1\n"
        << "  --wait-ms NUMBER         async timer/XHR/fetch wait budget, default 5000\n"
        << "  --js-timeout-ms NUMBER   per-script execution deadline, default 5000\n"
        << "  --js-memory-mb NUMBER    QuickJS heap limit, default 256\n";
}

// Largest render dimension we accept; well above any real viewport but
// small enough that width*height*4 cannot exhaust memory by accident.
constexpr int kMaxViewportDimension = 16384;
constexpr float kMaxScale = 8.0f;

int parse_int_in_range(const std::string& value, const std::string& name, int min_value, int max_value)
{
    int result = 0;
    std::size_t parsed = 0;
    try {
        result = std::stoi(value, &parsed);
    } catch (const std::out_of_range&) {
        throw std::runtime_error(name + " is out of range (max " + std::to_string(max_value) + ")");
    } catch (const std::invalid_argument&) {
        throw std::runtime_error(name + " must be an integer");
    }
    if (parsed != value.size() || result < min_value || result > max_value) {
        throw std::runtime_error(
            name + " must be an integer in [" + std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
    }
    return result;
}

int parse_positive_int(const std::string& value, const std::string& name)
{
    return parse_int_in_range(value, name, 1, std::numeric_limits<int>::max());
}

int parse_nonnegative_int(const std::string& value, const std::string& name)
{
    return parse_int_in_range(value, name, 0, std::numeric_limits<int>::max());
}

pagecore::Viewport parse_viewport(const std::string& value)
{
    const auto separator = value.find('x');
    if (separator == std::string::npos || separator == 0 || separator + 1 == value.size()) {
        throw std::runtime_error("viewport must be formatted as WIDTHxHEIGHT");
    }

    pagecore::Viewport viewport;
    viewport.width = parse_int_in_range(value.substr(0, separator), "viewport width", 1, kMaxViewportDimension);
    viewport.height = parse_int_in_range(value.substr(separator + 1), "viewport height", 1, kMaxViewportDimension);
    return viewport;
}

float parse_positive_float(const std::string& value, const std::string& name)
{
    float result = 0.0f;
    std::size_t parsed = 0;
    try {
        result = std::stof(value, &parsed);
    } catch (const std::out_of_range&) {
        throw std::runtime_error(name + " is out of range");
    } catch (const std::invalid_argument&) {
        throw std::runtime_error(name + " must be a number");
    }
    if (parsed != value.size() || !std::isfinite(result) || result <= 0.0f || result > kMaxScale) {
        throw std::runtime_error(name + " must be a number in (0, " + std::to_string(kMaxScale) + "]");
    }
    return result;
}

OutputFormat parse_output_format(const std::string& value)
{
    if (value == "html") {
        return OutputFormat::Html;
    }
    if (value == "png") {
        return OutputFormat::Png;
    }
    if (value == "pdf") {
        return OutputFormat::Pdf;
    }
    throw std::runtime_error("format must be one of: html, png, pdf");
}

const char* output_format_name(OutputFormat format)
{
    switch (format) {
    case OutputFormat::Html:
        return "html";
    case OutputFormat::Png:
        return "png";
    case OutputFormat::Pdf:
        return "pdf";
    }
    return "unknown";
}

void write_text_file(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open output file: " + path);
    }
    out << text;
    if (!out) {
        throw std::runtime_error("failed to write output file: " + path);
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        std::string url;
        std::string html;
        std::string file;
        std::string eval;
        std::string output;
        std::string display_list_dump;
        OutputFormat output_format = OutputFormat::Html;
        pagecore::RenderOptions render_options;
        pagecore::LoadOptions load_options;
        bool stdin_input = false;
        bool full_page = false;

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
            else if (arg == "--format") output_format = parse_output_format(next());
            else if (arg == "--output") output = next();
            else if (arg == "--screenshot") {
                output_format = OutputFormat::Png;
                output = next();
            }
            else if (arg == "--dump-display-list") display_list_dump = next();
            else if (arg == "--viewport") {
                const float scale = render_options.viewport.device_scale_factor;
                render_options.viewport = parse_viewport(next());
                render_options.viewport.device_scale_factor = scale;
            }
            else if (arg == "--scale") render_options.viewport.device_scale_factor = parse_positive_float(next(), "scale");
            else if (arg == "--wait-ms") load_options.wait_time = std::chrono::milliseconds(parse_nonnegative_int(next(), "wait-ms"));
            else if (arg == "--js-timeout-ms") load_options.js_timeout = std::chrono::milliseconds(parse_positive_int(next(), "js-timeout-ms"));
            else if (arg == "--js-memory-mb") {
                load_options.js_memory_limit_bytes = static_cast<std::size_t>(parse_positive_int(next(), "js-memory-mb")) * 1024 * 1024;
            }
            else if (arg == "--stdin") stdin_input = true;
            else if (arg == "--full-page") full_page = true;
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

        // Run --eval before rendering so eval-driven DOM mutations are reflected
        // in image/PDF output and display-list dumps.
        std::string eval_result;
        const bool has_eval = !eval.empty();
        if (has_eval) {
            eval_result = page.eval(eval);
        }

        if (full_page) {
            // The raster/PDF backends size their canvas strictly off
            // render_options.viewport (not content_width/content_height), so
            // expanding the viewport to the real content height up front
            // makes every later display_list()/render()/write_pdf() call
            // (and the --dump-display-list path below) capture the whole
            // page in a single, cached layout pass instead of cropping it
            // to the originally requested viewport height.
            const int content_height = page.display_list(render_options).content_height;
            if (content_height > kMaxViewportDimension) {
                throw std::runtime_error(
                    "--full-page content height (" + std::to_string(content_height)
                    + ") exceeds max viewport dimension (" + std::to_string(kMaxViewportDimension) + ")");
            }
            render_options.viewport.height = std::max(1, content_height);
        }

        if (output_format == OutputFormat::Html) {
            if (!output.empty()) {
                if (output == "-") {
                    throw std::runtime_error("omit --output to write html output to stdout");
                }
                write_text_file(output, page.serialize_html());
            }
        } else {
            if (output.empty()) {
                throw std::runtime_error(std::string("--output is required for ") + output_format_name(output_format) + " output");
            }
            if (output == "-") {
                throw std::runtime_error(std::string("--output - is not supported for ") + output_format_name(output_format) + " output");
            }

            if (output_format == OutputFormat::Png) {
                pagecore::write_png_rgba(page.render(render_options), output);
            } else if (output_format == OutputFormat::Pdf) {
                pagecore::write_pdf(page.display_list(render_options), output);
            }
        }

        if (!display_list_dump.empty()) {
            const std::string dump = pagecore::display_list_to_json(page.display_list(render_options));
            if (display_list_dump == "-") {
                std::cout << dump << "\n";
            } else {
                write_text_file(display_list_dump, dump);
            }
        }

        if (has_eval) {
            std::cout << eval_result << "\n";
        } else if (!output.empty()) {
            if (output != "-") {
                std::cout << output << "\n";
            }
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

#include "pagecore/page.hpp"
#include "pagecore/resource_loader.hpp"

#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kOrigin = "https://web-platform.test";

struct Options {
    std::vector<std::filesystem::path> roots;
    std::string path;
    std::string url;
    int wait_ms = 15000;
};

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string trim(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::string without_query_or_fragment(std::string_view value)
{
    const std::size_t end = value.find_first_of("?#");
    return std::string(value.substr(0, end == std::string_view::npos ? value.size() : end));
}

std::string ensure_leading_slash(std::string value)
{
    if (value.empty() || value.front() != '/') {
        value.insert(value.begin(), '/');
    }
    return value;
}

std::string strip_leading_slash(std::string value)
{
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    return value;
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string percent_decode_path(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char>(value[i]));
    }
    return out;
}

std::string resource_path_from_url(std::string_view url)
{
    std::string path;
    if (starts_with(url, kOrigin)) {
        path = std::string(url.substr(kOrigin.size()));
    } else if (starts_with(url, "/")) {
        path = std::string(url);
    } else {
        throw pagecore::ResourceError(pagecore::ResourceErrorCode::SchemeNotAllowed, std::string(url), "unsupported WPT URL");
    }

    path = without_query_or_fragment(path);
    path = strip_leading_slash(percent_decode_path(path));
    if (path.empty()) {
        throw pagecore::ResourceError(pagecore::ResourceErrorCode::NotFound, std::string(url), "empty WPT resource path");
    }
    if (path.find('\0') != std::string::npos) {
        throw pagecore::ResourceError(pagecore::ResourceErrorCode::InvalidRequest, std::string(url), "invalid WPT resource path");
    }
    return path;
}

// `canonical_root` must already be canonicalized (weakly_canonical); callers
// that check many candidates against the same root should canonicalize it
// once and reuse it rather than paying the filesystem-resolution cost here on
// every call.
bool is_under_root(const std::filesystem::path& canonical_root, const std::filesystem::path& candidate)
{
    std::error_code error_code;
    const std::filesystem::path canonical_candidate = std::filesystem::weakly_canonical(candidate, error_code);
    if (error_code) {
        return false;
    }
    auto root_it = canonical_root.begin();
    auto candidate_it = canonical_candidate.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == canonical_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::string mime_type_for_path(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".js" || ext == ".mjs") return "application/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".txt") return "text/plain";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

void append_headers_file(
    const std::filesystem::path& path,
    std::vector<std::pair<std::string, std::string>>& headers)
{
    if (!std::filesystem::is_regular_file(path)) {
        return;
    }

    std::istringstream input(read_file(path));
    std::string line;
    while (std::getline(input, line)) {
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#' || text.front() == '[') {
            continue;
        }
        const std::size_t colon = text.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        headers.emplace_back(trim(std::string_view(text).substr(0, colon)), trim(std::string_view(text).substr(colon + 1)));
    }
}

std::vector<std::pair<std::string, std::string>> headers_for_resource(
    const std::filesystem::path& root,
    const std::string& relative_path)
{
    std::vector<std::pair<std::string, std::string>> headers;
    std::filesystem::path current = root;
    std::filesystem::path parent = std::filesystem::path(relative_path).parent_path();
    append_headers_file(current / "__dir__.headers", headers);
    for (const auto& part : parent) {
        current /= part;
        append_headers_file(current / "__dir__.headers", headers);
    }
    append_headers_file(root / (relative_path + ".headers"), headers);
    return headers;
}

class WptResourceLoader final : public pagecore::ResourceLoader {
public:
    using ResourceLoader::load;

    explicit WptResourceLoader(std::vector<std::filesystem::path> roots)
        : roots_(std::move(roots))
    {
        canonical_roots_.reserve(roots_.size());
        for (const auto& root : roots_) {
            std::error_code error_code;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error_code);
            canonical_roots_.push_back(error_code ? root : canonical);
        }
    }

    pagecore::ResourceResponse load(const pagecore::ResourceRequest& request) override
    {
        // Every filesystem/parsing helper below throws pagecore::ResourceError for
        // expected conditions, but std::filesystem and std::ifstream can also throw
        // on OS-level errors (permission denied, symlink loops). Those must still
        // surface as a typed ResourceError so BatchErrorMode::Lenient callers (e.g.
        // Page::load_html's external-script loading) get a per-request placeholder
        // instead of an escaping exception that aborts the whole batch.
        try {
            return load_impl(request);
        } catch (const pagecore::ResourceError&) {
            throw;
        } catch (const std::exception& error) {
            throw pagecore::ResourceError(pagecore::ResourceErrorCode::NotFound, request.url, error.what());
        }
    }

private:
    pagecore::ResourceResponse load_impl(const pagecore::ResourceRequest& request) const
    {
        const std::string relative_path = resource_path_from_url(request.url);
        for (std::size_t i = 0; i < roots_.size(); ++i) {
            const std::filesystem::path& root = roots_[i];
            const std::filesystem::path candidate = (root / relative_path).lexically_normal();
            if (!std::filesystem::is_regular_file(candidate)) {
                continue;
            }
            if (!is_under_root(canonical_roots_[i], candidate)) {
                throw pagecore::ResourceError(
                    pagecore::ResourceErrorCode::SchemeNotAllowed,
                    request.url,
                    "WPT resource escaped corpus root");
            }

            // Resolve headers from the validated candidate's root-relative form, not
            // the raw request path: a raw path containing ".." segments that happen
            // to cancel out (still landing on `candidate` inside `root`) would
            // otherwise let the per-directory __dir__.headers walk below briefly
            // touch a directory outside `root`.
            const std::string safe_relative_path = candidate.lexically_relative(root).generic_string();

            std::string mime_type = mime_type_for_path(candidate);
            std::vector<std::pair<std::string, std::string>> headers = headers_for_resource(root, safe_relative_path);
            auto content_type = std::find_if(headers.begin(), headers.end(), [](const auto& header) {
                return pagecore::header_name_equals(header.first, "Content-Type");
            });
            if (content_type != headers.end()) {
                mime_type = content_type->second;
            } else {
                headers.emplace_back("Content-Type", mime_type);
            }
            return pagecore::ResourceResponse{
                request.url,
                read_file(candidate),
                200,
                std::move(mime_type),
                request.kind,
                false,
                "OK",
                std::move(headers),
            };
        }

        throw pagecore::ResourceError(
            pagecore::ResourceErrorCode::NotFound,
            request.url,
            "missing WPT resource: " + relative_path);
    }

    std::vector<std::filesystem::path> roots_;
    std::vector<std::filesystem::path> canonical_roots_;
};

std::string escape_html(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string escape_script_body(std::string_view value)
{
    std::string out(value);
    std::string lower(out.size(), '\0');
    std::transform(out.begin(), out.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    std::size_t pos = 0;
    while ((pos = lower.find("</script", pos)) != std::string::npos) {
        out.insert(pos + 1, "\\");
        lower.insert(pos + 1, "\\");
        pos += 9;
    }
    return out;
}

std::vector<std::string> meta_scripts(std::string_view source)
{
    std::vector<std::string> scripts;
    std::istringstream input{std::string(source)};
    std::string line;
    while (std::getline(input, line)) {
        const std::string text = trim(line);
        constexpr std::string_view prefix = "// META:";
        if (!starts_with(text, prefix)) {
            continue;
        }
        const std::string meta = trim(std::string_view(text).substr(prefix.size()));
        constexpr std::string_view script_prefix = "script=";
        if (starts_with(meta, script_prefix)) {
            scripts.push_back(trim(std::string_view(meta).substr(script_prefix.size())));
        }
    }
    return scripts;
}

std::string generated_url_path_for_source(std::string path)
{
    path = ensure_leading_slash(path);
    const std::string base = without_query_or_fragment(path);
    const std::string suffix = path.substr(base.size());
    if (ends_with(base, ".window.js")) {
        return base.substr(0, base.size() - 3) + ".html" + suffix;
    }
    if (ends_with(base, ".any.js")) {
        return base.substr(0, base.size() - 3) + ".html" + suffix;
    }
    return path;
}

std::string source_path_for_test_path(std::string path)
{
    path = strip_leading_slash(without_query_or_fragment(path));
    if (ends_with(path, ".window.html") || ends_with(path, ".any.html")) {
        return path.substr(0, path.size() - 5) + ".js";
    }
    return path;
}

bool is_generated_js_test(std::string_view source_path)
{
    return ends_with(source_path, ".window.js") || ends_with(source_path, ".any.js");
}

std::optional<std::filesystem::path> find_source_file(
    const std::vector<std::filesystem::path>& roots,
    const std::string& source_path)
{
    for (const auto& root : roots) {
        const std::filesystem::path candidate = (root / source_path).lexically_normal();
        if (!std::filesystem::is_regular_file(candidate)) {
            continue;
        }
        std::error_code error_code;
        const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error_code);
        if (!error_code && is_under_root(canonical_root, candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string wrap_js_test(std::string_view source, std::string_view source_path, std::string_view page_url)
{
    std::ostringstream out;
    out << "<!doctype html>\n"
        << "<meta charset=\"utf-8\">\n"
        << "<title>" << escape_html(source_path) << "</title>\n"
        << "<script src=\"/resources/testharness.js\"></script>\n"
        << "<script src=\"/resources/testharnessreport.js\"></script>\n";

    const std::string base_url = std::string(kOrigin) + std::string(page_url);
    for (const auto& script : meta_scripts(source)) {
        out << "<script src=\"" << escape_html(pagecore::resolve_url(base_url, script)) << "\"></script>\n";
    }

    out << "<script>\n" << escape_script_body(source) << "\n</script>\n";
    return out.str();
}

void usage(const char* argv0)
{
    std::cerr
        << "usage: " << argv0 << " --root PATH [--root PATH...] --path WPT_PATH [--url URL_PATH] [--wait-ms N]\n";
}

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };

        if (arg == "--root") {
            options.roots.emplace_back(next());
        } else if (arg == "--path") {
            options.path = next();
        } else if (arg == "--url") {
            options.url = next();
        } else if (arg == "--wait-ms") {
            options.wait_ms = std::stoi(next());
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (options.roots.empty()) {
        throw std::runtime_error("--root is required");
    }
    if (options.path.empty()) {
        throw std::runtime_error("--path is required");
    }
    if (options.wait_ms <= 0) {
        throw std::runtime_error("--wait-ms must be positive");
    }
    if (options.url.empty()) {
        options.url = generated_url_path_for_source(options.path);
    }
    options.url = ensure_leading_slash(options.url);
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parse_args(argc, argv);
        const std::string source_path = source_path_for_test_path(options.path);
        const auto source_file = find_source_file(options.roots, source_path);
        if (!source_file) {
            throw std::runtime_error("WPT source file not found in any root: " + source_path);
        }

        const std::string source = read_file(*source_file);
        const std::string html = is_generated_js_test(source_path)
            ? wrap_js_test(source, source_path, options.url)
            : source;
        const std::string page_url = std::string(kOrigin) + options.url;

        pagecore::LoadOptions load_options;
        load_options.base_url = page_url;
        load_options.wait_time = std::chrono::milliseconds(options.wait_ms);

        // --wait-ms is how long the caller is willing to wait for this test, so the
        // script budgets have to track it. Otherwise a big generated test (the
        // html/dom/reflection-* files run thousands of subtests from a single inline
        // script, taking tens of seconds) is killed by the default 30s per-script
        // deadline no matter how long --wait-ms says to wait, and reports as
        // "WPT did not complete" rather than as a result. Never lower the defaults:
        // a short --wait-ms should not make scripts *more* likely to be interrupted.
        const auto wait = std::chrono::milliseconds(options.wait_ms);
        load_options.js_timeout = std::max(load_options.js_timeout, wait);
        if (load_options.max_load_time) {
            load_options.max_load_time = std::max(*load_options.max_load_time, wait);
        }
        load_options.console_log = [](std::string_view severity, std::string_view message) {
            std::cerr << "console." << severity << ": " << message << "\n";
        };

        pagecore::Page page(load_options);
        page.set_resource_loader(std::make_shared<WptResourceLoader>(options.roots));
        // load_html() already waits for WaitUntil::Ready internally using
        // load_options.wait_time (LoadOptions::wait_until defaults to Ready), so an
        // additional explicit run_until_ready() call here would just re-wait up to
        // the same --wait-ms a second time for any test that isn't ready yet.
        page.load_html(html, page_url);

        const std::string report = page.eval("globalThis.__pagecore_wpt_json || ''");
        if (report.empty()) {
            throw std::runtime_error("WPT did not complete or did not install testharness completion callback");
        }
        std::cout << report << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pagecore_wpt_case: " << error.what() << "\n";
        return 1;
    }
}

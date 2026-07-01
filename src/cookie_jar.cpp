#include "cookie_jar.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace pagecore {
namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path = "/";
    bool secure = false;
};

std::string lower_ascii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::string trim_ascii(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

bool iequals(std::string_view left, std::string_view right)
{
    return lower_ascii(left) == lower_ascii(right);
}

ParsedUrl parse_url(std::string_view url)
{
    ParsedUrl parsed;
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos) {
        return parsed;
    }

    parsed.scheme = lower_ascii(url.substr(0, colon));
    parsed.secure = parsed.scheme == "https";
    std::size_t cursor = colon + 1;
    if (url.substr(cursor, 2) == "//") {
        cursor += 2;
        const std::size_t authority_end = url.find_first_of("/?#", cursor);
        std::string_view authority = url.substr(
            cursor,
            authority_end == std::string_view::npos ? std::string_view::npos : authority_end - cursor);
        if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
            authority = authority.substr(at + 1);
        }
        if (!authority.empty() && authority.front() == '[') {
            const std::size_t close = authority.find(']');
            parsed.host = lower_ascii(close == std::string_view::npos ? authority : authority.substr(0, close + 1));
            if (close != std::string_view::npos && close + 1 < authority.size() && authority[close + 1] == ':') {
                parsed.port = std::string(authority.substr(close + 2));
            }
        } else {
            const std::size_t port = authority.rfind(':');
            parsed.host = lower_ascii(port == std::string_view::npos ? authority : authority.substr(0, port));
            if (port != std::string_view::npos) {
                parsed.port = std::string(authority.substr(port + 1));
            }
        }
        cursor = authority_end == std::string_view::npos ? url.size() : authority_end;
    }

    if (cursor < url.size() && url[cursor] == '/') {
        const std::size_t path_end = url.find_first_of("?#", cursor);
        parsed.path = std::string(url.substr(
            cursor,
            path_end == std::string_view::npos ? std::string_view::npos : path_end - cursor));
        if (parsed.path.empty()) {
            parsed.path = "/";
        }
    }
    return parsed;
}

std::string default_port_for(std::string_view scheme)
{
    if (scheme == "http") {
        return "80";
    }
    if (scheme == "https") {
        return "443";
    }
    return {};
}

std::string origin_of(std::string_view url)
{
    ParsedUrl parsed = parse_url(url);
    if (parsed.scheme.empty() || parsed.host.empty()) {
        return {};
    }
    const std::string port = parsed.port.empty() ? default_port_for(parsed.scheme) : parsed.port;
    return port.empty()
        ? parsed.scheme + "://" + parsed.host
        : parsed.scheme + "://" + parsed.host + ":" + port;
}

bool domain_match(std::string_view host, std::string_view domain)
{
    if (host.empty() || domain.empty()) {
        return false;
    }
    const std::string host_l = lower_ascii(host);
    const std::string domain_l = lower_ascii(domain);
    if (host_l == domain_l) {
        return true;
    }
    return host_l.size() > domain_l.size()
        && host_l.compare(host_l.size() - domain_l.size(), domain_l.size(), domain_l) == 0
        && host_l[host_l.size() - domain_l.size() - 1] == '.';
}

bool path_match(std::string_view request_path, std::string_view cookie_path)
{
    if (cookie_path.empty() || cookie_path == "/") {
        return true;
    }
    if (request_path == cookie_path) {
        return true;
    }
    return request_path.size() > cookie_path.size()
        && request_path.substr(0, cookie_path.size()) == cookie_path
        && (cookie_path.back() == '/' || request_path[cookie_path.size()] == '/');
}

std::string default_path_for(std::string_view request_path)
{
    if (request_path.empty() || request_path.front() != '/') {
        return "/";
    }
    const std::size_t slash = request_path.rfind('/');
    if (slash == std::string_view::npos || slash == 0) {
        return "/";
    }
    return std::string(request_path.substr(0, slash));
}

bool is_ipv4_address(std::string_view host)
{
    if (host.empty()) {
        return false;
    }
    int parts = 0;
    std::size_t start = 0;
    while (start <= host.size()) {
        const std::size_t end = host.find('.', start);
        const std::string_view part = host.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty() || part.size() > 3) {
            return false;
        }
        int value = 0;
        for (char ch : part) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                return false;
            }
            value = value * 10 + (ch - '0');
        }
        if (value > 255) {
            return false;
        }
        ++parts;
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parts == 4;
}

std::string registrable_domain_heuristic(std::string_view host)
{
    const std::string lowered = lower_ascii(host);
    if (lowered.empty()
        || lowered == "localhost"
        || lowered.front() == '['
        || is_ipv4_address(lowered)) {
        return lowered;
    }

    const std::size_t last_dot = lowered.rfind('.');
    if (last_dot == std::string::npos) {
        return lowered;
    }
    const std::size_t previous_dot = lowered.rfind('.', last_dot - 1);
    return previous_dot == std::string::npos ? lowered : lowered.substr(previous_dot + 1);
}

std::string site_of(std::string_view url)
{
    const ParsedUrl parsed = parse_url(url);
    if (parsed.scheme.empty() || parsed.host.empty()) {
        return {};
    }
    return parsed.scheme + "://" + registrable_domain_heuristic(parsed.host);
}

bool same_site_url(std::string_view left, std::string_view right)
{
    const std::string left_site = site_of(left);
    const std::string right_site = site_of(right);
    return !left_site.empty() && left_site == right_site;
}

bool is_safe_method(std::string_view method)
{
    const std::string lowered = lower_ascii(method.empty() ? std::string_view("GET") : method);
    return lowered == "get" || lowered == "head" || lowered == "options" || lowered == "trace";
}

std::optional<CookieSameSite> parse_same_site(std::string_view value)
{
    const std::string lowered = lower_ascii(trim_ascii(value));
    if (lowered == "strict") {
        return CookieSameSite::Strict;
    }
    if (lowered == "lax") {
        return CookieSameSite::Lax;
    }
    if (lowered == "none") {
        return CookieSameSite::None;
    }
    return std::nullopt;
}

std::time_t timegm_compat(std::tm* tm)
{
#if defined(_WIN32)
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

std::optional<std::chrono::system_clock::time_point> parse_cookie_time(std::string_view value)
{
    std::tm tm{};
    std::istringstream rfc1123{std::string(value)};
    rfc1123 >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (!rfc1123.fail()) {
        return std::chrono::system_clock::from_time_t(timegm_compat(&tm));
    }

    tm = {};
    std::istringstream rfc850{std::string(value)};
    rfc850 >> std::get_time(&tm, "%A, %d-%b-%y %H:%M:%S GMT");
    if (!rfc850.fail()) {
        return std::chrono::system_clock::from_time_t(timegm_compat(&tm));
    }

    tm = {};
    std::istringstream asctime{std::string(value)};
    asctime >> std::get_time(&tm, "%a %b %d %H:%M:%S %Y");
    if (!asctime.fail()) {
        return std::chrono::system_clock::from_time_t(timegm_compat(&tm));
    }

    return std::nullopt;
}

bool is_expired(const CookieJar::Cookie& cookie, std::chrono::system_clock::time_point now)
{
    return cookie.expires && *cookie.expires <= now;
}

bool cookie_matches(const CookieJar::Cookie& cookie, const ParsedUrl& url)
{
    if (cookie.secure && !url.secure) {
        return false;
    }
    if (cookie.host_only) {
        if (url.host != cookie.domain) {
            return false;
        }
    } else if (!domain_match(url.host, cookie.domain)) {
        return false;
    }
    return path_match(url.path, cookie.path);
}

bool same_site_allows_cookie(
    const CookieJar::Cookie& cookie,
    std::string_view request_url,
    std::string_view site_url,
    bool top_level_navigation,
    std::string_view method)
{
    if (site_url.empty() || same_site_url(request_url, site_url)) {
        return true;
    }
    if (cookie.same_site == CookieSameSite::None) {
        return true;
    }
    if (cookie.same_site == CookieSameSite::Strict) {
        return false;
    }
    return top_level_navigation && is_safe_method(method);
}

bool has_cookie_header(const ResourceRequest& request)
{
    return std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
        return iequals(header.first, "cookie");
    });
}

std::vector<std::string_view> split_semicolon(std::string_view value)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(';', start);
        parts.push_back(value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

} // namespace

bool cookie_credentials_allow(
    CookieCredentials credentials,
    std::string_view request_url,
    std::string_view document_url)
{
    if (credentials == CookieCredentials::Omit) {
        return false;
    }
    if (credentials == CookieCredentials::Include) {
        return true;
    }
    const std::string request_origin = origin_of(request_url);
    const std::string document_origin = origin_of(document_url);
    return !request_origin.empty() && request_origin == document_origin;
}

std::string CookieJar::document_cookie(std::string_view document_url) const
{
    return cookie_header_for_url(document_url, false, false, document_url, false, "GET");
}

void CookieJar::set_document_cookie(std::string_view document_url, std::string_view cookie)
{
    set_cookie_from_header(document_url, cookie, false);
}

ResourceRequest CookieJar::with_cookie_header(
    ResourceRequest request,
    CookieCredentials credentials,
    std::string_view document_url) const
{
    if (!cookie_credentials_allow(credentials, request.url, document_url) || has_cookie_header(request)) {
        return request;
    }
    const bool top_level_navigation = request.kind == ResourceKind::Document;
    const std::string header = cookie_header_for_url(
        request.url,
        true,
        true,
        document_url,
        top_level_navigation,
        request.method.empty() ? std::string_view("GET") : std::string_view(request.method));
    if (!header.empty()) {
        request.headers.emplace_back("Cookie", header);
    }
    return request;
}

void CookieJar::store_from_response(
    std::string_view request_url,
    const ResourceResponse& response,
    CookieCredentials credentials,
    std::string_view document_url)
{
    if (response.from_cache || !cookie_credentials_allow(credentials, request_url, document_url)) {
        return;
    }

    if (!response.set_cookie_headers.empty()) {
        for (const auto& [source_url, value] : response.set_cookie_headers) {
            set_cookie_from_header(source_url.empty() ? request_url : source_url, value, true);
        }
        return;
    }

    for (const auto& [name, value] : response.headers) {
        if (iequals(name, "set-cookie")) {
            set_cookie_from_header(response.url.empty() ? request_url : std::string_view(response.url), value, true);
        }
    }
}

void CookieJar::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cookies_.clear();
}

std::string CookieJar::cookie_header_for_url(
    std::string_view url,
    bool include_http_only,
    bool apply_same_site,
    std::string_view site_url,
    bool top_level_navigation,
    std::string_view method) const
{
    const ParsedUrl parsed = parse_url(url);
    if (parsed.host.empty()) {
        return {};
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const Cookie& cookie : cookies_) {
        if (is_expired(cookie, now) || (!include_http_only && cookie.http_only) || !cookie_matches(cookie, parsed)) {
            continue;
        }
        if (apply_same_site && !same_site_allows_cookie(cookie, url, site_url, top_level_navigation, method)) {
            continue;
        }
        if (!out.empty()) {
            out += "; ";
        }
        out += cookie.name;
        out += '=';
        out += cookie.value;
    }
    return out;
}

void CookieJar::set_cookie_from_header(std::string_view request_url, std::string_view header, bool from_http)
{
    const ParsedUrl parsed = parse_url(request_url);
    if (parsed.host.empty()) {
        return;
    }

    std::vector<std::string_view> parts = split_semicolon(header);
    if (parts.empty()) {
        return;
    }

    const std::string pair = trim_ascii(parts.front());
    const std::size_t equals = pair.find('=');
    if (equals == std::string::npos || equals == 0) {
        return;
    }

    Cookie cookie;
    cookie.name = pair.substr(0, equals);
    cookie.value = pair.substr(equals + 1);
    cookie.domain = parsed.host;
    cookie.path = default_path_for(parsed.path);
    cookie.host_only = true;
    bool reject_cookie = false;

    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string attr = trim_ascii(parts[i]);
        if (attr.empty()) {
            continue;
        }
        const std::size_t attr_equals = attr.find('=');
        const std::string name = lower_ascii(attr_equals == std::string::npos ? attr : attr.substr(0, attr_equals));
        const std::string value = attr_equals == std::string::npos ? std::string{} : trim_ascii(attr.substr(attr_equals + 1));

        if (name == "domain") {
            std::string domain = lower_ascii(value);
            while (!domain.empty() && domain.front() == '.') {
                domain.erase(domain.begin());
            }
            if (!domain.empty() && domain_match(parsed.host, domain)) {
                cookie.domain = std::move(domain);
                cookie.host_only = false;
            } else {
                reject_cookie = true;
            }
        } else if (name == "path") {
            if (!value.empty() && value.front() == '/') {
                cookie.path = value;
            }
        } else if (name == "secure") {
            cookie.secure = true;
        } else if (name == "httponly") {
            if (from_http) {
                cookie.http_only = true;
            }
        } else if (name == "expires") {
            cookie.expires = parse_cookie_time(value);
        } else if (name == "max-age") {
            try {
                const long long seconds = std::stoll(value);
                cookie.expires = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
            } catch (...) {
            }
        } else if (name == "samesite") {
            if (auto same_site = parse_same_site(value)) {
                cookie.same_site = *same_site;
            }
        }
    }

    if (reject_cookie || (cookie.secure && !parsed.secure) || (cookie.same_site == CookieSameSite::None && !cookie.secure)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto same_identity = [&](const Cookie& existing) {
        return existing.name == cookie.name
            && existing.domain == cookie.domain
            && existing.path == cookie.path
            && existing.host_only == cookie.host_only;
    };
    const auto can_modify = [&](const Cookie& existing) {
        return from_http || !existing.http_only;
    };

    if (!from_http && std::any_of(cookies_.begin(), cookies_.end(), [&](const Cookie& existing) {
            return existing.http_only && same_identity(existing);
        })) {
        return;
    }

    if (is_expired(cookie, now)) {
        const auto before = cookies_.size();
        cookies_.erase(
            std::remove_if(cookies_.begin(), cookies_.end(), [&](const Cookie& existing) {
                return can_modify(existing) && same_identity(existing);
            }),
            cookies_.end());
        if (cookies_.size() == before) {
            cookies_.erase(
                std::remove_if(cookies_.begin(), cookies_.end(), [&](const Cookie& existing) {
                    return can_modify(existing) && existing.name == cookie.name && cookie_matches(existing, parsed);
                }),
                cookies_.end());
        }
        return;
    }

    cookies_.erase(
        std::remove_if(cookies_.begin(), cookies_.end(), [&](const Cookie& existing) {
            return can_modify(existing) && same_identity(existing);
        }),
        cookies_.end());
    cookies_.push_back(std::move(cookie));
}

} // namespace pagecore

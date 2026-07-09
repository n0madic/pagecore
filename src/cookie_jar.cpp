#include "cookie_jar.hpp"

#include "util.hpp"

#include <libpsl.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
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
    return ascii_lower(left) == ascii_lower(right);
}

ParsedUrl parse_url(std::string_view url)
{
    ParsedUrl parsed;
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos) {
        return parsed;
    }

    parsed.scheme = ascii_lower(url.substr(0, colon));
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
            parsed.host = ascii_lower(close == std::string_view::npos ? authority : authority.substr(0, close + 1));
            if (close != std::string_view::npos && close + 1 < authority.size() && authority[close + 1] == ':') {
                parsed.port = std::string(authority.substr(close + 2));
            }
        } else {
            const std::size_t port = authority.rfind(':');
            parsed.host = ascii_lower(port == std::string_view::npos ? authority : authority.substr(0, port));
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
    const std::string host_l = ascii_lower(host);
    const std::string domain_l = ascii_lower(domain);
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

const psl_ctx_t* psl_context()
{
    static const psl_ctx_t* const context = psl_builtin();
    return context; // null only if libpsl was built without embedded PSL data.
}

// Whether `host` (already lowercased) is a public suffix under which
// independent registrants live, per the Mozilla Public Suffix List (via
// libpsl). Used to reject cookies scoped to a suffix (supercookie defense)
// and to compute the registrable domain for SameSite. Falls back to the
// RFC 6265 minimum (bare single-label host only) if libpsl has no built-in
// PSL data, matching the previous fail-safe behavior.
bool is_public_suffix(std::string_view host)
{
    if (host.empty()) {
        return true;
    }
    const psl_ctx_t* psl = psl_context();
    if (!psl) {
        return host.find('.') == std::string_view::npos;
    }
    return psl_is_public_suffix(psl, std::string(host).c_str()) != 0;
}

std::string registrable_domain(std::string_view host)
{
    const std::string lowered = ascii_lower(host);
    if (lowered.empty()
        || lowered == "localhost"
        || lowered.front() == '['
        || is_ipv4_address(lowered)) {
        return lowered;
    }
    const psl_ctx_t* psl = psl_context();
    if (!psl) {
        return lowered; // No PSL data: fail safe to exact-host matching.
    }
    const char* registrable = psl_registrable_domain(psl, lowered.c_str());
    return registrable ? registrable : lowered; // Null: host itself is a public suffix.
}

std::string site_of(std::string_view url)
{
    const ParsedUrl parsed = parse_url(url);
    if (parsed.scheme.empty() || parsed.host.empty()) {
        return {};
    }
    return parsed.scheme + "://" + registrable_domain(parsed.host);
}

bool same_site_url(std::string_view left, std::string_view right)
{
    const std::string left_site = site_of(left);
    const std::string right_site = site_of(right);
    return !left_site.empty() && left_site == right_site;
}

bool is_safe_method(std::string_view method)
{
    const std::string lowered = ascii_lower(method.empty() ? std::string_view("GET") : method);
    return lowered == "get" || lowered == "head" || lowered == "options" || lowered == "trace";
}

std::optional<CookieSameSite> parse_same_site(std::string_view value)
{
    const std::string lowered = ascii_lower(trim_ascii(value));
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
    // An empty site_url means there is no known first-party context; treat it as
    // cross-site (fail closed) rather than allow-all. same_site_url() already
    // returns false for an empty/opaque site, so it flows into the checks below —
    // only SameSite=None (and Lax on a safe top-level navigation) is sent.
    if (same_site_url(request_url, site_url)) {
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
    // A Document-kind request is a top-level navigation in this single-document
    // engine, which has no nested/iframe browsing context. If iframe document
    // loads are ever added, this must distinguish nested documents (which are NOT
    // top-level navigations) so cross-site Lax cookies are not over-sent to them.
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

namespace {

constexpr std::size_t kMaxCookieNameValueBytes = 4096;
constexpr std::size_t kMaxCookiesPerDomain = 50;
constexpr std::size_t kMaxCookiesTotal = 3000;

bool contains_cookie_ctl(std::string_view value)
{
    return std::any_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte < 0x20 || byte == 0x7f;
    });
}

} // namespace

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

    // Reject control characters (and ';') in the name/value: they would otherwise
    // be re-serialized verbatim into an outgoing "Cookie:" request header, enabling
    // header injection — most importantly via document.cookie (from_http == false).
    if (contains_cookie_ctl(cookie.name) || cookie.name.find(';') != std::string::npos
        || contains_cookie_ctl(cookie.value) || cookie.value.find(';') != std::string::npos) {
        return;
    }
    if (cookie.name.size() + cookie.value.size() > kMaxCookieNameValueBytes) {
        return;
    }

    std::optional<std::chrono::system_clock::time_point> expires_attr;
    std::optional<long long> max_age_attr;

    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string attr = trim_ascii(parts[i]);
        if (attr.empty()) {
            continue;
        }
        const std::size_t attr_equals = attr.find('=');
        const std::string name = ascii_lower(attr_equals == std::string::npos ? attr : attr.substr(0, attr_equals));
        const std::string value = attr_equals == std::string::npos ? std::string{} : trim_ascii(attr.substr(attr_equals + 1));

        if (name == "domain") {
            std::string domain = ascii_lower(value);
            while (!domain.empty() && domain.front() == '.') {
                domain.erase(domain.begin());
            }
            const bool host_is_ip = is_ipv4_address(parsed.host)
                || (!parsed.host.empty() && parsed.host.front() == '[');
            if (domain.empty()) {
                reject_cookie = true;
            } else if (host_is_ip) {
                // RFC 6265 §5.3: when the request host is an IP literal, a Domain
                // attribute is only acceptable if it equals the host (and the
                // cookie stays host-only). Otherwise Domain=2.3.4 on host 1.2.3.4
                // would scope the cookie across unrelated IP hosts.
                if (domain != parsed.host) {
                    reject_cookie = true;
                }
            } else if (is_public_suffix(domain)) {
                // A cookie may only be scoped to a public suffix when that suffix is
                // exactly the request host (staying host-only); otherwise it would be
                // a supercookie shared across unrelated registrants (e.g. Domain=com).
                if (domain != parsed.host) {
                    reject_cookie = true;
                }
            } else if (domain_match(parsed.host, domain)) {
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
            expires_attr = parse_cookie_time(value);
        } else if (name == "max-age") {
            try {
                long long parsed_age = std::stoll(value);
                // Clamp to RFC 6265bis's 400-day maximum (matching browsers) so the
                // subsequent now() + seconds(age) cannot overflow the clock duration
                // and wrap a long-lived cookie into the past.
                constexpr long long kMaxCookieAgeSeconds = 400LL * 24 * 60 * 60;
                max_age_attr = std::clamp(parsed_age, -kMaxCookieAgeSeconds, kMaxCookieAgeSeconds);
            } catch (...) {
            }
        } else if (name == "samesite") {
            if (auto same_site = parse_same_site(value)) {
                cookie.same_site = *same_site;
            }
        }
    }

    // RFC 6265 §5.3: Max-Age takes precedence over Expires regardless of order.
    if (max_age_attr) {
        cookie.expires = std::chrono::system_clock::now() + std::chrono::seconds(*max_age_attr);
    } else if (expires_attr) {
        cookie.expires = expires_attr;
    }

    // Enforce the __Secure-/__Host- name prefixes servers rely on for integrity.
    // Prefix matching is ASCII case-insensitive per RFC 6265bis.
    const std::string lower_name = ascii_lower(cookie.name);
    if (lower_name.rfind("__secure-", 0) == 0 && (!cookie.secure || !parsed.secure)) {
        return;
    }
    if (lower_name.rfind("__host-", 0) == 0
        && (!cookie.secure || !parsed.secure || !cookie.host_only || cookie.path != "/")) {
        return;
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

    // RFC 6265bis §5.4: a cookie arriving over a non-secure transport must not
    // overwrite or delete an existing Secure cookie of the same name that it
    // domain-matches — otherwise an http:// attacker could evict an https:// session.
    if (!parsed.secure && std::any_of(cookies_.begin(), cookies_.end(), [&](const Cookie& existing) {
            return existing.secure
                && existing.name == cookie.name
                && domain_match(parsed.host, existing.domain);
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

    const std::string cookie_domain = cookie.domain;
    cookies_.push_back(std::move(cookie));

    // Bound growth: cookies_ is ordered oldest-first, so evict from the front to
    // stay within per-domain and global caps (memory-exhaustion defense).
    while (std::count_if(cookies_.begin(), cookies_.end(),
               [&](const Cookie& existing) { return existing.domain == cookie_domain; })
        > static_cast<std::ptrdiff_t>(kMaxCookiesPerDomain)) {
        const auto oldest = std::find_if(cookies_.begin(), cookies_.end(),
            [&](const Cookie& existing) { return existing.domain == cookie_domain; });
        if (oldest == cookies_.end()) {
            break;
        }
        cookies_.erase(oldest);
    }
    // Global cap: evict the oldest cookie of the largest-contributing domain
    // rather than the globally-oldest cookie. This keeps a single noisy origin
    // from evicting another origin's (possibly older) session cookie once the jar
    // is full — the origin that filled the jar sheds its own oldest entries first.
    while (cookies_.size() > kMaxCookiesTotal) {
        std::unordered_map<std::string, std::size_t> per_domain;
        for (const Cookie& existing : cookies_) {
            ++per_domain[existing.domain];
        }
        const std::string* heaviest = nullptr;
        std::size_t heaviest_count = 0;
        for (const auto& [domain, count] : per_domain) {
            if (count > heaviest_count) {
                heaviest_count = count;
                heaviest = &domain;
            }
        }
        const auto victim = heaviest == nullptr
            ? cookies_.begin()
            : std::find_if(cookies_.begin(), cookies_.end(),
                  [&](const Cookie& existing) { return existing.domain == *heaviest; });
        cookies_.erase(victim == cookies_.end() ? cookies_.begin() : victim);
    }
}

} // namespace pagecore

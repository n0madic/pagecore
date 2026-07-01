#include "pagecore/resource_loader.hpp"

#include "base64_codec.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace pagecore {

void ensure_curl_global_init();

namespace {

struct CurlBody {
    std::string body;
    std::size_t max_bytes = 0;
    bool too_large = false;
};

struct CurlHeaders {
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> set_cookie_sources;
    std::vector<std::pair<std::string, std::string>> set_cookie_headers;
    std::string current_url;
    std::string next_url;
    std::string status_text;
};

struct CurlHeaderListDeleter {
    void operator()(curl_slist* list) const
    {
        if (list != nullptr) {
            curl_slist_free_all(list);
        }
    }
};

using CurlHeaderList = std::unique_ptr<curl_slist, CurlHeaderListDeleter>;

// libcurl share handle for connection / DNS-cache / TLS-session reuse across
// requests, with one lock mutex per shared data type so the share is safe to use
// from multiple threads. Owned (type-erased) by each CurlResourceLoader.
struct CurlShared;
void share_lock_callback(CURL* handle, curl_lock_data data, curl_lock_access access, void* userptr);
void share_unlock_callback(CURL* handle, curl_lock_data data, void* userptr);

struct CurlShared {
    CURLSH* share = nullptr;
    std::array<std::mutex, CURL_LOCK_DATA_LAST> mutexes;

    CurlShared()
    {
        ensure_curl_global_init();
        share = curl_share_init();
        if (share != nullptr) {
            curl_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock_callback);
            curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock_callback);
            curl_share_setopt(share, CURLSHOPT_USERDATA, this);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        }
    }

    ~CurlShared()
    {
        if (share != nullptr) {
            curl_share_cleanup(share);
        }
    }

    CurlShared(const CurlShared&) = delete;
    CurlShared& operator=(const CurlShared&) = delete;
};

void share_lock_callback(CURL*, curl_lock_data data, curl_lock_access, void* userptr)
{
    static_cast<CurlShared*>(userptr)->mutexes[static_cast<std::size_t>(data)].lock();
}

void share_unlock_callback(CURL*, curl_lock_data data, void* userptr)
{
    static_cast<CurlShared*>(userptr)->mutexes[static_cast<std::size_t>(data)].unlock();
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* data = static_cast<CurlBody*>(userdata);
    const size_t len = size * nmemb;
    if (data->max_bytes > 0 && data->body.size() + len > data->max_bytes) {
        data->too_large = true;
        return 0;
    }
    data->body.append(ptr, len);
    return len;
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

std::string reason_phrase_from_status_line(std::string_view line)
{
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return {};
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return {};
    }
    return trim_ascii(line.substr(second_space + 1));
}

size_t write_header(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* data = static_cast<CurlHeaders*>(userdata);
    const size_t len = size * nmemb;
    std::string_view line(ptr, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }

    if (line.empty()) {
        return len;
    }

    if (line.substr(0, 5) == "HTTP/") {
        if (!data->next_url.empty()) {
            data->current_url = std::move(data->next_url);
            data->next_url.clear();
        }
        data->headers.clear();
        data->status_text = reason_phrase_from_status_line(line);
        return len;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return len;
    }

    std::string name = trim_ascii(line.substr(0, colon));
    if (!name.empty()) {
        std::string value = trim_ascii(line.substr(colon + 1));
        if (name.size() == 10
            && std::equal(name.begin(), name.end(), "Set-Cookie", [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
               })) {
            data->set_cookie_sources.emplace_back(data->current_url, value);
            data->set_cookie_headers.emplace_back(std::move(name), std::move(value));
        } else if (name.size() == 8
            && std::equal(name.begin(), name.end(), "Location", [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
               })) {
            data->next_url = resolve_url(data->current_url, value);
            data->headers.emplace_back(std::move(name), std::move(value));
        } else {
            data->headers.emplace_back(std::move(name), std::move(value));
        }
    }
    return len;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

bool header_name_equals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

bool has_request_header(const ResourceRequest& request, std::string_view name)
{
    return std::any_of(request.headers.begin(), request.headers.end(), [&](const auto& header) {
        return header_name_equals(header.first, name);
    });
}

std::string default_status_text(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return {};
    }
}

std::vector<std::pair<std::string, std::string>> content_type_header(std::string_view mime_type)
{
    if (mime_type.empty()) {
        return {};
    }
    return {{"Content-Type", std::string(mime_type)}};
}

bool has_url_scheme(std::string_view value)
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (i == 0) {
            if (!std::isalpha(ch)) {
                return false;
            }
        } else if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.') {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string scheme_of(std::string_view url)
{
    const auto colon = url.find(':');
    if (colon == std::string_view::npos) {
        return "file";
    }

    const auto slash = url.find('/');
    if (slash != std::string_view::npos && slash < colon) {
        return "file";
    }

    return to_lower(url.substr(0, colon));
}

bool is_network_scheme(std::string_view scheme)
{
    return scheme == "http" || scheme == "https";
}

bool is_file_scheme(std::string_view scheme)
{
    return scheme == "file";
}

bool is_data_scheme(std::string_view scheme)
{
    return scheme == "data";
}

std::string sanitize_http_referrer(std::string_view referrer)
{
    for (unsigned char ch : referrer) {
        if (ch <= 0x20 || ch == 0x7f) {
            return {};
        }
    }

    const std::string scheme = scheme_of(referrer);
    if (!is_network_scheme(scheme)) {
        return {};
    }

    const auto scheme_pos = referrer.find("://");
    if (scheme_pos == std::string_view::npos) {
        return {};
    }

    auto rest = referrer.substr(scheme_pos + 3);
    const auto authority_end = rest.find_first_of("/?#");
    auto authority = rest.substr(0, authority_end);
    rest = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    const auto at = authority.find_last_of('@');
    if (at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        return {};
    }

    std::string path_query = "/";
    if (!rest.empty() && rest.front() == '/') {
        const auto fragment = rest.find('#');
        path_query = std::string(rest.substr(0, fragment));
        if (path_query.empty()) {
            path_query = "/";
        }
    } else if (!rest.empty() && rest.front() == '?') {
        const auto fragment = rest.find('#');
        path_query += std::string(rest.substr(0, fragment));
    }

    return scheme + "://" + std::string(authority) + path_query;
}

bool scheme_allowed(std::string_view scheme, const ResourcePolicy& policy)
{
    return std::find(policy.allowed_schemes.begin(), policy.allowed_schemes.end(), scheme) != policy.allowed_schemes.end();
}

// Extract the lowercased host from a URL authority, stripping any userinfo,
// port, and IPv6 brackets. Returns empty for opaque/relative URLs.
std::string host_of(std::string_view url)
{
    const auto scheme_pos = url.find("://");
    if (scheme_pos == std::string_view::npos) {
        return {};
    }
    auto authority = url.substr(scheme_pos + 3);
    const auto authority_end = authority.find_first_of("/?#");
    if (authority_end != std::string_view::npos) {
        authority = authority.substr(0, authority_end);
    }
    const auto at = authority.find_last_of('@');
    if (at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close != std::string_view::npos) {
            return to_lower(authority.substr(1, close - 1));
        }
        return to_lower(authority.substr(1));
    }
    const auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        authority = authority.substr(0, colon);
    }
    return to_lower(authority);
}

// IPv4 host order. Loopback, private, link-local (incl. 169.254.169.254 cloud
// metadata), CGNAT, unspecified, and multicast/reserved are treated as internal.
bool is_blocked_ipv4(std::uint32_t addr)
{
    const std::uint8_t a = (addr >> 24) & 0xff;
    const std::uint8_t b = (addr >> 16) & 0xff;
    if (a == 127) return true;                       // loopback
    if (a == 10) return true;                        // private
    if (a == 172 && b >= 16 && b <= 31) return true; // private
    if (a == 192 && b == 168) return true;           // private
    if (a == 169 && b == 254) return true;           // link-local + metadata
    if (a == 100 && (b & 0xc0) == 64) return true;   // 100.64/10 CGNAT
    if (a == 0) return true;                         // "this" network
    if (a >= 224) return true;                       // multicast / reserved
    return false;
}

bool is_blocked_ipv6(const std::uint8_t* b)
{
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (b[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return true;                                    // ::
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0
        && b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0
        && b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0
        && b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 1) {
        return true;                                             // ::1 loopback
    }
    if ((b[0] & 0xfe) == 0xfc) return true;                       // fc00::/7 ULA
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;       // fe80::/10 link-local
    if (b[0] == 0xff) return true;                               // multicast
    // IPv4-mapped ::ffff:a.b.c.d
    bool mapped = true;
    for (int i = 0; i < 10; ++i) {
        if (b[i] != 0) { mapped = false; break; }
    }
    if (mapped && b[10] == 0xff && b[11] == 0xff) {
        const std::uint32_t v4 = (static_cast<std::uint32_t>(b[12]) << 24)
            | (static_cast<std::uint32_t>(b[13]) << 16)
            | (static_cast<std::uint32_t>(b[14]) << 8)
            | static_cast<std::uint32_t>(b[15]);
        return is_blocked_ipv4(v4);
    }
    return false;
}

// Block literal-IP and well-known-local hosts before any DNS lookup. DNS names
// that resolve to internal addresses are caught later by the socket callback.
bool is_blocked_literal_host(const std::string& host, const ResourcePolicy& policy)
{
    if (host.empty()) {
        return false;
    }
    for (const auto& blocked : policy.blocked_hosts) {
        if (to_lower(blocked) == host) {
            return true;
        }
    }
    if (!policy.block_private_hosts) {
        return false;
    }
    if (host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0)) {
        return true;
    }
    if (host.find(':') != std::string::npos) {
        std::array<std::uint8_t, 16> bytes{};
        if (inet_pton(AF_INET6, host.c_str(), bytes.data()) == 1) {
            return is_blocked_ipv6(bytes.data());
        }
        return false;
    }
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        return is_blocked_ipv4(ntohl(v4.s_addr));
    }
    return false;
}

// Platform-neutral so the same helpers compile on Windows, where the open-socket
// guard is unavailable and `blocked` simply stays false.
struct OpenSocketContext {
    const ResourcePolicy* policy = nullptr;
    bool blocked = false;
};

#if !defined(_WIN32)
bool is_blocked_sockaddr(const sockaddr* sa)
{
    if (sa == nullptr) {
        return true;
    }
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        return is_blocked_ipv4(ntohl(in->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        return is_blocked_ipv6(in6->sin6_addr.s6_addr);
    }
    return true; // Unknown families (e.g. AF_UNIX) are not legitimate web targets.
}

curl_socket_t guarded_open_socket(void* clientp, curlsocktype purpose, curl_sockaddr* address)
{
    (void) purpose;
    auto* context = static_cast<OpenSocketContext*>(clientp);
    if (context != nullptr && context->policy != nullptr && context->policy->block_private_hosts
        && is_blocked_sockaddr(&address->addr)) {
        context->blocked = true;
        return CURL_SOCKET_BAD;
    }
    return ::socket(address->family, address->socktype, address->protocol);
}
#endif

#if LIBCURL_VERSION_NUM >= 0x075500
std::string curl_request_protocols_string(const ResourcePolicy& policy)
{
    std::string protocols;
    if (scheme_allowed("http", policy)) {
        protocols += "http";
    }
    if (scheme_allowed("https", policy)) {
        if (!protocols.empty()) {
            protocols += ",";
        }
        protocols += "https";
    }
    return protocols;
}
#endif

#if LIBCURL_VERSION_NUM >= 0x075500
std::string curl_redirect_protocols_string(const ResourcePolicy& policy)
{
    std::string protocols;
    if (policy.allow_network && scheme_allowed("http", policy)) {
        protocols += "http";
    }
    if (policy.allow_network && scheme_allowed("https", policy)) {
        if (!protocols.empty()) {
            protocols += ",";
        }
        protocols += "https";
    }
    return protocols;
}
#else
long curl_redirect_protocols_mask(const ResourcePolicy& policy)
{
    long protocols = 0;
    if (policy.allow_network && scheme_allowed("http", policy)) {
        protocols |= CURLPROTO_HTTP;
    }
    if (policy.allow_network && scheme_allowed("https", policy)) {
        protocols |= CURLPROTO_HTTPS;
    }
    return protocols;
}
#endif

std::string origin_of(std::string_view url)
{
    const auto scheme_pos = url.find("://");
    if (scheme_pos == std::string_view::npos) {
        return {};
    }
    const std::string scheme = to_lower(url.substr(0, scheme_pos));
    const std::string host = host_of(url);

    // Authority between "://" and the first path/query/fragment separator.
    auto authority = url.substr(scheme_pos + 3);
    const auto authority_end = authority.find_first_of("/?#");
    if (authority_end != std::string_view::npos) {
        authority = authority.substr(0, authority_end);
    }
    const auto at = authority.find_last_of('@');
    if (at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }

    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close != std::string_view::npos && close + 1 < authority.size() && authority[close + 1] == ':') {
            port = std::string(authority.substr(close + 2));
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            port = std::string(authority.substr(colon + 1));
        }
    }
    if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443")) {
        port.clear();
    }

    std::string origin = scheme + "://" + host;
    if (!port.empty()) {
        origin += ":" + port;
    }
    return origin;
}

void enforce_request_policy(const ResourceRequest& request, const ResourcePolicy& policy)
{
    if (request.url.empty()) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "resource URL is empty");
    }

    const std::string scheme = scheme_of(request.url);
    if (!scheme_allowed(scheme, policy)) {
        throw ResourceError(ResourceErrorCode::SchemeNotAllowed, request.url, "resource scheme is not allowed: " + scheme);
    }

    if (is_network_scheme(scheme) && !policy.allow_network) {
        throw ResourceError(ResourceErrorCode::NetworkDisabled, request.url, "network resource loading is disabled");
    }

    if (is_file_scheme(scheme) && !policy.allow_file) {
        throw ResourceError(ResourceErrorCode::FileDisabled, request.url, "file resource loading is disabled");
    }

    // A network-origin document must not be able to read local files via a
    // file:// sub-resource unless explicitly allowed.
    if (is_file_scheme(scheme) && !policy.allow_file_from_network) {
        const std::string initiator = request.referrer.empty() ? request.base_url : request.referrer;
        if (is_network_scheme(scheme_of(initiator))) {
            throw ResourceError(
                ResourceErrorCode::FileDisabled,
                request.url,
                "file resource loading from a network origin is disabled");
        }
    }

    // Block loopback/private/link-local/metadata literal hosts (and any
    // explicitly blocked host) before the request leaves the process.
    if (is_network_scheme(scheme) && is_blocked_literal_host(host_of(request.url), policy)) {
        throw ResourceError(
            ResourceErrorCode::BlockedHost,
            request.url,
            "resource host is blocked: " + host_of(request.url));
    }

    if (policy.same_origin_only && !request.referrer.empty()) {
        const std::string request_origin = origin_of(request.url);
        const std::string referrer_origin = origin_of(request.referrer);
        if (!request_origin.empty() && !referrer_origin.empty() && request_origin != referrer_origin) {
            throw ResourceError(
                ResourceErrorCode::SameOriginViolation,
                request.url,
                "resource violates same-origin policy");
        }
    }
}

void enforce_response_policy(const ResourceRequest& request, const ResourceResponse& response, const ResourcePolicy& policy)
{
    if (policy.max_response_bytes > 0 && response.body.size() > policy.max_response_bytes) {
        throw ResourceError(ResourceErrorCode::TooLarge, request.url, "resource exceeds max response size");
    }
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

void append_data_byte(std::string& out, char byte, const ResourceRequest& request, const ResourcePolicy& policy)
{
    if (policy.max_response_bytes > 0 && out.size() + 1 > policy.max_response_bytes) {
        throw ResourceError(ResourceErrorCode::TooLarge, request.url, "data URL resource exceeds max response size");
    }
    out.push_back(byte);
}

std::string percent_decode_data_payload(
    std::string_view input,
    const ResourceRequest& request,
    const ResourcePolicy& policy)
{
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "invalid data URL percent escape");
            }
            const int hi = hex_value(input[i + 1]);
            const int lo = hex_value(input[i + 2]);
            if (hi < 0 || lo < 0) {
                throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "invalid data URL percent escape");
            }
            append_data_byte(out, static_cast<char>((hi << 4) | lo), request, policy);
            i += 2;
            continue;
        }
        append_data_byte(out, ch, request, policy);
    }

    return out;
}

std::string base64_decode_data_payload(
    std::string_view input,
    const ResourceRequest& request,
    const ResourcePolicy& policy)
{
    std::string decoded;
    try {
        decoded = base64_decode(input, Base64DecodeMode::Forgiving);
    } catch (const std::exception&) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "invalid data URL base64 payload");
    }

    std::string out;
    out.reserve(decoded.size());
    for (char byte : decoded) {
        append_data_byte(out, byte, request, policy);
    }

    return out;
}

ResourceResponse load_data_url(const ResourceRequest& request, const ResourcePolicy& policy)
{
    const auto colon = request.url.find(':');
    if (colon == std::string::npos) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "invalid data URL");
    }

    const std::string_view data_url(request.url);
    const std::string_view content = data_url.substr(colon + 1);
    const auto comma = content.find(',');
    if (comma == std::string_view::npos) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, request.url, "invalid data URL: missing comma");
    }

    const std::string_view metadata = content.substr(0, comma);
    const std::string_view payload = content.substr(comma + 1);
    const auto first_semicolon = metadata.find(';');

    std::string mime_type = first_semicolon == std::string_view::npos
        ? std::string(metadata)
        : std::string(metadata.substr(0, first_semicolon));
    if (mime_type.empty()) {
        mime_type = "text/plain";
    } else {
        mime_type = to_lower(mime_type);
    }

    bool base64 = false;
    std::size_t parameter_start = first_semicolon == std::string_view::npos
        ? metadata.size()
        : first_semicolon + 1;
    while (parameter_start < metadata.size()) {
        const auto parameter_end = metadata.find(';', parameter_start);
        const std::string_view parameter = metadata.substr(
            parameter_start,
            parameter_end == std::string_view::npos ? std::string_view::npos : parameter_end - parameter_start);
        if (to_lower(parameter) == "base64") {
            base64 = true;
        }
        if (parameter_end == std::string_view::npos) {
            break;
        }
        parameter_start = parameter_end + 1;
    }

    auto headers = content_type_header(mime_type);
    ResourceResponse response{
        request.url,
        base64
            ? base64_decode_data_payload(payload, request, policy)
            : percent_decode_data_payload(payload, request, policy),
        200,
        std::move(mime_type),
        request.kind,
        false,
        "OK",
        std::move(headers),
    };
    enforce_response_policy(request, response, policy);
    return response;
}

std::string extension_of(std::string_view url)
{
    const auto query = url.find('?');
    const auto clean = query == std::string_view::npos ? url : url.substr(0, query);
    const auto dot = clean.find_last_of('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return to_lower(clean.substr(dot + 1));
}

std::string infer_mime_type(std::string_view url, ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Document:
        return "text/html";
    case ResourceKind::Script:
        return "text/javascript";
    case ResourceKind::Stylesheet:
        return "text/css";
    default:
        break;
    }

    const std::string ext = extension_of(url);
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "js" || ext == "mjs") return "text/javascript";
    if (ext == "css") return "text/css";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    return "application/octet-stream";
}

// Decode "file:" / "file://" URLs into a filesystem path. Strips an optional
// (empty or "localhost") authority and the URL fragment/query.
std::filesystem::path file_path_of(std::string_view url)
{
    std::string_view rest = url;
    if (starts_with(rest, "file://")) {
        rest = rest.substr(7);
        // PageCore builds file URLs as "file://" + path, where the path may be
        // relative ("file://examples/x") or absolute ("file:///abs/x"). Keep the
        // remainder as the path; only strip an explicit "localhost" authority.
        if (starts_with(rest, "localhost/")) {
            rest = rest.substr(std::string_view("localhost").size());
        } else if (rest == "localhost") {
            rest = std::string_view{};
        }
    } else if (starts_with(rest, "file:")) {
        rest = rest.substr(5);
    }
    const auto cut = rest.find_first_of("?#");
    if (cut != std::string_view::npos) {
        rest = rest.substr(0, cut);
    }
    return std::filesystem::path(std::string(rest));
}

std::string read_file(std::string_view url, const ResourcePolicy& policy)
{
    std::filesystem::path fs_path = file_path_of(url);

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(fs_path, ec);
    if (ec) {
        canonical = std::filesystem::absolute(fs_path, ec);
    }

    // Confine reads to the configured sandbox root, if any. weakly_canonical
    // resolves "../" and symlink targets, so escapes are rejected here.
    if (!policy.file_root.empty()) {
        std::filesystem::path root = std::filesystem::weakly_canonical(policy.file_root, ec);
        if (ec) {
            root = std::filesystem::absolute(policy.file_root, ec);
        }
        const std::string root_str = root.lexically_normal().string();
        const std::string target_str = canonical.lexically_normal().string();
        const bool contained = target_str == root_str
            || (target_str.size() > root_str.size()
                && target_str.compare(0, root_str.size(), root_str) == 0
                && (root_str.back() == std::filesystem::path::preferred_separator
                    || target_str[root_str.size()] == std::filesystem::path::preferred_separator));
        if (!contained) {
            throw ResourceError(
                ResourceErrorCode::FileDisabled,
                std::string(url),
                "file resource escapes the sandbox root: " + fs_path.string());
        }
    }

    // Reject directories, devices, FIFOs, sockets — only regular files are
    // served, which also prevents /dev/zero and named-pipe hangs.
    if (!std::filesystem::is_regular_file(canonical, ec)) {
        throw ResourceError(
            ResourceErrorCode::NotFound,
            std::string(url),
            "file resource is not a regular file: " + fs_path.string());
    }

    std::ifstream in(canonical, std::ios::binary);
    if (!in) {
        throw ResourceError(
            ResourceErrorCode::NotFound,
            std::string(url),
            "failed to open file resource: " + fs_path.string());
    }

    std::string out;
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        if (policy.max_response_bytes > 0 && out.size() + static_cast<std::size_t>(got) > policy.max_response_bytes) {
            throw ResourceError(
                ResourceErrorCode::TooLarge,
                std::string(url),
                "file resource exceeds max response size");
        }
        out.append(buffer, static_cast<std::size_t>(got));
    }
    return out;
}

std::string without_query_or_fragment(std::string_view url)
{
    const auto suffix = url.find_first_of("?#");
    return std::string(suffix == std::string_view::npos ? url : url.substr(0, suffix));
}

std::string directory_of(std::string_view url)
{
    const std::string clean = without_query_or_fragment(url);
    const auto slash = clean.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }

    const auto scheme = clean.find("://");
    if (scheme != std::string::npos) {
        const auto authority_start = scheme + 3;
        if (slash < authority_start) {
            return clean + "/";
        }
    }

    return clean.substr(0, slash + 1);
}

std::string normalize_url_path(std::string value)
{
    const auto suffix_start = value.find_first_of("?#");
    const std::string suffix = suffix_start == std::string::npos ? std::string() : value.substr(suffix_start);
    const std::string without_suffix = suffix_start == std::string::npos ? value : value.substr(0, suffix_start);

    std::string prefix;
    std::string path;
    const auto scheme = without_suffix.find("://");
    if (scheme != std::string::npos) {
        const auto authority_start = scheme + 3;
        const auto path_start = without_suffix.find('/', authority_start);
        if (path_start == std::string::npos) {
            return without_suffix + suffix;
        }
        prefix = without_suffix.substr(0, path_start);
        path = without_suffix.substr(path_start);
    } else {
        path = without_suffix;
    }

    const bool absolute = starts_with(path, "/");
    const bool trailing_slash = path.size() > 1 && path.back() == '/';
    std::vector<std::string> segments;
    std::size_t start = absolute ? 1 : 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto count = end == std::string::npos ? std::string::npos : end - start;
        std::string segment = path.substr(start, count);
        if (segment.empty() || segment == ".") {
            // Skip.
        } else if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                segments.push_back(std::move(segment));
            }
        } else {
            segments.push_back(std::move(segment));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::string normalized = prefix;
    if (absolute || !prefix.empty()) {
        normalized += "/";
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            normalized += "/";
        }
        normalized += segments[i];
    }
    if (trailing_slash && !normalized.empty() && normalized.back() != '/') {
        normalized += "/";
    }
    if (normalized.empty()) {
        normalized = absolute ? "/" : ".";
    }
    return normalized + suffix;
}

std::string scheme_prefix_of(std::string_view url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return "http:";
    }
    return std::string(url.substr(0, scheme + 1));
}

bool looks_like_host_reference(std::string_view candidate)
{
    if (candidate.empty()
        || candidate.front() == '.'
        || candidate.front() == '#'
        || candidate.front() == '?'
        || candidate.front() == '/') {
        return false;
    }

    const auto first_separator = candidate.find_first_of("/?#");
    if (first_separator == std::string_view::npos || candidate[first_separator] != '/') {
        return false;
    }

    const std::string_view host = candidate.substr(0, first_separator);
    if (host.empty() || host.find('.') == std::string_view::npos || host.find(':') != std::string_view::npos) {
        return false;
    }

    const auto last_dot = host.find_last_of('.');
    if (last_dot == std::string_view::npos || last_dot + 1 >= host.size()) {
        return false;
    }
    const std::string_view tld = host.substr(last_dot + 1);
    if (tld.size() < 2) {
        return false;
    }
    for (char ch : tld) {
        if (!std::isalpha(static_cast<unsigned char>(ch))) {
            return false;
        }
    }

    for (char ch : host) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '.' && ch != '-') {
            return false;
        }
    }
    return true;
}

CurlHeaderList apply_request_metadata(CURL* curl, const ResourceRequest& request)
{
    if (request.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (request.method == "GET" || request.method.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        if (request.method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }

    curl_slist* raw_headers = nullptr;
    for (const auto& [name, value] : request.headers) {
        if (name.empty()) {
            continue;
        }
        if (header_name_equals(name, "referer")) {
            const std::string sanitized_referrer = sanitize_http_referrer(value);
            if (sanitized_referrer.empty()) {
                continue;
            }
            const std::string header = "Referer: " + sanitized_referrer;
            raw_headers = curl_slist_append(raw_headers, header.c_str());
        } else {
            const std::string header = name + ": " + value;
            raw_headers = curl_slist_append(raw_headers, header.c_str());
        }
    }
    CurlHeaderList headers(raw_headers);
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
    }
    return headers;
}

std::string cache_key_for(const ResourceRequest& request)
{
    std::string key = resource_kind_name(request.kind) + "\n" + request.url + "\n" + request.referrer
        + "\n" + request.method + "\n" + request.body;
    for (const auto& [name, value] : request.headers) {
        key += "\n";
        key += name;
        key += ": ";
        key += value;
    }
    return key;
}

// Applies every libcurl option for a single network transfer. Shared verbatim by
// the serial load() and the concurrent load_all() so both paths enforce the same
// protocol pinning, SSRF socket guard, size cap, timeout, and connection reuse.
CurlHeaderList configure_network_handle(
    CURL* curl,
    const ResourceRequest& request,
    const std::string& user_agent,
    const ResourcePolicy& policy,
    CURLSH* share,
    CurlBody& body,
    CurlHeaders& response_headers,
    OpenSocketContext& socket_context)
{
    body.max_bytes = policy.max_response_bytes;
    response_headers.current_url = request.url;

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // Pin the protocol set for the initial transfer too (not just redirects);
    // otherwise curl's build-dependent default enables file/scp/gopher/etc.
#if LIBCURL_VERSION_NUM >= 0x075500
    const std::string request_protocols = curl_request_protocols_string(policy);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, request_protocols.empty() ? "" : request_protocols.c_str());
    const std::string redirect_protocols = curl_redirect_protocols_string(policy);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, redirect_protocols.empty() ? "" : redirect_protocols.c_str());
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, curl_redirect_protocols_mask(policy));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, curl_redirect_protocols_mask(policy));
#endif
#if !defined(_WIN32)
    socket_context.policy = &policy;
    socket_context.blocked = false;
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, guarded_open_socket);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &socket_context);
#else
    (void) socket_context;
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(policy.timeout.count()));
    const std::string sanitized_referrer = sanitize_http_referrer(request.referrer);
    if (!sanitized_referrer.empty() && !has_request_header(request, "referer")) {
        curl_easy_setopt(curl, CURLOPT_REFERER, sanitized_referrer.c_str());
    }
    if (share != nullptr) {
        curl_easy_setopt(curl, CURLOPT_SHARE, share);
    }
    return apply_request_metadata(curl, request);
}

// Reads back the transfer result, enforces the post-transfer policy (size cap,
// SSRF socket block, effective-URL re-validation after redirects), and builds the
// response. Throws ResourceError on any failure. Shared by load()/load_all().
ResourceResponse build_network_response(
    CURL* curl,
    const ResourceRequest& request,
    const ResourcePolicy& policy,
    CurlBody& body,
    CurlHeaders& response_headers,
    OpenSocketContext& socket_context,
    CURLcode code)
{
    const std::string url_string(request.url);
    long status = 0;
    char* content_type = nullptr;
    char* effective_url = nullptr;
    long redirect_count = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirect_count);
    const std::string response_url = effective_url == nullptr ? url_string : std::string(effective_url);
    const std::string response_mime_type =
        content_type == nullptr ? infer_mime_type(url_string, request.kind) : std::string(content_type);
    if (response_headers.headers.empty() && !response_mime_type.empty()) {
        response_headers.headers = content_type_header(response_mime_type);
    }
    response_headers.headers.insert(
        response_headers.headers.end(),
        response_headers.set_cookie_headers.begin(),
        response_headers.set_cookie_headers.end());

    if (body.too_large) {
        throw ResourceError(ResourceErrorCode::TooLarge, request.url, "resource exceeds max response size");
    }

    if (socket_context.blocked) {
        throw ResourceError(
            ResourceErrorCode::BlockedHost,
            request.url,
            "resource resolved to a blocked (private/loopback/link-local) address");
    }

    if (code != CURLE_OK) {
        const auto error_code = code == CURLE_OPERATION_TIMEDOUT ? ResourceErrorCode::Timeout : ResourceErrorCode::Transport;
        throw ResourceError(error_code, request.url, std::string("resource load failed: ") + curl_easy_strerror(code));
    }

    // Re-validate the effective URL after any redirects (scheme, host, origin).
    enforce_request_policy(
        ResourceRequest{response_url, request.kind, request.referrer, request.base_url},
        policy);

    ResourceResponse response{
        response_url,
        std::move(body.body),
        static_cast<int>(status),
        response_mime_type,
        request.kind,
        false,
        response_headers.status_text.empty()
            ? default_status_text(static_cast<int>(status))
            : response_headers.status_text,
        std::move(response_headers.headers),
        static_cast<int>(redirect_count),
        std::move(response_headers.set_cookie_sources),
    };
    enforce_response_policy(request, response, policy);
    return response;
}

} // namespace

ResourceError::ResourceError(ResourceErrorCode code, std::string url, std::string message)
    : std::runtime_error(std::move(message))
    , code_(code)
    , url_(std::move(url))
{
}

ResourceErrorCode ResourceError::code() const noexcept
{
    return code_;
}

const std::string& ResourceError::url() const noexcept
{
    return url_;
}

ResourceResponse ResourceLoader::load(std::string_view url)
{
    return load(ResourceRequest{std::string(url)});
}

std::vector<ResourceResponse> ResourceLoader::load_all(const std::vector<ResourceRequest>& requests, BatchErrorMode mode)
{
    std::vector<ResourceResponse> responses;
    responses.reserve(requests.size());
    for (const auto& request : requests) {
        if (mode == BatchErrorMode::FailFast) {
            responses.push_back(load(request));
            continue;
        }
        try {
            responses.push_back(load(request));
        } catch (const ResourceError&) {
            responses.push_back(ResourceResponse{request.url, {}, 0, {}, request.kind, false});
        }
    }
    return responses;
}

CurlResourceLoader::CurlResourceLoader(std::string user_agent, ResourcePolicy policy)
    : user_agent_(std::move(user_agent))
    , policy_(std::move(policy))
    , shared_(std::make_shared<CurlShared>())
{
}

void ensure_curl_global_init()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

ResourceResponse CurlResourceLoader::load(const ResourceRequest& request)
{
    enforce_request_policy(request, policy_);

    const std::string url_string(request.url);
    const std::string scheme = scheme_of(url_string);

    if (is_data_scheme(scheme)) {
        return load_data_url(request, policy_);
    }

    if (!is_network_scheme(scheme)) {
        const std::string mime_type = infer_mime_type(url_string, request.kind);
        ResourceResponse response{
            url_string,
            read_file(url_string, policy_),
            200,
            mime_type,
            request.kind,
            false,
            "OK",
            content_type_header(mime_type),
        };
        enforce_response_policy(request, response, policy_);
        return response;
    }

    ensure_curl_global_init();

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (curl == nullptr) {
        throw ResourceError(ResourceErrorCode::Transport, request.url, "failed to initialize libcurl");
    }

    auto* curl_shared = static_cast<CurlShared*>(shared_.get());
    CURLSH* share = curl_shared == nullptr ? nullptr : curl_shared->share;

    CurlBody body;
    CurlHeaders response_headers;
    OpenSocketContext socket_context;
    CurlHeaderList headers =
        configure_network_handle(curl.get(), request, user_agent_, policy_, share, body, response_headers, socket_context);

    const CURLcode code = curl_easy_perform(curl.get());
    return build_network_response(curl.get(), request, policy_, body, response_headers, socket_context, code);
}

std::vector<ResourceResponse> CurlResourceLoader::load_all(const std::vector<ResourceRequest>& requests, BatchErrorMode mode)
{
    if (requests.empty()) {
        return {};
    }
    if (requests.size() == 1) {
        if (mode == BatchErrorMode::FailFast) {
            return {load(requests.front())};
        }
        try {
            return {load(requests.front())};
        } catch (const ResourceError&) {
            return {ResourceResponse{requests.front().url, {}, 0, {}, requests.front().kind, false}};
        }
    }

    ensure_curl_global_init();

    const std::size_t count = requests.size();
    std::vector<ResourceResponse> responses(count);
    std::vector<std::exception_ptr> errors(count);
    std::vector<bool> done(count, false);

    // Stable storage: handles in the multi reference these by address, so the
    // vectors must not reallocate while transfers are in flight.
    std::vector<CURL*> handles(count, nullptr);
    std::vector<CurlBody> bodies(count);
    std::vector<CurlHeaders> response_headers(count);
    std::vector<OpenSocketContext> contexts(count);
    std::vector<CurlHeaderList> header_lists(count);

    auto* curl_shared = static_cast<CurlShared*>(shared_.get());
    CURLSH* share = curl_shared == nullptr ? nullptr : curl_shared->share;

    CURLM* multi = curl_multi_init();
    if (multi == nullptr) {
        // Fall back to serial loading rather than failing the whole batch.
        for (std::size_t i = 0; i < count; ++i) {
            responses[i] = load(requests[i]);
        }
        return responses;
    }
    // Bound parallelism the way browsers do, so a page cannot open an unbounded
    // number of sockets at once.
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8L);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);

    for (std::size_t i = 0; i < count; ++i) {
        try {
            enforce_request_policy(requests[i], policy_);
            const std::string scheme = scheme_of(requests[i].url);
            if (is_data_scheme(scheme)) {
                responses[i] = load_data_url(requests[i], policy_);
                done[i] = true;
                continue;
            }
            if (!is_network_scheme(scheme)) {
                const std::string mime_type = infer_mime_type(requests[i].url, requests[i].kind);
                ResourceResponse response{
                    requests[i].url,
                    read_file(requests[i].url, policy_),
                    200,
                    mime_type,
                    requests[i].kind,
                    false,
                    "OK",
                    content_type_header(mime_type),
                };
                enforce_response_policy(requests[i], response, policy_);
                responses[i] = std::move(response);
                done[i] = true;
                continue;
            }

            CURL* handle = curl_easy_init();
            if (handle == nullptr) {
                throw ResourceError(ResourceErrorCode::Transport, requests[i].url, "failed to initialize libcurl");
            }
            header_lists[i] =
                configure_network_handle(
                    handle,
                    requests[i],
                    user_agent_,
                    policy_,
                    share,
                    bodies[i],
                    response_headers[i],
                    contexts[i]);
            handles[i] = handle;
            curl_multi_add_handle(multi, handle);
        } catch (...) {
            errors[i] = std::current_exception();
            done[i] = true;
        }
    }

    int still_running = 0;
    curl_multi_perform(multi, &still_running);
    while (still_running > 0) {
        const CURLMcode poll_code = curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        if (poll_code != CURLM_OK) {
            break;
        }
        curl_multi_perform(multi, &still_running);
    }

    // Collect the per-handle transfer result codes.
    std::unordered_map<CURL*, CURLcode> result_codes;
    CURLMsg* message = nullptr;
    int in_queue = 0;
    while ((message = curl_multi_info_read(multi, &in_queue)) != nullptr) {
        if (message->msg == CURLMSG_DONE) {
            result_codes[message->easy_handle] = message->data.result;
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (done[i] || handles[i] == nullptr) {
            continue;
        }
        const auto found = result_codes.find(handles[i]);
        const CURLcode code = found == result_codes.end() ? CURLE_RECV_ERROR : found->second;
        try {
            responses[i] =
                build_network_response(handles[i], requests[i], policy_, bodies[i], response_headers[i], contexts[i], code);
        } catch (...) {
            errors[i] = std::current_exception();
        }
        curl_multi_remove_handle(multi, handles[i]);
        curl_easy_cleanup(handles[i]);
        handles[i] = nullptr;
    }

    curl_multi_cleanup(multi);

    for (std::size_t i = 0; i < count; ++i) {
        if (!errors[i]) {
            continue;
        }
        if (mode == BatchErrorMode::FailFast) {
            // Match load(): surface the first request-order failure.
            std::rethrow_exception(errors[i]);
        }
        // Lenient: a failed transfer becomes a status-0 placeholder.
        responses[i] = ResourceResponse{requests[i].url, {}, 0, {}, requests[i].kind, false};
    }
    return responses;
}

const ResourcePolicy& CurlResourceLoader::policy() const noexcept
{
    return policy_;
}

void CurlResourceLoader::set_policy(ResourcePolicy policy)
{
    policy_ = std::move(policy);
}

MemoryResourceLoader::MemoryResourceLoader(ResourcePolicy policy)
    : policy_(std::move(policy))
{
}

void MemoryResourceLoader::add(std::string url, std::string body)
{
    add(std::move(url), std::move(body), {});
}

void MemoryResourceLoader::add(std::string url, std::string body, std::string mime_type)
{
    const std::string key = url;
    if (mime_type.empty()) {
        mime_type = infer_mime_type(key, ResourceKind::Other);
    }
    resources_[key] = ResourceResponse{
        std::move(url),
        std::move(body),
        200,
        mime_type,
        ResourceKind::Other,
        false,
        "OK",
        content_type_header(mime_type),
    };
}

ResourceResponse MemoryResourceLoader::load(const ResourceRequest& request)
{
    enforce_request_policy(request, policy_);

    if (is_data_scheme(scheme_of(request.url))) {
        return load_data_url(request, policy_);
    }

    const std::string key(request.url);
    auto it = resources_.find(key);
    if (it == resources_.end()) {
        throw ResourceError(ResourceErrorCode::NotFound, key, "memory resource not found: " + key);
    }

    ResourceResponse response = it->second;
    response.kind = request.kind;
    response.from_cache = false;
    if (response.mime_type.empty()) {
        response.mime_type = infer_mime_type(response.url, request.kind);
    }
    enforce_response_policy(request, response, policy_);
    return response;
}

const ResourcePolicy& MemoryResourceLoader::policy() const noexcept
{
    return policy_;
}

void MemoryResourceLoader::set_policy(ResourcePolicy policy)
{
    policy_ = std::move(policy);
}

CachingResourceLoader::CachingResourceLoader(std::shared_ptr<ResourceLoader> inner, std::size_t max_entries)
    : inner_(std::move(inner))
    , max_entries_(max_entries == 0 ? 1 : max_entries)
{
    if (!inner_) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, {}, "caching resource loader requires inner loader");
    }
}

ResourceResponse CachingResourceLoader::load(const ResourceRequest& request)
{
    const std::string key = cache_key_for(request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cached = cache_.find(key);
        if (cached != cache_.end()) {
            ResourceResponse response = cached->second;
            response.from_cache = true;
            return response;
        }
    }

    // The inner load runs without the lock so concurrent loads of different
    // resources do not serialize.
    ResourceResponse response = inner_->load(request);
    response.from_cache = false;

    // Do not pin error responses (4xx/5xx) — they are often transient.
    if (response.status < 400) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.find(key) == cache_.end()) {
            order_.push_back(key);
            while (order_.size() > max_entries_) {
                cache_.erase(order_.front());
                order_.pop_front();
            }
        }
        cache_[key] = response;
    }
    return response;
}

std::vector<ResourceResponse> CachingResourceLoader::load_all(const std::vector<ResourceRequest>& requests, BatchErrorMode mode)
{
    const std::size_t count = requests.size();
    std::vector<ResourceResponse> responses(count);
    std::vector<std::string> keys(count);
    std::vector<ResourceRequest> misses;
    std::vector<std::size_t> miss_slots;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            keys[i] = cache_key_for(requests[i]);
            auto cached = cache_.find(keys[i]);
            if (cached != cache_.end()) {
                responses[i] = cached->second;
                responses[i].from_cache = true;
            } else {
                misses.push_back(requests[i]);
                miss_slots.push_back(i);
            }
        }
    }

    if (!misses.empty()) {
        // Misses keep their original relative order, so the inner loader still
        // surfaces the first request-order failure (FailFast). Cache hits never
        // fail, so the first thrown error is also the first overall failure.
        std::vector<ResourceResponse> fetched = inner_->load_all(misses, mode);

        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t m = 0; m < fetched.size(); ++m) {
            const std::size_t i = miss_slots[m];
            fetched[m].from_cache = false;
            responses[i] = std::move(fetched[m]);

            // Cache real successes only; a Lenient failure placeholder (status 0)
            // must stay uncached so an on-demand retry can still fetch it.
            if (responses[i].status >= 200 && responses[i].status < 400) {
                if (cache_.find(keys[i]) == cache_.end()) {
                    order_.push_back(keys[i]);
                    while (order_.size() > max_entries_) {
                        cache_.erase(order_.front());
                        order_.pop_front();
                    }
                }
                cache_[keys[i]] = responses[i];
            }
        }
    }

    return responses;
}

void CachingResourceLoader::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    order_.clear();
}

std::size_t CachingResourceLoader::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

std::string resolve_url(std::string_view base_url, std::string_view candidate)
{
    if (candidate.empty()) {
        return std::string(base_url);
    }

    if (has_url_scheme(candidate)) {
        const std::string scheme = scheme_of(candidate);
        if (!is_network_scheme(scheme) && !is_file_scheme(scheme)) {
            return std::string(candidate);
        }
        return normalize_url_path(std::string(candidate));
    }

    if (starts_with(candidate, "//")) {
        return normalize_url_path(scheme_prefix_of(base_url) + std::string(candidate));
    }

    if (looks_like_host_reference(candidate)) {
        return normalize_url_path(scheme_prefix_of(base_url) + "//" + std::string(candidate));
    }

    if (candidate.front() == '/') {
        const auto scheme = base_url.find("://");
        if (scheme == std::string_view::npos) {
            return normalize_url_path(std::string(candidate));
        }

        const auto authority_start = scheme + 3;
        const auto authority_end = base_url.find('/', authority_start);
        if (authority_end == std::string_view::npos) {
            return normalize_url_path(std::string(base_url) + std::string(candidate));
        }
        return normalize_url_path(std::string(base_url.substr(0, authority_end)) + std::string(candidate));
    }

    return normalize_url_path(directory_of(base_url) + std::string(candidate));
}

std::string resource_kind_name(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Document:
        return "document";
    case ResourceKind::Script:
        return "script";
    case ResourceKind::Stylesheet:
        return "stylesheet";
    case ResourceKind::Image:
        return "image";
    case ResourceKind::Font:
        return "font";
    case ResourceKind::Other:
        return "other";
    }
    return "other";
}

std::string resource_error_code_name(ResourceErrorCode code)
{
    switch (code) {
    case ResourceErrorCode::InvalidRequest:
        return "invalid_request";
    case ResourceErrorCode::NetworkDisabled:
        return "network_disabled";
    case ResourceErrorCode::FileDisabled:
        return "file_disabled";
    case ResourceErrorCode::SchemeNotAllowed:
        return "scheme_not_allowed";
    case ResourceErrorCode::SameOriginViolation:
        return "same_origin_violation";
    case ResourceErrorCode::BlockedHost:
        return "blocked_host";
    case ResourceErrorCode::NotFound:
        return "not_found";
    case ResourceErrorCode::TooLarge:
        return "too_large";
    case ResourceErrorCode::Timeout:
        return "timeout";
    case ResourceErrorCode::Transport:
        return "transport";
    }
    return "transport";
}

} // namespace pagecore

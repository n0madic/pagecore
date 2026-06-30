#include "pagecore/resource_loader.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace pagecore {
namespace {

struct CurlBody {
    std::string body;
    std::size_t max_bytes = 0;
    bool too_large = false;
};

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

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
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

bool scheme_allowed(std::string_view scheme, const ResourcePolicy& policy)
{
    return std::find(policy.allowed_schemes.begin(), policy.allowed_schemes.end(), scheme) != policy.allowed_schemes.end();
}

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

    const auto authority_start = scheme_pos + 3;
    const auto authority_end = url.find('/', authority_start);
    if (authority_end == std::string_view::npos) {
        return to_lower(url);
    }
    return to_lower(url.substr(0, authority_end));
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

std::string read_file(std::string_view path)
{
    std::filesystem::path fs_path(path);
    if (starts_with(path, "file://")) {
        fs_path = std::filesystem::path(std::string(path.substr(7)));
    }

    std::ifstream in(fs_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file resource: " + fs_path.string());
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
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

CurlResourceLoader::CurlResourceLoader(std::string user_agent, ResourcePolicy policy)
    : user_agent_(std::move(user_agent))
    , policy_(std::move(policy))
{
}

ResourceResponse CurlResourceLoader::load(const ResourceRequest& request)
{
    enforce_request_policy(request, policy_);

    const std::string url_string(request.url);
    const std::string scheme = scheme_of(url_string);

    if (!is_network_scheme(scheme)) {
        ResourceResponse response{
            url_string,
            read_file(url_string),
            200,
            infer_mime_type(url_string, request.kind),
            request.kind,
            false,
        };
        enforce_response_policy(request, response, policy_);
        return response;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw ResourceError(ResourceErrorCode::Transport, request.url, "failed to initialize libcurl");
    }

    CurlBody body;
    body.max_bytes = policy_.max_response_bytes;
    long status = 0;
    char* content_type = nullptr;
    char* effective_url = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
#if LIBCURL_VERSION_NUM >= 0x075500
    const std::string redirect_protocols = curl_redirect_protocols_string(policy_);
    if (!redirect_protocols.empty()) {
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, redirect_protocols.c_str());
    }
#else
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, curl_redirect_protocols_mask(policy_));
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(policy_.timeout.count()));

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::string response_url = effective_url == nullptr ? url_string : std::string(effective_url);
    const std::string response_mime_type =
        content_type == nullptr ? infer_mime_type(url_string, request.kind) : std::string(content_type);
    curl_easy_cleanup(curl);

    if (body.too_large) {
        throw ResourceError(ResourceErrorCode::TooLarge, request.url, "resource exceeds max response size");
    }

    if (code != CURLE_OK) {
        const auto error_code = code == CURLE_OPERATION_TIMEDOUT ? ResourceErrorCode::Timeout : ResourceErrorCode::Transport;
        throw ResourceError(error_code, request.url, std::string("resource load failed: ") + curl_easy_strerror(code));
    }

    enforce_request_policy(
        ResourceRequest{response_url, request.kind, request.referrer, request.base_url},
        policy_);

    ResourceResponse response{
        response_url,
        std::move(body.body),
        static_cast<int>(status),
        response_mime_type,
        request.kind,
        false,
    };
    enforce_response_policy(request, response, policy_);
    return response;
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
    resources_[key] = ResourceResponse{
        std::move(url),
        std::move(body),
        200,
        mime_type.empty() ? infer_mime_type(key, ResourceKind::Other) : std::move(mime_type),
        ResourceKind::Other,
        false,
    };
}

ResourceResponse MemoryResourceLoader::load(const ResourceRequest& request)
{
    enforce_request_policy(request, policy_);

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

CachingResourceLoader::CachingResourceLoader(std::shared_ptr<ResourceLoader> inner)
    : inner_(std::move(inner))
{
    if (!inner_) {
        throw ResourceError(ResourceErrorCode::InvalidRequest, {}, "caching resource loader requires inner loader");
    }
}

ResourceResponse CachingResourceLoader::load(const ResourceRequest& request)
{
    const std::string key = resource_kind_name(request.kind) + "\n" + request.url + "\n" + request.referrer;
    auto cached = cache_.find(key);
    if (cached != cache_.end()) {
        ResourceResponse response = cached->second;
        response.from_cache = true;
        return response;
    }

    ResourceResponse response = inner_->load(request);
    response.from_cache = false;
    cache_[key] = response;
    return response;
}

void CachingResourceLoader::clear()
{
    cache_.clear();
}

std::size_t CachingResourceLoader::size() const noexcept
{
    return cache_.size();
}

std::string resolve_url(std::string_view base_url, std::string_view candidate)
{
    if (candidate.empty()) {
        return std::string(base_url);
    }

    if (has_url_scheme(candidate)) {
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

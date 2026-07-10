#pragma once

// Internal header shared between the blocking CurlResourceLoader paths and the
// libuv-driven curl_multi_socket_action engine (curl_async_loader.cpp). All
// implementations live in resource_loader.cpp so both paths enforce the exact
// same policy, SSRF guard, size caps, and response construction.

#include "pagecore/resource_loader.hpp"

#include <curl/curl.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pagecore {

void ensure_curl_global_init();

namespace detail {

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
    std::size_t header_bytes = 0;
    bool too_large = false;
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

// Connect-time SSRF guard context (CURLOPT_OPENSOCKETFUNCTION). The policy
// pointer must stay valid for the whole transfer.
struct OpenSocketContext {
    const ResourcePolicy* policy = nullptr;
    bool blocked = false;
};

std::string scheme_of(std::string_view url);
bool is_network_scheme(std::string_view scheme);
bool is_file_scheme(std::string_view scheme);
bool is_data_scheme(std::string_view scheme);
std::vector<std::pair<std::string, std::string>> content_type_header(std::string_view mime_type);
std::string infer_mime_type(std::string_view url, ResourceKind kind);

void enforce_request_policy(const ResourceRequest& request, const ResourcePolicy& policy);
void enforce_response_policy(
    const ResourceRequest& request,
    const ResourceResponse& response,
    const ResourcePolicy& policy);

ResourceResponse load_data_url(const ResourceRequest& request, const ResourcePolicy& policy);
std::string read_file(std::string_view url, const ResourcePolicy& policy);

// Applies every libcurl option for a single network transfer. `policy`,
// `body`, `response_headers`, and `socket_context` are referenced by the
// handle for the whole transfer and must outlive it.
CurlHeaderList configure_network_handle(
    CURL* curl,
    const ResourceRequest& request,
    const std::string& user_agent,
    const ResourcePolicy& policy,
    CURLSH* share,
    CurlBody& body,
    CurlHeaders& response_headers,
    OpenSocketContext& socket_context);

// Reads back the transfer result, enforces the post-transfer policy, and
// builds the response. Throws ResourceError on any failure.
ResourceResponse build_network_response(
    CURL* curl,
    const ResourceRequest& request,
    const ResourcePolicy& policy,
    CurlBody& body,
    CurlHeaders& response_headers,
    OpenSocketContext& socket_context,
    CURLcode code);

} // namespace detail
} // namespace pagecore

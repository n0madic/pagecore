#pragma once

#include "pagecore/resource_loader.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagecore {

enum class CookieCredentials {
    Omit,
    SameOrigin,
    Include,
};

class CookieJar {
public:
    struct Cookie {
        std::string name;
        std::string value;
        std::string domain;
        std::string path = "/";
        bool host_only = true;
        bool secure = false;
        bool http_only = false;
        std::optional<std::chrono::system_clock::time_point> expires;
    };

    std::string document_cookie(std::string_view document_url) const;
    void set_document_cookie(std::string_view document_url, std::string_view cookie);

    ResourceRequest with_cookie_header(
        ResourceRequest request,
        CookieCredentials credentials,
        std::string_view document_url) const;
    void store_from_response(
        std::string_view request_url,
        const ResourceResponse& response,
        CookieCredentials credentials,
        std::string_view document_url);

    void clear();

private:
    std::string cookie_header_for_url(std::string_view url, bool include_http_only) const;
    void set_cookie_from_header(std::string_view request_url, std::string_view header, bool from_http);

    mutable std::mutex mutex_;
    std::vector<Cookie> cookies_;
};

bool cookie_credentials_allow(
    CookieCredentials credentials,
    std::string_view request_url,
    std::string_view document_url);

} // namespace pagecore

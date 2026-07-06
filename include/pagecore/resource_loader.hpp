#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pagecore {

enum class ResourceKind {
    Document,
    Script,
    Stylesheet,
    Image,
    Font,
    Other,
};

enum class ResourceErrorCode {
    InvalidRequest,
    NetworkDisabled,
    FileDisabled,
    SchemeNotAllowed,
    SameOriginViolation,
    BlockedHost,
    NotFound,
    TooLarge,
    Timeout,
    Transport,
};

// Controls how load_all() reacts to a failing request.
//   FailFast: throw the first (request-order) ResourceError, like load().
//   Lenient:  never throw; a failed request yields a placeholder response with
//             status 0 and an empty body, so callers warming non-critical
//             sub-resources (images, fonts) can skip individual failures.
enum class BatchErrorMode {
    FailFast,
    Lenient,
};

struct ResourceRequest {
    std::string url;
    ResourceKind kind = ResourceKind::Other;
    std::string referrer;
    std::string base_url;
    std::string method = "GET";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct ResourceResponse {
    std::string url;
    std::string body;
    int status = 0;
    std::string mime_type;
    ResourceKind kind = ResourceKind::Other;
    bool from_cache = false;
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    int redirect_count = 0;
    std::vector<std::pair<std::string, std::string>> set_cookie_headers;
};

struct ResourcePolicy {
    bool allow_network = true;
    bool allow_file = true;
    bool same_origin_only = false;
    // Reject network requests whose target resolves to a loopback, private,
    // link-local, or cloud-metadata address. Enforced both on literal-IP URLs
    // and (via a connect-time socket callback) on the post-DNS address, which
    // closes DNS-rebinding and redirect-to-internal SSRF vectors.
    bool block_private_hosts = true;
    // Forbid file:// sub-resources whose initiator (referrer/base) is a network
    // origin, so a remote page cannot read local files. Top-level file:// loads
    // (empty initiator) are unaffected.
    bool allow_file_from_network = false;
    std::size_t max_response_bytes = 10 * 1024 * 1024;
    std::chrono::milliseconds timeout{10000};
    std::vector<std::string> allowed_schemes{"http", "https", "file", "data"};
    // Additional host names or literal IPs to reject (case-insensitive).
    std::vector<std::string> blocked_hosts{};
    // When non-empty, confine file:// reads to this directory (symlink escapes
    // are rejected after canonicalization).
    std::string file_root{};
    // Proxy for network transfers. std::nullopt (the default) disables proxying
    // AND ignores the ambient http_proxy/HTTPS_PROXY/ALL_PROXY environment
    // variables: an env proxy would relay to the real target itself, hiding it
    // from the connect-time SSRF socket guard (which only sees the proxy's
    // address) and silently defeating block_private_hosts. Set an explicit value
    // (including "" to force-disable) to opt back into a specific proxy.
    std::optional<std::string> proxy{};
};

class ResourceError final : public std::runtime_error {
public:
    ResourceError(ResourceErrorCode code, std::string url, std::string message);

    ResourceErrorCode code() const noexcept;
    const std::string& url() const noexcept;

private:
    ResourceErrorCode code_;
    std::string url_;
};

class ResourceLoader {
public:
    virtual ~ResourceLoader() = default;
    ResourceResponse load(std::string_view url);
    virtual ResourceResponse load(const ResourceRequest& request) = 0;

    // Loads multiple resources, returning responses in request order. In FailFast
    // mode (the default) it throws the first request-order ResourceError, matching
    // the single-resource load(); in Lenient mode it never throws and returns a
    // status-0 placeholder for each failed request. The default implementation
    // loads serially; loaders such as CurlResourceLoader override it to fetch
    // concurrently. The FailFast default is kept identical across overrides so a
    // single-argument call behaves the same regardless of static type.
    virtual std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast);
};

class CurlResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    explicit CurlResourceLoader(std::string user_agent = "PageCore/0.1", ResourcePolicy policy = {});
    ResourceResponse load(const ResourceRequest& request) override;
    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override;

    // policy() returns a reference for configuration before loading starts; it is
    // not synchronized against a concurrent set_policy. In-flight load()/load_all()
    // calls are safe against set_policy: each takes a private snapshot up front.
    const ResourcePolicy& policy() const noexcept;
    void set_policy(ResourcePolicy policy);

private:
    ResourcePolicy snapshot_policy() const;

    std::string user_agent_;
    mutable std::mutex policy_mutex_;
    ResourcePolicy policy_;
    // Opaque libcurl share handle (connection/DNS/TLS-session reuse) plus its
    // lock mutexes. Type-erased so the public header stays curl-free; shared so
    // the loader remains copyable. Defined as CurlShared in the .cpp.
    std::shared_ptr<void> shared_;
};

class MemoryResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    explicit MemoryResourceLoader(ResourcePolicy policy = {});

    void add(std::string url, std::string body);
    void add(std::string url, std::string body, std::string mime_type);
    ResourceResponse load(const ResourceRequest& request) override;

    const ResourcePolicy& policy() const noexcept;
    void set_policy(ResourcePolicy policy);

private:
    std::unordered_map<std::string, ResourceResponse> resources_;
    ResourcePolicy policy_;
};

class CachingResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    explicit CachingResourceLoader(std::shared_ptr<ResourceLoader> inner, std::size_t max_entries = 256);

    ResourceResponse load(const ResourceRequest& request) override;
    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override;
    void clear();
    std::size_t size() const noexcept;

private:
    // Promote key to the most-recently-used end of order_. Caller holds mutex_.
    void touch_locked(const std::string& key);

    std::shared_ptr<ResourceLoader> inner_;
    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ResourceResponse> cache_;
    std::deque<std::string> order_;
};

std::string resolve_url(std::string_view base_url, std::string_view candidate);
std::string resource_kind_name(ResourceKind kind);
std::string resource_error_code_name(ResourceErrorCode code);

} // namespace pagecore

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
    NotFound,
    TooLarge,
    Timeout,
    Transport,
};

struct ResourceRequest {
    std::string url;
    ResourceKind kind = ResourceKind::Other;
    std::string referrer;
    std::string base_url;
};

struct ResourceResponse {
    std::string url;
    std::string body;
    int status = 0;
    std::string mime_type;
    ResourceKind kind = ResourceKind::Other;
    bool from_cache = false;
};

struct ResourcePolicy {
    bool allow_network = true;
    bool allow_file = true;
    bool same_origin_only = false;
    std::size_t max_response_bytes = 10 * 1024 * 1024;
    std::chrono::milliseconds timeout{10000};
    std::vector<std::string> allowed_schemes{"http", "https", "file"};
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
};

class CurlResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    explicit CurlResourceLoader(std::string user_agent = "PageCore/0.1", ResourcePolicy policy = {});
    ResourceResponse load(const ResourceRequest& request) override;

    const ResourcePolicy& policy() const noexcept;
    void set_policy(ResourcePolicy policy);

private:
    std::string user_agent_;
    ResourcePolicy policy_;
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

    explicit CachingResourceLoader(std::shared_ptr<ResourceLoader> inner);

    ResourceResponse load(const ResourceRequest& request) override;
    void clear();
    std::size_t size() const noexcept;

private:
    std::shared_ptr<ResourceLoader> inner_;
    std::unordered_map<std::string, ResourceResponse> cache_;
};

std::string resolve_url(std::string_view base_url, std::string_view candidate);
std::string resource_kind_name(ResourceKind kind);
std::string resource_error_code_name(ResourceErrorCode code);

} // namespace pagecore

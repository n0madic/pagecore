#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"
#include "pagecore/render.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

extern "C" {
#include <quickjs.h>
}

namespace pagecore {

class ResourceLoader;
struct ResourceResponse;

class JsRuntime {
public:
    using ComputedStyleResolver = std::function<std::optional<ComputedStyle>(NodeId)>;

    JsRuntime(DomDocument& document, LoadOptions options, std::shared_ptr<ResourceLoader> loader);
    ~JsRuntime();

    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    void install();
    void execute(std::string_view script, std::string_view filename = "<eval>");
    void execute_module(std::string_view script, std::string_view filename);
    std::string evaluate(std::string_view script, std::string_view filename = "<eval>");
    void run_until_idle();

    DomDocument& document();
    ResourceResponse load_resource(std::string_view url, std::string_view kind);
    void log_console(std::string_view severity, std::string_view message);
    bool is_timed_out() const;

    // Injected by Page::Impl so getComputedStyle() can read back litehtml's
    // cascade without JsRuntime depending on Page directly.
    void set_computed_style_resolver(ComputedStyleResolver resolver);
    std::optional<ComputedStyle> computed_style(NodeId node);

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    DomDocument* document_ = nullptr;
    LoadOptions options_;
    std::shared_ptr<ResourceLoader> loader_;
    ComputedStyleResolver computed_style_resolver_;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_active_ = false;

    static char* normalize_module(JSContext* ctx, const char* module_base_name, const char* module_name, void* opaque);
    static JSModuleDef* load_module(JSContext* ctx, const char* module_name, void* opaque);

    void start_deadline();
    void clear_deadline();
    void drain_jobs();
    void check_exception(JSValue value, std::string_view source_name = {});
};

} // namespace pagecore

#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"

#include <chrono>
#include <memory>
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

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    DomDocument* document_ = nullptr;
    LoadOptions options_;
    std::shared_ptr<ResourceLoader> loader_;
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

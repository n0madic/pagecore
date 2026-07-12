#include "js_runtime.hpp"

#include "async_loader.hpp"
#include "base64_codec.hpp"
#include "cookie_jar.hpp"
#include "curl_async_loader.hpp"
#include "dom_shim.hpp"
#include "pagecore/resource_loader.hpp"

extern "C" {
#include <quickjs.h>
}

#include <lexbor/unicode/idna.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pagecore {
namespace {

JsRuntime* engine(JSContext* ctx)
{
    return static_cast<JsRuntime*>(JS_GetContextOpaque(ctx));
}

NodeId to_node_id(JSContext* ctx, JSValueConst value)
{
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, value) < 0) {
        throw std::runtime_error("expected numeric node id");
    }
    return id;
}

NodeId optional_to_node_id(JSContext* ctx, JSValueConst value)
{
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        return kInvalidNodeId;
    }
    return to_node_id(ctx, value);
}

std::string to_string(JSContext* ctx, JSValueConst value)
{
    size_t len = 0;
    const char* data = JS_ToCStringLen(ctx, &len, value);
    if (data == nullptr) {
        throw std::runtime_error("expected string");
    }
    std::string out(data, len);
    JS_FreeCString(ctx, data);
    return out;
}

std::vector<std::pair<std::string, std::string>> headers_from_js_pairs(JSContext* ctx, JSValueConst value)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        return out;
    }

    JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
    uint32_t length = 0;
    if (JS_ToUint32(ctx, &length, length_value) < 0) {
        JS_FreeValue(ctx, length_value);
        throw std::runtime_error("expected headers array");
    }
    JS_FreeValue(ctx, length_value);

    out.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        JSValue pair = JS_GetPropertyUint32(ctx, value, i);
        JSValue name = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue header_value = JS_GetPropertyUint32(ctx, pair, 1);
        try {
            out.emplace_back(to_string(ctx, name), to_string(ctx, header_value));
        } catch (...) {
            JS_FreeValue(ctx, header_value);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, pair);
            throw;
        }
        JS_FreeValue(ctx, header_value);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, pair);
    }
    return out;
}

CookieCredentials credentials_from_string(std::string_view value)
{
    if (value == "omit") {
        return CookieCredentials::Omit;
    }
    if (value == "include") {
        return CookieCredentials::Include;
    }
    return CookieCredentials::SameOrigin;
}

JSValue js_string(JSContext* ctx, const std::string& value)
{
    return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue js_header_pairs(JSContext* ctx, const std::vector<std::pair<std::string, std::string>>& headers)
{
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < headers.size(); ++i) {
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, js_string(ctx, headers[i].first));
        JS_SetPropertyUint32(ctx, pair, 1, js_string(ctx, headers[i].second));
        JS_SetPropertyUint32(ctx, array, i, pair);
    }
    return array;
}

std::string latin1_bytes_from_js_string(JSContext* ctx, JSValueConst value)
{
    size_t len = 0;
    const char* data = JS_ToCStringLen(ctx, &len, value);
    if (data == nullptr) {
        throw std::runtime_error("expected string");
    }

    std::string out;
    try {
        out.reserve(len);
        for (std::size_t i = 0; i < len;) {
            const auto first = static_cast<unsigned char>(data[i]);
            std::uint32_t codepoint = 0;
            std::size_t width = 0;

            if (first < 0x80) {
                codepoint = first;
                width = 1;
            } else if ((first & 0xe0) == 0xc0) {
                if (i + 1 >= len) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                const auto second = static_cast<unsigned char>(data[i + 1]);
                if ((second & 0xc0) != 0x80) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                codepoint = ((first & 0x1f) << 6) | (second & 0x3f);
                width = 2;
            } else if ((first & 0xf0) == 0xe0) {
                if (i + 2 >= len) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                const auto second = static_cast<unsigned char>(data[i + 1]);
                const auto third = static_cast<unsigned char>(data[i + 2]);
                if ((second & 0xc0) != 0x80 || (third & 0xc0) != 0x80) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                codepoint = ((first & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f);
                width = 3;
            } else if ((first & 0xf8) == 0xf0) {
                if (i + 3 >= len) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                const auto second = static_cast<unsigned char>(data[i + 1]);
                const auto third = static_cast<unsigned char>(data[i + 2]);
                const auto fourth = static_cast<unsigned char>(data[i + 3]);
                if ((second & 0xc0) != 0x80 || (third & 0xc0) != 0x80 || (fourth & 0xc0) != 0x80) {
                    throw std::runtime_error("invalid utf-8 string");
                }
                codepoint =
                    ((first & 0x07) << 18) | ((second & 0x3f) << 12) | ((third & 0x3f) << 6) | (fourth & 0x3f);
                width = 4;
            } else {
                throw std::runtime_error("invalid utf-8 string");
            }

            if (codepoint > 0xff) {
                throw std::runtime_error("string contains characters outside of the Latin1 range");
            }
            out.push_back(static_cast<char>(codepoint));
            i += width;
        }
    } catch (...) {
        JS_FreeCString(ctx, data);
        throw;
    }

    JS_FreeCString(ctx, data);
    return out;
}

JSValue js_latin1_string(JSContext* ctx, std::string_view bytes)
{
    std::string utf8;
    utf8.reserve(bytes.size());
    for (unsigned char byte : bytes) {
        if (byte < 0x80) {
            utf8.push_back(static_cast<char>(byte));
        } else {
            utf8.push_back(static_cast<char>(0xc0 | (byte >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (byte & 0x3f)));
        }
    }
    return JS_NewStringLen(ctx, utf8.data(), utf8.size());
}

JSValue js_id(JSContext* ctx, NodeId id)
{
    return JS_NewUint32(ctx, id);
}

JSValue js_ids(JSContext* ctx, const std::vector<NodeId>& ids)
{
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < ids.size(); ++i) {
        JS_SetPropertyUint32(ctx, array, i, js_id(ctx, ids[i]));
    }
    return array;
}

JSValue js_attributes(JSContext* ctx, const std::vector<DomDocument::Attribute>& attributes)
{
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < attributes.size(); ++i) {
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "name", js_string(ctx, attributes[i].name));
        JS_SetPropertyStr(ctx, item, "value", js_string(ctx, attributes[i].value));
        JS_SetPropertyUint32(ctx, array, i, item);
    }
    return array;
}

// A node "descriptor": everything the JS wrapper layer needs to construct a
// wrapper (type, and tag for elements) in a single bridge crossing instead of a
// hasNode + nodeType + tagName round-trip per node.
JSValue js_node_descriptor(JSContext* ctx, DomDocument& document, NodeId id)
{
    // Read everything that can throw (require_node/require_element) BEFORE
    // allocating the JS object, so an exception cannot leak a half-built object.
    const int type = document.node_type(id);
    std::string tag;
    if (type == 1) {
        tag = document.tag_name(id);
    }
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "id", js_id(ctx, id));
    JS_SetPropertyStr(ctx, item, "type", JS_NewInt32(ctx, type));
    if (type == 1) {
        JS_SetPropertyStr(ctx, item, "tag", js_string(ctx, tag));
    }
    return item;
}

JSValue js_node_descriptors(JSContext* ctx, DomDocument& document, const std::vector<NodeId>& ids)
{
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < ids.size(); ++i) {
        JS_SetPropertyUint32(ctx, array, i, js_node_descriptor(ctx, document, ids[i]));
    }
    return array;
}

template <typename Func>
JSValue bridge_call(JSContext* ctx, Func&& func)
{
    try {
        return func(*engine(ctx));
    } catch (const std::exception& error) {
        return JS_ThrowInternalError(ctx, "%s", error.what());
    } catch (...) {
        // Never let a non-std exception unwind through QuickJS's C frames.
        return JS_ThrowInternalError(ctx, "native exception");
    }
}

long long elapsed_us_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

std::string lower_ascii(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string network_origin(std::string_view url)
{
    const auto scheme_pos = url.find("://");
    if (scheme_pos == std::string_view::npos) {
        return {};
    }

    std::string scheme = lower_ascii(url.substr(0, scheme_pos));
    if (scheme != "http" && scheme != "https") {
        return {};
    }

    std::string_view authority = url.substr(scheme_pos + 3);
    const auto authority_end = authority.find_first_of("/?#");
    if (authority_end != std::string_view::npos) {
        authority = authority.substr(0, authority_end);
    }
    const auto at = authority.find_last_of('@');
    if (at != std::string_view::npos) {
        authority = authority.substr(at + 1);
    }

    std::string host;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        host = lower_ascii(close == std::string_view::npos ? authority.substr(1) : authority.substr(1, close - 1));
        if (close != std::string_view::npos && close + 1 < authority.size() && authority[close + 1] == ':') {
            port = std::string(authority.substr(close + 2));
        }
    } else {
        const auto colon = authority.find(':');
        host = lower_ascii(colon == std::string_view::npos ? authority : authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            port = std::string(authority.substr(colon + 1));
        }
    }

    if (host.empty()) {
        return {};
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

bool js_resource_load_blocked(
    JsResourceLoadPolicy policy,
    std::string_view base_url,
    std::string_view request_url)
{
    switch (policy) {
    case JsResourceLoadPolicy::Allow:
        return false;
    case JsResourceLoadPolicy::BlockAll:
        return true;
    case JsResourceLoadPolicy::SameOriginOnly: {
        const std::string request_origin = network_origin(request_url);
        // A non-network target (data:) has no network origin and is not a
        // cross-origin network fetch; leave it to the ResourceLoader policy, which
        // separately gates file:// reads from a network origin.
        if (request_origin.empty()) {
            return false;
        }
        // Fail closed: an http(s) target from a document with no comparable network
        // origin (a data:/file: or empty base_url) cannot be proven same-origin, so
        // it must be blocked rather than silently degrading to Allow.
        const std::string base_origin = network_origin(base_url);
        return base_origin.empty() || base_origin != request_origin;
    }
    }
    return false;
}

std::string js_resource_policy_name(JsResourceLoadPolicy policy)
{
    switch (policy) {
    case JsResourceLoadPolicy::Allow:
        return "allow";
    case JsResourceLoadPolicy::SameOriginOnly:
        return "same-origin";
    case JsResourceLoadPolicy::BlockAll:
        return "none";
    }
    return "unknown";
}

PerfEvent js_resource_blocked_event(
    const ResourceRequest& request,
    std::string reason)
{
    PerfEvent event{PerfPhase::ResourceLoad, "js_load_resource_blocked", 0, 0};
    event.property = resource_kind_name(request.kind);
    event.url = request.url;
    event.reason = std::move(reason);
    return event;
}

template <typename Func>
JSValue timed_bridge_call(JSContext* ctx, std::string_view name, Func&& func)
{
    return bridge_call(ctx, [name, func = std::forward<Func>(func)](JsRuntime& js) mutable {
        const auto start = std::chrono::steady_clock::now();
        JSValue result = func(js);
        js.record_dom_bridge_perf(name, elapsed_us_since(start));
        return result;
    });
}

void set_function(JSContext* ctx, JSValueConst object, const char* name, JSCFunction* function, int argc)
{
    JS_SetPropertyStr(ctx, object, name, JS_NewCFunction(ctx, function, name, argc));
}

int interrupt_handler(JSRuntime*, void* opaque)
{
    return static_cast<JsRuntime*>(opaque)->is_timed_out() ? 1 : 0;
}

// Surfaces unhandled promise rejections as console errors. Without this, a module
// that throws at top level (JS_EvalFunction returns a rejected promise, not an
// exception), a rejected async timer callback, and a dropped fetch()/XHR chain all
// vanish silently — unlike classic scripts, whose errors are logged.
void promise_rejection_tracker(
    JSContext* ctx, JSValueConst /*promise*/, JSValueConst reason, bool is_handled, void* opaque)
{
    if (is_handled) {
        return; // A previously unhandled rejection just gained a handler.
    }
    auto* runtime = static_cast<JsRuntime*>(opaque);
    if (runtime == nullptr) {
        return;
    }
    const char* reason_str = JS_ToCString(ctx, reason);
    std::string message = reason_str != nullptr ? reason_str : "unknown reason";
    if (reason_str != nullptr) {
        JS_FreeCString(ctx, reason_str);
    }
    if (JS_IsObject(reason)) {
        JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
        if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str != nullptr && stack_str[0] != '\0') {
                message += "\n";
                message += stack_str;
            }
            if (stack_str != nullptr) {
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }
    // A throwing toString/valueOf on `reason` (JS_ToCString above) or a throwing
    // `stack` getter leaves a pending exception on the context. This host hook runs
    // during job draining and must not leak that exception onto an unrelated later
    // operation, so clear it (JS_GetException returns JS_NULL when none is pending).
    JS_FreeValue(ctx, JS_GetException(ctx));
    runtime->log_console("error", "Uncaught (in promise) " + message);
}

ResourceKind resource_kind_from_string(std::string_view value)
{
    if (value == "script") return ResourceKind::Script;
    if (value == "stylesheet") return ResourceKind::Stylesheet;
    if (value == "image") return ResourceKind::Image;
    if (value == "font") return ResourceKind::Font;
    if (value == "document") return ResourceKind::Document;
    return ResourceKind::Other;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

bool has_url_scheme(std::string_view value)
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    const auto first_delimiter = value.find_first_of("/?#");
    return first_delimiter == std::string_view::npos || colon < first_delimiter;
}

std::string without_query_or_fragment(std::string_view value)
{
    const auto suffix = value.find_first_of("?#");
    return std::string(suffix == std::string_view::npos ? value : value.substr(0, suffix));
}

std::string url_origin(std::string_view value)
{
    const auto scheme = value.find("://");
    if (scheme == std::string_view::npos) {
        return {};
    }

    const auto authority_start = scheme + 3;
    auto authority_end = value.find('/', authority_start);
    const auto suffix = value.find_first_of("?#", authority_start);
    if (authority_end == std::string_view::npos || (suffix != std::string_view::npos && suffix < authority_end)) {
        authority_end = suffix;
    }
    if (authority_end == std::string_view::npos) {
        return std::string(value);
    }
    return std::string(value.substr(0, authority_end));
}

std::string url_directory(std::string_view value)
{
    const std::string clean = without_query_or_fragment(value);
    const auto origin = url_origin(clean);
    const auto slash = clean.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    if (!origin.empty() && slash < origin.size()) {
        return origin + "/";
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

std::string resolve_module_specifier(
    std::string_view module_base_name,
    std::string_view module_name,
    std::string_view fallback_base_url)
{
    if (has_url_scheme(module_name)) {
        return normalize_url_path(std::string(module_name));
    }

    std::string base(module_base_name);
    if (base.empty() || base.front() == '<') {
        base = std::string(fallback_base_url);
    }

    if (starts_with(module_name, "//")) {
        const auto scheme = base.find(':');
        if (scheme == std::string::npos) {
            return std::string(module_name);
        }
        return normalize_url_path(base.substr(0, scheme) + ":" + std::string(module_name));
    }

    if (starts_with(module_name, "/")) {
        const std::string origin = url_origin(base);
        return origin.empty()
            ? normalize_url_path(std::string(module_name))
            : normalize_url_path(origin + std::string(module_name));
    }

    if (starts_with(module_name, "./") || starts_with(module_name, "../")) {
        return normalize_url_path(url_directory(base) + std::string(module_name));
    }

    return std::string(module_name);
}

char* js_strdup_from_string(JSContext* ctx, const std::string& value)
{
    auto* out = static_cast<char*>(js_malloc(ctx, value.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

int set_import_meta(JSContext* ctx, JSValueConst module_value, bool is_main)
{
    if (!JS_IsModule(module_value)) {
        return 0;
    }

    auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module_value));
    JSAtom module_name_atom = JS_GetModuleName(ctx, module);
    const char* module_name = JS_AtomToCString(ctx, module_name_atom);
    JS_FreeAtom(ctx, module_name_atom);
    if (module_name == nullptr) {
        return -1;
    }

    JSValue meta = JS_GetImportMeta(ctx, module);
    if (JS_IsException(meta)) {
        JS_FreeCString(ctx, module_name);
        return -1;
    }

    const int url_status = JS_DefinePropertyValueStr(
        ctx,
        meta,
        "url",
        JS_NewString(ctx, module_name),
        JS_PROP_C_W_E);
    const int main_status = JS_DefinePropertyValueStr(
        ctx,
        meta,
        "main",
        JS_NewBool(ctx, is_main),
        JS_PROP_C_W_E);

    JS_FreeValue(ctx, meta);
    JS_FreeCString(ctx, module_name);
    return url_status < 0 || main_status < 0 ? -1 : 0;
}

} // namespace

JSValue bridge_document_node(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) { return js_id(ctx, js.document().document_node()); });
}

JSValue bridge_document_element(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) { return js_id(ctx, js.document().document_element()); });
}

JSValue bridge_head(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) { return js_id(ctx, js.document().head()); });
}

JSValue bridge_body(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) { return js_id(ctx, js.document().body()); });
}

JSValue bridge_create_element(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("createElement requires tag name");
        return js_id(ctx, js.document().create_element(to_string(ctx, argv[0])));
    });
}

JSValue bridge_create_text_node(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("createTextNode requires text");
        return js_id(ctx, js.document().create_text_node(to_string(ctx, argv[0])));
    });
}

JSValue bridge_create_comment(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("createComment requires text");
        return js_id(ctx, js.document().create_comment(to_string(ctx, argv[0])));
    });
}

JSValue bridge_create_processing_instruction(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("createProcessingInstruction requires target and data");
        return js_id(ctx, js.document().create_processing_instruction(to_string(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_create_cdata_section(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("createCDATASection requires data");
        return js_id(ctx, js.document().create_cdata_section(to_string(ctx, argv[0])));
    });
}

JSValue bridge_create_document_type(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("createDocumentType requires name, publicId, systemId");
        return js_id(ctx, js.document().create_document_type(
            to_string(ctx, argv[0]), to_string(ctx, argv[1]), to_string(ctx, argv[2])));
    });
}

JSValue bridge_doctype_public_id(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("doctypePublicId requires a node id");
        return js_string(ctx, js.document().doctype_public_id(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_doctype_system_id(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("doctypeSystemId requires a node id");
        return js_string(ctx, js.document().doctype_system_id(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_attach_shadow_root(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("attachShadowRoot requires host node id");
        return js_id(ctx, js.document().attach_shadow_root(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_node_type(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("nodeType requires node id");
        return JS_NewInt32(ctx, js.document().node_type(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_node_name(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("nodeName requires node id");
        return js_string(ctx, js.document().node_name(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_tag_name(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("tagName requires node id");
        return js_string(ctx, js.document().tag_name(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_parent_node(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("parentNode requires node id");
        return js_id(ctx, js.document().parent_node(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_child_nodes(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("childNodes requires node id");
        return js_ids(ctx, js.document().child_nodes(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_children(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("children requires node id");
        return js_ids(ctx, js.document().children(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_describe_node(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "describeNode", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("describeNode requires node id");
        const NodeId id = to_node_id(ctx, argv[0]);
        if (!js.document().has_node(id)) {
            return JS_NULL;
        }
        return js_node_descriptor(ctx, js.document(), id);
    });
}

JSValue bridge_child_nodes_described(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "childNodesDescribed", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("childNodesDescribed requires node id");
        return js_node_descriptors(ctx, js.document(), js.document().child_nodes(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_children_described(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "childrenDescribed", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("childrenDescribed requires node id");
        return js_node_descriptors(ctx, js.document(), js.document().children(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_has_node(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("hasNode requires node id");
        return JS_NewBool(ctx, js.document().has_node(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_contains(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("contains requires root and candidate ids");
        return JS_NewBool(ctx, js.document().contains(to_node_id(ctx, argv[0]), to_node_id(ctx, argv[1])));
    });
}

JSValue bridge_is_connected(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("isConnected requires node id");
        return JS_NewBool(ctx, js.document().is_connected(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_mutation_version(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        return JS_NewFloat64(ctx, static_cast<double>(js.document().mutation_version()));
    });
}

JSValue bridge_layout_mutation_version(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        return JS_NewFloat64(ctx, static_cast<double>(js.document().layout_mutation_version()));
    });
}

JSValue bridge_forget_version(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        return JS_NewFloat64(ctx, static_cast<double>(js.document().forget_version()));
    });
}

JSValue bridge_compat_mode(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        const auto mode = js.document().quirks_mode();
        return js_string(ctx, mode == DomDocument::QuirksMode::Quirks ? "BackCompat" : "CSS1Compat");
    });
}

JSValue bridge_computed_style(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("computedStyle requires a node id");
        auto style = js.computed_style(to_node_id(ctx, argv[0]));

        JSValue result = JS_NewObject(ctx);
        if (style) {
            for (const auto& [name, value] : style->properties) {
                JS_SetPropertyStr(ctx, result, name.c_str(), js_string(ctx, value));
            }
        }
        return result;
    });
}

JSValue bridge_computed_style_property(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("computedStyleProperty requires a node id and property name");
        auto value = js.computed_style_property(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return value ? js_string(ctx, *value) : js_string(ctx, "");
    });
}

JSValue bridge_element_geometry(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("elementGeometry requires a node id");
        auto geometry = js.element_geometry(to_node_id(ctx, argv[0]));
        if (!geometry) {
            return JS_NULL;
        }

        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "borderX", JS_NewFloat64(ctx, geometry->border_box.x));
        JS_SetPropertyStr(ctx, result, "borderY", JS_NewFloat64(ctx, geometry->border_box.y));
        JS_SetPropertyStr(ctx, result, "borderWidth", JS_NewFloat64(ctx, geometry->border_box.width));
        JS_SetPropertyStr(ctx, result, "borderHeight", JS_NewFloat64(ctx, geometry->border_box.height));
        JS_SetPropertyStr(ctx, result, "paddingX", JS_NewFloat64(ctx, geometry->padding_box.x));
        JS_SetPropertyStr(ctx, result, "paddingY", JS_NewFloat64(ctx, geometry->padding_box.y));
        JS_SetPropertyStr(ctx, result, "paddingWidth", JS_NewFloat64(ctx, geometry->padding_box.width));
        JS_SetPropertyStr(ctx, result, "paddingHeight", JS_NewFloat64(ctx, geometry->padding_box.height));
        return result;
    });
}

JSValue bridge_elements_at_point(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("elementsAtPoint requires x, y, and topmostOnly");
        double x = 0;
        double y = 0;
        if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0) {
            throw std::runtime_error("elementsAtPoint requires numeric x, y");
        }
        const bool topmost_only = JS_ToBool(ctx, argv[2]);
        auto ids = js.elements_at_point(static_cast<float>(x), static_cast<float>(y), topmost_only);
        return js_ids(ctx, ids);
    });
}

JSValue bridge_viewport(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        const Viewport viewport = js.viewport();
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, viewport.width));
        JS_SetPropertyStr(ctx, result, "height", JS_NewFloat64(ctx, viewport.height));
        JS_SetPropertyStr(ctx, result, "deviceScaleFactor", JS_NewFloat64(ctx, viewport.device_scale_factor));
        return result;
    });
}

JSValue bridge_get_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "getAttribute", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("getAttribute requires node id and name");
        auto value = js.document().get_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return value ? js_string(ctx, *value) : JS_NULL;
    });
}

JSValue bridge_has_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "hasAttribute", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("hasAttribute requires node id and name");
        return JS_NewBool(ctx, js.document().has_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_attributes(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "attributes", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("attributes requires node id");
        return js_attributes(ctx, js.document().attributes(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_set_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "setAttribute", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("setAttribute requires node id, name and value");
        js.document().set_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]), to_string(ctx, argv[2]));
        return JS_UNDEFINED;
    });
}

JSValue bridge_remove_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "removeAttribute", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("removeAttribute requires node id and name");
        js.document().remove_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return JS_UNDEFINED;
    });
}

JSValue bridge_text_content(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("textContent requires node id");
        return js_string(ctx, js.document().text_content(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_set_text_content(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("setTextContent requires node id and value");
        js.document().set_text_content(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return JS_UNDEFINED;
    });
}

JSValue bridge_inner_html(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "innerHTML", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("innerHTML requires node id");
        return js_string(ctx, js.document().inner_html(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_set_inner_html(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "setInnerHTML", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("setInnerHTML requires node id and html");
        js.document().set_inner_html(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return JS_UNDEFINED;
    });
}

JSValue bridge_outer_html(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("outerHTML requires node id");
        return js_string(ctx, js.document().outer_html(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_append_child(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "appendChild", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("appendChild requires parent and child ids");
        return js_id(ctx, js.document().append_child(to_node_id(ctx, argv[0]), to_node_id(ctx, argv[1])));
    });
}

JSValue bridge_insert_before(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "insertBefore", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("insertBefore requires parent, child and reference ids");
        return js_id(
            ctx,
            js.document().insert_before(
                to_node_id(ctx, argv[0]),
                to_node_id(ctx, argv[1]),
                optional_to_node_id(ctx, argv[2])));
    });
}

JSValue bridge_remove_child(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "removeChild", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("removeChild requires parent and child ids");
        return js_id(ctx, js.document().remove_child(to_node_id(ctx, argv[0]), to_node_id(ctx, argv[1])));
    });
}

JSValue bridge_replace_child(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "replaceChild", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("replaceChild requires parent, child and replaced child ids");
        return js_id(
            ctx,
            js.document().replace_child(
                to_node_id(ctx, argv[0]),
                to_node_id(ctx, argv[1]),
                to_node_id(ctx, argv[2])));
    });
}

JSValue bridge_clone_node(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("cloneNode requires node id");
        return js_id(ctx, js.document().clone_node(to_node_id(ctx, argv[0]), argc > 1 && JS_ToBool(ctx, argv[1])));
    });
}

JSValue bridge_query_selector(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "querySelector", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("querySelector requires root id and selector");
        return js_id(ctx, js.document().query_selector(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_query_selector_all(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "querySelectorAll", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("querySelectorAll requires root id and selector");
        return js_ids(ctx, js.document().query_selector_all(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_get_element_by_id(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "getElementById", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("getElementById requires id");
        return js_id(ctx, js.document().get_element_by_id(to_string(ctx, argv[0])));
    });
}

JSValue host_log(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        std::string severity = argc > 0 ? to_string(ctx, argv[0]) : "log";
        std::string message;
        for (int i = 1; i < argc; ++i) {
            const char* value = JS_ToCString(ctx, argv[i]);
            if (!message.empty()) {
                message.push_back(' ');
            }
            message += value == nullptr ? "<unprintable>" : value;
            if (value != nullptr) {
                JS_FreeCString(ctx, value);
            } else {
                // Stringification threw; clear the pending exception so it does
                // not leak out of this logging helper.
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
        }
        js.log_console(severity, message);
        return JS_UNDEFINED;
    });
}

std::uint64_t task_id_from_js(JSContext* ctx, JSValueConst value)
{
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, value) < 0 || id < 0) {
        throw std::runtime_error("expected non-negative numeric task id");
    }
    return static_cast<std::uint64_t>(id);
}

// The host-visible shape of a completed resource load, shared by the blocking
// loadResource binding and the async completion path.
JSValue js_resource_response_object(JSContext* ctx, const ResourceResponse& response)
{
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "url", js_string(ctx, response.url));
    JS_SetPropertyStr(ctx, out, "body", js_string(ctx, response.body));
    JS_SetPropertyStr(ctx, out, "mimeType", js_string(ctx, response.mime_type));
    JS_SetPropertyStr(ctx, out, "kind", js_string(ctx, resource_kind_name(response.kind)));
    JS_SetPropertyStr(ctx, out, "fromCache", JS_NewBool(ctx, response.from_cache));
    JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, response.status));
    JS_SetPropertyStr(ctx, out, "statusText", js_string(ctx, response.status_text));
    JS_SetPropertyStr(ctx, out, "headers", js_header_pairs(ctx, response.headers));
    JS_SetPropertyStr(ctx, out, "redirectCount", JS_NewInt32(ctx, response.redirect_count));
    return out;
}

// Parses the (url, kind, method, body, headers, credentials, referrer) tail
// shared by loadResource and startResourceLoad, starting at argv[offset].
struct JsResourceLoadArgs {
    std::string url;
    std::string kind = "other";
    std::string method = "GET";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string credentials = "same-origin";
    std::string referrer = "about:client";
};

JsResourceLoadArgs resource_load_args(JSContext* ctx, int argc, JSValue* argv, int offset)
{
    JsResourceLoadArgs args;
    if (argc <= offset) throw std::runtime_error("resource load requires url");
    args.url = to_string(ctx, argv[offset]);
    if (argc > offset + 1 && !JS_IsNull(argv[offset + 1]) && !JS_IsUndefined(argv[offset + 1])) {
        args.kind = to_string(ctx, argv[offset + 1]);
    }
    if (argc > offset + 2 && !JS_IsNull(argv[offset + 2]) && !JS_IsUndefined(argv[offset + 2])) {
        args.method = to_string(ctx, argv[offset + 2]);
    }
    if (argc > offset + 3 && !JS_IsNull(argv[offset + 3]) && !JS_IsUndefined(argv[offset + 3])) {
        args.body = to_string(ctx, argv[offset + 3]);
    }
    if (argc > offset + 4) {
        args.headers = headers_from_js_pairs(ctx, argv[offset + 4]);
    }
    if (argc > offset + 5 && !JS_IsNull(argv[offset + 5]) && !JS_IsUndefined(argv[offset + 5])) {
        args.credentials = to_string(ctx, argv[offset + 5]);
    }
    if (argc > offset + 6 && !JS_IsNull(argv[offset + 6]) && !JS_IsUndefined(argv[offset + 6])) {
        args.referrer = to_string(ctx, argv[offset + 6]);
    }
    return args;
}

JSValue host_load_resource(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "loadResource", [ctx, argc, argv](JsRuntime& js) {
        JsResourceLoadArgs args = resource_load_args(ctx, argc, argv, 0);
        const auto response =
            js.load_resource(
                args.url,
                args.kind,
                std::move(args.method),
                std::move(args.body),
                std::move(args.headers),
                std::move(args.credentials),
                std::move(args.referrer));
        return js_resource_response_object(ctx, response);
    });
}

JSValue host_start_resource_load(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return timed_bridge_call(ctx, "startResourceLoad", [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("startResourceLoad requires id and url");
        const std::uint64_t js_id = task_id_from_js(ctx, argv[0]);
        JsResourceLoadArgs args = resource_load_args(ctx, argc, argv, 1);
        js.start_resource_load(
            js_id,
            args.url,
            args.kind,
            std::move(args.method),
            std::move(args.body),
            std::move(args.headers),
            std::move(args.credentials),
            std::move(args.referrer));
        return JS_UNDEFINED;
    });
}

JSValue host_cancel_resource_load(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("cancelResourceLoad requires id");
        js.cancel_resource_load(task_id_from_js(ctx, argv[0]));
        return JS_UNDEFINED;
    });
}

// Maps a shim task kind onto its HTML task source and readiness relevance.
// Mirrors the shim's isReadinessRelevantKind predicate: timers and the
// network-ish kinds gate readiness, everything else is misc background work.
void task_kind_traits(std::string_view kind, TaskSource& source, bool& relevant)
{
    if (kind == "timer") {
        source = TaskSource::Timers;
        relevant = true;
    } else if (kind == "xhr-fetch" || kind == "dynamic-script" || kind == "dom-resource") {
        source = TaskSource::Networking;
        relevant = true;
    } else {
        source = TaskSource::Misc;
        relevant = false;
    }
}

JSValue host_now(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        return JS_NewFloat64(ctx, js.event_loop().now_ms());
    });
}

JSValue host_schedule_timer(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("scheduleTimer requires id and delay");
        const std::uint64_t id = task_id_from_js(ctx, argv[0]);
        double delay = 0;
        if (JS_ToFloat64(ctx, &delay, argv[1]) < 0) {
            throw std::runtime_error("scheduleTimer delay must be numeric");
        }
        // Clamp BEFORE the double->uint64 cast: casting a value that does not
        // fit (setTimeout(f, 1e30), Infinity, NaN) is undefined behavior. The
        // ceiling matches the browser convention of INT32_MAX milliseconds.
        constexpr double kMaxTimerDelayMs = 2147483647.0;
        if (!(delay > 0)) {
            delay = 0;
        } else if (delay > kMaxTimerDelayMs) {
            delay = kMaxTimerDelayMs;
        }
        const bool repeat = argc > 2 && JS_ToBool(ctx, argv[2]);
        const bool relevant = argc > 3 && JS_ToBool(ctx, argv[3]);
        js.event_loop().schedule_timer(id, static_cast<std::uint64_t>(delay), repeat, relevant);
        return JS_UNDEFINED;
    });
}

JSValue host_cancel_timer(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("cancelTimer requires id");
        js.event_loop().cancel_timer(task_id_from_js(ctx, argv[0]));
        return JS_UNDEFINED;
    });
}

JSValue host_queue_task(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("queueTask requires id");
        const std::uint64_t id = task_id_from_js(ctx, argv[0]);
        const std::string kind = argc > 1 ? to_string(ctx, argv[1]) : "other";
        TaskSource source = TaskSource::Misc;
        bool relevant = false;
        task_kind_traits(kind, source, relevant);
        JsRuntime* runtime = &js;
        js.event_loop().post(source, [runtime, id] { runtime->run_queued_task(id); }, relevant);
        return JS_UNDEFINED;
    });
}

JSValue host_request_animation_frame(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("requestAnimationFrame requires id");
        js.event_loop().request_animation_frame(task_id_from_js(ctx, argv[0]));
        return JS_UNDEFINED;
    });
}

JSValue host_cancel_animation_frame(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("cancelAnimationFrame requires id");
        js.event_loop().cancel_animation_frame(task_id_from_js(ctx, argv[0]));
        return JS_UNDEFINED;
    });
}

JSValue host_get_cookie_string(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        const std::string url = argc > 0 ? to_string(ctx, argv[0]) : std::string{};
        return js_string(ctx, js.document_cookie(url));
    });
}

lxb_status_t idna_to_ascii_collect(const lxb_char_t* data, size_t length, void* ctx)
{
    static_cast<std::string*>(ctx)->assign(reinterpret_cast<const char*>(data), length);
    return LXB_STATUS_OK;
}

// WHATWG URL "domain to ASCII": mapping, NFC normalization, Punycode encoding,
// and UTS46 validity checks, always with beStrict=false (the URL Standard
// never invokes the strict/STD3 variant from host parsing). Mirrors the exact
// flags lexbor's own URL parser uses, so this stays consistent with any
// future native URL parsing built on the same library.
JSValue host_domain_to_ascii(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime&) {
        if (argc < 1) throw std::runtime_error("domainToAscii requires input");
        const std::string input = to_string(ctx, argv[0]);

        lxb_unicode_idna_t* idna = lxb_unicode_idna_create();
        if (idna == nullptr) {
            throw std::runtime_error("domainToAscii: failed to allocate IDNA processor");
        }
        if (lxb_unicode_idna_init(idna) != LXB_STATUS_OK) {
            lxb_unicode_idna_destroy(idna, true);
            throw std::runtime_error("domainToAscii: failed to initialize IDNA processor");
        }

        std::string result;
        const lxb_status_t status = lxb_unicode_idna_to_ascii(
            idna, reinterpret_cast<const lxb_char_t*>(input.data()), input.size(),
            idna_to_ascii_collect, &result,
            static_cast<lxb_unicode_idna_flag_t>(
                LXB_UNICODE_IDNA_FLAG_CHECK_BIDI | LXB_UNICODE_IDNA_FLAG_CHECK_JOINERS));

        lxb_unicode_idna_destroy(idna, true);

        // A processing failure (disallowed code point, bad hyphens, empty
        // label, ...) is not a host error: the JS caller reports it as a
        // TypeError from the URL constructor, per spec.
        if (status != LXB_STATUS_OK) {
            return JS_NULL;
        }
        return js_string(ctx, result);
    });
}

JSValue host_text_decoder_create(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("textDecoderCreate requires label and fatal");
        const std::string label = to_string(ctx, argv[0]);
        const bool fatal = JS_ToBool(ctx, argv[1]) != 0;

        const auto handle = js.create_text_decoder(label, fatal);
        if (!handle) {
            return JS_NULL;
        }

        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "handle", JS_NewInt32(ctx, handle->id));
        JS_SetPropertyStr(ctx, result, "encoding", js_string(ctx, handle->encoding));
        return result;
    });
}

JSValue host_text_decoder_decode(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("textDecoderDecode requires handle, bytes, and isFinal");
        int32_t handle = 0;
        if (JS_ToInt32(ctx, &handle, argv[0]) < 0) {
            throw std::runtime_error("textDecoderDecode: invalid handle");
        }
        size_t byte_length = 0;
        uint8_t* bytes = JS_GetUint8Array(ctx, &byte_length, argv[1]);
        if (bytes == nullptr && byte_length != 0) {
            throw std::runtime_error("textDecoderDecode requires a Uint8Array");
        }
        const bool is_final = JS_ToBool(ctx, argv[2]) != 0;

        const std::string_view view(reinterpret_cast<const char*>(bytes), byte_length);
        const auto decoded = js.decode_text_chunk(handle, view, is_final);
        if (!decoded) {
            return JS_NULL;
        }
        return js_string(ctx, *decoded);
    });
}

JSValue host_set_cookie_string(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("setCookieString requires url and cookie");
        const std::string url = to_string(ctx, argv[0]);
        const std::string cookie = to_string(ctx, argv[1]);
        js.set_document_cookie(url, cookie);
        return JS_UNDEFINED;
    });
}

JSValue host_activity_begin(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("activityBegin requires kind");
        js.activity_tracker().begin(page_activity_kind_from_string(to_string(ctx, argv[0])));
        return JS_UNDEFINED;
    });
}

JSValue host_activity_end(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("activityEnd requires kind");
        js.activity_tracker().end(page_activity_kind_from_string(to_string(ctx, argv[0])));
        return JS_UNDEFINED;
    });
}

JSValue host_activity_mark_mutation(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        int64_t version = 0;
        if (argc > 0 && JS_ToInt64(ctx, &version, argv[0]) < 0) {
            throw std::runtime_error("activityMarkMutation requires numeric mutation version");
        }
        int64_t clock = 0;
        if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
            // Propagate (don't swallow) a failed conversion: leaving the pending JS
            // exception set while returning normally violates the QuickJS contract.
            if (JS_ToInt64(ctx, &clock, argv[1]) < 0) {
                throw std::runtime_error("activityMarkMutation clock must be numeric");
            }
            js.activity_tracker().set_clock(std::chrono::milliseconds{std::max<int64_t>(0, clock)});
        }
        if (version < 0) version = 0;
        js.activity_tracker().mark_mutation(static_cast<std::uint64_t>(version));
        return JS_UNDEFINED;
    });
}

JSValue host_random_bytes(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime&) {
        int32_t count = 0;
        if (argc > 0 && JS_ToInt32(ctx, &count, argv[0]) < 0) {
            // Propagate the pending exception instead of returning an array with an
            // exception still set (QuickJS contract violation).
            throw std::runtime_error("randomBytes length must be numeric");
        }
        if (count < 0) {
            count = 0;
        }
        if (count > 65536) {
            count = 65536;
        }

        // std::random_device is backed by the OS CSPRNG (getentropy / urandom)
        // on the supported platforms, unlike Math.random().
        std::random_device device;
        JSValue array = JS_NewArray(ctx);
        int32_t produced = 0;
        while (produced < count) {
            std::uint32_t word = device();
            for (int byte = 0; byte < 4 && produced < count; ++byte) {
                const int value = static_cast<int>(word & 0xff);
                word >>= 8;
                JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(produced), JS_NewInt32(ctx, value));
                ++produced;
            }
        }
        return array;
    });
}

JSValue host_base64_encode(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime&) {
        if (argc < 1) throw std::runtime_error("base64Encode requires input");
        return js_string(ctx, base64_encode(latin1_bytes_from_js_string(ctx, argv[0])));
    });
}

JSValue host_base64_decode(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime&) {
        if (argc < 1) throw std::runtime_error("base64Decode requires input");
        return js_latin1_string(ctx, base64_decode(to_string(ctx, argv[0]), Base64DecodeMode::Forgiving));
    });
}

char* JsRuntime::normalize_module(JSContext* ctx, const char* module_base_name, const char* module_name, void* opaque)
{
    auto* js = static_cast<JsRuntime*>(opaque);
    if (module_name == nullptr) {
        JS_ThrowReferenceError(ctx, "module specifier is empty");
        return nullptr;
    }

    // This is a C callback from QuickJS's module loader; contain any C++
    // exception (e.g. std::bad_alloc) rather than unwinding through C frames.
    try {
        const std::string resolved = resolve_module_specifier(
            module_base_name == nullptr ? std::string_view() : std::string_view(module_base_name),
            module_name,
            js == nullptr ? std::string_view() : std::string_view(js->options_.base_url));
        return js_strdup_from_string(ctx, resolved);
    } catch (const std::exception& error) {
        JS_ThrowInternalError(ctx, "%s", error.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(ctx, "native exception while resolving module specifier");
        return nullptr;
    }
}

JSModuleDef* JsRuntime::load_module(JSContext* ctx, const char* module_name, void* opaque)
{
    auto* js = static_cast<JsRuntime*>(opaque);
    if (js == nullptr || module_name == nullptr) {
        JS_ThrowReferenceError(ctx, "module loader is not available");
        return nullptr;
    }

    try {
        const auto response = js->load_resource(module_name, "script");
        const std::string filename = response.url.empty() ? std::string(module_name) : response.url;
        JSValue module_value = JS_Eval(
            ctx,
            response.body.data(),
            response.body.size(),
            filename.c_str(),
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(module_value)) {
            return nullptr;
        }
        if (set_import_meta(ctx, module_value, false) < 0) {
            JS_FreeValue(ctx, module_value);
            return nullptr;
        }

        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module_value));
        JS_FreeValue(ctx, module_value);
        return module;
    } catch (const std::exception& error) {
        JS_ThrowReferenceError(ctx, "could not load module '%s': %s", module_name, error.what());
        return nullptr;
    }
}

JsRuntime::JsRuntime(
    DomDocument& document,
    LoadOptions options,
    std::shared_ptr<ResourceLoader> loader,
    CookieJar* cookie_jar,
    std::shared_ptr<ResourceLoader> raw_loader)
    : document_(&document)
    , options_(std::move(options))
    , loader_(std::move(loader))
    , cookie_jar_(cookie_jar)
{
    runtime_ = JS_NewRuntime();
    if (runtime_ == nullptr) {
        throw std::runtime_error("failed to create QuickJS runtime");
    }

    JS_SetMemoryLimit(runtime_, options_.js_memory_limit_bytes);
    JS_SetMaxStackSize(runtime_, 1024 * 1024);
    JS_SetInterruptHandler(runtime_, interrupt_handler, this);
    JS_SetModuleLoaderFunc(runtime_, normalize_module, load_module, this);
    JS_SetHostPromiseRejectionTracker(runtime_, promise_rejection_tracker, this);

    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr) {
        JS_FreeRuntime(runtime_);
        runtime_ = nullptr;
        throw std::runtime_error("failed to create QuickJS context");
    }

    JS_SetContextOpaque(context_, this);
    activity_tracker_.reset(document_->mutation_version());
    event_loop_ = std::make_unique<EventLoop>();
    // Fired timers become queued Timers-source tasks; the task carries only the
    // id, which the JS shim resolves through its registry.
    event_loop_->set_timer_callback([this](std::uint64_t id) { run_queued_task(id); });

    // Async engine selection: a curl-backed page gets the real
    // curl_multi_socket_action engine (with an explicit JS resource cache);
    // any other loader is adapted by blocking one queued task per transfer,
    // which also preserves that loader's own caching behaviour.
    if (auto curl_loader = std::dynamic_pointer_cast<CurlResourceLoader>(raw_loader)) {
        async_loader_ = std::make_unique<CurlMultiAsyncLoader>(
            *event_loop_, std::move(curl_loader), options_.user_agent);
        js_resource_cache_ = std::dynamic_pointer_cast<CachingResourceLoader>(loader_);
    } else if (loader_) {
        async_loader_ = std::make_unique<TaskQueueAsyncLoader>(*event_loop_, loader_);
    }
}

JsRuntime::~JsRuntime()
{
    // Teardown order matters: the async loader aborts in-flight transfers and
    // closes its libuv handles (draining their close callbacks) first, then
    // the event loop drops queued tasks and closes its own handles - all while
    // the JS context is still alive. Only then is anything freed.
    if (async_loader_) {
        async_loader_->shutdown();
    }
    if (event_loop_) {
        event_loop_->shutdown();
    }
    async_loader_.reset();
    event_loop_.reset();
    if (context_ != nullptr) {
        JS_FreeContext(context_);
        context_ = nullptr;
    }
    if (runtime_ != nullptr) {
        JS_FreeRuntime(runtime_);
        runtime_ = nullptr;
    }
}

void JsRuntime::install()
{
    JSValue global = JS_GetGlobalObject(context_);
    JSValue dom = JS_NewObject(context_);
    JSValue host = JS_NewObject(context_);

    set_function(context_, dom, "documentNode", bridge_document_node, 0);
    set_function(context_, dom, "documentElement", bridge_document_element, 0);
    set_function(context_, dom, "head", bridge_head, 0);
    set_function(context_, dom, "body", bridge_body, 0);
    set_function(context_, dom, "createElement", bridge_create_element, 1);
    set_function(context_, dom, "createTextNode", bridge_create_text_node, 1);
    set_function(context_, dom, "createComment", bridge_create_comment, 1);
    set_function(context_, dom, "createProcessingInstruction", bridge_create_processing_instruction, 2);
    set_function(context_, dom, "createCDATASection", bridge_create_cdata_section, 1);
    set_function(context_, dom, "createDocumentType", bridge_create_document_type, 3);
    set_function(context_, dom, "doctypePublicId", bridge_doctype_public_id, 1);
    set_function(context_, dom, "doctypeSystemId", bridge_doctype_system_id, 1);
    set_function(context_, dom, "attachShadowRoot", bridge_attach_shadow_root, 1);
    set_function(context_, dom, "nodeType", bridge_node_type, 1);
    set_function(context_, dom, "nodeName", bridge_node_name, 1);
    set_function(context_, dom, "tagName", bridge_tag_name, 1);
    set_function(context_, dom, "parentNode", bridge_parent_node, 1);
    set_function(context_, dom, "childNodes", bridge_child_nodes, 1);
    set_function(context_, dom, "children", bridge_children, 1);
    set_function(context_, dom, "describeNode", bridge_describe_node, 1);
    set_function(context_, dom, "childNodesDescribed", bridge_child_nodes_described, 1);
    set_function(context_, dom, "childrenDescribed", bridge_children_described, 1);
    set_function(context_, dom, "hasNode", bridge_has_node, 1);
    set_function(context_, dom, "contains", bridge_contains, 2);
    set_function(context_, dom, "isConnected", bridge_is_connected, 1);
    set_function(context_, dom, "mutationVersion", bridge_mutation_version, 0);
    set_function(context_, dom, "layoutMutationVersion", bridge_layout_mutation_version, 0);
    set_function(context_, dom, "forgetVersion", bridge_forget_version, 0);
    set_function(context_, dom, "compatMode", bridge_compat_mode, 0);
    set_function(context_, dom, "computedStyle", bridge_computed_style, 1);
    set_function(context_, dom, "computedStyleProperty", bridge_computed_style_property, 2);
    set_function(context_, dom, "elementGeometry", bridge_element_geometry, 1);
    set_function(context_, dom, "elementsAtPoint", bridge_elements_at_point, 3);
    set_function(context_, dom, "viewport", bridge_viewport, 0);
    set_function(context_, dom, "getAttribute", bridge_get_attribute, 2);
    set_function(context_, dom, "hasAttribute", bridge_has_attribute, 2);
    set_function(context_, dom, "attributes", bridge_attributes, 1);
    set_function(context_, dom, "setAttribute", bridge_set_attribute, 3);
    set_function(context_, dom, "removeAttribute", bridge_remove_attribute, 2);
    set_function(context_, dom, "textContent", bridge_text_content, 1);
    set_function(context_, dom, "setTextContent", bridge_set_text_content, 2);
    set_function(context_, dom, "innerHTML", bridge_inner_html, 1);
    set_function(context_, dom, "setInnerHTML", bridge_set_inner_html, 2);
    set_function(context_, dom, "outerHTML", bridge_outer_html, 1);
    set_function(context_, dom, "appendChild", bridge_append_child, 2);
    set_function(context_, dom, "insertBefore", bridge_insert_before, 3);
    set_function(context_, dom, "removeChild", bridge_remove_child, 2);
    set_function(context_, dom, "replaceChild", bridge_replace_child, 3);
    set_function(context_, dom, "cloneNode", bridge_clone_node, 2);
    set_function(context_, dom, "querySelector", bridge_query_selector, 2);
    set_function(context_, dom, "querySelectorAll", bridge_query_selector_all, 2);
    set_function(context_, dom, "getElementById", bridge_get_element_by_id, 1);
    set_function(context_, host, "log", host_log, 1);
    set_function(context_, host, "loadResource", host_load_resource, 2);
    set_function(context_, host, "startResourceLoad", host_start_resource_load, 3);
    set_function(context_, host, "cancelResourceLoad", host_cancel_resource_load, 1);
    set_function(context_, host, "now", host_now, 0);
    set_function(context_, host, "scheduleTimer", host_schedule_timer, 4);
    set_function(context_, host, "cancelTimer", host_cancel_timer, 1);
    set_function(context_, host, "queueTask", host_queue_task, 2);
    set_function(context_, host, "requestAnimationFrame", host_request_animation_frame, 1);
    set_function(context_, host, "cancelAnimationFrame", host_cancel_animation_frame, 1);
    set_function(context_, host, "getCookieString", host_get_cookie_string, 1);
    set_function(context_, host, "setCookieString", host_set_cookie_string, 2);
    set_function(context_, host, "domainToAscii", host_domain_to_ascii, 1);
    set_function(context_, host, "textDecoderCreate", host_text_decoder_create, 2);
    set_function(context_, host, "textDecoderDecode", host_text_decoder_decode, 3);
    set_function(context_, host, "activityBegin", host_activity_begin, 1);
    set_function(context_, host, "activityEnd", host_activity_end, 1);
    set_function(context_, host, "activityMarkMutation", host_activity_mark_mutation, 1);
    set_function(context_, host, "randomBytes", host_random_bytes, 1);
    set_function(context_, host, "base64Encode", host_base64_encode, 1);
    set_function(context_, host, "base64Decode", host_base64_decode, 1);
    JS_SetPropertyStr(context_, host, "baseURL", js_string(context_, options_.base_url));
    JS_SetPropertyStr(context_, host, "userAgent", js_string(context_, options_.user_agent));

    JS_SetPropertyStr(context_, global, "__dom", dom);
    JS_SetPropertyStr(context_, global, "__host", host);
    JS_FreeValue(context_, global);

    execute(kDomShim, "dom_shim.js");
}

void JsRuntime::execute(std::string_view script, std::string_view filename)
{
    const auto perf_start = std::chrono::steady_clock::now();
    start_deadline();
    try {
        const std::string filename_string(filename);
        JSValue value = JS_Eval(
            context_,
            script.data(),
            script.size(),
            filename_string.c_str(),
            JS_EVAL_TYPE_GLOBAL);
        check_exception(value, filename_string);
        JS_FreeValue(context_, value);
        run_microtask_checkpoint();
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("execute", perf_start, script.size());
    } catch (...) {
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("execute", perf_start, script.size());
        throw;
    }
}

void JsRuntime::execute_module(std::string_view script, std::string_view filename)
{
    const auto perf_start = std::chrono::steady_clock::now();
    start_deadline();
    try {
        const std::string filename_string(filename);
        JSValue module_value = JS_Eval(
            context_,
            script.data(),
            script.size(),
            filename_string.c_str(),
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        check_exception(module_value, filename_string);

        if (set_import_meta(context_, module_value, true) < 0) {
            JS_FreeValue(context_, module_value);
            check_exception(JS_EXCEPTION, filename_string);
        }

        JSValue value = JS_EvalFunction(context_, module_value);
        check_exception(value, filename_string);
        JS_FreeValue(context_, value);
        run_microtask_checkpoint();
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("execute_module", perf_start, script.size());
    } catch (...) {
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("execute_module", perf_start, script.size());
        throw;
    }
}

std::string JsRuntime::evaluate(std::string_view script, std::string_view filename)
{
    const auto perf_start = std::chrono::steady_clock::now();
    start_deadline();
    const std::string filename_string(filename);
    JSValue value = JS_Eval(
        context_,
        script.data(),
        script.size(),
        filename_string.c_str(),
        JS_EVAL_TYPE_GLOBAL);

    // Guard both the exception check and job draining: on either throw path we must
    // still clear the deadline and flush the perf events (matching execute()), and
    // free the value if it is live. When check_exception throws, `value` is the
    // JS_EXCEPTION sentinel and JS_FreeValue on it is a safe no-op.
    try {
        check_exception(value, filename_string);
        run_microtask_checkpoint();
    } catch (...) {
        JS_FreeValue(context_, value);
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("evaluate", perf_start, script.size());
        throw;
    }

    const char* cstr = JS_ToCString(context_, value);
    std::string result = cstr == nullptr ? "" : cstr;
    if (cstr != nullptr) {
        JS_FreeCString(context_, cstr);
    } else {
        // Stringification failed (e.g. a throwing toString); discard the
        // pending exception so it cannot surface on a later operation.
        JS_FreeValue(context_, JS_GetException(context_));
    }
    JS_FreeValue(context_, value);
    clear_deadline();
    flush_dom_bridge_perf();
    emit_script_perf("evaluate", perf_start, script.size());
    return result;
}

void JsRuntime::run_until_idle()
{
    const auto perf_start = std::chrono::steady_clock::now();
    start_deadline();
    int iterations = 0;

    auto finish = [&]() {
        clear_deadline();
        flush_dom_bridge_perf();
        emit_script_perf("run_until_idle", perf_start, static_cast<std::uint64_t>(iterations));
    };

    if (run_microtask_checkpoint_logged() < 0) {
        finish();
        return;
    }

    // Already-ready tasks (0-delay) always execute, even with wait_time=0;
    // the budget only bounds how long we WAIT for future work (timers,
    // in-flight transfers). Interval timers and animation frames never count
    // toward idleness: the loop stops once only they remain (their ticks run
    // only while something else keeps the loop pumping within the budget).
    const auto wait_time = options_.wait_time.count() < 0
        ? std::chrono::milliseconds{0}
        : options_.wait_time;
    const auto deadline = std::chrono::steady_clock::now() + wait_time;
    constexpr int kMaxIdleIterations = 1000;

    for (int i = 0; i < kMaxIdleIterations; ++i) {
        const int ran = run_event_loop_turn();
        if (ran < 0) {
            break;
        }
        if (ran > 0) {
            ++iterations;
            continue;
        }
        if (event_loop_->idle()) {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        event_loop_->wait_for_activity(remaining);
    }

    finish();
}

int JsRuntime::run_microtask_checkpoint_logged()
{
    try {
        run_microtask_checkpoint();
        return 0;
    } catch (const std::exception& error) {
        log_console("error", error.what());
        return -1;
    }
}

int JsRuntime::deliver_mutation_observers()
{
    JSValue global = JS_GetGlobalObject(context_);
    JSValue deliver = JS_GetPropertyStr(context_, global, "__pagecore_deliver_mutation_observers");
    JS_FreeValue(context_, global);

    if (!JS_IsFunction(context_, deliver)) {
        JS_FreeValue(context_, deliver);
        return 0;
    }

    JSValue result = JS_Call(context_, deliver, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(context_, deliver);
    check_exception(result, "<mutation-observer>");

    int32_t delivered = 0;
    if (JS_ToInt32(context_, &delivered, result) < 0) {
        // The shim contract returns a number; clear any stray pending exception a
        // non-numeric return would leave so it can't surface on the next JS call.
        JS_FreeValue(context_, JS_GetException(context_));
        delivered = 0;
    }
    JS_FreeValue(context_, result);
    return delivered;
}

void JsRuntime::run_microtask_checkpoint()
{
    constexpr int kMaxCheckpointIterations = 100;
    for (int i = 0; i < kMaxCheckpointIterations; ++i) {
        drain_jobs();
        const int delivered = deliver_mutation_observers();
        drain_jobs();
        if (delivered == 0) {
            break;
        }
    }
}

int JsRuntime::run_event_loop_turn()
{
    event_loop_->poll();
    int ran = 0;
    if (event_loop_->run_one_task()) {
        ran = 1;
        if (run_microtask_checkpoint_logged() < 0) {
            return -1;
        }
    }
    // Rendering phase: when the frame timer has fired, run every currently
    // registered rAF callback once (callbacks registered during the frame go
    // to the next frame), followed by its own microtask checkpoint.
    if (event_loop_->animation_frame_due()) {
        if (run_animation_frames() > 0) {
            ran = 1;
            if (run_microtask_checkpoint_logged() < 0) {
                return -1;
            }
        }
    }
    return ran;
}

int JsRuntime::run_animation_frames()
{
    const std::deque<std::uint64_t> ids = event_loop_->take_due_animation_frames();
    if (ids.empty()) {
        return 0;
    }

    start_deadline();
    JSValue global = JS_GetGlobalObject(context_);
    JSValue run_raf = JS_GetPropertyStr(context_, global, "__pagecore_run_raf_callbacks");
    JS_FreeValue(context_, global);
    if (!JS_IsFunction(context_, run_raf)) {
        JS_FreeValue(context_, run_raf);
        return 0;
    }

    JSValue id_array = JS_NewArray(context_);
    uint32_t index = 0;
    for (const std::uint64_t id : ids) {
        JS_SetPropertyUint32(context_, id_array, index++, JS_NewFloat64(context_, static_cast<double>(id)));
    }
    JSValue args[2] = {id_array, JS_NewFloat64(context_, event_loop_->now_ms())};
    JSValue result = JS_Call(context_, run_raf, JS_UNDEFINED, 2, args);
    JS_FreeValue(context_, args[0]);
    JS_FreeValue(context_, args[1]);
    JS_FreeValue(context_, run_raf);
    try {
        check_exception(result, "<animation-frame>");
    } catch (const std::exception& error) {
        log_console("error", error.what());
    }
    JS_FreeValue(context_, result);
    return static_cast<int>(ids.size());
}

void JsRuntime::run_queued_task(std::uint64_t id)
{
    // Each task gets a fresh js_timeout budget (still clamped to the aggregate
    // load deadline), matching how individual scripts are bounded.
    start_deadline();
    JSValue global = JS_GetGlobalObject(context_);
    JSValue run_task = JS_GetPropertyStr(context_, global, "__pagecore_run_task");
    JS_FreeValue(context_, global);

    if (!JS_IsFunction(context_, run_task)) {
        JS_FreeValue(context_, run_task);
        return;
    }

    JSValue args[1] = {JS_NewFloat64(context_, static_cast<double>(id))};
    JSValue result = JS_Call(context_, run_task, JS_UNDEFINED, 1, args);
    JS_FreeValue(context_, args[0]);
    JS_FreeValue(context_, run_task);
    try {
        check_exception(result, "<event-loop-task>");
    } catch (const std::exception& error) {
        // A throwing task is logged and the loop keeps going, like a browser.
        log_console("error", error.what());
    }
    JS_FreeValue(context_, result);
}

bool JsRuntime::readiness_satisfied(WaitUntil wait_until, std::chrono::milliseconds stable_window) const
{
    if (!activity_tracker_.snapshot().load_fired || !activity_tracker_.mutation_observers_drained()) {
        return false;
    }

    // A provably quiescent loop cannot mutate the DOM again, so the DOM is
    // trivially stable without waiting out the (real-time) stability window.
    const bool dom_stable = event_loop_->quiescent()
        || activity_tracker_.dom_stable(stable_window);

    switch (wait_until) {
    case WaitUntil::Load:
        return true;
    case WaitUntil::NetworkIdle:
        return activity_tracker_.network_idle();
    case WaitUntil::DomStable:
        return activity_tracker_.snapshot().pending_relevant_timers == 0
            && dom_stable;
    case WaitUntil::Ready:
        return activity_tracker_.network_idle()
            && dom_stable;
    }
    return false;
}

bool JsRuntime::run_until_ready(PageReadinessOptions options)
{
    if (options.wait_time.count() < 0) {
        options.wait_time = std::chrono::milliseconds{0};
    }
    if (options.stable_window.count() < 0) {
        options.stable_window = std::chrono::milliseconds{0};
    }

    const auto perf_start = std::chrono::steady_clock::now();
    const auto wait_start = std::chrono::steady_clock::now();
    start_deadline();

    bool ready = false;
    int iterations = 0;
    constexpr int kMaxReadinessIterations = 1000;

    for (;;) {
        ++iterations;
        if (run_microtask_checkpoint_logged() < 0) {
            break;
        }

        event_loop_->poll();
        activity_tracker_.set_clock(event_loop_->now());
        activity_tracker_.set_relevant_timers(
            event_loop_->relevant_pending_count(options.stable_window));
        activity_tracker_.sync_mutation_version(document_->mutation_version());

        if (readiness_satisfied(options.wait_until, options.stable_window)) {
            ready = true;
            break;
        }

        if (options.wait_time.count() <= 0 || iterations >= kMaxReadinessIterations) {
            break;
        }
        const auto elapsed = std::chrono::steady_clock::now() - wait_start;
        if (elapsed >= options.wait_time) {
            break;
        }

        if (event_loop_->has_ready_task() || event_loop_->animation_frame_due()) {
            if (run_event_loop_turn() < 0) {
                break;
            }
            continue;
        }

        // Next wake-up: the nearest timer, the moment the stability window
        // would elapse, or the remaining wall-clock budget - whichever is
        // soonest. uv_run(UV_RUN_ONCE) also wakes early on any loop activity.
        const bool wants_dom_stability = options.wait_until == WaitUntil::DomStable
            || options.wait_until == WaitUntil::Ready;
        std::optional<std::chrono::milliseconds> advance = event_loop_->next_timer_delay();
        if (wants_dom_stability && !activity_tracker_.dom_stable(options.stable_window)) {
            const auto snapshot = activity_tracker_.snapshot();
            auto stable_remaining = options.stable_window - (snapshot.clock - snapshot.last_mutation_clock);
            if (stable_remaining.count() < 0) {
                stable_remaining = std::chrono::milliseconds{0};
            }
            if (!advance || stable_remaining < *advance) {
                advance = stable_remaining;
            }
        }

        if (!advance && event_loop_->idle()) {
            // No timers, no tasks, no in-flight work: nothing can change the
            // readiness verdict anymore.
            break;
        }

        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            options.wait_time - elapsed);
        auto wait = advance ? std::min(*advance, budget) : budget;
        if (wait.count() < 1) {
            wait = std::chrono::milliseconds{1};
        }
        event_loop_->wait_for_activity(wait);
    }

    clear_deadline();
    flush_dom_bridge_perf();
    emit_script_perf("run_until_ready", perf_start, static_cast<std::uint64_t>(iterations));
    return ready;
}

DomDocument& JsRuntime::document()
{
    return *document_;
}

PageActivityTracker& JsRuntime::activity_tracker()
{
    return activity_tracker_;
}

EventLoop& JsRuntime::event_loop()
{
    return *event_loop_;
}

void JsRuntime::set_computed_style_resolver(ComputedStyleResolver resolver)
{
    computed_style_resolver_ = std::move(resolver);
}

std::optional<ComputedStyle> JsRuntime::computed_style(NodeId node)
{
    if (!computed_style_resolver_) {
        return std::nullopt;
    }
    return computed_style_resolver_(node);
}

void JsRuntime::set_computed_style_property_resolver(ComputedStylePropertyResolver resolver)
{
    computed_style_property_resolver_ = std::move(resolver);
}

std::optional<std::string> JsRuntime::computed_style_property(NodeId node, std::string_view property)
{
    if (!computed_style_property_resolver_) {
        return std::nullopt;
    }
    return computed_style_property_resolver_(node, property);
}

void JsRuntime::set_element_geometry_resolver(ElementGeometryResolver resolver)
{
    element_geometry_resolver_ = std::move(resolver);
}

std::optional<ElementGeometry> JsRuntime::element_geometry(NodeId node)
{
    if (!element_geometry_resolver_) {
        return std::nullopt;
    }
    return element_geometry_resolver_(node);
}

void JsRuntime::set_elements_at_point_resolver(ElementsAtPointResolver resolver)
{
    elements_at_point_resolver_ = std::move(resolver);
}

std::vector<NodeId> JsRuntime::elements_at_point(float x, float y, bool topmost_only)
{
    if (!elements_at_point_resolver_) {
        return {};
    }
    return elements_at_point_resolver_(x, y, topmost_only);
}

void JsRuntime::set_viewport_resolver(ViewportResolver resolver)
{
    viewport_resolver_ = std::move(resolver);
}

Viewport JsRuntime::viewport()
{
    if (!viewport_resolver_) {
        return Viewport{};
    }
    return viewport_resolver_();
}

void JsRuntime::record_dom_bridge_perf(std::string_view name, long long elapsed_us)
{
    if (!options_.perf_trace) {
        return;
    }
    auto& aggregate = dom_bridge_perf_[std::string(name)];
    aggregate.elapsed_us += std::max<long long>(0, elapsed_us);
    ++aggregate.calls;
}

void JsRuntime::flush_dom_bridge_perf()
{
    if (dom_bridge_perf_.empty()) {
        return;
    }
    if (options_.perf_trace) {
        for (const auto& [name, aggregate] : dom_bridge_perf_) {
            emit_perf_trace(
                options_.perf_trace,
                PerfEvent{PerfPhase::DomBridge, name, aggregate.elapsed_us, aggregate.calls});
        }
    }
    dom_bridge_perf_.clear();
}

void JsRuntime::emit_script_perf(
    std::string_view name,
    std::chrono::steady_clock::time_point start,
    std::uint64_t count)
{
    emit_perf_trace(options_.perf_trace, PerfPhase::Script, name, elapsed_us_since(start), count);
}

std::optional<std::string> JsRuntime::js_resource_budget_block_reason() const
{
    if (options_.max_js_resource_loads && js_resource_load_count_ >= *options_.max_js_resource_loads) {
        return "budget:max_js_resource_loads";
    }
    if (options_.max_js_resource_bytes && js_resource_load_bytes_ >= *options_.max_js_resource_bytes) {
        return "budget:max_js_resource_bytes";
    }
    if (options_.max_js_resource_time) {
        const auto max_us = std::chrono::duration_cast<std::chrono::microseconds>(
            *options_.max_js_resource_time).count();
        if (js_resource_load_elapsed_us_ >= max_us) {
            return "budget:max_js_resource_time_ms";
        }
    }
    return std::nullopt;
}

void JsRuntime::record_js_resource_load(long long elapsed_us, std::size_t bytes)
{
    // The load COUNT budget is spent at request start (see
    // prepare_js_resource_request); only bytes/time are known at completion.
    js_resource_load_bytes_ += bytes;
    js_resource_load_elapsed_us_ += std::max<long long>(0, elapsed_us);
}

JsRuntime::JsResourceRequestContext JsRuntime::prepare_js_resource_request(
    std::string_view url,
    std::string_view kind,
    std::string method,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string credentials,
    std::string referrer)
{
    if (!loader_) {
        throw std::runtime_error("resource loader is not available");
    }
    const std::string base = options_.base_url;
    std::string request_referrer = base;
    if (referrer.empty() || referrer == "no-referrer") {
        request_referrer.clear();
    } else if (referrer != "about:client") {
        request_referrer = resolve_url(base, referrer);
    }
    ResourceRequest request{
        resolve_url(base, url),
        resource_kind_from_string(kind),
        request_referrer,
        base,
        std::move(method),
        std::move(body),
        std::move(headers),
    };
    const CookieCredentials cookie_credentials = credentials_from_string(credentials);

    if (js_resource_load_blocked(options_.js_resource_load_policy, base, request.url)) {
        emit_perf_trace(
            options_.perf_trace,
            js_resource_blocked_event(request, "policy:" + js_resource_policy_name(options_.js_resource_load_policy)));
        throw ResourceError(
            ResourceErrorCode::NetworkDisabled,
            request.url,
            "JS resource load blocked by policy: " + js_resource_policy_name(options_.js_resource_load_policy));
    }
    if (auto budget_reason = js_resource_budget_block_reason()) {
        emit_perf_trace(options_.perf_trace, js_resource_blocked_event(request, *budget_reason));
        throw ResourceError(
            ResourceErrorCode::NetworkDisabled,
            request.url,
            "JS resource load blocked by budget: " + *budget_reason);
    }
    // Spend the count budget when the transfer STARTS: with truly async loads,
    // several transfers can be in flight before any completion is recorded, so
    // a completion-time count would let a page overshoot max_js_resource_loads.
    ++js_resource_load_count_;

    JsResourceRequestContext context;
    context.credentials = cookie_credentials;
    context.request_url = request.url;
    if (cookie_jar_ != nullptr) {
        request = cookie_jar_->with_cookie_header(std::move(request), cookie_credentials, base);
    }
    context.request = std::move(request);
    return context;
}

ResourceResponse JsRuntime::load_resource(
    std::string_view url,
    std::string_view kind,
    std::string method,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string credentials,
    std::string referrer)
{
    JsResourceRequestContext context = prepare_js_resource_request(
        url, kind, std::move(method), std::move(body), std::move(headers),
        std::move(credentials), std::move(referrer));

    const auto perf_start = std::chrono::steady_clock::now();
    try {
        auto response = loader_->load(context.request);
        if (cookie_jar_ != nullptr) {
            cookie_jar_->store_from_response(context.request_url, response, context.credentials, options_.base_url);
        }
        const long long elapsed_us = elapsed_us_since(perf_start);
        record_js_resource_load(elapsed_us, response.body.size());
        PerfEvent event{PerfPhase::ResourceLoad, "js_load_resource", elapsed_us, response.body.size()};
        event.property = resource_kind_name(context.request.kind);
        event.url = context.request.url;
        emit_perf_trace(options_.perf_trace, std::move(event));
        return response;
    } catch (...) {
        const long long elapsed_us = elapsed_us_since(perf_start);
        record_js_resource_load(elapsed_us, 0);
        PerfEvent event{PerfPhase::ResourceLoad, "js_load_resource", elapsed_us, 0};
        event.property = resource_kind_name(context.request.kind);
        event.url = context.request.url;
        emit_perf_trace(options_.perf_trace, std::move(event));
        throw;
    }
}

void JsRuntime::start_resource_load(
    std::uint64_t js_id,
    std::string_view url,
    std::string_view kind,
    std::string method,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string credentials,
    std::string referrer)
{
    if (!async_loader_) {
        throw std::runtime_error("async resource loading is not available");
    }
    auto context = std::make_shared<JsResourceRequestContext>(prepare_js_resource_request(
        url, kind, std::move(method), std::move(body), std::move(headers),
        std::move(credentials), std::move(referrer)));
    const auto perf_start = std::chrono::steady_clock::now();

    // Synchronous cache probe on the curl path (the task-queue adapter hits the
    // caching loader inside load() instead). Hits still complete as a queued
    // Networking task so JS observes a uniform async contract.
    if (js_resource_cache_) {
        if (auto cached = js_resource_cache_->cached(context->request)) {
            auto result = std::make_shared<AsyncLoadResult>();
            result->response = std::move(*cached);
            // Sentinel 0: no engine transfer behind this id, but the pending
            // entry lets cancel_resource_load() drop the queued completion.
            pending_resource_loads_[js_id] = 0;
            event_loop_->post(
                TaskSource::Networking,
                [this, js_id, context, result, perf_start] {
                    if (pending_resource_loads_.erase(js_id) == 0) {
                        return; // cancelled before delivery
                    }
                    finish_resource_load(js_id, *context, std::move(*result), perf_start, /*store_in_cache=*/false);
                },
                /*readiness_relevant=*/true);
            return;
        }
    }

    const std::uint64_t async_id = async_loader_->start(
        context->request,
        [this, js_id, context, perf_start](AsyncLoadResult result) {
            // Runs inside the queued Networking task the loader posted.
            pending_resource_loads_.erase(js_id);
            finish_resource_load(js_id, *context, std::move(result), perf_start, /*store_in_cache=*/true);
        });
    pending_resource_loads_[js_id] = async_id;
}

std::uint64_t JsRuntime::start_document_script_load(ResourceRequest request, AsyncLoadCallback callback)
{
    if (!async_loader_) {
        throw std::runtime_error("async resource loading is not available");
    }
    const std::string base = request.base_url.empty() ? options_.base_url : request.base_url;
    const std::string request_url = request.url;
    if (cookie_jar_ != nullptr) {
        request = cookie_jar_->with_cookie_header(std::move(request), CookieCredentials::SameOrigin, base);
    }

    // Same explicit cache handling as start_resource_load on the curl path
    // (the task-queue adapter caches inside the blocking loader instead). The
    // cache key includes the attached cookie header, matching the blocking
    // CookieAware -> Caching pipeline this replaces.
    if (js_resource_cache_) {
        if (auto cached = js_resource_cache_->cached(request)) {
            auto result = std::make_shared<AsyncLoadResult>();
            result->response = std::move(*cached);
            event_loop_->post(
                TaskSource::Networking,
                [callback = std::move(callback), result] { callback(std::move(*result)); },
                /*readiness_relevant=*/true);
            return 0;
        }
    }

    auto shared_request = std::make_shared<ResourceRequest>(request);
    return async_loader_->start(
        *shared_request,
        [this, request_url, base, shared_request, callback = std::move(callback)](AsyncLoadResult result) {
            if (!result.error) {
                if (cookie_jar_ != nullptr) {
                    cookie_jar_->store_from_response(
                        request_url, result.response, CookieCredentials::SameOrigin, base);
                }
                if (js_resource_cache_) {
                    js_resource_cache_->store(*shared_request, result.response);
                }
            }
            callback(std::move(result));
        });
}

bool JsRuntime::run_event_loop_until(
    const std::function<bool()>& condition,
    std::chrono::steady_clock::time_point deadline)
{
    if (run_microtask_checkpoint_logged() < 0) {
        return condition();
    }
    for (;;) {
        if (condition()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const int ran = run_event_loop_turn();
        if (ran < 0) {
            return condition();
        }
        if (ran == 0) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            event_loop_->wait_for_activity(remaining);
        }
    }
}

void JsRuntime::cancel_resource_load(std::uint64_t js_id)
{
    const auto found = pending_resource_loads_.find(js_id);
    if (found == pending_resource_loads_.end()) {
        return;
    }
    const std::uint64_t async_id = found->second;
    pending_resource_loads_.erase(found);
    // async_id 0 is the cache-hit sentinel: nothing to cancel in the engine,
    // erasing the pending entry already drops the queued completion.
    if (async_id != 0 && async_loader_) {
        async_loader_->cancel(async_id);
    }
}

void JsRuntime::finish_resource_load(
    std::uint64_t js_id,
    const JsResourceRequestContext& context,
    AsyncLoadResult result,
    std::chrono::steady_clock::time_point started,
    bool store_in_cache)
{
    const long long elapsed_us = elapsed_us_since(started);
    std::string error_message;
    if (!result.error) {
        if (cookie_jar_ != nullptr) {
            cookie_jar_->store_from_response(
                context.request_url, result.response, context.credentials, options_.base_url);
        }
        if (store_in_cache && js_resource_cache_) {
            js_resource_cache_->store(context.request, result.response);
        }
        record_js_resource_load(elapsed_us, result.response.body.size());
        PerfEvent event{PerfPhase::ResourceLoad, "js_load_resource", elapsed_us, result.response.body.size()};
        event.property = resource_kind_name(context.request.kind);
        event.url = context.request.url;
        emit_perf_trace(options_.perf_trace, std::move(event));
    } else {
        record_js_resource_load(elapsed_us, 0);
        PerfEvent event{PerfPhase::ResourceLoad, "js_load_resource", elapsed_us, 0};
        event.property = resource_kind_name(context.request.kind);
        event.url = context.request.url;
        emit_perf_trace(options_.perf_trace, std::move(event));
        try {
            std::rethrow_exception(result.error);
        } catch (const std::exception& error) {
            error_message = error.what();
        } catch (...) {
            error_message = "resource load failed";
        }
    }

    // Deliver to JS through the shim's completion registry. Each completion
    // gets a fresh execution budget, like any other task.
    start_deadline();
    JSValue global = JS_GetGlobalObject(context_);
    JSValue complete = JS_GetPropertyStr(context_, global, "__pagecore_resource_load_complete");
    JS_FreeValue(context_, global);
    if (!JS_IsFunction(context_, complete)) {
        JS_FreeValue(context_, complete);
        return;
    }

    JSValue args[3];
    args[0] = JS_NewFloat64(context_, static_cast<double>(js_id));
    if (!result.error) {
        args[1] = js_resource_response_object(context_, result.response);
        args[2] = JS_UNDEFINED;
    } else {
        args[1] = JS_NULL;
        args[2] = js_string(context_, error_message);
    }
    JSValue call_result = JS_Call(context_, complete, JS_UNDEFINED, 3, args);
    for (JSValue& arg : args) {
        JS_FreeValue(context_, arg);
    }
    JS_FreeValue(context_, complete);
    try {
        check_exception(call_result, "<resource-load-complete>");
    } catch (const std::exception& error) {
        log_console("error", error.what());
    }
    JS_FreeValue(context_, call_result);
}

std::string JsRuntime::document_cookie(std::string_view url) const
{
    return cookie_jar_ == nullptr ? std::string{} : cookie_jar_->document_cookie(url);
}

void JsRuntime::set_document_cookie(std::string_view url, std::string_view cookie)
{
    if (cookie_jar_ != nullptr) {
        cookie_jar_->set_document_cookie(url, cookie);
    }
}

namespace {

void append_utf8_codepoint(std::string& out, lxb_codepoint_t code)
{
    if (code <= 0x7f) {
        out += static_cast<char>(code);
    } else if (code <= 0x7ff) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code <= 0xffff) {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

} // namespace

std::optional<JsRuntime::TextDecoderHandle> JsRuntime::create_text_decoder(std::string_view label, bool fatal)
{
    const auto* encoding_data = lxb_encoding_data_by_name(
        reinterpret_cast<const lxb_char_t*>(label.data()), label.size());

    // "replacement" (and its historical aliases) is a real lxb_encoding_data_t
    // to Lexbor, but the WHATWG Encoding Standard carves it out specifically:
    // TextDecoder's constructor must reject it with a RangeError rather than
    // ever decode with it (see api-replacement-encodings.any.js).
    if (encoding_data == nullptr || encoding_data->encoding == LXB_ENCODING_REPLACEMENT) {
        return std::nullopt;
    }

    const int id = next_text_decoder_id_++;
    TextDecoderState state;
    state.encoding_data = encoding_data;
    state.fatal = fatal;
    state.replacement = LXB_ENCODING_REPLACEMENT_CODEPOINT;

    const lxb_status_t init_status = lxb_encoding_decode_init(&state.ctx, encoding_data, nullptr, 0);
    if (init_status != LXB_STATUS_OK) {
        return std::nullopt;
    }
    if (!fatal) {
        // A non-fatal decoder replaces bad sequences with U+FFFD inline
        // instead of erroring; buffer_length must already reflect a real
        // output buffer for this to succeed, so it is (re)armed per chunk in
        // decode_text_chunk rather than here.
        state.ctx.replace_to = &state.replacement;
        state.ctx.replace_len = 1;
    }

    text_decoders_.emplace(id, state);

    TextDecoderHandle handle;
    handle.id = id;
    handle.encoding = reinterpret_cast<const char*>(encoding_data->name);
    return handle;
}

std::optional<std::string> JsRuntime::decode_text_chunk(int handle, std::string_view bytes, bool is_final)
{
    auto iterator = text_decoders_.find(handle);
    if (iterator == text_decoders_.end()) {
        throw std::runtime_error("decode_text_chunk: unknown TextDecoder handle");
    }
    TextDecoderState& state = iterator->second;

    std::string result;
    lxb_codepoint_t codepoint_buffer[256];
    state.ctx.buffer_out = codepoint_buffer;
    state.ctx.buffer_length = std::size(codepoint_buffer);
    state.ctx.buffer_used = 0;
    // replace_to points at state.replacement, which does not move, but the
    // pointer must be re-armed since buffer_set (via buffer_length above)
    // does not touch it -- re-set for clarity and because init() above only
    // set it once against a zero-length buffer.
    if (!state.fatal) {
        state.ctx.replace_to = &state.replacement;
        state.ctx.replace_len = 1;
    }

    const auto* data = reinterpret_cast<const lxb_char_t*>(bytes.data());
    const lxb_char_t* end = data + bytes.size();

    auto drain = [&]() {
        const lxb_codepoint_t* cps = lxb_encoding_decode_buf(&state.ctx);
        const size_t used = lxb_encoding_decode_buf_used(&state.ctx);
        for (size_t i = 0; i < used; ++i) {
            append_utf8_codepoint(result, cps[i]);
        }
        lxb_encoding_decode_buf_used_set(&state.ctx, 0);
    };

    while (data < end) {
        const lxb_status_t status = state.encoding_data->decode(&state.ctx, &data, end);
        if (status == LXB_STATUS_OK) {
            drain();
            break;
        }
        if (status == LXB_STATUS_SMALL_BUFFER) {
            drain();
            continue;
        }
        if (status == LXB_STATUS_CONTINUE) {
            // Input exhausted mid-sequence; state.ctx retains the pending
            // lead byte(s) for the next chunk (WHATWG streaming decode).
            drain();
            break;
        }
        // Only reachable in fatal mode (replace_to unset): an invalid byte
        // sequence with no replacement configured. Per spec, throwing here
        // still resets the decoder ("set this's do not flush to false"): a
        // later decode() call on the same instance must not inherit the
        // failed attempt's partial byte state.
        lxb_encoding_decode_init(&state.ctx, state.encoding_data, nullptr, 0);
        return std::nullopt;
    }

    if (is_final) {
        const lxb_status_t finish_status = lxb_encoding_decode_finish(&state.ctx);
        // finish() appends a replacement codepoint into buffer_out on
        // success; drain it before the reset below discards buffer_out.
        if (finish_status == LXB_STATUS_OK) {
            drain();
        }
        // A fresh decoder must not carry EOF-flush state into the next
        // sequence if this same TextDecoder instance is reused after `stream:
        // false` -- the WHATWG algorithm re-initializes decoder state at the
        // start of every non-streaming decode() call. This applies whether
        // finish() succeeded or (fatal mode) hit a dangling partial sequence.
        lxb_encoding_decode_init(&state.ctx, state.encoding_data, nullptr, 0);
        if (finish_status != LXB_STATUS_OK) {
            return std::nullopt;
        }
    }

    return result;
}

void JsRuntime::log_console(std::string_view severity, std::string_view message)
{
    if (options_.console_log) {
        options_.console_log(severity, message);
        return;
    }

    std::cerr << "[js:" << severity << "]";
    if (!message.empty()) {
        std::cerr << ' ' << message;
    }
    std::cerr << '\n';
}

void JsRuntime::set_load_deadline(std::optional<std::chrono::steady_clock::time_point> deadline)
{
    load_deadline_ = deadline;
}

bool JsRuntime::load_deadline_passed() const
{
    return load_deadline_.has_value() && std::chrono::steady_clock::now() >= *load_deadline_;
}

void JsRuntime::start_deadline()
{
    deadline_ = std::chrono::steady_clock::now() + options_.js_timeout;
    // Clamp the per-call deadline to the aggregate load deadline so the whole
    // <script> sequence cannot exceed it even though each script gets js_timeout.
    if (load_deadline_.has_value() && *load_deadline_ < deadline_) {
        deadline_ = *load_deadline_;
    }
    deadline_active_ = true;
}

void JsRuntime::clear_deadline()
{
    deadline_active_ = false;
}

bool JsRuntime::is_timed_out() const
{
    return deadline_active_ && std::chrono::steady_clock::now() > deadline_;
}

int JsRuntime::drain_jobs()
{
    JSContext* job_context = nullptr;
    int count = 0;
    for (;;) {
        const int status = JS_ExecutePendingJob(runtime_, &job_context);
        if (status <= 0) {
            if (status < 0) {
                check_exception(JS_EXCEPTION, "<pending-job>");
            }
            break;
        }
        ++count;
    }
    return count;
}

void JsRuntime::check_exception(JSValue value, std::string_view source_name)
{
    if (!JS_IsException(value)) {
        return;
    }

    JSValue exception = JS_GetException(context_);
    // Reading ".stack" off a thrown non-object (e.g. `throw null`) would set a
    // secondary pending exception; only do it when the thrown value is an object.
    JSValue stack = JS_IsObject(exception)
        ? JS_GetPropertyStr(context_, exception, "stack")
        : JS_UNDEFINED;
    const char* stack_cstr = JS_IsUndefined(stack) ? nullptr : JS_ToCString(context_, stack);
    const char* exception_cstr = JS_ToCString(context_, exception);

    std::string message = "JS exception";
    if (!source_name.empty()) {
        message += " (";
        message += source_name;
        message += ")";
    }
    if (exception_cstr != nullptr) {
        message += ": ";
        message += exception_cstr;
    }
    if (stack_cstr != nullptr) {
        message += "\n";
        message += stack_cstr;
    }

    if (stack_cstr != nullptr) JS_FreeCString(context_, stack_cstr);
    if (exception_cstr != nullptr) JS_FreeCString(context_, exception_cstr);
    JS_FreeValue(context_, stack);
    JS_FreeValue(context_, exception);

    // Stringifying a thrown object whose stack/toString itself throws leaves a new
    // pending exception in the runtime; clear it so it can't surface spuriously on
    // a later, unrelated call after we unwind.
    JS_FreeValue(context_, JS_GetException(context_));

    clear_deadline();
    throw std::runtime_error(message);
}

} // namespace pagecore

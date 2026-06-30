#include "js_runtime.hpp"

#include "dom_shim.hpp"
#include "pagecore/resource_loader.hpp"

extern "C" {
#include <quickjs.h>
}

#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
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

JSValue js_string(JSContext* ctx, const std::string& value)
{
    return JS_NewStringLen(ctx, value.data(), value.size());
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
    JSValue item = JS_NewObject(ctx);
    const int type = document.node_type(id);
    JS_SetPropertyStr(ctx, item, "id", js_id(ctx, id));
    JS_SetPropertyStr(ctx, item, "type", JS_NewInt32(ctx, type));
    if (type == 1) {
        JS_SetPropertyStr(ctx, item, "tag", js_string(ctx, document.tag_name(id)));
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

void set_function(JSContext* ctx, JSValueConst object, const char* name, JSCFunction* function, int argc)
{
    JS_SetPropertyStr(ctx, object, name, JS_NewCFunction(ctx, function, name, argc));
}

int interrupt_handler(JSRuntime*, void* opaque)
{
    return static_cast<JsRuntime*>(opaque)->is_timed_out() ? 1 : 0;
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("childNodesDescribed requires node id");
        return js_node_descriptors(ctx, js.document(), js.document().child_nodes(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_children_described(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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

JSValue bridge_forget_version(JSContext* ctx, JSValue, int, JSValue*)
{
    return bridge_call(ctx, [ctx](JsRuntime& js) {
        return JS_NewFloat64(ctx, static_cast<double>(js.document().forget_version()));
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("getAttribute requires node id and name");
        auto value = js.document().get_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]));
        return value ? js_string(ctx, *value) : JS_NULL;
    });
}

JSValue bridge_has_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("hasAttribute requires node id and name");
        return JS_NewBool(ctx, js.document().has_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_attributes(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("attributes requires node id");
        return js_attributes(ctx, js.document().attributes(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_set_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 3) throw std::runtime_error("setAttribute requires node id, name and value");
        js.document().set_attribute(to_node_id(ctx, argv[0]), to_string(ctx, argv[1]), to_string(ctx, argv[2]));
        return JS_UNDEFINED;
    });
}

JSValue bridge_remove_attribute(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("innerHTML requires node id");
        return js_string(ctx, js.document().inner_html(to_node_id(ctx, argv[0])));
    });
}

JSValue bridge_set_inner_html(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("appendChild requires parent and child ids");
        return js_id(ctx, js.document().append_child(to_node_id(ctx, argv[0]), to_node_id(ctx, argv[1])));
    });
}

JSValue bridge_insert_before(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("removeChild requires parent and child ids");
        return js_id(ctx, js.document().remove_child(to_node_id(ctx, argv[0]), to_node_id(ctx, argv[1])));
    });
}

JSValue bridge_replace_child(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("querySelector requires root id and selector");
        return js_id(ctx, js.document().query_selector(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_query_selector_all(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 2) throw std::runtime_error("querySelectorAll requires root id and selector");
        return js_ids(ctx, js.document().query_selector_all(to_node_id(ctx, argv[0]), to_string(ctx, argv[1])));
    });
}

JSValue bridge_get_element_by_id(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
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

JSValue host_load_resource(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime& js) {
        if (argc < 1) throw std::runtime_error("loadResource requires url");
        const std::string url = to_string(ctx, argv[0]);
        const std::string kind = argc > 1 ? to_string(ctx, argv[1]) : "other";
        const auto response = js.load_resource(url, kind);

        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "url", js_string(ctx, response.url));
        JS_SetPropertyStr(ctx, out, "body", js_string(ctx, response.body));
        JS_SetPropertyStr(ctx, out, "mimeType", js_string(ctx, response.mime_type));
        JS_SetPropertyStr(ctx, out, "kind", js_string(ctx, resource_kind_name(response.kind)));
        JS_SetPropertyStr(ctx, out, "fromCache", JS_NewBool(ctx, response.from_cache));
        JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, response.status));
        return out;
    });
}

JSValue host_random_bytes(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return bridge_call(ctx, [ctx, argc, argv](JsRuntime&) {
        int32_t count = 0;
        if (argc > 0) {
            JS_ToInt32(ctx, &count, argv[0]);
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

JsRuntime::JsRuntime(DomDocument& document, LoadOptions options, std::shared_ptr<ResourceLoader> loader)
    : document_(&document)
    , options_(std::move(options))
    , loader_(std::move(loader))
{
    runtime_ = JS_NewRuntime();
    if (runtime_ == nullptr) {
        throw std::runtime_error("failed to create QuickJS runtime");
    }

    JS_SetMemoryLimit(runtime_, options_.js_memory_limit_bytes);
    JS_SetMaxStackSize(runtime_, 1024 * 1024);
    JS_SetInterruptHandler(runtime_, interrupt_handler, this);
    JS_SetModuleLoaderFunc(runtime_, normalize_module, load_module, this);

    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr) {
        JS_FreeRuntime(runtime_);
        runtime_ = nullptr;
        throw std::runtime_error("failed to create QuickJS context");
    }

    JS_SetContextOpaque(context_, this);
}

JsRuntime::~JsRuntime()
{
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
    set_function(context_, dom, "forgetVersion", bridge_forget_version, 0);
    set_function(context_, dom, "computedStyle", bridge_computed_style, 1);
    set_function(context_, dom, "elementGeometry", bridge_element_geometry, 1);
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
    set_function(context_, host, "randomBytes", host_random_bytes, 1);
    JS_SetPropertyStr(context_, host, "baseURL", js_string(context_, options_.base_url));
    JS_SetPropertyStr(context_, host, "userAgent", js_string(context_, options_.user_agent));

    JS_SetPropertyStr(context_, global, "__dom", dom);
    JS_SetPropertyStr(context_, global, "__host", host);
    JS_FreeValue(context_, global);

    execute(kDomShim, "dom_shim.js");
}

void JsRuntime::execute(std::string_view script, std::string_view filename)
{
    start_deadline();
    const std::string filename_string(filename);
    JSValue value = JS_Eval(
        context_,
        script.data(),
        script.size(),
        filename_string.c_str(),
        JS_EVAL_TYPE_GLOBAL);
    check_exception(value, filename_string);
    JS_FreeValue(context_, value);
    drain_jobs();
    clear_deadline();
}

void JsRuntime::execute_module(std::string_view script, std::string_view filename)
{
    start_deadline();
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
    drain_jobs();
    clear_deadline();
}

std::string JsRuntime::evaluate(std::string_view script, std::string_view filename)
{
    start_deadline();
    const std::string filename_string(filename);
    JSValue value = JS_Eval(
        context_,
        script.data(),
        script.size(),
        filename_string.c_str(),
        JS_EVAL_TYPE_GLOBAL);
    check_exception(value, filename_string);

    // value holds a live reference; if draining a pending job throws, free it
    // (and clear the deadline) before propagating so it does not leak.
    try {
        drain_jobs();
    } catch (...) {
        JS_FreeValue(context_, value);
        clear_deadline();
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
    return result;
}

void JsRuntime::run_until_idle()
{
    start_deadline();
    bool timer_budget_available = true;

    for (int i = 0; i < 100; ++i) {
        drain_jobs();

        JSValue global = JS_GetGlobalObject(context_);
        JSValue run_timers = JS_GetPropertyStr(context_, global, "__pagecore_run_timers");
        JS_FreeValue(context_, global);

        if (!JS_IsFunction(context_, run_timers)) {
            JS_FreeValue(context_, run_timers);
            break;
        }

        const auto wait_time = options_.wait_time.count();
        const auto timer_budget = timer_budget_available && wait_time > 0 ? wait_time : 0;
        timer_budget_available = false;

        JSValue args[1] = {JS_NewInt64(context_, static_cast<int64_t>(timer_budget))};
        JSValue result = JS_Call(context_, run_timers, JS_UNDEFINED, 1, args);
        JS_FreeValue(context_, args[0]);
        JS_FreeValue(context_, run_timers);
        check_exception(result, "<timer-callback>");

        int32_t ran = 0;
        JS_ToInt32(context_, &ran, result);
        JS_FreeValue(context_, result);
        drain_jobs();

        if (ran == 0) {
            break;
        }
    }

    clear_deadline();
}

DomDocument& JsRuntime::document()
{
    return *document_;
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

ResourceResponse JsRuntime::load_resource(std::string_view url, std::string_view kind)
{
    if (!loader_) {
        throw std::runtime_error("resource loader is not available");
    }
    const std::string base = options_.base_url;
    return loader_->load(ResourceRequest{
        resolve_url(base, url),
        resource_kind_from_string(kind),
        base,
        base,
    });
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

void JsRuntime::start_deadline()
{
    deadline_ = std::chrono::steady_clock::now() + options_.js_timeout;
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

void JsRuntime::drain_jobs()
{
    JSContext* job_context = nullptr;
    for (;;) {
        const int status = JS_ExecutePendingJob(runtime_, &job_context);
        if (status <= 0) {
            if (status < 0) {
                check_exception(JS_EXCEPTION, "<pending-job>");
            }
            break;
        }
    }
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

    clear_deadline();
    throw std::runtime_error(message);
}

} // namespace pagecore

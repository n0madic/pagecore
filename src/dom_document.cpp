#include "dom_document.hpp"

#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/serialize.h>

#include <cctype>
#include <stdexcept>
#include <utility>

namespace pagecore {
namespace {

std::string to_string(const lxb_char_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(data), len};
}

lxb_status_t append_serialized(const lxb_char_t* data, size_t len, void* ctx)
{
    // C callback from Lexbor serialization — contain any C++ exception (e.g.
    // std::bad_alloc from append) so it cannot unwind through the C frame.
    try {
        auto* out = static_cast<std::string*>(ctx);
        out->append(reinterpret_cast<const char*>(data), len);
    } catch (...) {
        return LXB_STATUS_ERROR;
    }
    return LXB_STATUS_OK;
}

bool is_character_data(lxb_dom_node_t* node)
{
    return node != nullptr
        && (node->type == LXB_DOM_NODE_TYPE_TEXT
            || node->type == LXB_DOM_NODE_TYPE_COMMENT
            || node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION);
}

bool is_text_content_descendant(lxb_dom_node_t* node)
{
    return node != nullptr
        && (node->type == LXB_DOM_NODE_TYPE_TEXT
            || node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION);
}

std::string character_data(lxb_dom_node_t* node)
{
    if (!is_character_data(node)) {
        return {};
    }

    auto* data = lxb_dom_interface_character_data(node);
    return to_string(data->data.data, data->data.length);
}

std::string ascii_lower(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_service_attribute(std::string_view lower)
{
    return starts_with(lower, "data-")
        || starts_with(lower, "aria-")
        || lower == "role"
        || starts_with(lower, "on");
}

bool is_always_layout_attribute(std::string_view lower)
{
    return lower == "class"
        || lower == "id"
        || lower == "style"
        || lower == "hidden"
        || lower == "src"
        || lower == "srcset"
        || lower == "sizes"
        || lower == "href"
        || lower == "rel"
        || lower == "media"
        || lower == "width"
        || lower == "height"
        || lower == "type"
        || lower == "disabled"
        || lower == "checked"
        || lower == "selected"
        || lower == "open";
}

bool mutation_blocks_cached_width_for_self(std::string_view reason)
{
    return reason != "set_attribute:style";
}

bool mutation_blocks_cached_width_for_descendants(std::string_view reason)
{
    return reason != "append_child"
        && reason != "remove_child"
        && reason != "set_inner_html";
}

// Iterative pre-order traversal — recursion here would overflow the native
// stack on deeply nested DOM trees (attacker-controlled via HTML or JS).
void append_descendant_text(lxb_dom_node_t* node, std::string& out)
{
    if (node == nullptr) {
        return;
    }

    std::vector<lxb_dom_node_t*> stack;
    for (auto* child = node->last_child; child != nullptr; child = child->prev) {
        stack.push_back(child);
    }
    while (!stack.empty()) {
        auto* current = stack.back();
        stack.pop_back();
        if (is_text_content_descendant(current)) {
            out += character_data(current);
        } else {
            for (auto* child = current->last_child; child != nullptr; child = child->prev) {
                stack.push_back(child);
            }
        }
    }
}

void collect_subtree(lxb_dom_node_t* node, std::vector<lxb_dom_node_t*>& out)
{
    if (node == nullptr) {
        return;
    }

    std::vector<lxb_dom_node_t*> stack;
    stack.push_back(node);
    while (!stack.empty()) {
        auto* current = stack.back();
        stack.pop_back();
        out.push_back(current);
        for (auto* child = current->last_child; child != nullptr; child = child->prev) {
            stack.push_back(child);
        }
    }
}

constexpr std::string_view kLayoutIdAttribute = "data-pc-sid";

struct TransientAttributeRestore {
    lxb_dom_element_t* element = nullptr;
    std::string name;
    bool had_attribute = false;
    std::string value;
};

// Bypasses DomDocument::set_attribute/remove_attribute, which bump
// mutation_version: layout tagging is an internal serialization detail and must
// stay invisible to layout-cache keys.
void set_transient_attribute(
    lxb_dom_element_t* element,
    std::string_view name,
    std::string_view value,
    std::vector<TransientAttributeRestore>& restore)
{
    size_t old_len = 0;
    const auto* old_value = lxb_dom_element_get_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        &old_len);

    restore.push_back(TransientAttributeRestore{
        element,
        std::string(name),
        old_value != nullptr,
        old_value == nullptr ? std::string() : to_string(old_value, old_len),
    });

    auto* attr = lxb_dom_element_set_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        reinterpret_cast<const lxb_char_t*>(value.data()),
        value.size());
    if (attr == nullptr) {
        throw std::runtime_error("failed to set transient layout attribute");
    }
}

void set_layout_id_attribute(lxb_dom_element_t* element, NodeId id, std::vector<TransientAttributeRestore>& restore)
{
    const std::string value = std::to_string(id);
    set_transient_attribute(element, kLayoutIdAttribute, value, restore);
}

void restore_transient_attributes(std::vector<TransientAttributeRestore>& restore)
{
    for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
        if (it->element == nullptr) {
            continue;
        }
        if (it->had_attribute) {
            lxb_dom_element_set_attribute(
                it->element,
                reinterpret_cast<const lxb_char_t*>(it->name.data()),
                it->name.size(),
                reinterpret_cast<const lxb_char_t*>(it->value.data()),
                it->value.size());
        } else {
            lxb_dom_element_remove_attribute(
                it->element,
                reinterpret_cast<const lxb_char_t*>(it->name.data()),
                it->name.size());
        }
    }
    restore.clear();
}

std::string append_css_declarations(std::string_view style, std::string_view extra_style)
{
    std::string out(style);
    if (!out.empty() && out.back() != ';') {
        out.push_back(';');
    }
    out.append(extra_style);
    return out;
}

bool is_document_like_node(lxb_dom_node_t* node)
{
    return node != nullptr
        && (node->type == LXB_DOM_NODE_TYPE_DOCUMENT
            || node->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT);
}

bool is_html_tag(lxb_dom_node_t* node, lxb_tag_id_t tag)
{
    return node != nullptr && node->ns == LXB_NS_HTML && node->local_name == tag;
}

bool should_detach_layout_node(lxb_dom_node_t* node, bool omit_js_disabled_content)
{
    if (node == nullptr) {
        return true;
    }
    if (omit_js_disabled_content && is_html_tag(node, LXB_TAG_NOSCRIPT)) {
        return true;
    }
    return node->type == LXB_DOM_NODE_TYPE_TEXT && is_html_tag(node->parent, LXB_TAG_HEAD);
}

void push_children_reverse(std::vector<lxb_dom_node_t*>& stack, lxb_dom_node_t* parent)
{
    for (auto* child = parent == nullptr ? nullptr : parent->last_child; child != nullptr; child = child->prev) {
        stack.push_back(child);
    }
}

struct DetachedLayoutNode {
    lxb_dom_node_t* node = nullptr;
    lxb_dom_node_t* parent = nullptr;
    lxb_dom_node_t* prev = nullptr;
    lxb_dom_node_t* next = nullptr;
};

void collect_layout_detach_nodes(
    lxb_dom_node_t* root,
    bool omit_js_disabled_content,
    std::vector<lxb_dom_node_t*>& out)
{
    std::vector<lxb_dom_node_t*> stack;
    if (is_document_like_node(root)) {
        push_children_reverse(stack, root);
    } else if (root != nullptr) {
        stack.push_back(root);
    }

    while (!stack.empty()) {
        auto* node = stack.back();
        stack.pop_back();
        if (should_detach_layout_node(node, omit_js_disabled_content)) {
            out.push_back(node);
            continue;
        }
        push_children_reverse(stack, node);
    }
}

void detach_layout_nodes(
    const std::vector<lxb_dom_node_t*>& nodes,
    std::vector<DetachedLayoutNode>& detached)
{
    detached.reserve(detached.size() + nodes.size());
    for (auto* node : nodes) {
        if (node == nullptr || node->parent == nullptr) {
            continue;
        }
        detached.push_back(DetachedLayoutNode{node, node->parent, node->prev, node->next});
        lxb_dom_node_remove_wo_events(node);
    }
}

void restore_detached_layout_nodes(std::vector<DetachedLayoutNode>& detached)
{
    for (auto it = detached.rbegin(); it != detached.rend(); ++it) {
        auto* node = it->node;
        auto* parent = it->parent;
        if (node == nullptr || parent == nullptr || node->parent != nullptr) {
            continue;
        }
        if (it->next != nullptr && it->next->parent == parent) {
            lxb_dom_node_insert_before_wo_events(it->next, node);
            continue;
        }
        if (it->prev != nullptr && it->prev->parent == parent) {
            lxb_dom_node_insert_after_wo_events(it->prev, node);
            continue;
        }
        lxb_dom_node_insert_child_wo_events(parent, node);
    }
    detached.clear();
}

struct SelectorResults {
    const DomDocument::Impl* impl;
    std::vector<NodeId> ids;
    bool first_only = false;
};

lxb_status_t selector_callback(lxb_dom_node_t* node, lxb_css_selector_specificity_t, void* ctx)
{
    // This runs as a C callback from Lexbor; a C++ exception unwinding through
    // the C frame would be undefined behavior, so contain it here.
    try {
        auto* results = static_cast<SelectorResults*>(ctx);
        results->ids.push_back(results->impl->id_for(node));
        if (results->first_only) {
            // Stops the tree walk after the first match; Lexbor treats STOP as a
            // clean early exit and lxb_selectors_find still returns LXB_STATUS_OK.
            return LXB_STATUS_STOP;
        }
    } catch (...) {
        return LXB_STATUS_ERROR;
    }
    return LXB_STATUS_OK;
}

} // namespace

DomDocument::Impl::Impl()
{
    document = lxb_html_document_create();
    if (document == nullptr) {
        throw std::runtime_error("failed to create Lexbor document");
    }
    lxb_html_document_scripting_set(document, true);
    layout_mutation_history.push_back(DomDocument::LayoutMutationRecord{
        layout_mutation_version,
        last_layout_mutation_reason,
        last_layout_mutation_node,
    });
}

DomDocument::Impl::~Impl()
{
    for (auto& [text, list] : selector_cache) {
        if (list != nullptr) {
            lxb_css_selector_list_destroy_memory(list);
        }
    }
    selector_cache.clear();

    if (selectors != nullptr) {
        lxb_selectors_destroy(selectors, true);
        selectors = nullptr;
    }

    if (document != nullptr) {
        lxb_html_document_destroy(document);
        document = nullptr;
    }
}

lxb_css_selector_list_t* DomDocument::Impl::compiled_selector(std::string_view selector)
{
    std::string key(selector);
    auto cached = selector_cache.find(key);
    if (cached != selector_cache.end()) {
        return cached->second;
    }

    auto* parser = lxb_css_parser_create();
    if (parser == nullptr) {
        throw std::runtime_error("failed to create CSS parser");
    }
    if (lxb_css_parser_init(parser, nullptr) != LXB_STATUS_OK) {
        lxb_css_parser_destroy(parser, true);
        throw std::runtime_error("failed to initialize CSS parser");
    }

    auto* list = lxb_css_selectors_parse(
        parser,
        reinterpret_cast<const lxb_char_t*>(selector.data()),
        selector.size());
    const bool ok = list != nullptr && parser->status == LXB_STATUS_OK;

    // Destroying the parser leaves the compiled list and its own memory pool
    // intact (lxb_css_parser_destroy does not own list->memory), so the cached
    // list stays valid until the document is destroyed.
    lxb_css_parser_destroy(parser, true);

    if (!ok) {
        if (list != nullptr) {
            lxb_css_selector_list_destroy_memory(list);
        }
        throw std::runtime_error("invalid CSS selector");
    }

    selector_cache.emplace(std::move(key), list);
    return list;
}

std::vector<NodeId> DomDocument::Impl::run_selector(lxb_dom_node_t* root, std::string_view selector, bool first_only)
{
    if (selectors == nullptr) {
        selectors = lxb_selectors_create();
        if (selectors == nullptr || lxb_selectors_init(selectors) != LXB_STATUS_OK) {
            if (selectors != nullptr) {
                lxb_selectors_destroy(selectors, true);
                selectors = nullptr;
            }
            throw std::runtime_error("failed to initialize CSS selector engine");
        }
    }

    auto* list = compiled_selector(selector);

    SelectorResults results{this, {}, first_only};
    lxb_selectors_opt_set(selectors, static_cast<lxb_selectors_opt_t>(
        LXB_SELECTORS_OPT_MATCH_ROOT | LXB_SELECTORS_OPT_MATCH_FIRST));

    const lxb_status_t status = lxb_selectors_find(selectors, root, list, selector_callback, &results);
    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("CSS selector lookup failed");
    }

    return std::move(results.ids);
}

lxb_dom_node_t* DomDocument::Impl::require_node(NodeId id) const
{
    auto it = id_to_node.find(id);
    if (it == id_to_node.end() || it->second == nullptr) {
        throw std::runtime_error("invalid DOM node id");
    }
    return it->second;
}

lxb_dom_element_t* DomDocument::Impl::require_element(NodeId id) const
{
    auto* node = require_node(id);
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        throw std::runtime_error("DOM node is not an element");
    }
    return lxb_dom_interface_element(node);
}

NodeId DomDocument::Impl::id_for(lxb_dom_node_t* node) const
{
    if (node == nullptr) {
        return kInvalidNodeId;
    }

    // The forward map is the node's own user slot: calloc'd to nullptr on
    // creation, and since ids start at 1 (kInvalidNodeId == 0) a null slot
    // unambiguously means "no id yet".
    if (node->user != nullptr) {
        return static_cast<NodeId>(reinterpret_cast<uintptr_t>(node->user));
    }

    const NodeId id = next_id++;
    node->user = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
    id_to_node[id] = node;
    return id;
}

bool DomDocument::Impl::has_node(NodeId id) const
{
    return id != kInvalidNodeId && id_to_node.find(id) != id_to_node.end();
}

void DomDocument::Impl::forget_node(lxb_dom_node_t* node)
{
    if (node == nullptr || node->user == nullptr) {
        return;
    }

    const auto id = static_cast<NodeId>(reinterpret_cast<uintptr_t>(node->user));
    id_to_node.erase(id);
    cached_width_self_blockers.erase(id);
    cached_width_ancestor_blockers.erase(id);
    node->user = nullptr;
    // A previously tracked id just became invalid; signal the wrapper layer.
    ++forget_version;
}

bool DomDocument::Impl::attribute_affects_layout(std::string_view name) const
{
    const std::string lower = ascii_lower(name);
    if (is_service_attribute(lower)) {
        return layout_sensitive_attribute_wildcard
            || layout_sensitive_attributes.find(lower) != layout_sensitive_attributes.end();
    }
    if (is_always_layout_attribute(lower)) {
        return true;
    }

    // Keep ordinary unknown attributes conservative: CSS selectors and resource
    // semantics commonly depend on them, while service metadata churn is the hot
    // path this split is intended to avoid.
    return true;
}

bool DomDocument::Impl::node_affects_layout(lxb_dom_node_t* node) const
{
    if (node == nullptr) {
        return true;
    }
    if (node->type == LXB_DOM_NODE_TYPE_COMMENT) {
        return false;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT && is_html_tag(node->parent, LXB_TAG_HEAD)) {
        return false;
    }
    for (auto* current = node; current != nullptr; current = current->parent) {
        if (is_html_tag(current, LXB_TAG_SCRIPT)) {
            return false;
        }
    }
    return true;
}

void DomDocument::Impl::mark_mutated(bool affects_layout, std::string_view layout_reason, NodeId layout_node)
{
    ++mutation_version;
    if (affects_layout) {
        ++layout_mutation_version;
        last_layout_mutation_reason.assign(layout_reason);
        if (last_layout_mutation_reason.empty()) {
            last_layout_mutation_reason = "unknown";
        }
        last_layout_mutation_node = layout_node;
        layout_mutation_history.push_back(DomDocument::LayoutMutationRecord{
            layout_mutation_version,
            last_layout_mutation_reason,
            last_layout_mutation_node,
        });
        if (layout_node != kInvalidNodeId) {
            if (mutation_blocks_cached_width_for_self(last_layout_mutation_reason)) {
                cached_width_self_blockers[layout_node] = layout_mutation_version;
            }
            if (mutation_blocks_cached_width_for_descendants(last_layout_mutation_reason)) {
                cached_width_ancestor_blockers[layout_node] = layout_mutation_version;
            }
        }
        if (layout_mutation_history.size() > kMaxLayoutMutationHistory) {
            layout_mutation_history.erase(layout_mutation_history.begin());
        }
    }
}

void DomDocument::Impl::clear_user_data_subtree(lxb_dom_node_t* node)
{
    // Iterative pre-order walk (matching the other traversals here) so a deep
    // cloned subtree cannot overflow the native stack. lxb_dom_node_clone copies
    // the user slot for the node and every descendant, so each must be reset to
    // nullptr before id_for assigns fresh ids.
    if (node == nullptr) {
        return;
    }

    std::vector<lxb_dom_node_t*> stack;
    stack.push_back(node);
    while (!stack.empty()) {
        auto* current = stack.back();
        stack.pop_back();
        current->user = nullptr;
        for (auto* child = current->first_child; child != nullptr; child = child->next) {
            stack.push_back(child);
        }
    }
}

DomDocument::DomDocument()
    : impl_(new Impl())
{
}

DomDocument::~DomDocument()
{
    delete impl_;
}

DomDocument::DomDocument(DomDocument&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
{
}

DomDocument& DomDocument::operator=(DomDocument&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

void DomDocument::parse(std::string_view html)
{
    // Carry the id counter and version counters forward across reparse so node
    // ids from a previous document are never reused, the monotonic mutation
    // version keeps invalidating any stale wrappers held elsewhere, and the
    // forget version bumps so the wrapper layer prunes ids from the old document.
    NodeId carried_next_id = 1;
    std::uint64_t carried_mutation_version = 1;
    std::uint64_t carried_layout_mutation_version = 1;
    std::uint64_t carried_forget_version = 1;
    if (impl_ != nullptr) {
        carried_next_id = impl_->next_id;
        carried_mutation_version = impl_->mutation_version;
        carried_layout_mutation_version = impl_->layout_mutation_version;
        carried_forget_version = impl_->forget_version;
    }

    delete impl_;
    impl_ = new Impl();
    impl_->next_id = carried_next_id;
    impl_->mutation_version = carried_mutation_version + 1;
    impl_->layout_mutation_version = carried_layout_mutation_version + 1;
    impl_->last_layout_mutation_reason = "parse";
    impl_->last_layout_mutation_node = kInvalidNodeId;
    impl_->layout_mutation_history.clear();
    impl_->layout_mutation_history.push_back(DomDocument::LayoutMutationRecord{
        impl_->layout_mutation_version,
        impl_->last_layout_mutation_reason,
        impl_->last_layout_mutation_node,
    });
    impl_->forget_version = carried_forget_version + 1;

    const auto status = lxb_html_document_parse(
        impl_->document,
        reinterpret_cast<const lxb_char_t*>(html.data()),
        html.size());

    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("Lexbor failed to parse HTML");
    }

    (void) document_node();
    (void) document_element();
    (void) head();
    (void) body();
}

NodeId DomDocument::document_node()
{
    return impl_->id_for(lxb_dom_interface_node(impl_->document));
}

NodeId DomDocument::document_element()
{
    auto* element = impl_->document->dom_document.element;
    return impl_->id_for(element == nullptr ? nullptr : lxb_dom_interface_node(element));
}

NodeId DomDocument::head()
{
    auto* element = lxb_html_document_head_element(impl_->document);
    return impl_->id_for(element == nullptr ? nullptr : lxb_dom_interface_node(element));
}

NodeId DomDocument::body()
{
    auto* element = lxb_html_document_body_element(impl_->document);
    return impl_->id_for(element == nullptr ? nullptr : lxb_dom_interface_node(element));
}

NodeId DomDocument::create_element(std::string_view tag_name)
{
    auto* element = lxb_html_document_create_element(
        impl_->document,
        reinterpret_cast<const lxb_char_t*>(tag_name.data()),
        tag_name.size(),
        nullptr);

    if (element == nullptr) {
        throw std::runtime_error("failed to create DOM element");
    }

    return impl_->id_for(lxb_dom_interface_node(element));
}

NodeId DomDocument::create_text_node(std::string_view text)
{
    auto* node = lxb_dom_document_create_text_node(
        &impl_->document->dom_document,
        reinterpret_cast<const lxb_char_t*>(text.data()),
        text.size());

    if (node == nullptr) {
        throw std::runtime_error("failed to create DOM text node");
    }

    return impl_->id_for(lxb_dom_interface_node(node));
}

NodeId DomDocument::create_comment(std::string_view text)
{
    auto* node = lxb_dom_document_create_comment(
        &impl_->document->dom_document,
        reinterpret_cast<const lxb_char_t*>(text.data()),
        text.size());

    if (node == nullptr) {
        throw std::runtime_error("failed to create DOM comment node");
    }

    return impl_->id_for(lxb_dom_interface_node(node));
}

int DomDocument::node_type(NodeId id) const
{
    return static_cast<int>(impl_->require_node(id)->type);
}

std::string DomDocument::node_name(NodeId id) const
{
    size_t len = 0;
    const auto* name = lxb_dom_node_name(impl_->require_node(id), &len);
    return to_string(name, len);
}

std::string DomDocument::tag_name(NodeId id) const
{
    auto* element = impl_->require_element(id);
    size_t len = 0;
    const auto* name = lxb_dom_element_tag_name(element, &len);
    return to_string(name, len);
}

NodeId DomDocument::parent_node(NodeId id)
{
    return impl_->id_for(impl_->require_node(id)->parent);
}

std::vector<NodeId> DomDocument::child_nodes(NodeId id)
{
    std::vector<NodeId> ids;
    for (auto* child = impl_->require_node(id)->first_child; child != nullptr; child = child->next) {
        ids.push_back(impl_->id_for(child));
    }
    return ids;
}

std::vector<NodeId> DomDocument::children(NodeId id)
{
    std::vector<NodeId> ids;
    for (auto* child = impl_->require_node(id)->first_child; child != nullptr; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            ids.push_back(impl_->id_for(child));
        }
    }
    return ids;
}

bool DomDocument::has_node(NodeId id) const
{
    return impl_ != nullptr && impl_->has_node(id);
}

bool DomDocument::contains(NodeId root, NodeId candidate) const
{
    if (!has_node(root) || !has_node(candidate)) {
        return false;
    }

    auto* root_node = impl_->require_node(root);
    for (auto* node = impl_->require_node(candidate); node != nullptr; node = node->parent) {
        if (node == root_node) {
            return true;
        }
    }

    return false;
}

bool DomDocument::is_connected(NodeId id) const
{
    if (!has_node(id)) {
        return false;
    }

    auto* document = lxb_dom_interface_node(impl_->document);
    for (auto* node = impl_->require_node(id); node != nullptr; node = node->parent) {
        if (node == document) {
            return true;
        }
    }

    return false;
}

std::uint64_t DomDocument::mutation_version() const
{
    return impl_ == nullptr ? 0 : impl_->mutation_version;
}

std::uint64_t DomDocument::layout_mutation_version() const
{
    return impl_ == nullptr ? 0 : impl_->layout_mutation_version;
}

std::string DomDocument::last_layout_mutation_reason() const
{
    return impl_ == nullptr ? std::string() : impl_->last_layout_mutation_reason;
}

NodeId DomDocument::last_layout_mutation_node() const
{
    return impl_ == nullptr ? kInvalidNodeId : impl_->last_layout_mutation_node;
}

std::vector<DomDocument::LayoutMutationRecord> DomDocument::layout_mutations_since(std::uint64_t version) const
{
    std::vector<LayoutMutationRecord> records;
    if (impl_ == nullptr) {
        return records;
    }
    for (const auto& record : impl_->layout_mutation_history) {
        if (record.version > version) {
            records.push_back(record);
        }
    }
    return records;
}

std::uint64_t DomDocument::cached_width_self_blocking_layout_mutation_version(NodeId id) const
{
    if (impl_ == nullptr) {
        return 0;
    }
    const auto found = impl_->cached_width_self_blockers.find(id);
    return found == impl_->cached_width_self_blockers.end() ? 0 : found->second;
}

std::uint64_t DomDocument::cached_width_ancestor_blocking_layout_mutation_version(NodeId id) const
{
    if (impl_ == nullptr) {
        return 0;
    }
    const auto found = impl_->cached_width_ancestor_blockers.find(id);
    return found == impl_->cached_width_ancestor_blockers.end() ? 0 : found->second;
}

void DomDocument::set_layout_sensitive_attributes(std::vector<std::string> attribute_names, bool wildcard)
{
    if (impl_ == nullptr) {
        return;
    }
    impl_->layout_sensitive_attributes.clear();
    impl_->layout_sensitive_attribute_wildcard = wildcard;
    for (const auto& name : attribute_names) {
        const std::string lower = ascii_lower(name);
        if (!lower.empty()) {
            impl_->layout_sensitive_attributes.insert(lower);
        }
    }
}

std::uint64_t DomDocument::forget_version() const
{
    return impl_ == nullptr ? 0 : impl_->forget_version;
}

std::optional<std::string> DomDocument::get_attribute(NodeId id, std::string_view name) const
{
    auto* element = impl_->require_element(id);
    size_t len = 0;
    const auto* value = lxb_dom_element_get_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        &len);

    if (value == nullptr) {
        return std::nullopt;
    }
    return to_string(value, len);
}

bool DomDocument::has_attribute(NodeId id, std::string_view name) const
{
    return lxb_dom_element_has_attribute(
        impl_->require_element(id),
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size());
}

std::vector<DomDocument::Attribute> DomDocument::attributes(NodeId id) const
{
    std::vector<Attribute> out;
    auto* element = impl_->require_element(id);

    for (auto* attr = lxb_dom_element_first_attribute(element);
         attr != nullptr;
         attr = lxb_dom_element_next_attribute(attr)) {
        size_t name_len = 0;
        const auto* name = lxb_dom_attr_qualified_name(attr, &name_len);

        size_t value_len = 0;
        const auto* value = lxb_dom_attr_value(attr, &value_len);

        out.push_back(Attribute{
            to_string(name, name_len),
            to_string(value, value_len),
        });
    }

    return out;
}

void DomDocument::set_attribute(NodeId id, std::string_view name, std::string_view value)
{
    auto* element = impl_->require_element(id);
    auto* attr = lxb_dom_element_set_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        reinterpret_cast<const lxb_char_t*>(value.data()),
        value.size());

    if (attr == nullptr) {
        throw std::runtime_error("failed to set DOM attribute");
    }

    impl_->mark_mutated(
        impl_->node_affects_layout(lxb_dom_interface_node(element)) && impl_->attribute_affects_layout(name),
        "set_attribute:" + ascii_lower(name),
        id);
}

void DomDocument::remove_attribute(NodeId id, std::string_view name)
{
    const bool had_attribute = has_attribute(id, name);
    auto* element = impl_->require_element(id);
    const auto status = lxb_dom_element_remove_attribute(
        element,
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size());

    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("failed to remove DOM attribute");
    }

    if (had_attribute) {
        impl_->mark_mutated(
            impl_->node_affects_layout(lxb_dom_interface_node(element)) && impl_->attribute_affects_layout(name),
            "remove_attribute:" + ascii_lower(name),
            id);
    }
}

std::string DomDocument::text_content(NodeId id) const
{
    auto* node = impl_->require_node(id);
    if (is_character_data(node)) {
        return character_data(node);
    }

    std::string out;
    append_descendant_text(node, out);
    return out;
}

void DomDocument::set_text_content(NodeId id, std::string_view value)
{
    auto* node = impl_->require_node(id);
    if (is_character_data(node)) {
        auto* data = lxb_dom_interface_character_data(node);
        const auto status = lxb_dom_character_data_replace(
            data,
            reinterpret_cast<const lxb_char_t*>(value.data()),
            value.size(),
            0,
            data->data.length);

        if (status != LXB_STATUS_OK) {
            throw std::runtime_error("failed to replace character data");
        }
        impl_->mark_mutated(impl_->node_affects_layout(node), "set_text_content", id);
        return;
    }

    const bool had_children = node->first_child != nullptr;
    while (node->first_child != nullptr) {
        lxb_dom_node_remove(node->first_child);
    }

    if (!value.empty()) {
        const NodeId text = create_text_node(value);
        append_child(id, text);
    } else if (had_children) {
        impl_->mark_mutated(impl_->node_affects_layout(node), "set_text_content", id);
    }
}

std::string DomDocument::inner_html(NodeId id) const
{
    std::string out;
    auto* node = impl_->require_node(id);
    for (auto* child = node->first_child; child != nullptr; child = child->next) {
        const auto status = lxb_html_serialize_tree_cb(child, append_serialized, &out);
        if (status != LXB_STATUS_OK) {
            throw std::runtime_error("failed to serialize DOM innerHTML");
        }
    }
    return out;
}

void DomDocument::set_inner_html(NodeId id, std::string_view html)
{
    auto* node = impl_->require_node(id);
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        set_text_content(id, html);
        return;
    }

    std::vector<lxb_dom_node_t*> previous_nodes;
    for (auto* child = node->first_child; child != nullptr; child = child->next) {
        collect_subtree(child, previous_nodes);
    }

    auto* result = lxb_html_element_inner_html_set(
        lxb_html_interface_element(lxb_dom_interface_element(node)),
        reinterpret_cast<const lxb_char_t*>(html.data()),
        html.size());

    if (result == nullptr) {
        throw std::runtime_error("failed to set DOM innerHTML");
    }

    for (auto* previous : previous_nodes) {
        impl_->forget_node(previous);
    }
    impl_->mark_mutated(impl_->node_affects_layout(node), "set_inner_html", id);
}

std::string DomDocument::outer_html(NodeId id) const
{
    std::string out;
    const auto status = lxb_html_serialize_tree_cb(impl_->require_node(id), append_serialized, &out);
    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("failed to serialize DOM outerHTML");
    }
    return out;
}

std::string DomDocument::serialize_html() const
{
    std::string out;
    const auto status = lxb_html_serialize_tree_cb(
        lxb_dom_interface_node(impl_->document),
        append_serialized,
        &out);

    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("failed to serialize HTML document");
    }
    return out;
}

std::string DomDocument::serialize_html_for_layout(
    bool omit_js_disabled_content,
    const std::vector<LayoutStyleOverride>& style_overrides) const
{
    std::vector<lxb_dom_node_t*> nodes;
    collect_subtree(lxb_dom_interface_node(impl_->document), nodes);

    std::string html;
    std::vector<TransientAttributeRestore> transient_restore;
    std::vector<lxb_dom_node_t*> detach_nodes;
    std::vector<DetachedLayoutNode> detached;

    try {
        for (auto* node : nodes) {
            if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                continue;
            }
            set_layout_id_attribute(lxb_dom_interface_element(node), impl_->id_for(node), transient_restore);
        }

        for (const auto& override : style_overrides) {
            const auto found = impl_->id_to_node.find(override.node);
            if (found == impl_->id_to_node.end()
                || found->second == nullptr
                || found->second->type != LXB_DOM_NODE_TYPE_ELEMENT
                || override.extra_style.empty()) {
                continue;
            }

            auto* element = lxb_dom_interface_element(found->second);
            size_t old_len = 0;
            const auto* old_style = lxb_dom_element_get_attribute(
                element,
                reinterpret_cast<const lxb_char_t*>("style"),
                5,
                &old_len);
            const std::string merged_style = append_css_declarations(
                old_style == nullptr ? std::string_view() : std::string_view(reinterpret_cast<const char*>(old_style), old_len),
                override.extra_style);
            set_transient_attribute(element, "style", merged_style, transient_restore);
        }

        collect_layout_detach_nodes(
            lxb_dom_interface_node(impl_->document),
            omit_js_disabled_content,
            detach_nodes);
        detach_layout_nodes(detach_nodes, detached);

        const auto status = lxb_html_serialize_tree_cb(
            lxb_dom_interface_node(impl_->document),
            append_serialized,
            &html);
        if (status != LXB_STATUS_OK) {
            throw std::runtime_error("failed to serialize layout HTML document");
        }
    } catch (...) {
        restore_detached_layout_nodes(detached);
        restore_transient_attributes(transient_restore);
        throw;
    }

    restore_detached_layout_nodes(detached);
    restore_transient_attributes(transient_restore);

    return html;
}

NodeId DomDocument::append_child(NodeId parent, NodeId child)
{
    auto* parent_node = impl_->require_node(parent);
    auto* child_node = impl_->require_node(child);
    const bool affects_layout = impl_->node_affects_layout(parent_node) && impl_->node_affects_layout(child_node);
    const auto status = lxb_dom_node_append_child(parent_node, child_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to append DOM child");
    }
    const NodeId child_id = impl_->id_for(child_node);
    impl_->mark_mutated(affects_layout, "append_child", child_id);
    return child_id;
}

NodeId DomDocument::insert_before(NodeId parent, NodeId child, NodeId reference_child)
{
    if (reference_child == kInvalidNodeId) {
        return append_child(parent, child);
    }

    auto* parent_node = impl_->require_node(parent);
    auto* child_node = impl_->require_node(child);
    auto* reference_node = impl_->require_node(reference_child);
    const bool affects_layout = impl_->node_affects_layout(parent_node) && impl_->node_affects_layout(child_node);
    const auto status = lxb_dom_node_insert_before_spec(
        parent_node,
        child_node,
        reference_node);

    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to insert DOM child");
    }

    const NodeId child_id = impl_->id_for(child_node);
    impl_->mark_mutated(affects_layout, "insert_before", child_id);
    return child_id;
}

NodeId DomDocument::remove_child(NodeId parent, NodeId child)
{
    auto* parent_node = impl_->require_node(parent);
    auto* child_node = impl_->require_node(child);
    const bool affects_layout = impl_->node_affects_layout(parent_node) && impl_->node_affects_layout(child_node);
    const auto status = lxb_dom_node_remove_child(parent_node, child_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to remove DOM child");
    }
    const NodeId child_id = impl_->id_for(child_node);
    impl_->mark_mutated(affects_layout, "remove_child", child_id);
    return child_id;
}

NodeId DomDocument::replace_child(NodeId parent, NodeId child, NodeId replaced_child)
{
    auto* parent_node = impl_->require_node(parent);
    auto* child_node = impl_->require_node(child);
    auto* replaced_node = impl_->require_node(replaced_child);
    const bool affects_layout = impl_->node_affects_layout(parent_node)
        && (impl_->node_affects_layout(child_node) || impl_->node_affects_layout(replaced_node));
    const auto status = lxb_dom_node_replace_child(parent_node, child_node, replaced_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to replace DOM child");
    }

    const NodeId child_id = impl_->id_for(child_node);
    const NodeId replaced_id = impl_->id_for(replaced_node);
    impl_->mark_mutated(affects_layout, "replace_child", child_id);
    return replaced_id;
}

NodeId DomDocument::clone_node(NodeId id, bool deep)
{
    auto* cloned = lxb_dom_node_clone(impl_->require_node(id), deep);
    if (cloned == nullptr) {
        throw std::runtime_error("failed to clone DOM node");
    }

    // lxb_dom_node_clone copies the user slot (id) from the source onto the
    // clone and, for a deep clone, every descendant. Reset the whole subtree so
    // the clones get fresh ids instead of aliasing the originals'.
    Impl::clear_user_data_subtree(cloned);

    return impl_->id_for(cloned);
}

std::vector<NodeId> DomDocument::query_selector_all(NodeId root, std::string_view selector)
{
    return impl_->run_selector(impl_->require_node(root), selector, /*first_only=*/false);
}

NodeId DomDocument::query_selector(NodeId root, std::string_view selector)
{
    auto ids = impl_->run_selector(impl_->require_node(root), selector, /*first_only=*/true);
    if (ids.empty()) {
        return kInvalidNodeId;
    }
    return ids.front();
}

NodeId DomDocument::get_element_by_id(std::string_view id)
{
    const NodeId root_id = document_element();
    if (root_id == kInvalidNodeId) {
        return kInvalidNodeId;
    }

    auto* found = lxb_dom_element_by_id(
        impl_->require_element(root_id),
        reinterpret_cast<const lxb_char_t*>(id.data()),
        id.size());

    return impl_->id_for(found == nullptr ? nullptr : lxb_dom_interface_node(found));
}

} // namespace pagecore

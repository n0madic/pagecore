#include "dom_document.hpp"

#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/serialize.h>

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

struct SelectorResults {
    const DomDocument::Impl* impl;
    std::vector<NodeId> ids;
};

lxb_status_t selector_callback(lxb_dom_node_t* node, lxb_css_selector_specificity_t, void* ctx)
{
    // This runs as a C callback from Lexbor; a C++ exception unwinding through
    // the C frame would be undefined behavior, so contain it here.
    try {
        auto* results = static_cast<SelectorResults*>(ctx);
        results->ids.push_back(results->impl->id_for(node));
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
}

DomDocument::Impl::~Impl()
{
    if (document != nullptr) {
        lxb_html_document_destroy(document);
        document = nullptr;
    }
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

    auto existing = node_to_id.find(node);
    if (existing != node_to_id.end()) {
        return existing->second;
    }

    const NodeId id = next_id++;
    node_to_id[node] = id;
    id_to_node[id] = node;
    return id;
}

bool DomDocument::Impl::has_node(NodeId id) const
{
    return id != kInvalidNodeId && id_to_node.find(id) != id_to_node.end();
}

void DomDocument::Impl::forget_node(lxb_dom_node_t* node)
{
    auto it = node_to_id.find(node);
    if (it != node_to_id.end()) {
        id_to_node.erase(it->second);
        node_to_id.erase(it);
    }
}

void DomDocument::Impl::forget_subtree(lxb_dom_node_t* node)
{
    if (node == nullptr) {
        return;
    }

    std::vector<lxb_dom_node_t*> stack;
    stack.push_back(node);
    while (!stack.empty()) {
        auto* current = stack.back();
        stack.pop_back();
        for (auto* child = current->first_child; child != nullptr; child = child->next) {
            stack.push_back(child);
        }
        forget_node(current);
    }
}

void DomDocument::Impl::mark_mutated()
{
    ++mutation_version;
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
    // Carry the id counter and mutation version forward across reparse so node
    // ids from a previous document are never reused and the monotonic mutation
    // version keeps invalidating any stale wrappers held elsewhere.
    NodeId carried_next_id = 1;
    std::uint64_t carried_mutation_version = 1;
    if (impl_ != nullptr) {
        carried_next_id = impl_->next_id;
        carried_mutation_version = impl_->mutation_version;
    }

    delete impl_;
    impl_ = new Impl();
    impl_->next_id = carried_next_id;
    impl_->mutation_version = carried_mutation_version + 1;

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
    auto* attr = lxb_dom_element_set_attribute(
        impl_->require_element(id),
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size(),
        reinterpret_cast<const lxb_char_t*>(value.data()),
        value.size());

    if (attr == nullptr) {
        throw std::runtime_error("failed to set DOM attribute");
    }

    impl_->mark_mutated();
}

void DomDocument::remove_attribute(NodeId id, std::string_view name)
{
    const bool had_attribute = has_attribute(id, name);
    const auto status = lxb_dom_element_remove_attribute(
        impl_->require_element(id),
        reinterpret_cast<const lxb_char_t*>(name.data()),
        name.size());

    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("failed to remove DOM attribute");
    }

    if (had_attribute) {
        impl_->mark_mutated();
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
        impl_->mark_mutated();
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
        impl_->mark_mutated();
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
    impl_->mark_mutated();
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

NodeId DomDocument::append_child(NodeId parent, NodeId child)
{
    auto* child_node = impl_->require_node(child);
    const auto status = lxb_dom_node_append_child(impl_->require_node(parent), child_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to append DOM child");
    }
    impl_->mark_mutated();
    return impl_->id_for(child_node);
}

NodeId DomDocument::insert_before(NodeId parent, NodeId child, NodeId reference_child)
{
    if (reference_child == kInvalidNodeId) {
        return append_child(parent, child);
    }

    auto* child_node = impl_->require_node(child);
    auto* reference_node = impl_->require_node(reference_child);
    const auto status = lxb_dom_node_insert_before_spec(
        impl_->require_node(parent),
        child_node,
        reference_node);

    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to insert DOM child");
    }

    impl_->mark_mutated();
    return impl_->id_for(child_node);
}

NodeId DomDocument::remove_child(NodeId parent, NodeId child)
{
    auto* child_node = impl_->require_node(child);
    const auto status = lxb_dom_node_remove_child(impl_->require_node(parent), child_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to remove DOM child");
    }
    impl_->mark_mutated();
    return impl_->id_for(child_node);
}

NodeId DomDocument::replace_child(NodeId parent, NodeId child, NodeId replaced_child)
{
    auto* child_node = impl_->require_node(child);
    auto* replaced_node = impl_->require_node(replaced_child);
    const auto status = lxb_dom_node_replace_child(impl_->require_node(parent), child_node, replaced_node);
    if (status != LXB_DOM_EXCEPTION_OK) {
        throw std::runtime_error("failed to replace DOM child");
    }

    impl_->mark_mutated();
    return impl_->id_for(replaced_node);
}

NodeId DomDocument::clone_node(NodeId id, bool deep)
{
    auto* cloned = lxb_dom_node_clone(impl_->require_node(id), deep);
    if (cloned == nullptr) {
        throw std::runtime_error("failed to clone DOM node");
    }

    return impl_->id_for(cloned);
}

std::vector<NodeId> DomDocument::query_selector_all(NodeId root, std::string_view selector)
{
    auto* parser = lxb_css_parser_create();
    if (parser == nullptr) {
        throw std::runtime_error("failed to create CSS parser");
    }

    auto* selectors = lxb_selectors_create();
    if (selectors == nullptr) {
        lxb_css_parser_destroy(parser, true);
        throw std::runtime_error("failed to create CSS selector engine");
    }

    lxb_status_t status = lxb_css_parser_init(parser, nullptr);
    if (status != LXB_STATUS_OK) {
        lxb_selectors_destroy(selectors, true);
        lxb_css_parser_destroy(parser, true);
        throw std::runtime_error("failed to initialize CSS parser");
    }

    status = lxb_selectors_init(selectors);
    if (status != LXB_STATUS_OK) {
        lxb_selectors_destroy(selectors, true);
        lxb_css_parser_destroy(parser, true);
        throw std::runtime_error("failed to initialize CSS selector engine");
    }

    auto* list = lxb_css_selectors_parse(
        parser,
        reinterpret_cast<const lxb_char_t*>(selector.data()),
        selector.size());

    if (list == nullptr || parser->status != LXB_STATUS_OK) {
        lxb_selectors_destroy(selectors, true);
        lxb_css_parser_destroy(parser, true);
        throw std::runtime_error("invalid CSS selector");
    }

    SelectorResults results{impl_, {}};
    lxb_selectors_opt_set(selectors, static_cast<lxb_selectors_opt_t>(
        LXB_SELECTORS_OPT_MATCH_ROOT | LXB_SELECTORS_OPT_MATCH_FIRST));

    status = lxb_selectors_find(selectors, impl_->require_node(root), list, selector_callback, &results);

    lxb_css_selector_list_destroy_memory(list);
    lxb_selectors_destroy(selectors, true);
    lxb_css_parser_destroy(parser, true);

    if (status != LXB_STATUS_OK) {
        throw std::runtime_error("CSS selector lookup failed");
    }

    return results.ids;
}

NodeId DomDocument::query_selector(NodeId root, std::string_view selector)
{
    auto ids = query_selector_all(root, selector);
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

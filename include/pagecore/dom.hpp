#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagecore {

using NodeId = std::uint32_t;

inline constexpr NodeId kInvalidNodeId = 0;

class DomDocument {
public:
    struct Impl;

    struct Attribute {
        std::string name;
        std::string value;
    };

    DomDocument();
    ~DomDocument();

    DomDocument(const DomDocument&) = delete;
    DomDocument& operator=(const DomDocument&) = delete;

    DomDocument(DomDocument&&) noexcept;
    DomDocument& operator=(DomDocument&&) noexcept;

    void parse(std::string_view html);

    NodeId document_node();
    NodeId document_element();
    NodeId head();
    NodeId body();

    NodeId create_element(std::string_view tag_name);
    NodeId create_text_node(std::string_view text);
    NodeId create_comment(std::string_view text);

    int node_type(NodeId id) const;
    std::string node_name(NodeId id) const;
    std::string tag_name(NodeId id) const;

    NodeId parent_node(NodeId id);
    std::vector<NodeId> child_nodes(NodeId id);
    std::vector<NodeId> children(NodeId id);
    bool has_node(NodeId id) const;
    bool contains(NodeId root, NodeId candidate) const;
    bool is_connected(NodeId id) const;
    std::uint64_t mutation_version() const;
    std::uint64_t layout_mutation_version() const;
    // Monotonic counter bumped only when a node id is invalidated (forgotten via
    // innerHTML replacement) or the document is reparsed. Wrapper layers use it
    // to know when a cached id may have become stale.
    std::uint64_t forget_version() const;

    std::optional<std::string> get_attribute(NodeId id, std::string_view name) const;
    bool has_attribute(NodeId id, std::string_view name) const;
    std::vector<Attribute> attributes(NodeId id) const;
    void set_attribute(NodeId id, std::string_view name, std::string_view value);
    void remove_attribute(NodeId id, std::string_view name);

    std::string text_content(NodeId id) const;
    void set_text_content(NodeId id, std::string_view value);

    std::string inner_html(NodeId id) const;
    void set_inner_html(NodeId id, std::string_view html);
    std::string outer_html(NodeId id) const;
    std::string serialize_html() const;
    // Like serialize_html(), but tags every live element with a transient
    // data-pc-sid="<NodeId>" attribute first, so a downstream HTML consumer
    // (e.g. a layout engine) can be queried back by NodeId. The attribute is
    // removed before this call returns and never bumps mutation_version().
    // When omit_js_disabled_content is true, the layout serialization also
    // skips <noscript> subtrees and direct text children of <head>, matching the
    // rendered DOM that JavaScript-enabled pages should expose to litehtml.
    std::string serialize_html_for_layout(bool omit_js_disabled_content = false) const;

    NodeId append_child(NodeId parent, NodeId child);
    NodeId insert_before(NodeId parent, NodeId child, NodeId reference_child);
    NodeId remove_child(NodeId parent, NodeId child);
    NodeId replace_child(NodeId parent, NodeId child, NodeId replaced_child);
    NodeId clone_node(NodeId id, bool deep);

    NodeId query_selector(NodeId root, std::string_view selector);
    std::vector<NodeId> query_selector_all(NodeId root, std::string_view selector);
    NodeId get_element_by_id(std::string_view id);

private:
    Impl* impl_ = nullptr;
};

} // namespace pagecore

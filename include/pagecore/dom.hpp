#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagecore {

using NodeId = std::uint32_t;

inline constexpr NodeId kInvalidNodeId = 0;

// One layout-affecting DOM mutation, recorded in the ring buffer read back by
// layout_mutations_since(). Exactly one record exists per layout_mutation_version
// bump. Only InlineStyle records carry the old/new style attribute values;
// every other layout mutation is Kind::Other with just the version and node.
struct LayoutMutationRecord {
    enum class Kind { InlineStyle, Other };
    Kind kind = Kind::Other;
    std::uint64_t layout_mutation_version = 0;  // the version after this mutation's bump
    NodeId node = kInvalidNodeId;
    std::string old_value;  // InlineStyle only: style attribute before the write
    std::string new_value;  // InlineStyle only: style attribute after the write ("" = removed)
    bool had_old_value = false;
    bool has_new_value = false;
};

// The layout mutations that happened after a caller's known version. `complete`
// is false when the ring buffer has evicted records the caller would need
// (i.e. `since_version` predates the retained window), in which case `records`
// holds only the still-retained suffix and the caller must fall back to a full
// rebuild.
struct LayoutMutationJournal {
    bool complete = false;
    std::vector<LayoutMutationRecord> records;
};

class DomDocument {
public:
    struct Impl;

    struct Attribute {
        std::string name;
        std::string value;
    };

    struct LayoutStyleOverride {
        NodeId node = kInvalidNodeId;
        std::string extra_style;
    };

    enum class QuirksMode { NoQuirks, Quirks, LimitedQuirks };

    // Receives the layout view of the document from visit_layout_tree(): the
    // same set of nodes serialize_html_for_layout() emits, without the
    // serialize/re-parse round trip. enter/leave calls are balanced per
    // element; text_run receives coalesced adjacent character data (raw_run is
    // true inside <script>, whose text must not be whitespace-split).
    class LayoutTreeVisitor {
    public:
        virtual ~LayoutTreeVisitor() = default;
        virtual void enter_element(NodeId id, std::string_view tag_name,
                                   const std::vector<Attribute>& attributes) = 0;
        virtual void leave_element(NodeId id) = 0;
        virtual void text_run(std::string_view text, bool raw_run) = 0;
        virtual void comment(std::string_view text) = 0;
    };

    DomDocument();
    ~DomDocument();

    DomDocument(const DomDocument&) = delete;
    DomDocument& operator=(const DomDocument&) = delete;

    DomDocument(DomDocument&&) noexcept;
    DomDocument& operator=(DomDocument&&) noexcept;

    void parse(std::string_view html);

    // Sets the HTML parser scripting flag for subsequent parse() calls. When
    // false, <noscript> content is parsed as real elements — the fallback
    // content real browsers lay out with JavaScript disabled — instead of one
    // raw text node. Defaults to true.
    void set_scripting_enabled(bool enabled);

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

    // Monotonic counter bumped whenever a mutation changes the set of CSS rules
    // the cascade sees (a <style>/<link>/<base> subtree change). Folded into
    // layout_input_digest() so any stylesheet change invalidates every cached
    // computed style. Conservative over-bumping is acceptable (perf only).
    std::uint64_t stylesheet_generation() const;

    // A cheap, allocation-free rolling hash (FNV-1a) over a conservative SUPERSET
    // of every DOM-derived input to node `id`'s CSS cascade, computed directly
    // from Lexbor without any serialize/parse/cascade. For `id` and each ancestor
    // it folds tag/id/class/inline-style and layout-relevant presentational and
    // selector-referenced attributes; at every level the full ordered sibling
    // context (each sibling's tag/id/class/selector attributes and this level's
    // index/count) plus child-presence; the global stylesheet_generation(); and
    // the viewport. Property: identical digest => identical cascade result for
    // `id` => identical computed style. A digest *collision* would be a
    // correctness bug and is avoided by folding a superset with length-prefixed
    // fields; a digest *miss* only costs one extra exact rebuild.
    std::uint64_t layout_input_digest(
        NodeId id,
        int viewport_width,
        int viewport_height,
        float device_scale_factor) const;

    // Per-node layout dirty epochs, keyed by layout_mutation_version. `self`
    // records the version of the most recent mutation to the node itself;
    // `subtree` records the most recent mutation anywhere in the node's subtree
    // (including the node itself). Used as an approximate, image-isolated gate
    // for cross-version geometry read-back (never for the final render).
    std::uint64_t self_dirty_layout_version(NodeId id) const;
    std::uint64_t subtree_dirty_layout_version(NodeId id) const;

    void set_layout_sensitive_attributes(std::vector<std::string> attribute_names, bool wildcard = false);

    // The layout-affecting mutations recorded after `since_version` (exclusive).
    // Used to decide whether a batch of DOM changes can be applied to an
    // already-built layout document incrementally instead of rebuilding it.
    LayoutMutationJournal layout_mutations_since(std::uint64_t since_version) const;

    // True when `name` is referenced by an attribute selector in the current
    // stylesheets (i.e. registered via set_layout_sensitive_attributes or the
    // wildcard). An inline-style change is only safe to patch in isolation when
    // "style" is NOT layout-sensitive, since a [style...] selector could make it
    // change other elements' matches.
    bool is_layout_sensitive_attribute(std::string_view name) const;
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
    std::string serialize_html_for_layout(
        bool omit_js_disabled_content = false,
        const std::vector<LayoutStyleOverride>& style_overrides = {}) const;

    // Pre-order walk over the layout view of the document: the node set a
    // layout engine renders (same <noscript>/head-text detach rules as
    // serialize_html_for_layout()), with style_overrides merged into the
    // reported "style" attribute value. Adjacent text nodes — including ones
    // separated only by detached nodes — are coalesced into one text_run,
    // mirroring what a serialize/re-parse round trip would produce. Inert
    // <template> subtrees are skipped entirely (browsers never lay them out)
    // but still break text-run coalescing like any element boundary. Doctype
    // and processing instructions are skipped; comments are reported. The
    // document is not mutated and no version counters move.
    void visit_layout_tree(
        LayoutTreeVisitor& visitor,
        bool omit_js_disabled_content = false,
        const std::vector<LayoutStyleOverride>& style_overrides = {}) const;

    // Compatibility mode the document was parsed in (from the doctype).
    QuirksMode quirks_mode() const;

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

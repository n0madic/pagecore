#pragma once

#include "pagecore/dom.hpp"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagecore {

struct DomDocument::Impl {
    // Per-node layout dirty epoch (see DomDocument::self/subtree_dirty_layout_version).
    struct DirtyEpoch {
        std::uint64_t self_version = 0;
        std::uint64_t subtree_version = 0;
    };

    // Memoized per-parent sibling context, shared by all children of one level so
    // a wide list pays a single O(breadth) scan instead of O(breadth^2). Keyed by
    // layout_mutation_version: the whole cache is dropped when the version moves.
    struct LevelContext {
        // child NodeId -> hash folding the ordered sibling list identity together
        // with this child's index and the sibling count.
        std::unordered_map<NodeId, std::uint64_t> child_context;
    };

    lxb_html_document_t* document = nullptr;
    // Reverse map only: NodeId -> node. The forward direction (node -> id) lives
    // in each node's own lxb_dom_node_t.user slot (see id_for). The reverse map
    // stays because it is the validation boundary for JS-supplied ids and there
    // is no per-node structure for the reverse lookup.
    mutable std::unordered_map<NodeId, lxb_dom_node_t*> id_to_node;
    mutable NodeId next_id = 1;
    std::uint64_t mutation_version = 1;
    std::uint64_t layout_mutation_version = 1;
    // HTML parser scripting flag (see DomDocument::set_scripting_enabled).
    // Carried across parse() so the mode survives document replacement.
    bool scripting_enabled = true;
    std::unordered_set<std::string> layout_sensitive_attributes;
    bool layout_sensitive_attribute_wildcard = false;

    // Bounded ring of layout-affecting mutations, one record per
    // layout_mutation_version bump. `layout_journal_base` is the highest version
    // for which no record is retained: records cover exactly
    // (layout_journal_base, layout_mutation_version]. Overflow past the cap
    // evicts the oldest record and advances the base, so queries older than the
    // base report incomplete. Reset on parse().
    static constexpr std::size_t kLayoutJournalCap = 64;
    std::deque<LayoutMutationRecord> layout_journal;
    std::uint64_t layout_journal_base = 1;
    void record_layout_mutation(LayoutMutationRecord record);

    // Bumped whenever a mutation changes the CSS rules the cascade sees.
    std::uint64_t stylesheet_generation = 1;
    // Per-node self/subtree layout dirty epochs.
    std::unordered_map<NodeId, DirtyEpoch> dirty_epochs;
    // Sibling-context memo for layout_input_digest(), rebuilt lazily per version.
    mutable std::unordered_map<NodeId, LevelContext> level_context_cache;
    mutable std::uint64_t level_context_cache_version = 0;
    // Bumped only when a tracked node id is invalidated (forgotten) or the
    // document is reparsed — i.e. exactly when a JS wrapper may become stale.
    // Lets the wrapper layer skip its O(N) prune scan on ordinary mutations
    // (append/remove/setAttribute) that never invalidate an id.
    std::uint64_t forget_version = 1;

    // Reused across queries so each querySelector(All) avoids recreating the
    // CSS parser and selector engine. The selector engine is created lazily and
    // auto-cleans after every find; compiled selector lists are cached by their
    // source text (each owns its own memory pool and is freed in the dtor).
    lxb_selectors_t* selectors = nullptr;
    std::unordered_map<std::string, lxb_css_selector_list_t*> selector_cache;
    // Insertion order of selector_cache keys, used to bound the cache with FIFO
    // eviction so adversarial JS generating unbounded distinct selectors (e.g.
    // querySelectorAll('[data-i="'+i+'"]') in a loop) cannot grow memory without
    // limit over a single document's lifetime.
    std::deque<std::string> selector_cache_order;

    Impl();
    ~Impl();

    lxb_dom_node_t* require_node(NodeId id) const;
    lxb_dom_element_t* require_element(NodeId id) const;
    NodeId id_for(lxb_dom_node_t* node) const;
    // Resets the user slot over a cloned subtree so each cloned node gets a fresh
    // id. lxb_dom_node_clone copies the user slot (node.c), which would otherwise
    // alias the source nodes' ids onto the clones.
    static void clear_user_data_subtree(lxb_dom_node_t* node);
    bool has_node(NodeId id) const;
    void forget_node(lxb_dom_node_t* node);
    bool attribute_affects_layout(std::string_view name) const;
    bool node_affects_layout(lxb_dom_node_t* node) const;
    // True when mutating `node` changes the CSS rules the cascade sees: `node`
    // is (or is inside) a <style>, or is a <link>/<base> element.
    bool mutation_affects_stylesheets(lxb_dom_node_t* node) const;
    void mark_mutated(
        bool affects_layout = true,
        NodeId layout_node = kInvalidNodeId,
        bool affects_stylesheets = false,
        LayoutMutationRecord detail = {});
    // Marks `anchor` self-dirty and every node on anchor->root subtree-dirty at
    // the current layout_mutation_version. Called after mark_mutated so the
    // version is already bumped.
    void mark_layout_dirty_from(lxb_dom_node_t* anchor);

    std::uint64_t layout_input_digest(
        NodeId id,
        int viewport_width,
        int viewport_height,
        float device_scale_factor) const;
    // True when attribute `lower` (already ASCII-lowercased) is a cascade input
    // whose value must be folded into layout_input_digest().
    bool digest_folds_attribute(std::string_view lower) const;
    // Folds one element's own selector-relevant inputs (tag, presentational and
    // selector attributes, and optionally inline style) into `hash`.
    void fold_element_selectors(std::uint64_t& hash, lxb_dom_element_t* element, bool include_style) const;
    // Returns the memoized, position-aware sibling context for `parent`'s element
    // children at the current layout_mutation_version.
    const LevelContext& level_context_for(lxb_dom_node_t* parent) const;

    lxb_css_selector_list_t* compiled_selector(std::string_view selector);
    std::vector<NodeId> run_selector(lxb_dom_node_t* root, std::string_view selector, bool first_only);
};

} // namespace pagecore

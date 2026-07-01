#pragma once

#include "pagecore/dom.hpp"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagecore {

struct DomDocument::Impl {
    lxb_html_document_t* document = nullptr;
    // Reverse map only: NodeId -> node. The forward direction (node -> id) lives
    // in each node's own lxb_dom_node_t.user slot (see id_for). The reverse map
    // stays because it is the validation boundary for JS-supplied ids and there
    // is no per-node structure for the reverse lookup.
    mutable std::unordered_map<NodeId, lxb_dom_node_t*> id_to_node;
    mutable NodeId next_id = 1;
    std::uint64_t mutation_version = 1;
    std::uint64_t layout_mutation_version = 1;
    std::string last_layout_mutation_reason = "initial";
    std::unordered_set<std::string> layout_sensitive_attributes;
    bool layout_sensitive_attribute_wildcard = false;
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
    void mark_mutated(bool affects_layout = true, std::string_view layout_reason = {});

    lxb_css_selector_list_t* compiled_selector(std::string_view selector);
    std::vector<NodeId> run_selector(lxb_dom_node_t* root, std::string_view selector, bool first_only);
};

} // namespace pagecore

#pragma once

#include "pagecore/dom.hpp"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pagecore {

struct DomDocument::Impl {
    lxb_html_document_t* document = nullptr;
    mutable std::unordered_map<lxb_dom_node_t*, NodeId> node_to_id;
    mutable std::unordered_map<NodeId, lxb_dom_node_t*> id_to_node;
    mutable NodeId next_id = 1;
    std::uint64_t mutation_version = 1;
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
    bool has_node(NodeId id) const;
    void forget_node(lxb_dom_node_t* node);
    void forget_subtree(lxb_dom_node_t* node);
    void mark_mutated();

    lxb_css_selector_list_t* compiled_selector(std::string_view selector);
    std::vector<NodeId> run_selector(lxb_dom_node_t* root, std::string_view selector, bool first_only);
};

} // namespace pagecore

#pragma once

#include "pagecore/dom.hpp"

#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>

#include <string>
#include <unordered_map>

namespace pagecore {

struct DomDocument::Impl {
    lxb_html_document_t* document = nullptr;
    mutable std::unordered_map<lxb_dom_node_t*, NodeId> node_to_id;
    mutable std::unordered_map<NodeId, lxb_dom_node_t*> id_to_node;
    mutable NodeId next_id = 1;
    std::uint64_t mutation_version = 1;

    Impl();
    ~Impl();

    lxb_dom_node_t* require_node(NodeId id) const;
    lxb_dom_element_t* require_element(NodeId id) const;
    NodeId id_for(lxb_dom_node_t* node) const;
    bool has_node(NodeId id) const;
    void forget_node(lxb_dom_node_t* node);
    void forget_subtree(lxb_dom_node_t* node);
    void mark_mutated();
};

} // namespace pagecore

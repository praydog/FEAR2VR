#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "regenny/regenny/LTVisTree.hpp"

namespace sdk {

// The engine's visibility tree: a KD-tree over convex sectors, which is what
// LTObjects are associated with for culling.
//
// HOW IT IS REACHED, and why this is a free helper rather than a method on
// interfaces::IWorldClientBSP: the manager is an EMBEDDED SUB-OBJECT of that
// interface at +0x24 -- its vtable slot 14 is literally `return this + 36`. The
// interface headers are generated from reversing/interfaces.txt and are declared
// opaque on purpose, so the offset lives here next to the schema that proves it
// instead of being hand-edited into a generated file.
//
// No pattern scan and no hardcoded VA: the interface itself is resolved by
// engine-side NAME through the interface registry, and this only adds a
// schema-confirmed offset on top.
class VisTree {
public:
    // nullptr when IWorldClientBSP is unresolved (early startup, unloaded
    // module) -- never cache this across a frame.
    static const regenny::LTVisTree* get();

    // Structural self-check of the tree, walked from `root`.
    //
    // The two count fields make this unusually strong for a pointer-chasing
    // structure: the engine stores both how many nodes and how many sectors
    // there should be, so the walk can be checked against the engine's own
    // numbers rather than against a value we recorded earlier.
    //
    //   * nodes_walked == node_count      -- the tree is exactly as big as claimed
    //   * every element is an ALIGNED entry of the sector array, in range
    //   * sectors_reached <= sector_count  -- and live it is equal, i.e. the tree
    //     covers every sector
    //
    // `split_axis` doubles as the leaf marker (> 2 means leaf), so a wrong
    // offset there would either truncate the walk or run it off into garbage;
    // both show up as a node-count mismatch.
    struct TreeCheck {
        size_t sector_count;    // the engine's own count
        size_t node_count;      // the engine's own count
        size_t nodes_walked;    // nodes actually reached from root
        size_t elements_seen;   // element slots visited (with duplicates)
        size_t elements_in_arr; // of those, aligned entries inside the sector array
        size_t sectors_reached; // distinct sectors reached
        size_t leaves;          // nodes with split_axis > 2
        size_t max_depth;
    };

    // nullopt when the interface is unresolved, the tree is empty, or the walk
    // faulted / failed to terminate.
    static std::optional<TreeCheck> check(size_t max_nodes = 8192);
};

} // namespace sdk

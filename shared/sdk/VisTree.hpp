#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "regenny/regenny/LTVisTree.hpp"
#include "regenny/regenny/LTWorldClientBSP.hpp"

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


// The client world container behind IWorldClientBSP: world bounds, the world
// tree (the X/Z quadtree objects are bucketed into), the embedded vis tree, and
// the loaded .wld path.
//
// Same reachability story as VisTree: the interface is resolved by engine-side
// NAME, and this only adds schema-confirmed offsets.
class WorldBSP {
public:
    // nullptr when IWorldClientBSP is unresolved. Never cache across a frame.
    static const regenny::LTWorldClientBSP* get();

    // Structural self-check of the WORLD tree (distinct from VisTree::check,
    // which checks the vis KD-tree).
    //
    // The valuable part is `root_matches_objects`: the world-tree root is
    // reachable two completely independent ways -- read from this header, or
    // found by climbing LTObject.world_tree_link's parent chain from any linked
    // object. An earlier pass only had the second route. Agreement between them
    // is strong evidence for parent_offset, world_tree_link AND this field at
    // once; disagreement means one of the three moved.
    struct WorldTreeCheck {
        size_t stored_node_count; // the engine's own count
        size_t nodes_walked;      // nodes reached from the stored root
        size_t occupied;          // nodes holding at least one object
        size_t max_depth;
        // root_matches_objects lives in CClientMgr::check_world_tree, which
        // already climbs parent_offset from a linked object; see the note there.
        bool bounds_ordered;       // bounds_min <= bounds_max componentwise
        bool bounds_copies_agree;  // the +0x04 and +0x22C pairs are identical
        size_t sectors_in_bounds;  // vis sectors whose AABB fits inside the bounds
        size_t sector_count;
    };

    // nullopt when the interface is unresolved, no world is loaded, or a walk
    // faulted / failed to terminate.
    static std::optional<WorldTreeCheck> check(size_t max_nodes = 8192);
};

} // namespace sdk

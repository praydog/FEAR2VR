#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "regenny/regenny/LTVector.hpp"

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
        // Portals: the other half of the visibility graph. Each connects two
        // sectors, so validating them exercises the pointer table, the 0x5C
        // stride, and the plane/vertex geometry together. The plane checks are
        // the load-bearing ones: `center` and all four vertices must satisfy the
        // portal's own plane equation, which no wrong offset survives.
        size_t portal_count;       // the engine's own count
        size_t portal_unit_normal; // |plane_normal| == 1
        size_t portal_center_on_plane;
        size_t portal_sectors_ok;  // both sector pointers aligned+in-range, and distinct
        size_t portal_verts_on_plane; // all vertex_count vertices satisfy the plane
    };

    // nullopt when the interface is unresolved, the tree is empty, or the walk
    // faulted / failed to terminate.
    static std::optional<TreeCheck> check(size_t max_nodes = 8192);

    // ---- CONSUMER API: WHERE AM I IN THE WORLD? ---------------------------
    //
    // Everything below was reachable only through check() until this pass -- the tree
    // walk, the sector geometry and the KD descent all lived inside its SEH guard and it
    // returned counters. For a VR mod this is the locomotion question: is a point inside
    // the level, which room is it in, and where are that room's walls.

    // One convex sector: the leaf volume the engine culls and lights with.
    struct Sector {
        // Index into the tree's sector array. A STABLE KEY -- unlike a pointer it can be
        // logged, compared across frames and stored, because the array is built at load.
        size_t index;
        // THE SECTOR'S SHAPE. Measured: this box is the primary volume, not merely an
        // extent. Only 19 of 263 live sectors carry bounding PLANES at all (125 planes
        // between them) -- an earlier version of this comment had it backwards and claimed
        // every sector was a plane-bounded convex cell with the box as a loose hull. The
        // box is what almost every sector actually has.
        regenny::LTVector min;
        regenny::LTVector max;
        // Extra bounding planes, when the sector has them -- ZERO on 244 of 263 live, so a
        // caller must not require them. Where present they refine the box.
        size_t plane_count;
    };
    struct SectorPlane {
        regenny::LTVector normal;
        float distance;
    };

    // The engine's own sector total, straight from the tree header.
    static std::optional<size_t> sector_count();

    // Sector by index. nullopt when the tree is unresolved, `index` is out of range, or
    // the read faulted.
    static std::optional<Sector> sector(size_t index);

    // Its bounding planes. Empty when the sector has none, is out of range, or the read
    // faulted -- a caller that needs to tell those apart should call sector() first.
    static std::vector<SectorPlane> sector_planes(size_t index);

    // IS THIS POINT INSIDE that sector? THE BOX DECIDES, and the planes refine it when the
    // sector has any -- which 244 of 263 do not. An earlier version of this tested the
    // planes ALONE and therefore answered "cannot say" for 93% of the world, which read as
    // "the player is nowhere" and made a brute-force cross-check vacuous rather than
    // failing loudly.
    //
    // `slop` widens the box on every axis and is added to each plane distance, which is how
    // a caller asks "within a few units of inside" -- useful for a position resting on a
    // floor, where the exact boundary is the thing being tested.
    static std::optional<bool> sector_contains(size_t index, const regenny::LTVector& point,
                                               float slop = 0.0f);

    // POINT LOCATION, the query a mod actually makes. Descends the KD-tree to the leaf
    // containing `point` and returns that leaf's sectors, nearest-first is NOT promised --
    // a leaf can hold several sectors and they are returned in the engine's own order.
    //
    // Empty when the point is outside the tree, the tree is unresolved, or the walk
    // faulted. Combine with sector_contains() when you need the exact cell rather than the
    // candidates: the KD leaf narrows it to a handful, the planes decide.
    static std::vector<Sector> sectors_at(const regenny::LTVector& point);

    // THE WHOLE QUERY IN ONE CALL: the first sector whose planes contain the point.
    // nullopt means the point is in no sector, which for a world position means outside
    // the playable volume -- exactly what a teleport check wants to know.
    static std::optional<Sector> sector_containing(const regenny::LTVector& point,
                                                   float slop = 0.0f);

    // ---- PORTALS: how sectors CONNECT ---------------------------------------
    //
    // The other half of the visibility graph, and the other thing reachable only through
    // check(). A portal is the opening between two sectors -- a doorway, a window, an arch
    // -- carrying its own plane, a bounding circle and the quad describing it.
    //
    // FOR A MOD THIS IS REACHABILITY. "Which rooms connect to this one" is what a teleport
    // validator, an audio-propagation pass or a navigation heuristic needs, and that is
    // sector_neighbours() below. Live: 344 portals joining 262 of the 263 sectors --
    // exactly one sector connects to nothing.
    struct Portal {
        size_t index;  // into the portal table; stable for the level's lifetime
        // The portal's plane. Its centre and all its vertices lie ON this plane, which is
        // what pins the record's layout.
        SectorPlane plane;
        regenny::LTVector center;
        float radius;  // bounding circle about `center`, in the plane
        // THE TWO SECTORS IT JOINS, as INDICES rather than pointers -- the conversion is
        // the fiddly part a caller should not repeat: a pointer is only a sector if it is
        // an aligned, in-range entry of the sector array. nullopt when it is not, which is
        // how a torn or partially-built portal reports itself instead of yielding a
        // plausible index.
        std::optional<size_t> sector_a;
        std::optional<size_t> sector_b;
        // The quad. vertex_count is 4 on every live portal and the array holds exactly 4,
        // so a larger value is CLAMPED rather than trusted -- reading past the record is
        // the failure that prevents.
        size_t vertex_count;
        regenny::LTVector vertices[4];
    };

    // The engine's own portal total.
    static std::optional<size_t> portal_count();

    // Portal by index. nullopt when the tree is unresolved, `index` is out of range, or the
    // read faulted.
    static std::optional<Portal> portal(size_t index);

    // Every portal touching `sector_index`, in table order. Empty for the one sector that
    // has none, and for an out-of-range index.
    static std::vector<Portal> sector_portals(size_t sector_index);

    // THE REACHABILITY QUERY: the sectors directly connected to this one. Deduplicated, and
    // `sector_index` itself is never included even if a malformed portal named it twice.
    static std::vector<size_t> sector_neighbours(size_t sector_index);
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

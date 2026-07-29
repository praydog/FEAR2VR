#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "regenny/regenny/LTVector.hpp"

#include "regenny/regenny/LTVisTree.hpp"
#include "regenny/regenny/LTWorldClientBSP.hpp"

// Forward declaration rather than the generated header: objects_near() hands out pointers
// and a caller that dereferences one includes LTObject itself.
namespace regenny {
class LTObject;
}

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
        // The engine's cached corner selector. Exposed because it is a CACHE of `normal` and
        // can therefore be stale -- see plane_corner_code_is_current.
        uint32_t corner_code;
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

    // ---- REGION QUERIES: what does a VOLUME touch? --------------------------
    //
    // sector_containing() answers for a point. These answer for a sphere, which is what a
    // mod usually has: a play-space extent, a grab or interaction radius, a blast radius, an
    // audio propagation seed. Both are the ENGINE'S OWN tests, not approximations:
    //
    //   per-sector  LTVisSector_TestSphere -- squared point-to-AABB distance against radius
    //               squared, then the planes with `-radius` slack (positive is inside)
    //   traversal   LTVisTree_QuerySphere -- child_a is the LOW side, child_b the HIGH, and a
    //               sphere spanning the split visits BOTH
    //
    // sector_contains() is now literally the radius-zero case of the first, so the point and
    // volume paths cannot drift apart.

    // Does this sector overlap the sphere? nullopt when the tree is unresolved, `index` is out
    // of range, or a read faulted.
    static std::optional<bool> sector_overlaps_sphere(size_t index,
                                                      const regenny::LTVector& center,
                                                      float radius);

    // Every sector the sphere touches, by the engine's own descent. Empty when nothing is
    // touched, the tree is unresolved, or the walk faulted.
    //
    // A NOTE ON WHAT THE FIXTURE PROVES about this: the oracle compares it against a scan of
    // all sectors using sector_overlaps_sphere, so it validates the TRAVERSAL only -- both
    // paths share the per-sector test. That test is independently grounded in the engine's
    // code and in the "a sector contains its own centre" invariant, which is what makes the
    // arrangement honest rather than circular. It became honest the hard way: an earlier
    // oracle shared an INVERTED predicate with the code it was checking and agreed with it
    // perfectly for several passes.
    static std::vector<Sector> sectors_in_sphere(const regenny::LTVector& center, float radius,
                                                 size_t max_results = 256);

    // ---- THE BOX VARIANT ----------------------------------------------------
    //
    // A room-scale play space IS a box, not a sphere, and so is a trigger volume or a
    // level-streaming region. The engine carries a parallel pair for it:
    //
    //   per-sector  LTVisSector_TestAABB -- box-vs-box, then the planes
    //   traversal   LTVisTree_QueryAABB -- the same descent, using the query box's own
    //               extents: `split <= max[axis]` reaches the high side, `split >= min[axis]`
    //               also reaches the low, and a box spanning the split visits BOTH
    //
    // The plane reject is where the two paths differ, and it is worth understanding because
    // it explains what corner_code is for. A sphere has one slack value in every direction,
    // so the engine writes `dot(n, centre) - d < -radius`. A box does not, so instead of a
    // slack term the engine picks the box CORNER reaching furthest toward the plane's
    // positive side and tests that one against zero. Same rule -- reject only when the whole
    // volume is on the negative side -- expressed without a radius.

    // Does this sector overlap the box? nullopt when the tree is unresolved, `index` is out
    // of range, or a read faulted. `min`/`max` are not required to be ordered; they are
    // normalised, because a mod computing a play space from two tracked points has no reason
    // to have sorted them.
    static std::optional<bool> sector_overlaps_box(size_t index, const regenny::LTVector& min,
                                                   const regenny::LTVector& max);

    // Every sector the box touches, by the engine's own descent. Same guarantees, and the
    // same note about what the fixture proves, as sectors_in_sphere.
    static std::vector<Sector> sectors_in_box(const regenny::LTVector& min,
                                              const regenny::LTVector& max,
                                              size_t max_results = 256);

    // ---- corner_code, AND WHY A CONSUMER SHOULD CARE ------------------------
    //
    // LTVisPlane.corner_code is a 3-bit selector CACHING a fact about the normal: which
    // corner of a query box reaches furthest toward the positive side. Per axis, take the
    // box's max where the normal's component is >= 0 and its min where it is < 0.
    //
    // Being a cache, it can be WRONG -- and if it is, the engine's box test reads the wrong
    // corner and silently mis-answers, while the sphere test (which never consults it) keeps
    // working. That is the shape of bug this project has already been bitten by twice: a
    // derived value that disagrees with what it was derived from. So anything writing a
    // plane normal must rewrite the code, and these two exist to make that checkable rather
    // than assumed.

    // The code a given normal implies. Pure arithmetic -- no process read, safe to call on a
    // normal you are about to write.
    static uint32_t corner_code_for(const regenny::LTVector& normal);

    // Does the stored code still match the stored normal? nullopt when out of range or the
    // read faulted; false means the cache is stale and the engine's BOX queries against that
    // sector are unreliable.
    static std::optional<bool> plane_corner_code_is_current(size_t sector_index,
                                                            size_t plane_index);

    // ---- OBJECT <-> SECTOR: THE ENGINE'S OWN ANSWER -------------------------
    //
    // The queries above COMPUTE which sectors a volume touches. The engine already knows, for
    // every object it culls, and stores it: LTSpatialRecord_CollectSphere runs exactly the
    // query above with LTSpatialRecord_AddEntry as its callback, so an object's entry list IS
    // the query's result, frozen at the moment the engine last relinked it.
    //
    // A mod should usually ASK rather than compute. It is cheaper, and more importantly it is
    // the answer the renderer, the audio system and the streaming code are all acting on --
    // including when it is out of date, which is a thing that happens and which
    // spatial_record_matches_volume() below exists to detect.
    //
    // The association is doubly linked, so both directions are cheap:
    //
    //   LTSpatialRecord.entry_list --record_next-->  this object's sectors
    //   LTVisSector.entry_list     --hit_next-->     this sector's objects
    //
    // and an entry reaches its sector through `hit_head`, which points AT the sector's own
    // list-head slot -- and since that slot is at offset 0, it points at the sector itself.

    // WHICH SECTORS IS THIS OBJECT IN, per the engine. Empty when the object has no record,
    // is not culled (see the gate on LTSpatialRecord.entry_list in the schema: an object
    // failing `(flags & 1) && !(flags2 & 0x700)` is released rather than collected), the tree
    // is unresolved, or a read faulted.
    static std::vector<size_t> sectors_for_object(const regenny::LTObject* obj);

    // WHICH OBJECTS ARE IN THIS SECTOR, per the engine -- the reverse index, which no amount
    // of querying can produce because it needs every object's volume at once. This is what
    // "what is in this room" wants.
    static std::vector<const regenny::LTObject*> objects_in_sector(size_t index);

    // The record's own stored entry count, for callers that only need the size. This is a
    // MAINTAINED counter (LinkEntry increments it, DetachEntries zeroes it), not a hint --
    // live it equals the walked list length on every record -- so it is worth having
    // separately from sectors_for_object().size(), which walks.
    static std::optional<size_t> spatial_entry_count(const regenny::LTObject* obj);

    // DOES THE STORED ANSWER STILL MATCH ITS OWN INPUT? Runs the query using the volume the
    // record itself stores, and compares against the entries the engine collected.
    //
    // This is deliberately narrower than "is the record correct": it holds the volume fixed,
    // so it cannot be confused by a stale VOLUME (cull_volume_is_current() answers that).
    // What it catches is a volume that was rewritten without recollecting -- the same class of
    // bug as the stale world-tree entries, where 370 of 2142 objects had moved without their
    // index following.
    //
    // nullopt when there is no record, no volume, or a read faulted. Note an object with no
    // entries and no overlaps trivially matches; use spatial_entry_count() if a caller needs
    // to distinguish "agrees" from "agrees about nothing".
    static std::optional<bool> spatial_record_matches_volume(const regenny::LTObject* obj);

    // WHERE the stored answer and a fresh query differ, which is the diagnostic a mod chasing
    // a culling problem actually needs. `missing` are sectors the query finds that the record
    // does not list -- the object reaches somewhere the engine has not been told about, the
    // signature of a stale collection. `extra` are sectors the record lists that the query
    // does not, which would instead mean the SDK's traversal is failing to reach them.
    //
    // Keeping the two directions apart is the point: they blame opposite sides, and a single
    // "does not match" boolean cannot tell a stale engine record from a broken reimplementation.
    struct RecordDiff {
        size_t stored;   // entries the record holds
        size_t computed; // sectors a query on its own stored volume finds
        size_t missing;  // computed but not stored
        size_t extra;    // stored but not computed
    };
    static std::optional<RecordDiff> spatial_record_diff(const regenny::LTObject* obj);

    // IS THIS OBJECT'S SPATIAL STATE TRUSTWORTHY, by the engine's own rule rather than by a
    // flat comparison? LTObjectOwner_UpdateSpatialRecord stores the volume UNCONDITIONALLY and
    // collects only when is_renderable() holds, so the contract has two branches:
    //
    //   not renderable  ->  the entry list must be EMPTY (Release ran)
    //   renderable      ->  the entries must match a query on the stored volume
    //
    // Measured live: 1087 of 1087 non-renderable objects have empty lists, so that branch is
    // exact. This is the predicate to ask before trusting sectors_for_object(); the flat
    // comparison above answers a narrower question and reports every gated-out object as a
    // mismatch, which it is not.
    static std::optional<bool> spatial_record_is_consistent(const regenny::LTObject* obj);

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
        // THE POLYGON. `vertex_count` is the engine's own stored count and is AUTHORITATIVE:
        // LTVisPortal is a VARIABLE-LENGTH record, sized `12*(vertex_count-1) + 56`, so a
        // portal with more than four vertices is a legal 116-byte record and not corruption.
        //
        // This fixed array therefore cannot always hold the whole polygon. It holds the first
        // min(vertex_count, 4) and sets `vertices_truncated` when there were more. Every portal
        // in the shipped level has exactly 4 -- which is why the 0x5C stride appears uniform --
        // but that is the art's business, so use portal_polygon() when you need all of them.
        size_t vertex_count;
        regenny::LTVector vertices[4];
        bool vertices_truncated;
    };

    // The engine's own portal total.
    static std::optional<size_t> portal_count();

    // Portal by index. nullopt when the tree is unresolved, `index` is out of range, or the
    // read faulted.
    //
    // INDEXING GOES THROUGH LTVisTree.portals, the engine's pointer table -- never by striding
    // from the first body. The bodies happen to be contiguous because one arena allocation is
    // carved up in order by LTLinearAlloc_Alloc, but with variable-length records that is a
    // consequence of uniform art rather than a guarantee.
    static std::optional<Portal> portal(size_t index);

    // THE WHOLE POLYGON, however many vertices it has -- what a caller clipping a play space
    // against a doorway, or drawing the portal, actually needs. Reads exactly `vertex_count`
    // entries from the variable-length record.
    //
    // Empty when the tree is unresolved, `index` is out of range, or the read faulted. The
    // count is sanity-bounded, because it sizes a read: a wildly large one means a torn record
    // and yields empty rather than walking off the arena.
    static std::vector<regenny::LTVector> portal_polygon(size_t index);

    // Every portal touching `sector_index`, from THE SECTOR'S OWN ARRAY. Empty for the one
    // sector that has none, and for an out-of-range index.
    //
    // This reads the engine's per-sector list directly rather than scanning the portal table
    // for matches, which is both cheaper and the primary data: LTVisSector_LoadFromStream
    // fills this array from portal indices in the asset and then calls LTVisPortal_AttachSector
    // to populate each portal's sector_a/sector_b, so the back-references are DERIVED from
    // these. When the two disagree the array is right and the back-reference is wrong.
    static std::vector<Portal> sector_portals(size_t sector_index);

    // Just the count, from the stored byte -- for callers sizing a buffer or testing for a
    // dead-end room without materialising the portals.
    static std::optional<size_t> sector_portal_count(size_t sector_index);

    // DO THE TWO DIRECTIONS AGREE for this sector? Every portal in its array must name it in
    // sector_a or sector_b, and no portal naming it may be absent from the array.
    //
    // Worth having as a real function rather than test scaffolding: a mod that edits world
    // connectivity -- opening a sealed room, stitching a play space across a doorway -- has to
    // keep both representations in step, and this is the predicate that says whether it did.
    // Live, all 688 links pass in both directions.
    static std::optional<bool> sector_portal_links_agree(size_t sector_index);

    // Does the sector's STORED index match the index used to reach it? Every accessor here
    // converts a pointer to an index by address arithmetic against the table base; the engine
    // also stores the index in the sector itself, so the two can be compared. nullopt when out
    // of range or the read faulted.
    //
    // This is the cheapest available check that the sector stride and table base are right --
    // a wrong stride still yields plausible-looking sectors, but the stored indices stop
    // matching immediately.
    static std::optional<bool> sector_index_is_stored_index(size_t sector_index);

    // THE REACHABILITY QUERY: the sectors directly connected to this one. Deduplicated, and
    // `sector_index` itself is never included even if a malformed portal named it twice.
    static std::vector<size_t> sector_neighbours(size_t sector_index);

    // ---- REACHABILITY BEYOND ONE HOP ----------------------------------------
    //
    // sector_neighbours() answers "next door". These answer the question a mod actually asks:
    // what is within N rooms of here -- which is how you decide what to keep streamed in
    // around a play space, how far to propagate a sound, or whether two points are in the same
    // connected part of the level at all.
    //
    // These are a BREADTH-FIRST WALK over the portal graph and nothing more; no engine function
    // does this, so it is composition rather than reversing. It is sound because the graph
    // itself is: portal adjacency was independently shown symmetric over all 688 links, and the
    // per-sector portal arrays agree with the portals' back-references on all 263 sectors.

    // Every sector reachable within `max_hops` portal crossings, INCLUDING `sector_index`
    // itself at hop zero. `max_hops` of 0 therefore yields just that sector, and 1 yields it
    // plus its neighbours.
    //
    // Empty when `sector_index` is out of range or the tree is unresolved -- note that is
    // distinguishable from a valid isolated sector, which yields exactly itself.
    static std::vector<size_t> sectors_within(size_t sector_index, size_t max_hops);

    // HOW MANY ROOMS APART, or nullopt when `to` is not reachable from `from` at all -- which
    // for a level means they are in different connected components, something a teleport or
    // streaming decision wants to know before it trusts a straight-line distance.
    //
    // Zero when `from == to`. Symmetric, because the underlying adjacency is.
    static std::optional<size_t> sector_hops(size_t from, size_t to);

    // THE WHOLE CONNECTED COMPONENT containing `sector_index`, itself included. This is the
    // set of rooms the player can walk between without leaving the portal graph, and it is
    // what "the reachable level" means for a mod that needs to bound its work.
    static std::vector<size_t> sector_component(size_t sector_index);
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

    // ---- THE WORLD'S EXTENT, WHICH THE ENGINE KEEPS TWICE -------------------
    //
    // LTWorldClientBSP holds the world extent TWICE: bounds_min/max at +0x04 and a second pair
    // at +0x22C. Its own out-of-bounds test -- IWorldClientBSP vtable slot 16 -- reads THE SECOND
    // PAIR and ignores `this` entirely, referencing it by absolute address because the object is
    // a static singleton. A decompile of that function shows bare addresses that look like
    // globals; they are this instance's own fields (0x6F6BD8 + 0x22C is the 0x6F6E04 it cites).
    //
    // That matters to a mod rather than being trivia. A teleport, a play-space bound, or a
    // spawn check written against the instance fields can disagree with the test the engine
    // actually applies, and the engine is the one that decides whether an object "goes outside
    // world". So both are exposed, the engine's predicate is reproduced from the globals it
    // really uses, and bounds_agree() answers whether the two copies are currently the same.
    // Live they are identical, which is why the distinction is documented rather than dramatic.
    struct Bounds {
        regenny::LTVector min;
        regenny::LTVector max;
    };

    // The bounds stored IN THE INSTANCE, at +0x04 and +0x10.
    static std::optional<Bounds> bounds();

    // The SECOND pair, at +0x22C -- the one the engine's own test consults.
    static std::optional<Bounds> engine_bounds();

    // Do the two copies match? false means a mod's own containment test and the engine's will
    // disagree, and the engine's is the one that counts.
    static std::optional<bool> bounds_agree();

    // IS THIS POINT OUTSIDE THE WORLD, reproducing IWorldClientBSP_IsPointOutsideWorld exactly:
    // strict `min > p` or `max < p` on any axis, against the GLOBALS. Note the boundary is
    // INSIDE -- a point exactly on min or max is not outside.
    static std::optional<bool> is_point_outside_world(const regenny::LTVector& point);

    // The same question asked of THE ENGINE, through vtable slot 16. Costs a call and exists so
    // a caller can confirm the reimplementation above still matches the shipped code -- and so
    // this SDK can be checked against the engine rather than against itself.
    static std::optional<bool> is_point_outside_world_engine(const regenny::LTVector& point);

    // ---- THE SPATIAL INDEX: WHAT IS NEAR A POINT? ---------------------------
    //
    // The world tree is an X/Z quadtree (the ground plane, not an octree) that every
    // renderable object is bucketed into. "What is around me" is the query behind grab
    // detection, interaction prompts and proximity triggers, and it was reachable only
    // through check() until this pass.
    //
    // THE DESCENT IS THE ENGINE'S OWN, transcribed from LTWorldTree_FindNodeForObject
    // (0x46462F) rather than guessed -- and it starts at the SAME NODE the engine starts at,
    // which took following two indirections to establish: LTWorldTree_AddObject begins at
    // `world + 0x1C`, `world` is IWorldClientBSP vtable slot 13 (`lea eax, [ecx+4]`), so
    // `world + 0x1C` is the BSP's +0x20 -- world_tree_root, exactly what get() gives us:
    //
    //   child index = (x > split_x ? 2 : 0) + (z > split_z ? 1 : 0)
    //
    // and -- the part that decides the API's shape -- an object whose AABB STRADDLES a
    // split is linked at that node instead of being pushed into a child. So objects live
    // at internal nodes as well as leaves, and a point query must collect along the WHOLE
    // path from the root. (Same shape as the vis tree; there it was learned the hard way.)
    //
    // NOT EVERY OBJECT IS IN THE INDEX: live 2142 of 3583 are linked and 1441 sit
    // self-pointing. A proximity query cannot find the unlinked ones, so ask is_linked()
    // before concluding something is not near you.

    // Objects the index holds along the path covering `point`, nearest node last. Empty
    // when no world is loaded, the point is outside the tree, or a read faulted.
    //
    // A KNOWN LIMIT WITH A KNOWN CAUSE: STALE INDEX ENTRIES. Live, 370 of 2142 indexed
    // objects are parked at a node that is NOT the one their current bounds would choose,
    // and every object this query fails to find (235, all worldmodels) is among them.
    //
    // THE MECHANISM IS AN ASYMMETRY INSIDE LTObject_SetPos, visible in its own code: it
    // writes the world AABB UNCONDITIONALLY (SetWorldAABB with position -/+ dims) but relinks
    // only `if (LTObject_IsRenderable(this))`. So an object that moves while not renderable
    // gets fresh bounds and KEEPS ITS OLD NODE. That is not a guess -- the contrast is
    // measurable from the other side: zero of 3583 world AABBs are stale while 370 of 2142
    // index entries are, and both are written by the same function.
    //
    // Ask index_is_current() before trusting a proximity result for something that moves;
    // the staleness is the engine's, not this SDK's.
    //
    // Four other explanations were measured and REFUTED before this one was confirmed, and
    // they are recorded so nobody re-runs them: result truncation (raising the cap 256 ->
    // 4096 recovered 3, not 235), a position outside the object's own AABB (zero of the
    // 235), a split-boundary tie between the engine's `split <= aabb_min` and a point's
    // `p > split` (branching on it changed nothing), and absence from this tree (tree_slot
    // finds all 235, every one at a leaf).
    //
    // These are ENGINE POINTERS, valid for this frame only -- the one place this SDK hands
    // them out, because a proximity query whose results were copies could not be used to
    // call anything. Resolve to a handle via CClientMgr if you need to keep one.
    static std::vector<const regenny::LTObject*> objects_near(const regenny::LTVector& point,
                                                              size_t max_results = 256);

    // Is this object in the spatial index at all? An unlinked object's world_tree_link
    // self-points, which is what the engine's own remove leaves behind.
    //
    // nullopt when `obj` is null or the read faulted -- distinct from a definite `false`.
    static std::optional<bool> is_linked(const regenny::LTObject* obj);

    // WHICH NODE holds this object, found by walking the tree and looking for the list it
    // is on. That is a useful answer on its own -- two objects in the same node are spatial
    // neighbours by the engine's own bucketing, a cheaper proximity test than any distance
    // computation -- and it is also the only way to tell whether objects_near() reached the
    // right place.
    //
    // `depth` is 0 at the root. nullopt when the object is not linked, no world is loaded,
    // or the walk faulted.
    struct TreeSlot {
        uintptr_t node;   // the LTWorldTreeNode holding it; identity and diagnostics only
        size_t depth;     // 0 = root
        float split_x;    // the node's own split planes, for reasoning about neighbours
        float split_z;
        bool leaf;        // child_offset == 0
    };
    static std::optional<TreeSlot> tree_slot(const regenny::LTObject* obj);

    // WHERE THE ENGINE WOULD LINK THIS OBJECT NOW, by descending its own rule
    // (LTWorldTree_FindNodeForObject) with the object's CURRENT world AABB.
    //
    // The box descent is not the point descent: a box whose span crosses a split STOPS at
    // that node instead of choosing a child, which is why objects sit at internal nodes at
    // all. This reproduces that exactly.
    //
    // nullopt when no world is loaded, the object is null, or a read faulted.
    static std::optional<TreeSlot> slot_for_current_bounds(const regenny::LTObject* obj);

    // IS THE OBJECT'S INDEX ENTRY CURRENT? True when the node it is actually parked in is
    // the node its current bounds would choose.
    //
    // A MOD SHOULD ASK THIS BEFORE TRUSTING A PROXIMITY RESULT for something that moves. The
    // engine relinks from LTObject_SetPos, SetPosRot, SetDims and SetFlags only, so anything
    // moved by another route keeps the node it was linked at -- and its neighbours, as the
    // index sees them, are wherever it used to be.
    //
    // nullopt when either slot could not be determined (unlinked object, no world, fault).
    static std::optional<bool> index_is_current(const regenny::LTObject* obj);
};

} // namespace sdk

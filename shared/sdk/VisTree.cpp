#include "VisTree.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "regenny/regenny/LTVisPortal.hpp"
#include "regenny/regenny/LTVisPlane.hpp"
#include "regenny/regenny/LTVisSector.hpp"
#include "regenny/regenny/LTVisTreeNode.hpp"
#include "regenny/regenny/LTWorldTreeLink.hpp"
#include "regenny/regenny/LTWorldTreeNode.hpp"
#include "regenny/regenny/LTObject.hpp"
#include "regenny/regenny/LTSpatialEntry.hpp"
#include "regenny/regenny/LTSpatialRecord.hpp"

#include "Object.hpp"

#include "interfaces/IWorldClientBSP.hpp"

namespace sdk {

namespace {

// IWorldClientBSP's vtable slot 14 is `return this + 36`, so the vis tree is an
// embedded sub-object at this offset. CONFIRMED against the live header: reading
// `sectors` here yields the same array base that scanning outwards from a known
// sector had found independently, and `sector_count` / `node_count` match the
// walked totals exactly. See fear2.genny's LTVisTree.
constexpr uintptr_t kVisTreeOffset = 0x24;

// Bitmap capacity for the distinct-sector tally. 8192 bits covers the live 263
// by a wide margin; a world exceeding it makes check() report `nullopt` rather
// than silently under-counting.
constexpr size_t kMaxSectors = 8192;

// Reads the header and walks the tree under ONE guard, so a bad pointer anywhere
// in the chain is a caught fault rather than a crash.
//
// Iterative with an explicit stack: the tree is only ~7 deep live, but a corrupt
// `split_axis` could make it look unbounded, and recursing on engine data inside
// an SEH guard turns a bad read into a stack overflow instead of a caught fault.
//
// POD-only for the guard. Returns nodes walked, or -1 on fault / non-termination
// / a tree too large for the bitmap.
int64_t seh_read_and_walk(const regenny::LTVisTree* tree, VisTree::TreeCheck* out, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        out->sector_count = tree->sector_count;
        out->node_count = tree->node_count;

        const auto* root = tree->root;
        const auto* sectors = tree->sectors;
        if (root == nullptr || sectors == nullptr || out->node_count == 0 ||
            out->sector_count == 0 || out->sector_count > kMaxSectors) {
            return -1;
        }

        // Distinct sectors, tracked properly rather than inferred from a
        // duplicate-counting tally: elements repeat across nodes, so a clamped
        // total would report a plausible-looking number that means nothing.
        unsigned char seen[kMaxSectors / 8] = {};

        const regenny::LTVisTreeNode* stack[128];
        size_t depth_of[128];
        size_t sp = 0;
        stack[sp] = root;
        depth_of[sp] = 0;
        ++sp;

        size_t walked = 0;
        bool overflow = false;
        while (sp != 0 && walked < cap) {
            --sp;
            const auto* n = stack[sp];
            const size_t d = depth_of[sp];
            if (n == nullptr) {
                continue;
            }
            ++walked;
            if (d > out->max_depth) {
                out->max_depth = d;
            }

            const uint32_t count = n->element_count;
            if (n->elements != nullptr && count != 0 && count < 100000u) {
                for (uint32_t i = 0; i < count; ++i) {
                    const auto* s = n->elements[i];
                    ++out->elements_seen;
                    if (s == nullptr) {
                        continue;
                    }
                    const auto sa = reinterpret_cast<uintptr_t>(s);
                    const auto ba = reinterpret_cast<uintptr_t>(sectors);
                    if (sa < ba) {
                        continue;
                    }
                    const uintptr_t off = sa - ba;
                    if (off % sizeof(regenny::LTVisSector) != 0) {
                        continue;
                    }
                    const uintptr_t idx = off / sizeof(regenny::LTVisSector);
                    if (idx >= out->sector_count) {
                        continue;
                    }
                    ++out->elements_in_arr;
                    const size_t byte = static_cast<size_t>(idx) >> 3;
                    const unsigned char bit =
                        static_cast<unsigned char>(1u << (static_cast<size_t>(idx) & 7u));
                    if ((seen[byte] & bit) == 0) {
                        seen[byte] |= bit;
                        ++out->sectors_reached;
                    }
                }
            }

            // split_axis > 2 marks a leaf; the engine's own walk uses the same
            // test rather than a separate flag.
            if (n->split_axis > 2u) {
                ++out->leaves;
                continue;
            }
            if (sp + 2 > 128) {
                overflow = true;
                break;
            }
            stack[sp] = n->child_a;
            depth_of[sp] = d + 1;
            ++sp;
            stack[sp] = n->child_b;
            depth_of[sp] = d + 1;
            ++sp;
        }
        result = (sp == 0 && !overflow) ? static_cast<int64_t>(walked) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

const regenny::LTVisTree* VisTree::get() {
    auto* bsp = interfaces::IWorldClientBSP::get();
    if (bsp == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const regenny::LTVisTree*>(reinterpret_cast<uintptr_t>(bsp) +
                                                      kVisTreeOffset);
}

std::optional<VisTree::TreeCheck> VisTree::check(size_t max_nodes) {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    TreeCheck out{};
    const int64_t walked = seh_read_and_walk(tree, &out, max_nodes);
    if (walked < 0) {
        return std::nullopt;
    }
    out.nodes_walked = static_cast<size_t>(walked);
    // AGGREGATES the public portal accessor rather than re-walking the table. The pointer
    // to index conversion, the vertex clamp and the plane arithmetic all live on
    // VisTree::portal() now, where a consumer can use them; this counts.
    const size_t pcount = portal_count().value_or(0);
    out.portal_count = pcount;
    for (size_t i = 0; i < pcount; ++i) {
        const auto p = portal(i);
        if (!p.has_value()) {
            continue;
        }
        const float nx = p->plane.normal.x, ny = p->plane.normal.y, nz = p->plane.normal.z;
        const float len2 = nx * nx + ny * ny + nz * nz;
        if (len2 > 0.98f && len2 < 1.02f) {
            ++out.portal_unit_normal;
        }
        const float cd =
            nx * p->center.x + ny * p->center.y + nz * p->center.z - p->plane.distance;
        if (cd > -0.5f && cd < 0.5f) {
            ++out.portal_center_on_plane;
        }
        if (p->sector_a.has_value() && p->sector_b.has_value() &&
            *p->sector_a != *p->sector_b) {
            ++out.portal_sectors_ok;
        }
        bool all = p->vertex_count != 0;
        for (size_t v = 0; v < p->vertex_count; ++v) {
            const float vd = nx * p->vertices[v].x + ny * p->vertices[v].y +
                             nz * p->vertices[v].z - p->plane.distance;
            if (vd < -0.5f || vd > 0.5f) {
                all = false;
            }
        }
        if (all) {
            ++out.portal_verts_on_plane;
        }
    }
    return out;
}


namespace {

// Walks the world tree (the X/Z quadtree) from a node. 24-byte nodes; the four
// children live at node + sizeof*(child_offset + k), and child_offset 0 marks a
// leaf. POD-only for the guard.
//
// Returns nodes walked, or -1 on fault / non-termination.
int64_t seh_walk_world_tree(const regenny::LTWorldTreeNode* root, size_t* occupied,
                            size_t* max_depth, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::LTWorldTreeNode* stack[256];
        size_t depth_of[256];
        size_t sp = 0;
        stack[sp] = root;
        depth_of[sp] = 0;
        ++sp;

        size_t walked = 0;
        bool overflow = false;
        while (sp != 0 && walked < cap) {
            --sp;
            const auto* n = stack[sp];
            const size_t d = depth_of[sp];
            if (n == nullptr) {
                continue;
            }
            ++walked;
            if (d > *max_depth) {
                *max_depth = d;
            }
            // A node is "occupied" when its own object list is non-empty. The
            // list head is the node's first field and self-points when empty.
            if (n->objects.next != reinterpret_cast<const regenny::LTWorldTreeLink*>(&n->objects)) {
                ++*occupied;
            }
            const uint16_t co = n->child_offset;
            if (co == 0) {
                continue; // leaf
            }
            if (sp + 4 > 256) {
                overflow = true;
                break;
            }
            for (uint16_t k = 0; k < 4; ++k) {
                stack[sp] = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                    reinterpret_cast<uintptr_t>(n) +
                    sizeof(regenny::LTWorldTreeNode) * static_cast<size_t>(co + k));
                depth_of[sp] = d + 1;
                ++sp;
            }
        }
        result = (sp == 0 && !overflow) ? static_cast<int64_t>(walked) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// Climbs parent_offset to the root from an arbitrary node. Returns nullptr on
// fault or if the chain did not terminate.
const regenny::LTWorldTreeNode* seh_climb_to_root(const regenny::LTWorldTreeNode* node) {
    const regenny::LTWorldTreeNode* result = nullptr;
    KANANLIB_SEH_TRY {
        const auto* p = node;
        size_t hops = 0;
        while (p != nullptr && p->parent_offset != 0 && hops < 64) {
            p = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                reinterpret_cast<uintptr_t>(p) -
                sizeof(regenny::LTWorldTreeNode) * p->parent_offset);
            ++hops;
        }
        result = (p != nullptr && p->parent_offset == 0) ? p : nullptr;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

// Bounds + sector containment, guarded together.
bool seh_bounds(const regenny::LTWorldClientBSP* bsp, WorldBSP::WorldTreeCheck* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto& a = bsp->bounds_min;
        const auto& b = bsp->bounds_max;
        out->bounds_ordered = a.x <= b.x && a.y <= b.y && a.z <= b.z;
        const auto& a2 = bsp->bounds_min_2;
        const auto& b2 = bsp->bounds_max_2;
        out->bounds_copies_agree = a.x == a2.x && a.y == a2.y && a.z == a2.z && b.x == b2.x &&
                                   b.y == b2.y && b.z == b2.z;

        const auto& tree = bsp->vis_tree;
        out->sector_count = tree.sector_count;
        if (tree.sectors != nullptr && tree.sector_count != 0 && tree.sector_count < 65536u) {
            for (uint32_t i = 0; i < tree.sector_count; ++i) {
                const auto& s = tree.sectors[i];
                if (s.aabb_min.x >= a.x && s.aabb_min.y >= a.y && s.aabb_min.z >= a.z &&
                    s.aabb_max.x <= b.x && s.aabb_max.y <= b.y && s.aabb_max.z <= b.z) {
                    ++out->sectors_in_bounds;
                }
            }
        }
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

} // namespace

const regenny::LTWorldClientBSP* WorldBSP::get() {
    auto* bsp = interfaces::IWorldClientBSP::get();
    if (bsp == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const regenny::LTWorldClientBSP*>(bsp);
}

std::optional<WorldBSP::WorldTreeCheck> WorldBSP::check(size_t max_nodes) {
    const auto* bsp = get();
    if (bsp == nullptr) {
        return std::nullopt;
    }
    WorldTreeCheck out{};
    const regenny::LTWorldTreeNode* root = nullptr;
    KANANLIB_SEH_TRY {
        out.stored_node_count = bsp->world_tree_node_count;
        root = bsp->world_tree_root;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (root == nullptr || out.stored_node_count == 0) {
        return std::nullopt; // no world loaded
    }

    const int64_t walked =
        seh_walk_world_tree(root, &out.occupied, &out.max_depth, max_nodes);
    if (walked < 0) {
        return std::nullopt;
    }
    out.nodes_walked = static_cast<size_t>(walked);

    if (!seh_bounds(bsp, &out)) {
        return std::nullopt;
    }

    // NOTE: the "does the stored root match the one reachable from objects?"
    // cross-check deliberately lives in CClientMgr::check_world_tree instead of
    // here -- that walk already climbs parent_offset from a linked object, so
    // duplicating the object-side traversal in this file would mean two copies
    // of the same fragile logic. It compares against WorldBSP::get()'s field.
    return out;
}

} // namespace sdk

namespace sdk {

namespace {

// POD mirror of one sector, copied out under the guard.
struct SectorRaw {
    float mn[3];
    float mx[3];
    uint8_t plane_count;
    const void* planes;
    bool ok;
};

SectorRaw seh_read_sector(const regenny::LTVisTree* tree, size_t index) {
    SectorRaw r{};
    KANANLIB_SEH_TRY {
        const auto* arr = tree->sectors;
        if (arr == nullptr || index >= tree->sector_count) {
            return r;
        }
        const auto& s = arr[index];
        r.mn[0] = s.aabb_min.x;
        r.mn[1] = s.aabb_min.y;
        r.mn[2] = s.aabb_min.z;
        r.mx[0] = s.aabb_max.x;
        r.mx[1] = s.aabb_max.y;
        r.mx[2] = s.aabb_max.z;
        r.plane_count = s.plane_count;
        r.planes = s.planes;
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

VisTree::Sector make_sector(size_t index, const SectorRaw& r) {
    VisTree::Sector out{};
    out.index = index;
    out.min.x = r.mn[0];
    out.min.y = r.mn[1];
    out.min.z = r.mn[2];
    out.max.x = r.mx[0];
    out.max.y = r.mx[1];
    out.max.z = r.mx[2];
    out.plane_count = r.plane_count;
    return out;
}

}  // namespace

std::optional<size_t> VisTree::sector_count() {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    size_t n = 0;
    bool ok = false;
    KANANLIB_SEH_TRY {
        n = tree->sector_count;
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok ? std::optional<size_t>{n} : std::nullopt;
}

std::optional<VisTree::Sector> VisTree::sector(size_t index) {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_sector(tree, index);
    if (!r.ok) {
        return std::nullopt;
    }
    return make_sector(index, r);
}

// The guarded half, POD only: MSVC refuses __try in any function that must unwind an
// object, and the vector below is exactly that. Returns planes copied, or -1 on fault.
namespace {

struct PlaneRaw {
    float n[3];
    float d;
    uint32_t code;
};

int64_t seh_copy_planes(const void* src, size_t count, PlaneRaw* out) {
    int64_t n = -1;
    KANANLIB_SEH_TRY {
        const auto* p = static_cast<const regenny::LTVisPlane*>(src);
        for (size_t i = 0; i < count; ++i) {
            out[i].n[0] = p[i].normal.x;
            out[i].n[1] = p[i].normal.y;
            out[i].n[2] = p[i].normal.z;
            out[i].d = p[i].distance;
            out[i].code = p[i].corner_code;
        }
        n = static_cast<int64_t>(count);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

}  // namespace

std::vector<VisTree::SectorPlane> VisTree::sector_planes(size_t index) {
    std::vector<SectorPlane> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    const auto r = seh_read_sector(tree, index);
    if (!r.ok || r.planes == nullptr || r.plane_count == 0) {
        return out;
    }
    // plane_count is a uint8, so this bound is the field's own maximum rather than a
    // guess about what a sector "should" have.
    PlaneRaw raw[256]{};
    const int64_t n = seh_copy_planes(r.planes, r.plane_count, raw);
    if (n <= 0) {
        return out;
    }
    out.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].normal.x = raw[i].n[0];
        out[i].normal.y = raw[i].n[1];
        out[i].normal.z = raw[i].n[2];
        out[i].distance = raw[i].d;
        out[i].corner_code = raw[i].code;
    }
    return out;
}

std::optional<bool> VisTree::sector_contains(size_t index, const regenny::LTVector& point,
                                             float slop) {
    // A POINT IS A ZERO-RADIUS SPHERE, so this is literally the volume test with `slop` as the
    // radius -- box first, then planes rejecting only a wholly-negative side. Expressing it
    // this way is deliberate: the point and volume paths used to be separate code and that is
    // exactly how a sign error survived in one of them.
    return sector_overlaps_sphere(index, point, slop);
}

}  // namespace sdk

namespace sdk {

namespace {

// Descends the KD-tree to the leaf containing `point` and copies out that leaf's sector
// indices, all under one guard.
//
// THE SIDE CONVENTION IS NOT ASSUMED. `split_axis` selects the axis and `split_value` the
// plane, but which child holds the lower half is not stated anywhere in the structure. So
// when the point sits within `kSplitSlop` of the split plane BOTH children are followed --
// which makes the descent correct whichever way round the engine stores them, at the cost
// of returning a few extra candidates near a boundary. The caller narrows with the plane
// test, and the fixture cross-checks the whole thing against a brute-force scan of every
// sector, so a wrong convention shows up as a disagreement rather than as a plausible miss.
constexpr float kSplitSlop = 0.5f;

int64_t seh_leaf_sectors(const regenny::LTVisTree* tree, const float p[3], size_t* out,
                         size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* root = tree->root;
        const auto* sectors = tree->sectors;
        const uint32_t sector_count = tree->sector_count;
        if (root == nullptr || sectors == nullptr || sector_count == 0) {
            return -1;
        }
        const regenny::LTVisTreeNode* stack[128];
        size_t sp = 0;
        stack[sp++] = root;
        size_t n = 0;
        size_t guard = 0;
        while (sp != 0 && guard < 4096) {
            ++guard;
            const auto* node = stack[--sp];
            if (node == nullptr) {
                continue;
            }
            // COLLECT AT EVERY NODE, not only at leaves. The tree's own walk (see
            // seh_read_and_walk) harvests `elements` from each node it visits, and that is
            // not incidental: a sector overlapping an internal node's whole region is
            // attached THERE rather than pushed down to every leaf below it. Collecting
            // only at leaves missed the sector the player was actually standing in, which a
            // brute-force scan of all 263 found immediately.
            {
                const uint32_t count = node->element_count;
                if (node->elements != nullptr && count != 0 && count < 100000u) {
                    for (uint32_t i = 0; i < count && n < max_out; ++i) {
                        const auto* s = node->elements[i];
                        if (s == nullptr) {
                            continue;
                        }
                        const auto sa = reinterpret_cast<uintptr_t>(s);
                        const auto ba = reinterpret_cast<uintptr_t>(sectors);
                        if (sa < ba) {
                            continue;
                        }
                        const uintptr_t off = sa - ba;
                        if (off % sizeof(regenny::LTVisSector) != 0) {
                            continue;
                        }
                        const uintptr_t idx = off / sizeof(regenny::LTVisSector);
                        if (idx >= sector_count) {
                            continue;
                        }
                        // Skip a duplicate: a sector can appear in several leaves and both
                        // may be followed near a split.
                        bool dup = false;
                        for (size_t k = 0; k < n; ++k) {
                            if (out[k] == static_cast<size_t>(idx)) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            out[n++] = static_cast<size_t>(idx);
                        }
                    }
                }
            }
            if (node->split_axis > 2u) {
                continue;  // leaf: nothing below it
            }
            if (sp + 2 > 128) {
                return -1;
            }
            const float v = p[node->split_axis] - node->split_value;
            if (v < -kSplitSlop) {
                stack[sp++] = node->child_a;
            } else if (v > kSplitSlop) {
                stack[sp++] = node->child_b;
            } else {
                stack[sp++] = node->child_a;
                stack[sp++] = node->child_b;
            }
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

}  // namespace

std::vector<VisTree::Sector> VisTree::sectors_at(const regenny::LTVector& point) {
    std::vector<Sector> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    const float p[3] = {point.x, point.y, point.z};
    size_t idx[64]{};
    const int64_t n = seh_leaf_sectors(tree, p, idx, 64);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        if (const auto s = sector(idx[static_cast<size_t>(i)]); s.has_value()) {
            out.push_back(*s);
        }
    }
    return out;
}

std::optional<VisTree::Sector> VisTree::sector_containing(const regenny::LTVector& point,
                                                         float slop) {
    for (const auto& s : sectors_at(point)) {
        if (sector_contains(s.index, point, slop).value_or(false)) {
            return s;
        }
    }
    return std::nullopt;
}

}  // namespace sdk

namespace sdk {

namespace {

// POD mirror of one portal, copied out under one guard. The sector pointers arrive as
// raw addresses and are converted to indices outside, where the arithmetic is readable.
struct PortalRaw {
    float n[3];
    float d;
    float c[3];
    float radius;
    uintptr_t sector_a;
    uintptr_t sector_b;
    uint32_t vertex_count;
    float verts[4][3];
    // The sector array's geometry, read in the same guard so the index conversion below
    // cannot be done against a stale base.
    uintptr_t sector_base;
    uint32_t sector_count;
    bool ok;
    uint32_t copied = 0;  // how many fitted into verts[]; vertex_count may exceed it
};

PortalRaw seh_read_portal(const regenny::LTVisTree* tree, size_t index) {
    PortalRaw r{};
    KANANLIB_SEH_TRY {
        const auto* table = tree->portals;
        if (table == nullptr || index >= tree->portal_count) {
            return r;
        }
        const auto* p = table[index];
        if (p == nullptr) {
            return r;
        }
        r.sector_base = reinterpret_cast<uintptr_t>(tree->sectors);
        r.sector_count = tree->sector_count;
        r.n[0] = p->plane_normal.x;
        r.n[1] = p->plane_normal.y;
        r.n[2] = p->plane_normal.z;
        r.d = p->plane_distance;
        r.c[0] = p->center.x;
        r.c[1] = p->center.y;
        r.c[2] = p->center.z;
        r.radius = p->radius;
        r.sector_a = reinterpret_cast<uintptr_t>(p->sector_a);
        r.sector_b = reinterpret_cast<uintptr_t>(p->sector_b);
        // The stored count is AUTHORITATIVE -- the record is variable-length -- so keep it and
        // report the shortfall separately instead of quietly lowering it. Clamping the count
        // itself would tell the caller a six-vertex portal has four, which is a wrong answer
        // rather than a cautious one.
        r.vertex_count = p->vertex_count;
        r.copied = p->vertex_count > 4u ? 4u : p->vertex_count;
        for (uint32_t v = 0; v < r.copied; ++v) {
            r.verts[v][0] = p->vertices[v].x;
            r.verts[v][1] = p->vertices[v].y;
            r.verts[v][2] = p->vertices[v].z;
        }
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

// A pointer is a sector only if it is an ALIGNED, IN-RANGE entry of the array. Both tests
// matter: an arbitrary address inside the span that is not on a stride boundary is not a
// sector, and treating it as one would hand back a plausible neighbouring index.
std::optional<size_t> sector_index_of(uintptr_t ptr, uintptr_t base, uint32_t count) {
    if (base == 0 || ptr < base) {
        return std::nullopt;
    }
    const uintptr_t off = ptr - base;
    if (off % sizeof(regenny::LTVisSector) != 0) {
        return std::nullopt;
    }
    const uintptr_t idx = off / sizeof(regenny::LTVisSector);
    if (idx >= count) {
        return std::nullopt;
    }
    return static_cast<size_t>(idx);
}

}  // namespace

namespace {

// Reads exactly `vertex_count` vertices out of the variable-length record. The cap is a read
// bound, not a claim about the format: 64 is far past anything a portal polygon plausibly has,
// so a count beyond it means a torn record and the read is refused instead of walking the arena.
constexpr size_t kMaxPortalVertices = 64;

int64_t seh_portal_polygon(const regenny::LTVisTree* tree, size_t index, float (*out)[3],
                           size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* table = tree->portals;
        if (table == nullptr || index >= tree->portal_count) {
            return -1;
        }
        const auto* p = table[index];
        if (p == nullptr) {
            return -1;
        }
        const uint32_t n = p->vertex_count;
        if (n == 0 || n > max_out) {
            return n == 0 ? 0 : -1;
        }
        // The declared array is four long; past that the vertices simply continue in the
        // record, which is what "variable-length" means here.
        const auto* v = &p->vertices[0];
        for (uint32_t i = 0; i < n; ++i) {
            out[i][0] = v[i].x;
            out[i][1] = v[i].y;
            out[i][2] = v[i].z;
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

}  // namespace

std::vector<regenny::LTVector> VisTree::portal_polygon(size_t index) {
    std::vector<regenny::LTVector> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    float verts[kMaxPortalVertices][3]{};
    const int64_t n = seh_portal_polygon(tree, index, verts, kMaxPortalVertices);
    if (n <= 0) {
        return out;
    }
    out.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].x = verts[i][0];
        out[i].y = verts[i][1];
        out[i].z = verts[i][2];
    }
    return out;
}

std::optional<size_t> VisTree::portal_count() {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    size_t n = 0;
    bool ok = false;
    KANANLIB_SEH_TRY {
        n = tree->portal_count;
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok ? std::optional<size_t>{n} : std::nullopt;
}

std::optional<VisTree::Portal> VisTree::portal(size_t index) {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_portal(tree, index);
    if (!r.ok) {
        return std::nullopt;
    }
    Portal out{};
    out.index = index;
    out.plane.normal.x = r.n[0];
    out.plane.normal.y = r.n[1];
    out.plane.normal.z = r.n[2];
    out.plane.distance = r.d;
    out.center.x = r.c[0];
    out.center.y = r.c[1];
    out.center.z = r.c[2];
    out.radius = r.radius;
    out.sector_a = sector_index_of(r.sector_a, r.sector_base, r.sector_count);
    out.sector_b = sector_index_of(r.sector_b, r.sector_base, r.sector_count);
    out.vertex_count = r.vertex_count;
    out.vertices_truncated = r.vertex_count > r.copied;
    // BOUNDED BY WHAT WAS COPIED, not by the stored count -- the two differ exactly when the
    // polygon does not fit, and using the stored count here would overrun out.vertices[4].
    for (size_t v = 0; v < r.copied; ++v) {
        out.vertices[v].x = r.verts[v][0];
        out.vertices[v].y = r.verts[v][1];
        out.vertices[v].z = r.verts[v][2];
    }
    return out;
}

namespace {

// The sector's own portal array: `portal_count` pointers into the tree's portal table. POD out
// -- indices, converted here so the caller never holds a raw pointer across the SEH boundary.
int64_t seh_sector_portal_indices(const regenny::LTVisTree* tree, size_t sector_index,
                                  size_t* out, size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* sectors = tree->sectors;
        const auto* portals = tree->portals;
        const uint32_t pcount = tree->portal_count;
        if (sectors == nullptr || portals == nullptr) {
            return -1;
        }
        const auto& sec = sectors[sector_index];
        const size_t n_listed = sec.portal_count;
        const auto* arr = reinterpret_cast<const regenny::LTVisPortal* const*>(sec.portals);
        if (n_listed == 0 || arr == nullptr) {
            return 0;
        }
        // `portals` is a table of POINTERS, so it is NOT the base to difference against --
        // the portal bodies follow the table in one allocation. Compute the index from the body
        // base, then CONFIRM it by reading the table back: that keeps the fast path O(1)
        // without trusting the contiguous layout, and the linear fallback covers the case where
        // the layout is not what the schema recorded.
        const auto body_base =
            reinterpret_cast<uintptr_t>(portals) + static_cast<uintptr_t>(pcount) * sizeof(void*);
        size_t n = 0;
        for (size_t i = 0; i < n_listed && n < max_out; ++i) {
            const auto* pp = arr[i];
            if (pp == nullptr) {
                continue;
            }
            const auto pa = reinterpret_cast<uintptr_t>(pp);
            size_t idx = pcount;  // sentinel: not found
            if (pa >= body_base) {
                const uintptr_t off = pa - body_base;
                if (off % sizeof(regenny::LTVisPortal) == 0) {
                    const uintptr_t cand = off / sizeof(regenny::LTVisPortal);
                    if (cand < pcount && portals[cand] == pp) {
                        idx = static_cast<size_t>(cand);
                    }
                }
            }
            if (idx == pcount) {
                for (uint32_t k = 0; k < pcount; ++k) {
                    if (portals[k] == pp) {
                        idx = k;
                        break;
                    }
                }
            }
            if (idx < pcount) {
                out[n++] = idx;
            }
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

int64_t seh_sector_bytes(const regenny::LTVisTree* tree, size_t sector_index, uint32_t* stored,
                         uint32_t* pcount) {
    int64_t ok = -1;
    KANANLIB_SEH_TRY {
        const auto* sectors = tree->sectors;
        if (sectors == nullptr) {
            return -1;
        }
        *stored = sectors[sector_index].index;
        *pcount = sectors[sector_index].portal_count;
        ok = 1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = -1;
    }
    return ok;
}

}  // namespace

std::vector<VisTree::Portal> VisTree::sector_portals(size_t sector_index) {
    std::vector<Portal> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    if (const auto n = sector_count(); !n.has_value() || sector_index >= *n) {
        return out;
    }
    // portal_count is a uint8, so this bound is the field's own maximum.
    size_t idx[256]{};
    const int64_t n = seh_sector_portal_indices(tree, sector_index, idx, 256);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        if (const auto p = portal(idx[static_cast<size_t>(i)]); p.has_value()) {
            out.push_back(*p);
        }
    }
    return out;
}

std::optional<size_t> VisTree::sector_portal_count(size_t sector_index) {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    if (const auto n = sector_count(); !n.has_value() || sector_index >= *n) {
        return std::nullopt;
    }
    uint32_t stored = 0, pcount = 0;
    if (seh_sector_bytes(tree, sector_index, &stored, &pcount) < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(pcount);
}

std::optional<bool> VisTree::sector_index_is_stored_index(size_t sector_index) {
    const auto* tree = get();
    if (tree == nullptr) {
        return std::nullopt;
    }
    if (const auto n = sector_count(); !n.has_value() || sector_index >= *n) {
        return std::nullopt;
    }
    uint32_t stored = 0, pcount = 0;
    if (seh_sector_bytes(tree, sector_index, &stored, &pcount) < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(stored) == sector_index;
}

std::optional<bool> VisTree::sector_portal_links_agree(size_t sector_index) {
    const auto listed = sector_portals(sector_index);
    const auto declared = sector_portal_count(sector_index);
    if (!declared.has_value()) {
        return std::nullopt;
    }
    // Every element resolved to a real table entry, and every one names this sector.
    if (listed.size() != *declared) {
        return false;
    }
    for (const auto& p : listed) {
        if (p.sector_a != sector_index && p.sector_b != sector_index) {
            return false;
        }
    }
    // And nothing naming this sector is absent from the array. This half needs the table scan
    // the array exists to avoid, which is exactly why it belongs in a checker and not in the
    // accessor.
    const auto total = portal_count();
    if (!total.has_value()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < *total; ++i) {
        const auto p = portal(i);
        if (!p.has_value()) {
            continue;
        }
        if (p->sector_a != sector_index && p->sector_b != sector_index) {
            continue;
        }
        bool seen = false;
        for (const auto& l : listed) {
            if (l.index == p->index) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            return false;
        }
    }
    return true;
}

std::vector<size_t> VisTree::sector_neighbours(size_t sector_index) {
    std::vector<size_t> out;
    for (const auto& p : sector_portals(sector_index)) {
        // The far side of this portal, whichever end we came in on.
        const auto other = p.sector_a == sector_index ? p.sector_b : p.sector_a;
        if (!other.has_value() || *other == sector_index) {
            continue;
        }
        bool dup = false;
        for (const size_t s : out) {
            if (s == *other) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(*other);
        }
    }
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// The guarded root read, POD-only: MSVC refuses __try in any function holding a type that
// unwinds, and objects_near's vectors are exactly that.
const regenny::LTWorldTreeNode* seh_world_tree_root(const regenny::LTWorldClientBSP* bsp) {
    const regenny::LTWorldTreeNode* r = nullptr;
    KANANLIB_SEH_TRY {
        r = bsp->world_tree_root;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r = nullptr;
    }
    return r;
}

// Descends the X/Z quadtree collecting object addresses from every node on the path.
//
// THE CONVENTION IS THE ENGINE'S, from LTWorldTree_FindNodeForObject: child index is
// (x > split_x ? 2 : 0) + (z > split_z ? 1 : 0), children laid out contiguously at
// `node + stride * (child_offset + k)`. Objects whose AABB STRADDLES a split are linked at
// that node rather than pushed down, which is why every node on the path is harvested and
// not just the leaf.
//
// AND IT BRANCHES ON A BOUNDARY, which is not a nicety. The engine sends a box high when
// `split <= aabb_min`, i.e. a box whose LOW EDGE SITS EXACTLY ON the split goes high; a
// point test of `p > split` sends that same coordinate LOW. Level brushes are axis-aligned
// grid geometry and their AABB edges land on split planes constantly, so the two disagree
// often -- and only for worldmodels, which is exactly the population that failed to
// self-locate before this. Following both children when the point is within kBoundarySlop
// of a split costs a few extra candidates and removes the whole class of miss.
constexpr float kBoundarySlop = 0.01f;

int64_t seh_objects_near(const regenny::LTWorldTreeNode* root, float px, float pz,
                         uintptr_t* out, size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const regenny::LTWorldTreeNode* stack[128];
        size_t sp = 0;
        stack[sp++] = root;
        size_t n = 0;
        size_t guard_nodes = 0;
        while (sp != 0 && guard_nodes < 512) {
            ++guard_nodes;
            const auto* node = stack[--sp];
            if (node == nullptr) {
                continue;
            }
            // Harvest this node's list. The head self-points when empty, and every element
            // is an LTObject's world_tree_link, so the object is `link - offsetof`.
            const auto* head = &node->objects;
            size_t guard = 0;
            for (const auto* l = node->objects.next; l != head && l != nullptr && guard < 4096;
                 l = l->next) {
                ++guard;
                if (n >= max_out) {
                    break;
                }
                out[n++] = reinterpret_cast<uintptr_t>(l) -
                           offsetof(regenny::LTObject, world_tree_link);
            }
            const uint16_t co = node->child_offset;
            if (co == 0) {
                continue;  // leaf
            }
            const float dx = px - node->split_x;
            const float dz = pz - node->split_z;
            // Which x halves to follow, and which z halves. On a boundary, both.
            const bool x_lo = dx <= kBoundarySlop;
            const bool x_hi = dx >= -kBoundarySlop;
            const bool z_lo = dz <= kBoundarySlop;
            const bool z_hi = dz >= -kBoundarySlop;
            for (size_t k = 0; k < 4; ++k) {
                const bool want_hi_x = (k & 2u) != 0;
                const bool want_hi_z = (k & 1u) != 0;
                if ((want_hi_x ? x_hi : x_lo) && (want_hi_z ? z_hi : z_lo)) {
                    if (sp >= 128) {
                        break;
                    }
                    stack[sp++] = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                        reinterpret_cast<uintptr_t>(node) +
                        sizeof(regenny::LTWorldTreeNode) * (static_cast<size_t>(co) + k));
                }
            }
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

}  // namespace

std::vector<const regenny::LTObject*> WorldBSP::objects_near(const regenny::LTVector& point,
                                                             size_t max_results) {
    std::vector<const regenny::LTObject*> out;
    const auto* bsp = get();
    if (bsp == nullptr) {
        return out;
    }
    const auto* root = seh_world_tree_root(bsp);
    if (root == nullptr) {
        return out;
    }
    if (max_results == 0 || max_results > 4096) {
        max_results = 256;
    }
    std::vector<uintptr_t> addrs(max_results);
    const int64_t n = seh_objects_near(root, point.x, point.z, addrs.data(), max_results);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        out.push_back(reinterpret_cast<const regenny::LTObject*>(addrs[static_cast<size_t>(i)]));
    }
    return out;
}

std::optional<bool> WorldBSP::is_linked(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    bool linked = false;
    bool ok = false;
    KANANLIB_SEH_TRY {
        // Self-pointing means unlinked -- that is what the engine's remove leaves behind.
        linked = obj->world_tree_link.next != &obj->world_tree_link;
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok ? std::optional<bool>{linked} : std::nullopt;
}

}  // namespace sdk

namespace sdk {

namespace {

// Walks the whole tree looking for the node whose object list contains `target`. POD out
// params so the guard holds nothing that unwinds. Returns depth, or -1 when not found.
int64_t seh_find_slot(const regenny::LTWorldTreeNode* root, uintptr_t target, uintptr_t* node,
                      float* sx, float* sz, bool* leaf) {
    int64_t depth = -1;
    KANANLIB_SEH_TRY {
        const regenny::LTWorldTreeNode* stack[256];
        size_t depth_of[256];
        size_t sp = 0;
        stack[sp] = root;
        depth_of[sp] = 0;
        ++sp;
        size_t visited = 0;
        while (sp != 0 && visited < 4096) {
            --sp;
            const auto* n = stack[sp];
            const size_t d = depth_of[sp];
            if (n == nullptr) {
                continue;
            }
            ++visited;
            const auto* head = &n->objects;
            size_t guard = 0;
            for (const auto* l = n->objects.next; l != head && l != nullptr && guard < 4096;
                 l = l->next) {
                ++guard;
                const uintptr_t obj = reinterpret_cast<uintptr_t>(l) -
                                      offsetof(regenny::LTObject, world_tree_link);
                if (obj == target) {
                    *node = reinterpret_cast<uintptr_t>(n);
                    *sx = n->split_x;
                    *sz = n->split_z;
                    *leaf = n->child_offset == 0;
                    depth = static_cast<int64_t>(d);
                    return depth;
                }
            }
            const uint16_t co = n->child_offset;
            if (co == 0 || sp + 4 > 256) {
                continue;
            }
            for (size_t k = 0; k < 4; ++k) {
                stack[sp] = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                    reinterpret_cast<uintptr_t>(n) +
                    sizeof(regenny::LTWorldTreeNode) * (static_cast<size_t>(co) + k));
                depth_of[sp] = d + 1;
                ++sp;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        depth = -1;
    }
    return depth;
}

}  // namespace

std::optional<WorldBSP::TreeSlot> WorldBSP::tree_slot(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto* bsp = get();
    if (bsp == nullptr) {
        return std::nullopt;
    }
    const auto* root = seh_world_tree_root(bsp);
    if (root == nullptr) {
        return std::nullopt;
    }
    uintptr_t node = 0;
    float sx = 0.0f, sz = 0.0f;
    bool leaf = false;
    const int64_t d =
        seh_find_slot(root, reinterpret_cast<uintptr_t>(obj), &node, &sx, &sz, &leaf);
    if (d < 0) {
        return std::nullopt;
    }
    TreeSlot out{};
    out.node = node;
    out.depth = static_cast<size_t>(d);
    out.split_x = sx;
    out.split_z = sz;
    out.leaf = leaf;
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// The ENGINE'S BOX DESCENT, transcribed instruction-for-instruction from
// LTWorldTree_FindNodeForObject (0x46462F). Reproduced rather than approximated because the
// whole point is to compare against what the engine chose:
//
//   if (split_x < aabb_max.x) {
//       if (split_x > aabb_min.x) stop;            // spans the x split
//       if (split_z < aabb_max.z) {
//           if (split_z > aabb_min.z) stop;        // spans the z split
//           k = 3;                                 // +x +z
//       } else k = 2;                              // +x -z
//   } else {
//       if (split_z >= aabb_max.z) k = 0;          // -x -z
//       else { if (split_z > aabb_min.z) stop; k = 1; }   // -x +z
//   }
//
// Note the asymmetry between the two halves is the ENGINE'S, not a transcription slip: the
// low-x branch tests `>=` against aabb_max.z where the high-x branch tests `<`.
int64_t seh_slot_for_bounds(const regenny::LTWorldTreeNode* root, const regenny::LTObject* obj,
                            uintptr_t* node_out, float* sx, float* sz, bool* leaf) {
    int64_t depth = -1;
    KANANLIB_SEH_TRY {
        const float min_x = obj->aabb_min.x, min_z = obj->aabb_min.z;
        const float max_x = obj->aabb_max.x, max_z = obj->aabb_max.z;
        const auto* node = root;
        size_t d = 0;
        while (node != nullptr && d < 64) {
            const uint16_t co = node->child_offset;
            size_t k = 0;
            bool stop = co == 0;
            if (!stop) {
                if (node->split_x < max_x) {
                    if (node->split_x > min_x) {
                        stop = true;
                    } else if (node->split_z < max_z) {
                        if (node->split_z > min_z) {
                            stop = true;
                        } else {
                            k = 3;
                        }
                    } else {
                        k = 2;
                    }
                } else if (node->split_z >= max_z) {
                    k = 0;
                } else if (node->split_z > min_z) {
                    stop = true;
                } else {
                    k = 1;
                }
            }
            if (stop) {
                *node_out = reinterpret_cast<uintptr_t>(node);
                *sx = node->split_x;
                *sz = node->split_z;
                *leaf = node->child_offset == 0;
                depth = static_cast<int64_t>(d);
                break;
            }
            node = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                reinterpret_cast<uintptr_t>(node) +
                sizeof(regenny::LTWorldTreeNode) * (static_cast<size_t>(co) + k));
            ++d;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        depth = -1;
    }
    return depth;
}

}  // namespace

std::optional<WorldBSP::TreeSlot> WorldBSP::slot_for_current_bounds(
    const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto* bsp = get();
    if (bsp == nullptr) {
        return std::nullopt;
    }
    const auto* root = seh_world_tree_root(bsp);
    if (root == nullptr) {
        return std::nullopt;
    }
    uintptr_t node = 0;
    float sx = 0.0f, sz = 0.0f;
    bool leaf = false;
    const int64_t d = seh_slot_for_bounds(root, obj, &node, &sx, &sz, &leaf);
    if (d < 0) {
        return std::nullopt;
    }
    TreeSlot out{};
    out.node = node;
    out.depth = static_cast<size_t>(d);
    out.split_x = sx;
    out.split_z = sz;
    out.leaf = leaf;
    return out;
}

std::optional<bool> WorldBSP::index_is_current(const regenny::LTObject* obj) {
    const auto actual = tree_slot(obj);
    const auto wanted = slot_for_current_bounds(obj);
    if (!actual.has_value() || !wanted.has_value()) {
        return std::nullopt;
    }
    return actual->node == wanted->node;
}

}  // namespace sdk

namespace sdk {

namespace {

// LTVisSector_AABBOverlapsSphere: the SQUARED distance from the centre to the box, against
// radius squared. An axis the centre already lies within contributes nothing.
bool aabb_overlaps_sphere(const VisTree::Sector& s, const regenny::LTVector& c, float radius) {
    const float mn[3] = {s.min.x, s.min.y, s.min.z};
    const float mx[3] = {s.max.x, s.max.y, s.max.z};
    const float p[3] = {c.x, c.y, c.z};
    float d2 = 0.0f;
    for (size_t i = 0; i < 3; ++i) {
        const float over = mn[i] > p[i] ? p[i] - mn[i] : (mx[i] < p[i] ? p[i] - mx[i] : 0.0f);
        d2 += over * over;
        if (d2 > radius * radius) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<bool> VisTree::sector_overlaps_sphere(size_t index,
                                                    const regenny::LTVector& center,
                                                    float radius) {
    const auto s = sector(index);
    if (!s.has_value()) {
        return std::nullopt;
    }
    // THE BOX FIRST, exactly as the engine orders it -- and it is the only test 244 of 263
    // sectors have.
    if (!aabb_overlaps_sphere(*s, center, radius)) {
        return false;
    }
    // Then the planes, rejecting only when the sphere lies WHOLLY on the negative side.
    // Positive is inside; see LTVisPlane.distance in the schema for how that was settled.
    for (const auto& pl : sector_planes(index)) {
        const float d = pl.normal.x * center.x + pl.normal.y * center.y +
                        pl.normal.z * center.z - pl.distance;
        if (d < -radius) {
            return false;
        }
    }
    return true;
}

namespace {

// LTVisTree_QuerySphere's descent, POD in and POD out.
int64_t seh_sphere_sectors(const regenny::LTVisTree* tree, const float c[3], float radius,
                           size_t* out, size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* root = tree->root;
        const auto* sectors = tree->sectors;
        const uint32_t sector_count = tree->sector_count;
        if (root == nullptr || sectors == nullptr || sector_count == 0) {
            return -1;
        }
        const regenny::LTVisTreeNode* stack[128];
        size_t sp = 0;
        stack[sp++] = root;
        size_t n = 0;
        size_t guard = 0;
        while (sp != 0 && guard < 4096) {
            ++guard;
            const auto* node = stack[--sp];
            if (node == nullptr) {
                continue;
            }
            const uint32_t count = node->element_count;
            if (node->elements != nullptr && count != 0 && count < 100000u) {
                for (uint32_t i = 0; i < count && n < max_out; ++i) {
                    const auto* s = node->elements[i];
                    if (s == nullptr) {
                        continue;
                    }
                    const auto sa = reinterpret_cast<uintptr_t>(s);
                    const auto ba = reinterpret_cast<uintptr_t>(sectors);
                    if (sa < ba) {
                        continue;
                    }
                    const uintptr_t off = sa - ba;
                    if (off % sizeof(regenny::LTVisSector) != 0) {
                        continue;
                    }
                    const uintptr_t idx = off / sizeof(regenny::LTVisSector);
                    if (idx >= sector_count) {
                        continue;
                    }
                    bool dup = false;
                    for (size_t k = 0; k < n; ++k) {
                        if (out[k] == static_cast<size_t>(idx)) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        out[n++] = static_cast<size_t>(idx);
                    }
                }
            }
            const uint32_t axis = node->split_axis;
            if (axis > 2u) {
                continue;  // leaf
            }
            if (sp + 2 > 128) {
                continue;
            }
            const float sv = node->split_value;
            const float ci = c[axis];
            // child_a is the LOW side, child_b the HIGH -- from LTVisTree_QuerySphere itself.
            if (sv > ci + radius) {
                stack[sp++] = node->child_a;
            } else if (sv < ci - radius) {
                stack[sp++] = node->child_b;
            } else {
                stack[sp++] = node->child_b;
                stack[sp++] = node->child_a;
            }
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

}  // namespace

std::vector<VisTree::Sector> VisTree::sectors_in_sphere(const regenny::LTVector& center,
                                                        float radius, size_t max_results) {
    std::vector<Sector> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    if (max_results == 0 || max_results > 4096) {
        max_results = 256;
    }
    const float c[3] = {center.x, center.y, center.z};
    std::vector<size_t> idx(max_results);
    const int64_t n = seh_sphere_sectors(tree, c, radius, idx.data(), max_results);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const size_t si = idx[static_cast<size_t>(i)];
        // The descent narrows; the per-sector test decides. Both are the engine's.
        if (!sector_overlaps_sphere(si, center, radius).value_or(false)) {
            continue;
        }
        if (const auto s = sector(si); s.has_value()) {
            out.push_back(*s);
        }
    }
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

struct QueryBox {
    float mn[3];
    float mx[3];
};

// A mod computing a play space from two tracked points has no reason to have sorted them.
QueryBox normalised_box(const regenny::LTVector& a, const regenny::LTVector& b) {
    const float av[3] = {a.x, a.y, a.z};
    const float bv[3] = {b.x, b.y, b.z};
    QueryBox out{};
    for (size_t i = 0; i < 3; ++i) {
        out.mn[i] = av[i] < bv[i] ? av[i] : bv[i];
        out.mx[i] = av[i] < bv[i] ? bv[i] : av[i];
    }
    return out;
}

}  // namespace

uint32_t VisTree::corner_code_for(const regenny::LTVector& normal) {
    // g_LTVisAABBCornerTable's encoding: bit0 -> x=max, bit1 -> y=MIN, bit2 -> z=max. The
    // engine wants the corner MAXIMISING dot(normal, corner), so each axis takes max where
    // the component is >= 0 and min where it is < 0 -- note y's bit is therefore inverted
    // relative to x and z. Reproduces all 125 live codes.
    uint32_t code = 0;
    if (normal.x >= 0.0f) {
        code |= 1u;
    }
    if (normal.y < 0.0f) {
        code |= 2u;
    }
    if (normal.z >= 0.0f) {
        code |= 4u;
    }
    return code;
}

std::optional<bool> VisTree::plane_corner_code_is_current(size_t sector_index,
                                                          size_t plane_index) {
    const auto planes = sector_planes(sector_index);
    if (plane_index >= planes.size()) {
        return std::nullopt;
    }
    const auto& pl = planes[plane_index];
    return pl.corner_code == corner_code_for(pl.normal);
}

std::optional<bool> VisTree::sector_overlaps_box(size_t index, const regenny::LTVector& min,
                                                 const regenny::LTVector& max) {
    const auto s = sector(index);
    if (!s.has_value()) {
        return std::nullopt;
    }
    const QueryBox q = normalised_box(min, max);
    // LTVisSector_AABBOverlapsAABB: separated on any axis means no overlap.
    const float smn[3] = {s->min.x, s->min.y, s->min.z};
    const float smx[3] = {s->max.x, s->max.y, s->max.z};
    for (size_t i = 0; i < 3; ++i) {
        if (q.mn[i] > smx[i] || q.mx[i] < smn[i]) {
            return false;
        }
    }
    // LTVisPlane_RejectsAABB: the corner reaching FURTHEST TOWARD the positive side, tested
    // against zero. No epsilon and no radius term -- the corner choice is the slack.
    //
    // The code is recomputed from the normal rather than read, on purpose: it is a cache, and
    // this way a stale one cannot make the SDK disagree with its own arithmetic. Ask
    // plane_corner_code_is_current() when the cache itself is the question.
    for (const auto& pl : sector_planes(index)) {
        const uint32_t code = corner_code_for(pl.normal);
        const float cx = (code & 1u) != 0 ? q.mx[0] : q.mn[0];
        const float cy = (code & 2u) != 0 ? q.mn[1] : q.mx[1];
        const float cz = (code & 4u) != 0 ? q.mx[2] : q.mn[2];
        if (pl.normal.x * cx + pl.normal.y * cy + pl.normal.z * cz - pl.distance < 0.0f) {
            return false;
        }
    }
    return true;
}

namespace {

// LTVisTree_QueryAABB's descent, POD in and POD out.
int64_t seh_box_sectors(const regenny::LTVisTree* tree, const QueryBox& q, size_t* out,
                        size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* root = tree->root;
        const auto* sectors = tree->sectors;
        const uint32_t sector_count = tree->sector_count;
        if (root == nullptr || sectors == nullptr || sector_count == 0) {
            return -1;
        }
        const regenny::LTVisTreeNode* stack[128];
        size_t sp = 0;
        stack[sp++] = root;
        size_t n = 0;
        size_t guard = 0;
        while (sp != 0 && guard < 4096) {
            ++guard;
            const auto* node = stack[--sp];
            if (node == nullptr) {
                continue;
            }
            const uint32_t count = node->element_count;
            if (node->elements != nullptr && count != 0 && count < 100000u) {
                for (uint32_t i = 0; i < count && n < max_out; ++i) {
                    const auto* s = node->elements[i];
                    if (s == nullptr) {
                        continue;
                    }
                    const auto sa = reinterpret_cast<uintptr_t>(s);
                    const auto ba = reinterpret_cast<uintptr_t>(sectors);
                    if (sa < ba) {
                        continue;
                    }
                    const uintptr_t off = sa - ba;
                    if (off % sizeof(regenny::LTVisSector) != 0) {
                        continue;
                    }
                    const uintptr_t idx = off / sizeof(regenny::LTVisSector);
                    if (idx >= sector_count) {
                        continue;
                    }
                    bool dup = false;
                    for (size_t k = 0; k < n; ++k) {
                        if (out[k] == static_cast<size_t>(idx)) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        out[n++] = static_cast<size_t>(idx);
                    }
                }
            }
            const uint32_t axis = node->split_axis;
            if (axis > 2u) {
                continue;  // leaf
            }
            if (sp + 2 > 128) {
                continue;
            }
            const float sv = node->split_value;
            // The box's OWN extents decide, which is the only difference from the sphere
            // walk: `split <= max` reaches the high side, `split >= min` also reaches the low.
            const bool high = sv <= q.mx[axis];
            const bool low = sv >= q.mn[axis];
            if (high && low) {
                stack[sp++] = node->child_b;
                stack[sp++] = node->child_a;
            } else if (high) {
                stack[sp++] = node->child_b;
            } else {
                stack[sp++] = node->child_a;
            }
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

}  // namespace

std::vector<VisTree::Sector> VisTree::sectors_in_box(const regenny::LTVector& min,
                                                     const regenny::LTVector& max,
                                                     size_t max_results) {
    std::vector<Sector> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    if (max_results == 0 || max_results > 4096) {
        max_results = 256;
    }
    const QueryBox q = normalised_box(min, max);
    std::vector<size_t> idx(max_results);
    const int64_t n = seh_box_sectors(tree, q, idx.data(), max_results);
    if (n <= 0) {
        return out;
    }
    out.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const size_t si = idx[static_cast<size_t>(i)];
        if (!sector_overlaps_box(si, min, max).value_or(false)) {
            continue;
        }
        if (const auto s = sector(si); s.has_value()) {
            out.push_back(*s);
        }
    }
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// LTSpatialRecord.entry_list threaded by record_next. Each entry's hit_head points AT the
// sector's own list-head slot, and that slot is at offset 0, so it IS the sector address --
// which is how an entry names its sector without storing an index.
int64_t seh_record_sectors(const regenny::LTObject* obj, const regenny::LTVisSector* sectors,
                           uint32_t sector_count, size_t* out, size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        const auto* rec = obj->spatial_record;
        if (rec == nullptr) {
            return -1;
        }
        size_t n = 0;
        const auto* e = static_cast<const regenny::LTSpatialEntry*>(
            static_cast<const void*>(rec->entry_list));
        size_t guard = 0;
        while (e != nullptr && n < max_out && guard < 4096) {
            ++guard;
            const auto sa = reinterpret_cast<uintptr_t>(e->hit_head);
            const auto ba = reinterpret_cast<uintptr_t>(sectors);
            if (sa >= ba) {
                const uintptr_t off = sa - ba;
                if (off % sizeof(regenny::LTVisSector) == 0) {
                    const uintptr_t idx = off / sizeof(regenny::LTVisSector);
                    if (idx < sector_count) {
                        out[n++] = static_cast<size_t>(idx);
                    }
                }
            }
            e = e->record_next;
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

// The reverse walk: LTVisSector.entry_list threaded by hit_next, each entry naming its owner
// through record->object.
int64_t seh_sector_objects(const regenny::LTVisSector* sector, const regenny::LTObject** out,
                           size_t max_out) {
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        size_t n = 0;
        const auto* e = sector->entry_list;
        size_t guard = 0;
        while (e != nullptr && n < max_out && guard < 65536) {
            ++guard;
            const auto* rec = static_cast<const regenny::LTSpatialRecord*>(
                static_cast<const void*>(e->record));
            if (rec != nullptr) {
                const auto* o = static_cast<const regenny::LTObject*>(
                    static_cast<const void*>(rec->object));
                if (o != nullptr) {
                    out[n++] = o;
                }
            }
            e = e->hit_next;
        }
        found = static_cast<int64_t>(n);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    return found;
}

int64_t seh_entry_count(const regenny::LTObject* obj) {
    int64_t n = -1;
    KANANLIB_SEH_TRY {
        const auto* rec = obj->spatial_record;
        n = rec == nullptr ? -1 : static_cast<int64_t>(rec->entry_count);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

}  // namespace

std::vector<size_t> VisTree::sectors_for_object(const regenny::LTObject* obj) {
    std::vector<size_t> out;
    const auto* tree = get();
    if (tree == nullptr || obj == nullptr) {
        return out;
    }
    const auto* sectors = tree->sectors;
    const uint32_t count = tree->sector_count;
    if (sectors == nullptr || count == 0) {
        return out;
    }
    // entry_count is a uint16, so this bound is the field's own maximum.
    std::vector<size_t> idx(1024);
    const int64_t n = seh_record_sectors(obj, sectors, count, idx.data(), idx.size());
    if (n <= 0) {
        return out;
    }
    out.assign(idx.begin(), idx.begin() + static_cast<size_t>(n));
    return out;
}

std::vector<const regenny::LTObject*> VisTree::objects_in_sector(size_t index) {
    std::vector<const regenny::LTObject*> out;
    const auto* tree = get();
    if (tree == nullptr) {
        return out;
    }
    const auto* sectors = tree->sectors;
    if (sectors == nullptr || index >= tree->sector_count) {
        return out;
    }
    std::vector<const regenny::LTObject*> buf(4096);
    const int64_t n = seh_sector_objects(&sectors[index], buf.data(), buf.size());
    if (n <= 0) {
        return out;
    }
    out.assign(buf.begin(), buf.begin() + static_cast<size_t>(n));
    return out;
}

std::optional<size_t> VisTree::spatial_entry_count(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const int64_t n = seh_entry_count(obj);
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

std::optional<VisTree::RecordDiff> VisTree::spatial_record_diff(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    // The volume the RECORD stores, not the one the object's fields imply -- holding it fixed
    // is what makes this a check of the collection step alone.
    const auto vol = cull_volume(obj);
    if (!vol.has_value()) {
        return std::nullopt;
    }
    std::vector<Sector> want;
    switch (vol->shape) {
    case CullShape::Sphere:
        want = sectors_in_sphere(vol->center, vol->radius, 1024);
        break;
    case CullShape::Box:
        want = sectors_in_box(vol->min, vol->max, 1024);
        break;
    case CullShape::None:
        break;  // nothing to collect
    }
    const auto have = sectors_for_object(obj);
    RecordDiff out{};
    out.stored = have.size();
    out.computed = want.size();
    for (const auto& w : want) {
        bool seen = false;
        for (const size_t h : have) {
            if (h == w.index) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++out.missing;
        }
    }
    for (const size_t h : have) {
        bool seen = false;
        for (const auto& w : want) {
            if (h == w.index) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++out.extra;
        }
    }
    return out;
}

std::optional<bool> VisTree::spatial_record_matches_volume(const regenny::LTObject* obj) {
    const auto d = spatial_record_diff(obj);
    if (!d.has_value()) {
        return std::nullopt;
    }
    return d->missing == 0 && d->extra == 0;
}

std::optional<bool> VisTree::spatial_record_is_consistent(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto rend = is_renderable(obj);
    if (!rend.has_value()) {
        return std::nullopt;
    }
    // Gated out: the engine ran Release, so the only correct state is an empty list. Note this
    // deliberately does NOT look at the volume, which stays current -- the volume write is
    // unconditional and only the collect is gated.
    if (!*rend) {
        return sectors_for_object(obj).empty();
    }
    return spatial_record_matches_volume(obj);
}

}  // namespace sdk

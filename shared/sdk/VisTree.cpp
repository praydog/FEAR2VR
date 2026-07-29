#include "VisTree.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "regenny/regenny/LTVisPortal.hpp"
#include "regenny/regenny/LTVisPlane.hpp"
#include "regenny/regenny/LTVisSector.hpp"
#include "regenny/regenny/LTVisTreeNode.hpp"
#include "regenny/regenny/LTWorldTreeLink.hpp"
#include "regenny/regenny/LTWorldTreeNode.hpp"

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

// Validates the portal table. The plane checks are what make this strong: a
// portal's own `center` and all of its vertices must satisfy its own plane
// equation, so a wrong offset anywhere in the record breaks the arithmetic
// rather than merely looking odd.
//
// POD-only for the guard; on a fault the counters keep whatever they reached, and
// the caller compares them against portal_count so a partial walk shows up as a
// shortfall rather than as success.
void seh_check_portals(const regenny::LTVisTree* tree, VisTree::TreeCheck* out) {
    KANANLIB_SEH_TRY {
        const auto* table = tree->portals;
        const uint32_t count = tree->portal_count;
        const auto* sectors = tree->sectors;
        const uint32_t sector_count = tree->sector_count;
        out->portal_count = count;
        if (table == nullptr || count == 0 || count > 65536u || sectors == nullptr) {
            return;
        }
        const auto ba = reinterpret_cast<uintptr_t>(sectors);
        const auto span = static_cast<uintptr_t>(sector_count) * sizeof(regenny::LTVisSector);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* p = table[i];
            if (p == nullptr) {
                continue;
            }
            const float nx = p->plane_normal.x, ny = p->plane_normal.y, nz = p->plane_normal.z;
            const float d = p->plane_distance;
            const float len2 = nx * nx + ny * ny + nz * nz;
            if (len2 > 0.98f && len2 < 1.02f) {
                ++out->portal_unit_normal;
            }
            const float cd = nx * p->center.x + ny * p->center.y + nz * p->center.z - d;
            if (cd > -0.5f && cd < 0.5f) {
                ++out->portal_center_on_plane;
            }

            const auto a = reinterpret_cast<uintptr_t>(p->sector_a);
            const auto b = reinterpret_cast<uintptr_t>(p->sector_b);
            const bool a_ok =
                a >= ba && a - ba < span && (a - ba) % sizeof(regenny::LTVisSector) == 0;
            const bool b_ok =
                b >= ba && b - ba < span && (b - ba) % sizeof(regenny::LTVisSector) == 0;
            if (a_ok && b_ok && a != b) {
                ++out->portal_sectors_ok;
            }

            // vertex_count is 4 on every live portal and the array holds exactly
            // 4, so anything larger would read past the record -- clamp rather
            // than trust the field.
            const uint32_t vc = p->vertex_count > 4u ? 4u : p->vertex_count;
            bool all = vc != 0;
            for (uint32_t v = 0; v < vc; ++v) {
                const float vd =
                    nx * p->vertices[v].x + ny * p->vertices[v].y + nz * p->vertices[v].z - d;
                if (vd < -0.5f || vd > 0.5f) {
                    all = false;
                }
            }
            if (all) {
                ++out->portal_verts_on_plane;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
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
    seh_check_portals(tree, &out);
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
    }
    return out;
}

std::optional<bool> VisTree::sector_contains(size_t index, const regenny::LTVector& point,
                                             float slop) {
    const auto s = sector(index);
    if (!s.has_value()) {
        return std::nullopt;
    }
    // THE BOX FIRST, because it is what a sector actually has: only 19 of 263 live sectors
    // carry planes, so a plane-only test cannot answer for the rest.
    if (point.x < s->min.x - slop || point.x > s->max.x + slop ||
        point.y < s->min.y - slop || point.y > s->max.y + slop ||
        point.z < s->min.z - slop || point.z > s->max.z + slop) {
        return false;
    }
    // Then any planes, which refine the box where the art provided them.
    for (const auto& pl : sector_planes(index)) {
        const float d = pl.normal.x * point.x + pl.normal.y * point.y +
                        pl.normal.z * point.z - pl.distance;
        if (d > slop) {
            return false;
        }
    }
    return true;
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

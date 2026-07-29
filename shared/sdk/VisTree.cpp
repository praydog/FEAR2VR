#include "VisTree.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

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

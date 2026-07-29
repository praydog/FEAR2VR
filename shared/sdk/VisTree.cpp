#include "VisTree.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "regenny/regenny/LTVisSector.hpp"
#include "regenny/regenny/LTVisTreeNode.hpp"

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

} // namespace sdk

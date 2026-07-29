#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "regenny/regenny/LTObject.hpp"
#include "regenny/regenny/LTRotation.hpp"
#include "regenny/regenny/LTVector.hpp"

// Consumer-facing model and skeleton access.
//
// Everything here answers a question a mod actually asks -- "what model is this
// object?", "where is its head bone?", "what is that bone's pose?" -- rather than
// a question the test suite asks. The CClientMgr::check_* family validates that
// the mapping underneath is still correct; this is what you build on top of it.
//
// Two rules shape the interface:
//
//  * Nothing returns a raw engine pointer. The skeleton lives in an allocation
//    owned by a SHARED asset that can be released when the last model referencing
//    it goes away, so handing out `const char*` into it would be a use-after-free
//    waiting for a level transition. Names and lists are copied out.
//
//  * Every read is SEH-guarded and bounds-checked against the engine's own counts,
//    so a stale view returns nullopt rather than faulting. That matters because a
//    view is only valid for as long as the object lives, and a mod holding one
//    across a load screen is the expected mistake, not an exotic one.
namespace sdk {

// A read-only view of one model object's skeleton.
//
// Construction is two pointer reads, so build it per use rather than caching it.
// The node data belongs to the shared LTModelAsset, which means every model using
// the same .mdl sees the same skeleton -- node indices are stable per ASSET, not
// per object, and are safe to remember for as long as the asset stays loaded.
class ModelSkeleton {
public:
    // nullopt when `obj` is not an OT_MODEL, has no asset, or the asset carries no
    // node array. Does not fault on a dangling object.
    static std::optional<ModelSkeleton> from_object(const regenny::LTObject* obj);

    size_t node_count() const { return m_count; }

    // Copied out, because the engine's storage may be freed with the asset.
    std::optional<std::string> node_name(size_t index) const;

    // Case-insensitive, matching how the engine matches names itself (its own
    // lookup hash folds case through a translation table). Returns the node index,
    // which is what every other method here takes.
    //
    // Linear over the name array. That is deliberate: skeletons here run 2..84
    // nodes, so a scan is a few hundred byte comparisons, and it needs no engine
    // data beyond what this view already holds. The engine's hash array is used by
    // the test suite to prove the mapping, not by this lookup.
    std::optional<size_t> find_node(std::string_view name) const;

    // nullopt for the root (which stores 255, the engine's "no parent" sentinel)
    // and for an out-of-range index.
    std::optional<size_t> parent_of(size_t index) const;

    // A node's children are a CONTIGUOUS run, so this is a range rather than a
    // list: indices [first, first + count).
    struct Children {
        size_t first;
        size_t count;
    };
    std::optional<Children> children_of(size_t index) const;

    // Each node stores TWO (position, rotation) pairs. Which is which is NOT
    // established -- the obvious reading, that the second is the first's rigid
    // inverse, was tested and fails on most nodes (see fear2.genny's LTModelNode).
    // They are exposed raw and named non-committally so a caller can experiment
    // without inheriting a guess dressed up as an API.
    struct Pose {
        regenny::LTVector position;
        regenny::LTRotation rotation;
    };
    std::optional<Pose> pose_a(size_t index) const;
    std::optional<Pose> pose_b(size_t index) const;

    // Indices from `index` up to and including the root, nearest first. Useful for
    // composing a world transform, since the array is in topological order and a
    // parent always precedes its children. Bounded by node_count, so a corrupt
    // parent chain returns nullopt instead of looping.
    std::optional<std::vector<size_t>> path_to_root(size_t index) const;

    // The asset backing this skeleton. Two models with the same value share node
    // indices; comparing it is how a caller can cache per-asset work.
    uintptr_t asset_id() const { return reinterpret_cast<uintptr_t>(m_asset); }

private:
    const void* m_asset{};
    const void* m_records{};
    const void* m_names{};
    size_t m_count{};
};

// The object's .mdl path, e.g. "char\ai\rep_heavyweapons\rep_heavyweapons.mdl".
// This is the cheapest way to identify what an object IS -- far more reliable than
// inferring it from geometry -- and it is the engine's own cache key, so it matches
// case-insensitively against whatever a level references.
std::optional<std::string> model_filename(const regenny::LTObject* obj);

// The object's material paths (the .mat files it renders with). Empty entries are
// legal and are returned as empty strings, because the slot count is meaningful.
std::optional<std::vector<std::string>> model_materials(const regenny::LTObject* obj);

}  // namespace sdk

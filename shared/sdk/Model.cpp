#include "Model.hpp"

#include <cmath>

#include <windows.h>

#include "Memory.hpp"
#include "CClientMgr.hpp"
#include "Modules.hpp"
#include "interfaces/ILTModel.hpp"
#include "Object.hpp"
#include "regenny/regenny/LTModelAsset.hpp"
#include "regenny/regenny/LTAnimNameEntry.hpp"
#include "regenny/regenny/LTModelSocket.hpp"
#include "regenny/regenny/LTNodeTransform.hpp"
#include "regenny/regenny/LTAnimRecordSlot.hpp"
#include "regenny/regenny/LTAnimRecord.hpp"
#include "regenny/regenny/LTMatrix3x4.hpp"
#include "regenny/regenny/LTModelNode.hpp"
#include "regenny/regenny/LTModelObject.hpp"
#include "regenny/regenny/StdString.hpp"

namespace sdk {

namespace {

// Bounds every string decode. A model or material path is a filename; anything
// longer is a bad read, not a long name, and clamping before the copy is what
// keeps a moved offset from turning into a huge memcpy.
constexpr size_t kMaxPath = 260;
constexpr size_t kMaxName = 128;
constexpr size_t kMaxNodes = 1024;
constexpr size_t kMaxMaterials = 64;

// Case-insensitive compare over the printable range, deliberately not <cctype>:
// this runs inside SEH guards, and the engine folds case with a plain table too.
bool equals_i(const char* a, const char* b, size_t cap) {
    for (size_t i = 0; i < cap; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb + 32);
        }
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
    return false;
}

// POD result of resolving an object to its skeleton. Separate from the class so
// the SEH guard never touches a type with a destructor.
struct SkelRaw {
    const void* asset;
    const void* records;
    const void* names;
    uint32_t count;
    // The engine's per-object allocation, reconstructed from the SAME expression
    // LTModelObject_BindAsset uses to size it. Carrying the extent (not just the
    // base) is what lets every later read be bounded exactly rather than merely
    // "at or after the base".
    uintptr_t alloc_base;
    uintptr_t alloc_end;
    // Sockets are per-asset, read under the same guard as the nodes so a caller sees
    // one consistent asset rather than two reads of a possibly-rebound one.
    const void* sockets;
    uint32_t socket_count;
    // The ACTIVE cache, chosen exactly as LTModelObject_GetNodeTransform chooses:
    // the mode selector picks block130's pair (dirty stride 3, flag at byte +1) or
    // block120's (dirty stride 2, flag at byte +0).
    const void* node_xform;
    const void* node_dirty;
    uint32_t dirty_stride;
    uint32_t dirty_offset;
    bool world_space;
};

bool seh_resolve(const regenny::LTObject* obj, SkelRaw* out) {
    if (obj == nullptr) {
        return false;
    }
    // THE TYPE CHECK, preserved exactly: only an OT_MODEL object is resolved further.
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return false;
    }
    const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);

    // The whole model struct, in ONE guarded read -- the same "read under one guard" the
    // original relied on for internal consistency, now expressed as a single bulk copy
    // instead of a run of individual field dereferences inside one SEH frame.
    regenny::LTModelObject model_copy{};
    if (!sdk::mem::copy(&model_copy, reinterpret_cast<uintptr_t>(model), sizeof(model_copy))) {
        return false;
    }
    const auto* asset = model_copy.record.asset;
    if (asset == nullptr) {
        return false;
    }

    // Likewise the asset, in one guarded read, so records/names/count/sockets all come
    // from the same asset generation.
    regenny::LTModelAsset asset_copy{};
    if (!sdk::mem::copy(&asset_copy, reinterpret_cast<uintptr_t>(asset), sizeof(asset_copy))) {
        return false;
    }

    const auto* recs = asset_copy.node_records;
    const auto* names = asset_copy.node_names;
    const uint32_t n = asset_copy.node_count;
    if (recs == nullptr || names == nullptr || n == 0 || n > kMaxNodes) {
        return false;
    }
    out->asset = asset;
    out->records = recs;
    out->names = names;
    out->count = n;
    // A socket table is OPTIONAL: an asset may define none, so a
    // zero count is a normal answer and not a resolve failure.
    const auto* socks = asset_copy.sockets;
    const uint32_t sn = asset_copy.socket_count;
    if (socks != nullptr && sn > 0 && sn <= kMaxNodes) {
        out->sockets = socks;
        out->socket_count = sn;
    }

    // THE ENGINE'S OWN BRANCH, reproduced rather than chosen: a model
    // holds two per-node caches and GetNodeTransform reads whichever
    // the mode selector at +0x156 names. Picking one here would be
    // right on 193 of 215 models and silently wrong on the other 22.
    if (model_copy.sphere_source != 0) {
        out->node_xform = model_copy.block_130.node_transforms;
        out->node_dirty = model_copy.block_130.node_dirty_stride3;
        out->dirty_stride = 3;
        out->dirty_offset = 1;  // the engine tests the SECOND byte
    } else {
        out->node_xform = model_copy.block_120.node_transforms;
        out->node_dirty = model_copy.block_120.node_dirty_stride2;
        out->dirty_stride = 2;
        out->dirty_offset = 0;
    }
    // WHICH SPACE that cache is in. Measured, not assumed: over every
    // clean slot, selector==0 put 297/297 bones near the model origin
    // and selector!=0 put 46/46 at the object's world position.
    out->world_space = model_copy.sphere_source != 0;

    // BindAsset's own arithmetic, term for term. Verified live: every
    // pointer the engine carves out lands inside the result on
    // 215/215 models, including the bone palette's full extent.
    const auto align4 = [](uint32_t v) { return v + 3u - ((v + 3u) & 3u); };
    const uint32_t c08 = asset_copy.region_count_a;
    const uint32_t c10 = asset_copy.region_count_b;
    uint32_t size = align4(34u * n) + align4(31u * n + 4u * (c08 + c10)) +
                    align4(28u * asset_copy.material_count);
    if ((model_copy.base.flags3 & 0x400) != 0) {
        size += align4(2u * n) + align4(48u * n);
    }
    const auto base = reinterpret_cast<uintptr_t>(model_copy.per_node_alloc);
    out->alloc_base = base;
    out->alloc_end = (base != 0) ? base + size : 0;
    return true;
}

// Copies a NUL-terminated name out of engine memory into `dst`. Returns the length,
// or -1 on fault / an unterminated or non-printable run.
int64_t seh_copy_cstr(const char* src, char* dst, size_t cap) {
    if (src == nullptr) {
        return -1;
    }
    // Printable and terminated within cap-1 bytes, exactly as the original loop bound it,
    // but with no minimum length: an immediately-terminated (empty) string is still a
    // valid answer here.
    const auto text = sdk::mem::read_name(reinterpret_cast<uintptr_t>(src), cap - 1, /*min_length=*/0);
    if (!text.has_value()) {
        return -1;
    }
    for (size_t i = 0; i < text->size(); ++i) {
        dst[i] = (*text)[i];
    }
    dst[text->size()] = '\0';
    return static_cast<int64_t>(text->size());
}

int64_t seh_find_name(const void* names_v, uint32_t count, const char* needle, char* scratch) {
    const auto* names = static_cast<const char* const*>(names_v);
    for (uint32_t i = 0; i < count; ++i) {
        const char* nm = nullptr;
        if (!sdk::mem::copy(&nm, reinterpret_cast<uintptr_t>(&names[i]), sizeof(nm))) {
            return -1;  // any fault anywhere aborts the whole search, matching the original guard
        }
        if (nm == nullptr) {
            continue;
        }
        bool matched = false;
        for (size_t j = 0; j < kMaxName; ++j) {
            char ca = 0;
            if (!sdk::mem::copy(&ca, reinterpret_cast<uintptr_t>(nm + j), sizeof(ca))) {
                return -1;
            }
            char cb = needle[j];
            if (ca >= 'A' && ca <= 'Z') {
                ca = static_cast<char>(ca + 32);
            }
            if (cb >= 'A' && cb <= 'Z') {
                cb = static_cast<char>(cb + 32);
            }
            if (ca != cb) {
                break;
            }
            if (ca == '\0') {
                matched = true;
                break;
            }
        }
        if (matched) {
            (void)scratch;
            return static_cast<int64_t>(i);
        }
    }
    (void)scratch;
    return -1;
}

struct NodeRaw {
    uint8_t parent;
    uint8_t first_child_offset;
    uint8_t child_count;
    regenny::LTVector pos_a;
    regenny::LTRotation rot_a;
    regenny::LTVector pos_b;
    regenny::LTRotation rot_b;
};

bool seh_read_node(const void* records, uint32_t count, size_t index, NodeRaw* out) {
    if (index >= count) {
        return false;
    }
    const auto* nd = static_cast<const regenny::LTModelNode*>(records) + index;
    uint8_t parent = 0;
    uint8_t first_child_offset = 0;
    uint8_t child_count = 0;
    regenny::LTVector pos_a{};
    regenny::LTRotation rot_a{};
    regenny::LTVector pos_b{};
    regenny::LTRotation rot_b{};
    bool ok = true;
    ok = ok && sdk::mem::copy(&parent, reinterpret_cast<uintptr_t>(&nd->parent_index), sizeof(parent));
    ok = ok && sdk::mem::copy(&first_child_offset, reinterpret_cast<uintptr_t>(&nd->first_child_offset), sizeof(first_child_offset));
    ok = ok && sdk::mem::copy(&child_count, reinterpret_cast<uintptr_t>(&nd->child_count), sizeof(child_count));
    ok = ok && sdk::mem::copy(&pos_a, reinterpret_cast<uintptr_t>(&nd->inverse_bind_position), sizeof(pos_a));
    ok = ok && sdk::mem::copy(&rot_a, reinterpret_cast<uintptr_t>(&nd->inverse_bind_rotation), sizeof(rot_a));
    ok = ok && sdk::mem::copy(&pos_b, reinterpret_cast<uintptr_t>(&nd->anim_fallback_position), sizeof(pos_b));
    ok = ok && sdk::mem::copy(&rot_b, reinterpret_cast<uintptr_t>(&nd->anim_getter_rotation), sizeof(rot_b));
    if (!ok) {
        return false;
    }
    out->parent = parent;
    out->first_child_offset = first_child_offset;
    out->child_count = child_count;
    out->pos_a = pos_a;
    out->rot_a = rot_a;
    out->pos_b = pos_b;
    out->rot_b = rot_b;
    return true;
}

// Walks parent links into a POD buffer. Returns the count written, or -1 if the
// chain did not terminate within the node count (which means a corrupt parent).
int64_t seh_walk_parents(const void* records, uint32_t count, size_t start, size_t* out,
                         size_t cap) {
    const auto* nodes = static_cast<const regenny::LTModelNode*>(records);
    size_t n = 0, cur = start;
    bool terminated = false;
    while (n < cap) {
        out[n++] = cur;
        uint8_t parent = 0;
        if (!sdk::mem::copy(&parent, reinterpret_cast<uintptr_t>(&nodes[cur].parent_index), sizeof(parent))) {
            return -1;
        }
        if (parent == 255) {
            terminated = true;
            break;
        }
        if (parent >= count || parent >= cur) {
            break;  // topological order guarantees parent < cur; refuse a loop
        }
        cur = parent;
    }
    return terminated ? static_cast<int64_t>(n) : -1;
}

// Reads material_names[index] out of the model's std::string array.
int64_t seh_copy_material(const regenny::LTModelObject* model, uint32_t index, char* dst,
                          size_t cap) {
    regenny::StdString* arr = nullptr;
    uint32_t material_count = 0;
    if (!sdk::mem::copy(&arr, reinterpret_cast<uintptr_t>(&model->material_names), sizeof(arr)) ||
        !sdk::mem::copy(&material_count, reinterpret_cast<uintptr_t>(&model->material_count), sizeof(material_count))) {
        return -1;
    }
    if (arr == nullptr || index >= material_count) {
        return -1;
    }
    regenny::StdString s{};
    if (!sdk::mem::copy(&s, reinterpret_cast<uintptr_t>(&arr[index]), sizeof(s))) {
        return -1;
    }
    if (!(s.size <= s.capacity && s.size < cap)) {
        return -1;
    }
    // capacity >= 16 means the body moved to the heap and buf holds the
    // pointer; below that the body IS buf.
    const char* data = (s.capacity >= 16) ? *reinterpret_cast<const char* const*>(s.buf)
                                          : reinterpret_cast<const char*>(s.buf);
    if (data == nullptr) {
        return -1;
    }
    if (s.capacity >= 16) {
        // The string body lives on the engine's heap; fetch it through the guard.
        if (!sdk::mem::copy(dst, reinterpret_cast<uintptr_t>(data), s.size)) {
            return -1;
        }
    } else {
        // The body is inline in `s.buf`, already copied into local memory above.
        for (uint32_t i = 0; i < s.size; ++i) {
            dst[i] = data[i];
        }
    }
    dst[s.size] = '\0';
    return static_cast<int64_t>(s.size);
}

int64_t seh_material_count(const regenny::LTModelObject* model) {
    uint32_t c = 0;
    regenny::StdString* names = nullptr;
    if (!sdk::mem::copy(&c, reinterpret_cast<uintptr_t>(&model->material_count), sizeof(c)) ||
        !sdk::mem::copy(&names, reinterpret_cast<uintptr_t>(&model->material_names), sizeof(names))) {
        return -1;
    }
    if (names == nullptr || c > kMaxMaterials) {
        return -1;
    }
    return static_cast<int64_t>(c);
}

// Two one-line pointer reads that exist only because __try cannot share a function
// with anything that unwinds -- the callers build a std::string, so the guarded
// read has to live out here.
const char* seh_name_ptr(const void* names, size_t index) {
    const auto* arr = static_cast<const char* const*>(names);
    const char* p = nullptr;
    if (!sdk::mem::copy(&p, reinterpret_cast<uintptr_t>(&arr[index]), sizeof(p))) {
        return nullptr;
    }
    return p;
}

const char* seh_asset_filename(const regenny::LTModelObject* model) {
    regenny::LTModelAsset* asset = nullptr;
    if (!sdk::mem::copy(&asset, reinterpret_cast<uintptr_t>(&model->record.asset), sizeof(asset)) ||
        asset == nullptr) {
        return nullptr;
    }
    char* p = nullptr;
    if (!sdk::mem::copy(&p, reinterpret_cast<uintptr_t>(&asset->filename), sizeof(p))) {
        return nullptr;
    }
    return p;
}

const regenny::LTModelObject* as_model(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return nullptr;
    }
    // THE TYPE CHECK, preserved exactly: only the guarded byte access moved to sdk::mem.
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return nullptr;
    }
    return reinterpret_cast<const regenny::LTModelObject*>(obj);
}

}  // namespace

std::optional<ModelSkeleton> ModelSkeleton::from_object(const regenny::LTObject* obj) {
    SkelRaw raw{};
    if (!seh_resolve(obj, &raw)) {
        return std::nullopt;
    }
    ModelSkeleton s;
    s.m_object = obj;
    s.m_alloc_base = raw.alloc_base;
    s.m_alloc_end = raw.alloc_end;
    s.m_asset = raw.asset;
    s.m_records = raw.records;
    s.m_names = raw.names;
    s.m_count = raw.count;
    s.m_sockets = raw.sockets;
    s.m_socket_count = raw.socket_count;
    s.m_node_xform = raw.node_xform;
    s.m_node_dirty = raw.node_dirty;
    s.m_dirty_stride = raw.dirty_stride;
    s.m_dirty_offset = raw.dirty_offset;
    s.m_world_space = raw.world_space;
    return s;
}

std::optional<std::string> ModelSkeleton::node_name(size_t index) const {
    if (index >= m_count) {
        return std::nullopt;
    }
    const char* src = seh_name_ptr(m_names, index);
    char buf[kMaxName]{};
    const int64_t len = seh_copy_cstr(src, buf, sizeof(buf));
    if (len < 0) {
        return std::nullopt;
    }
    return std::string(buf, static_cast<size_t>(len));
}

std::optional<size_t> ModelSkeleton::find_node(std::string_view name) const {
    if (name.empty() || name.size() >= kMaxName) {
        return std::nullopt;
    }
    char needle[kMaxName]{};
    for (size_t i = 0; i < name.size(); ++i) {
        needle[i] = name[i];
    }
    const int64_t idx = seh_find_name(m_names, static_cast<uint32_t>(m_count), needle, nullptr);
    if (idx < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(idx);
}

std::optional<size_t> ModelSkeleton::parent_of(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    if (nd.parent == 255 || nd.parent >= m_count) {
        return std::nullopt;  // the root, or a parent that does not exist
    }
    return static_cast<size_t>(nd.parent);
}

std::optional<ModelSkeleton::Children> ModelSkeleton::children_of(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    const size_t first = index + nd.first_child_offset;
    const size_t count = nd.child_count;
    if (count == 0) {
        return Children{first, 0};
    }
    if (first + count > m_count) {
        return std::nullopt;  // the range the engine would read is out of bounds
    }
    return Children{first, count};
}



std::optional<std::vector<size_t>> ModelSkeleton::path_to_root(size_t index) const {
    if (index >= m_count) {
        return std::nullopt;
    }
    size_t buf[kMaxNodes]{};
    const int64_t n = seh_walk_parents(m_records, static_cast<uint32_t>(m_count), index, buf,
                                       m_count < kMaxNodes ? m_count : kMaxNodes);
    if (n < 0) {
        return std::nullopt;
    }
    return std::vector<size_t>(buf, buf + static_cast<size_t>(n));
}

std::optional<std::string> model_filename(const regenny::LTObject* obj) {
    const auto* model = as_model(obj);
    if (model == nullptr) {
        return std::nullopt;
    }
    const char* src = seh_asset_filename(model);
    char buf[kMaxPath]{};
    const int64_t len = seh_copy_cstr(src, buf, sizeof(buf));
    if (len < 0) {
        return std::nullopt;
    }
    return std::string(buf, static_cast<size_t>(len));
}

std::optional<std::vector<std::string>> model_materials(const regenny::LTObject* obj) {
    const auto* model = as_model(obj);
    if (model == nullptr) {
        return std::nullopt;
    }
    const int64_t count = seh_material_count(model);
    if (count < 0) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        char buf[kMaxPath]{};
        const int64_t len = seh_copy_material(model, static_cast<uint32_t>(i), buf, sizeof(buf));
        // An empty or unreadable slot still occupies an index, and the slot count is
        // meaningful to a caller, so hold the position rather than compacting.
        out.emplace_back(len > 0 ? std::string(buf, static_cast<size_t>(len)) : std::string{});
    }
    return out;
}

}  // namespace sdk

namespace sdk {

std::vector<ModelMatch> find_models(std::string_view needle, size_t max) {
    std::vector<ModelMatch> out;
    auto* mgr = CClientMgr::get();
    if (mgr == nullptr || max == 0) {
        return out;
    }

    // Snapshot, then resolve. The snapshot is what makes this safe to call from a
    // thread that is not the one mutating the object lists; the pointers it hands
    // back are still only good for this frame, which is why each match also carries
    // a handle.
    std::vector<CClientMgr::ObjectSnapshot> snaps(2048);
    const auto taken = mgr->snapshot_objects(static_cast<ObjectType>(1), snaps.data(), snaps.size());
    if (!taken.has_value()) {
        return out;
    }

    std::string lowered;
    lowered.reserve(needle.size());
    for (const char c : needle) {
        lowered += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }

    for (size_t i = 0; i < *taken && out.size() < max; ++i) {
        const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
        auto file = model_filename(obj);
        if (!file.has_value()) {
            continue;
        }
        if (!lowered.empty()) {
            std::string hay;
            hay.reserve(file->size());
            for (const char c : *file) {
                hay += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            }
            if (hay.find(lowered) == std::string::npos) {
                continue;
            }
        }
        const auto h = mgr->handle_of(obj);
        out.push_back(ModelMatch{obj, std::move(*file), h.value_or(CClientMgr::kNoHandle)});
    }
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// Reads one 48-byte palette entry, bounded EXACTLY: the read must fall inside the
// engine's own per-object allocation, whose extent is reconstructed from the same
// size expression LTModelObject_BindAsset uses (verified against every carved
// pointer on 215/215 models). An earlier version only required the region pointer to
// be at or after the allocation base, which would not have caught a stale region
// pointer left over from a rebind -- the base moves, the region pointer does not, and
// ">= base" happily accepts a read past the end of a smaller new block.
bool seh_bone_matrix(const void* obj_v, size_t index, size_t count, uintptr_t alloc_base,
                     uintptr_t alloc_end, float* out12) {
    const auto* model = static_cast<const regenny::LTModelObject*>(obj_v);
    regenny::LTMatrix3x4* mats = nullptr;
    if (!sdk::mem::copy(&mats, reinterpret_cast<uintptr_t>(&model->node_matrices), sizeof(mats))) {
        return false;
    }
    const auto first = reinterpret_cast<uintptr_t>(mats);
    const auto want_end = first + sizeof(regenny::LTMatrix3x4) * (index + 1);
    if (mats == nullptr || alloc_base == 0 || alloc_end <= alloc_base || index >= count ||
        first < alloc_base || want_end > alloc_end) {
        return false;
    }
    return sdk::mem::copy(out12, reinterpret_cast<uintptr_t>(mats + index), sizeof(float) * 12);
}

}  // namespace

std::optional<ModelSkeleton::BoneMatrix> ModelSkeleton::bone_matrix(size_t index) const {
    if (index >= m_count || m_object == nullptr) {
        return std::nullopt;
    }
    BoneMatrix out{};
    if (!seh_bone_matrix(m_object, index, m_count, m_alloc_base, m_alloc_end, out.m)) {
        return std::nullopt;
    }
    return out;
}

bool ModelSkeleton::is_populated(const BoneMatrix& mat) {
    // Finite and not all zero. Nothing stronger, on purpose: a written palette entry
    // is whatever the renderer put there, and an UNWRITTEN one is zeros -- so "has
    // anyone touched this" is the only question the data can answer. Testing for a
    // proper rotation (the previous version of this) rejected perfectly good entries
    // and told a caller nothing about whether the frame had filled them in.
    bool any = false;
    for (int i = 0; i < 12; ++i) {
        const float v = mat.m[i];
        if (!(v > -1.0e9f && v < 1.0e9f)) {
            return false;  // NaN or absurd: not a value the renderer wrote
        }
        if (v > 1.0e-9f || v < -1.0e-9f) {
            any = true;
        }
    }
    return any;
}

}  // namespace sdk

namespace sdk {

namespace {

struct AnimRaw {
    uint16_t index;
    uint16_t index_b;
    uint16_t node_a;
    uint16_t node_b;
    float fraction;
    uint32_t anim_count;
    bool ok;
};

// One guarded read of both the record's animation words and the asset's table
// length, so a caller gets the value and its bound from the same instant. Reading
// them in two calls would let the asset change between, which is exactly the kind of
// seam that produces an "index out of range" that never actually happened.
AnimRaw seh_anim(const regenny::LTObject* obj) {
    AnimRaw r{};
    if (obj == nullptr) {
        return r;
    }
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return r;
    }
    const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
    // The record and the asset's table length, in one guarded read each, so a caller
    // gets the value and its bound from as close to the same instant as sdk::mem allows.
    regenny::LTModelObject model_copy{};
    if (!sdk::mem::copy(&model_copy, reinterpret_cast<uintptr_t>(model), sizeof(model_copy))) {
        return r;
    }
    const auto* asset = model_copy.record.asset;
    if (asset == nullptr) {
        return r;
    }
    regenny::LTModelAsset asset_copy{};
    if (!sdk::mem::copy(&asset_copy, reinterpret_cast<uintptr_t>(asset), sizeof(asset_copy))) {
        return r;
    }
    r.index = model_copy.record.anim_index;
    r.index_b = model_copy.record.current_anim;
    r.fraction = model_copy.record.anim_fraction;
    r.node_a = model_copy.record.node_a;
    r.node_b = model_copy.record.node_b;
    const auto* first = asset_copy.anim_names.first;
    const auto* last = asset_copy.anim_names.last;
    if (first != nullptr && last >= first) {
        r.anim_count = static_cast<uint32_t>(last - first);
    }
    r.ok = true;
    return r;
}

}  // namespace

std::optional<AnimState> model_anim_state(const regenny::LTObject* obj) {
    const AnimRaw r = seh_anim(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    return AnimState{r.index, r.index_b, r.fraction, r.node_a, r.node_b};
}

std::optional<size_t> model_anim_count(const regenny::LTObject* obj) {
    const AnimRaw r = seh_anim(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    return static_cast<size_t>(r.anim_count);
}

}  // namespace sdk

namespace sdk {

namespace {

// POD copy of one socket record, taken under the guard. The name is copied into a
// asset's blob and dies with it. The NAME POINTER is captured here and copied by the
// caller through seh_copy_cstr -- that helper carries its own guard, and nesting SEH
// frames is exactly the mistake this split avoids.
struct SocketRaw {
    const char* name_ptr;
    uint32_t node_index;
    float pos[3];
    float rot[4];
    bool ok;
};

SocketRaw seh_socket(const void* base, size_t index, size_t count, size_t node_count) {
    SocketRaw r{};
    if (base == nullptr || index >= count) {
        return r;
    }
    const auto* s = static_cast<const regenny::LTModelSocket*>(base) + index;
    regenny::LTModelSocket copy{};
    if (!sdk::mem::copy(&copy, reinterpret_cast<uintptr_t>(s), sizeof(copy))) {
        return r;
    }
    // The engine bounds this itself when it uses the socket -- GetSocketTransform
    // hands node_index straight to GetNodeTransform, which range-checks against
    // node_count. Checking here means a caller never receives an index it cannot
    // then feed back into node_name()/bone_matrix().
    if (copy.node_index < node_count) {
        r.node_index = copy.node_index;
        r.pos[0] = copy.position.x;
        r.pos[1] = copy.position.y;
        r.pos[2] = copy.position.z;
        r.rot[0] = copy.rotation.x;
        r.rot[1] = copy.rotation.y;
        r.rot[2] = copy.rotation.z;
        r.rot[3] = copy.rotation.w;
        r.name_ptr = copy.name;
        r.ok = true;
    }
    return r;
}

} // namespace

namespace {

// Offsets named by the LogModels CSV header; see Model.hpp for the derivation. Read as raw offsets rather
// than through the schema because two of the three are fields the schema still calls region_count_a and
// unk_52 -- renaming those is a separate change to fear2.genny and its generated headers.
constexpr uintptr_t kAssetPhysicsNodeCount = 0x08;
constexpr uintptr_t kAssetWeightSetCount = 0x38;
constexpr uintptr_t kAssetChildModelCount = 0x52;  // u16
}  // namespace

std::optional<uint32_t> ModelSkeleton::physics_node_count() const {
    if (m_asset == nullptr) {
        return std::nullopt;
    }
    uint32_t v = 0;
    if (!sdk::mem::copy(&v, reinterpret_cast<uintptr_t>(m_asset) + kAssetPhysicsNodeCount, sizeof(v))) {
        return std::nullopt;
    }
    return v;
}

std::optional<uint32_t> ModelSkeleton::weight_set_count() const {
    if (m_asset == nullptr) {
        return std::nullopt;
    }
    uint32_t v = 0;
    if (!sdk::mem::copy(&v, reinterpret_cast<uintptr_t>(m_asset) + kAssetWeightSetCount, sizeof(v))) {
        return std::nullopt;
    }
    return v;
}

std::optional<uint32_t> ModelSkeleton::child_model_count() const {
    if (m_asset == nullptr) {
        return std::nullopt;
    }
    uint16_t v = 0;
    if (!sdk::mem::copy(&v, reinterpret_cast<uintptr_t>(m_asset) + kAssetChildModelCount, sizeof(v))) {
        return std::nullopt;
    }
    return v;
}

std::optional<ModelSkeleton::Socket> ModelSkeleton::socket(size_t index) const {
    const SocketRaw r = seh_socket(m_sockets, index, m_socket_count, m_count);
    if (!r.ok) {
        return std::nullopt;
    }
    char buf[kMaxName]{};
    if (seh_copy_cstr(r.name_ptr, buf, sizeof(buf)) < 0) {
        return std::nullopt;
    }
    Socket out{};
    out.name = buf;
    out.node_index = r.node_index;
    out.position.x = r.pos[0];
    out.position.y = r.pos[1];
    out.position.z = r.pos[2];
    out.rotation.x = r.rot[0];
    out.rotation.y = r.rot[1];
    out.rotation.z = r.rot[2];
    out.rotation.w = r.rot[3];
    return out;
}

std::optional<size_t> ModelSkeleton::find_socket(const char* name) const {
    if (name == nullptr) {
        return std::nullopt;
    }
    // A linear scan, exactly as the engine does it: the tables are 1..15 long, so an
    // index would cost more than it saves.
    for (size_t i = 0; i < m_socket_count; ++i) {
        const SocketRaw r = seh_socket(m_sockets, i, m_socket_count, m_count);
        if (!r.ok) {
            continue;
        }
        char buf[kMaxName]{};
        if (seh_copy_cstr(r.name_ptr, buf, sizeof(buf)) < 0) {
            continue;
        }
        if (equals_i(buf, name, sizeof(buf))) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace sdk

namespace sdk {

namespace {

struct NodeXformRaw {
    float pos[3];
    float rot[4];
    uint8_t dirty;
    bool ok;
};

// The transform and its dirty byte in ONE guard. Reading them separately would let
// the engine clear the flag and rewrite the slot in between, which is precisely the
// case that would hand back a "clean" answer holding stale bytes.
NodeXformRaw seh_node_xform(const void* xform, const void* dirty, size_t stride,
                            size_t offset, size_t index) {
    NodeXformRaw r{};
    if (xform == nullptr || dirty == nullptr) {
        return r;
    }
    // BOTH READS UNDER ONE GUARD, which is the point: the engine may clear the dirty flag and rewrite the
    // slot at any moment, and reading them under separate guards leaves a window where a "clean" answer
    // comes back holding stale bytes. The migration to sdk::mem briefly widened that window by issuing two
    // guarded copies back to back; sdk::mem::guarded takes a whole body, so the single guard is restored.
    //
    // The two addresses are unrelated -- a transform array and a dirty-flag array with its own stride -- so
    // no single bulk copy can span them. A guarded BODY can, which is exactly why that primitive exists.
    const bool ok = sdk::mem::guarded([&] {
        const regenny::LTNodeTransform& t = static_cast<const regenny::LTNodeTransform*>(xform)[index];
        r.pos[0] = t.position.x;
        r.pos[1] = t.position.y;
        r.pos[2] = t.position.z;
        r.rot[0] = t.rotation.x;
        r.rot[1] = t.rotation.y;
        r.rot[2] = t.rotation.z;
        r.rot[3] = t.rotation.w;
        r.dirty = *(static_cast<const uint8_t*>(dirty) + stride * index + offset);
    });
    if (!ok) {
        // A fault part-way through may have written some of r, so nothing partially filled escapes.
        return NodeXformRaw{};
    }
    r.ok = true;
    return r;
}

} // namespace

std::optional<ModelSkeleton::NodeTransform> ModelSkeleton::node_transform(size_t index) const {
    if (index >= m_count) {
        return std::nullopt;
    }
    const NodeXformRaw r =
        seh_node_xform(m_node_xform, m_node_dirty, m_dirty_stride, m_dirty_offset, index);
    if (!r.ok) {
        return std::nullopt;
    }
    NodeTransform out{};
    out.position.x = r.pos[0];
    out.position.y = r.pos[1];
    out.position.z = r.pos[2];
    out.rotation.x = r.rot[0];
    out.rotation.y = r.rot[1];
    out.rotation.z = r.rot[2];
    out.rotation.w = r.rot[3];
    // Non-zero is the engine's own test -- it recomputes rather than reading the slot.
    out.stale = r.dirty != 0;
    // Which space the active cache is in -- see the header; the two caches differ.
    out.world_space = m_world_space;
    return out;
}

} // namespace sdk

namespace sdk {

namespace {

// The animation-record lookup, guarded, following exactly the chain
// ILTModel_GetAnimName walks: bound the index against the vector, take slot+0x04,
// null-check the record, then hand back its name POINTER for the caller to copy
// through seh_copy_cstr (nesting SEH frames is the mistake this split avoids).
const char* seh_anim_name_ptr(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return nullptr;
    }
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return nullptr;
    }
    const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
    regenny::LTModelObject model_copy{};
    if (!sdk::mem::copy(&model_copy, reinterpret_cast<uintptr_t>(model), sizeof(model_copy))) {
        return nullptr;
    }
    const auto* asset = model_copy.record.asset;
    if (asset == nullptr) {
        return nullptr;
    }
    regenny::LTModelAsset asset_copy{};
    if (!sdk::mem::copy(&asset_copy, reinterpret_cast<uintptr_t>(asset), sizeof(asset_copy))) {
        return nullptr;
    }
    const auto* first = asset_copy.anim_records.first;
    const auto* last = asset_copy.anim_records.last;
    if (first == nullptr || last <= first) {
        return nullptr;
    }
    const size_t n = static_cast<size_t>(last - first);
    const size_t i = model_copy.record.current_anim;
    if (i >= n) {
        return nullptr;
    }
    regenny::LTAnimRecordSlot slot{};
    if (!sdk::mem::copy(&slot, reinterpret_cast<uintptr_t>(first + i), sizeof(slot)) || slot.record == nullptr) {
        return nullptr;
    }
    char* name = nullptr;
    if (!sdk::mem::copy(&name, reinterpret_cast<uintptr_t>(&slot.record->name), sizeof(name))) {
        return nullptr;
    }
    return name;
}

// Returns 1/0 for the bit, or -1 on fault or an out-of-range piece.
int seh_piece_hidden(const regenny::LTObject* obj, size_t index) {
    if (obj == nullptr) {
        return -1;
    }
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return -1;
    }
    const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
    regenny::LTModelObject model_copy{};
    if (!sdk::mem::copy(&model_copy, reinterpret_cast<uintptr_t>(model), sizeof(model_copy))) {
        return -1;
    }
    const auto* asset = model_copy.record.asset;
    if (asset == nullptr) {
        return -1;
    }
    uint32_t piece_count = 0;
    if (!sdk::mem::copy(&piece_count, reinterpret_cast<uintptr_t>(&asset->piece_count), sizeof(piece_count))) {
        return -1;
    }
    // THE BOUND IS THE ASSET'S PIECE COUNT, not the model's material count.
    // An earlier version used material_count and was WRONG: the two differ on
    // 17 of 34 live assets, so a grunt (7 pieces, 3 materials) would have had
    // four of its pieces refused. ILTModel::GetPieceName bounds by this field.
    // The second bound is the mask's own width -- two dwords, pinned by
    // material_names starting immediately after it.
    if (index >= piece_count || index >= 64) {
        return -1;
    }
    const uint32_t word = model_copy.piece_hide_bits[index >> 5];
    return ((word & (1u << (index & 31))) != 0) ? 1 : 0;
}

}  // namespace

std::optional<std::string> model_current_anim_name(const regenny::LTObject* obj) {
    const char* src = seh_anim_name_ptr(obj);
    if (src == nullptr) {
        return std::nullopt;
    }
    char buf[kMaxName]{};
    if (seh_copy_cstr(src, buf, sizeof(buf)) < 0) {
        return std::nullopt;
    }
    return std::string{buf};
}

std::optional<bool> model_piece_hidden(const regenny::LTObject* obj, size_t index) {
    const int r = seh_piece_hidden(obj, index);
    if (r < 0) {
        return std::nullopt;
    }
    return r != 0;
}

} // namespace sdk

namespace sdk {

namespace {

// One guarded read of the asset's piece table: the count and the requested name
// POINTER together, so the bound and the entry come from the same instant. The copy
// happens outside, through seh_copy_cstr, to avoid nesting SEH frames.
struct PieceRaw {
    uint32_t count;
    const char* name_ptr;
    bool ok;
};

PieceRaw seh_piece(const regenny::LTObject* obj, size_t index, bool want_name) {
    PieceRaw r{};
    if (obj == nullptr) {
        return r;
    }
    regenny::LTObjectType type{};
    if (!sdk::mem::copy(&type, reinterpret_cast<uintptr_t>(&obj->type), sizeof(type)) ||
        type != regenny::LTObjectType::OT_MODEL) {
        return r;
    }
    const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
    regenny::LTModelAsset* asset = nullptr;
    if (!sdk::mem::copy(&asset, reinterpret_cast<uintptr_t>(&model->record.asset), sizeof(asset)) ||
        asset == nullptr) {
        return r;
    }
    uint32_t piece_count = 0;
    if (!sdk::mem::copy(&piece_count, reinterpret_cast<uintptr_t>(&asset->piece_count), sizeof(piece_count))) {
        return r;
    }
    char* name_ptr = nullptr;
    if (want_name && index < piece_count) {
        char** piece_names = nullptr;
        if (!sdk::mem::copy(&piece_names, reinterpret_cast<uintptr_t>(&asset->piece_names), sizeof(piece_names))) {
            return r;  // any fault here invalidates the whole read, matching the original guard
        }
        if (piece_names != nullptr &&
            !sdk::mem::copy(&name_ptr, reinterpret_cast<uintptr_t>(&piece_names[index]), sizeof(name_ptr))) {
            return r;
        }
    }
    r.count = piece_count;
    r.name_ptr = name_ptr;
    r.ok = true;
    return r;
}

}  // namespace

std::optional<size_t> model_piece_count(const regenny::LTObject* obj) {
    const PieceRaw r = seh_piece(obj, 0, false);
    if (!r.ok) {
        return std::nullopt;
    }
    return r.count;
}

std::optional<std::string> model_piece_name(const regenny::LTObject* obj, size_t index) {
    const PieceRaw r = seh_piece(obj, index, true);
    if (!r.ok || r.name_ptr == nullptr) {
        return std::nullopt;
    }
    char buf[kMaxName]{};
    if (seh_copy_cstr(r.name_ptr, buf, sizeof(buf)) < 0) {
        return std::nullopt;
    }
    return std::string{buf};
}

std::optional<size_t> model_find_piece(const regenny::LTObject* obj, const char* name) {
    if (name == nullptr) {
        return std::nullopt;
    }
    const PieceRaw probe = seh_piece(obj, 0, false);
    if (!probe.ok) {
        return std::nullopt;
    }
    // Linear, like every other name lookup here: piece counts run 1..10 live.
    for (size_t i = 0; i < probe.count; ++i) {
        const auto nm = model_piece_name(obj, i);
        if (nm.has_value() && equals_i(nm->c_str(), name, kMaxName)) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace sdk

namespace sdk {

namespace {

// A local transform triple, matching the engine's 0x20-byte layout in meaning if not
// in storage: position, rotation, uniform scale.
struct Xform {
    float p[3];
    float q[4];  // x, y, z, w -- w last, as LTRotation stores it
    float s;
};

// LTRotation_Multiply (dump 0x424C4F), term for term. Transcribed rather than written
// from a formula: quaternion conventions differ by sign in several places and the
// engine's own expression is the only one guaranteed to agree with the engine.
void quat_mul(const float a[4], const float b[4], float out[4]) {
    out[0] = b[0] * a[3] + a[0] * b[3] + b[2] * a[1] - b[1] * a[2];
    out[1] = b[1] * a[3] - b[2] * a[0] + a[1] * b[3] + a[2] * b[0];
    out[2] = b[2] * a[3] + b[1] * a[0] - a[1] * b[0] + a[2] * b[3];
    out[3] = b[3] * a[3] - b[0] * a[0] - b[1] * a[1] - a[2] * b[2];
}

// LTRotation_RotateVector (dump 0x404C7F), term for term -- the conjugate sandwich
// written out, negations included exactly as the engine has them.
void quat_rotate(const float q[4], const float v[3], float out[3]) {
    const float t0 = q[3] * v[0] + v[2] * q[1] - v[1] * q[2];
    const float t1 = v[1] * q[3] - q[0] * v[2] + q[2] * v[0];
    const float t2 = v[1] * q[0] + q[3] * v[2] - v[0] * q[1];
    const float nx = -q[0];
    const float t3 = nx * v[0] - v[1] * q[1] - q[2] * v[2];
    const float ny = -q[1];
    const float nz = -q[2];
    const float w = q[3];
    out[0] = nx * t3 + w * t0 + nz * t1 - ny * t2;
    out[1] = ny * t3 - nz * t0 + w * t1 + nx * t2;
    out[2] = nz * t3 + ny * t0 - nx * t1 + w * t2;
}

// LTTransform_Compose (dump 0x4292C7): parent then child.
Xform compose(const Xform& parent, const Xform& child) {
    Xform out{};
    quat_mul(parent.q, child.q, out.q);
    float rotated[3];
    quat_rotate(parent.q, child.p, rotated);
    for (int i = 0; i < 3; ++i) {
        out.p[i] = parent.p[i] + rotated[i] * parent.s;
    }
    out.s = parent.s * child.s;
    return out;
}

// The owning object's own transform, read under a guard. LTObject carries ONE uniform
// scale rather than the reference's vector, which is why `s` is a single float here.
bool seh_object_xform(const void* obj_v, Xform* out) {
    const auto* obj = static_cast<const regenny::LTObject*>(obj_v);
    regenny::LTVector position{};
    regenny::LTRotation rotation{};
    float scale = 0.0f;
    bool ok = true;
    ok = ok && sdk::mem::copy(&position, reinterpret_cast<uintptr_t>(&obj->position), sizeof(position));
    ok = ok && sdk::mem::copy(&rotation, reinterpret_cast<uintptr_t>(&obj->rotation), sizeof(rotation));
    ok = ok && sdk::mem::copy(&scale, reinterpret_cast<uintptr_t>(&obj->scale), sizeof(scale));
    if (!ok) {
        return false;
    }
    out->p[0] = position.x;
    out->p[1] = position.y;
    out->p[2] = position.z;
    out->q[0] = rotation.x;
    out->q[1] = rotation.y;
    out->q[2] = rotation.z;
    out->q[3] = rotation.w;
    out->s = scale;
    return true;
}

}  // namespace

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::socket_transform(size_t socket_index) const {
    const auto sock = socket(socket_index);
    if (!sock.has_value()) {
        return std::nullopt;
    }
    const auto node = node_transform(sock->node_index);
    if (!node.has_value()) {
        return std::nullopt;
    }
    Xform parent{};
    parent.p[0] = node->position.x;
    parent.p[1] = node->position.y;
    parent.p[2] = node->position.z;
    parent.q[0] = node->rotation.x;
    parent.q[1] = node->rotation.y;
    parent.q[2] = node->rotation.z;
    parent.q[3] = node->rotation.w;
    // The bone transform carries no scale of its own in the cached record; the engine
    // supplies 1.0 at this point (LTModelObject_TransformFromRecord passes the literal).
    parent.s = 1.0f;

    Xform child{};
    child.p[0] = sock->position.x;
    child.p[1] = sock->position.y;
    child.p[2] = sock->position.z;
    child.q[0] = sock->rotation.x;
    child.q[1] = sock->rotation.y;
    child.q[2] = sock->rotation.z;
    child.q[3] = sock->rotation.w;
    child.s = 1.0f;

    const Xform r = compose(parent, child);
    SocketTransform out{};
    out.position.x = r.p[0];
    out.position.y = r.p[1];
    out.position.z = r.p[2];
    out.rotation.x = r.q[0];
    out.rotation.y = r.q[1];
    out.rotation.z = r.q[2];
    out.rotation.w = r.q[3];
    out.scale = r.s;
    out.stale = node->stale;
    return out;
}

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::socket_world_transform(const char* name) const {
    const auto index = find_socket(name);
    if (!index.has_value()) {
        return std::nullopt;
    }
    return socket_world_transform(*index);
}

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::socket_world_transform(size_t socket_index) const {
    const auto local = socket_transform(socket_index);
    if (!local.has_value() || m_object == nullptr) {
        return std::nullopt;
    }
    // IF THE BONE CACHE IS ALREADY IN WORLD SPACE, composing with the object again
    // double-applies its position. Measured: on the 22 models whose selector is set,
    // every clean bone sits AT the object's world position, and composing anyway put a
    // socket 5449 units away. So the socket transform is already world-space here and
    // is returned unchanged.
    if (m_world_space) {
        return local;
    }
    Xform world{};
    if (!seh_object_xform(m_object, &world)) {
        return std::nullopt;
    }
    Xform child{};
    child.p[0] = local->position.x;
    child.p[1] = local->position.y;
    child.p[2] = local->position.z;
    child.q[0] = local->rotation.x;
    child.q[1] = local->rotation.y;
    child.q[2] = local->rotation.z;
    child.q[3] = local->rotation.w;
    child.s = local->scale;

    const Xform r = compose(world, child);
    SocketTransform out{};
    out.position.x = r.p[0];
    out.position.y = r.p[1];
    out.position.z = r.p[2];
    out.rotation.x = r.q[0];
    out.rotation.y = r.q[1];
    out.rotation.z = r.q[2];
    out.rotation.w = r.q[3];
    out.scale = r.s;
    out.stale = local->stale;
    return out;
}

std::optional<bool>
ModelSkeleton::socket_world_transform_is_usable(size_t socket_index) const {
    const auto wt = socket_world_transform(socket_index);
    if (!wt.has_value()) {
        return std::nullopt;
    }
    if (wt->stale) {
        return false;
    }
    if (!std::isfinite(wt->position.x) || !std::isfinite(wt->position.y) ||
        !std::isfinite(wt->position.z)) {
        return false;
    }
    const float n = wt->rotation.x * wt->rotation.x + wt->rotation.y * wt->rotation.y +
                    wt->rotation.z * wt->rotation.z + wt->rotation.w * wt->rotation.w;
    // A tolerance, not an equality: the product of two unit quaternions accumulates rounding.
    return n > 0.98f && n < 1.02f;
}

namespace {

// ILTModel vtable slot 3 == ILTModel_GetSocketTransform, and its module offset, used ONLY to
// verify the slot before calling -- never to call. Both the client and server ILTModel tables
// place GetSocket at 2 and GetSocketTransform at 3.
constexpr size_t kSlotGetSocketTransform = 3;
// ILTModelClient's mapped entry count, from reversing/fear2.genny -- the bound every slot read needs.
constexpr size_t kILTModelClientSlots = 83;
// GetBindPoseNodeTransform: slot 22, and its RVA pins the exact function so a reshuffled table cannot
// hand back something else. Its own error string is 'ILTModel::GetBindPoseNodeTransform'.
constexpr size_t kSlotGetBindPose = 22;
constexpr uintptr_t kGetBindPoseRva = 0x42C958 - 0x400000;
constexpr uintptr_t kGetSocketTransformRva = 0x42B775 - 0x400000;

// THE FIRST STACK ARGUMENT IS THE OBJECT, and `this` is ignored. The disassembly is explicit:
// `mov ecx, [esp+arg_0]` loads arg 0 into ECX and then `cmp byte ptr [ecx+10h], 1` tests the
// OBJECT's type for OT_MODEL before re-dispatching as a thiscall on it. So this vtable entry is a
// __stdcall wrapper over the object, not a member function of the interface.
//
// A first attempt passed the interface here, and the engine did exactly the right thing: the gate
// read iface+0x10 (252, not 1) and returned LT_INVALIDPARAMS (60) without touching anything. The
// wrong signature produced a clean refusal rather than a crash -- but only by luck of argument
// count, which `retn 10h` had already pinned at four dwords.
using GetSocketTransformFn = int(__stdcall*)(const void*, uint32_t, float*, int);

// Verifies the slot and returns the entry, or nullptr.
GetSocketTransformFn resolve_get_socket_transform() {
    auto* iface = interfaces::ILTModel::get_client();
    const auto* exe = Modules::get().exe();
    if (iface == nullptr || exe == nullptr) {
        return nullptr;
    }
    void* const* vt = nullptr;
    if (!sdk::mem::copy(&vt, reinterpret_cast<uintptr_t>(iface), sizeof(vt)) || vt == nullptr) {
        return nullptr;
    }
    void* entry_ptr = nullptr;
    if (!sdk::mem::copy(&entry_ptr, reinterpret_cast<uintptr_t>(&vt[kSlotGetSocketTransform]), sizeof(entry_ptr))) {
        return nullptr;
    }
    const auto entry = reinterpret_cast<uintptr_t>(entry_ptr);
    if (entry - exe->base != kGetSocketTransformRva) {
        return nullptr;
    }
    return reinterpret_cast<GetSocketTransformFn>(entry);
}

// __stdcall, THREE stack dwords, and the caller's ECX is dead -- all read off 0x42C958 itself, where
// both ECX reads are dominated by `mov ecx, [esp+node_index]`. Do NOT assume the sibling slots' arity:
// GetSocketTransform pops four dwords, this pops three.
using GetBindPoseFn = int(__stdcall*)(const void*, unsigned, ModelSkeleton::Pose*);

GetBindPoseFn resolve_get_bind_pose() {
    GetBindPoseFn fn = nullptr;
    auto* iface = interfaces::ILTModel::get_client();
    const auto* exe = Modules::get().exe();
    if (iface == nullptr || exe == nullptr || exe->base == 0) {
        return nullptr;
    }
    const auto entry = interfaces::vtable_slot(iface, kSlotGetBindPose, kILTModelClientSlots);
    if (entry != 0 && entry - exe->base == kGetBindPoseRva) {
        fn = reinterpret_cast<GetBindPoseFn>(entry);
    }
    return fn;
}

// The object can be unregistered between a snapshot and this call, so the invocation itself is guarded.
int64_t seh_call_get_bind_pose(GetBindPoseFn fn, const void* obj, unsigned index,
                               ModelSkeleton::Pose* out) {
    int64_t r = -1;
    if (!sdk::mem::guarded([&] {
            r = fn(obj, index, out);
        })) {
        r = -1;
    }
    return r;
}

int64_t seh_call_get_socket_transform(GetSocketTransformFn fn, const void* obj,
                                      uint32_t handle, float* out, int world_space) {
    int64_t r = -1;
    if (!sdk::mem::guarded([&] {
            r = fn(obj, handle, out, world_space);
        })) {
        r = -1;
    }
    return r;
}

}  // namespace

// DIAGNOSTIC: the engine's own return code from the last call, and the byte its wrapper gates
// on. "The call failed" is not actionable; the code it returned names the reason.
int64_t g_last_engine_rc = -999;

int64_t ModelSkeleton::last_engine_rc() { return g_last_engine_rc; }

int64_t ModelSkeleton::engine_iface_gate_byte() {
    auto* iface = interfaces::ILTModel::get_client();
    if (iface == nullptr) {
        return -1;
    }
    const auto v = sdk::mem::read_u8(reinterpret_cast<uintptr_t>(iface) + 16);
    return v.has_value() ? static_cast<int64_t>(*v) : -1;
}

namespace {


}  // namespace

namespace {


}  // namespace


std::optional<size_t> ModelSkeleton::node_depth(size_t index) const {
    const auto chain = path_to_root(index);
    if (!chain.has_value() || chain->empty()) {
        return std::nullopt;
    }
    return chain->size() - 1;
}

bool ModelSkeleton::node_has_ancestor(size_t index, size_t ancestor) const {
    if (index == ancestor) {
        return false;  // strictly above
    }
    const auto chain = path_to_root(index);
    if (!chain.has_value()) {
        return false;
    }
    for (size_t i = 1; i < chain->size(); ++i) {
        if ((*chain)[i] == ancestor) {
            return true;
        }
    }
    return false;
}

std::optional<regenny::LTVector> ModelSkeleton::anim_fallback_position(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    // POSITION ONLY: the rotation half of this pair is read by one secondary getter and never by the
    // skeleton evaluator, so its role is undescribed and it is not handed out.
    return nd.pos_b;
}


std::optional<ModelSkeleton::Pose> ModelSkeleton::inverse_bind_pose(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    return Pose{nd.pos_a, nd.rot_a};
}

ModelSkeleton::Pose ModelSkeleton::invert_rigid(const Pose& p) {
    // Conjugate the rotation, then rotate the position by that conjugate and negate it. Component order
    // is (x, y, z, w), per the schema and the engine's own LTRotation_Conjugate (0x41DAB6), which negates
    // x/y/z and copies w.
    const double x = -static_cast<double>(p.rotation.x);
    const double y = -static_cast<double>(p.rotation.y);
    const double z = -static_cast<double>(p.rotation.z);
    const double w = static_cast<double>(p.rotation.w);
    const double px = p.position.x, py = p.position.y, pz = p.position.z;
    const double tx = 2.0 * (y * pz - z * py);
    const double ty = 2.0 * (z * px - x * pz);
    const double tz = 2.0 * (x * py - y * px);
    Pose out{};
    out.position.x = static_cast<float>(-(px + w * tx + (y * tz - z * ty)));
    out.position.y = static_cast<float>(-(py + w * ty + (z * tx - x * tz)));
    out.position.z = static_cast<float>(-(pz + w * tz + (x * ty - y * tx)));
    out.rotation.x = static_cast<float>(x);
    out.rotation.y = static_cast<float>(y);
    out.rotation.z = static_cast<float>(z);
    out.rotation.w = p.rotation.w;
    return out;
}

std::optional<ModelSkeleton::Pose> ModelSkeleton::bind_pose(size_t index) const {
    const auto p = inverse_bind_pose(index);
    if (!p.has_value()) {
        return std::nullopt;
    }
    return invert_rigid(*p);
}

std::optional<ModelSkeleton::EyeGeometry> ModelSkeleton::eye_geometry() const {
    const auto li = find_socket("socket_left_eye");
    const auto ri = find_socket("socket_right_eye");
    if (!li.has_value() || !ri.has_value()) {
        return std::nullopt;
    }
    const auto ls = socket(*li);
    const auto rs = socket(*ri);
    if (!ls.has_value() || !rs.has_value()) {
        return std::nullopt;
    }
    // DIFFERENT NODES MEAN DIFFERENT FRAMES. Refuse instead of subtracting across them.
    if (ls->node_index != rs->node_index) {
        return std::nullopt;
    }
    EyeGeometry out{};
    out.left = ls->position;
    out.right = rs->position;
    out.node_index = ls->node_index;
    const float dx = ls->position.x - rs->position.x;
    const float dy = ls->position.y - rs->position.y;
    const float dz = ls->position.z - rs->position.z;
    out.separation = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.center.x = (ls->position.x + rs->position.x) * 0.5f;
    out.center.y = (ls->position.y + rs->position.y) * 0.5f;
    out.center.z = (ls->position.z + rs->position.z) * 0.5f;
    return out;
}

std::optional<regenny::LTVector> ModelSkeleton::camera_to_eye_center() const {
    const auto eyes = eye_geometry();
    if (!eyes.has_value()) {
        return std::nullopt;
    }
    const auto ci = find_socket("camera");
    if (!ci.has_value()) {
        return std::nullopt;
    }
    const auto cs = socket(*ci);
    if (!cs.has_value() || cs->node_index != eyes->node_index) {
        return std::nullopt;
    }
    regenny::LTVector out{};
    out.x = eyes->center.x - cs->position.x;
    out.y = eyes->center.y - cs->position.y;
    out.z = eyes->center.z - cs->position.z;
    return out;
}

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::socket_pose(size_t handle) const {
    // The clean path first: no engine call, no mutation, and independently confirmed to match the
    // engine's own answer on every clean socket.
    if (const auto cached = socket_world_transform(handle); cached.has_value() && !cached->stale) {
        return cached;
    }
    // Otherwise let the engine do the work. This is the branch that needs the game thread.
    return engine_socket_transform(handle, 1);
}

bool ModelSkeleton::engine_bind_pose_available() { return resolve_get_bind_pose() != nullptr; }

std::optional<ModelSkeleton::Pose> ModelSkeleton::engine_bind_pose(size_t index) const {
    auto* fn = resolve_get_bind_pose();
    if (fn == nullptr || m_object == nullptr || index >= m_count) {
        return std::nullopt;
    }
    Pose out{};
    const auto rc = seh_call_get_bind_pose(fn, m_object, static_cast<unsigned>(index), &out);
    g_last_engine_rc = rc;
    if (rc != 0) {
        return std::nullopt;
    }
    return out;
}

bool ModelSkeleton::engine_socket_transform_available() {
    return resolve_get_socket_transform() != nullptr;
}

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::engine_socket_transform(size_t handle, int world_space) const {
    if (m_object == nullptr) {
        return std::nullopt;
    }
    auto* fn = resolve_get_socket_transform();
    if (fn == nullptr) {
        return std::nullopt;
    }
    // Eight floats is the documented extent; the extra room costs nothing and means a wrong
    // guess about the layout cannot write past the buffer.
    float out[16]{};
    const int64_t rc = seh_call_get_socket_transform(fn, m_object,
                                                     static_cast<uint32_t>(handle), out, world_space);
    g_last_engine_rc = rc;
    if (rc != 0) {
        return std::nullopt;  // non-zero is the engine's own failure code
    }
    SocketTransform t{};
    t.position.x = out[0];
    t.position.y = out[1];
    t.position.z = out[2];
    t.rotation.x = out[3];
    t.rotation.y = out[4];
    t.rotation.z = out[5];
    t.rotation.w = out[6];
    t.scale = out[7];
    // The engine composed it, so there is no cache staleness to inherit.
    t.stale = false;
    return t;
}

std::optional<ModelSkeleton::ResolvedHandle>
ModelSkeleton::resolve_socket_handle(size_t handle) const {
    // The engine's own split, from ILTModel_GetSocketTransform: sockets first, then nodes
    // at `handle - socket_count`.
    if (handle < m_socket_count) {
        return ResolvedHandle{HandleKind::Socket, handle};
    }
    const size_t node = handle - m_socket_count;
    if (node < m_count) {
        return ResolvedHandle{HandleKind::Node, node};
    }
    return std::nullopt;
}

std::optional<ModelSkeleton::SocketTransform>
ModelSkeleton::socket_handle_transform(size_t handle) const {
    const auto r = resolve_socket_handle(handle);
    if (!r.has_value()) {
        return std::nullopt;
    }
    if (r->kind == HandleKind::Socket) {
        return socket_world_transform(r->index);
    }
    // The NODE path. A bare node transform is not a socket transform: it carries no
    // socket offset, so the composition is the object against the bone alone -- and it
    // needs the same world-space guard, for the same reason.
    const auto bone = node_transform(r->index);
    if (!bone.has_value()) {
        return std::nullopt;
    }
    SocketTransform out{};
    out.scale = 1.0f;
    out.stale = bone->stale;
    if (bone->world_space) {
        out.position = bone->position;
        out.rotation = bone->rotation;
        return out;
    }
    Xform world{};
    if (m_object == nullptr || !seh_object_xform(m_object, &world)) {
        return std::nullopt;
    }
    Xform child{};
    child.p[0] = bone->position.x;
    child.p[1] = bone->position.y;
    child.p[2] = bone->position.z;
    child.q[0] = bone->rotation.x;
    child.q[1] = bone->rotation.y;
    child.q[2] = bone->rotation.z;
    child.q[3] = bone->rotation.w;
    child.s = 1.0f;
    const Xform c = compose(world, child);
    out.position.x = c.p[0];
    out.position.y = c.p[1];
    out.position.z = c.p[2];
    out.rotation.x = c.q[0];
    out.rotation.y = c.q[1];
    out.rotation.z = c.q[2];
    out.rotation.w = c.q[3];
    out.scale = c.s;
    return out;
}

}  // namespace sdk

namespace sdk {

std::optional<AttachedSocket> attached_socket(const regenny::LTObject* parent,
                                              const char* socket_name) {
    if (parent == nullptr || socket_name == nullptr) {
        return std::nullopt;
    }
    // Deliberately reuses the public attachment walk rather than re-reading the list:
    // that walk already carries the cycle cap and the handle resolution, and duplicating
    // it here would mean maintaining two versions of the same guard.
    for (const auto& att : attachments(parent)) {
        if (att.child == nullptr) {
            continue;
        }
        const auto sk = ModelSkeleton::from_object(att.child);
        if (!sk.has_value()) {
            continue;
        }
        const auto xf = sk->socket_world_transform(socket_name);
        if (!xf.has_value()) {
            continue;
        }
        AttachedSocket out{};
        out.object = att.child;
        out.child_handle = att.child_handle;
        out.socket_handle = att.socket_handle;
        out.transform = *xf;
        return out;
    }
    return std::nullopt;
}

}  // namespace sdk

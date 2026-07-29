#include "Model.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "CClientMgr.hpp"
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
    bool ok = false;
    KANANLIB_SEH_TRY {
        if (obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL) {
            const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
            const auto* asset = model->record.asset;
            if (asset != nullptr) {
                const auto* recs = asset->node_records;
                const auto* names = asset->node_names;
                const uint32_t n = asset->node_count;
                if (recs != nullptr && names != nullptr && n > 0 && n <= kMaxNodes) {
                    out->asset = asset;
                    out->records = recs;
                    out->names = names;
                    out->count = n;
                    // A socket table is OPTIONAL: an asset may define none, so a
                    // zero count is a normal answer and not a resolve failure.
                    const auto* socks = asset->sockets;
                    const uint32_t sn = asset->socket_count;
                    if (socks != nullptr && sn > 0 && sn <= kMaxNodes) {
                        out->sockets = socks;
                        out->socket_count = sn;
                    }

                    // THE ENGINE'S OWN BRANCH, reproduced rather than chosen: a model
                    // holds two per-node caches and GetNodeTransform reads whichever
                    // the mode selector at +0x156 names. Picking one here would be
                    // right on 193 of 215 models and silently wrong on the other 22.
                    if (model->sphere_source != 0) {
                        out->node_xform = model->block_130.node_transforms;
                        out->node_dirty = model->block_130.node_dirty_stride3;
                        out->dirty_stride = 3;
                        out->dirty_offset = 1;  // the engine tests the SECOND byte
                    } else {
                        out->node_xform = model->block_120.node_transforms;
                        out->node_dirty = model->block_120.node_dirty_stride2;
                        out->dirty_stride = 2;
                        out->dirty_offset = 0;
                    }
                    // WHICH SPACE that cache is in. Measured, not assumed: over every
                    // clean slot, selector==0 put 297/297 bones near the model origin
                    // and selector!=0 put 46/46 at the object's world position.
                    out->world_space = model->sphere_source != 0;

                    // BindAsset's own arithmetic, term for term. Verified live: every
                    // pointer the engine carves out lands inside the result on
                    // 215/215 models, including the bone palette's full extent.
                    const auto align4 = [](uint32_t v) { return v + 3u - ((v + 3u) & 3u); };
                    const uint32_t c08 = asset->region_count_a;
                    const uint32_t c10 = asset->region_count_b;
                    uint32_t size = align4(34u * n) + align4(31u * n + 4u * (c08 + c10)) +
                                    align4(28u * asset->material_count);
                    if ((obj->flags3 & 0x400) != 0) {
                        size += align4(2u * n) + align4(48u * n);
                    }
                    const auto base = reinterpret_cast<uintptr_t>(model->per_node_alloc);
                    out->alloc_base = base;
                    out->alloc_end = (base != 0) ? base + size : 0;
                    ok = true;
                }
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Copies a NUL-terminated name out of engine memory into `dst`. Returns the length,
// or -1 on fault / an unterminated or non-printable run.
int64_t seh_copy_cstr(const char* src, char* dst, size_t cap) {
    int64_t len = -1;
    KANANLIB_SEH_TRY {
        if (src != nullptr) {
            size_t i = 0;
            bool good = true;
            for (; i < cap - 1; ++i) {
                const unsigned char c = static_cast<unsigned char>(src[i]);
                if (c == 0) {
                    break;
                }
                if (c < 0x20 || c > 0x7E) {
                    good = false;
                    break;
                }
                dst[i] = src[i];
            }
            if (good && i < cap - 1) {
                dst[i] = '\0';
                len = static_cast<int64_t>(i);
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        len = -1;
    }
    return len;
}

int64_t seh_find_name(const void* names_v, uint32_t count, const char* needle, char* scratch) {
    const auto* names = static_cast<const char* const*>(names_v);
    int64_t found = -1;
    KANANLIB_SEH_TRY {
        for (uint32_t i = 0; i < count; ++i) {
            const char* nm = names[i];
            if (nm == nullptr) {
                continue;
            }
            if (equals_i(nm, needle, kMaxName)) {
                found = static_cast<int64_t>(i);
                break;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        found = -1;
    }
    (void)scratch;
    return found;
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
    bool ok = false;
    KANANLIB_SEH_TRY {
        if (index < count) {
            const auto* nd = static_cast<const regenny::LTModelNode*>(records) + index;
            out->parent = nd->parent_index;
            out->first_child_offset = nd->first_child_offset;
            out->child_count = nd->child_count;
            out->pos_a = nd->position_a;
            out->rot_a = nd->rotation_a;
            out->pos_b = nd->position_b;
            out->rot_b = nd->rotation_b;
            ok = true;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// Walks parent links into a POD buffer. Returns the count written, or -1 if the
// chain did not terminate within the node count (which means a corrupt parent).
int64_t seh_walk_parents(const void* records, uint32_t count, size_t start, size_t* out,
                         size_t cap) {
    int64_t written = -1;
    KANANLIB_SEH_TRY {
        const auto* nodes = static_cast<const regenny::LTModelNode*>(records);
        size_t n = 0, cur = start;
        bool terminated = false;
        while (n < cap) {
            out[n++] = cur;
            const uint8_t parent = nodes[cur].parent_index;
            if (parent == 255) {
                terminated = true;
                break;
            }
            if (parent >= count || parent >= cur) {
                break;  // topological order guarantees parent < cur; refuse a loop
            }
            cur = parent;
        }
        if (terminated) {
            written = static_cast<int64_t>(n);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        written = -1;
    }
    return written;
}

// Reads material_names[index] out of the model's std::string array.
int64_t seh_copy_material(const regenny::LTModelObject* model, uint32_t index, char* dst,
                          size_t cap) {
    int64_t len = -1;
    KANANLIB_SEH_TRY {
        const auto* arr = model->material_names;
        if (arr != nullptr && index < model->material_count) {
            const auto& s = arr[index];
            if (s.size <= s.capacity && s.size < cap) {
                // capacity >= 16 means the body moved to the heap and buf holds the
                // pointer; below that the body IS buf.
                const char* data = (s.capacity >= 16)
                                       ? *reinterpret_cast<const char* const*>(s.buf)
                                       : reinterpret_cast<const char*>(s.buf);
                if (data != nullptr) {
                    for (uint32_t i = 0; i < s.size; ++i) {
                        dst[i] = data[i];
                    }
                    dst[s.size] = '\0';
                    len = static_cast<int64_t>(s.size);
                }
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        len = -1;
    }
    return len;
}

int64_t seh_material_count(const regenny::LTModelObject* model) {
    int64_t n = -1;
    KANANLIB_SEH_TRY {
        const uint32_t c = model->material_count;
        if (model->material_names != nullptr && c <= kMaxMaterials) {
            n = static_cast<int64_t>(c);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

// Two one-line pointer reads that exist only because __try cannot share a function
// with anything that unwinds -- the callers build a std::string, so the guarded
// read has to live out here.
const char* seh_name_ptr(const void* names, size_t index) {
    const char* p = nullptr;
    KANANLIB_SEH_TRY {
        p = static_cast<const char* const*>(names)[index];
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

const char* seh_asset_filename(const regenny::LTModelObject* model) {
    const char* p = nullptr;
    KANANLIB_SEH_TRY {
        const auto* asset = model->record.asset;
        if (asset != nullptr) {
            p = asset->filename;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

const regenny::LTModelObject* as_model(const regenny::LTObject* obj) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        ok = obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok ? reinterpret_cast<const regenny::LTModelObject*>(obj) : nullptr;
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

std::optional<ModelSkeleton::Pose> ModelSkeleton::pose_a(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    return Pose{nd.pos_a, nd.rot_a};
}

std::optional<ModelSkeleton::Pose> ModelSkeleton::pose_b(size_t index) const {
    NodeRaw nd{};
    if (!seh_read_node(m_records, static_cast<uint32_t>(m_count), index, &nd)) {
        return std::nullopt;
    }
    return Pose{nd.pos_b, nd.rot_b};
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
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* model = static_cast<const regenny::LTModelObject*>(obj_v);
        const auto* mats = model->node_matrices;
        const auto first = reinterpret_cast<uintptr_t>(mats);
        const auto want_end = first + sizeof(regenny::LTMatrix3x4) * (index + 1);
        if (mats != nullptr && alloc_base != 0 && alloc_end > alloc_base && index < count &&
            first >= alloc_base && want_end <= alloc_end) {
            const float* src = mats[index].m;
            for (int i = 0; i < 12; ++i) {
                out12[i] = src[i];
            }
            ok = true;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
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
    KANANLIB_SEH_TRY {
        if (obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL) {
            const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
            const auto* asset = model->record.asset;
            if (asset != nullptr) {
                r.index = model->record.anim_index;
                r.index_b = model->record.current_anim;
                r.fraction = model->record.anim_fraction;
                r.node_a = model->record.node_a;
                r.node_b = model->record.node_b;
                const auto* first = asset->anim_names.first;
                const auto* last = asset->anim_names.last;
                if (first != nullptr && last >= first) {
                    r.anim_count = static_cast<uint32_t>(last - first);
                }
                r.ok = true;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
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
    KANANLIB_SEH_TRY {
        const auto* s = static_cast<const regenny::LTModelSocket*>(base) + index;
        // The engine bounds this itself when it uses the socket -- GetSocketTransform
        // hands node_index straight to GetNodeTransform, which range-checks against
        // node_count. Checking here means a caller never receives an index it cannot
        // then feed back into node_name()/bone_matrix().
        if (s->node_index < node_count) {
            r.node_index = s->node_index;
            r.pos[0] = s->position.x;
            r.pos[1] = s->position.y;
            r.pos[2] = s->position.z;
            r.rot[0] = s->rotation.x;
            r.rot[1] = s->rotation.y;
            r.rot[2] = s->rotation.z;
            r.rot[3] = s->rotation.w;
            r.name_ptr = s->name;
            r.ok = true;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

} // namespace

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
    KANANLIB_SEH_TRY {
        const auto* t = static_cast<const regenny::LTNodeTransform*>(xform) + index;
        r.pos[0] = t->position.x;
        r.pos[1] = t->position.y;
        r.pos[2] = t->position.z;
        r.rot[0] = t->rotation.x;
        r.rot[1] = t->rotation.y;
        r.rot[2] = t->rotation.z;
        r.rot[3] = t->rotation.w;
        r.dirty = static_cast<const uint8_t*>(dirty)[stride * index + offset];
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
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
    const char* name = nullptr;
    KANANLIB_SEH_TRY {
        if (obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL) {
            const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
            const auto* asset = model->record.asset;
            if (asset != nullptr) {
                const auto* first = asset->anim_records.first;
                const auto* last = asset->anim_records.last;
                if (first != nullptr && last > first) {
                    const size_t n = static_cast<size_t>(last - first);
                    const size_t i = model->record.current_anim;
                    if (i < n && first[i].record != nullptr) {
                        name = first[i].record->name;
                    }
                }
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return name;
}

// Returns 1/0 for the bit, or -1 on fault or an out-of-range piece.
int seh_piece_hidden(const regenny::LTObject* obj, size_t index) {
    int result = -1;
    KANANLIB_SEH_TRY {
        if (obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL) {
            const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
            // THE BOUND IS THE ASSET'S PIECE COUNT, not the model's material count.
            // An earlier version used material_count and was WRONG: the two differ on
            // 17 of 34 live assets, so a grunt (7 pieces, 3 materials) would have had
            // four of its pieces refused. ILTModel::GetPieceName bounds by this field.
            // The second bound is the mask's own width -- two dwords, pinned by
            // material_names starting immediately after it.
            const auto* asset = model->record.asset;
            if (asset != nullptr && index < asset->piece_count && index < 64) {
                const uint32_t word = model->piece_hide_bits[index >> 5];
                result = ((word & (1u << (index & 31))) != 0) ? 1 : 0;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
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
    KANANLIB_SEH_TRY {
        if (obj != nullptr && obj->type == regenny::LTObjectType::OT_MODEL) {
            const auto* model = reinterpret_cast<const regenny::LTModelObject*>(obj);
            const auto* asset = model->record.asset;
            if (asset != nullptr) {
                r.count = asset->piece_count;
                r.ok = true;
                if (want_name && index < r.count && asset->piece_names != nullptr) {
                    r.name_ptr = asset->piece_names[index];
                }
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
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
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* obj = static_cast<const regenny::LTObject*>(obj_v);
        out->p[0] = obj->position.x;
        out->p[1] = obj->position.y;
        out->p[2] = obj->position.z;
        out->q[0] = obj->rotation.x;
        out->q[1] = obj->rotation.y;
        out->q[2] = obj->rotation.z;
        out->q[3] = obj->rotation.w;
        out->s = obj->scale;
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
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

}  // namespace sdk

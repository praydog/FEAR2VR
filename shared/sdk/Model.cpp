#include "Model.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "CClientMgr.hpp"
#include "regenny/regenny/LTModelAsset.hpp"
#include "regenny/regenny/LTAnimNameEntry.hpp"
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
                r.index_b = model->record.anim_index_b;
                r.fraction = model->record.anim_fraction;
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
    return AnimState{r.index, r.index_b, r.fraction};
}

std::optional<size_t> model_anim_count(const regenny::LTObject* obj) {
    const AnimRaw r = seh_anim(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    return static_cast<size_t>(r.anim_count);
}

}  // namespace sdk

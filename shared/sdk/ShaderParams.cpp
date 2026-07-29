#include "ShaderParams.hpp"

#include <cstring>
#include <unordered_set>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// g_ShaderParamList_Head. The list head is a static record, so this is a fixed exe offset
// rather than something reached through an engine pointer.
constexpr uintptr_t kListHeadOffset = 0x32F080;

// g_LTShaderParam_vftable. Every one of the 60 records stores this in its first dword, which
// is what makes it a usable end-of-list check while walking.
constexpr uintptr_t kRecordVftableOffset = 0x291518;

constexpr size_t kName = 0x04;
constexpr size_t kType = 0x08;
constexpr size_t kBinding = 0x0A;  // engine handle index, not a D3D constant register
constexpr size_t kValuePtr = 0x0C;
constexpr size_t kSize = 0x10;
constexpr size_t kPending = 0x12;
constexpr size_t kNext = 0x18;
constexpr size_t kInlineValue = 0x1C;

// The longest engine parameter name is k_mModelObjectNodes_Previous at 28 characters. A cap
// well above that keeps a corrupt pointer from being read as an unbounded string.
constexpr size_t kMaxNameLength = 128;

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
}

// A fault-guarded copy of `bytes` from `at`. The single place raw engine memory is touched.
bool seh_copy(void* out, uintptr_t at, size_t bytes) {
    if (at == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    bool ok = false;
    // Byte-wise via memcpy: the guarded frame must hold nothing that unwinds, which MSVC
    // enforces, so the string building in seh_read_string stays outside this function.
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

// A fault-guarded NUL-terminated read, one byte at a time so a string ending on the last
// mapped byte of a page still reads.
bool seh_read_string(std::string& out, uintptr_t at, size_t max) {
    out.clear();
    for (size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!seh_copy(&c, at + i, sizeof(c))) {
            return false;
        }
        if (c == '\0') {
            return true;
        }
        out.push_back(c);
    }
    return false;  // no terminator within the cap: treat as not a name
}

}  // namespace

bool ShaderParam::bound() const {
    return binding_index != ShaderParams::kUnboundBinding;
}

bool ShaderParam::has_pending_upload() const {
    return pending != 0;
}

size_t ShaderParam::element_count() const {
    const auto element = ShaderParams::element_size(type);
    if (element == 0 || size == 0 || size % element != 0) {
        return 0;
    }
    return size / element;
}

bool ShaderParam::size_agrees_with_type() const {
    return element_count() != 0;
}

size_t ShaderParams::element_size(ShaderParamType type) {
    switch (type) {
    case ShaderParamType::Texture:
        return 4;
    case ShaderParamType::Float:
        return 4;
    case ShaderParamType::Float2:
        return 8;
    case ShaderParamType::Float3:
        return 12;
    case ShaderParamType::Float4:
        return 16;
    case ShaderParamType::Matrix4x3:
        return 48;
    case ShaderParamType::Matrix4x4:
        return 64;
    default:
        return 0;
    }
}

const char* ShaderParams::type_name(ShaderParamType type) {
    switch (type) {
    case ShaderParamType::Texture:
        return "texture";
    case ShaderParamType::Float:
        return "float";
    case ShaderParamType::Float2:
        return "float2";
    case ShaderParamType::Float3:
        return "float3";
    case ShaderParamType::Float4:
        return "float4";
    case ShaderParamType::Matrix4x3:
        return "float4x3";
    case ShaderParamType::Matrix4x4:
        return "float4x4";
    default:
        return "unknown";
    }
}

uintptr_t ShaderParams::list_head_address() {
    return exe_at(kListHeadOffset);
}

std::optional<ShaderParam> ShaderParams::read_record(uintptr_t address) {
    if (address == 0) {
        return std::nullopt;
    }
    const auto expected_vftable = exe_at(kRecordVftableOffset);
    if (expected_vftable == 0) {
        return std::nullopt;
    }
    uintptr_t vftable = 0;
    if (!seh_copy(&vftable, address, sizeof(vftable)) || vftable != expected_vftable) {
        return std::nullopt;
    }

    ShaderParam param{};
    param.address = address;

    uintptr_t name_ptr = 0;
    uint16_t type = 0;
    if (!seh_copy(&name_ptr, address + kName, sizeof(name_ptr)) ||
        !seh_copy(&type, address + kType, sizeof(type)) ||
        !seh_copy(&param.binding_index, address + kBinding, sizeof(param.binding_index)) ||
        !seh_copy(&param.value_address, address + kValuePtr, sizeof(param.value_address)) ||
        !seh_copy(&param.size, address + kSize, sizeof(param.size)) ||
        !seh_copy(&param.pending, address + kPending, sizeof(param.pending))) {
        return std::nullopt;
    }
    param.type = static_cast<ShaderParamType>(type);
    if (!seh_read_string(param.name, name_ptr, kMaxNameLength) || param.name.empty()) {
        return std::nullopt;
    }
    return param;
}

std::vector<ShaderParam> ShaderParams::all(size_t limit) {
    std::vector<ShaderParam> out;
    auto cursor = list_head_address();
    std::unordered_set<uintptr_t> visited{cursor};
    for (size_t i = 0; i < limit && cursor != 0; ++i) {
        auto record = read_record(cursor);
        if (!record.has_value()) {
            break;
        }
        uintptr_t next = 0;
        if (!seh_copy(&next, cursor + kNext, sizeof(next))) {
            out.push_back(std::move(*record));
            break;
        }
        out.push_back(std::move(*record));
        // A corrupt or circular list must not repeat records up to `limit`. Remembering
        // every record already walked catches a loop of any length, not just next == self.
        if (next == 0 || !visited.insert(next).second) {
            break;
        }
        cursor = next;
    }
    return out;
}

std::optional<ShaderParam> ShaderParams::find(std::string_view name) {
    for (auto& param : all()) {
        if (param.name == name) {
            return param;
        }
    }
    return std::nullopt;
}

bool ShaderParams::read_value(const ShaderParam& param, void* out, size_t bytes) {
    if (bytes == 0 || bytes > param.size) {
        return false;
    }
    // The value pointer is the record's own inline storage in every record observed. Trust
    // the pointer, but only after it agrees with that -- a stale or patched pointer aiming
    // elsewhere is exactly what a caller does not want to read blindly.
    if (param.value_address != param.address + kInlineValue) {
        return false;
    }
    return seh_copy(out, param.value_address, bytes);
}

namespace {

// Read a fixed-size value after checking the parameter's declared type AND that the record
// holds exactly that many bytes. The size equality is what refuses an ARRAY parameter here:
// k_mModelObjectNodes is a float4x3 like k_mObjectToWorld, but 1152 bytes rather than 48, and
// silently returning its first matrix would be a lie a caller could not see.
template <size_t N>
std::optional<std::array<float, N>> typed_floats(std::string_view name, ShaderParamType want) {
    auto param = ShaderParams::find(name);
    if (!param.has_value() || param->type != want) {
        return std::nullopt;
    }
    std::array<float, N> out{};
    if (param->size != sizeof(out)) {
        return std::nullopt;
    }
    if (!ShaderParams::read_value(*param, out.data(), sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

}  // namespace

std::optional<std::array<float, 16>> ShaderParams::matrix4x4(std::string_view name) {
    return typed_floats<16>(name, ShaderParamType::Matrix4x4);
}

std::optional<std::array<float, 12>> ShaderParams::matrix4x3(std::string_view name) {
    return typed_floats<12>(name, ShaderParamType::Matrix4x3);
}

std::optional<std::array<float, 4>> ShaderParams::float4(std::string_view name) {
    return typed_floats<4>(name, ShaderParamType::Float4);
}

std::optional<std::array<float, 3>> ShaderParams::float3(std::string_view name) {
    return typed_floats<3>(name, ShaderParamType::Float3);
}

std::optional<std::array<float, 2>> ShaderParams::float2(std::string_view name) {
    return typed_floats<2>(name, ShaderParamType::Float2);
}

std::optional<float> ShaderParams::scalar(std::string_view name) {
    auto param = find(name);
    if (!param.has_value() || param->type != ShaderParamType::Float) {
        return std::nullopt;
    }
    float out = 0.0f;
    if (!read_value(*param, &out, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

std::optional<IDirect3DBaseTexture9*> ShaderParams::texture(std::string_view name) {
    auto param = find(name);
    if (!param.has_value() || param->type != ShaderParamType::Texture) {
        return std::nullopt;
    }
    IDirect3DBaseTexture9* out = nullptr;
    if (param->size != sizeof(out) || !read_value(*param, &out, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

std::vector<std::array<float, 12>> ShaderParams::matrix4x3_array(std::string_view name) {
    std::vector<std::array<float, 12>> out;
    auto param = find(name);
    if (!param.has_value() || param->type != ShaderParamType::Matrix4x3) {
        return out;
    }
    const auto count = param->element_count();
    if (count == 0) {
        return out;
    }
    out.resize(count);
    // One copy of the whole run: element_count() has already established that the record's
    // size divides evenly into matrices, so this cannot read past the value.
    if (!read_value(*param, out.data(), count * sizeof(std::array<float, 12>))) {
        out.clear();
    }
    return out;
}

std::optional<std::array<float, 2>> ShaderParams::screen_resolution() {
    return float2("k_vScene_ScreenRes");
}

bool ShaderParams::HalfViewPlane::reciprocals_consistent(float tolerance) const {
    if (half_width == 0.0f || half_height == 0.0f) {
        return false;
    }
    const float want_x = 1.0f / half_width;
    const float want_y = 1.0f / half_height;
    const float dx = want_x - inv_half_width;
    const float dy = want_y - inv_half_height;
    // Relative to the expected value: the reciprocals differ in scale by orders of magnitude
    // between passes (0.44 in a 3D pass, 0.00039 in the screen pass), so a fixed absolute
    // epsilon would either pass everything or fail the small one.
    const float ex = want_x == 0.0f ? dx : dx / want_x;
    const float ey = want_y == 0.0f ? dy : dy / want_y;
    return (ex > -tolerance && ex < tolerance) && (ey > -tolerance && ey < tolerance);
}

float ShaderParams::HalfViewPlane::aspect() const {
    if (half_height == 0.0f) {
        return 0.0f;
    }
    return half_width / half_height;
}

std::optional<ShaderParams::HalfViewPlane> ShaderParams::half_view_plane() {
    const auto raw = float4("k_vHalfViewPlane");
    if (!raw.has_value()) {
        return std::nullopt;
    }
    HalfViewPlane out{};
    out.half_width = (*raw)[0];
    out.half_height = (*raw)[1];
    out.inv_half_width = (*raw)[2];
    out.inv_half_height = (*raw)[3];
    return out;
}

std::optional<std::array<float, 2>> ShaderParams::z_range() {
    return float2("k_vScene_ZRange");
}

std::optional<std::array<float, 3>> ShaderParams::world_space_camera_dir() {
    return float3("k_vWorldSpaceCameraDir");
}

std::optional<float> ShaderParams::frame_time() {
    return scalar("k_fTime");
}

}  // namespace sdk

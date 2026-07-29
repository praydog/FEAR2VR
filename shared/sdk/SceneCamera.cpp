#include "SceneCamera.hpp"

#include <cmath>
#include <cstring>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// g_SceneRenderer + 8. The scene renderer object's first dword is its state (the 1..4 value
// compared throughout the render code); the camera record follows it.
constexpr uintptr_t kRecordOffset = 0x32E790;

// Field offsets within the record, all from sub_610BA1's and sub_610DA2's own accesses.
constexpr size_t kMode = 0x00;
constexpr size_t kViewportLeft = 0x04;
constexpr size_t kHalfViewPlane = 0x30;      // 48
constexpr size_t kProjCenterOffset = 0x38;   // 56
constexpr size_t kDepthRange = 0x40;         // 64
constexpr size_t kView = 0x48;               // 72, 3 rows of 4
constexpr size_t kProjection = 0x78;         // 120
constexpr size_t kViewProjection = 0xB8;     // 184
constexpr size_t kDerived = 0xF8;            // 248
constexpr size_t kTrailing = 0x138;          // 312, 12 floats
constexpr size_t kRecordSize = 0x168;        // 360 -- through the trailing floats

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
}

// The single guarded copy. Nothing that unwinds may live in this frame, which is why the buffer
// is a raw array supplied by the caller.
bool seh_copy(void* out, uintptr_t at, size_t bytes) {
    if (at == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

float float_at(const unsigned char* base, size_t offset) {
    float v = 0.0f;
    std::memcpy(&v, base + offset, sizeof(v));
    return v;
}

int32_t int_at(const unsigned char* base, size_t offset) {
    int32_t v = 0;
    std::memcpy(&v, base + offset, sizeof(v));
    return v;
}

template <size_t N>
std::array<float, N> floats_at(const unsigned char* base, size_t offset) {
    std::array<float, N> out{};
    std::memcpy(out.data(), base + offset, sizeof(out));
    return out;
}

// Returns false for a non-finite input: NaN fails every comparison below, which is the answer
// we want from a validator rather than an accident to rely on.
bool near_equal(float a, float b, float relative_tolerance) {
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return false;
    }
    const float d = a - b;
    const float scale = b < 0.0f ? -b : b;
    const float allow = scale * relative_tolerance + 1e-9f;
    return d > -allow && d < allow;
}

}  // namespace

int64_t SceneCameraSnapshot::viewport_width() const {
    return static_cast<int64_t>(viewport_right) - static_cast<int64_t>(viewport_left);
}

int64_t SceneCameraSnapshot::viewport_height() const {
    return static_cast<int64_t>(viewport_bottom) - static_cast<int64_t>(viewport_top);
}

bool SceneCameraSnapshot::viewport_valid() const {
    return viewport_width() > 0 && viewport_height() > 0;
}

bool SceneCameraSnapshot::view_is_identity(float tolerance) const {
    // Row-major 3x4: the leading 3x3 is identity and the translation column is zero.
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            const float got = view[row * 4 + col];
            if (!std::isfinite(got)) {
                return false;
            }
            const float want = (col == row) ? 1.0f : 0.0f;
            if (std::fabs(got - want) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

bool SceneCameraSnapshot::is_orthographic_projection() const {
    // [3][3] is 1 for orthographic and 0 for row-major perspective, where w comes from z.
    return near_equal(projection[15], 1.0f, 1e-3f);
}

bool SceneCameraSnapshot::projection_matches_viewport_ortho(float tolerance) const {
    if (!viewport_valid() || !is_orthographic_projection()) {
        return false;
    }
    const float want_x = 2.0f / static_cast<float>(viewport_width());
    const float want_y = -2.0f / static_cast<float>(viewport_height());
    return near_equal(projection[0], want_x, tolerance) &&
           near_equal(projection[5], want_y, tolerance);
}

uintptr_t SceneCamera::record_address() {
    return exe_at(kRecordOffset);
}

std::optional<SceneCameraSnapshot> SceneCamera::snapshot() {
    const auto at = record_address();
    if (at == 0) {
        return std::nullopt;
    }
    // ONE copy, deliberately: the render thread rewrites this record per pass, and reading it
    // field by field can splice two passes together without faulting.
    unsigned char raw[kRecordSize];
    if (!seh_copy(raw, at, sizeof(raw))) {
        return std::nullopt;
    }

    SceneCameraSnapshot out{};
    out.mode = static_cast<uint32_t>(int_at(raw, kMode));
    out.viewport_left = int_at(raw, kViewportLeft);
    out.viewport_top = int_at(raw, kViewportLeft + 4);
    out.viewport_right = int_at(raw, kViewportLeft + 8);
    out.viewport_bottom = int_at(raw, kViewportLeft + 12);
    out.half_view_plane_x = float_at(raw, kHalfViewPlane);
    out.half_view_plane_y = float_at(raw, kHalfViewPlane + 4);
    out.proj_center_offset_x = float_at(raw, kProjCenterOffset);
    out.proj_center_offset_y = float_at(raw, kProjCenterOffset + 4);
    out.depth_min = float_at(raw, kDepthRange);
    out.depth_max = float_at(raw, kDepthRange + 4);
    out.view = floats_at<12>(raw, kView);
    out.projection = floats_at<16>(raw, kProjection);
    out.view_projection = floats_at<16>(raw, kViewProjection);
    out.derived = floats_at<16>(raw, kDerived);
    out.trailing = floats_at<12>(raw, kTrailing);
    return out;
}

}  // namespace sdk

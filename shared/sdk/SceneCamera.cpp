#include "SceneCamera.hpp"

#include <cmath>
#include <cstring>

#include "Memory.hpp"
#include "Modules.hpp"
#include "interfaces/ILTRenderer.hpp"

namespace sdk {

namespace {

// g_SceneRenderer + 8. The scene renderer object's first dword is its state (the 1..4 value
// compared throughout the render code); the camera record follows it.
constexpr uintptr_t kRecordOffset = 0x32E790;

// g_SceneRenderer itself: the record is at +8, and the object's first dword is the state.
constexpr uintptr_t kStateOffset = 0x32E788;

// Field offsets within the record, all from sub_610BA1's and sub_610DA2's own accesses.
constexpr size_t kMode = 0x00;
constexpr size_t kPose = 0x14;  // LTNodeTransform: position 0x00, rotation 0x0C
constexpr size_t kViewportLeft = 0x04;
constexpr size_t kHalfViewPlane = 0x30;      // 48
constexpr size_t kProjCenterOffset = 0x38;   // 56
constexpr size_t kDepthRange = 0x40;         // 64
constexpr size_t kView = 0x48;               // 72, 3 rows of 4
constexpr size_t kProjection = 0x78;         // 120
constexpr size_t kViewProjection = 0xB8;     // 184
constexpr size_t kWorldToScreen = 0xF8;      // 248, viewport * view_projection
constexpr size_t kScreenToClip = 0x138;      // 312, pixels -> clip, from the viewport
constexpr size_t kRecordSize = 0x168;        // 360 -- through the trailing floats

// The snapshot memcpy's correctness rests on these generated types staying packed at the sizes
// the engine uses. If the schema is regenerated differently, every field after the pose shifts
// silently -- so fail at compile time instead.
static_assert(sizeof(regenny::LTNodeTransform) == 0x1C,
              "camera pose layout: LTVector position at 0x00 + LTRotation at 0x0C");
static_assert(sizeof(regenny::LTRotation) == 0x10, "quaternion is 4 floats, w last");
static_assert(sizeof(regenny::LTMatrix3x4) == 0x30, "view matrix is 12 floats");
static_assert(kPose + sizeof(regenny::LTNodeTransform) <= kHalfViewPlane,
              "the pose must not overlap the half view-plane pair");
static_assert(kView + sizeof(regenny::LTMatrix3x4) <= kProjection,
              "the view matrix must not overlap the projection");

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
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

// A tolerance a validator may act on: finite and non-negative.
//
// EVERY PUBLIC PREDICATE HERE GUARDS ON THIS, and the reason is that the failure directions differ
// in ways no reader would spot. A predicate written as `deviation > tolerance` accepts EVERYTHING
// when the tolerance is NaN, because the comparison is false. One written through near_equal rejects
// on NaN -- but accepts everything on +inf, since the allowance becomes infinite. Same argument,
// opposite outcomes, depending only on how the comparison happens to be spelled. So the guard lives
// in one place and every entry point calls it rather than each relying on its own arithmetic.
bool usable_tolerance(float tolerance) {
    return std::isfinite(tolerance) && tolerance >= 0.0f;
}

// Returns false for a non-finite input: NaN fails every comparison below, which is the answer
// we want from a validator rather than an accident to rely on.
bool near_equal(float a, float b, float relative_tolerance) {
    if (!usable_tolerance(relative_tolerance)) {
        return false;
    }
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
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    // Row-major 3x4: the leading 3x3 is identity and the translation column is zero.
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            const float got = view.m[row * 4 + col];
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

bool SceneCameraSnapshot::view_projection_is_coherent(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    const auto recomposed = SceneCamera::compose_view_projection(projection, view);
    for (size_t i = 0; i < 16; ++i) {
        const float want = view_projection[i];
        if (!std::isfinite(want) || !std::isfinite(recomposed[i])) {
            return false;
        }
        // Relative, because the coefficients of a projection span several orders of magnitude.
        const float allow = std::fabs(want) * tolerance + 1e-5f;
        if (std::fabs(recomposed[i] - want) > allow) {
            return false;
        }
    }
    return true;
}

bool SceneCameraSnapshot::view_matches_pose(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    const auto want = SceneCamera::view_matrix_from_pose(pose);
    if (!want.has_value()) {
        return false;
    }
    for (size_t i = 0; i < 12; ++i) {
        if (!std::isfinite(view.m[i]) || !std::isfinite(want->m[i])) {
            return false;
        }
        const float allow = std::fabs(want->m[i]) * tolerance + 1e-4f;
        if (std::fabs(view.m[i] - want->m[i]) > allow) {
            return false;
        }
    }
    return true;
}

std::optional<regenny::LTMatrix3x4> SceneCameraSnapshot::viewport_transform() const {
    if (!viewport_valid()) {
        return std::nullopt;
    }
    const float half_w = static_cast<float>(viewport_width()) * 0.5f;
    const float half_h = static_cast<float>(viewport_height()) * 0.5f;
    const float centre_x = static_cast<float>(viewport_left) + half_w;
    const float centre_y = static_cast<float>(viewport_top) + half_h;
    if (!std::isfinite(half_w) || !std::isfinite(half_h) || !std::isfinite(centre_x) ||
        !std::isfinite(centre_y)) {
        return std::nullopt;
    }
    regenny::LTMatrix3x4 out{};
    out.m[0] = half_w;
    out.m[3] = centre_x;
    out.m[5] = -half_h;  // y flipped, as the engine emits it
    out.m[7] = centre_y;
    out.m[10] = 1.0f;
    return out;
}

bool SceneCameraSnapshot::world_to_screen_is_coherent(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    const auto viewport = viewport_transform();
    if (!viewport.has_value()) {
        return false;
    }
    std::array<float, 12> affine{};
    for (size_t i = 0; i < 12; ++i) {
        affine[i] = viewport->m[i];
    }
    // MulAffineBy4x4's shape: the affine operand on the LEFT, the 4x4 on the right, with the output's
    // row 3 taken from the 4x4.
    std::array<float, 16> expected{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (size_t k = 0; k < 4; ++k) {
                sum += affine[row * 4 + k] * view_projection[k * 4 + col];
            }
            expected[row * 4 + col] = sum;
        }
    }
    for (size_t col = 0; col < 4; ++col) {
        expected[12 + col] = view_projection[12 + col];
    }
    for (size_t i = 0; i < 16; ++i) {
        if (!near_equal(world_to_screen[i], expected[i], tolerance)) {
            return false;
        }
    }
    return true;
}

std::optional<SceneCameraSnapshot::ScreenPoint> SceneCameraSnapshot::project_point(
    float world_x, float world_y, float world_z) const {
    if (!std::isfinite(world_x) || !std::isfinite(world_y) || !std::isfinite(world_z)) {
        return std::nullopt;
    }
    const auto& m = world_to_screen;
    for (size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) {
            return std::nullopt;
        }
    }
    // Row-major, point as a column vector: p' = M * p.
    const float x = m[0] * world_x + m[1] * world_y + m[2] * world_z + m[3];
    const float y = m[4] * world_x + m[5] * world_y + m[6] * world_z + m[7];
    const float w = m[12] * world_x + m[13] * world_y + m[14] * world_z + m[15];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || w <= 0.0f) {
        // Under perspective this is "on or behind the camera plane". Under an affine projection w is
        // the constant m[3][3], so reaching here means that constant is non-positive -- degenerate
        // either way, and in neither case is there an honest pixel.
        return std::nullopt;
    }
    ScreenPoint out{};
    out.x = x / w;
    out.y = y / w;
    out.w = w;
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) {
        return std::nullopt;
    }
    return out;
}

std::optional<SceneCameraSnapshot::ClipPoint> SceneCameraSnapshot::pixel_to_clip(
    float pixel_x, float pixel_y) const {
    if (!std::isfinite(pixel_x) || !std::isfinite(pixel_y)) {
        return std::nullopt;
    }
    const auto& m = screen_to_clip.m;
    for (size_t i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) {
            return std::nullopt;
        }
    }
    // Row-major, point as a column vector, z taken as 0 since this maps a screen position.
    ClipPoint out{};
    out.x = m[0] * pixel_x + m[1] * pixel_y + m[3];
    out.y = m[4] * pixel_x + m[5] * pixel_y + m[7];
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) {
        return std::nullopt;
    }
    return out;
}

bool SceneCameraSnapshot::screen_to_clip_inverts_viewport(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    const auto viewport = viewport_transform();
    if (!viewport.has_value()) {
        return false;
    }
    std::array<float, 12> affine{};
    for (size_t i = 0; i < 12; ++i) {
        affine[i] = viewport->m[i];
    }
    return SceneCamera::affines_are_inverse(*viewport, screen_to_clip, tolerance, tolerance,
                                            4e-7f) ||
           SceneCamera::affines_are_inverse(screen_to_clip, *viewport, tolerance, tolerance, 4e-7f);
}

bool SceneCameraSnapshot::w_is_view_space_depth() const {
    // CLASSIFIES world_to_screen, NOT projection, because that is the matrix project_point transforms
    // through. The first version asked is_perspective_projection() -- which reads `projection` -- and a
    // snapshot carrying only a world_to_screen answered "not perspective" while project_point happily
    // produced depth-bearing w values from it. Two matrices, one question, and the predicate has to
    // describe the one its sibling uses.
    //
    // Same scale-invariant test as the projection classifier: m[3][2] nonzero relative to its own w row.
    float scale = 0.0f;
    for (size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(world_to_screen[i])) {
            return false;
        }
    }
    for (size_t i = 12; i < 16; ++i) {
        const float magnitude = std::fabs(world_to_screen[i]);
        if (magnitude > scale) {
            scale = magnitude;
        }
    }
    if (scale <= 0.0f) {
        return false;
    }
    return std::fabs(world_to_screen[14]) > scale * 1e-6f;
}

bool SceneCameraSnapshot::pose_rotation_is_unit(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    const float x = pose.rotation.x, y = pose.rotation.y, z = pose.rotation.z, w = pose.rotation.w;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
        return false;
    }
    const float magnitude = std::sqrt(x * x + y * y + z * z + w * w);
    return std::fabs(magnitude - 1.0f) <= tolerance;
}

bool SceneCameraSnapshot::pose_position_is_finite() const {
    return std::isfinite(pose.position.x) && std::isfinite(pose.position.y) &&
           std::isfinite(pose.position.z);
}

bool SceneCameraSnapshot::pose_is_identity(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    if (!pose_position_is_finite() || !pose_rotation_is_unit()) {
        return false;
    }
    const float components[7] = {
        pose.position.x, pose.position.y, pose.position.z,
        pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w,
    };
    const float wanted[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < 7; ++i) {
        if (std::fabs(components[i] - wanted[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

namespace {

// tan(fov/2) = m[3][2] / scale_coefficient, scale-invariant. nullopt unless the result is a
// finite POSITIVE half-extent, since a negative or zero tangent is not a field of view.
std::optional<float> fov_from(float zw, float scale_coefficient) {
    if (!std::isfinite(scale_coefficient) || std::fabs(scale_coefficient) < 1e-12f) {
        return std::nullopt;
    }
    const float half_extent = zw / scale_coefficient;
    if (!std::isfinite(half_extent) || half_extent <= 0.0f) {
        return std::nullopt;
    }
    return 2.0f * std::atan(half_extent);
}

}  // namespace

std::optional<float> SceneCameraSnapshot::fov_x_radians() const {
    if (!is_perspective_projection()) {
        return std::nullopt;
    }
    return fov_from(projection[14], projection[0]);
}

std::optional<float> SceneCameraSnapshot::fov_y_radians() const {
    if (!is_perspective_projection()) {
        return std::nullopt;
    }
    return fov_from(projection[14], projection[5]);
}

bool SceneCameraSnapshot::projection_agrees_with_half_view_plane(float tolerance) const {
    // Validated explicitly even though near_equal already fails CLOSED on a bad tolerance -- a NaN
    // allowance makes both of its comparisons false, so a mismatch is rejected rather than accepted.
    // That is the opposite of affines_are_inverse's earlier hole, where the comparison was written
    // out and NaN accepted everything. Stating the precondition here means the safety is a contract
    // rather than a property of a helper someone may later rewrite.
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    if (!std::isfinite(half_view_plane_x) || !std::isfinite(half_view_plane_y) ||
        half_view_plane_x == 0.0f || half_view_plane_y == 0.0f) {
        return false;
    }
    // Whichever element of the w row is nonzero is the matrix's overall scale, and every builder
    // divides the half-extent into exactly that. Perspective puts it at m[3][2], the two affine
    // forms at m[3][3].
    const float scale = is_perspective_projection() ? projection[14] : projection[15];
    if (!std::isfinite(scale) || scale == 0.0f) {
        return false;
    }
    // The y row carries a sign the x row does not: the screen ortho builder emits -1/half_y, so
    // compare magnitudes there rather than demanding the sign match.
    const float x_product = projection[0] * half_view_plane_x;
    const float y_product = projection[5] * half_view_plane_y;
    const float y_magnitude = y_product < 0.0f ? -y_product : y_product;
    const float scale_magnitude = scale < 0.0f ? -scale : scale;
    return near_equal(x_product, scale, tolerance) &&
           near_equal(y_magnitude, scale_magnitude, tolerance);
}

// Largest absolute coefficient of the W ROW, m[3][0..3], or 0 when any entry of the matrix is
// non-finite.
//
// THE W ROW SPECIFICALLY, not the whole matrix, and the difference is a real misclassification:
// the w row is what produces the output w, so comparing m[3][2] against it is invariant to
// wholesale scaling WITHOUT coupling the test to the projection's dynamic range. Measured against
// max(all 16), a legitimately narrow field of view makes m[0][0] enormous, and a perspective
// m[3][2] of 1 can then fall under max*epsilon and read as affine.
float SceneCameraSnapshot::projection_w_row_scale() const {
    for (size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(projection[i])) {
            return 0.0f;
        }
    }
    float scale = 0.0f;
    for (size_t i = 12; i < 16; ++i) {
        const float magnitude = std::fabs(projection[i]);
        if (magnitude > scale) {
            scale = magnitude;
        }
    }
    return scale;
}

bool SceneCameraSnapshot::is_perspective_projection() const {
    // m[3][2] is the z -> w coupling; nonzero means w varies with depth. Compared RELATIVE to the
    // matrix's own w row, because homogeneous matrices may be scaled wholesale and a valid
    // perspective coefficient can then be arbitrarily small in absolute terms.
    const float scale = projection_w_row_scale();
    if (scale <= 0.0f) {
        return false;
    }
    return std::fabs(projection[14]) > scale * 1e-6f;
}

bool SceneCameraSnapshot::is_affine_projection() const {
    const float scale = projection_w_row_scale();
    if (scale <= 0.0f) {
        return false;  // a non-finite or all-zero matrix is neither, not affine by default
    }
    return std::fabs(projection[14]) <= scale * 1e-6f;
}

bool SceneCameraSnapshot::is_normalized_orthographic_projection() const {
    return is_affine_projection() && near_equal(projection[15], 1.0f, 1e-3f);
}

bool SceneCameraSnapshot::projection_matches_viewport_ortho(float tolerance) const {
    if (!usable_tolerance(tolerance)) {
        return false;
    }
    if (!viewport_valid() || !is_normalized_orthographic_projection()) {
        return false;
    }
    const float want_x = 2.0f / static_cast<float>(viewport_width());
    const float want_y = -2.0f / static_cast<float>(viewport_height());
    return near_equal(projection[0], want_x, tolerance) &&
           near_equal(projection[5], want_y, tolerance);
}

namespace {

// A usable frustum extent: finite and strictly positive. A negative or zero half-extent has no
// geometric meaning and would put a zero or inverted scale in the matrix.
bool usable_half_extent(float v) {
    return std::isfinite(v) && v > 0.0f;
}

}  // namespace

std::optional<std::array<float, 16>> SceneCamera::make_perspective_projection(float half_x,
                                                                             float half_y,
                                                                             float near_z) {
    if (!usable_half_extent(half_x) || !usable_half_extent(half_y) || !std::isfinite(near_z)) {
        return std::nullopt;
    }
    // THE RECIPROCALS ARE CHECKED, not assumed: a finite positive but very small extent overflows
    // to infinity here, so validating the input alone would let nullopt-means-usable be a lie.
    const float sx = 1.0f / half_x;
    const float sy = 1.0f / half_y;
    if (!std::isfinite(sx) || !std::isfinite(sy)) {
        return std::nullopt;
    }
    // Column 3 holds the translation and m[3][2] = 1 makes w = z, exactly as the engine emits.
    return std::array<float, 16>{
        sx, 0.0f, 0.0f, 0.0f,
        0.0f, sy, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, -near_z,
        0.0f, 0.0f, 1.0f, 0.0f,
    };
}

std::optional<std::array<float, 16>> SceneCamera::make_affine_projection(float half_x, float half_y,
                                                                        float near_z, float far_z) {
    if (!usable_half_extent(half_x) || !usable_half_extent(half_y) || !std::isfinite(near_z) ||
        !std::isfinite(far_z)) {
        return std::nullopt;
    }
    const float k = far_z - near_z;
    // A zero span collapses the matrix; a non-finite one poisons every coefficient.
    if (!std::isfinite(k) || std::fabs(k) < 1e-9f) {
        return std::nullopt;
    }
    const float sx = k / half_x;
    const float sy = k / half_y;
    if (!std::isfinite(sx) || !std::isfinite(sy)) {
        return std::nullopt;
    }
    return std::array<float, 16>{
        sx, 0.0f, 0.0f, 0.0f,
        0.0f, sy, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, -near_z,
        0.0f, 0.0f, 0.0f, k,
    };
}

std::optional<regenny::LTMatrix3x4> SceneCamera::transform_to_matrix(
    const regenny::LTNodeTransform& transform) {
    const auto rotation = rotation_matrix(transform.rotation);
    if (!rotation.has_value()) {
        return std::nullopt;
    }
    if (!std::isfinite(transform.position.x) || !std::isfinite(transform.position.y) ||
        !std::isfinite(transform.position.z)) {
        return std::nullopt;
    }
    regenny::LTMatrix3x4 out{};
    for (size_t i = 0; i < 12; ++i) {
        out.m[i] = rotation->m[i];
    }
    // Column 3, exactly where LTTransform_ToMatrix3x4 puts it.
    out.m[3] = transform.position.x;
    out.m[7] = transform.position.y;
    out.m[11] = transform.position.z;
    return out;
}

std::optional<regenny::LTNodeTransform> SceneCamera::invert_transform(
    const regenny::LTNodeTransform& transform) {
    regenny::LTRotation conjugate{};
    conjugate.x = -transform.rotation.x;
    conjugate.y = -transform.rotation.y;
    conjugate.z = -transform.rotation.z;
    conjugate.w = transform.rotation.w;

    // Rotate the position by the conjugate and negate it. Done through the rotation MATRIX rather
    // than a second quaternion-product transcription: R(conj q) is R(q) transposed for any norm, so
    // this is the rigid inverse without duplicating LTRotation_RotateVector's expansion -- and the
    // round-trip check in the suite is what confirms it composes to the identity.
    const auto r = rotation_matrix(conjugate);
    if (!r.has_value()) {
        return std::nullopt;
    }
    const float px = transform.position.x, py = transform.position.y, pz = transform.position.z;
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) {
        return std::nullopt;
    }
    // COMPUTE THEN CHECK, the same rule the projection builders needed: three dot products over
    // finite inputs near the top of float range still overflow, and returning has_value() with an
    // infinite position would make this optional mean "the input looked fine" rather than "here is a
    // usable transform".
    const float ix = -(r->m[0] * px + r->m[1] * py + r->m[2] * pz);
    const float iy = -(r->m[4] * px + r->m[5] * py + r->m[6] * pz);
    const float iz = -(r->m[8] * px + r->m[9] * py + r->m[10] * pz);
    if (!std::isfinite(ix) || !std::isfinite(iy) || !std::isfinite(iz)) {
        return std::nullopt;
    }
    regenny::LTNodeTransform out{};
    out.rotation = conjugate;
    out.position.x = ix;
    out.position.y = iy;
    out.position.z = iz;
    return out;
}

std::optional<regenny::LTMatrix3x4> SceneCamera::view_matrix_from_pose(
    const regenny::LTNodeTransform& pose) {
    const auto inverted = invert_transform(pose);
    if (!inverted.has_value()) {
        return std::nullopt;
    }
    return transform_to_matrix(*inverted);
}

std::array<float, 16> SceneCamera::promote_affine(const regenny::LTMatrix3x4& affine) {
    std::array<float, 16> out{};
    for (size_t i = 0; i < 12; ++i) {
        out[i] = affine.m[i];
    }
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
    return out;
}

bool SceneCamera::affines_are_inverse(const regenny::LTMatrix3x4& forward,
                                      const regenny::LTMatrix3x4& inverse,
                                      float rotation_tolerance, float translation_base,
                                      float translation_per_unit) {
    // VALIDATE THE TOLERANCES, because they are public arguments and a NaN one silently accepts
    // everything: every `deviation > allow` comparison is false against NaN, so a matrix that is not
    // an inverse would pass. A negative tolerance rejects everything, which is merely useless.
    if (!usable_tolerance(rotation_tolerance) || !usable_tolerance(translation_base) ||
        !usable_tolerance(translation_per_unit)) {
        return false;
    }
    std::array<float, 12> rhs{};
    for (size_t i = 0; i < 12; ++i) {
        if (!std::isfinite(forward.m[i]) || !std::isfinite(inverse.m[i])) {
            return false;
        }
        rhs[i] = forward.m[i];
    }
    const auto product = multiply_by_affine(promote_affine(inverse), rhs);

    // Scaled by how far the FORWARD matrix translates, since that is the magnitude the cancellation
    // works against. The coefficient is epsilon-sized, NOT the rotation tolerance: at 98000 units it
    // permits about 0.04 against a measured residual of 0.
    float distance = 0.0f;
    const size_t translation_indices[3] = {3, 7, 11};
    for (size_t i = 0; i < 3; ++i) {
        const float magnitude = std::fabs(forward.m[translation_indices[i]]);
        if (magnitude > distance) {
            distance = magnitude;
        }
    }
    if (!std::isfinite(distance)) {
        return false;
    }
    const float translation_allowance = translation_base + translation_per_unit * distance;
    if (!std::isfinite(translation_allowance)) {
        return false;  // a large per-unit against a distant pose can overflow to infinity
    }

    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            const float got = product[row * 4 + col];
            if (!std::isfinite(got)) {
                return false;
            }
            const float want = (col == row) ? 1.0f : 0.0f;
            const float allow = (col == 3) ? translation_allowance : rotation_tolerance;
            if (std::fabs(got - want) > allow) {
                return false;
            }
        }
    }
    return true;
}

bool SceneCamera::view_inverts_pose(const regenny::LTNodeTransform& pose) {
    const auto view = view_matrix_from_pose(pose);
    const auto forward = transform_to_matrix(pose);
    if (!view.has_value() || !forward.has_value()) {
        return false;
    }
    return affines_are_inverse(*forward, *view);
}

std::optional<SceneCamera::RoundTripError> SceneCamera::view_inverse_round_trip_error(
    const regenny::LTNodeTransform& pose) {
    const auto view = view_matrix_from_pose(pose);
    const auto forward = transform_to_matrix(pose);
    if (!view.has_value() || !forward.has_value()) {
        return std::nullopt;
    }
    std::array<float, 12> rhs{};
    for (size_t i = 0; i < 12; ++i) {
        rhs[i] = forward->m[i];
    }
    const auto product = multiply_by_affine(promote_affine(*view), rhs);
    RoundTripError out{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            const float got = product[row * 4 + col];
            if (!std::isfinite(got)) {
                return std::nullopt;
            }
            const float deviation = std::fabs(got - ((col == row) ? 1.0f : 0.0f));
            float& worst = (col == 3) ? out.translation : out.rotation;
            if (deviation > worst) {
                worst = deviation;
            }
        }
    }
    return out;
}

std::optional<regenny::LTRotation> SceneCamera::rotation_from_matrix(
    const regenny::LTMatrix3x4& m) {
    for (size_t i = 0; i < 12; ++i) {
        if (!std::isfinite(m.m[i])) {
            return std::nullopt;
        }
    }
    regenny::LTRotation out{};
    const float trace = m.m[0] + m.m[5] + m.m[10];
    // The engine's own threshold is -0.999 rather than 0: below it the trace branch loses precision.
    if (trace >= -0.999f) {
        const float root = std::sqrt(trace + 1.0f);
        if (!std::isfinite(root) || root <= 0.0f) {
            return std::nullopt;
        }
        const float scale = 0.5f / root;
        out.w = root * 0.5f;
        out.x = (m.m[9] - m.m[6]) * scale;
        out.y = (m.m[2] - m.m[8]) * scale;
        out.z = (m.m[4] - m.m[1]) * scale;
    } else {
        // Largest diagonal element, then the cyclic permutation around it.
        size_t i = (m.m[0] < m.m[5]) ? 1 : 0;
        if (m.m[5 * i] < m.m[10]) {
            i = 2;
        }
        const size_t j = (i + 1) % 3;
        const size_t k = (i + 2) % 3;
        const float root = std::sqrt(m.m[5 * i] - (m.m[5 * j] + m.m[5 * k]) + 1.0f);
        if (!std::isfinite(root) || root <= 0.0f) {
            return std::nullopt;
        }
        const float scale = 0.5f / root;
        float q[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        q[i] = root * 0.5f;
        q[3] = (m.m[4 * k + j] - m.m[4 * j + k]) * scale;
        q[j] = (m.m[4 * j + i] + m.m[4 * i + j]) * scale;
        q[k] = (m.m[4 * k + i] + m.m[4 * i + k]) * scale;
        out.x = q[0];
        out.y = q[1];
        out.z = q[2];
        out.w = q[3];
    }
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z) ||
        !std::isfinite(out.w)) {
        return std::nullopt;
    }
    return out;
}

regenny::LTMatrix3x4 SceneCamera::matrix_from_basis_columns(float right_x, float right_y,
                                                            float right_z, float up_x, float up_y,
                                                            float up_z, float fwd_x, float fwd_y,
                                                            float fwd_z) {
    regenny::LTMatrix3x4 out{};
    out.m[0] = right_x;
    out.m[4] = right_y;
    out.m[8] = right_z;
    out.m[1] = up_x;
    out.m[5] = up_y;
    out.m[9] = up_z;
    out.m[2] = fwd_x;
    out.m[6] = fwd_y;
    out.m[10] = fwd_z;
    return out;
}

std::optional<regenny::LTRotation> SceneCamera::rotation_from_forward_up(
    float forward_x, float forward_y, float forward_z, float up_x, float up_y, float up_z) {
    const float f[3] = {forward_x, forward_y, forward_z};
    float u[3] = {up_x, up_y, up_z};
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i]) || !std::isfinite(u[i])) {
            return std::nullopt;
        }
    }
    // The engine's degenerate handling: beyond +/-0.99 the hint is SWIZZLED rather than rejected.
    const float dot = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
    if (!std::isfinite(dot)) {
        return std::nullopt;
    }
    if (dot > 0.99f || dot < -0.99f) {
        u[0] = f[1];
        u[1] = f[2];
        u[2] = f[0];
    }
    // LTVector_Cross computes a3 x this, so the first call yields up x forward.
    float right[3] = {
        u[1] * f[2] - u[2] * f[1],
        u[2] * f[0] - u[0] * f[2],
        u[0] * f[1] - u[1] * f[0],
    };
    const float length_sq = right[0] * right[0] + right[1] * right[1] + right[2] * right[2];
    if (!std::isfinite(length_sq) || length_sq <= 0.0f) {
        return std::nullopt;  // forward and the (possibly swizzled) hint are still parallel
    }
    const float inv = 1.0f / std::sqrt(length_sq);
    if (!std::isfinite(inv)) {
        return std::nullopt;
    }
    for (size_t i = 0; i < 3; ++i) {
        right[i] *= inv;
    }
    // Second cross: this = right, a3 = forward, so the result is forward x right.
    const float new_up[3] = {
        f[1] * right[2] - f[2] * right[1],
        f[2] * right[0] - f[0] * right[2],
        f[0] * right[1] - f[1] * right[0],
    };
    const auto basis = matrix_from_basis_columns(right[0], right[1], right[2], new_up[0], new_up[1],
                                                 new_up[2], f[0], f[1], f[2]);
    return rotation_from_matrix(basis);
}

regenny::LTNodeTransform SceneCamera::make_camera_transform(float x, float y, float z,
                                                            const regenny::LTRotation& rotation) {
    regenny::LTNodeTransform out{};
    out.position.x = x;
    out.position.y = y;
    out.position.z = z;
    out.rotation = rotation;
    return out;
}

std::array<float, 12> SceneCamera::affine_identity() {
    return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
}

std::array<float, 16> SceneCamera::multiply_by_affine(const std::array<float, 16>& a,
                                                      const std::array<float, 12>& b) {
    std::array<float, 16> out{};
    for (size_t row = 0; row < 4; ++row) {
        const float a0 = a[row * 4 + 0];
        const float a1 = a[row * 4 + 1];
        const float a2 = a[row * 4 + 2];
        const float a3 = a[row * 4 + 3];
        for (size_t col = 0; col < 3; ++col) {
            // Three terms only: b's implicit fourth row contributes nothing outside column 3.
            out[row * 4 + col] = a0 * b[0 * 4 + col] + a1 * b[1 * 4 + col] + a2 * b[2 * 4 + col];
        }
        // Column 3 picks up a[row][3] directly, which is what b[3] = (0,0,0,1) contributes.
        out[row * 4 + 3] = a0 * b[0 * 4 + 3] + a1 * b[1 * 4 + 3] + a2 * b[2 * 4 + 3] + a3;
    }
    return out;
}

std::array<float, 16> SceneCamera::compose_view_projection(const std::array<float, 16>& projection,
                                                           const regenny::LTMatrix3x4& view) {
    std::array<float, 12> affine{};
    for (size_t i = 0; i < 12; ++i) {
        affine[i] = view.m[i];
    }
    return multiply_by_affine(projection, affine);
}

std::optional<std::array<float, 2>> SceneCamera::predicted_half_view_plane(float fov_x,
                                                                          float fov_y) {
    if (!std::isfinite(fov_x) || !std::isfinite(fov_y)) {
        return std::nullopt;
    }
    // The engine CLAMPS rather than rejects, so reproduce that instead of refusing out-of-range input:
    // a consumer predicting the result of a call it is about to make wants the engine's answer.
    const float clamped_x = fov_x < 0.0f ? 0.0f : (fov_x > kMaxFovRadians ? kMaxFovRadians : fov_x);
    const float clamped_y = fov_y < 0.0f ? 0.0f : (fov_y > kMaxFovRadians ? kMaxFovRadians : fov_y);
    std::array<float, 2> out{std::tan(clamped_x * 0.5f), std::tan(clamped_y * 0.5f)};
    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::array<int32_t, 4>> SceneCamera::predicted_viewport_pixels(
    const std::array<float, 4>& normalized_rect, int32_t target_width, int32_t target_height) {
    if (target_width <= 0 || target_height <= 0) {
        return std::nullopt;
    }
    std::array<int32_t, 4> out{};
    for (size_t i = 0; i < 4; ++i) {
        const float v = normalized_rect[i];
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        // Even indices scale by width, odd by height -- left, top, right, bottom.
        const float extent = static_cast<float>((i % 2 == 0) ? target_width : target_height);
        const float scaled = clamped * extent + 0.5f;
        if (!std::isfinite(scaled)) {
            return std::nullopt;
        }
        out[i] = static_cast<int32_t>(scaled);
    }
    return out;
}

std::optional<uint32_t> SceneCamera::state() {
    const auto at = exe_at(kStateOffset);
    if (at == 0) {
        return std::nullopt;
    }
    uint32_t value = 0;
    if (!sdk::mem::copy(&value, at, sizeof(value))) {
        return std::nullopt;
    }
    return value;
}

size_t SceneCamera::renderer_slot_index(RendererSlot slot) {
    switch (slot) {
    case RendererSlot::BeginFrame:
        return 8;
    case RendererSlot::BeginRenderTarget:
        return 11;
    case RendererSlot::EndRenderTarget:
        return 12;
    case RendererSlot::SetupPassPerspective:
        return 15;
    case RendererSlot::SetupPassAffine:
        return 16;
    case RendererSlot::SetupPassStored:
        return 17;
    case RendererSlot::EndPass:
        return 18;
    case RendererSlot::DrawScene:
        return 20;
    case RendererSlot::DrawObjectList:
        return 21;
    default:
        return 0;
    }
}

uintptr_t SceneCamera::renderer_fn(RendererSlot which) {
    const auto slot = renderer_slot_index(which);
    if (slot == 0) {
        return 0;
    }
    auto* renderer = interfaces::ILTRenderer::get();
    if (renderer == nullptr) {
        return 0;  // not resolved right now; the interface database clears slots
    }
    uintptr_t vftable = 0;
    if (!sdk::mem::copy(&vftable, reinterpret_cast<uintptr_t>(renderer), sizeof(vftable)) ||
        vftable == 0) {
        return 0;
    }
    uintptr_t fn = 0;
    if (!sdk::mem::copy(&fn, vftable + slot * sizeof(uintptr_t), sizeof(fn)) || fn == 0) {
        return 0;
    }
    // BOUNDS-CHECKED, because the whole point of this address is that something will hook it: a
    // plausible-looking pointer from an unexpected table would be written to, not merely read.
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || exe->size == 0) {
        return 0;
    }
    if (fn < exe->base || fn >= exe->base + exe->size) {
        return 0;
    }
    return fn;
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
    if (!sdk::mem::copy(raw, at, sizeof(raw))) {
        return std::nullopt;
    }

    SceneCameraSnapshot out{};
    out.mode = static_cast<uint32_t>(int_at(raw, kMode));
    std::memcpy(&out.pose, raw + kPose, sizeof(out.pose));
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
    std::memcpy(&out.view, raw + kView, sizeof(out.view));
    out.projection = floats_at<16>(raw, kProjection);
    out.view_projection = floats_at<16>(raw, kViewProjection);
    out.world_to_screen = floats_at<16>(raw, kWorldToScreen);
    std::memcpy(&out.screen_to_clip, raw + kScreenToClip, sizeof(out.screen_to_clip));
    return out;
}

}  // namespace sdk

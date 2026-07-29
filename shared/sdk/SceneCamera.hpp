#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "regenny/regenny/LTMatrix3x4.hpp"
#include "regenny/regenny/LTNodeTransform.hpp"

// The engine's SCENE CAMERA record: the viewport, the view matrix, the projection, and the
// composed view-projection, all in one static structure at g_SceneRenderer+8.
//
// WHY THIS IS THE INTERESTING STRUCTURE. sub_610BA1 builds the matrices here from the record's
// own scalars, and +184 is the finished view-projection. A stereo path needs exactly these
// quantities, and reading them is far cheaper than intercepting D3D calls -- which this project
// tried at length and could not locate statically.
//
// HOW THE LAYOUT WAS ESTABLISHED, because the offsets carry the whole weight:
//   - The viewport ints read (0, 0, 5120, 1440) live, exactly the screen.
//   - The projection's [0][0] read 0.00039063 against 2/5120 = 0.00039063, and [1][1] read
//     -0.00138889 against -2/1440, to eight decimals. So the screen pass builds a screen-to-clip
//     orthographic matrix, and the matrices are ROW-MAJOR with translation in column 3.
//   - The compose was verified rather than assumed: with the view matrix identity, +184 equalled
//     +120 exactly, which is what P*I = P demands.
//
// THE RECORD IS WRITTEN BY THE RENDER THREAD, once per pass. That is the hazard this class is
// shaped around: a field-by-field read can pick up one pass's viewport and the next pass's
// matrices, and the result faults nothing and looks entirely plausible. So snapshot() takes the
// WHOLE record in a single guarded memcpy. That narrows the window to one copy; it does not
// close it, because nothing here synchronises with the render thread. Validate what you get --
// projection_matches_viewport_ortho() covers the viewport-versus-projection pairing, which is the
// most useful single check available, but it says nothing about the view, derived or trailing
// matrices. There is no invariant here that proves the whole record came from one pass.
namespace sdk {

// A copy of the record, so callers hold values rather than pointers into engine memory.
struct SceneCameraSnapshot {
    // Pass mode. 2 is the engine's screen/2D pass, in which the projection is orthographic and
    // the view matrix is identity. Other values accompany the 3D passes.
    uint32_t mode{};

    // The camera POSE, at +0x14. The generated LTNodeTransform is used rather than a local
    // 7-float type because it is exactly this shape, and LTTransform_Copy confirms the layout
    // from the engine side: three fld/fstp to +0x00/+0x04/+0x08, then LTRotation_Copy at +0x0C.
    // Read live in the screen pass it is the identity -- position (0,0,0), rotation (0,0,0,1) --
    // which also pins the quaternion as w-LAST, matching regenny::LTRotation.
    regenny::LTNodeTransform pose{};

    int32_t viewport_left{};
    int32_t viewport_top{};
    int32_t viewport_right{};
    int32_t viewport_bottom{};

    // Half-extents of the view plane, the pair published as k_vHalfViewPlane. Units follow the
    // pass: pixels in the screen pass, half-extents at unit depth in a 3D pass.
    float half_view_plane_x{};
    float half_view_plane_y{};

    // Centre offset, applied by the builder as the shear
    //   [1 0 -x 0; 0 1 -y 0; 0 0 1 0]
    // so the projection need not be centred. (0, 0) in the screen pass. This is the mechanism
    // an off-centre or per-eye projection would use; the engine's own use of it is not
    // established here, so the name describes the maths, not a purpose.
    float proj_center_offset_x{};
    float proj_center_offset_y{};

    // Fed to the projection builder. In the screen pass these read (0, 1), which are MinZ/MaxZ
    // rather than a scene near/far -- see z_range() on ShaderParams for the scene's own pair.
    float depth_min{};
    float depth_max{};

    regenny::LTMatrix3x4 view{};              // 3 rows of 4, the generated 0x30 type
    std::array<float, 16> projection{};       // proj(depth) * shear
    std::array<float, 16> view_projection{};  // projection * view
    std::array<float, 16> derived{};          // from view_projection; role not established
    std::array<float, 12> trailing{};

    // int64 ON PURPOSE: these are differences of two ints read from a record the render thread
    // rewrites, so a torn read can put values at opposite ends of the range and the subtraction
    // would overflow. Widening keeps that a wrong number rather than undefined behaviour.
    int64_t viewport_width() const;
    int64_t viewport_height() const;

    // A non-degenerate rect. The builder's own callers reject the inverted case, so a snapshot
    // failing this was either read early or read torn.
    bool viewport_valid() const;

    // Rejects non-finite values rather than accepting them: a NaN compares false against every
    // bound, so a naive range test would call a torn matrix identity.
    bool view_is_identity(float tolerance = 1e-4f) const;

    // Is the pose's quaternion unit-length? The project's established OFFSET PROOF: a rotation
    // read at the wrong offset is overwhelmingly unlikely to normalise. Holds in any pass, so
    // unlike the orthographic checks this one is not gated on the mode.
    bool pose_rotation_is_unit(float tolerance = 1e-2f) const;

    // Is the pose's position finite? Kept separate from the rotation check because this project
    // has already been bitten by assuming both travel together: stale bone caches carry
    // non-finite positions beside perfectly unit-length rotations.
    bool pose_position_is_finite() const;

    // Row-major perspective puts w in column 2 and 0 at [3][3]; an orthographic matrix keeps
    // [3][3] == 1. Useful as a NECESSARY condition before treating half_view_plane as
    // tan(fov/2), since that reading is certainly meaningless for the screen pass, and this
    // tells the passes apart from the data rather than by trusting `mode`.
    //
    // NOT SUFFICIENT, though: a false answer only rules out THIS orthographic shape. It does
    // not establish that the projection is a standard unit-depth perspective matrix, nor that
    // the half-plane extents are taken at unit depth, so the tan(fov/2) reading stays
    // provisional until the perspective builder (sub_610560 / sub_6105DA) is read.
    bool is_orthographic_projection() const;

    // The verified invariant: for an orthographic pass, [0][0] == 2/width and [1][1] == -2/height.
    //
    // Worth calling on any snapshot you intend to trust. It ties the viewport ints to the
    // projection floats, which the render thread writes at different moments, so it detects a
    // tear BETWEEN THOSE TWO REGIONS. It is a partial check, not an atomicity proof: a write
    // that tore only the view, derived or trailing matrices would still satisfy it. Returns
    // false for a perspective projection, where the identity does not apply; check
    // is_orthographic_projection() first.
    bool projection_matches_viewport_ortho(float tolerance = 0.02f) const;
};

class SceneCamera {
public:
    // Address of the record (g_SceneRenderer+8), or 0 when the exe is not mapped.
    static uintptr_t record_address();

    // ONE guarded copy of the whole record, then unpacked. nullopt when the exe is not mapped
    // or the copy faulted. See the note above: this bounds tearing, it does not prevent it.
    static std::optional<SceneCameraSnapshot> snapshot();
};

}  // namespace sdk

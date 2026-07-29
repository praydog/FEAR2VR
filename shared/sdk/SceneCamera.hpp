#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "Object.hpp"
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
    // WORLD -> SCREEN PIXELS: viewport_transform * view_projection. Use project_point() rather than
    // multiplying by hand.
    std::array<float, 16> world_to_screen{};
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

    // ---- THE FIELD OF VIEW, NOW ESTABLISHED RATHER THAN GUESSED --------------------
    //
    // LTMatrix_BuildPerspectiveProjection builds m[0][0] = 1/half_x with m[3][2] = 1, and a
    // perspective projection's x scale IS 1/tan(fov_x/2). So half_x = tan(fov_x/2), read off the
    // builder rather than inferred from a ratio -- which is what two earlier passes could not do
    // and why they exposed no FOV at all.
    //
    // Derived from the MATRIX, not from half_view_plane, and by the scale-invariant ratio
    // tan(fov/2) = m[3][2] / m[0][0]. That form survives the wholesale scaling homogeneous
    // matrices permit, so it stays correct if a pass hands over a matrix scaled by anything.
    //
    // nullopt unless the projection is perspective -- which is the precondition the earlier
    // passes could not check and therefore declined to expose.
    std::optional<float> fov_x_radians() const;
    std::optional<float> fov_y_radians() const;

    // Does the projection's x scale agree with the stored half-extent?
    //
    // HOLDS IN EVERY PASS, which took reading all three builders to see. Each divides the half-extent
    // into its own overall scale, so m[0][0] * half_x equals whichever element of the w row is
    // nonzero:
    //
    //     perspective   m[0][0] = 1/half_x        so the product is 1     = m[3][2]
    //     affine        m[0][0] = (f-n)/half_x    so the product is f-n   = m[3][3]
    //     screen ortho  m[0][0] = 1/half_x        so the product is 1     = m[3][3]
    //
    // An earlier version only handled the perspective case and returned false for everything else,
    // which meant it could never be exercised on a live snapshot -- the engine leaves this record in
    // its screen pass between frames. Now it is checkable against the running game.
    //
    // Ties the MATRIX to the SCALAR pair the engine publishes as k_vHalfViewPlane -- two regions
    // written at different moments -- so it is an offset check, a coherence check, and the evidence
    // behind fov_x_radians() all at once.
    //
    // Returns false for a non-finite or negative tolerance. Note this predicate fails CLOSED either
    // way, unlike a hand-written comparison: a NaN allowance rejects rather than accepts.
    bool projection_agrees_with_half_view_plane(float tolerance = 0.02f) const;

    // Does the stored view-projection actually equal projection * view?
    //
    // THE STRONGEST COHERENCE CHECK AVAILABLE ON THIS RECORD, and the reason to prefer it over
    // projection_matches_viewport_ortho(): it spans THREE regions the render thread writes at
    // different moments -- the view matrix at +0x48, the projection at +0x78 and the product at
    // +0xB8 -- so a snapshot that spliced passes together fails it. It also fails if our
    // transcription of the engine's multiply is wrong, which is why the suite asserts it.
    //
    // Its discriminating power is pass-dependent, and a caller should know that: while the view
    // matrix is identity, the product is just the projection and the comparison cannot detect a
    // transposed multiply. Pair it with view_is_identity() when that distinction matters.
    bool view_projection_is_coherent(float tolerance = 1e-4f) const;

    // Does the record's view matrix match the view matrix its own pose implies?
    //
    // Ties the pose at +0x14 to the view matrix at +0x48 -- two more regions the render thread
    // writes at different moments, so it is both a coherence check and a check that our
    // reconstruction of the engine's view-matrix recipe is right.
    //
    // Pass-dependent in the same way as view_projection_is_coherent(): in the screen pass the pose
    // is identity and so is the view matrix, which the comparison cannot distinguish from a wrong
    // recipe. Pair it with pose_is_identity() when that matters.
    bool view_matches_pose(float tolerance = 1e-3f) const;

    // The viewport transform this record implies: clip -> pixels, y flipped. The engine builds this
    // from the viewport rect and composes it with the view-projection to get world_to_screen.
    //
    //   [ halfW    0      0   centreX ]
    //   [ 0     -halfH    0   centreY ]
    //   [ 0        0      1   0       ]
    //
    // nullopt for a degenerate viewport.
    std::optional<regenny::LTMatrix3x4> viewport_transform() const;

    // Does world_to_screen equal viewport_transform * view_projection?
    //
    // Ties FOUR regions the render thread writes separately -- the viewport rect, the view matrix,
    // the projection and the composed result -- so it is the widest coherence check on this record,
    // and it holds in every pass rather than only a perspective one.
    bool world_to_screen_is_coherent(float tolerance = 1e-3f) const;

    // Project a world-space point to PIXELS. Returns nullopt when the matrix is unusable or the point
    // lands on or behind the camera plane (w <= 0), which is the case a caller must not paper over --
    // a point behind the viewer otherwise projects to a plausible-looking pixel on screen.
    //
    // This is the reason the matrix is worth mapping: an overlay, a reticle, a debug marker or a
    // stereo-disparity check all need world -> pixel, and doing it through the engine's own composed
    // matrix means agreeing with what the engine drew rather than approximating it.
    struct ScreenPoint {
        float x{};
        float y{};
        float depth{};  // the w that was divided out; larger is further away
    };
    std::optional<ScreenPoint> project_point(float world_x, float world_y, float world_z) const;

    // Is the pose the identity -- position at the origin and rotation (0, 0, 0, 1)? True of the
    // engine's screen pass, whose camera is not a camera at all. A consumer distinguishing "the
    // engine gave me a real viewpoint" from "this is the 2D overlay pass" wants this rather than
    // the mode field, since it answers from the data.
    bool pose_is_identity(float tolerance = 1e-4f) const;

    // ---- WHAT KIND OF PROJECTION IS THIS ------------------------------------------
    //
    // Both tests key on m[3][2], the coefficient that makes the output w depend on z. That is
    // the SCALE-INVARIANT discriminator, which matters because homogeneous matrices are
    // scale-equivalent and this engine ships two affine variants with different overall scales:
    //
    //     builder                              m[3][2]  m[3][3]  m[0][0]
    //     LTMatrix_BuildPerspectiveProjection     1        0      1/half_x
    //     LTMatrix_BuildAffineProjection          0       f-n     (f-n)/half_x
    //     the screen pass's stored matrix         0        1      2/width
    //
    // An earlier version of this class tested m[3][3] == 1, which quietly meant "the screen
    // pass's normalisation" and mislabelled the mode-1 matrix.
    bool is_perspective_projection() const;

    // Largest absolute coefficient of the W ROW m[3][0..3]; 0 when any matrix entry is
    // non-finite. Exposed because it is the yardstick the classifiers use, and a consumer writing
    // its own relative comparison wants this one rather than an absolute epsilon -- or the whole
    // matrix's maximum, which couples the comparison to the projection's dynamic range and can
    // read a narrow-FOV perspective matrix as affine.
    float projection_w_row_scale() const;
    bool is_affine_projection() const;

    // The screen pass's specific NORMALISED orthographic shape: affine AND m[3][3] == 1, which
    // is what makes the 2/width identity below exact. Narrower than "not perspective".
    bool is_normalized_orthographic_projection() const;

    // The verified invariant: for an orthographic pass, [0][0] == 2/width and [1][1] == -2/height.
    //
    // Worth calling on any snapshot you intend to trust. It ties the viewport ints to the
    // projection floats, which the render thread writes at different moments, so it detects a
    // tear BETWEEN THOSE TWO REGIONS. It is a partial check, not an atomicity proof: a write
    // that tore only the view, derived or trailing matrices would still satisfy it. Returns
    // false for anything but a normalised orthographic matrix, where the identity does not
    // apply; check is_normalized_orthographic_projection() first.
    bool projection_matches_viewport_ortho(float tolerance = 0.02f) const;
};

class SceneCamera {
public:
    // ---- BUILDING A PROJECTION IN THE ENGINE'S OWN LAYOUT ---------------------------
    //
    // Transcribed from LTMatrix_BuildPerspectiveProjection (0x610560) and
    // LTMatrix_BuildAffineProjection (0x6105DA), including their row-major order and the
    // translation-in-column-3 convention that a live read of the screen pass confirmed.
    //
    // A consumer that wants to REPLACE a projection -- a per-eye frustum, a changed field of
    // view -- needs one built the way the engine builds them, not the way some other convention
    // would. That is what these are for. They also make the classifiers and fov_*_radians()
    // exercisable on a known matrix, which matters because the engine leaves this record in its
    // affine screen pass between frames: the perspective path is not reachable by sampling.
    //
    //   perspective:  [ 1/hx  0     0   0    ]      affine (k = far - near):
    //                 [ 0     1/hy  0   0    ]        [ k/hx  0     0   0     ]
    //                 [ 0     0     1  -near ]        [ 0     k/hy  0   0     ]
    //                 [ 0     0     1   0    ]        [ 0     0     1  -near  ]
    //                                                 [ 0     0     0   k     ]
    // BOTH RETURN nullopt RATHER THAN A MALFORMED MATRIX. A zero or non-finite half-extent would
    // otherwise produce a matrix with a zero scale coefficient that still classifies as
    // perspective -- usable-looking and wrong, which is the worst thing a builder can hand back.
    // Requires finite, strictly positive half-extents, a finite near plane, and for the affine
    // form a non-zero finite depth span -- AND checks the computed scale coefficients, because a
    // finite but tiny extent overflows its reciprocal to infinity. nullopt therefore means "no
    // usable matrix", not merely "bad-looking input".
    static std::optional<std::array<float, 16>> make_perspective_projection(float half_x,
                                                                           float half_y,
                                                                           float near_z);
    static std::optional<std::array<float, 16>> make_affine_projection(float half_x, float half_y,
                                                                      float near_z, float far_z);

    // ---- COMPOSING MATRICES THE WAY THE ENGINE DOES ---------------------------------
    //
    // Transcribed from LTMatrix_Mul4x4ByAffine (0x610193), which is how the camera builder composes
    // both the shear and the view matrix. `b` is AFFINE: 12 floats, its fourth row taken as
    // (0, 0, 0, 1) -- which is not an assumption but the storage, since the engine keeps the view
    // matrix and the shear as 3x4.
    //
    // out = a * b, ROW-MAJOR with translation in column 3, so points are COLUMN vectors and the
    // engine's own composition reads view_projection = projection * view. A consumer building a
    // per-eye matrix must use this order; the transpose silently produces a plausible-looking wrong
    // result, which is exactly the class of bug worth spending a helper to avoid.
    static std::array<float, 16> multiply_by_affine(const std::array<float, 16>& a,
                                                   const std::array<float, 12>& b);

    // The engine's own composition, named for what it is. Equivalent to multiply_by_affine with the
    // view matrix as the affine operand, and the order the builder uses at record+0xB8.
    static std::array<float, 16> compose_view_projection(const std::array<float, 16>& projection,
                                                        const regenny::LTMatrix3x4& view);

    // The affine identity, for callers wanting an unmodified composition or a starting point.
    static std::array<float, 12> affine_identity();

    // ---- THE VIEW MATRIX, AND WHERE IT COMES FROM ------------------------------------
    //
    // The builder produces it as matrix_from(inverse(pose)) -- LTTransform_CopyInverted (0x41DB87)
    // called with the camera pose, then LTTransform_ToMatrix3x4 (0x62E31E). That construction is
    // what settles the pose at +0x14 as the camera's WORLD transform rather than something already
    // view-relative.
    //
    // A consumer overriding the camera -- the whole point of a stereo path -- has to produce the
    // matching view matrix, so these mirror the engine's steps rather than leaving each caller to
    // rediscover the convention.

    // LTNodeTransform -> 3x4: rotation via sdk::rotation_matrix (which applies the engine's own
    // 2/|q|^2 scale) with the position written into COLUMN 3. nullopt when the quaternion describes
    // no rotation or the position is not finite.
    static std::optional<regenny::LTMatrix3x4> transform_to_matrix(
        const regenny::LTNodeTransform& transform);

    // The rigid inverse: conjugated rotation, and the position negated after rotating by it -- the
    // same two steps the engine takes (LTRotation_Conjugate, then LTRotation_RotateVector negated).
    //
    // ON NON-UNIT INPUT, precisely. The returned rotation is the CONJUGATE, which as a quaternion is
    // the true inverse only when |q| = 1. But every consumer here goes on to build a matrix, and
    // R(conj q) equals R(q) transposed for ANY nonzero norm because sdk::rotation_matrix divides by
    // |q|^2 -- so transform_to_matrix(invert_transform(t)) is the correct inverse matrix regardless.
    // What is NOT preserved for non-unit input is the scaling: composing the two QUATERNIONS gives
    // |q|^2 rather than 1. If you intend to keep composing quaternions, normalise first;
    // pose_rotation_is_unit() reports whether the engine's own pose already is.
    //
    // nullopt when the rotated position overflows, not merely when the input looks wrong: the three
    // dot products can produce an infinity from finite inputs near the top of float range, and this
    // optional is meant to guarantee a usable transform.
    static std::optional<regenny::LTNodeTransform> invert_transform(
        const regenny::LTNodeTransform& transform);

    // matrix_from(inverse(pose)), i.e. the view matrix for a given camera pose.
    static std::optional<regenny::LTMatrix3x4> view_matrix_from_pose(
        const regenny::LTNodeTransform& pose);

    // A 3x4 widened to 4x4 with the implicit (0,0,0,1) row. The engine stores its view matrices and
    // shears as 3x4 but composes into 4x4, so a consumer doing its own composition needs this.
    static std::array<float, 16> promote_affine(const regenny::LTMatrix3x4& affine);

    // ARE THESE TWO 3x4 MATRICES INVERSES? Composes them and compares against the identity.
    //
    // The general form takes BOTH matrices, so a consumer can validate a view matrix it did not build
    // here -- one read from the record, one from its own maths, or one it is about to install. A
    // wrong conjugate sign, a transposed rotation or a mis-signed translation each yield a matrix
    // that looks entirely plausible and renders wrongly; this is what catches them.
    //
    // TOLERANCES ARE SEPARATE AND SIZED FROM MEASUREMENT. The rotation block sits near absolute
    // identity; the translation column is a cancellation back to zero whose residual can scale with
    // distance from the origin. Measured on this build: rotation 1.2e-07, about one FLT_EPSILON, and
    // translation exactly 0 for a pose 98000 units out. The defaults leave orders of margin over that
    // without repeating the earlier mistake of scaling by the ROTATION tolerance, which permitted 196
    // units of translation error at those coordinates.
    // Returns false for a non-finite or negative tolerance, and for an allowance that overflows:
    // a NaN tolerance would otherwise accept ANY pair, since every comparison against NaN is false.
    static bool affines_are_inverse(const regenny::LTMatrix3x4& forward,
                                    const regenny::LTMatrix3x4& inverse,
                                    float rotation_tolerance = 1e-4f,
                                    float translation_base = 1e-3f,
                                    float translation_per_unit = 4e-7f);

    // Convenience for the common case: does OUR view_matrix_from_pose(pose) invert pose? A self-check
    // after building a custom pose. Use affines_are_inverse for a matrix from anywhere else.
    static bool view_inverts_pose(const regenny::LTNodeTransform& pose);

    // The worst absolute deviation from the identity in that composition, split so a caller sees
    // WHICH part drifted. nullopt when either matrix could not be built. Exposed because sizing a
    // tolerance from a guess is exactly how the first version allowed 196 units of error.
    struct RoundTripError {
        float rotation{};
        float translation{};
    };
    static std::optional<RoundTripError> view_inverse_round_trip_error(
        const regenny::LTNodeTransform& pose);

    // Address of the record (g_SceneRenderer+8), or 0 when the exe is not mapped.
    static uintptr_t record_address();

    // ONE guarded copy of the whole record, then unpacked. nullopt when the exe is not mapped
    // or the copy faulted. See the note above: this bounds tearing, it does not prevent it.
    static std::optional<SceneCameraSnapshot> snapshot();
};

}  // namespace sdk

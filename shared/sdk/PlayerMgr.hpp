#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

//
// THE GAME'S OWN VIEW OF THE PLAYER -- gameclient.dll's player manager, which is a different thing from the
// engine's object the shell hands out, and for a VR mod the more interesting of the two.
//
// WHY THIS EXISTS BESIDE CClientShell::local_player. The engine keeps the player as an LTObject with position
// at +0x14 and rotation at +0x20. The GAME keeps its own pose in a holder hanging off its player object, at
// +300 and +324, and that holder is also what owns the two engine objects that matter:
//
//     holder +188  ->  the CAMERA OBJECT (engine LTObject, OT_NORMAL) -- see below
//     holder +232      position        LTVector      <- the APPLIED pose; see below
//     holder +244      rotation        LTRotation
//     holder +300      position        LTVector      <- a DIFFERENT generation
//     holder +324      rotation        LTRotation, unit quaternion
//     holder +600  ->  the PLAYER MODEL (engine LTObject, OT_MODEL)
//
// So "the player's position" has two answers and a caller must say which it means. This class answers with
// the game's, and hands over both engine objects so a caller can ask the engine's too.
//
// THE CAMERA IS NOT A MULTIPLE-INHERITANCE OBJECT, and an earlier version of this comment said it was, on the
// evidence of vtable pointers at +0x00, +0x10 and +0x24. Its constructor shows what those are: a repeating
// twenty-byte pattern of {vtable, prev = self, next = self, 0, 0} at +16, +36, +56, +76, +96, +116, +136 and
// +156 -- EIGHT EMBEDDED SUB-OBJECTS, each an intrusive list link with its own vtable. Only the pointer at
// +0x00 is the camera's own.
//
// Live, all eight read Linked with eight DISTINCT vtables, so they are event sinks the camera has registered
// into eight other subsystems' lists. Three further links at +420, +468 and +516 read Empty and share ONE
// vtable: those are lists the camera owns and nothing has been added to. Nothing but sdk::mem::classify_link
// separates a registered node from an owned head, since the constructor self-links both.
//
// THE MODEL IS *NOT* THE OBJECT THE SHELL NAMES, and getting that wrong is instructive. It reads back as an
// OT_MODEL with dims (40, 95, 40) whose asset is char\player\player\fp_playerm05.mdl -- exactly the pair
// recorded for CClientShell's player slot -- at exactly the same world position. On that evidence an earlier
// pass called them one object "proven by two independent routes". Pointer equality says otherwise:
//
//     holder + 600            handle 0xFFFF   flags 0x00020021   client-created, no server counterpart
//     CClientShell::local_player   handle 0x1CE2   flags 0x000000B9   server-backed
//
// THERE ARE TWO CO-LOCATED PLAYER MODELS, and asset, dims and position are precisely what a duplicate
// shares. The engine's own discriminator is the handle: CLTClient_IsServerObject is nothing but
// `handle != 0xFFFF`. See is_server_backed below.
//
// WHY A CONSUMER MUST CARE: moving or hiding the client-only model is a local change, while the
// server-backed one participates in replication. A VR mod that repoints "the player model" without checking
// which it holds will get one of those two behaviours by accident.
//
// THE OBJECT AT +188 IS THE PLAYER CAMERA'S ENGINE OBJECT, and that is now supported by a code path rather
// than by co-location. The holder's own vtable slot 1 is the class initialiser, and what it initialises is the
// entire camera tuning surface: CameraClipDist, CameraSwayXFreq/YFreq/XSpeed/YSpeed, FovY,
// FovAspectRatioScale, ChaseCam*, CamDamage*, HeadBob*, CameraLeashLen. The reference gives that job to
// CPlayerCamera::Init, and five of its defaults are bit-equal to FEAR 2's live values (CameraClipDist 30,
// ChaseCamDistBack 180, CameraSwayYFreq 5, CameraSwayXFreq 13, CameraLeashLen 30) while three were retuned
// (FovY 70 -> 65, CameraSwayXSpeed 12 -> 3, CameraSwayYSpeed 1.5 -> 1). Names alone would be weak evidence;
// names plus five exact defaults are not, and the three differences are what a sequel's retuning looks like.
//
// The reference's CPlayerCamera owns exactly one engine object, m_hCamera, which it drives with SetObjectPos
// and hands to RenderCamera. That is this object. Its measured properties fit:
//   * engine class vtbl_LTObject_OT_NORMAL -- the plainest object type there is
//   * dims (0, 0, 0) and flags 0x00000000 -- it CANNOT render and CANNOT collide, so carrying a transform is
//     the only thing it does
//   * handle 0xFFFF, i.e. client-created with no server counterpart
//   * sits 70.95 units ABOVE the model's origin (full delta -8.911, 70.948, -0.177) -- eye height
//   * its rotation is BIT-IDENTICAL to the holder's, while its position differs in the low bits: the rotation
//     is copied wholesale, the position is maintained by a separate path or lags a frame
//
// IT IS OT_NORMAL, NOT OT_CAMERA, and that resolves an older puzzle: this project measured all 474 OT_CAMERA
// objects in a live level as static furniture with none tracking the player. None does, because the player
// camera is not one of them.
//
// WHAT IS STILL NOT PROVEN is that the engine RENDERS from this object's transform on the frame path. The
// scene camera record holds the identity whenever it is read from outside a render pass, so that link needs a
// hook to observe. Everything above is about who owns and drives the object.
//
// WHICH SLOT IS "THE PLAYER". The manager holds four slots and the game's own console commands take the FIRST
// OCCUPIED one, walking past nulls rather than assuming slot 0. Live, slot 0 is filled and 1..3 are null --
// single-player, the same four-slot shape the engine's shell uses. This class follows the game: local_player()
// means first occupied, and slot indices are available for a caller that needs a specific one.
//

namespace sdk {

class PlayerMgr {
public:
    // Manager layout, from ConsoleCmd_GetPlayerPos and its siblings.
    static constexpr size_t kSlotCount = 4;
    static constexpr uintptr_t kSlotsBegin = 80;
    static constexpr uintptr_t kSlotsEnd = 96;  // begin + 4 pointers

    //
    // THERE ARE TWO POSITION GENERATIONS, and only one of them is what the engine carries.
    //
    // Measured over repeated samples: the position at +232 is BIT-EQUAL to the camera object's own
    // LTObject.position, every time. The one at +300 is consistently a few thousandths away (2183.0381 vs
    // 2183.0332 on the sample this was established from) and equals a third copy at +540. The rotation is the
    // same value in both places, so only position is double-buffered.
    //
    // So +232 is the pose that has been APPLIED to the engine object, and +300 is the camera's other
    // generation -- logical, pre-offset, or one frame behind; which of those is not established. A consumer
    // that wants the pose the engine is actually using wants applied_pose.
    //
    // The reference declares both m_vPos and m_vActivePos on this class, which is consistent with two
    // generations existing, but WHICH member is which is NOT pinned here -- the names below describe what was
    // measured (one matches the engine object, one does not) rather than asserting the reference's mapping.
    //
    // Player-object and holder layout.
    static constexpr uintptr_t kHolder = 252;
    static constexpr uintptr_t kCameraObject = 188;
    static constexpr uintptr_t kAppliedPosition = 232;
    static constexpr uintptr_t kAppliedRotation = 244;
    static constexpr uintptr_t kPosition = 300;
    static constexpr uintptr_t kRotation = 324;
    static constexpr uintptr_t kModelObject = 600;

    struct Pose {
        std::array<float, 3> position{};
        std::array<float, 4> rotation{};  // x, y, z, w -- w LAST, matching regenny::LTRotation

        // Is the rotation a real orientation? A unit-length quaternion is the single strongest cheap check
        // that a rotation offset is right: a wrong one does not produce norm 1. Exposed because a consumer
        // holding a Pose it cached, or one it built itself, wants the same test.
        bool rotation_is_unit(float tolerance = 0.001f) const;
    };

    struct Player {
        uintptr_t object{};       // the game-side player object -- the slot's contents
        uintptr_t holder{};       // object + 252
        // The camera's other generation, from +300. Kept because it is what two earlier passes recorded, and
        // because the pair being different is itself the finding.
        Pose pose{};

        // THE POSE THE ENGINE CARRIES, from +232 -- bit-equal to the camera object's LTObject fields. This is
        // the one to read when the question is "where is the view".
        Pose applied_pose{};
        uintptr_t camera_object{};  // the camera's engine LTObject, transform-only; 0 when absent

        // The CLIENT-ONLY player model -- not the object CClientShell::local_player returns. See the note
        // above: two co-located models exist and only the handle tells them apart.
        uintptr_t model_object{}; // engine OT_MODEL; 0 when absent
    };

    // The engine's own server/client discriminator, which is literally `handle != 0xFFFF` --
    // CLTClient_IsServerObject's entire body. Exposed because it is the ONLY field that separates the two
    // player models, and a consumer holding an LTObject from anywhere needs the same test.
    //
    // nullopt when the handle cannot be read; false means client-created with no server counterpart.
    static constexpr uint16_t kNoServerHandle = 0xFFFF;
    static std::optional<bool> is_server_backed(uintptr_t object);

    //
    // THE CAMERA'S EVENT DELEGATES -- eight of them, and the layout comes from their own handlers.
    //
    // Every handler in every sink vtable begins `mov ecx, [ecx+0Ch]`: the node carries a pointer to its OWNER
    // and the handler dereferences it to reach the camera. So a node is twenty bytes:
    //
    //     +0x00  vtable          one per sink -- eight distinct ones
    //     +0x04  link.prev       self-pointing when detached
    //     +0x08  link.next
    //     +0x0C  owner           the camera; verified equal on 8 of 8
    //     +0x10  subject         what it is attached to, and non-null exactly when registered
    //
    // The unregister method (shared by all eight, gameclient 0x100EE9F0) is what pins the last two: it guards
    // on the subject being non-null, unlinks the node, SELF-LINKS IT AGAIN, then clears both the subject and
    // the owner. So sdk::mem::LinkState::Empty meaning "not registered" is the engine's own construction
    // rather than an inference from what an empty list looks like.
    //
    // WHY A CONSUMER WANTS THIS. Six of the eight subjects point into the player OBJECT's region and two
    // elsewhere, so the set says what the camera is currently listening to -- and a camera whose delegates have
    // gone Empty has been torn down, which is exactly the state in which its cached pose must not be trusted.
    // That is a cheaper liveness test than re-walking the manager.
    static constexpr uintptr_t kDelegateVtable = 0x00;
    static constexpr uintptr_t kDelegateLink = 0x04;
    static constexpr uintptr_t kDelegateOwner = 0x0C;
    static constexpr uintptr_t kDelegateSubject = 0x10;
    static constexpr size_t kDelegateSize = 20;

    struct Delegate {
        uintptr_t address{};   // the node inside the camera
        uintptr_t vtable{};
        uintptr_t owner{};     // should equal the camera
        uintptr_t subject{};   // what it listens to; 0 when detached
        bool registered{};     // link is threaded AND subject is non-null
    };

    // The eight delegates of the camera for `index`, in offset order. Empty when the player or camera cannot
    // be read. A node that does not read is skipped rather than reported with zeroed fields.
    static std::vector<Delegate> camera_delegates(unsigned index);

    // Do all eight name the camera as their owner? The internal-consistency check that establishes the owner
    // field, and a liveness test for a consumer holding a cached camera.
    static std::optional<bool> camera_delegates_consistent(unsigned index);

    // ---- THE PHYSICS TARGET, AND THE CLASS IDENTITY THAT LICENSES IT ----------------------
    //
    // Everything above is GAME-side: gameclient.dll's own player class. Its per-frame movement update is driven
    // through a CONTROLLER held at player[59], and the controller points back at its owner at +0x04. That
    // back-pointer is what licenses the offsets below: it proves the object this class hands out is the same
    // class the update code operates on, rather than a plausible pointer the offsets were guessed against.
    //
    //     controller = *(player + 236)          and     *(controller + 4) == player
    //
    // Live that holds for the occupied slot. movement_controller_owner_agrees() is the check, and it is worth
    // making before trusting any offset read out of the update path.
    static constexpr uintptr_t kControllerField = 236;       // player[59]
    static constexpr uintptr_t kControllerOwnerField = 0x04;  // back-pointer within the controller
    static constexpr uintptr_t kEngineHolderField = 260;      // player[65]
    static constexpr uintptr_t kEngineObjectField = 320;      // within that holder

    // The player's movement controller. nullopt for an empty slot or a faulted read.
    static std::optional<uintptr_t> movement_controller(unsigned index);

    // Does the controller point back at this player? The invariant above, and a liveness check on the pair.
    static std::optional<bool> movement_controller_owner_agrees(unsigned index);

    // THE LTObject THE GAME PASSES TO ILTPhysics for this player, reached exactly as the update path reaches it:
    // *(*(player + 260) + 320).
    //
    // IT IS NOT CClientShell's LOCAL-PLAYER OBJECT, and that surprised this project. Measured live, side by side:
    //
    //                       this route          CClientShell::local_player(0)
    //     kind              1                   1
    //     dims.y            95.0                95.0
    //     handle            0xFFFF  (none)      7394
    //     slot_index        0xFFFFFFFF (none)   3458
    //
    // Two player-shaped objects, and THIS ONE IS UNREGISTERED -- it carries neither an engine handle nor a slot
    // index, putting it among the 335 of 3583 objects sdk::object_info records that way. So it cannot be used
    // with any ILT* entry point that takes an HOBJECT; it can only be used as a raw LTObject*, which is exactly
    // how the update path uses it (ILTPhysics::SetVelocity takes the pointer and writes fields directly).
    //
    // IT IS THE MODEL OBJECT. An earlier pass left this as "an unregistered object of unknown role"; it is this
    // class's model_object, reached through the OTHER holder -- *(player + 252) + 600 rather than
    // *(player + 260) + 320. Two routes sharing no offsets, confirmed live by engine_object_is_model_object().
    // That also explains the shape: it is player-shaped because it IS the player's model.
    //
    // WHICH ONE A CONSUMER WANTS depends on the question. To affect what the game's own physics calls affect, or
    // to read the fields the update path writes, use this. To go through a handle-taking engine API, use the
    // shell's -- and see engine_object_is_registered() before assuming either. engine_objects() returns all
    // three side by side with their roles, which is the accessor to prefer.
    static std::optional<uintptr_t> engine_object(unsigned index);

    // ---- THE GAME-SIDE MOVEMENT STATE, which is the velocity that is actually real ---------
    //
    // sdk::Physics documents that the engine's velocity for a player is forced to zero every frame and tells a
    // consumer to read the game-side state instead. THIS IS THAT STATE. It lives on the movement controller and
    // the game recomputes it every frame in PlayerMovement_CommitPositionAndVelocity:
    //
    //     pos      = ILTClient::GetObjectPos(engine_object)      -- the engine's authority on where it is
    //     delta    = pos - controller[+1400]                     -- movement since the last commit
    //     velocity = delta / dt                                  -- LTVector_DivideByScalar, literally * (1/dt)
    //     controller[+1412] = velocity
    //     controller[+1400] = pos                                -- cache for next frame
    //     controller[+352]  = 0                                  -- accumulator, consumed and cleared
    //
    // SO THE VELOCITY IS DERIVED FROM POSITION, not integrated into it. That is worth knowing before trusting
    // it: it is exact for what the player DID last frame and carries one frame of lag, and when dt is zero the
    // game writes a zero vector rather than dividing.
    //
    // THE CACHED POSITION IS THE CHECK. It is written from the engine object's position every frame, so it must
    // equal LTObject.position BIT-FOR-BIT -- and live it does, to 0.000000. That is what establishes these
    // offsets: a wrong +1400 would land on some other triple, and the controller carries exactly ONE triple
    // within 1.0 of the player's position. See cached_position_matches_engine().
    //
    // FOR VR: speed() is the number a comfort vignette or locomotion blend wants, and it is in world units per
    // second because dt is seconds.
    static constexpr uintptr_t kCachedPositionField = 1400;
    static constexpr uintptr_t kVelocityField = 1412;
    static constexpr uintptr_t kExternalDeltaField = 352;

    struct MovementState {
        std::array<float, 3> cached_position{};  // +1400, last frame's engine position
        std::array<float, 3> velocity{};         // +1412, world units per second
        // +352. Subtracted from the frame's delta before the divide and then cleared, so displacement that is
        // not the player moving under its own power does not show up as velocity. Reads zero after a commit.
        std::array<float, 3> external_delta{};
    };

    // ---- THE GAME-SIDE FIELD OF VIEW, AND THE CINEMATIC CAMERA THAT OVERRIDES IT ------------
    //
    // PlayerCamera_UpdateCinematicCamera walks a vector of cinematic camera descriptors on the player manager and,
    // when one is active, writes a PAIR OF FLOATS onto the pose holder from that descriptor's FOV in DEGREES times
    // pi/180 and an aspect ratio. Those two floats are the game's own field of view.
    //
    // +296 IS THE VERTICAL FOV IN RADIANS, on three pieces of evidence -- none of which is a live cross-check
    // against the renderer, and it is worth being exact about that:
    //
    //   1. THE PRODUCER converts DEGREES to radians, multiplying the descriptor's FOV by 0.01745329 before
    //      storing the pair. So one of the two fields is an angle in radians by construction.
    //   2. IT READS 1.1344640, which is 65.0000 degrees to four decimals. A wrong offset landing on exactly a
    //      round degree value is unlikely; this is the strongest evidence available without frames.
    //   3. It sits in the plausible band for a vertical FOV, which +292 does not.
    //
    // THE CROSS-CHECK EXISTS BUT COULD NOT BE RUN. sdk::SceneCamera's projection-derived fov_y_radians() is gated
    // on the current pass being perspective, and with the render path frozen the engine's last record is its
    // SCREEN ORTHOGRAPHIC pass -- so it correctly refuses rather than comparing against a stale matrix.
    // fov_y_matches_projection() therefore returns nullopt today and becomes meaningful the moment the game
    // renders; the suite asserts it conditionally so it upgrades itself rather than being deleted.
    //
    // AN EARLIER DRAFT OF THIS COMMENT claimed agreement to 4e-5 with the projection. That number came from a
    // SYNTHETIC round-trip self-test in the shader-parameter suite -- a matrix this SDK built from a chosen FOV
    // and read back -- not from the live camera. Comparing a game field against our own test fixture and calling
    // it independent corroboration is precisely the mistake this file keeps recording.
    //
    // +292 IS THE HORIZONTAL FOV IN RADIANS, and this was settled by reading the producer rather than by measuring
    // anything. sub_100DFFD0 computes both fields in one place:
    //
    //     fov_y = clamp(input_radians, 0, kFovClampRadians)
    //     fov_x = clamp(2 * atan(tan(fov_y / 2) * aspect) * scale, 0, kFovClampRadians)
    //     out[0] = fov_x    out[1] = fov_y
    //
    // An earlier draft called +292 "not established" and reasoned that 2.31 rad = 132 degrees was implausible
    // beside a 65-degree vertical. The magnitude WAS the wrong thing to reason from: the identity is unambiguous in
    // the code, and what the magnitude actually tells us is the effective ratio the engine used.
    //
    // AND THE RATIO IS A RECOGNISABLE NUMBER, which narrows what remains open. tan(fov_x/2) / tan(fov_y/2) recovers
    // 3.5556 live -- that is 32/9, EXACTLY TWICE 16/9, not the 1.7778 a 16:9 viewport alone would give.
    //
    // The producer derives its aspect from a rect as (b + d - f - h) / (a + c - e - g) over integer fields, and
    // separately multiplies the half-angle by a `scale` argument. A factor of exactly two therefore points at a
    // doubled-width or halved-height rect rather than at an arbitrary scale -- but WHICH, and which object supplies
    // the rect, is not established: the caller passes it in ECX and the decompiler does not show it.
    //
    // aspect_ratio() returns the recovered ratio and names it for what it is. A VR consumer should use it rather
    // than assuming the display aspect, precisely because the engine's effective value is not the display's.
    //
    // THE CINEMATIC FLAG MATTERS TO A VR CONSUMER independently of the FOV: while a cinematic camera is driving
    // the view, overriding the pose fights the script. cinematic_active() is the cheap test, and the descriptor
    // count says how many the level registered at all.
    static constexpr uintptr_t kCameraFovPair = 292;      // +292 unknown, +296 vertical FOV in radians
    static constexpr uintptr_t kCinematicActiveFlag = 1006;
    static constexpr uintptr_t kSavedNearZ = 6336;
    static constexpr uintptr_t kCinematicVectorBegin = 4660;  // on the MANAGER, not the holder
    static constexpr uintptr_t kCinematicVectorEnd = 4664;

    // The engine clamps both angles to this, which is 178.0 degrees -- so a value at the clamp means the setting
    // was out of range rather than that the view is that wide.
    static constexpr float kFovClampRadians = 3.1066861f;

    struct CameraFov {
        float fov_x{};  // +292, horizontal FOV in radians
        float fov_y{};  // +296, vertical FOV in radians
    };

    static std::optional<CameraFov> camera_fov(unsigned index);

    // Does the holder's vertical FOV still match the one derived from the renderer's projection? The cross-check
    // that establishes the field, and a staleness test for a consumer that cached either.
    static std::optional<bool> fov_y_matches_projection(unsigned index, float tolerance = 0.01f);

    // The same question for the horizontal FOV against the projection's own. Kept because the two are computed by
    // different code from different inputs -- the game from a setting and an aspect, the renderer's from its
    // projection matrix -- so agreement is a real cross-check once a perspective pass exists to read.
    static std::optional<bool> fov_x_matches_projection(unsigned index, float tolerance = 0.01f);

    // The ratio the pair implies: tan(fov_x/2) / tan(fov_y/2). That is what the engine effectively applied, which
    // is NOT necessarily the display aspect -- the producer also scales the half-angle. A VR consumer building
    // per-eye projections wants this number rather than assuming 16:9.
    //
    // nullopt when the pair cannot be read or either angle is outside (0, kFovClampRadians).
    static std::optional<float> aspect_ratio(unsigned index);

    // Is a cinematic camera currently driving the view? While it is, the pose comes from the descriptor's own
    // object and NearZ is overridden, with the previous value parked at kSavedNearZ until the cinematic ends.
    static std::optional<bool> cinematic_active(unsigned index);

    // The NearZ the cinematic path saved so it can restore it. Only meaningful while cinematic_active().
    static std::optional<float> saved_near_z(unsigned index);

    // How many cinematic camera descriptors this level registered, from the manager's vector bounds. nullopt when
    // the manager is absent or the bounds are not a sane pointer pair.
    static std::optional<size_t> cinematic_camera_count();

    // ---- WHERE THE CAMERA POSE COMES FROM: A MODEL SOCKET, PLUS THREE TUNABLE FLOATS -------
    //
    // The base pose is not computed from player state at all -- it is READ OFF THE MODEL. Per frame,
    // PlayerCamera_ComputeBasePoseFromSocket picks a source object, asks ILTModel for a socket BY NAME, takes that
    // socket's transform, and adds a console-configurable offset:
    //
    //     name   = "Camera", or "CameraDEAD" when player[+364] is 2, 3 or 4 (the death states)
    //     source = the model object, or an alternate depending on player[+364] and holder[+688]
    //     pose   = ILTModel socket transform of that name
    //     pose.position += rotate(CameraAttachedOffsetX, ...OffsetY, ...OffsetZ)
    //
    // THE OFFSET IS THREE CACHED CONSOLE VARIABLES, which makes it the cheapest useful camera control this project
    // has found: a VR consumer can move the eye point with three float stores, no hook and no engine call, through
    // the cache pointers sdk::Engine already discovers. That is what camera_attached_offset() exposes.
    //
    // AND THE SOCKET NAME IS CHECKABLE. "Camera" is a string in gameclient's code; the socket table belongs to the
    // model ASSET. camera_socket_index() looks it up through sdk::ModelSkeleton, so agreement is two independent
    // artefacts -- a code literal and an asset's own table -- naming the same thing.
    static constexpr const char* kCameraSocketName = "Camera";
    static constexpr const char* kCameraDeadSocketName = "CameraDEAD";

    // The three CameraAttachedOffset floats, in X, Y, Z order. nullopt when any of them is not in the cached set,
    // which is a real state before the camera has initialised.
    static std::optional<std::array<float, 3>> camera_attached_offset();

    // Store all three. False when any is missing or a write faulted; a partial write is still possible, so a
    // caller that cares should read back.
    static bool set_camera_attached_offset(const std::array<float, 3>& offset);

    // The index of the socket the camera pose is read from, on this player's model. nullopt when the model or the
    // socket cannot be resolved -- and a missing "Camera" socket would mean the name in the code and the asset
    // have diverged, which is worth knowing before trusting the pose path.
    static std::optional<size_t> camera_socket_index(unsigned index, const char* name = kCameraSocketName);

    // ---- HOW THE CAMERA'S ROTATION IS COMPOSED, AND WHERE A VR MOD WOULD CUT IN -------------
    //
    // The camera pose write path is now read. Per frame, on the POSE HOLDER:
    //
    //     base pose  = PlayerCamera_ComputeBasePose(...)        -> holder[+300] position, [+568] rotation
    //     rotation   = LTRotation_Multiply(holder[+552], holder[+324])
    //     position   = PlayerCamera_ApplyCollisionAndLeash(position, rotation)   -- clamps in place
    //     ILTClient::SetObjectPosAndRotation(holder[+188], position, rotation)   -- slot 24
    //
    // SO THE FINAL ORIENTATION IS A PRODUCT OF TWO STORED QUATERNIONS, holder[+552] and holder[+324], and the
    // camera object carries the result. That product is the natural place for a headset rotation to enter: a VR
    // mod either writes one operand or replaces the composed result before it reaches the engine object.
    //
    // THE OUTER OPERAND IS IDENTITY WHEN THE VIEW IS UNPERTURBED, and that is the interesting part. Measured live
    // with the player standing still, holder[+552] is the identity quaternion, so the product equals holder[+324]
    // alone. That makes +552 an ADDITIVE rotation slot -- whatever shakes, sways or leans the camera writes it --
    // and it is exactly where a headset orientation would be composed in.
    //
    // IT ALSO MEANS THE LIVE DATA CANNOT ESTABLISH THE ORDER. With one operand identity, both multiplication
    // orders produce the same result, so camera_rotation_is_composed() passing proves the operands and NOT the
    // order. The order comes from the disassembly -- LTRotation_Multiply(holder+552, out, holder+324), operands
    // pinned by register -- and sdk::multiply_rotations(a, b) is R(a)*R(b), i.e. apply b first. A consumer that
    // gets it backwards produces a rotation that looks plausible and turns the wrong way.
    //
    // The suite asserts the degeneracy CONDITIONALLY: while the outer operand is identity, the reversed product
    // must also match, and if it ever stops being identity the reversed product must stop matching. That way the
    // check discriminates as soon as the data allows and never pretends to in the meantime.
    static constexpr uintptr_t kCameraRotationOuter = 552;  // the LEFT operand
    static constexpr uintptr_t kCameraRotationInner = 324;  // the RIGHT operand -- also kRotation
    static constexpr uintptr_t kCameraBasePosition = 300;   // also kPosition
    static constexpr uintptr_t kCameraBaseRotation = 568;

    struct CameraRotationOperands {
        std::array<float, 4> outer{};    // holder[+552]
        std::array<float, 4> inner{};    // holder[+324]
        std::array<float, 4> composed{}; // multiply_rotations(outer, inner)
        std::array<float, 4> actual{};   // what the camera object carries
    };

    // The two operands, their product, and the camera object's rotation side by side. nullopt when the player,
    // the holder or the camera object cannot be read.
    static std::optional<CameraRotationOperands> camera_rotation_operands(unsigned index);

    // Does the product of the two stored quaternions equal the camera object's rotation? Compared with a
    // tolerance rather than bitwise, because the engine's multiply and this SDK's are separate implementations of
    // the same algebra and need not round identically.
    static std::optional<bool> camera_rotation_is_composed(unsigned index, float tolerance = 0.001f);

    // IS AN ATTACHMENT DRIVING THE VIEW? The outer operand has exactly ONE writer,
    // PlayerCamera_UpdateAttachedRotation, and it either copies an attached object's rotation into it or -- in the
    // ordinary case -- stores IDENTITY. So a non-identity outer operand means something is currently steering the
    // camera: a cinematic, a mounted position, a scripted view.
    //
    // nullopt when the operand cannot be read.
    static std::optional<bool> camera_attachment_driving(unsigned index);

    // DOES WRITING THE OUTER OPERAND ACTUALLY STEER THE VIEW? This is the question a VR mod turns on, and it is
    // answered by measurement rather than by reading the writer.
    //
    // WHAT THE PROBES ACTUALLY SHOW, AND WHAT THEY CANNOT. Measured with the player standing still: a quaternion
    // written to the outer operand survives every sample over ~200ms and the camera object does NOT change; the
    // same holds for the inner operand; AND a value written directly to the camera object's own rotation also
    // survives untouched.
    //
    // ALL THREE SURVIVING MEANS THE RENDER PATH IS NOT RUNNING, not that any field is unread. And that state was
    // ALREADY DOCUMENTED before these probes were written: sdk::ShaderParams::frame_time is the engine's own
    // render-path liveness signal, and its header records the measured disagreement -- the main loop pumping at
    // ~170/s while the engine clock and k_fTime are frozen. Writing these probes without consulting it was the
    // actual mistake; the surprising survival was a rediscovery of a known state.
    //
    // SO THE PROBES NOW CARRY THE GUARD THEMSELVES. Each samples frame_time inside its own window and returns
    // ProbeVerdict::Inconclusive when no frame was rendered, whatever the written value did. A survival count is
    // never handed back as evidence on its own.
    //
    // THAT ALSO VOIDS ANY CAUSAL READING of the equalities this class documents. The applied pose equals the
    // camera object's transform and the composed product equals its rotation, but with the pipeline idle those
    // equalities are a last-synced snapshot: they do not establish which side is upstream. An earlier note here
    // called the outer operand "the natural place to compose a headset orientation" -- that was inferred from the
    // write path's code, and nothing measured so far supports or refutes it.
    //
    // IT WRITES ENGINE STATE briefly and restores it, so it is a verification tool rather than something to call
    // from a shipping mod.
    // A PROBE'S RESULT IS A VERDICT, NOT A COUNT, because a raw survival count invites the wrong reading. This
    // project produced two contradictory conclusions from one such count before a control measurement showed the
    // experiment could not discriminate at all.
    enum class ProbeVerdict {
        // The render path did not advance during the window, so nothing could have overwritten or consumed the
        // written value. Says NOTHING about the field. This is the verdict on an unfocused or paused game.
        Inconclusive,
        // Something overwrote the value while frames were being rendered: live code writes this field.
        Reclaimed,
        // The value survived across rendered frames: nothing writes this field in the observed state.
        Held,
    };

    struct OuterOperandProbe {
        unsigned samples{};          // how many samples were taken
        unsigned survived{};         // how many still held the written value
        bool followed{};             // the camera object's rotation matched probe * inner
        unsigned frames_observed{};  // DISTINCT frame_time values seen; 0 or 1 means the render path was frozen
        ProbeVerdict verdict{ProbeVerdict::Inconclusive};
    };

    // nullopt when the operand cannot be read or the write faulted.
    static std::optional<OuterOperandProbe> probe_outer_operand(unsigned index, unsigned samples = 12);

    // THE GENERAL FORM: does writing a quaternion at `holder_offset` steer the camera object? Answers the only
    // question that matters when choosing where to inject a head orientation, and answers it by doing it.
    //
    //   survived  -- how many samples kept the written value, i.e. whether anything reclaims the field
    //   followed  -- whether the camera object's rotation changed to match, i.e. whether anything READS it
    //
    // The two are independent and both matter. A field that survives but is not followed is dead in this state
    // (nothing reads it); a field that is followed but does not survive needs writing every frame. Measured on
    // the OUTER operand: survives every sample and is NOT followed -- so the composer that reads it is not the
    // one currently driving the view.
    //
    // `expect_composed_with_inner` selects what "followed" compares against: true for an operand that the engine
    // multiplies by holder[+324], false for a field that becomes the rotation outright.
    //
    // WRITES ENGINE STATE briefly and restores it. A verification tool, not a shipping path.
    static std::optional<OuterOperandProbe> probe_holder_quaternion(unsigned index, uintptr_t holder_offset,
                                                                    bool expect_composed_with_inner,
                                                                    unsigned samples = 12);

    // AND THE ONE THAT ACTUALLY MATTERS: write the CAMERA OBJECT's own rotation and see whether it holds. The
    // engine builds the view from this object, so a value that survives here is a value the renderer will use.
    //
    // `followed` is meaningless for this probe (the field IS the target) and is reported as the survival result,
    // so read `survived` alone: equal to `samples` means nothing overwrote it for the whole window.
    //
    // WRITES ENGINE STATE briefly and restores it.
    static std::optional<OuterOperandProbe> probe_camera_object_rotation(unsigned index, unsigned samples = 12);

    // ---- PLATFORM CARRY, WHICH IS WHERE external_delta COMES FROM --------------------------
    //
    // The external_delta field above was documented last pass from the CONSUMER side: the velocity commit
    // subtracts it and clears it, so displacement it accounts for does not read as player velocity. Its PRODUCER
    // is now known -- PlayerMovement_CarryWithPlatform -- and it is platform carry:
    //
    //     if (moved_object == controller[+336]) {          // the object we are riding
    //         delta = new_position - controller[+340];     // how far it moved since last carry
    //         SetObjectPos(model,  GetObjectPos(model)  + delta);   // carry the player
    //         PlayerCamera_TranslateCameraObject(delta);            // AND carry the camera object
    //         controller[+352] += delta;                            // accumulate, for the velocity subtract
    //         controller[+340]  = new_position;                     // remember where it is now
    //     }
    //
    // FOR VR THIS IS LOAD-BEARING TWICE OVER. First, a mod reading speed() gets the player's own motion with
    // platform motion already excluded, which is what a comfort vignette wants. Second, riding a platform
    // TRANSLATES THE CAMERA OBJECT DIRECTLY, bypassing the normal pose path -- so a mod that overrides the camera
    // must add platform motion back itself or the view will lag the floor.
    //
    // NOT THE SAME AS sdk::standing_on(). That reads the ENGINE's standing-on relationship off an LTObject; this
    // is the GAME's own carry tracking on its movement controller, and the two are different fields on different
    // objects with different lifetimes.
    static constexpr uintptr_t kStandingOnField = 336;
    static constexpr uintptr_t kStandingOnPositionField = 340;

    // LAST_POSITION IS ONLY POPULATED WHILE CARRYING, and that is enforced rather than documented. Nothing
    // initialises or clears controller[+340] when carry ends: live, with nothing being ridden, those three dwords
    // read 0x1C559DD4 / 0x1C559DE8 / 0x1C559DFC -- heap pointers spaced 0x14 apart, i.e. leftover data. As floats
    // they are denormals around 7e-22, which print as 0.000 at any sane precision and compare unequal to zero.
    //
    // So a consumer that read the triple unconditionally would get a "position" that looks like the origin in a
    // log and is not a position at all. An optional makes that unrepresentable.
    struct PlatformCarry {
        uintptr_t object{};                              // the LTObject being ridden; 0 when not carrying
        std::optional<std::array<float, 3>> last_position;  // its position at the last carry step; nullopt idle
        bool active{};                                   // object != 0
    };

    // The controller's carry state. nullopt for an empty slot or a faulted read; an inactive state is a normal
    // result, not a failure.
    static std::optional<PlatformCarry> platform_carry(unsigned index);

    // Is the recorded platform position still that object's current position? The carry step writes them equal,
    // so a difference means the platform has moved and the carry has not run yet this frame.
    //
    // nullopt when not carrying -- there is nothing to compare, which is different from "they disagree".
    static std::optional<bool> platform_carry_position_current(unsigned index);

    // The controller's movement state. nullopt for an empty slot or a faulted read.
    static std::optional<MovementState> movement_state(unsigned index);

    // Magnitude of the velocity above, in world units per second -- what a locomotion or comfort layer wants.
    static std::optional<float> speed(unsigned index);

    // Does the cached position still equal the engine object's position, bit for bit? The invariant that
    // establishes these offsets, and a liveness check: it is refreshed every frame, so a stale or wrong
    // controller fails it. nullopt when either side cannot be read.
    static std::optional<bool> cached_position_matches_engine(unsigned index);

    // Does the physics target carry an engine handle and slot index? Live this is FALSE, which is the point: a
    // consumer that assumed otherwise would pass 0xFFFF to a handle-taking API.
    // ---- THE THREE PLAYER-RELATED ENGINE OBJECTS, AND WHICH IS WHICH ----------------------
    //
    // There are THREE distinct LTObjects a consumer may reasonably call "the player", they are all player-shaped,
    // and picking the wrong one produces plausible-looking results rather than an error. They are reached through
    // THREE DIFFERENT ROUTES, two of them through different holders hanging off ADJACENT player fields:
    //
    //     camera   *(player + 252) + 188    carries the APPLIED camera pose; what the view is built from
    //     model    *(player + 252) + 600    the client-only player model -- AND the object handed to ILTPhysics
    //     shell    CClientShell's array     the handle-bearing registered local player (handle 7394 live)
    //
    // THE MODEL OBJECT AND THE PHYSICS TARGET ARE THE SAME OBJECT. Last pass reached the physics target as
    // *(*(player + 260) + 320) and recorded it as an unregistered object of unknown role; it is this class's
    // model_object, reached through a different holder and a different offset. engine_object_is_model_object()
    // is that identity as a runtime check -- two routes sharing no offsets agreeing on one pointer.
    //
    // WHY THIS IS SPELLED OUT: player+252 and player+260 both hold holder-like objects, and applying one's
    // offsets to the other yields ZEROS THAT LOOK LIKE DATA -- reading the pose fields off the +260 holder gives
    // position (0,0,0) and a quaternion of norm 0, which is exactly what a wrong-offset read looks like when it
    // lands on unused space. The offsets are meaningless without the object they belong to.
    struct EngineObjects {
        uintptr_t camera{};  // transform-only; equals the applied pose bit for bit
        uintptr_t model{};   // the ILTPhysics target; unregistered, no engine handle
        uintptr_t shell{};   // registered, handle-bearing; 0 when the shell cannot be read
    };

    // All three at once, so a caller chooses a role rather than an offset. nullopt when the player is absent;
    // individual members may be 0.
    static std::optional<EngineObjects> engine_objects(unsigned index);

    // Is the ILTPhysics target the same object as the player's model? Live: yes. nullopt when either route fails.
    static std::optional<bool> engine_object_is_model_object(unsigned index);

    static std::optional<bool> engine_object_is_registered(unsigned index);

    // Is the physics target the same object CClientShell reports as the local player? Live this is FALSE. Kept as
    // a question a consumer can ask rather than a fact to assume, because the answer is build- and state-
    // dependent and getting it wrong silently targets the wrong object. nullopt when either side is unavailable.
    static std::optional<bool> engine_object_is_shell_object(unsigned index);

    // The manager object, or 0 when gameclient.dll is not mapped. This is a POINTER variable in the DLL's
    // data, so the value is the object rather than the global's address.
    static uintptr_t manager();

    // One slot's contents (a game-side player object), or nullopt for an out-of-range index or an empty
    // slot. Empty and unreadable are deliberately not distinguished: a caller can do nothing different.
    static std::optional<uintptr_t> slot(unsigned index);

    // How many of the four slots are filled. Live this is 1; a split-screen session would report more.
    static unsigned occupied_slot_count();

    // The index the game's own commands would use -- the first non-empty slot, not slot 0.
    static std::optional<unsigned> first_occupied_slot();

    // Everything above for one slot, read in one pass. The pose is validated: a holder whose rotation is not
    // unit-length is refused rather than returned, because that means the offsets are wrong and a caller
    // would otherwise get a plausible-looking transform.
    static std::optional<Player> player(unsigned index);

    // The first occupied slot, which is what "the local player" means to the game.
    static std::optional<Player> local_player();

    //
    // HELPERS, exposed because a consumer needs the same questions answered that this class needed.
    //

    // Read a pose out of a holder address the caller already has. Validated as above. `applied` selects the
    // +232/+244 pair rather than +300/+324.
    static std::optional<Pose> read_pose(uintptr_t holder, bool applied = false);

    // Does the applied pose match the camera object's own transform, field for field? Compared as BITS: the
    // point is whether they are the same stored value, and an epsilon would hide a recomputed one.
    //
    // Live this is true for both position and rotation. A consumer can use it as a staleness check on a
    // camera pose it cached, or as a guard before trusting either source after a level transition.
    static std::optional<bool> applied_pose_matches_camera_object(unsigned index);

    // Is this quaternion unit-length? The free function behind Pose::rotation_is_unit, for a caller holding
    // four floats from somewhere else.
    static bool quaternion_is_unit(const std::array<float, 4>& q, float tolerance = 0.001f);

    // The camera object's offset from the model's origin -- the eye offset, and the number a VR mod needs
    // to reconcile a headset pose with the body. Computed from the two ENGINE objects' positions rather than
    // from the holder, so it measures the same thing the engine would.
    //
    // nullopt when either object is missing. Live it reads (-8.911, 70.948, -0.177).
    static std::optional<std::array<float, 3>> eye_offset(unsigned index);

    // Do the camera object's rotation and the holder's pose agree exactly? Live this is true bit-for-bit, and it is
    // the cheapest way for a consumer to tell whether the anchor it is about to read has been refreshed this
    // frame -- the position lags, the rotation does not.
    static std::optional<bool> camera_rotation_matches_pose(unsigned index);
};

}  // namespace sdk

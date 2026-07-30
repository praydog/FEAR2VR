#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "regenny/Primitives.hpp"
#include "regenny/regenny/DatabaseMgrRecord.hpp"

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
    // ---- THE PLAYER'S SUBSYSTEM TABLE ------------------------------------------------------
    //
    // The player does not hold three camera sub-objects, it holds TWENTY-THREE SUBSYSTEMS in a contiguous
    // table, and CPlayerCameraOwner_ctor -- the constructor of the object this class calls "the player" --
    // builds every one of them. The three that earlier passes mapped are slots in it:
    //
    //     +228 .. +320, four-byte stride, 24 slots of which 23 are class instances
    //
    // (Counted wrong once on the way here: the live scan reported 23 instances and 22 carrying the owner
    // back-pointer, and reading those two numbers as one gave "22 subsystems". The endpoint disagreed with the
    // header, which is the only reason it was caught -- two numbers that differ by one are easy to conflate.)
    //
    //     +236  movement controller   vtable +0x1D87CC  ctor 0x1010B390  >= 2220 bytes  (embedded, see below)
    //     +252  CPlayerCamera         vtable +0x1D5B94  ctor 0x100E3F80  >= 6345 bytes
    //     +260  physics holder        vtable +0x1D582C  ctor 0x100DBDA0  >= 1949 bytes
    //
    // TEN MORE ARE NOW IDENTIFIED, from their METHODS rather than their constructors -- the ctors initialise
    // fields and reference nothing, but the classes' own methods name their console variables:
    //
    //     +228  head bob          'UseHeadBob', 'DefaultHeadBobGroup'
    //     +232  flashlight        'FlashlightBattery', 'FlashlightWaverSpeedScale'
    //     +244  weapon chooser    'KeepCurrentAmmo', 'ChooserAutoSwitchTime', 'ChooserAutoSwitchFreq'
    //     +248  target info       'DrawTarget', 'ClampInfoTarget'
    //     +264  ladder            'Ladder', 'ApproachDistanceBottom/Top', 'LadderBottomDist/TopDist'
    //     +268  weapon perturb    'DebugPerturb', 'DebugPerturbPercent'
    //     +272  damage FX         'EnableDamageFX'
    //     +276  special move      'SpecialMoveMinimumPlayTime'
    //     +280  health / armor    'Armor', 'MaxArmor', 'Health', 'MaxHealth', 'ShowHealthChanges'
    //     +300  input bindings    'SplitScreenShareController', 'DebugCommandSets', 'DebugPlayerInput'
    //
    // A STRING SWEEP ALONE WOULD HAVE NAMED ELEVEN, AND ONE WOULD HAVE BEEN WRONG. Reading a vtable's methods
    // picks up INHERITED ones, whose strings belong to the base class. The discriminator is address locality,
    // calibrated on the three classes established by other means: their vtable methods sit within 0x73B0 of
    // their own constructors, so 0x8000 is a radius measured rather than chosen.
    //
    //   * for all ten above, EVERY function referencing those literals is inside the radius.
    //   * +312 references 'BaseUIInputDeadZone', 'DrawInterface', 'ResourceBundlePC' -- and every one of those
    //     functions is at least 0x89650 away. Those are a BASE class's strings, so the "player UI" reading they
    //     invite is not established, and +312 stays unnamed.
    //
    // +280 was additionally confirmed at runtime: its fields read 100 / 147 / 100 / 150 at +228..+240, two
    // (current, max) pairs, which is the shape a health and armor holder has and no other subsystem showed.
    // Three independent routes for that one, two for the rest.
    //
    // NINE STILL HAVE NO ESTABLISHED ROLE (+240, +256, +284, +292, +296, +304, +308, +316, +320) plus +312.
    // +292 and +304 do reference strings -- 'Shared', 'Mode', 'SupportedPatterns', 'PatternsInUse', 'Pattern'
    // -- which pass the locality test but do not say what the class DOES, so they are recorded and not named.
    //
    // TWO SLOTS IN THE SPAN ARE NOT SUBSYSTEMS, and both were found by the checks below failing:
    //
    //   +288  its first dword is 0x1C48429C -- a HEAP address, so not a vtable. Its fields step by 0x14,
    //         the delegate node size, so it points at a node table rather than being a class instance.
    //   +312  a real class (vtable +0x1D91A4, ctor 0x1011C680) whose +4 is NOT the player. Its +4 and +8 are
    //         0x14 apart and its +16/+20 are a self-linked pair -- +4 is a LINK field for this class.
    //
    // WHICH CORRECTS THE PREVIOUS PASS. That pass found owner-at-+4 on three sub-objects and called it "a
    // convention shared by all three, which makes it a uniform validity test". Over the 23 class instances it
    // holds for 22. It is a strong heuristic with one known exception, not an invariant, and a consumer using
    // it as a guard must accept that the +312 subsystem fails it while being perfectly valid.
    //
    // > The general lesson, since this is the second time in three passes: a property measured on three
    // > samples is a property of three samples. The table was there to be enumerated both times.
    static constexpr uintptr_t kSubsystemTableFirst = 228;
    static constexpr uintptr_t kSubsystemTableLast = 320;
    // THE THREE EMBEDDED SUB-OBJECTS, and an earlier mis-attribution corrected.
    //
    // A previous pass carried a single `kControllerEmbeddedOffset = 0xE88` and asserted that the controller sat
    // there while the camera and physics holder were separate allocations "far outside the player's extent".
    // Live memory says the opposite in both halves. With player = 0x1C636F60:
    //
    //     *(player + 236) controller = player + 0x2760
    //     *(player + 252) camera     = player + 0xE88     <- 0xE88 is the CAMERA's offset, not the controller's
    //     *(player + 260) physics    = player + 0x3020
    //     *(player + 256) aim/zoom   = a separate allocation, nowhere near the player
    //
    // So all THREE are embedded members and the fourth pointer is the one that is not. The old claim also used a
    // 0x10000 window to decide "outside", which both embedded members fall inside anyway.
    //
    // NO IDA EVIDENCE EXISTS for the old constant: neither 0xE88 nor 0x2760 appears as an immediate anywhere in
    // gameclient.dll's .text, so it was never derived from the constructor. These three are LIVE-MEASURED from
    // one player instance, and the identity is what carries the weight -- an exact address match that no
    // unrelated pointer satisfies. A different player class would move them and this check would say so.
    static constexpr uintptr_t kOwnerBackPointer = 0x04;
    static constexpr uintptr_t kControllerVtable = 0x1D87CC;   // gameclient-relative
    static constexpr uintptr_t kPhysicsHolderVtable = 0x1D582C;

    // One slot of the table. `is_class_instance` is the section test on the first dword: false for +288.
    struct Subsystem {
        uintptr_t offset{};           // byte offset within the player
        uintptr_t object{};           // the pointer stored there
        uintptr_t vtable{};           // its first dword, whatever that is
        uintptr_t ctor{};             // gameclient-relative constructor, 0 when unrecorded
        uint32_t size_lower_bound{};  // from the ctor's highest touched offset; 0 when unrecorded
        const char* name{};           // nullptr when the role is not established -- most of them
        bool is_class_instance{};     // first dword lands in a data section
        bool owner_is_player{};       // +4 holds the player: true for 22 of the 23
    };

    // Every slot of the table, read live. Empty when the player cannot be resolved. Always the full span,
    // including the two non-subsystem slots, because a consumer walking the table needs to see them rather
    // than have them silently dropped.
    static std::vector<Subsystem> subsystem_slots(unsigned index);

    // One slot by its byte offset. nullopt when the offset is outside the table or the read faults.
    static std::optional<Subsystem> subsystem_at(unsigned index, uintptr_t offset);

    // How many slots in the span are validated class instances -- 23 of 24 live.
    static std::optional<size_t> subsystem_count(unsigned index);

    // Every subsystem's vtable is distinct: 23 different classes, no aliasing. A repeat would mean the table
    // holds the same subsystem twice, which would invalidate reading them as separate objects.
    static std::optional<bool> subsystem_vtables_distinct(unsigned index);

    // A subsystem by the role name recorded above ("head bob", "flashlight", ...). nullopt when the name is
    // unknown or the player cannot be resolved. This is the accessor a consumer wants: reaching the flashlight
    // by name rather than remembering that it lives at +232.
    static std::optional<Subsystem> subsystem_by_name(unsigned index, std::string_view name);

    // How many slots carry an established role -- THIRTEEN: the three earlier passes established plus the ten
    // this table's methods sweep added. Deliberately not more. Exposed so a consumer can
    // tell "this build maps N of the 23" rather than assuming the table is fully identified.
    static size_t named_subsystem_count();

    // ---- CPlayerStats: THE SUBSYSTEM AT +280, AND ITS FIVE VALUES ---------------------------
    //
    // IDENTIFIED, and by a route that also names the class. CPlayerStats_Init registers five console programs
    // through ILTClient slot 77 -- 'Armor', 'MaxArmor', 'Health', 'MaxHealth', 'Air' -- and the reference
    // source's FEAR/ClientShellDLL/PlayerStats.cpp registers exactly those five names in exactly that order.
    // (Slot 77 was already mapped as CLTClient_RegisterConsoleProgram by an earlier pass, so the call site
    // cross-validates that mapping too.)
    //
    // THE FIELDS COME FROM THE SETTERS' CLAMPING, not from the order of string literals:
    //
    //     +228 int32  health       CPlayerStats_SetHealth clamps it to +236
    //     +232 int32  armor        CPlayerStats_SetArmor clamps it to +240
    //     +236 int32  max_health
    //     +240 int32  max_armor
    //     +244 float  air          a FRACTION in [0,1] -- the console handler passes atof(arg)/100
    //     +284 int32  health lost, accumulated: SetHealth adds (old - new) whenever health drops
    //
    // The reference declares m_nHealth, m_nArmor, m_nMaxHealth, m_nMaxArmor, m_fAirPercent in that same order.
    // Three routes agree: the setters' code, the live values, and the reference.
    //
    // WHICH RETRACTS THE PREVIOUS PASS, and the way it was wrong is the useful part. That pass read the four
    // dwords as two (current, max) pairs -- (100,147) and (100,150) -- and guarded them with `current <= max`.
    // The real pairs are (100,100) and (147,150). BOTH PAIRINGS SATISFY THE GUARD:
    //
    //     wrong:  100 <= 147   and  100 <= 150      passes
    //     right:  100 <= 100   and  147 <= 150      passes
    //
    // An invariant that holds under the wrong structure as well as the right one cannot discriminate between
    // them, so it was never evidence for the pairing it appeared to confirm. The pairing needed the setters.
    static constexpr uintptr_t kStatsHealth = 228;
    static constexpr uintptr_t kStatsArmor = 232;
    static constexpr uintptr_t kStatsMaxHealth = 236;
    static constexpr uintptr_t kStatsMaxArmor = 240;
    static constexpr uintptr_t kStatsAir = 244;
    static constexpr uintptr_t kStatsHealthLost = 284;

    struct PlayerStats {
        int32_t health{};
        int32_t armor{};
        int32_t max_health{};
        int32_t max_armor{};
        float air{};              // [0,1]
        int32_t health_lost{};    // cumulative, never decreases while alive

        // Each current within its OWN limit -- the pairing the setters establish.
        bool limits_respected() const {
            return max_health > 0 && max_armor > 0 && health >= 0 && armor >= 0 && health <= max_health &&
                   armor <= max_armor;
        }

        bool air_in_range() const { return air >= 0.0f && air <= 1.0f; }

        // The check a consumer should make before trusting any of it. air_in_range is the DISCRIMINATING part:
        // a float in [0,1] at +244 is not something the wrong offsets would produce, whereas the ordering
        // check passes under the mispairing too.
        bool consistent() const { return limits_respected() && air_in_range(); }

        bool alive() const { return health > 0; }
    };

    // The five values plus the accumulator, read from CPlayerStats. nullopt when it cannot be resolved or a
    // read faults.
    static std::optional<PlayerStats> player_stats(unsigned index);

    // consistent() over a live read, as one call. nullopt when the stats cannot be read at all.
    static std::optional<bool> player_stats_consistent(unsigned index);

    // ---- CMoveMgr's OWN FIELDS, AND WHY THERE ARE ONLY TWO ---------------------------------
    //
    // CMoveMgr_Init reads sixteen console variables, and it caches fifteen of them in GLOBALS -- the
    // {LTConVar* record, ILTClient* owner} pairs sdk::Engine::cached_console_vars discovers. Only two things
    // land on the instance:
    //
    //     +521   uint8   WaterAffectsSpeed, read from a game database record with the CURRENT field value as
    //                    the default, so Init is idempotent for it. Live 0.
    //     +1308  ptr     the {record, owner} cache pair for SpectatorSpeedMul, ON THE INSTANCE rather than in
    //     +1312  ptr     .data like every other one. Live the record reads 2.0, its registered default.
    //
    // THE ANOMALY IS CONFIRMED FROM BOTH SIDES: there is no g_cvar_SpectatorSpeedMul global in gameclient at
    // all, which is why the instance holds the pair. Every other variable in that function has one.
    //
    // A CROSS-CHECK WORTH KEEPING, because it validates the reading of the whole function. The LTConVar records
    // Init creates are allocated at a 0x48 stride with no gaps, and their addresses run in exactly the order the
    // decompiled function registers them -- SpectatorSpeedMul, FallDamageDebug, PlayerGravity, TestFireRecoil,
    // SurfaceSwimHeight, ShowPlayerPos, ShowPlayerVel, YawClamp, YawBias, YawInterp, MouseYawMult,
    // KeyboardYawMult, GPadTimePastThreshold, SpecialJumpDrawPredict, OnCharacterPush{Directions,Magnitude,Time}.
    // A source order and a heap layout agreeing across seventeen entries with zero slack.
    //
    // (The two loops over 71 items in that function -- 'GunLead' and 'GamePad' -- allocate nothing in this run,
    // so whatever they build is not a console variable of this shape.)
    static constexpr uintptr_t kMoveMgrWaterAffectsSpeed = 521;
    static constexpr uintptr_t kMoveMgrSpectatorSpeedMulCache = 1308;

    // Does water slow the player? The database-driven flag CMoveMgr keeps on itself. nullopt when CMoveMgr
    // cannot be resolved or the read faults.
    static std::optional<bool> water_affects_speed(unsigned index);

    // The live SpectatorSpeedMul, read through the cache pair on the CMoveMgr instance rather than through the
    // global scan -- because for this one variable there is no global. nullopt when the slot is unset.
    static std::optional<float> spectator_speed_mul(unsigned index);

    // The {record, owner} pair itself, for a consumer that wants to WRITE the value: the record's first float is
    // what the movement code reads. nullopt when CMoveMgr cannot be resolved.
    struct VarCache {
        uintptr_t record{};
        uintptr_t owner{};

        bool populated() const { return record != 0; }
    };

    static std::optional<VarCache> spectator_speed_mul_cache(unsigned index);

    // Is SpectatorSpeedMul still its registered default of 2.0? sdk::Engine::is_at_default CANNOT answer this
    // one -- it works from the discovered globals and this variable has none -- so the question is answered
    // here, where the instance cache is reachable. nullopt when CMoveMgr or the slot is unavailable.
    //
    // Exists because the gap is real rather than theoretical: a consumer asking "has anything touched the
    // movement tunables" would silently miss this variable otherwise.
    static std::optional<bool> spectator_speed_mul_is_default(unsigned index);

    // ---- THE CAMERA CLAMP, WHICH IS WHAT LIMITS A HEAD-TRACKED VIEW -------------------------
    //
    // CPlayerCamera_GetActiveCameraClamp picks a clamp for the player's current state and writes it as two
    // floats. Its inputs are both on CPlayerCamera:
    //
    //     +6332  the Client/CameraClamping record CPlayerCamera_Init resolved
    //     +688   the nine-state machine an earlier pass mapped and could not explain -- states 1 and 7 select
    //            the "Chase" clamp, which is the first concrete meaning attached to any of those values
    //
    // and its FALLBACK when no record is set is (85, 85) -- wider than most of the shipped clamps, so an absent
    // record loosens the view rather than locking it.
    //
    // Shipped Default record, in degrees: StandIdle 80/85, StandMoving 80/85, CrouchIdle 42/85,
    // CrouchMoving 42/85, Chase 40/45, SlideKick 5/5. All 18 pairs across the three records (Default, Turret,
    // ElitePoweredArmor) are ordered min <= max.
    //
    // THE PAIR IS ONE AXIS, NOT TWO, and the dispatcher's own tail settles it -- the second component is
    // NEGATED on the way out:
    //
    //     out[0] = degrees[0] *  (pi/180)
    //     out[1] = degrees[1] * -(pi/180)
    //
    // You do not negate one member of a two-axis limit pair. So the record stores two POSITIVE MAGNITUDES in
    // degrees and the engine turns them into a SIGNED RANGE in radians: Default StandIdle 80/85 becomes
    // +1.396 / -1.484 rad, an ASYMMETRIC range. Chase is +40/-45 degrees and SlideKick a symmetric +-5.
    //
    // THE AXIS IS PITCH, AND THAT IS NOW A READ RATHER THAN AN INFERENCE. CPlayerCamera_ClampPitch is the
    // consumer: it converts the camera's rotation at +324 to Euler angles, takes component [1], tests it against
    // this signed range, and when the range is exceeded it consults a console variable named SmoothPitchTime.
    // The variable names the axis.
    //
    // +6332 sits four bytes below saved_near_z at +6336, consistent with the >= 6342-byte class size an early
    // pass derived from the constructor.
    static constexpr uintptr_t kCameraClampRecord = 6332;
    static constexpr uintptr_t kCameraStateMachine = 688;
    static constexpr float kCameraClampFallback = 85.0f;

    // The Client/CameraClamping record the camera resolved, or nullptr when none is set (in which case the
    // engine uses the fallback above).
    static regenny::DatabaseMgrRecord* camera_clamp_record(unsigned index);

    // The camera's state-machine value, which selects among the clamp attributes.
    static std::optional<uint32_t> camera_clamp_state(unsigned index);

    // One named clamp from that record, e.g. "Chase" or "StandIdle", AS STORED -- two positive magnitudes in
    // degrees. nullopt when the record is absent or the attribute is not a float pair.
    static std::optional<std::pair<float, float>> camera_clamp(unsigned index, std::string_view state);

    // The same clamp AS THE ENGINE APPLIES IT: a signed range in radians, second component negated. This is the
    // form a consumer comparing against a live view angle needs, and computing it here means a caller cannot
    // forget the negation.
    static std::optional<std::pair<float, float>> camera_clamp_radians(unsigned index, std::string_view state);

    // ---- WHICH CLAMP THE ENGINE WILL PICK, AND THE STATE IT READS IT FROM -----------------------
    //
    // The dispatcher's selection is not driven by the state machine alone. Its real inputs are CMoveMgr's flags
    // and velocity:
    //
    //     state 1 or 7                        -> Chase
    //     an action id of 575 is active        -> SlideKick
    //     CMoveMgr flags & 0x20 (crouching)    -> CrouchMoving if moving else CrouchIdle
    //     otherwise                            -> StandMoving  if moving else StandIdle
    //
    // and "moving" is CMoveMgr_IsMoving: the ENGINE's physics velocity exceeding 0.1, or flag 0x800 set.
    static constexpr uintptr_t kMoveMgrFlags = 296;
    static constexpr uint32_t kMoveFlagCrouching = 0x20;
    static constexpr uint32_t kMoveFlagForceMoving = 0x800;
    static constexpr float kMoveSpeedThreshold = 0.1f;

    // CMoveMgr's flags dword. Two bits have established meanings (above); the rest are unread and unnamed.
    static std::optional<uint32_t> move_mgr_flags(unsigned index);

    static std::optional<bool> is_crouching(unsigned index);

    // ---- THE MOVEMENT FLAGS, DECODED ------------------------------------------------------------
    //
    // CMoveMgr + 296. CMoveMgr_UpdateInputFlags (gameclient 0x10104BA0) is its producer -- 41 instructions in
    // that one function touch it -- and the bits below are established from two directions: the code that sets
    // or tests each one, and live play with a human naming what they were doing.
    //
    // The four DIRECTION bits are a signed-axis pair encoding, from CMoveMgr_SetInputDirectionFlags:
    // exactly-zero input sets NEITHER bit of a pair, so "no input on this axis" is distinguishable from either
    // direction. That is the same shape as the camera clamp record, which also stores two positive magnitudes
    // and lets the consumer build the signed range.
    //
    // 0x1 IS NOT "on ground", which was the natural guess from the composites -- the encoder proves it is an
    // axis component. Live 0xA01 (moving) and 0xB00 only make sense that way.
    enum class MoveFlag : uint32_t {
        // THE FORWARD AXIS IS NEGATED, which matters to anything feeding synthetic input. The encoder sets
        // 0x2 for a POSITIVE axis-1 value and 0x1 for a negative one, and live play shows 0x1 is FORWARD --
        // so positive input on that axis means backward. Named for the direction the player moves, not for
        // the sign of the raw axis, because the direction is what a consumer means.
        Forward            = 0x00001,  // CMoveMgr_SetInputDirectionFlags, axis 1 negative
        Backward           = 0x00002,  // axis 1 positive
        Crouching          = 0x00020,  // tested 4x in the producer; 17 live transitions while crouching
        Left               = 0x00080,  // axis 0 negative
        Right              = 0x00100,  // axis 0 positive
        // NORMAL WALKING SPEED -- cleared by ANY speed modifier, not just one. Live:
        //     forward           0xA01   normal speed set
        //     forward + ADS     0x801   CLEARED
        //     sprinting         0x40801 CLEARED
        // So it is not "sprinting inverted"; it marks the unmodified gait. That the only code reading it is
        // PlayerMovement_UpdateAirAcceleration fits -- air control depends on the gait you left the ground in.
        NormalSpeed        = 0x00200,
        // Read by CMoveMgr_IsMoving, so it makes the engine treat the player as moving. A jump sets it with
        // zero ground speed, which is what that is for.
        CountsAsMoving     = 0x00800,
        // ORd in exactly one place and EDGE TRIGGERED: holding the key does not re-set it.
        Melee              = 0x04000,
        GrenadeHeld        = 0x20000,  // live observation; no immediate in code (ORd from a register)
        Sprinting          = 0x40000,  // live observation; likewise register-sourced
    };

    struct MovementFlags {
        uint32_t raw{};

        bool has(MoveFlag f) const { return (raw & static_cast<uint32_t>(f)) != 0; }

        bool crouching() const { return has(MoveFlag::Crouching); }
        bool forward() const { return has(MoveFlag::Forward); }
        bool backward() const { return has(MoveFlag::Backward); }
        bool left() const { return has(MoveFlag::Left); }
        bool right() const { return has(MoveFlag::Right); }
        bool counts_as_moving() const { return has(MoveFlag::CountsAsMoving); }
        bool normal_speed() const { return has(MoveFlag::NormalSpeed); }
        bool sprinting() const { return has(MoveFlag::Sprinting); }
        bool melee() const { return has(MoveFlag::Melee); }
        bool grenade_held() const { return has(MoveFlag::GrenadeHeld); }

        // {strafe, forward} as -1, 0 or +1, in the PLAYER's frame: +1 strafe is right, +1 forward is
        // forward. Zero means genuinely no input on that axis -- the encoder sets neither bit of a pair for
        // an exactly-zero value, so it is distinguishable from a direction rather than defaulted.
        //
        // Note the forward component is the NEGATION of the raw axis; see MoveFlag::Forward.
        std::array<int, 2> input_direction() const {
            std::array<int, 2> d{0, 0};
            if (has(MoveFlag::Right)) { d[0] = 1; }
            else if (has(MoveFlag::Left)) { d[0] = -1; }
            if (has(MoveFlag::Forward)) { d[1] = 1; }
            else if (has(MoveFlag::Backward)) { d[1] = -1; }
            return d;
        }

        // BOTH BITS OF A PAIR SET is contradictory -- the encoder writes at most one per axis. Reported so a
        // consumer can tell a stale read from a live one instead of silently preferring positive.
        bool direction_contradicts() const {
            return (has(MoveFlag::Right) && has(MoveFlag::Left)) ||
                   (has(MoveFlag::Forward) && has(MoveFlag::Backward));
        }

        // THE BITS THIS MAPPING DOES NOT NAME. Non-zero is expected, not a failure: the producer also ORs
        // 0x4 and 0x8, which nothing here has established. Exposed so a consumer sees what is unaccounted
        // for rather than assuming the named set is complete.
        uint32_t unmapped() const;
    };

    static std::optional<MovementFlags> movement_flags(unsigned index);

    // "sprinting|moving|crouching|+x|-y", or "" for zero. Named bits only; see MovementFlags::unmapped.
    static std::string movement_flag_names(uint32_t raw);

    // The engine's live physics velocity for the player, via ILTPhysics GetVelocity -- NOT CMoveMgr's cached
    // copy at +1412, which can disagree.
    static std::optional<std::array<float, 3>> physics_velocity(unsigned index);

    // CMoveMgr_IsMoving reproduced: speed over the threshold, or the force-moving flag. nullopt when neither
    // input can be read.
    static std::optional<bool> is_moving(unsigned index);

    // Which clamp name the dispatcher would pick right now, from the same inputs it uses. nullopt when the
    // inputs cannot be read. Returns nullptr-equivalent empty string never -- the four stance cases always
    // resolve, and the two special cases are reported as themselves.
    //
    // THE SlideKick CASE IS NOT REPRODUCED: it depends on an action id this project has not mapped, so this
    // returns the stance choice the engine would make in its absence and says so via `slide_kick_unchecked`.
    struct ClampChoice {
        std::string state;             // the clamp attribute name
        bool slide_kick_unchecked{};   // true always, until the action-id test is mapped
    };

    static std::optional<ClampChoice> predicted_clamp_state(unsigned index);

    // ---- WHAT THE CLAMP DID, WHICH IS WHERE A VR MOD MUST INTERVENE -----------------------------
    //
    // CPlayerCamera_ClampPitch writes both sides of its decision, but ONLY when the clamp actually engages:
    //
    //     +756  float  the pitch BEFORE clamping
    //     +760  float  the pitch AFTER clamping
    //
    // So the pair is a record of the last violation, not a per-frame snapshot. Equal values mean the last
    // violation happened to need no correction on one bound; UNWRITTEN values mean the clamp has never engaged
    // for this camera, and nothing distinguishes "never engaged" from "engaged with these numbers" -- which is
    // why this is reported as a raw pair rather than as a "was clamped" boolean.
    //
    // FOR A HEAD-TRACKED VIEW this is the fight: the shipped Chase range is +40/-45 degrees, and a player looking
    // further will have their pitch rewritten here every frame it happens.
    static constexpr uintptr_t kCameraPitchPreClamp = 756;
    static constexpr uintptr_t kCameraPitchPostClamp = 760;

    struct PitchClampRecord {
        float before{};
        float after{};

        // Did the recorded correction actually change the value?
        bool corrected() const { return before != after; }
    };

    static std::optional<PitchClampRecord> camera_pitch_clamp_record(unsigned index);

    // Is the recorded post-clamp pitch inside the clamp the engine would apply right now? A caller can use this
    // to tell whether the last violation was resolved against the CURRENT stance's bounds or an earlier one --
    // the record survives a stance change, the bounds do not.
    static std::optional<bool> pitch_clamp_record_within_active(unsigned index);

    // ---- THE RECOVERY TIMER, WHICH IS WHAT MAKES THE RECORD MEAN SOMETHING ----------------------
    //
    // CPlayerCamera_ClampPitch RECORDS a violation at +756/+760; CPlayerCamera_ApplyLookDelta CONSUMES it:
    //
    //     if (GameTimer_IsElapsed(camera + 768))  pitch = Math_Clamp(pitch, -bound, +bound)   hard clamp
    //     else if (pitch out of range)            pitch = lerp(+756, +760, remaining / duration)
    //     else                                    reset the timer and its flags
    //
    // So the pair is not a log -- it is the endpoints of an interpolation, and the timer at +768 is its clock.
    // That is why the record survives across frames, and why reading it without the timer tells a consumer
    // nothing about what the camera is doing now.
    //
    // A VR MOD THAT DEFEATS THE CLAMP MUST ACCOUNT FOR BOTH PATHS: with the timer elapsed the pitch is clamped
    // hard, and while it runs the pitch is dragged from `before` to `after` regardless of input.
    static constexpr uintptr_t kCameraPitchRecoveryTimer = 768;

    struct TimerState {
        double start{};
        double duration{};
        bool active{};
        bool use_cached{};

        // Reproduces GameTimer_IsElapsed's own reading: an inactive timer counts as elapsed.
        bool elapsed(double now) const { return !active || (now - start) >= duration; }
    };

    // The pitch-recovery timer's state. nullopt when the camera cannot be resolved.
    static std::optional<TimerState> pitch_recovery_timer(unsigned index);

    // ANY timer of this shape, by address. The layout is shared: two doubles then two flags, and this SDK has
    // now found it in two unrelated places (the camera's pitch recovery at camera+768 and the zoom transition
    // at aim+232). A consumer that finds a third needs to read it without waiting for an accessor.
    static std::optional<TimerState> timer_at(uintptr_t address);

    // ---- THE SECOND PITCH LIMIT, WHICH IS NOT THE CLAMP ----------------------------------------
    //
    // ApplyLookDelta tests the new pitch against a console variable as well as the clamp record:
    // CameraAimTrackingYMax (70 deg) normally, CameraAimTrackingYMaxZoomed (65 deg) otherwise.
    //
    // WHICH ONE APPLIES DEPENDS ON *(player + 256) + 224 == 3, and that field IS now established -- see the
    // aim state machine below. Equal to 3 means hip fire and takes the normal limit; every other value is part
    // of the ADS lifecycle and takes the zoomed one.
    //
    // AN EARLIER VERSION OF THIS COMMENT claimed the limit check "clears a byte at camera+1005", and named that
    // byte as the zoom selector. It is neither: a live session aiming down sights six times left all 512 bytes
    // around camera+1005 untouched, while the real field moved through four values.
    // THE AIM SUB-OBJECT, a fourth pointer beside the three already mapped: controller +236, camera +252,
    // THIS at +256, physics +260. CPlayerCamera reaches it as *(*(camera + 4) + 256).
    static constexpr uintptr_t kAimSubObject = 256;

    // THE AIM STATE MACHINE, four states, established by freezing the field live and watching the game:
    //
    //     3  hip fire        (steady state)
    //     0  entering ADS    (transition in)
    //     1  full ADS
    //     2  leaving ADS     (transition out)
    //
    // CPlayerCamera_ApplyLookDelta compares it against 3: equal selects CameraAimTrackingYMax (70 deg), and
    // ANY other value selects CameraAimTrackingYMaxZoomed (65 deg). So the tighter limit covers the whole ADS
    // lifecycle including both transitions, which is why the fall-through is not the inversion it looks like.
    //
    // Frozen at 3 the FOV stops zooming while the weapon still animates into ADS -- and recoil stays hip-fire
    // heavy, so this field gates recoil as well as the camera.
    static constexpr uintptr_t kAimState = 224;

    // A SEPARATE ADS FLAG, 1 while aiming. Frozen at 0 the FOV stops zooming but recoil stays ADS-light, which
    // is what distinguishes it from kAimState: this one drives the FOV, that one drives the aim limit and
    // recoil. A VR consumer wanting to keep the aim behaviour while suppressing the FOV zoom wants THIS one.
    static constexpr uintptr_t kAdsFovFlag = 356;

    enum class AimState {
        EnteringAds = 0,
        Ads = 1,
        LeavingAds = 2,
        Hip = 3,
    };

    // The raw dword, for a consumer that wants to see a value this mapping does not name.
    static std::optional<uint32_t> aim_state_raw(unsigned index);
    // Named, and std::nullopt for a value outside the four observed states rather than a guess.
    static std::optional<AimState> aim_state(unsigned index);
    // Is the FOV-zoom flag set? Independent of aim_state; see the note above.
    static std::optional<bool> ads_fov_active(unsigned index);
    // Which aim-tracking limit would ApplyLookDelta pick right now -- exactly its own test.
    static std::optional<bool> uses_zoomed_aim_limit(unsigned index);

    // The zoom transition's clock, at aim + 232. UpdateTransition commits the zoom when this elapses.
    static constexpr uintptr_t kZoomTransitionTimer = 232;

    // THE ZOOM FRACTION, 0 at the hip and 1 at full ADS, reproducing PlayerZoom_GetZoomFraction exactly:
    //
    //     state 1 -> 1.0                       state 3 -> 0.0
    //     state 0 -> the timer's fraction      state 2 -> 1.0 - that fraction
    //
    // WHY A VR MOD WANTS THIS: it is the game's own progress through the aim transition, and it is what the
    // engine interpolates depth-of-field with. A mod suppressing the FOV zoom still needs the fraction to keep
    // anything it drives itself in step with the weapon animation.
    //
    // `now` is the engine clock the timer is measured against; pass Engine::seconds(). nullopt when the aim
    // object cannot be resolved or the state is not one of the four.
    static std::optional<float> zoom_fraction(unsigned index, double now);

    // ---- THE VIEW WRITER, FOR HOOKING ----------------------------------------------------------
    //
    // CPlayerCamera_ApplyLookDelta in gameclient.dll: adds a look delta to the camera's rotation and clamps
    // the resulting pitch. This is THE interception point for a head-tracked view, and the reason is measured
    // rather than assumed -- writing the camera object's rotation or the holder's outer operand is RECLAIMED
    // within one frame (probe_camera_object_rotation / probe_outer_operand both return Reclaimed on a running
    // game), so a consumer cannot steer the view by writing a field. It has to own the writer.
    //
    // ABI, verified in the disassembly rather than taken from the decompiler:
    //     __thiscall, `this` in ecx (mov esi, ecx at +5)
    //     THREE 4-byte stack arguments -- both exits are `retn 0Ch`
    // So an x86 detour is `__fastcall(this, edx_dummy, a2, a3, a4)`: fastcall is callee-cleans and emits the
    // matching `retn 0Ch`. Getting the arity wrong here corrupts esp at every call, so it is pinned by the
    // return instruction, not by the prototype Hex-Rays guessed.
    //
    // 0 until gameclient.dll is resolved; the caller may retry (RETRYABLE, per AGENT.MD rule 5).
    static uintptr_t apply_look_delta_fn();

    // PlayerCamera_UpdateViewPose, the PER-FRAME half of the view path.
    //
    // ApplyLookDelta is driven by look INPUT: measured live, the frame hook ticked ~300/s while it stayed at
    // exactly 0 with the mouse still. So it is the place to intercept a look delta and NOT a place to write a
    // pose every frame -- a head-tracked view has to update whether or not the player touched the mouse.
    // This one runs from the camera's own update path and is the candidate for that.
    //
    // ABI, from the disassembly: __thiscall with NO stack arguments (the single exit is a plain `retn`), so an
    // x86 detour is __fastcall(this, edx_dummy) and the callee cleanup is zero either way. Note the prologue
    // realigns the stack (`and esp, 0FFFFFFC0h`), which is why the whole prologue is matched rather than a
    // short lead-in.
    //
    // 0 until gameclient.dll resolves; RETRYABLE, same as above.
    static uintptr_t update_view_pose_fn();

    // ---- WRITING THE VIEW ------------------------------------------------------------------------
    //
    // THE method a head-tracked view needs, and the placement is measured rather than chosen:
    //
    //   * writing the camera OBJECT's rotation is reclaimed within one frame (probe_camera_object_rotation
    //     returns Reclaimed on a running game), so the object is downstream and not writable;
    //   * the applied pose is UPSTREAM -- sampled inside the UpdateViewPose detour, the camera object still
    //     holds the previous frame's value when that function returns, and something later in the frame
    //     propagates the pose into it.
    //
    // So an override writes HERE and lets the engine carry it. Called from inside the UpdateViewPose detour,
    // after the original has run, it replaces the pose the engine just computed for this frame.
    //
    // REFUSES A NON-UNIT QUATERNION, mirroring read_pose's own validity test: the read side treats non-unit as
    // proof of a wrong offset, so the write side must not be the thing that creates one. Returns false on a
    // rejected value or a faulting store; per Memory.hpp's note on `store`, a true return is not proof the
    // engine accepted it -- read it back if that matters.
    static bool write_applied_rotation(unsigned index, const std::array<float, 4>& rotation);

    // THE OTHER GENERATION, at +324, and the evidence says it is the SOURCE the applied pose is derived from.
    //
    // Writing the applied rotation is visible -- a live override there makes the view blur, so the renderer
    // does consume it -- but it does NOT survive to the next frame and never reaches the camera object. So the
    // engine rebuilds the applied pose each frame, and CPlayerCamera_ClampPitch shows what from: it takes euler
    // angles out of `this + 324` via Quaternion_ToEuler, clamps the pitch, and rebuilds a quaternion.
    //
    // That makes +324 the upstream candidate. An override that writes here is asking the engine to derive the
    // view from our rotation rather than fighting its output.
    static bool write_view_rotation(unsigned index, const std::array<float, 4>& rotation);
    static std::optional<std::array<float, 4>> view_rotation(unsigned index);

    // The applied pose's rotation as the engine last wrote it. Separate from player()'s snapshot so an override
    // can read exactly the value it is about to replace, with no other fields in the way.
    static std::optional<std::array<float, 4>> applied_rotation(unsigned index);

    // DO THE CAMERA'S TWO POSE GENERATIONS HOLD DIFFERENT POSITIONS? Which is, in practice, a VIEW BOB
    // DETECTOR -- established by a controlled A/B rather than inferred:
    //
    //     bob ON  : 2 of 47 same-phase samples matched
    //     bob OFF : 46 of 46 matched, and the applied pose became bit-identical to the camera object
    //
    // So the difference between the generations IS the bob/sway offset. Two consequences worth stating:
    //
    //   * A VR mod must know this. A head-tracked view has to suppress bob, and this is how a consumer tells
    //     whether it is still active without owning the setting.
    //   * A test suite must not force it off. Two checks in this project were passing only because bob was on,
    //     and bob-on is the mode every real player runs.
    //
    // Compares POSITION, because that is where the offset lands; the rotations can agree while the positions do
    // not. nullopt when the camera holder cannot be read.
    static std::optional<bool> pose_generations_differ(unsigned index);

    struct AimTrackingLimits {
        std::optional<float> normal_degrees;
        std::optional<float> zoomed_degrees;
    };

    // Both aim-tracking limits, read from their cached console variables. Either may be absent when its cache
    // slot is unset.
    static AimTrackingLimits aim_tracking_limits();



    // Does the state machine currently select the Chase clamp? The only mapping established so far, exposed as a
    // question rather than as a full state decode.
    static std::optional<bool> camera_state_is_chase(unsigned index);

    struct CameraSubObjects {
        uintptr_t controller{};      // player + 236, embedded at player + 0x2760
        uintptr_t player_camera{};   // player + 252
        uintptr_t physics_holder{};  // player + 260
    };

    static std::optional<CameraSubObjects> camera_sub_objects(unsigned index);

    // Does every one of the three name the player as its owner at +4? True live, but see the table above: the
    // property holds for 21 of the 22 subsystems, not all, so this is a check on THESE THREE rather than a
    // general validity test. nullopt when the player or any sub-object pointer cannot be read.
    static std::optional<bool> sub_objects_own_player(unsigned index);

    // Is each sub-object exactly the embedded member it should be -- an exact address identity, which
    // discriminates far better than a class check. The aim object is expected NOT to be embedded; it is included
    // so that "embedded" is a distinction the data supports rather than a label every pointer would satisfy.
    // WHERE THE SUB-OBJECTS ACTUALLY SIT, measured rather than compared against a constant.
    //
    // RETRACTION: "EMBEDDED" WAS NEVER A PROPERTY OF THIS CLASS.
    //
    // An earlier pass recorded the three sub-objects as members embedded at player+0x2760, +0xE88 and +0x3020,
    // live-measured from one instance with no IDA evidence -- neither constant appears as an immediate in
    // gameclient.dll's .text. A later session settled it: ALL FOUR sub-objects sit BELOW the player address,
    // so not one of them can be a member of it, while every owner back-pointer and vtable still identified
    // them correctly. They are separate allocations whose position relative to the player is arbitrary.
    //
    // The constants and the four *_is_embedded predicates are gone rather than loosened. What identifies a
    // sub-object is its class vtable and its owner back-pointer at +4, both of which hold in every session
    // measured; where it happens to sit does not, and is reported here instead of asserted.
    //
    // A consumer that wants to know where a sub-object lives must therefore ASK, not assume. Each offset is
    // the byte distance from the player, or nullopt for a pointer that is not above the player at all (the
    // aim object is a separate allocation and legitimately answers nullopt).
    struct SubObjectOffsets {
        std::optional<uintptr_t> controller;
        std::optional<uintptr_t> player_camera;
        std::optional<uintptr_t> physics_holder;
        std::optional<uintptr_t> aim;
    };
    static std::optional<SubObjectOffsets> sub_object_offsets(unsigned index);


    // The aim/zoom controller's address -- *(player + 256). Separate allocation, so its lifetime is not the
    // player's; a consumer holding it across a level change must re-resolve.
    static std::optional<uintptr_t> aim_object(unsigned index);

    // Does the aim object name the player at +4, like the other three sub-objects? The same convention, and a
    // strong identity: an exact address match that no unrelated pointer satisfies. Verified live in ReGenny
    // (aim = 0x5FD20A8, *(aim + 4) = 0x1C636F60 = the player).
    static std::optional<bool> aim_object_owns_player(unsigned index);

    // Do the controller and physics holder carry the vtables their constructors install? The same one-load guard as
    // holder_is_player_camera, for the other two.
    static std::optional<bool> controller_class_matches(unsigned index);
    static std::optional<bool> physics_holder_class_matches(unsigned index);

    // ---- THE HOLDER IS CPlayerCamera, AND THAT IS CHECKABLE ---------------------------------
    //
    // Everything this class calls "the holder" is a CPlayerCamera instance -- the game-side camera class an early
    // pass mapped from the other end, by its vtable, constructor and eight delegate sinks. The identification is
    // vtable identity, verified live: *(holder) is gameclient + 0x1D5B94 exactly, the secondary vtables at +0x10
    // and +0x24 are the ones that pass recorded, and all eight sink nodes are threaded rather than self-linked --
    // which is what it predicted would happen at runtime after the constructor self-links them.
    //
    // WHY A CONSUMER WANTS THE CHECK: this class reads about thirty offsets off that pointer, and a wrong pointer
    // produces plausible numbers rather than a fault -- exactly what happened when the physics holder was read with
    // pose offsets and returned a position of (0,0,0) with a zero-norm quaternion. Checking the class first is one
    // load and it fails closed.
    //
    // See the CPlayerCamera struct in reversing/fear2.genny for the full field map.
    static constexpr uintptr_t kPlayerCameraVtable = 0x1D5B94;  // gameclient-relative

    // Does this player's holder carry CPlayerCamera's vtable? nullopt when the player or the pointer cannot be
    // read; false means the pointer is not what every other accessor here assumes.
    static std::optional<bool> holder_is_player_camera(unsigned index);

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

    // ---- CAMERA HEIGHT SMOOTHING, WHICH IS OFF AND WOULD BE A NO-OP ANYWAY ------------------
    //
    // PlayerCamera_UpdateViewPose lerps the camera's height toward its new value instead of snapping, which is
    // exactly the kind of lag that fights a head-tracked view. The whole block is:
    //
    //     if (CameraSmoothingEnabled == 1.0) {
    //         holder[+996] = 0
    //         if (holder[+748] && holder[+752] != new_height) {
    //             rate = (holder[+752] >= new_height) ? CameraHeightInterpSpeedDown
    //                                                 : CameraHeightInterpSpeedUp
    //             rate = min(rate, 1.0)                          <- the clamp that matters
    //             new_height = lerp(holder[+752], new_height, rate)
    //             holder[+996] = new_height - previous           <- what smoothing moved this frame
    //         }
    //         holder[+748] = 1;  holder[+752] = new_height
    //     }
    //
    // holder[+752] IS THE SAME FIELD PlayerCamera_TranslateCameraObject ACCUMULATES INTO. Platform carry adds its
    // vertical delta there so the smoother does not treat being carried as the player moving -- two subsystems
    // sharing one field, which is why that function writes both +752 and +1000.
    //
    // AND IT IS DOING NOTHING ON THIS BUILD, which is worth stating because the obvious VR advice would be wasted
    // effort. Measured live: CameraSmoothingEnabled is 0.0, so the block never runs -- and the two interpolation
    // speeds are 1000.0, which `min(rate, 1.0)` turns into an INSTANT lerp, so even enabling it would smooth
    // nothing at the default settings. is_effective() reports that conjunction rather than just the gate, because
    // reading the gate alone would say "off" for the wrong reason if a mod set it to 1.
    struct HeightSmoothing {
        float enabled{};          // the CameraSmoothingEnabled setting; the block requires exactly 1.0
        float up_speed{};         // CameraHeightInterpSpeedUp
        float down_speed{};       // CameraHeightInterpSpeedDown
        bool has_previous{};      // holder[+748]
        float previous_height{};  // holder[+752], shared with platform carry
        float applied_delta{};    // holder[+996], what the last lerp moved

        // Would this configuration actually smooth anything? The gate must be exactly 1.0 AND at least one speed
        // must survive the clamp below 1.0.
        bool is_effective() const;
    };

    static constexpr uintptr_t kSmoothingHasPrevious = 748;
    static constexpr uintptr_t kSmoothingPreviousHeight = 752;
    static constexpr uintptr_t kSmoothingAppliedDelta = 996;

    static std::optional<HeightSmoothing> camera_height_smoothing(unsigned index);

    // Turn the gate on or off. Provided because a consumer that DOES want to change it should not have to know the
    // variable's name -- but see is_effective(): on the default speeds this changes nothing.
    static bool set_camera_smoothing_enabled(bool enabled);

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
    // AND THE WHOLE CHAIN IS NOW CLOSED, every input identified and read live. A previous draft called the 3.5556
    // ratio "unexplained" and reasoned it was "twice 16/9". It is not a factor of two over anything -- it is the
    // ACTUAL VIEWPORT:
    //
    //     FovY console variable        65.0 degrees   x pi/180  ->  holder[+296] = 1.134464 rad   (exact)
    //     viewport rect                5120 x 1440              ->  aspect = 3.555556 = 32/9
    //     FovAspectRatioScale          1.0                      ->  scale
    //     fov_x = 2 * atan(tan(fov_y/2) * aspect) * scale       ->  holder[+292] = 2.31011 rad = 132.36 deg
    //
    // 5120 x 1440 is a real framebuffer, so 32:9 is simply the display. THE ASSUMPTION OF 16:9 WAS THE ERROR, not
    // the engine, and "twice 16/9" was pattern-matching on a number that had a plainer cause.
    //
    // The rect lives on the POSE HOLDER at +196..224, which the three call sites establish: each does `mov ecx,
    // esi` from its own `this`, so the object supplying the rect is the holder itself. Exposed as viewport_rect().
    //
    // fov_inputs() reads the two console variables and the rect together, and fov_derivation_holds() recomputes
    // BOTH stored angles from them -- so the chain is checked end to end rather than at its output.
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

    // The viewport rect the aspect comes from, on the pose holder. The producer reads it as
    // (b + d - f - h) / (a + c - e - g) over eight integer fields at +196..224; live only two are non-zero, giving
    // 5120 x 1440 directly. Reported as the numerator and denominator that arithmetic produces, so a consumer sees
    // exactly what the engine divided rather than a guess at which fields are width and height.
    struct ViewportRect {
        int32_t fields[8]{};   // +196..224 verbatim
        int32_t width{};       // the numerator of the engine's own expression
        int32_t height{};      // its denominator
    };

    static std::optional<ViewportRect> viewport_rect(unsigned index);

    // The three inputs the FOV pair is built from: the FovY setting in DEGREES, the FovAspectRatioScale setting,
    // and the aspect the rect yields. nullopt when any is unavailable.
    struct FovInputs {
        float fov_y_degrees{};
        float aspect_scale{};
        float aspect{};
    };

    static std::optional<FovInputs> fov_inputs(unsigned index);

    // Do the two stored angles follow from those three inputs, by the producer's own formula? This checks the
    // ENTIRE chain -- console variable, degree conversion, rect-derived aspect, scale, clamp -- against the pair the
    // engine stored, rather than checking the output against itself.
    //
    // nullopt when the inputs or the pair cannot be read.
    static std::optional<bool> fov_derivation_holds(unsigned index, float tolerance = 1e-3f);

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
    // COMPARING A CACHED COPY AGAINST THE LIVE FIELD IT MIRRORS, without lying when the engine writes between
    // the two reads.
    //
    // The bool accessors below read one side, then the other. In a FROZEN game that is exact; in a running one
    // a frame can land in the gap, and the two sides then disagree for a reason that has nothing to do with the
    // mapping. This project passed those checks for many sessions purely because the game was always unfocused
    // -- and therefore frozen -- while the suite ran. The first live run failed four of them at once.
    //
    // WHY A VR CONSUMER NEEDS THE DISTINCTION and not just a bool: reading the view pose every frame while the
    // engine updates it is the entire job. "These disagree" and "I looked while it was being written" call for
    // opposite responses -- the first means the mapping is wrong, the second means read it again.
    enum class PoseAgreement {
        Unreadable,  // an address was rejected, or the camera object moved between reads
        Torn,        // the engine wrote one of the two sides while we were looking; ask again
        Equal,       // both sides held still and hold the same bits
        Differ,      // both sides held still and hold DIFFERENT bits -- a real disagreement
    };

    // Both sides are read twice; a change in either between the reads yields Torn rather than a verdict.
    static PoseAgreement camera_rotation_agreement(unsigned index);
    static PoseAgreement applied_pose_agreement(unsigned index);

    // The controller's cached position against the engine object's, with the same double read. Same defect as
    // the two above: the cached triple is read, then the engine object is looked up and read, and a frame in
    // between moves one of them.
    static PoseAgreement cached_position_agreement(unsigned index);

    // HOW OFTEN DOES A READ TEAR? A consumer reading the view pose every frame needs a retry policy, and the
    // only honest basis for one is a measurement: on a running engine some fraction of double reads straddle an
    // update, and on a frozen one none do.
    //
    // It also keeps the checks built on the verdicts from going VACUOUS. "Never Differs" is satisfied by a Torn
    // result, so a suite that only asserted that would pass forever without the comparison ever happening --
    // the same trap as a name-consistency check over names that never repeat. Assert `equal > 0` beside it.
    struct AgreementCensus {
        unsigned equal{};
        unsigned differ{};
        unsigned torn{};
        unsigned unreadable{};
    };

    // Samples one of the three agreement accessors `samples` times. `which`: 0 camera rotation, 1 applied pose,
    // 2 cached position.
    static AgreementCensus agreement_census(unsigned index, unsigned which, unsigned samples);

    // The older bool forms, kept because callers exist. std::nullopt now means unreadable OR torn -- use the
    // agreement accessors above when the difference matters, which in a running game it does.
    static std::optional<bool> camera_rotation_matches_pose(unsigned index);
};

}  // namespace sdk

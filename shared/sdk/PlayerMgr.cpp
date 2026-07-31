#include <utility/Seh.hpp>
#include "PlayerMgr.hpp"

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstring>
#include <iterator>

#include "Memory.hpp"
#include "Modules.hpp"
#include "Physics.hpp"
#include "CClientShell.hpp"
#include "DatabaseMgr.hpp"
#include "Engine.hpp"
#include "Model.hpp"
#include "Object.hpp"
#include "SceneCamera.hpp"
#include "ShaderParams.hpp"

namespace sdk {

namespace {

// The manager pointer in gameclient.dll's data. ConsoleCmd_GetPlayerPos loads this and adds the slot offsets
// to its VALUE, so the global holds the object rather than being the object.
constexpr uintptr_t kManagerPointerOffset = 0x1F9E30;

// An engine object must have a vtable inside FEAR2.exe. That is the cheapest test that a pointer the game
// handed us is an LTObject at all, and it is what stops a stale or mis-offset field from being reported as
// one.
bool is_engine_object(uintptr_t address) {
    if (address == 0) {
        return false;
    }
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || exe->size == 0) {
        return false;
    }
    const auto vtable = mem::read_ptr(address);
    if (!vtable.has_value()) {
        return false;
    }
    return *vtable >= exe->base && *vtable < exe->base + exe->size;
}

// LTObject.position, for the eye-offset arithmetic.
constexpr uintptr_t kObjectPosition = 0x14;
constexpr uintptr_t kObjectRotation = 0x20;

}  // namespace

bool PlayerMgr::quaternion_is_unit(const std::array<float, 4>& q, float tolerance) {
    float sum = 0.0f;
    for (const float c : q) {
        if (!std::isfinite(c)) {
            return false;
        }
        sum += c * c;
    }
    return std::fabs(sum - 1.0f) <= tolerance;
}

bool PlayerMgr::Pose::rotation_is_unit(float tolerance) const {
    return PlayerMgr::quaternion_is_unit(rotation, tolerance);
}

std::optional<bool> PlayerMgr::is_server_backed(uintptr_t object) {
    if (object == 0) {
        return std::nullopt;
    }
    // LTObject.handle at +0x12, and the predicate is the engine's own: CLTClient_IsServerObject is
    // `*out = handle != 0xFFFF` and nothing else.
    const auto handle = mem::read_u16(object + 0x12);
    if (!handle.has_value()) {
        return std::nullopt;
    }
    return *handle != kNoServerHandle;
}

uintptr_t PlayerMgr::manager() {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return mem::read_ptr(gc->base + kManagerPointerOffset).value_or(0);
}

std::optional<uintptr_t> PlayerMgr::slot(unsigned index) {
    if (index >= kSlotCount) {
        return std::nullopt;
    }
    const auto mgr = manager();
    if (mgr == 0) {
        return std::nullopt;
    }
    const auto value = mem::read_ptr(mgr + kSlotsBegin + index * sizeof(void*));
    if (!value.has_value() || *value == 0) {
        return std::nullopt;  // empty slot, which is the normal state for 1..3
    }
    return value;
}

unsigned PlayerMgr::occupied_slot_count() {
    unsigned n = 0;
    for (unsigned i = 0; i < kSlotCount; ++i) {
        if (slot(i).has_value()) {
            ++n;
        }
    }
    return n;
}

std::optional<unsigned> PlayerMgr::first_occupied_slot() {
    for (unsigned i = 0; i < kSlotCount; ++i) {
        if (slot(i).has_value()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<PlayerMgr::Pose> PlayerMgr::read_pose(uintptr_t holder, bool applied) {
    if (holder == 0) {
        return std::nullopt;
    }

    const uintptr_t pos_off = applied ? kAppliedPosition : kPosition;
    const uintptr_t rot_off = applied ? kAppliedRotation : kRotation;

    Pose pose{};
    if (!mem::copy(pose.position.data(), holder + pos_off, sizeof(pose.position)) ||
        !mem::copy(pose.rotation.data(), holder + rot_off, sizeof(pose.rotation))) {
        return std::nullopt;
    }

    // THE VALIDITY TEST, and it is the rotation rather than the position: world coordinates can be almost
    // anything, but a quaternion that is not unit-length means the offset is wrong. Refusing here is what
    // keeps a mis-offset holder from handing back a transform that looks usable.
    if (!pose.rotation_is_unit()) {
        return std::nullopt;
    }
    for (const float c : pose.position) {
        if (!std::isfinite(c)) {
            return std::nullopt;
        }
    }
    return pose;
}

std::optional<PlayerMgr::Player> PlayerMgr::player(unsigned index) {
    const auto object = slot(index);
    if (!object.has_value()) {
        return std::nullopt;
    }

    const auto holder = mem::read_ptr(*object + kHolder);
    if (!holder.has_value() || *holder == 0) {
        return std::nullopt;
    }

    auto pose = read_pose(*holder);
    if (!pose.has_value()) {
        return std::nullopt;
    }

    Player p{};
    p.object = *object;
    p.holder = *holder;
    p.pose = *pose;
    // The applied pair is read too, and a failure there does NOT fail the whole player: a caller that only
    // wants the camera's own pose should still get it.
    if (const auto applied = read_pose(*holder, true)) {
        p.applied_pose = *applied;
    }

    // Both engine objects are validated as engine objects rather than trusted: a non-null field whose vtable
    // is not in the exe is not an LTObject, and reporting it as one would send a caller reading LTObject
    // offsets into whatever it actually is.
    if (const auto anchor = mem::read_ptr(*holder + kCameraObject)) {
        if (is_engine_object(*anchor)) {
            p.camera_object = *anchor;
        }
    }
    if (const auto model = mem::read_ptr(*holder + kModelObject)) {
        if (is_engine_object(*model)) {
            p.model_object = *model;
        }
    }
    return p;
}

std::optional<PlayerMgr::Player> PlayerMgr::local_player() {
    const auto index = first_occupied_slot();
    if (!index.has_value()) {
        return std::nullopt;
    }
    return player(*index);
}

namespace {

// The eight delegate offsets, from the constructor's repeating twenty-byte pattern.
constexpr uintptr_t kDelegateOffsets[] = {16, 36, 56, 76, 96, 116, 136, 156};

}  // namespace

std::vector<PlayerMgr::Delegate> PlayerMgr::camera_delegates(unsigned index) {
    std::vector<Delegate> out;
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return out;
    }

    out.reserve(std::size(kDelegateOffsets));
    for (const uintptr_t off : kDelegateOffsets) {
        const auto base = p->holder + off;
        const auto vtable = mem::read_ptr(base + kDelegateVtable);
        const auto owner = mem::read_ptr(base + kDelegateOwner);
        const auto subject = mem::read_ptr(base + kDelegateSubject);
        if (!vtable.has_value() || !owner.has_value() || !subject.has_value()) {
            continue;  // a node that does not read is left out rather than reported as zeroes
        }

        Delegate d{};
        d.address = base;
        d.vtable = *vtable;
        d.owner = *owner;
        d.subject = *subject;
        // Registered means BOTH: threaded into a list and holding a subject. The engine's unregister clears
        // the subject and self-links the node, so the two agree -- and requiring both catches a half-torn
        // node rather than reporting it as live.
        d.registered = *subject != 0 && mem::classify_link(base + kDelegateLink) == mem::LinkState::Linked;
        out.push_back(d);
    }
    return out;
}

std::optional<bool> PlayerMgr::camera_delegates_consistent(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto nodes = camera_delegates(index);
    if (nodes.size() != std::size(kDelegateOffsets)) {
        return false;  // a node failed to read at all
    }
    for (const auto& d : nodes) {
        if (d.owner != p->holder) {
            return false;
        }
    }
    return true;
}

std::optional<std::array<float, 3>> PlayerMgr::eye_offset(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->camera_object == 0 || p->model_object == 0) {
        return std::nullopt;
    }

    std::array<float, 3> anchor{};
    std::array<float, 3> model{};
    if (!mem::copy(anchor.data(), p->camera_object + kObjectPosition, sizeof(anchor)) ||
        !mem::copy(model.data(), p->model_object + kObjectPosition, sizeof(model))) {
        return std::nullopt;
    }

    std::array<float, 3> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        if (!std::isfinite(anchor[i]) || !std::isfinite(model[i])) {
            return std::nullopt;
        }
        out[i] = anchor[i] - model[i];
    }
    return out;
}

std::optional<bool> PlayerMgr::applied_pose_matches_camera_object(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->camera_object == 0) {
        return std::nullopt;
    }

    std::array<float, 3> obj_pos{};
    std::array<float, 4> obj_rot{};
    if (!mem::copy(obj_pos.data(), p->camera_object + kObjectPosition, sizeof(obj_pos)) ||
        !mem::copy(obj_rot.data(), p->camera_object + kObjectRotation, sizeof(obj_rot))) {
        return std::nullopt;
    }

    // Bit comparison, for the reason given in the header.
    const auto same_bits = [](float a, float b) {
        uint32_t x = 0;
        uint32_t y = 0;
        std::memcpy(&x, &a, sizeof(x));
        std::memcpy(&y, &b, sizeof(y));
        return x == y;
    };
    for (size_t i = 0; i < obj_pos.size(); ++i) {
        if (!same_bits(obj_pos[i], p->applied_pose.position[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < obj_rot.size(); ++i) {
        if (!same_bits(obj_rot[i], p->applied_pose.rotation[i])) {
            return false;
        }
    }
    return true;
}

namespace {

// Bit equality over a float array, for the reason the accessors give: an epsilon comparison would hide a value
// that has been recomputed to almost-but-not-quite the same thing, which is exactly what these detect.
template <size_t N>
bool bits_equal(const std::array<float, N>& a, const std::array<float, N>& b) {
    for (size_t i = 0; i < N; ++i) {
        uint32_t x = 0;
        uint32_t y = 0;
        std::memcpy(&x, &a[i], sizeof(x));
        std::memcpy(&y, &b[i], sizeof(y));
        if (x != y) {
            return false;
        }
    }
    return true;
}

}  // namespace

PlayerMgr::PoseAgreement PlayerMgr::camera_rotation_agreement(unsigned index) {
    const auto p1 = player(index);
    if (!p1.has_value() || p1->camera_object == 0) {
        return PoseAgreement::Unreadable;
    }
    std::array<float, 4> live1{};
    if (!mem::copy(live1.data(), p1->camera_object + kObjectRotation, sizeof(live1))) {
        return PoseAgreement::Unreadable;
    }

    // THE SECOND READ OF BOTH SIDES. A frame that landed in the window moved one of them, and the comparison
    // below would then be measuring the clock rather than the mapping.
    const auto p2 = player(index);
    if (!p2.has_value() || p2->camera_object != p1->camera_object) {
        return PoseAgreement::Unreadable;
    }
    std::array<float, 4> live2{};
    if (!mem::copy(live2.data(), p2->camera_object + kObjectRotation, sizeof(live2))) {
        return PoseAgreement::Unreadable;
    }
    if (!bits_equal(live1, live2) || !bits_equal(p1->pose.rotation, p2->pose.rotation)) {
        return PoseAgreement::Torn;
    }
    return bits_equal(live1, p1->pose.rotation) ? PoseAgreement::Equal : PoseAgreement::Differ;
}

PlayerMgr::PoseAgreement PlayerMgr::applied_pose_agreement(unsigned index) {
    const auto p1 = player(index);
    if (!p1.has_value() || p1->camera_object == 0) {
        return PoseAgreement::Unreadable;
    }
    std::array<float, 3> pos1{};
    std::array<float, 4> rot1{};
    if (!mem::copy(pos1.data(), p1->camera_object + kObjectPosition, sizeof(pos1)) ||
        !mem::copy(rot1.data(), p1->camera_object + kObjectRotation, sizeof(rot1))) {
        return PoseAgreement::Unreadable;
    }

    const auto p2 = player(index);
    if (!p2.has_value() || p2->camera_object != p1->camera_object) {
        return PoseAgreement::Unreadable;
    }
    std::array<float, 3> pos2{};
    std::array<float, 4> rot2{};
    if (!mem::copy(pos2.data(), p2->camera_object + kObjectPosition, sizeof(pos2)) ||
        !mem::copy(rot2.data(), p2->camera_object + kObjectRotation, sizeof(rot2))) {
        return PoseAgreement::Unreadable;
    }
    if (!bits_equal(pos1, pos2) || !bits_equal(rot1, rot2) ||
        !bits_equal(p1->applied_pose.position, p2->applied_pose.position) ||
        !bits_equal(p1->applied_pose.rotation, p2->applied_pose.rotation)) {
        return PoseAgreement::Torn;
    }
    return (bits_equal(pos1, p1->applied_pose.position) && bits_equal(rot1, p1->applied_pose.rotation))
               ? PoseAgreement::Equal
               : PoseAgreement::Differ;
}

std::optional<bool> PlayerMgr::camera_rotation_matches_pose(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->camera_object == 0) {
        return std::nullopt;
    }

    std::array<float, 4> anchor{};
    if (!mem::copy(anchor.data(), p->camera_object + kObjectRotation, sizeof(anchor))) {
        return std::nullopt;
    }

    // Compared as BITS, not as floats. The point of this accessor is to report whether the two are the same
    // stored value, and an epsilon comparison would hide exactly the case it exists to detect: a rotation
    // that has been recomputed to something almost-but-not-quite equal.
    for (size_t i = 0; i < anchor.size(); ++i) {
        uint32_t a = 0;
        uint32_t b = 0;
        std::memcpy(&a, &anchor[i], sizeof(a));
        std::memcpy(&b, &p->pose.rotation[i], sizeof(b));
        if (a != b) {
            return false;
        }
    }
    return true;
}


std::optional<uintptr_t> PlayerMgr::engine_object(unsigned index) {
    const auto p = slot(index);
    if (!p.has_value() || *p == 0) {
        return std::nullopt;
    }
    const auto holder = mem::read_ptr(*p + kEngineHolderField);
    if (!holder.has_value() || *holder == 0) {
        return std::nullopt;
    }
    const auto object = mem::read_ptr(*holder + kEngineObjectField);
    if (!object.has_value() || *object == 0) {
        return std::nullopt;
    }
    return *object;
}

std::optional<bool> PlayerMgr::engine_object_is_shell_object(unsigned index) {
    const auto mine = engine_object(index);
    const auto theirs = CClientShell::local_player(index);
    if (!mine.has_value() || !theirs.has_value() || theirs->object == nullptr) {
        return std::nullopt;
    }
    return *mine == reinterpret_cast<uintptr_t>(theirs->object);
}

std::optional<uintptr_t> PlayerMgr::movement_controller(unsigned index) {
    const auto p = slot(index);
    if (!p.has_value() || *p == 0) {
        return std::nullopt;
    }
    const auto ctrl = mem::read_ptr(*p + kControllerField);
    if (!ctrl.has_value() || *ctrl == 0) {
        return std::nullopt;
    }
    return *ctrl;
}

std::optional<bool> PlayerMgr::movement_controller_owner_agrees(unsigned index) {
    const auto p = slot(index);
    const auto ctrl = movement_controller(index);
    if (!p.has_value() || !ctrl.has_value()) {
        return std::nullopt;
    }
    const auto back = mem::read_ptr(*ctrl + kControllerOwnerField);
    if (!back.has_value()) {
        return std::nullopt;
    }
    return *back == *p;
}

std::optional<bool> PlayerMgr::engine_object_is_registered(unsigned index) {
    const auto obj = engine_object(index);
    if (!obj.has_value()) {
        return std::nullopt;
    }
    const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(*obj));
    if (!info.has_value()) {
        return std::nullopt;
    }
    return info->handle != 0xFFFF && info->slot_index != 0xFFFFFFFFu;
}



std::optional<float> PlayerMgr::body_origin_height(unsigned index) {
    const auto p = player(index);

    if (!p.has_value() || p->model_object == 0) {
        return std::nullopt;
    }

    std::array<float, 3> model{};

    if (!mem::copy(model.data(), p->model_object + kObjectPosition, sizeof(model))) {
        return std::nullopt;
    }

    return std::isfinite(model[1]) ? std::optional<float>(model[1]) : std::nullopt;
}

std::optional<float> PlayerMgr::eye_height(unsigned index) {
    const auto off = eye_offset(index);

    if (!off.has_value()) {
        return std::nullopt;
    }

    // The VERTICAL component, not the vector's length: a VR consumer placing an eye wants height
    // above the body, and the small horizontal terms (the camera sits slightly back and left) would
    // inflate a magnitude without describing how high anything is.
    return (*off)[1];
}

std::optional<PlayerMgr::MovementState> PlayerMgr::movement_state(unsigned index) {
    const auto ctrl = movement_controller(index);
    if (!ctrl.has_value()) {
        return std::nullopt;
    }
    const auto read_vec = [](uintptr_t at) -> std::optional<std::array<float, 3>> {
        const auto x = mem::read<float>(at);
        const auto y = mem::read<float>(at + 4);
        const auto z = mem::read<float>(at + 8);
        if (!x.has_value() || !y.has_value() || !z.has_value()) {
            return std::nullopt;
        }
        return std::array<float, 3>{*x, *y, *z};
    };
    const auto pos = read_vec(*ctrl + kCachedPositionField);
    const auto vel = read_vec(*ctrl + kVelocityField);
    const auto ext = read_vec(*ctrl + kExternalDeltaField);
    if (!pos.has_value() || !vel.has_value() || !ext.has_value()) {
        return std::nullopt;
    }
    MovementState s;
    s.cached_position = *pos;
    s.velocity = *vel;
    s.external_delta = *ext;
    return s;
}

std::optional<float> PlayerMgr::speed(unsigned index) {
    const auto s = movement_state(index);
    if (!s.has_value()) {
        return std::nullopt;
    }
    const auto& v = s->velocity;
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

PlayerMgr::AgreementCensus PlayerMgr::agreement_census(unsigned index, unsigned which, unsigned samples) {
    AgreementCensus out;
    for (unsigned i = 0; i < samples; ++i) {
        const PoseAgreement a = which == 0   ? camera_rotation_agreement(index)
                                : which == 1 ? applied_pose_agreement(index)
                                             : cached_position_agreement(index);
        switch (a) {
        case PoseAgreement::Equal: ++out.equal; break;
        case PoseAgreement::Differ: ++out.differ; break;
        case PoseAgreement::Torn: ++out.torn; break;
        default: ++out.unreadable; break;
        }
    }
    return out;
}

PlayerMgr::PoseAgreement PlayerMgr::cached_position_agreement(unsigned index) {
    const auto read_pair = [index](std::array<float, 3>& cached, std::array<float, 3>& live) -> bool {
        const auto s = movement_state(index);
        const auto obj = engine_object(index);
        if (!s.has_value() || !obj.has_value()) {
            return false;
        }
        const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(*obj));
        if (!info.has_value()) {
            return false;
        }
        cached = s->cached_position;
        live = {info->position.x, info->position.y, info->position.z};
        return true;
    };

    std::array<float, 3> cached1{}, live1{}, cached2{}, live2{};
    if (!read_pair(cached1, live1)) {
        return PoseAgreement::Unreadable;
    }
    if (!read_pair(cached2, live2)) {
        return PoseAgreement::Unreadable;
    }
    if (!bits_equal(cached1, cached2) || !bits_equal(live1, live2)) {
        return PoseAgreement::Torn;
    }
    return bits_equal(cached1, live1) ? PoseAgreement::Equal : PoseAgreement::Differ;
}

std::optional<bool> PlayerMgr::cached_position_matches_engine(unsigned index) {
    const auto s = movement_state(index);
    const auto obj = engine_object(index);
    if (!s.has_value() || !obj.has_value()) {
        return std::nullopt;
    }
    const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(*obj));
    if (!info.has_value()) {
        return std::nullopt;
    }
    // Compared as BITS: the controller copies the engine's floats verbatim, so equality is the claim. An
    // epsilon here would accept a recomputed position and hide a wrong offset.
    const auto& a = s->cached_position;
    const std::array<float, 3> b{info->position.x, info->position.y, info->position.z};
    return std::memcmp(a.data(), b.data(), sizeof(b)) == 0;
}


std::optional<PlayerMgr::EngineObjects> PlayerMgr::engine_objects(unsigned index) {
    const auto p = player(index);
    if (!p.has_value()) {
        return std::nullopt;
    }
    EngineObjects out;
    out.camera = p->camera_object;
    out.model = p->model_object;
    if (const auto lp = CClientShell::local_player(index); lp.has_value()) {
        out.shell = reinterpret_cast<uintptr_t>(lp->object);
    }
    return out;
}

std::optional<bool> PlayerMgr::engine_object_is_model_object(unsigned index) {
    const auto phys = engine_object(index);
    const auto p = player(index);
    if (!phys.has_value() || !p.has_value() || p->model_object == 0) {
        return std::nullopt;
    }
    return *phys == p->model_object;
}


std::optional<PlayerMgr::PlatformCarry> PlayerMgr::platform_carry(unsigned index) {
    const auto ctrl = movement_controller(index);
    if (!ctrl.has_value()) {
        return std::nullopt;
    }
    PlatformCarry out;
    out.object = mem::read_ptr(*ctrl + kStandingOnField).value_or(0);
    out.active = out.object != 0;
    if (!out.active) {
        return out;  // the triple is stale leftover data while idle; do not offer it
    }
    std::array<float, 3> pos{};
    for (size_t i = 0; i < pos.size(); ++i) {
        const auto v = mem::read<float>(*ctrl + kStandingOnPositionField + i * sizeof(float));
        if (!v.has_value()) {
            return std::nullopt;
        }
        pos[i] = *v;
    }
    out.last_position = pos;
    return out;
}

std::optional<bool> PlayerMgr::platform_carry_position_current(unsigned index) {
    const auto carry = platform_carry(index);
    if (!carry.has_value() || !carry->active) {
        return std::nullopt;  // nothing being ridden, so nothing to compare
    }
    const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(carry->object));
    if (!info.has_value()) {
        return std::nullopt;
    }
    // Compared as BITS: the carry step assigns the position verbatim, so equality is the claim.
    if (!carry->last_position.has_value()) {
        return std::nullopt;
    }
    const std::array<float, 3> cur{info->position.x, info->position.y, info->position.z};
    return std::memcmp(carry->last_position->data(), cur.data(), sizeof(cur)) == 0;
}


std::optional<PlayerMgr::CameraRotationOperands> PlayerMgr::camera_rotation_operands(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0 || p->camera_object == 0) {
        return std::nullopt;
    }
    const auto read_quat = [](uintptr_t at) -> std::optional<std::array<float, 4>> {
        std::array<float, 4> q{};
        if (!mem::copy(q.data(), at, sizeof(q))) {
            return std::nullopt;
        }
        return q;
    };
    const auto outer = read_quat(p->holder + kCameraRotationOuter);
    const auto inner = read_quat(p->holder + kCameraRotationInner);
    if (!outer.has_value() || !inner.has_value()) {
        return std::nullopt;
    }
    const auto* cam = reinterpret_cast<const regenny::LTObject*>(p->camera_object);
    const auto info = object_info(cam);
    if (!info.has_value()) {
        return std::nullopt;
    }
    CameraRotationOperands out;
    out.outer = *outer;
    out.inner = *inner;
    regenny::LTRotation a{};
    regenny::LTRotation b{};
    a.x = out.outer[0]; a.y = out.outer[1]; a.z = out.outer[2]; a.w = out.outer[3];
    b.x = out.inner[0]; b.y = out.inner[1]; b.z = out.inner[2]; b.w = out.inner[3];
    const auto prod = multiply_rotations(a, b);
    out.composed = {prod.x, prod.y, prod.z, prod.w};
    out.actual = {info->rotation.x, info->rotation.y, info->rotation.z, info->rotation.w};
    return out;
}

std::optional<bool> PlayerMgr::camera_rotation_is_composed(unsigned index, float tolerance) {
    const auto ops = camera_rotation_operands(index);
    if (!ops.has_value()) {
        return std::nullopt;
    }
    // A quaternion and its negation are the same rotation, so both signs are accepted -- rejecting one would
    // report a correct composition as wrong roughly half the time.
    float same = 0.0f;
    float flipped = 0.0f;
    for (size_t i = 0; i < 4; ++i) {
        same += std::fabs(ops->composed[i] - ops->actual[i]);
        flipped += std::fabs(ops->composed[i] + ops->actual[i]);
    }
    return std::min(same, flipped) <= tolerance * 4.0f;
}


std::optional<bool> PlayerMgr::camera_attachment_driving(unsigned index) {
    const auto ops = camera_rotation_operands(index);
    if (!ops.has_value()) {
        return std::nullopt;
    }
    const auto& q = ops->outer;
    const bool identity = std::fabs(q[0]) < 1e-4f && std::fabs(q[1]) < 1e-4f && std::fabs(q[2]) < 1e-4f &&
                          std::fabs(std::fabs(q[3]) - 1.0f) < 1e-4f;
    return !identity;
}

// PlayerCamera_UpdateAttachedRotation @ gameclient.dll 0x100DED80. __thiscall; `this` is the camera holder,
// the same object CPlayerCamera_ClampPitch reads +324 from. Prologue only, with the trailing call's
// displacement left off so the signature carries no link-time offset:
//   sub esp,88h / push ebx / push ebp / mov ebp,ecx / mov eax,[ebp+4] / mov ecx,[eax+0ECh] /
//   mov ecx,[ecx+524h] / push esi / push edi
static constexpr const char* kCameraAttachedRotation =
    "81 EC 88 00 00 00 53 55 8B E9 8B 45 04 8B 88 EC 00 00 00 8B 89 24 05 00 00 56 57";

std::optional<PlayerMgr::CameraRotationOperands> PlayerMgr::camera_rotation_operands_from_holder(
    uintptr_t holder) {
    if (holder == 0) {
        return std::nullopt;
    }
    CameraRotationOperands out{};
    if (!mem::copy(out.outer.data(), holder + kCameraRotationOuter, sizeof(float) * 4) ||
        !mem::copy(out.inner.data(), holder + kCameraRotationInner, sizeof(float) * 4)) {
        return std::nullopt;
    }
    // Same conversion the index form uses -- multiply_rotations takes LTRotation, and the operands are stored
    // here as plain float quads.
    regenny::LTRotation a{};
    regenny::LTRotation b{};
    a.x = out.outer[0]; a.y = out.outer[1]; a.z = out.outer[2]; a.w = out.outer[3];
    b.x = out.inner[0]; b.y = out.inner[1]; b.z = out.inner[2]; b.w = out.inner[3];
    const auto prod = multiply_rotations(a, b);
    out.composed = {prod.x, prod.y, prod.z, prod.w};
    return out;
}

std::optional<PlayerMgr::Locomotion> PlayerMgr::locomotion(unsigned index) {
    const auto ms = movement_state(index);
    const auto avd = aim_vs_view(index);
    if (!ms.has_value() || !avd.has_value()) {
        return std::nullopt;
    }
    Locomotion out{};
    out.velocity = ms->velocity;
    out.speed = std::sqrt(ms->velocity[0] * ms->velocity[0] + ms->velocity[2] * ms->velocity[2]);
    // Below this the bearing is numerical noise rather than a direction. 1 unit/second against a
    // walk of ~312 is comfortably inside the dead zone and well above float jitter at rest.
    out.moving = out.speed > 1.0f;
    if (!out.moving) {
        return out;
    }
    out.bearing = std::atan2(ms->velocity[0], ms->velocity[2]);

    auto wrap = [](float a) {
        constexpr float kPi = 3.14159265358979f;
        while (a > kPi) {
            a -= 2.0f * kPi;
        }
        while (a < -kPi) {
            a += 2.0f * kPi;
        }
        return a;
    };
    const float aim_h = std::atan2(avd->aim_forward[0], avd->aim_forward[2]);
    const float view_h = std::atan2(avd->view_forward[0], avd->view_forward[2]);
    out.bearing_to_aim = wrap(out.bearing - aim_h);
    out.bearing_to_view = wrap(out.bearing - view_h);
    return out;
}

namespace {

// CPlayerCamera_GetActiveCameraClamp -- gameclient.dll .text+0xDE160.
//
//   fld  ds:flt_101D59C8      <- 85.0f, the no-record fallback
//   sub  esp, 8
//   push esi
//   mov  esi, [esp+0Ch+arg_0]
//   push edi
//   fst  dword ptr [esi]      <- out[0] = 85.0
//   mov  edi, ecx
//   fstp dword ptr [esi+4]    <- out[1] = 85.0
//   mov  eax, [edi+18BCh]     <- this+6332, the CameraClamping record
//   test eax, eax
//
// The four bytes of the fld operand are an absolute address and are wildcarded.
constexpr const char* kGetActiveCameraClamp =
    "D9 05 ? ? ? ? 83 EC 08 56 8B 74 24 10 57 D9 16 8B F9 D9 5E 04 8B 87 BC 18 00 00 85 C0";

using GetActiveCameraClampFn = float*(__thiscall*)(void* self, float* out);

GetActiveCameraClampFn get_active_camera_clamp_fn() {
    // RETRYABLE, not latched-on-failure: gameclient is a separately loaded module and a caller
    // reaching here before Modules::initialize() must be able to succeed later (AGENT.MD rule 5).
    static GetActiveCameraClampFn s_fn = nullptr;

    if (s_fn == nullptr) {
        const auto addr = Modules::get().scan_game_client(kGetActiveCameraClamp,
                                                          "CPlayerCamera::GetActiveCameraClamp");
        s_fn = reinterpret_cast<GetActiveCameraClampFn>(addr);
    }

    return s_fn;
}

// SEH-ISOLATED: calling into gameclient with a holder that has just been freed is a fault, not a
// null. Its own free function with POD-only locals and a POD return, because MSVC C2712 forbids
// __try in any scope holding a non-trivial local (AGENT.MD rule 6).
bool seh_get_camera_clamp(GetActiveCameraClampFn fn, uintptr_t holder, float* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        fn(reinterpret_cast<void*>(holder), out);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

} // namespace

namespace {

// CPlayerCamera_ApplyLookToRotation -- gameclient.dll .text+0xE0830.
//
//   sub esp, 20h ; push esi ; mov esi, ecx ; mov ecx, [esp+24h+arg_0]
//   fld dword ptr [ecx] ; fldz ; fucom st(1) ; fnstsw ax ; fstp st(1) ; test ah, 44h
//
// No absolute addresses, so nothing needs wildcarding.
constexpr const char* kApplyLookToRotation =
    "83 EC 20 56 8B F1 8B 4C 24 28 D9 01 D9 EE DD E1 DF E0 DD D9 F6 C4 44 7A 1A D9 41 04 DD E9 DF E0";

using ApplyLookToRotationFn = void(__thiscall*)(void* camera, float* delta);

ApplyLookToRotationFn apply_look_fn() {
    // RETRYABLE: gameclient may not be mapped yet (AGENT.MD rule 5).
    static ApplyLookToRotationFn s_fn = nullptr;

    if (s_fn == nullptr) {
        s_fn = reinterpret_cast<ApplyLookToRotationFn>(
            Modules::get().scan_game_client(kApplyLookToRotation, "CPlayerCamera::ApplyLookToRotation"));
    }

    return s_fn;
}

bool seh_apply_look(ApplyLookToRotationFn fn, uintptr_t camera, float* delta) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        fn(reinterpret_cast<void*>(camera), delta);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

} // namespace

bool PlayerMgr::apply_look_delta(unsigned index, float pitch_radians, float yaw_radians) {
    auto* fn = apply_look_fn();

    if (fn == nullptr) {
        return false;
    }

    const auto p = player(index);

    if (!p.has_value() || p->holder == 0) {
        return false;
    }

    // The engine early-outs on an all-zero delta, so a caller passing zeros gets a truthful "the
    // call happened and changed nothing" rather than a failure.
    // Component 0 is pitch and grows DOWNWARD; component 1 is yaw; component 2 does nothing
    // measurable. Negating here keeps the public signature in aim_pitch's upward-positive
    // convention -- see the header for the measurement.
    float delta[3]{-pitch_radians, yaw_radians, 0.0f};

    return seh_apply_look(fn, p->holder, delta);
}

std::optional<PlayerMgr::PitchLimits> PlayerMgr::pitch_limits(unsigned index) {
    auto* fn = get_active_camera_clamp_fn();

    if (fn == nullptr) {
        return std::nullopt;
    }

    const auto p = player(index);

    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }

    float out[2]{};

    if (!seh_get_camera_clamp(fn, p->holder, out)) {
        return std::nullopt;
    }

    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) {
        return std::nullopt;
    }

    // SIGN, AND WHY IT IS NOT A GUESS. The engine writes out[0] = degreesA * +(pi/180) and
    // out[1] = degreesB * -(pi/180) -- the negation is in the function itself. With the shipped
    // StandIdle record (80/85) that is out[0] = +1.396, out[1] = -1.484, i.e. the engine's pitch
    // grows DOWNWARD. `aim_pitch` grows upward, so both flip.
    PitchLimits limits{};
    limits.up = -out[1];
    limits.down = -out[0];

    if (limits.up < limits.down) {
        // A record with the pair the other way round would otherwise hand back an empty interval
        // that silently rejects every pitch. Fail closed instead of lying.
        return std::nullopt;
    }

    return limits;
}

std::optional<float> PlayerMgr::aim_pitch(unsigned index) {
    const auto d = aim_vs_view(index);
    if (!d.has_value()) {
        return std::nullopt;
    }
    float fy = d->aim_forward[1];
    fy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    return std::asin(fy);
}

std::optional<float> PlayerMgr::aim_yaw(unsigned index) {
    const auto d = aim_vs_view(index);
    if (!d.has_value()) {
        return std::nullopt;
    }
    return std::atan2(d->aim_forward[0], d->aim_forward[2]);
}

std::optional<PlayerMgr::AimViewDivergence> PlayerMgr::aim_vs_view(unsigned index) {
    const auto ops = camera_rotation_operands(index);
    if (!ops.has_value()) {
        return std::nullopt;
    }
    auto as_rot = [](const std::array<float, 4>& q) {
        regenny::LTRotation r{};
        r.x = q[0];
        r.y = q[1];
        r.z = q[2];
        r.w = q[3];
        return r;
    };
    const auto view = forward_of(as_rot(ops->actual));
    const auto aim = forward_of(as_rot(ops->inner));

    AimViewDivergence out{};
    out.view_forward = {view.x, view.y, view.z};
    out.aim_forward = {aim.x, aim.y, aim.z};

    // The outer operand is identity when nothing is leaning, shaking or head-tracking the view.
    // Compared against 1 rather than against zero components, so a sign convention cannot fool it.
    out.composed = std::fabs(ops->outer[3]) < 0.99999f || std::fabs(ops->outer[0]) > 1e-4f ||
                   std::fabs(ops->outer[1]) > 1e-4f || std::fabs(ops->outer[2]) > 1e-4f;

    auto angle_between = [](const regenny::LTVector& a, const regenny::LTVector& b) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z;
        d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
        return std::acos(d);
    };
    out.angle = angle_between(view, aim);

    // The body's own facing, from the player object's transform rather than from either operand.
    if (const auto p = player(index); p.has_value() && p->object != 0) {
        if (const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(p->object))) {
            const auto body = forward_of(info->rotation);
            out.body_forward = {body.x, body.y, body.z};
            out.body_to_view_angle = angle_between(body, view);
            out.body_to_aim_angle = angle_between(body, aim);
            out.body_readable = true;
        }
    }

    // The OTHER player object -- the shell's -- read through the same instant.
    if (const auto shell = CClientShell::local_player(0); shell.has_value() && shell->object != nullptr) {
        if (const auto info = object_info(shell->object)) {
            const auto sf = forward_of(info->rotation);
            out.shell_forward = {sf.x, sf.y, sf.z};
            out.shell_to_view_angle = angle_between(sf, view);
            out.shell_to_aim_angle = angle_between(sf, aim);
            out.shell_readable = true;
            if (const auto p = player(index); p.has_value()) {
                out.shell_is_body = reinterpret_cast<uintptr_t>(shell->object) == p->object;
            }
        }
    }
    return out;
}

std::optional<float> PlayerMgr::aim_roll(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    std::array<float, 4> q{};
    if (!mem::copy(q.data(), p->holder + kCameraRotationInner, sizeof(q))) {
        return std::nullopt;
    }
    // Guarded against the degenerate quaternion rather than trusting the field: an all-zero read would
    // otherwise report a confident 0 that actually means "unreadable".
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 0.5f) || !(n < 1.5f)) {
        return std::nullopt;
    }
    // MEASURED AS THE RIGHT VECTOR'S DEPARTURE FROM HORIZONTAL, deliberately not as an Euler term.
    //
    // The first version of this used the textbook atan2 roll extraction and read a rock-steady +180.000 deg,
    // which looks like a finding and is an artefact: that formula assumes Z-up, this engine is Y-UP, and for
    // a level view its two arguments degenerate to atan2(0, cos(yaw)) -- which flips to pi the moment the
    // player faces backwards. A constant that tracks nothing is the tell.
    //
    // Rotating (1,0,0) by the quaternion and taking the Y component needs no convention at all: the right
    // vector leaves the horizontal plane exactly when the view is rolled, whatever the Euler order is.
    const float right_y = 2.0f * (q[0] * q[1] + q[3] * q[2]);
    return std::asin(right_y < -1.0f ? -1.0f : (right_y > 1.0f ? 1.0f : right_y));
}

uintptr_t PlayerMgr::camera_attached_rotation_fn() {
    static const uintptr_t s_fn =
        Modules::get().scan_game_client(kCameraAttachedRotation, "PlayerCamera_UpdateAttachedRotation");
    return s_fn;
}

bool PlayerMgr::write_camera_outer_rotation(uintptr_t holder, const std::array<float, 4>& rotation) {
    if (holder == 0) {
        return false;
    }
    for (float v : rotation) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return mem::store(holder + kCameraRotationOuter, rotation.data(), sizeof(float) * 4);
}

std::optional<PlayerMgr::OuterOperandProbe> PlayerMgr::probe_outer_operand(unsigned index, unsigned samples) {
    return probe_holder_quaternion(index, kCameraRotationOuter, true, samples);
}

std::optional<PlayerMgr::OuterOperandProbe> PlayerMgr::probe_holder_quaternion(unsigned index,
                                                                              uintptr_t holder_offset,
                                                                              bool expect_composed_with_inner,
                                                                              unsigned samples) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0 || p->camera_object == 0) {
        return std::nullopt;
    }
    const auto at = p->holder + holder_offset;
    std::array<float, 4> saved{};
    if (!mem::copy(saved.data(), at, sizeof(saved))) {
        return std::nullopt;
    }
    std::array<float, 4> inner{};
    if (!mem::copy(inner.data(), p->holder + kCameraRotationInner, sizeof(inner))) {
        return std::nullopt;
    }
    // A 90-degree yaw: unmistakably not identity, and harmless for the frames it lasts.
    const std::array<float, 4> probe{0.0f, 0.70710678f, 0.0f, 0.70710678f};
    if (!mem::write<std::array<float, 4>>(at, probe)) {
        return std::nullopt;
    }
    std::array<float, 4> exp = probe;
    if (expect_composed_with_inner) {
        regenny::LTRotation a{};
        regenny::LTRotation b{};
        a.x = probe[0]; a.y = probe[1]; a.z = probe[2]; a.w = probe[3];
        b.x = inner[0]; b.y = inner[1]; b.z = inner[2]; b.w = inner[3];
        const auto prod = multiply_rotations(a, b);
        exp = {prod.x, prod.y, prod.z, prod.w};
    }

    OuterOperandProbe out;
    out.samples = samples;
    std::vector<float> frames;
    for (unsigned i = 0; i < samples; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        // The render-path clock is sampled INSIDE the loop, so the verdict covers the same window as the writes.
        if (const auto t = ShaderParams::frame_time();
            t.has_value() && std::find(frames.begin(), frames.end(), *t) == frames.end()) {
            frames.push_back(*t);
        }
        std::array<float, 4> now{};
        if (!mem::copy(now.data(), at, sizeof(now))) {
            break;
        }
        if (std::memcmp(now.data(), probe.data(), sizeof(probe)) != 0) {
            break;  // reclaimed by whatever writes this field
        }
        ++out.survived;
        if (!out.followed) {
            if (const auto info = object_info(reinterpret_cast<const regenny::LTObject*>(p->camera_object));
                info.has_value()) {
                const std::array<float, 4> act{info->rotation.x, info->rotation.y, info->rotation.z,
                                               info->rotation.w};
                float same = 0.0f;
                float flip = 0.0f;
                for (size_t k = 0; k < 4; ++k) {
                    same += std::fabs(exp[k] - act[k]);
                    flip += std::fabs(exp[k] + act[k]);
                }
                if (std::min(same, flip) <= 0.01f) {
                    out.followed = true;
                }
            }
        }
    }
    // RESTORE ONLY WHAT WE STILL OWN. If the value was reclaimed, the engine has already written something
    // NEWER than our saved copy, and putting the copy back would clobber live state with a stale rotation --
    // which is visible in game and breaks the camera-pose equality invariants the suite checks. Measured: a
    // probe run left four checks red until this became conditional.
    //
    // So the restore is a compare-and-set: only if the field still holds exactly what we wrote is it ours to
    // put back. Otherwise the engine owns it again and the right action is none.
    {
        std::array<float, 4> current{};
        if (mem::copy(current.data(), at, sizeof(current)) &&
            std::memcmp(current.data(), probe.data(), sizeof(probe)) == 0) {
            mem::write<std::array<float, 4>>(at, saved);
        }
    }
    // A SURVIVING VALUE MEANS NOTHING WITHOUT RENDERED FRAMES: with the render path frozen nothing could
    // have overwritten it, so the verdict stays Inconclusive however long it lasted.
    //
    // INCONCLUSIVE MEANS "NO INFORMATION", AND RECLAIMED-IMMEDIATELY IS NOT THAT.
    //
    // The old rule was `frames_observed <= 1 -> Inconclusive`, meant to catch a frozen render path. But the
    // loop BREAKS the moment the value is overwritten, so a write reclaimed inside the first frame also exits
    // with one frame time recorded -- and got reported as Inconclusive. Those are opposite outcomes: one is
    // "nothing could have overwritten it", the other is "something overwrote it at once", which is precisely
    // the answer a consumer wanting to steer the view needs.
    //
    // So the frame clock is read AFTER the loop regardless of how it ended, and the question becomes whether
    // the render path advanced across the probe's own window. A value that vanished while frames were being
    // produced was reclaimed by a producer, whatever the sample count says.
    // SLEEP FIRST, or this lands in the same frame as the last in-loop sample and observes nothing. The loop
    // breaks on reclaim, so on a running game it typically took exactly ONE sample -- and two reads
    // microseconds apart cannot see a frame boundary however live the path is.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (const auto t_end = ShaderParams::frame_time();
        t_end.has_value() && std::find(frames.begin(), frames.end(), *t_end) == frames.end()) {
        frames.push_back(*t_end);
    }
    out.frames_observed = static_cast<unsigned>(frames.size());
    out.verdict = out.frames_observed <= 1  ? ProbeVerdict::Inconclusive
                  : out.survived == samples ? ProbeVerdict::Held
                                            : ProbeVerdict::Reclaimed;
    return out;
}


std::optional<PlayerMgr::OuterOperandProbe> PlayerMgr::probe_camera_object_rotation(unsigned index,
                                                                                   unsigned samples) {
    const auto p = player(index);
    if (!p.has_value() || p->camera_object == 0) {
        return std::nullopt;
    }
    // LTObject.rotation, the field the engine's own SetObjectRotation writes.
    constexpr uintptr_t kObjectRotation = 0x20;
    const auto at = p->camera_object + kObjectRotation;
    std::array<float, 4> saved{};
    if (!mem::copy(saved.data(), at, sizeof(saved))) {
        return std::nullopt;
    }
    const std::array<float, 4> probe{0.0f, 0.70710678f, 0.0f, 0.70710678f};
    if (!mem::write<std::array<float, 4>>(at, probe)) {
        return std::nullopt;
    }
    OuterOperandProbe out;
    out.samples = samples;
    std::vector<float> frames;
    for (unsigned i = 0; i < samples; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        // The render-path clock is sampled INSIDE the loop, so the verdict covers the same window as the writes.
        if (const auto t = ShaderParams::frame_time();
            t.has_value() && std::find(frames.begin(), frames.end(), *t) == frames.end()) {
            frames.push_back(*t);
        }
        std::array<float, 4> now{};
        if (!mem::copy(now.data(), at, sizeof(now))) {
            break;
        }
        if (std::memcmp(now.data(), probe.data(), sizeof(probe)) != 0) {
            break;
        }
        ++out.survived;
    }
    out.followed = out.survived == samples;
    // RESTORE ONLY WHAT WE STILL OWN. If the value was reclaimed, the engine has already written something
    // NEWER than our saved copy, and putting the copy back would clobber live state with a stale rotation --
    // which is visible in game and breaks the camera-pose equality invariants the suite checks. Measured: a
    // probe run left four checks red until this became conditional.
    //
    // So the restore is a compare-and-set: only if the field still holds exactly what we wrote is it ours to
    // put back. Otherwise the engine owns it again and the right action is none.
    {
        std::array<float, 4> current{};
        if (mem::copy(current.data(), at, sizeof(current)) &&
            std::memcmp(current.data(), probe.data(), sizeof(probe)) == 0) {
            mem::write<std::array<float, 4>>(at, saved);
        }
    }
    // A SURVIVING VALUE MEANS NOTHING WITHOUT RENDERED FRAMES: with the render path frozen nothing could
    // have overwritten it, so the verdict stays Inconclusive however long it lasted.
    //
    // INCONCLUSIVE MEANS "NO INFORMATION", AND RECLAIMED-IMMEDIATELY IS NOT THAT.
    //
    // The old rule was `frames_observed <= 1 -> Inconclusive`, meant to catch a frozen render path. But the
    // loop BREAKS the moment the value is overwritten, so a write reclaimed inside the first frame also exits
    // with one frame time recorded -- and got reported as Inconclusive. Those are opposite outcomes: one is
    // "nothing could have overwritten it", the other is "something overwrote it at once", which is precisely
    // the answer a consumer wanting to steer the view needs.
    //
    // So the frame clock is read AFTER the loop regardless of how it ended, and the question becomes whether
    // the render path advanced across the probe's own window. A value that vanished while frames were being
    // produced was reclaimed by a producer, whatever the sample count says.
    // SLEEP FIRST, or this lands in the same frame as the last in-loop sample and observes nothing. The loop
    // breaks on reclaim, so on a running game it typically took exactly ONE sample -- and two reads
    // microseconds apart cannot see a frame boundary however live the path is.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (const auto t_end = ShaderParams::frame_time();
        t_end.has_value() && std::find(frames.begin(), frames.end(), *t_end) == frames.end()) {
        frames.push_back(*t_end);
    }
    out.frames_observed = static_cast<unsigned>(frames.size());
    out.verdict = out.frames_observed <= 1  ? ProbeVerdict::Inconclusive
                  : out.survived == samples ? ProbeVerdict::Held
                                            : ProbeVerdict::Reclaimed;
    return out;
}


namespace {

constexpr const char* kCameraOffsetVars[] = {"CameraAttachedOffsetX", "CameraAttachedOffsetY",
                                             "CameraAttachedOffsetZ"};

}  // namespace

std::optional<std::array<float, 3>> PlayerMgr::camera_attached_offset() {
    std::array<float, 3> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        const auto var = Engine::find_cached_var(kCameraOffsetVars[i]);
        if (!var.has_value()) {
            return std::nullopt;
        }
        const auto v = Engine::read_cached(*var);
        if (!v.has_value()) {
            return std::nullopt;
        }
        out[i] = *v;
    }
    return out;
}

bool PlayerMgr::set_camera_attached_offset(const std::array<float, 3>& offset) {
    bool ok = true;
    for (size_t i = 0; i < offset.size(); ++i) {
        const auto var = Engine::find_cached_var(kCameraOffsetVars[i]);
        if (!var.has_value() || !Engine::write_cached(*var, offset[i])) {
            ok = false;
        }
    }
    return ok;
}

std::optional<size_t> PlayerMgr::camera_socket_index(unsigned index, const char* name) {
    if (name == nullptr) {
        return std::nullopt;
    }
    const auto p = player(index);
    if (!p.has_value() || p->model_object == 0) {
        return std::nullopt;
    }
    const auto skel = ModelSkeleton::from_object(reinterpret_cast<const regenny::LTObject*>(p->model_object));
    if (!skel.has_value()) {
        return std::nullopt;
    }
    return skel->find_socket(name);
}


std::optional<PlayerMgr::CameraFov> PlayerMgr::camera_fov(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto a = mem::read<float>(p->holder + kCameraFovPair);
    const auto b = mem::read<float>(p->holder + kCameraFovPair + sizeof(float));
    if (!a.has_value() || !b.has_value()) {
        return std::nullopt;
    }
    CameraFov out;
    out.fov_x = *a;
    out.fov_y = *b;
    return out;
}

namespace {

std::optional<bool> compare_fov_against_projection(std::optional<float> field, bool horizontal, float tolerance) {
    if (!field.has_value() || !std::isfinite(tolerance) || tolerance < 0.0f) {
        return std::nullopt;
    }
    const auto snap = SceneCamera::snapshot();
    if (!snap.has_value()) {
        return std::nullopt;
    }
    const auto proj = horizontal ? snap->fov_x_radians() : snap->fov_y_radians();
    if (!proj.has_value()) {
        return std::nullopt;  // not a perspective pass, so there is nothing to compare against
    }
    return std::fabs(*field - *proj) <= tolerance;
}

}  // namespace

std::optional<bool> PlayerMgr::fov_y_matches_projection(unsigned index, float tolerance) {
    const auto fov = camera_fov(index);
    if (!fov.has_value()) {
        return std::nullopt;
    }
    return compare_fov_against_projection(fov->fov_y, false, tolerance);
}

std::optional<bool> PlayerMgr::fov_x_matches_projection(unsigned index, float tolerance) {
    const auto fov = camera_fov(index);
    if (!fov.has_value()) {
        return std::nullopt;
    }
    return compare_fov_against_projection(fov->fov_x, true, tolerance);
}

std::optional<bool> PlayerMgr::cinematic_active(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto flag = mem::read<uint8_t>(p->holder + kCinematicActiveFlag);
    if (!flag.has_value()) {
        return std::nullopt;
    }
    return *flag != 0;
}

std::optional<float> PlayerMgr::saved_near_z(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    return mem::read<float>(p->holder + kSavedNearZ);
}

std::optional<size_t> PlayerMgr::cinematic_camera_count() {
    const auto mgr = manager();
    if (mgr == 0) {
        return std::nullopt;
    }
    const auto begin = mem::read_ptr(mgr + kCinematicVectorBegin);
    const auto end = mem::read_ptr(mgr + kCinematicVectorEnd);
    if (!begin.has_value() || !end.has_value() || *begin == 0 || *end < *begin) {
        return std::nullopt;
    }
    const auto span = *end - *begin;
    // A vector of pointers, so the span must divide evenly and stay within reason -- a garbage pair would
    // otherwise report a plausible count.
    if (span % sizeof(uintptr_t) != 0 || span > 0x10000) {
        return std::nullopt;
    }
    return span / sizeof(uintptr_t);
}


std::optional<float> PlayerMgr::aspect_ratio(unsigned index) {
    const auto fov = camera_fov(index);
    if (!fov.has_value()) {
        return std::nullopt;
    }
    // Both angles must be inside the engine's own clamp for the ratio to mean anything: at the clamp the setting
    // was out of range, and at zero the tangent ratio is undefined.
    if (!(fov->fov_x > 0.0f && fov->fov_x < kFovClampRadians && fov->fov_y > 0.0f &&
          fov->fov_y < kFovClampRadians)) {
        return std::nullopt;
    }
    const auto ty = std::tan(fov->fov_y * 0.5f);
    if (!(ty > 0.0f)) {
        return std::nullopt;
    }
    return std::tan(fov->fov_x * 0.5f) / ty;
}


std::optional<PlayerMgr::ViewportRect> PlayerMgr::viewport_rect(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    ViewportRect out;
    for (size_t i = 0; i < 8; ++i) {
        const auto v = mem::read<int32_t>(p->holder + 196 + i * sizeof(int32_t));
        if (!v.has_value()) {
            return std::nullopt;
        }
        out.fields[i] = *v;
    }
    // The producer's own expression, indices relative to this[49]: numerator uses [4],[2] minus [6],[0];
    // denominator uses [3],[5] minus [7],[1].
    out.width = out.fields[4] + out.fields[2] - out.fields[6] - out.fields[0];
    out.height = out.fields[3] + out.fields[5] - out.fields[7] - out.fields[1];
    return out;
}

std::optional<PlayerMgr::FovInputs> PlayerMgr::fov_inputs(unsigned index) {
    const auto fov_var = Engine::find_cached_var("FovY");
    const auto scale_var = Engine::find_cached_var("FovAspectRatioScale");
    if (!fov_var.has_value() || !scale_var.has_value()) {
        return std::nullopt;
    }
    const auto fov = Engine::read_cached(*fov_var);
    const auto scale = Engine::read_cached(*scale_var);
    const auto rect = viewport_rect(index);
    if (!fov.has_value() || !scale.has_value() || !rect.has_value()) {
        return std::nullopt;
    }
    FovInputs out;
    out.fov_y_degrees = *fov;
    out.aspect_scale = *scale;
    // The engine falls back to 16:9 when the denominator is zero, so this reproduces that rather than failing.
    out.aspect = rect->height != 0 ? static_cast<float>(rect->width) / static_cast<float>(rect->height)
                                   : 1.7777778f;
    return out;
}

std::optional<bool> PlayerMgr::fov_derivation_holds(unsigned index, float tolerance) {
    const auto in = fov_inputs(index);
    const auto pair = camera_fov(index);
    if (!in.has_value() || !pair.has_value() || !std::isfinite(tolerance) || tolerance < 0.0f) {
        return std::nullopt;
    }
    const auto clamp = [](float v) {
        if (!(v >= 0.0f)) {
            return 0.0f;
        }
        return v > kFovClampRadians ? kFovClampRadians : v;
    };
    constexpr float kDegToRad = 0.01745329238474369f;
    const auto want_y = clamp(in->fov_y_degrees * kDegToRad);
    const auto want_x =
        clamp(2.0f * std::atan(std::tan(want_y * 0.5f) * in->aspect) * in->aspect_scale);
    return std::fabs(want_y - pair->fov_y) <= tolerance && std::fabs(want_x - pair->fov_x) <= tolerance;
}


bool PlayerMgr::HeightSmoothing::is_effective() const {
    if (enabled != 1.0f) {
        return false;
    }
    // The producer clamps the rate to 1.0, and a rate of exactly 1.0 makes the lerp land on the target -- so a
    // speed at or above the clamp smooths nothing.
    const auto up = up_speed < 1.0f ? up_speed : 1.0f;
    const auto down = down_speed < 1.0f ? down_speed : 1.0f;
    return up < 1.0f || down < 1.0f;
}

std::optional<PlayerMgr::HeightSmoothing> PlayerMgr::camera_height_smoothing(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto gate = Engine::find_cached_var("CameraSmoothingEnabled");
    const auto up = Engine::find_cached_var("CameraHeightInterpSpeedUp");
    const auto down = Engine::find_cached_var("CameraHeightInterpSpeedDown");
    if (!gate.has_value() || !up.has_value() || !down.has_value()) {
        return std::nullopt;
    }
    const auto gv = Engine::read_cached(*gate);
    const auto uv = Engine::read_cached(*up);
    const auto dv = Engine::read_cached(*down);
    const auto flag = mem::read<uint8_t>(p->holder + kSmoothingHasPrevious);
    const auto prev = mem::read<float>(p->holder + kSmoothingPreviousHeight);
    const auto delta = mem::read<float>(p->holder + kSmoothingAppliedDelta);
    if (!gv.has_value() || !uv.has_value() || !dv.has_value() || !flag.has_value() || !prev.has_value() ||
        !delta.has_value()) {
        return std::nullopt;
    }
    HeightSmoothing out;
    out.enabled = *gv;
    out.up_speed = *uv;
    out.down_speed = *dv;
    out.has_previous = *flag != 0;
    out.previous_height = *prev;
    out.applied_delta = *delta;
    return out;
}

bool PlayerMgr::set_camera_smoothing_enabled(bool enabled) {
    const auto gate = Engine::find_cached_var("CameraSmoothingEnabled");
    if (!gate.has_value()) {
        return false;
    }
    return Engine::write_cached(*gate, enabled ? 1.0f : 0.0f);
}


std::optional<bool> PlayerMgr::holder_is_player_camera(unsigned index) {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return std::nullopt;
    }
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto vt = mem::read_ptr(p->holder);
    if (!vt.has_value()) {
        return std::nullopt;
    }
    return *vt == gc->base + kPlayerCameraVtable;
}


std::optional<PlayerMgr::CameraSubObjects> PlayerMgr::camera_sub_objects(unsigned index) {
    const auto p = slot(index);
    if (!p.has_value() || *p == 0) {
        return std::nullopt;
    }
    const auto ctrl = mem::read_ptr(*p + kControllerField);
    const auto cam = mem::read_ptr(*p + kHolder);
    const auto phys = mem::read_ptr(*p + kEngineHolderField);
    if (!ctrl.has_value() || !cam.has_value() || !phys.has_value()) {
        return std::nullopt;
    }
    CameraSubObjects out;
    out.controller = *ctrl;
    out.player_camera = *cam;
    out.physics_holder = *phys;
    return out;
}

std::optional<bool> PlayerMgr::sub_objects_own_player(unsigned index) {
    const auto p = slot(index);
    const auto subs = camera_sub_objects(index);
    if (!p.has_value() || !subs.has_value()) {
        return std::nullopt;
    }
    for (const auto obj : {subs->controller, subs->player_camera, subs->physics_holder}) {
        if (obj == 0) {
            return std::nullopt;
        }
        const auto owner = mem::read_ptr(obj + kOwnerBackPointer);
        if (!owner.has_value()) {
            return std::nullopt;
        }
        if (*owner != *p) {
            return false;
        }
    }
    return true;
}

std::optional<PlayerMgr::SubObjectOffsets> PlayerMgr::sub_object_offsets(unsigned index) {
    const auto p = slot(index);
    const auto subs = camera_sub_objects(index);
    if (!p.has_value() || !subs.has_value()) {
        return std::nullopt;
    }
    const auto delta = [&](uintptr_t obj) -> std::optional<uintptr_t> {
        if (obj == 0 || obj <= *p) {
            return std::nullopt;  // not above the player: a separate allocation, not an embedded member
        }
        return obj - *p;
    };
    SubObjectOffsets out{};
    out.controller = delta(subs->controller);
    out.player_camera = delta(subs->player_camera);
    out.physics_holder = delta(subs->physics_holder);
    if (const auto a = aim_object(index)) {
        out.aim = delta(*a);
    }
    return out;
}

std::optional<uintptr_t> PlayerMgr::aim_object(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->object == 0) {
        return std::nullopt;
    }
    const auto sub = mem::read<uint32_t>(p->object + kAimSubObject);
    if (!sub.has_value() || *sub == 0) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(*sub);
}

namespace {

// The named bits, paired with the word a consumer would use. Order is the order they print in.
struct MoveFlagName {
    PlayerMgr::MoveFlag flag;
    const char* name;
};

constexpr MoveFlagName kMoveFlagNames[] = {
    {PlayerMgr::MoveFlag::Sprinting, "sprinting"},
    {PlayerMgr::MoveFlag::CountsAsMoving, "moving"},
    {PlayerMgr::MoveFlag::NormalSpeed, "normal_speed"},
    {PlayerMgr::MoveFlag::Crouching, "crouching"},
    {PlayerMgr::MoveFlag::Melee, "melee"},
    {PlayerMgr::MoveFlag::GrenadeHeld, "grenade_held"},
    {PlayerMgr::MoveFlag::Forward, "forward"},
    {PlayerMgr::MoveFlag::Backward, "backward"},
    {PlayerMgr::MoveFlag::Left, "left"},
    {PlayerMgr::MoveFlag::Right, "right"},
};

}  // namespace

uint32_t PlayerMgr::MovementFlags::unmapped() const {
    uint32_t named = 0;
    for (const auto& e : kMoveFlagNames) {
        named |= static_cast<uint32_t>(e.flag);
    }
    return raw & ~named;
}

std::string PlayerMgr::movement_flag_names(uint32_t raw) {
    std::string out;
    for (const auto& e : kMoveFlagNames) {
        if ((raw & static_cast<uint32_t>(e.flag)) != 0) {
            if (!out.empty()) {
                out += '|';
            }
            out += e.name;
        }
    }
    return out;
}

std::optional<PlayerMgr::MovementFlags> PlayerMgr::movement_flags(unsigned index) {
    const auto raw = move_mgr_flags(index);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    MovementFlags out;
    out.raw = *raw;
    return out;
}

std::optional<bool> PlayerMgr::aim_object_owns_player(unsigned index) {
    const auto p = slot(index);
    const auto a = aim_object(index);
    if (!p.has_value() || !a.has_value()) {
        return std::nullopt;
    }
    const auto owner = mem::read<uint32_t>(*a + kOwnerBackPointer);
    if (!owner.has_value()) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(*owner) == *p;
}

namespace {

std::optional<bool> vtable_is(uintptr_t object, uintptr_t module_relative) {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0 || object == 0) {
        return std::nullopt;
    }
    const auto vt = mem::read_ptr(object);
    if (!vt.has_value()) {
        return std::nullopt;
    }
    return *vt == gc->base + module_relative;
}

}  // namespace

std::optional<bool> PlayerMgr::controller_class_matches(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value()) {
        return std::nullopt;
    }
    return vtable_is(subs->controller, kControllerVtable);
}

std::optional<bool> PlayerMgr::physics_holder_class_matches(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value()) {
        return std::nullopt;
    }
    return vtable_is(subs->physics_holder, kPhysicsHolderVtable);
}


namespace {

// The table as recorded from IDA: offset -> {ctor (gameclient-relative), size lower bound, name}.
// Nineteen entries carry no name on purpose; see the header.
struct SubsystemRecord {
    uintptr_t offset;
    uintptr_t ctor;
    uint32_t size;
    const char* name;
};

constexpr SubsystemRecord kSubsystemRecords[] = {
    {228, 0x0F3B70, 156, "head bob"},
    {232, 0x0EF8F0, 836, "flashlight"},
    {236, 0x10B390, 2220, "CMoveMgr"},
    {240, 0x0FB470, 95, nullptr},
    {244, 0x137B10, 536, "weapon chooser"},
    {248, 0x1179C0, 360, "target info"},
    {252, 0x0E3F80, 6345, "CPlayerCamera"},
    {256, 0x0D0D70, 352, nullptr},
    {260, 0x0DBDA0, 1949, "physics holder"},
    {264, 0x0F9BC0, 84, "ladder"},
    {268, 0x0CF780, 19, "weapon perturb"},
    {272, 0x0EDD30, 149, "damage fx"},
    {276, 0x110CA0, 396, "special move"},
    {280, 0x114110, 412, "player stats"},
    {284, 0x0EBA50, 500, nullptr},
    {288, 0, 0, nullptr},  // not a class instance -- a node table
    {292, 0x100400, 228, nullptr},
    {296, 0x0B8070, 48, nullptr},
    {300, 0x0F5870, 109, "input bindings"},
    {304, 0x0F80A0, 4, nullptr},
    {308, 0x055FA0, 141, nullptr},
    {312, 0x11C680, 1896, nullptr},  // real class; its +4 is a link, not the owner
    {316, 0x127E40, 274, nullptr},
    {320, 0x1265D0, 17, nullptr},
};

}  // namespace

std::vector<PlayerMgr::Subsystem> PlayerMgr::subsystem_slots(unsigned index) {
    std::vector<Subsystem> out;
    const auto p = slot(index);
    if (!p.has_value() || *p == 0) {
        return out;
    }
    out.reserve(std::size(kSubsystemRecords));
    for (const auto& rec : kSubsystemRecords) {
        Subsystem s;
        s.offset = rec.offset;
        s.ctor = rec.ctor;
        s.size_lower_bound = rec.size;
        s.name = rec.name;
        const auto obj = mem::read_ptr(*p + rec.offset);
        if (!obj.has_value()) {
            out.push_back(s);
            continue;
        }
        s.object = *obj;
        if (s.object != 0) {
            if (const auto vt = mem::read_ptr(s.object); vt.has_value()) {
                s.vtable = *vt;
                s.is_class_instance = Modules::looks_like_vtable_pointer(*vt);
            }
            if (const auto owner = mem::read_ptr(s.object + kOwnerBackPointer); owner.has_value()) {
                s.owner_is_player = (*owner == *p);
            }
        }
        out.push_back(s);
    }
    return out;
}

std::optional<PlayerMgr::Subsystem> PlayerMgr::subsystem_at(unsigned index, uintptr_t offset) {
    for (const auto& s : subsystem_slots(index)) {
        if (s.offset == offset) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<size_t> PlayerMgr::subsystem_count(unsigned index) {
    const auto slots = subsystem_slots(index);
    if (slots.empty()) {
        return std::nullopt;
    }
    size_t n = 0;
    for (const auto& s : slots) {
        if (s.is_class_instance) {
            ++n;
        }
    }
    return n;
}

std::optional<bool> PlayerMgr::subsystem_vtables_distinct(unsigned index) {
    const auto slots = subsystem_slots(index);
    if (slots.empty()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].is_class_instance) {
            continue;
        }
        for (size_t j = i + 1; j < slots.size(); ++j) {
            if (slots[j].is_class_instance && slots[i].vtable == slots[j].vtable) {
                return false;
            }
        }
    }
    return true;
}


std::optional<PlayerMgr::Subsystem> PlayerMgr::subsystem_by_name(unsigned index, std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const auto& s : subsystem_slots(index)) {
        if (s.name != nullptr && name == s.name) {
            return s;
        }
    }
    return std::nullopt;
}

size_t PlayerMgr::named_subsystem_count() {
    size_t n = 0;
    for (const auto& rec : kSubsystemRecords) {
        if (rec.name != nullptr) {
            ++n;
        }
    }
    return n;
}

namespace {

// The Arsenal/Ammo category, and the position of a record within it. The engine indexes its count
// array by exactly this position (IDatabaseMgr slot 21 on the record), so the enumeration order
// here IS the array order -- an assumption the fixture discriminates rather than trusts: firing a
// weapon must move the entry whose name matches that weapon's ammo, and no other.
constexpr const char* kAmmoCategory = "Arsenal/Ammo";

const regenny::DatabaseMgrCategory* ammo_category() {
    auto* db = DatabaseMgr::get();

    if (db == nullptr || db->entry_count() < 1) {
        return nullptr;
    }

    auto* e = db->entry(0);

    if (e == nullptr || e->record_a == nullptr) {
        return nullptr;
    }

    return DatabaseMgr::find_category(e->record_a, kAmmoCategory);
}

} // namespace

std::optional<uintptr_t> PlayerMgr::ammo_array(unsigned index) {
    const auto sub = subsystem_by_name(index, "player stats");

    if (!sub.has_value() || sub->object == 0) {
        return std::nullopt;
    }

    const auto base = mem::read<uintptr_t>(sub->object + kStatsAmmoArray);

    if (!base.has_value() || *base == 0) {
        return std::nullopt;
    }

    return *base;
}

std::optional<int32_t> PlayerMgr::ammo_count(unsigned index, const std::string& ammo_name) {
    const auto* cat = ammo_category();

    if (cat == nullptr) {
        return std::nullopt;
    }

    const auto base = ammo_array(index);

    if (!base.has_value()) {
        return std::nullopt;
    }

    const size_t n = DatabaseMgr::record_count(cat);

    for (size_t i = 0; i < n; ++i) {
        auto* rec = DatabaseMgr::record(cat, i);

        if (rec == nullptr || DatabaseMgr::record_name(rec) != ammo_name) {
            continue;
        }

        return mem::read<int32_t>(*base + i * sizeof(int32_t));
    }

    return std::nullopt;
}

std::vector<PlayerMgr::AmmoHolding> PlayerMgr::ammo_held(unsigned index) {
    std::vector<AmmoHolding> out;

    const auto* cat = ammo_category();
    const auto base = ammo_array(index);

    if (cat == nullptr || !base.has_value()) {
        return out;
    }

    const size_t n = DatabaseMgr::record_count(cat);

    for (size_t i = 0; i < n; ++i) {
        const auto count = mem::read<int32_t>(*base + i * sizeof(int32_t));

        // A guarded read that faults ends the walk rather than skipping a hole: past the end of
        // the allocation every later index is equally unreadable, and continuing would turn one
        // bad offset into 52 silent zeroes.
        if (!count.has_value()) {
            break;
        }

        if (*count == 0) {
            continue;
        }

        auto* rec = DatabaseMgr::record(cat, i);

        if (rec == nullptr) {
            continue;
        }

        out.push_back(AmmoHolding{DatabaseMgr::record_name(rec), *count});
    }

    return out;
}

bool PlayerMgr::set_ammo_count(unsigned index, const std::string& ammo_name, int32_t rounds) {
    const auto* cat = ammo_category();

    if (cat == nullptr) {
        return false;
    }

    const auto base = ammo_array(index);

    if (!base.has_value()) {
        return false;
    }

    const size_t n = DatabaseMgr::record_count(cat);

    for (size_t i = 0; i < n; ++i) {
        auto* rec = DatabaseMgr::record(cat, i);

        if (rec == nullptr || DatabaseMgr::record_name(rec) != ammo_name) {
            continue;
        }

        return mem::write<int32_t>(*base + i * sizeof(int32_t), rounds);
    }

    return false;
}

size_t PlayerMgr::replenish_ammo(unsigned index, int32_t floor) {
    const auto* cat = ammo_category();
    const auto base = ammo_array(index);

    if (cat == nullptr || !base.has_value() || floor <= 0) {
        return 0;
    }

    const size_t n = DatabaseMgr::record_count(cat);
    size_t raised = 0;

    for (size_t i = 0; i < n; ++i) {
        const auto addr = *base + i * sizeof(int32_t);
        const auto count = mem::read<int32_t>(addr);

        // Same rule as ammo_held: an unreadable slot ends the walk. Past the end of
        // the allocation every later index is equally unreadable, and continuing
        // would turn one bad offset into 52 writes into whatever follows.
        if (!count.has_value()) {
            break;
        }

        if (*count <= 0 || *count >= floor) {
            continue;
        }

        if (mem::write<int32_t>(addr, floor)) {
            ++raised;
        }
    }

    return raised;
}

std::optional<int32_t> PlayerMgr::ammo_total(unsigned index) {
    const auto* cat = ammo_category();
    const auto base = ammo_array(index);

    if (cat == nullptr || !base.has_value()) {
        return std::nullopt;
    }

    const size_t n = DatabaseMgr::record_count(cat);
    int32_t total = 0;

    for (size_t i = 0; i < n; ++i) {
        const auto count = mem::read<int32_t>(*base + i * sizeof(int32_t));

        if (!count.has_value()) {
            return std::nullopt;
        }

        // Negative would mean the array is not what this claims; refuse rather than fold it in.
        if (*count < 0) {
            return std::nullopt;
        }

        total += *count;
    }

    return total;
}

std::optional<PlayerMgr::PlayerStats> PlayerMgr::player_stats(unsigned index) {
    const auto sub = subsystem_by_name(index, "player stats");
    if (!sub.has_value() || sub->object == 0) {
        return std::nullopt;
    }
    const auto health = mem::read<int32_t>(sub->object + kStatsHealth);
    const auto armor = mem::read<int32_t>(sub->object + kStatsArmor);
    const auto max_health = mem::read<int32_t>(sub->object + kStatsMaxHealth);
    const auto max_armor = mem::read<int32_t>(sub->object + kStatsMaxArmor);
    const auto air = mem::read<float>(sub->object + kStatsAir);
    const auto lost = mem::read<int32_t>(sub->object + kStatsHealthLost);
    if (!health.has_value() || !armor.has_value() || !max_health.has_value() || !max_armor.has_value() ||
        !air.has_value() || !lost.has_value()) {
        return std::nullopt;
    }
    PlayerStats out;
    out.health = *health;
    out.armor = *armor;
    out.max_health = *max_health;
    out.max_armor = *max_armor;
    out.air = *air;
    out.health_lost = *lost;
    return out;
}

std::optional<bool> PlayerMgr::player_stats_consistent(unsigned index) {
    const auto s = player_stats(index);
    if (!s.has_value()) {
        return std::nullopt;
    }
    return s->consistent();
}


std::optional<bool> PlayerMgr::water_affects_speed(unsigned index) {
    const auto sub = subsystem_by_name(index, "CMoveMgr");
    if (!sub.has_value() || sub->object == 0) {
        return std::nullopt;
    }
    const auto v = mem::read<uint8_t>(sub->object + kMoveMgrWaterAffectsSpeed);
    if (!v.has_value()) {
        return std::nullopt;
    }
    return *v != 0;
}

std::optional<PlayerMgr::VarCache> PlayerMgr::spectator_speed_mul_cache(unsigned index) {
    const auto sub = subsystem_by_name(index, "CMoveMgr");
    if (!sub.has_value() || sub->object == 0) {
        return std::nullopt;
    }
    const auto rec = mem::read_ptr(sub->object + kMoveMgrSpectatorSpeedMulCache);
    const auto own = mem::read_ptr(sub->object + kMoveMgrSpectatorSpeedMulCache + sizeof(uintptr_t));
    if (!rec.has_value() || !own.has_value()) {
        return std::nullopt;
    }
    VarCache out;
    out.record = *rec;
    out.owner = *own;
    return out;
}

std::optional<bool> PlayerMgr::spectator_speed_mul_is_default(unsigned index) {
    const auto live = spectator_speed_mul(index);
    if (!live.has_value()) {
        return std::nullopt;
    }
    const auto* def = Engine::registered_default("SpectatorSpeedMul");
    if (def == nullptr || def->source != Engine::DefaultSource::CodeLiteral) {
        return std::nullopt;
    }
    return *live == def->value;
}

std::optional<float> PlayerMgr::spectator_speed_mul(unsigned index) {
    const auto cache = spectator_speed_mul_cache(index);
    if (!cache.has_value() || !cache->populated()) {
        return std::nullopt;
    }
    return mem::read<float>(cache->record);
}


regenny::DatabaseMgrRecord* PlayerMgr::camera_clamp_record(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value() || subs->player_camera == 0) {
        return nullptr;
    }
    const auto p = mem::read_ptr(subs->player_camera + kCameraClampRecord);
    if (!p.has_value() || *p == 0) {
        return nullptr;
    }
    return reinterpret_cast<regenny::DatabaseMgrRecord*>(*p);
}

std::optional<uint32_t> PlayerMgr::camera_clamp_state(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value() || subs->player_camera == 0) {
        return std::nullopt;
    }
    return mem::read<uint32_t>(subs->player_camera + kCameraStateMachine);
}

std::optional<std::pair<float, float>> PlayerMgr::camera_clamp(unsigned index, std::string_view state) {
    auto* rec = camera_clamp_record(index);
    if (rec == nullptr) {
        return std::nullopt;
    }
    const auto attr = DatabaseMgr::find_attribute(rec, state);
    if (!attr.has_value()) {
        return std::nullopt;
    }
    const auto pair = DatabaseMgr::attribute_float_pair(*attr, 0);
    if (!pair.has_value()) {
        return std::nullopt;
    }
    return std::make_pair(pair->first, pair->second);
}

std::optional<bool> PlayerMgr::camera_state_is_chase(unsigned index) {
    const auto st = camera_clamp_state(index);
    if (!st.has_value()) {
        return std::nullopt;
    }
    return *st == 1 || *st == 7;
}


std::optional<std::pair<float, float>> PlayerMgr::camera_clamp_radians(unsigned index,
                                                                      std::string_view state) {
    const auto deg = camera_clamp(index, state);
    if (!deg.has_value()) {
        return std::nullopt;
    }
    // Exactly the dispatcher's tail, negation included.
    constexpr float kDeg2Rad = 0.01745329238474369f;
    return std::make_pair(deg->first * kDeg2Rad, deg->second * -kDeg2Rad);
}

std::optional<uint32_t> PlayerMgr::move_mgr_flags(unsigned index) {
    const auto sub = subsystem_by_name(index, "CMoveMgr");
    if (!sub.has_value() || sub->object == 0) {
        return std::nullopt;
    }
    return mem::read<uint32_t>(sub->object + kMoveMgrFlags);
}

std::optional<bool> PlayerMgr::is_crouching(unsigned index) {
    const auto flags = move_mgr_flags(index);
    if (!flags.has_value()) {
        return std::nullopt;
    }
    return (*flags & kMoveFlagCrouching) != 0;
}

std::optional<std::array<float, 3>> PlayerMgr::physics_velocity(unsigned index) {
    const auto p = slot(index);
    if (!p.has_value() || *p == 0) {
        return std::nullopt;
    }
    // The engine object the physics interface knows this player by is the MODEL object -- the same one
    // PlayerPhysics_EngineObject hands to ILTPhysics. Object.hpp owns that resolution.
    const auto obj = mem::read_ptr(*p + kEngineHolderField);
    if (!obj.has_value() || *obj == 0) {
        return std::nullopt;
    }
    const auto model = mem::read_ptr(*obj + 320);  // physics holder +320, the LTObject given to ILTPhysics
    if (!model.has_value() || *model == 0) {
        return std::nullopt;
    }
    return Physics::velocity(*model);
}

std::optional<bool> PlayerMgr::is_moving(unsigned index) {
    const auto flags = move_mgr_flags(index);
    const auto vel = physics_velocity(index);
    if (!flags.has_value() && !vel.has_value()) {
        return std::nullopt;
    }
    if (flags.has_value() && (*flags & kMoveFlagForceMoving) != 0) {
        return true;
    }
    if (!vel.has_value()) {
        return std::nullopt;
    }
    const float speed2 =
        (*vel)[0] * (*vel)[0] + (*vel)[1] * (*vel)[1] + (*vel)[2] * (*vel)[2];
    return speed2 > kMoveSpeedThreshold * kMoveSpeedThreshold;
}

std::optional<PlayerMgr::ClampChoice> PlayerMgr::predicted_clamp_state(unsigned index) {
    const auto st = camera_clamp_state(index);
    if (!st.has_value()) {
        return std::nullopt;
    }
    ClampChoice out;
    out.slide_kick_unchecked = true;
    if (*st == 1 || *st == 7) {
        out.state = "Chase";
        return out;
    }
    const auto crouch = is_crouching(index);
    const auto moving = is_moving(index);
    if (!crouch.has_value() || !moving.has_value()) {
        return std::nullopt;
    }
    if (*crouch) {
        out.state = *moving ? "CrouchMoving" : "CrouchIdle";
    } else {
        out.state = *moving ? "StandMoving" : "StandIdle";
    }
    return out;
}


std::optional<PlayerMgr::PitchClampRecord> PlayerMgr::camera_pitch_clamp_record(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value() || subs->player_camera == 0) {
        return std::nullopt;
    }
    const auto before = mem::read<float>(subs->player_camera + kCameraPitchPreClamp);
    const auto after = mem::read<float>(subs->player_camera + kCameraPitchPostClamp);
    if (!before.has_value() || !after.has_value()) {
        return std::nullopt;
    }
    return PitchClampRecord{*before, *after};
}

std::optional<bool> PlayerMgr::pitch_clamp_record_within_active(unsigned index) {
    const auto rec = camera_pitch_clamp_record(index);
    const auto pick = predicted_clamp_state(index);
    if (!rec.has_value() || !pick.has_value()) {
        return std::nullopt;
    }
    const auto bounds = camera_clamp_radians(index, pick->state);
    if (!bounds.has_value()) {
        return std::nullopt;
    }
    // bounds.first is the positive bound, bounds.second the negative one -- see camera_clamp_radians.
    return rec->after <= bounds->first && rec->after >= bounds->second;
}


std::optional<PlayerMgr::TimerState> PlayerMgr::timer_at(uintptr_t address) {
    if (address == 0) {
        return std::nullopt;
    }
    const auto start = mem::read<double>(address);
    const auto duration = mem::read<double>(address + 8);
    const auto use_cached = mem::read<uint8_t>(address + 0x18);
    const auto active = mem::read<uint8_t>(address + 0x19);
    if (!start.has_value() || !duration.has_value() || !use_cached.has_value() || !active.has_value()) {
        return std::nullopt;
    }
    TimerState out;
    out.start = *start;
    out.duration = *duration;
    out.use_cached = *use_cached != 0;
    out.active = *active != 0;
    return out;
}

std::optional<bool> PlayerMgr::pose_generations_differ(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    const auto other = read_pose(p->holder, false);
    if (!other.has_value()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < other->position.size(); ++i) {
        uint32_t a = 0;
        uint32_t b = 0;
        std::memcpy(&a, &other->position[i], sizeof(a));
        std::memcpy(&b, &p->applied_pose.position[i], sizeof(b));
        if (a != b) {
            return true;
        }
    }
    return false;
}

std::optional<std::array<float, 4>> PlayerMgr::applied_rotation(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    std::array<float, 4> q{};
    if (!mem::copy(q.data(), p->holder + kAppliedRotation, sizeof(q))) {
        return std::nullopt;
    }
    return q;
}

std::optional<std::array<float, 4>> PlayerMgr::view_rotation(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return std::nullopt;
    }
    std::array<float, 4> q{};
    if (!mem::copy(q.data(), p->holder + kRotation, sizeof(q))) {
        return std::nullopt;
    }
    return q;
}

bool PlayerMgr::write_view_rotation(unsigned index, const std::array<float, 4>& rotation) {
    const float n2 = rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] +
                     rotation[3] * rotation[3];
    if (!(n2 > 0.98f && n2 < 1.02f)) {
        return false;
    }
    for (const float c : rotation) {
        if (!std::isfinite(c)) {
            return false;
        }
    }
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return false;
    }
    return mem::store(p->holder + kRotation, rotation.data(), sizeof(rotation));
}

bool PlayerMgr::write_applied_rotation(unsigned index, const std::array<float, 4>& rotation) {
    // UNIT CHECK FIRST. read_pose refuses a non-unit quaternion because that is the signature of a wrong
    // offset; writing one would manufacture exactly the state the read side treats as a mapping error.
    const float n2 = rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] +
                     rotation[3] * rotation[3];
    if (!(n2 > 0.98f && n2 < 1.02f)) {
        return false;
    }
    for (const float c : rotation) {
        if (!std::isfinite(c)) {
            return false;
        }
    }
    const auto p = player(index);
    if (!p.has_value() || p->holder == 0) {
        return false;
    }
    return mem::store(p->holder + kAppliedRotation, rotation.data(), sizeof(rotation));
}

uintptr_t PlayerMgr::update_view_pose_fn() {
    static uintptr_t s_fn = 0;
    if (s_fn != 0) {
        return s_fn;
    }
    // PlayerCamera_UpdateViewPose prologue, gameclient 0x100E34E0. Wildcards cover two relative calls; UNIQUE
    // in .text as of this mapping.
    //     push ebp / mov ebp,esp / and esp,0FFFFFFC0h / sub esp,0F4h / push ebx,esi,edi / mov esi,ecx
    //     call <a> / mov ecx,esi / call <b> / mov eax,[esi+4]
    s_fn = Modules::get().scan_game_client(
        "55 8B EC 83 E4 C0 81 EC F4 00 00 00 53 56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B 46 04",
        "PlayerCamera::UpdateViewPose");
    return s_fn;
}

uintptr_t PlayerMgr::apply_look_delta_fn() {
    // RETRYABLE, NOT LATCHED ON FAILURE. gameclient.dll is present for the whole process life once resolved,
    // but a caller may ask before Modules::initialize() has run -- and a function-local static would then cache
    // 0 forever and leave the hook permanently uninstallable. Latch only a successful resolution.
    static uintptr_t s_fn = 0;
    if (s_fn != 0) {
        return s_fn;
    }
    // CPlayerCamera_ApplyLookDelta prologue, gameclient 0x100E03D0. Wildcards cover the absolute
    // g_UiSystemState operand and three relative displacements; UNIQUE in .text as of this mapping.
    //     sub esp,38h / push ebx / push esi / mov esi,ecx / mov eax,[esi+4] / push edi / push eax
    //     mov ecx, g_UiSystemState / call <pred> / test al,al / jz <far> / mov ecx,[esi+4]
    //     mov edx,[ecx+110h] / xor ebx,ebx
    s_fn = Modules::get().scan_game_client(
        "83 EC 38 53 56 8B F1 8B 46 04 57 50 B9 ? ? ? ? E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 8B 4E 04 "
        "8B 91 10 01 00 00 33 DB",
        "CPlayerCamera::ApplyLookDelta");
    return s_fn;
}

std::optional<float> PlayerMgr::zoom_fraction(unsigned index, double now) {
    const auto st = aim_state_raw(index);
    if (!st.has_value()) {
        return std::nullopt;
    }
    if (*st == 1) {
        return 1.0f;
    }
    if (*st == 3) {
        return 0.0f;
    }
    if (*st != 0 && *st != 2) {
        return std::nullopt;
    }

    const auto p = player(index);
    if (!p.has_value() || p->object == 0) {
        return std::nullopt;
    }
    const auto sub = mem::read<uint32_t>(p->object + kAimSubObject);
    if (!sub.has_value() || *sub == 0) {
        return std::nullopt;
    }
    const auto t = timer_at(*sub + kZoomTransitionTimer);
    if (!t.has_value()) {
        return std::nullopt;
    }
    // AN ELAPSED OR INACTIVE TIMER IS A FINISHED TRANSITION, matching GameTimer_IsElapsed's own reading that an
    // inactive timer counts as elapsed. UpdateTransition commits on exactly that condition.
    float f = 1.0f;
    if (t->active && t->duration > 0.0) {
        const double done = (now - t->start) / t->duration;
        f = static_cast<float>(done < 0.0 ? 0.0 : (done > 1.0 ? 1.0 : done));
    }
    return *st == 0 ? f : 1.0f - f;
}

std::optional<PlayerMgr::TimerState> PlayerMgr::pitch_recovery_timer(unsigned index) {
    const auto subs = camera_sub_objects(index);
    if (!subs.has_value() || subs->player_camera == 0) {
        return std::nullopt;
    }
    // Same layout as every other timer of this shape; read through the one reader rather than a second copy.
    return timer_at(subs->player_camera + kCameraPitchRecoveryTimer);
}

PlayerMgr::AimTrackingLimits PlayerMgr::aim_tracking_limits() {
    AimTrackingLimits out;
    if (const auto v = Engine::find_cached_var("CameraAimTrackingYMax"); v.has_value()) {
        out.normal_degrees = Engine::read_cached(*v);
    }
    if (const auto v = Engine::find_cached_var("CameraAimTrackingYMaxZoomed"); v.has_value()) {
        out.zoomed_degrees = Engine::read_cached(*v);
    }
    return out;
}

std::optional<uint32_t> PlayerMgr::aim_state_raw(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->object == 0) {
        return std::nullopt;
    }
    const auto sub = mem::read<uint32_t>(p->object + kAimSubObject);
    if (!sub.has_value() || *sub == 0) {
        return std::nullopt;
    }
    return mem::read<uint32_t>(*sub + kAimState);
}

std::optional<PlayerMgr::AimState> PlayerMgr::aim_state(unsigned index) {
    const auto raw = aim_state_raw(index);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    switch (*raw) {
    case 0: return AimState::EnteringAds;
    case 1: return AimState::Ads;
    case 2: return AimState::LeavingAds;
    case 3: return AimState::Hip;
    default:
        // A FIFTH VALUE WAS NEVER OBSERVED, so it is refused rather than folded into one of the four. Naming an
        // unseen value is how a mapping starts lying.
        return std::nullopt;
    }
}

std::optional<bool> PlayerMgr::ads_fov_active(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->object == 0) {
        return std::nullopt;
    }
    const auto sub = mem::read<uint32_t>(p->object + kAimSubObject);
    if (!sub.has_value() || *sub == 0) {
        return std::nullopt;
    }
    const auto v = mem::read<uint32_t>(*sub + kAdsFovFlag);
    if (!v.has_value()) {
        return std::nullopt;
    }
    return *v != 0;
}

std::optional<bool> PlayerMgr::uses_zoomed_aim_limit(unsigned index) {
    const auto raw = aim_state_raw(index);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    // ApplyLookDelta's own test, not a paraphrase of it: == 3 takes the normal limit.
    return *raw != 3;
}

}  // namespace sdk

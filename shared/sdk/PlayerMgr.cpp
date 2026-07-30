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
#include "CClientShell.hpp"
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
    mem::write<std::array<float, 4>>(at, saved);
    out.frames_observed = static_cast<unsigned>(frames.size());
    // A SURVIVING VALUE MEANS NOTHING WITHOUT RENDERED FRAMES. With the render path frozen nothing could have
    // overwritten it, so the verdict is Inconclusive however long it lasted.
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
    mem::write<std::array<float, 4>>(at, saved);
    out.frames_observed = static_cast<unsigned>(frames.size());
    // A SURVIVING VALUE MEANS NOTHING WITHOUT RENDERED FRAMES. With the render path frozen nothing could have
    // overwritten it, so the verdict is Inconclusive however long it lasted.
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

}  // namespace sdk

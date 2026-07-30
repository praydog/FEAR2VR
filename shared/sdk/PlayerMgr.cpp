#include "PlayerMgr.hpp"

#include <cmath>
#include <cstring>
#include <iterator>

#include "Memory.hpp"
#include "Modules.hpp"
#include "CClientShell.hpp"
#include "Object.hpp"

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

}  // namespace sdk

#include "PlayerMgr.hpp"

#include <cmath>
#include <cstring>

#include "Memory.hpp"
#include "Modules.hpp"

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

std::optional<PlayerMgr::Pose> PlayerMgr::read_pose(uintptr_t holder) {
    if (holder == 0) {
        return std::nullopt;
    }

    Pose pose{};
    if (!mem::copy(pose.position.data(), holder + kPosition, sizeof(pose.position)) ||
        !mem::copy(pose.rotation.data(), holder + kRotation, sizeof(pose.rotation))) {
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

    // Both engine objects are validated as engine objects rather than trusted: a non-null field whose vtable
    // is not in the exe is not an LTObject, and reporting it as one would send a caller reading LTObject
    // offsets into whatever it actually is.
    if (const auto anchor = mem::read_ptr(*holder + kViewAnchor)) {
        if (is_engine_object(*anchor)) {
            p.view_anchor = *anchor;
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

std::optional<std::array<float, 3>> PlayerMgr::eye_offset(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->view_anchor == 0 || p->model_object == 0) {
        return std::nullopt;
    }

    std::array<float, 3> anchor{};
    std::array<float, 3> model{};
    if (!mem::copy(anchor.data(), p->view_anchor + kObjectPosition, sizeof(anchor)) ||
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

std::optional<bool> PlayerMgr::anchor_rotation_matches_pose(unsigned index) {
    const auto p = player(index);
    if (!p.has_value() || p->view_anchor == 0) {
        return std::nullopt;
    }

    std::array<float, 4> anchor{};
    if (!mem::copy(anchor.data(), p->view_anchor + kObjectRotation, sizeof(anchor))) {
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

}  // namespace sdk

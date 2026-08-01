#include "Physics.hpp"

#include <cstring>

#include "Memory.hpp"

#include "Modules.hpp"
#include "PlayerMgr.hpp"
#include "Vtables.hpp"
#include "interfaces/Registry.hpp"

namespace sdk {

namespace {

// "ILTPhysics.Client" is the name the holders request; the registry re-reads the slot every call, since a
// resolved interface pointer is not stable across module load and unload.
constexpr const char* kInterfaceName = "ILTPhysics.Client";

// Every call below goes through the real vtable slot. Two guarded reads first -- the vtable pointer, then
// the entry -- and a bounds check into the exe, so a stale instance faults here instead of transferring
// control to whatever the pointer happened to hold.
uintptr_t slot_fn(Physics::Slot slot) {
    const auto addr = Physics::slot_address(slot);
    return addr.value_or(0);
}

// The three-float out-parameter shape shared by velocity, acceleration and dims. The LTRESULT is checked:
// the engine returns non-zero on refusal and leaves the buffer untouched, which would otherwise read as a
// legitimate zero vector.
std::optional<std::array<float, 3>> query_vector(Physics::Slot slot, uintptr_t object) {
    const uintptr_t fn = slot_fn(slot);
    const uintptr_t self = Physics::instance();
    if (fn == 0 || self == 0 || object == 0) {
        return std::nullopt;
    }
    using Fn = int32_t(__thiscall*)(void*, uintptr_t, float*);
    float out[3]{};
    int32_t rc = -1;
    if (!sdk::mem::guarded([&] {
            rc = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(self), object, out);
        })) {
        return std::nullopt;
    }
    if (rc != Physics::kLtOk) {
        return std::nullopt;
    }
    return std::array<float, 3>{out[0], out[1], out[2]};
}

}  // namespace

bool Physics::move_object(uintptr_t object, const std::array<float, 3>& position, uint32_t flags) {
    const uintptr_t fn = slot_fn(Slot::MoveObject);
    const uintptr_t self = Physics::instance();
    if (fn == 0 || self == 0 || object == 0) {
        return false;
    }

    // The engine takes the position BY POINTER (`lea` of a stack triple at every call site), so the
    // vector has to outlive the call in our frame rather than being passed by value.
    using Fn = int32_t(__thiscall*)(void*, uintptr_t, const float*, uint32_t);
    const float pos[3]{position[0], position[1], position[2]};
    bool called = false;
    if (!sdk::mem::guarded([&] {
            reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(self), object, pos, flags);
            called = true;
        })) {
        return false;
    }
    return called;
}

uintptr_t Physics::instance() {
    auto& registry = interfaces::Registry::get();
    if (!registry.is_initialized() && !registry.initialize()) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(registry.resolve(kInterfaceName));
}

uintptr_t Physics::vtable() {
    return Vtables::vtable_of(instance()).value_or(0);
}

std::optional<std::string> Physics::class_name() {
    const uintptr_t vt = vtable();
    if (vt == 0) {
        return std::nullopt;
    }
    // Asked of the binary through the name getter, not looked up in the catalogue: an object that is not
    // what this header expects should say so itself.
    return Vtables::name_from_getter(vt, static_cast<size_t>(Slot::InterfaceImplementation));
}

std::optional<uintptr_t> Physics::slot_address(Slot slot) {
    const auto index = static_cast<size_t>(slot);
    if (index >= kSlotCount) {
        return std::nullopt;
    }
    const uintptr_t vt = vtable();
    if (vt == 0) {
        return std::nullopt;
    }
    uint32_t fn = 0;
    if (!sdk::mem::copy(&fn, vt + index * sizeof(uint32_t), sizeof(fn)) || fn == 0) {
        return std::nullopt;
    }
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || fn < exe->base || fn >= exe->base + exe->size) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(fn);
}

std::optional<float> Physics::stair_height() {
    const uintptr_t fn = slot_fn(Slot::GetStairHeight);
    const uintptr_t self = instance();
    if (fn == 0 || self == 0) {
        return std::nullopt;
    }
    using Fn = int32_t(__thiscall*)(void*, float*);
    float out = 0.0f;
    int32_t rc = -1;
    if (!sdk::mem::guarded([&] {
            rc = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(self), &out);
        })) {
        return std::nullopt;
    }
    if (rc != kLtOk) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::array<float, 3>> Physics::global_force() {
    const uintptr_t fn = slot_fn(Slot::GetGlobalForce);
    const uintptr_t self = instance();
    if (fn == 0 || self == 0) {
        return std::nullopt;
    }
    using Fn = int32_t(__thiscall*)(void*, float*);
    float out[3]{};
    int32_t rc = -1;
    if (!sdk::mem::guarded([&] {
            rc = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(self), out);
        })) {
        return std::nullopt;
    }
    if (rc != kLtOk) {
        return std::nullopt;
    }
    return std::array<float, 3>{out[0], out[1], out[2]};
}

std::optional<bool> Physics::is_world_object(uintptr_t object) {
    const uintptr_t fn = slot_fn(Slot::IsWorldObject);
    const uintptr_t self = instance();
    if (fn == 0 || self == 0 || object == 0) {
        return std::nullopt;
    }
    using Fn = int32_t(__thiscall*)(void*, uintptr_t);
    int32_t rc = -1;
    if (!sdk::mem::guarded([&] {
            rc = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(self), object);
        })) {
        return std::nullopt;
    }
    // This one answers through the LT_YES/LT_NO pair rather than a bool, so anything else is a refusal
    // and must not be folded into "false".
    if (rc == kLtYes) {
        return true;
    }
    if (rc == kLtNo) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::array<float, 3>> Physics::velocity(uintptr_t object) {
    return query_vector(Slot::GetVelocity, object);
}

std::optional<std::array<float, 3>> Physics::acceleration(uintptr_t object) {
    return query_vector(Slot::GetAcceleration, object);
}

std::optional<std::array<float, 3>> Physics::object_dims(uintptr_t object) {
    return query_vector(Slot::GetObjectDims, object);
}


bool Physics::velocity_zeroed_by_game(uintptr_t engine_object) {
    if (engine_object == 0) {
        return false;
    }
    for (unsigned i = 0; i < PlayerMgr::kSlotCount; ++i) {
        const auto obj = PlayerMgr::engine_object(i);
        if (obj.has_value() && *obj == engine_object) {
            return true;
        }
    }
    return false;
}

}  // namespace sdk

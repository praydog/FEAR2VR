#include "Common.hpp"

#include <cstring>

#include <utility/Seh.hpp>

#include "Modules.hpp"
#include "Vtables.hpp"
#include "interfaces/Registry.hpp"

namespace sdk {

namespace {

constexpr const char* kInterfaceName = "ILTCommon.Client";

}  // namespace

uintptr_t Common::instance() {
    auto& registry = interfaces::Registry::get();
    if (!registry.is_initialized() && !registry.initialize()) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(registry.resolve(kInterfaceName));
}

uintptr_t Common::vtable() {
    return Vtables::vtable_of(instance()).value_or(0);
}

std::optional<std::string> Common::class_name() {
    const uintptr_t vt = vtable();
    if (vt == 0) {
        return std::nullopt;
    }
    return Vtables::name_from_getter(vt, static_cast<size_t>(Slot::InterfaceImplementation));
}

std::optional<uintptr_t> Common::slot_address(Slot slot) {
    const auto index = static_cast<size_t>(slot);
    if (index >= kSlotCount) {
        return std::nullopt;
    }
    const uintptr_t vt = vtable();
    if (vt == 0) {
        return std::nullopt;
    }
    const auto entry = Vtables::vtable_of(vt + index * sizeof(uintptr_t));
    if (!entry.has_value()) {
        return std::nullopt;
    }
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || *entry < exe->base || *entry >= exe->base + exe->size) {
        return std::nullopt;
    }
    return *entry;
}

std::optional<uint32_t> Common::object_type(uintptr_t object) {
    const auto fn = slot_address(Slot::GetObjectType);
    const uintptr_t self = instance();
    if (!fn.has_value() || self == 0 || object == 0) {
        return std::nullopt;
    }
    using Fn = int32_t(__thiscall*)(void*, uintptr_t, uint32_t*);
    uint32_t out = 0xFFFFFFFFu;
    int32_t rc = -1;
    KANANLIB_SEH_TRY {
        rc = reinterpret_cast<Fn>(*fn)(reinterpret_cast<void*>(self), object, &out);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (rc != kLtOk) {
        return std::nullopt;
    }
    return out;
}

std::optional<bool> Common::is_low_violence() {
    const auto fn = slot_address(Slot::IsLowViolence);
    const uintptr_t self = instance();
    if (!fn.has_value() || self == 0) {
        return std::nullopt;
    }
    // Returns a byte in al: the ISteamApps query is reduced to a bool by the engine itself, and 0 is
    // returned outright when Steam is unavailable rather than treated as an error.
    using Fn = uint8_t(__thiscall*)(void*);
    uint8_t out = 0;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<Fn>(*fn)(reinterpret_cast<void*>(self));
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    return out != 0;
}

}  // namespace sdk

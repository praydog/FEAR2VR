#include "ViewmodelDecouple.hpp"

#include <atomic>
#include <cinttypes>
#include <cmath>

#include "sdk/CClientShell.hpp"
#include "sdk/Object.hpp"
#include "sdk/PlayerMgr.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "LTObject::SetRotation";

std::atomic<bool> g_enabled{false};
std::atomic<uintptr_t> g_target{0};
std::atomic<uintptr_t> g_object{0};   // the shell player object, refreshed on the game thread
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_matched{0};
std::atomic<uint64_t> g_corrected{0};
std::atomic<float> g_last_correction{0.0f};

using SetRotFn = void(__thiscall*)(const regenny::LTObject*, const regenny::LTRotation*);

// x86 __thiscall detour: ecx carries `this`, so it is written as __fastcall with a dummy edx.
void __fastcall set_rotation_detour(const regenny::LTObject* self, void* /*edx*/,
                                    const regenny::LTRotation* rot) {
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return;
    }
    const auto original = hook->original<SetRotFn>();
    g_calls.fetch_add(1, std::memory_order_relaxed);

    // FAST PATH FIRST. This entry is the rotation setter for EVERY object of its type, so the
    // filter has to be a pointer compare and nothing else -- anything heavier here is paid by the
    // whole world, not by the one object we care about.
    if (rot == nullptr || reinterpret_cast<uintptr_t>(self) != g_object.load(std::memory_order_relaxed)) {
        original(self, rot);
        return;
    }
    g_matched.fetch_add(1, std::memory_order_relaxed);

    if (!g_enabled.load(std::memory_order_relaxed)) {
        original(self, rot);
        return;
    }

    // The head pose that was composed into the camera. Read from the holder rather than from
    // HeadTracking's own state, so this corrects whatever is in the additive slot -- a head pose,
    // a lean, a shake -- rather than only what this framework put there.
    const auto ops = sdk::PlayerMgr::camera_rotation_operands(0);
    if (!ops.has_value()) {
        original(self, rot);
        return;
    }
    regenny::LTRotation outer{};
    outer.x = ops->outer[0];
    outer.y = ops->outer[1];
    outer.z = ops->outer[2];
    outer.w = ops->outer[3];

    // Identity outer means nothing is being added, so the correction is the identity. Skipping it
    // keeps the mod genuinely inert rather than multiplying by 1 several hundred times a second.
    if (std::fabs(outer.w) > 0.99999f) {
        original(self, rot);
        return;
    }

    regenny::LTRotation conj{};
    conj.x = -outer.x;
    conj.y = -outer.y;
    conj.z = -outer.z;
    conj.w = outer.w;

    const auto corrected = sdk::multiply_rotations(conj, *rot);

    // What we changed, as an angle -- reported rather than assumed, because "the correction ran"
    // and "the correction did something" are different claims.
    const auto a = sdk::forward_of(*rot);
    const auto b = sdk::forward_of(corrected);
    float d = a.x * b.x + a.y * b.y + a.z * b.z;
    d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
    g_last_correction.store(std::acos(d), std::memory_order_relaxed);
    g_corrected.fetch_add(1, std::memory_order_relaxed);

    // A COPY, never the caller's buffer. The engine may reuse or read that rotation after this
    // returns, and rewriting it in place would leak our correction into whatever else looks at it.
    original(self, &corrected);
}

} // namespace

std::optional<std::string> ViewmodelDecouple::on_initialize() {
    // Nothing to hook yet: the entry is resolved FROM THE OBJECT, and no player exists until a
    // world is loaded. on_frame installs it once one does.
    return std::nullopt;
}

void ViewmodelDecouple::on_frame() {
    const auto shell = sdk::CClientShell::local_player(0);
    const uintptr_t obj = shell.has_value() ? reinterpret_cast<uintptr_t>(shell->object) : 0;
    g_object.store(obj, std::memory_order_relaxed);
    if (obj == 0 || g_target.load(std::memory_order_relaxed) != 0) {
        return;
    }

    // The object names its own setter (vtable slot 4), so there is no address to hardcode and the
    // resolution is self-validating: a wrong object type yields an entry outside the exe and
    // object_rotation_setter refuses it.
    const auto fn = sdk::object_rotation_setter(shell->object);
    if (fn == 0) {
        return;
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(fn),
                              reinterpret_cast<void*>(&set_rotation_detour))) {
        LOGX("[viewmodel] could not hook LTObject::SetRotation at 0x%08" PRIXPTR, fn);
        g_target.store(0, std::memory_order_relaxed);
        return;
    }
    g_target.store(fn, std::memory_order_relaxed);
    LOGX("[viewmodel] owning LTObject::SetRotation at 0x%08" PRIXPTR " for object 0x%08" PRIXPTR, fn, obj);
}

void ViewmodelDecouple::set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

bool ViewmodelDecouple::enabled() const {
    return g_enabled.load(std::memory_order_relaxed);
}

ViewmodelDecouple::Observed ViewmodelDecouple::observed() const {
    Observed out{};
    out.target = g_target.load(std::memory_order_relaxed);
    out.hooked = out.target != 0 && Hooks::get().find(kHookName) != nullptr;
    out.enabled = g_enabled.load(std::memory_order_relaxed);
    out.calls = g_calls.load(std::memory_order_relaxed);
    out.matched = g_matched.load(std::memory_order_relaxed);
    out.corrected = g_corrected.load(std::memory_order_relaxed);
    out.last_correction = g_last_correction.load(std::memory_order_relaxed);
    out.object_resolved = g_object.load(std::memory_order_relaxed) != 0;
    return out;
}

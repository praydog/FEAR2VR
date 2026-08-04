#include "HudGeomProbe.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Modules.hpp"

namespace {

// The type-1 draw wrapper, FEAR2.exe 0x0046F715, __thiscall(elem).
constexpr uintptr_t kTypeOneDrawRva = 0x06F715;
constexpr const char* kHookName = "HUD_TypeOneDraw";

// Inside `elem[1]`: the 2x3 affine that places the interface's content.
constexpr uint32_t kAffineOffset = 152;

// Measured on the confirmed HUD element (see ENGINE_NOTES): the element's own vtable and the inner
// geometry object's. Both are checked because this wrapper serves more than one object.
constexpr uint32_t kElementVtable = 0x00678640;
constexpr uint32_t kInnerVtable = 0x00687DF8;

std::atomic<bool> g_hooked{false};
std::atomic<uint32_t> g_retry{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<uintptr_t> g_element{0};
std::atomic<uintptr_t> g_inner{0};

bool read_dword(uintptr_t at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall draw_detour(uint32_t* elem, void* /*edx*/) {
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    if (elem != nullptr) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        // IDENTIFY THE INSTANCE, NOT JUST THE CALL. This wrapper is reached for more than one
        // object -- an unfiltered version logged elem/inner pairs in gameserver.dll's range too --
        // so watching whatever came through last would point a hardware breakpoint at unrelated
        // module data. Both vtables were measured on the confirmed HUD element and both must match.
        uint32_t elem_vtbl = 0;
        uint32_t inner = 0;
        uint32_t inner_vtbl = 0;
        if (read_dword(reinterpret_cast<uintptr_t>(elem), &elem_vtbl) &&
            elem_vtbl == kElementVtable &&
            read_dword(reinterpret_cast<uintptr_t>(elem) + 4, &inner) && inner != 0 &&
            read_dword(inner, &inner_vtbl) && inner_vtbl == kInnerVtable) {
            const auto prev = g_inner.exchange(inner, std::memory_order_relaxed);
            g_element.store(reinterpret_cast<uintptr_t>(elem), std::memory_order_relaxed);
            // Only when it moves, so a session logs one line per allocation rather than one a frame.
            // BOUNDED. Both vtables match on MANY objects -- 2235 distinct instances in one
            // session -- so they identify a Scaleform element class, not the HUD. Until something
            // narrows it to the interface itself, this is a lead to follow rather than an address
            // to trust, and it must not fill the log saying so.
            static std::atomic<uint32_t> s_logged{0};
            if (prev != inner && s_logged.fetch_add(1, std::memory_order_relaxed) < 3) {
                LOGX("[hudgeom] element=0x%08X inner=0x%08X affine=0x%08X  "
                     "-> /watch/arm?addr=0x%08X&size=4&type=write",
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(elem)), inner,
                     inner + kAffineOffset, inner + kAffineOffset);
            }
        }
    }
    return hook->original<int(__fastcall*)(uint32_t*, void*)>()(elem, nullptr);
}

}  // namespace

HudGeomProbe& HudGeomProbe::get() {
    static HudGeomProbe s_instance;
    return s_instance;
}

void HudGeomProbe::on_frame() {
    if (g_hooked.load(std::memory_order_acquire)) {
        return;
    }
    if ((g_retry.fetch_add(1, std::memory_order_relaxed) & 15u) == 0) {
        try_install();
    }
}

void HudGeomProbe::try_install() {
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return;
    }
    if (Hooks::get().install(kHookName, reinterpret_cast<void*>(exe->base + kTypeOneDrawRva),
                             reinterpret_cast<void*>(&draw_detour))) {
        g_hooked.store(true, std::memory_order_release);
    }
}

HudGeomProbe::State HudGeomProbe::state() const {
    State s{};
    s.hooked = g_hooked.load(std::memory_order_acquire);
    s.element = g_element.load(std::memory_order_relaxed);
    s.inner = g_inner.load(std::memory_order_relaxed);
    s.affine_addr = s.inner != 0 ? s.inner + kAffineOffset : 0;
    s.calls = g_calls.load(std::memory_order_relaxed);
    return s;
}

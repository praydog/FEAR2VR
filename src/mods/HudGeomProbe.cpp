#include "HudGeomProbe.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Modules.hpp"

namespace {

// Screen2D_IssuePass_Shared, gameclient.dll RVA 0x30E10 -- the HUD's OWN 2D pass. Its loop walks
// four element slots: type at this[24] stepping 8, element at this[61] stepping 6.
//
// This is the discriminator the wrapper could not provide. Hooking the type-1 DRAW instead matched
// 2235 instances in one session, because the element vtables identify a Scaleform class rather than
// the interface. The pass is the HUD's context by construction, so the type-1 slot inside it is the
// HUD's element and nothing else.
constexpr uintptr_t kIssuePassSharedRva = 0x30E10;
constexpr const char* kHookName = "Screen2D_IssuePass_Shared";
constexpr size_t kTypeIndex = 24;
constexpr size_t kTypeStride = 8;
constexpr size_t kElemIndex = 61;
constexpr size_t kElemStride = 6;

// Inside `elem[1]`: the 2x3 affine that places the interface's content.
constexpr uint32_t kAffineOffset = 152;

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

char __fastcall pass_detour(uint32_t* self, void* /*edx*/) {
    if (self != nullptr) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < 4; ++i) {
            uint32_t type = 0;
            uint32_t elem = 0;
            if (!read_dword(reinterpret_cast<uintptr_t>(&self[kTypeIndex + i * kTypeStride]),
                            &type) ||
                type != 1 ||
                !read_dword(reinterpret_cast<uintptr_t>(&self[kElemIndex + i * kElemStride]),
                            &elem) ||
                elem == 0) {
                continue;
            }
            uint32_t inner = 0;
            if (!read_dword(elem + 4, &inner) || inner == 0) {
                continue;
            }
            const auto prev = g_inner.exchange(inner, std::memory_order_relaxed);
            g_element.store(elem, std::memory_order_relaxed);
            static std::atomic<uint32_t> s_logged{0};
            if (prev != inner && s_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
                LOGX("[hudgeom] HUD slot%zu elem=0x%08X inner=0x%08X affine=0x%08X  "
                     "-> /watch/arm?addr=0x%08X&size=4&type=write",
                     i, elem, inner, inner + kAffineOffset, inner + kAffineOffset);
            }
            break;
        }
    }
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 1;
    }
    return hook->original<char(__fastcall*)(uint32_t*, void*)>()(self, nullptr);
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
    const auto* gc = sdk::Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return;
    }
    if (Hooks::get().install(kHookName, reinterpret_cast<void*>(gc->base + kIssuePassSharedRva),
                             reinterpret_cast<void*>(&pass_detour))) {
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

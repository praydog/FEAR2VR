#include "HudProbe.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

#include <d3d9.h>

#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Modules.hpp"
#include "sdk/PlayerMgr.hpp"
#include "sdk/Render.hpp"

namespace {

// Player::holder is the engine's `object + 252` -- the same pointer HUD_ClampElementPos walks to in
// its mode-2 branch. The rect sits at DWORD indices 49..52 of it.
constexpr size_t kRectByteOffset = 49 * sizeof(int32_t);

std::atomic<bool> g_have{false};
std::atomic<uintptr_t> g_holder{0};
std::atomic<int32_t> g_rect[4]{};
std::atomic<uint32_t> g_screen_w{0};
std::atomic<uint32_t> g_screen_h{0};
std::atomic<uint32_t> g_tick{0};

// SEH-guarded because the holder is engine memory that can be torn down between the SDK handing it
// over and this read -- AGENTS.md rule on caller-provided dereferences. POD-only scope, no locals
// with destructors, or MSVC rejects the __try.
bool read_rect(uintptr_t holder, int32_t out[4]) {
    __try {
        const auto* r = reinterpret_cast<const int32_t*>(holder + kRectByteOffset);
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
        out[3] = r[3];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// Screen2D_IssuePass_Shared. Its loop walks four element slots -- type at this[24] stepping 8, and
// the element pointer at this[61] stepping 6, both read straight off the decompile. Type 1 means
// the element draws ITSELF through its own vtable +24 and the pass builds no geometry, so whichever
// slot is type 1 owns the arcs' geometry.
//
// ONE hook, on the pass, rather than four on the element vtables: the pass's convention is already
// verified (__thiscall, no args) and hooking it reaches every element through the same object,
// which is the smaller crash surface for the same information.
constexpr uintptr_t kIssuePassSharedRva = 0x30E10;
constexpr const char* kPassHookName = "Screen2D_IssuePass_Shared";
constexpr size_t kTypeIndex = 24;
constexpr size_t kTypeStride = 8;
constexpr size_t kElemIndex = 61;
constexpr size_t kElemStride = 6;
constexpr uintptr_t kScaleGlobalRva = 0x2E34F4;  // flt_6E34F4 in FEAR2.exe

std::atomic<bool> g_pass_hooked{false};
std::atomic<uint32_t> g_pass_logged{0};

// POD-only and SEH-guarded: engine pointers read off a live object mid-frame.
bool read_dword(uintptr_t at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

char __fastcall issue_pass_detour(uint32_t* self, void* /*edx*/) {
    if (self != nullptr && g_pass_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        uint32_t sw = 0;
        uint32_t sh = 0;
        if (const auto pp = sdk::Render::present_params()) {
            sw = pp->BackBufferWidth;
            sh = pp->BackBufferHeight;
        }
        for (size_t i = 0; i < 4; ++i) {
            uint32_t type = 0;
            uint32_t elem = 0;
            if (!read_dword(reinterpret_cast<uintptr_t>(&self[kTypeIndex + i * kTypeStride]),
                            &type) ||
                !read_dword(reinterpret_cast<uintptr_t>(&self[kElemIndex + i * kElemStride]),
                            &elem)) {
                continue;
            }
            uint32_t vtbl = 0;
            uint32_t draw = 0;
            if (elem != 0 && read_dword(elem, &vtbl) && vtbl != 0) {
                read_dword(vtbl + 24, &draw);
            }
            // flt_6E34F4 gates the element's own scale setter: the draw wrapper at FEAR2.exe
            // 0x46F715 only pushes a scale through vtable +24 when this global is >= 0. So it is
            // an override the engine already supports and normally leaves off.
            float scale_global = -1.0f;
            const auto* exe = sdk::Modules::get().exe();
            if (exe != nullptr && exe->base != 0) {
                uint32_t bits = 0;
                if (read_dword(exe->base + kScaleGlobalRva, &bits)) {
                    std::memcpy(&scale_global, &bits, sizeof(scale_global));
                }
            }
            // The draw wrapper ends in (*(this[1] vtable + 116))(this[1]). this[1] is a DIFFERENT
            // object from the element, so its vtable is NOT the element's -- it has to be resolved
            // at runtime, which is the whole reason this is logged rather than read out of IDA.
            uint32_t inner = 0;
            uint32_t inner_vtbl = 0;
            uint32_t submit = 0;
            if (elem != 0 && read_dword(elem + 4, &inner) && inner != 0 &&
                read_dword(inner, &inner_vtbl) && inner_vtbl != 0) {
                read_dword(inner_vtbl + 116, &submit);
            }
            // Inside the submit (FEAR2.exe 0x57BA90) four floats off `inner` are handed to a
            // vtable +20 call as the destination rect: +136, +144, +140, +148, with +108 alongside.
            // These ARE the submitted geometry -- the thing every earlier round inferred from a
            // bounding box instead of reading.
            float g[5]{};
            if (inner != 0) {
                const uint32_t off[5] = {108, 136, 140, 144, 148};
                for (int j = 0; j < 5; ++j) {
                    uint32_t bits = 0;
                    if (read_dword(inner + off[j], &bits)) {
                        std::memcpy(&g[j], &bits, sizeof(float));
                    }
                }
            }
            // sub_5E2400 copies SIX floats from inner+152 into a GMatrix2D -- this is Scaleform
            // GFx, and those six are the 2x3 affine that actually places the content. If the scale
            // at +108 is right and the content is still small, this is where it is lost.
            float m[6]{};
            if (inner != 0) {
                for (int j = 0; j < 6; ++j) {
                    uint32_t bits = 0;
                    if (read_dword(inner + 152 + 4u * static_cast<uint32_t>(j), &bits)) {
                        std::memcpy(&m[j], &bits, sizeof(float));
                    }
                }
            }
            LOGX("[hudprobe] screen=%ux%u +108=%.3f rect(%.1f,%.1f,%.1f,%.1f) "
                 "mtx[%.5f %.5f %.5f %.5f %.5f %.5f]",
                 sw, sh, g[0], g[1], g[2], g[3], g[4], m[0], m[1], m[2], m[3], m[4], m[5]);
        }
    }
    auto* hook = Hooks::get().find(kPassHookName);
    if (hook == nullptr) {
        return 1;
    }
    return hook->original<char(__fastcall*)(uint32_t*, void*)>()(self, nullptr);
}

}  // namespace

HudProbe& HudProbe::get() {
    static HudProbe s_instance;
    return s_instance;
}

void HudProbe::on_frame() {
    // Once a second at 60fps. This is a measurement, not a feature, and the rect only changes when
    // the engine relays out the interface.
    if ((g_tick.fetch_add(1, std::memory_order_relaxed) % 60u) != 0) {
        return;
    }
    if (!g_pass_hooked.load(std::memory_order_acquire)) {
        const auto* gc = sdk::Modules::get().game_client();
        if (gc != nullptr && gc->base != 0 &&
            Hooks::get().install(kPassHookName,
                                 reinterpret_cast<void*>(gc->base + kIssuePassSharedRva),
                                 reinterpret_cast<void*>(&issue_pass_detour))) {
            g_pass_hooked.store(true, std::memory_order_release);
        }
    }

    const auto player = sdk::PlayerMgr::local_player();
    if (!player.has_value() || player->holder == 0) {
        return;
    }
    int32_t rect[4]{};
    if (!read_rect(player->holder, rect)) {
        return;
    }

    uint32_t sw = 0;
    uint32_t sh = 0;
    if (const auto pp = sdk::Render::present_params()) {
        sw = pp->BackBufferWidth;
        sh = pp->BackBufferHeight;
    }

    const bool changed = !g_have.load(std::memory_order_acquire) ||
                         g_rect[0].load(std::memory_order_relaxed) != rect[0] ||
                         g_rect[1].load(std::memory_order_relaxed) != rect[1] ||
                         g_rect[2].load(std::memory_order_relaxed) != rect[2] ||
                         g_rect[3].load(std::memory_order_relaxed) != rect[3];

    for (int i = 0; i < 4; ++i) {
        g_rect[i].store(rect[i], std::memory_order_relaxed);
    }
    g_holder.store(player->holder, std::memory_order_relaxed);
    g_screen_w.store(sw, std::memory_order_relaxed);
    g_screen_h.store(sh, std::memory_order_relaxed);
    g_have.store(true, std::memory_order_release);

    if (changed) {
        LOGX("[hudprobe] mode-2 rect (%d,%d)-(%d,%d) = %dx%d, back buffer %ux%u, holder 0x%08X",
             rect[0], rect[1], rect[2], rect[3], rect[2] - rect[0], rect[3] - rect[1], sw, sh,
             static_cast<unsigned>(player->holder));
    }
}

HudProbe::State HudProbe::state() const {
    State s{};
    s.have = g_have.load(std::memory_order_acquire);
    s.holder = g_holder.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        s.rect[i] = g_rect[i].load(std::memory_order_relaxed);
    }
    s.screen_w = g_screen_w.load(std::memory_order_relaxed);
    s.screen_h = g_screen_h.load(std::memory_order_relaxed);
    return s;
}

void HudProbe::set_scale(float v) {
    // Writes flt_6E34F4, which the HUD element's draw wrapper (FEAR2.exe 0x46F715) consults every
    // frame: at >= 0 it pushes the value through the element's own scale setter (vtable +24), and
    // below 0 it leaves the element alone. So this is the engine's OWN override, not a patch --
    // -1 restores stock behaviour exactly.
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return;
    }
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    *reinterpret_cast<uint32_t*>(exe->base + kScaleGlobalRva) = bits;
    LOGX("[hudprobe] ui scale global -> %.4f", v);
}

float HudProbe::scale() const {
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return -1.0f;
    }
    float v = -1.0f;
    std::memcpy(&v, reinterpret_cast<const void*>(exe->base + kScaleGlobalRva), sizeof(v));
    return v;
}

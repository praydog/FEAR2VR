#include "HudPassHook.hpp"

#include <atomic>
#include <intrin.h>
#include <cstring>

#include "sdk/SceneCamera.hpp"

#include "Hooks.hpp"
#include "Log.hpp"
#include "RenderHook.hpp"

namespace {

constexpr const char* kHookName = "CLTRenderer::SetupPassAffine";
constexpr const char* kStoredHookName = "CLTRenderer::SetupPassStored";

std::atomic<uintptr_t> g_target{0};
std::atomic<uint64_t> g_passes{0};
std::atomic<uint32_t> g_this_frame{0};
std::atomic<uint32_t> g_last_frame{0};
std::atomic<float> g_rect[4]{{0.0f}, {0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_depth_min{0.0f};
std::atomic<float> g_depth_max{0.0f};
std::atomic<int32_t> g_viewport[4]{{0}, {0}, {0}, {0}};
std::atomic<bool> g_ortho{false};
std::atomic<bool> g_record_read{false};

std::atomic<uintptr_t> g_stored_target{0};
std::atomic<uint64_t> g_stored_passes{0};
std::atomic<uint32_t> g_stored_this_frame{0};
HudPassHook::PassCallback g_pass_cbs[HudPassHook::kMaxPassCallbacks]{};
std::atomic<size_t> g_pass_cb_count{0};
std::atomic<uint32_t> g_stored_last_frame{0};
std::atomic<bool> g_stored_ortho{false};
std::atomic<int32_t> g_stored_viewport[4]{{0}, {0}, {0}, {0}};
// A tiny fixed table: the caller set is expected to be small, and a growing container on a render-thread
// hot path would be a worse bug than a truncated census.
constexpr size_t kMaxCallers = 8;
std::atomic<uintptr_t> g_callers[kMaxCallers]{};
std::atomic<uint32_t> g_caller_counts[kMaxCallers]{};

void note_caller(uintptr_t ret) {
    for (size_t i = 0; i < kMaxCallers; ++i) {
        uintptr_t have = g_callers[i].load(std::memory_order_relaxed);
        if (have == ret) {
            g_caller_counts[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (have == 0) {
            uintptr_t expected = 0;
            if (g_callers[i].compare_exchange_strong(expected, ret, std::memory_order_relaxed)) {
                g_caller_counts[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (g_callers[i].load(std::memory_order_relaxed) == ret) {
                g_caller_counts[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
}

std::atomic<bool> g_off_armed{false};
std::atomic<int32_t> g_off_req[2]{{0}, {0}};
std::atomic<uint64_t> g_off_writes{0};
std::atomic<bool> g_off_read{false};
std::atomic<bool> g_off_gate{false};
std::atomic<int32_t> g_off_eff[2]{{0}, {0}};
std::atomic<int32_t> g_off_stored[2]{{0}, {0}};

using SetupFn = char(__stdcall*)(const void*, const float*, const float*, float, float);

char __stdcall setup_detour(const void* camera, const float* fov, const float* rect, float depth_min,
                            float depth_max) {
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    const char r = hook->original<SetupFn>()(camera, fov, rect, depth_min, depth_max);

    g_passes.fetch_add(1, std::memory_order_relaxed);
    g_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (rect != nullptr) {
        for (size_t i = 0; i < 4; ++i) {
            g_rect[i].store(rect[i], std::memory_order_relaxed);
        }
    }
    g_depth_min.store(depth_min, std::memory_order_relaxed);
    g_depth_max.store(depth_max, std::memory_order_relaxed);

    // IN PHASE, because that is the only place the record describes THIS pass. The same read from another
    // thread lands on whichever pass ran last.
    if (const auto scam = sdk::SceneCamera::snapshot()) {
        g_viewport[0].store(scam->viewport_left, std::memory_order_relaxed);
        g_viewport[1].store(scam->viewport_top, std::memory_order_relaxed);
        g_viewport[2].store(scam->viewport_right, std::memory_order_relaxed);
        g_viewport[3].store(scam->viewport_bottom, std::memory_order_relaxed);
        g_ortho.store(!scam->is_perspective_projection(), std::memory_order_relaxed);
        g_record_read.store(true, std::memory_order_relaxed);
    }
    return r;
}

// Slot 17 takes ONE argument, per the vtable wrapper. Its purpose was never established, so this observes
// it rather than assuming: what matters is whether it is the entry that leaves the record orthographic.
using StoredFn = char(__stdcall*)(int);

char __stdcall stored_detour(int a1) {
    auto* hook = Hooks::get().find(kStoredHookName);
    if (hook == nullptr) {
        return 0;
    }
    // BEFORE the original, because the original is what reads the descriptor and derives the rect. Writing
    // after it would land on a value nothing reads until the engine has already rebuilt it.
    if (g_off_armed.load(std::memory_order_relaxed)) {
        if (sdk::SceneCamera::set_pass_offset(g_off_req[0].load(std::memory_order_relaxed),
                                              g_off_req[1].load(std::memory_order_relaxed))) {
            g_off_writes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Consumers see the pass BEFORE the engine sets it up, and are told which one it is within the
    // frame -- see the header for why the ordinal matters.
    {
        const uint32_t ordinal = g_stored_this_frame.load(std::memory_order_relaxed);
        const size_t n = g_pass_cb_count.load(std::memory_order_acquire);
        for (size_t k = 0; k < n; ++k) {
            if (auto cb = g_pass_cbs[k]; cb != nullptr) {
                cb(ordinal);
            }
        }
    }
    note_caller(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const char r = hook->original<StoredFn>()(a1);
    g_stored_passes.fetch_add(1, std::memory_order_relaxed);
    g_stored_this_frame.fetch_add(1, std::memory_order_relaxed);
    if (const auto scam = sdk::SceneCamera::snapshot()) {
        g_stored_ortho.store(!scam->is_perspective_projection(), std::memory_order_relaxed);
        g_stored_viewport[0].store(scam->viewport_left, std::memory_order_relaxed);
        g_stored_viewport[1].store(scam->viewport_top, std::memory_order_relaxed);
        g_stored_viewport[2].store(scam->viewport_right, std::memory_order_relaxed);
        g_stored_viewport[3].store(scam->viewport_bottom, std::memory_order_relaxed);
    }
    // In phase, where the descriptor actually points at the bound target.
    if (const auto gate = sdk::SceneCamera::pass_offset_enabled()) {
        g_off_gate.store(*gate, std::memory_order_relaxed);
        g_off_read.store(true, std::memory_order_relaxed);
        if (const auto eff = sdk::SceneCamera::pass_offset()) {
            g_off_eff[0].store((*eff)[0], std::memory_order_relaxed);
            g_off_eff[1].store((*eff)[1], std::memory_order_relaxed);
        }
        if (const auto st = sdk::SceneCamera::pass_offset_stored()) {
            g_off_stored[0].store((*st)[0], std::memory_order_relaxed);
            g_off_stored[1].store((*st)[1], std::memory_order_relaxed);
        }
    } else {
        g_off_read.store(false, std::memory_order_relaxed);
    }
    return r;
}

void close_frame() {
    g_last_frame.store(g_this_frame.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    g_stored_last_frame.store(g_stored_this_frame.exchange(0, std::memory_order_relaxed),
                              std::memory_order_relaxed);
}

} // namespace

std::optional<std::string> HudPassHook::on_initialize() {
    const auto fn = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::SetupPassAffine);
    if (fn == 0) {
        return std::string{"could not resolve CLTRenderer vtable slot 16"};
    }
    g_target.store(fn, std::memory_order_relaxed);
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(fn),
                              reinterpret_cast<void*>(&setup_detour))) {
        return std::string{"could not hook the affine pass setup"};
    }
    if (!RenderHook::get().add_present_callback(&close_frame)) {
        LOGX("[hudpass] no frame-boundary callback -- per-frame count unavailable");
    }
    LOGX("[hudpass] hooked slot 16 at 0x%08X", static_cast<unsigned>(fn));

    // Slot 17 beside it. Not fatal if absent: the mod's job is to report which one runs.
    const auto stored = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::SetupPassStored);
    g_stored_target.store(stored, std::memory_order_relaxed);
    if (stored != 0 && Hooks::get().install(kStoredHookName, reinterpret_cast<void*>(stored),
                                            reinterpret_cast<void*>(&stored_detour))) {
        LOGX("[hudpass] hooked slot 17 at 0x%08X", static_cast<unsigned>(stored));
    } else {
        LOGX("[hudpass] slot 17 unavailable -- stored-pass counts will stay zero");
    }
    return std::nullopt;
}

void HudPassHook::set_offset(int32_t x, int32_t y) {
    g_off_req[0].store(x, std::memory_order_relaxed);
    g_off_req[1].store(y, std::memory_order_relaxed);
    g_off_armed.store(true, std::memory_order_relaxed);
}

void HudPassHook::clear_offset() {
    g_off_armed.store(false, std::memory_order_relaxed);
    // Put the engine's own value back so the release is immediate rather than waiting for a rebuild.
    sdk::SceneCamera::set_pass_offset(0, 0);
}

HudPassHook::Observed HudPassHook::observed() const {
    Observed out{};
    out.hooked = Hooks::get().find(kHookName) != nullptr;
    out.target = g_target.load(std::memory_order_relaxed);
    out.passes = g_passes.load(std::memory_order_relaxed);
    out.passes_last_frame = g_last_frame.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 4; ++i) {
        out.rect[i] = g_rect[i].load(std::memory_order_relaxed);
        out.viewport[i] = g_viewport[i].load(std::memory_order_relaxed);
    }
    out.depth_min = g_depth_min.load(std::memory_order_relaxed);
    out.depth_max = g_depth_max.load(std::memory_order_relaxed);
    out.ortho = g_ortho.load(std::memory_order_relaxed);
    out.record_read = g_record_read.load(std::memory_order_relaxed);
    out.stored_hooked = Hooks::get().find(kStoredHookName) != nullptr;
    out.stored_passes = g_stored_passes.load(std::memory_order_relaxed);
    out.stored_last_frame = g_stored_last_frame.load(std::memory_order_relaxed);
    out.stored_ortho = g_stored_ortho.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 4; ++i) {
        out.stored_viewport[i] = g_stored_viewport[i].load(std::memory_order_relaxed);
    }
    out.offset_read = g_off_read.load(std::memory_order_relaxed);
    out.offset_armed = g_off_armed.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMaxCallers && i < out.callers.size(); ++i) {
        out.callers[i] = g_callers[i].load(std::memory_order_relaxed);
        out.caller_counts[i] = g_caller_counts[i].load(std::memory_order_relaxed);
        if (out.callers[i] != 0) {
            ++out.distinct_callers;
        }
    }
    out.offset_writes = g_off_writes.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 2; ++i) {
        out.offset_requested[i] = g_off_req[i].load(std::memory_order_relaxed);
    }
    out.offset_gate = g_off_gate.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 2; ++i) {
        out.offset_effective[i] = g_off_eff[i].load(std::memory_order_relaxed);
        out.offset_stored[i] = g_off_stored[i].load(std::memory_order_relaxed);
    }
    return out;
}

bool HudPassHook::add_pass_callback(PassCallback cb) {
    if (cb == nullptr) {
        return false;
    }
    const size_t n = g_pass_cb_count.load(std::memory_order_relaxed);
    if (n >= kMaxPassCallbacks) {
        return false;
    }
    g_pass_cbs[n] = cb;
    g_pass_cb_count.store(n + 1, std::memory_order_release);
    return true;
}

#include "RenderTimeline.hpp"

#include "CameraPassHook.hpp"
#include "Hooks.hpp"
#include "HudPassHook.hpp"
#include "Log.hpp"
#include "RenderHook.hpp"
#include "sdk/SceneCamera.hpp"

namespace {

constexpr const char* kBeginHook = "render_timeline_begin_target";
constexpr const char* kEndHook = "render_timeline_end_target";

// CLTRenderer slot 11: bool __stdcall(target, unknown, offset_pair, unknown).
// Slot 12: char __stdcall(finish_flag). Both are the vtable WRAPPERS, which is what
// HudPassHook hooks too -- they take their arguments on the stack, so a __stdcall
// detour matches even though the entry is reached as a method.
using BeginFn = bool(__stdcall*)(int, int, int*, int);
using EndFn = char(__stdcall*)(int);

bool __stdcall begin_detour(int target, int a2, int* offsets, int a4) {
    auto* hook = Hooks::get().find(kBeginHook);
    if (hook == nullptr) {
        return 0;
    }
    // AFTER the original, because the size we want is the one the ENGINE derived for
    // this target -- it is written into the scene renderer during the call. Reading
    // before would report the previous target's dimensions.
    const bool r = hook->original<BeginFn>()(target, a2, offsets, a4);
    RenderTimeline::get().record(RenderTimeline::Kind::BeginTarget,
                                 static_cast<uintptr_t>(static_cast<unsigned>(target)));
    return r;
}

char __stdcall end_detour(int a1) {
    auto* hook = Hooks::get().find(kEndHook);
    if (hook == nullptr) {
        return 0;
    }
    // BEFORE the original: the size and counters still describe the target being
    // closed. Afterwards the renderer has already stepped back to state 2.
    RenderTimeline::get().record(RenderTimeline::Kind::EndTarget, 0);
    return hook->original<EndFn>()(a1);
}

void close_frame_cb() {
    RenderTimeline::get().close_frame();
}

} // namespace

RenderTimeline& RenderTimeline::get() {
    static RenderTimeline instance;
    return instance;
}

std::optional<std::string> RenderTimeline::on_initialize() {
    const auto begin_fn = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::BeginRenderTarget);
    const auto end_fn = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::EndRenderTarget);
    if (begin_fn == 0 || end_fn == 0) {
        return std::string{"could not resolve CLTRenderer render-target slots 11/12"};
    }

    if (!Hooks::get().install(kBeginHook, reinterpret_cast<void*>(begin_fn),
                              reinterpret_cast<void*>(&begin_detour))) {
        return std::string{"could not hook BeginRenderTarget"};
    }
    if (!Hooks::get().install(kEndHook, reinterpret_cast<void*>(end_fn),
                              reinterpret_cast<void*>(&end_detour))) {
        return std::string{"could not hook EndRenderTarget"};
    }
    if (!RenderHook::get().add_present_callback(&close_frame_cb)) {
        LOGX("[timeline] no frame-boundary callback -- recordings will never close");
    }

    m_hooked.store(true, std::memory_order_relaxed);
    LOGX("[timeline] hooked render-target slots 11/12 at 0x%08X / 0x%08X",
         static_cast<unsigned>(begin_fn), static_cast<unsigned>(end_fn));
    return std::nullopt;
}

void RenderTimeline::arm() {
    m_write.store(0, std::memory_order_relaxed);
    m_count.store(0, std::memory_order_release);
    m_overflow.store(0, std::memory_order_relaxed);
    m_armed.store(true, std::memory_order_release);
}

void RenderTimeline::record(Kind kind, uintptr_t target) {
    if (kind == Kind::BeginTarget) {
        m_begins.fetch_add(1, std::memory_order_relaxed);
    } else if (kind == Kind::EndTarget) {
        m_ends.fetch_add(1, std::memory_order_relaxed);
    }

    // Arming takes effect at the NEXT frame boundary, so a recording always starts at
    // a frame's first target rather than halfway through the one in flight.
    if (!m_recording.load(std::memory_order_acquire)) {
        return;
    }

    const size_t i = m_write.load(std::memory_order_relaxed);
    if (i >= kMaxEvents) {
        m_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Event e{};
    e.kind = kind;
    e.target = target;
    if (const auto sz = sdk::SceneCamera::current_target_size()) {
        e.size = *sz;
    }
    // RELATIVE TO THE RECORDING, not to injection. Absolute totals make the first event look
    // like it contained every pass since the mod loaded, which is exactly how a reader
    // concludes the wrong bracket drew the HUD.
    e.camera_passes = CameraPassHook::get().observed().passes - m_base_cam;
    e.hud_passes = HudPassHook::get().observed().stored_passes - m_base_hud;
    m_events[i] = e;
    m_write.store(i + 1, std::memory_order_relaxed);
}

void RenderTimeline::close_frame() {
    if (m_recording.load(std::memory_order_acquire)) {
        // Close the recording with the frame boundary itself, so a reader can see what
        // ran AFTER the last target was released -- which is exactly where a UI layer
        // would have to be captured.
        const size_t i = m_write.load(std::memory_order_relaxed);
        if (i < kMaxEvents) {
            Event e{};
            e.kind = Kind::Present;
            e.camera_passes = CameraPassHook::get().observed().passes - m_base_cam;
            e.hud_passes = HudPassHook::get().observed().stored_passes - m_base_hud;
            m_events[i] = e;
            m_write.store(i + 1, std::memory_order_relaxed);
        }
        m_recording.store(false, std::memory_order_release);
        m_count.store(m_write.load(std::memory_order_relaxed), std::memory_order_release);
        return;
    }

    if (m_armed.exchange(false, std::memory_order_acq_rel)) {
        m_write.store(0, std::memory_order_relaxed);
        m_base_cam = CameraPassHook::get().observed().passes;
        m_base_hud = HudPassHook::get().observed().stored_passes;
        m_recording.store(true, std::memory_order_release);
    }
}

RenderTimeline::Event RenderTimeline::event(size_t i) const {
    if (i >= m_count.load(std::memory_order_acquire)) {
        return Event{};
    }
    return m_events[i];
}

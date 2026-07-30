#include "CameraPassHook.hpp"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "sdk/SceneCamera.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "CLTRenderer::SetupPassPerspective";

std::atomic<uint64_t> g_passes{0};
std::atomic<uint64_t> g_overridden{0};
std::atomic<uint64_t> g_rejected{0};
std::atomic<uint8_t> g_eye{static_cast<uint8_t>(CameraPassHook::Eye::Off)};
std::atomic<float> g_half_ipd{0.0f};
std::atomic<bool> g_split{false};
std::atomic<float> g_fov_x{0.0f};
std::atomic<float> g_fov_y{0.0f};
std::atomic<uintptr_t> g_target{0};

// Last arguments seen, published for diagnostics. Plain floats behind an atomic sequence would be tidier, but
// these are read for reporting rather than for control, and a torn pair here misleads nobody.
std::atomic<float> g_pos[3]{};
std::atomic<float> g_rot[4]{};
std::atomic<float> g_fov_seen[2]{};
std::atomic<float> g_rect_seen[4]{};
std::atomic<float> g_depth_min{0.0f};
std::atomic<float> g_depth_max{0.0f};

// __stdcall, five stack arguments, `retn 14h`. NOT __thiscall despite being vtable slot 15: the wrapper loads
// ecx with g_SceneRenderer itself and never reads an incoming this.
using SetupFn = char(__stdcall*)(const regenny::LTNodeTransform*, const float*, const float*, float, float);

char __stdcall setup_pass_detour(const regenny::LTNodeTransform* camera, const float* fov, const float* rect,
                                 float depth_min, float depth_max) {
    g_passes.fetch_add(1, std::memory_order_relaxed);

    // CAPTURE FIRST, and capture the ARGUMENTS. These are in phase with the frame they configure; the record
    // read from the IPC thread afterwards is not, which is the distinction this project keeps relearning.
    if (camera != nullptr) {
        g_pos[0].store(camera->position.x, std::memory_order_relaxed);
        g_pos[1].store(camera->position.y, std::memory_order_relaxed);
        g_pos[2].store(camera->position.z, std::memory_order_relaxed);
        g_rot[0].store(camera->rotation.x, std::memory_order_relaxed);
        g_rot[1].store(camera->rotation.y, std::memory_order_relaxed);
        g_rot[2].store(camera->rotation.z, std::memory_order_relaxed);
        g_rot[3].store(camera->rotation.w, std::memory_order_relaxed);
    }
    if (fov != nullptr) {
        g_fov_seen[0].store(fov[0], std::memory_order_relaxed);
        g_fov_seen[1].store(fov[1], std::memory_order_relaxed);
    }
    if (rect != nullptr) {
        for (size_t i = 0; i < 4; ++i) {
            g_rect_seen[i].store(rect[i], std::memory_order_relaxed);
        }
    }
    g_depth_min.store(depth_min, std::memory_order_relaxed);
    g_depth_max.store(depth_max, std::memory_order_relaxed);

    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    const auto original = hook->original<SetupFn>();

    const auto eye = static_cast<CameraPassHook::Eye>(g_eye.load(std::memory_order_relaxed));
    if (eye == CameraPassHook::Eye::Off || camera == nullptr) {
        return original(camera, fov, rect, depth_min, depth_max);
    }

    // SUBSTITUTED COPIES, never a write through the engine's pointers. The caller owns that storage and may
    // reuse it after this returns; mutating it would be a side effect outside the frame we were asked about.
    const float sign = eye == CameraPassHook::Eye::Left ? -1.0f : 1.0f;
    const auto shifted = sdk::SceneCamera::offset_transform_local(
        *camera, sign * g_half_ipd.load(std::memory_order_relaxed), 0.0f, 0.0f);
    if (!shifted.has_value()) {
        // A pose whose rotation is not usable as one. Passing the original through is the only safe answer --
        // rendering an eye from a garbage transform is worse than rendering it monocular.
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        return original(camera, fov, rect, depth_min, depth_max);
    }

    float fov_local[2] = {fov != nullptr ? fov[0] : 0.0f, fov != nullptr ? fov[1] : 0.0f};
    const float ox = g_fov_x.load(std::memory_order_relaxed);
    const float oy = g_fov_y.load(std::memory_order_relaxed);
    if (ox > 0.0f) {
        fov_local[0] = ox;
    }
    if (oy > 0.0f) {
        fov_local[1] = oy;
    }

    float rect_local[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (rect != nullptr) {
        memcpy(rect_local, rect, sizeof(rect_local));
    }
    if (g_split.load(std::memory_order_relaxed)) {
        // Halve the horizontal extent of whatever the game asked for, rather than assuming full width: the
        // engine already uses sub-rects, and clobbering one would break a pass we did not intend to touch.
        const float left = rect_local[0];
        const float right = rect_local[2];
        const float mid = left + (right - left) * 0.5f;
        if (eye == CameraPassHook::Eye::Left) {
            rect_local[2] = mid;
        } else {
            rect_local[0] = mid;
        }
    }

    g_overridden.fetch_add(1, std::memory_order_relaxed);
    return original(&shifted.value(), fov_local, rect_local, depth_min, depth_max);
}

}  // namespace

CameraPassHook& CameraPassHook::get() {
    static CameraPassHook s_instance;
    return s_instance;
}

std::optional<std::string> CameraPassHook::on_initialize() {
    const uintptr_t target = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::SetupPassPerspective);
    g_target.store(target, std::memory_order_relaxed);
    if (target == 0) {
        LOGX("[camerapass] SetupPassPerspective did not resolve -- no stereo intervention point");
        return std::string{"CLTRenderer slot 15 did not resolve"};
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&setup_pass_detour))) {
        return std::string{"failed to hook CLTRenderer::SetupPassPerspective"};
    }
    LOGX("[camerapass] perspective pass hooked at 0x%08" PRIXPTR " (observing)", target);
    return std::nullopt;
}

void CameraPassHook::on_shutdown() {
    // Stop overriding before the hooks retire, so the last frames the engine renders on its way out are its
    // own. Hook removal is Hooks::retire()'s job; this is about what the player sees.
    g_eye.store(static_cast<uint8_t>(Eye::Off), std::memory_order_relaxed);
}

void CameraPassHook::set_eye(Eye eye, float half_ipd, bool split_viewport) {
    g_half_ipd.store(half_ipd, std::memory_order_relaxed);
    g_split.store(split_viewport, std::memory_order_relaxed);
    g_eye.store(static_cast<uint8_t>(eye), std::memory_order_release);
    LOGX("[camerapass] eye=%u half_ipd=%.3f split=%d", static_cast<unsigned>(eye), half_ipd,
         split_viewport ? 1 : 0);
}

void CameraPassHook::set_fov_override(float fov_x, float fov_y) {
    g_fov_x.store(fov_x, std::memory_order_relaxed);
    g_fov_y.store(fov_y, std::memory_order_relaxed);
}

CameraPassHook::Observed CameraPassHook::observed() const {
    Observed out;
    out.target = g_target.load(std::memory_order_relaxed);
    out.hooked = Hooks::get().find(kHookName) != nullptr;
    out.passes = g_passes.load(std::memory_order_relaxed);
    out.overridden = g_overridden.load(std::memory_order_relaxed);
    out.rejected = g_rejected.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
        out.camera_position[i] = g_pos[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 4; ++i) {
        out.camera_rotation[i] = g_rot[i].load(std::memory_order_relaxed);
        out.rect[i] = g_rect_seen[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 2; ++i) {
        out.fov[i] = g_fov_seen[i].load(std::memory_order_relaxed);
    }
    out.depth_min = g_depth_min.load(std::memory_order_relaxed);
    out.depth_max = g_depth_max.load(std::memory_order_relaxed);
    out.eye = static_cast<Eye>(g_eye.load(std::memory_order_acquire));
    out.half_ipd = g_half_ipd.load(std::memory_order_relaxed);
    out.split_viewport = g_split.load(std::memory_order_relaxed);
    out.fov_override = {g_fov_x.load(std::memory_order_relaxed), g_fov_y.load(std::memory_order_relaxed)};
    return out;
}

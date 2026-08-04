// XR_USE_GRAPHICS_API_D3D11 must be defined before openxr_platform.h, same as main.cpp's own
// top-of-file defines -- it is what gates XrSwapchainImageD3D11KHR and friends into existence.
#define XR_USE_GRAPHICS_API_D3D11

#include "SettingsUi.hpp"

#include "UiRenderInterfaceD3D11.hpp"
#include "UiSystemInterface.hpp"

#include <windows.h>

#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include "HttpClient.hpp" // shared/ is on the include path -- see tools/xr64/CMakeLists.txt

namespace xrui {

namespace {

constexpr uint32_t kPanelPxW = 960;
constexpr uint32_t kPanelPxH = 680;
constexpr float kPanelWidthM = 0.55f;
constexpr float kPanelHeightM = kPanelWidthM * static_cast<float>(kPanelPxH) / static_cast<float>(kPanelPxW);
constexpr float kPanelDistanceM = 1.0f;
constexpr int32_t kModPort = 8798;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string fmtX(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.2fx", v); return b; }
std::string fmtDeg(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.0f deg", v); return b; }
std::string fmtM(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.2f m", v); return b; }
std::string fmtNum2(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.2f", v); return b; }

// Same convention as test/fixture_test_runner.cpp's json_bool/json_double: the mod's JSON is
// always flat "key":value pairs, so a substring search is enough and needs no parser dependency.
bool jsonBool(const std::string& body, const char* key, bool& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t v = p + needle.size();
    if (body.compare(v, 4, "true") == 0) { out = true; return true; }
    if (body.compare(v, 5, "false") == 0) { out = false; return true; }
    return false;
}

bool jsonDouble(const std::string& body, const char* key, double& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t start = p + needle.size();
    char* endp = nullptr;
    const double v = std::strtod(body.c_str() + start, &endp);
    if (endp == body.c_str() + start) return false;
    out = v;
    return true;
}

std::string exeDir() {
    char buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    const std::string path(buf, n);
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

// Standard optimized quaternion-vector rotation: v' = v + 2w(q x v) + 2(q x (q x v)).
void rotateByQuat(const XrQuaternionf& q, float vx, float vy, float vz, float& ox, float& oy, float& oz) {
    const float tx = 2.0f * (q.y * vz - q.z * vy);
    const float ty = 2.0f * (q.z * vx - q.x * vz);
    const float tz = 2.0f * (q.x * vy - q.y * vx);
    ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// What the background poll thread learns from the mod, mirrored into the document by the main
// thread (applySyncIfDue()) -- see the mutex on Impl::snapshot. Every "*_known"/"*_route_ok" flag
// exists because an unreachable mod, or a route this build doesn't have yet, must leave the
// corresponding control showing its last good value rather than snapping to a bogus default.
struct ModSnapshot {
    bool reachable = false;
    bool locomotion_known = false;
    bool locomotion_on = false;
    float snap_deg = 30.0f;
    bool comfort_known = false;
    bool comfort_on = false;
    bool ipd_known = false;
    float half_ipd = 3.2f;
    bool supersample_route_ok = false;
    float supersample_scale = 1.0f;
};

} // namespace

struct SettingsUi::Impl {
    // ---- borrowed handles, set once by init() ------------------------------------------------
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    int64_t swapchain_format = 0;
    XrAction trigger_action = XR_NULL_HANDLE;
    XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrPath hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
    float* hud_distance_m = nullptr;

    // ---- RmlUi ---------------------------------------------------------------------------------
    UiSystemInterface system_interface;
    UiRenderInterfaceD3D11 render_interface;
    Rml::Context* rml_context = nullptr;
    Rml::ElementDocument* document = nullptr;

    // ---- this panel's own OpenXR swapchain ------------------------------------------------------
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

    // ---- open/close + pointer state -------------------------------------------------------------
    // Written by main.cpp's Menu latch on the frame thread, read by the poll thread.
    std::atomic<bool> visible{false};
    // The aim point, in panel pixels, shared with the render side for the cursor dot.
    std::atomic<int> cursor_x{0};
    std::atomic<int> cursor_y{0};
    std::atomic<bool> cursor_on{false};
    bool last_trigger_down = false;

    // ---- the mod link ----------------------------------------------------------------------------
    // The poll thread ONLY touches `snapshot`/`snapshot_fresh` (guarded by snapshot_mutex) and does
    // socket I/O -- it never touches an Rml object, none of which are safe to call off the render
    // thread. `suppress_sync_until` (an UiSystemInterface elapsed-time timestamp) is how a value the
    // wearer is actively dragging does not get overwritten mid-drag by a poll that started before
    // the drag did.
    std::thread poll_thread;
    std::atomic<bool> poll_stop{false};
    std::mutex snapshot_mutex;
    ModSnapshot snapshot;
    bool snapshot_fresh = false;
    double suppress_sync_until = -1.0;

    bool createSwapchain();
    void destroySwapchain();
    void wireControls();
    void pollInputAndInject(XrTime time);
    void applySyncIfDue();
    void applySnapshotToDocument(const ModSnapshot& s);
    const XrCompositionLayerQuad* renderAndBuildQuad();
    void pollThreadMain();
    void fireModRequest(const std::string& path);
    void onControlChanged(Rml::Event& event);
    void onPointerMove(int x, int y);
    void updateCursorElement();
    void onPointerButton(bool down);

    void setLabel(const char* id, const std::string& text);
    void setSliderValue(const char* id, float value);
    void setChecked(const char* id, bool on);
};

namespace {
// One listener services every control -- see Impl::wireControls() -- dispatching by element id.
// RmlUi does not take ownership of a listener added via Element::AddEventListener, so this lives
// as long as the Impl that owns the elements it is attached to, and is detached in shutdown()
// implicitly by the document (and therefore the elements) being destroyed first.
class ControlListener final : public Rml::EventListener {
public:
    SettingsUi::Impl* owner = nullptr;
    void ProcessEvent(Rml::Event& event) override {
        if (owner != nullptr) {
            owner->onControlChanged(event);
        }
    }
};
ControlListener g_listener;
} // namespace

bool SettingsUi::Impl::createSwapchain() {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sc.format = swapchain_format;
    sc.sampleCount = 1;
    sc.width = kPanelPxW;
    sc.height = kPanelPxH;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;

    const XrResult r = xrCreateSwapchain(session, &sc, &swapchain);
    std::printf("[host] [ui] settings swapchain %ux%u -> XrResult %d\n", kPanelPxW, kPanelPxH, static_cast<int>(r));
    if (XR_FAILED(r)) {
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));

    // The runtime commonly hands back TYPELESS images for an sRGB-requested format specifically so
    // an app can choose either an sRGB or a UNORM view; the UNORM one is what matches this host's
    // existing convention of treating the UI surface as "just bytes" (see the raw memcpy upload
    // for the mod's own HUD swapchain in main.cpp -- no colour-space conversion happens there
    // either). If the runtime instead handed back a concretely-typed image, the sRGB view create
    // fails and the fallback below asks for the native format instead.
    const bool want_unorm_view = (swapchain_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    D3D11_RENDER_TARGET_VIEW_DESC unorm_desc{};
    unorm_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    unorm_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    for (uint32_t i = 0; i < n; ++i) {
        images.push_back(imgs[i].texture);
        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = want_unorm_view ? device->CreateRenderTargetView(imgs[i].texture, &unorm_desc, &rtv)
                                     : device->CreateRenderTargetView(imgs[i].texture, nullptr, &rtv);
        if (FAILED(hr)) {
            hr = device->CreateRenderTargetView(imgs[i].texture, nullptr, &rtv);
        }
        if (FAILED(hr)) {
            std::printf("[host] [ui] RTV create failed for settings image %u: 0x%08lx\n", i,
                        static_cast<unsigned long>(hr));
        }
        rtvs.push_back(rtv);
    }
    return true;
}

void SettingsUi::Impl::destroySwapchain() {
    for (ID3D11RenderTargetView* rtv : rtvs) {
        if (rtv != nullptr) {
            rtv->Release();
        }
    }
    rtvs.clear();
    images.clear();
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
        swapchain = XR_NULL_HANDLE;
    }
}

void SettingsUi::Impl::setLabel(const char* id, const std::string& text) {
    if (Rml::Element* el = document->GetElementById(id)) {
        el->SetInnerRML(text);
    }
}

void SettingsUi::Impl::setSliderValue(const char* id, float value) {
    if (Rml::Element* el = document->GetElementById(id)) {
        el->SetAttribute("value", value);
    }
}

void SettingsUi::Impl::setChecked(const char* id, bool on) {
    if (Rml::Element* el = document->GetElementById(id)) {
        if (on) {
            el->SetAttribute("checked", Rml::String("checked"));
        } else {
            el->RemoveAttribute("checked");
        }
    }
}

void SettingsUi::Impl::wireControls() {
    g_listener.owner = this;
    static const char* const kIds[] = {"ss-slider", "snap-toggle", "snap-slider",
                                       "comfort-toggle", "hud-slider", "ipd-slider"};
    for (const char* id : kIds) {
        if (Rml::Element* el = document->GetElementById(id)) {
            el->AddEventListener("change", &g_listener);
        } else {
            std::printf("[host] [ui] settings.rml is missing expected control '#%s'\n", id);
        }
    }
}

void SettingsUi::Impl::fireModRequest(const std::string& path) {
    // Detached and short-lived: a control change is rare (a wearer adjusting a slider), not a
    // per-frame event, so a thread per change is simpler than a persistent request queue and never
    // accumulates. http::get() does its own connect/send/recv and its own WSAStartup/WSACleanup, so
    // nothing here needs to coordinate with the poll thread doing the same on its own cadence.
    std::thread([path]() {
        std::string resp;
        const bool ok = http::get(kModPort, path.c_str(), resp);
        std::printf("[host] [ui] -> %s : %s\n", path.c_str(),
                    ok ? "sent" : "mod unreachable (127.0.0.1:8798)");
    }).detach();
}

void SettingsUi::Impl::onControlChanged(Rml::Event& event) {
    Rml::Element* el = event.GetTargetElement();
    if (el == nullptr || document == nullptr) {
        return;
    }
    const Rml::String id = el->GetId();

    // The suppression window matters here, not just in applySyncIfDue(): a control the wearer just
    // touched should hold its new value until the mod's own next poll confirms it, rather than
    // being overwritten by a snapshot the poll thread grabbed a moment before this edit landed.
    suppress_sync_until = system_interface.GetElapsedTime() + 1.5;

    if (id == "ss-slider") {
        // Route contract from Main: GET /render/supersample?scale=<0.5-2.0>. Not live in this
        // build yet -- fireModRequest()/the poll thread both tolerate that (404/unreachable), and
        // ss-status reflects it so the panel is honest about it rather than pretending it worked.
        const float v = clampf(event.GetParameter("value", 1.0f), 0.5f, 2.0f);
        setLabel("ss-value", fmtX(v));
        char path[96];
        std::snprintf(path, sizeof(path), "/render/supersample?scale=%.3f", v);
        fireModRequest(path);
    } else if (id == "snap-toggle") {
        const bool on = event.GetParameter("checked", false);
        fireModRequest(on ? "/xr/capture?locomotion=1" : "/xr/capture?locomotion=0");
    } else if (id == "snap-slider") {
        const float deg = clampf(event.GetParameter("value", 30.0f), 5.0f, 90.0f);
        setLabel("snap-value", fmtDeg(deg));
        char path[96];
        std::snprintf(path, sizeof(path), "/xr/capture?snap_deg=%.1f", deg);
        fireModRequest(path);
    } else if (id == "comfort-toggle") {
        const bool on = event.GetParameter("checked", false);
        // NOT a vignette -- see the report. /vr/comfort suppresses the engine's camera bob/sway,
        // which is the closest real switch the mod exposes; there is no vignette effect to toggle.
        fireModRequest(on ? "/vr/comfort?on=1" : "/vr/comfort?on=0");
    } else if (id == "hud-slider") {
        // Host-local, no HTTP: this is main.cpp's own `ui_distance_m` (its `--ui-distance`), which
        // positions the MOD's HUD quad and lives in this same process.
        const float m = clampf(event.GetParameter("value", 1.8f), 0.5f, 4.0f);
        setLabel("hud-value", fmtM(m));
        if (hud_distance_m != nullptr) {
            *hud_distance_m = m;
        }
    } else if (id == "ipd-slider") {
        // `both=1&split=1` are carried on every call, not just the first: /stereo/eye mutates only
        // the keys present in its query string (see Framework.cpp), but an absent `both` still
        // routes the call down the single-eye branch, which would silently turn stereo off.
        const float v = clampf(event.GetParameter("value", 3.2f), 0.0f, 6.0f);
        setLabel("ipd-value", fmtNum2(v));
        char path[128];
        std::snprintf(path, sizeof(path), "/stereo/eye?both=1&split=1&half_ipd=%.3f", v);
        fireModRequest(path);
    }
}

void SettingsUi::Impl::applySnapshotToDocument(const ModSnapshot& s) {
    setLabel("mod-status", s.reachable ? "mod: connected (127.0.0.1:8798)" : "mod: unreachable (127.0.0.1:8798)");

    if (s.locomotion_known) {
        setChecked("snap-toggle", s.locomotion_on);
    }
    setSliderValue("snap-slider", s.snap_deg);
    setLabel("snap-value", fmtDeg(s.snap_deg));

    if (s.comfort_known) {
        setChecked("comfort-toggle", s.comfort_on);
    }

    if (s.ipd_known) {
        setSliderValue("ipd-slider", s.half_ipd);
        setLabel("ipd-value", fmtNum2(s.half_ipd));
    }

    if (s.supersample_route_ok) {
        setSliderValue("ss-slider", s.supersample_scale);
        setLabel("ss-value", fmtX(s.supersample_scale));
        setLabel("ss-status", "");
    } else {
        setLabel("ss-status", "(mod route not available in this build yet)");
    }
}

void SettingsUi::Impl::applySyncIfDue() {
    ModSnapshot snap;
    bool have_fresh = false;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        if (snapshot_fresh) {
            snap = snapshot;
            have_fresh = true;
        }
    }
    if (!have_fresh) {
        return;
    }
    if (system_interface.GetElapsedTime() < suppress_sync_until) {
        return; // a local edit is still fresh; leave it alone rather than fight the wearer's drag
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        snapshot_fresh = false;
    }
    applySnapshotToDocument(snap);
}

void SettingsUi::Impl::pollThreadMain() {
    while (!poll_stop.load(std::memory_order_relaxed)) {
        ModSnapshot snap;

        std::string resp;
        if (http::get(kModPort, "/xr/head", resp)) {
            snap.reachable = true;
            const std::string body = http::body_of(resp);
            double d = 0.0;
            bool b = false;
            if (jsonDouble(body, "vr_snap_deg", d)) {
                snap.snap_deg = static_cast<float>(d);
            }
            if (jsonBool(body, "vr_locomotion", b)) {
                snap.locomotion_known = true;
                snap.locomotion_on = b;
            }
        }

        std::string resp_stereo;
        if (http::get(kModPort, "/stereo/eye", resp_stereo)) {
            double d = 0.0;
            if (jsonDouble(http::body_of(resp_stereo), "half_ipd", d)) {
                snap.ipd_known = true;
                snap.half_ipd = static_cast<float>(d);
            }
        }

        std::string resp_comfort;
        if (http::get(kModPort, "/vr/comfort", resp_comfort)) {
            bool b = false;
            if (jsonBool(http::body_of(resp_comfort), "suppressed", b)) {
                snap.comfort_known = true;
                snap.comfort_on = b;
            }
        }

        // /render/supersample does not exist in this build yet (Main is landing it separately --
        // see SettingsUi.hpp and the report). A 404 answers here with no "scale" key, so this just
        // leaves supersample_route_ok false; nothing here needs to special-case the status code.
        std::string resp_ss;
        if (http::get(kModPort, "/render/supersample", resp_ss)) {
            double d = 0.0;
            if (jsonDouble(http::body_of(resp_ss), "scale", d)) {
                snap.supersample_route_ok = true;
                snap.supersample_scale = static_cast<float>(d);
            }
        }

        {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            snapshot = snap;
            snapshot_fresh = true;
        }

        // Slept in slices so shutdown() (which sets poll_stop and joins) is not left waiting a
        // full second for a thread that already has nothing left to do.
        for (int i = 0; i < 20 && !poll_stop.load(std::memory_order_relaxed); ++i) {
            Sleep(50);
        }
    }
}

void SettingsUi::Impl::pollInputAndInject(XrTime time) {
    // Visibility is driven by main.cpp via toggle(). It cannot be decided here: Menu is also the
    // game's pause button, so the bit has to be withheld from game input while we wait to see
    // whether this is a tap or a hold, and only main.cpp writes that input.
    if (!visible.load(std::memory_order_acquire)) {
        return;
    }

    const float half_w = kPanelWidthM * 0.5f;
    const float half_h = kPanelHeightM * 0.5f;
    bool hit_any = false;

    // Try the right hand first (index 1, matching shared/xr/SharedFrame.hpp's kHandRight) -- most
    // wearers point with the dominant/right controller; the left is a fallback, not a second cursor.
    for (int pass = 0; pass < 2 && !hit_any; ++pass) {
        const int h = (pass == 0) ? 1 : 0;
        if (aim_space[h] == XR_NULL_HANDLE) {
            continue;
        }

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(aim_space[h], view_space, time, &loc))) {
            continue;
        }
        constexpr XrSpaceLocationFlags kNeed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((loc.locationFlags & kNeed) != kNeed) {
            continue;
        }

        float dx = 0, dy = 0, dz = 0;
        rotateByQuat(loc.pose.orientation, 0.0f, 0.0f, -1.0f, dx, dy, dz);
        if (dz > -0.05f) {
            continue; // pointing away from, or edge-on to, the panel's plane
        }

        const float t = (-kPanelDistanceM - loc.pose.position.z) / dz;
        if (t <= 0.0f) {
            continue;
        }
        const float hit_x = loc.pose.position.x + t * dx;
        const float hit_y = loc.pose.position.y + t * dy;
        if (hit_x < -half_w || hit_x > half_w || hit_y < -half_h || hit_y > half_h) {
            continue;
        }

        const float u = (hit_x + half_w) / (half_w * 2.0f);
        const float v = (half_h - hit_y) / (half_h * 2.0f); // v=0 at the top: +Y is up in view space
        onPointerMove(static_cast<int>(clampf(u, 0.0f, 1.0f) * static_cast<float>(kPanelPxW - 1)),
                     static_cast<int>(clampf(v, 0.0f, 1.0f) * static_cast<float>(kPanelPxH - 1)));
        hit_any = true;
        cursor_x.store(static_cast<int>(u * static_cast<float>(kPanelPxW)),
                       std::memory_order_relaxed);
        cursor_y.store(static_cast<int>(v * static_cast<float>(kPanelPxH)),
                       std::memory_order_relaxed);
        cursor_on.store(true, std::memory_order_relaxed);

        XrActionStateGetInfo trig_gi{XR_TYPE_ACTION_STATE_GET_INFO};
        trig_gi.action = trigger_action;
        trig_gi.subactionPath = hand_path[h];
        XrActionStateFloat trig{XR_TYPE_ACTION_STATE_FLOAT};
        const bool trig_ok = XR_SUCCEEDED(xrGetActionStateFloat(session, &trig_gi, &trig));
        const bool down = trig_ok && trig.isActive != XR_FALSE && trig.currentState > 0.5f;
        if (down != last_trigger_down) {
            onPointerButton(down);
            last_trigger_down = down;
        }
    }

    if (!hit_any) {
        cursor_on.store(false, std::memory_order_relaxed);
        rml_context->ProcessMouseLeave();
        if (last_trigger_down) {
            onPointerButton(false);
            last_trigger_down = false;
        }
    }
}

const XrCompositionLayerQuad* SettingsUi::Impl::renderAndBuildQuad() {
    if (swapchain == XR_NULL_HANDLE) {
        return nullptr;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain, &ai, &index)) || index >= rtvs.size()) {
        return nullptr;
    }

    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain, &ri);
        return nullptr;
    }

    if (rtvs[index] != nullptr) {
        render_interface.beginFrame(rtvs[index], kPanelPxW, kPanelPxH);
        rml_context->Render();
        render_interface.endFrame();
    }

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain, &ri);

    // PREMULTIPLIED, matching UiRenderInterfaceD3D11's blend state and the same convention
    // main.cpp already uses for the mod's HUD quad (see the comment there).
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = view_space; // head-locked: appears in front of wherever the wearer opened it
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {static_cast<int32_t>(kPanelPxW), static_cast<int32_t>(kPanelPxH)};
    quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    quad.pose.position = {0.0f, 0.0f, -kPanelDistanceM};
    quad.size.width = kPanelWidthM;
    quad.size.height = kPanelHeightM;
    return &quad;
}

// ---- SettingsUi: thin forwarders onto Impl -----------------------------------------------------

SettingsUi::SettingsUi() : m_impl(new Impl()) {}

SettingsUi::~SettingsUi() {
    delete m_impl;
}

bool SettingsUi::init(ID3D11Device* device, ID3D11DeviceContext* context, XrInstance instance, XrSession session,
                      XrSpace view_space, int64_t swapchain_format, XrAction trigger_action, const XrSpace aim_space[2], const XrPath hand_path[2],
                      float* hud_distance_m) {
    m_impl->device = device;
    m_impl->d3d_context = context;
    m_impl->instance = instance;
    m_impl->session = session;
    m_impl->view_space = view_space;
    m_impl->swapchain_format = swapchain_format;
    m_impl->trigger_action = trigger_action;
    m_impl->aim_space[0] = aim_space[0];
    m_impl->aim_space[1] = aim_space[1];
    m_impl->hand_path[0] = hand_path[0];
    m_impl->hand_path[1] = hand_path[1];
    m_impl->hud_distance_m = hud_distance_m;

    if (!m_impl->render_interface.init(device, context)) {
        std::printf("[host] [ui] D3D11 render interface init failed -- settings UI disabled\n");
        return false;
    }

    Rml::SetSystemInterface(&m_impl->system_interface);
    Rml::SetRenderInterface(&m_impl->render_interface);
    if (!Rml::Initialise()) {
        std::printf("[host] [ui] Rml::Initialise failed -- settings UI disabled\n");
        return false;
    }

    // System fonts, not vendored assets: this host is Windows-only already (D3D11, OpenXR's D3D11
    // extension), so Segoe UI is as reliable a dependency as the OS itself, and it avoids shipping
    // a font's license alongside a debug tool.
    const std::string regular_font = "C:\\Windows\\Fonts\\segoeui.ttf";
    const std::string bold_font = "C:\\Windows\\Fonts\\seguisb.ttf";
    if (!Rml::LoadFontFace(regular_font)) {
        std::printf("[host] [ui] font load failed: %s (text will not render)\n", regular_font.c_str());
    }
    if (!Rml::LoadFontFace(bold_font, false, Rml::Style::FontWeight::Bold)) {
        std::printf("[host] [ui] font load failed: %s\n", bold_font.c_str());
    }

    m_impl->rml_context = Rml::CreateContext("settings", Rml::Vector2i(static_cast<int>(kPanelPxW), static_cast<int>(kPanelPxH)));
    if (m_impl->rml_context == nullptr) {
        std::printf("[host] [ui] Rml::CreateContext failed -- settings UI disabled\n");
        return false;
    }

    const std::string doc_path = exeDir() + "\\ui\\assets\\settings.rml";
    m_impl->document = m_impl->rml_context->LoadDocument(doc_path);
    if (m_impl->document == nullptr) {
        std::printf("[host] [ui] failed to load %s -- settings UI disabled\n", doc_path.c_str());
        return false;
    }
    m_impl->document->Show();
    m_impl->wireControls();

    if (!m_impl->createSwapchain()) {
        std::printf("[host] [ui] settings UI swapchain create failed -- panel will not render\n");
        return false;
    }

    m_impl->poll_stop.store(false, std::memory_order_relaxed);
    m_impl->poll_thread = std::thread([impl = m_impl]() { impl->pollThreadMain(); });

    std::printf("[host] [ui] settings panel initialised: %ux%u px, menu button toggles, RmlUi %s\n",
                kPanelPxW, kPanelPxH, Rml::GetVersion().c_str());
    return true;
}

const XrCompositionLayerQuad* SettingsUi::update(XrTime predicted_display_time) {
    if (m_impl->rml_context == nullptr) {
        return nullptr;
    }
    m_impl->pollInputAndInject(predicted_display_time);
    m_impl->applySyncIfDue();
    m_impl->updateCursorElement();
    m_impl->rml_context->Update();
    if (!m_impl->visible) {
        return nullptr;
    }
    return m_impl->renderAndBuildQuad();
}

void SettingsUi::shutdown() {
    m_impl->poll_stop.store(true, std::memory_order_relaxed);
    if (m_impl->poll_thread.joinable()) {
        m_impl->poll_thread.join();
    }

    m_impl->destroySwapchain();

    if (m_impl->rml_context != nullptr) {
        Rml::RemoveContext(m_impl->rml_context->GetName());
        m_impl->rml_context = nullptr;
        m_impl->document = nullptr;
    }
    Rml::Shutdown();
    m_impl->render_interface.shutdown();
    std::printf("[host] [ui] settings UI shut down\n");
}

bool SettingsUi::visible() const {
    return m_impl->visible.load(std::memory_order_acquire);
}

void SettingsUi::toggle() {
    const bool now = !m_impl->visible.load(std::memory_order_acquire);
    m_impl->visible.store(now, std::memory_order_release);
    if (!now) {
        m_impl->cursor_on.store(false, std::memory_order_relaxed);
    }
    std::printf("[host] [ui] settings panel %s\n", now ? "opened" : "closed");
}

void SettingsUi::onPointerMove(int x, int y) {
    m_impl->onPointerMove(x, y);
}

void SettingsUi::onPointerButton(bool down) {
    m_impl->onPointerButton(down);
}

void SettingsUi::Impl::onPointerMove(int x, int y) {
    if (rml_context != nullptr) {
        rml_context->ProcessMouseMove(x, y, 0);
    }
}

// The dot lives in the document so it costs no new rendering, and it is placed from the same hit
// point that feeds ProcessMouseMove -- so what you see and what RmlUi thinks you are pointing at
// cannot drift apart.
void SettingsUi::Impl::updateCursorElement() {
    if (document == nullptr) {
        return;
    }
    auto* dot = document->GetElementById("aim-cursor");
    if (dot == nullptr) {
        return;
    }
    if (!cursor_on.load(std::memory_order_relaxed)) {
        dot->SetProperty("visibility", "hidden");
        return;
    }
    dot->SetProperty("visibility", "visible");
    dot->SetProperty("left", std::to_string(cursor_x.load(std::memory_order_relaxed)) + "px");
    dot->SetProperty("top", std::to_string(cursor_y.load(std::memory_order_relaxed)) + "px");
}

void SettingsUi::Impl::onPointerButton(bool down) {
    if (rml_context == nullptr) {
        return;
    }
    if (down) {
        rml_context->ProcessMouseButtonDown(0, 0);
    } else {
        rml_context->ProcessMouseButtonUp(0, 0);
    }
}

} // namespace xrui

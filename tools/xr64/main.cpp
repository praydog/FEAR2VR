// ---- THE 64-BIT XR HOST ------------------------------------------------------------------------
//
// FEAR2 is 32-bit and the 32-bit Oculus runtime dies inside its own RuntimeIPC init during
// xrCreateSession -- measured, and proven against this program, which performs the identical
// sequence in 64 bits on the same machine and same headset and succeeds. So submission lives here.
//
// This is deliberately NOT built by build.bat: the mod is 32-bit because the game is.
//
//   cmake -B build64 -A x64 -S tools/xr64 && cmake --build build64 --config Release
//   build64/Release/xr64.exe --probe     one-shot capability report, exits
//   build64/Release/xr64.exe             run the frame loop and submit
//
// FIRST MILESTONE, WHICH IS WHAT THIS CURRENTLY DOES: put ANY image in front of the wearer. Each eye
// is cleared to a different colour, pulsing, with no shaders, no vertex buffers and no texture
// upload -- so that when nothing appears, the thing at fault is the session, the swapchain or the
// submission, and not a triangle. The game's pixels come next and change nothing about this loop.

#define XR_USE_GRAPHICS_API_D3D11
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xr/SharedFrame.hpp"

namespace {

volatile bool g_stop = false;

BOOL WINAPI console_handler(DWORD) {
    g_stop = true;
    return TRUE;
}

XrInstance g_instance = XR_NULL_HANDLE;

const char* rs(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];

    if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) {
        return buf;
    }

    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(r));
    return buf;
}

const char* state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

// ---- THE GAME'S FRAME ---------------------------------------------------------------------------
//
// A seqlock reader: take the sequence, read, take it again, and only believe the frame if it was
// EVEN and unchanged. No locking, so a stalled or dead host can never hold up the game's render
// thread -- which is the property that matters, since the writer is the render thread.
struct SharedReader {
    HANDLE mapping = nullptr;
    const xr::SharedFrameHeader* header = nullptr;
    xr::HostState* host = nullptr;
    xr::HandsState* hands = nullptr;
    const uint8_t* base_bytes = nullptr;
    uint32_t last_sequence = 0;

    bool open() {
        if (header != nullptr) {
            return true;
        }

        mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, xr::kSharedFrameName);

        if (mapping == nullptr) {
            return false;
        }

        void* base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);

        if (base == nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }

        header = static_cast<const xr::SharedFrameHeader*>(base);
        host = reinterpret_cast<xr::HostState*>(static_cast<uint8_t*>(base) +
                                                xr::kHostStateOffset);
        hands = reinterpret_cast<xr::HandsState*>(static_cast<uint8_t*>(base) +
                                                  xr::kHandsStateOffset);
        base_bytes = static_cast<const uint8_t*>(base);

        // Fresh committed memory reads as zero already, but the mapping outlives a crashed host --
        // a prior run could have left `sequence` odd (torn) or the hand data mid-write. Zeroing
        // once here, before the game or the frame loop can observe it, guarantees a reader always
        // finds an even sequence and all-invalid poses rather than whatever a dead process left.
        std::memset(hands, 0, sizeof(xr::HandsState));
        return true;
    }

    // True when a COMPLETE frame newer than the last one is available.
    uint32_t layout() const { return header == nullptr ? 0u : header->layout; }
    uint32_t frame_host_sequence() const { return header == nullptr ? 0u : header->host_sequence; }

    bool poll(uint32_t& w, uint32_t& h, uint32_t& pitch, const uint8_t*& bits) {
        if (header == nullptr || header->magic != xr::kSharedFrameMagic) {
            return false;
        }

        const uint32_t seq = header->sequence;

        if ((seq & 1u) != 0u || seq == last_sequence) {
            return false;  // mid-write, or nothing new
        }

        w = header->width;
        h = header->height;
        pitch = header->pitch;

        // FROM THE PUBLISHED SLOT, not a fixed offset. The writer is already filling a different
        // one, which is what makes it safe to upload straight out of shared memory instead of
        // copying it somewhere private first.
        bits = base_bytes + xr::slot_offset(header->slot);

        if (w == 0 || h == 0 || pitch == 0) {
            return false;
        }

        // Re-read: if the writer moved on while we looked, the fields may not describe the pixels.
        if (header->sequence != seq) {
            return false;
        }

        last_sequence = seq;
        return true;
    }
};

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<ID3D11RenderTargetView*> views;
};

ID3D11Device* create_device_on(LUID luid, D3D_FEATURE_LEVEL min_level, ID3D11DeviceContext** ctx) {
    IDXGIFactory1* factory = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }

    IDXGIAdapter1* chosen = nullptr;

    // THE RUNTIME PICKS THE ADAPTER. A device on any other one is rejected, or worse, presents to
    // nothing -- and on a machine with an iGPU that is not hypothetical.
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;

        if (factory->EnumAdapters1(i, &a) != S_OK) {
            break;
        }

        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);

        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            chosen = a;
            break;
        }

        a->Release();
    }

    factory->Release();

    if (chosen == nullptr) {
        return nullptr;
    }

    ID3D11Device* device = nullptr;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want = min_level;
    const HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, &want, 1,
                                         D3D11_SDK_VERSION, &device, &got, ctx);
    chosen->Release();
    return SUCCEEDED(hr) ? device : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(console_handler, TRUE);

    bool probe_only = false;
    int max_seconds = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            probe_only = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            max_seconds = std::atoi(argv[++i]);
        }
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "FEAR2VR");
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "LithTech Jupiter EX");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;

    XrResult r = xrCreateInstance(&ici, &g_instance);
    std::printf("[host] xrCreateInstance -> %s\n", rs(r));

    if (XR_FAILED(r)) {
        return 1;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &props);
    std::printf("[host] runtime '%s' %llu.%llu.%llu\n", props.runtimeName,
                static_cast<unsigned long long>(XR_VERSION_MAJOR(props.runtimeVersion)),
                static_cast<unsigned long long>(XR_VERSION_MINOR(props.runtimeVersion)),
                static_cast<unsigned long long>(XR_VERSION_PATCH(props.runtimeVersion)));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(g_instance, &sgi, &system);
    std::printf("[host] xrGetSystem -> %s\n", rs(r));

    if (XR_FAILED(r)) {
        std::printf("[host] no headset. Connect one and try again.\n");
        return 1;
    }

    // What the runtime wants each eye rendered at. Reported rather than chosen: this is the number a
    // supersampling multiplier would later scale.
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(g_instance, system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count,
                                      nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count,
                                                      {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_instance, system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                                      &view_count, config_views.data());

    for (uint32_t i = 0; i < view_count; ++i) {
        std::printf("[host] view %u recommended %ux%u (max %ux%u), %u sample(s)\n", i,
                    config_views[i].recommendedImageRectWidth,
                    config_views[i].recommendedImageRectHeight,
                    config_views[i].maxImageRectWidth, config_views[i].maxImageRectHeight,
                    config_views[i].recommendedSwapchainSampleCount);
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = get_reqs(g_instance, system, &reqs);
    std::printf("[host] graphics requirements -> %s\n", rs(r));

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Device* device = create_device_on(reqs.adapterLuid, reqs.minFeatureLevel, &ctx);
    std::printf("[host] d3d11 device %p\n", static_cast<void*>(device));

    if (device == nullptr) {
        return 1;
    }

    if (probe_only) {
        std::printf("[host] probe only -- not creating a session\n");
        xrDestroyInstance(g_instance);
        return 0;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;

    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession(g_instance, &sci, &session);
    std::printf("[host] xrCreateSession -> %s\n", rs(r));

    if (XR_FAILED(r)) {
        return 1;
    }

    // Format negotiation: take the first of our preferences the runtime offers, rather than assuming
    // one. A mismatch here is rejected at swapchain creation with an error that names nothing useful.
    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());

    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    int64_t chosen_format = formats.empty() ? 0 : formats[0];

    for (int64_t want : preferred) {
        bool found = false;

        for (int64_t have : formats) {
            found = found || have == want;
        }

        if (found) {
            chosen_format = want;
            break;
        }
    }

    std::printf("[host] %u swapchain format(s), using %lld\n", format_count,
                static_cast<long long>(chosen_format));

    std::vector<Eye> eyes(view_count);

    for (uint32_t i = 0; i < view_count; ++i) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = chosen_format;
        sc.sampleCount = 1;
        sc.width = config_views[i].recommendedImageRectWidth;
        sc.height = config_views[i].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;

        r = xrCreateSwapchain(session, &sc, &eyes[i].swapchain);
        eyes[i].width = sc.width;
        eyes[i].height = sc.height;
        std::printf("[host] eye %u swapchain %ux%u -> %s\n", i, sc.width, sc.height, rs(r));

        if (XR_FAILED(r)) {
            return 1;
        }

        uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eyes[i].swapchain, 0, &image_count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(image_count,
                                                     {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(eyes[i].swapchain, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

        for (uint32_t k = 0; k < image_count; ++k) {
            ID3D11RenderTargetView* rtv = nullptr;
            D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
            rtvd.Format = static_cast<DXGI_FORMAT>(chosen_format);
            rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(images[k].texture, &rtvd, &rtv);
            eyes[i].views.push_back(rtv);
        }

        std::printf("[host]   %u image(s)\n", image_count);
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    r = xrCreateReferenceSpace(session, &rsci, &space);
    std::printf("[host] reference space -> %s\n", rs(r));

    // ---- CONTROLLERS ------------------------------------------------------------------------
    //
    // One action set, created and attached once. Each action exists ONCE with both subaction
    // paths bound to it rather than once per hand -- that is how OpenXR itself models "the same
    // input, on either hand", and it halves the bookkeeping below since xrGetActionState* takes
    // the hand as a parameter rather than needing a second action.
    XrPath hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};  // xr::kHandLeft, xr::kHandRight
    xrStringToPath(g_instance, "/user/hand/left", &hand_path[xr::kHandLeft]);
    xrStringToPath(g_instance, "/user/hand/right", &hand_path[xr::kHandRight]);

    XrActionSet action_set = XR_NULL_HANDLE;
    {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::snprintf(asci.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
        std::snprintf(asci.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
                      "Gameplay");
        r = xrCreateActionSet(g_instance, &asci, &action_set);
        std::printf("[host] xrCreateActionSet -> %s\n", rs(r));
    }

    XrAction aim_pose_action = XR_NULL_HANDLE;
    XrAction grip_pose_action = XR_NULL_HANDLE;
    XrAction trigger_action = XR_NULL_HANDLE;
    XrAction squeeze_action = XR_NULL_HANDLE;
    XrAction stick_action = XR_NULL_HANDLE;
    XrAction a_click_action = XR_NULL_HANDLE;
    XrAction b_click_action = XR_NULL_HANDLE;
    XrAction x_click_action = XR_NULL_HANDLE;
    XrAction y_click_action = XR_NULL_HANDLE;
    XrAction thumbstick_click_action = XR_NULL_HANDLE;
    XrAction menu_click_action = XR_NULL_HANDLE;

    auto create_action = [&](const char* name, const char* localized, XrActionType type,
                             XrAction& out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(aci.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        std::snprintf(aci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localized);
        aci.actionType = type;
        aci.countSubactionPaths = 2;
        aci.subactionPaths = hand_path;
        const XrResult ar = xrCreateAction(action_set, &aci, &out);
        std::printf("[host] action '%s' -> %s\n", name, rs(ar));
    };

    create_action("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, aim_pose_action);
    create_action("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, grip_pose_action);
    create_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, trigger_action);
    create_action("squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT, squeeze_action);
    create_action("stick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT, stick_action);
    create_action("a_click", "A Click", XR_ACTION_TYPE_BOOLEAN_INPUT, a_click_action);
    create_action("b_click", "B Click", XR_ACTION_TYPE_BOOLEAN_INPUT, b_click_action);
    create_action("x_click", "X Click", XR_ACTION_TYPE_BOOLEAN_INPUT, x_click_action);
    create_action("y_click", "Y Click", XR_ACTION_TYPE_BOOLEAN_INPUT, y_click_action);
    create_action("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT,
                  thumbstick_click_action);
    create_action("menu_click", "Menu Click", XR_ACTION_TYPE_BOOLEAN_INPUT, menu_click_action);

    // Suggesting a path a profile does not expose fails the WHOLE call with
    // XR_ERROR_PATH_UNSUPPORTED -- the classic way to end up with no bindings at all and no clue
    // why. So every profile gets its own call, logged with its own result, and only the paths that
    // profile actually has.
    auto suggest_bindings =
        [&](const char* profile_path_str,
           const std::vector<std::pair<XrAction, const char*>>& bindings) {
            XrPath profile_path = XR_NULL_PATH;
            xrStringToPath(g_instance, profile_path_str, &profile_path);

            std::vector<XrActionSuggestedBinding> suggestions;
            suggestions.reserve(bindings.size());

            for (const auto& b : bindings) {
                XrPath p = XR_NULL_PATH;
                xrStringToPath(g_instance, b.second, &p);
                suggestions.push_back({b.first, p});
            }

            XrInteractionProfileSuggestedBinding ipsb{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            ipsb.interactionProfile = profile_path;
            ipsb.countSuggestedBindings = static_cast<uint32_t>(suggestions.size());
            ipsb.suggestedBindings = suggestions.data();

            const XrResult sr = xrSuggestInteractionProfileBindings(g_instance, &ipsb);
            std::printf("[host] suggest bindings '%s' (%zu path(s)) -> %s\n", profile_path_str,
                        suggestions.size(), rs(sr));
            return sr;
        };

    // THE ONE THAT MUST WORK: Quest Pro through the Oculus runtime. A/B exist only on the right
    // Touch controller and X/Y/menu only on the left -- bound accordingly, per hand, rather than
    // offered to both and left to fail.
    suggest_bindings(
        "/interaction_profiles/oculus/touch_controller",
        {
            {aim_pose_action, "/user/hand/left/input/aim/pose"},
            {aim_pose_action, "/user/hand/right/input/aim/pose"},
            {grip_pose_action, "/user/hand/left/input/grip/pose"},
            {grip_pose_action, "/user/hand/right/input/grip/pose"},
            {trigger_action, "/user/hand/left/input/trigger/value"},
            {trigger_action, "/user/hand/right/input/trigger/value"},
            {squeeze_action, "/user/hand/left/input/squeeze/value"},
            {squeeze_action, "/user/hand/right/input/squeeze/value"},
            {stick_action, "/user/hand/left/input/thumbstick"},
            {stick_action, "/user/hand/right/input/thumbstick"},
            {thumbstick_click_action, "/user/hand/left/input/thumbstick/click"},
            {thumbstick_click_action, "/user/hand/right/input/thumbstick/click"},
            {x_click_action, "/user/hand/left/input/x/click"},
            {y_click_action, "/user/hand/left/input/y/click"},
            {menu_click_action, "/user/hand/left/input/menu/click"},
            {a_click_action, "/user/hand/right/input/a/click"},
            {b_click_action, "/user/hand/right/input/b/click"},
        });

    // FALLBACK: whatever the runtime offers when it is not Touch. khr/simple_controller is the
    // one profile every conformant runtime supports, so this guarantees poses even with nothing
    // configured -- select doubles as trigger, and there is no squeeze, stick or face buttons.
    suggest_bindings("/interaction_profiles/khr/simple_controller",
                     {
                         {aim_pose_action, "/user/hand/left/input/aim/pose"},
                         {aim_pose_action, "/user/hand/right/input/aim/pose"},
                         {grip_pose_action, "/user/hand/left/input/grip/pose"},
                         {grip_pose_action, "/user/hand/right/input/grip/pose"},
                         {trigger_action, "/user/hand/left/input/select/click"},
                         {trigger_action, "/user/hand/right/input/select/click"},
                         {menu_click_action, "/user/hand/left/input/menu/click"},
                         {menu_click_action, "/user/hand/right/input/menu/click"},
                     });

    // Illegal to suggest bindings after this point, so every profile above must be suggested
    // first.
    {
        XrSessionActionSetsAttachInfo saasi{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        saasi.countActionSets = 1;
        saasi.actionSets = &action_set;
        r = xrAttachSessionActionSets(session, &saasi);
        std::printf("[host] xrAttachSessionActionSets -> %s\n", rs(r));
    }

    // Two spaces per hand -- aim and grip genuinely differ in orientation on a Touch controller,
    // by roughly 45 degrees of pitch, so one space cannot serve both.
    XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    for (uint32_t h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo aim_asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        aim_asci.action = aim_pose_action;
        aim_asci.subactionPath = hand_path[h];
        aim_asci.poseInActionSpace.orientation.w = 1.0f;
        const XrResult aim_r = xrCreateActionSpace(session, &aim_asci, &aim_space[h]);
        std::printf("[host] aim action space, hand %u -> %s\n", h, rs(aim_r));

        XrActionSpaceCreateInfo grip_asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        grip_asci.action = grip_pose_action;
        grip_asci.subactionPath = hand_path[h];
        grip_asci.poseInActionSpace.orientation.w = 1.0f;
        const XrResult grip_r = xrCreateActionSpace(session, &grip_asci, &grip_space[h]);
        std::printf("[host] grip action space, hand %u -> %s\n", h, rs(grip_r));
    }

    // Snapshot of the last hand publish, for the periodic status line further down -- read back
    // rather than recomputed, since we are the only writer and just wrote it.
    bool hands_bound_log = false;
    bool hand_active_log[2] = {false, false};
    bool hand_tracked_log[2] = {false, false};
    float hand_aim_pos_log[2][3] = {{0, 0, 0}, {0, 0, 0}};

    // ---- THE GAME'S SCREEN ---------------------------------------------------------------------
    //
    // A QUAD LAYER, not a projection one, and deliberately so for this milestone. A quad is a flat
    // rectangle placed in space: it needs no per-eye FOV, no projection maths and no pose
    // correctness to look RIGHT, so if the game's pixels arrive wrong the fault is in the pixels.
    // Stereo projection is the next step and reuses everything above.
    SharedReader reader;

    // ONE SWAPCHAIN PER EYE, always -- mono simply puts the same picture in both. Keeping a single
    // code path means the mono case is not a special case that rots while stereo is developed.
    XrSwapchain screen[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<ID3D11Texture2D*> screen_images[2];
    uint32_t screen_w = 0;       // the size of ONE eye's picture, not of the published frame
    uint32_t screen_h = 0;
    uint32_t screen_layout = 0xFFFFFFFFu;
    bool screen_ready = false;   // an image has been released at least once
    uint64_t held = 0;           // frames where we re-showed the last picture
    bool use_projection = true;  // quads only until the game is tracking the head

    // ---- THE POSES WE PUBLISHED, KEPT ----------------------------------------------------------
    //
    // The game renders from a pose it read a frame or two ago and tells us which one. Submitting a
    // projection layer with the CURRENT pose instead would claim the image is newer than it is:
    // reprojection would then have nothing to correct and the world would swim with head motion.
    // Sixteen entries is about a quarter of a second at 90 Hz -- far more lag than the pipeline has.
    struct PosePair {
        uint32_t sequence = 0xFFFFFFFFu;
        XrPosef pose[2]{};

        // What the game was ASKED to render: one symmetric frustum, the same for both eyes.
        XrFovf rendered{};

        // What the headset actually WANTS, per eye, asymmetric. The difference between these two is
        // recovered by cropping -- see the crop maths at submission.
        XrFovf wanted[2]{};
    };

    PosePair pose_history[16];
    uint32_t published_sequence = 0;
    uint64_t pose_hits = 0;
    uint64_t pose_misses = 0;

    // BGRA to match a D3D9 back buffer byte for byte, so the upload is a copy and not a conversion.
    int64_t screen_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    {
        bool have_bgra = false;

        for (int64_t f : formats) {
            have_bgra = have_bgra || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }

        if (!have_bgra) {
            screen_format = chosen_format;
            std::printf("[host] no BGRA_SRGB swapchain format; falling back to %lld (red and blue "
                        "may swap)\n", static_cast<long long>(screen_format));
        }
    }

    // Created rather than opened: the host is normally up first, and CreateEvent returns the
    // existing object if the game got there before us.
    HANDLE tick_event = CreateEventA(nullptr, FALSE, FALSE, xr::kFrameTickEventName);

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    uint64_t frames = 0;
    uint64_t submitted = 0;
    const ULONGLONG started = GetTickCount64();

    std::printf("[host] entering frame loop -- PUT THE HEADSET ON if nothing appears; the runtime\n"
                "[host] keeps the session IDLE while it is unworn and will not accept frames.\n");

    while (!g_stop) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};

        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            // THE RUNTIME RECENTRED. Whatever the wearer just did in the headset moved the
            // origin of LOCAL space, so every position published from here on is measured from
            // somewhere new. The game cannot know that on its own -- it would keep differencing
            // against a stale origin and quietly place the wearer beside their character, which
            // is exactly what was reported. Telling it costs one counter.
            if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto* rc = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);

                if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL &&
                    reader.host != nullptr) {
                    ++reader.host->recenter_serial;
                    std::printf("[host] runtime recentred LOCAL space (serial %u)\n",
                                reader.host->recenter_serial);
                }
            }

            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                std::printf("[host] session -> %s\n", state_name(state));

                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    std::printf("[host] xrBeginSession -> %s\n", rs(xrBeginSession(session, &sbi)));
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (state == XR_SESSION_STATE_EXITING ||
                           state == XR_SESSION_STATE_LOSS_PENDING) {
                    g_stop = true;
                }
            }

            // THE MOST USEFUL LINE WHEN BINDINGS SILENTLY DO NOT APPLY: the runtime tells us it
            // picked a different interaction profile for a hand (including none, at session
            // start or when a controller is powered off), and we ask it which one rather than
            // guessing from the suggestions we made.
            if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                for (uint32_t h = 0; h < 2; ++h) {
                    XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
                    const XrResult pr = xrGetCurrentInteractionProfile(session, hand_path[h], &ips);

                    if (XR_SUCCEEDED(pr) && ips.interactionProfile != XR_NULL_PATH) {
                        uint32_t len = 0;
                        char path_buf[XR_MAX_PATH_LENGTH] = {};
                        xrPathToString(g_instance, ips.interactionProfile, sizeof(path_buf), &len,
                                       path_buf);
                        std::printf("[host] interaction profile changed, hand %u -> %s\n", h,
                                    path_buf);
                    } else {
                        std::printf("[host] interaction profile changed, hand %u -> none (%s)\n", h,
                                    rs(pr));
                    }
                }
            }

            ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!running) {
            Sleep(20);

            if (max_seconds > 0 && GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
                break;
            }

            continue;
        }

        // xrWaitFrame is the compositor's throttle -- it is what paces this loop, not a sleep.
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        xrWaitFrame(session, &fwi, &fs);

        // THE FRAME CLOCK, relayed. xrWaitFrame has just told us when the runtime wants the next
        // frame; releasing the game here makes its update run on the compositor's cadence instead
        // of free-running at whatever the hardware allows.
        if (tick_event != nullptr) {
            SetEvent(tick_event);
        }

        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        std::vector<XrCompositionLayerProjectionView> layer_views(view_count);
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[2]{};
        uint32_t layer_count = 0;

        // Pull the newest complete frame the game has published, if any.
        uint32_t fw = 0, fh = 0, fpitch = 0;
        const uint8_t* fbits = nullptr;
        bool have_frame = false;

        if (reader.open()) {
            have_frame = reader.poll(fw, fh, fpitch, fbits);
        }

        // A side-by-side frame is TWO pictures: each eye gets half the width.
        const uint32_t layout = reader.layout();
        const uint32_t eye_w = (layout == xr::kLayoutSideBySide) ? fw / 2u : fw;
        const uint32_t eye_h = fh;

        if (have_frame &&
            (screen[0] == XR_NULL_HANDLE || eye_w != screen_w || eye_h != screen_h ||
             layout != screen_layout)) {
            // Sized to the GAME's frame, not the runtime's recommendation: at native size the
            // upload is a straight copy with no resampling anywhere in the path.
            bool ok = true;

            for (int e = 0; e < 2; ++e) {
                screen_images[e].clear();

                if (screen[e] != XR_NULL_HANDLE) {
                    xrDestroySwapchain(screen[e]);
                    screen[e] = XR_NULL_HANDLE;
                }

                XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sc.usageFlags =
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sc.format = screen_format;
                sc.sampleCount = 1;
                // Sized to the EYE's picture, so the upload stays a straight copy with no
                // resampling anywhere between the game's back buffer and the compositor.
                sc.width = eye_w;
                sc.height = eye_h;
                sc.faceCount = 1;
                sc.arraySize = 1;
                sc.mipCount = 1;

                const XrResult screen_r = xrCreateSwapchain(session, &sc, &screen[e]);
                std::printf("[host] eye %d screen swapchain %ux%u (%s) -> %s\n", e, eye_w, eye_h,
                            layout == xr::kLayoutSideBySide ? "side-by-side" : "mono", rs(screen_r));

                if (XR_FAILED(screen_r)) {
                    ok = false;
                    break;
                }

                uint32_t n = 0;
                xrEnumerateSwapchainImages(screen[e], 0, &n, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                xrEnumerateSwapchainImages(
                    screen[e], n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));

                for (uint32_t k = 0; k < n; ++k) {
                    screen_images[e].push_back(imgs[k].texture);
                }
            }

            if (ok) {
                screen_w = eye_w;
                screen_h = eye_h;
                screen_layout = layout;
                screen_ready = false;
            } else {
                have_frame = false;
            }
        }

        // ---- TELL THE GAME WHERE THE HEAD IS ---------------------------------------------------
        //
        // Published every frame, whether or not the game is listening: the pose is what makes a
        // projection layer honest later, and a game that starts listening mid-session should find
        // current data rather than wait a frame for it.
        if (fs.shouldRender != XR_FALSE && reader.host != nullptr) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;

            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t located = 0;
            std::vector<XrView> hv(view_count, {XR_TYPE_VIEW});

            if (XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, view_count, &located, hv.data())) &&
                located >= 2) {
                auto* hs = reader.host;
                hs->sequence |= 1u;
                MemoryBarrier();

                // The HEAD is the midpoint of the two eyes -- the runtime reports eyes, not a head,
                // and handing the game one eye's pose would offset the whole world by half an IPD.
                for (int k = 0; k < 3; ++k) {
                    const float a = (&hv[0].pose.position.x)[k];
                    const float b = (&hv[1].pose.position.x)[k];
                    (&hs->position[0])[k] = (a + b) * 0.5f;
                }

                hs->orientation[0] = hv[0].pose.orientation.x;
                hs->orientation[1] = hv[0].pose.orientation.y;
                hs->orientation[2] = hv[0].pose.orientation.z;
                hs->orientation[3] = hv[0].pose.orientation.w;

                const float dx = hv[1].pose.position.x - hv[0].pose.position.x;
                const float dy = hv[1].pose.position.y - hv[0].pose.position.y;
                const float dz = hv[1].pose.position.z - hv[0].pose.position.z;
                hs->ipd_m = sqrtf(dx * dx + dy * dy + dz * dz);

                // The SMALLEST SYMMETRIC frustum containing the headset's asymmetric one. This
                // engine offers no asymmetric projection, so the game over-renders the corners and
                // the compositor crops -- pixels spent to keep the frustum truthful.
                float mx = 0.0f;
                float my = 0.0f;

                for (uint32_t v = 0; v < 2; ++v) {
                    mx = (std::max)(mx, (std::max)(fabsf(hv[v].fov.angleLeft),
                                                   fabsf(hv[v].fov.angleRight)));
                    my = (std::max)(my, (std::max)(fabsf(hv[v].fov.angleUp),
                                                   fabsf(hv[v].fov.angleDown)));
                }

                hs->fov_x = mx;
                hs->fov_y = my;
                hs->valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ? 1u : 0u;
                hs->write_qpc = 0;
                ++hs->frames;

                MemoryBarrier();
                hs->sequence = (hs->sequence + 1u) & ~1u;
                published_sequence = hs->sequence;

                // Keep what we just handed out, so the frame rendered from it can be submitted with
                // it rather than with whatever is current by the time the pixels arrive.
                PosePair& slot = pose_history[(published_sequence / 2u) % 16u];
                slot.sequence = published_sequence;
                slot.pose[0] = hv[0].pose;
                slot.pose[1] = hv[1].pose;

                // The SYMMETRIC frustum we asked the game for...
                slot.rendered.angleLeft = -mx;
                slot.rendered.angleRight = mx;
                slot.rendered.angleUp = my;
                slot.rendered.angleDown = -my;

                // ...and the ASYMMETRIC one each eye actually needs. A headset's frustums are
                // sheared toward the nose, and that shear IS the convergence: two parallel
                // symmetric frustums put infinity at a non-zero disparity, so nothing ever
                // converges and the eyes are asked to diverge. Reported as "the eyes are not
                // converging, at all", which is exactly what parallel cameras look like.
                slot.wanted[0] = hv[0].fov;
                slot.wanted[1] = hv[1].fov;
            }
        }

        // ---- TELL THE GAME WHAT THE HANDS ARE DOING --------------------------------------------
        //
        // Synced and located at the SAME predictedDisplayTime and in the SAME reference `space` as
        // the head above, so a weapon driven by aim_pose and a camera driven by the head pose
        // describe one instant and one origin -- a mismatch here would not error, it would just
        // make the hands lag or sit in the wrong place.
        if (reader.hands != nullptr) {
            // xrSyncActions is only meaningful once the session is FOCUSED -- XR_SESSION_NOT_FOCUSED
            // is the documented result otherwise (e.g. while the system UI has input), and action
            // state is not defined to still track live input after that. Treating "not focused" as
            // "not active" here, rather than publishing whatever was last synced, is what keeps a
            // menu overlay from leaving a weapon aimed at a frozen ray.
            bool hands_active = (state == XR_SESSION_STATE_FOCUSED);

            if (hands_active) {
                XrActiveActionSet active_set{action_set, XR_NULL_PATH};
                XrActionsSyncInfo asi{XR_TYPE_ACTIONS_SYNC_INFO};
                asi.countActiveActionSets = 1;
                asi.activeActionSets = &active_set;
                hands_active = XR_SUCCEEDED(xrSyncActions(session, &asi));
            }

            // Polled rather than tracked off XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: one
            // call here is simpler than a persistent flag threaded through the event loop, and
            // this runs every frame regardless, so there is no missed-event window. The event
            // handler above still logs the transition, since a silent poll is a far worse
            // debugging experience than one that names what changed and when.
            XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
            const XrResult prof_r =
                xrGetCurrentInteractionProfile(session, hand_path[xr::kHandRight], &ips);
            const bool profile_bound =
                XR_SUCCEEDED(prof_r) && ips.interactionProfile != XR_NULL_PATH;

            auto* hs = reader.hands;
            hs->sequence |= 1u;
            MemoryBarrier();

            hs->profile_bound = profile_bound ? 1u : 0u;

            for (uint32_t h = 0; h < 2; ++h) {
                xr::HandInput& hi = hs->hand[h];

                if (!hands_active) {
                    hi.active = 0;
                    hi.tracked = 0;
                    hi.aim.valid = 0;
                    hi.grip.valid = 0;
                    hi.trigger = 0.0f;
                    hi.squeeze = 0.0f;
                    hi.stick[0] = 0.0f;
                    hi.stick[1] = 0.0f;
                    hi.buttons = 0;
                    hand_active_log[h] = false;
                    hand_tracked_log[h] = false;
                    continue;
                }

                XrActionStateGetInfo pgi{XR_TYPE_ACTION_STATE_GET_INFO};
                pgi.action = aim_pose_action;
                pgi.subactionPath = hand_path[h];
                XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
                xrGetActionStatePose(session, &pgi, &pose_state);
                hi.active = (pose_state.isActive != XR_FALSE) ? 1u : 0u;

                XrSpaceLocation aim_loc{XR_TYPE_SPACE_LOCATION};
                xrLocateSpace(aim_space[h], space, fs.predictedDisplayTime, &aim_loc);
                const bool aim_valid =
                    (aim_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (aim_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
                const bool aim_tracked =
                    (aim_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) &&
                    (aim_loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
                hi.aim.orientation[0] = aim_loc.pose.orientation.x;
                hi.aim.orientation[1] = aim_loc.pose.orientation.y;
                hi.aim.orientation[2] = aim_loc.pose.orientation.z;
                hi.aim.orientation[3] = aim_loc.pose.orientation.w;
                hi.aim.position[0] = aim_loc.pose.position.x;
                hi.aim.position[1] = aim_loc.pose.position.y;
                hi.aim.position[2] = aim_loc.pose.position.z;
                hi.aim.valid = aim_valid ? 1u : 0u;

                // TRACKED is carried once per hand, not once per pose, so it is read off the AIM
                // location: aim is what a weapon follows, so whether gameplay sees a tracked pose
                // or an inferred one should be about that ray, not the grip.
                hi.tracked = aim_tracked ? 1u : 0u;

                XrSpaceLocation grip_loc{XR_TYPE_SPACE_LOCATION};
                xrLocateSpace(grip_space[h], space, fs.predictedDisplayTime, &grip_loc);
                const bool grip_valid =
                    (grip_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (grip_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
                hi.grip.orientation[0] = grip_loc.pose.orientation.x;
                hi.grip.orientation[1] = grip_loc.pose.orientation.y;
                hi.grip.orientation[2] = grip_loc.pose.orientation.z;
                hi.grip.orientation[3] = grip_loc.pose.orientation.w;
                hi.grip.position[0] = grip_loc.pose.position.x;
                hi.grip.position[1] = grip_loc.pose.position.y;
                hi.grip.position[2] = grip_loc.pose.position.z;
                hi.grip.valid = grip_valid ? 1u : 0u;

                // Float, vector2f and boolean action states: not logged per call, same as
                // xrLocateViews and the swapchain acquire/wait/release calls above -- these run up
                // to eight times a hand, every frame, and a log line per call would bury the ones
                // that matter under noise. isActive false (no binding on this profile, or on this
                // hand -- A/B and X/Y are single-hand on Touch) reads as the at-rest value rather
                // than whatever was last synced.
                XrActionStateGetInfo fgi{XR_TYPE_ACTION_STATE_GET_INFO};
                fgi.subactionPath = hand_path[h];

                fgi.action = trigger_action;
                XrActionStateFloat trig{XR_TYPE_ACTION_STATE_FLOAT};
                xrGetActionStateFloat(session, &fgi, &trig);
                hi.trigger = (trig.isActive != XR_FALSE) ? trig.currentState : 0.0f;

                fgi.action = squeeze_action;
                XrActionStateFloat sq{XR_TYPE_ACTION_STATE_FLOAT};
                xrGetActionStateFloat(session, &fgi, &sq);
                hi.squeeze = (sq.isActive != XR_FALSE) ? sq.currentState : 0.0f;

                XrActionStateGetInfo vgi{XR_TYPE_ACTION_STATE_GET_INFO};
                vgi.action = stick_action;
                vgi.subactionPath = hand_path[h];
                XrActionStateVector2f v2{XR_TYPE_ACTION_STATE_VECTOR2F};
                xrGetActionStateVector2f(session, &vgi, &v2);
                hi.stick[0] = (v2.isActive != XR_FALSE) ? v2.currentState.x : 0.0f;
                hi.stick[1] = (v2.isActive != XR_FALSE) ? v2.currentState.y : 0.0f;

                XrActionStateGetInfo bgi{XR_TYPE_ACTION_STATE_GET_INFO};
                bgi.subactionPath = hand_path[h];

                auto get_bool = [&](XrAction action) {
                    bgi.action = action;
                    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
                    xrGetActionStateBoolean(session, &bgi, &st);
                    return st.isActive != XR_FALSE && st.currentState != XR_FALSE;
                };

                uint32_t buttons = 0;
                buttons |= get_bool(a_click_action) ? xr::kHandButtonA : 0u;
                buttons |= get_bool(b_click_action) ? xr::kHandButtonB : 0u;
                buttons |= get_bool(x_click_action) ? xr::kHandButtonX : 0u;
                buttons |= get_bool(y_click_action) ? xr::kHandButtonY : 0u;
                buttons |= get_bool(thumbstick_click_action) ? xr::kHandButtonThumbstick : 0u;
                buttons |= get_bool(menu_click_action) ? xr::kHandButtonMenu : 0u;
                hi.buttons = buttons;

                hand_active_log[h] = hi.active != 0;
                hand_tracked_log[h] = hi.tracked != 0;
                hand_aim_pos_log[h][0] = hi.aim.position[0];
                hand_aim_pos_log[h][1] = hi.aim.position[1];
                hand_aim_pos_log[h][2] = hi.aim.position[2];
            }

            hs->write_qpc = 0;
            ++hs->frames;

            MemoryBarrier();
            hs->sequence = (hs->sequence + 1u) & ~1u;

            hands_bound_log = profile_bound;
        }

        XrCompositionLayerQuad quad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                          {XR_TYPE_COMPOSITION_LAYER_QUAD}};

        // ---- UPLOAD ONLY WHEN THERE IS SOMETHING NEW -------------------------------------------
        //
        // The compositor asks for a frame 90 times a second; the game publishes about 68. So on
        // roughly a quarter of frames there is no new picture, and the first version of this loop
        // fell through to the colour clear on exactly those -- which the wearer saw as the red/blue
        // test pattern FLICKERING THROUGH the game.
        //
        // The right answer is to show the last picture again. A swapchain whose image has been
        // released may be submitted on later frames without re-acquiring, and the runtime uses that
        // last released image -- so holding a frame costs nothing and is what a compositor expects.
        if (fs.shouldRender != XR_FALSE && have_frame && screen[0] != XR_NULL_HANDLE) {
            for (int e = 0; e < 2; ++e) {
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(screen[e], &ai, &index);

                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(screen[e], &wi);

                // SLICING WITHOUT A SHADER. UpdateSubresource walks the source using the stride it
                // is given, so starting the right eye half a row in and keeping the FULL pitch
                // lifts the right half out in one copy. A wrong pitch here does not tint anything,
                // it shears the picture diagonally.
                const uint8_t* eye_bits = (screen_layout == xr::kLayoutSideBySide && e == 1)
                                              ? fbits + static_cast<size_t>(screen_w) * 4u
                                              : fbits;

                ctx->UpdateSubresource(screen_images[e][index], 0, nullptr, eye_bits, fpitch, 0);

                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(screen[e], &ri);
            }

            screen_ready = true;
        }

        // ---- PROJECTION, ONCE THE GAME IS ACTUALLY TRACKING THE HEAD ---------------------------
        //
        // Submitted with the pose the game RENDERED FROM, looked up by the sequence it echoed back,
        // and with the same symmetric FOV it was asked to use. Both halves of that matter: a
        // projection layer is a claim about how the image was produced, and the compositor acts on
        // the claim. Get the pose wrong and reprojection corrects the wrong amount; get the FOV
        // wrong and the world is the wrong size.
        //
        // Falls back to the quads when the game is not tracking -- a flat screen is honest, a
        // projection layer built on a stationary camera is not.
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView proj_views[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};

        const uint32_t rendered_seq = reader.frame_host_sequence();
        const PosePair& slot = pose_history[(rendered_seq / 2u) % 16u];
        const bool pose_known = use_projection && rendered_seq != 0 && slot.sequence == rendered_seq;

        if (fs.shouldRender != XR_FALSE && screen_ready && pose_known) {
            if (!have_frame) {
                ++held;
            }

            ++pose_hits;

            // ---- CROPPING IS THE OFF-AXIS PROJECTION -------------------------------------------
            //
            // Cutting an off-centre rectangle out of a symmetric render is mathematically identical
            // to having rendered an asymmetric frustum, provided the symmetric one CONTAINS it --
            // which it does by construction, since its half-angles are the max over both eyes.
            //
            // The mapping is through TANGENTS, not angles: a perspective image is linear in
            // tan(angle), so an angle a inside a symmetric frustum of half-angle m lands at
            //     x = W * (tan a + tan m) / (2 tan m)
            // Interpolating in angle instead would be subtly wrong everywhere and grossly wrong at
            // the edges -- and it would look like a lens problem rather than an arithmetic one.
            const float tx = tanf(slot.rendered.angleRight);
            const float ty = tanf(slot.rendered.angleUp);

            for (int e = 0; e < 2; ++e) {
                const XrFovf& want = slot.wanted[e];

                const float x0 = (tanf(want.angleLeft) + tx) / (2.0f * tx);
                const float x1 = (tanf(want.angleRight) + tx) / (2.0f * tx);

                // Y IS FLIPPED: angleUp is positive upward, image rows run downward, so the TOP of
                // the rectangle comes from angleUp.
                const float y0 = (ty - tanf(want.angleUp)) / (2.0f * ty);
                const float y1 = (ty - tanf(want.angleDown)) / (2.0f * ty);

                auto to_px = [](float f, uint32_t extent) {
                    const int32_t v = static_cast<int32_t>(lroundf(f * static_cast<float>(extent)));
                    return v < 0 ? 0 : (v > static_cast<int32_t>(extent) ? static_cast<int32_t>(extent) : v);
                };

                const int32_t px0 = to_px(x0, screen_w);
                const int32_t px1 = to_px(x1, screen_w);
                const int32_t py0 = to_px(y0, screen_h);
                const int32_t py1 = to_px(y1, screen_h);

                proj_views[e].pose = slot.pose[e];

                // Declare what the CROP represents, not what was rendered: the rectangle now is the
                // headset's own asymmetric frustum.
                proj_views[e].fov = want;
                proj_views[e].subImage.swapchain = screen[e];
                proj_views[e].subImage.imageRect.offset = {px0, py0};
                proj_views[e].subImage.imageRect.extent = {(std::max)(1, px1 - px0),
                                                           (std::max)(1, py1 - py0)};
            }

            proj.space = space;
            proj.viewCount = 2;
            proj.views = proj_views;
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            layer_count = 1;
            ++submitted;
        } else if (fs.shouldRender != XR_FALSE && screen_ready) {
            if (!have_frame) {
                ++held;
            }

            if (use_projection && rendered_seq != 0) {
                ++pose_misses;
            }

            // TWO QUADS, ONE PER EYE, and this is the honest choice until the game's camera follows
            // the head. A projection layer asserts "this image was rendered from the pose you just
            // gave me", and the compositor would then reproject it against head motion the game did
            // not apply -- a world that swings when you look around, which is both wrong and
            // sickening. A quad claims nothing: a flat rectangle at a fixed place, with each eye
            // given its own half, which is genuine stereo depth on it.
            const float width_m = 2.0f;

            for (int e = 0; e < 2; ++e) {
                quad[e].space = space;
                quad[e].eyeVisibility = (e == 0) ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                quad[e].subImage.swapchain = screen[e];
                quad[e].subImage.imageRect.offset = {0, 0};
                quad[e].subImage.imageRect.extent = {static_cast<int32_t>(screen_w),
                                                     static_cast<int32_t>(screen_h)};
                quad[e].pose.orientation.w = 1.0f;
                quad[e].pose.position = {0.0f, 0.0f, -1.6f};

                // STRETCHED BACK, because a split half is not a narrower VIEW -- it is the whole
                // view squeezed. Measured earlier in this project: "the left half IS the whole
                // scene at half width", i.e. the engine keeps its horizontal FOV and renders it
                // into half the pixels. Displaying such a half at its own 1280x1440 pixel aspect
                // would show a correct picture of the wrong shape: everything tall and thin.
                //
                // So the quad takes the FULL frame's aspect and each eye's half fills it.
                const float content_w =
                    static_cast<float>(screen_w) * (screen_layout == xr::kLayoutSideBySide ? 2.0f
                                                                                           : 1.0f);
                quad[e].size = {width_m, width_m * static_cast<float>(screen_h) / content_w};
                layers[e] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad[e]);
            }

            layer_count = 2;
            ++submitted;
        } else if (fs.shouldRender != XR_FALSE) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;

            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t located = 0;
            std::vector<XrView> views(view_count, {XR_TYPE_VIEW});
            xrLocateViews(session, &vli, &vs, view_count, &located, views.data());

            for (uint32_t i = 0; i < view_count; ++i) {
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(eyes[i].swapchain, &ai, &index);

                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eyes[i].swapchain, &wi);

                // NO SHADERS ON PURPOSE. A clear is the smallest thing that can put light in front
                // of someone, so if this does not appear the fault is in the session, the swapchain
                // or the submission -- never in a triangle.
                const float t = static_cast<float>((frames % 120)) / 120.0f;
                const float colour[2][4] = {{0.15f + 0.35f * t, 0.05f, 0.05f, 1.0f},
                                            {0.05f, 0.05f, 0.15f + 0.35f * t, 1.0f}};
                ctx->ClearRenderTargetView(eyes[i].views[index], colour[i % 2]);

                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(eyes[i].swapchain, &ri);

                layer_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                layer_views[i].pose = views[i].pose;
                layer_views[i].fov = views[i].fov;
                layer_views[i].subImage.swapchain = eyes[i].swapchain;
                layer_views[i].subImage.imageRect.offset = {0, 0};
                layer_views[i].subImage.imageRect.extent = {static_cast<int32_t>(eyes[i].width),
                                                            static_cast<int32_t>(eyes[i].height)};
            }

            layer.space = space;
            layer.viewCount = view_count;
            layer.views = layer_views.data();
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            layer_count = 1;
            ++submitted;
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layer_count;
        fei.layers = layer_count != 0 ? layers : nullptr;
        const XrResult end = xrEndFrame(session, &fei);

        if (++frames % 90 == 0) {
            std::printf("[host] %llu frames, %llu submitted, %llu held, projection %llu (missed "
                        "pose %llu), state %s, last xrEndFrame %s, hands bound %s, "
                        "L %s/%s (%.2f,%.2f,%.2f) R %s/%s (%.2f,%.2f,%.2f)\n",
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(submitted),
                        static_cast<unsigned long long>(held),
                        static_cast<unsigned long long>(pose_hits),
                        static_cast<unsigned long long>(pose_misses), state_name(state), rs(end),
                        hands_bound_log ? "yes" : "no", hand_active_log[xr::kHandLeft] ? "active" : "idle",
                        hand_tracked_log[xr::kHandLeft] ? "tracked" : "inferred",
                        hand_aim_pos_log[xr::kHandLeft][0], hand_aim_pos_log[xr::kHandLeft][1],
                        hand_aim_pos_log[xr::kHandLeft][2],
                        hand_active_log[xr::kHandRight] ? "active" : "idle",
                        hand_tracked_log[xr::kHandRight] ? "tracked" : "inferred",
                        hand_aim_pos_log[xr::kHandRight][0], hand_aim_pos_log[xr::kHandRight][1],
                        hand_aim_pos_log[xr::kHandRight][2]);
        }

        if (max_seconds > 0 && GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
            break;
        }
    }

    std::printf("[host] stopping: %llu frames, %llu submitted, final state %s\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(submitted), state_name(state));

    if (running) {
        xrEndSession(session);
    }

    for (auto& e : eyes) {
        for (auto* v : e.views) {
            if (v != nullptr) {
                v->Release();
            }
        }

        if (e.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(e.swapchain);
        }
    }

    for (int e = 0; e < 2; ++e) {
        if (screen[e] != XR_NULL_HANDLE) {
            xrDestroySwapchain(screen[e]);
        }
    }

    if (space != XR_NULL_HANDLE) {
        xrDestroySpace(space);
    }

    // xrDestroyActionSet also destroys the actions it owns, per spec -- nothing to release there.
    for (int h = 0; h < 2; ++h) {
        if (aim_space[h] != XR_NULL_HANDLE) {
            xrDestroySpace(aim_space[h]);
        }

        if (grip_space[h] != XR_NULL_HANDLE) {
            xrDestroySpace(grip_space[h]);
        }
    }

    if (action_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set);
    }

    xrDestroySession(session);
    xrDestroyInstance(g_instance);

    if (ctx != nullptr) {
        ctx->Release();
    }

    device->Release();
    return 0;
}

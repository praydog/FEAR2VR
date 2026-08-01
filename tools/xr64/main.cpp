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
    const uint8_t* payload = nullptr;
    uint32_t last_sequence = 0;

    bool open() {
        if (header != nullptr) {
            return true;
        }

        mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, xr::kSharedFrameName);

        if (mapping == nullptr) {
            return false;
        }

        const void* base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);

        if (base == nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }

        header = static_cast<const xr::SharedFrameHeader*>(base);
        payload = static_cast<const uint8_t*>(base) + sizeof(xr::SharedFrameHeader);
        return true;
    }

    // True when a COMPLETE frame newer than the last one is available.
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
        bits = payload;

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

    // ---- THE GAME'S SCREEN ---------------------------------------------------------------------
    //
    // A QUAD LAYER, not a projection one, and deliberately so for this milestone. A quad is a flat
    // rectangle placed in space: it needs no per-eye FOV, no projection maths and no pose
    // correctness to look RIGHT, so if the game's pixels arrive wrong the fault is in the pixels.
    // Stereo projection is the next step and reuses everything above.
    SharedReader reader;
    XrSwapchain screen = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> screen_images;
    uint32_t screen_w = 0;
    uint32_t screen_h = 0;
    bool screen_ready = false;   // an image has been released at least once
    uint64_t held = 0;           // frames where we re-showed the last picture

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

        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        std::vector<XrCompositionLayerProjectionView> layer_views(view_count);
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1]{};
        uint32_t layer_count = 0;

        // Pull the newest complete frame the game has published, if any.
        uint32_t fw = 0, fh = 0, fpitch = 0;
        const uint8_t* fbits = nullptr;
        bool have_frame = false;

        if (reader.open()) {
            have_frame = reader.poll(fw, fh, fpitch, fbits);
        }

        if (have_frame && (screen == XR_NULL_HANDLE || fw != screen_w || fh != screen_h)) {
            // Sized to the GAME's frame, not the runtime's recommendation: at native size the
            // upload is a straight copy with no resampling anywhere in the path.
            for (auto* t : screen_images) {
                (void)t;
            }

            screen_images.clear();

            if (screen != XR_NULL_HANDLE) {
                xrDestroySwapchain(screen);
                screen = XR_NULL_HANDLE;
            }

            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = screen_format;
            sc.sampleCount = 1;
            sc.width = fw;
            sc.height = fh;
            sc.faceCount = 1;
            sc.arraySize = 1;
            sc.mipCount = 1;

            const XrResult screen_r = xrCreateSwapchain(session, &sc, &screen);
            std::printf("[host] screen swapchain %ux%u -> %s\n", fw, fh, rs(screen_r));

            if (XR_SUCCEEDED(screen_r)) {
                screen_w = fw;
                screen_h = fh;
                uint32_t n = 0;
                xrEnumerateSwapchainImages(screen, 0, &n, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                xrEnumerateSwapchainImages(screen, n, &n,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));

                for (uint32_t k = 0; k < n; ++k) {
                    screen_images.push_back(imgs[k].texture);
                }
            } else {
                have_frame = false;
            }
        }

        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

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
        if (fs.shouldRender != XR_FALSE && have_frame && screen != XR_NULL_HANDLE) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xrAcquireSwapchainImage(screen, &ai, &index);

            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(screen, &wi);

            // ONE copy, straight from the shared section into the compositor's texture. The row
            // pitch comes from the writer: a locked D3D surface is padded, and assuming width*4
            // shears the picture into diagonal stripes.
            ctx->UpdateSubresource(screen_images[index], 0, nullptr, fbits, fpitch, 0);

            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(screen, &ri);
            screen_ready = true;
        }

        if (fs.shouldRender != XR_FALSE && screen_ready) {
            if (!have_frame) {
                ++held;
            }

            // A 2 m wide screen, 1.6 m ahead, at eye level, aspect preserved from the source.
            const float width_m = 2.0f;
            quad.space = space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = screen;
            quad.subImage.imageRect.offset = {0, 0};
            quad.subImage.imageRect.extent = {static_cast<int32_t>(screen_w),
                                              static_cast<int32_t>(screen_h)};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position = {0.0f, 0.0f, -1.6f};
            quad.size = {width_m, width_m * static_cast<float>(screen_h) /
                                      static_cast<float>(screen_w)};
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            layer_count = 1;
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
            std::printf("[host] %llu frames, %llu submitted, %llu held (no new game frame), "
                        "state %s, last xrEndFrame %s\n",
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(submitted),
                        static_cast<unsigned long long>(held), state_name(state), rs(end));
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

    if (space != XR_NULL_HANDLE) {
        xrDestroySpace(space);
    }

    xrDestroySession(session);
    xrDestroyInstance(g_instance);

    if (ctx != nullptr) {
        ctx->Release();
    }

    device->Release();
    return 0;
}

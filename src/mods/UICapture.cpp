#include "UICapture.hpp"

#include "SceneTarget.hpp"

#include <d3d9.h>

#include <cstdio>

#include "Log.hpp"
#include "RenderHook.hpp"
#include "FramePublisher.hpp"
#include "HudPassHook.hpp"
#include "RenderTimeline.hpp"
#include "sdk/Modules.hpp"
#include "sdk/Render.hpp"

namespace {

void bracket_cb(bool begin, int32_t width, int32_t height, uint32_t index) {
    UICapture::get().on_bracket(begin, width, height, index);
}

void pass_cb(uint32_t ordinal, uintptr_t caller) {
    UICapture::get().on_pass(ordinal, caller);
}

void present_cb() {
    UICapture::get().on_present();
}

// A 32-bit BGRA bitmap, alpha preserved. Written top-down (negative height) so a viewer shows it
// the same way up as the frame; FrameCapture's own writer is 24-bit and would discard exactly the
// channel this mod exists to produce.
bool write_bmp32(const char* path, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t pitch) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        return false;
    }
    const uint32_t bytes = w * h * 4u;
    uint8_t file_hdr[14]{};
    uint8_t info_hdr[40]{};
    const uint32_t off = 14 + 40;
    const uint32_t total = off + bytes;
    file_hdr[0] = 'B';
    file_hdr[1] = 'M';
    memcpy(file_hdr + 2, &total, 4);
    memcpy(file_hdr + 10, &off, 4);
    const uint32_t hdr_size = 40;
    const int32_t neg_h = -static_cast<int32_t>(h);
    const uint16_t planes = 1, bpp = 32;
    memcpy(info_hdr + 0, &hdr_size, 4);
    memcpy(info_hdr + 4, &w, 4);
    memcpy(info_hdr + 8, &neg_h, 4);
    memcpy(info_hdr + 12, &planes, 2);
    memcpy(info_hdr + 14, &bpp, 2);
    memcpy(info_hdr + 20, &bytes, 4);
    fwrite(file_hdr, 1, sizeof(file_hdr), f);
    fwrite(info_hdr, 1, sizeof(info_hdr), f);
    for (uint32_t y = 0; y < h; ++y) {
        fwrite(pixels + static_cast<size_t>(y) * pitch, 1, static_cast<size_t>(w) * 4u, f);
    }
    fclose(f);
    return true;
}


// Samples a surface's non-black fraction (per mille) through a tiny blit. Costs two temporary
// surfaces and one lock, so it runs only when asked.
int32_t sample_lit(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    IDirect3DSurface9* tiny = nullptr;
    IDirect3DSurface9* sys = nullptr;
    int32_t out = -1;
    if (SUCCEEDED(dev->CreateRenderTarget(64, 36, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                          &tiny, nullptr)) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 36, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys,
                                                   nullptr)) &&
        SUCCEEDED(dev->StretchRect(src, nullptr, tiny, nullptr, D3DTEXF_LINEAR)) &&
        SUCCEEDED(dev->GetRenderTargetData(tiny, sys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            uint32_t lit = 0;
            for (int y = 0; y < 36; ++y) {
                const auto* row = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
                for (int x = 0; x < 64; ++x) {
                    if (row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]) {
                        ++lit;
                    }
                }
            }
            out = static_cast<int32_t>(lit * 1000u / (64u * 36u));
            sys->UnlockRect();
        }
    }
    if (sys != nullptr) { sys->Release(); }
    if (tiny != nullptr) { tiny->Release(); }
    return out;
}
} // namespace

UICapture& UICapture::get() {
    static UICapture instance;
    return instance;
}

std::optional<std::string> UICapture::on_initialize() {
    if (!RenderTimeline::get().add_bracket_callback(&bracket_cb)) {
        return std::string{"could not register a render-target bracket callback"};
    }
    if (!HudPassHook::get().add_pass_callback(&pass_cb)) {
        return std::string{"could not register a 2D-pass callback"};
    }
    if (!RenderHook::get().add_present_callback(&present_cb)) {
        return std::string{"could not register a present callback -- the surface would never be released"};
    }
    return std::nullopt;
}

void UICapture::free_device_resources() {
    // The saved surface first: it is an engine surface we hold a reference to, and letting it go
    // is never wrong -- if we are mid-bracket the restore is lost, which is a dropped frame of
    // HUD, not a leak.
    if (auto* saved = static_cast<IDirect3DSurface9*>(m_saved.exchange(nullptr, std::memory_order_acq_rel))) {
        saved->Release();
    }
    if (auto* surf = static_cast<IDirect3DSurface9*>(m_surface.exchange(nullptr, std::memory_order_acq_rel))) {
        surf->Release();
    }
    if (auto* scratch = static_cast<IDirect3DSurface9*>(m_scratch.exchange(nullptr, std::memory_order_acq_rel))) {
        scratch->Release();
    }
    if (auto* s = static_cast<IDirect3DSurface9*>(m_scaled.exchange(nullptr, std::memory_order_acq_rel))) {
        s->Release();
    }
    if (auto* g = static_cast<IDirect3DSurface9*>(m_stage.exchange(nullptr, std::memory_order_acq_rel))) {
        g->Release();
    }
    m_width.store(0, std::memory_order_relaxed);
    m_height.store(0, std::memory_order_relaxed);
}

void UICapture::on_bracket(bool begin, int32_t width, int32_t height, uint32_t index) {
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (index != RenderTimeline::get().hud_bracket() || width <= 0 || height <= 0) {
        return;
    }
    auto* dev = sdk::Render::device();
    if (dev == nullptr) {
        return;
    }

    if (begin) {
        // The bracket only PREPARES. Which passes inside it are the HUD is decided per pass, by
        // who issued them -- see on_pass().
        m_seen_fullscreen = false;
        m_cleared_this_frame = false;
        m_pass_seen = 0;

        auto* surf = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));
        if (surf != nullptr && m_width.load(std::memory_order_relaxed) == width &&
            m_height.load(std::memory_order_relaxed) == height) {
            return;
        }
        // Recreate on a size change as well as on absence: a resolution change leaves a surface the
        // engine's viewport no longer matches, and binding it would silently letterbox.
        free_device_resources();
        IDirect3DSurface9* created = nullptr;
        if (FAILED(dev->CreateRenderTarget(static_cast<UINT>(width), static_cast<UINT>(height),
                                           D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &created,
                                           nullptr)) ||
            created == nullptr) {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_surface.store(created, std::memory_order_release);
        m_width.store(width, std::memory_order_relaxed);
        m_height.store(height, std::memory_order_relaxed);

        // ---- THE LAYER FOLLOWS THE SOURCE'S SHAPE ---------------------------------------------
        //
        // The layer height was fixed at 720 against a fixed 1280, which is only correct while the
        // screen happens to be 16:9. Supersampling makes it 4320x2224 -- 1.94 -- and squeezing that
        // into 1.78 compressed the HUD horizontally by 1.778/1.942. Measured on the published
        // layer, not inferred: content 1226 px wide became 1123, a ratio of 0.916 against a
        // predicted 0.9155.
        //
        // Width stays the budget (it is what the readback costs); the height is whatever keeps the
        // pixels square.
        const int32_t lw_fixed = m_layer_w.load(std::memory_order_relaxed);
        const auto lh_fit = static_cast<int32_t>(
            (static_cast<int64_t>(lw_fixed) * height + width / 2) / width) & ~1;
        if (lh_fit > 0 && lh_fit != m_layer_h.load(std::memory_order_relaxed)) {
            m_layer_h.store(lh_fit, std::memory_order_relaxed);
            // The downscale surfaces are sized from the layer, so they have to go with it.
            if (auto* s = static_cast<IDirect3DSurface9*>(
                    m_scaled.exchange(nullptr, std::memory_order_acq_rel))) {
                s->Release();
            }
            if (auto* g = static_cast<IDirect3DSurface9*>(
                    m_stage.exchange(nullptr, std::memory_order_acq_rel))) {
                g->Release();
            }
            LOGX("[uicap] layer %dx%d to match a %dx%d source", lw_fixed, lh_fit, width, height);
        }
        LOGX("[uicap] created a %dx%d A8R8G8B8 UI target", width, height);
        return;
    }

    // End of the bracket: hand the engine's target back if a pass left us holding it.
    restore_target(dev);

    // The presented frame, sampled IN PHASE and only when asked. This is the check that catches
    // the mod costing the picture -- the failure it exists for looked perfectly healthy in every
    // counter.
    if (m_probe_at_end.exchange(false, std::memory_order_acq_rel)) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb != nullptr) {
            m_probe_lit.store(sample_lit(dev, bb), std::memory_order_relaxed);
            bb->Release();
        }
    }
}

bool UICapture::request_shot(const std::string& path, bool backbuffer) {
    if (path.empty() || m_shot_pending.load(std::memory_order_acquire)) {
        return false;
    }
    m_shot_path = path;
    m_shot_backbuffer.store(backbuffer, std::memory_order_release);
    m_shot_pending.store(true, std::memory_order_release);
    return true;
}

void UICapture::on_present() {
    // Teardown runs HERE, on the thread that owns the device. See on_shutdown().
    if (m_release_requested.load(std::memory_order_acquire)) {
        free_device_resources();
        m_released.store(true, std::memory_order_release);
        return;
    }

    auto* dev = sdk::Render::device();
    if (dev == nullptr) {
        return;
    }
    // See the header: holding a default-pool surface across a lost device makes the device
    // permanently unresettable.
    const HRESULT tcl = dev->TestCooperativeLevel();
    m_last_tcl.store(static_cast<uint32_t>(tcl), std::memory_order_relaxed);
    if (tcl != D3D_OK) {
        free_device_resources();
        m_device_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Live device geometry after any reshape -- what the buffer/RT/viewport ACTUALLY are.
    SceneTarget::get().trace_if_pending(dev);

    // ---- IS THE MENU EVEN BEING CAPTURED? --------------------------------------------------
    // "No quad at the initial main menu, but a working one after returning from a world" has two
    // very different explanations: the UI is never captured, or it is captured and never
    // published. A viewport trace cannot tell them apart, so report both counters -- once a
    // second, so a whole session stays readable.
    {
        static std::atomic<uint64_t> last_report{0};
        const auto now = static_cast<uint64_t>(GetTickCount64() / 1000);
        if (last_report.exchange(now, std::memory_order_relaxed) != now) {
            LOGX("[trace] ui: seen=%llu swaps=%llu publishes=%llu failures=%llu layer=%dx%d target=%dx%d",
                 static_cast<unsigned long long>(m_pass_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m_swaps.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m_publishes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m_failures.load(std::memory_order_relaxed)),
                 layer_width(), layer_height(),
                 m_width.load(std::memory_order_relaxed),
                 m_height.load(std::memory_order_relaxed));
        }
    }

    publish_layer(dev);

    if (!m_shot_pending.load(std::memory_order_acquire)) {
        return;
    }
    // ---- DOWN THE GPU FIRST, THEN READ BACK SMALL ----------------------------------------------
    //
    // This used to allocate a SYSTEMMEM surface the size of the capture target and read the whole
    // thing back. At a supersampled 4320x2224 that is 36.7 MB in a 32-bit process, and it FAILS --
    // the route answered shot_accepted:true, no file appeared, and `failures` ticked once per
    // attempt. The shot was silently unavailable exactly at the resolution worth inspecting.
    //
    // The publish path already solves this: scale on the GPU into m_scaled and read back the
    // layer-sized m_stage instead, ~3.4 MB. Reusing those two means the shot costs what a published
    // frame costs and works at any buffer size.
    //
    // `source=backbuffer` grabs the real post-HUD back buffer rather than our redirected target,
    // which is what distinguishes "our pass selection dropped elements" from "the HUD really is
    // drawn this way".
    const bool from_backbuffer = m_shot_backbuffer.load(std::memory_order_acquire);
    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* back = nullptr;
    if (from_backbuffer) {
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || back == nullptr) {
            m_shot_pending.store(false, std::memory_order_release);
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        src = back;
    } else {
        src = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));
    }
    auto* scaled = static_cast<IDirect3DSurface9*>(m_scaled.load(std::memory_order_relaxed));
    auto* sys = static_cast<IDirect3DSurface9*>(m_stage.load(std::memory_order_relaxed));
    const int32_t w = m_layer_w.load(std::memory_order_relaxed);
    const int32_t h = m_layer_h.load(std::memory_order_relaxed);
    if (src == nullptr || scaled == nullptr || sys == nullptr || w <= 0 || h <= 0) {
        if (back != nullptr) { back->Release(); }
        m_shot_pending.store(false, std::memory_order_release);
        return;
    }
    if (FAILED(dev->StretchRect(src, nullptr, scaled, nullptr, D3DTEXF_LINEAR))) {
        if (back != nullptr) { back->Release(); }
        m_shot_pending.store(false, std::memory_order_release);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (back != nullptr) {
        back->Release();
        back = nullptr;
    }
    if (SUCCEEDED(dev->GetRenderTargetData(scaled, sys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            auto* base = static_cast<uint8_t*>(lr.pBits);
            // ---- ALPHA IS RECONSTRUCTED, NOT READ ------------------------------------------
            //
            // The engine's UI shaders emit zero alpha. Measured, and not for want of trying: the
            // colour write mask was forced to RGBA for the whole bracket and read back as 0xF at
            // the end, so the mask is not the cause -- the Scaleform pixel shaders simply do not
            // write the channel (the shader set has explicit `NoAddAlpha` variants).
            //
            // It does not matter, because of how the surface is prepared: it is cleared to
            // TRANSPARENT BLACK and only the UI draws into it. So a pixel's RGB is already its
            // PREMULTIPLIED contribution, and its coverage is how bright it is. max(r,g,b) is
            // therefore an exact reconstruction for anything additive and a close one otherwise,
            // and it composites correctly as premultiplied alpha -- dst*(1-a) + rgb.
            //
            // The alternative, replacing the engine's shaders, would buy a more accurate alpha for
            // dark UI pixels at a cost this does not justify.
            uint64_t lit = 0;
            for (int32_t y = 0; y < h; ++y) {
                uint8_t* row = base + static_cast<size_t>(y) * lr.Pitch;
                for (int32_t x = 0; x < w; ++x) {
                    uint8_t* px = row + x * 4;  // BGRA
                    const uint8_t a = px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2])
                                                    : (px[1] > px[2] ? px[1] : px[2]);
                    px[3] = a;
                    if (a != 0) {
                        ++lit;
                    }
                }
            }
            const auto total = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
            m_alpha_coverage.store(total ? static_cast<int32_t>(lit * 1000u / total) : -1,
                                   std::memory_order_relaxed);
            write_bmp32(m_shot_path.c_str(), base, static_cast<uint32_t>(w),
                        static_cast<uint32_t>(h), static_cast<uint32_t>(lr.Pitch));
            sys->UnlockRect();
        }
    } else {
        m_failures.fetch_add(1, std::memory_order_relaxed);
    }
    // sys is m_stage, BORROWED from the publish path. Releasing it here would leave m_stage holding
    // a freed surface and fault on the next publish.
    m_shot_backbuffer.store(false, std::memory_order_release);
    m_shot_pending.store(false, std::memory_order_release);
}

void UICapture::on_shutdown() {
    m_enabled.store(false, std::memory_order_release);

    // RETIRE THE LAYER HERE, not on the next present -- there is no next present. The frame
    // callbacks are retired before this runs, so the usual "publish nothing once disabled" path
    // never executes on an uninject, and the host would go on showing the last HUD it was given:
    // a frozen ammo counter floating in the headset over a game that is no longer modded.
    //
    // Safe from here: it writes the shared header, which needs no device and no game thread.
    if (m_published.exchange(false, std::memory_order_acq_rel)) {
        FramePublisher::get().publish_ui(nullptr, 0, 0, 0, true, true, true);
    }

    // ---- THE DEVICE IS SINGLE-THREADED, AND THIS RUNS ON THE UNLOAD THREAD -------------------
    //
    // Releasing D3D surfaces from here HUNG THE GAME. The device is created with BehaviorFlags
    // 0x42 -- no D3DCREATE_MULTITHREADED -- so a Release from a thread that does not own it is
    // undefined, and what it did in practice was wedge the renderer mid-unload: the log stopped
    // after the previous mod's shutdown line and the process never came back.
    //
    // FrameCapture already had this exactly right and I did not copy it. Ask the render thread to
    // do it, wait a bounded time, and LEAK rather than hang if the game is not presenting -- a
    // stuck unload is worse than a leaked surface, and an unload happens when the payload is going
    // away anyway.
    m_released.store(false, std::memory_order_relaxed);
    m_release_requested.store(true, std::memory_order_release);
    for (int i = 0; i < 200 && !m_released.load(std::memory_order_acquire); ++i) {
        ::Sleep(5);
    }
    m_release_requested.store(false, std::memory_order_release);
    if (!m_released.load(std::memory_order_acquire)) {
        LOGX("[uicap] surfaces NOT released -- the game was not presenting; leaking rather than hanging");
    }
}

void UICapture::on_pass(uint32_t ordinal, uintptr_t caller) {
    // Record who issued each pass, for one frame at a time, so the boundary between the engine's
    // own full-screen work and the game's HUD can be READ rather than assumed to sit at a fixed
    // ordinal -- which is what broke when a scope overlay pushed the HUD later in the frame.
    if (ordinal < kMaxPassRecord) {
        m_pass_caller[ordinal] = caller;
        const uint32_t seen = ordinal + 1u;
        if (seen > m_pass_seen) {
            m_pass_seen = seen;
        }
    }

    // ---- TURN OURSELVES ON WHEN THE INTERFACE IS ACTUALLY DRAWING ---------------------------
    //
    // m_enabled defaulted false and only /render/ui?on=1 ever set it, so in ordinary use nothing
    // was captured and the quad stayed empty. Enabling at framework init instead crashed the game:
    //
    //     0xC0000005 read of 0x00000004 at GameClient.dll+0x9CFE3
    //
    // which decompiles to `switch (*(_DWORD *)(this[2] + 4))` -- a game-state query whose state
    // object is still null that early, so it reads address 4. Framework init is simply before the
    // interface exists.
    //
    // A pass ISSUED BY gameclient.dll proves it exists and is drawing, and the caller recording
    // above runs whether or not we are enabled -- so that can be observed from the off state. The
    // mod becomes self-timing instead of depending on a route nobody calls.
    if (!m_enabled.load(std::memory_order_relaxed)) {
        const auto* gc = sdk::Modules::get().game_client();
        if (gc != nullptr && gc->base != 0 && caller >= gc->base && caller < gc->base + gc->size) {
            m_enabled.store(true, std::memory_order_release);
            LOGX("[uicap] interface is drawing (pass from gameclient.dll) -- capture enabled");
        }
    }

    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    auto* dev = sdk::Render::device();
    if (dev == nullptr) {
        return;
    }

    // ---- WHO DREW IT, NOT WHEN --------------------------------------------------------------
    //
    // The first version keyed off the pass ORDINAL and it was wrong in play: aiming down sights and
    // entering a mech each add a full-screen effect, which pushes the HUD one pass later, and the
    // fixed index then captured the effect into the quad -- the whole frame appearing inside the
    // HUD layer. Measured, both layouts on the same build:
    //
    //     hip fire   0 gameclient | 1 ?         | 2..10 gameclient   (HUD from 2)
    //     ADS        0 gameclient | 1 ? | 2 ?   | 3..10 gameclient   (HUD from 3)
    //
    // The ordinal moves; the CALLER does not. The engine's full-screen work is issued from outside
    // gameclient.dll, the HUD from inside it, so that is the discriminator.
    const auto* gc = sdk::Modules::get().game_client();
    const bool from_game =
        gc != nullptr && gc->base != 0 && caller >= gc->base && caller < gc->base + gc->size;

    // ---- WHO IS ISSUING THESE, AND FROM WHERE ----------------------------------------------
    // "Installed hooks and swaps=0" has two explanations and they need different fixes: the pass
    // is never issued at all, or it IS issued and this classifier rejects every one of them.
    // Counting the passes we SEE and naming the caller's module separates them. Bounded to the
    // first handful so a session stays readable.
    {
        const auto n = m_pass_calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 12) {
            const auto* exe = sdk::Modules::get().exe();
            const char* where = "?";
            uintptr_t rel = caller;
            if (gc != nullptr && gc->base != 0 && caller >= gc->base && caller < gc->base + gc->size) {
                where = "gameclient.dll";
                rel = caller - gc->base;
            } else if (exe != nullptr && exe->base != 0 && caller >= exe->base &&
                       caller < exe->base + exe->size) {
                where = "FEAR2.exe";
                rel = caller - exe->base;
            }
            LOGX("[trace] pass #%llu ordinal=%u caller=0x%08X (%s+0x%IX) from_game=%d gc_loaded=%d",
                 static_cast<unsigned long long>(n), ordinal, static_cast<unsigned>(caller), where,
                 rel, from_game ? 1 : 0, (gc != nullptr && gc->base != 0) ? 1 : 0);
        }
    }

    if (!from_game) {
        // An engine full-screen pass. It belongs on the back buffer, so if we are currently
        // borrowing the target, give it back for the duration -- interleaving is handled rather
        // than assumed away, because "the effects are contiguous" is exactly the kind of thing that
        // holds until some weapon or vehicle proves it does not.
        //
        // TESTED AND REJECTED: treating exe-issued passes AFTER the boundary as HUD, on the theory
        // that Scaleform's type-1 draw comes from FEAR2.exe and was being handed back. It changed
        // the captured layer by nothing at all -- 1113x393, aspect 2.83, still clipped -- so pass
        // selection is not what drops the HUD, and the positional rule would only have captured
        // scope and vehicle effects for free.
        m_seen_fullscreen = true;
        restore_target(dev);
        return;
    }

    // A gameclient pass BEFORE any full-screen work is part of building the frame, not the HUD:
    // borrowing the target there blackened the presented image.
    if (!m_seen_fullscreen) {
        // ---- THE PRE-WORLD MENU IS DRAWN OVERSIZED AND THEN CLIPPED ---------------------------
        //
        // Measured at the initial menu, in one line:
        //
        //     viewport 2560x1440 at 0,0 | source surface 4320x2224 | bracket 2560x1440
        //
        // The interface scales itself to the RENDER TARGET, so with the buffer inflated for
        // supersampling it lays out across 4320x2224 -- the title sits on x=2160, which is that
        // buffer's centre, and the hint bar on 0.9 of its height. The viewport is still the native
        // 2560x1440, so everything right of 2560 is cut: the captured menu reads "MAIN ME" against
        // a hard vertical edge.
        //
        // This is the same fault HudScreenDims fixes for the in-world HUD, arriving by a different
        // route: that one rewrites ILTClient::GetScreenDims, and the menu never asks it -- it takes
        // the target's size instead. So the size cannot be corrected at the source here, and the
        // viewport is widened to match what is actually being drawn.
        //
        // Only on frames with no scene in them. There is nothing else in the pass to distort, and
        // in-world is left entirely alone.
        widen_viewport_to_target(dev);
        return;
    }

    swap_target(dev);
}

void UICapture::widen_viewport_to_target(IDirect3DDevice9* dev) {
    if (dev == nullptr) {
        return;
    }

    // The target's size is asked for once and kept: this runs on every interface pass, and there
    // are thousands of them per menu, so a COM round trip per pass would be paid forever to learn
    // a number that only changes when the device does.
    uint32_t w = m_target_w.load(std::memory_order_relaxed);
    uint32_t h = m_target_h.load(std::memory_order_relaxed);
    if (w == 0 || h == 0) {
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &rt)) || rt == nullptr) {
            return;
        }
        D3DSURFACE_DESC d{};
        const bool ok = SUCCEEDED(rt->GetDesc(&d));
        rt->Release();
        if (!ok || d.Width == 0 || d.Height == 0) {
            return;
        }
        w = d.Width;
        h = d.Height;
        m_target_w.store(w, std::memory_order_relaxed);
        m_target_h.store(h, std::memory_order_relaxed);
    }

    D3DVIEWPORT9 vp{};
    if (FAILED(dev->GetViewport(&vp))) {
        return;
    }
    if (vp.X == 0 && vp.Y == 0 && vp.Width == w && vp.Height == h) {
        return; // already correct -- do not spend a call saying so
    }

    const D3DVIEWPORT9 full{0, 0, w, h, vp.MinZ, vp.MaxZ};
    if (SUCCEEDED(dev->SetViewport(&full))) {
        const uint64_t n = m_viewport_widened.fetch_add(1, std::memory_order_relaxed);
        if (n == 0) {
            LOGX("[uicap] pre-world interface: viewport %ux%u -> %ux%u (it draws at target size)",
                 vp.Width, vp.Height, w, h);
        }
    }
}

void UICapture::publish_layer(IDirect3DDevice9* dev) {
    auto& pub = FramePublisher::get();

    // RETIRE ONCE, not every frame. A host left holding the last HUD forever is worse than one
    // holding none -- a frozen ammo counter looks live -- but re-announcing "gone" 90 times a
    // second would be pointless traffic through a sequence the host is polling.
    if (!m_enabled.load(std::memory_order_acquire) ||
        (m_surface.load(std::memory_order_relaxed) == nullptr && m_seen_fullscreen)) {
        if (m_published.exchange(false, std::memory_order_acq_rel)) {
            pub.publish_ui(nullptr, 0, 0, 0, true, true, true);
        }
        return;
    }

    // ---- A FRAME WITH NO SCENE IS ALREADY THE PICTURE WE WANT --------------------------------
    //
    // The pre-world menu and the opening movies issue only gameclient passes: no engine full-screen
    // pass runs, so on_pass's `m_seen_fullscreen` guard drops every one and nothing is ever swapped
    // into m_surface. Measured at the initial menu as swaps=0 while publishes climbed -- a layer
    // being published that nothing had drawn into.
    //
    // That guard is right in world, where borrowing the target before the scene is drawn blackens
    // the presented image. On a frame with no scene there is nothing to blacken, and the BACK
    // BUFFER already holds exactly what belongs on the quad, so take it directly and borrow
    // nothing.
    //
    // It is also why the menu works when reached FROM a world but not at startup: that case still
    // renders a scene behind the menu, so the flag is set and the normal path runs.
    IDirect3DSurface9* back_src = nullptr;
    if (!m_seen_fullscreen &&
        FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_src))) {
        back_src = nullptr;
    }
    struct BackRelease {
        IDirect3DSurface9* s;
        ~BackRelease() { if (s != nullptr) { s->Release(); } }
    } back_guard{back_src};

    auto* surf = back_src != nullptr
                     ? back_src
                     : static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));

    // DOWNSCALED ON THE GPU FIRST. The captured surface is the back buffer's size (2560x1440 here),
    // and a quad two metres away does not resolve that -- so the readback, which is the expensive
    // part and lands on the render thread, is paid at the layer's size instead of the frame's.
    const int32_t lw = m_layer_w.load(std::memory_order_relaxed);
    const int32_t lh = m_layer_h.load(std::memory_order_relaxed);
    if (lw <= 0 || lh <= 0) {
        return;
    }

    auto* scaled = static_cast<IDirect3DSurface9*>(m_scaled.load(std::memory_order_relaxed));
    auto* stage = static_cast<IDirect3DSurface9*>(m_stage.load(std::memory_order_relaxed));
    if (scaled == nullptr || stage == nullptr) {
        IDirect3DSurface9* s = nullptr;
        IDirect3DSurface9* g = nullptr;
        if (FAILED(dev->CreateRenderTarget(static_cast<UINT>(lw), static_cast<UINT>(lh),
                                           D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s,
                                           nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(static_cast<UINT>(lw), static_cast<UINT>(lh),
                                                    D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g,
                                                    nullptr))) {
            if (s != nullptr) { s->Release(); }
            if (g != nullptr) { g->Release(); }
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_scaled.store(s, std::memory_order_release);
        m_stage.store(g, std::memory_order_release);
        scaled = s;
        stage = g;
    }

    // ---- CROP TO WHAT THE GAME ACTUALLY DREW --------------------------------------------------
    //
    // We inflate the back buffer and leave the WINDOW alone, so before a world the movies and the
    // menu are drawn at the engine's own size into the top-left corner of a much larger buffer --
    // confirmed by capturing it: a tiny picture on a field of black. Publishing the whole surface
    // put that same postage stamp on the quad.
    //
    // So take only the region the game drew. StretchRect scales it up to the layer, which is what
    // the quad wanted all along. In world this is skipped entirely: the scene target IS the buffer
    // and the full surface is correct.
    RECT src_rect{};
    const RECT* src_rect_p = nullptr;
    if (back_src != nullptr) {
        // The BRACKET's size, not SceneTarget's saved screen dims. With the OEP gate in place
        // init_render_detour has not run by the time the menu draws, so the saved dims are still 0
        // -- measured, and it silently disabled the crop. m_width/m_height are what the engine told
        // us its render target is when the bracket opened, which is precisely the region it draws.
        const auto ow = static_cast<uint32_t>(m_width.load(std::memory_order_relaxed));
        const auto oh = static_cast<uint32_t>(m_height.load(std::memory_order_relaxed));
        D3DSURFACE_DESC bd{};
        if (ow != 0 && oh != 0 && SUCCEEDED(back_src->GetDesc(&bd)) && ow <= bd.Width &&
            oh <= bd.Height && (ow != bd.Width || oh != bd.Height)) {
            src_rect = {0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
            src_rect_p = &src_rect;
            static std::atomic<uint32_t> said{0};
            if (said.fetch_add(1, std::memory_order_relaxed) < 2) {
                {
            // WHAT THE ENGINE IS ACTUALLY ALLOWED TO DRAW INTO. The bracket size is our own
            // bookkeeping; the viewport and the real surface are the engine's, and the published
            // picture showed a hard clip that matched neither. Log all three once.
            static std::atomic<bool> s_once{false};
            if (!s_once.exchange(true, std::memory_order_relaxed)) {
                D3DVIEWPORT9 vp{};
                D3DSURFACE_DESC bd{};
                const bool vp_ok = SUCCEEDED(dev->GetViewport(&vp));
                const bool bd_ok = (surf != nullptr) && SUCCEEDED(surf->GetDesc(&bd));
                LOGX("[uicap] viewport %ux%u at %u,%u (ok=%d) | source surface %ux%u (ok=%d) | bracket %dx%d",
                     vp.Width, vp.Height, vp.X, vp.Y, (int)vp_ok, bd.Width, bd.Height, (int)bd_ok,
                     (int)m_width.load(std::memory_order_relaxed),
                     (int)m_height.load(std::memory_order_relaxed));
            }
        }
        LOGX("[uicap] UI-only frame: cropping %ux%u out of the %ux%u buffer", ow, oh,
                     bd.Width, bd.Height);
            }
        }
    }

    if (FAILED(dev->StretchRect(surf, src_rect_p, scaled, nullptr, D3DTEXF_LINEAR)) ||
        FAILED(dev->GetRenderTargetData(scaled, stage))) {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Alpha is DERIVED BY THE HOST (see xr::UiFrameHeader::derive_alpha): it is already copying
    // every pixel, and doing it here would put a megapixel pass inside the game's frame.
    const bool ok = pub.publish_ui(lr.pBits, static_cast<uint32_t>(lr.Pitch),
                                   static_cast<uint32_t>(lw), static_cast<uint32_t>(lh), true, true,
                                   /*derive_alpha=*/true);
    stage->UnlockRect();
    if (ok) {
        m_published.store(true, std::memory_order_release);
        m_publishes.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void UICapture::swap_target(IDirect3DDevice9* dev) {
    if (m_saved.load(std::memory_order_relaxed) != nullptr) {
        return;  // already borrowed
    }
    auto* surf = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));
    if (surf == nullptr) {
        return;
    }
    IDirect3DSurface9* current = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &current)) || current == nullptr) {
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    D3DVIEWPORT9 vp{};
    m_have_saved_viewport.store(SUCCEEDED(dev->GetViewport(&vp)), std::memory_order_relaxed);
    m_saved_viewport = vp;
    if (FAILED(dev->SetRenderTarget(0, surf))) {
        current->Release();
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_saved.store(current, std::memory_order_release);

    // ---- WHAT VIEWPORT DID THE RE-BIND LEAVE? -------------------------------------------------
    //
    // SetRenderTarget resets the D3D9 viewport to the new target's full extent, discarding whatever
    // the engine had set for this pass. If Scaleform's geometry was built against a different
    // viewport than the one it ends up drawing through, the content lands at the wrong size --
    // which is the open question, since the matrix it submits is already known to be correct.
    {
        static std::atomic<uint32_t> s_vp_logged{0};
        if (s_vp_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
            D3DVIEWPORT9 after{};
            const bool got = SUCCEEDED(dev->GetViewport(&after));
            LOGX("[uicap] rebind viewport before=(%lu,%lu %lux%lu) after=(%lu,%lu %lux%lu) got=%d "
                 "target=%dx%d",
                 m_saved_viewport.X, m_saved_viewport.Y, m_saved_viewport.Width,
                 m_saved_viewport.Height, after.X, after.Y, after.Width, after.Height,
                 got ? 1 : 0, m_width.load(std::memory_order_relaxed),
                 m_height.load(std::memory_order_relaxed));
        }
    }

    // CLEARED ONCE PER FRAME, not once per swap. The target is handed back for every engine
    // full-screen pass and taken again afterwards, so clearing on each swap would wipe the HUD
    // elements drawn before the interruption.
    if (!m_cleared_this_frame) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        m_cleared_this_frame = true;
    }
    m_swaps.fetch_add(1, std::memory_order_relaxed);
}

void UICapture::restore_target(IDirect3DDevice9* dev) {
    auto* saved = static_cast<IDirect3DSurface9*>(m_saved.exchange(nullptr, std::memory_order_acq_rel));
    if (saved == nullptr) {
        return;
    }
    if (SUCCEEDED(dev->SetRenderTarget(0, saved))) {
        m_restores.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_failures.fetch_add(1, std::memory_order_relaxed);
    }
    saved->Release();
    if (m_have_saved_viewport.exchange(false, std::memory_order_acq_rel)) {
        dev->SetViewport(&m_saved_viewport);
    }
}

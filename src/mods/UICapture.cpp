#include "UICapture.hpp"

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

bool UICapture::request_shot(const std::string& path) {
    if (path.empty() || m_shot_pending.load(std::memory_order_acquire)) {
        return false;
    }
    m_shot_path = path;
    m_shot_pending.store(true, std::memory_order_release);
    return true;
}

void UICapture::on_present() {
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

    publish_layer(dev);

    if (!m_shot_pending.load(std::memory_order_acquire)) {
        return;
    }
    auto* surf = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));
    const int32_t w = m_width.load(std::memory_order_relaxed);
    const int32_t h = m_height.load(std::memory_order_relaxed);
    if (surf == nullptr || w <= 0 || h <= 0) {
        m_shot_pending.store(false, std::memory_order_release);
        return;
    }

    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(static_cast<UINT>(w), static_cast<UINT>(h),
                                                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        sys == nullptr) {
        m_shot_pending.store(false, std::memory_order_release);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (SUCCEEDED(dev->GetRenderTargetData(surf, sys))) {
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
    sys->Release();
    m_shot_pending.store(false, std::memory_order_release);
}

void UICapture::on_shutdown() {
    m_enabled.store(false, std::memory_order_release);
    free_device_resources();
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

    if (!from_game) {
        // An engine full-screen pass. It belongs on the back buffer, so if we are currently
        // borrowing the target, give it back for the duration -- interleaving is handled rather
        // than assumed away, because "the effects are contiguous" is exactly the kind of thing that
        // holds until some weapon or vehicle proves it does not.
        m_seen_fullscreen = true;
        restore_target(dev);
        return;
    }

    // A gameclient pass BEFORE any full-screen work is part of building the frame, not the HUD:
    // borrowing the target there blackened the presented image.
    if (!m_seen_fullscreen) {
        return;
    }

    swap_target(dev);
}

void UICapture::publish_layer(IDirect3DDevice9* dev) {
    auto& pub = FramePublisher::get();

    // RETIRE ONCE, not every frame. A host left holding the last HUD forever is worse than one
    // holding none -- a frozen ammo counter looks live -- but re-announcing "gone" 90 times a
    // second would be pointless traffic through a sequence the host is polling.
    if (!m_enabled.load(std::memory_order_acquire) ||
        m_surface.load(std::memory_order_relaxed) == nullptr) {
        if (m_published.exchange(false, std::memory_order_acq_rel)) {
            pub.publish_ui(nullptr, 0, 0, 0, true, true, true);
        }
        return;
    }

    auto* surf = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));

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

    if (FAILED(dev->StretchRect(surf, nullptr, scaled, nullptr, D3DTEXF_LINEAR)) ||
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

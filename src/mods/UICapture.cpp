#include "UICapture.hpp"

#include <d3d9.h>

#include <cstdio>

#include "Log.hpp"
#include "RenderHook.hpp"
#include "FramePublisher.hpp"
#include "HudPassHook.hpp"
#include "RenderTimeline.hpp"
#include "sdk/Render.hpp"

namespace {

void bracket_cb(bool begin, int32_t width, int32_t height, uint32_t index) {
    UICapture::get().on_bracket(begin, width, height, index);
}

void pass_cb(uint32_t ordinal) {
    UICapture::get().on_pass(ordinal);
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

    // In per-pass mode the bracket only CREATES the surface and performs the restore; the swap
    // itself happens later, at the chosen pass.
    const bool per_pass = m_swap_from_pass.load(std::memory_order_relaxed) >= 0;

    if (begin) {
        // Recreate on a size change as well as on absence: a resolution change leaves a surface
        // the engine's viewport no longer matches, and binding it would silently letterbox.
        auto* surf = static_cast<IDirect3DSurface9*>(m_surface.load(std::memory_order_relaxed));
        if (surf == nullptr || m_width.load(std::memory_order_relaxed) != width ||
            m_height.load(std::memory_order_relaxed) != height) {
            free_device_resources();
            IDirect3DSurface9* created = nullptr;
            // A8R8G8B8 with no multisampling and no lockable flag: the read back path is
            // GetRenderTargetData into SYSTEMMEM, which does not need a lockable target.
            if (FAILED(dev->CreateRenderTarget(static_cast<UINT>(width), static_cast<UINT>(height),
                                               D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &created, nullptr)) ||
                created == nullptr) {
                m_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_surface.store(created, std::memory_order_release);
            m_width.store(width, std::memory_order_relaxed);
            m_height.store(height, std::memory_order_relaxed);
            surf = created;
            LOGX("[uicap] created a %dx%d A8R8G8B8 UI target", width, height);
        }

        // THE VIEWPORT, WHICH SetRenderTarget SILENTLY RESETS. D3D9 sets the viewport to the full
        // surface on every target bind, so binding ours and putting the engine's back leaves the
        // viewport at full size even though the engine had configured its own. Measured, and it is
        // not subtle: with the swap in place the final frame came out ENTIRELY BLACK while our
        // surface held a perfect HUD, the bracket structure was unchanged and no call failed --
        // the engine's own composite after the bracket was drawing through a viewport we had
        // clobbered. Saved here and restored after the target goes back.
        D3DVIEWPORT9 vp{};
        m_have_saved_viewport.store(SUCCEEDED(dev->GetViewport(&vp)), std::memory_order_relaxed);
        m_saved_viewport = vp;

        IDirect3DSurface9* current = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &current)) || current == nullptr) {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // MODE 4 IS THE CONTROL: rebind the surface that is ALREADY bound. If the frame still comes
        // out black then the damage is done by the unbind/rebind cycle itself -- a swap chain with
        // D3DSWAPEFFECT_DISCARD does not guarantee back-buffer contents survive one -- rather than
        // by where the HUD's draws went.
        // ---- PRESERVE THE BACK BUFFER ACROSS THE SWAP ------------------------------------
        //
        // Binding a DIFFERENT surface discards what the back buffer held. Measured with a control:
        // rebinding the SAME surface leaves the frame intact (100% non-black) while binding ours
        // blackens it completely, and our surface receives only the HUD either way -- so nothing is
        // being redirected, the contents are simply gone. That is legal: a swap chain created
        // D3DSWAPEFFECT_DISCARD makes no promise about back-buffer contents once it is unbound.
        //
        // The headset does not care -- its frame is captured at the second eye, before this bracket
        // -- but the desktop window shows the back buffer, and a black window is not something to
        // ship. So the scene is copied out before the swap and put back after it: two full-screen
        // GPU blits, which is the cost of borrowing the target the engine is presenting.
        if (m_preserve.load(std::memory_order_relaxed)) {
            if (m_scratch.load(std::memory_order_relaxed) == nullptr) {
                IDirect3DSurface9* scratch = nullptr;
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(current->GetDesc(&sd)) &&
                    SUCCEEDED(dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format,
                                                      D3DMULTISAMPLE_NONE, 0, FALSE, &scratch, nullptr))) {
                    m_scratch.store(scratch, std::memory_order_release);
                }
            }
            if (auto* scratch = static_cast<IDirect3DSurface9*>(m_scratch.load(std::memory_order_relaxed))) {
                if (FAILED(dev->StretchRect(current, nullptr, scratch, nullptr, D3DTEXF_NONE))) {
                    m_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (per_pass) {
            current->Release();   // the surface exists now; the swap waits for its pass
            return;
        }

        IDirect3DSurface9* bind_target = m_mode.load(std::memory_order_relaxed) == 4 ? current : surf;
        if (FAILED(dev->SetRenderTarget(0, bind_target))) {
            current->Release();
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // IS THE UI BRACKET'S TARGET THE SWAP CHAIN'S BACK BUFFER? The answer decides what the
        // engine does after the bracket, and it is one call to find out rather than an argument.
        if (IDirect3DSurface9* bb = nullptr;
            SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb != nullptr) {
            m_target_is_backbuffer.store(bb == current, std::memory_order_relaxed);
            m_backbuffer_ptr.store(reinterpret_cast<uintptr_t>(bb), std::memory_order_relaxed);
            bb->Release();
        }
        m_displaced_ptr.store(reinterpret_cast<uintptr_t>(current), std::memory_order_relaxed);

        if (m_probe_at_begin.exchange(false, std::memory_order_acq_rel)) {
            m_probe_begin_lit.store(sample_lit(dev, current), std::memory_order_relaxed);
        }

        m_saved.store(current, std::memory_order_release);

        // ALPHA WRITES, WHICH THE ENGINE HAS NO REASON TO LEAVE ON. Drawing to a back buffer whose
        // alpha nobody reads, it masks the channel off -- measured: the isolated HUD came back with
        // correct RGB on 3.9% of pixels and alpha 0 everywhere. A composition layer needs that
        // channel, so it is forced for the bracket and put back at the end.
        DWORD cwe = 0;
        if (m_mode.load(std::memory_order_relaxed) >= 3 &&
            SUCCEEDED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cwe))) {
            m_saved_colorwrite.store(static_cast<uint32_t>(cwe), std::memory_order_relaxed);
            m_have_saved_colorwrite.store(true, std::memory_order_relaxed);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        }
        // TRANSPARENT, not black: the whole point is that everything the UI does not cover stays
        // see-through when it is composited as a layer.
        if (m_mode.load(std::memory_order_relaxed) >= 2) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        }
        m_swaps.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // End of the bracket: put the engine's surface back before it tears the target down.
    // WHAT THE ENGINE LEFT IT AS. If this reads back without the alpha bit, the mask is being set
    // per pass or per draw and a bracket-level force cannot win -- which decides whether alpha is
    // obtainable at all or has to be reconstructed.
    if (DWORD end_cwe = 0; SUCCEEDED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &end_cwe))) {
        m_colorwrite_at_end.store(static_cast<uint32_t>(end_cwe), std::memory_order_relaxed);
    }
    if (m_have_saved_colorwrite.exchange(false, std::memory_order_acq_rel)) {
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, m_saved_colorwrite.load(std::memory_order_relaxed));
    }
    if (auto* saved = static_cast<IDirect3DSurface9*>(m_saved.exchange(nullptr, std::memory_order_acq_rel))) {
        if (SUCCEEDED(dev->SetRenderTarget(0, saved))) {
            m_restores.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_failures.fetch_add(1, std::memory_order_relaxed);
        }
        saved->Release();
        // AFTER the target is back, because the bind is what resets it.
        if (m_have_saved_viewport.exchange(false, std::memory_order_acq_rel)) {
            dev->SetViewport(&m_saved_viewport);
        }
        // And put the scene back into the target we borrowed.
        if (m_preserve.load(std::memory_order_relaxed)) {
            // MODE 5 sources the blit from our UI surface instead of the scratch. It has KNOWN
            // content, so if the back buffer stays black with it too, the destination is refusing
            // the blit rather than the scratch being empty.
            void* src = m_mode.load(std::memory_order_relaxed) == 5
                            ? m_surface.load(std::memory_order_relaxed)
                            : m_scratch.load(std::memory_order_relaxed);
            if (auto* from = static_cast<IDirect3DSurface9*>(src)) {
                if (FAILED(dev->StretchRect(from, nullptr, saved, nullptr, D3DTEXF_NONE))) {
                    m_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // IN PHASE, AT BOTH ENDS. Which end the back buffer is black at is the whole question:
        // black at BEGIN means the scene arrives during this bracket, black only at END means
        // unbinding lost it. One probe answers what a dozen arguments could not.
        if (m_probe_at_end.exchange(false, std::memory_order_acq_rel)) {
            m_probe_lit.store(sample_lit(dev, saved), std::memory_order_relaxed);
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

void UICapture::on_pass(uint32_t ordinal) {
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    // ---- SWAP LATE, NOT AT THE BRACKET ---------------------------------------------------
    //
    // The bracket's first passes composite the SCENE into the back buffer; only the later ones
    // draw the HUD. Taking the whole bracket therefore costs the presented frame, so the target is
    // borrowed from `swap_from_pass` onward and the composite is left to run where it belongs.
    //
    // Which pass that is, is a measurement, not a constant -- sweep it and watch two numbers: the
    // back buffer must stay lit and our surface must still hold the whole HUD.
    if (ordinal != static_cast<uint32_t>(m_swap_from_pass.load(std::memory_order_relaxed))) {
        return;
    }
    auto* dev = sdk::Render::device();
    if (dev == nullptr || m_saved.load(std::memory_order_relaxed) != nullptr) {
        return;  // already swapped this frame
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
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    m_swaps.fetch_add(1, std::memory_order_relaxed);
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

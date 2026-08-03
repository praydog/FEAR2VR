#include "FrameCapture.hpp"

#include <windows.h>

#include <d3d9.h>

#include <cstdio>

#include "Log.hpp"
#include <array>

#include "sdk/PlayerMgr.hpp"

#include "CameraPassHook.hpp"
#include "CameraPassHook.hpp"
#include "VR.hpp"
#include "FramePublisher.hpp"
#include "RenderHook.hpp"
#include "sdk/Render.hpp"

namespace {

int64_t now_ticks() {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double ticks_to_ms(int64_t ticks) {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    if (freq.QuadPart == 0) {
        return 0.0;
    }
    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(freq.QuadPart);
}

// A 32-bit bottom-up BMP. Deliberately not a PNG: no dependency, no compression to get wrong, and
// anything can open it. The engine's back buffer is X8R8G8B8 or A8R8G8B8, both of which are already
// BGRA in memory, so the rows copy out verbatim.
bool write_bmp(const char* path, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t pitch) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        return false;
    }
    const uint32_t row_bytes = w * 4;
    const uint32_t image_bytes = row_bytes * h;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + image_bytes;
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);  // positive: bottom-up, so rows are written in reverse
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image_bytes;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (uint32_t y = 0; y < h; ++y) {
        fwrite(pixels + static_cast<size_t>(h - 1 - y) * pitch, row_bytes, 1, f);
    }
    fclose(f);
    return true;
}

} // namespace

FrameCapture& FrameCapture::get() {
    static FrameCapture instance;
    return instance;
}

std::optional<std::string> FrameCapture::on_initialize() {
    return std::nullopt;
}

void FrameCapture::on_present() {
    // Teardown runs HERE, on the thread that owns the device, because the device is single-threaded
    // (BehaviorFlags 0x42, measured). See on_shutdown() for what this is preventing.
    if (FrameCapture::get().m_release_requested.load(std::memory_order_acquire)) {
        FrameCapture::get().release_surfaces();
        FrameCapture::get().m_released.store(true, std::memory_order_release);
        return;
    }

    // ---- DEVICE LOSS, WHICH THIS MOD CAN CAUSE TO BE PERMANENT -----------------------------
    //
    // Our mirror, scaled and pipe surfaces are CreateRenderTarget, which is implicitly
    // D3DPOOL_DEFAULT -- and D3D9 refuses to Reset a device while ANY default-pool resource is
    // alive. So an ordinary alt-tab, which loses the device and makes the engine reset it, turns
    // into a device that can never be reset again: every subsequent GetBackBuffer returns
    // D3DERR_DEVICELOST and the game renders nothing this mod can read for the rest of the session.
    // Observed exactly that way, with the window visible and frames still counting.
    //
    // Holding these is therefore not merely wasteful, it is destructive, and the fix is to let go
    // the moment the device stops being OK. They are all created lazily, so recovery is automatic.
    if (auto* dev = sdk::Render::device(); dev != nullptr) {
        if (dev->TestCooperativeLevel() != D3D_OK) {
            FrameCapture::get().free_device_resources();
            FrameCapture::get().m_device_lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // The same drop, on demand, so the rebuild path can be tested without alt-tabbing.
    if (FrameCapture::get().m_drop_requested.exchange(false, std::memory_order_acq_rel)) {
        FrameCapture::get().free_device_resources();
    }

    // A capture aimed at an earlier stage is serviced there instead; servicing here too would read
    // the finished frame and quietly answer a different question than the one that was asked. That
    // now includes the PIPELINED path, which feeds the headset.
    if (FrameCapture::get().stage() == FrameCapture::Stage::Present) {
        FrameCapture::get().service_continuous();
        FrameCapture::get().service();
        return;
    }

    // ---- THE MENU, WHICH HAS NO SCENE TO HANG A CAPTURE ON ---------------------------------
    //
    // Every other stage is serviced from inside a scene hook: AfterSecondEye runs in
    // CLTRenderer's DrawScene detour. The MAIN MENU never draws a scene, so that stage simply
    // never fires and the headset gets nothing at all -- not a black world, no frames whatsoever,
    // which is exactly how the front end came to be invisible in VR while everything else worked.
    //
    // So when the configured stage has not produced a frame for a while, publish from here
    // instead. The back buffer at present IS the menu, and it is mono -- there is no stereo split
    // without a scene -- so the layout tag comes out kLayoutMono and the host shows it to both
    // eyes, which is what a flat front end should look like.
    //
    // Threshold rather than a world flag: this asks the question that matters ("is anything
    // producing frames") instead of a proxy for it, and it recovers on its own the moment the
    // scene starts drawing again.
    auto& fc = FrameCapture::get();
    const uint64_t idle = fc.m_presents_since_service.fetch_add(1, std::memory_order_relaxed);
    if (idle >= kMenuFallbackPresents) {
        fc.service_continuous();
        fc.service();
        fc.m_menu_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

void FrameCapture::service_now() {
    // A scene-driven capture just happened, so the fallback above stands down.
    m_presents_since_service.store(0, std::memory_order_relaxed);
    // THE PIPELINE RUNS AT WHATEVER STAGE CALLS THIS, which for stereo is the only correct place.
    // The published frame used to come from the present hook, and present DESTROYS the right half
    // of a split-stereo frame -- measured: the right eye is correct immediately after it draws and
    // is tiled garbage by the time it presents. A mono screen never noticed; a stereo pair would
    // have shown one good eye and one wrong one, which reads as a stereo bug rather than a staging
    // one.
    service_continuous();
    service_mirror();
    service();
}

void FrameCapture::set_gpu_mirror(bool enabled) {
    if (enabled && !m_registered.load(std::memory_order_relaxed)) {
        if (RenderHook::get().add_present_callback(&FrameCapture::on_present)) {
            m_registered.store(true, std::memory_order_relaxed);
        }
    }
    m_mirror_on.store(enabled, std::memory_order_release);
    LOGX("[capture] gpu mirror %s", enabled ? "ON" : "off");
}

double FrameCapture::last_gpu_copy_ms() const {
    return ticks_to_ms(m_mirror_copy_ticks.load(std::memory_order_relaxed));
}

double FrameCapture::mirror_left_luma() const {
    return static_cast<double>(m_mirror_left_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::mirror_right_luma() const {
    return static_cast<double>(m_mirror_right_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::mirror_ref_left_luma() const {
    return static_cast<double>(m_mirror_ref_left_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::mirror_ref_right_luma() const {
    return static_cast<double>(m_mirror_ref_right_milli.load(std::memory_order_relaxed)) / 1000.0;
}

// GPU -> GPU. No lock, no system memory, no stall: this is the copy a compositor submission makes.
void FrameCapture::service_mirror() {
    if (!m_mirror_on.load(std::memory_order_acquire)) {
        return;
    }

    auto* device = sdk::Render::device();

    if (device == nullptr) {
        return;
    }

    IDirect3DSurface9* back = nullptr;

    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || back == nullptr) {
        return;
    }

    D3DSURFACE_DESC desc{};

    if (FAILED(back->GetDesc(&desc))) {
        back->Release();
        return;
    }

    if (m_mirror == nullptr || m_mirror_w != desc.Width || m_mirror_h != desc.Height) {
        if (m_mirror != nullptr) {
            static_cast<IDirect3DSurface9*>(m_mirror)->Release();
            m_mirror = nullptr;
        }
        IDirect3DSurface9* rt = nullptr;
        // DEFAULT pool, lockable=FALSE: a submission surface is never read by the CPU, and asking
        // for lockable would put it somewhere slower for no benefit. verify_gpu_mirror() copies it
        // to its own staging surface instead.
        if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                 D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
            rt != nullptr) {
            m_mirror = rt;
            m_mirror_w = desc.Width;
            m_mirror_h = desc.Height;
            m_mirror_fmt = static_cast<uint32_t>(desc.Format);
        } else {
            back->Release();
            return;
        }
    }

    const int64_t t0 = now_ticks();
    const HRESULT hr = device->StretchRect(back, nullptr, static_cast<IDirect3DSurface9*>(m_mirror),
                                           nullptr, D3DTEXF_NONE);
    m_mirror_copy_ticks.store(now_ticks() - t0, std::memory_order_relaxed);
    back->Release();

    if (SUCCEEDED(hr)) {
        m_mirror_frames.fetch_add(1, std::memory_order_relaxed);
    }

    // A verification asked for from the IPC thread is performed HERE, where the device lives.
    if (m_mirror_verify.exchange(false, std::memory_order_acq_rel)) {
        m_mirror_verified.store(verify_gpu_mirror(), std::memory_order_relaxed);
    }
}

bool FrameCapture::verify_gpu_mirror() {
    if (m_mirror == nullptr) {
        return false;
    }

    auto* device = sdk::Render::device();

    if (device == nullptr) {
        return false;
    }

    IDirect3DSurface9* sys = nullptr;

    // THE MIRROR'S OWN FORMAT. Hardcoding X8R8G8B8 made GetRenderTargetData fail silently and the
    // luminances read a very convincing 0.000.
    if (FAILED(device->CreateOffscreenPlainSurface(m_mirror_w, m_mirror_h,
                                                   static_cast<D3DFORMAT>(m_mirror_fmt),
                                                   D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        sys == nullptr) {
        return false;
    }

    bool ok = false;

    // Sample the MIRROR, then the BACK BUFFER, through the same staging surface in the same call --
    // one instant, one grid, so the comparison is an identity rather than a tolerance.
    IDirect3DSurface9* back = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);

    if (SUCCEEDED(device->GetRenderTargetData(static_cast<IDirect3DSurface9*>(m_mirror), sys))) {
        D3DLOCKED_RECT lr{};

        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            const auto* base = static_cast<const uint8_t*>(lr.pBits);
            uint64_t left = 0, right = 0, nl = 0, nr = 0;
            const uint32_t mid = m_mirror_w / 2;

            // The SAME grid the direct path samples, so the two are comparable by construction.
            for (uint32_t y = 0; y < m_mirror_h; y += 8) {
                const auto* row =
                    reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * lr.Pitch);
                for (uint32_t x = 0; x < m_mirror_w; x += 8) {
                    const uint32_t px = row[x] & 0x00FFFFFFu;
                    const uint64_t l =
                        ((px >> 16 & 0xFF) * 77 + (px >> 8 & 0xFF) * 150 + (px & 0xFF) * 29) >> 8;
                    if (x < mid) {
                        left += l;
                        ++nl;
                    } else {
                        right += l;
                        ++nr;
                    }
                }
            }
            m_mirror_left_milli.store(nl == 0 ? 0 : static_cast<int64_t>((left * 1000ull) / nl),
                                      std::memory_order_relaxed);
            m_mirror_right_milli.store(nr == 0 ? 0 : static_cast<int64_t>((right * 1000ull) / nr),
                                       std::memory_order_relaxed);
            sys->UnlockRect();
            ok = true;
        }
    }

    if (ok && back != nullptr && SUCCEEDED(device->GetRenderTargetData(back, sys))) {
        D3DLOCKED_RECT lr{};

        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            const auto* base = static_cast<const uint8_t*>(lr.pBits);
            uint64_t left = 0, right = 0, nl = 0, nr = 0;
            const uint32_t mid = m_mirror_w / 2;

            for (uint32_t y = 0; y < m_mirror_h; y += 8) {
                const auto* row =
                    reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * lr.Pitch);
                for (uint32_t x = 0; x < m_mirror_w; x += 8) {
                    const uint32_t px = row[x] & 0x00FFFFFFu;
                    const uint64_t l =
                        ((px >> 16 & 0xFF) * 77 + (px >> 8 & 0xFF) * 150 + (px & 0xFF) * 29) >> 8;
                    if (x < mid) {
                        left += l;
                        ++nl;
                    } else {
                        right += l;
                        ++nr;
                    }
                }
            }
            m_mirror_ref_left_milli.store(nl == 0 ? 0 : static_cast<int64_t>((left * 1000ull) / nl),
                                          std::memory_order_relaxed);
            m_mirror_ref_right_milli.store(
                nr == 0 ? 0 : static_cast<int64_t>((right * 1000ull) / nr),
                std::memory_order_relaxed);
            sys->UnlockRect();
        }
    }

    if (back != nullptr) {
        back->Release();
    }

    sys->Release();
    return ok;
}

bool FrameCapture::request_capture() {
    return request_capture_to(std::string{});
}

bool FrameCapture::request_capture_to(const std::string& path) {
    if (!m_registered.load(std::memory_order_relaxed)) {
        // Register lazily: RenderHook installs its own hook only once a device exists, and a
        // callback added before that is simply never called.
        if (!RenderHook::get().add_present_callback(&FrameCapture::on_present)) {
            return false;
        }
        m_registered.store(true, std::memory_order_relaxed);
    }
    bool expected = false;
    if (!m_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;  // one already in flight
    }
    m_path = path;
    return true;
}

double FrameCapture::last_copy_ms() const {
    return ticks_to_ms(m_copy_ticks.load(std::memory_order_relaxed));
}

double FrameCapture::worst_copy_ms() const {
    return ticks_to_ms(m_worst_ticks.load(std::memory_order_relaxed));
}

double FrameCapture::last_total_ms() const {
    return ticks_to_ms(m_total_ticks.load(std::memory_order_relaxed));
}

double FrameCapture::last_lock_ms() const {
    return ticks_to_ms(m_lock_ticks.load(std::memory_order_relaxed));
}

double FrameCapture::continuous_lock_ms() const {
    return ticks_to_ms(m_cont_lock_ticks.load(std::memory_order_relaxed));
}

void FrameCapture::set_publishing(bool enabled) {
    if (enabled) {
        if (!FramePublisher::get().open()) {
            LOGX("[capture] cannot publish: %s", FramePublisher::get().last_error().c_str());
            return;
        }

        // A publisher with no feed is pointless, so this turns the pipelined readback on rather
        // than quietly doing nothing until someone else does.
        set_continuous(true);
    }

    m_publishing.store(enabled, std::memory_order_release);
    LOGX("[capture] publishing to the 64-bit host %s", enabled ? "ON" : "off");

    if (!enabled) {
        FramePublisher::get().close();
    }
}

void FrameCapture::set_continuous(bool enabled) {
    if (enabled && !m_registered.load(std::memory_order_relaxed)) {
        if (RenderHook::get().add_present_callback(&FrameCapture::on_present)) {
            m_registered.store(true, std::memory_order_relaxed);
        }
    }
    if (!enabled) {
        // Priming state is per-session: leaving it set would make the first frame after
        // re-enabling lock a surface nothing had issued into.
        m_pipe_primed = false;
    }
    m_continuous.store(enabled, std::memory_order_release);

    // RELEASING GIVES THE SLOT BACK, when nothing else still needs it. This is deliberately the
    // same primitive the shutdown path depends on: exercising it on every release means the
    // teardown's correctness is covered by ordinary use, instead of resting on a code path that
    // runs twice per suite and never in normal play.
    if (!enabled && !m_pending.load(std::memory_order_acquire) &&
        m_registered.load(std::memory_order_relaxed)) {
        if (RenderHook::get().remove_present_callback(&FrameCapture::on_present)) {
            m_registered.store(false, std::memory_order_relaxed);
        }
    }
    LOGX("[capture] continuous readback %s", enabled ? "ON (pipelined)" : "off");
}

void FrameCapture::service_continuous() {
    if (!m_continuous.load(std::memory_order_acquire)) {
        return;
    }
    auto* device = sdk::Render::device();
    if (device == nullptr) {
        return;
    }
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || back == nullptr) {
        return;
    }
    D3DSURFACE_DESC desc{};
    if (FAILED(back->GetDesc(&desc))) {
        back->Release();
        return;
    }
    const uint32_t div = m_divisor.load(std::memory_order_relaxed);
    const uint32_t w = desc.Width / (div < 1 ? 1 : div);
    const uint32_t h = desc.Height / (div < 1 ? 1 : div);

    if (m_pipe[0] == nullptr || m_pipe_w != w || m_pipe_h != h) {
        for (auto& p : m_pipe) {
            if (p != nullptr) {
                static_cast<IDirect3DSurface9*>(p)->Release();
                p = nullptr;
            }
        }
        bool ok = true;
        for (auto& p : m_pipe) {
            IDirect3DSurface9* sys = nullptr;
            if (FAILED(device->CreateOffscreenPlainSurface(w, h, desc.Format, D3DPOOL_SYSTEMMEM,
                                                           &sys, nullptr)) ||
                sys == nullptr) {
                ok = false;
                break;
            }
            p = sys;
        }
        m_pipe_w = w;
        m_pipe_h = h;
        m_pipe_primed = false;
        if (!ok) {
            back->Release();
            return;
        }
    }

    // Downscale first when asked, exactly as the one-shot path does.
    IDirect3DSurface9* source = back;
    if (div > 1) {
        if (m_scaled == nullptr || m_scaled_w != w || m_scaled_h != h) {
            if (m_scaled != nullptr) {
                static_cast<IDirect3DSurface9*>(m_scaled)->Release();
                m_scaled = nullptr;
            }
            IDirect3DSurface9* rt = nullptr;
            if (SUCCEEDED(device->CreateRenderTarget(w, h, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &rt, nullptr)) &&
                rt != nullptr) {
                m_scaled = rt;
                m_scaled_w = w;
                m_scaled_h = h;
            }
        }
        if (m_scaled != nullptr &&
            SUCCEEDED(device->StretchRect(back, nullptr, static_cast<IDirect3DSurface9*>(m_scaled),
                                          nullptr, D3DTEXF_LINEAR))) {
            source = static_cast<IDirect3DSurface9*>(m_scaled);
        }
    }

    // ISSUE this frame's readback, then LOCK the one issued last frame. The GPU has had a whole
    // frame to complete it, so the lock should not stall -- which is the entire point, and the
    // number continuous_lock_ms() reports.
    const uint32_t issue = m_issue;
    const uint32_t ready = issue ^ 1u;
    // ---- THE ONE READBACK WHOSE RESULT WE NEVER CHECKED ----------------------------------------
    //
    // Every other GetRenderTargetData in this file tests its HRESULT -- one of them carries a
    // comment about it "failing silently" -- and this, the call that fills the frame we actually
    // publish, threw it away.
    //
    // A failure does not clear the destination: the slot keeps the PREVIOUS frame's pixels. We then
    // publish that slot stamped with the CURRENT pose. A stale image wearing a fresh pose, which is
    // indistinguishable from a pose/frame mis-association and is exactly what "it judders to a
    // stale frame" looks like.
    const HRESULT grtd = device->GetRenderTargetData(source, static_cast<IDirect3DSurface9*>(m_pipe[issue]));
    m_pipe_ok[issue] = SUCCEEDED(grtd);
    if (!m_pipe_ok[issue]) {
        m_readback_failures.fetch_add(1, std::memory_order_relaxed);
        m_last_readback_hr.store(static_cast<int32_t>(grtd), std::memory_order_relaxed);
    }

    // Recorded HERE, against the surface being issued, because this is the moment whose pixels it
    // captures. Reading it later -- when the surface is finally locked -- would attribute a frame
    // to whatever pose had arrived in the meantime.
    const uint32_t stamped = VR::get().last_host_sequence();
    // ---- IS THE STAMP TELLING THE TRUTH? -------------------------------------------------------
    // Measured rather than argued: the view matrix was built from CameraPassHook::last_view_seq()
    // at the top of this same DrawScene, on this thread. If the engine thread has ingested a newer
    // pose since, `stamped` names a pose this image was NOT rendered from.
    const uint32_t view_seq = CameraPassHook::last_view_seq();
    m_stamp_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
    if (view_seq == stamped) {
        m_stamp_agree.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_stamp_drift.fetch_add(1, std::memory_order_relaxed);
        const uint32_t ahead = stamped - view_seq;  // sequences step by 2 (seqlock)
        uint32_t worst = m_stamp_worst.load(std::memory_order_relaxed);
        while (ahead > worst &&
               !m_stamp_worst.compare_exchange_weak(worst, ahead, std::memory_order_relaxed)) {
        }
    }
    // ---- IS THE ROTATION WE STAMP THE ROTATION THAT WAS DRAWN? ---------------------------------
    //
    // The sequence check above only proves we named the right pose RECORD. It cannot see the game
    // adding rotation of its own -- a scripted camera, a lean, recoil, a mounted view -- after our
    // head pose went in. When that happens the compositor timewarps using a rotation the image was
    // never rendered with, and the error is proportional to head speed.
    //
    // Compared as PER-FRAME MAGNITUDES, which needs no agreement about handedness or order between
    // the engine's quaternion and OpenXR's: if the camera is faithfully following the head, the two
    // turn by the same ANGLE each frame whatever basis each is expressed in. A divergence is the
    // game rotating the view by itself.
    {
        float cam[4];
        CameraPassHook::camera_rotation_now(cam);
        const auto* host = FramePublisher::get().host_state();
        if (host != nullptr) {
            const float hq[4] = {host->orientation[0], host->orientation[1], host->orientation[2],
                                 host->orientation[3]};
            auto angle_between = [](const float a[4], const float b[4]) {
                float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                d = d < 0.0f ? -d : d;
                if (d > 1.0f) {
                    d = 1.0f;
                }
                return 2.0f * acosf(d) * 57.2957795f;  // degrees
            };
            if (m_rot_primed) {
                const float d_cam = angle_between(m_prev_cam, cam);
                const float d_host = angle_between(m_prev_host, hq);
                // Only while the head is actually turning: at rest both are noise, and a ratio of
                // two noises says nothing.
                if (d_host > 0.05f) {
                    const float miss = d_cam > d_host ? d_cam - d_host : d_host - d_cam;
                    // ---- IS IT A LAG RATHER THAN AN ERROR? ---------------------------------
                    // A camera that follows the head ONE FRAME LATE turns by the same total
                    // amount -- so the means agree and only the per-frame deltas disagree,
                    // which is a phase signature and not a scale one. Comparing this frame's
                    // camera delta against the PREVIOUS head delta separates them: if that
                    // matches better, the picture is a frame behind the pose stamped on it,
                    // and timewarp is correcting by a difference we introduced.
                    const float miss_lag =
                        d_cam > m_prev_d_host ? d_cam - m_prev_d_host : m_prev_d_host - d_cam;
                    m_rot_sum_lag.store(m_rot_sum_lag.load(std::memory_order_relaxed) + miss_lag,
                                        std::memory_order_relaxed);
                    m_prev_d_host = d_host;
                    m_rot_samples.fetch_add(1, std::memory_order_relaxed);
                    m_rot_sum_cam.store(m_rot_sum_cam.load(std::memory_order_relaxed) + d_cam,
                                        std::memory_order_relaxed);
                    m_rot_sum_host.store(m_rot_sum_host.load(std::memory_order_relaxed) + d_host,
                                         std::memory_order_relaxed);
                    m_rot_sum_miss.store(m_rot_sum_miss.load(std::memory_order_relaxed) + miss,
                                         std::memory_order_relaxed);
                    float worst = m_rot_worst.load(std::memory_order_relaxed);
                    while (miss > worst && !m_rot_worst.compare_exchange_weak(
                                               worst, miss, std::memory_order_relaxed)) {
                    }

                    // ---- AXIS, NOT JUST ANGLE -------------------------------------------
                    //
                    // Everything above compares MAGNITUDES, and a rotation of the same size
                    // about a DIFFERENT AXIS passes it as a perfect zero. That is not a
                    // hypothetical: rotating the view with the stick is smooth while rotating
                    // the head judders, and the only thing that distinguishes them is that the
                    // head changes the pose handed to timewarp. If the game applied our head
                    // motion about a wrong axis on some frames the picture would still turn by
                    // the right amount, this check would read 0.000, and the warp would be
                    // wrong exactly when the head moves.
                    //
                    // So compare the DELTAS AS ROTATIONS. The head delta is converted into the
                    // engine's basis first -- the conversion is conjugation by diag(1,1,-1),
                    // verified to be a homomorphism, so a delta may be converted directly.
                    const auto to_eng = VR::runtime_to_engine_rotation(
                        {hq[0], hq[1], hq[2], hq[3]});
                    const auto prev_eng = VR::runtime_to_engine_rotation(
                        {m_prev_host[0], m_prev_host[1], m_prev_host[2], m_prev_host[3]});
                    auto mul = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
                        return std::array<float, 4>{
                            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
                    };
                    // ---- CONJUGATE BY THE HEADING, WHICH IS HOW IT IS APPLIED --------
                    //
                    // The head is NOT written into the camera raw. HeadTracking writes
                    // `heading * head * heading^-1` as the outer operand, deliberately, so the
                    // head turns in the BODY's frame rather than the world's -- there is a
                    // measurement in VR.cpp showing what happens otherwise (pitching 20 degrees
                    // at a 26.86 degree heading gave 17.851, and 20*cos(26.86) is 17.84).
                    //
                    // So the camera's delta is the head's delta CONJUGATED BY THE HEADING, and
                    // comparing it against the raw head delta measures that conjugation: same
                    // magnitude, rotated axis, scaling with the head motion. Which is exactly
                    // the "axis error" this check has been reporting -- my own instrument
                    // omitting a term the engine path applies on purpose.
                    std::array<float, 4> head_delta =
                        mul(to_eng, {-prev_eng[0], -prev_eng[1], -prev_eng[2], prev_eng[3]});
                    if (const auto yaw = sdk::PlayerMgr::aim_yaw(0)) {
                        const float h = *yaw * 0.5f;
                        const std::array<float, 4> hq_head{0.0f, sinf(h), 0.0f, cosf(h)};
                        const std::array<float, 4> hq_inv{0.0f, -sinf(h), 0.0f, cosf(h)};
                        head_delta = mul(mul(hq_head, head_delta), hq_inv);
                    }
                    const std::array<float, 4> cam_delta =
                        mul({cam[0], cam[1], cam[2], cam[3]},
                            {-m_prev_cam[0], -m_prev_cam[1], -m_prev_cam[2], m_prev_cam[3]});
                    const float axis_err =
                        angle_between(cam_delta.data(), head_delta.data());

                    m_rot_sum_axis.store(m_rot_sum_axis.load(std::memory_order_relaxed) + axis_err,
                                         std::memory_order_relaxed);
                    float aworst = m_rot_axis_worst.load(std::memory_order_relaxed);
                    while (axis_err > aworst &&
                           !m_rot_axis_worst.compare_exchange_weak(aworst, axis_err,
                                                                   std::memory_order_relaxed)) {
                    }
                    if (axis_err > 0.5f) {
                        m_rot_axis_bad.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            memcpy(m_prev_cam, cam, sizeof(cam));
            memcpy(m_prev_host, hq, sizeof(hq));
            m_rot_primed = true;
        }
    }

    // ---- STATE THE POSE, DO NOT JUST NAME IT ---------------------------------------------------
    //
    // The sequence is an INDEX and the host resolves it against its own record of what it SENT. That
    // says nothing about what the engine finally DREW: the conversion into its basis, the
    // conjugation into the body's frame, its own outer*inner composition and its pitch clamp all
    // sit in between, and any of them altering the rotation leaves the compositor warping from a
    // pose this image was never rendered with.
    //
    // `outer` here is read from the engine's own holder, so whatever it did to our write is in it.
    // Undo the two transforms we applied and the result is the head pose this frame actually
    // corresponds to, in the runtime's space.
    //
    //     outer     = heading * head_eng * heading^-1
    //     head_eng  = heading^-1 * outer * heading
    //     head_xr   = phi(head_eng)          -- phi is its own inverse
    //
    // Travels with the SLOT, like the sequence, because the readback is a frame deep.
    m_pipe_pose_ok[issue] = false;
    if (const auto ops = sdk::PlayerMgr::camera_rotation_operands(0)) {
        if (const auto yaw = sdk::PlayerMgr::aim_yaw(0)) {
            const float h = *yaw * 0.5f;
            const std::array<float, 4> hq{0.0f, sinf(h), 0.0f, cosf(h)};
            const std::array<float, 4> hqi{0.0f, -sinf(h), 0.0f, cosf(h)};
            auto qm = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
                return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
            };
            const auto head_eng = qm(qm(hqi, ops->outer), hq);
            const auto head_xr = VR::runtime_to_engine_rotation(head_eng);  // involution
            const float n = head_xr[0] * head_xr[0] + head_xr[1] * head_xr[1] +
                            head_xr[2] * head_xr[2] + head_xr[3] * head_xr[3];
            if (n > 0.9f && n < 1.1f) {  // a quaternion that is not unit length is not a rotation
                for (size_t k = 0; k < 4; ++k) {
                    m_pipe_pose[issue][k] = head_xr[k];
                }
                m_pipe_pose_ok[issue] = true;
            }
        }
    }

    m_pipe_seq[issue] = stamped;
    back->Release();

    if (m_pipe_primed) {
        D3DLOCKED_RECT lr{};
        const int64_t l0 = now_ticks();
        const HRESULT hr = static_cast<IDirect3DSurface9*>(m_pipe[ready])
                               ->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        m_cont_lock_ticks.store(now_ticks() - l0, std::memory_order_relaxed);
        if (SUCCEEDED(hr)) {
            // The frame is in system memory and nothing else is going to touch it -- publish it
            // BEFORE unlocking, because after UnlockRect the pointer is not ours to read.
            //
            // desc.Format is the back buffer's, and every D3D9 back buffer format this engine uses
            // (X8R8G8B8 / A8R8G8B8) is BGRA in memory. Told to the reader rather than assumed by it,
            // because a wrong guess swaps red and blue and looks like a grading bug.
            // NEVER PUBLISH A SLOT WHOSE READBACK FAILED. Holding the previous frame is honest --
            // the host counts it as `held` and the compositor reprojects it -- whereas publishing
            // stale pixels under a fresh pose is a lie the whole pipeline then acts on.
            if (m_publishing.load(std::memory_order_acquire) && m_pipe_ok[ready]) {
                // Tell the reader what it is looking at rather than letting it guess. Split stereo
                // puts both eyes in one frame side by side; anything else is a single image for
                // both eyes.
                //
                // THE SWITCHES ARE NOT THE FRAME. `cam.stereo && cam.split_viewport` say the split
                // is ARMED, not that this image got one -- the split is applied to a scene pass, and
                // the main menu draws no scene. Publishing a mono menu tagged side-by-side makes the
                // host halve it and give one half to each eye, which is exactly how the front end
                // came out with a different piece of itself in either eye. So the tag follows the
                // FRAME: one published by the no-scene fallback is mono, whatever the switches say.
                // ---- IS THIS THE SAME PICTURE AS LAST TIME? ---------------------------------
                //
                // Theory-free. Every counter in this pipeline can read clean while the PIXELS
                // repeat: the sequence is honest, the association is exact, the transport is in
                // order, and the image is still the one shown a moment ago. A duplicate image
                // published under a NEW pose is what "it judders to a stale frame" is, and it is
                // the one thing never actually looked at -- the pixels themselves.
                //
                // Sparse on purpose: a few hundred taps spread across the frame cost nothing next
                // to the megabyte already being copied, and two different frames of a moving scene
                // will not agree on all of them.
                {
                    const auto* px = static_cast<const uint8_t*>(lr.pBits);
                    uint64_t hash = 1469598103934665603ull;
                    for (uint32_t y = 0; y < h; y += 37) {
                        const auto* row = px + static_cast<size_t>(y) * lr.Pitch;
                        for (uint32_t x = 0; x < w; x += 53) {
                            hash ^= static_cast<uint64_t>(row[static_cast<size_t>(x) * 4u]);
                            hash *= 1099511628211ull;
                        }
                    }
                    const uint32_t seq_now = m_pipe_seq[ready];
                    if (m_last_hash != 0) {
                        if (hash == m_last_hash) {
                            m_dup_frames.fetch_add(1, std::memory_order_relaxed);
                            if (seq_now != m_last_hash_seq) {
                                // The picture repeated but the POSE moved on. The compositor is
                                // being handed an old image wearing a new pose.
                                m_dup_moved.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                    m_last_hash = hash;
                    m_last_hash_seq = seq_now;
                }

                const auto cam = CameraPassHook::get().observed();
                const bool really_split = cam.stereo && cam.split_viewport &&
                                          m_presents_since_service.load(std::memory_order_relaxed) <
                                              kMenuFallbackPresents;
                const uint32_t layout = really_split ? xr::kLayoutSideBySide : xr::kLayoutMono;
                FramePublisher::get().publish(lr.pBits, static_cast<uint32_t>(lr.Pitch), w, h, true,
                                              layout, m_pipe_seq[ready],
                                              m_pipe_pose_ok[ready] ? m_pipe_pose[ready] : nullptr);
            }

            static_cast<IDirect3DSurface9*>(m_pipe[ready])->UnlockRect();
            m_cont_frames.fetch_add(1, std::memory_order_relaxed);
            m_width.store(w, std::memory_order_relaxed);
            m_height.store(h, std::memory_order_relaxed);
        }
    }
    m_pipe_primed = true;
    m_issue = ready;
}

double FrameCapture::last_left_luma() const {
    return static_cast<double>(m_left_luma_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::last_right_luma() const {
    return static_cast<double>(m_right_luma_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::last_mean_luma() const {
    return static_cast<double>(m_mean_luma_milli.load(std::memory_order_relaxed)) / 1000.0;
}

double FrameCapture::last_stretch_ms() const {
    return ticks_to_ms(m_stretch_ticks.load(std::memory_order_relaxed));
}

void FrameCapture::set_divisor(uint32_t divisor) {
    // Clamped rather than refused: a caller asking for an absurd reduction wants the
    // smallest useful frame, and 16 already takes 2560x1440 to 160x90.
    m_divisor.store(divisor < 1 ? 1 : (divisor > 16 ? 16 : divisor), std::memory_order_relaxed);
}

void FrameCapture::service() {
    if (!m_pending.load(std::memory_order_acquire)) {
        return;
    }

    const int64_t t0 = now_ticks();
    auto* device = sdk::Render::device();
    if (device == nullptr) {
        m_hr.store(-1, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        m_pending.store(false, std::memory_order_release);
        return;
    }

    IDirect3DSurface9* back = nullptr;
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr) || back == nullptr) {
        // WHICH THREAD asked matters more than the HRESULT: this device is single-threaded
        // (BehaviorFlags 0x42), so a call from anywhere but the render thread can fail for reasons
        // that have nothing to do with the swap chain.
        LOGX("[capture] GetBackBuffer hr=0x%08X on tid %lu (render tid %lu)",
             static_cast<unsigned>(hr), ::GetCurrentThreadId(), sdk::Render::render_thread_id());
        m_hr.store(hr, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        m_pending.store(false, std::memory_order_release);
        return;
    }

    D3DSURFACE_DESC desc{};
    hr = back->GetDesc(&desc);
    if (FAILED(hr)) {
        back->Release();
        m_hr.store(hr, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        m_pending.store(false, std::memory_order_release);
        return;
    }

    // ---- OPTIONAL GPU DOWNSCALE -----------------------------------------------------
    //
    // With a divisor set, the readback source becomes a smaller render target rather than the
    // back buffer. StretchRect does the filtering on the GPU, so what follows measures the
    // readback of a realistic eye-sized surface instead of the desktop.
    IDirect3DSurface9* source = back;
    const uint32_t div = m_divisor.load(std::memory_order_relaxed);
    m_stretch_ticks.store(0, std::memory_order_relaxed);
    if (div > 1) {
        const uint32_t sw = desc.Width / div;
        const uint32_t sh = desc.Height / div;
        if (m_scaled == nullptr || m_scaled_w != sw || m_scaled_h != sh) {
            if (m_scaled != nullptr) {
                static_cast<IDirect3DSurface9*>(m_scaled)->Release();
                m_scaled = nullptr;
            }
            IDirect3DSurface9* rt = nullptr;
            // A RENDER TARGET, not an offscreen plain surface: StretchRect from a render target
            // requires the destination to be one too.
            hr = device->CreateRenderTarget(sw, sh, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt,
                                            nullptr);
            if (SUCCEEDED(hr) && rt != nullptr) {
                m_scaled = rt;
                m_scaled_w = sw;
                m_scaled_h = sh;
            }
        }
        if (m_scaled != nullptr) {
            const int64_t s0 = now_ticks();
            hr = device->StretchRect(back, nullptr, static_cast<IDirect3DSurface9*>(m_scaled),
                                     nullptr, D3DTEXF_LINEAR);
            m_stretch_ticks.store(now_ticks() - s0, std::memory_order_relaxed);
            if (SUCCEEDED(hr)) {
                source = static_cast<IDirect3DSurface9*>(m_scaled);
                desc.Width = sw;
                desc.Height = sh;
            }
        }
    }

    // Reuse the staging surface. Creating one per capture would make the measurement an
    // allocation benchmark rather than a copy benchmark, and the copy is the number that decides
    // whether this path can hold a frame budget.
    if (m_staging == nullptr || m_staging_w != desc.Width || m_staging_h != desc.Height ||
        m_staging_fmt != static_cast<uint32_t>(desc.Format)) {
        if (m_staging != nullptr) {
            static_cast<IDirect3DSurface9*>(m_staging)->Release();
            m_staging = nullptr;
        }
        IDirect3DSurface9* sys = nullptr;
        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                 D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (FAILED(hr) || sys == nullptr) {
            back->Release();
            m_hr.store(hr, std::memory_order_relaxed);
            m_failures.fetch_add(1, std::memory_order_relaxed);
            m_pending.store(false, std::memory_order_release);
            return;
        }
        m_staging = sys;
        m_staging_w = desc.Width;
        m_staging_h = desc.Height;
        m_staging_fmt = static_cast<uint32_t>(desc.Format);
    }

    auto* staging = static_cast<IDirect3DSurface9*>(m_staging);

    // THE MEASUREMENT. GetRenderTargetData is a GPU->CPU readback and a synchronisation point;
    // everything either side of it is bookkeeping.
    const int64_t c0 = now_ticks();
    hr = device->GetRenderTargetData(source, staging);
    const int64_t c1 = now_ticks();
    back->Release();

    if (FAILED(hr)) {
        m_hr.store(hr, std::memory_order_relaxed);
        m_failures.fetch_add(1, std::memory_order_relaxed);
        m_pending.store(false, std::memory_order_release);
        return;
    }

    m_copy_ticks.store(c1 - c0, std::memory_order_relaxed);
    int64_t worst = m_worst_ticks.load(std::memory_order_relaxed);
    while ((c1 - c0) > worst &&
           !m_worst_ticks.compare_exchange_weak(worst, c1 - c0, std::memory_order_relaxed)) {
    }
    m_width.store(desc.Width, std::memory_order_relaxed);
    m_height.store(desc.Height, std::memory_order_relaxed);
    m_format.store(static_cast<uint32_t>(desc.Format), std::memory_order_relaxed);

    D3DLOCKED_RECT lr{};
    const int64_t l0 = now_ticks();
    hr = staging->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    m_lock_ticks.store(now_ticks() - l0, std::memory_order_relaxed);
    if (SUCCEEDED(hr)) {
        const auto* base = static_cast<const uint8_t*>(lr.pBits);

        // CONTENT, not just success. A capture that reads back a black frame is exactly what the
        // stale desktop grabs looked like, so the oracle has to be able to say "there are pixels
        // here" independently of the copy having returned S_OK. Sampled on a grid rather than
        // every pixel: this runs on the render thread and 3.7M pixels is not bookkeeping.
        uint64_t nonblack = 0;
        uint64_t sampled = 0;
        // The same walk also builds the SIGNATURE and the mean luminance -- see last_signature().
        // Position-sensitive on purpose: a plain sum of pixels would be equal for two frames whose
        // content merely moved, which is precisely the difference a stereo check must detect.
        uint64_t sig = 1469598103934665603ull;  // FNV-1a offset basis
        uint64_t luma_sum = 0;
        uint64_t luma_left = 0, luma_right = 0;
        uint64_t n_left = 0, n_right = 0;
        const uint32_t mid_x = desc.Width / 2;
        for (uint32_t y = 0; y < desc.Height; y += 8) {
            const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * lr.Pitch);
            for (uint32_t x = 0; x < desc.Width; x += 8) {
                ++sampled;
                const uint32_t px = row[x] & 0x00FFFFFFu;
                if (px != 0) {
                    ++nonblack;
                }
                sig = (sig ^ px) * 1099511628211ull;
                // Rec. 601 weights in integer form; the exact curve does not matter, only that two
                // different pictures give two different numbers.
                const uint64_t luma =
                    ((px >> 16 & 0xFF) * 77 + (px >> 8 & 0xFF) * 150 + (px & 0xFF) * 29) >> 8;
                luma_sum += luma;
                if (x < mid_x) {
                    luma_left += luma;
                    ++n_left;
                } else {
                    luma_right += luma;
                    ++n_right;
                }
            }
        }
        m_nonblack.store(nonblack, std::memory_order_relaxed);
        m_sampled.store(sampled, std::memory_order_relaxed);
        m_signature.store(sig, std::memory_order_relaxed);
        m_mean_luma_milli.store(sampled == 0 ? 0
                                             : static_cast<int64_t>((luma_sum * 1000ull) / sampled),
                                std::memory_order_relaxed);
        m_left_luma_milli.store(n_left == 0 ? 0 : static_cast<int64_t>((luma_left * 1000ull) / n_left),
                                std::memory_order_relaxed);
        m_right_luma_milli.store(
            n_right == 0 ? 0 : static_cast<int64_t>((luma_right * 1000ull) / n_right),
            std::memory_order_relaxed);

        if (!m_path.empty()) {
            write_bmp(m_path.c_str(), base, desc.Width, desc.Height,
                      static_cast<uint32_t>(lr.Pitch));
        }
        staging->UnlockRect();
    }

    m_hr.store(hr, std::memory_order_relaxed);
    m_captures.fetch_add(1, std::memory_order_relaxed);
    m_total_ticks.store(now_ticks() - t0, std::memory_order_relaxed);
    m_pending.store(false, std::memory_order_release);
}

bool FrameCapture::request_resource_drop() {
    if (!m_registered.load(std::memory_order_relaxed)) {
        return false;
    }

    m_drop_requested.store(true, std::memory_order_release);
    return true;
}

void FrameCapture::free_device_resources() {
    if (m_staging != nullptr) {
        static_cast<IDirect3DSurface9*>(m_staging)->Release();
        m_staging = nullptr;
    }
    if (m_scaled != nullptr) {
        static_cast<IDirect3DSurface9*>(m_scaled)->Release();
        m_scaled = nullptr;
    }
    for (auto& p : m_pipe) {
        if (p != nullptr) {
            static_cast<IDirect3DSurface9*>(p)->Release();
            p = nullptr;
        }
    }
    if (m_mirror != nullptr) {
        static_cast<IDirect3DSurface9*>(m_mirror)->Release();
        m_mirror = nullptr;
    }

    // The pipeline has to re-prime: its surfaces are gone, and reading one that nothing has issued
    // into would hand back whatever the driver left in fresh memory.
    m_pipe_primed = false;
}

void FrameCapture::release_surfaces() {
    // Shutdown: drop the intent as well, so nothing rebuilds behind us.
    m_mirror_on.store(false, std::memory_order_release);
    m_publishing.store(false, std::memory_order_release);
    FramePublisher::get().close();
    free_device_resources();
}

void FrameCapture::on_shutdown() {
    // ---- FREE ON THE THREAD THAT OWNS THE DEVICE -----------------------------------------------
    //
    // Two separate hazards, and an earlier version fixed only the first.
    //
    // ONE: a callback that may still be executing must not have its surfaces freed underneath it.
    // remove_present_callback() returns only once the slot is empty AND no dispatch pass is still
    // running, which is the actual precondition -- a flag is not synchronisation.
    //
    // TWO, AND THIS ONE KILLED THE GAME: the device is SINGLE-THREADED. Measured live through
    // GetCreationParameters, BehaviorFlags is 0x42 -- HARDWARE_VERTEXPROCESSING | FPU_PRESERVE,
    // with no D3DCREATE_MULTITHREADED. on_shutdown runs on the UNLOAD thread, so releasing device
    // children here races the renderer inside the display driver even when no callback of ours is
    // running. It showed up as an access violation on an nvd3dum.dll worker thread immediately
    // after "unload requested; retiring", with no frame of ours anywhere on the stack.
    //
    // So the release is HANDED to the render thread while the present callback is still installed,
    // and this thread waits for it. Deregistering first would remove the only thread allowed to do
    // the work.
    m_pending.store(false, std::memory_order_release);
    m_continuous.store(false, std::memory_order_release);

    if (!m_registered.load(std::memory_order_relaxed)) {
        // Never registered means no other thread has ever touched these, so this thread is the
        // only one that could -- and it is safe to do it here.
        release_surfaces();
        return;
    }

    m_released.store(false, std::memory_order_relaxed);
    m_release_requested.store(true, std::memory_order_release);

    // Bounded: a game that is not presenting -- minimised, paused, already dying -- would otherwise
    // hang the unload forever, and a stuck unload is worse than a leak.
    for (int i = 0; i < 200 && !m_released.load(std::memory_order_acquire); ++i) {
        ::Sleep(5);
    }

    m_release_requested.store(false, std::memory_order_release);

    if (!m_released.load(std::memory_order_acquire)) {
        // Fail-closed, the same doctrine as the callback rule: leaking is survivable, releasing
        // off-thread is not. The leak lasts until the game exits, which is a price worth paying.
        LOGX("[capture] no frame serviced the release -- leaking surfaces deliberately");
    }

    if (!RenderHook::get().remove_present_callback(&FrameCapture::on_present)) {
        LOGX("[capture] present callback still in flight -- leaving the slot installed");
        return;
    }

    m_registered.store(false, std::memory_order_relaxed);
}

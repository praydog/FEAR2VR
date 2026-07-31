#include "FrameCapture.hpp"

#include <windows.h>

#include <d3d9.h>

#include <cstdio>

#include "Log.hpp"
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
    FrameCapture::get().service_continuous();
    FrameCapture::get().service();
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
    device->GetRenderTargetData(source, static_cast<IDirect3DSurface9*>(m_pipe[issue]));
    back->Release();

    if (m_pipe_primed) {
        D3DLOCKED_RECT lr{};
        const int64_t l0 = now_ticks();
        const HRESULT hr = static_cast<IDirect3DSurface9*>(m_pipe[ready])
                               ->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        m_cont_lock_ticks.store(now_ticks() - l0, std::memory_order_relaxed);
        if (SUCCEEDED(hr)) {
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

void FrameCapture::on_shutdown() {
    // DEREGISTER FIRST, THEN FREE. The previous version cleared flags and released -- and released
    // m_scaled BEFORE clearing m_continuous, so the render thread could still be inside
    // service_continuous() using a surface that had just gone away. A flag is not synchronisation:
    // the callback can already be past the check when the release lands.
    //
    // remove_present_callback() returns only once the slot is empty AND no dispatch pass is still
    // running, which is the actual precondition for freeing what the callback touches.
    m_pending.store(false, std::memory_order_release);
    m_continuous.store(false, std::memory_order_release);

    if (m_registered.load(std::memory_order_relaxed)) {
        if (!RenderHook::get().remove_present_callback(&FrameCapture::on_present)) {
            // Fail-closed, matching the framework's rule: a callback that may still be executing
            // means the surfaces must NOT be released. Leaking them is survivable; a use-after-free
            // on the render thread is not.
            LOGX("[capture] present callback still in flight -- leaking surfaces deliberately");
            return;
        }
        m_registered.store(false, std::memory_order_relaxed);
    }

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
}

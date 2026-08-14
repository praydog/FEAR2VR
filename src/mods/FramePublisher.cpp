#include "FramePublisher.hpp"

#include <windows.h>

#include <cstring>

#include "Log.hpp"

namespace {

int64_t qpc_frequency() {
    static const int64_t f = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart != 0 ? v.QuadPart : 1;
    }();
    return f;
}

int64_t now_ticks() {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t tick_freq() {
    static int64_t freq = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return freq;
}

double ticks_to_ms(int64_t t) {
    const int64_t f = tick_freq();
    return f == 0 ? 0.0 : (static_cast<double>(t) * 1000.0) / static_cast<double>(f);
}

}  // namespace

FramePublisher& FramePublisher::get() {
    static FramePublisher instance;
    return instance;
}

bool FramePublisher::open() {
    if (m_control_base != nullptr) {
        return true;
    }

    m_error.clear();

    const uint32_t total =
        xr::kSharedFrameTotalBytes;  // headers + frame slots + UI slots, named in the contract

    HANDLE mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total,
                                          xr::kSharedFrameName);

    if (mapping == nullptr) {
        m_error = "CreateFileMapping failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }

    // THE CONTROL BLOCK -- header, HostState, HandsState, HapticsState, UiFrameHeader -- is small
    // and always needed, so it gets its own view at file offset 0 (trivially aligned) covering the
    // first kPayloadOffset bytes.
    void* control_base = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, xr::kPayloadOffset);

    if (control_base == nullptr) {
        m_error = "MapViewOfFile(control) failed (" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(mapping);
        return false;
    }

    // Every frame and UI slot gets its OWN view too, at its own aligned offset -- see the class
    // comment on m_control_base for why. Collected locally first and only published to the member
    // arrays once every single one has succeeded: a publisher missing one slot would write through
    // a null pointer the first time the writer's round-robin reached it, which is worse than simply
    // staying closed and leaving the caller to retry open() on a later frame.
    void* frame_base[xr::kFrameSlots]{};
    void* ui_base[xr::kUiSlots]{};
    bool all_mapped = true;

    for (uint32_t i = 0; all_mapped && i < xr::kFrameSlots; ++i) {
        frame_base[i] = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, xr::slot_offset(i),
                                        xr::kSharedFrameMaxBytes);
        if (frame_base[i] == nullptr) {
            m_error = "MapViewOfFile(frame slot " + std::to_string(i) + ") failed (" +
                      std::to_string(::GetLastError()) + ")";
            all_mapped = false;
        }
    }

    for (uint32_t i = 0; all_mapped && i < xr::kUiSlots; ++i) {
        ui_base[i] = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, xr::ui_slot_offset(i),
                                     xr::kUiMaxBytes);
        if (ui_base[i] == nullptr) {
            m_error = "MapViewOfFile(ui slot " + std::to_string(i) + ") failed (" +
                      std::to_string(::GetLastError()) + ")";
            all_mapped = false;
        }
    }

    if (!all_mapped) {
        LOGX("[publish] %s -- leaving the publisher inactive", m_error.c_str());
        for (uint32_t i = 0; i < xr::kFrameSlots; ++i) {
            if (frame_base[i] != nullptr) {
                ::UnmapViewOfFile(frame_base[i]);
            }
        }
        for (uint32_t i = 0; i < xr::kUiSlots; ++i) {
            if (ui_base[i] != nullptr) {
                ::UnmapViewOfFile(ui_base[i]);
            }
        }
        ::UnmapViewOfFile(control_base);
        ::CloseHandle(mapping);
        return false;
    }

    auto* header = static_cast<xr::SharedFrameHeader*>(control_base);

    // Reusing an existing section is normal -- the mod is injected and unloaded repeatedly while
    // the host keeps running -- so the frame counter is preserved rather than reset, and only the
    // identifying fields are (re)stamped.
    if (header->magic != xr::kSharedFrameMagic) {
        std::memset(header, 0, sizeof(*header));
        header->magic = xr::kSharedFrameMagic;
    }

    header->version = xr::kSharedFrameVersion;
    header->qpc_freq = tick_freq();
    header->writer_pid = ::GetCurrentProcessId();

    m_mapping = mapping;
    m_control_base = control_base;
    for (uint32_t i = 0; i < xr::kFrameSlots; ++i) {
        m_frame_base[i] = frame_base[i];
    }
    for (uint32_t i = 0; i < xr::kUiSlots; ++i) {
        m_ui_base[i] = ui_base[i];
    }

    LOGX("[publish] shared frame section open (%u bytes across %u views)", total,
        1u + xr::kFrameSlots + xr::kUiSlots);
    return true;
}

void FramePublisher::close() {
    if (m_control_base != nullptr) {
        // Leave the sequence EVEN. A reader that arrives after the mod unloads should see the last
        // complete frame rather than a half-written one it will wait on forever.
        auto* header = static_cast<xr::SharedFrameHeader*>(m_control_base);
        header->sequence = (header->sequence + 1u) & ~1u;
        ::UnmapViewOfFile(m_control_base);
        m_control_base = nullptr;
    }

    for (uint32_t i = 0; i < xr::kFrameSlots; ++i) {
        if (m_frame_base[i] != nullptr) {
            ::UnmapViewOfFile(m_frame_base[i]);
            m_frame_base[i] = nullptr;
        }
    }

    for (uint32_t i = 0; i < xr::kUiSlots; ++i) {
        if (m_ui_base[i] != nullptr) {
            ::UnmapViewOfFile(m_ui_base[i]);
            m_ui_base[i] = nullptr;
        }
    }

    if (m_mapping != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }

    if (m_tick_event != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_tick_event));
        m_tick_event = nullptr;
    }
}

void FramePublisher::set_flat(bool on) {
    m_flat.store(on, std::memory_order_release);

    // WRITTEN THROUGH IMMEDIATELY, not left for the next publish. The flag exists for the case
    // where the game has stopped being a 3D world, and "stopped" is exactly when publishing may
    // stop as well -- a loading screen, or a menu that freezes the renderer. Leaving it to
    // publish() would mean the one situation it is for is the one where it never arrives.
    //
    // A lone aligned uint32 needs no sequence protocol: x86 will not tear it, and the host reads it
    // outside the pixel seqlock for the same reason.
    if (m_control_base != nullptr) {
        static_cast<xr::SharedFrameHeader*>(m_control_base)->flat = on ? 1u : 0u;
    }
}

bool FramePublisher::publish_ui(const void* bits, uint32_t pitch, uint32_t width, uint32_t height,
                                bool bgra, bool premultiplied, bool derive_alpha) {
    if (m_control_base == nullptr) {
        return false;
    }
    auto* ui =
        reinterpret_cast<xr::UiFrameHeader*>(static_cast<uint8_t*>(m_control_base) + xr::kUiStateOffset);

    // A NULL PAYLOAD RETIRES THE LAYER. The mod is armed and disarmed at runtime, and without this
    // the host would keep showing the last HUD it was given forever -- a frozen ammo counter is a
    // worse failure than no ammo counter, because it looks live.
    if (bits == nullptr || width == 0 || height == 0) {
        ui->sequence |= 1u;
        ::MemoryBarrier();
        ui->present = 0u;
        ::MemoryBarrier();
        ui->sequence = (ui->sequence + 1u) & ~1u;
        return true;
    }

    const uint64_t needed = static_cast<uint64_t>(pitch) * height;
    if (needed > xr::kUiMaxBytes) {
        m_error = "UI layer does not fit its slot";
        return false;
    }

    const uint32_t write_slot = (ui->slot + 1u) % xr::kUiSlots;
    auto* payload = static_cast<uint8_t*>(m_ui_base[write_slot]);

    ui->sequence |= 1u;  // odd: the pixels are in flux
    ::MemoryBarrier();

    ui->width = width;
    ui->height = height;
    ui->pitch = pitch;
    ui->bytes = static_cast<uint32_t>(needed);
    ui->bgra = bgra ? 1u : 0u;
    ui->premultiplied = premultiplied ? 1u : 0u;
    ui->derive_alpha = derive_alpha ? 1u : 0u;
    ui->slot = write_slot;

    std::memcpy(payload, bits, static_cast<size_t>(needed));

    ui->write_qpc = now_ticks();
    ++ui->frames_written;
    ui->present = 1u;

    ::MemoryBarrier();
    ui->sequence = (ui->sequence + 1u) & ~1u;  // even: a whole layer is present
    return true;
}

bool FramePublisher::publish(const void* bits, uint32_t pitch, uint32_t width, uint32_t height,
                             bool bgra, uint32_t layout, uint32_t host_sequence,
                             const float* rendered_orientation) {
    if (m_control_base == nullptr || bits == nullptr || width == 0 || height == 0) {
        return false;
    }

    const uint64_t needed = static_cast<uint64_t>(pitch) * height;

    if (needed > xr::kSharedFrameMaxBytes) {
        m_error = "frame does not fit the shared section";
        return false;
    }

    auto* header = static_cast<xr::SharedFrameHeader*>(m_control_base);

    // WRITE INTO A BUFFER THE READER IS NOT USING. The published slot is the one it may be reading,
    // so anything but that is safe; advancing by one cycles through all three.
    const uint32_t write_slot = (header->slot + 1u) % xr::kFrameSlots;
    auto* payload = static_cast<uint8_t*>(m_frame_base[write_slot]);
    const int64_t t0 = now_ticks();

    // ODD while the pixels are in flux. A reader that samples mid-copy sees an odd sequence and
    // simply keeps the frame it already had, which is why this needs no lock and cannot stall the
    // render thread.
    // THIS SLOT'S PIXELS ARE ABOUT TO CHANGE. Odd first, so a host uploading out of that slot can
    // tell a wrap started underneath it instead of silently submitting a mixture of two frames.
    header->slot_generation[write_slot % xr::kFrameSlots] |= 1u;
    MemoryBarrier();

    header->sequence |= 1u;
    ::MemoryBarrier();

    header->width = width;
    header->height = height;
    header->pitch = pitch;
    header->bytes = static_cast<uint32_t>(needed);
    header->bgra = bgra ? 1u : 0u;
    header->layout = layout;
    header->host_sequence = host_sequence;
    // Stamped on every frame rather than latched: the flag has to be true for exactly the frames a
    // menu is up, and a stale one either reprojects a menu or flattens live gameplay.
    header->flat = m_flat.load(std::memory_order_relaxed) ? 1u : 0u;
    header->slot = write_slot;

    // ONE memcpy, straight from the locked surface. Row-by-row would be tidier if the destination
    // were tightly packed, but the reader is told the pitch instead -- copying padding is cheaper
    // than 1440 separate copies.
    std::memcpy(payload, bits, static_cast<size_t>(needed));

    const int64_t t1 = now_ticks();
    // Written inside the odd/even window with the rest of the header, so a reader either sees this
    // frame's pose or the previous frame's -- never half of one.
    if (rendered_orientation != nullptr) {
        for (int k = 0; k < 4; ++k) {
            header->rendered_orientation[k] = rendered_orientation[k];
        }
        header->rendered_valid = 1u;
    } else {
        header->rendered_valid = 0u;
    }

    header->write_qpc = t1;
    ++header->frames_written;

    ::MemoryBarrier();
    // Payload complete: even again, and BEFORE the header's sequence closes, so a reader that accepts
    // the header can already have seen a settled generation for the slot that header names.
    header->slot_generation[write_slot % xr::kFrameSlots] =
        (header->slot_generation[write_slot % xr::kFrameSlots] + 1u) & ~1u;
    MemoryBarrier();
    header->sequence = (header->sequence + 1u) & ~1u;  // EVEN: a whole frame is present

    m_last_ticks = t1 - t0;
    m_worst_ticks = m_last_ticks > m_worst_ticks ? m_last_ticks : m_worst_ticks;

    // ---- HOW EVENLY IS THE GAME PRODUCING FRAMES? ----------------------------------------------
    //
    // Not how FAST -- that reads a flat 38 fps in both a smooth place and a juddering one. How
    // EVENLY. A mean cannot tell a steady 26 ms from an alternating 20/32, and only the second one
    // makes the relationship between when a pose was sampled and when its frame is shown move
    // about. That would disturb the head path, which depends on that relationship through timewarp,
    // and leave stick rotation alone, which does not -- which is the asymmetry actually observed.
    //
    // Kept as sum and sum-of-squares so the spread comes out of the same pass, plus the extremes.
    if (m_prev_publish_qpc != 0) {
        const double ms = static_cast<double>(t1 - m_prev_publish_qpc) * 1000.0 /
                          static_cast<double>(qpc_frequency());
        if (ms > 0.0 && ms < 1000.0) {
            ++m_gap_count;
            m_gap_sum += ms;
            m_gap_sum_sq += ms * ms;
            m_gap_min = m_gap_count == 1 || ms < m_gap_min ? ms : m_gap_min;
            m_gap_max = ms > m_gap_max ? ms : m_gap_max;
        }
    }
    m_prev_publish_qpc = t1;
    m_last_w = width;
    m_last_h = height;
    return true;
}

const xr::HostState* FramePublisher::host_state() const {
    if (m_control_base == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<const xr::HostState*>(static_cast<uint8_t*>(m_control_base) +
                                                  xr::kHostStateOffset);
}

const xr::HandsState* FramePublisher::hands_state() const {
    if (m_control_base == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<const xr::HandsState*>(static_cast<uint8_t*>(m_control_base) +
                                                   xr::kHandsStateOffset);
}

xr::HapticsState* FramePublisher::haptics_state() {
    if (m_control_base == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<xr::HapticsState*>(static_cast<uint8_t*>(m_control_base) +
                                               xr::kHapticsStateOffset);
}

uint32_t FramePublisher::haptics_queued() const {
    if (m_control_base == nullptr) {
        return 0u;
    }

    return reinterpret_cast<const xr::HapticsState*>(static_cast<uint8_t*>(m_control_base) +
                                                     xr::kHapticsStateOffset)
        ->write_index;
}

bool FramePublisher::queue_haptic(uint32_t hand, int64_t duration_ns, float frequency_hz,
                                  float amplitude, bool stop) {
    auto* haptics = haptics_state();

    if (haptics == nullptr || (hand != xr::kHandLeft && hand != xr::kHandRight)) {
        return false;
    }

    // ---- ONE PRODUCER AT A TIME, ENFORCED --------------------------------------------------
    //
    // The ring's discipline below assumes a single producer: the ticket is claimed as
    // `write_index + 1` with no compare-exchange, because "the game" was originally the only
    // caller. That stopped being true the moment there were two entry points -- /vr/haptic runs
    // on the IPC thread and the gunfire feedback runs on the game thread -- and two producers
    // computing `write_index + 1` claim the SAME ticket, so one silently overwrites the other's
    // slot and the host fires one pulse where two were asked for.
    //
    // A LOCK RATHER THAN AN INTERLOCKED CLAIM. Making the claim atomic is easy; making the
    // PUBLISH correct afterwards is not, because write_index may only advance once every earlier
    // slot is committed, which needs a second cursor and a retry loop. This path runs a handful of
    // times a second at most, so serialising the whole claim-to-publish keeps the single-producer
    // invariant TRUE BY CONSTRUCTION instead of defending against its absence. It is a plain
    // uncontended lock with no allocation and no I/O inside it -- the game thread's worst case is
    // waiting on about twenty stores.
    std::lock_guard<std::mutex> guard(m_haptic_lock);

    // TICKETS ARE 1-BASED: the Nth pulse ever queued is ticket N and lives in slot (N-1) mod the
    // ring. Single producer -- guaranteed by the lock above -- so the next ticket is one past the
    // newest published index and needs no compare-exchange to claim.
    const uint32_t ticket = haptics->write_index + 1u;
    xr::HapticPulse* pulse = &haptics->slot[(ticket - 1u) % xr::kHapticSlots];

    // NO ODD/EVEN SEQUENCE HERE, unlike every other block in the mapping. A monotonic index
    // published after its payload already is one, and the per-slot commit stamp covers what the
    // index cannot: the host can be halfway through copying this very slot when we lap the ring,
    // and a block-wide sequence would have gone even again long before that mattered. Invalidated
    // first, stamped last -- a reader that does not see its own ticket on BOTH sides of its copy
    // discards the entry rather than firing half of the old pulse and half of this one.
    //
    // THE IN-PROGRESS MARKER IS ~ticket, NOT ZERO. Zero looks obvious and is wrong exactly once
    // per 2^32 pulses: write_index wraps, ticket becomes 0, and then the "being written" stamp and
    // the "committed" stamp are the same value -- so a host copying that slot mid-write sees the
    // ticket it expected on both sides and fires a torn entry. Any value that cannot equal the
    // ticket works; the complement is free and needs no extra state.
    pulse->commit = ~ticket;
    ::MemoryBarrier();

    // NON-POSITIVE DURATIONS BECOME XR_MIN_HAPTIC_DURATION (-1). OpenXR requires
    // XrHapticVibration::duration to be positive or exactly that sentinel, so a zero queued by a
    // direct caller would reach the runtime as a value it must reject. The /vr/haptic route
    // already maps an absent or zero `ms` this way; doing it here too means the C++ API carries
    // the same guarantee instead of relying on every caller knowing the rule. A stop ignores the
    // duration entirely, so it is left alone.
    pulse->duration_ns = (!stop && duration_ns <= 0) ? -1 : duration_ns;
    pulse->frequency_hz = frequency_hz;
    // CLAMPED HERE rather than trusted. A runtime is entitled to reject an amplitude outside
    // [0,1], and the split across this boundary is that the mod owns policy while the host passes
    // values straight through. Written as "greater than zero" rather than "less than zero" so a
    // NaN from a caller's own arithmetic lands on 0 instead of surviving every comparison.
    pulse->amplitude = amplitude > 0.0f ? (amplitude < 1.0f ? amplitude : 1.0f) : 0.0f;
    pulse->hand = hand;
    pulse->stop = stop ? 1u : 0u;

    ::MemoryBarrier();
    pulse->commit = ticket;
    ::MemoryBarrier();

    haptics->write_qpc = now_ticks();

    // PUBLISHED LAST, and interlocked rather than plain: this index is the release the host reads
    // against, and it must not become visible before the slot it names. Never decremented.
    ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&haptics->write_index),
                          static_cast<LONG>(ticket));
    return true;
}

bool FramePublisher::request_haptic(uint32_t hand, int64_t duration_ns, float frequency_hz,
                                    float amplitude) {
    return queue_haptic(hand, duration_ns, frequency_hz, amplitude, false);
}

bool FramePublisher::stop_haptic(uint32_t hand) {
    // The remaining fields are ignored by the host on a stop, but they are written all the same:
    // a slot carries whatever the last pulse left in it otherwise, and a diagnostic reading the
    // ring back should not see a stop wearing an old pulse's amplitude.
    return queue_haptic(hand, 0, 0.0f, 0.0f, true);
}

namespace {

// The block lives at a fixed offset inside the control view, like every other block.
xr::FrameHandshake* handshake_block(void* control_base) {
    if (control_base == nullptr) {
        return nullptr;
    }
    auto* base = static_cast<uint8_t*>(control_base);
    const auto* header = reinterpret_cast<const xr::SharedFrameHeader*>(base);
    if (header->magic != xr::kSharedFrameMagic || header->version != xr::kSharedFrameVersion) {
        return nullptr;  // a foreign or unstamped layout: never write acks into it
    }
    return reinterpret_cast<xr::FrameHandshake*>(base + xr::kHandshakeOffset);
}

// POST THE PHASE, THEN WAIT FOR THE HOST TO SERVE THAT EXACT REQUEST.
//
// The id is checked as well as the phase: an ack left over from the previous frame carries the
// previous id, and accepting it would let this thread run a frame ahead of the runtime -- which is
// the desynchronisation the whole tagged design exists to prevent.
//
// Bounded, because a game blocked forever on a dead host is worse than an unpaced game. A timeout
// is a real event, counted and reported, never smoothed over.
// Microseconds, from the same QPC the rest of this file uses. Cheap enough to call twice per stage:
// three pairs a frame against a 13.9 ms budget is noise, and guessing at where the time went has
// already cost more than measuring it.
int64_t qpc_now() {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpc_freq_once() {
    static const int64_t f = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart ? v.QuadPart : 1;
    }();
    return f;
}

bool post_and_wait(xr::FrameHandshake* hs, uint32_t id, uint32_t phase, uint32_t timeout_ms,
                   int32_t* status_out) {
    hs->request_id = id;
    hs->phase = phase;
    MemoryBarrier();
    hs->req_id[phase] = id;  // LAST: this is what makes the request visible to the host

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        const uint32_t s0 = hs->sequence;
        MemoryBarrier();
        const uint32_t ap = hs->ack_phase;
        const uint32_t ai = hs->ack_id;
        const int32_t st = hs->ack_status;
        MemoryBarrier();
        if (hs->ack_slot[phase] == id && (s0 & 1u) == 0u && hs->sequence == s0) {
            if (status_out != nullptr) {
                *status_out = st;
            }
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(0);
    }
}

}  // namespace

bool FramePublisher::handshake_ready() const {
    // BOTH: the block exists AND the host says it is servicing. Mapping presence alone is not
    // readiness -- xr64 maps this before its session runs and keeps it mapped after the session
    // stops, and posting a request nobody will answer costs a full timeout per update.
    const auto* hs = handshake_block(m_control_base);
    return hs != nullptr && hs->host_servicing != 0u;
}

FramePublisher::FrameLease FramePublisher::xr_wait() {
    FrameLease lease{};
    auto* hs = handshake_block(m_control_base);
    if (hs == nullptr) {
        return lease;
    }
    ++m_request_id;
    m_xr_waits.fetch_add(1, std::memory_order_relaxed);

    int32_t status = 0;
    const int64_t t0 = qpc_now();
    const bool served = post_and_wait(hs, m_request_id, xr::kPhaseWait, 100u, &status);
    m_rpc_wait_n.fetch_add(1, std::memory_order_relaxed);
    m_rpc_wait_us.fetch_add(
        static_cast<uint64_t>((qpc_now() - t0) * 1000000 / qpc_freq_once()),
        std::memory_order_relaxed);
    if (!served) {
        m_xr_wait_to.fetch_add(1, std::memory_order_relaxed);
        return lease;
    }
    lease.ok = status >= 0;  // XR_SUCCEEDED
    lease.should_render = hs->should_render != 0u;
    lease.predicted_display_time = hs->predicted_display_time;
    lease.predicted_period_ns = hs->predicted_period_ns;
    for (size_t i = 0; i < 4; ++i) {
        lease.head_orientation[i] = hs->head_orientation[i];
    }
    for (size_t i = 0; i < 3; ++i) {
        lease.head_position[i] = hs->head_position[i];
    }
    return lease;
}

bool FramePublisher::xr_begin() {
    auto* hs = handshake_block(m_control_base);
    if (hs == nullptr) {
        return false;
    }
    int32_t status = 0;
    const int64_t t0 = qpc_now();
    const bool served = post_and_wait(hs, m_request_id, xr::kPhaseBegin, 100u, &status);
    m_rpc_begin_n.fetch_add(1, std::memory_order_relaxed);
    m_rpc_begin_us.fetch_add(
        static_cast<uint64_t>((qpc_now() - t0) * 1000000 / qpc_freq_once()),
        std::memory_order_relaxed);
    if (!served) {
        m_xr_begin_to.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return status >= 0;
}

// ---- FIRE AND FORGET, AND THAT IS THE WHOLE POINT ----------------------------------------------
//
// The pixels are published before this is called, so there is nothing the GAME needs from the host's
// submission -- it does not read the result, and the frame is already out of its hands. Blocking here
// cost ~4.8 ms of the game's own frame, measured, which is most of the gap between 68 fps and the
// compositor's 72 and the bulk of why putting the headset on halved the frame rate.
//
// Posting without waiting is only possible because each stage has its own request slot: with a single
// `phase` field the next frame's WAIT would overwrite this END before the host read it, the request
// would vanish, and the host would sit out its whole timeout. That is exactly what happened at the
// menu and it is why this waited in the first place.
//
// The host still ends the frame it began, and slot_generation still catches it wrapping into a slot
// mid-upload -- which is the risk overlap introduces, now measured instead of assumed.
bool FramePublisher::xr_end() {
    auto* hs = handshake_block(m_control_base);
    if (hs == nullptr) {
        return false;
    }
    m_rpc_end_n.fetch_add(1, std::memory_order_relaxed);

    hs->request_id = m_request_id;
    hs->phase = xr::kPhaseEnd;
    MemoryBarrier();
    hs->req_id[xr::kPhaseEnd] = m_request_id;  // LAST: makes the request visible
    return true;
}

bool FramePublisher::wait_for_host_tick() {
    if (m_tick_event == nullptr) {
        // Auto-reset: each tick releases exactly one wait, so a game that falls behind does not
        // bank up credits and then sprint through several frames without waiting.
        m_tick_event = ::CreateEventA(nullptr, FALSE, FALSE, xr::kFrameTickEventName);

        if (m_tick_event == nullptr) {
            return false;
        }
    }

    // ---- THE BUDGET IS DERIVED, NEVER ASSUMED --------------------------------------------------
    //
    // Two display periods, from the runtime's OWN XrFrameState::predictedDisplayPeriod, which the
    // host publishes in HostState. Two periods is the long-standing rule -- long enough for a tick
    // that is coming, short enough that a tick which is NOT coming costs little before the give-up
    // counter takes over. The rule was never the bug; hardcoding it as 22 ms was. That is two
    // periods at 90 Hz and only 1.58 at the 72 Hz this runtime presents at, so one slow host
    // iteration overran the wait and that frame published unpaced.
    //
    // READ THROUGH THE SEQLOCK, not as a bare field. The 32-bit field cannot tear on its own, but
    // the period and the frame counter this function paces against must come from the SAME host
    // publish -- otherwise a cadence change is observed half-applied, which is precisely the class
    // of bug this whole path keeps producing.
    //
    // FAIL OPEN AT ZERO. The period is zero until the host's first xrWaitFrame completes, and
    // pacing against a substituted default is the failure being removed here, so we do not pace at
    // all until the host has stated its cadence.
    uint32_t period_ns = 0;
    if (const auto* hs = host_state()) {
        const uint32_t s0 = hs->sequence;
        MemoryBarrier();
        const uint32_t p = hs->predicted_display_period_ns;
        MemoryBarrier();
        if ((s0 & 1u) == 0u && hs->sequence == s0) {
            period_ns = p;
        }
    }
    m_host_period_ns.store(period_ns, std::memory_order_relaxed);
    if (period_ns == 0u) {
        m_tick_timeout_ms.store(0u, std::memory_order_relaxed);
        return false;
    }
    const DWORD timeout_ms = static_cast<DWORD>((2ull * period_ns + 999'999ull) / 1'000'000ull);
    m_tick_timeout_ms.store(static_cast<uint32_t>(timeout_ms), std::memory_order_relaxed);

    // GIVE UP AFTER A RUN OF SILENCE, and keep checking for free. Without this, a host that exits
    // -- or a session that goes idle because the headset came off -- would cost the game a full
    // timeout every frame, silently capping it at 1000/timeout fps for no benefit at all.
    const DWORD wait = m_consecutive_timeouts >= kPacingGiveUp ? 0u : timeout_ms;
    ++m_tick_waits;

    // ---- LOCK TO A SUBMULTIPLE, WHICH IS THE WHOLE POINT OF PACING -----------------------------
    //
    // An auto-reset event holds ONE credit, and one credit is exactly enough to let every late
    // frame straight through: a tick that fired WHILE we were rendering leaves the event signalled,
    // so the wait returns immediately and the game free-runs. It is therefore unpaced in precisely
    // the case that needs pacing -- when it cannot hit the compositor's rate -- and 66 fps against
    // 72 Hz beats at 6 Hz, which is the judder.
    //
    // A tick we missed is not permission to start the next frame; it is the boundary we already
    // missed. Dropping it makes the wait land on the NEXT boundary, so a game that cannot hold 72
    // settles on 36 rather than sliding between them. That is the same hard step a native OpenXR
    // application takes, and it is why runtimes show 45/90 rather than 66.
    //
    // It does not over-correct: a frame that finishes BEFORE its boundary finds nothing to drop and
    // still runs at full rate. Only work that overran pays, and it pays by waiting rather than by
    // juddering.
    //
    // NOT WHILE GIVEN UP, or the state can never clear. With the give-up counter latched the wait
    // below is non-blocking, so the single pending tick is the ONLY evidence the host is alive --
    // and draining it here consumed exactly that, leaving the poll to time out forever. Pacing then
    // stayed off permanently with phase_lock reading true, which is how a run showed a 1.36 cadence
    // and a lock that appeared to be enabled.
    // ---- COUNT BOUNDARIES, DO NOT JUST FEEL THEM ---------------------------------------------
    //
    // An auto-reset event can only ever say "at least one boundary passed". It cannot say how many,
    // and a divisor needs the number: at 1/4 the drain ate one legitimate boundary every frame and
    // the game landed on FIVE periods, measuring 30 fps against a 150 Hz tick when 37.5 was the
    // intent. So the clock is the host's own frame COUNTER, which is exact, and the event is
    // demoted to what it is good at -- waking us up.
    //
    // Holding a divisor rather than aligning to the nearest edge is the actual rule: a workload
    // that straddles a boundary otherwise alternates between N and N+1 periods and the beat
    // survives. Runtimes show a hard 45/90 for this reason.
    if (m_phase_lock.load(std::memory_order_relaxed) && m_consecutive_timeouts < kPacingGiveUp &&
        host_state() != nullptr) {
        // A FRESH START AFTER SILENCE -- a load, an alt-tab, a host restart. The old divisor
        // describes a situation that no longer exists, and inheriting it means crawling through
        // whatever comes next.
        if (m_consecutive_timeouts > 0 || !m_tick_primed) {
            m_divisor = 1;
            m_window_frames = 0;
            m_window_overruns = 0;
            m_clean_windows = 0;
            m_last_tick_seen = host_frames();
            m_tick_primed = true;
        }

        LARGE_INTEGER wait_begin{};
        ::QueryPerformanceCounter(&wait_begin);
        const uint32_t target = m_last_tick_seen + m_divisor;
        while (static_cast<int32_t>(host_frames() - target) < 0) {
            if (::WaitForSingleObject(static_cast<HANDLE>(m_tick_event), wait) != WAIT_OBJECT_0) {
                ++m_tick_timeouts;
                if (m_consecutive_timeouts < kPacingGiveUp) {
                    ++m_consecutive_timeouts;
                }
                return false;
            }
        }

        LARGE_INTEGER wait_end{};
        ::QueryPerformanceCounter(&wait_end);
        m_wait_qpc = wait_end.QuadPart - wait_begin.QuadPart;

        // How many periods the frame ACTUALLY took, which is the only honest input to the divisor.
        const uint32_t now = host_frames();
        const uint32_t elapsed = now - m_last_tick_seen;
        m_last_tick_seen = now;
        m_consecutive_timeouts = 0;

        // ---- WHAT THE DOWNGRADE MUST ACTUALLY ASK ----------------------------------------------
        //
        // Not "did this frame fit in the current budget" -- at 1/2 it always does, which is what
        // made the divisor FLAP: climb because 1 period is too tight, drop because 2 is roomy,
        // repeat. Measured 1.72 periods per frame at 150 Hz, which is the average of a 1 and a 2,
        // and a divisor that alternates is the very beat this exists to remove.
        //
        // The honest question is whether the WORK would fit in the SMALLER budget, so the frame is
        // timed and compared against (divisor - 1) periods with a margin. A game sitting between
        // two divisors then stays on the slower one instead of oscillating across the gap.
        LARGE_INTEGER qpc{};
        ::QueryPerformanceCounter(&qpc);
        if (m_work_start_qpc != 0 && elapsed > 0) {
            const int64_t frame_qpc = qpc.QuadPart - m_work_start_qpc;
            const int64_t period = frame_qpc / static_cast<int64_t>(elapsed);
            m_period_qpc = m_period_qpc == 0 ? period : (m_period_qpc * 7 + period) / 8;
            // The wait is the idle part; the work is what is left of the frame.
            const int64_t work = frame_qpc - m_wait_qpc;
            if (work > m_window_work_qpc) {
                m_window_work_qpc = work;
            }
        }
        m_work_start_qpc = qpc.QuadPart;

        if (elapsed > m_divisor) {
            ++m_ticks_dropped;
            ++m_window_overruns;
        }

        if (++m_window_frames >= kDivisorWindow) {
            if (m_window_overruns > kDivisorWindow / 10) {
                if (m_divisor < kMaxDivisor) {
                    ++m_divisor;
                }
                m_clean_windows = 0;
            } else if (m_window_overruns == 0 && m_divisor > 1 && m_period_qpc > 0 &&
                       m_window_work_qpc * 23 <
                           m_period_qpc * static_cast<int64_t>(m_divisor - 1) * 20) {
                // The WORST frame in the window would fit the next divisor down with ~15% to
                // spare. Worst rather than mean, because a divisor chosen for the average is wrong
                // for every frame above it.
                if (++m_clean_windows >= 2) {
                    --m_divisor;
                    m_clean_windows = 0;
                }
            } else {
                m_clean_windows = 0;
            }
            m_window_frames = 0;
            m_window_overruns = 0;
            m_window_work_qpc = 0;
        }
        return true;
    }

    if (::WaitForSingleObject(static_cast<HANDLE>(m_tick_event), wait) == WAIT_OBJECT_0) {
        m_consecutive_timeouts = 0;
        return true;
    }

    ++m_tick_timeouts;

    if (m_consecutive_timeouts < kPacingGiveUp) {
        ++m_consecutive_timeouts;
    }

    return false;
}

uint32_t FramePublisher::frames() const {
    return m_control_base == nullptr ? 0u
                                      : static_cast<xr::SharedFrameHeader*>(m_control_base)->frames_written;
}

double FramePublisher::last_publish_ms() const {
    return ticks_to_ms(m_last_ticks);
}

double FramePublisher::worst_publish_ms() const {
    return ticks_to_ms(m_worst_ticks);
}

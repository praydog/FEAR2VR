#include "FramePublisher.hpp"

#include <windows.h>

#include <cstring>

#include "Log.hpp"

namespace {

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
    if (m_base != nullptr) {
        return true;
    }

    m_error.clear();

    const uint32_t total =
        xr::kPayloadOffset + xr::kSharedFrameMaxBytes;

    HANDLE mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total,
                                          xr::kSharedFrameName);

    if (mapping == nullptr) {
        m_error = "CreateFileMapping failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }

    void* base = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, total);

    if (base == nullptr) {
        m_error = "MapViewOfFile failed (" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(mapping);
        return false;
    }

    auto* header = static_cast<xr::SharedFrameHeader*>(base);

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
    m_base = base;
    LOGX("[publish] shared frame section open (%u bytes)", total);
    return true;
}

void FramePublisher::close() {
    if (m_base != nullptr) {
        // Leave the sequence EVEN. A reader that arrives after the mod unloads should see the last
        // complete frame rather than a half-written one it will wait on forever.
        auto* header = static_cast<xr::SharedFrameHeader*>(m_base);
        header->sequence = (header->sequence + 1u) & ~1u;
        ::UnmapViewOfFile(m_base);
        m_base = nullptr;
    }

    if (m_mapping != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }
}

bool FramePublisher::publish(const void* bits, uint32_t pitch, uint32_t width, uint32_t height,
                             bool bgra, uint32_t layout, uint32_t host_sequence) {
    if (m_base == nullptr || bits == nullptr || width == 0 || height == 0) {
        return false;
    }

    const uint64_t needed = static_cast<uint64_t>(pitch) * height;

    if (needed > xr::kSharedFrameMaxBytes) {
        m_error = "frame does not fit the shared section";
        return false;
    }

    auto* header = static_cast<xr::SharedFrameHeader*>(m_base);
    auto* payload = static_cast<uint8_t*>(m_base) + xr::kPayloadOffset;
    const int64_t t0 = now_ticks();

    // ODD while the pixels are in flux. A reader that samples mid-copy sees an odd sequence and
    // simply keeps the frame it already had, which is why this needs no lock and cannot stall the
    // render thread.
    header->sequence |= 1u;
    ::MemoryBarrier();

    header->width = width;
    header->height = height;
    header->pitch = pitch;
    header->bytes = static_cast<uint32_t>(needed);
    header->bgra = bgra ? 1u : 0u;
    header->layout = layout;
    header->host_sequence = host_sequence;

    // ONE memcpy, straight from the locked surface. Row-by-row would be tidier if the destination
    // were tightly packed, but the reader is told the pitch instead -- copying padding is cheaper
    // than 1440 separate copies.
    std::memcpy(payload, bits, static_cast<size_t>(needed));

    const int64_t t1 = now_ticks();
    header->write_qpc = t1;
    ++header->frames_written;

    ::MemoryBarrier();
    header->sequence = (header->sequence + 1u) & ~1u;  // EVEN: a whole frame is present

    m_last_ticks = t1 - t0;
    m_worst_ticks = m_last_ticks > m_worst_ticks ? m_last_ticks : m_worst_ticks;
    m_last_w = width;
    m_last_h = height;
    return true;
}

const xr::HostState* FramePublisher::host_state() const {
    if (m_base == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<const xr::HostState*>(static_cast<uint8_t*>(m_base) +
                                                  sizeof(xr::SharedFrameHeader));
}

uint32_t FramePublisher::frames() const {
    return m_base == nullptr ? 0u : static_cast<xr::SharedFrameHeader*>(m_base)->frames_written;
}

double FramePublisher::last_publish_ms() const {
    return ticks_to_ms(m_last_ticks);
}

double FramePublisher::worst_publish_ms() const {
    return ticks_to_ms(m_worst_ticks);
}

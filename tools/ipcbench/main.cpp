// ---- WHAT A 64-BIT HOST ACTUALLY COSTS ---------------------------------------------------------
//
// FEAR2 is 32-bit and the 32-bit Oculus runtime cannot create a session, so submission has to
// happen in a 64-bit process. The question that decides the architecture is whether crossing that
// boundary costs latency, and it is worth measuring rather than assuming -- "IPC is slow" is folk
// knowledge from pipes and sockets, not from shared memory.
//
// This is the real transport: one file mapping, both sides mapping the same pages, a sequence
// number, and no kernel transition in the steady state. Built for BOTH bitnesses from one source,
// because cross-bitness shared memory is exactly the case in question and a layout mistake there
// would be silent -- every field is fixed-width and the header is asserted.
//
//   ipcbench --server    (64-bit: the host, reads)
//   ipcbench --client    (32-bit: the game, writes)
//
// Reports, per payload size: notification latency (write visible -> read observed) and the cost of
// the copy itself, which is the part that is NOT free.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Fixed-width and explicitly padded: a 32-bit writer and a 64-bit reader must agree byte for byte,
// and `size_t` or a bare enum here would quietly differ between them.
struct alignas(64) Header {
    volatile uint32_t sequence;   // odd while writing, even when a frame is complete
    uint32_t payload_bytes;
    int64_t write_start_qpc;
    int64_t write_end_qpc;
    int64_t read_qpc;
    uint32_t frames;
    uint32_t stop;
};

static_assert(sizeof(Header) == 64, "the shared header must be identical in both bitnesses");

constexpr const char* kName = "Local\\fear2vr_ipcbench";
// Up to 4K PER EYE side-by-side (7680x2160 BGRA = 66 MB), because that is the resolution the
// architecture has to be judged against, not the one the game happens to render today.
constexpr uint32_t kMaxPayload = 7680u * 2160u * 4u;

double ms(int64_t ticks, int64_t freq) {
    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(freq);
}

int64_t qpc() {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpf() {
    LARGE_INTEGER v{};
    ::QueryPerformanceFrequency(&v);
    return v.QuadPart;
}

void* map(bool create, HANDLE& out) {
    const uint32_t total = sizeof(Header) + kMaxPayload;

    out = create ? ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total,
                                        kName)
                 : ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kName);

    if (out == nullptr) {
        return nullptr;
    }

    return ::MapViewOfFile(out, FILE_MAP_ALL_ACCESS, 0, 0, total);
}

}  // namespace

int main(int argc, char** argv) {
    ::setvbuf(stdout, nullptr, _IONBF, 0);

    bool server = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0) {
            server = true;
        }
    }

    std::printf("[ipc] %s, %u-bit\n", server ? "server (host)" : "client (game)",
                static_cast<unsigned>(sizeof(void*) * 8));

    HANDLE section = nullptr;
    void* base = map(server, section);

    if (base == nullptr) {
        std::printf("[ipc] mapping failed (%lu)\n", ::GetLastError());
        return 1;
    }

    auto* header = static_cast<Header*>(base);
    auto* payload = static_cast<uint8_t*>(base) + sizeof(Header);
    const int64_t freq = qpf();

    // Payloads spanning the range that matters: a quarter-resolution eye pair up to a full frame.
    const uint32_t sizes[] = {
        1280u * 1440u * 2u * 4u,  // half-res per eye, side by side
        2064u * 2208u * 2u * 4u,  // Quest Pro's own recommended render size, both eyes
        3840u * 2160u * 2u * 4u,  // 4K per eye
    };
    constexpr int kIterations = 200;

    if (server) {
        std::memset(header, 0, sizeof(Header));
        std::printf("[ipc] waiting for the client...\n");

        std::vector<uint8_t> sink(kMaxPayload);
        uint32_t last = 0;
        int64_t notify_total = 0;
        int64_t copy_total = 0;
        int64_t notify_worst = 0;
        int count = 0;
        uint32_t current_size = 0;

        while (header->stop == 0) {
            const uint32_t seq = header->sequence;

            if (seq == last || (seq & 1u) != 0u) {
                ::YieldProcessor();  // spin: a kernel wait would measure the scheduler, not the transport
                continue;
            }

            const int64_t seen = qpc();
            const uint32_t bytes = header->payload_bytes;

            // The read side's real work: get the pixels somewhere it can use them. In the mod this
            // is an UpdateSubresource into the swapchain texture; here it is a plain copy, which is
            // the same order and does not pretend the pixels move for free.
            const int64_t copy_start = qpc();
            std::memcpy(sink.data(), payload, bytes);
            const int64_t copy_end = qpc();

            header->read_qpc = seen;
            last = seq;

            if (bytes != current_size) {
                if (count > 0) {
                    std::printf("[ipc] %7u bytes (%5.2f MB): notify mean %.4f ms worst %.4f ms | "
                                "copy mean %.3f ms  -> %.0f MB/s\n",
                                current_size, current_size / 1048576.0,
                                ms(notify_total / count, freq), ms(notify_worst, freq),
                                ms(copy_total / count, freq),
                                (static_cast<double>(current_size) / 1048576.0) /
                                    (ms(copy_total / count, freq) / 1000.0));
                }

                current_size = bytes;
                notify_total = copy_total = notify_worst = 0;
                count = 0;
                continue;
            }

            const int64_t notify = seen - header->write_end_qpc;
            notify_total += notify;
            copy_total += copy_end - copy_start;
            notify_worst = notify > notify_worst ? notify : notify_worst;
            ++count;
        }

        if (count > 0) {
            std::printf("[ipc] %7u bytes (%5.2f MB): notify mean %.4f ms worst %.4f ms | "
                        "copy mean %.3f ms  -> %.0f MB/s\n",
                        current_size, current_size / 1048576.0, ms(notify_total / count, freq),
                        ms(notify_worst, freq), ms(copy_total / count, freq),
                        (static_cast<double>(current_size) / 1048576.0) /
                            (ms(copy_total / count, freq) / 1000.0));
        }

        std::printf("[ipc] done\n");
    } else {
        std::vector<uint8_t> source(kMaxPayload, 0x7F);
        int64_t write_total = 0;
        int writes = 0;

        for (uint32_t bytes : sizes) {
            for (int i = 0; i < kIterations; ++i) {
                const int64_t t0 = qpc();
                header->sequence |= 1u;  // odd: a reader must not take a half-written frame
                header->payload_bytes = bytes;
                header->write_start_qpc = t0;

                // The WRITE side's cost, which in the mod is the memcpy out of the locked staging
                // surface into the shared section -- unavoidable, and on the game's render thread.
                std::memcpy(payload, source.data(), bytes);

                const int64_t t1 = qpc();
                header->write_end_qpc = t1;
                _ReadWriteBarrier();
                ::MemoryBarrier();
                header->sequence = (header->sequence + 1u) & ~1u;

                write_total += t1 - t0;
                ++writes;
                ::Sleep(1);  // pace it like frames rather than hammering the cache
            }

            std::printf("[ipc] wrote %7u bytes x%d: write mean %.3f ms\n", bytes, kIterations,
                        ms(write_total / writes, freq));
            write_total = 0;
            writes = 0;
        }

        header->stop = 1;
    }

    ::UnmapViewOfFile(base);
    ::CloseHandle(section);
    return 0;
}

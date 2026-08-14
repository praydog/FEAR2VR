// ---- THE 64-BIT XR HOST ------------------------------------------------------------------------
//
// FEAR2 is 32-bit and the 32-bit Oculus runtime dies inside its own RuntimeIPC init during
// xrCreateSession -- measured, and proven against this program, which performs the identical
// sequence in 64 bits on the same machine and same headset and succeeds. So submission lives here.
//
// This is deliberately NOT built by build.bat: the mod is 32-bit because the game is.
//
//   cmake -B build64 -A x64 -S tools/xr64 && cmake --build build64 --config Release
//   build64/RelWithDebInfo/xr64.exe --probe   one-shot capability report, exits
//   build64/RelWithDebInfo/xr64.exe           run the frame loop and submit
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
#include <dbghelp.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xr/SharedFrame.hpp"

// All real logic for the in-headset settings panel lives under ui/ -- this is the include, and
// the only other touches in this file are one construction+init call, one per-frame call that
// returns a layer to submit, and one shutdown call. See ui/SettingsUi.hpp for why it owns its
// own OpenXR swapchain rather than reusing ui_swapchain below.
#include "ui/SettingsUi.hpp"
#include <chrono>

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

// ---- HOW MANY COMPOSITION LAYERS THIS HOST CAN SUBMIT -------------------------------------------
//
// The worst case today is four: the two-quad fallback, the mod's HUD quad and the settings panel.
// The array used to be exactly four with nothing comparing the count against it, so the next layer
// type anyone added would have written past the end of a stack array in silence. Sized with room,
// and every append is bounds-checked anyway.
constexpr uint32_t kMaxLayers = 8u;
static_assert(kMaxLayers >= 4, "two quads plus the mod's HUD plus the settings panel");

// ---- TANGENTS THAT STAY FINITE ------------------------------------------------------------------
//
// The crop maths maps angles to pixels through tan(), which is exactly right and which blows up as
// a half-angle approaches pi/2. At Quest FOVs (~50 degrees a side) that is invisible; on a wide
// Pimax mode a per-eye half-angle gets close enough that tanf returns something enormous, the
// computed rectangle collapses, and the eye sees a sliver. Clamping just under the asymptote keeps
// the mapping finite and changes nothing on any headset that was never near it.
constexpr float kMaxHalfAngleRad = 1.56206968f;  // 89.5 degrees

float tan_half_angle(float radians) {
    return tanf((std::max)(-kMaxHalfAngleRad, (std::min)(kMaxHalfAngleRad, radians)));
}

const char* blend_mode_name(XrEnvironmentBlendMode m) {
    switch (m) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "ALPHA_BLEND";
    default: return "UNKNOWN";
    }
}

const char* view_config_name(XrViewConfigurationType t) {
    switch (t) {
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO: return "PRIMARY_MONO";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO: return "PRIMARY_STEREO";
    // Also XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, which openxr.h defines as an alias of
    // this same value rather than a distinct one -- naming both here would be a duplicate case.
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET:
        return "PRIMARY_STEREO_WITH_FOVEATED_INSET";
    case XR_VIEW_CONFIGURATION_TYPE_SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT:
        return "SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT";
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

    // GAME -> HOST, the only block in this mapping that travels that way. Never zeroed on open
    // (see below) and never written here: this reader consumes the ring the mod fills.
    xr::HapticsState* haptics = nullptr;
    const xr::UiFrameHeader* ui = nullptr;

    // ---- ONE VIEW PER SLOT, mirroring the writer (see FramePublisher::open) ---------------------
    //
    // At native resolution the section is well over 100 MB, more than a 32-bit-friendly single
    // view can rely on finding as one contiguous reservation -- see xr::kViewGranularity in
    // SharedFrame.hpp for the failure this avoids. The control block (header through UiFrameHeader)
    // is small enough to stay one view; every frame and UI slot gets its own.
    void* control_base = nullptr;
    void* frame_base[xr::kFrameSlots] = {};
    void* ui_base[xr::kUiSlots] = {};

    uint32_t last_sequence = 0;
    uint32_t last_ui_sequence = 0;
    bool logged_version_mismatch = false;

    // Unmaps every view this reader currently holds and clears the pointers into them. Shared by
    // the two failure paths below (a slot view that would not map, and a version mismatch) so
    // neither can leave a partial mapping that the next poll() would read through a dangling or
    // null pointer.
    void unmap_all() {
        for (uint32_t i = 0; i < xr::kFrameSlots; ++i) {
            if (frame_base[i] != nullptr) {
                UnmapViewOfFile(frame_base[i]);
                frame_base[i] = nullptr;
            }
        }
        for (uint32_t i = 0; i < xr::kUiSlots; ++i) {
            if (ui_base[i] != nullptr) {
                UnmapViewOfFile(ui_base[i]);
                ui_base[i] = nullptr;
            }
        }
        if (control_base != nullptr) {
            UnmapViewOfFile(control_base);
            control_base = nullptr;
        }
        header = nullptr;
        host = nullptr;
        hands = nullptr;
        haptics = nullptr;
        ui = nullptr;
    }

    bool open() {
        if (header != nullptr) {
            return true;
        }

        mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, xr::kSharedFrameName);

        if (mapping == nullptr) {
            return false;
        }

        void* base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, xr::kPayloadOffset);

        if (base == nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }

        control_base = base;

        // Collected into the member arrays as each succeeds rather than staged locally first: a
        // failure partway through still needs unmap_all() to know exactly which of these are real
        // views, and the member arrays already default-initialise to null for the ones it never
        // reaches.
        bool all_mapped = true;

        for (uint32_t i = 0; all_mapped && i < xr::kFrameSlots; ++i) {
            frame_base[i] = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, xr::slot_offset(i),
                                          xr::kSharedFrameMaxBytes);
            all_mapped = frame_base[i] != nullptr;
        }

        for (uint32_t i = 0; all_mapped && i < xr::kUiSlots; ++i) {
            ui_base[i] = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, xr::ui_slot_offset(i),
                                       xr::kUiMaxBytes);
            all_mapped = ui_base[i] != nullptr;
        }

        if (!all_mapped) {
            // Same posture as the version gate below: a reader missing one slot's view would read
            // through a null pointer the moment the writer's round-robin reached it, so this
            // refuses the mapping entirely rather than serving a partial one.
            std::printf("[host] shared frame mapping incomplete -- a per-slot view failed to map "
                        "(GetLastError %lu), refusing it\n", GetLastError());
            unmap_all();
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }

        header = static_cast<const xr::SharedFrameHeader*>(control_base);
        host = reinterpret_cast<xr::HostState*>(static_cast<uint8_t*>(control_base) +
                                                xr::kHostStateOffset);
        hands = reinterpret_cast<xr::HandsState*>(static_cast<uint8_t*>(control_base) +
                                                  xr::kHandsStateOffset);
        haptics = reinterpret_cast<xr::HapticsState*>(static_cast<uint8_t*>(control_base) +
                                                      xr::kHapticsStateOffset);
        ui = reinterpret_cast<const xr::UiFrameHeader*>(static_cast<uint8_t*>(control_base) +
                                                         xr::kUiStateOffset);

        // Fresh committed memory reads as zero already, but the mapping outlives a crashed host --
        // a prior run could have left `sequence` odd (torn) or the hand data mid-write. Zeroing
        // once here, before the game or the frame loop can observe it, guarantees a reader always
        // finds an even sequence and all-invalid poses rather than whatever a dead process left.
        std::memset(hands, 0, sizeof(xr::HandsState));

        // NOT the haptics block, and that is the point of saying so. It is written by the GAME,
        // so zeroing it here would erase a ring the mod may already be filling. A stale mapping is
        // handled instead by the host seeding its read cursor from the first write_index it
        // observes, which touches nothing the other side owns.

        // VERSION GATE, checked once here rather than on every poll(): the layout MOVED in
        // version 2 (the UI block was inserted before the frame slots) and again in version 5
        // (each slot became its own view at its own aligned offset), so a mapping stamped by an
        // old writer -- or a new writer read by an old host -- would have every offset below this
        // point computed wrong. That reads as garbage pixels, not a crash, which is why this
        // refuses the mapping outright instead of trusting it. Magic is a prerequisite: version is
        // meaningless (and may still be zero) before the writer has stamped the header at all.
        if (header->magic == xr::kSharedFrameMagic && header->version != xr::kSharedFrameVersion) {
            if (!logged_version_mismatch) {
                std::printf("[host] shared frame version mismatch: host wants %u, mapping has %u "
                            "-- refusing to read it (rebuild the mod and the host together)\n",
                            xr::kSharedFrameVersion, header->version);
                logged_version_mismatch = true;
            }

            unmap_all();
            CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }

        return true;
    }

    // True when a COMPLETE frame newer than the last one is available.
    uint32_t layout() const { return layout_is_ours() ? header->layout : 0u; }
    // ---- ONE COHERENT SNAPSHOT PER POLL ---------------------------------------------------------
    //
    // poll() used to seqlock-snapshot only the dimensions and the slot pointer, while
    // frame_host_sequence, frame_flat and frame_rendered_pose re-read the LIVE header afterwards. If
    // the game published in between, the host uploaded THIS frame's pixels and declared the NEXT
    // frame's pose -- a mismatch that grows with head speed, which is what was being seen as jitter.
    //
    // Adding rendered_orientation to the header fixed the VALUE (the old index -> pose lookup could
    // pick the wrong one) but not the INSTANT, so the same class of fault survived the fix. These
    // fields are captured inside the same window as the slot they describe, and the accessors below
    // return them rather than reading the live header again.
    uint32_t snap_slot{0};
    uint32_t snap_slot_generation{0};
    uint64_t payload_overwritten{0};  // times the writer wrapped into a slot we were uploading
    uint32_t snap_host_sequence{0};
    uint32_t snap_flat{0};
    uint32_t snap_rendered_valid{0};
    float snap_rendered_orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};

    uint32_t frame_host_sequence() const { return snap_host_sequence; }
    // The game asking to be shown FLAT -- see xr::SharedFrameHeader::flat. True while a menu is up.
    bool frame_flat() const { return snap_flat != 0u; }

    // THE POSE THE FRAME WAS ACTUALLY DRAWN FROM, stated by the writer rather than looked up here.
    // False when the writer could not recover it, in which case the sequence lookup stands.
    // The handshake block, or nullptr on a foreign layout. See FRAME_LOOP.md.
    xr::FrameHandshake* handshake() const {
        if (!layout_is_ours()) {
            return nullptr;
        }
        // const_cast because the reader owns a const view of the header but the handshake is the one
        // block the host WRITES through it -- the acks. The mapping itself is mapped writable.
        auto* base = reinterpret_cast<uint8_t*>(const_cast<xr::SharedFrameHeader*>(header));
        return reinterpret_cast<xr::FrameHandshake*>(base + xr::kHandshakeOffset);
    }

    bool frame_rendered_pose(XrQuaternionf& out) const {
        if (snap_rendered_valid == 0u) {
            return false;
        }
        out.x = snap_rendered_orientation[0];
        out.y = snap_rendered_orientation[1];
        out.z = snap_rendered_orientation[2];
        out.w = snap_rendered_orientation[3];
        return true;
    }

    // REVALIDATED ON EVERY USE, not just at open(). The section is a named kernel object that
    // outlives a mod reload, and CreateFileMapping hands the reloaded mod the SAME section -- so a
    // one-shot check at connect time could be satisfied by one build and then silently serve
    // another. Requires BOTH magic and version: magic alone accepts an unstamped or garbage
    // header, which is the state a section is in before any writer has touched it.
    //
    // The object names now carry the version too (xr::kSharedFrameName), which should make this
    // unreachable; it stays because the cost is one compare against a value already in cache and
    // the failure it prevents is reading another layout's bytes as pixels.
    bool layout_is_ours() const {
        return header != nullptr && header->magic == xr::kSharedFrameMagic &&
               header->version == xr::kSharedFrameVersion;
    }

    bool poll(uint32_t& w, uint32_t& h, uint32_t& pitch, const uint8_t*& bits) {
        if (!layout_is_ours()) {
            return false;
        }

        const uint32_t seq = header->sequence;

        if ((seq & 1u) != 0u || seq == last_sequence) {
            return false;  // mid-write, or nothing new
        }

        w = header->width;
        h = header->height;
        pitch = header->pitch;

        // FROM THE PUBLISHED SLOT'S OWN VIEW, not a fixed offset into one big one. The writer is
        // already filling a different slot, which is what makes it safe to upload straight out of
        // shared memory instead of copying it somewhere private first. `% kFrameSlots` matches the
        // masking slot_offset() used to do, so a torn or stale slot value still can't index past
        // the array of views.
        bits = static_cast<const uint8_t*>(frame_base[header->slot % xr::kFrameSlots]);

        // READ INTO LOCALS INSIDE THE WINDOW, COMMITTED ONLY IF IT VALIDATES. These used to be read
        // from the live header later in the frame, so a publish landing in between paired THIS
        // frame's pixels with the NEXT frame's pose and sequence -- an error that grows with head
        // speed, which is the jitter that was being chased.
        //
        // Locals rather than the members directly, because the members OUTLIVE a failed poll: the
        // host repeats the previous frame when poll returns false, and writing them before the
        // re-read would let a torn poll leave mixed metadata for that repeat to submit. That is the
        // same mispairing wearing a different hat.
        // THE PIXELS ARE NOT UNDER THE HEADER'S SEQLOCK -- see slot_generation. Refuse a slot whose
        // payload is mid-write, and remember the generation so the upload can be validated after.
        const uint32_t slot_now = header->slot % xr::kFrameSlots;
        const uint32_t gen = header->slot_generation[slot_now];
        if ((gen & 1u) != 0u) {
            return false;
        }
        const uint32_t hseq = header->host_sequence;
        const uint32_t flat = header->flat;
        const uint32_t rvalid = header->rendered_valid;
        float rorient[4];
        for (size_t i = 0; i < 4; ++i) {
            rorient[i] = header->rendered_orientation[i];
        }

        if (w == 0 || h == 0 || pitch == 0) {
            return false;
        }

        // Re-read: if the writer moved on while we looked, the fields may not describe the pixels.
        if (header->sequence != seq) {
            return false;
        }

        // VALIDATED. Commit the whole set together, so pose, sequence, flat flag and slot either
        // all describe these pixels or none of them are touched.
        snap_slot = slot_now;
        snap_slot_generation = gen;
        snap_host_sequence = hseq;
        snap_flat = flat;
        snap_rendered_valid = rvalid;
        for (size_t i = 0; i < 4; ++i) {
            snap_rendered_orientation[i] = rorient[i];
        }

        last_sequence = seq;
        return true;
    }

    // ---- THE UI LAYER, READ THE SAME WAY -------------------------------------------------------
    //
    // Same seqlock discipline as poll() above -- an odd sequence is a writer mid-update, and the
    // fields are trusted only if the sequence did not move between the first and second read.
    // `present` is checked here rather than treated as just another field: the mod is off by
    // default, and a stale sequence must never be mistaken for a published layer.
    bool poll_ui(uint32_t& w, uint32_t& h, uint32_t& pitch, uint32_t& derive_alpha,
                const uint8_t*& bits) {
        if (ui == nullptr || !layout_is_ours()) {
            return false;
        }

        const uint32_t seq = ui->sequence;

        if ((seq & 1u) != 0u || seq == last_ui_sequence) {
            return false;  // mid-write, or nothing new
        }

        if (ui->present == 0) {
            return false;
        }

        w = ui->width;
        h = ui->height;
        pitch = ui->pitch;
        derive_alpha = ui->derive_alpha;
        const uint32_t bytes = ui->bytes;
        bits = static_cast<const uint8_t*>(ui_base[ui->slot % xr::kUiSlots]);

        if (w == 0 || h == 0 || bytes == 0 || bytes > xr::kUiMaxBytes) {
            return false;
        }

        // Re-read: if the writer moved on while we looked, the fields may not describe the pixels.
        if (ui->sequence != seq) {
            return false;
        }

        last_ui_sequence = seq;
        return true;
    }

    // A live read of `present`, independent of the discipline above: used to notice the layer
    // being turned off promptly rather than waiting on the next unrelated sequence bump, and to
    // decide whether an already-uploaded image should still be shown this frame. A torn read here
    // costs at most one frame of stale visibility -- the PIXELS are only ever trusted through
    // poll_ui(), which does not have that luxury.
    bool ui_present() const {
        return ui != nullptr && layout_is_ours() && ui->present != 0u;
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

// ---- A CRASH HAS TO LEAVE SOMETHING BEHIND ------------------------------------------------------
//
// The mod has had a handler for a long time -- it writes a dump and a symbolised stack, and every
// crash it caught was diagnosed from that output. The host had NOTHING: it simply vanished, and the
// entire report available was "xr64 crashed".
//
// Module + offset rather than resolved names on purpose, matching the mod's format: the offset can
// be pasted straight into a disassembler against the matching binary, and it needs no symbol server
// or dbghelp initialisation at the worst possible moment. The PDB sits beside the exe for anything
// that wants more.
LONG WINAPI host_crash_handler(EXCEPTION_POINTERS* info) {
    const auto* rec = info != nullptr ? info->ExceptionRecord : nullptr;
    const auto* ctx = info != nullptr ? info->ContextRecord : nullptr;

    std::fprintf(stderr, "[host] ============== UNHANDLED EXCEPTION ==============\n");
    if (rec != nullptr) {
        std::fprintf(stderr, "[host] code 0x%08lX at %p\n",
                     static_cast<unsigned long>(rec->ExceptionCode), rec->ExceptionAddress);
    }

    // Walk the return addresses the cheap way. RtlCaptureStackBackTrace does not need the faulting
    // context to be intact, which a hand-rolled RBP walk would.
    void* frames[24]{};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH]{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod) &&
            GetModuleFileNameA(mod, name, MAX_PATH) != 0) {
            const char* base = std::strrchr(name, '\\');
            const auto off = reinterpret_cast<uintptr_t>(frames[i]) -
                             reinterpret_cast<uintptr_t>(mod);
            std::fprintf(stderr, "[host]   #%02u %p  %s+0x%llX\n", i, frames[i],
                         base != nullptr ? base + 1 : name,
                         static_cast<unsigned long long>(off));
        } else {
            std::fprintf(stderr, "[host]   #%02u %p  (no module)\n", i, frames[i]);
        }
    }
    (void)ctx;

    // The dump last: if writing it faults in turn, the stack above is already out.
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                      PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);
        if (auto* write = reinterpret_cast<WriteFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) {
            HANDLE f = CreateFileA("xr64_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                write(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal,
                      info != nullptr ? &mei : nullptr, nullptr, nullptr);
                CloseHandle(f);
                std::fprintf(stderr, "[host] dump written to xr64_crash.dmp\n");
            }
        }
    }

    std::fprintf(stderr, "[host] =================================================\n");
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- WHAT THE HEADSET SHOWS WHEN THERE IS NO FRAME ---------------------------------------------
//
// Skipping the upload leaves the swapchain image holding whatever was in that memory, which the
// compositor happily shows: flickering garbage colours. Uninitialised is not "nothing" -- it is
// noise, and it is unpleasant to wear.
//
// So paint something deliberate instead: black, with FEAR2VR spelled out so it is obvious the host
// is alive and simply has no picture yet (opening movies, main menu, or a stalled game).
//
// A 5x7 bitmap font, drawn by hand rather than pulled in as a dependency. Seven glyphs is less code
// than any font library's initialisation, and this must work before anything else does.
namespace placeholder {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

// Row bits, MSB-left across 5 columns. F E A R 2 V R
constexpr uint8_t kGlyphs[7][kGlyphH] = {
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
};

// BGRA, black background, dim so it is restful to look at rather than a bright slab.
void build(std::vector<uint8_t>& out, uint32_t w, uint32_t h) {
    out.assign(static_cast<size_t>(w) * h * 4u, 0u);
    if (w < 64 || h < 32) {
        return;
    }
    // Scale the word to roughly a quarter of the eye's width, and keep it whole pixels so the
    // glyphs stay crisp instead of shimmering.
    const int glyphs = 7;
    const int spacing = 1;
    const int cells = glyphs * (kGlyphW + spacing) - spacing;
    int scale = static_cast<int>(w / 4u) / cells;
    if (scale < 1) {
        scale = 1;
    }
    const int text_w = cells * scale;
    const int text_h = kGlyphH * scale;
    const int ox = (static_cast<int>(w) - text_w) / 2;
    const int oy = (static_cast<int>(h) - text_h) / 2;

    for (int g = 0; g < glyphs; ++g) {
        const int gx = ox + g * (kGlyphW + spacing) * scale;
        for (int row = 0; row < kGlyphH; ++row) {
            const uint8_t bits = kGlyphs[g][row];
            for (int col = 0; col < kGlyphW; ++col) {
                if ((bits & (1u << (kGlyphW - 1 - col))) == 0u) {
                    continue;
                }
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        const int px = gx + col * scale + sx;
                        const int py = oy + row * scale + sy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(w) ||
                            py >= static_cast<int>(h)) {
                            continue;
                        }
                        auto* p = &out[(static_cast<size_t>(py) * w + px) * 4u];
                        p[0] = 0x60;  // B
                        p[1] = 0x50;  // G
                        p[2] = 0x40;  // R
                        p[3] = 0xFF;
                    }
                }
            }
        }
    }
}

} // namespace placeholder

// ---- ACK THE GAME'S FRAME REQUEST -------------------------------------------------------------
//
// The host does not need to become request-DRIVEN to give the game a real OpenXR frame loop: it
// already passes through Wait, Begin and End once per iteration, so acking the game's pending
// request AT each of those points lock-steps the game to those stages. The game blocks on the WAIT
// ack, so xrWaitFrame -- which throttles whoever blocks on it -- transitively throttles the GAME.
// That is the whole point: pacing and ASW then apply to the process doing the work.
//
// Writes under the block's own seqlock and only ever ADVANCES the ack; a game waiting for
// (phase, id) either sees it or keeps waiting. Never fabricates an ack for a request that was not
// made -- `phase == kPhaseIdle` means nobody is asking, and acking anyway would let a game race
// ahead of a frame the runtime has not begun.
// ---- WAIT FOR THE GAME TO ASK, THEN CALL ------------------------------------------------------
//
// Acking opportunistically is not enough and was wrong in the first cut: if the host merely acks
// whatever request happens to be posted when it passes a stage, it will usually pass xrBeginFrame
// BEFORE the game has asked, and then ack that request against a different OpenXR frame. The game's
// work would be paired with the wrong frame -- the exact class of bug the tagged id exists to make
// impossible.
//
// So each stage blocks for its own (phase, id) first. The host stalling while the game is slow is
// the POINT: the frame genuinely takes longer, xrEndFrame lands late, and the runtime throttles the
// next xrWaitFrame -- which the game is blocked in. That is how pacing and ASW start applying to the
// process doing the work.
//
// BOUNDED, and a timeout means "this game is not driving us": returns false and the caller
// free-runs, so a mod without the handshake (or one mid-reload) cannot hang the compositor.
// IDENTITY, NOT PHASE. Testing `ack_phase != phase` looks right and is broken from the SECOND frame
// on: frame 2 asks for WAIT again while ack_phase is still WAIT from frame 1, so the stage would
// wait out its whole timeout and then serve a request it had already served. Three stages x 100 ms
// per frame, and an ack landing on the wrong id. The request is (phase, id) and so is the test.
//
// Returns the id it matched, so the caller acks THAT rather than re-reading a field the game may
// already have advanced.
static bool handshake_await(xr::FrameHandshake* hs, uint32_t phase, uint32_t timeout_ms,
                            uint32_t* id_out) {
    if (hs == nullptr) {
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        // Coherent snapshot: phase and id must come from the same post, or a stage can pair this
        // frame's phase with the previous frame's id.
        // PER-STAGE SLOT. A request is outstanding while req_id != ack_slot for THIS stage, which is
        // what lets END(N) and WAIT(N+1) both be pending without either overwriting the other. The
        // aligned 32-bit reads cannot tear, and the two fields belong to one stage, so no seqlock is
        // needed to relate them -- unlike the ack payload, which still travels under the block's own.
        const uint32_t id = hs->req_id[phase];
        const bool coherent = true;

        if (coherent && id != hs->ack_slot[phase]) {
            if (id_out != nullptr) {
                *id_out = id;
            }
            return true;  // asked for, and not already served for THIS id
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(0);  // the game is on another core and about to post; do not burn a whole slice
    }
}

static void handshake_ack(xr::FrameHandshake* hs, uint32_t phase, XrResult status,
                          const XrFrameState* fs, const XrPosef* head, uint32_t begun_id,
                          uint32_t id) {
    if (hs == nullptr) {
        return;
    }

    hs->sequence |= 1u;
    MemoryBarrier();
    if (fs != nullptr) {
        hs->predicted_display_time = fs->predictedDisplayTime;
        hs->predicted_period_ns = static_cast<uint32_t>(fs->predictedDisplayPeriod);
        hs->should_render = fs->shouldRender != XR_FALSE ? 1u : 0u;
    }
    if (head != nullptr) {
        hs->head_orientation[0] = head->orientation.x;
        hs->head_orientation[1] = head->orientation.y;
        hs->head_orientation[2] = head->orientation.z;
        hs->head_orientation[3] = head->orientation.w;
        hs->head_position[0] = head->position.x;
        hs->head_position[1] = head->position.y;
        hs->head_position[2] = head->position.z;
    }
    hs->begun_id = begun_id;
    hs->ack_status = static_cast<int32_t>(status);
    hs->ack_id = id;
    hs->ack_phase = phase;
    MemoryBarrier();
    hs->ack_slot[phase] = id;  // LAST: this is what releases the waiter for this stage
    MemoryBarrier();
    hs->sequence = (hs->sequence + 1u) & ~1u;
}

// ---- THE FRAME TRACE RING (--trace-frames) ------------------------------------------------------
//
// POD, no allocation, no I/O at capture time, and no flush inside the frame loop: the thing being
// measured is frame TIMING, so the instrument must not cost a frame. Dumped exactly once, after the
// loop has exited.
struct TraceSample {
    double iv, wait_blk, begin_dur, period_ms, content_cost, end_dur, end_req_wait;
    double aw_wait, aw_begin, aw_end, post_end, eye_upload, actions;
    uint32_t seq, layers, new_content;
    int32_t begin_r, end_r;
};
constexpr size_t kTraceCap = 8192;  // ~113 s at 72 Hz; a wrapped ring keeps the most recent frames
static TraceSample g_trace[kTraceCap]{};
static size_t g_trace_n = 0;

// OLDEST FIRST, even when wrapped, so the rows read as a timeline rather than as two halves spliced
// at an arbitrary point. A wrapped ring holds the LAST kTraceCap frames and n counts from the
// oldest retained sample, not from session start -- the absolute frame number is not what a
// transition is read by.
static void trace_flush() {
    if (g_trace_n == 0) {
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, "xr64_trace.csv", "wb") != 0 || f == nullptr) {
        std::printf("[trace] could not open xr64_trace.csv -- %zu samples discarded\n", g_trace_n);
        return;
    }
    std::fprintf(f, "n,iv_ms,wait_blocked_ms,begin_ms,period_ms,content_ms,end_req_wait_ms,"
                    "end_ms,aw_wait_ms,aw_begin_ms,aw_end_ms,post_end_ms,eye_upload_ms,actions_ms,"
                    "seq,layers,new_content,"
                    "begin_r,end_r\n");
    const bool wrapped = g_trace_n > kTraceCap;
    const size_t count = wrapped ? kTraceCap : g_trace_n;
    const size_t first = wrapped ? g_trace_n % kTraceCap : 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& s = g_trace[(first + i) % kTraceCap];
        std::fprintf(f,
                     "%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                     "%u,%u,%u,%d,%d\n",
                     i, s.iv, s.wait_blk, s.begin_dur, s.period_ms, s.content_cost, s.end_req_wait,
                     s.end_dur, s.aw_wait, s.aw_begin, s.aw_end, s.post_end,
                     s.eye_upload, s.actions, s.seq, s.layers,
                     s.new_content, s.begin_r, s.end_r);
    }
    std::fclose(f);
    std::printf("[trace] wrote %zu samples to xr64_trace.csv%s\n", count,
                wrapped ? " (most recent; ring wrapped)" : "");
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(host_crash_handler);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(console_handler, TRUE);

    bool probe_only = false;
    int max_seconds = 0;
    bool ui_enabled = true;
    bool ui_world_fixed = false;         // --no-ui disables the UI quad layer entirely
    float ui_distance_m = 1.8f;     // --ui-distance <metres>, along -Z from the reference space origin
    // How long to wait for a NEW game frame before giving up and re-showing the last one. This is
    // what lets the runtime observe the game's real rate and engage its own reprojection; see the
    // wait itself. Zero restores the old always-submit behaviour for an A/B.
    bool no_crop = false;  // submit the whole rendered picture with its symmetric FOV
    // Default is the pre-cadence bootstrap only: once the runtime has stated a period the
    // bound is derived from it (two periods), unless --content-wait said otherwise.
    uint32_t content_wait_ms = 12;
    bool content_wait_explicit = false;
    bool content_wait_derive = false;  // --content-wait-derive, see the bound below
    // ---- ONE LINE PER FRAME, FOR THE 36<->60 TRANSITION --------------------------------------
    //
    // The oscillation has two candidate causes that call for opposite fixes -- a variable host loop
    // period, or feedback through the runtime's ASW -- and six earlier fixes in this subsystem were
    // judged by feel and were wrong. This is the log that separates them: if
    // predictedDisplayPeriod never moves while the overlay reports 36, feedback through the runtime
    // is impossible and the loop period is the whole story.
    //
    // Off by default because it is one line per frame at 72 Hz.
    bool trace_frames = false;  // --trace-frames
    // ---- DELIBERATELY SUBMIT AN OLDER POSE ------------------------------------------------------
    //
    // The diagnostic for "does pose age matter". A projection layer declares the pose its image was
    // rendered from, and the compositor is supposed to warp that image to wherever the head
    // actually is at scanout. If it does, adding age changes NOTHING you can see -- the correction
    // simply gets bigger. If the picture starts swimming as this goes up, the layer is NOT being
    // reprojected and the 1.3 frames of age we already have is being shown raw.
    //
    // Steps, not milliseconds: one step is one published pose.
    uint32_t pose_lag_steps = 0;
    // ---- HOLD THE PIPELINE AT A CONSTANT DEPTH --------------------------------------------------
    //
    // Measured at the wall: the submitted frame's pose age alternates 1, 2, 1, 2 -- half the frames
    // each -- because the game's frame time straddles the host's period there, so sometimes its
    // frame lands before the next pick and sometimes after.
    //
    // A CONSTANT age is invisible: every frame is warped by the same amount and the picture is
    // merely late. An ALTERNATING age changes the warp by a whole step every frame, which is the
    // scene stepping back and forth and the black edges jumping with it.
    //
    // So a frame that arrives EARLY is held back one step rather than shown immediately. That costs
    // a fixed extra step of latency and removes the variance, which is the trade the eye wants:
    // smooth and late beats sharp and juddering.
    //
    // 0 disables it.
    float ui_width_m = 1.6f;        // --ui-width <metres>, height follows the UI image's aspect ratio

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            probe_only = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            max_seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pose-lag") == 0 && i + 1 < argc) {
            pose_lag_steps = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            // ---- THE HOST HAD NO LOG, WHICH IS WHY THE RUNTIME SIDE WAS BLIND ----------------
            //
            // Everything here goes to stdout and dies with the console, so the ONE component that
            // actually talks to the runtime left no record -- while the mod's side has been fully
            // logged all along. Chasing a fault that only reproduces on one runtime, that is the
            // wrong half to be able to read.
            //
            // Line-buffered so a hang or a crash still leaves everything up to that moment on disk.
            if (std::freopen(argv[++i], "w", stdout) != nullptr) {
                std::setvbuf(stdout, nullptr, _IOLBF, 4096);
            }
        } else if (std::strcmp(argv[i], "--no-crop") == 0) {
            no_crop = true;
        } else if (std::strcmp(argv[i], "--content-wait") == 0 && i + 1 < argc) {
            content_wait_ms = static_cast<uint32_t>(std::atoi(argv[++i]));
            content_wait_explicit = true;
        } else if (std::strcmp(argv[i], "--content-wait-derive") == 0) {
            content_wait_derive = true;
        } else if (std::strcmp(argv[i], "--trace-frames") == 0) {
            trace_frames = true;
        } else if (std::strcmp(argv[i], "--no-ui") == 0) {
            ui_enabled = false;
        } else if (std::strcmp(argv[i], "--ui-distance") == 0 && i + 1 < argc) {
            ui_distance_m = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--ui-width") == 0 && i + 1 < argc) {
            ui_width_m = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--ui-world-fixed") == 0) {
            ui_world_fixed = true;  // pin the HUD to the room instead of to the head
        }
    }

    // ---- ASK WHAT THE RUNTIME HAS BEFORE NAMING IT ----------------------------------------------
    //
    // xrCreateInstance fails OUTRIGHT with XR_ERROR_EXTENSION_NOT_PRESENT if any name in the list
    // is one the runtime does not have -- so an optional extension cannot simply be listed and
    // hoped for. Naming XR_KHR_generic_controller on a runtime without it would cost the entire
    // session rather than one interaction profile.
    uint32_t ext_count = 0;
    XrResult ext_r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});

    if (XR_SUCCEEDED(ext_r) && ext_count > 0) {
        ext_r = xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    }

    if (XR_FAILED(ext_r)) {
        // Not fatal: the D3D11 extension below is not optional and its absence will be reported by
        // xrCreateInstance itself. Everything else degrades to "not available".
        std::printf("[host] xrEnumerateInstanceExtensionProperties -> %s -- optional extensions "
                    "treated as absent\n", rs(ext_r));
        exts.clear();
    }

    auto have_extension = [&](const char* name) {
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, name) == 0) {
                return true;
            }
        }

        return false;
    };

    // Both are interaction-profile extensions and nothing else -- they add no functions and no
    // structs, only the right to suggest bindings for a profile this host would otherwise have to
    // watch fall back to khr/simple_controller. See the suggest_bindings block for what each one
    // buys.
    const bool have_generic_controller = have_extension(XR_KHR_GENERIC_CONTROLLER_EXTENSION_NAME);
    const bool have_bytedance_controller =
        have_extension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);

    if (have_generic_controller) {
        enabled.push_back(XR_KHR_GENERIC_CONTROLLER_EXTENSION_NAME);
    }

    if (have_bytedance_controller) {
        enabled.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }

    std::printf("[host] %u extension(s) offered; generic controller %s, bytedance controller %s\n",
                ext_count, have_generic_controller ? "yes" : "no",
                have_bytedance_controller ? "yes" : "no");

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "FEAR2VR");
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "LithTech Jupiter EX");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();

    XrResult r = xrCreateInstance(&ici, &g_instance);
    std::printf("[host] xrCreateInstance -> %s\n", rs(r));

    if (XR_FAILED(r)) {
        return 1;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    const XrResult props_r = xrGetInstanceProperties(g_instance, &props);

    // `props` is value-initialised, so an unchecked failure prints an empty runtime name at
    // version 0.0.0 -- a line that looks like a real answer and sends whoever reads the log
    // hunting for a runtime that does not exist. Not fatal: nothing below depends on the name.
    if (XR_SUCCEEDED(props_r)) {
        std::printf("[host] runtime '%s' %llu.%llu.%llu\n", props.runtimeName,
                    static_cast<unsigned long long>(XR_VERSION_MAJOR(props.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_MINOR(props.runtimeVersion)),
                    static_cast<unsigned long long>(XR_VERSION_PATCH(props.runtimeVersion)));
    } else {
        std::printf("[host] xrGetInstanceProperties -> %s (runtime name and version unknown)\n",
                    rs(props_r));
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(g_instance, &sgi, &system);
    std::printf("[host] xrGetSystem -> %s\n", rs(r));

    if (XR_FAILED(r)) {
        std::printf("[host] no headset. Connect one and try again.\n");
        return 1;
    }

    // ---- WHICH VIEW CONFIGURATIONS THIS SYSTEM ACTUALLY HAS -------------------------------------
    //
    // PRIMARY_STEREO was assumed. The spec promises only that "for any supported form factor, a
    // system will support one or more primary view configurations" -- not that stereo is among
    // them. On a system without it every enumerate and locate below returns zero views and this
    // host runs on with view_count == 0: no swapchains, no layers, no error, nothing on screen.
    //
    // Stereo stays the ONLY configuration driven here, because the whole loop is two-eyed. What
    // changes is that "not supported" is now a named failure listing what the system does offer,
    // instead of a silent zero.
    uint32_t view_config_count = 0;
    XrResult vc_r =
        xrEnumerateViewConfigurations(g_instance, system, 0, &view_config_count, nullptr);
    std::vector<XrViewConfigurationType> view_configs(view_config_count);

    if (XR_SUCCEEDED(vc_r) && view_config_count > 0) {
        vc_r = xrEnumerateViewConfigurations(g_instance, system, view_config_count,
                                             &view_config_count, view_configs.data());
    }

    if (XR_FAILED(vc_r)) {
        std::printf("[host] xrEnumerateViewConfigurations -> %s\n", rs(vc_r));
        return 1;
    }

    bool have_stereo = false;

    for (XrViewConfigurationType t : view_configs) {
        std::printf("[host] view configuration %s\n", view_config_name(t));
        have_stereo = have_stereo || t == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }

    if (!have_stereo) {
        std::printf("[host] this system offers %u view configuration(s), none of them\n"
                    "[host] PRIMARY_STEREO -- which is the only one this host drives\n",
                    view_config_count);
        return 1;
    }

    // ---- WHAT THE COMPOSITOR WILL ACCEPT AT xrEndFrame ------------------------------------------
    //
    // OPAQUE was hardcoded and never enumerated. "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED must
    // be returned if and only if the XrFrameEndInfo::environmentBlendMode was not enumerated by
    // xrEnumerateEnvironmentBlendModes for the XrInstance and XrSystemId used to create session"
    // -- so on an additive-only or alpha-blend-only system, which is what passthrough-first
    // hardware reports, EVERY xrEndFrame failed and nothing was ever presented.
    //
    // OPAQUE is still the PREFERENCE: this is a fully-rendered game, and the runtime's own first
    // choice is not necessarily the one that hides the room.
    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t blend_count = 0;
    XrResult blend_r = xrEnumerateEnvironmentBlendModes(
        g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count, nullptr);
    std::vector<XrEnvironmentBlendMode> blend_modes(blend_count);

    if (XR_SUCCEEDED(blend_r) && blend_count > 0) {
        blend_r = xrEnumerateEnvironmentBlendModes(g_instance, system,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   blend_count, &blend_count, blend_modes.data());

        // RETRY ON SIZE_INSUFFICIENT. Observed live against Meta XR Simulator: the count call
        // answered 1, the fill call then demanded 2 and failed --
        //
        //   xrEnumerateEnvironmentBlendModes -> XR_ERROR_SIZE_INSUFFICIENT (2)
        //   environment blend mode OPAQUE, from 2 offered:
        //     UNKNOWN
        //
        // so the modes were never read and OPAQUE went out unverified, with a bogus "UNKNOWN" in
        // the log. The two-call idiom has to tolerate the count growing between the calls; the
        // failure writes the required size, so one retry is enough.
        if (blend_r == XR_ERROR_SIZE_INSUFFICIENT && blend_count > blend_modes.size()) {
            blend_modes.resize(blend_count);
            blend_r = xrEnumerateEnvironmentBlendModes(g_instance, system,
                                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       blend_count, &blend_count,
                                                       blend_modes.data());
        }
        blend_modes.resize(blend_count);
    }

    if (XR_FAILED(blend_r) || blend_modes.empty()) {
        std::printf("[host] xrEnumerateEnvironmentBlendModes -> %s (%u) -- submitting OPAQUE "
                    "unverified\n", rs(blend_r), blend_count);
    } else {
        bool have_opaque = false;

        for (XrEnvironmentBlendMode m : blend_modes) {
            have_opaque = have_opaque || m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        }

        // Element 0 is the runtime's own preference order, which is the right thing to fall back
        // on when the mode we want is simply not offered.
        blend_mode = have_opaque ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blend_modes[0];
    }

    std::printf("[host] environment blend mode %s, from %u offered:\n", blend_mode_name(blend_mode),
                blend_count);

    for (XrEnvironmentBlendMode m : blend_modes) {
        std::printf("[host]   %s\n", blend_mode_name(m));
    }

    // What the runtime wants each eye rendered at. Reported rather than chosen: this is the number a
    // supersampling multiplier would later scale.
    uint32_t view_count = 0;
    XrResult vcv_r = xrEnumerateViewConfigurationViews(
        g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count,
                                                      {XR_TYPE_VIEW_CONFIGURATION_VIEW});

    if (XR_SUCCEEDED(vcv_r) && view_count > 0) {
        vcv_r = xrEnumerateViewConfigurationViews(g_instance, system,
                                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                  view_count, &view_count, config_views.data());
    }

    // EXACTLY TWO, checked rather than assumed. Everything downstream is stereo-shaped -- hv[0]
    // and hv[1], screen[2], proj_views[2], the IPD taken as the distance between the two -- so any
    // other count is an out-of-range read, not a degraded mode worth limping along in. Unchecked,
    // a failed enumerate left view_count at 0 and the host carried on with no swapchains, no
    // layers and no complaint.
    if (XR_FAILED(vcv_r) || view_count != 2) {
        std::printf("[host] xrEnumerateViewConfigurationViews -> %s, %u view(s) -- PRIMARY_STEREO "
                    "must report exactly 2\n", rs(vcv_r), view_count);
        return 1;
    }

    // ---- TELL THE MOD WHAT THE HEADSET ASKED FOR ------------------------------------------------
    //
    // The 32-bit mod has to size the engine's scene target BEFORE the renderer builds it, which is
    // seconds after injection and long before this process could answer an HTTP request -- and the
    // mod may be injected into a game that started without a host at all. So the number is left on
    // disk rather than served: the launcher starts this process first, and by the time the game is
    // up the file is already there.
    //
    // LOCALAPPDATA because it is the one directory both bitnesses can name without either knowing
    // where the other is installed.
    if (view_count > 0) {
        if (const char* local = std::getenv("LOCALAPPDATA")) {
            char dir[MAX_PATH];
            std::snprintf(dir, sizeof(dir), "%s\\fear2vr", local);
            CreateDirectoryA(dir, nullptr);
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\runtime.ini", dir);
            if (FILE* fp = std::fopen(path, "w")) {
                std::fprintf(fp,
                             "; Written by xr64.exe. The per-eye size THIS headset and runtime ask\n"
                             "; for. The mod reads it to size the engine's scene target.\n"
                             "[render]\nper_eye_width=%u\nper_eye_height=%u\n",
                             config_views[0].recommendedImageRectWidth,
                             config_views[0].recommendedImageRectHeight);
                std::fclose(fp);
                std::printf("[host] published recommended size to %s\n", path);
            }
        }
    }

    for (uint32_t i = 0; i < view_count; ++i) {
        std::printf("[host] view %u recommended %ux%u (max %ux%u), %u sample(s)\n", i,
                    config_views[i].recommendedImageRectWidth,
                    config_views[i].recommendedImageRectHeight,
                    config_views[i].maxImageRectWidth, config_views[i].maxImageRectHeight,
                    config_views[i].recommendedSwapchainSampleCount);
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    const XrResult proc_r =
        xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));

    // BOTH the result AND the pointer. An extension entry point that did not resolve leaves this
    // null and the call below was made through it unconditionally -- a call through a null
    // function pointer, which faults at address zero instead of returning an error anyone could
    // read. XR_KHR_D3D11_enable is what makes this host possible at all, so a miss is fatal.
    if (XR_FAILED(proc_r) || get_reqs == nullptr) {
        std::printf("[host] xrGetInstanceProcAddr xrGetD3D11GraphicsRequirementsKHR -> %s%s -- the "
                    "D3D11 extension did not take\n", rs(proc_r),
                    get_reqs == nullptr ? " (null pointer)" : "");
        return 1;
    }

    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = get_reqs(g_instance, system, &reqs);
    std::printf("[host] graphics requirements -> %s\n", rs(r));

    // `reqs` is value-initialised, so a failure means an all-zero adapter LUID -- which
    // create_device_on() below would match against no adapter at all, reporting nothing but
    // "d3d11 device 0000000000000000" for a cause that is three calls upstream.
    if (XR_FAILED(r)) {
        return 1;
    }

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Device* device = create_device_on(reqs.adapterLuid, reqs.minFeatureLevel, &ctx);
    std::printf("[host] d3d11 device %p\n", static_cast<void*>(device));

    if (device == nullptr) {
        return 1;
    }

    if (probe_only) {
        std::printf("[host] probe only -- not creating a session\n");

        // Release what this function created. The probe builds a D3D11 device to prove the
        // runtime's LUID and feature level are satisfiable, then leaves; these were simply leaked
        // before, which is harmless for a process about to exit but is still this function's mess.
        //
        // NOT a fix for anything. The runtime never receives this device -- it is only ever handed
        // over through XrGraphicsBindingD3D11KHR at xrCreateSession, which the probe skips, and
        // xrGetD3D11GraphicsRequirementsKHR reports a LUID without taking a reference. So this
        // ordering cannot affect xrDestroyInstance, and if the probe still fails to exit (the Meta
        // XR Simulator did, after printing everything below), the cause is elsewhere.
        if (ctx != nullptr) {
            ctx->Release();
        }
        device->Release();

        // Result ignored, like every other destroy in this file: the only failures xrDestroy*
        // can report are an invalid handle or a lost instance, and the process is exiting in
        // either case. There is nothing a caller could do differently.
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
    XrResult fmt_r = xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);

    if (XR_SUCCEEDED(fmt_r) && format_count > 0) {
        fmt_r = xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    }

    // An empty list means no swapchain can be created at all, and the preference walk below would
    // otherwise settle on a `chosen_format` of 0 and hand that to xrCreateSwapchain as though it
    // were a DXGI format.
    if (XR_FAILED(fmt_r) || formats.empty()) {
        std::printf("[host] xrEnumerateSwapchainFormats -> %s (%u) -- no swapchain is possible\n",
                    rs(fmt_r), format_count);
        return 1;
    }

    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    int64_t chosen_format = formats[0];

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

    // EVERY format, named. The runtime skips a blit when our image's centre pixel reads zero
    // ("[DIAG-ZEROSKIP] Skipping blit: source image center pixel is zero"), and it reads zero
    // because the game's back buffer is X8R8G8B8 -- alpha bits undefined, in practice 0 -- copied
    // straight into an ALPHA format. A format that ignores alpha would fix that for nothing, so it
    // has to be known whether one is offered rather than assumed either way.
    for (uint32_t fi = 0; fi < format_count && fi < formats.size(); ++fi) {
        const char* name = "?";
        switch (formats[fi]) {
            case 28: name = "R8G8B8A8_UNORM"; break;
            case 29: name = "R8G8B8A8_UNORM_SRGB"; break;
            case 87: name = "B8G8R8A8_UNORM"; break;
            case 91: name = "B8G8R8A8_UNORM_SRGB"; break;
            case 88: name = "B8G8R8X8_UNORM (no alpha)"; break;
            case 93: name = "B8G8R8X8_UNORM_SRGB (no alpha)"; break;
            case 10: name = "R16G16B16A16_FLOAT"; break;
            case 24: name = "R10G10B10A2_UNORM"; break;
            default: break;
        }
        std::printf("[host]   format %lld = %s\n", static_cast<long long>(formats[fi]), name);
    }

    // ---- NO PER-EYE SWAPCHAINS: THE SCREEN CHAINS ARE THE ONLY ONES --------------------------
    //
    // A second set used to be created here, at the runtime's recommended size, for a startup test
    // pattern. The pattern is gone (the black FEAR2VR placeholder covers "no game frame yet" and
    // paints into the screen chains), but merely CREATING these was still fatal on Meta XR
    // Simulator:
    //
    //   [host] eye 0 swapchain 1440x1584 -> XR_SUCCESS      <- claims view 0
    //   [host] eye 1 swapchain 1440x1584 -> XR_SUCCESS      <- claims view 1
    //   [sim]  Replacing existing color swapchain (viewIndex=0) - cleaning up previous swapchain
    //
    // The simulator associates swapchains with view indices in CREATION ORDER, so the first two
    // claimed views 0 and 1; when the real screen chains were created they replaced them, and the
    // compositor destroyed the previous pair and stopped compositing for the rest of the session.
    //
    // That is why the menu looked fine and only the world was frozen: the menu the wearer sees is
    // the UI QUAD, on its own swapchain. The projection layer had been dead since startup -- there
    // was simply nothing in it to notice until a world put content there.
    //
    // Nothing in the spec ties a swapchain to a view before it is submitted, so this is a runtime
    // behaviour to avoid rather than a rule we were breaking. Avoiding it is free: these were
    // unused.
    std::vector<Eye> eyes(view_count);


    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    r = xrCreateReferenceSpace(session, &rsci, &space);
    std::printf("[host] reference space -> %s\n", rs(r));

    // FATAL, not merely reported. This handle flows into every xrLocateViews, every xrLocateSpace
    // and every layer submitted below, so a null one is XR_ERROR_HANDLE_INVALID on each of them,
    // ninety times a second, forever. The result used to be printed and then ignored -- which is
    // the shape of failure that gets diagnosed as a rendering bug.
    if (XR_FAILED(r) || space == XR_NULL_HANDLE) {
        std::printf("[host] no LOCAL reference space -- nothing can be located or submitted\n");
        return 1;
    }

    // ---- A HEAD-LOCKED SPACE, FOR THE UI QUAD ONLY ------------------------------------------
    //
    // VIEW space is defined as the viewer's own frame, so a layer posed in it follows the head
    // with no per-frame relocation by this code. That is what a HUD has to do: the health bar and
    // the ammo counter are instrument-panel furniture, and one pinned to a spot in the ROOM is
    // behind the wearer the moment they turn around -- which is what posing the quad in LOCAL
    // space gives you, however stable it looks while facing forward.
    //
    // Only the UI quad uses this. The world layers stay in LOCAL, where they belong.
    XrReferenceSpaceCreateInfo vsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace view_space = XR_NULL_HANDLE;
    r = xrCreateReferenceSpace(session, &vsci, &view_space);
    std::printf("[host] view (head-locked) space -> %s\n", rs(r));

    // Fatal for the same reason: the head pose handed to the mod is an xrLocateSpace on THIS
    // space (see the frame loop), and the settings panel poses its quad in it. A host that runs
    // without it publishes no head orientation at all, which is worse than not starting.
    if (XR_FAILED(r) || view_space == XR_NULL_HANDLE) {
        std::printf("[host] no VIEW reference space -- the head pose and the settings panel both "
                    "depend on it\n");
        return 1;
    }

    // ---- CONTROLLERS ------------------------------------------------------------------------
    //
    // One action set, created and attached once. Each action exists ONCE with both subaction
    // paths bound to it rather than once per hand -- that is how OpenXR itself models "the same
    // input, on either hand", and it halves the bookkeeping below since xrGetActionState* takes
    // the hand as a parameter rather than needing a second action.
    XrPath hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};  // xr::kHandLeft, xr::kHandRight
    const XrResult left_hand_path_r =
        xrStringToPath(g_instance, "/user/hand/left", &hand_path[xr::kHandLeft]);
    const XrResult right_hand_path_r =
        xrStringToPath(g_instance, "/user/hand/right", &hand_path[xr::kHandRight]);

    // A null subaction path is XR_ERROR_PATH_INVALID on every xrCreateAction, every
    // xrGetActionState* and every haptic call below -- hundreds of failures a second out of one
    // silent miss here.
    if (XR_FAILED(left_hand_path_r) || XR_FAILED(right_hand_path_r)) {
        std::printf("[host] xrStringToPath /user/hand/{left,right} -> %s / %s -- no controller "
                    "input is possible\n", rs(left_hand_path_r), rs(right_hand_path_r));
        return 1;
    }

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

    // OUTPUT, and the only action in this set that this process WRITES rather than reads. It is
    // created here with the others so it is attached by the same xrAttachSessionActionSets:
    // xrApplyHapticFeedback on an unattached action is XR_ERROR_ACTIONSET_NOT_ATTACHED.
    XrAction haptic_action = XR_NULL_HANDLE;

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
    create_action("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, haptic_action);

    // Suggesting a path a profile does not expose fails the WHOLE call with
    // XR_ERROR_PATH_UNSUPPORTED -- the classic way to end up with no bindings at all and no clue
    // why. So every profile gets its own call, logged with its own result, and only the paths that
    // profile actually has.
    auto suggest_bindings =
        [&](const char* profile_path_str,
           const std::vector<std::pair<XrAction, const char*>>& bindings) {
            XrPath profile_path = XR_NULL_PATH;
            const XrResult profile_r = xrStringToPath(g_instance, profile_path_str, &profile_path);

            if (XR_FAILED(profile_r)) {
                std::printf("[host] suggest bindings '%s' -> profile path rejected (%s)\n",
                            profile_path_str, rs(profile_r));
                return profile_r;
            }

            std::vector<XrActionSuggestedBinding> suggestions;
            suggestions.reserve(bindings.size());

            for (const auto& b : bindings) {
                XrPath p = XR_NULL_PATH;
                const XrResult path_r = xrStringToPath(g_instance, b.second, &p);

                // Dropped rather than passed on as XR_NULL_PATH: a path the runtime will not even
                // parse would fail the whole call for the wrong reason, and the profile would get
                // blamed for what is a typo in the string above.
                if (XR_FAILED(path_r)) {
                    std::printf("[host] suggest bindings '%s': path '%s' -> %s, dropped\n",
                                profile_path_str, b.second, rs(path_r));
                    continue;
                }

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
            {haptic_action, "/user/hand/left/output/haptic"},
            {haptic_action, "/user/hand/right/output/haptic"},
        });

    // FALLBACK OF LAST RESORT: khr/simple_controller is the one profile every conformant runtime
    // supports, so this guarantees poses and a trigger even with nothing else configured -- select
    // doubles as trigger, and there is no squeeze, stick or face buttons. Everything between here
    // and the Touch block above exists so that this is not what a Vive, an Index, a WMR headset or
    // a Pico actually lands on, because landing here means no locomotion and no turning.
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
                         {haptic_action, "/user/hand/left/output/haptic"},
                         {haptic_action, "/user/hand/right/output/haptic"},
                     });

    // ---- THE GENERIC FALLBACK, WHERE THE RUNTIME OFFERS ONE ------------------------------------
    //
    // XR_KHR_generic_controller exists for exactly the case this host kept hitting: a runtime
    // whose hardware nobody named, falling back to simple_controller, which has no thumbstick, no
    // squeeze and no face buttons -- the whole of this mod's locomotion and turning, gone.
    //
    // Suggested only when the extension actually took (see the scan before xrCreateInstance):
    // without it the profile path does not exist and the call fails as a whole.
    //
    // NO MENU HERE. The profile does not define /input/menu/click -- the extension lists Touch's
    // menu among the paths with "no generic controller equivalent" -- so the long-press that opens
    // the settings panel is unreachable on a runtime that lands here, and asking for the path
    // anyway would take every other binding down with it.
    if (have_generic_controller) {
        suggest_bindings(
            "/interaction_profiles/khr/generic_controller",
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
                // primary/secondary onto the same buttons the extension's own equivalence table
                // gives for Touch: left primary is X, left secondary is Y, right primary is A,
                // right secondary is B.
                {x_click_action, "/user/hand/left/input/primary/click"},
                {y_click_action, "/user/hand/left/input/secondary/click"},
                {a_click_action, "/user/hand/right/input/primary/click"},
                {b_click_action, "/user/hand/right/input/secondary/click"},
                {haptic_action, "/user/hand/left/output/haptic"},
                {haptic_action, "/user/hand/right/output/haptic"},
            });
    }

    // VALVE INDEX. a/click and b/click exist on BOTH hands here, unlike Touch, so the mod's four
    // face buttons map onto them by hand -- left a/b as X/Y, right a/b as A/B, keeping the layout
    // Touch establishes above.
    //
    // There is NO menu/click on this profile. The long-press that opens the settings panel is
    // therefore unavailable on an Index, and suggesting the path regardless would fail the entire
    // call and cost the wearer every other binding with it.
    suggest_bindings(
        "/interaction_profiles/valve/index_controller",
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
            {x_click_action, "/user/hand/left/input/a/click"},
            {y_click_action, "/user/hand/left/input/b/click"},
            {a_click_action, "/user/hand/right/input/a/click"},
            {b_click_action, "/user/hand/right/input/b/click"},
            {haptic_action, "/user/hand/left/output/haptic"},
            {haptic_action, "/user/hand/right/output/haptic"},
        });

    // HTC VIVE WAND. No thumbstick and no face buttons at all -- it has a TRACKPAD, and that is
    // where locomotion has to come from. The profile defines trackpad/x and trackpad/y, which is
    // exactly what a vector2f action requires of a parent path, so the stick action binds to
    // /input/trackpad and the stick click to /input/trackpad/click.
    //
    // Squeeze is a CLICK on this controller, not a value. A float action bound to a boolean source
    // is defined to read 0.0 or 1.0, so the grip reads as fully open or fully closed rather than
    // not at all.
    suggest_bindings(
        "/interaction_profiles/htc/vive_controller",
        {
            {aim_pose_action, "/user/hand/left/input/aim/pose"},
            {aim_pose_action, "/user/hand/right/input/aim/pose"},
            {grip_pose_action, "/user/hand/left/input/grip/pose"},
            {grip_pose_action, "/user/hand/right/input/grip/pose"},
            {trigger_action, "/user/hand/left/input/trigger/value"},
            {trigger_action, "/user/hand/right/input/trigger/value"},
            {squeeze_action, "/user/hand/left/input/squeeze/click"},
            {squeeze_action, "/user/hand/right/input/squeeze/click"},
            {stick_action, "/user/hand/left/input/trackpad"},
            {stick_action, "/user/hand/right/input/trackpad"},
            {thumbstick_click_action, "/user/hand/left/input/trackpad/click"},
            {thumbstick_click_action, "/user/hand/right/input/trackpad/click"},
            {menu_click_action, "/user/hand/left/input/menu/click"},
            {menu_click_action, "/user/hand/right/input/menu/click"},
            {haptic_action, "/user/hand/left/output/haptic"},
            {haptic_action, "/user/hand/right/output/haptic"},
        });

    // WINDOWS MIXED REALITY. This one has a thumbstick AND a trackpad, and no face buttons
    // whatsoever. The stick action takes the THUMBSTICK, because that is what this mod's
    // locomotion is shaped like; the trackpad is deliberately left unbound rather than fighting
    // the thumbstick for the same action. Squeeze is a click here too.
    suggest_bindings(
        "/interaction_profiles/microsoft/motion_controller",
        {
            {aim_pose_action, "/user/hand/left/input/aim/pose"},
            {aim_pose_action, "/user/hand/right/input/aim/pose"},
            {grip_pose_action, "/user/hand/left/input/grip/pose"},
            {grip_pose_action, "/user/hand/right/input/grip/pose"},
            {trigger_action, "/user/hand/left/input/trigger/value"},
            {trigger_action, "/user/hand/right/input/trigger/value"},
            {squeeze_action, "/user/hand/left/input/squeeze/click"},
            {squeeze_action, "/user/hand/right/input/squeeze/click"},
            {stick_action, "/user/hand/left/input/thumbstick"},
            {stick_action, "/user/hand/right/input/thumbstick"},
            {thumbstick_click_action, "/user/hand/left/input/thumbstick/click"},
            {thumbstick_click_action, "/user/hand/right/input/thumbstick/click"},
            {menu_click_action, "/user/hand/left/input/menu/click"},
            {menu_click_action, "/user/hand/right/input/menu/click"},
            {haptic_action, "/user/hand/left/output/haptic"},
            {haptic_action, "/user/hand/right/output/haptic"},
        });

    // BYTEDANCE PICO NEO 3. Core only from OpenXR 1.1, and this instance asks for 1.0.34, so on a
    // 1.0 instance the profile is reachable purely through XR_BD_controller_interaction -- hence
    // the same availability gate as the generic controller above. Without it the path does not
    // exist and the call would fail as a whole. Face buttons are split by hand exactly as they are
    // on Touch, and menu/click is defined on BOTH hands here.
    if (have_bytedance_controller) {
        suggest_bindings(
            "/interaction_profiles/bytedance/pico_neo3_controller",
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
                {a_click_action, "/user/hand/right/input/a/click"},
                {b_click_action, "/user/hand/right/input/b/click"},
                {menu_click_action, "/user/hand/left/input/menu/click"},
                {menu_click_action, "/user/hand/right/input/menu/click"},
                {haptic_action, "/user/hand/left/output/haptic"},
                {haptic_action, "/user/hand/right/output/haptic"},
            });
    }

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
    uint32_t screen_alloc_w = 0;  // what the swapchain images were ALLOCATED at, once
    uint32_t screen_alloc_h = 0;
    uint32_t screen_w = 0;       // the size of ONE eye's picture, not of the published frame
    uint32_t screen_h = 0;
    uint32_t screen_layout = 0xFFFFFFFFu;
    bool screen_ready = false;   // an image has been released at least once
    uint64_t held = 0;           // frames where we re-showed the last picture
    bool use_projection = true;  // quads only until the game is tracking the head

    // ---- THE UI LAYER'S OWN SWAPCHAIN -----------------------------------------------------------
    //
    // A second swapchain, independent of the eyes above: the UI is not stereo, is not the game's
    // resolution, and is created LAZILY -- only once a UI frame is actually published, because the
    // mod is off by default and there is no reason to reserve a texture for a layer that may never
    // appear.
    XrSwapchain ui_swapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> ui_images;
    uint32_t ui_w = 0;
    uint32_t ui_h = 0;
    bool ui_uploaded = false;    // an image has been released at least once at the current size
    bool ui_shown = false;       // for the one-time appear/disappear log, not per-frame
    std::vector<uint8_t> ui_staging;  // holds the alpha-derived copy when UiFrameHeader::derive_alpha is set

    // ---- THE POSES WE PUBLISHED, KEPT ----------------------------------------------------------
    //
    // The game renders from a pose it read a frame or two ago and tells us which one. Submitting a
    // projection layer with the CURRENT pose instead would claim the image is newer than it is:
    // reprojection would then have nothing to correct and the world would swim with head motion.
    // Sixteen entries is about a quarter of a second at 90 Hz -- far more lag than the pipeline has.
    struct PosePair {
        uint32_t sequence = 0xFFFFFFFFu;
        XrPosef pose[2]{};

        // WHEN THIS POSE WAS PREDICTED FOR. Sequence steps say how many publishes ago a frame's
        // pose was; this says how far in the past it was aimed, which is the number that actually
        // describes the error the compositor has to undo.
        XrTime predicted_for = 0;

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

    // ---- THE SETTINGS PANEL ----------------------------------------------------------------
    //
    // One-time init, right where every dependency it borrows (the D3D11 device, the session, the
    // view-locked space, the resolved swapchain format, the menu/trigger actions and aim spaces,
    // and --ui-distance) is already in scope and nothing later reassigns any of it. A failed init
    // (font missing, D3D11 or swapchain create failed) disables the panel rather than the host --
    // see SettingsUi::update()'s own null check.
    xrui::SettingsUi settings_ui;
    settings_ui.init(device, ctx, g_instance, session, view_space, screen_format,
                     trigger_action, aim_space, hand_path, &ui_distance_m);

    // Created rather than opened: the host is normally up first, and CreateEvent returns the
    // existing object if the game got there before us.
    HANDLE tick_event = CreateEventA(nullptr, FALSE, FALSE, xr::kFrameTickEventName);

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    uint64_t frames = 0;
    uint64_t content_waits_expired = 0;

    // ---- WHERE THE HOST'S FRAME ACTUALLY GOES ---------------------------------------------
    //
    // Split three ways, because "the host is slow" is not actionable: time BLOCKED IN
    // xrWaitFrame is the runtime pacing us and is normal; time in the content wait is us
    // waiting for the game; time in xrEndFrame is the compositor accepting the submission.
    // Only the last two can indicate a fault on our side, and on a software runtime the third
    // is where contention would appear.
    // ---- WALL CLOCK ON OUR OWN LINES ------------------------------------------------------
    //
    // The runtime's frontend (MetaXRSimulator.exe) writes its diagnostics -- including
    // "[DIAG-ZEROSKIP] Skipping blit" -- DIRECTLY TO THE CONSOLE, not through a stdout handle it
    // inherited from us. Console-direct writes bypass pipes, so neither freopen(--log) nor
    // Tee-Object can ever capture them; they are only visible in the window. Two attempts to count
    // them in a file returned zero for that reason, which is worse than no measurement.
    //
    // The frontend stamps its lines with wall clock. Ours had none, so a pasted console dump could
    // not be lined up against them. Now it can: same clock, same format.
    auto stamp = []() -> const char* {
        static char buf[16];
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond,
                      st.wMilliseconds);
        return buf;
    };

    double t_wait_ms = 0.0, t_content_ms = 0.0, t_end_ms = 0.0;
    double t_wait_max = 0.0, t_content_max = 0.0, t_end_max = 0.0;

    // THE UPLOAD -- the hole in the first pass of this instrumentation. Acquire/wait/
    // UpdateSubresource/release is the ONLY place the host touches memory the RUNTIME owns, so a
    // software-backed swapchain costs here what a hardware one does not. A large CPU copy per eye
    // per frame is also the right shape for a fault that slows the whole MACHINE, not just the
    // game -- which is what separates the simulator from real hardware.
    double t_upload_ms = 0.0, t_upload_max = 0.0;
    uint64_t upload_bytes = 0;
    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    auto now_ms = [&qpf]() -> double {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return qpf.QuadPart ? (static_cast<double>(t.QuadPart) * 1000.0 /
                               static_cast<double>(qpf.QuadPart))
                            : 0.0;
    };
    // ---- IS THE TRANSPORT DELIVERING IN ORDER, AND HOW OLD IS WHAT IT DELIVERS? ----------------
    // The consumer is the only place that can see either. `rendered_seq` is the pose the game says
    // it drew with; it must never go BACKWARDS, and its distance behind the pose we last published
    // is the age of the picture in compositor frames.
    uint32_t last_rendered_seq = 0;
    uint64_t seq_backwards = 0;
    uint64_t seq_repeats = 0;
    uint64_t age_samples = 0;
    uint64_t age_total = 0;
    uint32_t age_worst = 0;
    // How far the pose the game SAYS it drew with differs from the one we recorded sending under
    // that sequence. Zero means the index was telling the truth all along and this whole question
    // is closed; anything else is the discrepancy, measured rather than argued.
    uint64_t stated_samples = 0;
    double stated_sum_deg = 0.0;
    double stated_worst_deg = 0.0;
    uint64_t stated_absent = 0;
    // ---- THE DISTRIBUTION, NOT THE MEAN ---------------------------------------------------------
    //
    // A CONSTANT age is smooth: every frame shows the world from the same offset behind the head,
    // which reads as latency. A VARYING age is judder: consecutive frames show it from different
    // offsets, so the world steps back and forth. The mean cannot tell those apart -- 1.6 could be
    // every frame at 1.6, or 60% at 1 and 40% at 2.5, and only the second one judders.
    uint64_t age_hist[4] = {0, 0, 0, 0};  // 0, 1, 2, 3+
    bool pose_ever_published = false;
    // WHY A FRAME PUBLISHED NO POSE. If the head pose stalls for a frame the game's camera stalls
    // with it, and that is an update-rate defect however clean the association is.
    uint64_t pose_skip_should = 0;   // shouldRender was false
    uint64_t pose_skip_locate = 0;   // xrLocateViews failed or returned fewer than two views
    // Pose staleness in TIME rather than in steps: predicted-for versus the display time of the
    // frame it is finally submitted in.
    uint64_t stale_ms_samples = 0;
    double stale_ms_total = 0.0;
    double stale_ms_worst = 0.0;
    uint64_t submitted = 0;
    const ULONGLONG started = GetTickCount64();

    // ---- HAPTICS, WHICH FLOW THE OTHER WAY -----------------------------------------------------
    //
    // The read cursor into xr::HapticsState's ring plus what became of the entries it walked.
    // `haptics_seeded` is the difference between "the mod has never queued anything" and "the mod
    // has been queueing since before this process existed": the first write_index observed becomes
    // the cursor, so a running game's backlog is dropped rather than delivered into the wearer's
    // hands all at once.
    uint32_t haptic_read = 0;
    bool haptics_seeded = false;
    bool haptic_failure_logged = false;
    uint64_t haptics_fired = 0;
    uint64_t haptic_overruns = 0;  // entries the producer overwrote before we reached them
    uint64_t haptic_torn = 0;      // entries lapped DURING our copy, caught by the commit stamp
    // Swapchain acquire/wait failures across the game screen, the UI layer and the test pattern.
    // Counted rather than logged per frame: one is a hiccup, a rising count is the fault.
    uint64_t upload_failures = 0;
    bool layer_overflow_logged = false;
    // What the two paired frame calls last returned, so a persistent failure logs on the
    // transition instead of ninety times a second.
    XrResult last_wait_result = XR_SUCCESS;
    XrResult last_begin_result = XR_SUCCESS;

    // ---- ONE EVENT PUMP, TWO CALLERS -----------------------------------------------------------
    //
    // The frame loop drains events at the top of every iteration; shutdown drains them again while
    // waiting for the session to reach STOPPING. A second copy of this switch would be a second
    // place for a transition to be handled differently, and the transition shutdown waits for is
    // precisely the one that would be missed.
    auto pump_events = [&]() {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult poll_r = xrPollEvent(g_instance, &ev);

        for (; poll_r == XR_SUCCESS; poll_r = xrPollEvent(g_instance, &ev)) {
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

            // ---- THE RUNTIME ITSELF IS GOING AWAY ---------------------------------------------
            //
            // Routine rather than exotic: a SteamVR or Oculus service restart delivers this, and
            // at `lossTime` every handle this process holds -- instance, session, swapchains,
            // spaces -- stops being valid. Unhandled, the loop carried on calling into a dead
            // instance and every result after it was an error with no visible cause.
            if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                const auto* ilp = reinterpret_cast<XrEventDataInstanceLossPending*>(&ev);
                std::printf("[host] INSTANCE LOSS PENDING at %lld -- the runtime is going away, "
                            "stopping\n", static_cast<long long>(ilp->lossTime));
                g_stop = true;
            }

            // WE FELL BEHIND. The runtime's event queue is finite and drops the oldest when it
            // fills, so this says a session state change or a profile change may simply never
            // have been seen -- which is the only explanation for a state machine that looks
            // impossible from the log alone.
            if (ev.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
                const auto* lost = reinterpret_cast<XrEventDataEventsLost*>(&ev);
                std::printf("[host] %u event(s) LOST -- this pump fell behind the runtime\n",
                            lost->lostEventCount);
            }

            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                std::printf("[host] session -> %s\n", state_name(state));

                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    const XrResult session_begin_r = xrBeginSession(session, &sbi);
                    std::printf("[host] xrBeginSession -> %s\n", rs(session_begin_r));
                    // `running` is what lets the frame loop call xrWaitFrame. Setting it after a
                    // FAILED begin would aim every frame at a session that was never started.
                    running = XR_SUCCEEDED(session_begin_r);
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    // The ONE state xrEndSession is legal from, which is why it lives here rather
                    // than in the shutdown path -- shutdown reaches this state by asking for it
                    // and pumping until this line runs.
                    std::printf("[host] xrEndSession -> %s\n", rs(xrEndSession(session)));
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
                        const XrResult name_r = xrPathToString(g_instance, ips.interactionProfile,
                                                               sizeof(path_buf), &len, path_buf);
                        std::printf("[host] interaction profile changed, hand %u -> %s\n", h,
                                    XR_SUCCEEDED(name_r) ? path_buf : rs(name_r));
                    } else {
                        std::printf("[host] interaction profile changed, hand %u -> none (%s)\n", h,
                                    rs(pr));
                    }
                }
            }

            ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        // XR_EVENT_UNAVAILABLE is the ordinary "queue empty" and ends the drain. Anything else is
        // the instance failing underneath us, and without this the loop would spin on it in
        // silence for as long as the process lived.
        if (poll_r != XR_EVENT_UNAVAILABLE) {
            std::printf("[host] xrPollEvent -> %s -- stopping\n", rs(poll_r));
            g_stop = true;
        }
    };

    std::printf("[host] entering frame loop -- PUT THE HEADSET ON if nothing appears; the runtime\n"
                "[host] keeps the session IDLE while it is unworn and will not accept frames.\n");

    while (!g_stop) {
        pump_events();

        if (!running) {
            // NOT SERVICING. Readiness has to be cleared on the way INTO idle, not just at
            // teardown: after one active frame it would otherwise stay set through a session stop or
            // the headset coming off, and the game -- which gates its whole frame protocol on it --
            // would post a WAIT nobody answers and eat a 100 ms timeout every single update.
            //
            // Set where a request will be served, cleared everywhere it will not. Those are the only
            // two states this flag has, and both edges matter.
            if (auto* hsk_idle = reader.handshake()) {
                hsk_idle->host_servicing = 0u;
            }

            Sleep(20);

            if (max_seconds > 0 && GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
                break;
            }

            continue;
        }

        // xrWaitFrame is the compositor's throttle -- it is what paces this loop, not a sleep.
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        const double t_wait_0 = now_ms();
        double trace_content_cost = 0.0;
        double trace_end_req_wait = 0.0;  // time blocked waiting for the game's END request
        // HOST-SIDE AWAIT SPANS, one per stage. The game-side request->ack timing measures the same
        // rendezvous from the other end; the DIFFERENCE between the two is the handoff itself, which
        // is the only honest way to attribute the frame's unaccounted milliseconds instead of calling
        // them "IPC overhead" and moving on.
        double trace_aw_wait = 0.0;
        double trace_aw_begin = 0.0;
        double trace_aw_end = 0.0;
        double trace_post_end = 0.0;  // END receipt -> xrEndFrame, EVERYTHING in between
        // Narrow spans inside it, because post_end covers far more than uploads: frame and UI
        // polling, swapchain recreation, action sync and hand locating, haptics, the settings UI,
        // layer construction and logging. Attributing all of it to "upload" was a guess and this is
        // what replaces it.
        double trace_eye_upload = 0.0;  // acquire + wait + UpdateSubresource + Flush + release, both eyes
        double trace_actions = 0.0;     // xrSyncActions + hand locating + haptics
        // DRIVEN, when the game is driving. A latch rather than a per-frame test: once a request
        // has been seen, the game owns the loop and a missing request is a stall to be reported, not
        // a reason to silently go back to free-running and desynchronise.
        auto* const hsk = reader.handshake();
        // We are in the loop and about to serve: say so. The game gates its whole frame protocol on
        // this, so it must be true only where a WAIT will actually be answered.
        if (hsk != nullptr) {
            hsk->host_servicing = 1u;
        }
        static bool s_game_drives = false;
        // Which request has an unmatched xrBeginFrame. The contract allows exactly one, and the spec
        // permits no more -- xrBeginFrame twice without an intervening end returns XR_FRAME_DISCARDED.
        static uint32_t s_begun_request = xr::kNoRequest;
        if (hsk != nullptr && hsk->phase != xr::kPhaseIdle) {
            s_game_drives = true;
        }
        uint32_t hsk_id = 0;
        const double trace_aw_wait_t0 = now_ms();
        const bool hsk_wait = s_game_drives && handshake_await(hsk, xr::kPhaseWait, 100u, &hsk_id);
        trace_aw_wait = now_ms() - trace_aw_wait_t0;

        const double trace_wait_begin_ms = now_ms();
        const XrResult wait_r = xrWaitFrame(session, &fwi, &fs);
        // The INTERVAL BETWEEN RETURNS is the runtime's actual throttle, which is the thing being
        // argued about. Taken here, before anything else in the iteration can add to it.
        const double trace_wait_ret_ms = now_ms();

        // WAIT ACK. The game is blocked here, so this is where the runtime's throttle reaches it.
        if (hsk_wait) {
            handshake_ack(hsk, xr::kPhaseWait, wait_r, &fs, nullptr, xr::kNoRequest, hsk_id);
        }
        const double trace_wait_blocked = trace_wait_ret_ms - trace_wait_begin_ms;
        static double s_trace_prev_wait_ms = 0.0;
        const double trace_wait_interval =
            s_trace_prev_wait_ms > 0.0 ? trace_wait_ret_ms - s_trace_prev_wait_ms : 0.0;
        s_trace_prev_wait_ms = trace_wait_ret_ms;
        {
            const double d = now_ms() - t_wait_0;
            t_wait_ms += d;
            if (d > t_wait_max) { t_wait_max = d; }
        }

        // ---- A FAILED WAIT IS NOT A FRAME ------------------------------------------------------
        //
        // `fs` is value-initialised, so a failure leaves predictedDisplayTime at ZERO -- and the
        // rest of this loop would hand that zero to xrLocateViews and xrEndFrame, which answer
        // XR_ERROR_TIME_INVALID forever, as fast as the CPU allows, with nothing on screen to show
        // for it. The result used to be discarded outright, so that is what a runtime restart
        // looked like from here: a pegged core and a black headset.
        //
        // Skipped means SKIPPED: no xrBeginFrame, therefore no xrEndFrame. The two are a pair and
        // an end without a begin is as wrong as a begin without an end. Teardown is left to the
        // event pump, which is where the session-state change actually arrives.
        if (XR_FAILED(wait_r) || fs.predictedDisplayTime <= 0) {
            if (wait_r != last_wait_result) {
                std::printf("[host] xrWaitFrame -> %s (predicted display time %lld) -- skipping "
                            "frames until it recovers\n", rs(wait_r),
                            static_cast<long long>(fs.predictedDisplayTime));
                last_wait_result = wait_r;
            }

            Sleep(5);

            if (max_seconds > 0 &&
                GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
                break;
            }

            continue;
        }

        if (wait_r != last_wait_result) {
            // XR_SESSION_LOSS_PENDING is a SUCCESS code and the ordinary way a SteamVR or Oculus
            // service restart surfaces here: the frame state is valid and this frame is flown
            // normally, while the session-state event that follows brings the process down.
            if (wait_r == XR_SESSION_LOSS_PENDING) {
                std::printf("[host] xrWaitFrame -> %s -- the runtime is going away\n", rs(wait_r));
            }

            last_wait_result = wait_r;
        }

        // THE FRAME CLOCK, relayed. xrWaitFrame has just told us when the runtime wants the next
        // frame; releasing the game here makes its update run on the compositor's cadence instead
        // of free-running at whatever the hardware allows.
        // ---- THE POSE MUST EXIST BEFORE THE GAME IS RELEASED -----------------------------------
        //
        // This used to sit two hundred lines further down, AFTER the tick. So the game was woken to
        // render a frame and then read whatever pose happened to be in the mapping -- last frame's,
        // unless it lost the race with the publish below. Which one it got depended on how the two
        // processes interleaved, so the frame was stamped with an honest sequence for a pose that
        // was sometimes a whole compositor frame stale. Intermittent, and indistinguishable from
        // the wrong pose being sent.
        //
        // The tick means "a new pose is ready and a boundary has happened". It cannot mean that if
        // the pose is written afterwards.
        //
        // xrLocateViews needs only predictedDisplayTime, which xrWaitFrame has just given us, so
        // there is nothing that required it to run after xrBeginFrame.
        // ---- TELL THE GAME WHERE THE HEAD IS ---------------------------------------------------
        //
        // Published every frame, whether or not the game is listening: the pose is what makes a
        // projection layer honest later, and a game that starts listening mid-session should find
        // current data rather than wait a frame for it.
        if (fs.shouldRender == XR_FALSE && reader.host != nullptr) {
            ++pose_skip_should;
        }
        // PER ITERATION, never a lifetime flag: the question is whether THIS frame's pose publish
        // happened, so the cadence-only update below can fill in for it without ever double-bumping
        // HostState::sequence on a normal frame. Declared OUTSIDE the shouldRender gate on purpose
        // -- the case it exists to cover is precisely the one where that gate is false.
        bool pose_published_this_frame = false;

        if (fs.shouldRender != XR_FALSE && reader.host != nullptr) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;

            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t located = 0;
            std::vector<XrView> hv(view_count, {XR_TYPE_VIEW});

            const XrResult lr_res = xrLocateViews(session, &vli, &vs, view_count, &located, hv.data());

            // ---- THE HEAD IS NOT THE LEFT EYE ------------------------------------------------
            //
            // The position below was always averaged over both eyes, but the ORIENTATION used to
            // be taken verbatim from hv[0]. On a canted-display headset -- Index, Pimax, Varjo,
            // Bigscreen Beyond -- the panels are physically rotated and each eye's orientation
            // carries that cant, so the game's camera ends up permanently yawed with nothing on
            // screen to explain it.
            //
            // VIEW space IS the head: its origin is the point between the eyes, which is exactly
            // what this publishes. Locating it is the answer rather than averaging two rotations,
            // which is not a well-defined operation in the first place.
            XrSpaceLocation head_loc{XR_TYPE_SPACE_LOCATION};
            const XrResult head_r =
                xrLocateSpace(view_space, space, fs.predictedDisplayTime, &head_loc);

            // BOTH BITS. hs->position is published from this same location and the mod steers a
            // camera with it, so a pose with a believable orientation and an unknown position
            // would look right while standing in the wrong place.
            const bool head_ok =
                XR_SUCCEEDED(head_r) &&
                (head_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
                (head_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            const bool views_ok = XR_SUCCEEDED(lr_res) && located >= 2;

            if (!views_ok || !head_ok) {
                ++pose_skip_locate;
            }

            if (views_ok && head_ok) {
                auto* hs = reader.host;
                hs->sequence |= 1u;
                MemoryBarrier();

                // THE WHOLE POSE FROM ONE LOCATED SPACE, orientation and position together.
                // VIEW is defined as "the view origin used to generate view transforms for the
                // primary viewer (or centroid of view origins if stereo)", so its position IS the
                // eye midpoint this used to average out of hv[0]/hv[1] by hand -- the two agree by
                // definition, and taking both halves from the same XrSpaceLocation means they
                // cannot disagree even if a runtime defines that centroid differently than we
                // would. It also leaves exactly one validity check (head_ok) covering the pose the
                // mod steers a camera with, instead of one covering the orientation and a
                // different one covering the position.
                //
                // The eye poses below are still needed, but only for IPD and for the frustum --
                // never for the head.
                hs->position[0] = head_loc.pose.position.x;
                hs->position[1] = head_loc.pose.position.y;
                hs->position[2] = head_loc.pose.position.z;

                hs->orientation[0] = head_loc.pose.orientation.x;
                hs->orientation[1] = head_loc.pose.orientation.y;
                hs->orientation[2] = head_loc.pose.orientation.z;
                hs->orientation[3] = head_loc.pose.orientation.w;

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
                // Nothing above runs unless xrLocateSpace reported BOTH orientation and position
                // valid, so this records that condition rather than re-deriving it. It used to be
                // set from the view state's ORIENTATION bit alone while hs->position was published
                // regardless -- a pose the mod was told to trust with half of it unknown.
                hs->valid = 1u;
                // THE RUNTIME'S OWN CADENCE, inside the same seqlock window as the pose, from the
                // xrWaitFrame that succeeded above.
                //
                // GAP, STATED HERE SO IT IS NOT REDISCOVERED: this sits inside the pose-valid
                // block, so a run of frames where the pose does not publish will not refresh
                // the cadence. It cannot disable pacing -- the mod keeps the last period and
                // only zero fails open -- but a runtime REFRESH-RATE CHANGE during such a run
                // is observed late, on the next valid pose frame. Publishing it in its own
                // seqlock window would bump HostState::sequence twice per frame, and the
                // host indexes pose_history at published_sequence/2 % 16, so that trade was
                // refused deliberately rather than missed.
                // The mod paces its entire update loop on our
                // tick and must size its wait in terms of this rather than assuming a rate -- it
                // assumed 90 Hz for a 72 Hz runtime and lost a frame to it whenever we hitched.
                hs->predicted_display_period_ns =
                    static_cast<uint32_t>(fs.predictedDisplayPeriod);
                hs->reserved_period_hi = 0u;

                MemoryBarrier();
                hs->sequence = (hs->sequence + 1u) & ~1u;
                published_sequence = hs->sequence;
                pose_published_this_frame = true;

                // Keep what we just handed out, so the frame rendered from it can be submitted with
                // it rather than with whatever is current by the time the pixels arrive.
                PosePair& slot = pose_history[(published_sequence / 2u) % 16u];
                slot.sequence = published_sequence;
                slot.predicted_for = fs.predictedDisplayTime;
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
                pose_ever_published = true;
            }
        }

        // ---- CADENCE STILL PROPAGATES WHEN THE POSE DID NOT --------------------------------------
        //
        // The period publishes inside the pose block, which is right for every normal frame. But a
        // runtime REFRESH-RATE CHANGE during a run of pose-invalid frames -- or while shouldRender
        // is false -- would otherwise not reach the game until a pose published again, and the game
        // paces its whole update loop on a wait sized from that period.
        //
        // Gated on BOTH conditions so it can never double-bump a normal frame: the pose publish did
        // not happen this iteration, AND the period actually differs. A normal frame publishes
        // once, in the pose block, exactly as before -- which matters because the host indexes
        // pose_history at published_sequence/2 % 16 and a spurious second bump would walk that off.
        //
        // The compare happens BEFORE the window is opened, which is safe because this is the only
        // writer: opening a seqlock on every pose-invalid frame just to find nothing changed would
        // manufacture torn reads for a reader that is polling. Pose fields are untouched, so a
        // reader catching this publish sees the previous pose with the new cadence -- the truth.
        // Neither published_sequence nor pose_history advances, because no pose was published.
        if (reader.host != nullptr && fs.predictedDisplayPeriod > 0 && !pose_published_this_frame) {
            auto* hs = reader.host;
            const auto period = static_cast<uint32_t>(fs.predictedDisplayPeriod);
            if (hs->predicted_display_period_ns != period) {
                hs->sequence |= 1u;
                MemoryBarrier();
                hs->predicted_display_period_ns = period;
                hs->reserved_period_hi = 0u;
                MemoryBarrier();
                hs->sequence = (hs->sequence + 1u) & ~1u;
            }
        }

        // ---- ONE PER COMPOSITOR FRAME, AND ONLY HERE -------------------------------------------
        //
        // This is a CLOCK, and the game paces on it: it waits for the counter to advance by its
        // divisor. It used to be incremented in two different places -- once in the head-pose
        // publish and once in the hands block -- so it stepped by 0, 1 or 2 per loop depending on
        // what the runtime had to say that frame.
        //
        // Pacing then read a double step as the game having OVERRUN its budget, every frame, and
        // climbed the divisor to its cap: with forced ASW holding the loop at 36 Hz and the counter
        // running at 72, the game landed on 72/3 = 24 fps while 36 was asked for. Measured in the
        // headset as 23-26.
        //
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        uint32_t hsk_begin_id = 0;
        const double trace_aw_begin_t0 = now_ms();
        const bool hsk_begin =
            s_game_drives && handshake_await(hsk, xr::kPhaseBegin, 100u, &hsk_begin_id);
        trace_aw_begin = now_ms() - trace_aw_begin_t0;

        const double trace_begin_ms0 = now_ms();
        const XrResult begin_r = xrBeginFrame(session, &fbi);
        const double trace_begin_dur = now_ms() - trace_begin_ms0;

        // BEGIN ACK, carrying the begun id so the game -- and a later END -- can see which frame is
        // outstanding. Acked even on failure: the game must learn it may NOT send END.
        if (hsk_begin) {
            handshake_ack(hsk, xr::kPhaseBegin, begin_r, nullptr, nullptr,
                          XR_SUCCEEDED(begin_r) ? hsk_begin_id : xr::kNoRequest, hsk_begin_id);
        }

        // THE ONE REQUEST THAT MAY BE ENDED. Tracked locally rather than read back out of the block,
        // because the game owns those fields and a recovering game may already have advanced them.
        if (hsk_begin) {
            s_begun_request = XR_SUCCEEDED(begin_r) ? hsk_begin_id : xr::kNoRequest;
        }

        // XR_FRAME_DISCARDED is a SUCCESS code that still owes an xrEndFrame: the runtime is
        // saying a PREVIOUS frame was thrown away, not refusing this one. A real failure means no
        // frame was begun at all, so going on to xrEndFrame would be an unpaired end.
        if (XR_FAILED(begin_r)) {
            if (begin_r != last_begin_result) {
                std::printf("[host] xrBeginFrame -> %s -- skipping the frame\n", rs(begin_r));
                last_begin_result = begin_r;
            }

            Sleep(5);

            if (max_seconds > 0 &&
                GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
                break;
            }

            continue;
        }

        // ---- ONLY NOW IS THERE A FRAME FOR THE GAME TO RENDER --------------------------------
        //
        // The clock advance and the tick used to fire BEFORE xrBeginFrame, so the game was woken to
        // render into a frame the runtime had not begun -- and on a discarded or failed begin, into
        // one that never existed at all. The runtime throttles through xrWaitFrame using what it
        // sees between Begin and End, so work started ahead of Begin is work it cannot attribute to
        // the frame; the content wait already sits inside the pair, and this puts the game's render
        // inside it too.
        //
        // Still incremented before the event is signalled, so a game woken by the tick always reads
        // the count that wake corresponds to rather than the previous one.
        if (reader.host != nullptr) {
            ++reader.host->frames;
        }

        if (tick_event != nullptr) {
            SetEvent(tick_event);
        }

        last_begin_result = begin_r;

        std::vector<XrCompositionLayerProjectionView> layer_views(view_count);
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

        // ---- SIZED WITH ROOM, AND CHECKED ANYWAY -----------------------------------------------
        //
        // This array used to be exactly four, which is exactly the worst case it has today: the
        // two-quad fallback plus the mod's HUD plus the settings panel. It was full, nothing
        // compared the count against it, and one more layer type would have written past the end
        // in silence.
        const XrCompositionLayerBaseHeader* layers[kMaxLayers]{};
        uint32_t layer_count = 0;

        auto append_layer = [&](const XrCompositionLayerBaseHeader* appended) {
            if (layer_count < kMaxLayers) {
                layers[layer_count++] = appended;
                return;
            }

            // Loud once rather than counted quietly: overflowing this is a mistake in the
            // composition above, not a runtime condition that can be waited out.
            if (!layer_overflow_logged) {
                std::printf("[host] composition layer list full at %u -- dropping a layer; raise "
                            "kMaxLayers\n", kMaxLayers);
                layer_overflow_logged = true;
            }
        };

        // Pull the newest complete frame the game has published, if any.
        uint32_t fw = 0, fh = 0, fpitch = 0;
        const uint8_t* fbits = nullptr;
        // ---- WAIT FOR THE GAME'S END REQUEST *BEFORE* CHOOSING CONTENT ----------------------
        //
        // This has to precede the poll, not sit just above xrEndFrame. The game publishes its pixels
        // and THEN asks for END, so awaiting it after content was already selected and uploaded
        // means the frame being ended carries whatever was lying around a moment earlier -- the
        // variable content wait and the repeats, exactly what the handshake exists to delete.
        //
        // Waiting here makes the pixels ready BY CONSTRUCTION: the request is itself the statement
        // that they are published, so the poll below finds them and no bounded guess is needed.
        uint32_t hsk_end_id = 0;
        // TIMED, because moving a wait is not removing it. The bounded content poll is now zero by
        // CONSTRUCTION -- this await sits before it -- and reporting that as "the wait cost vanished"
        // would be measuring the instrument's blind spot. The cost is architecturally better here
        // (it ends when the game says the pixels are published, not when a guess expires) but it is
        // still cost, and the trace has to show it or the next reader draws the same wrong
        // conclusion I did.
        const double t_end_req_0 = now_ms();
        const double trace_aw_end_t0 = now_ms();
        const bool hsk_end = s_game_drives && handshake_await(hsk, xr::kPhaseEnd, 100u, &hsk_end_id);
        trace_aw_end = now_ms() - trace_aw_end_t0;
        // EVERYTHING BETWEEN THE GAME'S END REQUEST AND THE SUBMIT. The game reports ~4.77 ms blocked
        // in its END rendezvous while xrEndFrame itself costs 0.15 -- so ~4.6 ms happens in here and
        // was being lumped together: swapchain acquire and wait, two eye uploads with their Flush and
        // release, plus whatever else the loop does before submitting. t_upload_ms exists but is a
        // 90-frame aggregate, which cannot say what any single frame did.
        const double trace_post_end_t0 = now_ms();
        trace_end_req_wait = now_ms() - t_end_req_0;

        bool have_frame = false;

        // Same pull for the UI layer -- a second, independent publish that may or may not be
        // active. `ui_present_now` is a live read (see SharedReader::ui_present) used even on
        // frames where nothing NEW was published, so the layer disappears promptly when the mod
        // turns it off instead of waiting on an unrelated sequence bump.
        uint32_t uw = 0, uh = 0, upitch = 0, uderive_alpha = 0;
        const uint8_t* ubits = nullptr;
        bool have_ui_frame = false;
        bool ui_present_now = false;

        if (reader.open()) {
            have_frame = reader.poll(fw, fh, fpitch, fbits);

            // ---- LET THE RUNTIME SEE THE REAL FRAME RATE -------------------------------------
            //
            // Re-showing the last frame ON TIME is indistinguishable, from the runtime's side,
            // from an application comfortably hitting full rate -- so it has no reason to engage
            // ASW, and the wearer gets the same picture for two or three display frames with only
            // rotational timewarp between them. Pacing the GAME to a submultiple made that worse,
            // not better: we locked the content to 36 and kept telling the compositor it was 90.
            //
            // A slow application is late, so BE late. Waiting here for real content pushes
            // xrEndFrame past the deadline exactly as a heavy frame would, which is the signal the
            // runtime's reprojection is driven by.
            //
            // BOUNDED, because a game that has stopped -- loading, alt-tabbed, hitching -- must
            // not take the compositor down with it. Past the bound we submit the held frame, which
            // is the old behaviour and still better than nothing on screen.
            //
            // THE BOUND IS DERIVED, LIKE THE GAME'S PACING WAIT. It was a hardcoded 12 ms, and a
            // game capped at 60 has its next frame up to 16.7 ms away -- so the wait expired, the
            // held image went out ON TIME, and the runtime saw an application meeting cadence
            // perfectly. That is why ASW never engages here: xr64 launders the game's misses into
            // repeats, and a runtime cannot compensate for a miss it is never shown.
            //
            // Two display periods, the same policy the mod's tick wait uses, from the runtime's own
            // predictedDisplayPeriod. Zero (before the first xrWaitFrame) keeps the old behaviour of
            // not waiting at all rather than substituting a number.
            // An EXPLICIT --content-wait is honoured as the millisecond value it says, because a
            // flag whose number is silently ignored is worse than no flag. Only the default (the
            // sentinel below) derives from the runtime's cadence.
            // DERIVING THIS IS OPT-IN, AND THAT IS A RETREAT FROM A MEASURED REGRESSION.
            //
            // Two display periods is the right POLICY -- it is what the mod's tick wait uses -- but
            // as a content bound it was observed making both the game and xr64 oscillate between 36
            // and 60 with the game capped to 60. A bound that can span a whole extra compositor
            // period makes this loop's own period variable, which is a plausible cause of exactly
            // that hunting and was never separated from the alternative (feedback through the
            // runtime's ASW). Until the per-frame transition log exists, the default must not be the
            // behaviour that was seen misbehaving.
            //
            // So: default is the old fixed bound, --content-wait N is honoured verbatim, and
            // --content-wait-derive asks for two periods for anyone measuring it.
            // ZERO WHEN THE GAME DRIVES. Its END request already means "the pixels are
            // published", so a bounded guess on top of it is a second mechanism deciding the same
            // thing -- and two mechanisms deciding when a frame is ready is how the oscillation
            // this replaces was built.
            const uint32_t content_bound_ms =
                hsk_end ? 0u
                : content_wait_derive && !content_wait_explicit && fs.predictedDisplayPeriod > 0
                    ? static_cast<uint32_t>((2ll * fs.predictedDisplayPeriod + 999'999ll) /
                                            1'000'000ll)
                    : content_wait_ms;
            const double t_content_0 = now_ms();
            if (!have_frame && content_bound_ms > 0) {
                const ULONGLONG deadline = GetTickCount64() + content_bound_ms;
                while (!have_frame && GetTickCount64() < deadline) {
                    Sleep(1);
                    have_frame = reader.poll(fw, fh, fpitch, fbits);
                }
                if (!have_frame) {
                    ++content_waits_expired;
                }
            }
            trace_content_cost = now_ms() - t_content_0;
            {
                const double d = now_ms() - t_content_0;
                t_content_ms += d;
                if (d > t_content_max) { t_content_max = d; }
            }

            if (ui_enabled) {
                ui_present_now = reader.ui_present();
                have_ui_frame = reader.poll_ui(uw, uh, upitch, uderive_alpha, ubits);
            }
        }

        // A side-by-side frame is TWO pictures: each eye gets half the width.
        const uint32_t layout = reader.layout();
        const uint32_t eye_w = (layout == xr::kLayoutSideBySide) ? fw / 2u : fw;
        const uint32_t eye_h = fh;

        // ---- CREATE THEM EVEN WITH NO GAME --------------------------------------------------
        //
        // These used to be created only once a frame had arrived, so running the host on its own --
        // which is exactly what someone does first, and what happens during the opening movies and
        // the main menu -- built no swapchains, submitted NO LAYER at all, and left the compositor
        // showing uninitialised memory in both eyes. That is the flickering colour, and no
        // placeholder could help because there was nothing to paint into.
        //
        // With no frame to size against, the runtime's own recommendation is the right size: it is
        // what the headset asks for, and the placeholder is the only thing being drawn.
        // With no frame to size against, the runtime's own recommendation is the right size: it is
        // what the headset asks for, and the placeholder is the only thing being drawn.
        const bool want_placeholder_chain =
            !have_frame && screen[0] == XR_NULL_HANDLE && !config_views.empty();
        const uint32_t make_w = want_placeholder_chain
                                    ? config_views[0].recommendedImageRectWidth
                                    : eye_w;
        const uint32_t make_h = want_placeholder_chain
                                    ? config_views[0].recommendedImageRectHeight
                                    : eye_h;

        // ---- ALLOCATED ONCE, DESCRIBED PER FRAME ------------------------------------------
        //
        // The condition here used to include `make_w != screen_w || layout != screen_layout`, so
        // the chains were destroyed and rebuilt whenever the picture changed shape. Both change at
        // world load: the menu publishes ONE image, the world publishes side-by-side, so eye_w
        // halves AND the layout flips. The simulator's viewport visibly resizes at that moment and
        // the compositor dies -- it keeps one color/depth swapchain per view index, destroys the
        // previous when a different one appears, and never composites again.
        //
        // The spec's own wording says how this is meant to work: recommendedImageRectWidth is "the
        // optimal width of XrSwapchainSubImage::imageRect to use when rendering this view into a
        // swapchain" -- the recommendation sizes the RECT, not the image, and maxImageRectWidth
        // caps it. So allocate the image once at the maximum, and let imageRect say how much of it
        // is filled this frame. Nothing is ever replaced, so there is nothing to clean up.
        // ---- SIZED TO THE FRAME, NOT TO THE RUNTIME'S MAXIMUM ------------------------------
        //
        // This allocated at maxImageRectWidth/Height (8192x8192 here) so the chain would never need
        // recreating. It backfired, and Meta XR Simulator said so once a second:
        //
        //   [DIAG-ZEROSKIP] Skipping blit: source image center pixel is zero
        //
        // The runtime samples the centre of the swapchain IMAGE before blitting it. Only the
        // top-left ~2160x2224 of an 8192x8192 image is ever written, so its centre at (4096,4096)
        // is untouched memory -- zero, every frame, by construction. The runtime then refuses the
        // blit and keeps the last frame it accepted, which is exactly the reported symptom: one
        // frozen scene frame while the game and input carry on.
        //
        // Recreating the chain when the picture changes shape is FINE, which the same log settles:
        // "Swapchains changed detected, re-enumerating... Successfully re-enumerated swapchains."
        // The compositor handles replacement cleanly. So size to the frame and let it recreate.
        // ---- ONE ALLOCATION, LARGE ENOUGH FOR EVERY SHAPE ------------------------------------
        //
        // Two constraints, both measured the hard way, and every earlier attempt satisfied one
        // while breaking the other:
        //
        //   1. NEVER RECREATE THE CHAIN. Meta XR Simulator cannot survive a replacement: the
        //      inner window visibly resizes and from that moment NOTHING composites again -- the
        //      scene, the UI panel, everything. Its log says "Replacing existing color swapchain
        //      ... cleaning up previous" then "Successfully re-enumerated", which reads like
        //      recovery and is not one.
        //   2. THE IMAGE CENTRE MUST HOLD WRITTEN PIXELS. Allocating at maxImageRect (8192x8192)
        //      avoided (1) and broke this: only the top-left was filled, so the centre at
        //      (4096,4096) was untouched memory and the runtime skipped every blit --
        //      "[DIAG-ZEROSKIP] source image center pixel is zero" -- while our own centre-pixel
        //      probe read live content from the source buffer.
        //
        // The shape changes because the menu publishes one layout and a world another (that is the
        // width change on screen), so the allocation must cover BOTH from the first creation. An
        // eye is at most the whole published frame (a mono frame is not split), so the full frame
        // size is the bound. The picture is then CENTRED in it, which keeps the middle real in
        // every configuration, and imageRect names where it landed -- exactly what imageRect is
        // for, and it costs no reallocation.
        // Sized to the FRAME. A fixed capacity-sized allocation was tried while chasing a
        // freeze that turned out to be the MCP operator layer, not us -- it cost ~230 MB of VRAM to
        // satisfy a constraint that did not exist. Recreating on a shape change is fine.
        const uint32_t alloc_w = want_placeholder_chain ? config_views[0].recommendedImageRectWidth
                                                        : make_w;
        const uint32_t alloc_h = want_placeholder_chain ? config_views[0].recommendedImageRectHeight
                                                        : make_h;

        // The picture's size and layout may change at any time (menu -> world flips both). That is
        // now a bookkeeping update, not a reallocation.
        if (have_frame && screen[0] != XR_NULL_HANDLE) {
            screen_w = make_w;
            screen_h = make_h;
            screen_layout = layout;
        }

        // AGAINST THE CHAIN'S OWN ALLOCATION, never against the picture bookkeeping above. That
        // block has already set screen_w/screen_h/screen_layout to THIS frame, and alloc_w is
        // make_w whenever a frame exists -- so comparing the two asked "does this frame differ from
        // itself", which is false forever and made recreation dead code the moment the first chain
        // existed. The images then stayed at their first size while screen_alloc_w/h (assigned only
        // inside this branch) went stale, and every centred offset and imageRect derived from them
        // pointed at the wrong region. Visible as the menu rendering correctly once and breaking on
        // the way back from a world, and as a broken loading screen -- both are shape changes.
        //
        // It is the vestige of an "allocate once, never recreate" design that was reverted: the
        // bookkeeping it needed outlived the allocation it was written for.
        if ((have_frame || want_placeholder_chain) && alloc_w != 0 && alloc_h != 0 &&
            (screen[0] == XR_NULL_HANDLE || alloc_w != screen_alloc_w ||
             alloc_h != screen_alloc_h)) {
            // Sized to the GAME's frame, not the runtime's recommendation: at native size the
            // upload is a straight copy with no resampling anywhere in the path.
            bool ok = true;

            for (int e = 0; e < 2; ++e) {
                screen_images[e].clear();

                if (screen[e] != XR_NULL_HANDLE) {
                    // Result ignored: this handle is being replaced whatever the runtime says,
                    // and a destroy that fails leaves nothing this loop could act on.
                    xrDestroySwapchain(screen[e]);
                    screen[e] = XR_NULL_HANDLE;
                }

                XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sc.usageFlags =
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sc.format = screen_format;
                sc.sampleCount = 1;
                // Sized to the RUNTIME'S MAXIMUM, not to this frame: see above. The eye's
                // picture is placed in the top-left and imageRect names it.
                // Sized to the EYE's picture, so the upload stays a straight copy with no
                // resampling anywhere between the game's back buffer and the compositor.
                sc.width = alloc_w;
                sc.height = alloc_h;
                sc.faceCount = 1;
                sc.arraySize = 1;
                sc.mipCount = 1;

                const XrResult screen_r = xrCreateSwapchain(session, &sc, &screen[e]);
                std::printf("[host] eye %d screen swapchain %ux%u (%s) -> %s\n", e, make_w, make_h,
                            layout == xr::kLayoutSideBySide ? "side-by-side" : "mono", rs(screen_r));

                if (XR_FAILED(screen_r)) {
                    ok = false;
                    break;
                }

                uint32_t n = 0;
                XrResult img_r = xrEnumerateSwapchainImages(screen[e], 0, &n, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});

                if (XR_SUCCEEDED(img_r) && n > 0) {
                    img_r = xrEnumerateSwapchainImages(
                        screen[e], n, &n,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
                }

                // The upload path indexes screen_images[e], so an empty or failed enumeration is
                // a swapchain nothing may point at. Failing the resize here keeps the previous
                // (working) size in place instead of leaving a handle with no images behind it.
                if (XR_FAILED(img_r) || n == 0) {
                    std::printf("[host] eye %d screen swapchain images -> %s (%u) -- abandoning "
                                "this resize\n", e, rs(img_r), n);
                    ok = false;
                    break;
                }

                for (uint32_t k = 0; k < n; ++k) {
                    screen_images[e].push_back(imgs[k].texture);
                }

                // ---- NO IMAGE MAY HOLD UNTOUCHED MEMORY -------------------------------------
                //
                // Measured: over fifteen one-per-second samples the runtime handed us image index
                // 1 or 2 and NEVER 0 -- roughly a 0.2% coincidence if all three were rotating. So
                // image 0 was never written, and something that samples it finds exactly what the
                // runtime kept reporting:
                //
                //   [host] centre pixel B=24 G=25 R=23 A=10      <- what we upload
                //   [DIAG-ZEROSKIP] source image center pixel is zero   <- 160 ms later
                //
                // A contradiction on one clock, and this resolves it: the reader was not looking at
                // the image we wrote. Priming every image once removes the condition entirely,
                // whichever index anything samples, and it costs one clear per image at creation.
                //
                // Opaque black, not transparent: it also settles the other open question, since an
                // alpha of zero is the other way a pixel reads as zero.
                for (uint32_t k = 0; k < n; ++k) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
                    rtvd.Format = static_cast<DXGI_FORMAT>(screen_format);
                    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    if (SUCCEEDED(device->CreateRenderTargetView(imgs[k].texture, &rtvd, &rtv)) &&
                        rtv != nullptr) {
                        const float opaque_black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        ctx->ClearRenderTargetView(rtv, opaque_black);
                        rtv->Release();
                    }
                }
                std::printf("[host] primed %u swapchain image(s) for eye %d\n", n, e);
            }

            if (ok) {
                screen_alloc_w = alloc_w;
                screen_alloc_h = alloc_h;
                screen_w = make_w;
                screen_h = make_h;
                screen_layout = layout;
                screen_ready = false;
            } else {
                have_frame = false;
            }
        }

        // ---- THE UI LAYER'S SWAPCHAIN, CREATED LAZILY -----------------------------------------
        //
        // Only once a UI frame has actually arrived, and only at the UI header's own resolution --
        // never the runtime's recommendation, since the payload defines the size here just as it
        // does for the game's screen above. Same format family (BGRA, same sRGB-vs-fallback choice
        // as `screen_format`) because the payload is BGRA either way.
        if (ui_enabled && have_ui_frame &&
            (ui_swapchain == XR_NULL_HANDLE || uw != ui_w || uh != ui_h)) {
            ui_images.clear();

            if (ui_swapchain != XR_NULL_HANDLE) {
                // Result ignored for the same reason as the eye swapchains above -- the handle is
                // being replaced regardless.
                xrDestroySwapchain(ui_swapchain);
                ui_swapchain = XR_NULL_HANDLE;
            }

            XrSwapchainCreateInfo ui_sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ui_sc.usageFlags =
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ui_sc.format = screen_format;
            ui_sc.sampleCount = 1;
            ui_sc.width = uw;
            ui_sc.height = uh;
            ui_sc.faceCount = 1;
            ui_sc.arraySize = 1;
            ui_sc.mipCount = 1;

            const XrResult ui_r = xrCreateSwapchain(session, &ui_sc, &ui_swapchain);
            // Primed below once enumerated: the chain is capacity-sized while the content is
            // smaller, so the surround would otherwise be untouched memory -- the same hazard the
            // eye chains had.
            std::printf("[host] ui swapchain %ux%u -> %s\n", uw, uh, rs(ui_r));

            uint32_t ui_n = 0;
            XrResult ui_img_r = XR_SUCCESS;

            if (XR_SUCCEEDED(ui_r)) {
                ui_img_r = xrEnumerateSwapchainImages(ui_swapchain, 0, &ui_n, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ui_n,
                                                           {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});

                if (XR_SUCCEEDED(ui_img_r) && ui_n > 0) {
                    ui_img_r = xrEnumerateSwapchainImages(
                        ui_swapchain, ui_n, &ui_n,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
                }

                if (XR_SUCCEEDED(ui_img_r)) {
                    for (uint32_t k = 0; k < ui_n; ++k) {
                        ui_images.push_back(imgs[k].texture);
                    }
                }
            }

            if (!ui_images.empty()) {
                ui_w = uw;   // content size, not chain size: the chain is fixed at capacity
                ui_h = uh;
                ui_uploaded = false;
            } else {
                // Created but with no usable images is the same outcome as never created, and it
                // must not be left looking usable: the upload path bounds-checks against
                // ui_images, so a live handle with an empty vector would fail that check on every
                // frame from here on rather than once.
                if (ui_swapchain != XR_NULL_HANDLE) {
                    std::printf("[host] ui swapchain images -> %s (%u) -- destroying it\n",
                                rs(ui_img_r), ui_n);
                    // Result ignored: the failure being reported is the enumeration, and this is
                    // the cleanup for it.
                    xrDestroySwapchain(ui_swapchain);
                    ui_swapchain = XR_NULL_HANDLE;
                }

                ui_w = 0;
                ui_h = 0;
                have_ui_frame = false;  // nothing to upload this frame
            }
        }


        // FOCUSED gates every input and output action below -- the hands block here and the
        // haptic ring after it -- so it is derived once rather than spelled out twice.
        const bool session_focused = (state == XR_SESSION_STATE_FOCUSED);

        // ---- TELL THE GAME WHAT THE HANDS ARE DOING --------------------------------------------
        //
        // Synced and located at the SAME predictedDisplayTime and in the SAME reference `space` as
        // the head above, so a weapon driven by aim_pose and a camera driven by the head pose
        // describe one instant and one origin -- a mismatch here would not error, it would just
        // make the hands lag or sit in the wrong place.
        const double trace_act_t0 = now_ms();
        if (reader.hands != nullptr) {
            // xrSyncActions is only meaningful once the session is FOCUSED -- XR_SESSION_NOT_FOCUSED
            // is the documented result otherwise (e.g. while the system UI has input), and action
            // state is not defined to still track live input after that. Treating "not focused" as
            // "not active" here, rather than publishing whatever was last synced, is what keeps a
            // menu overlay from leaving a weapon aimed at a frozen ray.
            bool hands_active = session_focused;

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

                // ---- THESE RESULTS ARE IGNORED, AND THAT IS THE CORRECT CHOICE ----------------
                //
                // Every state and location struct here is value-initialised, so a failed call
                // leaves isActive at XR_FALSE and locationFlags at 0 -- which is precisely the
                // "no controller, nothing valid" reading the code below already handles. They run
                // eight times a hand, every frame; a log line per call would bury the ones that
                // matter, and there is no recovery available that the zeroed state does not
                // already express.
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

                // Float, vector2f and boolean action states, ignored for the reason given above:
                // each state struct is value-initialised, so isActive false -- no binding on this
                // profile, or none on this hand, since A/B and X/Y are single-hand on Touch --
                // reads as the at-rest value rather than whatever was last synced.
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

                // ---- MENU IS SHARED WITH THE GAME'S PAUSE BUTTON ------------------------------
                //
                // Right-hand Menu does not exist: the OpenXR `oculus/touch_controller` profile
                // defines menu/click on the LEFT hand only, and the right-hand system button
                // belongs to the runtime. So the panel has to share the pause button, and a
                // long-press is what separates them.
                //
                // The bit is WITHHELD on press rather than masked after the hold completes --
                // masking would be too late, the game would already have paused. On an early
                // release it is replayed as a synthetic tap so pause still works; if the hold
                // reaches the threshold the panel opens and the bit is consumed outright.
                //
                // Cost: pause is delayed by up to the threshold. That is the price of one button
                // doing two jobs, and it is only paid on the button that opens a settings panel.
                static constexpr int64_t kMenuHoldNs = 400'000'000; // 0.4s
                static constexpr int kReplayFrames = 2;             // the game polls per frame
                static XrTime menu_down_at[2] = {0, 0};
                static bool menu_consumed[2] = {false, false};
                static int menu_replay[2] = {0, 0};

                const bool menu_now = get_bool(menu_click_action);
                if (menu_now) {
                    if (menu_down_at[h] == 0) {
                        menu_down_at[h] = fs.predictedDisplayTime;
                        menu_consumed[h] = false;
                        // Prime every UI image opaque black, same reason as the eye chains.
                        for (auto* tex : ui_images) {
                            ID3D11RenderTargetView* rtv = nullptr;
                            D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
                            rtvd.Format = static_cast<DXGI_FORMAT>(screen_format);
                            rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                            if (SUCCEEDED(device->CreateRenderTargetView(tex, &rtvd, &rtv)) && rtv) {
                                const float ob[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                                ctx->ClearRenderTargetView(rtv, ob);
                                rtv->Release();
                            }
                        }

                    } else if (!menu_consumed[h] &&
                               fs.predictedDisplayTime - menu_down_at[h] >= kMenuHoldNs) {
                        settings_ui.toggle();
                        menu_consumed[h] = true;
                    }
                } else {
                    if (menu_down_at[h] != 0 && !menu_consumed[h]) {
                        menu_replay[h] = kReplayFrames; // a tap: hand it to the game now
                    }
                    menu_down_at[h] = 0;
                }
                if (menu_replay[h] > 0) {
                    --menu_replay[h];
                    buttons |= xr::kHandButtonMenu;
                }
                hi.buttons = buttons;

                hand_active_log[h] = hi.active != 0;
                hand_tracked_log[h] = hi.tracked != 0;
                hand_aim_pos_log[h][0] = hi.aim.position[0];
                hand_aim_pos_log[h][1] = hi.aim.position[1];
                hand_aim_pos_log[h][2] = hi.aim.position[2];
            }

            hs->write_qpc = 0;

            MemoryBarrier();
            hs->sequence = (hs->sequence + 1u) & ~1u;

            hands_bound_log = profile_bound;
        }
        trace_actions = now_ms() - trace_act_t0;

        // ---- THE OTHER DIRECTION: BUZZ THE CONTROLLER ------------------------------------------
        //
        // A ring the GAME writes and this loop drains -- xr::HapticsState says why it is a ring
        // and not one slot per hand. What looks paranoid below is the consumer half of that
        // contract, and every part of it is preventing a specific failure:
        //
        //   - The cursor is SEEDED from the first write_index observed, so a mod that has been
        //     shooting for ten minutes before this process started does not deliver ten minutes
        //     of backlog into the wearer's hands at once.
        //   - `avail` is an UNSIGNED difference, so the 2^32 wrap of a monotonic counter costs
        //     nothing and needs no special case.
        //   - Each entry is copied BETWEEN two reads of its `commit` stamp. A producer that laps
        //     the whole ring mid-copy changes the stamp, and the entry is dropped instead of
        //     fired with a duration that is half one pulse and half another.
        //   - The window is drained even when it cannot be fired. A buzz for a shot taken while
        //     the system menu had input, delivered whenever focus happens to come back, is
        //     attached to nothing the wearer is doing.
        if (reader.haptics != nullptr && haptic_action != XR_NULL_HANDLE) {
            xr::HapticsState* haptics = reader.haptics;
            const uint32_t write_index = haptics->write_index;
            MemoryBarrier();

            if (!haptics_seeded) {
                haptic_read = write_index;
                haptics_seeded = true;
            }

            const uint32_t avail = write_index - haptic_read;

            if (avail > xr::kHapticSlots) {
                // The oldest entries are already overwritten. Walking them anyway would fire a
                // mixture of old and new pulses; skipping to the newest full window and SAYING
                // SO in the status line is the honest failure.
                haptic_overruns += avail - xr::kHapticSlots;
                haptic_read = write_index - xr::kHapticSlots;
            }

            for (uint32_t t = haptic_read + 1u; static_cast<int32_t>(write_index - t) >= 0; ++t) {
                const xr::HapticPulse* entry = &haptics->slot[(t - 1u) % xr::kHapticSlots];

                const uint32_t commit_before = entry->commit;
                MemoryBarrier();
                const xr::HapticPulse pulse = *entry;
                MemoryBarrier();
                const uint32_t commit_after = entry->commit;

                if (commit_before != t || commit_after != t) {
                    ++haptic_torn;
                    continue;
                }

                // `hand` indexes hand_path[2], and the producer's clamp lives in another
                // process's binary. A stamp that happened to survive the copy is still not a
                // reason to index an array with a value from shared memory.
                if (pulse.hand > xr::kHandRight) {
                    ++haptic_torn;
                    continue;
                }

                if (!session_focused) {
                    continue;
                }

                XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
                hai.action = haptic_action;
                hai.subactionPath = hand_path[pulse.hand];

                XrResult haptic_r = XR_SUCCESS;

                if (pulse.stop != 0u) {
                    haptic_r = xrStopHapticFeedback(session, &hai);
                } else {
                    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                    // Range-checked HERE as well as at the producer. These go straight into the
                    // runtime, an amplitude outside [0,1] or a negative duration that is not
                    // XR_MIN_HAPTIC_DURATION is XR_ERROR_VALIDATION_FAILURE, and an entry that
                    // passed the commit check is still an entry we did not compute ourselves.
                    vib.duration =
                        pulse.duration_ns < 0 ? XR_MIN_HAPTIC_DURATION : pulse.duration_ns;
                    vib.frequency = (std::isfinite(pulse.frequency_hz) && pulse.frequency_hz > 0.0f)
                                        ? pulse.frequency_hz
                                        : XR_FREQUENCY_UNSPECIFIED;
                    vib.amplitude = std::isfinite(pulse.amplitude)
                                        ? (std::max)(0.0f, (std::min)(1.0f, pulse.amplitude))
                                        : 0.0f;
                    haptic_r = xrApplyHapticFeedback(
                        session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
                }

                if (XR_FAILED(haptic_r)) {
                    // Once, not per pulse: a runtime that refuses one refuses all of them, and a
                    // line per shot fired would bury every other message in this log.
                    if (!haptic_failure_logged) {
                        std::printf("[host] haptic feedback -> %s (not retried, logged once)\n",
                                    rs(haptic_r));
                        haptic_failure_logged = true;
                    }
                } else {
                    ++haptics_fired;
                }
            }

            haptic_read = write_index;
        }

        // One call, every frame regardless of whether the block above ran: the settings panel's
        // own menu-button/aim-ray polling must not depend on the mod publishing hands, since a
        // wearer must be able to open Settings before (or without) the mod running at all.
        const XrCompositionLayerQuad* settings_quad =
            settings_ui.update(fs.predictedDisplayTime, fs.shouldRender != XR_FALSE);

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
        // ---- ONLY WHEN THERE IS NO GAME, NOT ON A MISSED FRAME --------------------------------
        //
        // `have_frame` is per-frame, and a frame the mod has not published yet is NORMAL -- the
        // host is meant to hold the last picture (that is what `held` counts). Painting the
        // placeholder on every such frame made the logo and the game alternate, flickering badly.
        //
        // So this is a state, not a fallback: it runs until the FIRST frame ever arrives, and again
        // only after a long silence, which means the game has gone away rather than skipped a beat.
        static bool ever_had_frame = false;
        static auto last_frame_at = std::chrono::steady_clock::now();
        if (have_frame) {
            ever_had_frame = true;
            last_frame_at = std::chrono::steady_clock::now();
        }
        const auto silent_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - last_frame_at)
                                   .count();
        const bool no_game = !ever_had_frame || silent_ms > 2000;

        if (fs.shouldRender != XR_FALSE && no_game && !have_frame &&
            screen[0] != XR_NULL_HANDLE && screen_w != 0 && screen_h != 0) {
            static std::vector<uint8_t> ph;
            static uint32_t ph_w = 0, ph_h = 0;
            if (ph_w != screen_w || ph_h != screen_h) {
                placeholder::build(ph, screen_w, screen_h);
                ph_w = screen_w;
                ph_h = screen_h;
            }
            uint32_t painted = 0;
            for (uint32_t e = 0; e < 2 && !ph.empty(); ++e) {
                if (screen[e] == XR_NULL_HANDLE) {
                    continue;
                }
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_FAILED(xrAcquireSwapchainImage(screen[e], &ai, &index)) ||
                    index >= screen_images[e].size()) {
                    continue;
                }
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(screen[e], &wi))) {
                    const D3D11_BOX ph_box{0, 0, 0, screen_w, screen_h, 1};
                    ctx->UpdateSubresource(screen_images[e][index], 0, &ph_box, ph.data(),
                                           screen_w * 4u, 0);
                    ++painted;
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(screen[e], &ri);
            }
            // Without this the projection layer is never submitted -- screen_ready is otherwise
            // only set by the real-frame upload, so the placeholder would sit in a swapchain the
            // compositor is never told about.
            if (painted == 2) {
                screen_ready = true;
            }
        }

        if (fs.shouldRender != XR_FALSE && have_frame && screen[0] != XR_NULL_HANDLE) {
            bool both_eyes_uploaded = true;

            for (int e = 0; e < 2; ++e) {
                uint32_t index = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

                // BOUNDS-CHECKED AGAINST THIS SWAPCHAIN'S OWN IMAGES. A failed acquire never
                // writes `index`, so it stays 0 and the upload below would scribble into image 0
                // of a swapchain the runtime has not handed us -- and the image count is the
                // runtime's choice, two here and three there, so even a successful acquire is not
                // a promise about a vector we filled somewhere else. Same shape as
                // ui/SettingsUi.cpp's renderAndBuildQuad().
                const double t_up_0 = now_ms();
                const double trace_eye_t0 = now_ms();
                if (XR_FAILED(xrAcquireSwapchainImage(screen[e], &ai, &index)) ||
                    index >= screen_images[e].size()) {
                    ++upload_failures;
                    both_eyes_uploaded = false;
                    continue;
                }

                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;

                // "The swapchain image must have been successfully waited on without timeout
                // before it is released" -- so a failed wait forbids the copy below. The acquire
                // still has to be undone, or the swapchain loses an image per frame until it has
                // none left to give.
                if (XR_FAILED(xrWaitSwapchainImage(screen[e], &wi))) {
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(screen[e], &ri);
                    ++upload_failures;
                    both_eyes_uploaded = false;
                    continue;
                }

                // SLICING WITHOUT A SHADER. UpdateSubresource walks the source using the stride it
                // is given, so starting the right eye half a row in and keeping the FULL pitch
                // lifts the right half out in one copy. A wrong pitch here does not tint anything,
                // it shears the picture diagonally.
                const uint8_t* eye_bits = (screen_layout == xr::kLayoutSideBySide && e == 1)
                                              ? fbits + static_cast<size_t>(screen_w) * 4u
                                              : fbits;

                // A BOX, NOT THE WHOLE IMAGE. The swapchain image is allocated at the runtime's
                // maximum and only the top-left screen_w x screen_h of it is this frame's picture,
                // so a null box would both misplace the copy and walk off the end of the source.
                // ---- WHAT IS ACTUALLY IN THE MIDDLE OF THE PICTURE ----------------------------
                //
                // The runtime refuses the blit with "[DIAG-ZEROSKIP] Skipping blit: source image
                // center pixel is zero", so the centre pixel of what we upload IS zero. Which BYTES
                // are zero decides what kind of bug this is, and the two answers point opposite
                // ways:
                //   only the alpha byte -> the game's X8R8G8B8 back buffer leaves alpha 0 and the
                //                          runtime keys on it; fix with a format or a layer flag.
                //   all four bytes      -> we are publishing BLACK FRAMES and the fault is ours,
                //                          upstream of OpenXR entirely.
                // One log line a second settles it. Read from the source we are about to copy.
                if (e == 0) {
                    static ULONGLONG s_last_px = 0;
                    const ULONGLONG now_ms2 = GetTickCount64();
                    if (now_ms2 - s_last_px > 1000) {
                        s_last_px = now_ms2;
                        // WHICH IMAGE WE JUST WROTE, and how many the chain has. The runtime reads
                    // zero from "the source image" in the same second we upload a non-zero centre
                    // pixel, so it is not reading what we wrote. If acquire keeps returning the
                    // same index, the chain's other images are never written and a reader that
                    // samples one of those sees untouched memory forever -- which is a frozen view
                    // over live data, exactly as reported.
                    std::printf("%s [host] wrote image index %u of %zu\n", stamp(), index,
                                screen_images[e].size());
                    const size_t cx = static_cast<size_t>(screen_w) / 2u;
                        const size_t cy = static_cast<size_t>(screen_h) / 2u;
                        const uint8_t* px = eye_bits + cy * static_cast<size_t>(fpitch) + cx * 4u;
                        std::printf("%s [host] centre pixel B=%u G=%u R=%u A=%u (eye %ux%u pitch %u)\n",
                                    stamp(), px[0], px[1], px[2], px[3], screen_w, screen_h, fpitch);
                    }
                }

                // CENTRED, not top-left: see the allocation above for why the middle of the
                // image must never be untouched memory.
                const UINT off_x = screen_alloc_w > screen_w ? (screen_alloc_w - screen_w) / 2u : 0u;
                const UINT off_y = screen_alloc_h > screen_h ? (screen_alloc_h - screen_h) / 2u : 0u;
                const D3D11_BOX box{off_x, off_y, 0, off_x + screen_w, off_y + screen_h, 1};
                ctx->UpdateSubresource(screen_images[e][index], 0, &box, eye_bits, fpitch, 0);

                        // ---- THE FLUSH IS GONE, AND ITS RATIONALE WAS NEVER PROVEN ------------------
                //
                // It was added during the Meta XR Simulator freeze investigation, on the theory that
                // RenderingD3D11OnVulkan copies our D3D11 texture into a Vulkan image around
                // xrReleaseSwapchainImage and would therefore copy stale contents unless our
                // UpdateSubresource had already been submitted.
                //
                // That theory was never tested. The freeze it was written for turned out to be the
                // MCP operator API layer holding the simulator's display -- recorded in AGENTS.md --
                // and this was one of six changes aimed at innocent code during that hunt. Two of the
                // others were reverted for exactly this reason; this one survived because nothing
                // measured it.
                //
                // It costs a full pipeline flush per eye, measured at 3.14 ms mean across both eyes
                // for the whole acquire/upload/release span, in a frame that runs 0.3 ms over its
                // 13.889 ms budget.
                //
                // WHAT TO WATCH if this was wrong: the runtime would copy stale or black contents, so
                // the symptom is a frozen or dark view while our own centre-pixel probe still reads
                // live values -- the exact signature from that investigation. eye_upload_ms says
                // whether it was worth it.

                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                // Result ignored deliberately: a release that fails has still handed the image
                // back as far as this loop is concerned, there is nothing to retry, and the next
                // acquire reports the real state. Same choice in ui/SettingsUi.cpp.
                xrReleaseSwapchainImage(screen[e], &ri);
                trace_eye_upload += now_ms() - trace_eye_t0;
                {
                    const double d = now_ms() - t_up_0;
                    t_upload_ms += d;
                    if (d > t_upload_max) { t_upload_max = d; }
                    upload_bytes += static_cast<uint64_t>(fpitch) * static_cast<uint64_t>(eye_h);
                }
            }

            // Only once BOTH eyes landed. screen_ready is what lets the submit paths below point
            // a layer at these swapchains, and half a stereo pair -- one eye new, one eye holding
            // the previous frame -- is worse to look at than holding both.
            if (both_eyes_uploaded) {
                screen_ready = true;
            }
        }

        // ---- APPEAR / DISAPPEAR, LOGGED ONCE -----------------------------------------------
        //
        // Per-frame logging in this host is reserved for errors -- so these fire only on the
        // transition, not on every frame the layer happens to be shown or hidden.
        if (ui_enabled) {
            if (have_ui_frame && !ui_shown) {
                std::printf("[host] UI layer appeared, %ux%u\n", uw, uh);
                ui_shown = true;
            } else if (!ui_present_now && ui_shown) {
                std::printf("[host] UI layer disappeared\n");
                ui_shown = false;
            }
        }

        // ---- UPLOAD THE UI LAYER ----------------------------------------------------------------
        //
        // Same held-image approach as the game's screen above: only re-upload when there is a NEW
        // published frame, and let a swapchain whose image was already released keep showing it on
        // the frames in between.
        if (fs.shouldRender != XR_FALSE && have_ui_frame && ui_swapchain != XR_NULL_HANDLE) {
            uint32_t ui_index = 0;
            XrSwapchainImageAcquireInfo ui_ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            if (XR_FAILED(xrAcquireSwapchainImage(ui_swapchain, &ui_ai, &ui_index)) ||
                ui_index >= ui_images.size()) {
                ++upload_failures;
            } else {
                XrSwapchainImageWaitInfo ui_wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                ui_wi.timeout = XR_INFINITE_DURATION;
                const bool ui_waited = XR_SUCCEEDED(xrWaitSwapchainImage(ui_swapchain, &ui_wi));

                if (ui_waited) {
                    if (uderive_alpha != 0) {
                        // ---- ALPHA IS NOT IN THE PIXELS -------------------------------------
                        //
                        // The engine's UI shaders emit zero alpha and forcing the colour-write
                        // mask does not change that (measured 0xF for the whole bracket). The
                        // surface is cleared to transparent black and only the UI draws into it,
                        // so RGB is already the premultiplied contribution and its brightness IS
                        // its coverage -- deriving A = max(R,G,B) here is exact, not an
                        // approximation. Paid on the host rather than the game's render thread,
                        // because the host is already touching every pixel to upload it and the
                        // game would pay it inside the frame instead.
                        ui_staging.resize(static_cast<size_t>(upitch) * uh);

                        for (uint32_t row = 0; row < uh; ++row) {
                            const uint8_t* src = ubits + static_cast<size_t>(row) * upitch;
                            uint8_t* dst = ui_staging.data() + static_cast<size_t>(row) * upitch;

                            for (uint32_t x = 0; x < uw; ++x) {
                                const uint8_t b = src[x * 4 + 0];
                                const uint8_t g = src[x * 4 + 1];
                                const uint8_t r = src[x * 4 + 2];
                                dst[x * 4 + 0] = b;
                                dst[x * 4 + 1] = g;
                                dst[x * 4 + 2] = r;
                                dst[x * 4 + 3] = (std::max)(b, (std::max)(g, r));
                            }
                        }

                        // BOUNDED: the chain is capacity-sized and the content is smaller, so a
                        // null box would both misplace the copy and read past the source.
                        const D3D11_BOX ui_box{0, 0, 0, uw, uh, 1};
                        ctx->UpdateSubresource(ui_images[ui_index], 0, &ui_box, ui_staging.data(),
                                               upitch, 0);
                    } else {
                        const D3D11_BOX ui_box2{0, 0, 0, uw, uh, 1};
                        ctx->UpdateSubresource(ui_images[ui_index], 0, &ui_box2, ubits, upitch, 0);
                        ctx->Flush();  // same interop reason as the eye uploads
                    }

                    ui_uploaded = true;
                } else {
                    ++upload_failures;
                }

                // Released whether or not the wait succeeded: the wait is what licenses the COPY
                // above, but an acquired image that is never released is one the swapchain never
                // gets back. The release's own result is ignored for the reason given at the
                // game-screen upload. ui/SettingsUi.cpp makes the same pair of decisions.
                XrSwapchainImageReleaseInfo ui_ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(ui_swapchain, &ui_ri);
            }
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
        if (rendered_seq != 0) {
            if (last_rendered_seq != 0) {
                const int32_t d = static_cast<int32_t>(rendered_seq - last_rendered_seq);
                if (d < 0) {
                    ++seq_backwards;  // OUT OF ORDER -- a frame older than one already shown
                } else if (d == 0) {
                    ++seq_repeats;    // the same picture again; expected when the game is slower
                }
            }
            if (static_cast<int32_t>(rendered_seq - last_rendered_seq) > 0) {
                last_rendered_seq = rendered_seq;
            }
            // Age in POSE STEPS: sequences advance by two per publish (seqlock), so halve it.
            const int32_t delta = static_cast<int32_t>(published_sequence - rendered_seq);
            // The submit site's lookup, repeated here because that one is declared further down.
            const uint32_t ms_seq = rendered_seq - (pose_lag_steps * 2u);
            const PosePair& ms_slot = pose_history[(ms_seq / 2u) % 16u];
            if (pose_ever_published && ms_slot.predicted_for != 0 && ms_slot.sequence == ms_seq) {
                const double ms =
                    static_cast<double>(fs.predictedDisplayTime - ms_slot.predicted_for) / 1.0e6;
                if (ms >= 0.0 && ms < 1000.0) {
                    ++stale_ms_samples;
                    stale_ms_total += ms;
                    if (ms > stale_ms_worst) {
                        stale_ms_worst = ms;
                    }
                }
            }
            if (pose_ever_published && delta >= 0) {
                const uint32_t age = static_cast<uint32_t>(delta) / 2u;
                ++age_samples;
                age_total += age;
                if (age > age_worst) {
                    age_worst = age;
                }
                ++age_hist[age > 3u ? 3u : age];
            }
        }
        // Deliberately reach further back when asked. The lookup is by sequence, so an older slot
        // is a genuinely older pose the game really did render from at some point -- not a
        // fabricated one, which the runtime would be entitled to treat as garbage.
        const uint32_t lag_seq = rendered_seq - (pose_lag_steps * 2u);
        const PosePair& slot = pose_history[(lag_seq / 2u) % 16u];
        // ---- THE GAME CAN ASK NOT TO BE PROJECTED ------------------------------------------
        //
        // While a menu is up the game's camera stops following the head, so a projection layer's
        // promise -- "rendered from the pose you gave me" -- is false, and the compositor
        // reprojects a frozen world against real head motion. That is the swimming, nauseating
        // picture reported from the headset while paused.
        //
        // The quad path below already exists for exactly this reasoning; it was simply reserved
        // for the pre-tracking case. `flat` lets the GAME trigger it, because only the game knows
        // a menu is up.
        const bool flat_requested = reader.frame_flat();
        XrQuaternionf stated{};
        bool stated_ok = reader.frame_rendered_pose(stated);

        // ---- NORMALISED BEFORE IT GOES ANYWHERE NEAR A LAYER -----------------------------------
        //
        // "A runtime must return XR_ERROR_POSE_INVALID if the orientation norm deviates by more
        // than 1% from unit length", and xrEndFrame lists XR_ERROR_POSE_INVALID among its errors.
        // So one bad quaternion does not cost one eye -- it drops the WHOLE frame, both eyes and
        // every other layer with it. This value crossed a process boundary from an engine that
        // composes rotations of its own, so unit length is a hope rather than a guarantee.
        //
        // Once, not per eye: both eyes are given the same orientation, so normalising inside the
        // per-eye loop would only pay for the same divide twice.
        if (stated_ok) {
            const float norm2 = stated.x * stated.x + stated.y * stated.y + stated.z * stated.z +
                                stated.w * stated.w;

            if (std::isfinite(norm2) && norm2 > 1.0e-12f) {
                const float inv = 1.0f / sqrtf(norm2);
                stated.x *= inv;
                stated.y *= inv;
                stated.z *= inv;
                stated.w *= inv;
            } else {
                // No direction to recover. Dividing by a zero or NaN norm produces a quaternion
                // the compositor rejects exactly as hard as the one we started with, so the
                // remembered pose stands instead.
                stated_ok = false;
            }
        }

        if (!stated_ok) {
            ++stated_absent;
        }
        const bool pose_known =
            !flat_requested && use_projection && rendered_seq != 0 && slot.sequence == lag_seq;

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
            const float tx = tan_half_angle(slot.rendered.angleRight);
            const float ty = tan_half_angle(slot.rendered.angleUp);

            for (int e = 0; e < 2; ++e) {
                const XrFovf& want = slot.wanted[e];

                // What the GAME actually rendered: the symmetric frustum we asked it for.
                XrFovf symmetric_fov{};
                {
                    const float mx = std::atan(std::fabs(std::tan(want.angleLeft)) >
                                                       std::fabs(std::tan(want.angleRight))
                                                   ? std::fabs(std::tan(want.angleLeft))
                                                   : std::fabs(std::tan(want.angleRight)));
                    const float my = std::atan(std::fabs(std::tan(want.angleUp)) >
                                                       std::fabs(std::tan(want.angleDown))
                                                   ? std::fabs(std::tan(want.angleUp))
                                                   : std::fabs(std::tan(want.angleDown)));
                    symmetric_fov.angleLeft = -mx;
                    symmetric_fov.angleRight = mx;
                    symmetric_fov.angleUp = my;
                    symmetric_fov.angleDown = -my;
                }

                const float x0 = (tan_half_angle(want.angleLeft) + tx) / (2.0f * tx);
                const float x1 = (tan_half_angle(want.angleRight) + tx) / (2.0f * tx);

                // Y IS FLIPPED: angleUp is positive upward, image rows run downward, so the TOP of
                // the rectangle comes from angleUp.
                const float y0 = (ty - tan_half_angle(want.angleUp)) / (2.0f * ty);
                const float y1 = (ty - tan_half_angle(want.angleDown)) / (2.0f * ty);

                auto to_px = [](float f, uint32_t extent) {
                    const int32_t v = static_cast<int32_t>(lroundf(f * static_cast<float>(extent)));
                    return v < 0 ? 0 : (v > static_cast<int32_t>(extent) ? static_cast<int32_t>(extent) : v);
                };

                // MEASURED AGAINST THE PICTURE, which is what was rendered. Deriving this from the
                // allocation was wrong on its own terms: only 2160 px of a 4320-wide image are ever
                // written, so a rect claiming 3477 named pixels that do not exist and declared a
                // FOV to match. It also did not fix the freeze -- starting the host in an already
                // loaded world freezes with no geometry change at all -- so rect constancy was
                // never the issue and correctness wins.
                const int32_t px0 = to_px(x0, screen_w);
                const int32_t px1 = to_px(x1, screen_w);
                const int32_t py0 = to_px(y0, screen_h);
                const int32_t py1 = to_px(y1, screen_h);

                proj_views[e].pose = slot.pose[e];

            // ---- PREFER WHAT THE WRITER STATES OVER WHAT WE REMEMBER SENDING -------------------
            //
            // The orientation only: the per-eye POSITIONS come from this record and are still the
            // right ones, since the game derives its eye offset from the same IPD. What the game
            // cannot tell us is where its eyes were in the room, and what we cannot know is what
            // its engine did to the rotation after we handed it over.
            if (stated_ok) {
                if (e == 0) {
                    const float d = std::fabs(stated.x * slot.pose[e].orientation.x +
                                              stated.y * slot.pose[e].orientation.y +
                                              stated.z * slot.pose[e].orientation.z +
                                              stated.w * slot.pose[e].orientation.w);
                    const double deg =
                        2.0 * std::acos(static_cast<double>(d > 1.0f ? 1.0f : d)) * 57.2957795;
                    ++stated_samples;
                    stated_sum_deg += deg;
                    if (deg > stated_worst_deg) {
                        stated_worst_deg = deg;
                    }
                }
                proj_views[e].pose.orientation = stated;
            }

                // Declare what the CROP represents, not what was rendered: the rectangle now is the
                // headset's own asymmetric frustum.
                proj_views[e].fov = want;
                proj_views[e].subImage.swapchain = screen[e];
                // The picture may be centred inside a larger image, so the rect carries that
                // offset. With the allocation sized to the frame these are simply zero.
                const int32_t cx_off =
                    screen_alloc_w > screen_w
                        ? static_cast<int32_t>((screen_alloc_w - screen_w) / 2u) : 0;
                const int32_t cy_off =
                    screen_alloc_h > screen_h
                        ? static_cast<int32_t>((screen_alloc_h - screen_h) / 2u) : 0;

                // ---- TESTING WHETHER THE CROP ITSELF IS WHAT THE RUNTIME DISLIKES --------------
                //
                // The game renders the smallest SYMMETRIC frustum containing the headset's, and we
                // declare the headset's ASYMMETRIC one with a crop (see HostState::fov_x). That is
                // correct per the spec and works on hardware, but it is the one structural thing
                // this submission does that a conventional app does not -- so --no-crop submits the
                // whole rendered picture with the symmetric FOV instead, which is a plain
                // full-image projection layer. If the freeze goes with it, the crop is implicated.
                if (no_crop) {
                    proj_views[e].fov = symmetric_fov;
                    proj_views[e].subImage.imageRect.offset = {cx_off, cy_off};
                    proj_views[e].subImage.imageRect.extent = {
                        static_cast<int32_t>(screen_w), static_cast<int32_t>(screen_h)};
                } else {
                    proj_views[e].subImage.imageRect.offset = {px0 + cx_off, py0 + cy_off};
                }
                if (!no_crop) {
                    proj_views[e].subImage.imageRect.extent = {(std::max)(1, px1 - px0),
                                                               (std::max)(1, py1 - py0)};
                }
            }

            // ---- WHAT WE TELL THE RUNTIME TO SAMPLE -----------------------------------------
            //
            // The upload is proven live (the centre pixel changes as the wearer moves) and the
            // runtime still shows a stale frame, so the remaining unknown is whether the RECT we
            // declare covers the region we filled. The runtime sizes its readback to this rect --
            // it logged 1738x2224 while the eye picture is 2160x2224 -- so a rect offset into
            // unwritten pixels looks exactly like a freeze while every byte we upload is correct.
            {
                static ULONGLONG s_last_rect = 0;
                const ULONGLONG now_r = GetTickCount64();
                if (now_r - s_last_rect > 1000) {
                    s_last_rect = now_r;
                    const auto& l = proj_views[0].subImage.imageRect;
                    const auto& rr = proj_views[1].subImage.imageRect;
                    std::printf("%s [host] rect L off(%d,%d) ext(%dx%d) R off(%d,%d) ext(%dx%d) filled %ux%u\n",
                                stamp(), l.offset.x, l.offset.y, l.extent.width, l.extent.height,
                                rr.offset.x, rr.offset.y, rr.extent.width, rr.extent.height,
                                screen_w, screen_h);
                }
            }
            // ---- NAME WHATEVER CHANGES, THE MOMENT IT CHANGES -------------------------------
            //
            // Sampling once a second cannot say WHICH value moved when the runtime re-enumerates
            // and freezes. Three fixes were aimed at the swapchain and one at the rect on the
            // strength of a guess about that. This fires only on a CHANGE, and prints every
            // number we hand the runtime, so the next transition names its own cause.
            {
                static uint32_t p_aw = 0, p_ah = 0, p_sw = 0, p_sh = 0, p_lay = 99;
                static int32_t p_lx = -1, p_ly = -1, p_lw = -1, p_lh = -1;
                static int32_t p_rx = -1, p_ry = -1, p_rw = -1, p_rh = -1;
                static uint32_t p_uiw = 0, p_uih = 0;
                const auto& L = proj_views[0].subImage.imageRect;
                const auto& R = proj_views[1].subImage.imageRect;
                if (p_aw != screen_alloc_w || p_ah != screen_alloc_h || p_sw != screen_w ||
                    p_sh != screen_h || p_lay != static_cast<uint32_t>(screen_layout) ||
                    p_lx != L.offset.x || p_ly != L.offset.y || p_lw != L.extent.width ||
                    p_lh != L.extent.height || p_rx != R.offset.x || p_ry != R.offset.y ||
                    p_rw != R.extent.width || p_rh != R.extent.height || p_uiw != ui_w ||
                    p_uih != ui_h) {
                    std::printf("%s [host] GEOMETRY CHANGED alloc %ux%u picture %ux%u layout %u "
                                "L off(%d,%d) ext(%dx%d) R off(%d,%d) ext(%dx%d) ui %ux%u\n",
                                stamp(), screen_alloc_w, screen_alloc_h, screen_w, screen_h,
                                static_cast<uint32_t>(screen_layout), L.offset.x, L.offset.y,
                                L.extent.width, L.extent.height, R.offset.x, R.offset.y,
                                R.extent.width, R.extent.height, ui_w, ui_h);
                    p_aw = screen_alloc_w; p_ah = screen_alloc_h; p_sw = screen_w;
                    p_sh = screen_h; p_lay = static_cast<uint32_t>(screen_layout);
                    p_lx = L.offset.x; p_ly = L.offset.y; p_lw = L.extent.width;
                    p_lh = L.extent.height; p_rx = R.offset.x; p_ry = R.offset.y;
                    p_rw = R.extent.width; p_rh = R.extent.height;
                    p_uiw = ui_w; p_uih = ui_h;
                }
            }
            proj.space = space;
            proj.viewCount = 2;
            proj.views = proj_views;
            append_layer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
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
                append_layer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad[e]));
            }

            ++submitted;
        } else if (fs.shouldRender != XR_FALSE) {
            // ---- NOTHING TO SUBMIT YET -------------------------------------------------------
            //
            // A startup test pattern used to live here, submitting a coloured clear through its own
            // swapchain pair. It was removed while hunting a freeze that turned out to be the MCP
            // operator layer rather than anything we submit -- so the reasoning recorded here at the
            // time (that a second swapchain set was fatal to the simulator) was WRONG.
            //
            // It stays removed on its own merit: the black FEAR2VR placeholder already covers "no
            // game frame yet" and paints into screen[], the same chains everything else uses, so a
            // second set bought nothing. A frame with no layers is legal.
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                std::printf("[host] no content and no screen swapchains yet -- submitting an empty "
                            "frame rather than creating a second swapchain set\n");
            }
        }

        // ---- THE UI LAYER, ON TOP OF WHATEVER WORLD LAYER WAS JUST CHOSEN -----------------------
        //
        // Submitted AFTER layers[0..layer_count) above regardless of which of the three world
        // paths ran, so it composites in front of the projection layer, the two-quad fallback, or
        // even the startup test pattern. Gated on `ui_present_now` rather than `have_ui_frame`: the
        // mod being off, or the wearer having no HUD open, must hide the quad immediately rather
        // than keep showing whatever was uploaded last.
        XrCompositionLayerQuad ui_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

        if (ui_enabled && fs.shouldRender != XR_FALSE && ui_present_now && ui_uploaded &&
            ui_swapchain != XR_NULL_HANDLE) {
            // PREMULTIPLIED, NOT UNPREMULTIPLIED: the colour (and the alpha this host just derived
            // for it) is already multiplied by coverage -- see UiFrameHeader::premultiplied and the
            // upload above. Setting XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT here would ask
            // the compositor to multiply by alpha a SECOND time and double-darken every edge.
            ui_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            // HEAD-LOCKED, in VIEW space. A HUD is instrument-panel furniture: it has to be
            // where the wearer is looking, not at a fixed point in the room. Posed in LOCAL space
            // the quad looks perfectly stable while facing forward and is BEHIND YOU the moment you
            // turn -- a health bar you cannot see is not a health bar. `--ui-world-fixed` keeps the
            // LOCAL placement for anyone who wants the panel nailed to the room instead.
            ui_quad.space = ui_world_fixed ? space : view_space;
            ui_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            ui_quad.subImage.swapchain = ui_swapchain;
            ui_quad.subImage.imageRect.offset = {0, 0};
            ui_quad.subImage.imageRect.extent = {static_cast<int32_t>(ui_w),
                                                 static_cast<int32_t>(ui_h)};

            // Straight ahead at the configured distance. In VIEW space that is literally "in front
            // of the wearer"; in LOCAL it is in front of the recentred origin.
            ui_quad.pose.orientation.w = 1.0f;
            ui_quad.pose.position = {0.0f, 0.0f, -ui_distance_m};

            // HEIGHT FROM ASPECT, never stretched: the published UI image can be any resolution,
            // so only the width is a user-tunable constant and the height follows it.
            ui_quad.size.width = ui_width_m;
            ui_quad.size.height = ui_width_m * static_cast<float>(ui_h) / static_cast<float>(ui_w);

            append_layer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ui_quad));
        }

        // The settings panel's own quad, independent of ui_enabled/ui_present_now above -- it is
        // not the mod's HUD, so it must stay available even when that HUD is off. nullptr while
        // hidden (menu button not pressed), not yet initialised, or on a frame the runtime said
        // not to render.
        //
        // shouldRender is re-checked here rather than trusted to the panel: "the application
        // should avoid heavy GPU work where possible, for example by skipping layer rendering and
        // then omitting those layers when calling xrEndFrame" is an obligation on THIS call site,
        // and it must hold whatever ui/SettingsUi.cpp does next.
        if (fs.shouldRender != XR_FALSE && settings_quad != nullptr) {
            append_layer(reinterpret_cast<const XrCompositionLayerBaseHeader*>(settings_quad));
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = blend_mode;
        fei.layerCount = layer_count;
        fei.layers = layer_count != 0 ? layers : nullptr;
        const double t_end_0 = now_ms();
        // END is where the game's pixels arrive: it publishes, THEN asks. So waiting here replaces
        // the bounded content wait -- the frame is ready by construction rather than by hoping.
        // AN END MUST NAME THE FRAME THAT WAS BEGUN. A timeout upstream can leave the game
        // recovering a frame behind, and ending someone else's frame is worse than ending none --
        // the compositor would reproject using a pose belonging to a different frame.
        //
        // But the begun frame is still ended REGARDLESS. Skipping xrEndFrame here was wrong: it
        // leaves an outstanding begin, and the next iteration's xrBeginFrame then compounds a
        // call-order error instead of recovering from one. The mismatched request is refused; the
        // FRAME is still discharged, which is what the spec's pairing actually demands.
        const bool hsk_end_valid = hsk_end && hsk_end_id == s_begun_request;
        if (hsk_end && !hsk_end_valid) {
            handshake_ack(hsk, xr::kPhaseEnd, XR_ERROR_CALL_ORDER_INVALID, nullptr, nullptr,
                          s_begun_request, hsk_end_id);
        }

        // ---- DID THE WRITER WRAP INTO THE SLOT WE JUST UPLOADED? -------------------------------
        //
        // Asked here, while the answer still means something. A changed generation says the submitted
        // pixels are a mixture of two frames -- torn, and until now undetectable. Counted rather than
        // acted on: the upload has already happened, and reporting it is what turns "three slots is
        // enough grace" into a measurement instead of an assumption. It is also the gate on restoring
        // pipelining: if this stays zero with the game running freely, the grace is real.
        if (reader.header != nullptr && reader.snap_rendered_valid != 0u &&
            reader.header->slot_generation[reader.snap_slot] != reader.snap_slot_generation) {
            ++reader.payload_overwritten;
        }

        trace_post_end = now_ms() - trace_post_end_t0;
        const double trace_end_begin_ms = now_ms();
        const XrResult end = xrEndFrame(session, &fei);

        // END ACK. The frame just begun is now ended, so nothing is outstanding.
        // CLEARED UNCONDITIONALLY: xrEndFrame ran, so nothing is outstanding whatever the game did
        // or failed to do. Clearing it only on a served request left a stale id behind on the
        // END-timeout path, and the next frame would then refuse a perfectly good END.
        s_begun_request = xr::kNoRequest;

        if (hsk_end_valid) {
            handshake_ack(hsk, xr::kPhaseEnd, end, nullptr, nullptr, xr::kNoRequest, hsk_end_id);
        }
        if (trace_frames) {
            // CAPTURE ONLY. A printf per frame at 72 Hz perturbs the very interval it measures --
            // worse on a console or a captured harness -- so a sample is fixed POD into a ring and
            // NOTHING is written here. No flush at wrap either: writing thousands of CSV rows on
            // one frame is the same observer effect wearing a different hat. The ring is dumped
            // once, after the loop.
            //
            // A long session therefore keeps the LAST kTraceCap frames, which is what a transition
            // hunts for anyway: run it, let it oscillate, stop it, read the tail.
            auto& s = g_trace[g_trace_n % kTraceCap];
            s.iv = trace_wait_interval;
            s.wait_blk = trace_wait_blocked;
            s.begin_dur = trace_begin_dur;
            s.period_ms = static_cast<double>(fs.predictedDisplayPeriod) / 1.0e6;
            s.content_cost = trace_content_cost;
            s.end_req_wait = trace_end_req_wait;
            s.aw_wait = trace_aw_wait;
            s.aw_begin = trace_aw_begin;
            s.aw_end = trace_aw_end;
            s.post_end = trace_post_end;
            s.eye_upload = trace_eye_upload;
            s.actions = trace_actions;
            s.end_dur = now_ms() - trace_end_begin_ms;
            s.seq = static_cast<uint32_t>(reader.frame_host_sequence());
            s.layers = static_cast<uint32_t>(fei.layerCount);
            s.new_content = have_frame ? 1u : 0u;
            s.begin_r = static_cast<int32_t>(begin_r);
            s.end_r = static_cast<int32_t>(end);
            ++g_trace_n;
        }
        {
            const double d = now_ms() - t_end_0;
            t_end_ms += d;
            if (d > t_end_max) { t_end_max = d; }
        }

        if (++frames % 90 == 0) {
            // ---- SPLIT ON PURPOSE ----------------------------------------------------------
            //
            // This was ONE printf with 36 specifiers and it broke three times in a single
            // session: arguments appended where they read well rather than where the format
            // wanted them, a double's bit pattern printed as %llu, and finally a format field
            // left behind when its argument was deleted -- which shifted everything after it and
            // put a float where a %s expected a pointer. That one CRASHED THE HOST, in strnlen,
            // and it is the only crash this process has ever produced.
            //
            // A count that has to be maintained by hand will drift, and mine did, repeatedly.
            // Several small statements cannot: each is short enough to verify by eye, and a
            // mistake in one cannot corrupt the fields of another.
            std::printf("[host] %llu frames, %llu submitted, %llu held, projection %llu, "
                        "missed pose %llu, state %s, last xrEndFrame %s\n",
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(submitted),
                        static_cast<unsigned long long>(held),
                        static_cast<unsigned long long>(pose_hits),
                        static_cast<unsigned long long>(pose_misses),
                        state_name(state), rs(end));

            std::printf("[host]   pose: age %.2f/%llu, hist %llu/%llu/%llu/%llu, stale %.1f/%.1f ms, "
                        "STATED-vs-INDEX %.3f/%.3f deg over %llu (absent %llu)\n",
                        age_samples ? static_cast<double>(age_total) / static_cast<double>(age_samples)
                                    : 0.0,
                        static_cast<unsigned long long>(age_worst),
                        static_cast<unsigned long long>(age_hist[0]),
                        static_cast<unsigned long long>(age_hist[1]),
                        static_cast<unsigned long long>(age_hist[2]),
                        static_cast<unsigned long long>(age_hist[3]),
                        stale_ms_samples ? stale_ms_total / static_cast<double>(stale_ms_samples) : 0.0,
                        stale_ms_worst,
                        stated_samples ? stated_sum_deg / static_cast<double>(stated_samples) : 0.0,
                        stated_worst_deg,
                        static_cast<unsigned long long>(stated_samples),
                        static_cast<unsigned long long>(stated_absent));

            std::printf("[host]   frame ms/90: wait %.1f (max %.1f), content %.1f (max %.1f), "
                        "upload %.1f (max %.1f, %.1f MB), end %.1f (max %.1f)\n",
                        t_wait_ms, t_wait_max, t_content_ms, t_content_max,
                        t_upload_ms, t_upload_max,
                        static_cast<double>(upload_bytes) / (1024.0 * 1024.0),
                        t_end_ms, t_end_max);
            t_wait_ms = t_content_ms = t_end_ms = t_upload_ms = 0.0;
            t_wait_max = t_content_max = t_end_max = t_upload_max = 0.0;
            upload_bytes = 0;

            std::printf("[host]   transport: OUT-OF-ORDER %llu, repeats %llu, content-wait %llu, "
                        "POSE-SKIP %llu/%llu\n",
                        static_cast<unsigned long long>(seq_backwards),
                        static_cast<unsigned long long>(seq_repeats),
                        static_cast<unsigned long long>(content_waits_expired),
                        static_cast<unsigned long long>(pose_skip_should),
                        static_cast<unsigned long long>(pose_skip_locate));

            std::printf("[host]   hands bound %s, L %s/%s (%.2f,%.2f,%.2f) R %s/%s (%.2f,%.2f,%.2f)\n",
                        hands_bound_log ? "yes" : "no",
                        hand_active_log[xr::kHandLeft] ? "active" : "idle",
                        hand_tracked_log[xr::kHandLeft] ? "tracked" : "inferred",
                        hand_aim_pos_log[xr::kHandLeft][0], hand_aim_pos_log[xr::kHandLeft][1],
                        hand_aim_pos_log[xr::kHandLeft][2],
                        hand_active_log[xr::kHandRight] ? "active" : "idle",
                        hand_tracked_log[xr::kHandRight] ? "tracked" : "inferred",
                        hand_aim_pos_log[xr::kHandRight][0], hand_aim_pos_log[xr::kHandRight][1],
                        hand_aim_pos_log[xr::kHandRight][2]);

            // A LINE OF ITS OWN, for the reason spelled out above: these counters were added
            // after the crash that split this block, and appending them to a neighbouring format
            // string is exactly the edit that caused it.
            std::printf("[host]   haptics: %llu fired, %llu dropped (ring overrun), %llu torn, "
                        "swapchain upload failures %llu\n",
                        static_cast<unsigned long long>(haptics_fired),
                        static_cast<unsigned long long>(haptic_overruns),
                        static_cast<unsigned long long>(haptic_torn),
                        static_cast<unsigned long long>(upload_failures));

            // ITS OWN LINE, same reasoning as the comment above. Nonzero means the writer wrapped
            // into a slot while it was being uploaded, so those frames went out as a mixture of two
            // -- torn pixels, which was undetectable before slot_generation existed. It should read 0
            // under lock-step by construction; it is the gate on letting the game run freely again.
            std::printf("[host]   payload overwritten mid-upload: %llu\n",
                        static_cast<unsigned long long>(reader.payload_overwritten));
        }

        if (max_seconds > 0 && GetTickCount64() - started > static_cast<ULONGLONG>(max_seconds) * 1000) {
            break;
        }
    }

    std::printf("[host] stopping: %llu frames, %llu submitted, final state %s\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(submitted), state_name(state));

    // Before xrEndSession/xrDestroySession: shutdown() destroys this panel's own swapchain and its
    // D3D11 resources, both of which need `session`/`device`/`ctx` still alive to release cleanly.
    settings_ui.shutdown();

    // ---- ASK TO LEAVE, THEN WAIT TO BE TOLD TO -------------------------------------------------
    //
    // xrEndSession is legal ONLY from XR_SESSION_STATE_STOPPING: "it must call xrEndSession when
    // the session is in the XR_SESSION_STATE_STOPPING state, otherwise
    // XR_ERROR_SESSION_NOT_STOPPING will be returned". Calling it unconditionally, as this used
    // to, therefore did nothing on every clean shutdown -- the session was destroyed while still
    // running, and the runtime was left tearing down a compositor client that never said goodbye.
    //
    // The transition is the RUNTIME's to make. xrRequestExitSession asks for it ("if the
    // application wishes to exit a running session, the application can call
    // xrRequestExitSession") and the answer arrives as an ordinary session-state event, so the
    // same pump the frame loop uses drives it here rather than a second copy of the switch.
    //
    // BOUNDED, because a wedged or already-lost runtime must not hold shutdown open forever. Past
    // the bound xrEndSession is skipped -- out of state it would only add an error -- and
    // xrDestroySession below runs either way, which is legal from any state.
    if (running) {
        std::printf("[host] xrRequestExitSession -> %s\n", rs(xrRequestExitSession(session)));

        const ULONGLONG exit_deadline = GetTickCount64() + 2000;

        while (running && GetTickCount64() < exit_deadline) {
            pump_events();

            if (running) {
                Sleep(5);
            }
        }

        if (running) {
            std::printf("[host] session never reached STOPPING within 2s (state %s) -- skipping "
                        "xrEndSession and destroying it as-is\n", state_name(state));
        }
    }

    // ---- EVERY DESTROY BELOW IGNORES ITS RESULT, ON PURPOSE ------------------------------------
    //
    // xrDestroy* can only report an invalid handle or an already-lost instance, and both are
    // states this teardown is already walking out of -- there is no alternative action, and
    // xrDestroyInstance at the end reclaims anything a failure left behind anyway. Checking them
    // would add branches that can only ever print.
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

    if (ui_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ui_swapchain);
    }

    if (space != XR_NULL_HANDLE) {
        xrDestroySpace(space);
    }

    // Destroyed here rather than left to xrDestroyInstance: it is created beside `space` and the
    // head pose published every frame is an xrLocateSpace on it, so the two belong together.
    // Safe at this point because settings_ui.shutdown() above has already released its borrow.
    if (view_space != XR_NULL_HANDLE) {
        xrDestroySpace(view_space);
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

    // NOT SERVICING ANY MORE. Cleared before teardown so a game still running falls back to its
    // legacy path instead of blocking on a host that has left the loop.
    if (auto* hsk_exit = reader.handshake()) {
        hsk_exit->host_servicing = 0u;
    }

    trace_flush();  // once, outside the loop -- see the ring above

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

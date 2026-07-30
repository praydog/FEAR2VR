#include "Watchpoints.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "sdk/Memory.hpp"
#include "sdk/Modules.hpp"

#include "Log.hpp"

namespace {

constexpr size_t kSlots = 4;      // the hardware budget, not a tunable
constexpr size_t kTableSize = 96; // distinct instruction pointers we can aggregate
constexpr size_t kMaxTrackedThreads = 96;

struct SlotState {
    std::atomic<bool> armed{false};
    std::atomic<uintptr_t> address{0};
    std::atomic<uint8_t> size{0};
    std::atomic<uint8_t> access{0};
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> max_hits{0};
    std::atomic<bool> auto_disarmed{false};
    std::atomic<uint32_t> threads_applied{0};
};

// One aggregated accessor. `eip` doubles as the claim token: non-zero means the record is owned and its plain
// fields have been written by the claiming thread.
struct Record {
    std::atomic<uintptr_t> eip{0};
    std::atomic<uint64_t> count{0};
    uint32_t thread_id{};
    uint8_t slot{};
    bool is_fault{};
    uint32_t regs[8]{};  // eax ebx ecx edx esi edi ebp esp
    uint8_t value[8]{};
    uint8_t value_size{};
    uint32_t stack[24]{};
};

SlotState g_slots[kSlots];
Record g_table[kTableSize];
std::atomic<uint64_t> g_dropped{0};      // hits we could not aggregate because the table was full
std::atomic<bool> g_need_disarm{false};  // a handler auto-disarmed and wants the cross-thread clear
std::atomic<uint32_t> g_pending_disarm{0};
void* g_veh{nullptr};
std::atomic<uint32_t> g_frame{0};

// Threads we have already written debug registers into. Kept so the periodic re-application only has to touch
// threads created since -- suspending every thread in the process on a timer would be a visible hitch.
std::atomic<uint32_t> g_known_tids[kMaxTrackedThreads];
std::atomic<size_t> g_known_count{0};

bool tid_known(uint32_t tid) {
    const size_t n = g_known_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < kMaxTrackedThreads; ++i) {
        if (g_known_tids[i].load(std::memory_order_relaxed) == tid) {
            return true;
        }
    }
    return false;
}

void tid_remember(uint32_t tid) {
    const size_t n = g_known_count.load(std::memory_order_acquire);
    if (n >= kMaxTrackedThreads) {
        return;
    }
    g_known_tids[n].store(tid, std::memory_order_relaxed);
    g_known_count.store(n + 1, std::memory_order_release);
}

// ---- DR7 ENCODING ------------------------------------------------------------------------------------
//
// Per slot i: the local-enable bit at 2i, then a 4-bit field at 16 + 4i holding R/W in the low two bits and
// LEN in the high two. Read-modify-write throughout, preserving every bit we do not own -- DR7 has reserved
// bits that must keep their values, and clobbering the whole register would also silently drop another slot.

constexpr uint32_t kLenByte = 0b00;
constexpr uint32_t kLenWord = 0b01;
constexpr uint32_t kLenDword = 0b11;

uint32_t len_bits(uint8_t size) {
    switch (size) {
        case 1: return kLenByte;
        case 2: return kLenWord;
        case 4: return kLenDword;
        default: return kLenByte;
    }
}

uint32_t dr7_with_slot(uint32_t dr7, size_t i, bool enable, uint8_t size, uint8_t rw) {
    const uint32_t enable_bit = 1u << (i * 2);
    const uint32_t field_shift = 16 + static_cast<uint32_t>(i) * 4;
    dr7 &= ~enable_bit;
    dr7 &= ~(0xFu << field_shift);
    if (enable) {
        dr7 |= enable_bit;
        dr7 |= ((static_cast<uint32_t>(rw) & 0x3u) | (len_bits(size) << 2)) << field_shift;
        // LE/GE. Legacy on modern parts and ignored there, but Intel documents them for exact data-breakpoint
        // reporting on older processors, and setting them costs nothing.
        dr7 |= 0x300u;
    }
    return dr7;
}

void write_slot_into_context(CONTEXT& ctx, size_t i, bool enable, uintptr_t address, uint8_t size, uint8_t rw) {
    switch (i) {
        case 0: ctx.Dr0 = address; break;
        case 1: ctx.Dr1 = address; break;
        case 2: ctx.Dr2 = address; break;
        default: ctx.Dr3 = address; break;
    }
    ctx.Dr7 = dr7_with_slot(static_cast<uint32_t>(ctx.Dr7), i, enable, size, rw);
}

// Applies the current armed state of one slot to one thread. Suspension is required: Get/SetThreadContext on a
// running thread is not reliable for the debug registers.
bool apply_slot_to_thread(HANDLE h, size_t i, bool enable, uintptr_t address, uint8_t size, uint8_t rw) {
    if (h == nullptr) {
        return false;
    }
    if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        return false;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(h, &ctx)) {
        write_slot_into_context(ctx, i, enable, address, size, rw);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(h, &ctx) != FALSE;
    }
    ResumeThread(h);
    return ok;
}

// Enumerates the process's threads, EXCLUDING the caller. Self-exclusion is not a nicety: the calling thread is
// the IPC thread, which reads these same addresses to build /sdk/* reports, so arming it would trap on our own
// reads and name this mod as the accessor.
struct ThreadIter {
    HANDLE snap{INVALID_HANDLE_VALUE};
    THREADENTRY32 te{};
    uint32_t pid{};
    uint32_t self{};

    bool begin() {
        pid = GetCurrentProcessId();
        self = GetCurrentThreadId();
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return false;
        }
        te.dwSize = sizeof(te);
        return Thread32First(snap, &te) != FALSE;
    }

    bool valid() const { return te.th32OwnerProcessID == pid && te.th32ThreadID != self; }
    bool next() { return Thread32Next(snap, &te) != FALSE; }

    ~ThreadIter() {
        if (snap != INVALID_HANDLE_VALUE) {
            CloseHandle(snap);
        }
    }
};

uint32_t apply_slot_everywhere(size_t i, bool enable, uintptr_t address, uint8_t size, uint8_t rw,
                               bool only_new_threads) {
    ThreadIter it;
    if (!it.begin()) {
        return 0;
    }
    uint32_t applied = 0;
    do {
        if (!it.valid()) {
            continue;
        }
        const uint32_t tid = it.te.th32ThreadID;
        if (only_new_threads && tid_known(tid)) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
        if (h == nullptr) {
            continue;
        }
        if (apply_slot_to_thread(h, i, enable, address, size, rw)) {
            ++applied;
            if (enable) {
                tid_remember(tid);
            }
        }
        CloseHandle(h);
    } while (it.next());
    return applied;
}

// ---- THE HANDLER'S UNSAFE READS, ISOLATED ------------------------------------------------------------
//
// Copying above ESP can cross the stack guard page, and the watched address can be unmapped between arming and
// the trap. Both are wrapped in SEH in their own functions: mixing __try with C++ objects that need unwinding is
// ill-formed, so these stay free of them.

__declspec(noinline) void copy_stack_guarded(const void* esp, uint32_t* out, size_t words) {
    __try {
        memcpy(out, esp, words * sizeof(uint32_t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, words * sizeof(uint32_t));
    }
}

__declspec(noinline) uint8_t copy_value_guarded(const void* addr, uint8_t* out, uint8_t size) {
    __try {
        memcpy(out, addr, size);
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- THE HANDLER -------------------------------------------------------------------------------------
//
// Runs on the game's threads, inside an exception. No allocation, no lock, no CRT beyond memcpy/memset. A
// handler that blocked here on a mutex held by a thread this trap suspended would deadlock the game.

LONG CALLBACK watch_veh(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != static_cast<DWORD>(EXCEPTION_SINGLE_STEP)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* ctx = info->ContextRecord;
    const uint32_t dr6 = static_cast<uint32_t>(ctx->Dr6);
    const uint32_t fired = dr6 & 0xFu;
    if (fired == 0) {
        // A single-step that is not a data breakpoint -- DR6.BS, or someone else's trap. Not ours to claim.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool claimed = false;
    bool need_resume_flag = false;

    for (size_t i = 0; i < kSlots; ++i) {
        if ((fired & (1u << i)) == 0) {
            continue;
        }
        if (!g_slots[i].armed.load(std::memory_order_acquire)) {
            continue;  // a slot fired that we did not arm; leave it for whoever did
        }
        claimed = true;

        const uint8_t access = g_slots[i].access.load(std::memory_order_relaxed);
        const bool is_fault = access == static_cast<uint8_t>(Watchpoints::Access::Execute);
        if (is_fault) {
            // WITHOUT THIS THE GAME HANGS. An execute breakpoint is a fault, so resuming re-executes the same
            // instruction and traps again forever. EFlags.RF suppresses it for exactly one instruction.
            need_resume_flag = true;
        }

        const uint64_t n = g_slots[i].hits.fetch_add(1, std::memory_order_relaxed) + 1;
        const uintptr_t eip = static_cast<uintptr_t>(ctx->Eip);

        // Aggregate by instruction pointer. Linear probe; `eip` is the claim token.
        bool stored = false;
        for (size_t k = 0; k < kTableSize; ++k) {
            uintptr_t cur = g_table[k].eip.load(std::memory_order_acquire);
            if (cur == 0) {
                uintptr_t expected = 0;
                if (!g_table[k].eip.compare_exchange_strong(expected, eip, std::memory_order_acq_rel)) {
                    cur = expected;  // lost the race; fall through to the equality test below
                } else {
                    Record& r = g_table[k];
                    r.thread_id = GetCurrentThreadId();
                    r.slot = static_cast<uint8_t>(i);
                    r.is_fault = is_fault;
                    r.regs[0] = static_cast<uint32_t>(ctx->Eax);
                    r.regs[1] = static_cast<uint32_t>(ctx->Ebx);
                    r.regs[2] = static_cast<uint32_t>(ctx->Ecx);
                    r.regs[3] = static_cast<uint32_t>(ctx->Edx);
                    r.regs[4] = static_cast<uint32_t>(ctx->Esi);
                    r.regs[5] = static_cast<uint32_t>(ctx->Edi);
                    r.regs[6] = static_cast<uint32_t>(ctx->Ebp);
                    r.regs[7] = static_cast<uint32_t>(ctx->Esp);
                    const uint8_t sz = g_slots[i].size.load(std::memory_order_relaxed);
                    const uintptr_t watched = g_slots[i].address.load(std::memory_order_relaxed);
                    r.value_size = copy_value_guarded(reinterpret_cast<const void*>(watched), r.value,
                                                      sz <= sizeof(r.value) ? sz : 0);
                    copy_stack_guarded(reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx->Esp)),
                                       r.stack, sizeof(r.stack) / sizeof(r.stack[0]));
                    r.count.store(1, std::memory_order_release);
                    stored = true;
                    break;
                }
            }
            if (cur == eip) {
                g_table[k].count.fetch_add(1, std::memory_order_relaxed);
                stored = true;
                break;
            }
        }
        if (!stored) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }

        // AUTO-DISARM IN THE HANDLER ITSELF. A hot address can trap hundreds of thousands of times a second, so
        // waiting for the next frame to disarm is not fast enough to keep the game usable. Clearing this
        // thread's DR7 stops it here immediately; the flag asks on_frame to clear the other threads too.
        const uint64_t cap = g_slots[i].max_hits.load(std::memory_order_relaxed);
        if (cap != 0 && n >= cap) {
            ctx->Dr7 = dr7_with_slot(static_cast<uint32_t>(ctx->Dr7), i, false, 0, 0);
            g_slots[i].auto_disarmed.store(true, std::memory_order_relaxed);
            g_pending_disarm.fetch_or(1u << i, std::memory_order_relaxed);
            g_need_disarm.store(true, std::memory_order_release);
        }
    }

    if (!claimed) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // DR6 is sticky: leaving it set makes the next trap's cause ambiguous.
    ctx->Dr6 = 0;
    if (need_resume_flag) {
        ctx->EFlags |= 0x10000u;  // RF
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

Watchpoints& Watchpoints::get() {
    static Watchpoints s_instance;
    return s_instance;
}

std::optional<std::string> Watchpoints::on_initialize() {
    if (g_veh != nullptr) {
        return std::nullopt;
    }
    // First in the chain, so we see the trap before any of the game's own handlers.
    g_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (g_veh == nullptr) {
        return std::string{"AddVectoredExceptionHandler failed"};
    }
    LOGX("[watch] vectored handler registered; 4 hardware slots available");
    return std::nullopt;
}

void Watchpoints::on_frame() {
    const uint32_t f = g_frame.fetch_add(1, std::memory_order_relaxed);

    if (g_need_disarm.load(std::memory_order_acquire)) {
        const uint32_t pending = g_pending_disarm.exchange(0, std::memory_order_relaxed);
        g_need_disarm.store(false, std::memory_order_release);
        for (size_t i = 0; i < kSlots; ++i) {
            if ((pending & (1u << i)) != 0) {
                disarm(static_cast<int>(i));
            }
        }
        return;
    }

    // NEW THREADS DO NOT INHERIT DEBUG REGISTERS. Re-application is throttled and touches only threads we have
    // not armed yet, because suspending every thread in the process on a timer is a visible hitch.
    if ((f % 600) != 0) {
        return;
    }
    for (size_t i = 0; i < kSlots; ++i) {
        if (!g_slots[i].armed.load(std::memory_order_acquire)) {
            continue;
        }
        const uint32_t added = apply_slot_everywhere(i, true, g_slots[i].address.load(std::memory_order_relaxed),
                                                    g_slots[i].size.load(std::memory_order_relaxed),
                                                    g_slots[i].access.load(std::memory_order_relaxed), true);
        if (added != 0) {
            g_slots[i].threads_applied.fetch_add(added, std::memory_order_relaxed);
        }
    }
}

void Watchpoints::on_shutdown() {
    // ORDER IS THE WHOLE POINT. Clear the hardware first so no thread can enter the handler, then unregister the
    // handler. A registered vectored handler in unmapped code turns the next exception anywhere in the process
    // into an immediate crash, and the framework's quiescence check runs after this.
    disarm_all();
    if (g_veh != nullptr) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
        LOGX("[watch] vectored handler removed");
    }
}

Watchpoints::ArmResult Watchpoints::arm(uintptr_t address, uint8_t size, Access access, uint64_t max_hits) {
    ArmResult res;
    if (g_veh == nullptr) {
        res.error = "vectored handler not registered";
        return res;
    }
    if (address == 0) {
        res.error = "address is 0";
        return res;
    }

    uint8_t rw = static_cast<uint8_t>(access);
    if (access == Access::Execute) {
        // Execute breakpoints are 1-byte by architecture, and the address must be an instruction start.
        size = 1;
    } else if (size != 1 && size != 2 && size != 4) {
        res.error = "size must be 1, 2 or 4 (x86 has no 8-byte watch in 32-bit mode)";
        return res;
    } else if ((address % size) != 0) {
        res.error = "address must be aligned to size -- an unaligned watch covers the wrong bytes";
        return res;
    }

    int free_slot = -1;
    for (size_t i = 0; i < kSlots; ++i) {
        if (!g_slots[i].armed.load(std::memory_order_acquire)) {
            free_slot = static_cast<int>(i);
            break;
        }
    }
    if (free_slot < 0) {
        res.error = "all 4 hardware slots are in use -- clear one first";
        return res;
    }

    const size_t i = static_cast<size_t>(free_slot);
    g_slots[i].address.store(address, std::memory_order_relaxed);
    g_slots[i].size.store(size, std::memory_order_relaxed);
    g_slots[i].access.store(rw, std::memory_order_relaxed);
    g_slots[i].hits.store(0, std::memory_order_relaxed);
    g_slots[i].max_hits.store(max_hits, std::memory_order_relaxed);
    g_slots[i].auto_disarmed.store(false, std::memory_order_relaxed);
    g_slots[i].armed.store(true, std::memory_order_release);

    const uint32_t applied = apply_slot_everywhere(i, true, address, size, rw, false);
    g_slots[i].threads_applied.store(applied, std::memory_order_relaxed);
    if (applied == 0) {
        g_slots[i].armed.store(false, std::memory_order_release);
        res.error = "could not write debug registers into any thread";
        return res;
    }

    res.ok = true;
    res.slot = free_slot;
    res.threads_applied = applied;
    res.effective_size = size;
    LOGX("[watch] slot %d armed at 0x%08" PRIXPTR " size %u rw %u across %u thread(s)", free_slot, address,
         static_cast<unsigned>(size), static_cast<unsigned>(rw), applied);
    return res;
}

void Watchpoints::disarm(int slot) {
    if (slot < 0 || static_cast<size_t>(slot) >= kSlots) {
        return;
    }
    const size_t i = static_cast<size_t>(slot);
    // Clear the hardware even if we believe the slot is idle -- a stale enable bit left in some thread's context
    // would keep trapping into a handler whose slot state says nothing is armed.
    apply_slot_everywhere(i, false, 0, 0, 0, false);
    if (g_slots[i].armed.exchange(false, std::memory_order_acq_rel)) {
        LOGX("[watch] slot %d disarmed after %llu hit(s)", slot,
             static_cast<unsigned long long>(g_slots[i].hits.load(std::memory_order_relaxed)));
    }
}

void Watchpoints::disarm_all() {
    for (size_t i = 0; i < kSlots; ++i) {
        disarm(static_cast<int>(i));
    }
    for (size_t k = 0; k < kTableSize; ++k) {
        g_table[k].eip.store(0, std::memory_order_relaxed);
        g_table[k].count.store(0, std::memory_order_relaxed);
    }
    g_dropped.store(0, std::memory_order_relaxed);
    g_known_count.store(0, std::memory_order_release);
}

std::array<Watchpoints::Slot, 4> Watchpoints::slots() const {
    std::array<Slot, 4> out{};
    for (size_t i = 0; i < kSlots; ++i) {
        out[i].armed = g_slots[i].armed.load(std::memory_order_acquire);
        out[i].address = g_slots[i].address.load(std::memory_order_relaxed);
        out[i].size = g_slots[i].size.load(std::memory_order_relaxed);
        out[i].access = static_cast<Access>(g_slots[i].access.load(std::memory_order_relaxed));
        out[i].hits = g_slots[i].hits.load(std::memory_order_relaxed);
        out[i].max_hits = g_slots[i].max_hits.load(std::memory_order_relaxed);
        out[i].auto_disarmed = g_slots[i].auto_disarmed.load(std::memory_order_relaxed);
        out[i].threads_applied = g_slots[i].threads_applied.load(std::memory_order_relaxed);
    }
    return out;
}

std::vector<Watchpoints::Hit> Watchpoints::hits() const {
    std::vector<Hit> out;
    for (size_t k = 0; k < kTableSize; ++k) {
        const uintptr_t eip = g_table[k].eip.load(std::memory_order_acquire);
        const uint64_t count = g_table[k].count.load(std::memory_order_acquire);
        if (eip == 0 || count == 0) {
            continue;  // count 0 means claimed but not yet filled; skip rather than report a half-record
        }
        Hit h;
        h.eip_after = eip;
        h.count = count;
        h.thread_id = g_table[k].thread_id;
        h.slot = g_table[k].slot;
        h.is_fault = g_table[k].is_fault;
        h.eax = g_table[k].regs[0];
        h.ebx = g_table[k].regs[1];
        h.ecx = g_table[k].regs[2];
        h.edx = g_table[k].regs[3];
        h.esi = g_table[k].regs[4];
        h.edi = g_table[k].regs[5];
        h.ebp = g_table[k].regs[6];
        h.esp = g_table[k].regs[7];
        h.value_size = g_table[k].value_size;
        memcpy(h.value.data(), g_table[k].value, sizeof(g_table[k].value));
        memcpy(h.stack.data(), g_table[k].stack, sizeof(g_table[k].stack));
        out.push_back(h);
    }
    return out;
}

Watchpoints::AddressInfo Watchpoints::classify(uintptr_t address) {
    AddressInfo out;
    if (address == 0) {
        return out;
    }
    auto& mods = sdk::Modules::get();
    const sdk::Modules::Module* candidates[] = {mods.exe(), mods.game_client(), mods.game_server(),
                                                mods.game_database(), mods.lt_memory()};
    for (const auto* m : candidates) {
        if (m == nullptr || m->base == 0 || m->size == 0) {
            continue;
        }
        if (address < m->base || address >= m->base + m->size) {
            continue;
        }
        out.known = true;
        out.module = m->name != nullptr ? m->name : "?";
        out.offset = address - m->base;

        // The PE's own preferred ImageBase, read from the mapped headers. That is exactly the base a database
        // uses when the file is opened without rebasing, which makes static_address paste-able.
        const auto e_lfanew = sdk::mem::read<uint32_t>(m->base + 0x3C);
        if (e_lfanew.has_value()) {
            const auto image_base = sdk::mem::read<uint32_t>(m->base + *e_lfanew + 0x34);
            if (image_base.has_value()) {
                out.static_address = static_cast<uintptr_t>(*image_base) + out.offset;
            }
        }
        return out;
    }
    return out;
}

bool Watchpoints::handler_registered() const {
    return g_veh != nullptr;
}

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Mod.hpp"

// HARDWARE DATA BREAKPOINTS, SO "WHAT WRITES THIS ADDRESS?" IS A MEASUREMENT AND NOT A GUESS.
//
// This exists because of a specific, repeated, expensive failure in this project: to find the writer of a field
// we scanned the binary for instructions that store to a matching STRUCT OFFSET. That answers "what writes +144
// on ANY object" -- 67 functions across unrelated classes -- when the question was "what writes THIS object's
// +144". Every such scan in this project's history produced a plausible wrong answer, and each one cost hours.
// The offset-collision false positive is documented at length in reversing/MAPPING_WORKFLOW.MD.
//
// A data breakpoint on the live address answers the actual question in one hit, with the accessing instruction,
// the register state, and the stack. It replaces an external debugger for this workflow.
//
// ---- WHY DEBUG REGISTERS AND NOT PAGE GUARDS ------------------------------------------------------------
//
// The alternative is PAGE_GUARD / PAGE_NOACCESS plus an exception handler, which traps on ANY access to the
// enclosing 4 KiB page. A game's hot structures share pages with dozens of other fields, so that approach
// reports mostly noise and costs a fault per access on unrelated data -- it can drop a frame rate by an order of
// magnitude. Debug registers trap on the exact byte range and cost nothing when not hit.
//
// The price is that there are exactly FOUR of them, per thread, in hardware. That is the whole budget.
//
// ---- THE SEMANTICS THAT MISLEAD PEOPLE, STATED PLAINLY -------------------------------------------------
//
// 1. A DATA breakpoint is a TRAP: it is reported AFTER the accessing instruction completes. So the recorded
//    instruction pointer is the address of the NEXT instruction, and the instruction that actually touched the
//    memory ends immediately before it. Reported as `eip_after` for exactly that reason -- calling it "the
//    accessing instruction" would be a lie, and disassembling at it in IDA shows the wrong line.
//    An EXECUTE breakpoint is a FAULT and reports the instruction itself; `is_fault` distinguishes them.
//
// 2. x86 HAS NO READ-ONLY DATA BREAKPOINT. The hardware encodes execute, write, and read-or-write. A request
//    for reads is served by read-or-write, and the response says so rather than pretending.
//
// 3. THE ADDRESS MUST BE ALIGNED TO THE LENGTH -- 2 for a 2-byte watch, 4 for a 4-byte watch. An unaligned
//    request is rejected instead of silently watching the wrong bytes.
//
// 4. DEBUG REGISTERS ARE PER-THREAD STATE. They must be written into every thread's context individually, and a
//    thread created afterwards does not inherit them -- hence the periodic re-application from on_frame.
//
// 5. OUR OWN THREADS ARE EXCLUDED. The IPC thread reads these very addresses to build /sdk/* reports, so arming
//    it would trap on our own SDK reads and report this mod as the culprit. Self-exclusion is why the arming
//    thread is skipped.
//
// ---- SAFETY, WHICH IS NOT OPTIONAL HERE ----------------------------------------------------------------
//
// The handler runs on the game's threads inside an exception. It allocates nothing, takes no lock, calls no CRT
// function that could, and writes only into a fixed-size table -- a handler that blocks on a mutex held by a
// suspended thread deadlocks the game, which this project has already done once by other means.
//
// A hot address can be hit hundreds of thousands of times per second. `max_hits` auto-disarms, and the disarm
// happens in the handler itself (clearing DR7 in the trapping thread's own context) so it takes effect
// immediately rather than one frame later.
//
// TEARDOWN IS A CORRECTNESS REQUIREMENT, NOT HYGIENE. A registered vectored handler whose code has been
// unmapped turns the next exception anywhere in the process into an instant crash. on_shutdown clears the debug
// registers on every thread FIRST and removes the handler SECOND, before the framework's existing quiescence
// check runs -- that ordering is what makes uninject survivable while a watchpoint is armed.
class Watchpoints final : public Mod {
public:
    static Watchpoints& get();

    std::string_view get_name() const override { return "Watchpoints"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_shutdown() override;

    enum class Access : uint8_t {
        Execute = 0,  // DR7 R/W = 00, length forced to 1, reported as a fault
        Write = 1,    // DR7 R/W = 01, data writes only
        ReadWrite = 3 // DR7 R/W = 11, the only form that catches reads
    };

    // One recorded distinct accessor. Aggregated by instruction pointer, so a hot writer is one row with a
    // count rather than a flood.
    struct Hit {
        uintptr_t eip_after{};  // see semantics note 1 -- NOT the accessing instruction for data watches
        uint64_t count{};
        uint32_t thread_id{};
        uint8_t slot{};
        bool is_fault{};        // true for execute watches, where eip_after IS the instruction

        // Registers at the trap, from the first hit at this instruction. ECX matters most in this engine:
        // __thiscall puts `this` there, which identifies the OBJECT the accessor was working on -- the exact
        // thing an offset scan cannot tell you.
        uint32_t eax{}, ebx{}, ecx{}, edx{}, esi{}, edi{}, ebp{}, esp{};

        // The watched bytes as they stood at the trap. For a write watch this is the value just stored, because
        // the trap fires after the instruction.
        std::array<uint8_t, 8> value{};
        uint8_t value_size{};

        // Raw stack words above ESP, copied in the handler and interpreted LATER on the reporting thread --
        // resolving module attribution requires OS queries that have no business inside an exception handler.
        std::array<uint32_t, 24> stack{};
    };

    struct Slot {
        bool armed{};
        uintptr_t address{};
        uint8_t size{};
        Access access{};
        uint64_t hits{};
        uint64_t max_hits{};
        bool auto_disarmed{};
        uint32_t threads_applied{};
    };

    struct ArmResult {
        bool ok{};
        int slot{-1};
        std::string error;
        std::string note;  // e.g. the read-only downgrade, so the caller is told rather than surprised
        uint32_t threads_applied{};
        uint8_t effective_size{};  // what the hardware was actually given; exec forces 1
    };

    // Arms one of the four hardware slots. `size` must be 1, 2 or 4 and the address aligned to it.
    ArmResult arm(uintptr_t address, uint8_t size, Access access, uint64_t max_hits);

    // Disarms one slot, or every slot. Always safe to call, including when nothing is armed.
    void disarm(int slot);
    void disarm_all();

    std::array<Slot, 4> slots() const;
    std::vector<Hit> hits() const;

    // A RUNTIME ADDRESS TURNED INTO SOMETHING YOU CAN LOOK AT IN A DISASSEMBLER, which is the only form in
    // which a hit is actually actionable. The static address is computed from the module's own PE ImageBase, so
    // it matches a database opened on the file without rebasing -- no hardcoded per-module constant, and it
    // stays correct across the ASLR reshuffle that invalidates every address between two sessions.
    struct AddressInfo {
        bool known{};             // false when no tracked module owns it (heap, stack, an untracked DLL)
        std::string module;
        uintptr_t offset{};       // from the module's runtime base
        uintptr_t static_address{};
    };
    static AddressInfo classify(uintptr_t address);

    // Whether the vectored handler is currently registered. False means arming will refuse.
    bool handler_registered() const;

private:
    Watchpoints() = default;
};

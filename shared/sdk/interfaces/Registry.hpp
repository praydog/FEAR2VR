#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sdk::interfaces {

// Runtime discovery of LithTech's interface holders in FEAR2.exe.
//
// WHY THIS EXISTS -- and why it is not a table of addresses.
//
// FEAR2's engine publishes its subsystems as named interfaces. Each consumer
// declares a "holder" (the SDK's define_holder macro), which is a small static
// object registering a request with CInterfaceDatabase; the database later
// writes the resolved interface pointer into the consumer's own pointer
// variable. See reversing/INTERFACE_HOLDERS.md for the full reversing trail.
//
// A holder is 12 bytes:
//     +0x00  vftable   (&CAPIHolder_vftable)
//     +0x04  api_name  ("ILTClient.Default")
//     +0x08  output slot -- where the resolved interface pointer is written
//
// HOW WE FIND THEM: one pattern scan locates CAPIHolder_ctor, then kananlib's
// scan_relative_references finds every call to it, and each call site's own
// operands carry the name and the output slot:
//
//     push  <output slot>      68 imm32
//     push  <api_name>         68 imm32
//     mov   ecx, <holder>      B9 imm32
//     call  CAPIHolder_ctor    E8 rel32
//
// That shape was verified across all 147 call sites during the reversing pass.
// Deliberately preferred over hardcoding those 147 addresses: they are RVAs a
// patch would silently invalidate, whereas this rediscovers them and reports
// how many it found.
//
// SCOPE: FEAR2.exe only. Other modules (gameclient.dll, gameserver.dll) link
// their own copy of the template and have their own holders; this registry does
// not see them.
//
// LIFETIME -- THE IMPORTANT PART.
//
// Two very different kinds of address are involved, and only one is stable:
//
//   holder objects + output slot ADDRESSES are static globals inside the
//   image. They never move. We cache those.
//
//   the resolved interface POINTER stored in a slot is NOT stable. The
//   database calls APIFound() to fill it and APIRemoved() to clear it (both
//   are real, mapped vtable slots), so it is null before module resolution
//   and can go null again afterwards.
//
// Therefore resolve() RE-READS the slot on every call and never caches the
// result. A cached interface pointer would dangle exactly when a module is
// unloaded, which is the one moment it matters.
class Registry {
public:
    struct Holder {
        std::string name;     // "ILTClient.Default" -- includes the instance suffix
        uintptr_t holder_obj; // the CAPIHolder object itself (static)
        uintptr_t slot;       // address of the output pointer variable (static)
        uintptr_t call_site;  // the static-init call that constructed it (static)
    };

    static Registry& get();

    // Locate CAPIHolder_ctor and build the holder table from its call sites.
    //
    // RETRIES ON FAILURE. Returns false and stays uninitialized when the
    // pattern missed, module geometry is not ready yet, or the scan produced
    // nothing -- so an early call (before Modules::initialize(), or before the
    // image is in a scannable state) does not permanently poison the registry.
    // Only a scan that actually found holders latches.
    // THREAD SAFE: the getters are reachable from both the IPC thread
    // (diagnostics) and the game thread (mods), so initialization is
    // double-checked under a mutex. The holder table is written exactly once,
    // published with release semantics, and read-only thereafter.
    bool initialize();
    bool is_initialized() const { return m_initialized.load(std::memory_order_acquire); }

    // Runtime address of CAPIHolder_ctor, 0 until a successful initialize().
    uintptr_t ctor_addr() const { return m_ctor; }

    // Call sites seen vs. holders successfully decoded from them. A gap means
    // some call site did not match the expected operand shape; it is reported
    // rather than hidden.
    size_t call_sites_seen() const { return m_call_sites_seen; }

    // Every discovered holder.
    const std::vector<Holder>& holders() const { return m_holders; }

    // Holders requesting `name` (exact, e.g. "ILTClient.Default"). There are
    // usually SEVERAL: define_holder is emitted per translation unit, so one
    // interface legitimately has many holders, each with its own slot. They
    // should all end up with the same pointer once resolved.
    std::vector<const Holder*> find(const char* name) const;

    // Current interface pointer for `name`, or nullptr.
    //
    // Re-reads the slots every call (see the lifetime note above). Prefers a
    // non-null slot: an unresolved duplicate must not mask a resolved one.
    void* resolve(const char* name) const;

    // How many of `name`'s slots currently hold a non-null pointer, and
    // whether they all agree. Diagnostics/tests use this to distinguish
    // "not resolved yet" from "resolved inconsistently".
    struct Agreement {
        size_t total{};      // holders requesting this name
        size_t non_null{};   // how many currently hold a pointer
        bool all_agree{};    // every non-null slot holds the same value
        void* value{};       // that value, when all_agree
    };
    Agreement agreement(const char* name) const;

    // Distinct interface names, sorted. Diagnostics/tests.
    std::vector<std::string> names() const;

private:
    Registry() = default;

    // Written once under m_mutex, then read-only. Readers must have called
    // initialize() first (the shared helpers in Interface.hpp always do),
    // which is what orders the publication against their reads.
    mutable std::mutex m_mutex;
    std::atomic<bool> m_initialized{false};
    uintptr_t m_ctor{0};
    size_t m_call_sites_seen{0};
    std::vector<Holder> m_holders;
};

} // namespace sdk::interfaces

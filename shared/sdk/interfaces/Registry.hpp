#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
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
    // FAILURE HANDLING is deliberately two-tiered, because "retry forever" and
    // "latch forever" are both wrong:
    //
    //   RETRYABLE -- a prerequisite that can still become available: module
    //     geometry not resolved yet (Modules::initialize() has not run). No
    //     latch, so an early caller cannot poison the registry.
    //
    //   DEFINITIVE -- the owning module IS present and the evidence is absent:
    //     the CAPIHolder_ctor pattern missed, or it matched but no call site
    //     decoded. FEAR2.exe is necessarily loaded in-process by then, so
    //     rescanning cannot change the answer. Latched as failed and logged
    //     once -- otherwise every interface getter would re-scan the whole
    //     image, from the game thread, forever.
    //
    // THREAD SAFE: the getters are reachable from both the IPC thread
    // (diagnostics) and the game thread (mods), so initialization is
    // double-checked under a mutex. The holder table is written exactly once,
    // published with release semantics, and read-only thereafter.
    bool initialize();
    bool is_initialized() const { return m_initialized.load(std::memory_order_acquire); }

    // True once a DEFINITIVE failure latched (see above). Diagnostics use this
    // to report "the signature broke" rather than "not resolved yet".
    bool failed() const { return m_failed.load(std::memory_order_acquire); }

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

    // ---- WHERE THE GAME DLL KEEPS ITS OWN COPIES -------------------------------------------
    //
    // Everything above describes the ENGINE's holders. gameclient.dll keeps its own resolved pointers in .data,
    // and a consumer hooking game code generally wants the pointer THE GAME uses rather than a freshly resolved
    // one -- they should be the same object, and when they are not, one of them is stale.
    //
    // HOW THESE ARE FOUND, and why the exclusion matters more than the scan: any .data dword pointing at an
    // object whose vtable is in the exe's catalogue is a candidate. That test alone returns roughly 470 hits on
    // this build, because the game caches console variables as {LTConVar*, ILTClient*} pairs and every OWNER word
    // is such a pointer. Those are excluded by looking one dword back: if it holds a readable LTConVar name, the
    // hit is a cache pair and not an interface slot. What survives is about twenty genuine singletons.
    //
    // THE ACCOUNTING IS THE CHECK, and it is not circular. Slots are discovered by VTABLE, then matched to an
    // interface by POINTER against what the registry currently resolves. A slot holding a catalogued object that
    // the registry does not resolve to is a copy the engine has moved on from -- detectable precisely because
    // discovery never consulted the registry.
    struct GameclientSlot {
        uintptr_t offset{};          // gameclient-relative address of the pointer variable
        uintptr_t value{};           // the object it holds
        std::string class_name;      // implementation class, from the vtable catalogue
        std::string interface_name;  // registry name whose CURRENT pointer equals value; empty if unaccounted
    };

    // gameclient's interface pointer globals. Empty when gameclient is not mapped.
    std::vector<GameclientSlot> gameclient_interface_slots(size_t limit = 256) const;

    // The first slot accounted for by this interface name, e.g. "ILTPhysics.Default".
    std::optional<GameclientSlot> find_gameclient_slot(std::string_view interface_name) const;

private:
    Registry() = default;

    // Written once under m_mutex, then read-only. Readers must have called
    // initialize() first (the shared helpers in Interface.hpp always do),
    // which is what orders the publication against their reads.
    mutable std::mutex m_mutex;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_failed{false};
    uintptr_t m_ctor{0};
    size_t m_call_sites_seen{0};
    std::vector<Holder> m_holders;
};

} // namespace sdk::interfaces

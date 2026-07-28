#include "Registry.hpp"

#include <algorithm>
#include <cinttypes>

#include <windows.h>

#include <utility/Scan.hpp>
#include <utility/Seh.hpp>

#include "Log.hpp"
#include "../Modules.hpp"

namespace sdk::interfaces {

// CAPIHolder<I>::CAPIHolder -- FEAR2_dump.exe 0x4050DB (__thiscall, `retn 8`).
// One ICF-folded body serves all ~147 instantiations, so this is a single
// unique site. Bytes verified against the image:
//   56 | FF 74 24 08 | 8B F1 | E8 [rel32]      push esi; push name; esi=this; base ctor
//   8B 44 24 0C | C7 06 [&vftable]             eax=output_slot; *this = &CAPIHolder_vftable
//   89 46 08 | 83 20 00                        this[2] = slot; *slot = 0
static constexpr const char* kHolderCtor =
    "56 FF 74 24 08 8B F1 E8 ? ? ? ? 8B 44 24 0C C7 06 ? ? ? ? 89 46 08 83 20 00";

// Fixed static-init call-site shape preceding the `call` (verified on all 147
// sites during the reversing pass):
//   -15  68 imm32   push <output slot>
//   -10  68 imm32   push <api_name>
//    -5  B9 imm32   mov ecx, <holder object>
//     0  E8 rel32   call CAPIHolder_ctor
static constexpr const char* kCallSitePrologue = "68 ? ? ? ? 68 ? ? ? ? B9 ? ? ? ?";
static constexpr uint32_t kPrologueSize = 15;

namespace {

// ---- POD-only SEH helpers (MSVC C2712: no non-POD locals may share __try) --

struct CallSiteOperands {
    uint32_t output_slot;
    uint32_t api_name;
    uint32_t holder_obj;
};

// Decode the three imm32 operands out of a located prologue. Only reads the
// operand positions of an already-matched byte shape.
bool seh_decode_prologue(uintptr_t prologue, CallSiteOperands* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* p = reinterpret_cast<const uint8_t*>(prologue);
        if (p[0] == 0x68 && p[5] == 0x68 && p[10] == 0xB9) {
            out->output_slot = *reinterpret_cast<const uint32_t*>(prologue + 1);
            out->api_name = *reinterpret_cast<const uint32_t*>(prologue + 6);
            out->holder_obj = *reinterpret_cast<const uint32_t*>(prologue + 11);
            ok = true;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// A holder's api_name always looks like "IFoo.Bar": leading 'I', exactly one
// '.', printable ASCII. Validating the shape is what keeps a mis-decoded call
// site from entering the table as a plausible-looking lie.
//
// Copies into a caller-provided POD buffer; returns length, or -1 on
// fault/shape mismatch.
int32_t seh_copy_api_name(uintptr_t addr, char* buf, int32_t cap) {
    int32_t result = -1;
    KANANLIB_SEH_TRY {
        const char* s = reinterpret_cast<const char*>(addr);
        int32_t n = 0, dots = 0;
        while (n < cap - 1) {
            const char c = s[n];
            if (c == '\0') {
                break;
            }
            if (c < 0x20 || c > 0x7E) {
                n = -1;
                break;
            }
            if (c == '.') {
                ++dots;
            }
            buf[n] = c;
            ++n;
        }
        if (n >= 4 && dots == 1 && buf[0] == 'I' && s[n] == '\0') {
            buf[n] = '\0';
            result = n;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

bool seh_read_u32(uintptr_t addr, uint32_t* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        *out = *reinterpret_cast<const uint32_t*>(addr);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

} // namespace

Registry& Registry::get() {
    static Registry s_instance;
    return s_instance;
}

bool Registry::initialize() {
    // Fast path: latched, table is published and read-only.
    if (m_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    // Slow path: exactly one thread scans. Re-check under the lock so a racing
    // pair of first-callers cannot both mutate the table.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized.load(std::memory_order_relaxed)) {
        return true;
    }

    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->handle == nullptr || exe->base == 0 || exe->size == 0) {
        // Modules::initialize() has not run yet. Do NOT latch -- a caller this
        // early must be able to succeed on a later attempt.
        return false;
    }

    const uintptr_t ctor = Modules::get().scan_exe(kHolderCtor, "CAPIHolder_ctor");
    if (ctor == 0) {
        return false; // scan_exe already logged the miss
    }

    // kananlib finds every call to it. The filter keeps only real `E8 rel32`
    // call sites: scan_relative_references returns the displacement address, so
    // the opcode sits one byte earlier.
    auto refs = utility::scan_relative_references(
        exe->handle, ctor, [](uintptr_t disp) {
            uint32_t opcode = 0;
            return seh_read_u32(disp - 1, &opcode) && (opcode & 0xFF) == 0xE8;
        });

    // MUST dedupe: the plural scanner splits the module across threads and
    // overlaps adjacent segments by 4 bytes, so a displacement landing in an
    // overlap is reported twice. Without this the holder count varies with the
    // machine's core count -- a genuinely flaky result.
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

    const uintptr_t lo = exe->base;
    const uintptr_t hi = exe->base + exe->size;

    std::vector<Holder> found;
    char namebuf[128];
    size_t rejected = 0;

    for (const uintptr_t disp : refs) {
        const uintptr_t insn = disp - 1;

        // The prologue is immediately adjacent on every site we verified; fall
        // back to a bounded reverse scan rather than silently dropping a site
        // whose codegen differs.
        uintptr_t prologue = insn - kPrologueSize;
        CallSiteOperands ops{};
        if (!seh_decode_prologue(prologue, &ops)) {
            const auto alt = utility::scan_reverse(insn, 48, kCallSitePrologue);
            if (!alt.has_value() || !seh_decode_prologue(*alt, &ops)) {
                ++rejected;
                continue;
            }
            prologue = *alt;
        }

        // Both operands are statics inside this image.
        if (ops.api_name < lo || ops.api_name >= hi || ops.output_slot < lo ||
            ops.output_slot >= hi || (ops.output_slot & 3) != 0) {
            ++rejected;
            continue;
        }
        if (seh_copy_api_name(ops.api_name, namebuf, static_cast<int32_t>(sizeof(namebuf))) < 0) {
            ++rejected;
            continue;
        }

        found.push_back(Holder{namebuf, ops.holder_obj, ops.output_slot, insn});
    }

    if (found.empty()) {
        LOGX("[sdk] interfaces: CAPIHolder_ctor at 0x%08" PRIXPTR " but %zu call sites yielded no "
             "holders (%zu rejected) -- not latching, will retry",
             ctor, refs.size(), rejected);
        return false;
    }

    m_ctor = ctor;
    m_call_sites_seen = refs.size();
    m_holders = std::move(found);
    // Publish LAST, with release: a reader that observes true via acquire is
    // guaranteed to see the fully built table.
    m_initialized.store(true, std::memory_order_release);

    LOGX("[sdk] interfaces: %zu holders from %zu call sites (%zu rejected), %zu distinct names",
         m_holders.size(), m_call_sites_seen, rejected, names().size());
    return true;
}

std::vector<const Registry::Holder*> Registry::find(const char* name) const {
    std::vector<const Holder*> out;
    if (name == nullptr) {
        return out;
    }
    for (const auto& h : m_holders) {
        if (h.name == name) {
            out.push_back(&h);
        }
    }
    return out;
}

void* Registry::resolve(const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    // Re-read every matching slot, every call. Prefer the first non-null: an
    // unresolved duplicate holder must not mask a resolved one.
    for (const auto& h : m_holders) {
        if (h.name != name) {
            continue;
        }
        uint32_t v = 0;
        if (seh_read_u32(h.slot, &v) && v != 0) {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(v));
        }
    }
    return nullptr;
}

Registry::Agreement Registry::agreement(const char* name) const {
    Agreement a{};
    if (name == nullptr) {
        return a;
    }
    bool first = true;
    a.all_agree = true;
    for (const auto& h : m_holders) {
        if (h.name != name) {
            continue;
        }
        ++a.total;
        uint32_t v = 0;
        if (!seh_read_u32(h.slot, &v) || v == 0) {
            continue;
        }
        ++a.non_null;
        auto* p = reinterpret_cast<void*>(static_cast<uintptr_t>(v));
        if (first) {
            a.value = p;
            first = false;
        } else if (a.value != p) {
            a.all_agree = false;
        }
    }
    if (a.non_null == 0) {
        a.value = nullptr;
    }
    return a;
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> out;
    out.reserve(m_holders.size());
    for (const auto& h : m_holders) {
        out.push_back(h.name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace sdk::interfaces

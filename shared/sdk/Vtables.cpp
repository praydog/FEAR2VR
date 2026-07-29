#include "Vtables.hpp"

#include <cstring>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// GENERATED from reversing/vtable_catalogue.txt, which an IDA sweep produces. Transcribing 54 rows by
// hand is exactly the sort of step that introduces an off-by-one into data whose whole purpose is to
// prevent one, so the table is emitted from the sweep's own output.
constexpr Vtables::Entry kCatalogue[] = {
    {"Agent", 0x27F078, 14, 1, true},
    {"CCompress", 0x26FC38, 11, 1, true},
    {"CD3DDrawPrim", 0x290110, 24, 1, true},
    {"CLTClient", 0x26F258, 147, 1, true},
    {"CLTCommonClient", 0x26E600, 19, 1, true},
    {"CLTCommonServer", 0x274BD8, 19, 1, true},
    {"CLTCursor", 0x277E3C, 10, 1, true},
    {"CLTFileMgr", 0x272130, 18, 1, true},
    {"CLTGameUtil", 0x2D3270, 24, 1, true},
    {"CLTInput", 0x277FA0, 28, 1, true},
    {"CLTLoadingProgress", 0x272184, 4, 1, true},
    {"CLTModelClient", 0x26E7E8, 83, 1, true},
    {"CLTModelServer", 0x274CD8, 81, 1, true},
    {"CLTPhysicsClient", 0x26EA70, 18, 1, false},
    {"CLTPhysicsServer", 0x274E90, 17, 1, true},
    {"CLTPhysicsSimClient", 0x2723B0, 98, 1, true},
    {"CLTPhysicsSimServer", 0x272550, 98, 1, true},
    {"CLTRenderer", 0x28FD20, 92, 1, true},
    {"CLTResourceMgr", 0x272708, 35, 1, true},
    {"CLTServer", 0x275670, 138, 1, true},
    {"CLTSoundMgrServer", 0x274F60, 15, 1, true},
    {"CLTTextureMgr", 0x2900D4, 11, 1, true},
    {"CLTTextureString", 0x272808, 22, 1, true},
    {"CLTTimer", 0x272878, 22, 1, true},
    {"CLTTimerClient", 0x2728E0, 22, 1, true},
    {"CLTTimerServer", 0x272948, 22, 1, true},
    {"CLTUI", 0x27820C, 11, 1, true},
    {"CLTVideoTexture", 0x290090, 13, 1, true},
    {"CServerConsoleState", 0x274A38, 3, 1, true},
    {"CSoundMgr", 0x276520, 52, 1, true},
    {"CWin32CustomRender", 0x28FEA0, 29, 1, true},
    {"CWorldClientBSP", 0x2775F0, 19, 1, true},
    {"CWorldParticleBlockerData", 0x27764C, 6, 1, true},
    {"CWorldServerBSP", 0x277680, 13, 1, true},
    {"GameSpyPatch", 0x2D3458, 5, 1, true},
    {"ICommandLineArgsCommonImp", 0x271D64, 7, 1, true},
    {"ILTClientContentTransfer", 0x2720C8, 9, 1, true},
    {"ILTServerContentTransfer", 0x2727A4, 9, 1, true},
    {"LtGskAgent", 0x27FE10, 14, 6, true},
    {"StNarrow", 0x27EF20, 14, 6, true},
    {"StSepNormal", 0x27FDB8, 14, 3, true},
    {"StcheckBvShape", 0x27FA54, 14, 3, true},
    {"TtCapsCaps", 0x27F630, 14, 4, true},
    {"TtHeightField", 0x27F144, 14, 6, true},
    {"TtPostCollideCB", 0x27D8CC, 15, 5, true},
    {"TtSphereSphere", 0x27F808, 14, 4, true},
    {"TtTransform", 0x27F908, 14, 6, true},
    {"TtrcBox", 0x27E924, 16, 8, true},
    {"TtrcBvShape", 0x27E4B4, 9, 4, true},
    {"TtrcCapsule", 0x27E4FC, 16, 8, true},
    {"TtrcConvTransl", 0x27EE0C, 16, 8, true},
    {"TtrcCylinder", 0x2D6D9C, 16, 8, true},
    {"TtrcMopp", 0x27E784, 12, 4, true},
    {"TtrcSphere", 0x27E98C, 16, 8, true},
    {"TtrcTransform", 0x27E9F8, 9, 4, true},
    {"available", 0x28BAB8, 20, 4, true},
    {"captureFocus", 0x28D098, 20, 4, true},
};

uintptr_t exe_base() {
    const auto* exe = Modules::get().exe();
    return exe == nullptr ? 0 : exe->base;
}

bool seh_copy(void* out, uintptr_t at, size_t bytes) {
    if (at == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

}  // namespace

const Vtables::Entry* Vtables::all(size_t& count) {
    count = sizeof(kCatalogue) / sizeof(kCatalogue[0]);
    return kCatalogue;
}

const Vtables::Entry* Vtables::find(std::string_view name) {
    for (const auto& e : kCatalogue) {
        if (name == e.name) {
            return &e;
        }
    }
    return nullptr;
}

uintptr_t Vtables::address(std::string_view name) {
    const auto* e = find(name);
    const uintptr_t base = exe_base();
    if (e == nullptr || base == 0) {
        return 0;
    }
    return base + e->offset;
}

std::optional<uintptr_t> Vtables::resolve(std::string_view name, size_t slot) {
    const auto* e = find(name);
    if (e == nullptr) {
        return std::nullopt;
    }
    // THE BOUNDS CHECK IS THE WHOLE POINT: past the last slot lies the class-name string, whose first
    // four bytes read as a perfectly plausible address.
    if (slot >= e->slot_count) {
        return std::nullopt;
    }
    const uintptr_t at = address(name);
    if (at == 0) {
        return std::nullopt;
    }
    uint32_t fn = 0;
    if (!seh_copy(&fn, at + slot * sizeof(uint32_t), sizeof(fn))) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(fn);
}

uintptr_t Vtables::purecall_address() {
    // _purecall lives at exe+0x252900 in this build. Resolved through the module base rather than hardcoded
    // absolute, like every other address in this SDK.
    const uintptr_t base = exe_base();
    return base == 0 ? 0 : base + 0x252900;
}

std::optional<bool> Vtables::is_pure_virtual(uintptr_t vtable, size_t slot) {
    const uintptr_t pure = purecall_address();
    if (vtable == 0 || pure == 0) {
        return std::nullopt;
    }
    uint32_t fn = 0;
    if (!seh_copy(&fn, vtable + slot * sizeof(uint32_t), sizeof(fn))) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(fn) == pure;
}

const Vtables::Entry* Vtables::find_by_vtable(uintptr_t vtable) {
    const uintptr_t base = exe_base();
    if (base == 0 || vtable == 0 || vtable < base) {
        return nullptr;
    }
    const uintptr_t offset = vtable - base;
    for (const auto& e : kCatalogue) {
        if (e.offset == offset) {
            return &e;
        }
    }
    return nullptr;
}

std::optional<uintptr_t> Vtables::vtable_of(uintptr_t object) {
    if (object == 0) {
        return std::nullopt;
    }
    uint32_t vtable = 0;
    if (!seh_copy(&vtable, object, sizeof(vtable)) || vtable == 0) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(vtable);
}

std::optional<std::string> Vtables::class_name_of(uintptr_t object) {
    const auto vt = vtable_of(object);
    if (!vt.has_value()) {
        return std::nullopt;
    }
    const uintptr_t vtable = *vt;
    const auto* e = find_by_vtable(vtable);
    if (e == nullptr) {
        return std::nullopt;
    }
    return std::string{e->name};
}

std::optional<std::string> Vtables::name_from_getter(uintptr_t vtable, size_t slot) {
    if (vtable == 0) {
        return std::nullopt;
    }
    uint32_t fn = 0;
    if (!seh_copy(&fn, vtable + slot * sizeof(uint32_t), sizeof(fn)) || fn == 0) {
        return std::nullopt;
    }
    // `mov eax, imm32` (B8) then `ret` (C3) -- the whole body of an InterfaceImplementation stub.
    uint8_t code[6]{};
    if (!seh_copy(code, fn, sizeof(code))) {
        return std::nullopt;
    }
    if (code[0] != 0xB8 || code[5] != 0xC3) {
        return std::nullopt;
    }
    uint32_t str_at = 0;
    std::memcpy(&str_at, &code[1], sizeof(str_at));
    if (str_at == 0) {
        return std::nullopt;
    }
    char text[64]{};
    if (!seh_copy(text, str_at, sizeof(text) - 1)) {
        return std::nullopt;
    }
    text[sizeof(text) - 1] = '\0';
    if (text[0] == '\0') {
        return std::nullopt;
    }
    return std::string{text};
}

Vtables::NameCheck Vtables::check_name_getter(const Entry& entry) {
    const uintptr_t at = address(entry.name);
    if (at == 0) {
        return NameCheck::Unreadable;
    }
    const auto got = name_from_getter(at, entry.name_slot);
    if (!got.has_value()) {
        return NameCheck::NotAGetter;
    }
    return *got == entry.name ? NameCheck::Confirmed : NameCheck::Mismatch;
}

std::optional<Vtables::Verification> Vtables::verify(const Entry& entry) {
    const uintptr_t base = exe_base();
    const auto* exe = Modules::get().exe();
    if (base == 0 || exe == nullptr || exe->size == 0) {
        return std::nullopt;
    }
    const uintptr_t at = base + entry.offset;
    Verification v{};
    v.slots_in_image = true;
    for (size_t i = 0; i < entry.slot_count; ++i) {
        uint32_t fn = 0;
        if (!seh_copy(&fn, at + i * sizeof(uint32_t), sizeof(fn))) {
            return std::nullopt;
        }
        ++v.slots_checked;
        if (fn < base || fn >= base + exe->size) {
            v.slots_in_image = false;  // an over-long extent lands on the name string and fails here
        }
    }
    const uintptr_t after = at + static_cast<size_t>(entry.slot_count) * sizeof(uint32_t);
    if (entry.name_follows) {
        // The string immediately after must be the catalogued name -- which is what fails when an extent
        // is one slot short, since the read then lands on a function pointer instead of text.
        char text[64]{};
        const size_t want = std::strlen(entry.name);
        if (want + 1 > sizeof(text)) {
            return std::nullopt;
        }
        if (!seh_copy(text, after, want + 1)) {
            return std::nullopt;
        }
        v.name_matches = text[want] == '\0' && std::strncmp(text, entry.name, want) == 0;
    } else {
        // No name string to check, so the bound is that the table cannot continue: the dword after it is
        // not an exe-range address. One slot too long would have consumed that dword and failed
        // slots_in_image instead, so the two checks still close both directions.
        uint32_t next = 0;
        if (!seh_copy(&next, after, sizeof(next))) {
            return std::nullopt;
        }
        v.name_matches = next < base || next >= base + exe->size;
    }
    return v;
}

}  // namespace sdk

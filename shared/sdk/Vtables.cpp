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
    {"Agent", 0x27F078, 14, 1},
    {"CCompress", 0x26FC38, 11, 1},
    {"CD3DDrawPrim", 0x290114, 23, 0},
    {"CLTClient", 0x26F258, 147, 1},
    {"CLTCommonClient", 0x26E600, 19, 1},
    {"CLTCommonServer", 0x274BD8, 19, 1},
    {"CLTCursor", 0x277E38, 11, 2},
    {"CLTFileMgr", 0x272130, 18, 1},
    {"CLTGameUtil", 0x2D3270, 24, 1},
    {"CLTInput", 0x277FA0, 28, 1},
    {"CLTLoadingProgress", 0x272184, 4, 1},
    {"CLTModelClient", 0x26E7E8, 83, 1},
    {"CLTModelServer", 0x274CD8, 81, 1},
    {"CLTPhysicsServer", 0x274E90, 17, 1},
    {"CLTPhysicsSimClient", 0x2723B4, 97, 0},
    {"CLTPhysicsSimServer", 0x272554, 97, 0},
    {"CLTRenderer", 0x28FD20, 92, 1},
    {"CLTResourceMgr", 0x272708, 35, 1},
    {"CLTServer", 0x275670, 138, 1},
    {"CLTSoundMgrServer", 0x274F60, 15, 1},
    {"CLTTextureMgr", 0x2900D4, 11, 1},
    {"CLTTextureString", 0x272808, 22, 1},
    {"CLTTimer", 0x272878, 22, 1},
    {"CLTTimerClient", 0x2728E0, 22, 1},
    {"CLTTimerServer", 0x272948, 22, 1},
    {"CLTUI", 0x2781F8, 16, 6},
    {"CLTVideoTexture", 0x290090, 13, 1},
    {"CSoundMgr", 0x276520, 52, 1},
    {"CWin32CustomRender", 0x28FEA0, 29, 1},
    {"CWorldClientBSP", 0x2775F0, 19, 1},
    {"CWorldParticleBlockerData", 0x27764C, 6, 1},
    {"CWorldServerBSP", 0x277684, 12, 0},
    {"GameSpyPatch", 0x2D3408, 25, 21},
    {"ICommandLineArgsCommonImp", 0x271D68, 6, 0},
    {"ILTClientContentTransfer", 0x272044, 42, 34},
    {"ILTServerContentTransfer", 0x2727A4, 9, 1},
    {"LtGskAgent", 0x27FE10, 14, 6},
    {"StNarrow", 0x27EF20, 14, 6},
    {"StSepNormal", 0x27FDB8, 14, 3},
    {"StcheckBvShape", 0x27FA54, 14, 3},
    {"TtCapsCaps", 0x27F630, 14, 4},
    {"TtHeightField", 0x27F144, 14, 6},
    {"TtSphereSphere", 0x27F808, 14, 4},
    {"TtTransform", 0x27F908, 14, 6},
    {"TtrcBox", 0x27E924, 16, 8},
    {"TtrcCapsule", 0x27E4FC, 16, 8},
    {"TtrcConvTransl", 0x27EE0C, 16, 8},
    {"TtrcCylinder", 0x2D6D9C, 16, 8},
    {"TtrcMopp", 0x27E784, 12, 4},
    {"TtrcSphere", 0x27E98C, 16, 8},
    {"TtrcTransform", 0x27E9F8, 9, 4},
    {"available", 0x28BABC, 19, 3},
    {"captureFocus", 0x28D09C, 19, 3},
    {"gfxVersion", 0x286A38, 26, 3},
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
    // And the string immediately after the table must be the catalogued name -- which is what fails when
    // an extent is one slot short, since the read then lands on a function pointer instead of text.
    char text[64]{};
    const size_t want = std::strlen(entry.name);
    if (want + 1 > sizeof(text)) {
        return std::nullopt;
    }
    if (!seh_copy(text, at + static_cast<size_t>(entry.slot_count) * sizeof(uint32_t), want + 1)) {
        return std::nullopt;
    }
    v.name_matches = text[want] == '\0' && std::strncmp(text, entry.name, want) == 0;
    return v;
}

}  // namespace sdk

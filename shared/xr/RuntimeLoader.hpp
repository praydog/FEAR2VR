#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xr {

// ---- THE PART OF OPENXR THAT MUST NEVER CHANGE -------------------------------------------------
//
// Finding a runtime, loading it, and negotiating with it. Nothing else -- no instances, no
// sessions, no policy. This is deliberately the whole content of the resident proxy
// (fear2xr.dll), and the split exists for one concrete reason:
//
//   THE PROXY IS PINNED, so replacing it costs a game restart. When it shared an implementation
//   file with the mod's consumer API, every edit to that API forced a proxy rebuild and therefore a
//   restart -- which happened twice before the rule was taken seriously. The part that cannot be
//   iterated must be the part that never needs to be, and that means it must not share a
//   translation unit with the part that does.
//
// So: this file is expected to be finished. Everything a consumer actually iterates on lives in
// sdk::OpenXR, which reaches the runtime through the pointer this hands back.
class RuntimeLoader {
public:
    // OpenXR handles are uint64_t even in a 32-bit process -- XR_DEFINE_HANDLE only makes them
    // pointers when the pointer size IS 8.
    using PFN_GetInstanceProcAddr = int32_t(__stdcall*)(uint64_t, const char*, void**);

    // Every runtime registered for THIS bitness. Reading the registry is free; LOADING is not, so
    // these are separate calls and nothing here loads anything.
    static std::vector<std::string> available_runtimes();

    // The active runtime's manifest, and the library it names. A 32-bit process reads
    // HKLM\Software\Khronos through the WOW6432Node view automatically, so this resolves the
    // 32-bit runtime without asking -- which is the only one that can serve this process.
    bool discover();
    bool select_manifest(const std::string& manifest);
    bool discovered() const { return m_discovered; }
    const std::string& manifest_path() const { return m_manifest; }
    const std::string& library_path() const { return m_library; }

    // LoadLibrary + xrNegotiateLoaderRuntimeInterface, both guarded: a third-party runtime can and
    // does fault (the Oculus one did, writing through a null `this`), and a game must degrade to
    // flatscreen rather than die on a machine its author never tested.
    // PREFERS THE REAL LOADER. openxr_loader.dll does more than negotiate: it inserts API layers,
    // owns the instance-level dispatch, and is the environment every runtime is actually tested
    // against. Talking to the Oculus runtime directly got as far as its compositor client and a
    // texture swap chain, then jumped into reserved memory inside its own RuntimeIPC init -- a path
    // no shipped title exercises, because every shipped title goes through the loader.
    //
    // Direct negotiation remains as the fallback for a machine with no loader beside us; it is
    // enough for discovery and enumeration, which is all it was ever asked for.
    bool load();
    bool using_loader() const { return m_using_loader; }
    bool loaded() const { return m_get_proc != nullptr; }
    void unload();
    bool crashed() const { return m_crashed; }

    PFN_GetInstanceProcAddr get_proc() const { return m_get_proc; }
    uint32_t interface_version() const { return m_interface_version; }
    uint64_t api_version() const { return m_api_version; }
    uint16_t api_major() const { return static_cast<uint16_t>(m_api_version >> 48); }
    uint16_t api_minor() const { return static_cast<uint16_t>((m_api_version >> 32) & 0xFFFF); }
    const std::string& last_error() const { return m_error; }

private:
    bool m_discovered{false};
    std::string m_manifest;
    std::string m_library;
    std::string m_error;
    void* m_module{nullptr};
    PFN_GetInstanceProcAddr m_get_proc{nullptr};
    uint32_t m_interface_version{0};
    uint64_t m_api_version{0};
    bool m_crashed{false};
    bool m_using_loader{false};
};

}  // namespace xr

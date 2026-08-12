#include "D3D9ExUpgrade.hpp"

#include <atomic>

#include <windows.h>
#include <d3d9.h>

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "Direct3DCreate9";

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

// Resolved rather than linked. Linking d3d9.lib would make the whole mod refuse to load on a
// machine without Ex support, when the correct behaviour there is to leave the engine alone and
// keep the readback path; resolving it also tells us Ex exists BEFORE hooking anything.
std::atomic<CreateExFn> g_create_ex{nullptr};
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_upgraded{false};
std::atomic<uint32_t> g_attempts{0};
std::atomic<uint32_t> g_failures{0};
std::atomic<int32_t> g_last_hr{0};

IDirect3D9* WINAPI create_detour(UINT sdk_version) {
    g_attempts.fetch_add(1, std::memory_order_relaxed);

    // The SDK version the ENGINE asked for is forwarded unchanged. Substituting D3D_SDK_VERSION
    // from our own headers would silently hand the runtime a different contract than the caller
    // compiled against, which is the kind of mismatch that fails as a corrupted device rather than
    // as an error code.
    auto* const create_ex = g_create_ex.load(std::memory_order_relaxed);
    IDirect3D9Ex* ex = nullptr;
    const HRESULT hr = create_ex != nullptr ? create_ex(sdk_version, &ex) : E_NOINTERFACE;
    g_last_hr.store(static_cast<int32_t>(hr), std::memory_order_relaxed);

    if (SUCCEEDED(hr) && ex != nullptr) {
        // IDirect3D9Ex DERIVES from IDirect3D9, so this is the interface the engine asked for --
        // no wrapper, no thunking, and every device it creates is an Ex device that can allocate
        // shared render targets.
        g_upgraded.store(true, std::memory_order_relaxed);
        LOGX("[d3d9ex] handed the engine an IDirect3D9Ex factory (sdk %u)",
             static_cast<unsigned>(sdk_version));
        return static_cast<IDirect3D9*>(ex);
    }

    // FALL BACK RATHER THAN FAIL. A machine without Ex support must still get a game: the readback
    // path stays, which is slow but correct, and the log says which one is in force.
    g_failures.fetch_add(1, std::memory_order_relaxed);
    LOGX("[d3d9ex] Direct3DCreate9Ex failed (0x%08X) -- falling back to plain D3D9",
         static_cast<unsigned>(hr));

    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return nullptr;
    }
    return hook->original<CreateFn>()(sdk_version);
}

}  // namespace

D3D9ExUpgrade& D3D9ExUpgrade::get() {
    static D3D9ExUpgrade instance;
    return instance;
}

std::optional<std::string> D3D9ExUpgrade::on_initialize() {
    // ---- OFF UNTIL THE MANAGED POOL IS DEALT WITH ------------------------------------------
    //
    // The upgrade itself works -- the engine takes the Ex factory without complaint -- but the
    // game then dies during renderer startup:
    //
    //     0xC0000005 read of 0x00000010 at FEAR2.exe+0x218DEA, ECX = 0 (null this)
    //     #01 +0x20F370  #02 +0x20F9BA  #03 +0x20FB94  #04 +0x20FC8E
    //
    // 0x618D4E is LTShader_SetConstantByRegister, so this is the shader-constant path
    // dereferencing something that failed to be created. That is the documented consequence of an
    // Ex device: D3DPOOL_MANAGED IS REJECTED OUTRIGHT, every managed allocation the engine makes
    // now fails, and this engine does not check the result.
    //
    // Making it work therefore means intercepting the resource-creation calls and translating
    // MANAGED to DEFAULT -- which also means owning the restore-on-reset that the managed pool was
    // doing for the engine. That is a real piece of work, not a flag, so the upgrade stays behind
    // one until it exists rather than shipping a crash.
    //
    //     set FEAR2VR_D3D9EX=1 to arm it.
    char buf[8]{};
    const DWORD n = ::GetEnvironmentVariableA("FEAR2VR_D3D9EX", buf, sizeof(buf));
    if (n == 0 || buf[0] != '1') {
        LOGX("[d3d9ex] not arming: needs the managed-pool translation first "
             "(set FEAR2VR_D3D9EX=1 to try it anyway)");
        return std::nullopt;
    }

    // d3d9.dll is resident by the time anything renders, but this runs at the entry-point gate --
    // before the engine has started -- so it may genuinely not be loaded yet. LoadLibrary is safe
    // here and idempotent: the engine links against it, so this only pins a module that is about to
    // be loaded anyway rather than introducing a dependency of our own.
    HMODULE d3d9 = ::GetModuleHandleA("d3d9.dll");
    if (d3d9 == nullptr) {
        d3d9 = ::LoadLibraryA("d3d9.dll");
    }
    if (d3d9 == nullptr) {
        return "d3d9.dll is not loadable";
    }

    auto* target = ::GetProcAddress(d3d9, "Direct3DCreate9");
    if (target == nullptr) {
        return "d3d9.dll has no Direct3DCreate9";
    }

    // No Ex on this machine -> do not hook at all. An upgrade that cannot succeed should not sit in
    // the call path pretending it might.
    auto* const create_ex = reinterpret_cast<CreateExFn>(::GetProcAddress(d3d9, "Direct3DCreate9Ex"));
    if (create_ex == nullptr) {
        return "d3d9.dll has no Direct3DCreate9Ex (no shared-surface path on this machine)";
    }
    g_create_ex.store(create_ex, std::memory_order_relaxed);

    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&create_detour))) {
        return "could not hook Direct3DCreate9";
    }

    g_hooked.store(true, std::memory_order_release);
    LOGX("[d3d9ex] hooked Direct3DCreate9 at 0x%p -- the engine's next factory will be Ex", target);
    return std::nullopt;
}

D3D9ExUpgrade::State D3D9ExUpgrade::state() const {
    State s;
    s.hooked = g_hooked.load(std::memory_order_acquire);
    s.upgraded = g_upgraded.load(std::memory_order_relaxed);
    s.attempts = g_attempts.load(std::memory_order_relaxed);
    s.failures = g_failures.load(std::memory_order_relaxed);
    s.last_hr = g_last_hr.load(std::memory_order_relaxed);
    return s;
}

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- WHAT THE ENGINE ALLOCATES, AND OUT OF WHICH POOL ---------------------
//
// THE D3D9Ex GATE. Sharing a rendered surface with an OpenXR swapchain needs
// D3D9Ex, and D3D9Ex REFUSES `D3DPOOL_MANAGED`. So "does this engine allocate
// managed resources" decides whether the upgrade is a day or a month, and it is
// the single largest unknown left in the stereo path.
//
// A static sweep could not answer it. Of the 13 create call sites reachable from
// the renderer global, one passes D3DPOOL_DEFAULT as an immediate and EIGHT
// compute the pool at runtime -- which is exactly where a managed allocation
// would hide. Recording "no managed pool found" from that would have been the
// absence-of-evidence mistake; the answer has to come from the running game.
//
// ---- WHY IT IS A CONSUMER API AND NOT A PROBE -----------------------------
//
// The same interception is what a stereo path needs afterwards. Once the pools
// are known, a D3D9Ex bring-up REMAPS them on the way through -- managed becomes
// default plus an explicit upload -- and that happens at exactly these five
// entries. The counters are the diagnostic; the hook is the mechanism.
//
// A mod also legitimately wants to know when the engine allocates render
// targets: `reset_counts()` plus a poll bounds "what did this level load
// create", which is how you size an eye texture against real usage.
//
// ---- HOOKING d3d9.dll's BODIES, NOT THE VTABLE ----------------------------
//
// The device's vtable lives on the HEAP and its address does not survive a
// device reset -- which a stereo path provokes deliberately. The method BODIES
// are in d3d9.dll and do not move, so the hooks are installed there and stay
// valid across resets. The cost is scope: this sees every device in the process,
// not only the engine's. There is one, and the counters record the receiver so a
// second would be visible rather than silently merged.
class ResourceWatch final : public Mod {
public:
    static ResourceWatch& get();

    std::string_view get_name() const override { return "ResourceWatch"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_shutdown() override;

    // The five resource kinds, in IDirect3DDevice9 slot order 23..27.
    enum class Kind : uint32_t { Texture = 0, VolumeTexture, CubeTexture, VertexBuffer, IndexBuffer, kCount };

    // D3DPOOL is 0..3 (DEFAULT, MANAGED, SYSTEMMEM, SCRATCH). A fifth bucket catches
    // anything outside that, which would mean the argument was misread rather than
    // that D3D grew a pool.
    static constexpr size_t kPools = 5;

    bool hooked() const { return m_hooked.load(std::memory_order_relaxed); }
    uint64_t total() const { return m_total.load(std::memory_order_relaxed); }
    uint64_t count(Kind kind, size_t pool) const;
    uint64_t pool_total(size_t pool) const;

    // THE ANSWER, in one call. True when the engine has been observed allocating
    // out of D3DPOOL_MANAGED, which is what D3D9Ex forbids.
    bool uses_managed_pool() const { return pool_total(1) != 0; }

    // Start a fresh window. A consumer sizing an eye texture wants "what did THIS
    // level load allocate", not a running total since injection.
    void reset_counts();

    // Called by the detours. Public because the interception points are free functions with COM
    // signatures -- they cannot be members -- and a friend declaration per detour would be five
    // lines of ceremony to hide a counter increment. Not intended for callers outside this mod.
    void note_create(Kind kind, uint32_t pool);

    // Whether anything has been observed at all. Distinguishes "no managed
    // resources" from "nothing was created while we were watching", which is the
    // difference between an answer and an empty sample -- and the reason a level
    // load has to happen with the hooks already installed.
    bool observed_any() const { return total() != 0; }

private:
    ResourceWatch() = default;

    bool install();

    std::atomic<bool> m_hooked{false};
    std::atomic<uint32_t> m_retry{0};
    std::atomic<uint64_t> m_total{0};
    std::atomic<uint64_t> m_counts[static_cast<size_t>(Kind::kCount)][kPools]{};
};

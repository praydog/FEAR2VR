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

    // ---- WHAT THE MANAGED ALLOCATIONS ARE -----------------------------------
    //
    // Knowing that 99% of allocations are MANAGED says the D3D9Ex upgrade is large.
    // Whether it is POSSIBLE depends on what those resources are, because a
    // DEFAULT-pool texture CANNOT BE LOCKED and D3D9 has no way to supply initial
    // data at creation -- so anything the engine fills by locking cannot simply be
    // moved to DEFAULT. It would need the SYSTEMMEM-staging-plus-UpdateTexture dance,
    // which is a different and much larger change than swapping an argument.
    //
    // D3DUSAGE_DYNAMIC is the discriminator: a dynamic resource is lockable in
    // DEFAULT, a static one is not.
    //
    // This is also plain consumer information -- a mod sizing an eye texture wants to
    // know which formats and dimensions the engine already uses.
    uint64_t managed_dynamic() const { return m_managed_dynamic.load(std::memory_order_relaxed); }
    uint64_t managed_static() const { return m_managed_static.load(std::memory_order_relaxed); }
    uint64_t managed_rendertarget() const { return m_managed_rt.load(std::memory_order_relaxed); }

    // The largest managed texture seen, which bounds what a staging path would cost.
    uint32_t largest_managed_edge() const { return m_largest_edge.load(std::memory_order_relaxed); }
    uint32_t distinct_formats() const;

    // Called by the detours. Public because the interception points are free functions with COM
    // signatures -- they cannot be members -- and a friend declaration per detour would be five
    // lines of ceremony to hide a counter increment. Not intended for callers outside this mod.
    void note_create(Kind kind, uint32_t pool);

    // Detail for a TEXTURE creation: usage flags, format and edge length. Separate from
    // note_create because buffers carry neither a format nor a dimension.
    void note_texture(uint32_t pool, uint32_t usage, uint32_t format, uint32_t edge);

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
    std::atomic<uint64_t> m_managed_dynamic{0};
    std::atomic<uint64_t> m_managed_static{0};
    std::atomic<uint64_t> m_managed_rt{0};
    std::atomic<uint32_t> m_largest_edge{0};
    // A format is a D3DFORMAT enum; the engine uses a handful. Bounded set, and anything
    // past the cap is counted rather than dropped so the report cannot silently narrow.
    static constexpr size_t kFormatSlots = 24;
    std::atomic<uint32_t> m_formats[kFormatSlots]{};
    std::atomic<uint32_t> m_format_overflow{0};
};

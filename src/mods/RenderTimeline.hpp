#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- WHAT IS DRAWN, INTO WHICH TARGET, IN WHAT ORDER ----------------------
//
// A one-frame recording of the render-target bracket, so a UI-isolation path can
// answer the only question that decides its shape: WHICH target is bound when the
// HUD is painted, and is anything else painted into it at the same time.
//
// It exists because the obvious approach -- bracket everything drawn after the
// scene and call it "the UI" -- is only sound if nothing else draws there. The
// published VR frame today contains no UI at all (it is taken at the second eye,
// and the HUD's ten 2D passes run later), so the difference between the two
// capture points is the candidate UI layer. Measured, that difference also
// includes a right half that reads as TILED, which is either post-processing or a
// capture artefact -- and those two answers lead to different designs.
//
// HOW IT HOOKS. CLTRenderer vtable slots 11 and 12, the same way HudPassHook takes
// 16/17: engine functions in the exe's .text, not the D3D9 COM vtable, which is
// shared with Steam's overlay and replaced on device reset (see RenderHook.hpp).
// The nesting the engine guarantees is documented on sdk::SceneCamera::RendererSlot:
//
//     BeginFrame -> BeginRenderTarget -> SetupPass* -> DrawScene/EndPass -> EndRenderTarget
//
// WHAT EACH EVENT CARRIES. The target the engine bound and the size it derived,
// plus the two pass counters AT THAT INSTANT. The counters are the point: they
// turn a list of targets into "this target received the two scene passes, that one
// received the ten 2D passes", which is the actual finding.
//
// DIAGNOSTIC ONLY. It records and reports; it changes nothing. Per AGENT.MD rule 2
// it renders no verdict -- the host decides what the numbers mean.
class RenderTimeline final : public Mod {
public:
    static RenderTimeline& get();

    std::string_view get_name() const override { return "RenderTimeline"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override {}

    enum class Kind : uint8_t {
        BeginTarget = 0,
        EndTarget = 1,
        Present = 2,
    };

    struct Event {
        Kind kind{};
        uintptr_t target{};              // the descriptor the engine was handed (begin only)
        std::array<int32_t, 2> size{};   // what the engine derived for it, read in phase
        uint64_t camera_passes{};        // perspective setups so far this recording
        uint64_t hud_passes{};           // 2D/screen passes so far this recording
    };

    // ONE FRAME, then it disarms itself. A rolling recording would be a stream of
    // identical frames and a race to read a consistent one; a single latched frame is
    // what a consumer actually wants to look at.
    void arm();
    bool armed() const { return m_armed.load(std::memory_order_acquire); }

    // Non-zero once a frame has been latched. Reading is safe from any thread: the
    // recording buffer is only written while armed, and `count` is published last.
    size_t count() const { return m_count.load(std::memory_order_acquire); }
    Event event(size_t i) const;

    // Totals since injection, so a caller can see the machinery is live without arming.
    uint64_t begins() const { return m_begins.load(std::memory_order_relaxed); }
    uint64_t ends() const { return m_ends.load(std::memory_order_relaxed); }
    bool hooked() const { return m_hooked.load(std::memory_order_relaxed); }

    // Called from the detours and the frame boundary. Public because the detours are
    // free functions in the .cpp's anonymous namespace.
    void record(Kind kind, uintptr_t target);
    void close_frame();

private:
    RenderTimeline() = default;

    // Ten 2D passes plus a handful of scene targets per frame, so 64 is several times
    // the observed traffic. A frame that overflows it is itself a finding, and
    // `m_overflow` reports it rather than silently truncating.
    static constexpr size_t kMaxEvents = 64;

    std::array<Event, kMaxEvents> m_events{};
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_count{0};
    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_recording{false};
    std::atomic<uint32_t> m_overflow{0};
    std::atomic<uint64_t> m_begins{0};
    std::atomic<uint64_t> m_ends{0};
    std::atomic<bool> m_hooked{false};

    // Pass totals at the instant the recording started, so every event reports what happened
    // INSIDE the recording. Game-thread only: written when a recording opens, read in the
    // detours that run on the same thread.
    uint64_t m_base_cam{0};
    uint64_t m_base_hud{0};

public:
    uint32_t overflow() const { return m_overflow.load(std::memory_order_relaxed); }
};

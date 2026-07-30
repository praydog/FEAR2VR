#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- DRIVING THE PLAYER'S BONES, WHICH IS HOW HANDS AND WEAPONS GET INTO VR --------------
//
// `sdk::NodeControl` supplies the typed registration and says plainly that lifetime is the
// consumer's problem. THIS is that consumer. What it owns:
//
//   * WHEN registration happens. The engine walks the callback list on the game thread with no
//     synchronisation, so adding or removing from the IPC thread races a walk that is
//     dereferencing the very cells being unlinked. Every list mutation here happens in
//     `on_frame`, which runs on CClientShell::Update -- and the callback records its own thread
//     id so the claim "same thread" is MEASURED rather than assumed.
//
//   * THAT IT COMES BACK OUT. A registered cell holds a raw pointer into this DLL, and
//     `Hooks::retire()` does not know about it -- it only covers safetyhook. Leaving one behind
//     across an unload hands the engine a call into unmapped memory, which is the same class of
//     defect that killed the process when `Watchpoints` left a debug register armed. Teardown
//     is therefore an explicit, verified step rather than a side effect.
//
// WHAT IT IS FOR. A VR mod does not want the animation system's idea of where the hand is; it
// wants the hand where the player's controller is. Overriding a node's transform inside the
// evaluation puts it there, and everything socketed to that node -- the weapon, its muzzle,
// anything attached further down -- follows for free, because the engine composes them from
// this transform afterwards.
class BoneControl final : public Mod {
public:
    static BoneControl& get() {
        static BoneControl inst;
        return inst;
    }

    std::string_view get_name() const override { return "BoneControl"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_shutdown() override;

    // ---- CONSUMER API -------------------------------------------------------------------
    //
    // Attach to the local player's model, driving one node. Takes effect on the next frame,
    // because the registration itself must happen on the game thread. Safe to call repeatedly;
    // changing the node re-registers.
    //
    // Returns false only for an obviously invalid request (no node named that); the actual
    // registration result appears in `observed().attached`.
    bool attach_to_player_node(uint32_t node_index);

    // The same, by NODE name -- "L_Hand", "Head". Resolves through ModelSkeleton, so it follows
    // whatever the live asset defines.
    bool attach_to_player_node(const char* node_name);

    // By SOCKET name, which is the lookup a consumer actually reaches for and the one that
    // catches people out: "RightHand" is a SOCKET on this skeleton, riding node 38 -- there is
    // no node of that name, so the node lookup above correctly fails on it. Sockets are the
    // art's named attach points ("RightHand", "camera", "eyes"); nodes are the bones underneath.
    //
    // Driving the socket's OWNING NODE is what moves whatever is mounted there, because the
    // engine composes the socket's fixed offset onto that node afterwards.
    bool attach_to_player_socket(const char* socket_name);

    // Which node a named socket rides, without attaching -- so a caller can look before it
    // leaps, and so the fixture can assert the resolution independently of the override.
    std::optional<uint32_t> player_socket_node(const char* socket_name) const;

    // Stop driving. Idempotent, and the removal is performed on the game thread like the add.
    void detach();

    // ---- THE OVERRIDE ---------------------------------------------------------------------
    //
    // A translation added to the driven node's position, in the node's own frame, every time
    // the engine evaluates it. This is the primitive a controller-tracked hand is built from:
    // the VR runtime supplies a pose, this puts the bone there.
    //
    // Set to (0,0,0) to observe without moving anything -- which is the default, because a mod
    // that displaces the player's arm the instant it loads is not a diagnostic.
    void set_offset(float x, float y, float z);
    void clear_offset();

    // A rotation applied to the node, as a quaternion (x, y, z, w), composed with whatever the
    // animation produced. Identity by default.
    void set_rotation(float x, float y, float z, float w);
    void clear_rotation();

    struct Observed {
        bool available{};        // the mechanism resolves at all
        bool attached{};         // our callback is registered right now, per the ENGINE's list
        bool want_attached{};    // what this mod has been asked for
        uint32_t node{};         // which node we drive
        uint64_t calls{};        // callback invocations since attach
        uint64_t writes{};       // invocations in which an override was applied

        // THE RECORD LAYOUT, CHECKED IN PHASE. Every call verifies that the record's writable
        // transform is exactly the model's own `node_transforms[node_index]`; these count the
        // verdicts, so a wrong field reading shows up as a number rather than as a mystery.
        uint64_t record_consistent{};
        uint64_t record_inconsistent{};

        // Thread identity, so "the engine calls us on the game thread" is a measurement.
        uint32_t callback_thread{};
        uint32_t frame_thread{};
        bool same_thread{};

        // What the last invocation saw and wrote, so a consumer can confirm the write landed
        // rather than inferring it from the absence of a complaint.
        std::array<float, 3> last_seen_position{};
        std::array<float, 3> last_written_position{};
        bool readback_matches{};

        // How many callbacks the engine reports on this node, ours included. The difference
        // between this and `attached` is what catches a remove that silently did nothing.
        uint32_t engine_registered{};
    };

    Observed observed() const;

private:
    BoneControl() = default;
};

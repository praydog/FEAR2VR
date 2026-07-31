#include "FireRedirect.hpp"

#include <cmath>

#include <windows.h>

#include <safetyhook.hpp>

#include "../Hooks.hpp"
#include "Log.hpp"
#include "sdk/Memory.hpp"
#include "VR.hpp"
#include "sdk/CClientShell.hpp"
#include "sdk/Model.hpp"
#include "sdk/Modules.hpp"
#include "sdk/Object.hpp"
#include "sdk/PlayerMgr.hpp"

namespace {
// Weapon_TraceShot (sub_1014F4D0) prologue, gameserver.dll.
//
// THE THIRD TARGET, and the two rejected ones are the useful part of this
// comment because each was rejected by measurement, not by reading:
//
//   Weapon_FireServer entry          6 writes, impacts moved +1.23 deg when
//                                    asked for +40. Its VectorsPerRound loop
//                                    rewrites descriptor+0 with the per-pellet
//                                    spread direction after we run.
//   Weapon_FireHitscanVector entry   6 writes, impacts moved +0.06 deg when
//                                    asked for +25. It calls sub_1014D350 with
//                                    the descriptor before doing any work,
//                                    which refills the direction from the
//                                    weapon's own state.
//
// Both wrote successfully. Neither moved a bullet. A hook that fires on the
// right function at the wrong instant looks identical to a working one from
// the write counter alone, which is why `fr_writes` was never sufficient
// evidence and the impact bearing always had to be measured.
//
// This is the function the hitscan path actually does its work in -- the ELSE
// of `if (sub_100B94A0(desc))`, that predicate being an AABB containment test
// for a muzzle-inside-target contact case rather than the trace it looked like.
//
//   mov  eax, [esp+arg_4]
//   mov  ecx, dword_10333D1C
//   push esi
//   mov  esi, [esp+4+arg_28]
//   push esi / push eax
//   call sub_1011D810
// Weapon_FireServer's own prologue -- hooked ONLY to read the return address, which
// names which of its eight callers the player's shot comes through.
// Weapon_SendClientFireMessage, gameclient.dll -- THE CLIENT SIDE OF THE SHOT.
//
//   sub esp, 8 / push ebx,ebp,esi,edi / mov edi, ecx
//   lea ecx, [esp+10h] / mov dword [esp+10h], 0 / call <writer ctor>
//   mov esi, [esp+10h] / mov eax, [esi]
//
// Only the call displacement is wildcarded. NOTE the kananlib syntax: each `?`
// is ONE wildcard byte, so a rel32 needs four of them and IDA-style `??` would
// silently ask for eight.
// The instruction immediately AFTER the client's own fire-vector builder returns,
// inside the dispatcher (gameclient 0x1012DD61):
//
//     lea  eax, [esp+144h+var_130]   ; the DIRECTION out-parameter
//     push eax                        ; ... third argument
//     call sub_1012C8C0               ; builds direction + origin
//  -> test al, al                     ; WE HOOK HERE
//     ...
//     <VectorsPerRound loop: the CLIENT-SIDE effect prediction reads that direction>
//     call Weapon_SendClientFireMessage(this, spread, origin, direction)
//
// This is the only point that reaches BOTH consumers. Writing the direction at the
// sender redirects the server's trace and leaves the client drawing blood along the
// old aim, which is exactly what was observed in game.
//
// The builder is __thiscall and cleans its own four arguments, so at this
// instruction esp is back to its pre-push value and the buffer that `lea` addressed
// sits at esp+0x10. DERIVED, then PROVEN: the captured vector is published and
// compared against what the sender reports for the same shot.
constexpr const char* kVectorsBuiltPattern =
    "84 C0 0F 84 ? ? ? ? 80 BE 98 02 00 00 00 66 C7 86 B4 02 00 00 28 00";

constexpr uintptr_t kBuiltDirOffset = 0x10;

constexpr const char* kSendFirePattern =
    "83 EC 08 53 55 56 57 8B F9 8D 4C 24 10 C7 44 24 10 00 00 00 00 E8 ? ? ? ? 8B 74 24 10 8B 06";

// __thiscall(this, float a2, const float* origin, const float* direction): at the
// entry instruction the return address is at esp+0 and a2 at esp+4.
//
// THE ORDER WAS MEASURED, NOT READ. The first guess had these the other way
// round, and the captured values settled it in one run: the argument at esp+8
// read (2154.76, 2379.65, -7850.21) -- world coordinates, i.e. the muzzle -- while
// esp+12 held a small vector. Writing a unit direction into esp+8 therefore moved
// the ray's START to a point next to the world origin, which is exactly the sort
// of confident wrong answer a plausible-looking argument list produces.
constexpr uintptr_t kSendFireOriginArg = 8;
constexpr uintptr_t kSendFireDirArg = 12;

// Weapon_HandleClientFireMessage -- the server's handler for the client's fire
// request, hooked only to walk the stack back into the sender.
constexpr const char* kFireMessagePattern =
    "81 EC 98 00 00 00 83 3D ? ? ? ? 00 57 8B F9 89 7C 24 1C 0F 84";

constexpr const char* kFireServerPattern =
    "81 EC 30 02 00 00 53 55 56 8B B4 24 40 02 00 00 8B 46 24 33 DB 3B C3 8B E9";

constexpr const char* kFirePattern =
    "8B 44 24 08 8B 0D ? ? ? ? 56 8B 74 24 30 56 50 E8 ? ? ? ? 8B 0D ? ? ? ?";

// __stdcall(a1..a11) with the descriptor as a8: at the entry instruction the
// return address is at esp+0 and a8 is seven slots past a1.
constexpr uintptr_t kDescriptorStackOffset = 4 + 7 * 4;

// Fields within the descriptor, established in ENGINE_NOTES.md.
constexpr uintptr_t kDescriptorDirection = 0x00;
constexpr uintptr_t kDescriptorOrigin = 0x0C;

// gameserver.dll appears at session start, so a miss during initialize() is
// expected rather than fatal. Retry on a slow cadence -- a scan per frame would
// be wasted work for the entire time the player sits at a menu.
constexpr uint32_t kRescanFrames = 240;

void store3(std::atomic<float> (&dst)[3], float x, float y, float z) {
    dst[0].store(x, std::memory_order_relaxed);
    dst[1].store(y, std::memory_order_relaxed);
    dst[2].store(z, std::memory_order_relaxed);
}

std::array<float, 3> load3(const std::atomic<float> (&src)[3]) {
    return {src[0].load(std::memory_order_relaxed), src[1].load(std::memory_order_relaxed),
            src[2].load(std::memory_order_relaxed)};
}
} // namespace

FireRedirect& FireRedirect::get() {
    static FireRedirect instance;
    return instance;
}

std::array<float, 3> FireRedirect::built_dir() const { return load3(m_built_dir); }

std::array<float, 4> FireRedirect::weapon_quat() const {
    return {m_weapon_quat[0].load(std::memory_order_relaxed), m_weapon_quat[1].load(std::memory_order_relaxed),
            m_weapon_quat[2].load(std::memory_order_relaxed), m_weapon_quat[3].load(std::memory_order_relaxed)};
}

std::array<float, 4> FireRedirect::weapon_object_quat() const {
    return {m_weapon_obj_quat[0].load(std::memory_order_relaxed), m_weapon_obj_quat[1].load(std::memory_order_relaxed),
            m_weapon_obj_quat[2].load(std::memory_order_relaxed), m_weapon_obj_quat[3].load(std::memory_order_relaxed)};
}

std::array<float, 3> FireRedirect::weapon_forward() const { return load3(m_weapon_fwd); }

std::array<float, 3> FireRedirect::last_sent_dir() const { return load3(m_sent_dir); }
std::array<float, 3> FireRedirect::last_sent_origin() const { return load3(m_sent_origin); }

std::array<float, 3> FireRedirect::last_engine_dir() const {
    return load3(m_engine_dir);
}

std::array<float, 3> FireRedirect::last_written_dir() const {
    return load3(m_written_dir);
}

std::array<float, 3> FireRedirect::last_origin() const {
    return load3(m_origin);
}

bool FireRedirect::set_direction(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }
    // A direction that is not unit length is a symptom, not an inconvenience:
    // normalizing here would convert a caller's scaling bug into a subtly wrong
    // aim that nobody can see. Refuse it and let the caller be wrong loudly.
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.999f && len < 1.001f)) {
        return false;
    }
    store3(m_dir, x, y, z);
    m_armed.store(true, std::memory_order_release);
    return true;
}

void FireRedirect::set_mode(Mode mode) {
    m_mode.store(mode, std::memory_order_release);
    // Absolute mode still needs a direction supplied separately; Reverse does not,
    // so it arms itself. Off disarms both so nothing can be left applying.
    if (mode == Mode::Off) {
        m_armed.store(false, std::memory_order_release);
    } else if (mode != Mode::Absolute) {
        m_armed.store(true, std::memory_order_release);
    }
    LOGX("[fire] redirect mode = %s (hotkey 0x%02X)",
         mode == Mode::Off ? "off" : (mode == Mode::Reverse ? "REVERSE" : (mode == Mode::Weapon ? "WEAPON" : (mode == Mode::Controller ? "CONTROLLER" : "absolute"))),
         m_hotkey.load(std::memory_order_relaxed));
}

void FireRedirect::clear_direction() {
    m_armed.store(false, std::memory_order_release);
}

size_t FireRedirect::sender_frames(uintptr_t* out, size_t cap) const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxSenderFrames && n < cap; ++i) {
        const uintptr_t v = m_sender[i].load(std::memory_order_relaxed);
        if (v != 0) {
            out[n++] = v;
        }
    }
    return n;
}

void FireRedirect::on_vectors_built(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_builds.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t dir_ptr = ctx.esp + kBuiltDirOffset;
    const auto cur = sdk::mem::read<std::array<float, 3>>(dir_ptr);
    if (!cur.has_value()) {
        return;
    }
    store3(self.m_built_dir, (*cur)[0], (*cur)[1], (*cur)[2]);

    if (!self.m_armed.load(std::memory_order_acquire)) {
        return;
    }
    if (self.m_hotkey.load(std::memory_order_relaxed) != 0 &&
        !self.m_hotkey_held.load(std::memory_order_relaxed)) {
        return;
    }

    std::array<float, 3> dir{};
    const Mode mode = self.m_mode.load(std::memory_order_acquire);
    if (mode == Mode::Weapon || mode == Mode::Controller) {
        if (!self.m_weapon_ok.load(std::memory_order_relaxed)) {
            return;
        }
        dir = load3(self.m_weapon_fwd);
    } else if (mode == Mode::Reverse) {
        dir = {-(*cur)[0], -(*cur)[1], -(*cur)[2]};
    } else {
        dir = load3(self.m_dir);
    }

    if (sdk::mem::write<std::array<float, 3>>(dir_ptr, dir)) {
        store3(self.m_written_dir, dir[0], dir[1], dir[2]);
        self.m_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

void FireRedirect::on_send_fire(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_sends.fetch_add(1, std::memory_order_relaxed);

    if (const auto ret = sdk::mem::read<uintptr_t>(ctx.esp); ret.has_value() && *ret != 0) {
        self.m_send_caller.store(*ret, std::memory_order_relaxed);
    }
    const uintptr_t dir_ptr = sdk::mem::read<uintptr_t>(ctx.esp + kSendFireDirArg).value_or(0);
    const uintptr_t org_ptr = sdk::mem::read<uintptr_t>(ctx.esp + kSendFireOriginArg).value_or(0);
    if (dir_ptr == 0) {
        return;
    }
    // Record what the CLIENT was about to send before touching it, so a test can
    // show the value was REPLACED rather than merely present.
    if (const auto sent = sdk::mem::read<std::array<float, 3>>(dir_ptr)) {
        store3(self.m_sent_dir, (*sent)[0], (*sent)[1], (*sent)[2]);
    }
    if (org_ptr != 0) {
        if (const auto o = sdk::mem::read<std::array<float, 3>>(org_ptr)) {
            store3(self.m_sent_origin, (*o)[0], (*o)[1], (*o)[2]);
        }
    }
    // NO WRITE HERE. The redirect moved UPSTREAM to on_vectors_built, which is the
    // only point both consumers read: the client's own effect prediction runs between
    // the builder and this send, so writing here steered the server while the client
    // kept drawing blood along the old aim -- observed in game as a target that takes
    // no damage while still showing impacts. Redirecting in both places would also
    // double-apply, and Reverse would negate twice and cancel.
}

void FireRedirect::on_message(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_messages.fetch_add(1, std::memory_order_relaxed);

    const auto* gc = sdk::Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return;
    }
    // Walk a bounded window of the raw stack rather than the frame chain: these
    // are optimised frames with no reliable ebp, and a return address is simply
    // a stack dword pointing into a module's code. Bounded because an unbounded
    // walk on the game thread is the per-frame budget mistake this project has
    // already made once.
    size_t found = 0;
    for (size_t i = 0; i < 256 && found < kMaxSenderFrames; ++i) {
        const uintptr_t slot = ctx.esp + i * sizeof(uintptr_t);
        const auto v = sdk::mem::read<uintptr_t>(slot);
        if (!v.has_value()) {
            break;
        }
        if (*v >= gc->base && *v < gc->base + gc->size) {
            self.m_sender[found++].store(*v, std::memory_order_relaxed);
        }
    }
    for (size_t i = found; i < kMaxSenderFrames; ++i) {
        self.m_sender[i].store(0, std::memory_order_relaxed);
    }
}

void FireRedirect::on_fire_entry(SafetyHookContext& ctx) {
    // At the entry instruction the return address is the top of the stack.
    auto& self = FireRedirect::get();
    self.m_fire_entries.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t ret = sdk::mem::read<uintptr_t>(ctx.esp).value_or(0);
    if (ret != 0) {
        self.m_fire_caller.store(ret, std::memory_order_relaxed);
    }
}

void FireRedirect::on_fire(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_calls.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t desc = sdk::mem::read<uintptr_t>(ctx.esp + kDescriptorStackOffset).value_or(0);
    if (desc == 0) {
        return;
    }

    // Record what the ENGINE was about to fire before touching anything. Without
    // this a passing test proves only that our value is present, not that it
    // replaced a different one.
    self.m_last_desc.store(desc, std::memory_order_relaxed);
    const auto engine_dir = sdk::mem::read<std::array<float, 3>>(desc + kDescriptorDirection);
    const auto origin = sdk::mem::read<std::array<float, 3>>(desc + kDescriptorOrigin);
    if (!engine_dir.has_value() || !origin.has_value()) {
        return; // descriptor not readable -- our layout belief is wrong, do nothing
    }
    store3(self.m_engine_dir, (*engine_dir)[0], (*engine_dir)[1], (*engine_dir)[2]);
    store3(self.m_origin, (*origin)[0], (*origin)[1], (*origin)[2]);

    // NO WRITE HERE, deliberately. Writing this descriptor was measured inert at
    // three separate points on the server path (see the header), because by the
    // time the server has it the direction is a transcription of what the client
    // already sent. This hook stays for OBSERVATION -- it is what proves the
    // engine's own direction predicts the impacts -- and the redirection happens
    // in on_send_fire, upstream of the packet.
}

std::optional<std::string> FireRedirect::on_initialize() {
    // Not an error when it misses: the module is lazy. on_frame() retries.
    const uintptr_t target = sdk::Modules::get().scan_game_server(kFirePattern, "Weapon_TraceShot");
    if (target == 0) {
        m_retry_countdown.store(kRescanFrames, std::memory_order_relaxed);
        return std::nullopt;
    }
    m_target.store(target, std::memory_order_relaxed);
    m_hooked.store(Hooks::get().install_mid("weapon_trace_shot", reinterpret_cast<void*>(target), &FireRedirect::on_fire),
                   std::memory_order_relaxed);
    if (const uintptr_t entry = sdk::Modules::get().scan_game_server(kFireServerPattern, "Weapon_FireServer"); entry != 0) {
        Hooks::get().install_mid("weapon_fire_server_entry", reinterpret_cast<void*>(entry), &FireRedirect::on_fire_entry);
    }
    if (const uintptr_t msg = sdk::Modules::get().scan_game_server(kFireMessagePattern, "Weapon_HandleClientFireMessage"); msg != 0) {
        Hooks::get().install_mid("weapon_fire_message", reinterpret_cast<void*>(msg), &FireRedirect::on_message);
    }
    if (const uintptr_t vb = sdk::Modules::get().scan_game_client(kVectorsBuiltPattern, "Weapon_FireVectorsBuilt"); vb != 0) {
        Hooks::get().install_mid("weapon_vectors_built", reinterpret_cast<void*>(vb), &FireRedirect::on_vectors_built);
    }
    if (const uintptr_t snd = sdk::Modules::get().scan_game_client(kSendFirePattern, "Weapon_SendClientFireMessage"); snd != 0) {
        m_send_hooked.store(Hooks::get().install_mid("weapon_send_fire", reinterpret_cast<void*>(snd), &FireRedirect::on_send_fire),
                            std::memory_order_relaxed);
    }
    return std::nullopt;
}

void FireRedirect::on_frame() {
    // Poll the hold key on the GAME thread. GetAsyncKeyState is a snapshot of
    // physical key state, so it works while the game has focus and needs no hook
    // into the engine's input path -- which matters because this must not perturb
    // the very input the player is using to run the experiment.
    // Sample where the WEAPON points, on the thread allowed to resolve it. Only when
    // something will consume it -- this walks a socket chain and there is no reason
    // to pay for it every frame of normal play.
    if (m_mode.load(std::memory_order_relaxed) == Mode::Controller) {
        // The controller's orientation is something we KNOW rather than something read
        // back out of the engine's rig, which is why this works where Weapon mode
        // cannot: the rig is downstream of BoneControl and loses the pitch on the way.
        //
        // Basis is the body's HEADING ALONE, not its full aim -- the same correction
        // HeadTracking needed. Yawing about an axis tilted by the player's own pitch is
        // not yawing, and it showed up there as pitch drift.
        bool ok = false;
        const auto& rt = vr::simulated_runtime();
        const auto hand = rt.hand(vr::VRRuntime::Hand::RIGHT);
        if (hand.aim.valid) {
            if (const auto heading = sdk::PlayerMgr::aim_yaw(0); heading.has_value()) {
                const auto e = VR::runtime_to_engine_rotation(hand.aim.orientation);
                const float half = *heading * 0.5f;
                const regenny::LTRotation yaw{0.0f, std::sin(half), 0.0f, std::cos(half)};
                const regenny::LTRotation local{e[0], e[1], e[2], e[3]};
                const auto world = sdk::multiply_rotations(yaw, local);
                const auto fwd = sdk::forward_of(world);
                const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
                if (std::isfinite(len) && len > 0.9f && len < 1.1f) {
                    store3(m_weapon_fwd, fwd.x / len, fwd.y / len, fwd.z / len);
                    ok = true;
                }
            }
        }
        m_weapon_ok.store(ok, std::memory_order_relaxed);
    } else if (m_mode.load(std::memory_order_relaxed) == Mode::Weapon) {
        bool ok = false;
        if (const auto player = sdk::CClientShell::local_player(0); player.has_value()) {
            if (const auto muzzle = sdk::attached_socket(player->object, "flash"); muzzle.has_value()) {
                // A stale socket is built on a stale bone, so its direction is last
                // frame's at best and garbage at worst -- refuse rather than aim with it.
                if (!muzzle->transform.stale) {
                    // The socket transform is composed from the player model's bone cache,
                    // and that model's body does not pitch -- so compare against the attached
                    // WEAPON OBJECT's own rotation, which the engine writes separately.
                    if (muzzle->object != nullptr) {
                        if (const auto wq = sdk::mem::read<regenny::LTRotation>(
                                reinterpret_cast<uintptr_t>(muzzle->object) + 0x20)) {
                            m_weapon_obj_quat[0].store(wq->x, std::memory_order_relaxed);
                            m_weapon_obj_quat[1].store(wq->y, std::memory_order_relaxed);
                            m_weapon_obj_quat[2].store(wq->z, std::memory_order_relaxed);
                            m_weapon_obj_quat[3].store(wq->w, std::memory_order_relaxed);
                        }
                    }
                    const auto& q = muzzle->transform.rotation;
                    m_weapon_quat[0].store(q.x, std::memory_order_relaxed);
                    m_weapon_quat[1].store(q.y, std::memory_order_relaxed);
                    m_weapon_quat[2].store(q.z, std::memory_order_relaxed);
                    m_weapon_quat[3].store(q.w, std::memory_order_relaxed);
                    const auto fwd = sdk::forward_of(muzzle->transform.rotation);
                    const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
                    if (std::isfinite(len) && len > 0.9f && len < 1.1f) {
                        store3(m_weapon_fwd, fwd.x / len, fwd.y / len, fwd.z / len);
                        ok = true;
                    }
                }
            }
        }
        m_weapon_ok.store(ok, std::memory_order_relaxed);
    }

    if (const int vk = m_hotkey.load(std::memory_order_relaxed); vk != 0) {
        m_hotkey_held.store((GetAsyncKeyState(vk) & 0x8000) != 0, std::memory_order_relaxed);
    } else {
        m_hotkey_held.store(false, std::memory_order_relaxed);
    }

    if (m_hooked.load(std::memory_order_relaxed)) {
        return;
    }
    // The module arrives when a session starts, which is long after our own
    // initialize(). Poll for it, slowly.
    auto remaining = m_retry_countdown.load(std::memory_order_relaxed);
    if (remaining > 0) {
        m_retry_countdown.store(remaining - 1, std::memory_order_relaxed);
        return;
    }
    m_retry_countdown.store(kRescanFrames, std::memory_order_relaxed);

    const uintptr_t target = sdk::Modules::get().scan_game_server(kFirePattern, "Weapon_TraceShot");
    if (target == 0) {
        return;
    }
    m_target.store(target, std::memory_order_relaxed);
    m_hooked.store(Hooks::get().install_mid("weapon_trace_shot", reinterpret_cast<void*>(target), &FireRedirect::on_fire),
                   std::memory_order_relaxed);
    if (const uintptr_t entry = sdk::Modules::get().scan_game_server(kFireServerPattern, "Weapon_FireServer"); entry != 0) {
        Hooks::get().install_mid("weapon_fire_server_entry", reinterpret_cast<void*>(entry), &FireRedirect::on_fire_entry);
    }
    if (const uintptr_t msg = sdk::Modules::get().scan_game_server(kFireMessagePattern, "Weapon_HandleClientFireMessage"); msg != 0) {
        Hooks::get().install_mid("weapon_fire_message", reinterpret_cast<void*>(msg), &FireRedirect::on_message);
    }
    if (const uintptr_t vb = sdk::Modules::get().scan_game_client(kVectorsBuiltPattern, "Weapon_FireVectorsBuilt"); vb != 0) {
        Hooks::get().install_mid("weapon_vectors_built", reinterpret_cast<void*>(vb), &FireRedirect::on_vectors_built);
    }
    if (const uintptr_t snd = sdk::Modules::get().scan_game_client(kSendFirePattern, "Weapon_SendClientFireMessage"); snd != 0) {
        m_send_hooked.store(Hooks::get().install_mid("weapon_send_fire", reinterpret_cast<void*>(snd), &FireRedirect::on_send_fire),
                            std::memory_order_relaxed);
    }
}

void FireRedirect::on_shutdown() {
    // Disarm before hook retirement so a shot in flight during teardown gets the
    // engine's own direction rather than a stale one of ours.
    m_mode.store(Mode::Off, std::memory_order_release);
    m_hotkey.store(0, std::memory_order_relaxed);
    clear_direction();
}

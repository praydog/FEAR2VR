#include "FireRedirect.hpp"

#include <cmath>

#include <safetyhook.hpp>

#include "../Hooks.hpp"
#include "Log.hpp"
#include "sdk/Memory.hpp"
#include "sdk/Modules.hpp"

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

void FireRedirect::on_send_fire(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_sends.fetch_add(1, std::memory_order_relaxed);

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
    if (!self.m_armed.load(std::memory_order_acquire)) {
        return;
    }
    const auto dir = load3(self.m_dir);
    if (sdk::mem::write<std::array<float, 3>>(dir_ptr, dir)) {
        store3(self.m_written_dir, dir[0], dir[1], dir[2]);
        self.m_writes.fetch_add(1, std::memory_order_relaxed);
    }
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
    if (const uintptr_t snd = sdk::Modules::get().scan_game_client(kSendFirePattern, "Weapon_SendClientFireMessage"); snd != 0) {
        m_send_hooked.store(Hooks::get().install_mid("weapon_send_fire", reinterpret_cast<void*>(snd), &FireRedirect::on_send_fire),
                            std::memory_order_relaxed);
    }
    return std::nullopt;
}

void FireRedirect::on_frame() {
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
    if (const uintptr_t snd = sdk::Modules::get().scan_game_client(kSendFirePattern, "Weapon_SendClientFireMessage"); snd != 0) {
        m_send_hooked.store(Hooks::get().install_mid("weapon_send_fire", reinterpret_cast<void*>(snd), &FireRedirect::on_send_fire),
                            std::memory_order_relaxed);
    }
}

void FireRedirect::on_shutdown() {
    // Disarm before hook retirement so a shot in flight during teardown gets the
    // engine's own direction rather than a stale one of ours.
    clear_direction();
}

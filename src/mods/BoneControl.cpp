#include "BoneControl.hpp"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include <Windows.h>

#include "sdk/CClientShell.hpp"
#include "sdk/Model.hpp"
#include "sdk/NodeControl.hpp"
#include "sdk/Object.hpp"

#include "Log.hpp"

namespace {

// ---- STATE SHARED WITH THE ENGINE'S THREAD ------------------------------------------------
//
// The callback runs inside skeleton evaluation. It must not allocate, lock, or call anything
// that might. Everything it touches is a plain atomic, and the whole body is a handful of
// loads plus a copy.
std::atomic<bool> g_want_attached{false};
std::atomic<bool> g_attached{false};
std::atomic<uint32_t> g_node{0};
std::atomic<uint32_t> g_pending_node{0};

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_writes{0};
std::atomic<uint64_t> g_consistent{0};
std::atomic<uint64_t> g_inconsistent{0};
std::atomic<uint32_t> g_cb_thread{0};
std::atomic<uint32_t> g_frame_thread{0};

std::atomic<float> g_off[3]{{0.0f}, {0.0f}, {0.0f}};
std::atomic<bool> g_rot_armed{false};
std::atomic<float> g_rot[4]{{0.0f}, {0.0f}, {0.0f}, {1.0f}};

std::atomic<float> g_seen[3]{{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_wrote[3]{{0.0f}, {0.0f}, {0.0f}};
std::atomic<bool> g_readback_ok{false};

// The model we are registered against. Only ever written on the game thread; the callback does
// not use it (it gets everything from the record), and the IPC thread only reads it for
// diagnostics.
std::atomic<uintptr_t> g_model{0};

// Hamilton product, (x, y, z, w). Written out rather than pulled from the SDK's rotation helper
// because this runs on the engine's hot path and must not reach outside this translation unit.
void quat_mul(const float* a, const float* b, float* out) {
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

void __cdecl node_callback(sdk::NodeControlData* data, void* userdata) {
    (void)userdata;
    if (data == nullptr || data->transform == nullptr) {
        return;
    }
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_cb_thread.store(::GetCurrentThreadId(), std::memory_order_relaxed);

    // WHAT THE ANIMATION PRODUCED, before we touch it.
    auto* xf = data->transform;
    g_seen[0].store(xf->position.x, std::memory_order_relaxed);
    g_seen[1].store(xf->position.y, std::memory_order_relaxed);
    g_seen[2].store(xf->position.z, std::memory_order_relaxed);

    // THE LAYOUT CLAIM, CHECKED WHERE IT IS CHECKABLE. `record_is_consistent` compares the
    // record's transform pointer against the model's own node_transforms array, which only
    // means anything inside the call.
    const auto model = reinterpret_cast<const regenny::LTObject*>(g_model.load(std::memory_order_relaxed));
    if (model != nullptr) {
        if (sdk::NodeControl::record_is_consistent(model, data)) {
            g_consistent.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_inconsistent.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const float dx = g_off[0].load(std::memory_order_relaxed);
    const float dy = g_off[1].load(std::memory_order_relaxed);
    const float dz = g_off[2].load(std::memory_order_relaxed);
    const bool rot = g_rot_armed.load(std::memory_order_relaxed);
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f && !rot) {
        return;  // observing only
    }

    xf->position.x += dx;
    xf->position.y += dy;
    xf->position.z += dz;
    if (rot) {
        const float q[4] = {g_rot[0].load(std::memory_order_relaxed),
                            g_rot[1].load(std::memory_order_relaxed),
                            g_rot[2].load(std::memory_order_relaxed),
                            g_rot[3].load(std::memory_order_relaxed)};
        const float cur[4] = {xf->rotation.x, xf->rotation.y, xf->rotation.z, xf->rotation.w};
        float out[4];
        quat_mul(q, cur, out);
        xf->rotation.x = out[0];
        xf->rotation.y = out[1];
        xf->rotation.z = out[2];
        xf->rotation.w = out[3];
    }

    g_wrote[0].store(xf->position.x, std::memory_order_relaxed);
    g_wrote[1].store(xf->position.y, std::memory_order_relaxed);
    g_wrote[2].store(xf->position.z, std::memory_order_relaxed);
    // Read back through the same pointer: if the write did not stick, this is where it shows.
    g_readback_ok.store(xf->position.x == g_wrote[0].load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    g_writes.fetch_add(1, std::memory_order_relaxed);
}

// The local player's object, or 0. Both callers are on the game thread.
uintptr_t player_object() {
    const auto p = sdk::CClientShell::local_player(0);
    if (!p.has_value()) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(p->object);
}

} // namespace

std::optional<std::string> BoneControl::on_initialize() {
    // Nothing to install: registration targets a model that does not exist until a world is
    // loaded, and it must happen on the game thread. on_frame drives it.
    return std::nullopt;
}

void BoneControl::on_frame() {
    g_frame_thread.store(::GetCurrentThreadId(), std::memory_order_relaxed);

    const bool want = g_want_attached.load(std::memory_order_relaxed);
    const bool have = g_attached.load(std::memory_order_relaxed);
    const uint32_t want_node = g_pending_node.load(std::memory_order_relaxed);
    const uint32_t have_node = g_node.load(std::memory_order_relaxed);

    if (have && (!want || want_node != have_node)) {
        const auto model = reinterpret_cast<const regenny::LTObject*>(
            g_model.load(std::memory_order_relaxed));
        if (model != nullptr) {
            sdk::NodeControl::remove(model, have_node, &node_callback, this);
        }
        g_attached.store(false, std::memory_order_relaxed);
        g_model.store(0, std::memory_order_relaxed);
    }

    if (want && !g_attached.load(std::memory_order_relaxed)) {
        const uintptr_t obj = player_object();
        if (obj != 0) {
            // Publish the model BEFORE registering: the engine can call back the moment the
            // cell is linked, and the callback reads this to verify the record.
            g_model.store(obj, std::memory_order_relaxed);
            if (sdk::NodeControl::add(reinterpret_cast<const regenny::LTObject*>(obj), want_node,
                                      &node_callback, this)) {
                g_node.store(want_node, std::memory_order_relaxed);
                g_attached.store(true, std::memory_order_relaxed);
                g_calls.store(0, std::memory_order_relaxed);
                g_writes.store(0, std::memory_order_relaxed);
                g_consistent.store(0, std::memory_order_relaxed);
                g_inconsistent.store(0, std::memory_order_relaxed);
                LOGX("[bonecontrol] driving node %u of player model 0x%08" PRIXPTR, want_node, obj);
            } else {
                g_model.store(0, std::memory_order_relaxed);
            }
        }
    }
}

void BoneControl::on_shutdown() {
    // THE CELL MUST COME OUT BEFORE THE IMAGE DOES.
    //
    // `Mod`'s contract says mods remove nothing here because hook retirement is global -- but
    // that is about safetyhook, and this registration is not a hook. Nothing else in the
    // framework knows the engine is holding a pointer to `node_callback`, so if it is still
    // linked when the DLL unmaps, the next skeleton evaluation calls freed memory.
    //
    // Removal happens inline rather than via on_frame: by the time shutdown runs, there is no
    // guarantee another frame will ever be delivered (the unload path stops the world shortly
    // after), so waiting for one risks never removing it at all. The residual race with a
    // concurrent walk is bounded by the framework's quiescence proof, which suspends every
    // other thread and refuses to unmap while any of them is executing inside our image.
    g_want_attached.store(false, std::memory_order_relaxed);
    if (!g_attached.load(std::memory_order_relaxed)) {
        return;
    }
    const auto model = reinterpret_cast<const regenny::LTObject*>(g_model.load(std::memory_order_relaxed));
    const uint32_t node = g_node.load(std::memory_order_relaxed);
    if (model != nullptr) {
        const bool ok = sdk::NodeControl::remove(model, node, &node_callback, this);
        const auto still = sdk::NodeControl::is_registered(model, node, &node_callback);
        LOGX("[bonecontrol] shutdown remove node %u: rc=%d still_registered=%d", node, ok ? 1 : 0,
             still.has_value() ? (*still ? 1 : 0) : -1);
    }
    g_attached.store(false, std::memory_order_relaxed);
    g_model.store(0, std::memory_order_relaxed);
}

bool BoneControl::attach_to_player_node(uint32_t node_index) {
    g_pending_node.store(node_index, std::memory_order_relaxed);
    g_want_attached.store(true, std::memory_order_relaxed);
    return true;
}

bool BoneControl::attach_to_player_node(const char* node_name) {
    if (node_name == nullptr) {
        return false;
    }
    const auto obj = player_object();
    if (obj == 0) {
        return false;
    }
    const auto sk = sdk::ModelSkeleton::from_object(reinterpret_cast<const regenny::LTObject*>(obj));
    if (!sk.has_value()) {
        return false;
    }
    const auto idx = sk->find_node(node_name);
    if (!idx.has_value()) {
        return false;
    }
    return attach_to_player_node(static_cast<uint32_t>(*idx));
}

std::optional<uint32_t> BoneControl::player_socket_node(const char* socket_name) const {
    if (socket_name == nullptr) {
        return std::nullopt;
    }
    const auto obj = player_object();
    if (obj == 0) {
        return std::nullopt;
    }
    const auto sk = sdk::ModelSkeleton::from_object(reinterpret_cast<const regenny::LTObject*>(obj));
    if (!sk.has_value()) {
        return std::nullopt;
    }
    const auto idx = sk->find_socket(socket_name);
    if (!idx.has_value()) {
        return std::nullopt;
    }
    const auto so = sk->socket(*idx);
    if (!so.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(so->node_index);
}

bool BoneControl::attach_to_player_socket(const char* socket_name) {
    const auto node = player_socket_node(socket_name);
    if (!node.has_value()) {
        return false;
    }
    return attach_to_player_node(*node);
}

void BoneControl::detach() {
    g_want_attached.store(false, std::memory_order_relaxed);
}

void BoneControl::set_offset(float x, float y, float z) {
    g_off[0].store(x, std::memory_order_relaxed);
    g_off[1].store(y, std::memory_order_relaxed);
    g_off[2].store(z, std::memory_order_relaxed);
}

void BoneControl::clear_offset() {
    set_offset(0.0f, 0.0f, 0.0f);
}

void BoneControl::set_rotation(float x, float y, float z, float w) {
    g_rot[0].store(x, std::memory_order_relaxed);
    g_rot[1].store(y, std::memory_order_relaxed);
    g_rot[2].store(z, std::memory_order_relaxed);
    g_rot[3].store(w, std::memory_order_relaxed);
    g_rot_armed.store(true, std::memory_order_relaxed);
}

void BoneControl::clear_rotation() {
    g_rot_armed.store(false, std::memory_order_relaxed);
    g_rot[0].store(0.0f, std::memory_order_relaxed);
    g_rot[1].store(0.0f, std::memory_order_relaxed);
    g_rot[2].store(0.0f, std::memory_order_relaxed);
    g_rot[3].store(1.0f, std::memory_order_relaxed);
}

BoneControl::Observed BoneControl::observed() const {
    Observed out{};
    out.available = sdk::NodeControl::available();
    out.attached = g_attached.load(std::memory_order_relaxed);
    out.want_attached = g_want_attached.load(std::memory_order_relaxed);
    out.node = g_node.load(std::memory_order_relaxed);
    out.calls = g_calls.load(std::memory_order_relaxed);
    out.writes = g_writes.load(std::memory_order_relaxed);
    out.record_consistent = g_consistent.load(std::memory_order_relaxed);
    out.record_inconsistent = g_inconsistent.load(std::memory_order_relaxed);
    out.callback_thread = g_cb_thread.load(std::memory_order_relaxed);
    out.frame_thread = g_frame_thread.load(std::memory_order_relaxed);
    out.same_thread = out.callback_thread != 0 && out.callback_thread == out.frame_thread;
    for (size_t i = 0; i < 3; ++i) {
        out.last_seen_position[i] = g_seen[i].load(std::memory_order_relaxed);
        out.last_written_position[i] = g_wrote[i].load(std::memory_order_relaxed);
    }
    out.readback_matches = g_readback_ok.load(std::memory_order_relaxed);

    // WHAT THE ENGINE THINKS, not what we think. Reads the model's own list.
    const auto model = reinterpret_cast<const regenny::LTObject*>(g_model.load(std::memory_order_relaxed));
    if (model != nullptr) {
        out.engine_registered =
            static_cast<uint32_t>(sdk::NodeControl::registered_count(model, out.node).value_or(0));
    }
    return out;
}

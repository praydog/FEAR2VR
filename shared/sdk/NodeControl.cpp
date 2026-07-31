#include "NodeControl.hpp"

#include "Memory.hpp"
#include "Model.hpp"
#include "Vtables.hpp"
#include "interfaces/ILTModel.hpp"
#include "regenny/regenny/LTModelAsset.hpp"
#include "regenny/regenny/LTModelBlock120.hpp"
#include "regenny/regenny/LTModelObject.hpp"

namespace sdk {
namespace {

constexpr const char* kModelClientVtable = "CLTModelClient";

// The engine validates the object itself (`type == OT_MODEL`) inside the vtable entries, but
// it does so by dereferencing -- so a bad pointer faults there rather than being rejected.
// Everything below therefore reads the object through the guarded path first.
constexpr uint8_t kOtModel = 1;

// ILTModel's entries are __stdcall and take the object handle first.
using AddNodeFn = int(__stdcall*)(const void* obj, uint32_t node, void* fn, void* userdata);
using AddObjectFn = int(__stdcall*)(const void* obj, void* fn, void* userdata);
using RemoveNodeFn = int(__stdcall*)(const void* obj, uint32_t node, void* fn, void* userdata);
using RemoveObjectFn = int(__stdcall*)(const void* obj, void* fn, void* userdata);

// LT_OK. The entries return 60 (LT_INVALIDPARAMS) on rejection.
constexpr int kLtOk = 0;

uintptr_t slot(size_t index) {
    // Not latched: the catalogue depends on the exe being mapped and on the vtable extent, and
    // an early miss must not poison the rest of the session.
    return Vtables::resolve(kModelClientVtable, index).value_or(0);
}

// The model's node block, copied out. Returns false when the object is not a readable model.
//
// Reads through the GENERATED schema, so no offset for the block or its fields appears in this
// file -- `block_120` and its members are whatever fear2.genny says they are.
bool read_block(const regenny::LTObject* model, regenny::LTModelBlock120* out, uint32_t* node_count) {
    if (model == nullptr || out == nullptr) {
        return false;
    }
    regenny::LTObject base{};
    if (!mem::copy(&base, reinterpret_cast<uintptr_t>(model), sizeof(base))) {
        return false;
    }
    if (base.type != kOtModel) {
        return false;
    }
    const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(model);
    if (!mem::copy(out, reinterpret_cast<uintptr_t>(&obj->block_120), sizeof(*out))) {
        return false;
    }
    if (out->asset == nullptr) {
        return false;
    }
    if (node_count != nullptr) {
        // The SAME bound the engine itself applies: LTModel_NodeControl_IsValidNode rejects
        // `node >= asset->node_count`. Read through the schema, so the offset is the
        // generated field's, not one written here.
        regenny::LTModelAsset asset{};
        if (!mem::copy(&asset, reinterpret_cast<uintptr_t>(out->asset), sizeof(asset))) {
            return false;
        }
        *node_count = asset.node_count;
    }
    return true;
}

} // namespace

bool NodeControl::available() {
    return interfaces::ILTModel::get_client() != nullptr && slot(kSlotAddNode) != 0 &&
           slot(kSlotRemoveNode) != 0;
}

bool NodeControl::add(const regenny::LTObject* model, uint32_t node, NodeControlFn fn, void* userdata) {
    if (fn == nullptr) {
        return false;
    }
    auto* iface = interfaces::ILTModel::get_client();
    const uintptr_t entry = slot(kSlotAddNode);
    if (iface == nullptr || entry == 0) {
        return false;
    }
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count) || node >= count) {
        return false;
    }
    return reinterpret_cast<AddNodeFn>(entry)(model, node, reinterpret_cast<void*>(fn), userdata) == kLtOk;
}

bool NodeControl::add_all(const regenny::LTObject* model, NodeControlFn fn, void* userdata) {
    if (fn == nullptr) {
        return false;
    }
    auto* iface = interfaces::ILTModel::get_client();
    const uintptr_t entry = slot(kSlotAddObject);
    if (iface == nullptr || entry == 0) {
        return false;
    }
    regenny::LTModelBlock120 block{};
    if (!read_block(model, &block, nullptr)) {
        return false;
    }
    return reinterpret_cast<AddObjectFn>(entry)(model, reinterpret_cast<void*>(fn), userdata) == kLtOk;
}

bool NodeControl::remove(const regenny::LTObject* model, uint32_t node, NodeControlFn fn, void* userdata) {
    if (fn == nullptr) {
        return false;
    }
    auto* iface = interfaces::ILTModel::get_client();
    const uintptr_t entry = slot(kSlotRemoveNode);
    if (iface == nullptr || entry == 0) {
        return false;
    }
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count) || node >= count) {
        return false;
    }
    return reinterpret_cast<RemoveNodeFn>(entry)(model, node, reinterpret_cast<void*>(fn), userdata) == kLtOk;
}

bool NodeControl::remove_all(const regenny::LTObject* model, NodeControlFn fn, void* userdata) {
    if (fn == nullptr) {
        return false;
    }
    auto* iface = interfaces::ILTModel::get_client();
    const uintptr_t entry = slot(kSlotRemoveObject);
    if (iface == nullptr || entry == 0) {
        return false;
    }
    regenny::LTModelBlock120 block{};
    if (!read_block(model, &block, nullptr)) {
        return false;
    }
    return reinterpret_cast<RemoveObjectFn>(entry)(model, reinterpret_cast<void*>(fn), userdata) == kLtOk;
}

std::optional<size_t> NodeControl::registered_count(const regenny::LTObject* model, uint32_t node) {
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count) || node >= count) {
        return std::nullopt;
    }
    // The per-node head array, NAMED BY THE SCHEMA now that codegen is restored -- it used to be
    // reached as `unk_00` with the meaning recorded only in a comment.
    const auto head_array = reinterpret_cast<uintptr_t>(block.node_control_heads);
    if (head_array == 0) {
        return std::nullopt;
    }
    const auto head = mem::read<uintptr_t>(head_array + node * sizeof(uintptr_t));
    if (!head.has_value()) {
        return std::nullopt;
    }
    size_t n = 0;
    uintptr_t cur = *head;
    // Bounded: a corrupt chain must not spin the game thread. No live list is anywhere near
    // this long -- a model with 64 things driving one bone is already pathological.
    while (cur != 0 && n < 64) {
        NodeControlCell cell{};
        if (!mem::copy(&cell, cur, sizeof(cell))) {
            return std::nullopt;
        }
        ++n;
        cur = reinterpret_cast<uintptr_t>(cell.next);
    }
    return n;
}

std::optional<size_t> NodeControl::registered_total(const regenny::LTObject* model) {
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count)) {
        return std::nullopt;
    }
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto n = registered_count(model, i);
        if (!n.has_value()) {
            return std::nullopt;
        }
        total += *n;
    }
    return total;
}

std::optional<bool> NodeControl::is_registered(const regenny::LTObject* model, uint32_t node,
                                               NodeControlFn fn) {
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count) || node >= count) {
        return std::nullopt;
    }
    const auto head_array = reinterpret_cast<uintptr_t>(block.node_control_heads);
    if (head_array == 0) {
        return std::nullopt;
    }
    const auto head = mem::read<uintptr_t>(head_array + node * sizeof(uintptr_t));
    if (!head.has_value()) {
        return std::nullopt;
    }
    uintptr_t cur = *head;
    for (size_t guard = 0; cur != 0 && guard < 64; ++guard) {
        NodeControlCell cell{};
        if (!mem::copy(&cell, cur, sizeof(cell))) {
            return std::nullopt;
        }
        if (cell.fn == reinterpret_cast<void*>(fn)) {
            return true;
        }
        cur = reinterpret_cast<uintptr_t>(cell.next);
    }
    return false;
}

bool NodeControl::record_is_consistent(const regenny::LTObject* model, const NodeControlData* data) {
    if (data == nullptr) {
        return false;
    }
    regenny::LTModelBlock120 block{};
    uint32_t count = 0;
    if (!read_block(model, &block, &count) || data->node_index >= count) {
        return false;
    }
    const auto* base = block.node_transforms;
    if (base == nullptr || data->transform == nullptr) {
        return false;
    }
    // THE CLAIM, CHECKED: the record's writable transform is exactly this node's slot in the
    // block's own array. Both sides come from the schema -- `node_transforms` is a generated
    // field and the stride is sizeof(LTNodeTransform) -- so nothing here restates an offset.
    if (data->transform != base + data->node_index) {
        return false;
    }
    // And the parent pointer lands inside the same array, on a slot boundary. A parent index is
    // not otherwise recoverable here, so membership is the strongest available statement.
    if (data->parent_transform != nullptr) {
        const auto off = reinterpret_cast<uintptr_t>(data->parent_transform) -
                         reinterpret_cast<uintptr_t>(base);
        if (data->parent_transform < base ||
            off % sizeof(regenny::LTNodeTransform) != 0 ||
            off / sizeof(regenny::LTNodeTransform) >= count) {
            return false;
        }
    }
    return true;
}

} // namespace sdk

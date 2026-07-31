#pragma once

#include "regenny/regenny/LTNodeControlCell.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "regenny/regenny/LTNodeTransform.hpp"
#include "regenny/regenny/LTObject.hpp"

// ---- DRIVING A SKELETON NODE FROM A MOD --------------------------------------------------
//
// THE MECHANISM A VR MOD NEEDS FOR HANDS AND WEAPONS. The engine will call a function you
// register every time it evaluates a node's transform, and hand it a writable pointer to that
// node's transform. No engine code is hooked, and there is no per-frame race against the
// animation system: the callback runs INSIDE the evaluation, after the animation has produced
// the node's pose and before anything consumes it.
//
// `Model.hpp` documents the four ILTModel vtable slots and deliberately stopped short of
// wrapping them, on the grounds that registration lifetime is a consumer's problem. This class
// is that consumer surface: it supplies the typed call, the record layout and the list
// inspection, and it still does not own lifetime -- see the warning on `add`.
//
// ---- HOW THE ENGINE STORES IT -----------------------------------------------------------
//
// Registration is PER MODEL OBJECT, in the sub-object at LTObject + 0x120 (`LTModelBlock120`
// in fear2.genny). Both Add thunks reach it with a literal `add ecx, 120h`, which is what ties
// the API to that block rather than to the model as a whole. Inside it, field 0 is an array of
// per-node list heads -- one pointer per node, allocated as `4 * node_count` and zeroed by
// LTModelNodeBlock_Init -- and each list is a chain of 12-byte cells.
//
// So "is anything driving node N" is answerable by walking one list, which `registered_count`
// does and the diagnostics report.
//
// ---- THREADING, WHICH IS THE PART THAT BITES ---------------------------------------------
//
// The list is walked by LTModelObject_EvaluateSkeleton on the GAME thread, and Add/Remove
// unlink cells with no synchronisation of any kind. Registering from another thread races a
// walk that is dereferencing the same cells. Call these from `Mod::on_frame` (which runs on
// CClientShell::Update, the same thread that evaluates skeletons) and not from the IPC thread.
namespace sdk {

// The 12-byte cell the engine allocates per registration.
//
// THE GENERATED TYPE, not a copy of it. This was hand-declared for several sessions beside a
// static_assert, because codegen was believed broken; it was not (the crash needs a null `m_sdk`,
// i.e. an instance where no schema ever parsed -- see AGENT.MD 9a). The hand-written mirror is
// gone and the schema owns the layout, which is the rule: `reversing/fear2.genny` is ground truth
// for structure, and the SDK should not restate a field order the generator can emit.
using NodeControlCell = regenny::LTNodeControlCell;

// ---- THE RECORD THE CALLBACK RECEIVES ----------------------------------------------------
//
// Six dwords assembled on LTModelObject_EvaluateSkeleton's stack and passed by address. The
// field names below are claims, and two of them are CHECKABLE AT RUNTIME rather than taken
// from the decompiler: `transform` must equal the block's `node_transforms + node_index`, and
// `parent_transform` must land inside the same array. `NodeControl::record_is_consistent`
// performs exactly that test, and the fixture asserts it -- so a wrong reading here fails
// loudly instead of quietly handing a mod the wrong pointer to write.
struct NodeControlData {
    // A scratch pose the engine pre-sets to identity (rotation w = 1) before the walk. Offered
    // to the callback; what the engine does with it afterwards is NOT established, so treat it
    // as read-only until it is.
    regenny::LTNodeTransform* scratch;
    // The PARENT node's transform, in the same array as `transform`.
    regenny::LTNodeTransform* parent_transform;
    // The per-node info record (stride 0x40 in the asset's node table, + 0x24). Opaque here.
    void* node_info;
    // THIS NODE'S TRANSFORM -- the override point. Writing it changes where the bone ends up,
    // and therefore where anything socketed to it (a weapon, a hand, a camera) is placed.
    regenny::LTNodeTransform* transform;
    // The animation context the evaluation was invoked with. Opaque here; may be null.
    void* anim_context;
    // Which node this call is for. Indexes the same space as ModelSkeleton's node functions.
    uint32_t node_index;
};
static_assert(sizeof(NodeControlData) == 24, "six dwords, as assembled at 0x428A72..0x428A9A");

// `__cdecl` with two arguments, read off the call site at 0x428AA0: the engine pushes the
// cell's userdata, then the record's address, calls through the cell's first word, and cleans
// up with two pops.
using NodeControlFn = void(__cdecl*)(NodeControlData* data, void* userdata);

class NodeControl {
public:
    // Is the mechanism reachable at all? False when ILTModel.Client is unresolved (module not
    // yet loaded) or the vtable catalogue cannot supply the slots -- a state, not a fault.
    static bool available();

    // ---- REGISTRATION ------------------------------------------------------------------
    //
    // LIFETIME IS YOURS. The engine keeps the raw function pointer until something removes it,
    // and it will call that pointer on its own thread for as long as the model lives. A mod
    // that registers and then lets its image be unmapped has handed the engine a dangling
    // call. `Hooks::retire()` does NOT cover this -- it only knows about safetyhook -- so an
    // injected DLL MUST remove every registration before unload. See `BoneControl` for a
    // worked lifecycle.
    //
    // Call on the game thread (see the threading note above). Returns false when the object is
    // not a model, the node index is out of range, or the interface is unavailable.
    static bool add(const regenny::LTObject* model, uint32_t node, NodeControlFn fn, void* userdata);

    // Every node of the model. The engine implements this by looping the single-node path over
    // all `node_count` nodes, so removal must be symmetrical.
    static bool add_all(const regenny::LTObject* model, NodeControlFn fn, void* userdata);

    // Remove one registration.
    //
    // A userdata of NULL IS A WILDCARD, and that is the engine's behaviour, not a convenience
    // added here: LTModel_NodeControlList_Remove matches on
    // `fn == cell->fn && (!userdata || userdata == cell->userdata)`. A consumer that registers
    // several callbacks sharing one function pointer, distinguished only by userdata, cannot
    // remove them individually by passing null -- it will remove all of them.
    static bool remove(const regenny::LTObject* model, uint32_t node, NodeControlFn fn, void* userdata);
    static bool remove_all(const regenny::LTObject* model, NodeControlFn fn, void* userdata);

    // ---- INSPECTION ---------------------------------------------------------------------
    //
    // How many callbacks are currently driving this node. Walks the block's own list, so it
    // reports what the ENGINE will call rather than what this process believes it registered
    // -- which is the difference that catches a failed remove.
    //
    // nullopt when the model or its node block cannot be read; 0 is the ordinary answer.
    static std::optional<size_t> registered_count(const regenny::LTObject* model, uint32_t node);

    // Whether any node of the model has a registration. Cheap sweep for "did I leak one".
    static std::optional<size_t> registered_total(const regenny::LTObject* model);

    // Is a given function pointer registered on this node? Answers the question a teardown
    // path actually has -- "is MY callback still installed" -- without assuming the list holds
    // only ours.
    static std::optional<bool> is_registered(const regenny::LTObject* model, uint32_t node,
                                             NodeControlFn fn);

    // ---- VERIFYING THE RECORD -------------------------------------------------------------
    //
    // Does this record agree with the model's own node block? Checks that `transform` is
    // exactly `node_transforms + node_index` and that `parent_transform` lands inside the same
    // array, both derived from the generated schema rather than from any offset written here.
    //
    // This is what turns the field naming above from a decompiler reading into a measured
    // claim, and it can only be called from inside the callback -- the pointers are only
    // meaningful there.
    static bool record_is_consistent(const regenny::LTObject* model, const NodeControlData* data);

    // The slot numbers, exposed because a consumer verifying the mechanism wants to assert the
    // catalogue still resolves them rather than trusting a comment.
    static constexpr size_t kSlotAddObject = 23;
    static constexpr size_t kSlotAddNode = 24;
    static constexpr size_t kSlotRemoveObject = 25;
    static constexpr size_t kSlotRemoveNode = 26;
};

} // namespace sdk

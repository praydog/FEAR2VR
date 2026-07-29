#pragma once
#include "CClientMgrListLink.hpp"
#include "LTObjectType.hpp"
#include "LTRotation.hpp"
#include "LTVector.hpp"
#include "LTWorldTreeLink.hpp"
namespace regenny {
class LTObjectRef;
}
namespace regenny {
class LTSpatialRecord;
}
namespace regenny {
#pragma pack(push, 1)
class LTObject {
public:
    // Metadata: code
    void* vtable; // 0x0
    uint32_t unk_04; // 0x4
    regenny::LTObjectRef* shared_ref; // 0x8
    private: char pad_c[0x4]; public:
    regenny::LTObjectType type; // 0x10
    private: char pad_11[0x1]; public:
    uint16_t handle; // 0x12
    regenny::LTVector position; // 0x14
    regenny::LTRotation rotation; // 0x20
    float scale; // 0x30
    void* owner; // 0x34
    regenny::LTSpatialRecord* spatial_record; // 0x38
    uint32_t flags; // 0x3c
    uint32_t user_flags; // 0x40
    uint16_t flags2; // 0x44
    uint16_t flags3; // 0x46
    regenny::LTVector aabb_min; // 0x48
    regenny::LTVector aabb_max; // 0x54
    float radius; // 0x60
    regenny::LTVector dims; // 0x64
    float unk_70; // 0x70
    regenny::CClientMgrListLink child_list; // 0x74
    regenny::CClientMgrListLink parent_link; // 0x7c
    void* self; // 0x84
    regenny::LTObject* parent; // 0x88
    uint32_t attach_extra; // 0x8c
    private: char pad_90[0x18]; public:
    uint32_t slot_index; // 0xa8
    regenny::CClientMgrListLink list_link; // 0xac
    private: char pad_b4[0x4]; public:
    regenny::CClientMgrListLink owned_list; // 0xb8
    private: char pad_c0[0x4]; public:
    regenny::LTWorldTreeLink world_tree_link; // 0xc4
}; // Size: 0xcc
#pragma pack(pop)
}

#pragma once
#include "CClientMgrListLink.hpp"
#include "LTModelBlock120.hpp"
#include "LTModelBlock130.hpp"
#include "LTModelRecord.hpp"
#include "LTObject.hpp"
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
class StdString;
}
namespace regenny {
class LTMatrix3x4;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelObject {
public:
    regenny::LTObject base; // 0x0
    regenny::LTModelRecord record; // 0xcc
    regenny::CClientMgrListLink list_head; // 0x100
    uint32_t list_count; // 0x108
    uint32_t piece_hide_bits[2]; // 0x10c
    regenny::StdString* material_names; // 0x114
    uint32_t material_count; // 0x118
    uint32_t unk_11C; // 0x11c
    regenny::LTModelBlock120 block_120; // 0x120
    regenny::LTModelBlock130 block_130; // 0x130
    private: char pad_154[0x2]; public:
    uint16_t sphere_source; // 0x156
    private: char pad_158[0x8]; public:
    regenny::LTVector sphere_center; // 0x160
    float vis_radius; // 0x16c
    void* per_node_alloc; // 0x170
    regenny::LTMatrix3x4* node_matrices; // 0x174
    void* per_node_stride2_b; // 0x178
    private: char pad_17c[0xc]; public:
    regenny::LTRotation cached_rotation; // 0x188
}; // Size: 0x198
#pragma pack(pop)
}

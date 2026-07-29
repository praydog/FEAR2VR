#pragma once
#include "CClientMgrListLink.hpp"
namespace regenny {
class LTModelAsset;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelRecord {
public:
    regenny::CClientMgrListLink link; // 0x0
    void* self_ref; // 0x8
    private: char pad_c[0x10]; public:
    uint32_t unk_1C; // 0x1c
    regenny::LTModelAsset* asset; // 0x20
    uint16_t anim_index; // 0x24
    uint16_t node_a; // 0x26
    uint16_t anim_index_b; // 0x28
    uint16_t node_b; // 0x2a
    float anim_fraction; // 0x2c
    uint32_t unk_30; // 0x30
}; // Size: 0x34
#pragma pack(pop)
}

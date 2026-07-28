#pragma once
#include "LTWorldTreeLink.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTWorldTreeNode {
public:
    regenny::LTWorldTreeLink objects; // 0x0
    float split_x; // 0x8
    float split_z; // 0xc
    uint32_t occupied_count; // 0x10
    uint16_t parent_offset; // 0x14
    uint16_t child_offset; // 0x16
}; // Size: 0x18
#pragma pack(pop)
}

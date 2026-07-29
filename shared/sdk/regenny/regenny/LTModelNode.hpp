#pragma once
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTModelNode {
public:
    uint8_t parent_index; // 0x0
    uint8_t unk_01; // 0x1
    uint8_t own_index; // 0x2
    uint8_t first_child_offset; // 0x3
    uint8_t child_count; // 0x4
    uint8_t unk_05; // 0x5
    uint16_t unk_06; // 0x6
    regenny::LTVector bind_position; // 0x8
    regenny::LTRotation bind_rotation; // 0x14
    regenny::LTVector anim_fallback_position; // 0x24
    regenny::LTRotation anim_getter_rotation; // 0x30
}; // Size: 0x40
#pragma pack(pop)
}

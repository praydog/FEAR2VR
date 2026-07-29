#pragma once
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTAttachment {
public:
    regenny::LTVector offset_position; // 0x0
    regenny::LTRotation offset_rotation; // 0xc
    uint16_t child_handle; // 0x1c
    uint16_t parent_handle; // 0x1e
    uint32_t socket_handle; // 0x20
    regenny::LTAttachment* next; // 0x24
    uint32_t unk_28; // 0x28
    uint32_t unk_2C; // 0x2c
}; // Size: 0x30
#pragma pack(pop)
}

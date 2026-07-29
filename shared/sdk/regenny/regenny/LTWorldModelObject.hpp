#pragma once
#include "LTMatrix3x4.hpp"
#include "LTObject.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTWorldModelObject {
public:
    regenny::LTObject base; // 0x0
    void* unk_CC; // 0xcc
    uint16_t unk_D0; // 0xd0
    uint16_t unk_D2; // 0xd2
    uint32_t unk_D4; // 0xd4
    uint32_t unk_D8; // 0xd8
    regenny::LTMatrix3x4 world_transform; // 0xdc
    regenny::LTMatrix3x4 inverse_transform; // 0x10c
}; // Size: 0x13c
#pragma pack(pop)
}

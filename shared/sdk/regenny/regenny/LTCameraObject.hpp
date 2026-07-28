#pragma once
#include "LTMatrix3x4.hpp"
#include "LTObject.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTCameraObject {
public:
    regenny::LTObject base; // 0x0
    private: char pad_cc[0x10]; public:
    regenny::LTMatrix3x4 world_transform; // 0xdc
    regenny::LTMatrix3x4 inverse_transform; // 0x10c
    uint16_t unk_13C; // 0x13c
    private: char pad_13e[0x2]; public:
}; // Size: 0x140
#pragma pack(pop)
}

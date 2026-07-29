#pragma once
#include "LTObject.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTSpriteObject {
public:
    regenny::LTObject base; // 0x0
    float unk_CC; // 0xcc
    private: char pad_d0[0x18]; public:
    float angle_a; // 0xe8
    float angle_b; // 0xec
    float unk_F0; // 0xf0
    float unk_F4; // 0xf4
    float unk_F8; // 0xf8
    uint8_t none_bytes[6]; // 0xfc
    uint8_t zero_bytes[4]; // 0x102
    uint8_t kind; // 0x106
    private: char pad_107[0x1]; public:
    float unk_108; // 0x108
    float unk_10C; // 0x10c
    float unk_110; // 0x110
    regenny::LTVector unk_114; // 0x114
    regenny::LTVector aabb_min; // 0x120
    regenny::LTVector aabb_max; // 0x12c
    float radius; // 0x138
    void* owned[6]; // 0x13c
}; // Size: 0x154
#pragma pack(pop)
}

#pragma once
#include "LTObject.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTSpriteObject {
public:
    regenny::LTObject base; // 0x0
    private: char pad_cc[0x3a]; public:
    uint8_t kind; // 0x106
    private: char pad_107[0x19]; public:
    regenny::LTVector aabb_min; // 0x120
    regenny::LTVector aabb_max; // 0x12c
    float radius; // 0x138
    private: char pad_13c[0x18]; public:
}; // Size: 0x154
#pragma pack(pop)
}

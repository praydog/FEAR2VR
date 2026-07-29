#pragma once
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTVisPlane {
public:
    regenny::LTVector normal; // 0x0
    float distance; // 0xc
    uint32_t unk_10; // 0x10
}; // Size: 0x14
#pragma pack(pop)
}

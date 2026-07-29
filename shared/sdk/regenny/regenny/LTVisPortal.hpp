#pragma once
#include "LTVector.hpp"
namespace regenny {
class LTVisSector;
}
namespace regenny {
#pragma pack(push, 1)
class LTVisPortal {
public:
    regenny::LTVector plane_normal; // 0x0
    float plane_distance; // 0xc
    regenny::LTVector center; // 0x10
    float radius; // 0x1c
    regenny::LTVisSector* sector_a; // 0x20
    regenny::LTVisSector* sector_b; // 0x24
    uint32_t vertex_count; // 0x28
    regenny::LTVector vertices[4]; // 0x2c
}; // Size: 0x5c
#pragma pack(pop)
}

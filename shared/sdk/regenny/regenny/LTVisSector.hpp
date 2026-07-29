#pragma once
#include "LTVector.hpp"
namespace regenny {
class LTVisPlane;
}
namespace regenny {
class LTSpatialEntry;
}
namespace regenny {
#pragma pack(push, 1)
class LTVisSector {
public:
    regenny::LTSpatialEntry* entry_list; // 0x0
    regenny::LTVector aabb_min; // 0x4
    regenny::LTVector aabb_max; // 0x10
    float unk_1C; // 0x1c
    uint32_t unk_20; // 0x20
    uint8_t plane_count; // 0x24
    private: char pad_25[0x3]; public:
    regenny::LTVisPlane* planes; // 0x28
    void* unk_2C; // 0x2c
}; // Size: 0x30
#pragma pack(pop)
}

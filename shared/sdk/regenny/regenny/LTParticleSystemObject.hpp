#pragma once
#include "LTObject.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTParticleSystemObject {
public:
    regenny::LTObject base; // 0x0
    private: char pad_cc[0x11]; public:
    uint8_t cull_volume_type; // 0xdd
    private: char pad_de[0x2]; public:
    regenny::LTVector sphere_offset; // 0xe0
    float sphere_radius; // 0xec
    regenny::LTVector aabb_min_offset; // 0xf0
    regenny::LTVector aabb_max_offset; // 0xfc
}; // Size: 0x108
#pragma pack(pop)
}

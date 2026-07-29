#pragma once
#include "LTObject.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTParticleSystemObject {
public:
    regenny::LTObject base; // 0x0
    uint32_t unk_CC; // 0xcc
    uint32_t unk_D0; // 0xd0
    uint32_t unk_D4; // 0xd4
    uint32_t unk_D8; // 0xd8
    uint8_t unk_DC; // 0xdc
    uint8_t cull_volume_type; // 0xdd
    private: char pad_de[0x2]; public:
    regenny::LTVector sphere_offset; // 0xe0
    float sphere_radius; // 0xec
    regenny::LTVector aabb_min_offset; // 0xf0
    regenny::LTVector aabb_max_offset; // 0xfc
}; // Size: 0x108
#pragma pack(pop)
}

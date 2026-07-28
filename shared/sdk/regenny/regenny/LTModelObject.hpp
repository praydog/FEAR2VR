#pragma once
#include "LTObject.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTModelObject {
public:
    regenny::LTObject base; // 0x0
    private: char pad_cc[0x20]; public:
    void* unk_EC; // 0xec
    private: char pad_f0[0x66]; public:
    uint16_t sphere_source; // 0x156
    private: char pad_158[0x8]; public:
    regenny::LTVector sphere_center; // 0x160
    float vis_radius; // 0x16c
    private: char pad_170[0x28]; public:
}; // Size: 0x198
#pragma pack(pop)
}

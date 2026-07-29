#pragma once
#include "LTMatrix3x4.hpp"
#include "LTObject.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTWorldModelObject {
public:
    regenny::LTObject base; // 0x0
    void* unk_CC; // 0xcc
    private: char pad_d0[0xc]; public:
    regenny::LTMatrix3x4 world_transform; // 0xdc
    regenny::LTMatrix3x4 inverse_transform; // 0x10c
}; // Size: 0x13c
#pragma pack(pop)
}

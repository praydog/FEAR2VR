#pragma once
#include "LTWorldModelObject.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTCameraObject {
public:
    regenny::LTWorldModelObject base; // 0x0
    uint16_t unk_13C; // 0x13c
    private: char pad_13e[0x2]; public:
}; // Size: 0x140
#pragma pack(pop)
}

#pragma once
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTModelSocket {
public:
    regenny::LTVector position; // 0x0
    regenny::LTRotation rotation; // 0xc
    float unk_1C; // 0x1c
    char* name; // 0x20
    uint32_t node_index; // 0x24
}; // Size: 0x28
#pragma pack(pop)
}

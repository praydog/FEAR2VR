#pragma once
#include "CClientMgrListLink.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTConVar {
public:
    float value; // 0x0
    regenny::CClientMgrListLink link; // 0x4
    uint32_t name_hash; // 0xc
    char* name; // 0x10
    char* string_value; // 0x14
}; // Size: 0x18
#pragma pack(pop)
}

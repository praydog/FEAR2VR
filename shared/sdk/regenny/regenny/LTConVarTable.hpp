#pragma once
#include "CClientMgrListLink.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTConVarTable {
public:
    private: char pad_0[0x24]; public:
    regenny::CClientMgrListLink buckets[128]; // 0x24
}; // Size: 0x424
#pragma pack(pop)
}

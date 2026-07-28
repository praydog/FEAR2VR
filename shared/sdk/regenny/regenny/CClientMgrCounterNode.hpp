#pragma once
#include "CClientMgrListLink.hpp"
namespace regenny {
#pragma pack(push, 1)
class CClientMgrCounterNode {
public:
    private: char pad_0[0x14]; public:
    regenny::CClientMgrListLink self_link; // 0x14
    private: char pad_1c[0xc]; public:
    uint32_t unk_28; // 0x28
    float unk_2C; // 0x2c
    uint32_t elapsed_ms; // 0x30
    private: char pad_34[0x4]; public:
    double elapsed_time; // 0x38
}; // Size: 0x40
#pragma pack(pop)
}

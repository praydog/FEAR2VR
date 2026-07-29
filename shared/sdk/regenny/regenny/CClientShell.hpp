#pragma once
#include "CClientMgrListLink.hpp"
namespace regenny {
#pragma pack(push, 1)
class CClientShell {
public:
    void* vtable; // 0x0
    uint32_t unk_04; // 0x4
    double game_time; // 0x8
    double real_time; // 0x10
    float frame_interval; // 0x18
    void* unk_1C; // 0x1c
    void* subobject_20; // 0x20
    uint8_t local_client_ids[4]; // 0x24
    uint8_t unk_28; // 0x28
    private: char pad_29[0x3]; public:
    uint32_t unk_2C; // 0x2c
    uint32_t unk_30; // 0x30
    uint32_t unk_34; // 0x34
    uint32_t unk_38; // 0x38
    private: char pad_3c[0x14]; public:
    regenny::CClientMgrListLink list_50; // 0x50
    regenny::CClientMgrListLink list_58; // 0x58
    uint16_t handles_60[4]; // 0x60
    private: char pad_68[0x1]; public:
    uint8_t unk_69; // 0x69
    private: char pad_6a[0x6]; public:
}; // Size: 0x70
#pragma pack(pop)
}

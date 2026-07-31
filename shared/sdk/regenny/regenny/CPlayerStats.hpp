#pragma once
namespace regenny {
#pragma pack(push, 1)
class CPlayerStats {
public:
    private: char pad_0[0xe4]; public:
    int32_t health; // 0xe4
    int32_t armor; // 0xe8
    int32_t max_health; // 0xec
    int32_t max_armor; // 0xf0
    float air; // 0xf4
    int32_t* ammo_counts; // 0xf8
    private: char pad_fc[0x20]; public:
    int32_t health_lost; // 0x11c
}; // Size: 0x120
#pragma pack(pop)
}

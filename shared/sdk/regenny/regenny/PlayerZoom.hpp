#pragma once
#include "GameTimer.hpp"
namespace regenny {
#pragma pack(push, 1)
class PlayerZoom {
public:
    private: char pad_0[0x4]; public:
    void* owner; // 0x4
    private: char pad_8[0x18]; public:
    char delegates[24]; // 0x20
    private: char pad_38[0xa4]; public:
    char unmapped_dc[4]; // 0xdc
    uint32_t state; // 0xe0
    private: char pad_e4[0x4]; public:
    regenny::GameTimer transition_timer; // 0xe8
    private: char pad_108[0x14]; public:
    void* interp; // 0x11c
    uint32_t interp_arg; // 0x120
    char unmapped_124[40]; // 0x124
    private: char pad_14c[0x2]; public:
    uint8_t motion_blur; // 0x14e
    private: char pad_14f[0x1]; public:
    float scope_dof_blur; // 0x150
    private: char pad_154[0xc]; public:
    uint32_t ads_fov_committed; // 0x160
    uint32_t ads_fov; // 0x164
    private: char pad_168[0x8]; public:
    uint32_t zoom_sound; // 0x170
}; // Size: 0x174
#pragma pack(pop)
}

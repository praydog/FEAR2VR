#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTInputDeviceMouse {
public:
    void* vtable; // 0x0
    uint8_t buttons[3]; // 0x4
    uint8_t buttons_prev[3]; // 0x7
    uint8_t buttons_in[3]; // 0xa
    uint8_t pad_0D[3]; // 0xd
    float axis_in[2]; // 0x10
    float axis[2]; // 0x18
    float axis_prev[2]; // 0x20
    int32_t screen_x; // 0x28
    int32_t screen_y; // 0x2c
}; // Size: 0x30
#pragma pack(pop)
}

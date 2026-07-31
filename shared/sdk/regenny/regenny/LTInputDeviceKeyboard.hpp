#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTInputDeviceKeyboard {
public:
    void* vtable; // 0x0
    uint8_t current[256]; // 0x4
    uint8_t previous[256]; // 0x104
    uint8_t incoming[256]; // 0x204
}; // Size: 0x304
#pragma pack(pop)
}

#pragma once
namespace regenny {
#pragma pack(push, 1)
class StdString {
public:
    void* proxy; // 0x0
    uint8_t buf[16]; // 0x4
    uint32_t size; // 0x14
    uint32_t capacity; // 0x18
}; // Size: 0x1c
#pragma pack(pop)
}

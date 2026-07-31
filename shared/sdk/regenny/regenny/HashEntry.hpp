#pragma once
namespace regenny {
#pragma pack(push, 1)
class HashEntry {
public:
    uint32_t name_hash; // 0x0
    uint8_t type; // 0x4
    uint8_t num_values; // 0x5
    uint16_t value_index; // 0x6
}; // Size: 0x8
#pragma pack(pop)
}

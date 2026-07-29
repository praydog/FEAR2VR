#pragma once
namespace regenny {
class LTSpatialEntry;
}
namespace regenny {
#pragma pack(push, 1)
class LTSpatialRecord {
public:
    float volume[6]; // 0x0
    private: char pad_18[0x17]; public:
    uint8_t client_flags; // 0x2f
    uint16_t entry_count; // 0x30
    private: char pad_32[0x6]; public:
    void* object; // 0x38
    regenny::LTSpatialEntry* entry_list; // 0x3c
}; // Size: 0x40
#pragma pack(pop)
}

#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTSpatialRecord {
public:
    float volume[6]; // 0x0
    private: char pad_18[0x20]; public:
    void* object; // 0x38
}; // Size: 0x3c
#pragma pack(pop)
}

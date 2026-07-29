#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTModelAsset {
public:
    float radius; // 0x0
    private: char pad_4[0x44]; public:
    void* self_ref; // 0x48
    private: char pad_4c[0x4]; public:
    uint16_t unk_50; // 0x50
    uint16_t unk_52; // 0x52
    private: char pad_54[0x28]; public:
    uint32_t refcount; // 0x7c
    float radius_dup; // 0x80
    private: char pad_84[0x14]; public:
    char* filename; // 0x98
    char* filename_dup; // 0x9c
}; // Size: 0xa0
#pragma pack(pop)
}

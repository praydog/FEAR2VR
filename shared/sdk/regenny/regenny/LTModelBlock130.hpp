#pragma once
namespace regenny {
class LTModelAsset;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelBlock130 {
public:
    regenny::LTModelAsset* asset; // 0x0
    uint32_t unk_04; // 0x4
    uint32_t unk_08; // 0x8
    uint32_t unk_0C; // 0xc
    uint32_t unk_10; // 0x10
    uint32_t unk_14; // 0x14
    uint8_t unk_18; // 0x18
    uint8_t unk_19; // 0x19
    private: char pad_1a[0x2]; public:
    uint32_t unk_1C; // 0x1c
    float unk_20; // 0x20
}; // Size: 0x24
#pragma pack(pop)
}

#pragma once
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgrRecord {
public:
    strptr name; // 0x0
    uint32_t unk_04; // 0x4
    void* unk_08; // 0x8
    void* unk_0C; // 0xc
    void* owner_category; // 0x10
    uint32_t unk_14; // 0x14
}; // Size: 0x18
#pragma pack(pop)
}

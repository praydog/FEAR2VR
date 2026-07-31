#pragma once
namespace regenny {
class HashEntry;
}
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgrRecord {
public:
    strptr name; // 0x0
    uint32_t num_attributes; // 0x4
    regenny::HashEntry* attributes; // 0x8
    void* value_blob; // 0xc
    void* owner_category; // 0x10
    uint32_t name_hash; // 0x14
}; // Size: 0x18
#pragma pack(pop)
}

#pragma once
namespace regenny {
class DatabaseMgrRecord;
}
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgrCategory {
public:
    strptr name; // 0x0
    void* owner_database; // 0x4
    uint32_t num_records; // 0x8
    regenny::DatabaseMgrRecord* records; // 0xc
    uint32_t name_hash; // 0x10
}; // Size: 0x14
#pragma pack(pop)
}

#pragma once
namespace regenny {
class HashEntry;
}
namespace regenny {
class DatabaseMgrCategory;
}
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgrSubRecord {
public:
    strptr string_data; // 0x0
    regenny::HashEntry* hash_entries; // 0x4
    strptr path_data; // 0x8
    uint32_t num_categories; // 0xc
    uint32_t count_b; // 0x10
    regenny::DatabaseMgrCategory* categories; // 0x14
    uint32_t count_c; // 0x18
}; // Size: 0x1c
#pragma pack(pop)
}

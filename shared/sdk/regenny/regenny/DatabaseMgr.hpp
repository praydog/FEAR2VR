#pragma once
namespace regenny {
class DatabaseMgrVTable;
}
namespace regenny {
class DatabaseMgrEntry;
}
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgr {
public:
    regenny::DatabaseMgrVTable* vtable; // 0x0
    uint32_t unk_04; // 0x4
    regenny::DatabaseMgrEntry* array_begin; // 0x8
    regenny::DatabaseMgrEntry* array_end; // 0xc
    regenny::DatabaseMgrEntry* array_cap_end; // 0x10
    uint32_t unk_14; // 0x14
    private: char pad_18[0x8]; public:
}; // Size: 0x20
#pragma pack(pop)
}

#pragma once
namespace regenny {
class DatabaseMgrSubRecord;
}
namespace regenny {
#pragma pack(push, 1)
class DatabaseMgrEntry {
public:
    regenny::DatabaseMgrSubRecord* record_a; // 0x0
    regenny::DatabaseMgrSubRecord* record_b; // 0x4
}; // Size: 0x8
#pragma pack(pop)
}

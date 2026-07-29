#pragma once
namespace regenny {
class LTAnimRecordSlot;
}
namespace regenny {
#pragma pack(push, 1)
class LTAnimRecordTable {
public:
    void* proxy; // 0x0
    regenny::LTAnimRecordSlot* first; // 0x4
    regenny::LTAnimRecordSlot* last; // 0x8
    regenny::LTAnimRecordSlot* end_cap; // 0xc
}; // Size: 0x10
#pragma pack(pop)
}

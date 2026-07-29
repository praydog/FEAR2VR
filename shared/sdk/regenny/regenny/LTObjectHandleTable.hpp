#pragma once
namespace regenny {
class LTObjectHandleEntry;
}
namespace regenny {
#pragma pack(push, 1)
class LTObjectHandleTable {
public:
    void* proxy; // 0x0
    regenny::LTObjectHandleEntry* first; // 0x4
    regenny::LTObjectHandleEntry* last; // 0x8
    regenny::LTObjectHandleEntry* end_cap; // 0xc
}; // Size: 0x10
#pragma pack(pop)
}

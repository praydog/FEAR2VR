#pragma once
namespace regenny {
class LTAnimNameEntry;
}
namespace regenny {
#pragma pack(push, 1)
class LTAnimNameTable {
public:
    void* proxy; // 0x0
    regenny::LTAnimNameEntry* first; // 0x4
    regenny::LTAnimNameEntry* last; // 0x8
    regenny::LTAnimNameEntry* end_cap; // 0xc
}; // Size: 0x10
#pragma pack(pop)
}

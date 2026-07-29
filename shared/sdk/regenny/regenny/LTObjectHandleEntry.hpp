#pragma once
namespace regenny {
class LTObject;
}
namespace regenny {
#pragma pack(push, 1)
class LTObjectHandleEntry {
public:
    uint32_t tag; // 0x0
    regenny::LTObject* object; // 0x4
}; // Size: 0x8
#pragma pack(pop)
}

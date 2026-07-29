#pragma once
namespace regenny {
class LTAnimRecord;
}
namespace regenny {
#pragma pack(push, 1)
class LTAnimRecordSlot {
public:
    uint32_t unk_00; // 0x0
    regenny::LTAnimRecord* record; // 0x4
}; // Size: 0x8
#pragma pack(pop)
}

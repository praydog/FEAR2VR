#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTSpatialEntry {
public:
    void* record; // 0x0
    void** hit_head; // 0x4
    regenny::LTSpatialEntry* record_next; // 0x8
    regenny::LTSpatialEntry* hit_prev; // 0xc
    regenny::LTSpatialEntry* hit_next; // 0x10
}; // Size: 0x14
#pragma pack(pop)
}

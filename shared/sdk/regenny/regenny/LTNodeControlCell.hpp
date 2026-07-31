#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTNodeControlCell {
public:
    void* fn; // 0x0
    void* userdata; // 0x4
    regenny::LTNodeControlCell* next; // 0x8
}; // Size: 0xc
#pragma pack(pop)
}

#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTObjectRef {
public:
    regenny::LTObjectRef* link_prev; // 0x0
    regenny::LTObjectRef* link_next; // 0x4
    void* self_08; // 0x8
    void* unk_0C; // 0xc
    void* unk_10; // 0x10
    void* unk_14; // 0x14
    void* unk_18; // 0x18
    void* self_1C; // 0x1c
    uint32_t unk_20; // 0x20
    uint32_t refcount; // 0x24
}; // Size: 0x28
#pragma pack(pop)
}

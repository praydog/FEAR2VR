#pragma once
namespace regenny {
#pragma pack(push, 1)
class LTMemoryPool {
public:
    // Metadata: code
    void* vftable_a; // 0x0
    // Metadata: code
    void* vftable_b; // 0x4
    uint32_t unk_08; // 0x8
    uint32_t unk_0C; // 0xc
    private: char pad_10[0xc]; public:
    uint32_t unk_1C; // 0x1c
    uint32_t unk_20; // 0x20
    uint32_t unk_24; // 0x24
    uint32_t block_size; // 0x28
    uint32_t elems_per_chunk; // 0x2c
    void* chunk; // 0x30
    void* list_prev; // 0x34
    void* list_next; // 0x38
    regenny::LTMemoryPool* self; // 0x3c
}; // Size: 0x40
#pragma pack(pop)
}

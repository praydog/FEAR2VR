#pragma once
namespace regenny {
#pragma pack(push, 1)
struct CLTInput {
    void* vtable; // 0x0
    void* devices[6]; // 0x4
    uint8_t unmapped_1C[48]; // 0x1c
    uint32_t input_enabled; // 0x4c
    void* binding_list_unk50; // 0x50
    void* binding_list_begin; // 0x54
    void* binding_list_end; // 0x58
}; // Size: 0x5c
#pragma pack(pop)
}

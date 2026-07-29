#pragma once
#include "LTAnimNameTable.hpp"
#include "LTAnimRecordTable.hpp"
namespace regenny {
class LTModelSocket;
}
namespace regenny {
class LTModelNode;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelAsset {
public:
    float radius; // 0x0
    private: char pad_4[0x4]; public:
    uint32_t region_count_a; // 0x8
    private: char pad_c[0x4]; public:
    uint32_t region_count_b; // 0x10
    private: char pad_14[0x8]; public:
    regenny::LTModelNode* node_records; // 0x1c
    uint32_t node_count; // 0x20
    char** node_names; // 0x24
    uint32_t* node_hashes; // 0x28
    char** piece_names; // 0x2c
    uint32_t piece_count; // 0x30
    private: char pad_34[0x8]; public:
    void* container_3C; // 0x3c
    uint32_t node_count_dup; // 0x40
    private: char pad_44[0x4]; public:
    void* self_ref; // 0x48
    private: char pad_4c[0x4]; public:
    uint16_t unk_50; // 0x50
    uint16_t unk_52; // 0x52
    uint16_t material_count; // 0x54
    private: char pad_56[0x6]; public:
    regenny::LTAnimRecordTable anim_records; // 0x5c
    regenny::LTAnimNameTable anim_names; // 0x6c
    uint32_t refcount; // 0x7c
    float radius_from_file; // 0x80
    regenny::LTModelSocket* sockets; // 0x84
    uint32_t socket_count; // 0x88
    private: char pad_8c[0x4]; public:
    uint32_t string_blob_size; // 0x90
    private: char pad_94[0x4]; public:
    void* string_blob; // 0x98
    char* filename; // 0x9c
}; // Size: 0xa0
#pragma pack(pop)
}

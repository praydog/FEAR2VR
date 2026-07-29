#pragma once
namespace regenny {
class LTVisSector;
}
namespace regenny {
class LTVisTreeNode;
}
namespace regenny {
#pragma pack(push, 1)
class LTVisTree {
public:
    void* unk_00; // 0x0
    regenny::LTVisSector* sectors; // 0x4
    uint32_t sector_count; // 0x8
    void* unk_0C; // 0xc
    uint32_t unk_10; // 0x10
    regenny::LTVisTreeNode* root; // 0x14
    uint32_t node_count; // 0x18
    void* traversal_stack; // 0x1c
    void* unk_20; // 0x20
}; // Size: 0x24
#pragma pack(pop)
}

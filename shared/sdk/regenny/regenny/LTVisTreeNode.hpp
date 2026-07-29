#pragma once
namespace regenny {
class LTVisSector;
}
namespace regenny {
#pragma pack(push, 1)
class LTVisTreeNode {
public:
    regenny::LTVisSector** elements; // 0x0
    uint32_t element_count; // 0x4
    regenny::LTVisTreeNode* child_a; // 0x8
    regenny::LTVisTreeNode* child_b; // 0xc
    uint32_t split_axis; // 0x10
    float split_value; // 0x14
}; // Size: 0x18
#pragma pack(pop)
}

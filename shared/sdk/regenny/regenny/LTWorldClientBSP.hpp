#pragma once
#include "LTVector.hpp"
#include "LTVisTree.hpp"
namespace regenny {
class LTWorldTreeNode;
}
namespace regenny {
#pragma pack(push, 1)
class LTWorldClientBSP {
public:
    // Metadata: code
    void* vftable; // 0x0
    regenny::LTVector bounds_min; // 0x4
    regenny::LTVector bounds_max; // 0x10
    uint32_t world_tree_node_count; // 0x1c
    regenny::LTWorldTreeNode* world_tree_root; // 0x20
    regenny::LTVisTree vis_tree; // 0x24
    uint8_t world_attached; // 0x48
    uint8_t world_attached_2; // 0x49
    char world_path[44]; // 0x4a
    private: char pad_76[0xfa]; public:
}; // Size: 0x170
#pragma pack(pop)
}

#pragma once
namespace regenny {
class LTModelAsset;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelBlock120 {
public:
    void* unk_00; // 0x0
    void* per_node_stride2; // 0x4
    void* unk_08; // 0x8
    regenny::LTModelAsset* asset; // 0xc
}; // Size: 0x10
#pragma pack(pop)
}

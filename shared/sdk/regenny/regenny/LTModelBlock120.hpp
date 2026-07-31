#pragma once
namespace regenny {
class LTNodeControlCell;
}
namespace regenny {
class LTNodeTransform;
}
namespace regenny {
class LTModelAsset;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelBlock120 {
public:
    regenny::LTNodeControlCell** node_control_heads; // 0x0
    uint8_t* node_dirty_stride2; // 0x4
    regenny::LTNodeTransform* node_transforms; // 0x8
    regenny::LTModelAsset* asset; // 0xc
}; // Size: 0x10
#pragma pack(pop)
}

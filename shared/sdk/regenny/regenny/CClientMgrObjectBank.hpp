#pragma once
namespace regenny {
class LTMemoryPool;
}
namespace regenny {
#pragma pack(push, 1)
class CClientMgrObjectBank {
public:
    regenny::LTMemoryPool* pool; // 0x0
    uint32_t element_size; // 0x4
}; // Size: 0x8
#pragma pack(pop)
}

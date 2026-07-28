#pragma once
namespace regenny {
#pragma pack(push, 1)
class CClientMgrPtrVector {
public:
    uint32_t header; // 0x0
    void** begin; // 0x4
    void** end; // 0x8
}; // Size: 0xc
#pragma pack(pop)
}

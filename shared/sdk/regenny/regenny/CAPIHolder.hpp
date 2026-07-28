#pragma once
namespace regenny {
#pragma pack(push, 1)
class CAPIHolder {
public:
    // Metadata: code
    void* vftable; // 0x0
    strptr api_name; // 0x4
    void** output_slot; // 0x8
}; // Size: 0xc
#pragma pack(pop)
}

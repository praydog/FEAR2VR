#pragma once
namespace regenny {
#pragma pack(push, 1)
class CClientMgr {
public:
    private: char pad_0[0x1434]; public:
    void* client_shell; // 0x1434
    private: char pad_1438[0x4]; public:
    bool updating; // 0x143c
    private: char pad_143d[0x3]; public:
}; // Size: 0x1440
#pragma pack(pop)
}

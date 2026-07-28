#pragma once
namespace regenny {
#pragma pack(push, 1)
class CClientMgrListLink {
public:
    regenny::CClientMgrListLink* prev; // 0x0
    regenny::CClientMgrListLink* next; // 0x4
}; // Size: 0x8
#pragma pack(pop)
}

#pragma once
#include "CClientMgrListLink.hpp"
#include "CClientMgrPtrVector.hpp"
namespace regenny {
class CClientMgrCounterNode;
}
namespace regenny {
#pragma pack(push, 1)
class CClientMgr {
public:
    regenny::CClientMgrListLink object_lists[7]; // 0x0
    private: char pad_38[0x30]; public:
    regenny::CClientMgrPtrVector unk_68; // 0x68
    private: char pad_74[0x470]; public:
    void* unk_4E4; // 0x4e4
    private: char pad_4e8[0xf04]; public:
    regenny::CClientMgrListLink counter_list_head; // 0x13ec
    regenny::CClientMgrCounterNode* own_counter_node; // 0x13f4
    private: char pad_13f8[0x38]; public:
    uint16_t last_sent_bandwidth; // 0x1430
    private: char pad_1432[0x2]; public:
    void* client_shell; // 0x1434
    private: char pad_1438[0x4]; public:
    bool updating; // 0x143c
    private: char pad_143d[0x23]; public:
    regenny::CClientMgrListLink start_shell_list; // 0x1460
}; // Size: 0x1468
#pragma pack(pop)
}

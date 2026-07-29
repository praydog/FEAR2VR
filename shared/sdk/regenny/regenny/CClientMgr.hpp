#pragma once
#include "CClientMgrListLink.hpp"
#include "CClientMgrObjectBank.hpp"
#include "CClientMgrPtrVector.hpp"
#include "LTObjectHandleTable.hpp"
namespace regenny {
class CClientShell;
}
namespace regenny {
class CClientMgrCounterNode;
}
namespace regenny {
#pragma pack(push, 1)
class CClientMgr {
public:
    regenny::CClientMgrListLink object_lists[7]; // 0x0
    regenny::CClientMgrObjectBank object_banks[6]; // 0x38
    regenny::CClientMgrPtrVector unk_68; // 0x68
    private: char pad_74[0x470]; public:
    void* unk_4E4; // 0x4e4
    private: char pad_4e8[0xf04]; public:
    regenny::CClientMgrListLink counter_list_head; // 0x13ec
    regenny::CClientMgrCounterNode* own_counter_node; // 0x13f4
    private: char pad_13f8[0x8]; public:
    uint32_t last_sample_time_ms; // 0x1400
    private: char pad_1404[0x10]; public:
    regenny::LTObjectHandleTable handle_table; // 0x1414
    float unk_1424; // 0x1424
    private: char pad_1428[0x8]; public:
    uint16_t last_sent_bandwidth; // 0x1430
    private: char pad_1432[0x2]; public:
    regenny::CClientShell* client_shell; // 0x1434
    void* pending_shell_release; // 0x1438
    bool updating; // 0x143c
    private: char pad_143d[0x23]; public:
    regenny::CClientMgrListLink start_shell_list; // 0x1460
}; // Size: 0x1468
#pragma pack(pop)
}

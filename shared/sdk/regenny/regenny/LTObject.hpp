#pragma once
#include "CClientMgrListLink.hpp"
#include "LTObjectType.hpp"
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTObject {
public:
    // Metadata: code
    void* vtable; // 0x0
    private: char pad_4[0xc]; public:
    regenny::LTObjectType type; // 0x10
    private: char pad_11[0x1]; public:
    uint16_t handle; // 0x12
    regenny::LTVector position; // 0x14
    regenny::LTRotation rotation; // 0x20
    private: char pad_30[0x8]; public:
    void* unk_38; // 0x38
    private: char pad_3c[0x8]; public:
    uint16_t flags; // 0x44
    private: char pad_46[0x66]; public:
    regenny::CClientMgrListLink list_link; // 0xac
    private: char pad_b4[0x18]; public:
}; // Size: 0xcc
#pragma pack(pop)
}

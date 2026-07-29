#pragma once
#include "CClientMgrListLink.hpp"
#include "LTObject.hpp"
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
class LTModelAsset;
}
namespace regenny {
#pragma pack(push, 1)
class LTModelObject {
public:
    regenny::LTObject base; // 0x0
    regenny::CClientMgrListLink embedded_link; // 0xcc
    private: char pad_d4[0x18]; public:
    regenny::LTModelAsset* asset; // 0xec
    private: char pad_f0[0x10]; public:
    regenny::CClientMgrListLink list_head; // 0x100
    uint32_t list_count; // 0x108
    private: char pad_10c[0x14]; public:
    void* owned_120; // 0x120
    private: char pad_124[0xc]; public:
    regenny::LTModelAsset* asset_dup; // 0x130
    private: char pad_134[0x22]; public:
    uint16_t sphere_source; // 0x156
    private: char pad_158[0x8]; public:
    regenny::LTVector sphere_center; // 0x160
    float vis_radius; // 0x16c
    private: char pad_170[0x18]; public:
    regenny::LTRotation cached_rotation; // 0x188
}; // Size: 0x198
#pragma pack(pop)
}

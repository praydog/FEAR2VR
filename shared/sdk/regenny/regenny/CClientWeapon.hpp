#pragma once
namespace regenny {
class LTObject;
}
namespace regenny {
#pragma pack(push, 1)
class CClientWeapon {
public:
    private: char pad_0[0x38]; public:
    regenny::LTObject* model_object; // 0x38
    private: char pad_3c[0x4]; public:
}; // Size: 0x40
#pragma pack(pop)
}

#pragma once
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
#pragma pack(push, 1)
class LTNodeTransform {
public:
    regenny::LTVector position; // 0x0
    regenny::LTRotation rotation; // 0xc
}; // Size: 0x1c
#pragma pack(pop)
}

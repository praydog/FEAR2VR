#pragma once
namespace regenny {
#pragma pack(push, 1)
class GameTimer {
public:
    double start; // 0x0
    double duration; // 0x8
    double cached_now; // 0x10
    uint8_t use_cached; // 0x18
    uint8_t active; // 0x19
    private: char pad_1a[0x6]; public:
}; // Size: 0x20
#pragma pack(pop)
}

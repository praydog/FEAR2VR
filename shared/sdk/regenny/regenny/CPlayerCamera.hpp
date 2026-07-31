#pragma once
#include "LTRotation.hpp"
#include "LTVector.hpp"
namespace regenny {
class LTObject;
}
namespace regenny {
class LTModelObject;
}
namespace regenny {
#pragma pack(push, 1)
class CPlayerCamera {
public:
    // Metadata: code
    void* vtable; // 0x0
    void* owner; // 0x4
    private: char pad_8[0x8]; public:
    // Metadata: code
    void* vtable_secondary_10; // 0x10
    private: char pad_14[0x10]; public:
    // Metadata: code
    void* vtable_secondary_24; // 0x24
    private: char pad_28[0x94]; public:
    regenny::LTObject* camera_object; // 0xbc
    private: char pad_c0[0x4]; public:
    int32_t viewport_rect; // 0xc4
    private: char pad_c8[0x20]; public:
    regenny::LTVector applied_position; // 0xe8
    regenny::LTRotation applied_rotation; // 0xf4
    private: char pad_104[0x20]; public:
    float fov_x; // 0x124
    float fov_y; // 0x128
    regenny::LTVector position; // 0x12c
    regenny::LTVector cinematic_position; // 0x138
    regenny::LTRotation rotation; // 0x144
    private: char pad_154[0xc4]; public:
    uint8_t use_smoothed_x; // 0x218
    uint8_t use_smoothed_y; // 0x219
    uint8_t use_smoothed_z; // 0x21a
    private: char pad_21b[0x1]; public:
    regenny::LTVector smoothed_position; // 0x21c
    regenny::LTRotation attachment_rotation; // 0x228
    regenny::LTRotation base_rotation; // 0x238
    private: char pad_248[0x10]; public:
    regenny::LTModelObject* model_object; // 0x258
    private: char pad_25c[0xc]; public:
    void* alternate_source; // 0x268
    private: char pad_26c[0x44]; public:
    uint32_t state; // 0x2b0
    private: char pad_2b4[0x10]; public:
    void* collision_proxy; // 0x2c4
    private: char pad_2c8[0x4]; public:
    uint32_t anim_source; // 0x2cc
    uint8_t clamp_flag; // 0x2d0
    private: char pad_2d1[0x1b]; public:
    uint8_t has_previous_height; // 0x2ec
    private: char pad_2ed[0x3]; public:
    float previous_height; // 0x2f0
    private: char pad_2f4[0xf0]; public:
    float smoothing_delta; // 0x3e4
    float carry_accumulator; // 0x3e8
    private: char pad_3ec[0x1]; public:
    uint8_t aim_collision_latched; // 0x3ed
    uint8_t cinematic_active; // 0x3ee
    private: char pad_3ef[0x1]; public:
    regenny::LTVector aim_collision_position; // 0x3f0
    private: char pad_3fc[0x14c4]; public:
    float saved_near_z; // 0x18c0
    private: char pad_18c4[0x2]; public:
}; // Size: 0x18c6
#pragma pack(pop)
}

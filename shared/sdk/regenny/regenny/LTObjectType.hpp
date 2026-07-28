#pragma once
namespace regenny {
#pragma pack(push, 1)
enum LTObjectType : uint8_t {
    OT_NORMAL = 0,
    OT_MODEL = 1,
    OT_WORLDMODEL = 2,
    OT_SPRITE = 3,
    OT_LIGHT = 4,
    OT_CAMERA = 5,
    OT_PARTICLESYSTEM = 6,
};
#pragma pack(pop)
}

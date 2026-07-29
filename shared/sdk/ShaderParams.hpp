#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <d3d9.h>

// The engine's NAMED SHADER PARAMETERS.
//
// FEAR 2 does not scatter shader constants through its render code. It keeps a
// doubly-linked list of 60 named parameter records in the exe's static data, each holding a
// name, a type, a shader register, a byte size and its current value inline. The engine sets
// a value through LTShaderParam_SetValue (0x60C5B9) and flushes it through
// LTShaderParam_Flush (0x60C20A), which either uploads it -- via
// LTShader_SetConstantByRegister (0x618D4E) -- or, when the parameter has no register yet,
// records the byte count as pending and uploads later.
//
// HOW THE LIST WAS FOUND, because it matters for trusting the layout: not by scanning for
// this shape, but by following one function. Render_ClearRenderTargetTexture calls
// RenderTarget_GetD3DTexture, and a neighbouring function (sub_61312E) passes three such
// textures plus a float2 to LTShaderParam_Flush. The float2 turned out to be
// k_vScene_ScreenRes, whose inline value read 5120.0f, 1440.0f -- the resolution the game is
// actually running. A record whose value matches the live display is not a guessed layout.
//
// THE RECORD, offsets confirmed against LTShaderParam_Flush's own field accesses:
//
//     +0x00  vtable            (g_LTShaderParam_vftable, shared by all 60)
//     +0x04  const char* name
//     +0x08  u16 type          (ShaderParamType)
//     +0x0A  u16 binding index (kUnboundBinding when the engine has not assigned one)
//     +0x0C  void* value       -- always node + 0x1C
//     +0x10  u16 size          in bytes
//     +0x12  u16 pending       nonzero when set while unbound; Flush clears it on upload
//     +0x14  prev, +0x18 next
//     +0x1C  value storage     (inline, `size` bytes)
//
// TWO INDEPENDENT SIGNALS AGREE ON EVERY TYPE. The u16 at +0x08 is one; the engine's own
// Hungarian prefix on the name is the other -- `t` texture, `k_f` float, `k_v` vector, `k_m`
// matrix. All 60 records agree, so the type enum is read off the binary twice rather than
// once.
//
// READ-ONLY ON PURPOSE. Writing a value here is NOT the same as changing what a shader sees:
// a parameter like k_mObjectToClip is the composed object-to-clip transform and is set per
// draw, so a write would be overwritten before it reached a shader, and the upload path only
// runs for parameters that already hold a register. Establishing when each value is
// refreshed needs the render loop observed with a level loaded; until then this class reads
// and reports, and deliberately offers no setter.
//
// WHAT IS UNSETTLED, recorded so nobody assumes otherwise:
//   - The interface behind the upload. LTShader_SetConstantByRegister calls method index 20
//     on an object at g_pLTShaderSystem+0x10 with (handle, void*, byteCount). That shape
//     resembles ID3DXEffect::SetValue, but the pointer is null at the main menu, so the
//     interface could not be identified live. It is NOT confirmed as D3DX.
//   - Whether a register, once assigned, is stable across level loads.
namespace sdk {

enum class ShaderParamType : uint16_t {
    Texture = 0x0001,    // 4 bytes, an IDirect3DBaseTexture9*
    Float3 = 0x0002,     // 12
    Float4 = 0x0003,     // 16, or a multiple for arrays (k_vTranslucentLightCube is 96)
    Float = 0x0005,      // 4
    Matrix4x4 = 0x0008,  // 64
    Matrix4x3 = 0x0009,  // 48, or a multiple (k_mModelObjectNodes is 1152 = 24 of them)
    Float2 = 0x000A,     // 8
};

// One parameter record, already read out of the process. Copying it means callers can hold
// the values without re-reading memory that may fault.
struct ShaderParam {
    uintptr_t address{};        // the record itself
    std::string name;
    ShaderParamType type{};
    uint16_t binding_index{};   // kUnboundBinding when the engine has not assigned one
    uint16_t size{};            // value size in bytes, per the record
    uint16_t pending{};         // nonzero when a value was set while unbound
    uintptr_t value_address{};  // where the value lives (record + 0x1C)

    bool bound() const;
    bool has_pending_upload() const;

    // Does the size agree with the type? An array parameter is a whole multiple of its
    // element size, so this accepts those too, and rejects a record whose size cannot be
    // built out of its declared type at all.
    bool size_agrees_with_type() const;

    // How many elements the size implies (1 for the scalar cases, 24 for the bone array).
    // 0 when the size does not divide by the element size.
    size_t element_count() const;
};

class ShaderParams {
public:
    // The value meaning "the engine has not bound this parameter yet". Flush treats it as
    // "record the byte count and upload later".
    //
    // NOT a D3D constant register, though it sits where one would: Flush passes it to
    // LTShader_SetConstantByRegister, which uses it to index a 12-byte-stride table at
    // g_pLTShaderSystem+0xBC and hands the entry's +0x08 field to the upload method. So it
    // selects an ENGINE handle, and a consumer must not treat it as a shader register.
    static constexpr uint16_t kUnboundBinding = 0xFFFF;

    // Byte size of one element of a type; 0 for a type this build does not know.
    static size_t element_size(ShaderParamType type);

    // The engine's own name for a type, for logs and UI.
    static const char* type_name(ShaderParamType type);

    // Address of the list head record (g_ShaderParamList_Head), or 0 when the exe is not
    // mapped. The head is static data, so this is available before the engine initialises.
    static uintptr_t list_head_address();

    // ---- THE READS, EACH FAULT-GUARDED ----------------------------------------------
    //
    // These are the helpers a consumer wants, and the reason they are here rather than in a
    // test: every one of them touches engine memory that can be absent or torn down, so the
    // structured-exception handling belongs with the accessor, once, not copied into each
    // caller.

    // Read a single record. nullopt when the address does not read, or when the record does
    // not carry the shared vtable -- which is the check that stops a walk running off the
    // end of the list into unrelated data.
    static std::optional<ShaderParam> read_record(uintptr_t address);

    // Walk the list from the head. Stops on a bad record, a cycle, or `limit` records, so a
    // corrupt link cannot hang the caller. Empty when the head does not read.
    static std::vector<ShaderParam> all(size_t limit = 256);

    // Find one parameter by its exact engine name, e.g. "k_mObjectToClip".
    static std::optional<ShaderParam> find(std::string_view name);

    // Copy a parameter's raw value. Fails when `bytes` exceeds what the record declares, so
    // a caller cannot read past the value into the next record.
    static bool read_value(const ShaderParam& param, void* out, size_t bytes);

    // ---- TYPED CONVENIENCES ---------------------------------------------------------
    //
    // Each checks the declared type AND requires the record's size to equal exactly what it
    // returns. That second check is what stops matrix4x3("k_mModelObjectNodes") quietly
    // handing back the first of 24 matrices: an array parameter has the right type but the
    // wrong size, so it is refused here and served by the array accessor below.

    static std::optional<std::array<float, 16>> matrix4x4(std::string_view name);
    static std::optional<std::array<float, 12>> matrix4x3(std::string_view name);
    static std::optional<std::array<float, 4>> float4(std::string_view name);
    static std::optional<std::array<float, 3>> float3(std::string_view name);
    static std::optional<std::array<float, 2>> float2(std::string_view name);
    static std::optional<float> scalar(std::string_view name);

    // Every element of an array parameter, e.g. the 24 node transforms of
    // "k_mModelObjectNodes". Empty when the parameter is missing, is not a float4x3, or its
    // size is not a whole multiple of one matrix.
    static std::vector<std::array<float, 12>> matrix4x3_array(std::string_view name);

    // The texture currently bound to a texture parameter, e.g. "tDepthMap". A typed pointer
    // rather than an address, because a consumer wants to call the interface. nullptr when
    // the parameter holds none; nullopt when it is missing or not a texture.
    static std::optional<IDirect3DBaseTexture9*> texture(std::string_view name);

    // k_vScene_ScreenRes, the resolution the engine reports to its shaders. Distinct from
    // Render::present_params(), which is what the device was created with -- reading both
    // and comparing is a genuine consistency check rather than a duplicate.
    static std::optional<std::array<float, 2>> screen_resolution();

    // ---- THE CAMERA PARAMETERS A STEREO PATH CARES ABOUT ----------------------------

    // k_vHalfViewPlane, unpacked. The engine stores the view plane's half-extents together
    // with their reciprocals, so a shader can divide without a divide.
    //
    // UNITS FOLLOW THE RENDER PASS, which is how the meaning was established rather than
    // assumed. Read live during the engine's screen pass (mode 2) the extents are
    // (2560, 720) -- exactly half of a 5120x1440 screen, so pixels. Left by a 3D pass they
    // read (2.2651, 0.6371), whose ratio matches that screen's aspect, i.e. half-extents at
    // unit depth. So this is "half extent of the view plane" in whatever space the pass
    // works in, and a consumer must know which pass produced what it is holding.
    struct HalfViewPlane {
        float half_width{};
        float half_height{};
        float inv_half_width{};
        float inv_half_height{};

        // Do the stored reciprocals actually match the extents? An invariant of the tuple
        // rather than a property of this machine, so it is a real check: it holds at any
        // resolution and in either pass, and fails if the four floats were misread.
        bool reciprocals_consistent(float tolerance = 1e-3f) const;

        // half_width / half_height. Equals the viewport aspect ratio in both passes
        // observed, so it is the cheapest cross-check against screen_resolution().
        float aspect() const;

        // NO FOV CONVERSION IS OFFERED, deliberately. For a perspective pass the half-extent
        // at unit depth is tan(fov/2), so 2*atan(half_height) would be the vertical field of
        // view -- but this class cannot tell a caller WHICH pass left the value it just read,
        // and during the engine's screen pass the same parameter holds pixel extents. On
        // those, 2*atan(2560) is about pi radians: a plausible-looking number that means
        // nothing. A consumer that has independently established it is looking at a
        // perspective pass can do the arithmetic itself; an accessor here would only hide
        // the precondition it cannot check.
    };

    static std::optional<HalfViewPlane> half_view_plane();

    // k_vScene_ZRange as the engine last published it: (near, far). Live in a loaded level
    // this reads (4.3, 100000). NOTE it is not refreshed by the screen pass, so it can be
    // stale with respect to the current frame -- see SceneRenderer_SetupCameraShaderParams,
    // which skips this write when the pass mode is 2.
    static std::optional<std::array<float, 2>> z_range();

    // k_vWorldSpaceCameraDir, the camera's forward vector in world space.
    static std::optional<std::array<float, 3>> world_space_camera_dir();

    // ---- IS ANY OF THIS CURRENT? -----------------------------------------------------
    //
    // k_fTime, which SceneRenderer_BeginFrame publishes ONCE PER FRAME as fmod(frameTime, K). That makes
    // it the engine's own liveness signal, and reading it is the only way to know whether anything else
    // here is current.
    //
    // WHY THIS MATTERS MORE THAN IT LOOKS. When the render path is not running -- the window unfocused, a
    // pause, a level transition -- every value in this registry and in sdk::SceneCamera FREEZES at
    // whatever the last executed pass left. Those values stay entirely plausible: a real viewport, a real
    // projection, matrices that satisfy every coherence check in this SDK, because they were produced by
    // real passes. They are simply no longer current. Nothing else exposed here can tell the difference.
    //
    // A single read cannot detect it either. Sample this twice with real time in between: unchanged means
    // the render path has not run and every other reading is a snapshot of the past.
    static std::optional<float> frame_time();
};

}  // namespace sdk

#pragma once

// RmlUi's RenderInterface, on D3D11. Everything RmlUi draws -- element backgrounds, borders, text,
// scrollbars -- arrives here as compiled vertex/index buffers with a premultiplied-alpha colour per
// vertex (Rml::Vertex::colour is ColourbPremultiplied), so the blend state below is fixed to match:
// SRC=ONE, DST=INV_SRC_ALPHA, the same convention the host already uses for the mod's HUD quad (see
// the "PREMULTIPLIED, NOT UNPREMULTIPLIED" comment in main.cpp). One instance renders one Rml
// context into whatever render target BeginFrame() is given each frame; it owns no swapchain of its
// own.

#include <d3d11.h>

#include <cstdint>

#include <RmlUi/Core/RenderInterface.h>

namespace xrui {

class UiRenderInterfaceD3D11 final : public Rml::RenderInterface {
public:
    UiRenderInterfaceD3D11();
    ~UiRenderInterfaceD3D11() override;

    // Creates the shaders, states and the 1x1 white texture used for untextured geometry. Returns
    // false (and logs) on any D3D11 failure; the caller should treat that as "no settings UI this
    // session" rather than crash the host over a cosmetic feature.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    // Binds `target` and sets up an orthographic projection covering [0,width) x [0,height) in
    // pixels, y-down to match RmlUi's window-coordinate convention (ProcessMouseMove's doc: "0
    // should be the top of the client area"). Clears to transparent black -- premultiplied
    // transparent is (0,0,0,0), the only colour that composites as "nothing here" regardless of
    // blend mode.
    void beginFrame(ID3D11RenderTargetView* target, uint32_t width, uint32_t height);
    void endFrame();

    // Rml::RenderInterface -- required.
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    // Rml::RenderInterface -- optional; only the transform hook is implemented. Layers/filters/
    // shaders (opacity stacking contexts, CSS filters, custom shaders) fall back to the base
    // class's no-op defaults -- the settings panel uses none of those, so there is nothing for them
    // to do, and implementing them for a document that never triggers them would be untested code.
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    Rml::TextureHandle createTextureFromRgba(const Rml::byte* rgba_premultiplied, int width, int height);
    void updateTransformConstant();

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    ID3D11VertexShader* m_vertex_shader = nullptr;
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11InputLayout* m_input_layout = nullptr;
    ID3D11Buffer* m_transform_cb = nullptr;
    ID3D11BlendState* m_blend_state = nullptr;
    ID3D11RasterizerState* m_rasterizer_state = nullptr;
    ID3D11DepthStencilState* m_depth_state = nullptr;
    ID3D11SamplerState* m_sampler_state = nullptr;

    Rml::TextureHandle m_white_texture = 0;

    // The combined projection*transform, recomputed whenever either input changes (beginFrame or
    // SetTransform) rather than every draw call -- translation is the only thing RenderGeometry
    // varies per call, and it travels through the constant buffer separately.
    Rml::Matrix4f m_projection;
    Rml::Matrix4f m_element_transform;
    Rml::Matrix4f m_mvp;
    bool m_scissor_enabled = false;
    uint32_t m_target_width = 0;
    uint32_t m_target_height = 0;
};

} // namespace xrui

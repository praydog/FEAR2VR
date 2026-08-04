#include "UiRenderInterfaceD3D11.hpp"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace xrui {

namespace {

// One draw's worth of state: everything CompileGeometry needs to keep alive until ReleaseGeometry,
// heap-allocated because Rml::CompiledGeometryHandle is an opaque uintptr_t and this is the
// simplest thing to put behind it.
struct Geometry {
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11Buffer* index_buffer = nullptr;
    UINT index_count = 0;
};

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
};

// Vertex/pixel shaders, kept as source rather than precompiled bytecode: this is a settings panel
// compiled once per process launch, not a hot path, and source is what the next person can actually
// read. `column_major` matches Rml::Matrix4f's default storage (see Matrix4.h -- ColumnMajorStorage
// packs each Rml column as 4 consecutive floats), so the raw bytes from Matrix4f::data() can be
// uploaded to this constant buffer with no transpose.
const char* const kShaderSource = R"(
cbuffer Transform : register(b0) {
    column_major float4x4 uMvp;
    float2 uTranslation;
    float2 uPad;
};

Texture2D uTexture : register(t0);
SamplerState uSampler : register(s0);

struct VSInput {
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 pos = float4(input.position + uTranslation, 0.0, 1.0);
    output.position = mul(uMvp, pos);
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // Both operands are premultiplied alpha (Rml::Vertex::colour and the textures this interface
    // generates -- see GenerateTexture's doc). A plain component-wise multiply is the correct way
    // to tint a premultiplied glyph/image by a premultiplied vertex colour; see the file header.
    float4 tex = uTexture.Sample(uSampler, input.texcoord);
    return input.color * tex;
}
)";

bool compile(const char* entry, const char* target, ID3DBlob** out_blob) {
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "xrui_shader", nullptr, nullptr, entry,
                                  target, 0, 0, out_blob, &errors);
    if (FAILED(hr)) {
        std::printf("[host] [ui] shader compile failed (%s/%s): %s\n", entry, target,
                    errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostic");
        if (errors != nullptr) {
            errors->Release();
        }
        return false;
    }
    if (errors != nullptr) {
        errors->Release();
    }
    return true;
}

} // namespace

UiRenderInterfaceD3D11::UiRenderInterfaceD3D11() = default;
UiRenderInterfaceD3D11::~UiRenderInterfaceD3D11() = default;

bool UiRenderInterfaceD3D11::init(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    if (!compile("VSMain", "vs_5_0", &vs_blob) || !compile("PSMain", "ps_5_0", &ps_blob)) {
        return false;
    }

    HRESULT hr = m_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
                                              &m_vertex_shader);
    if (FAILED(hr)) {
        std::printf("[host] [ui] CreateVertexShader failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        vs_blob->Release();
        ps_blob->Release();
        return false;
    }

    hr = m_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixel_shader);
    if (FAILED(hr)) {
        std::printf("[host] [ui] CreatePixelShader failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        vs_blob->Release();
        ps_blob->Release();
        return false;
    }

    // Matches Rml::Vertex exactly: Vector2f position, ColourbPremultiplied colour (4 bytes,
    // red/green/blue/alpha in that member order -- see Colour.h), Vector2f tex_coord. 20 bytes.
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = m_device->CreateInputLayout(layout, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_input_layout);
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr)) {
        std::printf("[host] [ui] CreateInputLayout failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = 80; // 64 (mvp) + 8 (translation) + 8 (pad), 16-byte aligned
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cb_desc, nullptr, &m_transform_cb);
    if (FAILED(hr)) {
        std::printf("[host] [ui] transform cbuffer create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = m_device->CreateBlendState(&blend_desc, &m_blend_state);
    if (FAILED(hr)) {
        std::printf("[host] [ui] blend state create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC rs_desc{};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE; // flat 2D UI: winding order is not something the layout code tracks
    rs_desc.ScissorEnable = TRUE;       // always on; "disabled" just means the full render target, see EnableScissorRegion
    rs_desc.DepthClipEnable = TRUE;
    hr = m_device->CreateRasterizerState(&rs_desc, &m_rasterizer_state);
    if (FAILED(hr)) {
        std::printf("[host] [ui] rasterizer state create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds_desc{};
    ds_desc.DepthEnable = FALSE;
    ds_desc.StencilEnable = FALSE;
    hr = m_device->CreateDepthStencilState(&ds_desc, &m_depth_state);
    if (FAILED(hr)) {
        std::printf("[host] [ui] depth-stencil state create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    hr = m_device->CreateSamplerState(&samp_desc, &m_sampler_state);
    if (FAILED(hr)) {
        std::printf("[host] [ui] sampler state create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    // A 1x1 opaque white pixel, premultiplied (255,255,255,255 -- premultiplied white at full alpha
    // is unchanged from straight white). Bound whenever RenderGeometry gets texture==0, so the
    // pixel shader never needs an "untextured" branch.
    const Rml::byte white[4] = {255, 255, 255, 255};
    m_white_texture = createTextureFromRgba(white, 1, 1);
    if (m_white_texture == 0) {
        std::printf("[host] [ui] white fallback texture create failed\n");
        return false;
    }

    m_element_transform = Rml::Matrix4f::Identity();
    std::printf("[host] [ui] D3D11 render interface ready\n");
    return true;
}

void UiRenderInterfaceD3D11::shutdown() {
    if (m_white_texture != 0) {
        ReleaseTexture(m_white_texture);
        m_white_texture = 0;
    }
    if (m_sampler_state != nullptr) { m_sampler_state->Release(); m_sampler_state = nullptr; }
    if (m_depth_state != nullptr) { m_depth_state->Release(); m_depth_state = nullptr; }
    if (m_rasterizer_state != nullptr) { m_rasterizer_state->Release(); m_rasterizer_state = nullptr; }
    if (m_blend_state != nullptr) { m_blend_state->Release(); m_blend_state = nullptr; }
    if (m_transform_cb != nullptr) { m_transform_cb->Release(); m_transform_cb = nullptr; }
    if (m_input_layout != nullptr) { m_input_layout->Release(); m_input_layout = nullptr; }
    if (m_pixel_shader != nullptr) { m_pixel_shader->Release(); m_pixel_shader = nullptr; }
    if (m_vertex_shader != nullptr) { m_vertex_shader->Release(); m_vertex_shader = nullptr; }
    m_device = nullptr;
    m_context = nullptr;
}

void UiRenderInterfaceD3D11::beginFrame(ID3D11RenderTargetView* target, uint32_t width, uint32_t height) {
    m_target_width = width;
    m_target_height = height;

    // Far/near of +-10000 (not +-1): CSS `transform` on an element (rotate/perspective) can push
    // vertices well outside the panel's own plane, and the point is to see the panel skewed, not
    // clipped. RmlUi's own GL/VK backends use the same generous range for this projection.
    m_projection = Rml::Matrix4f::ProjectOrtho(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, -10000.0f, 10000.0f);
    updateTransformConstant();

    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // premultiplied transparent
    m_context->ClearRenderTargetView(target, clear);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    m_context->OMSetRenderTargets(1, &target, nullptr);
    m_context->OMSetBlendState(m_blend_state, nullptr, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_depth_state, 0);
    m_context->RSSetState(m_rasterizer_state);
    EnableScissorRegion(false);

    m_context->IASetInputLayout(m_input_layout);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertex_shader, nullptr, 0);
    m_context->PSSetShader(m_pixel_shader, nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, &m_transform_cb);
    m_context->PSSetSamplers(0, 1, &m_sampler_state);
}

void UiRenderInterfaceD3D11::endFrame() {
    ID3D11RenderTargetView* none = nullptr;
    m_context->OMSetRenderTargets(1, &none, nullptr);
}

Rml::CompiledGeometryHandle UiRenderInterfaceD3D11::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                                    Rml::Span<const int> indices) {
    auto* geom = new Geometry();
    geom->index_count = static_cast<UINT>(indices.size());

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vb_data{};
    vb_data.pSysMem = vertices.data();
    HRESULT hr = m_device->CreateBuffer(&vb_desc, &vb_data, &geom->vertex_buffer);
    if (FAILED(hr)) {
        std::printf("[host] [ui] vertex buffer create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        delete geom;
        return 0;
    }

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(int));
    ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ib_data{};
    ib_data.pSysMem = indices.data();
    hr = m_device->CreateBuffer(&ib_desc, &ib_data, &geom->index_buffer);
    if (FAILED(hr)) {
        std::printf("[host] [ui] index buffer create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        geom->vertex_buffer->Release();
        delete geom;
        return 0;
    }

    return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
}

void UiRenderInterfaceD3D11::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                            Rml::TextureHandle texture) {
    auto* geom = reinterpret_cast<Geometry*>(geometry);
    if (geom == nullptr) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_context->Map(m_transform_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* dst = static_cast<float*>(mapped.pData);
        std::memcpy(dst, m_mvp.data(), 16 * sizeof(float));
        dst[16] = translation.x;
        dst[17] = translation.y;
        dst[18] = 0.0f;
        dst[19] = 0.0f;
        m_context->Unmap(m_transform_cb, 0);
    }

    const auto* tex = reinterpret_cast<const Texture*>(texture != 0 ? texture : m_white_texture);
    m_context->PSSetShaderResources(0, 1, &tex->srv);

    const UINT stride = sizeof(Rml::Vertex);
    const UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, &geom->vertex_buffer, &stride, &offset);
    m_context->IASetIndexBuffer(geom->index_buffer, DXGI_FORMAT_R32_UINT, 0);
    m_context->DrawIndexed(geom->index_count, 0, 0);
}

void UiRenderInterfaceD3D11::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    auto* geom = reinterpret_cast<Geometry*>(geometry);
    if (geom == nullptr) {
        return;
    }
    if (geom->vertex_buffer != nullptr) {
        geom->vertex_buffer->Release();
    }
    if (geom->index_buffer != nullptr) {
        geom->index_buffer->Release();
    }
    delete geom;
}

Rml::TextureHandle UiRenderInterfaceD3D11::createTextureFromRgba(const Rml::byte* rgba_premultiplied, int width,
                                                                  int height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = rgba_premultiplied;
    init_data.SysMemPitch = static_cast<UINT>(width) * 4;

    ID3D11Texture2D* tex2d = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &init_data, &tex2d);
    if (FAILED(hr)) {
        std::printf("[host] [ui] texture create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 0;
    }

    auto* tex = new Texture();
    hr = m_device->CreateShaderResourceView(tex2d, nullptr, &tex->srv);
    tex2d->Release(); // the SRV holds its own reference
    if (FAILED(hr)) {
        std::printf("[host] [ui] SRV create failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        delete tex;
        return 0;
    }

    return reinterpret_cast<Rml::TextureHandle>(tex);
}

Rml::TextureHandle UiRenderInterfaceD3D11::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) {
    // The settings document (settings.rml/.rcss) is pure RCSS: colours, borders and gradients, no
    // <img> or `decorator: image(...)`. Nothing in this session's document ever calls this, so a
    // full WIC decode path would be untested code carried for a feature not in use -- refusing
    // cleanly is the honest option until a document actually needs an image asset.
    std::printf("[host] [ui] LoadTexture('%s') refused: no image loader wired up, document should not "
                "reference external images\n", source.c_str());
    texture_dimensions = Rml::Vector2i(0, 0);
    return 0;
}

Rml::TextureHandle UiRenderInterfaceD3D11::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                            Rml::Vector2i source_dimensions) {
    return createTextureFromRgba(source.data(), source_dimensions.x, source_dimensions.y);
}

void UiRenderInterfaceD3D11::ReleaseTexture(Rml::TextureHandle texture) {
    auto* tex = reinterpret_cast<Texture*>(texture);
    if (tex == nullptr) {
        return;
    }
    if (tex->srv != nullptr) {
        tex->srv->Release();
    }
    delete tex;
}

void UiRenderInterfaceD3D11::EnableScissorRegion(bool enable) {
    m_scissor_enabled = enable;
    if (!enable) {
        // "Disabled" is a scissor rect covering the whole render target -- the rasterizer state
        // always has ScissorEnable=TRUE (see init()), so there is no state object to switch.
        SetScissorRegion(Rml::Rectanglei::FromSize(Rml::Vector2i(static_cast<int>(m_target_width), static_cast<int>(m_target_height))));
    }
}

void UiRenderInterfaceD3D11::SetScissorRegion(Rml::Rectanglei region) {
    D3D11_RECT rect{};
    rect.left = region.Left();
    rect.top = region.Top();
    rect.right = region.Right();
    rect.bottom = region.Bottom();
    m_context->RSSetScissorRects(1, &rect);
}

void UiRenderInterfaceD3D11::SetTransform(const Rml::Matrix4f* transform) {
    m_element_transform = (transform != nullptr) ? *transform : Rml::Matrix4f::Identity();
    updateTransformConstant();
}

void UiRenderInterfaceD3D11::updateTransformConstant() {
    // Applied as projection * transform: the element transform operates in the same pixel space as
    // vertex positions, and the orthographic projection converts THAT into clip space last.
    m_mvp = m_projection * m_element_transform;
}

} // namespace xrui

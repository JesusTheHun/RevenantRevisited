// Windows compiles these shader strings at runtime, after the C++ build has
// succeeded. Check compilation and a few renderer contracts without game data
// or a window. WARP keeps the execution tests independent of the runner's GPU.

#include "../src/shaders/shaders.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Match Sokol's default D3D11 targets and runtime compiler flags.
constexpr const char* kVertexProfile = "vs_4_0";
constexpr const char* kPixelProfile = "ps_4_0";

::testing::AssertionResult Succeeded(HRESULT result)
{
    if (SUCCEEDED(result)) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "HRESULT 0x" << std::hex << result;
}

::testing::AssertionResult Compile(const std::string& source, const char* entry,
                                   const char* profile, ComPtr<ID3DBlob>& code)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source.data(), source.size(), "shader-test", nullptr, nullptr,
                                     entry, profile,
                                     D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                     code.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(result)) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
        << "Compiling " << entry << " (" << profile << "): HRESULT 0x" << std::hex << result
        << "\n" << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "No compiler diagnostic");
}

struct ShaderStage
{
    const char* name;
    std::string source;
    const char* entry;
    const char* profile;
};

void PrintTo(const ShaderStage& stage, std::ostream* out)
{
    *out << stage.name << " (" << stage.profile << ")";
}

// Keep this list alongside shaders.h's public stages. The shared light model
// is a prefix, so compile it with each pixel stage that consumes it, as the
// renderer's WithLightModel does.
std::vector<ShaderStage> ShaderStages()
{
    return {
        {"CompositeVs", kCompositeVs, kShaderVsEntry, kVertexProfile},
        {"CompositeFs", kCompositeFs, kShaderFsEntry, kPixelProfile},
        {"CompositeReduceFs", kCompositeReduceFs, kShaderFsEntry, kPixelProfile},
        {"ColorMeshVs", kColorMeshVs, kShaderVsEntry, kVertexProfile},
        {"ColorMeshFs", kColorMeshFs, kShaderFsEntry, kPixelProfile},
        {"TileVs", kTileVs, kShaderVsEntry, kVertexProfile},
        {"TileFs", kTileFs, kShaderFsEntry, kPixelProfile},
        {"MeshVs", kMeshVs, kShaderVsEntry, kVertexProfile},
        {"MeshFs", kMeshFs, kShaderFsEntry, kPixelProfile},
        {"MeshDepthFs", kMeshDepthFs, kShaderFsEntry, kPixelProfile},
        {"MeshTranslucentFs", std::string(kLightModel) + kMeshTranslucentFs, kShaderFsEntry, kPixelProfile},
        {"AOVs", kAOVs, kShaderVsEntry, kVertexProfile},
        {"AOFs", kAOFs, kShaderFsEntry, kPixelProfile},
        {"ShadowVs", kShadowVs, kShaderVsEntry, kVertexProfile},
        {"ShadowFs", kShadowFs, kShaderFsEntry, kPixelProfile},
        {"ShadowBlurFs", kShadowBlurFs, kShaderFsEntry, kPixelProfile},
        {"LightVs", kLightVs, kShaderVsEntry, kVertexProfile},
        {"LightFs", std::string(kLightModel) + kLightFs, kShaderFsEntry, kPixelProfile},
        {"FxBillboardVs", kFxBillboardVs, kShaderVsEntry, kVertexProfile},
        {"FxBillboardFs", kFxBillboardFs, kShaderFsEntry, kPixelProfile},
        {"FxParticleVs", kFxParticleVs, kShaderVsEntry, kVertexProfile},
        {"FxStripVs", kFxStripVs, kShaderVsEntry, kVertexProfile},
        {"FxStripFs", kFxStripFs, kShaderFsEntry, kPixelProfile},
    };
}

class HlslCompilation : public ::testing::TestWithParam<ShaderStage> {};

TEST_P(HlslCompilation, Compiles)
{
    const auto& stage = GetParam();
    ComPtr<ID3DBlob> code;
    ASSERT_TRUE(Compile(stage.source, stage.entry, stage.profile, code));
}

INSTANTIATE_TEST_SUITE_P(WindowsShaders, HlslCompilation, ::testing::ValuesIn(ShaderStages()),
                        [](const ::testing::TestParamInfo<ShaderStage>& info) { return info.param.name; });

struct Float4 { float x, y, z, w; };
struct Vertex { float x, y, u, v; };
struct ProjectedVertex { Float4 position; float u, v; };

constexpr D3D11_INPUT_ELEMENT_DESC kVertexLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
};

class D3D11ShaderExecution : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        ASSERT_TRUE(Succeeded(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                                               D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                               context.GetAddressOf())));
    }

    ::testing::AssertionResult Buffer(UINT size, UINT bind, ComPtr<ID3D11Buffer>& buffer,
                                      const void* bytes = nullptr, bool readback = false)
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = size;
        desc.Usage = readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = readback ? D3D11_CPU_ACCESS_READ : 0;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = bytes;
        return Succeeded(device->CreateBuffer(&desc, bytes ? &data : nullptr, buffer.GetAddressOf()));
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

// Matches InitTilePipeline / EmitTile's twelve float4 constant block.
struct TileUniforms
{
    Float4 rect, zparams, tile_root, tile_sprite, filter, obj_id;
    Float4 camera, proj, tile_rect, cam_tile, debug, raycast;
};
static_assert(sizeof(TileUniforms) == 12 * 4 * sizeof(float));

TileUniforms TileParams()
{
    TileUniforms params = {};
    params.rect = {-0.8f, -0.6f, 0.4f, 0.5f};
    params.zparams.w = 1.0f;
    params.tile_sprite = {32.0f, 32.0f, 64.0f, 64.0f};
    params.camera.z = 4096.0f;
    params.proj = {640.0f, 480.0f, 1280.0f, 960.0f};
    return params;
}

class D3D11TileProjection : public D3D11ShaderExecution
{
  protected:
    void SetUp() override
    {
        D3D11ShaderExecution::SetUp();
        if (HasFatalFailure()) return;
        ComPtr<ID3DBlob> vs_code, gs_code;
        ASSERT_TRUE(Compile(kTileVs, kShaderVsEntry, kVertexProfile, vs_code));
        // Capture the actual vertex shader's outputs before rasterization.
        const char* capture = R"HLSL(
struct vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
[maxvertexcount(1)]
void capture(point vertex input[1], inout PointStream<vertex> output) { output.Append(input[0]); }
)HLSL";
        ASSERT_TRUE(Compile(capture, "capture", "gs_4_0", gs_code));
        ASSERT_TRUE(Succeeded(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                                         nullptr, vertex_shader.GetAddressOf())));
        ASSERT_TRUE(Succeeded(device->CreateInputLayout(kVertexLayout, 2, vs_code->GetBufferPointer(),
                                                        vs_code->GetBufferSize(), layout.GetAddressOf())));
        const D3D11_SO_DECLARATION_ENTRY outputs[] = {
            {0, "SV_Position", 0, 0, 4, 0}, {0, "TEXCOORD", 0, 0, 2, 0},
        };
        const UINT stride = sizeof(ProjectedVertex);
        ASSERT_TRUE(Succeeded(device->CreateGeometryShaderWithStreamOutput(
            gs_code->GetBufferPointer(), gs_code->GetBufferSize(), outputs, 2, &stride, 1,
            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, capture_shader.GetAddressOf())));
        ASSERT_TRUE(Buffer(sizeof(TileUniforms), D3D11_BIND_CONSTANT_BUFFER, constants));
        ASSERT_TRUE(Buffer(sizeof(Vertex), D3D11_BIND_VERTEX_BUFFER, input));
        ASSERT_TRUE(Buffer(sizeof(ProjectedVertex), D3D11_BIND_STREAM_OUTPUT, output));
        ASSERT_TRUE(Buffer(sizeof(ProjectedVertex), 0, staging, nullptr, true));
    }

    void ExpectProjection(const TileUniforms& params, const Vertex& vertex, Float4 expected)
    {
        context->UpdateSubresource(constants.Get(), 0, nullptr, &params, 0, 0);
        context->UpdateSubresource(input.Get(), 0, nullptr, &vertex, 0, 0);
        const UINT stride = sizeof(Vertex), offset = 0;
        ID3D11Buffer* vb = input.Get();
        ID3D11Buffer* cb = constants.Get();
        ID3D11Buffer* so = output.Get();
        context->IASetInputLayout(layout.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &cb);
        context->GSSetShader(capture_shader.Get(), nullptr, 0);
        context->SOSetTargets(1, &so, &offset);
        context->Draw(1, 0);
        context->SOSetTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ASSERT_TRUE(Succeeded(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        ProjectedVertex actual;
        std::memcpy(&actual, mapped.pData, sizeof(actual));
        context->Unmap(staging.Get(), 0);
        EXPECT_NEAR(actual.position.x, expected.x, 1e-5f);
        EXPECT_NEAR(actual.position.y, expected.y, 1e-5f);
        EXPECT_NEAR(actual.position.z, expected.z, 1e-5f);
        EXPECT_NEAR(actual.position.w, expected.w, 1e-5f);
        EXPECT_FLOAT_EQ(actual.u, vertex.u);
        EXPECT_FLOAT_EQ(actual.v, vertex.v);
    }

    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11GeometryShader> capture_shader;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants, input, output, staging;
};

TEST_F(D3D11TileProjection, OrthographicRectangle)
{
    // A quarter of the way across and three quarters up the CPU's NDC rect.
    ExpectProjection(TileParams(), {0.25f, 0.75f, 0.5f, 0.5f}, {-0.7f, -0.225f, 0.0f, 1.0f});
}

TEST_F(D3D11TileProjection, OrthographicScreenCorner)
{
    auto params = TileParams();
    params.rect = {-1.0f, -1.0f, 2.0f, 2.0f};
    ExpectProjection(params, {1.0f, 0.0f, 0.25f, 0.75f}, {1.0f, -1.0f, 0.0f, 1.0f});
}

TEST_F(D3D11TileProjection, PerspectiveProxyIsAlreadyProjected)
{
    auto params = TileParams();
    params.camera.w = 1.0f;
    params.raycast.w = 2.0f;
    params.rect = {0.0f, 0.0f, 1.0f, 1.0f};
    ExpectProjection(params, {-0.25f, 0.5f, 0.25f, 0.75f}, {-0.25f, 0.5f, 0.0f, 1.0f});
}

TEST_F(D3D11TileProjection, FlatPerspectiveUsesViewportNotNdcRectangle)
{
    auto params = TileParams();
    params.camera.w = 1.0f;
    // The sprite's anchor is at world (0,0,0), hence the viewport centre.
    // The unrelated NDC rect must not be used as a pixel-space viewport.
    ExpectProjection(params, {0.25f, 0.75f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f});
}

class D3D11Composite : public D3D11ShaderExecution
{
  protected:
    void ExpectTint(Float4 tint, const std::array<uint8_t, 4>& expected)
    {
        ComPtr<ID3DBlob> vs_code, ps_code;
        ASSERT_TRUE(Compile(kCompositeVs, kShaderVsEntry, kVertexProfile, vs_code));
        ASSERT_TRUE(Compile(kCompositeFs, kShaderFsEntry, kPixelProfile, ps_code));
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ASSERT_TRUE(Succeeded(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                                         nullptr, vs.GetAddressOf())));
        ASSERT_TRUE(Succeeded(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
                                                        nullptr, ps.GetAddressOf())));
        ASSERT_TRUE(Succeeded(device->CreateInputLayout(kVertexLayout, 2, vs_code->GetBufferPointer(),
                                                       vs_code->GetBufferSize(), layout.GetAddressOf())));
        const Vertex quad[] = {{0,0,0,0}, {1,0,1,0}, {1,1,1,1}, {0,0,0,0}, {1,1,1,1}, {0,1,0,1}};
        const Float4 uniforms[] = {{-1,-1,2,2}, {0,0,1,1}, {0,0,0,0}, tint};
        ComPtr<ID3D11Buffer> vertices, constants;
        ASSERT_TRUE(Buffer(sizeof(quad), D3D11_BIND_VERTEX_BUFFER, vertices, quad));
        ASSERT_TRUE(Buffer(sizeof(uniforms), D3D11_BIND_CONSTANT_BUFFER, constants, uniforms));

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::array<uint8_t, 4> color = {128, 64, 32, 255};
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = color.data();
        data.SysMemPitch = 4;
        ComPtr<ID3D11Texture2D> source, target, staging;
        ComPtr<ID3D11ShaderResourceView> image;
        ComPtr<ID3D11RenderTargetView> rtv;
        ASSERT_TRUE(Succeeded(device->CreateTexture2D(&desc, &data, source.GetAddressOf())));
        ASSERT_TRUE(Succeeded(device->CreateShaderResourceView(source.Get(), nullptr, image.GetAddressOf())));
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ASSERT_TRUE(Succeeded(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf())));
        ASSERT_TRUE(Succeeded(device->CreateRenderTargetView(target.Get(), nullptr, rtv.GetAddressOf())));
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ASSERT_TRUE(Succeeded(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())));
        D3D11_SAMPLER_DESC sampler_desc = {};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        ASSERT_TRUE(Succeeded(device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf())));
        D3D11_RASTERIZER_DESC raster_desc = {};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        ASSERT_TRUE(Succeeded(device->CreateRasterizerState(&raster_desc, raster.GetAddressOf())));
        D3D11_DEPTH_STENCIL_DESC depth_desc = {};
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> depth;
        ASSERT_TRUE(Succeeded(device->CreateDepthStencilState(&depth_desc, depth.GetAddressOf())));

        ID3D11Buffer* vb = vertices.Get();
        ID3D11Buffer* cb = constants.Get();
        ID3D11RenderTargetView* output = rtv.Get();
        ID3D11ShaderResourceView* texture = image.Get();
        ID3D11SamplerState* sampling = sampler.Get();
        const UINT stride = sizeof(Vertex), offset = 0;
        const D3D11_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
        const float clear[] = {0, 0, 0, 0};
        context->ClearRenderTargetView(output, clear);
        context->IASetInputLayout(layout.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetShaderResources(0, 1, &texture);
        context->PSSetSamplers(0, 1, &sampling);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->OMSetRenderTargets(1, &output, nullptr);
        context->Draw(6, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ASSERT_TRUE(Succeeded(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        std::array<uint8_t, 4> actual;
        std::memcpy(actual.data(), mapped.pData, actual.size());
        context->Unmap(staging.Get(), 0);
        for (size_t channel = 0; channel < actual.size(); ++channel)
            EXPECT_NEAR(int(actual[channel]), int(expected[channel]), 1) << "channel " << channel;
    }
};

TEST_F(D3D11Composite, WhiteTintPreservesTheSample)
{
    ExpectTint({1, 1, 1, 1}, {128, 64, 32, 255});
}

TEST_F(D3D11Composite, TintMultipliesRgbAndAlpha)
{
    ExpectTint({0.5f, 0.25f, 1.0f, 0.5f}, {64, 16, 32, 128});
}

} // namespace

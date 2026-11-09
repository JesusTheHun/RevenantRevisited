// Exercise the production Sokol/D3D11 readback bridge without a window.
#define SOKOL_IMPL
#include <sokol_gfx.h>
#undef SOKOL_IMPL
#undef SOKOL_GFX_IMPL
#include "../src/renderer_readback.h"
#include "../src/meshpickpipeline.h"
#include "../src/shaders/mesh.hlsl.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
class D3D11Readback : public testing::Test
{
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    sg_image image{};
    std::array<uint8_t, 4 * 3 * 4> pixels{};
    void SetUp() override
    {
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
        sg_desc setup{};
        setup.context.d3d11.device = device.Get();
        setup.context.d3d11.device_context = context.Get();
        sg_setup(&setup);
        ASSERT_TRUE(sg_isvalid());
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i + 1);
        sg_image_desc desc{};
        desc.width = 4;
        desc.height = 3;
        desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.data.subimage[0][0] = {pixels.data(), pixels.size()};
        image = sg_make_image(&desc);
        ASSERT_EQ(sg_query_image_state(image), SG_RESOURCESTATE_VALID);
    }
    void TearDown() override { sg_destroy_image(image); sg_shutdown(); }
};

TEST_F(D3D11Readback, ReadsSubrectangleWithDriverRowPitch)
{
    std::array<uint8_t, 3 * 2 * 4> output{};
    ASSERT_TRUE(RendererReadback::ReadRect(image, 1, 1, 3, 2, output.data()));
    for (size_t y = 0; y < 2; ++y)
        for (size_t x = 0; x < 3 * 4; ++x)
            EXPECT_EQ(output[y * 12 + x], pixels[(y + 1) * 16 + 4 + x]);
}

TEST_F(D3D11Readback, RejectsInvalidAndOverflowingRegions)
{
    std::array<uint8_t, 4> output{};
    EXPECT_FALSE(RendererReadback::ReadRect(image, -1, 0, 1, 1, output.data()));
    EXPECT_FALSE(RendererReadback::ReadRect(image, 0, 0, INT32_MAX, 1, output.data()));
    EXPECT_FALSE(RendererReadback::ReadRect({}, 0, 0, 1, 1, output.data()));
    EXPECT_FALSE(RendererReadback::ReadRect(image, 0, 0, 1, 1, nullptr));
    sg_image_desc desc{};
    desc.width = 4;
    desc.height = 3;
    desc.data.subimage[0][0] = {pixels.data(), pixels.size()};
    sg_image stale = sg_make_image(&desc);
    sg_destroy_image(stale);
    EXPECT_EQ(sg_d3d11_query_image_handle(stale), nullptr);
}

TEST_F(D3D11Readback, AsyncSlotsDeliverOnceAndCanBeReused)
{
    std::array<uint8_t, 4> output{};
    EXPECT_FALSE(RendererReadback::BeginReadRect(-1, image, 0, 0, 1, 1));
    for (int n = 0; n < 2; ++n)
    {
        ASSERT_TRUE(RendererReadback::BeginReadRect(0, image, n, 1, 1, 1));
        EXPECT_FALSE(RendererReadback::BeginReadRect(0, image, 0, 0, 1, 1));
        EXPECT_FALSE(RendererReadback::PollReadRect(0, output.data(), 3));
        context->Flush();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool done = false;
        while (!(done = RendererReadback::PollReadRect(0, output.data(), int32_t(output.size()))) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        ASSERT_TRUE(done);
        for (size_t c = 0; c < 4; ++c) EXPECT_EQ(output[c], pixels[16 + n * 4 + c]);
        EXPECT_FALSE(RendererReadback::PollReadRect(0, output.data(), 4));
    }
}
ComPtr<ID3DBlob> CompileMeshShader(const char* source, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "mesh-picking", nullptr, nullptr,
                                     entry, profile,
                                     D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                     0, &code, &errors);
    if (FAILED(result))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer())
                                        : "Mesh shader compilation failed");
    return code;
}

// Execute the shipped mesh shaders, not a test shader that writes a chosen
// color: the readback bridge alone cannot catch a missing instance-id output.
class D3D11MeshPicking : public D3D11Readback, public testing::WithParamInterface<int>
{
protected:
    std::array<sg_image, 4> targets{};
    sg_image albedo{};

    void TearDown() override
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        for (sg_image target : targets) sg_destroy_image(target);
        sg_destroy_image(albedo);
        D3D11Readback::TearDown();
    }

    float depth_sample = 0.0f;

    void Render(bool opaque, bool picking_only = false, float scene_depth = 1.0f)
    {
        const auto vsCode = CompileMeshShader(kMeshVsHlsl, "main_vs", "vs_4_0");
        const auto psCode = CompileMeshShader(kMeshFsHlsl, "main_ps", "ps_4_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
        ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));

        std::array<ComPtr<ID3D11RenderTargetView>, 4> views;
        std::array<ID3D11RenderTargetView*, 4> rawViews{};
        for (size_t n = 0; n < targets.size(); ++n)
        {
            sg_image_desc desc{};
            desc.width = desc.height = 8;
            desc.render_target = true;
            desc.pixel_format = SG_PIXELFORMAT_RGBA8;
            targets[n] = sg_make_image(&desc);
            auto* texture = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_handle(targets[n])));
            ASSERT_NE(texture, nullptr);
            ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &views[n])));
            rawViews[n] = views[n].Get();
            const float clear[4] = {picking_only && n < 3 ? .125f : 0,
                                    picking_only && n < 3 ? .25f : 0,
                                    picking_only && n < 3 ? .5f : 0,
                                    picking_only && n < 3 ? 1.0f : 0};
            context->ClearRenderTargetView(rawViews[n], clear);
        }

        const uint8_t texel[4] = {255, 255, 255, uint8_t(opaque ? 255 : 0)};
        sg_image_desc desc{};
        desc.width = desc.height = 1;
        desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.data.subimage[0][0] = {texel, sizeof(texel)};
        albedo = sg_make_image(&desc);
        auto* texture = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_handle(albedo)));
        ComPtr<ID3D11ShaderResourceView> srv;
        ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &srv)));

        // Two adjacent instances of the same quad. The instance layout is
        // renderer.cpp's 28-float row, including all four packed id bytes.
        struct Vertex { float pos[3], normal[3], uv[2]; };
        std::array<Vertex, 6> vertices{};
        const float corners[6][2] = {{-1.5f,-1.5f}, {1.5f,-1.5f}, {-1.5f,1.5f},
                                     {-1.5f,1.5f}, {1.5f,-1.5f}, {1.5f,1.5f}};
        const bool projected = GetParam() == 2;
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            const float s = corners[i][0], t = corners[i][1];
            vertices[i] = projected ? Vertex{{s / 1.41421356f, -t / 1.41421356f, 0}, {0,0,1}, {.5f,.5f}}
                                    : Vertex{{s * .5f + t, t - s * .5f, 0}, {0,0,1}, {.5f,.5f}};
        }
        const uint32_t ids[2] = {0x81234567u, 0xFEDCBA98u};
        float instances[2][28]{};
        for (int n = 0; n < 2; ++n)
        {
            auto& row = instances[n];
            row[0] = row[5] = row[10] = row[15] = 1;
            const float x = n == 0 ? 2.0f : 6.0f;
            row[3] = projected ? x / 1.41421356f : x * .5f + 4;
            row[7] = projected ? -4 / 1.41421356f : 4 - x * .5f;
            if (projected) row[11] = .5f / 2.1798270608996972e-5f;
            for (int c = 0; c < 4; ++c)
            {
                row[16 + c] = 1;
                row[20 + c] = float((ids[n] >> (c * 8)) & 255u) / 255.0f;
            }
        }
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,1,32,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",4,DXGI_FORMAT_R32G32B32A32_FLOAT,1,48,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",5,DXGI_FORMAT_R32G32B32A32_FLOAT,1,64,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",6,DXGI_FORMAT_R32G32B32A32_FLOAT,1,80,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",7,DXGI_FORMAT_R32G32_FLOAT,1,96,D3D11_INPUT_PER_INSTANCE_DATA,1},
            {"TEXCOORD",8,DXGI_FORMAT_R32_FLOAT,1,104,D3D11_INPUT_PER_INSTANCE_DATA,1},
        };
        ComPtr<ID3D11InputLayout> layout;
        ASSERT_TRUE(SUCCEEDED(device->CreateInputLayout(elements, UINT(std::size(elements)),
                                                        vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout)));
        auto buffer = [&](const void* data, UINT bytes, UINT bind) {
            ComPtr<ID3D11Buffer> result;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = bind;
            D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
            if (FAILED(device->CreateBuffer(&bd, &initial, &result)))
                throw std::runtime_error("Could not create mesh test buffer");
            return result;
        };
        const auto vb = buffer(vertices.data(), sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
        const auto ib = buffer(instances, sizeof(instances), D3D11_BIND_VERTEX_BUFFER);
        float uniforms[20] = {0,0,8,8, 0,2000,1000,GetParam() == 1 ? 1.0f : 0.0f,
                               0,0,1,projected ? 1.0f : 0.0f, 1,1,1,0, 0,0,0,0};
        const auto cb = buffer(uniforms, sizeof(uniforms), D3D11_BIND_CONSTANT_BUFFER);
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> raster;
        ASSERT_TRUE(SUCCEEDED(device->CreateRasterizerState(&rasterDesc, &raster)));
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ComPtr<ID3D11SamplerState> sampler;
        ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, &sampler)));
        ID3D11Buffer* buffers[] = {vb.Get(), ib.Get()};
        const UINT strides[] = {sizeof(Vertex), sizeof(instances[0])}, offsets[] = {0,0};
        ID3D11Buffer* constant = cb.Get();
        ID3D11ShaderResourceView* resource = srv.Get();
        ID3D11SamplerState* sample = sampler.Get();
        const D3D11_VIEWPORT viewport{0,0,8,8,0,1};
        // Use the production picking descriptor for color/depth policy.
        sg_pipeline_desc pick{};
        ConfigureMeshPickPipeline(pick);
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.IndependentBlendEnable = TRUE;
        for (int n = 0; n < 4; ++n)
            blendDesc.RenderTarget[n].RenderTargetWriteMask =
                picking_only && pick.colors[n].write_mask == SG_COLORMASK_NONE
                    ? 0 : D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> blend;
        ASSERT_TRUE(SUCCEEDED(device->CreateBlendState(&blendDesc, &blend)));
        context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu);
        D3D11_TEXTURE2D_DESC depthDesc{};
        depthDesc.Width = depthDesc.Height = 8;
        depthDesc.MipLevels = depthDesc.ArraySize = depthDesc.SampleDesc.Count = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> depthView;
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, &depth)));
        ASSERT_TRUE(SUCCEEDED(device->CreateDepthStencilView(depth.Get(), nullptr, &depthView)));
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, scene_depth, 0);
        D3D11_DEPTH_STENCIL_DESC depthStateDesc{};
        depthStateDesc.DepthEnable = TRUE;
        depthStateDesc.DepthWriteMask = picking_only && !pick.depth.write_enabled
            ? D3D11_DEPTH_WRITE_MASK_ZERO : D3D11_DEPTH_WRITE_MASK_ALL;
        ASSERT_EQ(pick.depth.compare, SG_COMPAREFUNC_LESS_EQUAL);
        depthStateDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ComPtr<ID3D11DepthStencilState> depthState;
        ASSERT_TRUE(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc, &depthState)));
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetRenderTargets(4, rawViews.data(), depthView.Get());
        context->RSSetViewports(1, &viewport);
        context->RSSetState(raster.Get());
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &constant);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &resource);
        context->PSSetSamplers(0, 1, &sample);
        context->DrawInstanced(6, 2, 0, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->Flush();
        depthDesc.BindFlags = 0;
        depthDesc.Usage = D3D11_USAGE_STAGING;
        depthDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, &staging)));
        context->CopyResource(staging.Get(), depth.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + 4 * mapped.RowPitch;
        std::memcpy(&depth_sample, row + 2 * sizeof(float), sizeof(float));
        context->Unmap(staging.Get(), 0);
    }
};

TEST_P(D3D11MeshPicking, InstanceIdsReachPickingTarget)
{
    Render(true);
    const uint8_t expected[2][4] = {{0x67,0x45,0x23,0x81}, {0x98,0xBA,0xDC,0xFE}};
    for (int n = 0; n < 2; ++n)
    {
        std::array<uint8_t, 4> output{};
        ASSERT_TRUE(RendererReadback::ReadPixel(targets[3], n == 0 ? 2 : 6, 4, output.data()));
        EXPECT_EQ(output, (std::array<uint8_t,4>{expected[n][0],expected[n][1],expected[n][2],expected[n][3]}));
    }
}

TEST_P(D3D11MeshPicking, TransparentTexelsLeavePickingTargetClear)
{
    Render(false);
    std::array<uint8_t, 4> output{};
    ASSERT_TRUE(RendererReadback::ReadPixel(targets[3], 2, 4, output.data()));
    EXPECT_EQ(output, (std::array<uint8_t,4>{0,0,0,0}));
}

TEST_P(D3D11MeshPicking, PickingUpdatesIdsWithoutChangingVisualsOrDepth)
{
    Render(true, true);
    std::array<uint8_t, 4> output{};
    ASSERT_TRUE(RendererReadback::ReadPixel(targets[3], 2, 4, output.data()));
    EXPECT_EQ(output, (std::array<uint8_t,4>{0x67,0x45,0x23,0x81}));
    for (int n = 0; n < 3; ++n) {
        ASSERT_TRUE(RendererReadback::ReadPixel(targets[n], 2, 4, output.data()));
        EXPECT_EQ(output, (std::array<uint8_t,4>{32,64,128,255}));
    }
    EXPECT_FLOAT_EQ(depth_sample, 1.0f);
}

TEST_P(D3D11MeshPicking, PickingRejectsTransparentPixels)
{
    Render(false, true);
    std::array<uint8_t, 4> output{};
    ASSERT_TRUE(RendererReadback::ReadPixel(targets[3], 2, 4, output.data()));
    EXPECT_EQ(output, (std::array<uint8_t,4>{0,0,0,0}));
    EXPECT_FLOAT_EQ(depth_sample, 1.0f);
}

TEST_P(D3D11MeshPicking, PickingKeepsMeshesBehindOpaqueSceneryHidden)
{
    Render(true, true, .1f);
    std::array<uint8_t, 4> output{};
    ASSERT_TRUE(RendererReadback::ReadPixel(targets[3], 2, 4, output.data()));
    EXPECT_EQ(output, (std::array<uint8_t,4>{0,0,0,0}));
    EXPECT_FLOAT_EQ(depth_sample, .1f);
}

INSTANTIATE_TEST_SUITE_P(Projection, D3D11MeshPicking, testing::Values(0,1,2),
    [](const testing::TestParamInfo<int>& p) {
        return p.param == 0 ? "Orthographic" : p.param == 1 ? "Perspective" : "ProjectedProxy";
    });
} // namespace

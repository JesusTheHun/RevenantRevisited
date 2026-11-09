// Execute the production tile pixel shader without game data or a window.
#include "../src/shaders/tile.hlsl.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace {
struct TileCase { float depth; uint8_t alpha; bool visible; const char* name; };
class D3D11TileVisibility : public testing::TestWithParam<TileCase> {};

TEST_P(D3D11TileVisibility, DecodedCoverageControlsVisibility)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    // Geometry is independent of the behavior under test: one full-screen
    // triangle, with its sample at the center of a one-pixel source bitmap.
    const char* vertex = R"(
        struct V { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
        V main(uint id:SV_VertexID) {
            float2 points[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V v; v.pos=float4(points[id],0,1); v.uv=float2(.5,.5); return v;
        })";
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    ASSERT_TRUE(SUCCEEDED(D3DCompile(vertex, std::strlen(vertex), nullptr, nullptr, nullptr,
        "main", "vs_4_0", 0, 0, &vsCode, &errors)));
    ASSERT_TRUE(SUCCEEDED(D3DCompile(kTileFsHlsl, std::strlen(kTileFsHlsl), nullptr, nullptr, nullptr,
        "main_ps", "ps_4_0", 0, 0, &psCode, &errors)))
        << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));
    float uniforms[48]{};
    uniforms[4] = .4f; uniforms[5] = .01f; uniforms[7] = 1;
    uniforms[14] = uniforms[15] = uniforms[16] = 1;
    for (int n = 0; n < 4; ++n) uniforms[20+n] = float(25+n)/255;
    uniforms[30] = uniforms[31] = 1; // viewport width/height
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(uniforms); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{uniforms, 0, 0};
    ComPtr<ID3D11Buffer> constants;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&bd, &data, &constants)));
    const auto test = GetParam();
    const uint8_t color[4] = {128,64,32,test.alpha};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data = {color, sizeof(color), 0};
    ComPtr<ID3D11Texture2D> colorTexture, depthTexture;
    ComPtr<ID3D11ShaderResourceView> colorView, depthView;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&td, &data, &colorTexture)));
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(colorTexture.Get(), nullptr, &colorView)));
    td.Format = DXGI_FORMAT_R32_FLOAT; data = {&test.depth, sizeof(float), 0};
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&td, &data, &depthTexture)));
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView)));
    std::array<ComPtr<ID3D11Texture2D>,4> targets;
    std::array<ComPtr<ID3D11RenderTargetView>,4> views;
    std::array<ID3D11RenderTargetView*,4> outputs{};
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (int n = 0; n < 4; ++n) {
        td.Format = formats[n];
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &targets[n])));
        ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(targets[n].Get(), nullptr, &views[n])));
        outputs[n] = views[n].Get();
        const float clear[4]{}; context->ClearRenderTargetView(outputs[n], clear);
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&sd, &sampler)));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster;
    ASSERT_TRUE(SUCCEEDED(device->CreateRasterizerState(&rd, &raster)));
    ID3D11Buffer* cb = constants.Get();
    ID3D11ShaderResourceView* inputs[] = {colorView.Get(),depthView.Get()};
    ID3D11SamplerState* sample = sampler.Get();
    const D3D11_VIEWPORT viewport{0,0,1,1,0,1};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0,1,&cb);
    context->PSSetShaderResources(0,2,inputs);
    context->PSSetSamplers(0,1,&sample);
    context->RSSetState(raster.Get()); context->RSSetViewports(1,&viewport);
    context->OMSetRenderTargets(4,outputs.data(),nullptr);
    context->Draw(3,0);
    context->OMSetRenderTargets(0,nullptr,nullptr);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&staging)));
    for (int n : {0,3}) {
        context->CopyResource(staging.Get(),targets[n].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        std::array<uint8_t,4> actual{};
        std::memcpy(actual.data(),mapped.pData,actual.size()); context->Unmap(staging.Get(),0);
        const std::array<uint8_t,4> expected = !test.visible ? std::array<uint8_t,4>{}
            : n == 0 ? std::array<uint8_t,4>{128,64,32,255} : std::array<uint8_t,4>{25,26,27,28};
        EXPECT_EQ(actual,expected) << "attachment " << n;
    }
}
INSTANTIATE_TEST_SUITE_P(Bitmap, D3D11TileVisibility, testing::Values(
    TileCase{0,255,true,"FlatZeroDepth"}, TileCase{4,255,true,"PositiveDepth"},
    TileCase{-4,255,true,"NegativeDepth"}, TileCase{0,0,false,"TransparentFlat"},
    TileCase{4,0,false,"TransparentDepth"}, TileCase{100,255,false,"OutsideSceneDepth"}),
    [](const testing::TestParamInfo<TileCase>& p){return p.param.name;});
} // namespace

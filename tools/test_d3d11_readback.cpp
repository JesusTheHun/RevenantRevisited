// Exercise the production Sokol/D3D11 readback bridge without a window.
#define SOKOL_IMPL
#include <sokol_gfx.h>
#undef SOKOL_IMPL
#undef SOKOL_GFX_IMPL
#include "../src/renderer_readback.h"
#include <wrl/client.h>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <thread>

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
} // namespace

// D3D11 texture readback for object picking and frame captures.
#if defined(_WIN32) && defined(SOKOL_D3D11)

#include "../../renderer_readback.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

namespace RendererReadback {
namespace {
using Microsoft::WRL::ComPtr;

struct Readback {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Query> ready;
    int32_t width = 0, height = 0;
    bool bgra = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool pending = false;
};
std::array<Readback, kAsyncReadSlots> slots;

bool CopyRect(Readback& read, sg_image image, int32_t x, int32_t y, int32_t w, int32_t h)
{
    auto* source = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_handle(image)));
    if (!source || x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (uint32_t(x) >= desc.Width || uint32_t(y) >= desc.Height ||
        uint32_t(w) > desc.Width - uint32_t(x) || uint32_t(h) > desc.Height - uint32_t(y) ||
        desc.SampleDesc.Count != 1 || desc.ArraySize != 1) return false;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!bgra && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return false;
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    if (!read.texture || read.width != w || read.height != h || read.format != desc.Format ||
        read.device.Get() != device.Get())
    {
        read = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &read.texture))) return false;
        device->GetImmediateContext(&read.context);
        read.width = w;
        read.height = h;
        read.bgra = bgra;
        read.format = desc.Format;
        read.device = device;
    }
    D3D11_BOX box{UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1};
    read.context->CopySubresourceRegion(read.texture.Get(), 0, 0, 0, 0, source, 0, &box);
    return true;
}

bool ReadRows(Readback& read, uint8_t* output, UINT flags)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(read.context->Map(read.texture.Get(), 0, D3D11_MAP_READ, flags, &mapped))) return false;
    for (int32_t y = 0; y < read.height; ++y)
    {
        auto* row = output + size_t(y) * read.width * 4;
        std::memcpy(row, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(read.width) * 4);
        if (read.bgra)
            for (int32_t x = 0; x < read.width; ++x)
            {
                const uint8_t blue = row[x * 4];
                row[x * 4] = row[x * 4 + 2];
                row[x * 4 + 2] = blue;
            }
    }
    read.context->Unmap(read.texture.Get(), 0);
    return true;
}
} // namespace

bool ReadPixel(sg_image image, int32_t x, int32_t y, uint8_t output[4])
{
    if (!output) return false;
    std::memset(output, 0, 4);
    return ReadRect(image, x, y, 1, 1, output);
}

bool ReadRect(sg_image image, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t* output)
{
    Readback read;
    return output && CopyRect(read, image, x, y, w, h) && ReadRows(read, output, 0);
}

bool BeginReadRect(int32_t slot, sg_image image, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (slot < 0 || slot >= kAsyncReadSlots || w <= 0 || h <= 0 ||
        int64_t(w) * h * 4 > kMaxAsyncReadBytes) return false;
    Readback& read = slots[slot];
    if (read.pending || !CopyRect(read, image, x, y, w, h)) return false;
    if (!read.ready)
    {
        ComPtr<ID3D11Device> device;
        read.texture->GetDevice(&device);
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        if (FAILED(device->CreateQuery(&desc, &read.ready))) return false;
    }
    read.context->End(read.ready.Get());
    read.pending = true;
    return true;
}

bool PollReadRect(int32_t slot, uint8_t* output, int32_t bytes)
{
    if (slot < 0 || slot >= kAsyncReadSlots || !output) return false;
    Readback& read = slots[slot];
    if (!read.pending || int64_t(bytes) < int64_t(read.width) * read.height * 4) return false;
    const HRESULT status = read.context->GetData(read.ready.Get(), nullptr, 0, 0);
    if (status == S_FALSE) return false;
    if (FAILED(status)) { read.pending = false; return false; }
    if (!ReadRows(read, output, D3D11_MAP_FLAG_DO_NOT_WAIT)) return false;
    read.pending = false;
    return true;
}
} // namespace RendererReadback
#endif

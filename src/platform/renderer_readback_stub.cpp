// *************************************************************************
// *                  Revenant Revisited (port) - 2026                     *
// *   renderer_readback_stub.cpp - no GPU readback on this platform       *
// *************************************************************************
//
// Metal and D3D11 have native readback backends under platform/.
// Elsewhere every read reports "not supported", as renderer_readback.h
// lets it: editor picking and the headless snapshot paths then do nothing.

#if !defined(__APPLE__) && !defined(SOKOL_D3D11)

#include "../renderer_readback.h"

namespace RendererReadback
{

bool ReadPixel(sg_image /*img*/, int32_t /*x*/, int32_t /*y*/, uint8_t /*out_rgba*/[4])
{
    return false;
}

bool ReadRect(sg_image /*img*/, int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/,
              uint8_t* /*out_rgba*/)
{
    return false;
}

bool BeginReadRect(int32_t /*slot*/, sg_image /*img*/, int32_t /*x*/, int32_t /*y*/,
                   int32_t /*w*/, int32_t /*h*/)
{
    return false;
}

bool PollReadRect(int32_t /*slot*/, uint8_t* /*out_rgba*/, int32_t /*out_bytes*/)
{
    return false;
}

} // namespace RendererReadback

#endif

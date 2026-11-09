// *************************************************************************
// *                         Cinematix Revenant                            *
// *                  Revenant Revisited (port) - 2026                     *
// *           renderer_readback.h - GPU-to-CPU pixel readback             *
// *************************************************************************
//
// Pulls a single RGBA8 pixel out of a sokol-managed render target back to
// CPU memory. Used by the editor for the OBJID pixel-pick path. The
// implementation is platform-specific (Metal and D3D11);
// callers must be prepared for a not-implemented return when the build
// targets a backend without a readback helper
// (platform/renderer_readback_stub.cpp).
//
// *************************************************************************

#pragma once

#include <sokol_gfx.h>
#include <cstdint>

namespace RendererReadback {

// Read a single RGBA8 pixel at (x, y) in `img`'s native storage. Returns
// false if the readback isn't supported, if the image
// handle is invalid, or if the request is out of bounds.
//
// Synchronous: blocks until the GPU has finished the most recent frame
// touching `img`. Requires the current frame to have been committed;
// otherwise returns false rather than blocking behind an uncommitted frame.
// Cheap when called rarely (selection clicks).
bool ReadPixel(sg_image img, int32_t x, int32_t y, uint8_t out_rgba[4]);

// Read a rectangular region of `img` as tightly-packed RGBA8 bytes.
// `out_rgba` must point to at least `w * h * 4` bytes. Returns false on
// unsupported backends, invalid handle, or out-of-bounds request. Synchronous —
// blocks until the GPU finishes the most recent committed frame touching `img`.
// Returns false if a render frame is still awaiting sg_commit().
// Used by the headless filmstrip / single-snap PNG output path.
bool ReadRect(sg_image img, int32_t x, int32_t y, int32_t w, int32_t h,
              uint8_t* out_rgba);

// Asynchronous rect read, for a small read every frame without waiting on
// the GPU. BeginReadRect commits a copy of the rect (RGBA8, tightly packed,
// at most kMaxAsyncReadBytes) into `slot`'s staging buffer and returns at
// once; PollReadRect returns true, once, when that copy has finished, with
// the bytes in `out_rgba`. A slot holds one copy at a time: Begin on a busy
// slot fails. Returns false where readback isn't supported.
constexpr int32_t kAsyncReadSlots    = 2;
constexpr int32_t kMaxAsyncReadBytes = 4096;
bool BeginReadRect(int32_t slot, sg_image img, int32_t x, int32_t y,
                   int32_t w, int32_t h);
bool PollReadRect(int32_t slot, uint8_t* out_rgba, int32_t out_bytes);

} // namespace RendererReadback

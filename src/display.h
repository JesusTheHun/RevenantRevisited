// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *               display.h  - EXILE Display Include File                 *
// *************************************************************************
//
// TDisplay is the swapchain / window surface. It wraps sokol's context
// setup, owns the front/back TSurface buffers that legacy blits draw
// into (Put / WriteText / Box / ZPut / ...), and handles FlipPage.
//
// The core render engine -- pipelines, G-buffer, lighting, tile pass,
// composite submission -- now lives in TRenderer (renderer.h). TDisplay
// creates a TRenderer instance during Initialize and delegates the final
// swapchain composite to Renderer->PresentToSwapchain. All new game-side
// code that wants to submit draws should use Renderer-> directly; only
// legacy TSurface-style blits still route through Display..
//
// *************************************************************************

#pragma once

#include "revenant.h"

#include "graphics.h"
#include "surface.h"

#include <functional>
#include <vector>

#define UPDATE_THISFRAME        0x01        // Adds a dirty rectangle update rect for this frame
#define UPDATE_NEXTFRAME        0x02        // Adds a dirty rectangle update rect for the next frame
#define UPDATE_SCREENTOBUFFER   0x04        // Immediately copies the screen rect to the background
#define UPDATE_BUFFERTOSCREEN   0x08        // Immediately copies the background rect to the screen
#define UPDATE_NOMERGERECT      0x10        // Prevents dirty rectangle system from merging this rect

#define UPDATE_RESTORE          (UPDATE_THISFRAME)
#define UPDATE_BACKGROUND       (UPDATE_SCREENTOBUFFER | UPDATE_NEXTFRAME)

_STRUCTDEF(RestoreRect)
_CLASSDEF(TDisplay)

class TDisplay : public TSurface
{
  public:
    TDisplay();
    virtual ~TDisplay();

    // Legacy dirty-rect API -- no-op under sokol_gfx; screens just redraw.
    void AddUpdateRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t flags = 0) {
        (void)x; (void)y; (void)w; (void)h; (void)flags;
    }

    // Clear-zbuffer swap dance from the retail DirectX path. Not needed on
    // sokol (no video-memory zbuffer restriction) -- stubs kept for callers.
    void InitClearZBuffer()  {}
    void CloseClearZBuffer() {}
    bool UsingClearZBuffer() { return savezbuffer != nullptr; }
    TSurface* GetRealZBuffer() { if (savezbuffer) return savezbuffer; else return zbuffer; }

    [[nodiscard]] TSurface* BackBuffer()  const { return backbuffer; }
    [[nodiscard]] TSurface* FrontBuffer() const { return frontbuffer; }

    // Has Initialize() succeeded and Close() not yet been called? Used by
    // call sites that need to gate "do display work" against headless
    // mode and pre-init / post-shutdown phases. Replaces the old
    // `if (Display)` null-check that worked when Display was a pointer
    // global; now that Display is a value-typed global, we ask explicitly.
    [[nodiscard]] bool IsActive() const { return backbuffer != nullptr; }

    // Brings up the sokol_gfx context, builds the TRenderer core engine,
    // wires up ImGui, and allocates the backbuffer/front/z TSurfaces.
    bool Initialize(int32_t dwidth, int32_t dheight, int32_t dbitsperpixel);
    // Called between frames, including after resize and fullscreen changes.
    void UpdateCanvas(int32_t windowWidth, int32_t windowHeight);
    // Tears down everything Initialize brought up (including the TRenderer).
    virtual bool Close();
    // Restore after alt-tab -- legacy stub on sokol.
    bool Restore();

    // Open / close the Overlay2D sokol pass on the backbuffer texture.
    // Every legacy Display.Put / Display.Blit / Display.Line / etc. is
    // a draw into the current sokol pass. Without an open pass they
    // silently no-op. Screens normally don't call these directly --
    // TScreen::DrawFrame brackets the screen's Animate() with them.
    // Idempotent: nested Begin/End calls collapse to a single pass.
    // See docs/FRAME_PIPELINE.md.
    void BeginOverlay();
    void EndOverlay();
    [[nodiscard]] bool OverlayOpen() const { return overlay_pass_open; }

    // Present the frame. Composites Scene3D (from TRenderer) then
    // Overlay2D (this backbuffer) then DebugUI (ImGui) onto the
    // swapchain in that order. ImGui is always rendered last.
    bool FlipPage(bool Wait = true);

    // Reads back the next frame FlipPage presents and hands it to `done`
    // (RGBA8, top row first, display size; null and 0x0 when the readback
    // fails). False when the display isn't up, in which case `done` is never
    // called. Used for save thumbnails. With `hudBelowZ`, the capture holds
    // only the HUD items under that z (the screen's frame is unchanged):
    // the world and HUD panels without the pane tree, say.
    using TCaptureDone = std::function<void(const uint8_t* rgba, int32_t width, int32_t height)>;
    static constexpr float kWholeFrame = 1.0e30f;  // every HUD layer (TRenderer::kAllHudLayers)
    bool RequestCapture(TCaptureDone done, float hudBelowZ = kWholeFrame);
    // Reads back the frame last presented, now, from what it was drawn from
    // (the scene, the overlay and the HUD as they stand); `done` as above.
    // Between frames only: no pass may be open. False when the display
    // isn't up.
    bool CaptureNow(const TCaptureDone& done);
    // Presents the last frame again without drawing it anew: the scene's
    // targets, the overlay and the HUD as they stand, and the debug UI. A
    // playtest host that waits between steps keeps its window live with it.
    void Represent();

    // Headless-capture target: when FrameSnap is active, FlipPage mirrors
    // the swapchain composite into this offscreen sg_image RGBA8 RT so
    // framesnap.cpp can read it via Metal blit (renderer_readback). Lazily
    // allocated on the first FlipPage in snap mode; returns invalid id
    // outside snap mode. Display.IsActive() is the lifetime guard.
    [[nodiscard]] bool     ImGuiReady() const { return imgui_initialized; }
    [[nodiscard]] sg_image SnapCaptureImage() const { return snap_capture_color; }
    [[nodiscard]] int32_t  SnapCaptureWidth()  const { return snap_capture_w; }
    [[nodiscard]] int32_t  SnapCaptureHeight() const { return snap_capture_h; }

  private:
    int32_t currentpage = 0;
    bool    updateenabled = true;
    TSurface* frontbuffer = nullptr;
    TSurface* backbuffer  = nullptr;
    TSurface* zbuffer     = nullptr;
    TSurface* savezbuffer = nullptr;
    bool      imgui_initialized = false;
    bool      overlay_pass_open = false;
    sg_image  snap_capture_color { SG_INVALID_ID };
    int32_t   snap_capture_w = 0;
    int32_t   snap_capture_h = 0;
    struct SCaptureRequest
    {
        TCaptureDone done;
        float        hudBelowZ = kWholeFrame;
    };
    std::vector<SCaptureRequest> capture_requests;

    void MirrorComposite(float hudBelowZ);
};

// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                   cursor.h - Cursor handler file                      *
// *************************************************************************

#pragma once

#include "revenant.h"
#include "renderer.h"  // THudDrawable

extern int32_t cursorx;
extern int32_t cursory;
extern int32_t mousebutton;

void SetMouseBitmap(PTBitmap cursor);

// Enable/disable the hardware (OS) pointer cursor. When ENABLED (default),
// SetMouseBitmap hands the cursor pixels to the OS pointer (NSCursor on macOS)
// so AppKit draws it everywhere — and TCursorHud suppresses its in-frame draw
// to avoid double-drawing. When DISABLED, the OS pointer is hidden
// (sapp_show_mouse(false)) and reset to the default, and TCursorHud draws the
// game cursor bitmap in-frame at (cursorx, cursory) instead. Disable this when
// something needs to drive the cursor position itself — e.g. the scripted
// mouse simulator (--mouse-script) — or to force the classic in-game render.
void SetHardwareCursorEnabled(bool enabled);
bool HardwareCursorEnabled();

void RefreshOSCursor();
  // Re-push the current MouseCursor to the OS pointer. Used after
  // resetting to the OS arrow (e.g. while ImGui captures the mouse)
  // to restore the game cursor when focus returns to the playfield.
void SetCursorInsideWindow(bool inside);
void SetMouseShadow(PTBitmap shadow, int32_t offsetx = 1, int32_t offsety = 3);
void SetMouseCornerBitmap(PTBitmap corner, bool toppriority = false);
void SetMouseCornerBitmap(int32_t type, bool toppriority = false);
void CursorOverObject(TObjectInstance* inst, bool toppriority = false);
void BeginCursorFrame();
  // Each frame, first: the last frame's corner picture and released drag
  // bitmap go (the panes set them again as they animate).

constexpr int32_t kMaxDragBitmapLayers = 64;  // a stacked inventory icon: a pile of 64 coins
struct SDragBitmapLayer
{
    PTBitmap bitmap = nullptr;
    int32_t  x = 0;
    int32_t  y = 0;
};

void SetDragBitmap(PTBitmap drag, int32_t x = 0, int32_t y = 0);
void SetDragBitmapLayers(const SDragBitmapLayer* layers, int32_t count,
                         int32_t x = 0, int32_t y = 0);
void ClearDragBitmap();

void SetDragObj(TObjectInstance* inst);
TObjectInstance* GetDragObj();

// Cursor HUD drawable. Registered by the active runtime mode (game
// mode adds in OnEnter, removes in OnExit). Draws the current mouse
// cursor bitmap at (cursorx, cursory) inside the swapchain pass.
// Conventional z = 0 so HUD panels (z > 0) paint over it.
class TCursorHud : public THudDrawable
{
  public:
    void Draw() override;
};

void DrawMouseShadow();

void RestrictCursor();
void ReleaseCursor();

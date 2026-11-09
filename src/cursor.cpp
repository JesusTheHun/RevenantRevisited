// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                  cursor.cpp - Mouse cursor handling                   * 
// *************************************************************************

#include "cursor.h"

#include "bitmap.h"
#include "bmsurface.h"
#include "platform/cursor.h"
#include "display.h"
#include "imgui.h"
#include "logging.h"
#include "mainwnd.h"
#include "multi.h"
#include "object.h"
#include "playscreen.h"
#include "renderer.h"

#include <sokol_app.h>

// Globals for storing current mouse imagery
PTBitmap MouseCursor = nullptr;
PTBitmap MouseShadow = nullptr;
int32_t shadowoffsetx = 0, shadowoffsety = 0;
PTBitmap MouseCursorAdd = nullptr;     // Little picture that goes in the corner
bool priority;                      // Priority of mouse image

// True once the OS has accepted our cursor pixels via rev_platform::
// SetOSCursor (macOS path). TCursorHud::Draw consults this to suppress
// the in-game main cursor render so we don't double-draw on top of the
// OS pointer. On platforms without the OS handoff it stays false and
// the original in-game draw runs.
static bool g_os_cursor_owns_pixels = false;

// The bitmap the OS cursor shows while it owns the pixels: the arrow (or
// wedge), or the corner bitmap TCursorHud::Draw puts in its place.
static PTBitmap g_os_cursor_shown = nullptr;

// When false, the OS/hardware pointer is suppressed and TCursorHud draws the
// game cursor bitmap in-frame at (cursorx, cursory). Toggled via
// SetHardwareCursorEnabled (see cursor.h). Default: hardware cursor enabled.
static bool g_hw_cursor_enabled = true;
static bool g_cursor_inside_window = true;

void SetCursorInsideWindow(bool inside)
{
    g_cursor_inside_window = inside;
}

PTBitmap DragBitmap = nullptr;
int32_t grabx, graby;
SDragBitmapLayer DragLayers[kMaxDragBitmapLayers] = {};
int32_t DragLayerCount = 0;

TSafeRef<TObjectInstance> DragObj;     // what an item pane holds (retail 0x006563cc)

bool cleardragbitmap = false;

int32_t oldcursorx = WIDTH/2, oldcursory = HEIGHT/2;    // Save cursor pos between activations

// List of cursor types
char *CursorTypes[NUMCURSORTYPES] = { "hand", "eye", "mouth", "door", "stairs", "hourglass", "swords" };


void SetMouseBitmap(PTBitmap cursor)
{
    MouseCursor = cursor;

    if (cursor == GameData->Bitmap("cursor"))
        SetMouseShadow(GameData->Bitmap("cursorshadow"));
    else
    {
        //MouseShadow = nullptr;
        MouseCursorAdd = nullptr;      // no corner bitmaps if not in regular cursor
        priority = true;
    }

    // Hand the new pixels to the OS so AppKit draws our cursor wherever
    // it draws every other app's -- including out-of-window. regx/regy
    // is the registration point Display.Put uses (DM_USEREG), so it's
    // also the right NSCursor hotspot.
    //
    // Gated on Windowed: in fullscreen there's no "outside the window"
    // for the OS cursor to live in, and AppKit hides the cursor over
    // fullscreen apps by default -- which would leave us with no cursor
    // at all. Fall back to the in-game TCursorHud draw + sapp_show_mouse
    // (false) in that case. (macOS-only today; the non-macOS stub also
    // returns false and the same fallback applies.)
    if (cursor && Windowed && g_hw_cursor_enabled)
    {
        g_os_cursor_owns_pixels = rev_platform::SetOSCursor(cursor, cursor->regx, cursor->regy);
    }
    else
    {
        g_os_cursor_owns_pixels = false;
    }
    g_os_cursor_shown = g_os_cursor_owns_pixels ? cursor : nullptr;
    // A declined native handoff uses the in-frame sprite, not an OS arrow.
    sapp_show_mouse(Editor || g_os_cursor_owns_pixels);
}

void SetHardwareCursorEnabled(bool enabled)
{
    g_hw_cursor_enabled = enabled;
    if (enabled)
    {
        // Re-show the OS pointer and re-push the current cursor pixels so the
        // OS owns the pointer again; TCursorHud then suppresses its in-frame
        // draw (RefreshOSCursor sets g_os_cursor_owns_pixels on success).
        RefreshOSCursor();
    }
    else
    {
        // Hide the OS pointer and force the in-frame TCursorHud draw to run.
        rev_platform::ResetOSCursor();
        sapp_show_mouse(false);
        g_os_cursor_owns_pixels = false;
        g_os_cursor_shown = nullptr;
    }
}

bool HardwareCursorEnabled()
{
    return g_hw_cursor_enabled;
}

void RefreshOSCursor()
{
    SetMouseBitmap(MouseCursor);
}

void SetMouseShadow(PTBitmap shadow, int32_t offsetx, int32_t offsety)
{
    MouseShadow = shadow;
    shadowoffsetx = offsetx;
    shadowoffsety = offsety;
}

void SetMouseCornerBitmap(PTBitmap corner, bool toppriority)
{
    if (corner || toppriority)
    {
        MouseCursorAdd = corner;
        priority = toppriority;
    }
}

void SetMouseCornerBitmap(int32_t type, bool toppriority)
{
    if (type != CURSOR_NONE || toppriority)
        SetMouseCornerBitmap(type != CURSOR_NONE ? GameData->Bitmap(CursorTypes[type]) : nullptr);
}

void SetDragBitmap(PTBitmap drag, int32_t x, int32_t y)
{
    DragBitmap = drag;
    grabx = x;
    graby = y;
    DragLayerCount = drag ? 1 : 0;
    for (SDragBitmapLayer& layer : DragLayers)
        layer = SDragBitmapLayer{};
    if (drag)
        DragLayers[0] = { drag, 0, 0 };
}

void SetDragBitmapLayers(const SDragBitmapLayer* layers, int32_t count,
                         int32_t x, int32_t y)
{
    DragBitmap = nullptr;
    grabx = x;
    graby = y;
    DragLayerCount = 0;
    for (SDragBitmapLayer& layer : DragLayers)
        layer = SDragBitmapLayer{};

    if (!layers || count <= 0) return;
    for (int32_t i = 0; i < count && DragLayerCount < kMaxDragBitmapLayers; ++i)
    {
        if (!layers[i].bitmap) continue;
        DragLayers[DragLayerCount++] = layers[i];
        if (!DragBitmap)
            DragBitmap = layers[i].bitmap;
    }
}

void ClearDragBitmap()
{
    cleardragbitmap = true;
}

// REVSYNC: 0x0043a140 -- an item picked up makes its sound (PlayItemSound,
// 0x00473a10); what is held decides the cursor over the map and the panes.
void SetDragObj(TObjectInstance* inst)
{
    DragObj = inst;
    if (inst)
        inst->PlayItemSound();
}

TObjectInstance* GetDragObj()
{
    return DragObj.Get();
}

void DrawMouseShadow(int32_t x, int32_t y, int32_t width, int32_t height)
{
    Display.SetClipRect(x, y, width, height);
    Display.Put(cursorx + shadowoffsetx, cursory + shadowoffsety, MouseShadow, DM_TRANSPARENT | DM_USEREG | DM_ALIAS);
}

void TCursorHud::Draw()
{
    // Renderer invokes this inside the swapchain pass between the
    // 3D scene present and the ImGui debug overlay. We're inside an
    // active pass -- can issue Renderer->DrawBitmap calls directly.
    if (!Renderer || (g_hw_cursor_enabled && (!AppActive || !g_cursor_inside_window))) return;

    // Suppress the game-side overlay while ImGui owns the mouse. The
    // OS cursor still draws the pointer pixel (we reset it to the
    // arrow from runtimemode while ImGui captures); we just don't add
    // drag bitmaps or corner overlays on top.
    if (ImGui::GetIO().WantCaptureMouse)
        return;

    // Ground shadow under wedge cursors (and the regular cursor's
    // own drop-shadow). Drawn FIRST so the cursor / drag bitmap /
    // corner overlay all paint on top of it -- the shadow lives "in"
    // the world layer conceptually (it's the destination marker on
    // the ground plane during right-click walk-to). shadowoffsetx/y
    // come from SetMouseShadow; the wedge bitmaps use (0, 43) so the
    // shadow lands directly below the wedge tip.
    if (MouseShadow)
        Renderer->DrawBitmap(MouseShadow,
                             cursorx + shadowoffsetx,
                             cursory + shadowoffsety,
                             EBitmapDecode::Alias);   // soft shadow lives in the alias RLE buffer

    // Drag bitmap (e.g. inventory item being dragged) goes first so
    // the cursor sits above it. Still drawn here even when the OS owns
    // the cursor pixel -- the drag bitmap is a separate visual asset
    // (the item being moved), not part of the cursor sprite.
    if (DragLayerCount > 0)
    {
        const int32_t originX = cursorx - grabx;
        const int32_t originY = cursory - graby;
        for (int32_t i = 0; i < DragLayerCount; ++i)
        {
            const SDragBitmapLayer& layer = DragLayers[i];
            if (!layer.bitmap) continue;
            Renderer->DrawBitmap(layer.bitmap,
                                 originX + layer.x,
                                 originY + layer.y);
        }
    }

    // Main cursor pixel: only draw it ourselves if the OS didn't take
    // it. On macOS SetMouseBitmap routes through rev_platform::SetOSCursor
    // and g_os_cursor_owns_pixels = true; on other platforms (and the
    // brief window before the first SetMouseBitmap call) we fall back
    // to the in-game draw.
    //
    // The corner bitmap (hand, eye, mouth, door, ...: what the pointer is
    // over) is drawn in place of the arrow, not beside it: retail's cursor
    // draw (0x0043a480) puts the corner bitmap at the pointer when there is
    // one, else the arrow. With the OS drawing the pointer, the OS cursor
    // shows the corner bitmap for as long as it is set.
    PTBitmap const shown = MouseCursorAdd ? MouseCursorAdd : MouseCursor;
    if (shown && shown != g_os_cursor_shown && Windowed && g_hw_cursor_enabled)
    {
        g_os_cursor_owns_pixels = rev_platform::SetOSCursor(shown, shown->regx, shown->regy);
        g_os_cursor_shown = g_os_cursor_owns_pixels ? shown : nullptr;
    }
    if (shown && !g_os_cursor_owns_pixels)
        Renderer->DrawBitmap(shown, cursorx - shown->regx, cursory - shown->regy,
                             EBitmapDecode::PixelsAndAlias);
}

// REVSYNC: the tail of retail's cursor draw (0x0043a480): a released drag
// bitmap goes, and the corner picture and its priority last a frame. The
// port can draw a frame's HUD more than once (the window, then a capture),
// so they end as the next frame begins, before its input.
void BeginCursorFrame()
{
    if (cleardragbitmap)
    {
        SetDragBitmap(nullptr);
        cleardragbitmap = false;
    }

    MouseCursorAdd = nullptr;
    priority = false;
}

void DrawMouseShadow()
{
    if (MouseShadow)
    {
        Display.SetOrigin(0, 0);

        if (MouseShadow != GameData->Bitmap("cursorshadow"))
            DrawMouseShadow(MAPPANEX, MAPPANEY, MAPPANEWIDTH, MAPPANEHEIGHT);
        else
        {
            if (PlayScreen.InCompleteExclusion())
                DrawMouseShadow(0, 0, WIDTH, HEIGHT);
            else
            {
                if (cursorx <= MAPPANEX)
                    DrawMouseShadow(0, 0, MAPPANEX, HEIGHT);
                else if ((cursorx + MouseShadow->width) >= (MAPPANEX + MAPPANEWIDTH))
                    DrawMouseShadow(MAPPANEX + MAPPANEWIDTH, 0, WIDTH - (MAPPANEX + MAPPANEWIDTH), HEIGHT);

                if (cursory <= MAPPANEY)
                    DrawMouseShadow(0, 0, WIDTH, MAPPANEY);
                else if ((cursory + MouseShadow->height) >= (MAPPANEY + MAPPANEHEIGHT))
                    DrawMouseShadow(0, MAPPANEY + MAPPANEHEIGHT, WIDTH, HEIGHT - (MAPPANEY + MAPPANEHEIGHT));
            }
        }

        Display.ResetClipRect();
    }
}

void CursorOverObject(TObjectInstance* inst, bool toppriority)
{
    if (toppriority || !priority)
    {
        int32_t type = CURSOR_NONE;

        if (inst)
            type = inst->CursorType(DragObj.Get());

        SetMouseCornerBitmap(type, toppriority);
    }
}

void RestrictCursor()
{
#if 0 // TODO(port): Win32 cursor clipping; sokol_app has no equivalent yet
    // center the cursor
    RECT r;
    GetClientRect(MainWindow.Hwnd(), &r);
    ClientToScreen(MainWindow.Hwnd(), (LPPOINT)&r);
    SetCursorPos(r.left + oldcursorx, r.top + oldcursory);

    // clip if not windowed
    if (!Windowed || Borderless)
    {
        r.right = r.left + WIDTH;
        r.bottom = r.top + HEIGHT;
        ClipCursor(&r);
    }
#endif
}

void ReleaseCursor()
{
    // save the old position for restore after reactivation
    oldcursorx = cursorx;
    oldcursory = cursory;

#if 0 // TODO(port): Win32 cursor clipping; sokol_app has no equivalent yet
    // clear out the clip rect (cursor can move anywhere)
    ClipCursor(nullptr);
#endif
}


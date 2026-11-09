// *************************************************************************
// *                         Cinematix Revenant                            *
// *                  Revenant Revisited (port) - 2026                     *
// *               runtimemode.cpp - game / editor mode hooks              *
// *************************************************************************

#include "runtimemode.h"

#include "ctrlmap.h"
#include "cursor.h"
#include "gameflow.h"
#include "platform/cursor.h"
#include "imgui.h"
#include "logging.h"
#include "mappane.h"
#include "maprenderer.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "playscreen.h"
#include "playerbot.h"
#include "renderer.h"
#include "revenant.h"
#include "revtypes.h"
#include "uisidebartest.h"

#include <sokol_app.h>

namespace
{

class TGameModeImpl final : public IRuntimeMode
{
  public:
    const char* Name() const override { return "game"; }

    void OnEnter() override
    {
        log_info("[runtimemode] enter game mode");
        // Seed the default game cursor. In windowed mode SetMouseBitmap
        // pushes the pixels to AppKit's NSCursor so the OS handles
        // visibility for us (in/out of window, focus changes, ...).
        // In fullscreen there's no "outside" for the OS cursor, and
        // AppKit hides the cursor over fullscreen apps -- so we hide
        // sokol_app's cursor and let TCursorHud draw the cursor in-game
        // like the pre-OS-cursor path. SetMouseBitmap itself decides
        // (Windowed gate) whether to take the OS path; we just suppress
        // the OS arrow when it won't.
        if (!Windowed)
            sapp_show_mouse(false);
        if (GameData)
        {
            if (PTBitmap cursor = GameData->Bitmap("cursor"))
                SetMouseBitmap(cursor);
            else
                log_warn("[runtimemode] GameData->Bitmap(\"cursor\") returned null");
        }

        // Register the cursor HUD drawable. It no longer renders the
        // main cursor pixel (OS owns that); it still composites the
        // drag bitmap + corner-bitmap overlay on top.
        if (Renderer)
            Renderer->AddHud(&cursor_hud, /*z=*/1000.0f);
    }
    void OnExit() override
    {
        log_info("[runtimemode] exit game mode");
        if (Renderer)
            Renderer->RemoveHud(&cursor_hud);
        // Hand the cursor back to the system default. In windowed mode
        // that's the OS arrow via NSCursor; in fullscreen we re-show
        // sokol_app's cursor (we hid it in OnEnter) so the next mode
        // isn't left with an invisible pointer.
        rev_platform::ResetOSCursor();
        sapp_show_mouse(true);
    }

  private:
    TCursorHud cursor_hud;
    bool       imgui_had_mouse_last_tick = false;  // edge-trigger for OS cursor swap

    void Tick() override
    {
        // While ImGui wants the mouse (debug panels, menus, modal
        // popups) reset the OS cursor to the platform arrow so panel
        // hovers / resizes feel native. When focus returns to the
        // playfield, re-push the current game cursor. TCursorHud::Draw
        // mirrors the same predicate to suppress drag/corner overlays
        // during ImGui ownership.
        const bool imgui_owns_mouse = ImGui::GetIO().WantCaptureMouse;
        if (imgui_owns_mouse != imgui_had_mouse_last_tick)
        {
            if (imgui_owns_mouse)
            {
                rev_platform::ResetOSCursor();
                sapp_show_mouse(true);
            }
            else
                RefreshOSCursor();
            imgui_had_mouse_last_tick = imgui_owns_mouse;
        }
        // Re-assertion to defeat AppKit's auto-revert across tracking-
        // area / view / title-bar crossings is driven from the
        // SAPP_EVENTTYPE_MOUSE_MOVE / MOUSE_ENTER handlers in revmain
        // -- those only fire while the cursor is INSIDE our window, so
        // we don't fight AppKit when the mouse legitimately belongs to
        // another app. Don't re-assert from the tick path -- that
        // forces our cursor on top of the system arrow over the
        // desktop / other apps.

        // Drive player movement from the latest command-flag state.
        // Mouse-click walk-to is the primary input path; keyboard
        // arrows are the secondary fallback. UpdateMove translates
        // CMDFLAG bits into Player->Go / Stop.
        // --playerbot presses the player's keys first, as a player's
        // hands would before the held controls are read.
        PlayerBot.Tick();
        if (PlayScreen.IsControlOn() && !PlayScreen.IsDemoMode())
            PlayScreen.UpdateMove();

        // The map pane's tick (retail's pane pass pulses it before the
        // dialog pane): ambient fades, animation frames, object AI and
        // movement, the camera following its centeron target, then the
        // sector window around the camera's center.
        MapPane.Pulse();
    }

    bool HandleKey(int32_t key, bool down) override
    {
      // Shift+F9: developer reload of the last slot saved or loaded; retail
      // had no quick load. (Quick Save is retail's control, Ctrl+Backspace,
      // and F9 alone is its Belt Use 5, both through the control map below.)
      // Fires on key-down only and consumes the key so it doesn't fall
      // through to the gameplay command path.
        if (down && key == VK_F9 && ShiftDown) {
            GameFlow.Session().RequestReloadLastSlot();
            return true;
        }

      // Pump the global ControlMap: it keeps the held command flags
      // UpdateMove polls each tick (direction, block, leap) and returns the
      // key's command -- its down command on a press, its up command on a
      // release -- which goes to TPlayScreen::Command, as retail's key
      // handler (0x0047c630) passes it to Command (0x0047cf40).
      //
      // Same-key bindings disambiguate by the current control mode:
      // 'S' is "sneak" in normal/sneak mode and "thrust" in combat
      // mode; 'D' is "inventory-drop" in inventory mode and "chop" in
      // combat mode. ControlMap.GetCommand returns the *first* binding
      // whose mode mask meets the one passed, so ControlModeMask passes
      // exactly the active mode — ALLMODES would always pick the earlier entry.
        const int32_t cmd = ControlMap.GetCommand(key, down, PlayScreen.ControlModeMask());

        if (Player && PlayScreen.IsControlOn() && !PlayScreen.IsDemoMode() &&
            cmd != GAMECMD_NONE)
            PlayScreen.Command(static_cast<GAMECOMMAND>(cmd));

      // Returning false lets the rest of the pane stack also see the
      // key (for non-game accelerators like F-keys, dev overlays).
        return false;
    }

    // The play field's mouse belongs to the map pane, as retail's PlayScreen
    // handed it to MapPane (docs/gameflow/forensics/MAP_INPUT.md): the left
    // button uses, picks up, walks to or attacks; the right walks. The play
    // screen sends only what the HUD and modals don't take.
    bool HandleMouseClick(int32_t button, int32_t x, int32_t y) override
    {
        MapPane.MouseClick(button, x, y);
        return true;
    }

    bool HandleMouseMove(int32_t button, int32_t x, int32_t y) override
    {
        MapPane.MouseMove(button, x, y);
        return false;
    }

    // The cursor for what is under the pointer, after the world is drawn:
    // the map's, or the item panes'.
    void Animate() override
    {
        if (PlayScreen.MapHasPointer(cursorx, cursory))
            MapPane.Animate(true);
        else
            AnimateUISidebarItemCursor(cursorx, cursory);
    }
};

class TEditorModeImpl final : public IRuntimeMode
{
  public:
    const char* Name() const override { return "editor"; }

    void OnEnter() override
    {
        log_info("[runtimemode] enter editor mode");
        // Editor uses the platform arrow (with editor tool cursors
        // swapped in via SetMouseBitmap when a specific tool wants
        // one). Reset to the default arrow explicitly so the editor's
        // entry state is independent of whatever game mode left.
        rev_platform::ResetOSCursor();

        sapp_show_mouse(true);

        // Restore the editor's own camera. Otherwise we'd inherit
        // whatever camera position game mode last set (= Locke's last
        // location), which is jarring -- the editor's view should stay
        // wherever the user last left it. First entry of a session
        // skips the restore so the renderer's natural startup centering
        // (BuildAndCenter -> startup_sector pick) is preserved.
        if (cam_saved)
        {
            if (TMapRenderer* mr = PlayScreen.MapRenderer())
                mr->SetCameraWorld(cam_level, cam_world.x, cam_world.y, cam_world.z);
        }
    }
    void OnExit() override
    {
        log_info("[runtimemode] exit editor mode");
        // Snapshot the editor camera so it survives the round-trip
        // through game mode (which retargets the camera to Locke every
        // frame in TPlayScreen::RenderFrame).
        if (TMapRenderer* mr = PlayScreen.MapRenderer())
        {
            cam_world = mr->CameraWorld();
            cam_level = mr->CameraLevel();
            cam_saved = true;
        }
    }

  private:
    S3DPoint cam_world  = {0, 0, 0};
    int32_t  cam_level  = 0;
    bool     cam_saved  = false;

  public:

    // Editor mode owns no game-mode HUD overlay. Editor-specific
    // overlays (gizmos, selection outlines) draw via debugui / the
    // renderer's editor decoration passes, gated on
    // EditorOverlaysEnabled().

    void Tick() override
    {
        // Editor owns its own pulse / camera (Play button + viewport
        // controls). Nothing to drive from the screen layer.
    }

    bool HandleMouseClick(int32_t button, int32_t x, int32_t y) override
    {
        // Editor mouse: forward into the renderer's gizmo / drag picker.
        if (PlayScreen.MapRenderer())
        {
            PlayScreen.MapRenderer()->HandleMouseClick(button, x, y);
            return true;
        }
        return false;
    }
};

TGameModeImpl   g_gameMode;
TEditorModeImpl g_editorMode;
IRuntimeMode*   g_currentMode = &g_gameMode;

} // namespace

IRuntimeMode* CurrentMode() { return g_currentMode; }

void SetCurrentMode(IRuntimeMode* mode)
{
    if (mode == g_currentMode) return;
    if (g_currentMode) g_currentMode->OnExit();
    g_currentMode = mode;
    if (g_currentMode) g_currentMode->OnEnter();
}

IRuntimeMode* GameMode()   { return &g_gameMode;   }
IRuntimeMode* EditorMode() { return &g_editorMode; }

bool EditorOverlaysEnabled() { return g_currentMode == &g_editorMode; }

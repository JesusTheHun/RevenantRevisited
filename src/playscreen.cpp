// *************************************************************************
// *                         Cinematix Revenant                            *
// *                  Revenant Revisited (port) - 2026                     *
// *                  playscreen.cpp - main play-mode screen               *
// *************************************************************************
//
// Rewrite of TPlayScreen on top of the new render path. World rendering
// flows through the owned TMapRenderer (same instance type proven in
// --test=sector); panes composite 2D on top.
//
// Behaviors that lived in the retail TPlayScreen and were dropped because
// they don't fit the new architecture: BITMAP.100 backdrop, DrawOverhangs,
// the postanim/posttext arrays, and the Pulse/Animate -> Display.Put/ZPut/
// PutDim/WriteText draw chain. The legacy code is preserved at
// attic-final:attic/src/playscreen.cpp for reference.
//
// What survived (often as stubs or shims):
//  - Game-time tracking (gameframes, gametime, time-of-day curve).
//  - Mode flags (fullscreen / demo / control).
//  - Save-load entry points (deferred to next Update so we don't tear
//    down state mid-frame).
//  - GAMECOMMAND dispatch + UpdateMove plumbing for player input.
//  - Pane plumbing (AddPane / SetExclusivePane / etc. live on TScreen).
//
// *************************************************************************

#include "combattrace.h"
#include "testconfig.h"
#include "playscreen.h"
#include "shadow/shadowrun.h"
#include "playerbot.h"

#include "audio_backend.h"
#include "dialog.h"

#include <cstring>

#include "3dimage.h"
#include "area.h"
#include "automap.h"
#include "bottombar.h"
#include "buysell.h"
#include "inventory.h"
#include "spellpane.h"
#include "consoleexec.h"
#include "cursor.h"
#include "debugui.h"
#include "display.h"
#include "multi.h"
#include "editor.h"
#include "editorstub.h"
#include "imagery.h"
#include "imgui.h"
#include "ingamemenu.h"
#include "logging.h"
#include "ctrlmap.h"
#include "gamemap.h"
#include "gameoptions.h"
#include "gameflow.h"
#include "hudstate.h"
#include "mapmanager.h"
#include "mappane.h"
#include "maprenderer.h"
#include "risingnumbers.h"
#include "player.h"
#include "savegame.h"
#include "revisited_settings.h"
#include "runtimemode.h"
#include "sector.h"
#include "spell.h"
#include "statusbar.h"
#include "textbar.h"
#include "time.h"
#include "uidragstate.h"
#include "uiequiptest.h"
#include "uihudtest.h"
#include "uisidebartest.h"
#include "spellbook.h"
#include "statpane.h"

#include <vector>

// Cursor bitmaps loaded from gamedata at boot. Other modules
// (automap, editor console, mappane) reference these by symbol; keep
// them as globals so the existing call sites compile unchanged.
PTBitmap PointerCursor = nullptr;
PTBitmap HandCursor    = nullptr;
static bool g_playHudInitialized = false;

static void GetReconstructedPlayfieldRect(int32_t& x, int32_t& y,
                                          int32_t& w, int32_t& h)
{
    const SHudState& s = GetHudState();
    const int32_t dw = Display.Width()  > 0 ? Display.Width()  : WIDTH;
    const int32_t dh = Display.Height() > 0 ? Display.Height() : HEIGHT;
    constexpr int32_t kSidebarW = 188;
    x = 0;
    y = 0;
    w = dw - (s.sidebarState == HUD_SIDEBAR_OPEN ? kSidebarW : 0);
    h = dh - PlayScreen.DrawerHeight();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

// Whether (x, y) is off the map pane: over the side panel or the bottom
// drawer beside it. Retail's map pane is the play field (Pulse 0x0047b4d0
// sizes it to the display less both), and is offered a press only inside it
// (TScreen::MouseClick 0x004904dd) and picks its hover cursor only there
// (TMapPane::Animate 0x004544f6..0x00454527). The panes over the play field
// (the side tabs, the status bar, the text bar) are inside it.
static bool IsOffMapPane(int32_t x, int32_t y)
{
    int32_t px = 0, py = 0, pw = 0, ph = 0;
    GetReconstructedPlayfieldRect(px, py, pw, ph);
    return x < px || y < py || x >= px + pw || y >= py + ph;
}

// The control modes a binding works in (retail's mode bits).
static constexpr uint32_t CTRL_NORMALMODE    = 1;
static constexpr uint32_t CTRL_COMBATMODE    = 2;
static constexpr uint32_t CTRL_BOWMODE       = 4;
static constexpr uint32_t CTRL_SNEAKMODE     = 8;

// REVSYNC: retail's control table 0x005d5500 (69 entries of 0x4c, count at
// 0x005d697c): the order, names, INI ids ([Controls]), modes and default
// keys as the release builds them -- what the Options pane lists and the
// shipped Revenant.ini binds. Each control's command is the port's of the
// same meaning (retail's numbers in GAMECOMMAND); the ones Command doesn't
// handle yet do nothing. (The port's table used to be the 1998 build's:
// another order, ids such as SidePanel, and controls retail dropped.)
static SControlEntry g_defaultGameControls[] =
{
    {"Invoke 1", "Invoke1", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F1}}, GAMECMD_INVOKE1, 0, 0, false},
    {"Invoke 2", "Invoke2", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F2}}, GAMECMD_INVOKE2, 0, 0, false},
    {"Invoke 3", "Invoke3", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F3}}, GAMECMD_INVOKE3, 0, 0, false},
    {"Invoke 4", "Invoke4", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F4}}, GAMECMD_INVOKE4, 0, 0, false},
    {"Combat Combo 1", "CombatCombo1", CTRL_COMBATMODE, {{VK_CONTROL, 'A'}}, GAMECMD_COMBO1, 0, 0, false},
    {"Combat Combo 2", "CombatCombo2", CTRL_COMBATMODE, {{VK_CONTROL, 'S'}}, GAMECMD_COMBO2, 0, 0, false},
    {"Combat Combo 3", "CombatCombo3", CTRL_COMBATMODE, {{VK_CONTROL, 'D'}}, GAMECMD_COMBO3, 0, 0, false},
    {"Combat Combo 4", "CombatCombo4", CTRL_COMBATMODE, {{VK_CONTROL, 'Q'}}, GAMECMD_COMBO4, 0, 0, false},
    {"Combat Combo 5", "CombatCombo5", CTRL_COMBATMODE, {{VK_CONTROL, 'W'}}, GAMECMD_COMBO5, 0, 0, false},
    {"Combat Combo 6", "CombatCombo6", CTRL_COMBATMODE, {{VK_CONTROL, 'E'}}, GAMECMD_COMBO6, 0, 0, false},
    {"Combat Combo 7", "CombatCombo7", CTRL_COMBATMODE, {{'Q'}}, GAMECMD_COMBO7, 0, 0, false},
    {"Combat Combo 8", "CombatCombo8", CTRL_COMBATMODE, {{'W'}}, GAMECMD_COMBO8, 0, 0, false},
    {"Combat Combo 9", "CombatCombo9", CTRL_COMBATMODE, {{'E'}}, GAMECMD_COMBO9, 0, 0, false},
    {"Combat Combo 10", "CombatCombo10", CTRL_COMBATMODE, {{VK_MENU, 'Q'}}, GAMECMD_COMBO10, 0, 0, false},
    {"Combat Combo 11", "CombatCombo11", CTRL_COMBATMODE, {{VK_MENU, 'W'}}, GAMECMD_COMBO11, 0, 0, false},
    {"Combat Combo 12", "CombatCombo12", CTRL_COMBATMODE, {{VK_MENU, 'E'}}, GAMECMD_COMBO12, 0, 0, false},
    {"Belt Use 1", "BeltUse1", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F5}}, GAMECMD_BELTUSE1, 0, 0, false},
    {"Belt Use 2", "BeltUse2", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F6}}, GAMECMD_BELTUSE2, 0, 0, false},
    {"Belt Use 3", "BeltUse3", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F7}}, GAMECMD_BELTUSE3, 0, 0, false},
    {"Belt Use 4", "BeltUse4", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F8}}, GAMECMD_BELTUSE4, 0, 0, false},
    {"Belt Use 5", "BeltUse5", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{VK_F9}}, GAMECMD_BELTUSE5, 0, 0, false},
    {"Combat Mode", "CombatMode", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{'C'}}, GAMECMD_COMBAT, 0, 0, false},
    {"Bow Mode", "BowMode", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{'B'}}, GAMECMD_BOW, 0, 0, false},
    {"Full Screen", "FullScreen", ALLMODES, {{VK_SPACE}}, GAMECMD_FULLSCREEN, 0, 0, false},
    {"Bottom Pane", "BottomPane", ALLMODES, {{VK_TAB}}, GAMECMD_BOTTOMPANEL, 0, 0, false},
    {"Side Pane", "SidePane", ALLMODES, {{VK_RWIN}}, GAMECMD_SIDEPANEL, 0, 0, false},
    {"Show Spells", "Spells", ALLMODES, {{'1'}}, GAMECMD_SHOWSPELLS, 0, 0, false},
    {"Show Stats", "Stats", ALLMODES, {{'2'}}, GAMECMD_SHOWSTATS, 0, 0, false},
    {"Equip Character", "Equip", ALLMODES, {{'3'}}, GAMECMD_SHOWEQUIP, 0, 0, false},
    {"Build Spells", "BuildSpell", ALLMODES, {{'4'}}, GAMECMD_BUILDSPELLS, 0, 0, false},
    {"Show Inventory", "Invent", ALLMODES, {{'5'}}, GAMECMD_INVENTORY, 0, 0, false},
    {"Show Map", "Map", ALLMODES, {{'6'}}, GAMECMD_SHOWMAP, 0, 0, false},
    {"Run", "Run", CTRL_NORMALMODE | CTRL_COMBATMODE | CTRL_BOWMODE, {{'R'}}, GAMECMD_MOVEDOWN, GAMECMD_MOVEUP, CMDFLAG_RUN, false},
    {"Use", "Use", CTRL_NORMALMODE, {{'U'}, {'T'}}, GAMECMD_USE, 0, 0, false},
    {"Get", "Get", CTRL_NORMALMODE, {{'G'}}, GAMECMD_GET, 0, 0, false},
    {"Combat Get", "CombatGet", CTRL_COMBATMODE | CTRL_BOWMODE, {{'G'}}, GAMECMD_GET, 0, 0, false},
    {"Combat Block", "CombatBlock", CTRL_COMBATMODE, {{'F'}}, GAMECMD_BLOCKDOWN, GAMECMD_BLOCKUP, CMDFLAG_BLOCK, false},
    {"Combat Dodge Left", "DodgeLeft", CTRL_COMBATMODE, {{VK_CONTROL, VK_LEFT}, {VK_JOYLEFT | VK_DBLTAP}}, GAMECMD_DODGELEFT, 0, 0, false},
    {"Combat Dodge Right", "DodgeRight", CTRL_COMBATMODE, {{VK_CONTROL, VK_RIGHT}, {VK_JOYRIGHT | VK_DBLTAP}}, GAMECMD_DODGERIGHT, 0, 0, false},
    {"Combat Dodge Up", "DodgeUp", CTRL_COMBATMODE, {{VK_CONTROL, VK_UP}, {VK_JOYUP | VK_DBLTAP}}, GAMECMD_DODGEUP, 0, 0, false},
    {"Combat Dodge Down", "DodgeDown", CTRL_COMBATMODE, {{VK_CONTROL, VK_DOWN}, {VK_JOYDOWN | VK_DBLTAP}}, GAMECMD_DODGEDOWN, 0, 0, false},
    {"Combat Dodge UpLeft", "DodgeUpLeft", CTRL_COMBATMODE, {{VK_CONTROL, VK_HOME}, {VK_JOYUPLEFT | VK_DBLTAP}}, GAMECMD_DODGEUPLEFT, 0, 0, false},
    {"Combat Dodge UpRight", "DodgeUpRight", CTRL_COMBATMODE, {{VK_CONTROL, VK_PRIOR}, {VK_JOYUPRIGHT | VK_DBLTAP}}, GAMECMD_DODGEUPRIGHT, 0, 0, false},
    {"Combat Dodge DownLeft", "DodgeDownLeft", CTRL_COMBATMODE, {{VK_CONTROL, VK_END}, {VK_JOYDOWNLEFT | VK_DBLTAP}}, GAMECMD_DODGEDOWNLEFT, 0, 0, false},
    {"Combat Dodge DownRight", "DodgeDownRight", CTRL_COMBATMODE, {{VK_CONTROL, VK_NEXT}, {VK_JOYDOWNRIGHT | VK_DBLTAP}}, GAMECMD_DODGEDOWNRIGHT, 0, 0, false},
    {"Combat Swing", "CombatSwing", CTRL_COMBATMODE, {{'A'}}, GAMECMD_SWING, 0, 0, false},
    {"Combat Thrust", "CombatThrust", CTRL_COMBATMODE, {{'S'}}, GAMECMD_THRUST, 0, 0, false},
    {"Combat Chop", "CombatChop", CTRL_COMBATMODE, {{'D'}}, GAMECMD_CHOP, 0, 0, false},
    {"Bow Shoot", "BowShoot", CTRL_BOWMODE, {{'Z'}}, GAMECMD_BOWAIM, GAMECMD_BOWSHOOT, 0, false},
    {"Zoom In", "ZoomIn", ALLMODES, {{'I'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_ZOOMIN, false},
    {"Zoom Out", "ZoomOut", ALLMODES, {{'K'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_ZOOMOUT, false},
    {"Pan Left", "PanLeft", ALLMODES, {{'J'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_PANLEFT, false},
    {"Pan Right", "PanRight", ALLMODES, {{'L'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_PANRIGHT, false},
    {"Pan Up", "PanUp", ALLMODES, {{'Y'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_PANUP, false},
    {"Pan Down", "PanDown", ALLMODES, {{'H'}}, GAMECMD_CAMERADOWN, GAMECMD_CAMERAUP, CMDFLAG_PANDOWN, false},
    {"Left", "Left", ALLMODES, {{VK_LEFT}, {VK_JOYLEFT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_LEFT, false},
    {"Right", "Right", ALLMODES, {{VK_RIGHT}, {VK_JOYRIGHT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_RIGHT, false},
    {"Up", "Up", ALLMODES, {{VK_UP}, {VK_JOYUP}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_UP, false},
    {"Down", "Down", ALLMODES, {{VK_DOWN}, {VK_JOYDOWN}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_DOWN, false},
    {"Up Left", "UpLeft", ALLMODES, {{VK_HOME}, {VK_JOYUPLEFT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_UPLEFT, false},
    {"Up Right", "UpRight", ALLMODES, {{VK_PRIOR}, {VK_JOYUPRIGHT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_UPRIGHT, false},
    {"Down Left", "DownLeft", ALLMODES, {{VK_END}, {VK_JOYDOWNLEFT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_DOWNLEFT, false},
    {"Down Right", "DownRight", ALLMODES, {{VK_NEXT}, {VK_JOYDOWNRIGHT}}, GAMECMD_DIRDOWN, GAMECMD_DIRUP, CMDFLAG_DOWNRIGHT, false},
    {"Multiplayer", "Multiplayer", ALLMODES, {{'M'}}, GAMECMD_MULTIPLAYER, 0, 0, false},
    {"Edit Char", "EditChar", ALLMODES, {{'X'}}, GAMECMD_EDITCHAR, 0, 0, false},
    {"Game Options", "GameOpts", ALLMODES, {{'O'}}, GAMECMD_GAMEOPTIONS, 0, 0, false},
    {"Load Game", "LoadGame", ALLMODES, {{VK_CONTROL, VK_LWIN}}, GAMECMD_LOADGAME, 0, 0, false},
    {"Save Game", "SaveGame", ALLMODES, {{VK_CONTROL, VK_APPS}}, GAMECMD_SAVEGAME, 0, 0, false},
    {"Quick Save", "QuickSave", ALLMODES, {{VK_CONTROL, VK_BACK}}, GAMECMD_QUICKSAVE, 0, 0, false},
};

// REVSYNC: 0x00486177 / 0x00486186 -- retail initialized the control map
// from its table at boot and then read the player's bindings from
// [Controls] (each key written back); the Options pane's OK saves them
// (0x00439dc0). A binding the INI lacks keeps the table's.
void InitDefaultControlMap()
{
    if (ControlMap.NumControls() > 0)
        return;   // already populated (e.g. TPlayScreen ran first)
    ControlMap.Initialize(int32_t(sizearray(g_defaultGameControls)),
                          g_defaultGameControls);
    ControlMap.Load(const_cast<char*>("Controls"));
}

// REVSYNC: TPlayScreen::Animate @ 0x0047c2c0 (0x0047c3a5..0x0047c3de) --
// the minute of the day: game time (hundredths of a second) times 1440 over
// rules.def's DAYLENGTH, modulo 1440, in 64 bits. DAYLENGTH 80000 makes a
// day 800 seconds of play.
static int32_t TimeOfDayMinutes(int32_t gametime)
{
  // REVSYNC-DIVERGENCE: retail divides by zero before rules.def is loaded.
    if (Rules.daylength <= 0)
        return 0;
    return int32_t(int64_t(gametime) * 1440 / Rules.daylength % 1440);
}

// The day clock: rules.def's DAYLENGTH units make one day of 1440 game
// minutes. Both truncate toward zero; the products wrap at 32 bits as
// retail's imul does.
// REVSYNC: ConvertFramesToMinutes @ 0x0047eb30 -- frames * 1440 / daylength
int32_t ConvertFramesToMinutes(int32_t frames)
{
    const auto day = static_cast<int32_t>(static_cast<uint32_t>(frames) * 1440u);
  // REVSYNC-DIVERGENCE: retail divides by zero before rules.def is loaded.
    return Rules.daylength ? day / Rules.daylength : 0;
}

// REVSYNC: ConvertMinutesToFrames @ 0x0047eb50 -- minutes * daylength / 1440
int32_t ConvertMinutesToFrames(int32_t minutes)
{
    const auto product = static_cast<int32_t>(static_cast<uint32_t>(minutes) * static_cast<uint32_t>(Rules.daylength));
    return product / 1440;
}

// *************************************************************************
// * Construction / lifetime                                               *
// *************************************************************************

TPlayScreen::TPlayScreen()
    : ingamemenu(std::make_unique<TInGameMenu>(*this))
    , statusbar(std::make_unique<TPlyrStatusBar>())
    , bottombar(std::make_unique<TBottomBarPane>())
{
}
TPlayScreen::~TPlayScreen() = default;

bool TPlayScreen::Initialize()
{
    log_info("[playscreen] initialize start");
    // TScreen's base Initialize() returns false (it's a "must override"
    // hook); skip it and do our own setup.

    // Present the session's world: the renderer draws MapManager's current
    // map and follows it when a load or level change replaces it.
    mapRenderer = std::make_unique<TMapRenderer>();
    mapRenderer->Initialize();
    MapPane.SetMapView(mapRenderer.get());
    risingNumbers = std::make_unique<TRisingNumberLayer>();
    if (Renderer)
        Renderer->AddHud(risingNumbers.get(), TRisingNumberLayer::kHudZ);
    BindWorld();
    mapListener = MapManager.AddListener([this](EMapManagerEvent, TMapManager*) { BindWorld(); });
    // Push global Revisited point-light multipliers now that MapRenderer
    // is live. Per-area POINTLIGHTINT / POINTLIGHTRANGE will compose on
    // top of these each time TArea::Enter fires.
    ApplyRevisitedSettingsToMapRenderer(mapRenderer.get());
    log_info("[playscreen] map renderer ready");

    // Cache common effect imagery for damage/spell overlays. Optional --
    // missing imagery is logged but doesn't gate boot.
    auto load = [](const char* path) -> TObjectImagery* {
        const int32_t id = TObjectImagery::FindImagery(path);
        if (id < 0) return nullptr;
        return TObjectImagery::LoadImagery(id);
    };
    bloodimagery  = load("Misc\\Blood.I3D");
    sparksimagery = load("Misc\\Sparks.I3D");

    // Default keyboard bindings. Direction keys + Run / Sneak; classic
    // arrow + numpad-diagonal layout. UpdateMove synthesizes diagonals
    // from adjacent cardinals so laptop users without a Home/PgUp/End/
    // PgDn cluster still get full 8-way movement.
    InitDefaultControlMap();

    EditorLoadState();

    // Retail TPlayScreen::Initialize adds the dialog pane right after the
    // map pane; the screen's pane pass pulses it after the world each tick.
    if (!DialogPane.Initialize())
        log_error("[playscreen] Trouble initializing dialog pane");
    AddPane(&DialogPane);

    // Runtime mode owns mode-specific UI state (cursor, overlay
    // visibility, etc.). At static init g_currentMode defaults to game
    // mode, but OnEnter() is only invoked by SetCurrentMode for
    // *transitions* -- so the initial mode never gets its setup hook
    // fired. Invoke it explicitly here so the game cursor (and any
    // future game-mode init) is in place from the first frame.
    if (CurrentMode())
        CurrentMode()->OnEnter();

    // spell.def, reloaded for each game as retail's Initialize does
    // (0x0047add4: clear, then load); the spell panes and casting read it.
    SpellList.Close();
    if (!SpellList.Initialize())
        log_warn("[playscreen] spell.def failed to load; no spells");

    // REVSYNC: 0x0047ae4c..0x0047ae7a, 0x0047ebc0(1) -- every entry into play
    // (a new game, or a load from the title or death screen) shows the side
    // panel open on Equip and Inventory, and the bottom bar. A save's HUD
    // words never come back (TPlayer::Load keeps them at +0x304, unread), and
    // an in-game load, which runs in the pulse (0x0047bf01..0x0047bfd9), leaves
    // the HUD as it was.
    SHudState& hud = GetHudState();
    hud.topSlot       = HUD_TOP_EQUIP;
    hud.bottomSlot    = HUD_BOT_INV;
    hud.sidebarState  = HUD_SIDEBAR_OPEN;
    hud.bottomBarOpen = 1;

    SetUIHudCursorOverlayEnabled(false);
    SetUISidebarSyntheticStateEnabled(false);
    g_playHudInitialized = InitializeUIHudMode();
    log_info("[playscreen] reconstructed HUD init = %s",
             g_playHudInitialized ? "OK" : "FAIL");

    // REVSYNC: 0x0047abf8 / 0x0047adab -- the text bar, added after the side
    // tabs, so it draws over the dialog entries, then the player status bar.
    // After the HUD, so they anchor to the HUD's map view.
    if (!TextBar.Initialize())
        log_error("[playscreen] Trouble initializing text bar");
    AddPane(&TextBar);
    if (!statusbar->Initialize())
        log_error("[playscreen] Trouble initializing the status bar");
    AddPane(statusbar.get());

    // REVSYNC: 0x0047ac72 -- the bottom bar, which opens with the HUD's
    // lower panel along the bottom of the map view (UpdateBottomBar).
    if (bottombar->Initialize())
    {
        AddPane(bottombar.get());
        bottombar->Hide();
        UpdateBottomBar();
    }
    else
        log_error("[playscreen] Trouble initializing the bottom bar");

    // REVSYNC: 0x0047ab8f -- the inventory, in the side panel's lower slot
    // while the side panel shows it (UpdateSidePanes). Its container is the
    // main player's (BindMainPlayerToScreen, below).
    if (Inventory.Initialize())
    {
        AddPane(&Inventory);
        Inventory.Hide();
        UpdateSidePanes();
    }
    else
        log_error("[playscreen] Trouble initializing the inventory");

    // REVSYNC: 0x0047a660 ("Trouble initializing spell pane") -- the spell
    // pane, in the same slot while the side panel shows the spell tab.
    if (SpellPane.Initialize())
    {
        AddPane(&SpellPane);
        SpellPane.Hide();
        UpdateSidePanes();
    }
    else
        log_error("[playscreen] Trouble initializing the spell pane");

    // The stats (0x00546b50, after the spell pane), in the side panel's upper
    // slot while the side panel shows the stats tab: the main player's, or an
    // examined object's.
    if (StatPane.Initialize())
    {
        AddPane(&StatPane);
        StatPane.Hide();
        UpdateSidePanes();
    }
    else
        log_error("[playscreen] Trouble initializing the stat pane");

    // The spell book (0x005449e0, after the stats), in the side panel's
    // upper slot while the side panel shows the book tab. Its rows are the
    // main player's known spells.
    if (SpellBook.Initialize())
    {
        AddPane(&SpellBook);
        SpellBook.Hide();
        UpdateSidePanes();
    }
    else
        log_error("[playscreen] Trouble initializing the spell book");

    // REVSYNC: 0x0047a660 -- the automap, in the same slot while the side
    // panel shows the map tab. It loads AutoMap.dat from resources.rvr,
    // mounted by now. The main player's explored area comes with
    // BindMainPlayerToScreen.
    if (AutoMap.Initialize())
    {
        AddPane(&AutoMap);
        AutoMap.Hide();
        UpdateSidePanes();
    }
    else
        log_error("[playscreen] Trouble initializing the automap");

    // The session made the main player under the loading screen: the panes
    // show it now that they exist.
    PlayerManager.BindMainPlayerToScreen();

    // REVSYNC: 0x0047b10c -- the fader, starting black, outside the
    // editor. TScreen fades it in once Initialize returns.
    screenfade.Setup(TScreenFade::kDefaultSteps);
    fade = Editor ? nullptr : &screenfade;

    log_info("[playscreen] initialize done");
    return true;
}

void TPlayScreen::BindWorld()
{
    if (!mapRenderer)
        return;

    // The renderer anchors on the player when it is on the map's level;
    // the camera's sector is the fallback.
    S3DPoint center;
    MapPane.GetMapPos(center);
    mapRenderer->SetMap(MapManager.CurrentMap(), /*use_level_origin=*/false,
                        center.x >> SECTORWSHIFT, center.y >> SECTORHSHIFT);
}

// Hand-rolled starter loadout for the editor's default Locke (games start
// from newgame.sav, which brings its own). Each entry: (item name as known to
// class.def, target equipment slot). Name lookup scans every
// TObjectClass so we don't have to hand-pick OBJCLASS_WEAPON vs
// OBJCLASS_ARMOR per row. Order matters: PRIMEHAND first so the
// combat-root re-resolution in TPlayer::Equip has its target before
// the body armor decoration lands on top.
// Item names per the runtime class.def packed in imagery.rvi (not the
// older legacy/Class.Def at repo root, which uses different identifiers).
// Brown Cloth set is the unarmored peasant look Locke wakes up in.
struct SStarterItem { const char *name; int32_t slot; };
static const SStarterItem kStarterLoadout[] = {
    { "Short Sword",         EQ_PRIMEHAND },
    { "Brown Cloth Shirt",   EQ_BODY      },
    { "Brown Cloth Pants",   EQ_LEGS      },
    { "Brown Cloth Boots",   EQ_FEET      },
};

static void EquipStarterLoadout(TPlayer *p)
{
    if (!p) return;
    for (const SStarterItem &row : kStarterLoadout)
    {
        TObjectClass *cl = nullptr;
        int32_t ot = -1;
        for (int32_t i = 0; i < MAXOBJECTCLASSES && !cl; ++i)
        {
            TObjectClass *c = TObjectClass::GetClass(i);
            if (!c) continue;
            const int32_t t = c->FindObjType(row.name);
            if (t >= 0) { cl = c; ot = t; }
        }
        if (!cl)
        {
            log_warn("[player] starter: no class.def type named '%s'", row.name);
            continue;
        }

        SObjectDef def;
        memset(&def, 0, sizeof(def));
        def.objclass = cl->ClassId();
        def.objtype  = ot;
        p->GetPos(def.pos);    // placeholder; AddToInventory reparents

        TObjectInstance *inst = cl->NewObject(&def);
        if (!inst)
        {
            log_warn("[player] starter: NewObject('%s') failed", row.name);
            continue;
        }
        if (!p->AddToInventory(inst))
        {
            log_warn("[player] starter: AddToInventory('%s') failed", row.name);
            continue;
        }
        if (!p->Equip(inst, row.slot))
        {
            log_warn("[player] starter: Equip('%s', slot=%d) refused "
                     "(EqSlot mismatch or missing combat ani)",
                     row.name, row.slot);
            continue;
        }
        log_info("[player] starter: equipped '%s' in slot %d", row.name, row.slot);
    }
}

// Drop a "Locke" TPlayer into the loaded sector at (level, sx, sy) and
// register with PlayerManager. Called from inside the TMapRenderer
// post-load hook so the player exists in the sector before the
// renderer scans sector contents into its drawable list. Skipped if a
// Player already exists (loaded from a save).
bool TPlayScreen::SpawnDefaultPlayer(int32_t level, int32_t sx, int32_t sy)
{
    if (Player)
        return true;
    if (!mapRenderer)
        return false;

    TSector* sec = mapRenderer->FindLoadedSector(level, sx, sy);
    if (!sec)
    {
        log_warn("[player] anchor sector %d_%d_%d not loaded; cannot spawn Locke",
                 level, sx, sy);
        return false;
    }

    const int32_t locke_type = PlayerClass.FindObjType("Locke");
    if (locke_type < 0)
    {
        log_error("[player] PlayerClass has no 'Locke' objtype");
        return false;
    }

    // Sector-center is the Misthaven fountain at the default anchor.
    // Offset northwest of center onto open plaza paving so Locke isn't
    // stuck on / against the fountain geometry. Picked empirically --
    // TODO: replace with a real start-of-game position once save-load
    // / a NewGame data path is wired up.
    SObjectDef def;
    memset(&def, 0, sizeof(def));
    def.objclass = OBJCLASS_PLAYER;
    def.objtype  = locke_type;
    def.level    = level;
    def.pos.x    = (sx << SECTORWSHIFT) + (SECTORWIDTH  / 2) - 128;
    def.pos.y    = (sy << SECTORHSHIFT) + (SECTORHEIGHT / 2) - 128;
    // Read the floor height from the sector walkmap so Locke spawns
    // on top of the terrain instead of beneath it. MapPane's level
    // window doesn't follow Locke yet (paging not wired); read off
    // the renderer-owned sector directly.
    {
        const int32_t local_x = (def.pos.x & (SECTORWIDTH  - 1)) >> WALKMAPSHIFT;
        const int32_t local_y = (def.pos.y & (SECTORHEIGHT - 1)) >> WALKMAPSHIFT;
        def.pos.z = sec->ReturnWalkmap(local_x, local_y);
        log_info("[player] spawn: world=(%d,%d,%d) sector=(%d_%d_%d) "
                 "walkmap[%d,%d]=%d",
                 def.pos.x, def.pos.y, def.pos.z, level, sx, sy,
                 local_x, local_y, def.pos.z);

        // One-shot Z reference probes: walk every loaded sector of the current
        // map and log up to two example pos.z values -- one from any
        // OBJCLASS_CHARACTER (NPC / monster, since monsters are CHARACTER too),
        // one from the first non-tile static prop. Both render at correct Z
        // visually, so comparing their stored pos.z + walkmap-under-feet to
        // ours tells us whether Locke's spawn uses the wrong reference value.
        if (TGameMap* gm = MapManager.CurrentMap())
        {
            int32_t char_logged = 0, prop_logged = 0;
            for (TSector* probe_sec : gm->Sectors())
            {
                if (!probe_sec) continue;
                if (char_logged && prop_logged) break;
                for (int32_t i = 0; i < probe_sec->NumItems(); ++i)
                {
                    TObjectInstance* probe = probe_sec->GetInstance(i);
                    if (!probe) continue;
                    const int32_t cls = probe->ObjClass();
                    const bool is_char = (cls == OBJCLASS_CHARACTER);
                    const bool is_prop = (cls != OBJCLASS_TILE &&
                                          cls != OBJCLASS_EXIT &&
                                          cls != OBJCLASS_EFFECT &&
                                          cls != OBJCLASS_LIGHTSOURCE &&
                                          cls != OBJCLASS_HELPER &&
                                          cls != OBJCLASS_PLAYER &&
                                          cls != OBJCLASS_CHARACTER);
                    if (!is_char && !is_prop) continue;
                    if (is_char && char_logged) continue;
                    if (is_prop && prop_logged) continue;
                    S3DPoint pp; probe->GetPos(pp);
                    const int32_t plx = (pp.x & (SECTORWIDTH  - 1)) >> WALKMAPSHIFT;
                    const int32_t ply = (pp.y & (SECTORHEIGHT - 1)) >> WALKMAPSHIFT;
                    const int32_t pwalk = probe_sec->ReturnWalkmap(plx, ply);
                    log_info("[player] z-probe %s '%s' cls=%d pos=(%d,%d,%d) "
                             "sector=(%d_%d) walkmap[%d,%d]=%d delta(z-walk)=%d",
                             is_char ? "CHAR" : "PROP",
                             probe->GetTypeName() ? probe->GetTypeName() : "?",
                             cls, pp.x, pp.y, pp.z,
                             probe_sec->SectorX(), probe_sec->SectorY(),
                             plx, ply, pwalk, pp.z - pwalk);
                    if (is_char) ++char_logged;
                    else         ++prop_logged;
                    if (char_logged && prop_logged) break;
                }
            }
            if (!char_logged) log_info("[player] z-probe: no OBJCLASS_CHARACTER found in current map");
            if (!prop_logged) log_info("[player] z-probe: no static prop found in current map");
        }
    }

    TObjectInstance* oi = PlayerClass.NewObject(&def);
    if (!oi)
    {
        log_error("[player] PlayerClass.NewObject failed for Locke");
        return false;
    }

    // The Aggressive stat defaults to 1 in DEFOBJSTAT(Character, Aggressive,
    // ..., 1, 0, 1). The retail rules.def CHAR entry for the player class
    // overrode that to 0; with the fallback SCharData (no rules.def CHAR
    // entries) Locke inherits the default and spawns in "combat" root.
    // That gates all movement on combat-walk animations and effectively
    // freezes him. Force walk-mode by clearing Aggressive and re-running
    // the character init so DefaultRootState resolves to "walk".
    TCharacter* c = (TCharacter*)oi;
    c->SetAggressive(0);
    c->ClearChar();


    sec->AddObject(oi);

    // SetMainPlayer fires UI side effects (CenterOnObj / RefreshEquip /
    // Inventory) when CurrentScreen == &PlayScreen.
    // We are inside TPlayScreen::Initialize -- CurrentScreen is already
    // set, but the UI panes those calls touch are not yet initialized,
    // and one of them hangs. Clear CurrentScreen for the duration of
    // the spawn so the UI hookups are skipped; the player still becomes
    // the main player, just without the per-screen redraws.
    TScreen* saved_screen = CurrentScreen;
    CurrentScreen = nullptr;
    PlayerManager.AddPlayer((TPlayer*)oi);
    PlayerManager.SetMainPlayer((TPlayer*)oi);
    EquipStarterLoadout((TPlayer*)oi);
    CurrentScreen = saved_screen;

    log_info("[player] spawned Locke in sector %d_%d_%d at world (%d,%d,%d)",
             level, sx, sy, def.pos.x, def.pos.y, def.pos.z);

    // One-shot stat dump so we can see what ClearPlayer produced and
    // confirm the rules.def/char.def + parser fix actually populated
    // Locke's stats correctly.
    {
        TPlayer *p = (TPlayer*)oi;
        const SCharData *cd  = p->GetCharData();
        const SClassData *kd = (cd && cd->classdata) ? cd->classdata : nullptr;
        if (cd)
        {
            for (int32_t i = 0; i < cd->attacks.NumItems() && i < 6; i++)
            {
                const SCharAttackData &a = cd->attacks[i];
                log_info("[player-attacks] [%d] name='%s' btn=%d flags=0x%x "
                         "mindist=%d maxdist=%d fatigue=%d weaponmask=0x%x",
                         i, a.attackname, (int)a.button, (unsigned)a.flags,
                         (int)a.mindist, (int)a.maxdist,
                         (int)a.fatigue, (unsigned)a.weaponmask);
            }
        }
        log_info("[player-stats] Lv=%d STR=%d CON=%d AGI=%d RFL=%d MND=%d LCK=%d "
                 "H=%d/%d F=%d/%d M=%d/%d "
                 "chardata=%s classdata=%s "
                 "fatiguemod=%d healthmod=%d manamod=%d "
                 "statreqs=[%d,%d,%d,%d,%d,%d]",
                 (int)p->Level(),
                 (int)p->Strn(), (int)p->Cons(), (int)p->Agil(),
                 (int)p->Rflx(), (int)p->Mind(), (int)p->Luck(),
                 (int)p->Health(), (int)p->MaxHealth(),
                 (int)p->Fatigue(), (int)p->MaxFatigue(),
                 (int)p->Mana(), (int)p->MaxMana(),
                 cd ? cd->name : "(null)",
                 kd ? "yes" : "(null)",
                 kd ? kd->fatiguemod : -1,
                 kd ? kd->healthmod  : -1,
                 kd ? kd->manamod    : -1,
                 kd ? kd->statreqs[0] : -1, kd ? kd->statreqs[1] : -1,
                 kd ? kd->statreqs[2] : -1, kd ? kd->statreqs[3] : -1,
                 kd ? kd->statreqs[4] : -1, kd ? kd->statreqs[5] : -1);

        // One-shot dump of Locke's animation state names + their play-tag
        // counts. Answers two open questions: does a "run" state exist
        // (run-mode toggle), and which states carry "play" sound tags
        // (footsteps). Remove once both are wired.
        if (TObjectImagery *img = p->GetImagery())
        {
            const int32_t n = img->NumStates();
            log_info("[anim-dump] Locke has %d animation states:", n);
            for (int32_t i = 0; i < n; i++)
            {
                SImageryHeader *hdr = img->GetHeader();
                const char *nm = (hdr && i < hdr->numstates) ? hdr->states[i].animname : "?";
                log_info("[anim-dump]   [%d] '%s'", i, nm ? nm : "?");
            }
        }
    }

    return true;
}

void TPlayScreen::Close()
{
    ingamemenu->Close();
    menuPending = false;
    ThawFrame();
    if (drawer == EDrawer::BuySell)
        CloseBuySellDrawer();
    BuySellPane.Close();
    buysellrequest = false;
    drawerclose    = false;
    RemovePane(&SpellBook);
    SpellBook.Close();
    RemovePane(&StatPane);
    StatPane.Close();
    RemovePane(&SpellPane);
    SpellPane.Close();
    RemovePane(&Inventory);
    Inventory.Close();
    RemovePane(bottombar.get());
    bottombar->Close();
    RemovePane(statusbar.get());
    statusbar->Close();
    RemovePane(&TextBar);
    TextBar.Close();                        // REVSYNC: 0x0047b30c
    RemovePane(&DialogPane);
    DialogPane.Close();
    RemovePane(&AutoMap);
    AutoMap.Close();
    if (g_playHudInitialized)
    {
        CloseUIHudMode();
        g_playHudInitialized = false;
    }
    if (mapRenderer)
        mapRenderer->SetOutputViewport(0, 0);
    if (Renderer)
        Renderer->ResetPresentNDCRect();
    if (mapListener)
    {
        MapManager.RemoveListener(mapListener);
        mapListener = 0;
    }
    if (risingNumbers)
    {
        if (Renderer)
            Renderer->RemoveHud(risingNumbers.get());
        risingNumbers.reset();
    }
    if (mapRenderer)
    {
        MapPane.SetMapView(nullptr);
        mapRenderer->Shutdown();
        mapRenderer.reset();
    }
    // TScreen::Close is a no-op base hook; skip the call.
}

// *************************************************************************
// * Per-frame                                                             *
// *************************************************************************

// Headless save-cycle smoke test (--savecycle-test[=<frames>]). After
// <frames> frames, saves to slot "savecycle", loads it back on the next
// frame and logs the player's key fields before and after, through the same
// session requests the in-game save/load paths use. Identical lines = round
// trip OK. Runs before this frame's requests and tick, so with 0 frames the
// slot holds the game exactly as it was loaded.
static void PulseSaveCycleTest()
{
    enum class EStage { Settle, Save, Load, Report, Done };
    static EStage  stage  = EStage::Settle;
    static int32_t settle = StartupSaveCycleSettle;

    auto snapshot = [](const char* tag) {
        const S3DPoint p = Player->Pos();
        log_info("[savecycle] %s: pos=(%d,%d,%d) level=%d flags=0x%x "
                 "mapindex=%d sector=%s inv=%d",
                 tag, p.x, p.y, p.z, Player->GetLevel(),
                 Player->Flags(), Player->GetMapIndex(),
                 Player->GetSector() ? "live" : "null",
                 Player->RealNumInventoryItems());
    };

    if (!Player || stage == EStage::Done)
        return;

    switch (stage)
    {
    case EStage::Settle:
        if (settle-- > 0)
            break;
        stage = EStage::Save;
        [[fallthrough]];
    case EStage::Save:
        snapshot("pre-save ");
        GameFlow.Session().RequestSave("savecycle");
        stage = EStage::Load;
        break;
    case EStage::Load:
        GameFlow.Session().RequestLoad("savecycle");
        stage = EStage::Report;
        break;
    case EStage::Report:
        snapshot("post-load");
        stage = EStage::Done;
        StartupSaveCycle = false;
        break;
    case EStage::Done:
        break;
    }
}

void TPlayScreen::Update()
{
    // Shadow Smoke (--shadow): its audit of the last tick and its next step,
    // before anything else of this one -- a movie or a load included, which
    // it waits out or skips as a player would.
    Shadow.Pulse();

    // Apply any deferred pane add scheduled by SetNextPane().
    if (nextpane)
    {
        AddPane(nextpane);
        nextpane = nullptr;
    }

    // Retail's movie player blocked the game: nothing ticks while one plays.
    if (movieplaying)
        return;

    // Nor while a new level loads (retail loaded it synchronously): the
    // session brings in a slice a frame under the loading line.
    if (GameFlow.Session().LevelLoading())
    {
        GameFlow.Session().EnterLevel();
        return;
    }

    // Nor while a save loads (ProcessRequests below started it): it runs a
    // step a tick behind a still of the world.
    if (GameFlow.Session().Loading())
    {
        StepGameLoad();
        return;
    }

    if (StartupSaveCycle)
        PulseSaveCycleTest();

    // Save / load requests made during play (input, console, scripts, the
    // in-game dialogs). Retail ran them ahead of the pane pulse, so a paused
    // world doesn't hold them (0x0047bd20).
    GameFlow.Session().ProcessRequests();

    // The bottom drawer follows its requests (the shop opening or closing),
    // the bottom bar the HUD's lower panel, the side panes the side panel.
    UpdateDrawer();
    UpdateBottomBar();
    UpdateSidePanes();

    // REVSYNC: 0x0048fda0 / 0x0052b9f0 / 0x0047c2c0 -- under a MODAL_PAUSE
    // modal (the single-player in-game menu and its dialogs) only the modal
    // pulses: the world, the areas and the game clock stand still. The
    // --exec console queue keeps running.
    if (ModalHas(MODAL_PAUSE))
    {
        PulseStartupExec();
        return;
    }

    // Per-frame work that differs between game and editor: input ->
    // movement, pulse / move over the active sector window, camera
    // follow. Editor mode's Tick is a no-op (the editor drives camera
    // + pulse itself).
    CurrentMode()->Tick();

    // A teleport to another level moved the camera there this tick: bring
    // the level in and put the player back into the map (retail did both in
    // the map pane's sector update).
    const bool levelready = GameFlow.Session().EnterLevel();

    // --playerai: the player fights on his own (testconfig.h).
    if (StartupPlayerAI && Player)
        Player->SetRunsAI(true);
    // --playerbot: Locke played by code, once he's in the world.
    if (StartupPlayerBot && Player && !PlayerBot.Enabled())
        PlayerBot.Enable(true);

    // --exec console queue (no-op unless the flag was given).
    PulseStartupExec();

    // Tick the area system: detects player Enter/Exit of each TArea's
    // RECTs, runs day/night ambient interpolation, fires CDPLAYLIST /
    // AUDIOENV transitions. Must run after CurrentMode()->Tick() so
    // MapPane.GetMapPos reflects this frame's player position, and only
    // once the camera's level is in: retail entered the new area after the
    // sector update had loaded it.
    if (levelready)
        AreaManager.Pulse();

    // Advance fixed-tick counters. CurrentMode()->Tick() owns gameplay frame
    // advancement; the renderer only samples/interpolates the current pose.
    AdvanceGameFrame();

    // REVSYNC: screen slot 0x24 (0x004902c0) -> NextFrameObjects 0x00457ef0:
    // animation frames advance last in the tick, after Pulse and Move and
    // the frame count, so the next tick's input sees the advanced frame.
    MapPane.NextFrameObjects();

    // --combattrace: the tick's fighters (no-op otherwise).
    CombatTrace::Tick(gameframes);
}

namespace {

// The still behind a load: over the HUD panels (z 0..10), under the pane
// tree (TScreen::kPaneLayerZ).
constexpr float kFrozenFrameZ = 50.0f;

class TFrozenFrameLayer final : public THudDrawable
{
  public:
    explicit TFrozenFrameLayer(const TTextureHandle& texture) : texture(texture) {}
    void Draw() override
    {
        if (texture != kInvalidTexture)
            Renderer->DrawTextureFit(texture);
        else
            Renderer->FillScreen(0.0f, 0.0f, 0.0f, 1.0f);
    }

  private:
    const TTextureHandle& texture;
};

}  // namespace

// REVSYNC: the load dialog's in-game load (0x00539590) and the frame's
// request load (0x0047bfab) ran LoadGame and the sector load synchronously,
// so the screen stood still on the last frame (the popup's bar drawn straight
// to the display, 0x0053c3d0). Here: the first tick keeps the world and the
// HUD panels of the frame on screen (the capture leaves out the pane tree,
// which keeps drawing live over it); then each tick runs a load step behind
// that still, its progress going to the load dialog's popup, until the game
// is in (or the load failed: back to the title).
void TPlayScreen::StepGameLoad()
{
    TGameSession& session = GameFlow.Session();
    switch (freeze)
    {
    case EFreeze::None:
        freeze = EFreeze::Capturing;
        if (!Display.RequestCapture(
                [this](const uint8_t* rgba, int32_t width, int32_t height) {
                    FreezeFrame(rgba, width, height);
                },
                kPaneLayerZ))
            FreezeFrame(nullptr, 0, 0);
        return;
    case EFreeze::Capturing:
        return;                     // the capture comes with this frame's flip
    case EFreeze::Frozen:
        break;
    }

    session.Step();
    ingamemenu->LoadProgress(session.Progress());
    if (session.Loading())
        return;

    ThawFrame();
    const bool loaded = session.Ready();
    log_info("[playscreen] game load %s", loaded ? "done" : "failed");
    ingamemenu->LoadFinished(loaded);
    if (!loaded)
        GameFlow.ReturnToTitle();
}

// The capture's frame has been drawn by now; the still goes up with the
// next one (ShowStill).
void TPlayScreen::FreezeFrame(const uint8_t* rgba, int32_t width, int32_t height)
{
    freeze = EFreeze::Frozen;
    if (rgba && width > 0 && height > 0)
    {
        // The swapchain's alpha isn't the picture's: the still is opaque.
        frozenPixels.assign(rgba, rgba + size_t(width) * size_t(height) * 4);
        for (size_t i = 3; i < frozenPixels.size(); i += 4)
            frozenPixels[i] = 255;
        frozenWidth  = width;
        frozenHeight = height;
    }
    else
    {
        log_warn("[playscreen] no still of the frame for the load; it shows black");
    }
    log_info("[playscreen] the load runs behind a still of the frame (%dx%d)", width, height);
}

// From Animate, outside any pass: the still's texture, and its layer.
void TPlayScreen::ShowStill()
{
    if (stillShown || !Renderer)
        return;
    if (!frozenPixels.empty())
    {
        frozenTexture = Renderer->CreateDynamicTexture(frozenWidth, frozenHeight,
                                                       ERendererTextureFilter::Nearest);
        if (frozenTexture != kInvalidTexture)
            Renderer->UpdateDynamicTexture(frozenTexture, frozenPixels.data(), frozenPixels.size());
        frozenPixels.clear();
    }
    if (!frozenLayer)
        frozenLayer = std::make_unique<TFrozenFrameLayer>(frozenTexture);
    Renderer->AddHud(frozenLayer.get(), kFrozenFrameZ);
    stillShown = true;
}

void TPlayScreen::ThawFrame()
{
    if (freeze == EFreeze::None)
        return;
    freeze     = EFreeze::None;
    stillShown = false;
    frozenPixels.clear();
    if (Renderer)
    {
        if (frozenLayer)
            Renderer->RemoveHud(frozenLayer.get());
        if (frozenTexture != kInvalidTexture)
            Renderer->DestroyDynamicTexture(frozenTexture);
    }
    frozenTexture = kInvalidTexture;
}

int32_t TPlayScreen::DrawerHeight() const
{
    constexpr int32_t kBottomBarH = 60;           // BottomBarPane_SPEC §3 (0x3c)
    if (drawer == EDrawer::BuySell)
        return TBuySellPane::kHeight;
    return GetHudState().bottomBarOpen ? kBottomBarH : 0;
}

// REVSYNC: 0x0047ecc0 / 0x0047ece0 -- a close request reaches whatever the
// drawer holds: the shop, or the HUD's bottom bar (mode 2, the HUD's Lower
// Panel, SHudState::bottomBarOpen), which stays closed until that command
// opens it again. An in-game load closes the bar so (LoadGame's reset;
// seen in the emulator, AUTHOR_QUESTIONS.md 81), as would `hideresponse`,
// which no shipped script uses; entering play opens it (Initialize). The
// camera and fadescreenout ask only in mode 3, for the shop.
void TPlayScreen::CloseDrawer()
{
    if (drawer == EDrawer::BuySell)
        drawerclose = true;
    else if (drawer == EDrawer::Hud)
        GetHudState().bottomBarOpen = 0;
}

// REVSYNC: the drawer half of Pulse 0x0047b4d0, mode 3. A shop request opens
// the drawer once the shop has re-initialized (0x0052f390); the request's end
// (the shop's Exit) or a close request closes it.
void TPlayScreen::UpdateDrawer()
{
    if (drawer == EDrawer::BuySell)
    {
        if (!buysellrequest || drawerclose)
            CloseBuySellDrawer();
    }
    else if (buysellrequest)
    {
        if (BuySellPane.Initialize())
            OpenBuySellDrawer();
        else
        {
            // Retail retried every pulse and the script's `wait buysell`
            // never ended; the port lets the script go.
            buysellrequest = false;
            BuySellPane.Reset();
        }
    }
    drawerclose = false;
}

// REVSYNC: 0x0047b8c2..0x0047b966 -- the HUD's drawer content goes (bottom
// bar, belt, quick spells), the side panel opens (+0x6a4), the text bar hides
// (TTextBar::Hide 0x0054c9c0), and the shop is added and shown at the bottom
// left. The dialog pane and the side tabs lay themselves out against the map
// view, which the drawer shortens.
void TPlayScreen::OpenBuySellDrawer()
{
    SHudState& hud = GetHudState();
    hud.bottomBarOpen = 0;
    hud.sidebarState  = HUD_SIDEBAR_OPEN;
    TextBar.Hide();
    AddPane(&BuySellPane);
    BuySellPane.Show();
    drawer = EDrawer::BuySell;
    log_info("[buysell] the shop opens: %d rows", static_cast<int32_t>(BuySellPane.Items().size()));
}

// REVSYNC: 0x0047b7d5..0x0047b81c -- Reset the shop (0x00530600), take it
// out, show the text bar again; the drawer is closed in mode 2, so the
// bottom bar stays closed until the Lower Panel command opens it.
void TPlayScreen::CloseBuySellDrawer()
{
    BuySellPane.Reset();
    RemovePane(&BuySellPane);
    TextBar.Show();
    drawer         = EDrawer::Hud;
    buysellrequest = false;
    log_info("[buysell] the shop closes");
}

// REVSYNC: the drawer half of Pulse 0x0047b4d0, mode 2 -- the bottom bar
// shows while the HUD's lower panel is open and the shop doesn't hold the
// drawer, along the bottom of the map view and as wide as it: the display
// less the side panel (retail's drawer pane hands its rect on in mode 2,
// 0x0052da60 -> TBottomBarPane::SetRect 0x0052c930).
void TPlayScreen::UpdateBottomBar()
{
    if (!bottombar->IsOpen())
        return;
    const bool open = drawer == EDrawer::Hud && GetHudState().bottomBarOpen;
    if (open)
    {
        int32_t x = 0, y = 0, w = 0, h = 0;
        GetMapViewRect(x, y, w, h);
        bottombar->Place(x, y + h, w);
    }
    if (open == bottombar->IsHidden())
        open ? bottombar->Show() : bottombar->Hide();
}

// A pane of the side panel, at the display's right edge: the upper slot at
// the top, the lower slot under its 306 px. Placed and shown while the slot
// is its, else hidden.
enum class ESideSlot : int32_t { Upper = 0, Lower = 306 };
static void PlaceSidePane(TPane& pane, ESideSlot slot, bool shown)
{
    if (!pane.IsOpen())
        return;
    if (shown)
    {
        const int32_t dw = Display.Width() > 0 ? Display.Width() : WIDTH;
        pane.Resize(dw - pane.GetWidth(), static_cast<int32_t>(slot), pane.GetWidth(), pane.GetHeight());
        if (pane.WasResized())
            pane.PaneResized();
    }
    if (shown == pane.IsHidden())
        shown ? pane.Show() : pane.Hide();
}

// The side panel's rebuilt panes while the side panel is open on theirs:
// the stats and the spell book in the upper slot; the inventory, the spell
// pane and the automap in the lower. (The equipment is still the HUD
// harness's.)
void TPlayScreen::UpdateSidePanes()
{
    const SHudState& hud = GetHudState();
    const bool open = hud.sidebarState == HUD_SIDEBAR_OPEN;
    PlaceSidePane(StatPane, ESideSlot::Upper, open && hud.topSlot == HUD_TOP_STATS);
    PlaceSidePane(SpellBook, ESideSlot::Upper, open && hud.topSlot == HUD_TOP_BOOK);
    PlaceSidePane(Inventory, ESideSlot::Lower, open && hud.bottomSlot == HUD_BOT_INV);
    PlaceSidePane(SpellPane, ESideSlot::Lower, open && hud.bottomSlot == HUD_BOT_SPELL);
    PlaceSidePane(AutoMap, ESideSlot::Lower, open && hud.bottomSlot == HUD_BOT_MAP);
}

// The shop's rect while the drawer holds it: its clicks and moves are its
// own, not the world's.
static bool InBuySellDrawer(int32_t x, int32_t y)
{
    return PlayScreen.Drawer() == TPlayScreen::EDrawer::BuySell &&
           x >= BuySellPane.GetPosX() && x < BuySellPane.GetPosX() + BuySellPane.GetWidth() &&
           y >= BuySellPane.GetPosY() && y < BuySellPane.GetPosY() + BuySellPane.GetHeight();
}

void TPlayScreen::GetMapViewRect(int32_t& x, int32_t& y, int32_t& w, int32_t& h) const
{
    if (g_playHudInitialized)
    {
        GetReconstructedPlayfieldRect(x, y, w, h);
        return;
    }
    x = y = 0;
    w = Display.Width()  > 0 ? Display.Width()  : WIDTH;
    h = Display.Height() > 0 ? Display.Height() : HEIGHT;
}

void TPlayScreen::RenderFrame()
{
    if (!mapRenderer) return;
    if (g_playHudInitialized)
    {
        int32_t px = 0, py = 0, pw = 0, ph = 0;
        GetMapViewRect(px, py, pw, ph);
        mapRenderer->SetOutputViewport(pw, ph);
        if (Renderer)
        {
            const int32_t dw = Display.Width()  > 0 ? Display.Width()  : WIDTH;
            const int32_t dh = Display.Height() > 0 ? Display.Height() : HEIGHT;
            Renderer->SetPresentPixelRect(px, py, pw, ph, dw, dh,
                                          0, 0, pw, ph);
        }
    }
    else
    {
        mapRenderer->SetOutputViewport(0, 0);
        if (Renderer)
            Renderer->ResetPresentNDCRect();
    }

    // The view is the map pane's center, which TMapPane::UpdateMapPos moves
    // toward the centeron target each tick, as retail drew every frame from
    // it (the scroll origin is WorldToScreen(center) less half the pane,
    // 0x004539d0), a tick behind (TMapPane::CameraPos). Following the player
    // it moves on the tick as he does, so he doesn't shake against it; a pan
    // elsewhere glides between ticks.
    if (CurrentMode() == GameMode() && Player)
    {
        const double glide = MapPane.IsFollowingPlayer() ? 0.0 : TTime::LegacyFrameFraction();
        const S3DPoint p = MapPane.CameraPos(glide);
        mapRenderer->SetCameraWorld(MapPane.GetMapLevel(), p.x, p.y, p.z);
    }

    // While the editor is paused and nothing is dirty, skip the world
    // render entirely. The renderer's lit_target persists, so the Game
    // View panel keeps displaying the previously rendered frame.
    if (!EditorShouldRenderWorld()) return;
    mapRenderer->RenderFrame();
    if (risingNumbers)
        risingNumbers->Compose(*mapRenderer);
}

// Diagnostic overlay: prints Locke's world position, walkmap height
// under his feet, the delta that drives TCharacter::Blocked, sector
// id, and current action. Game-mode only -- editor has its own
// inspectors. Cheap; lifetime is the duration of the player-input
// bring-up and can be removed once movement reliably works.
static void DrawPlayerStatusOverlay()
{
    if (!Player) return;
    if (CurrentMode() != GameMode()) return;

    S3DPoint p; Player->GetPos(p);
    const int32_t lvl = Player->GetLevel();
    const int32_t walk = MapPane.GetWalkHeight(p);
    const int32_t z_delta = p.z - walk;
    const char* state_name = Player->GetStateName();
    const int32_t sx_world = p.x >> SECTORWSHIFT;
    const int32_t sy_world = p.y >> SECTORHSHIFT;
    const int32_t radius = ((TCharacter*)Player)->Radius();
    int32_t r_maxdelta = 0, r_h = 0;
    bool r_hole = false;
    MapPane.GetWalkHeightRadius(p, radius, r_maxdelta, r_h, r_hole);
    const TCharacter* blocker = ((TCharacter*)Player)->CharBlocking();
    const bool z_blocks    = std::abs(z_delta) > 32;
    const bool no_floor    = (walk == 0);
    const bool step_blocks = r_maxdelta > 32;
    const bool char_blocks = (blocker != nullptr);

    TObjectInstance* pinst = static_cast<TObjectInstance*>(Player);
    const int32_t st_idx   = pinst->GetState();
    const int32_t st_total = Player->NumStates();
    TObjectImagery* img    = Player->GetImagery();
    const int32_t fr_total = img ? img->GetAniLength(st_idx) : 0;
    const uint32_t ani_fl  = Player->GetAniFlags();
    char flagstr[64] = {};
    int  fpos = 0;
    auto add_flag = [&](const char* tag, uint32_t mask) {
        if ((ani_fl & mask) && fpos < int(sizeof(flagstr)) - 12) {
            fpos += std::snprintf(flagstr + fpos, sizeof(flagstr) - fpos,
                                  fpos ? "|%s" : "%s", tag);
        }
    };
    add_flag("LOOP", AF_LOOPING);
    add_flag("PP",   AF_PINGPONG);
    add_flag("ROOT", AF_ROOT);
    add_flag("R2R",  AF_ROOT2ROOT);
    add_flag("FLY",  AF_FLY);
    add_flag("MOVE", AF_MOVE);
    const int32_t prev_idx = pinst->GetPrevState();
    const char* prev_name = (img && prev_idx >= 0 && prev_idx < img->NumStates())
        ? img->GetAniName(prev_idx)
        : "?";

    TCharacter* pc = static_cast<TCharacter*>(Player);
    const SCharData* pcd = pc->GetCharData();

    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Player Debug", nullptr, ImGuiWindowFlags_NoNav))
    {
        if (ImGui::BeginTabBar("PlayerDebugTabs"))
        {
            if (ImGui::BeginTabItem("Move"))
            {
                ImGui::Text("pos     : (%d, %d, %d)  level=%d", p.x, p.y, p.z, lvl);
                ImGui::Text("sector  : %d_%d", sx_world, sy_world);
                ImGui::Text("walkmap : %d  (z-walk = %d)", walk, z_delta);
                ImGui::Text("radius  : %d   r_h=%d  maxdelta=%d  hole=%d",
                            radius, r_h, r_maxdelta, (int)r_hole);
                ImGui::Text("would block: %s%s%s%s%s",
                            z_blocks    ? "Z>32 "       : "",
                            no_floor    ? "no-floor "   : "",
                            r_hole      ? "hole "       : "",
                            step_blocks ? "step>32 "    : "",
                            char_blocks ? "char-blk "   : "",
                            (!z_blocks && !no_floor && !r_hole &&
                             !step_blocks && !char_blocks) ? "no" : "");
                ImGui::Text("moving  : %s   moveangle=%d   anim=%s",
                            Player->IsMoving() ? "yes" : "no",
                            Player->GetMoveAngle(),
                            state_name ? state_name : "?");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Anim"))
            {
                ImGui::Text("anim    : %s", state_name ? state_name : "?");
                ImGui::Text("state   : %d/%d  frame=%d  last=%d  count=%d  rate=%d",
                            st_idx, st_total, pinst->GetFrame(),
                            fr_total > 0 ? fr_total - 1 : -1,
                            fr_total, pinst->GetFrameRate());
                ImGui::Text("flags   : %s  done=%s  animator=%s",
                            fpos ? flagstr : "-",
                            Player->CommandDone() ? "yes" : "no",
                            Player->HasAnimator() ? "yes" : "NO");
                ImGui::Text("prev    : state=%d \"%s\"  frame=%d",
                            prev_idx, prev_name, pinst->GetPrevFrame());
                if (T3DAnimator* d3 = dynamic_cast<T3DAnimator*>(pinst->GetAnimator()))
                {
                    ImGui::Text("bridge  : transition=%d state=%d prev=%d/%d",
                                d3->DebugTransitionActive() ? 1 : 0,
                                d3->DebugTransitionState(),
                                d3->DebugTransitionPrevState(),
                                d3->DebugTransitionPrevFrame());
                    ImGui::Text("bridge  : last=%d high=%d",
                                d3->DebugTransitionLastFrame(),
                                d3->DebugTransitionHighestFrame());
                    ImGui::Text("render  : frame=%d next=%d/%d frac=%.3f",
                                d3->DebugPoseUpdateFrame(),
                                d3->DebugPoseUpdateNextState(),
                                d3->DebugPoseUpdateNextFrame(),
                                d3->DebugPoseUpdateFrameFrac());
                }
                ImGui::Separator();
                ImGui::Text("sets    : total=%u same=%u",
                            pinst->DebugStateSetCount(),
                            pinst->DebugSameStateSetCount());
                ImGui::Text("last set: %d->%d at frame=%d gf=%d",
                            pinst->DebugLastStateSetFrom(),
                            pinst->DebugLastStateSetTo(),
                            pinst->DebugLastStateSetFrame(),
                            pinst->DebugLastStateSetGameFrame());
                ImGui::Text("wraps   : total=%u",
                            pinst->DebugLoopWrapCount());
                ImGui::Text("last wrap: state=%d frame=%d gf=%d",
                            pinst->DebugLastLoopWrapState(),
                            pinst->DebugLastLoopWrapFrame(),
                            pinst->DebugLastLoopWrapGameFrame());
                ImGui::Separator();
                ImGui::Text("doing   : action=%d name=\"%s\"",
                            pc->DoingAction(), pc->DoingName());
                ImGui::Text("desired : action=%d name=\"%s\"",
                            pc->DesiredAction(), pc->DesiredName());
                ImGui::Text("root    : action=%d name=\"%s\"",
                            pc->RootAction(), pc->RootName());
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Combat"))
            {
                ImGui::Text("mode    : combat=%d bow=%d walk=%d sneak=%d run=%d",
                            pc->IsCombat() ? 1 : 0,
                            pc->IsBowMode() ? 1 : 0,
                            pc->IsWalkMode() ? 1 : 0,
                            pc->IsSneakMode() ? 1 : 0,
                            pc->IsRunMode() ? 1 : 0);
                if (pcd && pcd->classdata)
                {
                    const SClassData* cd = pcd->classdata;
                    ImGui::Text("classmod: H=%d F=%d M=%d  statreqs=[%d,%d,%d,%d,%d,%d]",
                                cd->healthmod, cd->fatiguemod, cd->manamod,
                                cd->statreqs[0], cd->statreqs[1], cd->statreqs[2],
                                cd->statreqs[3], cd->statreqs[4], cd->statreqs[5]);
                }
                if (pcd)
                    ImGui::Text("chardata: name=\"%s\" numattacks=%d combatrng=%d/%d attkrng=%d",
                                pcd->name, pcd->attacks.NumItems(),
                                pcd->combatrangemin, pcd->combatrangemax,
                                pcd->maxattackrange);
                else
                    ImGui::Text("chardata: NULL");
                ImGui::Text("hasAni  : combat=%d swing=%d thrust=%d chop=%d",
                            Player->HasActionAni("combat") ? 1 : 0,
                            Player->HasActionAni("swing")  ? 1 : 0,
                            Player->HasActionAni("thrust") ? 1 : 0,
                            Player->HasActionAni("chop")   ? 1 : 0);
                ImGui::EndTabItem();
            }

            if (pcd && ImGui::BeginTabItem("Attacks"))
            {
                const int32_t wt   = pc->WeaponType();
                const int32_t wbit = 1 << wt;
                TPlayer* tpl = (Player->ObjClass() == OBJCLASS_PLAYER) ? (TPlayer*)Player : nullptr;
                const int32_t skAtk = tpl ? tpl->Skill(SK_ATTACK) : 999;
                ImGui::Text("player  : weapontype=%d mask=0x%x attackskill=%d",
                            wt, wbit, skAtk);
                ImGui::Text("fatigue : %d/%d", pc->Fatigue(), pc->MaxFatigue());
                if (tpl)
                {
                    ImGui::Text("stats   : Lv=%d Exp=%d STR=%d CON=%d AGI=%d RFL=%d MND=%d LCK=%d",
                                tpl->Level(), tpl->Exp(),
                                tpl->Strn(), tpl->Cons(), tpl->Agil(),
                                tpl->Rflx(), tpl->Mind(), tpl->Luck());
                    ImGui::Text("max     : H=%d/%d F=%d/%d M=%d/%d classdata=%s",
                                tpl->Health(), tpl->MaxHealth(),
                                tpl->Fatigue(), tpl->MaxFatigue(),
                                tpl->Mana(), tpl->MaxMana(),
                                (pcd->classdata ? pcd->classdata->name : "(NULL)"));
                }
                ImGui::Separator();
                for (int btn = 1; btn <= 3; ++btn)
                {
                    int with_button = 0, after_anim = 0, after_mode = 0,
                        after_weapon = 0, after_skill = 0, after_wskill = 0,
                        after_fatigue = 0;
                    const char* first_pass = nullptr;
                    for (int32_t i = 0; i < pcd->attacks.NumItems(); ++i)
                    {
                        const SCharAttackData& ad = pcd->attacks[i];
                        if (ad.button != btn) continue;
                        ++with_button;
                        if (!Player->HasActionAni(ad.attackname)) continue;
                        ++after_anim;
                        bool mode_ok = true;
                        if ((ad.flags & CA_SNEAKMODE) && !pc->IsSneakMode()) mode_ok = false;
                        else if ((ad.flags & CA_WALKMODE) && !pc->IsWalkMode()) mode_ok = false;
                        else if ((ad.flags & CA_BOWMODE) && !pc->IsBowMode()) mode_ok = false;
                        else if (!pc->IsCombat()) mode_ok = false;
                        if (!mode_ok) continue;
                        ++after_mode;
                        if (Player->ObjClass() == OBJCLASS_PLAYER)
                        {
                            TPlayer* pl = (TPlayer*)Player;
                            if (!(ad.weaponmask & wbit)) continue;
                            ++after_weapon;
                            if (pl->Skill(SK_ATTACK) < ad.attackskill) continue;
                            ++after_skill;
                            if (pl->WeaponSkill(wt) < ad.weaponskill) continue;
                            ++after_wskill;
                        }
                        else
                        {
                            after_weapon = after_skill = after_wskill = after_mode;
                        }
                        if (pc->Fatigue() < ad.fatigue) continue;
                        ++after_fatigue;
                        if (!first_pass) first_pass = ad.attackname;
                    }
                    ImGui::Text("btn%d: btn=%d ani=%d mode=%d wpn=%d skl=%d wsk=%d fat=%d [%s]",
                                btn, with_button, after_anim, after_mode,
                                after_weapon, after_skill, after_wskill,
                                after_fatigue,
                                first_pass ? first_pass : "(rejected)");
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// Closest-monster diagnostic overlay. Walks the active sector window
// and finds the OBJCLASS_CHARACTER instance closest to the player
// (excluding dead, hidden, etc.). Surfaces the AI-relevant fields so
// "why is this monster not engaging" is answerable without instrumented
// logging on every Pulse. Game-mode only.
static void DrawClosestMonsterOverlay()
{
    if (!Player) return;
    if (CurrentMode() != GameMode()) return;

    const S3DPoint pp = Player->Pos();
    TCharacter*    closest      = nullptr;
    int32_t        closest_dist = INT32_MAX;

    if (TGameMap* gm = MapManager.CurrentMap())
    {
        for (TSector* sec : gm->Sectors())
        {
            if (!sec) continue;
            for (int32_t i = 0; i < sec->NumItems(); ++i)
            {
                TObjectInstance* oi = sec->GetInstance(i);
                if (!oi) continue;
                if (oi->ObjClass() != OBJCLASS_CHARACTER) continue;
                if (oi == (TObjectInstance*)Player) continue;
                S3DPoint mp = oi->Pos();
                const int32_t dx = mp.x - pp.x;
                const int32_t dy = mp.y - pp.y;
                const int32_t d2 = dx * dx + dy * dy;
                if (d2 < closest_dist)
                {
                    closest_dist = d2;
                    closest = (TCharacter*)oi;
                }
            }
        }
    }

    ImGui::SetNextWindowPos(ImVec2(8, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (!ImGui::Begin("Closest Monster", nullptr,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    if (!closest)
    {
        ImGui::Text("(no character in current map)");
        ImGui::End();
        return;
    }

    const S3DPoint mp = closest->Pos();
    const int32_t  dx = mp.x - pp.x;
    const int32_t  dy = mp.y - pp.y;
    const int32_t  flat_dist = int32_t(std::sqrt(double(dx) * dx + double(dy) * dy));
    const SCharData* cd = closest->GetCharData();

    ImGui::Text("name    : %s  type=%s  class=%d",
                closest->GetName() ? closest->GetName() : "?",
                closest->GetTypeName() ? closest->GetTypeName() : "?",
                closest->ObjClass());
    ImGui::Text("pos     : (%d, %d, %d)  dist=%d", mp.x, mp.y, mp.z, flat_dist);
    ImGui::Text("flags   : aggressive=%d dead=%d disabled=%d moving=%d fighting=%d",
                closest->Aggressive() ? 1 : 0,
                closest->IsDead() ? 1 : 0,
                (closest->Flags() & OF_DISABLED) ? 1 : 0,
                closest->IsMoving() ? 1 : 0,
                closest->IsFighting() ? 1 : 0);
    ImGui::Text("global  : NoAI=%d Editor=%d", NoAI ? 1 : 0, Editor ? 1 : 0);
    ImGui::Text("ticks   : pulse=%u ai=%u", closest->ai_pulse_count, closest->ai_ai_count);
    if (TCharacter* tgt = closest->Fighting())
        ImGui::Text("target  : %s  dist=%d",
                    tgt->GetName() ? tgt->GetName() : "?",
                    closest->Distance(tgt));
    else
        ImGui::Text("target  : (none)");
    if (cd)
        ImGui::Text("chardata: walkspd=%d combatrng=%d/%d attkrng=%d freq=%d-%d",
                    cd->walkspeed, cd->combatrangemin, cd->combatrangemax,
                    cd->maxattackrange, cd->minattackfreq, cd->maxattackfreq);
    else
        ImGui::Text("chardata: NULL");
    const char* anim = closest->GetStateName();
    TObjectInstance* cinst = static_cast<TObjectInstance*>(closest);
    TObjectImagery* cimg = closest->GetImagery();
    const int32_t cstate = cinst->GetState();
    const int32_t clen = cimg ? cimg->GetAniLength(cstate) : 0;
    ImGui::Text("anim    : %s  state=%d frame=%d/%d prev=(%d,%d) done=%d",
                anim ? anim : "?",
                cstate, cinst->GetFrame(), clen,
                cinst->GetPrevState(), cinst->GetPrevFrame(),
                closest->CommandDone() ? 1 : 0);
    ImGui::Text("sets    : total=%u same=%u last %d->%d frame=%d gf=%d",
                cinst->DebugStateSetCount(),
                cinst->DebugSameStateSetCount(),
                cinst->DebugLastStateSetFrom(),
                cinst->DebugLastStateSetTo(),
                cinst->DebugLastStateSetFrame(),
                cinst->DebugLastStateSetGameFrame());
    ImGui::Text("wraps   : total=%u last state=%d frame=%d gf=%d",
                cinst->DebugLoopWrapCount(),
                cinst->DebugLastLoopWrapState(),
                cinst->DebugLastLoopWrapFrame(),
                cinst->DebugLastLoopWrapGameFrame());

    ImGui::Text("doing   : action=%d name=\"%s\"  target=(%d,%d)",
                closest->DoingAction(), closest->DoingName(),
                closest->DoingTargetX(), closest->DoingTargetY());
    ImGui::Text("desired : action=%d name=\"%s\"",
                closest->DesiredAction(), closest->DesiredName());
    ImGui::Text("root    : action=%d name=\"%s\"",
                closest->RootAction(), closest->RootName());
    ImGui::Text("attack  : nextattack=%d radius=%d numattacks=%d",
                closest->NextAttack(), closest->Radius(),
                cd ? cd->attacks.NumItems() : -1);

    ImGui::End();
}

// Legacy entry points still referenced by drivers / pane code. Pulse
// pumps the per-frame state update; Animate fires the world render
// (matching what TTestScreen does for TestModes::Render); DrawBackground
// only consumes the redraw flag -- no BITMAP.100 backdrop on the new path.
// REVSYNC: Pulse @ 0x0047b4d0 -- the world, then the screen's panes
// (0x0048fda0 at its end), then what the player explored for the automap
// (0x0047c224: not while a modal pauses the world).
void TPlayScreen::Pulse()
{
    Update();
    TScreen::Pulse();
    if (!ModalHas(MODAL_PAUSE))
        AutoMap.RecordTravels();
}
void TPlayScreen::Animate(bool /*draw*/)
{
    // While a load runs behind the still, neither the world being replaced
    // nor the HUD reading its player is drawn.
    if (freeze == EFreeze::Frozen)
    {
        ShowStill();
        return;
    }
    // Refresh reconstructed HUD surfaces before the world render. The
    // EquipmentPane paperdoll temporarily uses the renderer's lit target;
    // rendering the world afterward overwrites that temporary target before
    // final present while the HUD keeps the sampled pane surface.
    if (g_playHudInitialized)
        RenderUIHudModeEmbedded();
    RenderFrame();
    CurrentMode()->Animate();
    EditorDrawChrome();
    if (DebugUI::IsVisible())
    {
        DrawPlayerStatusOverlay();
        DrawClosestMonsterOverlay();
    }
}
// No backdrop to blit on the new path, but this is where a screen consumes
// its redraw flag (TScreen::DrawBackground): DrawFrame has already passed it
// to the panes. Left set, every pane would recompose every frame after the
// first Redraw (a focus change, the editor closing).
void TPlayScreen::DrawBackground()
{
    dirty = false;
}

// *************************************************************************
// * Input                                                                 *
// *************************************************************************

void TPlayScreen::KeyPress(int32_t key, bool down)
{
    // REVSYNC: 0x0047c63e -- while the text bar's prompt is open the play
    // screen takes no keys at all, its panes', Escape and the editor's
    // included: the typing arrives as characters (TTextBar::CharPress).
    if (TextBar.IsPrompting())
        return;

    // A load is under way: the panes get the key (the progress popup holds
    // it, when up), the world and the editor nothing.
    if (GameFlow.Session().Loading())
    {
        TScreen::KeyPress(key, down);
        return;
    }

    // Editor toggle. F12 is the retail-era hotkey -- always handled at
    // the screen level so the user can flip modes regardless of who
    // currently owns input.
    if (down && key == VK_F12)
    {
        if (Editor)
        {
            ShutDownEditor();
            Redraw();
        }
        else
        {
            StartEditor();
        }
        return;
    }

    // REVSYNC: KeyPress @ 0x0047c630 -- the panes first (the dialog pane's
    // choice keys, a modal's keys), then the play screen's own keys, which
    // any flagged modal holds back: ESC opens the in-game menu (in demo
    // mode it asks to quit); then the control map's commands, unless the
    // modal keeps the keys (MODAL_KEYS).
    TScreen::KeyPress(key, down);
    if (down && key == VK_ESCAPE && TopModalFlags() == 0 && !Editor)
    {
        if (demomode)
            ingamemenu->AskExit();
        else
            OpenInGameMenu();
        return;
    }
    if (ModalHas(MODAL_KEYS))
        return;
    CurrentMode()->HandleKey(key, down);
}

void TPlayScreen::OpenInGameMenu()
{
    if (InGameMenuOpen())
        return;
    menuPending = true;
    ::SaveGame.CaptureThumbnail({}, [this] {
        menuPending = false;
        if (CurrentScreen == this && !IsDone())
            ingamemenu->Open();
    });
}

bool TPlayScreen::InGameMenuOpen() const
{
    return menuPending || ingamemenu->IsOpen();
}

bool TPlayScreen::MapHasPointer(int32_t x, int32_t y) const
{
    if (ModalHas(MODAL_MOUSE) || GameFlow.Session().Loading() || InBuySellDrawer(x, y))
        return false;
    // The side tabs over the play field don't take it: retail's hover
    // cursor looks only at the map pane's rect (0x004544f6..0x00454527).
    return !(g_playHudInitialized && (IsOffMapPane(x, y) || UIDragState::IsActive()));
}

uint32_t TPlayScreen::ControlModeMask() const
{
    // ControlMap.GetCommand returns the first binding whose modes meet the
    // mask, so exactly one mode goes in ('S' is sneak in walk mode and
    // thrust in combat mode).
    if (Player)
    {
        if (Player->IsCombat())     return CTRL_COMBATMODE;
        if (Player->IsBowMode())    return CTRL_BOWMODE;
        if (Player->IsSneakMode())  return CTRL_SNEAKMODE;
    }
    return CTRL_NORMALMODE;
}

TPlayScreen::EClickRoute TPlayScreen::ClickRoute(int32_t x, int32_t y)
{
    // REVSYNC: TScreen::MouseClick 0x004903b0 -- a modal holding the mouse
    // (MODAL_MOUSE) gets every click (0x004903f0); the HUD and the world get
    // none.
    if (ModalHas(MODAL_MOUSE))
        return EClickRoute::Modal;
    // A load is under way: the world takes no clicks.
    if (GameFlow.Session().Loading())
        return EClickRoute::Ignored;
    if (InBuySellDrawer(x, y))
        return EClickRoute::BuySell;
    // The HUD: a pane beside the map, a drag in flight, or one of the side
    // tabs' buttons over the play field. TMapPane::MouseClick leaves a press
    // to the tabs only when a button holds it (0x004364d0, 0x0044f146..
    // 0x0044f182); around and between the buttons it is the map's.
    if (g_playHudInitialized &&
        (IsOffMapPane(x, y) || UIDragState::IsActive() || UISideTabButtonAt(x, y) >= 0))
        return EClickRoute::Hud;
    return EClickRoute::World;
}

static bool IsButtonRelease(int32_t button)
{
    return button == MB_LEFTUP || button == MB_RIGHTUP || button == MB_MIDDLEUP;
}

void TPlayScreen::MouseClick(int32_t button, int32_t x, int32_t y)
{
    // REVSYNC: TScreen::MouseClick 0x004904ef..0x004904fc -- a release goes
    // to every pane wherever it lands, the map's too, which leaves one on a
    // side tab's button alone (0x0044f17b) and off its rect only forgets its
    // press or ends the right-button walk (0x0044f917, 0x0044fbad). The HUD's
    // item drag takes its own release (the drop).
    const bool releaseToWorld = IsButtonRelease(button) && !UIDragState::IsActive() &&
                                !(g_playHudInitialized && UISideTabButtonAt(x, y) >= 0);
    switch (ClickRoute(x, y))
    {
      case EClickRoute::Modal:
        TScreen::MouseClick(button, x, y);
        return;
      case EClickRoute::Ignored:
        return;
      case EClickRoute::BuySell:
        TScreen::MouseClick(button, x, y);
        break;
      case EClickRoute::Hud:
        // An item held from the HUD let go over the map: the map pane, the
        // first pane a release reaches (TPlayScreen::Initialize 0x0047ad65),
        // uses it on what is under it or drops it there; then the item panes
        // end the grab.
        if (button == MB_LEFTUP && UIDragState::HeldItem() && !IsOffMapPane(x, y) && UISideTabButtonAt(x, y) < 0)
            CurrentMode()->HandleMouseClick(button, x, y);
        if (!HandleMouseClickUISidebarModeConsumed(button, x, y))
        {
            const SHudState& s = GetHudState();
            TScreen::MouseClick(button, x, y);  // the screen's panes: the bars, the side panes
            if (s.sidebarState == HUD_SIDEBAR_OPEN && s.topSlot == HUD_TOP_EQUIP)
                HandleMouseClickUIEquipMode(button, x, y);
            if (button == MB_LEFTUP)
                UIDragState::ReleaseUnclaimed();    // a drag no pane took ends here
        }
        break;
      case EClickRoute::World:
        TScreen::MouseClick(button, x, y);
        // Route to the active runtime mode; editor mode forwards to the
        // renderer's gizmo / drag picker, game mode forwards to MapPane's
        // gameplay click handler (movement / interact / combat).
        CurrentMode()->HandleMouseClick(button, x, y);
        return;
    }
    if (releaseToWorld)
        CurrentMode()->HandleMouseClick(button, x, y);
}

void TPlayScreen::MouseWheel(int32_t units, int32_t x, int32_t y)
{
    if (ModalHas(MODAL_MOUSE) || GameFlow.Session().Loading() || Editor ||
        AutoMap.IsHidden() || !AutoMap.IsOpen()) return;
    if (x >= AutoMap.GetPosX() && x < AutoMap.GetPosX() + AutoMap.GetWidth() &&
        y >= AutoMap.GetPosY() && y < AutoMap.GetPosY() + AutoMap.GetHeight())
        AutoMap.ScrollZoom(units);
}

void TPlayScreen::MouseMove(int32_t button, int32_t x, int32_t y)
{
    if (ModalHas(MODAL_MOUSE))
    {
        TScreen::MouseMove(button, x, y);
        return;
    }
    if (GameFlow.Session().Loading())
        return;

    if (g_playHudInitialized && !InBuySellDrawer(x, y))
    {
        // A HUD item drag follows the pointer; the paper doll and the
        // screen's panes (the bars, the side panes) see the move.
        if (UIDragState::IsActive())
            UIDragState::UpdateDrag(x, y);
        if (!HandleMouseMoveUISidebarModeConsumed(button, x, y))
        {
            const SHudState& s = GetHudState();
            if (s.sidebarState == HUD_SIDEBAR_OPEN && s.topSlot == HUD_TOP_EQUIP)
                HandleMouseMoveUIEquipMode(button, x, y);
            TScreen::MouseMove(button, x, y);
        }
    }
    else
        TScreen::MouseMove(button, x, y);

    // REVSYNC: TScreen::MouseMove 0x00490530 -- every pane gets every move,
    // with no rect test (0x004905b9..0x0049063d), so the map's comes wherever
    // the pointer is: a right-button walk steers, and a drawn bow aims, over
    // the HUD as over the play field (TMapPane::MouseMove 0x004504d0). Game
    // mode -> MapPane.MouseMove; editor mode has no move handling yet.
    CurrentMode()->HandleMouseMove(button, x, y);
}

// REVSYNC: Joystick @ 0x0047ce80 -> TScreen 0x00490860: the panes. (Nothing
// sends joystick events yet under sokol_app.)
void TPlayScreen::Joystick(int32_t key, bool down)
{
    TScreen::Joystick(key, down);
}

// REVSYNC: Command @ 0x0047cf40, the control map's commands (a key's down
// command, or its up command on release). Every command is held back while
// control is off (the global control-off flag DAT_00666924, 0x0047d009), so
// none of these work in a cutscene or a conversation. The port's command
// numbers are the 1998 table's (playscreen.h), not retail's; each case names
// retail's. Block and leap are held controls UpdateMove polls (retail
// 0x0047de30), so their commands do nothing here, as in 1998.
// TODO(port): the inventory, spell (INVOKE*), use / get and bow commands.
void TPlayScreen::Command(GAMECOMMAND command)
{
    if (!controlon)
        return;
    switch (command)
    {
    // REVSYNC: 0x0047d066 (case 1) -- in the combat root the key ends the
    // fight and turns the player's auto-combat off (until it next finds no
    // one); anywhere else, the bow root too, it starts one on whoever is
    // ahead and turns auto-combat on. Whether the fight starts or ends or
    // not, the switch follows the key.
    case GAMECMD_COMBAT:
        if (Player)
        {
            if (Player->IsCombat())
            {
                Player->EndFighting();
                Player->SetAutoCombat(false);
            }
            else
            {
                TextBar.CommitInput();      // 0x0047d0d5
                Player->BeginFighting(nullptr, ACTION_COMBAT);
                Player->SetAutoCombat(true);
            }
        }
        break;
    case GAMECMD_SIDEPANEL:
        ToggleUISidebarPanel();
        break;
    case GAMECMD_BOTTOMPANEL:
        // Command 5 toggles the bottom drawer: with the shop in it, that
        // closes the shop.
        if (Drawer() == EDrawer::BuySell)
            CloseDrawer();
        else
            ToggleUIBottomPanel();
        break;
    case GAMECMD_MOVEDOWN:          // 0x4a / 0x4b: the run key's down and up
    case GAMECMD_MOVEUP:
        // The mode follows the held flags: run, else sneak, else walk.
        // Retail's sneak is a toggle of its own (case 3, 0x004cf2e0); the
        // port's control table is 1998's, where sneak is held like run.
        if (Player)
        {
            uint32_t state, changed;
            ControlMap.GetCommandFlags(state, changed);
            if (state & CMDFLAG_RUN)        Player->SetRunMode();
            else if (state & CMDFLAG_SNEAK) Player->SetSneakMode();
            else                            Player->SetWalkMode();
        }
        break;
    case GAMECMD_SWING:             // 0x21-0x23: ButtonAttack(1..3) 0x004d2480
    case GAMECMD_THRUST:
    case GAMECMD_CHOP:
        if (Player)
        {
            const int32_t button = command - GAMECMD_SWING + 1;
            const bool ok = Player->ButtonAttack(button);
            log_info("[input] attack button %d -> %s", button, ok ? "started" : "refused");
        }
        break;
    case GAMECMD_COMBO1:  case GAMECMD_COMBO2:  case GAMECMD_COMBO3:
    case GAMECMD_COMBO4:  case GAMECMD_COMBO5:  case GAMECMD_COMBO6:
    case GAMECMD_COMBO7:  case GAMECMD_COMBO8:  case GAMECMD_COMBO9:
    case GAMECMD_COMBO10: case GAMECMD_COMBO11: case GAMECMD_COMBO12:
        if (Player)         // 0x24-0x2f: ButtonAttack(4..15)
            Player->Combo(command - GAMECMD_COMBO1 + 1);
        break;
    case GAMECMD_DODGELEFT:     case GAMECMD_DODGERIGHT:
    case GAMECMD_DODGEUP:       case GAMECMD_DODGEDOWN:
    case GAMECMD_DODGEUPLEFT:   case GAMECMD_DODGEUPRIGHT:
    case GAMECMD_DODGEDOWNLEFT: case GAMECMD_DODGEDOWNRIGHT:
        if (Player)         // 0x30-0x37 (0x0047d900): Dodge(0..7)
            Player->Dodge(command - GAMECMD_DODGELEFT);
        break;
    case GAMECMD_BELTUSE1: case GAMECMD_BELTUSE2: case GAMECMD_BELTUSE3:
    case GAMECMD_BELTUSE4: case GAMECMD_BELTUSE5:
        if (Player)         // 0x18-0x1c (0x0047d882): UseBelt(0..4) 0x0051e420
            Player->UseBelt(command - GAMECMD_BELTUSE1);
        break;
    case GAMECMD_USE:               // 0xe: Use 0x004d01e0
        if (Player)
            Player->TryUse();
        break;
    case GAMECMD_GET:               // 0xf: Get 0x004d0430
        if (Player)
            Player->TryGet();
        break;
    case GAMECMD_JUMP:
        if (Player)
            Player->Jump();
        break;
    case GAMECMD_GAMEOPTIONS:       // 0x52: 0x0047e700
        ingamemenu->OpenOptions();
        break;
    case GAMECMD_LOADGAME:          // 0x53: = 0x0047e660
        ingamemenu->OpenLoad();
        break;
    case GAMECMD_SAVEGAME:          // 0x54: the thumbnail (0x0047dc05), then the dialog
        if (InGameMenuOpen())
            break;
        menuPending = true;
        ::SaveGame.CaptureThumbnail({}, [this] {
            menuPending = false;
            if (CurrentScreen == this && !IsDone())
                ingamemenu->OpenSave();
        });
        break;
    case GAMECMD_QUICKSAVE:         // 0x55: the thumbnail (0x0047dd08), QuickSave 0x0047e850
        GameFlow.Session().RequestQuickSave();
        break;
    default:
        break;
    }
}

void TPlayScreen::PlayMovie(const char* path)
{
    if (movieplaying)
        return;

    audio::MusicStop();
    movieplaying = true;
    movie.Initialize();
    movie.SetOnFinished([this] { movie.EndModal(0); });
    // Retail's player blocked the frame loop: the movie alone takes input,
    // pulses and draws.
    PushModal(&movie, MODAL_GAME | MODAL_ANIMATE, [this](int32_t) {
        movie.Close();
        movieplaying = false;
    });
    movie.Open(path);           // one that can't play ends at once
}

// REVSYNC: UpdateMove = retail 0x0047de30. Each tick it reads the held
// command flags: direction (Go / Leap), block, and, with no direction held,
// a Stop. The movement mode is not here: run and sneak change on their
// keys' commands (Command, GAMECMD_MOVEDOWN / MOVEUP). `changed` is not a
// per-tick edge mask: the control map only ever ORs bits into it (retail
// 0x0065a9c8 too), so it reads "a held control changed at some point".
void TPlayScreen::UpdateMove()
{
    if (!Player) return;

    uint32_t state, changed;
    ControlMap.GetCommandFlags(state, changed);

    // REVSYNC-DIVERGENCE: retail takes the lowest held direction bit (two
    // arrow keys walk along one of them). Synthesize diagonal flags from adjacent cardinals so keyboards
    // without a Home / PgUp / End / PgDn cluster can still walk
    // diagonally with two arrow keys. The retail bit-scan picks the
    // lowest set CMDFLAG bit, so without this UP+RIGHT would yield
    // pure RIGHT instead of UPRIGHT. We OR the diagonal in and clear
    // the cardinals so the scan resolves cleanly.
    auto synthesize_diagonal = [](uint32_t s, uint32_t a, uint32_t b, uint32_t diag) -> uint32_t {
        if ((s & a) && (s & b)) s = (s & ~(a | b)) | diag;
        return s;
    };
    state = synthesize_diagonal(state, CMDFLAG_UP,   CMDFLAG_RIGHT, CMDFLAG_UPRIGHT);
    state = synthesize_diagonal(state, CMDFLAG_UP,   CMDFLAG_LEFT,  CMDFLAG_UPLEFT);
    state = synthesize_diagonal(state, CMDFLAG_DOWN, CMDFLAG_RIGHT, CMDFLAG_DOWNRIGHT);
    state = synthesize_diagonal(state, CMDFLAG_DOWN, CMDFLAG_LEFT,  CMDFLAG_DOWNLEFT);

    // Bow-aim mode: left/right adjust aim instead of moving.
    if (Player->IsBowMode() && Player->IsBowDrawn())
    {
        if      (state & CMDFLAG_LEFT)  Player->AimBowLeft();
        else if (state & CMDFLAG_RIGHT) Player->AimBowRight();
        else                            return;
    }

    // Direction flags occupy bits 0..7 in clockwise order starting
    // from UPRIGHT; the lowest set bit picks the angle (steps of 32).
    int32_t angle = 0;
    int32_t c;
    for (c = 1; c < (1 << 8) && !(c & state); c <<= 1, angle += 32);
    if (angle > 255) angle = -1;  // no direction held

    // REVSYNC: UpdateMove @ 0x0047de30 -- leap in a combat or bow root;
    // else Go, unless a move already heads that way (the doing block's move
    // angle: holding a direction calls Go once, not every tick). (The debug
    // camera, 0x006671f0 set by the dev 'X' key, turns the direction and
    // has a mode of its own; not ported.)
    if (angle >= 0)
    {
        const TActionBlock* doing = Player->DoingBlock();
        const bool moving = doing && (doing->action == ACTION_MOVE || doing->action == ACTION_COMBATMOVE ||
                                      doing->action == ACTION_BOWMOVE);
        if (Player->IsFighting() && (state & CMDFLAG_LEAP))
            Player->Leap(angle);
        else if (!moving || doing->moveangle != angle)
            Player->Go(angle);
    }
    else if (changed && Player->IsMoving() && !Player->IsGoto())
    {
        Player->Stop();
    }

    // Block / unblock toggle (combat mode).
    if ((state & CMDFLAG_BLOCK) &&
        Player->IsFighting() && !Player->IsDoing(ACTION_BLOCK))
        Player->Block(10000);
    if (!(state & CMDFLAG_BLOCK) && Player->IsDoing(ACTION_BLOCK))
        Player->StopBlock();
}

// *************************************************************************
// * Mode flags                                                            *
// *************************************************************************

void TPlayScreen::SetFullScreen(bool on)
{
    fullscreen = on;
    interfacedirty = true;
}

// REVSYNC: 0x0047c550 -- demo mode: the main player plays itself (its AI
// flag, set by name in retail: 0x00472db0 "AI"); leaving it gives the player
// control back.
void TPlayScreen::SetDemoMode(bool on)
{
    demomode = on;
    if (!on)
        SetControlOn(true);
    if (Player)
        Player->SetFlag(OF_AI, on);
}

// REVSYNC: 0x0047c580 -- control on also ends demo mode. Control off lets go
// of the right button (walking) and the movement keys through the screen's
// own handlers, so nothing held keeps the player moving into a cutscene.
void TPlayScreen::SetControlOn(bool on)
{
    if (controlSeam)
    {
        controlSeam(on);
        return;
    }
    controlon = on;
    if (on)
    {
        demomode = false;
        return;
    }
    MouseClick(MB_RIGHTUP, 1, 1);
    for (const int32_t key : { VK_UP, VK_LEFT, VK_RIGHT, VK_DOWN, int32_t('R'),
                               VK_NEXT, VK_PRIOR, VK_HOME, VK_END })
        KeyPress(key, false);
}

void TPlayScreen::HideLowerPanes()
{
    // TODO(port): when inventory / health / stamina / textbar panes are
    // re-attached, drive their .Hide() here. Stubbed during scaffold.
}

void TPlayScreen::ShowLowerPanes()
{
    // TODO(port): symmetric reveal.
}

void TPlayScreen::ShowStatPane()
{
    SHudState& hud = GetHudState();
    hud.topSlot = HUD_TOP_STATS;
    hud.sidebarState = HUD_SIDEBAR_OPEN;
    StatPane.SetDirty(true);
}

// *************************************************************************
// * Game time                                                             *
// *************************************************************************

// REVSYNC: the frame count retail's callers read inline (`[+0x688] -
// [+0x684] + [+0x680]`): a loaded game's frames go on from its saved time.
int32_t TPlayScreen::GameFrame() const
{
    return lastsessionframes - sessionstart + gameframes;
}

// REVSYNC: TPlayScreen::Animate @ 0x0047c2c0 -- the frame count, then game
// time in hundredths of a second at 24 frames a second from the game's
// whole frame count (GameFrame() * 100 / 24, 0x0047c36b..0x0047c3af; the
// regen, poison and recovery timers read it), then the time of day.
void TPlayScreen::AdvanceGameFrame()
{
    ++gameframes;
    gametime = (int32_t)((int64_t)GameFrame() * 100 / 24);
    timeofday = TimeOfDayMinutes(gametime);
}

// REVSYNC: TPlayScreen::SetGameTime @ 0x0047e950 -- the saved game time
// becomes the frames before this session (t * 24 / 100), which GameFrame
// goes on from, and the minute of the day is set at once, so the area
// entered on load sees the saved time. The port also sets gametime at once;
// retail left it to the next Animate.
void TPlayScreen::SetGameTime(int32_t t)
{
    sessionstart = gameframes;
    lastsessionframes = int32_t(int64_t(t) * 24 / 100);
    gametime = int32_t(int64_t(lastsessionframes) * 100 / 24);
    timeofday = TimeOfDayMinutes(gametime);
}

int32_t TPlayScreen::TimeOfDay() const
{
    return timeofday;
}

// The bounds of the twilight ramps, TWILIGHT minutes centred on 06:00 and
// 18:00 (retail Daylight and DayTimeFlag both compute them inline).
namespace {
struct STwilight
{
    int32_t morningbeg, morningend, eveningbeg, eveningend;
};

STwilight Twilight()
{
    const int32_t half = Rules.twilight >> 1;
    STwilight t;
    t.morningbeg = 6 * 60 - half;
    t.morningend = t.morningbeg + Rules.twilight - 1;
    t.eveningbeg = 18 * 60 - half;
    t.eveningend = t.eveningbeg + Rules.twilight - 1;
    return t;
}
}  // namespace

// REVSYNC: TPlayScreen::Daylight @ 0x0047ea00 -- the daylight, 0 (night) to
// 255 (day): 0 outside the twilights, 255 between them, a ramp across each.
// With RealTimeLight on (0x0047ea46) the ramp is smooth; off, it moves in
// TWILIGHT's step count (rules.def: 8 steps over 60 minutes).
int32_t TPlayScreen::Daylight() const
{
    const STwilight tw = Twilight();
    const int32_t time = timeofday;
    if (time > tw.eveningend || time < tw.morningbeg)
        return 0;
    if (time > tw.morningend && time < tw.eveningbeg)
        return 255;

    // Minutes into the morning ramp, or left of the evening one.
    const int32_t into = (time <= tw.morningend) ? time - tw.morningbeg
                                                 : Rules.twilight - (time - tw.eveningbeg);
    if (RealTimeLight)
        return into * 255 / Rules.twilight;
    const int32_t step = into * Rules.twilightsteps / Rules.twilight;
    return step * 255 / Rules.twilightsteps;
}

// REVSYNC: TPlayScreen::DayTimeFlag @ 0x0047eab0 -- DAY_MIDNIGHT at minute 0,
// DAY_NOON at 720, else night, day, morning or evening by the twilights.
int32_t TPlayScreen::DayTimeFlag() const
{
    const STwilight tw = Twilight();
    const int32_t time = timeofday;
    if (time == 0)
        return DAY_MIDNIGHT;
    if (time == 12 * 60)
        return DAY_NOON;
    if (time > tw.eveningend || time < tw.morningbeg)
        return DAY_NIGHT;
    if (time > tw.morningend && time < tw.eveningbeg)
        return DAY_DAYTIME;
    return (time <= tw.morningend) ? DAY_MORNING : DAY_EVENING;
}

// Debug: the same day's game time at `minutes` past midnight.
void TPlayScreen::SetTimeOfDay(int32_t minutes)
{
    if (Rules.daylength <= 0)
        return;
    const int64_t day = int64_t(gametime) / Rules.daylength;
    SetGameTime(int32_t(day * Rules.daylength + int64_t(minutes) * Rules.daylength / 1440));
}

// *************************************************************************
// * Post-character overlays (stub)                                        *
// *************************************************************************

void TPlayScreen::AddPostCharAnim(int32_t /*x*/, int32_t /*y*/, int32_t /*z*/,
                                  PTBitmap /*bm*/, uint32_t /*drawmode*/, int32_t /*dim*/)
{
    // TODO(port): forward to TMapRenderer overlay queue once the spell-
    // / damage-overlay path is added there. Currently dropped on the
    // floor.
}

void TPlayScreen::AddPostCharText(char* /*text*/, int32_t /*x*/, int32_t /*y*/,
                                   PSColor /*color*/, int32_t /*wrapwidth*/)
{
    // TODO(port): same as AddPostCharAnim, for floating dialog text.
}

// *************************************************************************
// * Legacy hooks kept as no-ops                                            *
// *************************************************************************

void TPlayScreen::CreateBackgroundAreas() { /* legacy backdrop dead */ }
void TPlayScreen::DrawOverhangs(bool /*temporary*/) { /* ditto */ }

// *************************************************************************
// * TScreen pulse-driver hook                                              *
// *************************************************************************

TScreen* TPlayScreen::ShowScreen(TScreen* screen, int32_t /*ticks*/)
{
    // The base TScreen's ShowScreen handled fade transitions and timed
    // hand-offs to LogoScreen. We don't have those wired yet -- just
    // return the same screen so the main loop keeps driving it.
    return screen;
}

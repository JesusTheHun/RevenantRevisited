// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                  RevMain.cpp - System main module                     *
// *************************************************************************

#include <stdarg.h>

// For multimonitor support
#define COMPILE_MULTIMON_STUBS
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <fcntl.h>
#include <math.h>
#include <cmath>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

#include "argh.h"
#include "vfxpointoption.h"

#include "revenant.h"
#include "revisited.h"
#include "assetcache.h"
#include "audio_backend.h"
#include "framesnap.h"
#include "gameoptions.h"
#include "headless_window.h"
#include "logging.h"
#include "fonttable.h"
#include "3dscene.h"
#include "bitmap.h"
#include "display.h"
#include "gamecanvas.h"
#include "graphics.h"
#include "module.h"
#include "playscreen.h"
#include "playerbot.h"
#include "playtest.h"
#include "shadow/shadowrun.h"
#include "revisited_defaults.h"
#include "revisited_settings.h"
#include "testscreen.h"
#include "cinematicscreen.h"
#include "death.h"
#include "gameflow.h"
#include "logoscreen.h"
#include "i3dgltf.h"
#include "testmodes.h"
#include "testconfig.h"
#include "sceneviewport.h"
#include "time.h"
#include "combattrace.h"
#include "mappane.h"
#include "object.h"
#include "automap.h"
#include "inputqueue.h"
#include "inventory.h"
#include "mainwnd.h"
#include "savegame.h"
#include "screen.h"
#include "font.h"
#include "chunkcache.h"
#include "timer.h"
#include "equip.h"
#include "parse.h"
#include "mapmanager.h"
#include "player.h"
#include "multi.h"
#include "multictrl.h"
#include "spell.h"
#include "spellbook.h"
#include "spellpane.h"
#include "statpane.h"
#include "retailab.h"
#include "script.h"
#include "textbar.h"
#include "sound.h"
#include "editorstub.h"
#include "dls.h"
#include "ctrlmap.h"
#include "area.h"
#include "rules.h"
#include "dialog.h"
#include "debugui.h"
#include "cursor.h"
#include "platform/cursor.h"

#include <sokol_app.h>

// TODO(port): Subsystem 7 — MSVC CRT debug heap (crtdbg.h) not available on
// Clang/macOS. Stub the macros so assertions compile. Real validation should
// come from AddressSanitizer and a portable heap checker.
#define _CrtCheckMemory() (1)
#define _CRT_ERROR 0
#define _RPT0(mode, msg) ((void)0)
#define _RPT1(mode, msg, a) ((void)0)
#define _RPT2(mode, msg, a, b) ((void)0)

// Global Variables
// TODO(port): hInstance / StartMemory / PauseMutex were Win32-only.
// PauseMutex was for the loader-thread pause; Subsystem 4 will replace it
// with a worker-pool pause primitive. StartMemory was a GlobalMemoryStatus
// snapshot at WinMain entry, used only in debug prints. Dropped here.
HANDLE PauseMutex = nullptr;

// Game directories
char RunPath[MAXPATHLEN];
char SavePath[MAXPATHLEN];

// Define the Editor paths
char ClassDefPath[MAXPATHLEN];               // Where to load / save Class.Def
char ExileRCPath[MAXPATHLEN];                // Where to run ExileRC from & where
                                             // the graphics for the resources are
char ResourcePath[MAXPATHLEN];               // Where to read / write the resources
char ImageryPath[MAXPATHLEN];                // Imagery tree / imagery.rvi; class.def and rules rosters
char ModulesPath[MAXPATHLEN];                // Root of the game modules
char BaseMapPath[MAXPATHLEN];                // Where the untouched version of the game map is stored
char MoviePath[MAXPATHLEN];                  // Where the .smk movies live
char SaveGamePath[MAXPATHLEN];               // Root of the save slots (INI [Paths] SaveGamePath)

// Current language
TString Language;                   // Where the current map is stored

// Active and next game screens
TScreen*        CurrentScreen;      // Currently displayed screen object
TScreen*        NextScreen;         // Next Screen to be display object

// Loaded game assets
TFont*          SystemFont;         // Basic utility font for game
TFont*          DialogFont;         // Dialog font
TFont*          DialogFontShadow;   // Dialog font shadow
TFont*          SmallFont;          // Small game font
TFont*          GameFont;           // Medium game font
TFont*          GoldFont;           // Medium gold font
TFont*          MetalFont;          // Small gold/metal font
TFont*          MenuFont;           // Menu font
TPlayer*        Player;             // Main player for the game
TMulti*         GameData;           // Multiresource for in game data

// Global singleton objects
TDisplay        Display;            // Display object (TODO: see revenant.h note)
T3DScene        Scene3D;            // Display pointer
TPlayScreen     PlayScreen;         // PlayScreen Object
TMapPane        MapPane;            // Map pane for PlayScreen
TInventory      Inventory;          // Inventory of objects
TTextBar        TextBar;            // Info line for player
TMultiCtrlPane  MultiCtrl;          // Multipane control panel (buttons)
TEquipPane      EquipPane;          // Equipment pane
TAutoMap        AutoMap;            // Automapper
TSpellPane      SpellPane;          // Talisman stuff
TSpellBookPane  SpellBook;          // The spells the main player knows
TQuickSpellPane QuickSpells;        // Quick spell buttons
TStatPane       StatPane;           // Display of char's abilities
TScriptManager  ScriptManager;      // Manages all scripts in the game
TMainWindow     MainWindow;         // Windows Object
TSaveGame       SaveGame;           // SaveGame Object
TChunkCache     ChunkCache;         // Tile Cache
TTimer          Timer;              // Timer Object
#if 0 // TODO(port): Subsystem 3 — video capture (DirectShow → AVFoundation/sokol)
TVideoCapture   VideoCapture;       // Video capture object
#endif
TSoundPlayer    SoundPlayer;        // Sound effects player
TControlMap     ControlMap;         // Contains the key/joystick mappings for game control
TAreaManager    AreaManager;        // Manages the game area system
TPlayerManager  PlayerManager;      // Manages the game player list
TAssetCache     AssetCache;         // Shared engine CPU/source asset cache
TMapManager     MapManager;         // Cache of loaded TGameMap levels +
                                    // current-map pointer; renderer / pane
                                    // listen for CurrentMapChanged.
TRules  Rules;                      // Manages global rules data (classes, chars, stats for attacks, etc.)
// TODO(port): CRITICAL_SECTION replaced by Subsystem 4 threading work
// (worker pool + async completions). Removed here; revutils.cpp still
// references it via the stub in BEGIN_CRITICAL / END_CRITICAL below.
TSpellList      SpellList;          // a list of spells in the game
TDialogList     DialogList;         // List of dialog and other game messages for current language

// Game speed variable
int32_t GameSpeed = 3;              // Value 1-5 which determines how fast the game is running
                                    // Used to switch on/off processor intensive effects
// Violence Level
int32_t ViolenceLevel = 5;          // Value 0-5 which determines how much blood and gore, etc. to use
                                    // 0 = none

// Total size of preload area (in sectors) if PreloadSectors is on
int32_t PreloadSectorSize = -1;

// Total size of chunk cache to allocate (in MB's)
int32_t ChunkCacheSize = -1;    // -1 is calculate based on available physical memory

// Clear before drawing update buffers?
bool ClearBeforeDraw = true;        // Clear before drawing update buffers?
bool NoFrameSkip     = false;       // Disables frame skipping on slowdown
bool PauseFrameSkip  = false;       // Set during disk loading to avoid skipping afterwards
bool NoScrollZBuffer = true;        // Disables updating to video memory ZBuffer
bool WaitVertRetrace = true;        // Wait vert retrace (not used)
bool ShowDrawing     = false;       // Flips front and back buffer so you can see drawing
bool NoNormals       = true;        // Changes to no normals lighting mode
bool SmoothScroll    = true;        // Do smooth scrolling
bool ScrollLock      = true;        // Lock scrolling to the character
bool DrawRealtimeShadows = true;    // Draw alpha shadows for 3D characters
bool Show3D          = true;        // Turn on the 3D system
bool Interpolate     = true;        // Causes 3D animations to interpolate when state changes
bool DoPageFlip      = true;        // Allows the PageFlip function to flip pages
bool PauseWhenNotActive = true;     // Causes game to pause when not active
bool AppActive       = true;        // Flag for if the game is the active application
                                    // (starts true — sokol_app doesn't always deliver FOCUSED
                                    // at launch on macOS, and the window is already active)
bool NoWideBuffers   = false;       // Doesn't allow video buffers with stides wider than their widths
bool UseDirect3D2    = true;        // True if you want to be able to use drawprimive stuff
bool UseSoftware3D   = false;       // Use software 3D (False assumes we want hardware if available)
bool SoftRampMode    = true;        // Use RAMP mode for software 3D (default)
bool SoftRGBMode     = false;       // Use RGB mode for software 3D
bool UseBlue         = true;        // Use BLUE software renderer instead of DirectX
bool UseDrawPrimitive = true;       // If false, 3D system does everything with Execute Buffers (always false if UseDirect3D2 is false)
bool CacheExBufs     = true;        // Attempt to cache execute buffers for chars and other animation
bool PreloadSectors  = true;        // If true, will load a large sector cache for faster scrolling
bool NoPulseObjs     = false;       // Prevents the game objects from being pulsed
bool NoAnimateObjs   = false;       // Prevents the game objects from being animated
bool NoVidZBufLock   = false;       // Prevents simultaneous locking of video and zbuffer (for voodoo)
bool NoBlitZBuffer   = false;       // Card is incapable of blitting to the display ZBuffer (voodoo)
bool UseClearZBuffer = false;       // Use a clear z buffer for z restores (i.e. Viewport->Clear())
bool IsVooDoo        = false;       // True if using a voodoo card
bool IsMMX           = false;       // Has MMX extensions
bool NoAI            = false;       // Turns off monster AI
bool CheatNahkranoth = false;       // retail 0x00668108 (IsValidAttack, Damage); text-bar cheat "nahkranoth"
bool CheatAlreadyDead = false;      // retail 0x00668104 (Damage; also 0x004c2a20, 0x004d3590); text-bar cheat "alreadydead"
bool AutoBeginCombat = true;        // Automatically begins combat if enemy in range and facing him
bool PlaySpeech      = true;        // Play speech wave files
bool ShowDialog      = false;       // Show dialog lines (always shows if no speech file found)

// Multi-Monitor Variables
int32_t MonitorNum = 0;         // Monitor game will run on (0=default (primary), 1=monitor 1, 2=monitor 2, etc.)
int32_t MonitorX = 0;           // Relative position of monitor in desktop coordinates
int32_t MonitorY = 0;
int32_t MonitorW = 640;         // Relative position of monitor in desktop coordinates
int32_t MonitorH = 480;
// TODO(port): HMONITOR / MONITORINFOEX were Win32 state populated by the
// retired MonitorEnumProc. sokol_app doesn't expose a monitor index; the
// engine will need reworked multi-display support or we drop the feature.

// Game flags
bool Windowed = false;      // Do we run the game in a window in NORMAL mode (instead of EXLUSIVE)
bool SingleBuffer = false;  // True if single buffering (true if Windowed=true, or UsingHardware=false)
bool Borderless = false;    // When windowed, cut out the border (menu, other windows garabage)?
bool Debug = false;         // Should program be run in debug compatible mode?
bool Ignore3D = false;      // Whether to disallow 3D imagery (to make it run on systmes without Direct3D)
// `Editor` global moved to src/editor.cpp; revmain just references it.
extern bool Editor;
bool StartInEditor = false; // Whether to start the program in editor mode
bool RevisitedEnabled = false; // --revisited: mount Revisited overlay (FATAL if requested but missing)
bool NoQuickLoad = false;   // true if program should not try to use IMAGERY.DAT file
bool OldSaves = false;      // true to save retail slot folders instead of save files
bool Force15Bit = false;    // Forces video mode to assume 15 bit
bool Force16Bit = false;    // Forces video mode to assume 16 bit
bool FullScreen = false;    // Full screen mode
bool ShowZBuffer      = false;  // Show Z buffer for debugging
bool ShowNormalBuffer = false;  // Show normal buffer for debugging
// UnlockImmediately = true was a DirectDraw-era debug aid (let you
// step through the CPU rasterizer with the surface unlocked). Under
// the sokol GPU path, Unlock() is where the CPU staging buffer is
// uploaded to the GPU texture -- unlocking BEFORE the rasterizer writes
// uploads the (zeroed) buffer and the actual pixels never make it to
// the GPU. Every Display.Put / Display.Box silently no-ops.
// Default must be false on the new path.
bool UnlockImmediately = false;

// Video capture globals (set in command line parse, used after display is initialized)
static bool dovideocap = false;
static int32_t videocapmegs, videocapfps;

// Driver name string (allows user to select driver by just typing in name of card)
char DXDriverMatchStr[FILENAMELEN]; // Will use first DX driver who's description has the given string in it
                                    // i.e. use if string is "permidia" and driver desc is "Glint Permidia 2 3D"

// Optional save slot requested via `--loadmap=...` on the command line; the
// boot quickstarts into it.
char StartupSavePath[MAXPATHLEN] = "";
bool StartupSaveCycle = false;
int32_t StartupSaveCycleSettle = 60;

// Window/backbuffer size selected before sokol creates the native window.
// WIDTH/HEIGHT stay as the classic 640x480 layout baseline; the renderer and
// display surfaces use these runtime values.
int32_t GameScreenWidth = WIDTH;
int32_t GameScreenHeight = HEIGHT;

// Tick Sync variable
bool TickOccured = false;

// Disable Screen calls to timer functions
bool DisableTimer = false;

// Last Frame Milliseconds (Do we show it?)
int32_t LastFrameTicks;
bool ShowFramesPerSecond = false;

// Character movement mode
bool GridSnap = false;

// Music and sound effects
bool SoundSystemOn = true;

// Is Control, Shift, or Alt down
bool CtrlDown, ShiftDown, AltDown;

// Is System Closing Down?
bool Closing = false;

// For setting up 3D object bounding rects in the editor
bool UpdatingBoundingRect = false;

// Is joystick available?
bool HasJoyStick = false;
// TODO(port): JOYINFO was Win32 joyGetPos result; Subsystem 7 (input)
// will rework joystick support via sokol_app/IOKit.

// Memory used by program
uint32_t ImageryMemUsage = 0;

// Frame ticks for doubletap checking
int32_t DoubleTapTicks = 6;

// Global Map Pane Sizes (Set to initial frame sizes to insure
int32_t MAPPANEX, MAPPANEY, MAPPANEWIDTH, MAPPANEHEIGHT;
int32_t SCROLLBUFWIDTH, SCROLLBUFHEIGHT;

// ************************************************************************
// *                          Support Functions                           *
// ************************************************************************
//
// ATTIC: every utility below (thread pause gate, itos/stricmp, file copy,
// tickcount, FatalError/Error/Status, INI access, xmalloc family, random,
// list helpers, Mem*/*Phys/*Page) now lives in revutils.cpp. The 1998
// bodies are kept under `#if 0` so a reviewer can diff the old Win32 paths
// against the macOS-oriented rewrites. Delete this block once the port
// stabilizes.

#if 0 // ATTIC: duplicated in revutils.cpp
bool ThreadsPaused = false;

void PauseThreads()
{
    ThreadsPaused = true;
    TObjectImagery::PauseLoader();
    PAUSEUPDATE;
}

void ResumeThreads()
{
    ThreadsPaused = false;
    TObjectImagery::ResumeLoader();
    MapPane.ResumeUpdate();
}

// *************** Simple Support Functions *****************

char *itos(int32_t val, char *buf, int32_t buflen)
{
    if (!buf)
        return buf;
    if (buflen < 2)
    {
        if (buf != nullptr)
            buf[0] = '\0';
        return buf;
    }
    if (buflen >= 12) // Twelve is big enough for anything
    {
        snprintf(buf, buflen, "%d", val);
        return buf;
    }
    char b[12];
    snprintf(b, sizeof(b), "%d", val);
    strncpy(buf, b, buflen - 1);
    buf[buflen - 1] = '\0';
    return buf;
}

int32_t stricmp(const char* s1, const char* s2)
{
  assert(s1 != NULL);
  assert(s2 != NULL);

#ifdef HAVE_STRCASECMP
    return strcasecmp(s1, s2)
#else
    while (tolower((unsigned char) *s1) == tolower((unsigned char) *s2)) 
    {
        if (*s1 == '\0')
        {
            return 0;
        }
        s1++;
        s2++;
    }

    return (int) tolower((unsigned char) *s1) -
        (int) tolower((unsigned char) *s2);
#endif
}

#if 0 // TODO(port): Subsystem 6 — filesystem enumeration (→ POSIX opendir/readdir)
      // Duplicate of the canonical implementations in revutils.cpp — kept
      // disabled here until the enumeration port lands, then this dead copy
      // will be deleted outright.
// Copies a file, or group of files using wildcards
int32_t copyfiles(const char *from, const char *to, bool overwrite)
{
    if (!from || !to)
        return 0;

    struct _finddata_t data;

    char fdrive[_MAX_DRIVE];
    char fdir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char fext[_MAX_EXT];

    char fromdir[MAXPATHLEN];
    char todir[MAXPATHLEN];
    char source[MAXPATHLEN];
    char dest[MAXPATHLEN];

    _splitpath(from, fdrive, fdir, fname, fext);
    sprintf(fromdir, "%s%s", fdrive, fdir);
    if (!fromdir[0])
        return 0;
    if (fromdir[strlen(fromdir) - 1] != '\\')
        strncatz(fromdir, "\\", MAXPATHLEN);

    _splitpath(to, fdrive, fdir, fname, fext);
    sprintf(todir, "%s%s", fdrive, fdir);
    if (!todir[0])
        return 0;
    if (todir[strlen(todir) - 1] != '\\')
        strncatz(todir, "\\", MAXPATHLEN);

    int32_t copied = 0;

    int32_t found, handle;
    found = handle = _findfirst(from, &data);
    while (found != -1)
    {
        sprintf(source, "%s%s", fromdir, data.name);
        sprintf(dest, "%s%s", todir, data.name);

        if (CopyFile(source, dest, !overwrite))
            copied++;

        found = _findnext(handle, &data);
    }

    return copied;
}

// Deletes a file, or group of files using wildcards
int32_t deletefiles(const char *name)
{
    struct _finddata_t data;

    char dir[MAXPATHLEN], file[MAXPATHLEN];

    char fdrive[_MAX_DRIVE];
    char fdir[_MAX_DIR];
    char fname[_MAX_FNAME];
    char fext[_MAX_EXT];

    _splitpath(name, fdrive, fdir, fname, fext);
    sprintf(dir, "%s%s", fdrive, fdir);

    int32_t deleted = 0;

    int32_t found = _findfirst(name, &data);
    while (found != -1)
    {
        strncpyz(file, dir, MAXPATHLEN);
        strncatz(file, data.name, MAXPATHLEN);
        if (DeleteFile(file))
            deleted++;

        found = _findfirst(name, &data);
    }

    return deleted;
}
#endif

#if 0 // TODO(port): Subsystem 1 — duplicate of revutils.cpp flen()/tickcount(); retired here
uint64_t flen(FILE* f)
{
    fseek(fp, 0L, SEEK_END);
    uint64_t sz = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    return sz;
}

// Returns system ticks in milliseconds since system was turned on
uint32_t tickcount()
{
    return GetTickCount();
}
#endif

// *************** Error Functions *****************

void ThreadError(char *error, char *extra)
{
    char buf[101];
    snprintf(buf, sizeof(buf), error, extra);

#if 0 // TODO(port): Subsystem 2 — DirectDraw/Win32 presentation (→ sokol)
    if (extra)
        _RPT1(_CRT_ERROR, error, extra);
    else
        _RPT0(_CRT_ERROR, error);

//  MSG Message;
//  while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE))
//  {
//      TranslateMessage(&Message);
//      DispatchMessage(&Message);
//  }

//  MessageBox(nullptr, buf, "FATAL ERROR", MB_ICONSTOP | MB_OK);
#else
    if (extra)
        fprintf(stderr, error, extra);
    else
        fprintf(stderr, "%s", error);
    fprintf(stderr, "\n");
#endif
}

void FatalError(const char *error, const char *extra)
{
    static bool alreadyin;

    if (alreadyin)
        exit(1);
    alreadyin = true;

    char buf[101];
    sprintf(buf, error, extra);

  // Write error string to display
    Status(buf);
    Status("Press any key to exit");

  // FatalError aborts the process; the OS reclaims everything. Skipping
  // the partial teardown dance keeps this path from racing the live
  // ShutdownGlobals path and matches the "InitGlobals/ShutdownGlobals
  // are the canonical callers" rule for file-scope globals.
    if (extra)
        fprintf(stderr, error, extra);
    else
        fprintf(stderr, "%s", error);
    fprintf(stderr, "\n");

    exit(1);
}

void Error(const char *error, const char *extra)
{
    FatalError(error, extra);
/*
    char buf[101];
    sprintf(buf, error, extra);

  // Process windows messages (for Jong)
    MSG Message;
    while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&Message);
        DispatchMessage(&Message);
    }

    MessageBox(nullptr, buf, "FATAL ERROR", MB_ICONSTOP | MB_OK);

    exit(1);
*/
}

// Duplicate of revutils.cpp definitions. Agent A rewrites revmain.cpp
// around sokol_app; this block will be replaced wholesale there.
#if 0
void WaitSingleErr(HANDLE obj) {}
void WaitMultipleErr(uint32_t objs, const HANDLE *obj, bool all) {}
#endif

// *************** Display Status Functions *****************

void Status(const char *fmt, ...)
{
#if 0 // TODO(port): Subsystem 2 — DirectDraw/Win32 presentation (→ sokol)
    static char buf[1024]; // Temporary buf for output
    static y = 10;

    if (!DirectDraw || !SystemFont || !Display.GetSurface())
        return;

    va_list marker;
    va_start(marker, fmt);
    vsprintf(buf, fmt, marker);

    char *s = buf;
    char *p = buf;
    while (1)
    {
        if (*p == '\n' || *p == '\0')
        {
            char save = *p;
            *p = '\0';
            if (strlen(s) > 0)
            {
                Display.WriteText(s, 10, y, 1, SystemFont, nullptr, DM_TRANSPARENT | DM_ALIAS);
                Display.PutToScreen(0, 0, Display.Width(), Display.Height());
                y += 10;
            }
            if (!save)
                break;
            s = p + 1;
        }
        p++;
    }
#else
    va_list marker;
    va_start(marker, fmt);
    vfprintf(stderr, fmt, marker);
    va_end(marker);
    fprintf(stderr, "\n");
#endif
}

// *************** Critical Section Functions *****************
// TODO(port): these are stubs; Subsystem 4 (threading) will land proper
// mutex-backed critical sections. Safe as no-ops while we are single-
// threaded during Phase 2 bringup.

void BEGIN_CRITICAL() {}
void END_CRITICAL() {}

// ****** Exit the Game - Why would anyone want to do that? ******

void ExitGame()
{
    sapp_request_quit();
}

// *************** Settings Functions *****************
//
// The INI accessor family (INISetPath/INIGetInt/INIGetBool/...) lives in
// revutils.cpp now — the revutils port rebuilt them on top of CSimpleIniA.
// The Win32 GetPrivateProfile* bodies that used to live here have been
// retired to attic-final:attic/src/revutils_win32_ini.cpp. Leaving the banner as a
// landmark for anyone grepping.
#if 0  // ATTIC: retired duplicate — see revutils.cpp for the live impl

static TString INIPath;
static TString INISection;

void INISetSection(const char* newsection)
{
    INISection = newsection;
}

void INISetPath(const char *runpath)
{
    char *ininame = "Revenant.ini";

    strcpy(INIPath, SavePath);
    strcat(INIPath, ininame);

  // Make sure INI file is in writable (SavePath) directory
    if (stricmp(RunPath, SavePath) != 0) // Run/Save path are different
    {
        FILE *fp = fopen(INIPath, "r");
        if (!fp)    // INI file not in SavePath
        {
            char from[MAXPATHLEN];
            strcpy(from, RunPath);
            strcat(from, ininame);
            copyfiles(from, INIPath);
        }
    }
}

int32_t INIGetInt(const char *key, int32_t def, char *format)
{
    int32_t i = GetPrivateProfileInt(INISection, key, def, INIPath);

    INISetInt(key, i, format);

    return i;
}

void INISetInt(const char *key, int32_t i, char *format)
{
    if (!format)
        format = "%d";

    char buf[20];
    sprintf(buf, format, i);

    WritePrivateProfileString(INISection, key, buf, INIPath);
}

char *INIGetText(const char *key, char *def, char *buf, int32_t buflen)
{
    static char s[128];
    if (!buf)
    {
        buf = s;
        buflen = 128;
    }

    if (!def)
        def = "";

  // Quote the string
    char qdef[128];
    strncpyz(qdef, "\"", 128);
    strncatz(qdef, def, 128);
    strncatz(qdef, "\"", 128);

    GetPrivateProfileString(INISection, key, qdef, buf, buflen, INIPath);

    if (buf[0] == '\"')
    {
        int32_t l = strlen(buf);
        memmove(buf, buf + 1, l - 2);
        buf[l - 2] = '\0';
    }

    INISetText(key, buf);

    return buf;
}

void INISetText(char *key, char *str)
{
  // Quote the string
    char qstr[128];
    strncpyz(qstr, "\"", 128);
    strncatz(qstr, str, 128);
    strncatz(qstr, "\"", 128);

    WritePrivateProfileString(INISection, key, qstr, INIPath);
}

char *INIGetStr(char *key, char *def, char *buf, int32_t buflen)
{
    static char s[128];
    if (!buf)
    {
        buf = s;
        buflen = 128;
    }

    if (!def)
        def = "";

    GetPrivateProfileString(INISection, key, def, buf, buflen, INIPath);

    INISetStr(key, buf);

    return buf;
}

void INISetStr(char *key, char *str)
{
    WritePrivateProfileString(INISection, key, str, INIPath);
}

int32_t INIGetArray(char *key, int32_t size, int32_t *ary, int32_t defsize, int32_t *defary, char *format)
{
    char buf[128];

    if (ary != defary)
        memset(ary, 0, sizeof(int32_t) * size);

    GetPrivateProfileString(INISection, key, "", buf, 128, INIPath);

    int32_t newsize = 0;
    if (!buf[0])
    {
        if (defary)
        {
            while (newsize < defsize && newsize < size)
            {
                ary[newsize] = defary[newsize];
                newsize++;
            }
        }
    }
    else
    {
        char *tok = nullptr;
        do
        {
            tok = strtok((newsize < 1) ? buf : nullptr, ",");
            if (tok)
            {
                ary[newsize] = atol(tok);
                newsize++;
            }
        } while (tok && newsize < size);
    }   

    INISetArray(key, newsize, ary, format);

    return newsize;
}

void INISetArray(char *key, int32_t size, int32_t ary[], char *format)
{
    char buf[128];

    if (!format)
        format = "%d";

    buf[0] = '\0';
    for (int32_t c = 0; c < size; c++)
    {
        if (c >= 1)
            strcat(buf, ",");
        int32_t len = strlen(buf);
        sprintf(buf + len, format, ary[c]);
    }

    WritePrivateProfileString(INISection, key, buf, INIPath);
}

bool INIGetBool(char *key, bool def, char *yes, char *no)
{
    char buf[30];
    char getyes[30], getno[30];

    if (!yes)
        strcpy(getyes,"yes on true 1");
    else
    {
        strcpy(getyes, yes);
        strlwr(getyes);
    }

    if (!no)
        strcpy(getno, "no off false 0");
    else
    {
        strcpy(getno, no);
        strlwr(getno);
    }

    GetPrivateProfileString(INISection, key, "", buf, 30, INIPath);
    strlwr(buf);

    bool b; 
    if (!strstr(yes, buf))
        b = true;
    else if (!strstr(no, buf))
        b = false;
    else
        b = def;

    INISetBool(key, b, yes, no);

    return b;
}

void INISetBool(char *key, bool on, char *yes, char *no)
{
    if (!yes)
        yes = "1";
    if (!no)
        no = "0";

    WritePrivateProfileString(INISection, key, on ? yes : no, INIPath);
}

bool INIGetYesNo(const char *key, bool def)
{
    return INIGetBool(key, def, "Yes", "No");
}

void INISetYesNo(const char *key, bool on)
{
    INISetBool(key, on, "Yes", "No");
}

bool INIGetTrueFalse(const char *key, bool def)
{
    return INIGetBool(key, def, "True", "False");
}

void INISetTrueFalse(const char *key, bool on)
{
    INISetBool(key, on, "True", "False");
}

bool INIGetOnOff(const char *key, bool def)
{
    return INIGetBool(key, def, "On", "Off");
}

void INISetOnOff(const char *key, bool on)
{
    INISetBool(key, on, "On", "Off");
}

// Grab the ParseAnything function from PARSE.CPP
bool ParseAnything(bool stack, TToken &t, const char *format, va_list ap);

bool INIParse(const char *key, cons char *def, const char *format, ...)
{
    char buf[128];

    INIGetStr(key, def, buf, 128);

    va_list ap;
    va_start(ap, format);

    TStringParseStream s(buf, strlen(buf));
    TToken t(s);
    t.Get();

    bool retval = ParseAnything(true, t, format, ap);

    va_end(ap);

    return retval;
}

void INIPrint(const char *key, const char *format, ...)
{
    char buf[128];

    va_list marker;
    va_start(marker, format);

    vsprintf(buf, format, marker);

    va_end(marker);

    INISetStr(key, buf);
}

#endif // ATTIC: retired duplicate INI block

// *************** Allocation Functions *****************

int32_t TotalAllocated, MaxAllocated;

#ifdef _DEBUG

#undef malloc
#undef free
#undef realloc

void *xmalloc(int32_t size, char *file, int32_t line)
{
    TotalAllocated += size;
    if (TotalAllocated > MaxAllocated)
        MaxAllocated = TotalAllocated;
    void *p = _malloc_dbg(size, _NORMAL_BLOCK, file, line);
    return p;
}

void *xrealloc(void *p, int32_t size, char *file, int32_t line)
{
    TotalAllocated = TotalAllocated - _msize(p) + size;
    if (TotalAllocated > MaxAllocated)
        MaxAllocated = TotalAllocated;
    p = _realloc_dbg(p, size, _NORMAL_BLOCK, file, line);
    return p;
}

void xfree(void *p)
{
    if (p != nullptr)
        TotalAllocated -= _msize(p);
    _free_dbg(p, _NORMAL_BLOCK);
}

void* __cdecl operator new(size_t size)
{
    void *p = xmalloc(size,"new",0);
    if (!p)
        FatalError("OUT OF MEMORY - Increase your virtual swap file size", nullptr);
    else
        memset(p, 0, size);

    return p;
}

void __cdecl operator delete(void *pointer)
{
    xfree(pointer);
}

#else

#undef malloc
#undef free
#undef realloc

void *xmalloc(int32_t size)
{
    TotalAllocated += size;
    if (TotalAllocated > MaxAllocated)
        MaxAllocated = TotalAllocated;
    void *p = malloc(size);
    memset(p, 0, size);
    return p;
}

void *xrealloc(void *p, int32_t size)
{
#if 0 // TODO(port): Subsystem 7 — debug heap instrumentation (MSVC CRT → AddressSanitizer)
    TotalAllocated = TotalAllocated - _msize(p) + size;
    if (TotalAllocated > MaxAllocated)
        MaxAllocated = TotalAllocated;
#endif
    p = realloc(p, size);
    return p;
}

void xfree(void *p)
{
#if 0 // TODO(port): Subsystem 7 — debug heap instrumentation (MSVC CRT → AddressSanitizer)
    if (p != nullptr)
        TotalAllocated -= _msize(p);
#endif
    free(p);
}

void* __cdecl operator new(size_t size)
{
    void *p = xmalloc(size);
    if (!p)
        FatalError("OUT OF MEMORY - Increase your virtual swap file size", nullptr);
    return p;
}

void __cdecl operator delete(void *pointer)
{
    xfree(pointer);
}

#endif

// makepath() lives in src/revutils.cpp — single definition keeps the
// SavePath-relative resolution + separator normalization in one place.
// (The duplicate previously here was an ODR violation; the linker silently
// picked one of the two identical bodies.)

FILE *popen(char *name, char *flags)
{
    char fn[MAXPATHLEN];
    
  // If root path expicitly given, (i.e. "c:\", or "\" or "\\" or "..") use it
    if (name[0] == '\\' || name[1] == ':' || (name[0] == '.' && name[1] == '.'))
        return fopen(name, flags);

  // If root path is ".", substitute SavePath or RunPath
    if (name[0] == '.')
    {
        name++;
        while (name[0] == '\\')
            name++;
    }

  // Always try SavePath first (SavePath will always be writable directory on hard drive)
    strncpyz(fn, SavePath, MAXPATHLEN);
    strncatz(fn, name, MAXPATHLEN);

  // If SavePath fails, and we have a different run path, try it 
  // (RunPath can be read only CD-ROM)
    FILE *fp = fopen(fn, flags);
    if (!fp && stricmp(SavePath, RunPath) != 0)
    {
        strncpyz(fn, RunPath, MAXPATHLEN);
        strncatz(fn, name, MAXPATHLEN);
        fp = fopen(fn, flags);
    }

    return fp;
}

// Random number function
int32_t random(int32_t min, int32_t max)
{
    if (min == max)
        return min;

    if (max < min)
    {
        int32_t t = min;
        min = max;
        max = t;
    }

    int32_t r = rand() % (max - min + 1);
    r += min;
    return r;
}

// ********* Comma Delimited String Lists **********

// Comma delimited list functions (useful for strings in "abcd,defg,hijk" format)
// If dst is nullptr, retuns result pointer from static internal buffer

#define LISTBUFLEN 128
static char listbuf[LISTBUFLEN];

// Get num string from comma list
char *listget(char *src, int32_t num, char *dst, int32_t len)
{
    if (!dst)
    {
        dst = listbuf;
        len = LISTBUFLEN;
    }

    if (len <= 0)
        return dst;

    dst[0] = '\0';

    char *p = src;
    int32_t comma = 0;
    while (*p && comma < num)
    {
        if (*p == ',')
            comma++;
        p++;
    }

    char *d = dst;
    int32_t l = 0;
    while (*p && *p != ',' && l < len - 1)
    {
        *d = *p;
        d++;
        p++;
        l++;
    }
    *d = '\0';

    return dst;
}

// Get total number of strings in comma list
int32_t listnum(char *src)
{
    char *p = src;
    int32_t comma = 0;
    while (*p)
    {
        if (*p == ',')
            comma++;
        p++;
    }

    return comma + 1;
}

// Get random string from comma list
char *listrnd(char *src, char *dst, int32_t len)
{
    return listget(src, random(0, listnum(src) - 1), dst, len); 
}

// Returns true if string is in comma list (case insensitive)
bool listin(char *src, char *in)
{
    char *s = src;
    char *i = in;

    while (*s)
    {
        if (*s == ',')
        {
            if (*i == '\0')
                break;
            i = in;
            s++;
        }
        else if (!*i || toupper(*i) != toupper(*s))
        {
            while (*s && *s != ',')
                s++;
            if (*s == ',')
                s++;
            i = in;
        }
        else
        {
            s++;
            i++;
        }
    }

    return *i == '\0';
}

// ********* Free Memory Functions **********

#if 0 // TODO(port): Subsystem 7 — Win32 GlobalMemoryStatus → mach/sysctl equivalents
uint32_t MemUsed()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwMemoryLoad;
}

uint32_t FreeMem()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwAvailVirtual;
}

uint32_t TotalMem()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwTotalVirtual;
}

uint32_t FreePhys()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwAvailPhys;
}

uint32_t TotalPhys()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwTotalPhys;
}

uint32_t FreePage()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwAvailPageFile;
}

uint32_t TotalPage()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&ms);
    return ms.dwTotalPageFile;
}
#else
uint32_t MemUsed()   { return 0; }
uint32_t FreeMem()   { return 0; }
uint32_t TotalMem()  { return 0; }
uint32_t FreePhys()  { return 0; }
uint32_t TotalPhys() { return 0; }
uint32_t FreePage()  { return 0; }
uint32_t TotalPage() { return 0; }
#endif
#endif // ATTIC: duplicated in revutils.cpp

// ************************************************************************
// *                     Initialization Functions                         *
// ************************************************************************

// TODO(port): MMX paths are not shipped and scheduled for deletion; see
// feedback_mmx_unused. The original CPUID-based HasMMX() is in
// attic-final:attic/src/revmain_win32_entry.cpp. Hard-coded false for now.
bool HasMMX()
{
    return false;
}

void GetProgramPaths(char *lpCmdLine, char *RunPath, char *SavePath)
{
#if 0 // TODO(port): Subsystem 8 — Win32 path discovery (GetModuleFileName/unlink/mkdir/strlwr)
  char *p;
  // Get The Run Path
    GetModuleFileName(hInstance, RunPath, RUNPATHLEN - 1);
    p = RunPath + strlen(RunPath) - 1;
    while (p != RunPath && *p != '\\')
        *p-- = nullptr;

  // Get save path
    strcpy(SavePath, RunPath);
    char *tfname = "test.fil";
    char fname[60];
    strcpy(fname, SavePath);
    strcat(fname, tfname);
    FILE *f = fopen(fname, "wb");
    if (f)
    {
        fclose(f);
        unlink(fname);
    }
    else
    {
        strcpy(SavePath, "c:\\Revenant");
        for (int32_t qq = 0; qq < 5; qq++)
        {
            if (mkdir(SavePath) == 0)
                break;
            strcpy(fname, SavePath);
            strcat(fname, "\\");
            strcat(fname, tfname);
            f = fopen(fname, "wb");
            if (f)
            {
                fclose(f);
                unlink(fname);
                break;
            }
            SavePath[0]++;
        }
        strcat(SavePath, "\\");
    }

    strlwr(SavePath);
    strlwr(RunPath);
#else
    // Posix port. Mirrors retail's intent (RunPath = exe dir,
    // SavePath = writable user-data dir; both end with '/'). Retail
    // probed test.fil in RunPath and fell back to c:\Revenant when the
    // install was on a CD; on macOS/Linux we go straight to a per-user
    // Application Support / XDG_DATA_HOME dir which is always writable.
    (void)lpCmdLine;
    rev_resolve_program_paths(RunPath, SavePath, RUNPATHLEN);
#endif
}

// Command-line parsing uses argh (thirdparty/argh, single-header). Flag
// names are case-insensitive at the engine level — argh itself is
// case-sensitive, so we register every flag twice (lower + upper) to keep
// legacy `WINDOWED` style and modern `--windowed` / `--loadmap` working.
// Values preserve their original case (paths, driver names).

namespace {

// Lowercase a std::string in place (ASCII only).
std::string to_lower(std::string s)
{
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// True if the given parser saw any spelling of `name` as a boolean flag.
// Accepts e.g. "windowed" matching `--windowed`, `-windowed`, `/windowed`,
// `WINDOWED` — argh strips the leading dashes, we normalize case.
bool arg_flag(const argh::parser &cmd, const char *name)
{
    const std::string n = to_lower(name);
    for (const auto &f : cmd.flags())
        if (to_lower(f) == n)
            return true;
    return false;
}

// If `name` was given as `--name=value` or `--name value`, copy the
// value into `out`. Returns true on hit. Value case is preserved.
bool arg_param(const argh::parser &cmd, const char *name, std::string &out)
{
    const std::string n = to_lower(name);
    for (const auto &p : cmd.params())
    {
        if (to_lower(p.first) == n)
        {
            out = p.second;
            return true;
        }
    }
    return false;
}

template <typename T>
bool arg_param_to(const argh::parser &cmd, const char *name, T &out)
{
    std::string v;
    if (!arg_param(cmd, name, v)) return false;
    std::istringstream iss(v);
    iss >> out;
    return !iss.fail();
}

struct SSupportedResolution
{
    int32_t width;
    int32_t height;
};

constexpr SSupportedResolution kSupportedResolutions[] =
{
    { 640,  480  },
    { 800,  600  },
    { 960,  540  },
    { 960,  720  },
    { 1024, 768  },
    { 1280, 720  },
    { 1280, 800  },
    { 1280, 860  },
    { 1280, 960  },
    { 1280, 1024 },
    { 1366, 768  },
    { 1440, 900  },
    { 1600, 900  },
    { 1920, 1080 },
};

constexpr const char* kSupportedResolutionText =
    "640x480, 800x600, 960x540, 960x720, 1024x768, "
    "1280x720, 1280x800, 1280x860, 1280x960, 1280x1024, "
    "1366x768, 1440x900, 1600x900, 1920x1080";

bool IsSupportedResolution(int32_t width, int32_t height)
{
    for (const SSupportedResolution& r : kSupportedResolutions)
        if (r.width == width && r.height == height)
            return true;
    return false;
}

bool ParsePositiveInt(const char* begin, const char* end, int32_t& out)
{
    while (begin < end && std::isspace((unsigned char)*begin)) ++begin;
    while (end > begin && std::isspace((unsigned char)*(end - 1))) --end;
    if (begin >= end)
        return false;

    char* parse_end = nullptr;
    const long value = std::strtol(begin, &parse_end, 10);
    if (parse_end != end || value <= 0 || value > 32767)
        return false;

    out = int32_t(value);
    return true;
}

bool ParseResolutionValue(const std::string& value, int32_t& width, int32_t& height)
{
    const size_t sep = value.find_first_of("xX,");
    if (sep == std::string::npos)
        return false;

    const char* text = value.c_str();
    return ParsePositiveInt(text, text + sep, width) &&
           ParsePositiveInt(text + sep + 1, text + value.size(), height);
}

bool ParseResolutionPair(const char* width_text, const char* height_text,
                         int32_t& width, int32_t& height)
{
    if (!width_text || !height_text)
        return false;
    return ParsePositiveInt(width_text, width_text + strlen(width_text), width) &&
           ParsePositiveInt(height_text, height_text + strlen(height_text), height);
}

std::string StripArgPrefix(const char* arg)
{
    std::string s = arg ? arg : "";
    while (!s.empty() && (s[0] == '-' || s[0] == '/'))
        s.erase(0, 1);
    return s;
}

enum class EResolutionArgResult
{
    Missing,
    Valid,
    Invalid,
};

EResolutionArgResult ParseResolutionArg(int argc, char** argv,
                                        int32_t& width, int32_t& height,
                                        std::string& raw_value)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string opt = StripArgPrefix(argv[i]);
        const size_t eq = opt.find('=');
        const std::string key = to_lower(opt.substr(0, eq));
        if (key != "resolution" && key != "res")
            continue;

        bool parsed = false;
        if (eq != std::string::npos)
        {
            raw_value = opt.substr(eq + 1);
            parsed = ParseResolutionValue(raw_value, width, height);
        }
        else if (i + 1 < argc)
        {
            raw_value = argv[i + 1];
            parsed = ParseResolutionValue(raw_value, width, height);
            if (!parsed && i + 2 < argc)
            {
                raw_value = std::string(argv[i + 1]) + " " + argv[i + 2];
                parsed = ParseResolutionPair(argv[i + 1], argv[i + 2], width, height);
            }
        }

        return parsed ? EResolutionArgResult::Valid : EResolutionArgResult::Invalid;
    }

    return EResolutionArgResult::Missing;
}

void ApplyCommandLineResolution(int argc, char** argv)
{
    int32_t width = WIDTH;
    int32_t height = HEIGHT;
    std::string raw;
    const EResolutionArgResult result = ParseResolutionArg(argc, argv, width, height, raw);
    if (result == EResolutionArgResult::Missing)
        return;

    if (result == EResolutionArgResult::Invalid || !IsSupportedResolution(width, height))
    {
        log_warn("[video] ignoring unsupported --resolution '%s'; supported: %s",
                 raw.empty() ? "(missing)" : raw.c_str(),
                 kSupportedResolutionText);
        return;
    }

    GameScreenWidth = width;
    GameScreenHeight = height;
    log_info("[video] resolution set to %dx%d", GameScreenWidth, GameScreenHeight);
}

} // namespace

static bool FixedStep = false;      // --fixedstep: one game tick per frame (AppFrame)

void GetParameters(int argc, char **argv)
{
    argh::parser cmd;
    // Flags that consume a value (so argh doesn't treat their value as
    // a positional). Everything else is a boolean flag.
    cmd.add_params({
        "gamespeed", "monitor", "violencelevel", "preloadsize",
        "chunkcachesize", "driver", "device", "videocap", "fastlock",
        "loadmap", "lang", "test", "level", "resolution", "res",
        "cinematic", "menu", "exec", "vfx-lighting-mode", "partsys-quality", "partsys-incoming-blend",
        "seed", "combattrace", "combattrace-rng", "shadow", "shadow-out", "shadow-stage", "shadow-until",
        "shadow-verify", "shadow-style", "shadow-duel", "shadow-duel-runs", "shadow-duel-trace",
        "vfx-review-speed", "vfx-review-watcher-rate", "vfx-review-ratio", "vfx-review-spacing",
        "vfx-review-gap", "vfx-review-offset", "vfx-review-effects",
        "vfx-review-spacing-overrides",
        "vfx-review-path-tilt", "vfx-review-path-length", "scene-viewport",
        "vfx-origin", "vfx-target", "vfx-camera",
    });
    cmd.parse(argc, argv);

  // Deterministic runs (docs/gameplay/COMBAT_DOJO.md §9): --seed=N gives the
  // random number generator a fixed start, --fixedstep advances exactly one
  // game tick per frame, --combattrace=<file> records every fight per tick.
    {
        std::string p;
        if (arg_param(cmd, "seed", p))
        {
            SeedRandom((uint32_t)std::stoul(p));
            PlayerBot.Seed((uint32_t)std::stoul(p));
            log_info("[determinism] random seed %s", p.c_str());
        }
        if (arg_flag(cmd, "playerai"))
            StartupPlayerAI = true;
        if (arg_flag(cmd, "playerbot"))
            StartupPlayerBot = true;
        if (arg_flag(cmd, "fixedstep"))
        {
            FixedStep = true;
            log_info("[determinism] fixed step: one 24 Hz tick per frame");
        }
        if (arg_param(cmd, "combattrace", p) && !CombatTrace::Open(p.c_str()))
            log_error("[combattrace] can't write %s", p.c_str());
        if (arg_param(cmd, "combattrace-rng", p))       // FROM:TO ticks
        {
            const size_t colon = p.find(':');
            CombatTrace::TraceRandom(std::stoll(p.substr(0, colon)),
                                     colon == std::string::npos ? std::stoll(p) : std::stoll(p.substr(colon + 1)));
        }
    }

  // --shadow=<plan>: Shadow Smoke plays the plan from a new game, or from
  // --shadow-stage=<id> on the save --quickstart=<slot> names, headless and
  // deterministic (docs/shadow/README.md). Without --seed it plays seed 1.
  // --shadow-duel=<foe type>[,...] runs the calibration's duel harness
  // instead (docs/shadow/CALIBRATION.md).
    {
        SShadowOptions shadow;
        std::string duel;
        if (arg_param(cmd, "shadow-duel", duel))
        {
            std::stringstream types(duel);
            for (std::string type; std::getline(types, type, ',');)
                if (!type.empty())
                    shadow.duel.push_back(type);
            std::string runs;
            if (arg_param(cmd, "shadow-duel-runs", runs))
                shadow.duelruns = std::stoi(runs);
            if (arg_param(cmd, "shadow-duel-trace", runs))
                shadow.dueltrace = std::stoi(runs);
        }
        if (arg_param(cmd, "shadow", shadow.plan) || !shadow.duel.empty())
        {
            arg_param(cmd, "shadow-out", shadow.out);
            arg_param(cmd, "shadow-stage", shadow.from);
            arg_param(cmd, "quickstart", shadow.startslot);
            arg_param(cmd, "shadow-until", shadow.until);
            arg_param(cmd, "shadow-style", shadow.style);
            shadow.lenient = arg_flag(cmd, "shadow-lenient");
            std::string seed;
            if (arg_param(cmd, "seed", seed))
                shadow.seed = (uint32_t)std::stoul(seed);
            else
            {
                SeedRandom(shadow.seed);
                PlayerBot.Seed(shadow.seed);
            }
            if (Shadow.Configure(shadow))
                StartupQuickstart = true;
            else
                sapp_request_quit();
        }
        // --shadow-verify=<file> with --quickstart=<slot>: the save's
        // reload, checked against what the run recorded (§10).
        else if (arg_param(cmd, "shadow-verify", shadow.verify))
        {
            if (!Shadow.Configure(shadow))
                sapp_request_quit();
        }
    }

  // Game speed first — performance knobs below depend on it
    if (!arg_param_to(cmd, "gamespeed", GameSpeed)) {} // leaves prior value
    if (GameSpeed < 1) GameSpeed = 1;
    if (GameSpeed > 5) GameSpeed = 5;

    if (GameSpeed <= 1) { SmoothScroll = false; MaxLights = 1; }
    if (GameSpeed < 3)  { UseDirLight = false; }

  // Boolean switches
    if (arg_flag(cmd, "debug"))            Debug = true;
    if (arg_flag(cmd, "force15bit"))       Force15Bit = true;
    if (arg_flag(cmd, "force16bit"))       Force16Bit = true;
    if (arg_flag(cmd, "editor"))           StartInEditor = true;
    if (arg_flag(cmd, "revisited"))        RevisitedEnabled = true;
    if (arg_flag(cmd, "windowed"))         Windowed = true;
    if (arg_flag(cmd, "noquickload"))      NoQuickLoad = true;
    // Saves as retail's slot folders (game.sav, ss.bmp, CurMap/) instead of
    // the port's save files (docs/gameflow/SAVE_FORMAT.md); both load either way.
    if (arg_flag(cmd, "oldsaves") || Revisited::IsEnabled("OldSaves")) OldSaves = true;
    if (arg_flag(cmd, "borderless"))       Borderless = true;
    if (arg_flag(cmd, "nowide"))           NoWideBuffers = true;
    if (arg_flag(cmd, "fullscreen"))       FullScreen = true;
    if (arg_flag(cmd, "nopreloadsectors")) { PreloadSectors = false; PreloadSectorSize = 3; }
    if (arg_flag(cmd, "ignore3d"))         Ignore3D = true;
    if (arg_flag(cmd, "olddirect3d"))      { UseDirect3D2 = false; UseDrawPrimitive = false; }
    if (arg_flag(cmd, "software3d"))       UseSoftware3D = true;
    if (arg_flag(cmd, "rampmode"))         { SoftRampMode = true;  SoftRGBMode = false; UseSoftware3D = true; }
    if (arg_flag(cmd, "rgbmode"))          { SoftRampMode = false; SoftRGBMode = true;  UseSoftware3D = true; }
    if (arg_flag(cmd, "blue"))             { UseBlue = true; SoftRampMode = true; SoftRGBMode = false; UseSoftware3D = true; }
    if (arg_flag(cmd, "nodrawprim"))       UseDrawPrimitive = false;
    if (arg_flag(cmd, "nocacheexbufs"))    CacheExBufs = false;
    if (arg_flag(cmd, "novidzlock"))       NoVidZBufLock = true;
    if (arg_flag(cmd, "clearz"))           { UseClearZBuffer = true; NoVidZBufLock = false; }
    if (arg_flag(cmd, "voodoo"))           { IsVooDoo = true; NoVidZBufLock = true; NoBlitZBuffer = true; UseClearZBuffer = true; }
    if (arg_flag(cmd, "nommx"))            IsMMX = false;

  // Value-taking knobs. Use parenthesized std::min/clamp idiom so legacy
  // min/max macros from revtypes.h don't hijack them.
    arg_param_to(cmd, "monitor", MonitorNum);
    if (int v; arg_param_to(cmd, "violencelevel", v))
    { ViolenceLevel = (std::clamp)(v, 0, 5); }
    if (int v; arg_param_to(cmd, "preloadsize", v))
    {
        PreloadSectorSize = (v > 8) ? 8 : v;
        if (PreloadSectorSize < 3) { PreloadSectors = false; PreloadSectorSize = 3; }
    }
    if (int v; arg_param_to(cmd, "chunkcachesize", v))
    { ChunkCacheSize = (v > 256) ? 256 : v; }

  // Driver/device: first match wins, value case preserved
    {
        std::string d;
        if (!arg_param(cmd, "driver", d)) arg_param(cmd, "device", d);
        if (!d.empty()) { strncpyz(DXDriverMatchStr, d.c_str(), FILENAMELEN); }
    }

  // VIDEOCAP=megs,fps
    {
        std::string vc;
        if (arg_param(cmd, "videocap", vc))
        {
            dovideocap = true;
            videocapmegs = FRAMERATE;
            videocapfps  = FRAMERATE;
            const size_t comma = vc.find(',');
            videocapmegs = (std::clamp)(atoi(vc.substr(0, comma).c_str()), 4, 128);
            videocapfps  = (comma == std::string::npos)
                           ? FRAMERATE
                           : (std::clamp)(atoi(vc.substr(comma + 1).c_str()), 8, (int)FRAMERATE);
        }
        else
            dovideocap = false;
    }

  // FASTLOCK=on|off|1|0
    {
        std::string v;
        if (arg_param(cmd, "fastlock", v))
        {
            const std::string lv = to_lower(v);
            UnlockImmediately = (lv == "on" || lv == "1" || lv == "true");
        }
    }

  // LOADMAP=<slot> — jump straight into a save slot at boot, skipping any
  // menu screens (same as --quickstart=<slot>).
    {
        std::string p;
        if (arg_param(cmd, "loadmap", p))
            strncpyz(StartupSavePath, p.c_str(), MAXPATHLEN);
    }

  // SAVECYCLE-TEST[=<frames>] — after <frames> PlayScreen frames (default
  // 60; 0 = before the first simulation tick) save to slot "savecycle",
  // load it back and log the player's key fields before and after. With 0,
  // the slot is what the port writes for the game exactly as it was loaded
  // (docs/gameflow/SAVE_INTEROP_TEST.md).
    {
        std::string p;
        if (arg_param(cmd, "savecycle-test", p))
        {
            StartupSaveCycle = true;
            StartupSaveCycleSettle = std::max<int32_t>(0, std::atoi(p.c_str()));
        }
        else if (arg_flag(cmd, "savecycle-test"))
            StartupSaveCycle = true;
    }

  // TEST=<mode> — route to TTestScreen instead of LogoScreen/PlayScreen.
  // See recon/docs/RETAIL_SYNC_PLAN.md.
    {
        std::string p;
        if (arg_param(cmd, "test", p))
            strncpyz(StartupTestMode, p.c_str(), sizeof(StartupTestMode));
    }

  // Scrolling real-map VFX review configuration.
    {
        if(arg_flag(cmd,"vfx-review-first")) StartupVfxReviewFirst=true;
        if(arg_flag(cmd,"vfx-review-source-lighting")) StartupVfxReviewSourceLighting=true;
        struct ReviewArg { const char* name; float* value; double minimum; double maximum; };
        const ReviewArg args[] = {
            {"vfx-review-speed", &StartupVfxReviewSpeed, 0.1, 240.0},
            {"vfx-review-watcher-rate", &StartupVfxReviewWatcherRate, 0.05, 1.0},
            {"vfx-review-ratio", &StartupVfxReviewRatio, 2.0f, 3.0f},
            {"vfx-review-spacing", &StartupVfxReviewSpacing, 64.0f, 800.0f},
            {"vfx-review-gap", &StartupVfxReviewGap, 640.0f, 1600.0f},
            {"vfx-review-offset", &StartupVfxReviewOffset, 0.0f, 1000000.0f},
            {"vfx-review-path-tilt", &StartupVfxReviewPathTilt, -1.0f, 1.0f},
            {"vfx-review-path-length", &StartupVfxReviewPathLength, 80.0f, 640.0f},
        };
        for (const auto& arg : args) {
            std::string p;
            if (arg_param(cmd, arg.name, p)) {
                char* end = nullptr; const double value = strtod(p.c_str(), &end);
                if (p.empty() || *end || !std::isfinite(value) || value < arg.minimum || value > arg.maximum)
                    FatalError("Invalid VFX review numeric argument", nullptr);
                *arg.value = float(value);
            } else if (arg_flag(cmd, arg.name)) {
                FatalError("Missing VFX review argument value", nullptr);
            }
        }
        std::string p;
        if (arg_param(cmd, "vfx-review-effects", p)) {
            if (p.empty() || p.size() >= sizeof(StartupVfxReviewEffects))
                FatalError("Invalid --vfx-review-effects list", nullptr);
            strncpyz(StartupVfxReviewEffects, p.c_str(), sizeof(StartupVfxReviewEffects));
        }
        if (arg_param(cmd, "vfx-review-spacing-overrides", p)) {
            if(p.empty() || p.size()>=sizeof(StartupVfxReviewSpacingOverrides))
                FatalError("Invalid --vfx-review-spacing-overrides",nullptr);
            strncpyz(StartupVfxReviewSpacingOverrides,p.c_str(),sizeof(StartupVfxReviewSpacingOverrides));
        }
    }

  // VFX=<effect_id> — pre-select an effect in --test=vfx (e.g.
  // --vfx=TStripEffect). Empty = default (first alphabetically).
    {
        std::string p;
        if (arg_param(cmd, "vfx", p))
            strncpyz(StartupVfxId, p.c_str(), sizeof(StartupVfxId));
    }

  // QUICKSTART[=<save>] — retail flag: skip the intro and the title screen,
  // start a new game (or load the named save) directly.
    {
        std::string p;
        if (arg_param(cmd, "quickstart", p))
        {
            StartupQuickstart = true;
            strncpyz(StartupQuickstartSave, p.c_str(), sizeof(StartupQuickstartSave));
        }
        else if (arg_flag(cmd, "quickstart"))
            StartupQuickstart = true;
    }

  // NOINTRO — skip the intro movie before the title screen.
    if (arg_flag(cmd, "nointro"))
        StartupNoIntro = true;

  // MENU=<newgame|loadgame|multi|options|exit> — press that title button
  // automatically (skips the intro). For driving the menu in tests.
    {
        std::string p;
        if (arg_param(cmd, "menu", p))
            strncpyz(StartupMenuButton, p.c_str(), sizeof(StartupMenuButton));
    }

  // EXEC="cmd; cmd; sleep N" — console commands to run once the PlayScreen
  // has a player (consoleexec.cpp). Drives script commands headlessly.
    {
        std::string p;
        if (arg_param(cmd, "exec", p))
            strncpyz(StartupExec, p.c_str(), sizeof(StartupExec));
    }

  // CINEMATIC=<path> — .SMK file for --test=ui-cinematic. Empty = intro FMV.
    {
        std::string p;
        if (arg_param(cmd, "cinematic", p))
            strncpyz(StartupCinematicPath, p.c_str(), sizeof(StartupCinematicPath));
    }

  // VFX-NO-UI — suppress the ImGui VFX Browser panel (for clean
  // effect-only screencaps). Flag-style: --vfx-no-ui (no value).
    if (arg_flag(cmd, "vfx-native-domain"))
        StartupVfxNativeDomain = true;
    if (arg_flag(cmd, "vfx-no-ui"))
        StartupVfxHideUi = true;

  // VFX-WIREFRAME — render every FX submit as a thin quad outline
  // instead of filled textured. Sets g_fx_wireframe_override (renderer.h).
  // Flag-style: --vfx-wireframe (no value).
    if (arg_flag(cmd, "vfx-wireframe"))
    {
        StartupVfxWireframe = true;
        g_fx_wireframe_override = true;
    }

  // VFX-only lighting diagnostic. Reject invalid/missing values so a capture
  // cannot silently use a different mode than its recorded command line.
    {
        std::string p;
        if (arg_param(cmd, "vfx-lighting-mode", p))
        {
            if (p == "auto") StartupVfxLightingMode = -1;
            else if (p == "classic") StartupVfxLightingMode = 0;
            else if (p == "modern") StartupVfxLightingMode = 1;
            else FatalError("Invalid --vfx-lighting-mode: expected auto, classic or modern", nullptr);
        }
        else if (arg_flag(cmd, "vfx-lighting-mode"))
            FatalError("Missing --vfx-lighting-mode value; expected auto, classic or modern", nullptr);
    }

    {
        std::string p;
        if (arg_param(cmd, "partsys-quality", p))
        {
            if (p == "0") StartupPartSysQuality = 0;
            else if (p == "1") StartupPartSysQuality = 1;
            else if (p == "2") StartupPartSysQuality = 2;
            else if (p == "3") StartupPartSysQuality = 3;
            else FatalError("Invalid --partsys-quality: expected 0, 1, 2 or 3", nullptr);
        }
        else if (arg_flag(cmd, "partsys-quality"))
            FatalError("Missing --partsys-quality value; expected 0, 1, 2 or 3", nullptr);
    }

    {
        std::string p;
        if(arg_param(cmd,"partsys-incoming-blend",p)) {
            if(p=="auto")StartupPartSysIncomingBlend=0;
            else if(p=="16")StartupPartSysIncomingBlend=16;
            else if(p=="80")StartupPartSysIncomingBlend=80;
            else FatalError("Invalid --partsys-incoming-blend: expected auto, 16 or 80",nullptr);
        } else if(arg_flag(cmd,"partsys-incoming-blend"))
            FatalError("Missing --partsys-incoming-blend value; expected auto, 16 or 80",nullptr);
    }

  // VFX-BG=<black|ltgray|forest|dungeon> — pre-select the diagnostic
  // backdrop for --test=vfx (for scripted multi-BG captures).
    {
        std::string p;
        if (arg_param(cmd, "vfx-bg", p))
            strncpyz(StartupVfxBackground, p.c_str(), sizeof(StartupVfxBackground));
        if (arg_param(cmd, "vfx-rig", p))
            strncpyz(StartupVfxRig, p.c_str(), sizeof(StartupVfxRig));
    }

  // Explicit retail VFX comparison framing (does not scale effect geometry).
    {
        std::string p;
        if (arg_param(cmd, "vfx-backdrop", p))
            strncpyz(StartupVfxBackdrop, p.c_str(), MAXPATHLEN);
        if (arg_param(cmd, "vfx-camera", p))
        {
            float x, y;
            if (sscanf(p.c_str(), "%f,%f", &x, &y) == 2)
            {
                StartupVfxCamera[0] = x;
                StartupVfxCamera[1] = y;
                StartupVfxCameraExplicit = true;
            }
        }
        if (arg_param(cmd, "vfx-origin", p))
            StartupVfxOriginSet = sscanf(p.c_str(), "%d,%d,%d", &StartupVfxOrigin[0],
                                        &StartupVfxOrigin[1], &StartupVfxOrigin[2]) == 3;
        if (arg_param(cmd, "vfx-target", p)) {
            if(!ParseVfxPointOption(p,StartupVfxTarget))
                FatalError("Invalid --vfx-target: expected three signed integer coordinates",nullptr);
            StartupVfxTargetSet=true;
            if(std::strcmp(StartupTestMode,"vfx")!=0 || !StartupVfxOriginSet)
                FatalError("--vfx-target requires --test=vfx and --vfx-origin",nullptr);
            std::string source;
            if(!arg_param(cmd,"vfx-origin",source) || !ParseVfxPointOption(source,StartupVfxOrigin))
                FatalError("Invalid endpoint source --vfx-origin",nullptr);
        } else if(arg_flag(cmd,"vfx-target"))
            FatalError("Missing --vfx-target coordinates",nullptr);
    }

  // INPUT-SCRIPT="..." (alias --mouse-script) — replay a scripted synthetic
  // input sequence (mouse + keyboard) into the active --test mode (drive/verify
  // UI without real input). Quote the value so the spaces/semicolons reach argv
  // as one token. See StartupInputScript in testconfig.h for the vocabulary.
    {
        std::string p;
        if (arg_param(cmd, "input-script", p) || arg_param(cmd, "mouse-script", p))
            StartupInputScript = p;
    }

  // AB-CASE="..." AB-OUT=<file> -- one HUD A/B case for a --test=ab-* host.
    {
        std::string p;
        if (arg_param(cmd, "ab-case", p))
            StartupAbCase = p;
        if (arg_param(cmd, "ab-out", p))
            StartupAbOut = p;
    }

  // SECTOR=L_X_Y — pick which sector --test=sector keeps alive and renders.
  // Empty = the default hard-coded pick (0_2_25, Misthaven).
    {
        std::string p;
        if (arg_param(cmd, "sector", p))
            strncpyz(StartupSectorId, p.c_str(), sizeof(StartupSectorId));
    }

  // LEVEL=L — load the whole requested level in --test=sector and anchor the
  // initial camera/origin at world 0,0 instead of assuming sector 0,0 exists.
    {
        std::string p;
        if (arg_param(cmd, "level", p))
            strncpyz(StartupLevelId, p.c_str(), sizeof(StartupLevelId));
        if (arg_param(cmd, "scene-camera", p))
            StartupSceneCameraSet = sscanf(p.c_str(), "%d,%d,%d,%d", &StartupSceneCamera[0],
                &StartupSceneCamera[1], &StartupSceneCamera[2], &StartupSceneCamera[3]) == 4;
        if (arg_param(cmd, "scene-viewport", p)) {
            if (!scene_viewport::Parse(p.c_str(),StartupSceneViewport))
                FatalError("Invalid --scene-viewport: expected positive W,H within1920x1080",nullptr);
            StartupSceneViewportSet=true;
        } else if (arg_flag(cmd,"scene-viewport"))
            FatalError("Missing --scene-viewport W,H",nullptr);
        if (arg_param(cmd, "scene-module", p))
            strncpyz(StartupSceneModule, p.c_str(), sizeof(StartupSceneModule));
        if (arg_param(cmd, "scene-command-file", p))
            strncpyz(StartupSceneCommandFile, p.c_str(), sizeof(StartupSceneCommandFile));
        if (arg_param(cmd, "scene-ambient", p))
        {
            StartupSceneAmbientSet = sscanf(p.c_str(), "%d,%d,%d,%d", &StartupSceneAmbient[0],
                &StartupSceneAmbient[1], &StartupSceneAmbient[2], &StartupSceneAmbient[3]) == 4;
            for (const int32_t channel : StartupSceneAmbient)
                StartupSceneAmbientSet = StartupSceneAmbientSet && channel >= 0 && channel <= 255;
        }
    }

  // ASSET=path — which imagery --test=i3d3d loads.
    {
        std::string p;
        if (arg_param(cmd, "asset", p))
            strncpyz(StartupAssetPath, p.c_str(), sizeof(StartupAssetPath));
    }

  // SCALE=f — uniform mesh scale multiplier for --test=i3d3d (0 = auto-fit).
    {
        std::string p;
        if (arg_param(cmd, "scale", p))
            StartupAssetScale = float(atof(p.c_str()));
    }

  // DUMPTILES=path — export tile albedo PNGs to the given folder, creating it if needed.
    {
        std::string p;
        if (arg_param(cmd, "dumptiles", p))
            strncpyz(StartupDumpTilesPath, p.c_str(), MAXPATHLEN);
    }

  // DUMPICONS=path — export all baked inventory icons / portraits to the
  // given folder, creating it if needed.
    {
        std::string p;
        if (arg_param(cmd, "dumpicons", p))
            strncpyz(StartupDumpIconsPath, p.c_str(), MAXPATHLEN);
    }

  // DUMPGLTF=asset|@list [DUMPGLTFOUT=dir|file.glb] — export I3D asset(s)
  // as Blender-loadable .glb with per-state animations.
    {
        std::string p;
        if (arg_param(cmd, "dumpgltf", p))
            strncpyz(StartupDumpGltfPath, p.c_str(), MAXPATHLEN);
        if (arg_param(cmd, "dumpgltfout", p))
            strncpyz(StartupDumpGltfOutPath, p.c_str(), MAXPATHLEN);
    }

  // DUMPI3D=asset [DUMPI3DOUT=dir] — extract one I3D's textures (PNG) +
  // sub-objects (Wavefront OBJ) + manifest into a folder, then exit.
    {
        std::string p;
        if (arg_param(cmd, "dumpi3d", p))
        {
            strncpyz(StartupDumpI3DPath, p.c_str(), MAXPATHLEN);
            // Default test mode to i3ddump when --dumpi3d is supplied and
            // no other test mode was named (mirrors the dumptiles pattern).
            if (!StartupTestMode[0])
                strncpyz(StartupTestMode, "i3ddump", sizeof(StartupTestMode));
        }
        if (arg_param(cmd, "dumpi3dout", p))
            strncpyz(StartupDumpI3DOutPath, p.c_str(), MAXPATHLEN);
    }
}

// REVSYNC: GetINISettings @ 0x00484500 — the [Paths] section, with retail's
// defaults. Each path ends with a backslash. The loaders compose paths from
// these: shared game data under ClassDefPath / ResourcePath (resources.rvr
// answers .\Resources\), imagery data under ImageryPath (imagery.rvi answers
// .\Imagery\), a module's own files under ModulesPath\<module>\. See
// docs/DATA_LAYOUT.md. Retail's CurMapPath isn't read: the port keeps the
// changed sectors in memory, not in a curmap folder (sectorstore.h).
void GetINISettings()
{
  // ***** Get Program Paths *****

    INISetSection("Paths");
    INIGetText("ExileRCPath", ".", ExileRCPath, MAXPATHLEN);
    INIGetText("ClassDefPath", ".\\Resources", ClassDefPath, MAXPATHLEN);
    INIGetText("ResourcePath", ".\\Resources", ResourcePath, MAXPATHLEN);
    INIGetText("ImageryPath", ".\\Imagery", ImageryPath, MAXPATHLEN);
    INIGetText("BaseMapPath", ".", BaseMapPath, MAXPATHLEN);
    INIGetText("MoviePath", ".\\Resources\\FMV", MoviePath, MAXPATHLEN);
    INIGetText("SaveGamePath", ".\\Save", SaveGamePath, MAXPATHLEN);
    INIGetText("ModulesPath", ".\\Modules", ModulesPath, MAXPATHLEN);

    // Make sure each string ends with a backslash
    for (char *path : {ClassDefPath, ExileRCPath, ResourcePath, ImageryPath,
                       BaseMapPath, MoviePath, SaveGamePath, ModulesPath})
    {
        if (path[0] == '\0' || path[strlen(path) - 1] != '\\')
            strncatz(path, "\\", MAXPATHLEN);
    }

    // Defaults as retail GetINISettings (FUN_00484500);
    // docs/LIGHTING_FIDELITY.md §3.1. ReadOptions (gameoptions.cpp) reads
    // [Options].
    INISetSection("Lighting");
    MaxLights = INIGetInt("MaxLights", 3);
    Ambient3D = INIGetInt("Ambient3D", 130);
    LightRange3D = INIGetInt("LightRange3D", 180);
    LightMult3D = INIGetInt("LightMult3D", 250);

    // FUN_00484500: the language names the dialog lists and the voice folder,
    // so it is known before the main module mounts.
    INISetSection("Language");
    char language[64];
    INIGetText("Language", "English", language, sizeof(language));
    Language = language;
}

// This function gets called as soon as the display system finds the right driver.
// You can check the parameters of the driver and set system flags here so that things
// work correctly.
void DriverSetupCallback()
{
#if 0 // TODO(port): Subsystem 2 — DirectDraw driver description inspection (→ sokol backend)
    char buf[DRIVERDESCLEN];
    strncpyz(buf, DirectDrawDesc, DRIVERDESCLEN);
    strlwr(buf);

    // Set special flags for VooDoo cards
    if (strstr(buf, "voodoo"))  // Only voodoo cards would match this!
    {
        NoVidZBufLock = true; // Don't try to lock video and zbuffer at same time
        NoBlitZBuffer = true; // Can't blit to or from the zbuffer
        UseClearZBuffer = true; // Use a secondary clear zbuffer for drawing instead of display zbuffer
    }
#endif
}

// TODO(port): multi-monitor selection. sokol_app puts the window on the
// user's active display; the MONITOR=n command-line selector from the
// Win32 build is not wired up yet. Originals in attic-final:attic/src/revmain_win32_entry.cpp.
bool InitMonitor()
{
    MonitorX = 0;
    MonitorY = 0;
    MonitorW = GameScreenWidth;
    MonitorH = GameScreenHeight;
    return true;
}

// ************************************************************************
// *                  InitGlobals and ShutdownGlobals                      *
// ************************************************************************
//
// One canonical caller for every file-scope global's lifecycle: each
// singleton's existing API name (Initialize/Close, Init/Shutdown, or
// AllocCache, depending on the class) is invoked from here in a fixed
// construction order. ShutdownGlobals tears them down in EXACT reverse,
// with comments numbered to match step-for-step.

bool InitGlobals()
{
    // (1) MainWindow — sokol_app already owns the actual window by the
    // time AppInit calls us, so this is just the engine-side state-flip.
    if (!MainWindow.Initialize())
        FatalError("Couldn't bring up MainWindow", nullptr);

    // (2) FontTable — parses FONT.DEF. Runs before Display.Initialize()
    // (sokol_gfx setup) because GPU atlas building is deferred to first use.
    FontTable = new TFontTable;
    if (!FontTable->Initialize())
        FatalError("Unable to load FONT.DEF");

    SystemFont       = FontTable->Bitmap("System");
    DialogFont       = FontTable->Bitmap("Dialog");
    DialogFontShadow = FontTable->Bitmap("Dialog");  // WINFONT - shadow handled by TrueType later
    SmallFont        = FontTable->Bitmap("Small");
    GameFont         = FontTable->Bitmap("Med");
    
#if 0 // TODO(port): Subsystem 7 — MSVC CRT debug heap check (_CrtCheckMemory)
    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif
  // (8) ImageryPath — pure setting, no allocation.
    if (NoNormals == true)
        TObjectImagery::SetImageryPath(NONORMALPATH);
    else
        TObjectImagery::SetImageryPath(NORMALPATH);

#if 0 // TODO(port): Subsystem 7 — MSVC CRT debug heap check (_CrtCheckMemory)
    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif
  // Allocate the chunk cache as 1/4 free physical memory or 4MB
    int32_t physmegs = TotalPhys() / (1024 * 1024);
    if (ChunkCacheSize < 1)
    {
        if (physmegs < 16)
        {
#if 0 // TODO(port): Subsystem 2 — Win32 MessageBox warning; stub as warning log
            uint32_t res = MessageBox(nullptr, "This game requires at least 16MB of system minimum to run well. "
                             "If you press OK the game will load normally, but game "
                             "performance will be severely degraded.",
                             "WARNING", MB_ICONSTOP | MB_OK);

            if (res == IDCANCEL)
                return false;
#else
            fprintf(stderr, "WARNING: less than 16MB physical memory detected; continuing.\n");
#endif

            ChunkCacheSize = 2;
        }
        else if (physmegs <= 24)
            ChunkCacheSize = 3;
        else if (physmegs <= 32)
            ChunkCacheSize = 4;
        else
            ChunkCacheSize = min(physmegs - 32, 16);
    }
#if 0 // TODO(port): Subsystem 7 — MSVC CRT debug heap check (_CrtCheckMemory)
    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif
    if (PreloadSectorSize < 0)
    {
        if (physmegs < 16)
        {
            PreloadSectorSize = 3;
            PreloadSectors = false;
        }
        else if (physmegs <= 24)
        {
            PreloadSectorSize = 3;
            PreloadSectors = false;
        }
        else if (physmegs <= 32)
        {
            PreloadSectorSize = 4;
            PreloadSectors = true;
        }
        else
        {
            PreloadSectorSize = 5;
            PreloadSectors = true;
        }
    }
    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
    // (9) ChunkCache — AllocCache reserves the per-tile pinned buffers.
    ChunkCache.AllocCache(ChunkCacheSize);
    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }

  // Do the critical section object
#if 0 // TODO(port): Subsystem 4 — Win32 CRITICAL_SECTION (→ std::mutex / worker pool)
    InitializeCriticalSection(&CriticalSection);
#endif

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (10) Display — sokol_gfx context setup + backbuffer/zbuffer surfaces
  // + ImGui wiring. Heavy on sokol calls but no game data is loaded.
    if (!Display.Initialize(GameScreenWidth, GameScreenHeight, BPP))
        FatalError("Couldn't open main display", nullptr);

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // Now display is initialized, set up video cap system (if needed)
#if 0 // TODO(port): Subsystem 3 — video capture (DirectShow → AVFoundation/sokol)
    // (11) VideoCapture — optional, off by default; the entire block is
    // currently #if 0'd because the Win32 capture backend isn't ported.
    if (dovideocap)
        VideoCapture.Initialize(videocapmegs, videocapfps);
#endif

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (12) Color tables — free-function setup; nothing to undo.
    MakeColorTables();

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // Print stats
    Status("Revenant Version 0.30 Build %s %s\n"
           "Copyright (C) Cinematix Studios, Inc. 1998\n", __DATE__, __TIME__);
    if (IsMMX)
        Status("MMX detected, using advanced lighting\n");
    else
        Status("No MMX detected, using normal lighting\n");
    Status("Physical memory %d MB\n", physmegs);
    Status("Chunk cache size %d MB\n", ChunkCacheSize);
#if 0 // TODO(port): Subsystem 2 — DirectDraw device enumeration (→ sokol backend)
    Status("Available display devices: %s\n", DriversAvailable);
    Status("Current display device: %s - %s\n", DirectDrawName, DirectDrawDesc);
#endif
    Status("Preload sectors %s, preload size %d\n", (PreloadSectors?"ON":"OFF"), PreloadSectorSize);

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (13) Timer
    Status("Initializing timers\n");
    if (!Timer.Initialize())
        FatalError("Couldn't initialize timer", nullptr);

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // Capture the mouse cursor
//  SetCapture(MainWindow.Hwnd());

  // (14) Direct Input / Joystick — pending Subsystem 5 port; no-op now.
    Status("Initializing direct input\n");
#if 0 // TODO(port): Subsystem 5 — DirectInput (→ sokol_app event translation)
    InitializeDirectInput();
#endif

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // Initialize Joysticks (if any)
    Status("Initializing joysticks\n");
#if 0 // TODO(port): Subsystem 5 — DirectInput joystick (→ GameController framework)
    InitializeJoysticks();
#endif

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (15) Imagery loader thread — static-method API, paired with
  // TObjectImagery::EndLoaderThread() in ShutdownGlobals.
    TObjectImagery::BeginLoaderThread();

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (16) TObjectClass — static load (paired with TObjectClass::FreeClasses
  // in ShutdownGlobals). Loads class.def into the global class registry.
    Status("Loading classes with %s option\n", NoQuickLoad?"NOQUICKLOAD":"QUICKLOAD");
    TObjectClass::LoadClasses();

    if (StartupDumpTilesPath[0])
    {
        Status("Dumping tiles to %s\n", StartupDumpTilesPath);
        if (!TestModes::DumpTilesToFolder(StartupDumpTilesPath))
            FatalError("Failed dumping any tiles to %s", StartupDumpTilesPath);
        Status("Tile dump complete. Exiting.\n");
        return false;
    }

    if (StartupDumpIconsPath[0])
    {
        Status("Dumping icons to %s\n", StartupDumpIconsPath);
        if (!TestModes::DumpIconsToFolder(StartupDumpIconsPath))
            FatalError("Failed dumping any icons to %s", StartupDumpIconsPath);
        Status("Icon dump complete. Exiting.\n");
        return false;
    }

    if (StartupDumpGltfPath[0])
    {
        Status("Dumping glTF for %s\n", StartupDumpGltfPath);
        if (!DumpGltfFromStartupArgs(StartupDumpGltfPath, StartupDumpGltfOutPath))
            FatalError("Failed dumping glTF for %s", StartupDumpGltfPath);
        Status("glTF dump complete. Exiting.\n");
        return false;
    }

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (17) PlayerManager
    Status("Initializing multi-player manager\n");
    PlayerManager.Initialize();

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
  // (18) Rules — Initialize() loads rules.def + char.def.
    Status("Loading game rules\n");
    if (!Rules.Initialize())
        FatalError("Unable to load game rules");

    // Spell lookup must be live before gameplay/story CastByName dispatch.
    // TSpellList::Initialize is idempotent and loads authored spell.def.
    if (!SpellList.Initialize())
        FatalError("Unable to load spell list");

  // (19) DialogList loads the base <ClassDefPath><Language>.def table.
  // The module table loads when TModuleManager mounts the module.
    if (!DialogList.Initialize())
        FatalError("Unable to load dialog list");

  // (20) AssetCache - shared CPU/source asset registry. Loading here gives
  // callers a safe engine handle, not renderer GPU residency.
    AssetCache.Init();

  // (21) MapManager — Init() is a state-flip; the cache populates lazily
  // as PlayScreen / TMapRenderer call GetOrLoad. Has to be live before
  // any subsystem that observes CurrentMapChanged.
    MapManager.Init();

  // (22) Sound — brings up miniaudio + scans data/sound/effects + the
  // language voice dir for WAVs. Comes after ResourcePath / Language are
  // valid and the data manager is up; before any caller can fire SFX.
    Status("Initializing audio system\n");
    SoundPlayer.Initialize();   // failure here is non-fatal; game runs silent

  // (23) ScriptManager — parses master.s into the proto registry and
  // loads gamestate names from state.def. Both files live inside the
  // mounted module archive (master.s is in resources.rvr; state.def is
  // in both base and module), resolved via rev_fopen's basename-keyed
  // VFS. Has to run after MountArchive/MountModule and before any
  // sector spawn (TObjectInstance::InitScript calls back into the
  // proto registry when an instance comes to life). Fatal if the
  // master script can't be parsed — the world has no AI without it.
    Status("Loading scripts\n");
    if (!ScriptManager.Initialize())
        FatalError("Unable to load master script (master.s)");

  // (24) GameData — the shared in-game resource archive (cursor, hand
  // cursor, walk wedges). REVSYNC: retail engine init (0x00485870) loads
  // gamedata.dat into this global before any screen runs; the title screen
  // and the PlayScreen both draw its cursors. (The 1998 snapshot loaded the
  // identical playscrn.dat lazily from TPlayScreen::Initialize.)
    GameData = TMulti::LoadMulti(const_cast<char*>("gamedata.dat"));
    if (!GameData)
        FatalError("Unable to load gamedata.dat");

    if (!_CrtCheckMemory())
    {
//      _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
    return true;
}

void ShutdownGlobals()
{
  // Tear down in EXACT reverse of InitGlobals' construction order, so each
  // global is shut down while everything it depends on is still alive.
  // The numbered comments below mirror the numbered steps at the top of
  // InitGlobals — keep the two in lockstep when adding/removing
  // subsystems. Each shutdown call leaves members in empty/zero state so
  // the trivial dtor that runs at process exit only walks already-emptied
  // structures.
  //
  // Game-state-level teardown (active screen + worker threads) runs once
  // before the InitGlobals-inverse so per-map state has somewhere alive
  // to unwind into.

  // ---- game-state-level teardown (above InitGlobals) ----

  // Stop the active screen (PlayScreen owns the renderer + its listeners
  // on TGameMap). Drop it before the maps it observes get evicted.
    if (CurrentScreen)
    {
        CurrentScreen->Close();
        CurrentScreen = nullptr;
    }

  // Resume any paused worker threads so EndLoaderThread can join cleanly.
    extern bool ThreadsPaused;
    if (ThreadsPaused)
        ResumeThreads();

  // ---- inverse of InitGlobals ----

  // (24) GameData
    if (GameData)
    {
        free(GameData);                 // TMulti is a malloc'd resource blob
        GameData = nullptr;
    }

  // (23) ScriptManager — flushes the proto registry + gamestate names.
  // Editor builds write any dirty scripts back to master.s before the
  // memory is dropped. No live dependents so this can lead the inverse.
    ScriptManager.Close();

  // (22) Sound — stop the playback thread + drain music before anything
  // else unwinds; nothing else depends on audio so this is the safest
  // first inverse step. Also flushes the cached SFX list.
    SoundPlayer.Close();

  // (21) MapManager - inits last in InitGlobals, so closes first here.
  // Walks each cached TGameMap → Unload → CloseSector while sectors,
  // player inventories, and scripts are still alive. Was the use-after-
  // free site that motivated the whole Init/Shutdown refactor.
    MapManager.Shutdown();

  // (20) AssetCache - outlives maps and any source asset handles they held.
    AssetCache.Shutdown();

  // (19) DialogList
    DialogList.Close();

  // Spell definitions depend on Rules and must close first.
    SpellList.Close();

  // (18) Rules
    Rules.Close();

  // SpellList — loaded by TPlayScreen::Initialize (retail 0x0047add4) or by
  // a --test=ui-* demo player; nothing to close when neither ran.
    SpellList.Close();

  // (17) PlayerManager
    PlayerManager.Close();

  // (16) TObjectClass — free the class registry/instances.
    TObjectClass::FreeClasses();

  // (15) Imagery loader thread
    TObjectImagery::EndLoaderThread();

  // (14) DirectInput / joystick teardown
#if 0 // TODO(port): Subsystem 5 — DirectInput teardown (→ sokol_app cleanup)
    CloseDirectInput();
    ReleaseCapture();
#endif

  // (13) Timer
    Timer.Close();

  // (12) Color tables — no per-instance cleanup needed.

  // (11) Video capture
#if 0 // TODO(port): Subsystem 3 — video capture (DirectShow → AVFoundation/sokol)
    VideoCapture.Close();
#endif

  // (10) Display
    Display.Close();

  // (9) ChunkCache — currently no shutdown; allocations free at process
  // exit via the dtor.

  // (8) ImageryPath — pure setting, nothing to undo.

  // (2) FontTable
    if (FontTable) {
        FontTable->Close();
        delete FontTable;
        FontTable = nullptr;
    }
    SystemFont = DialogFont = DialogFontShadow = SmallFont = GameFont = nullptr;

#if 0 // TODO(port): Subsystem 2 — Win32 message pump drain (→ sokol_app cleanup)
    MSG Message;
    while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&Message);
        DispatchMessage(&Message);
    }
#endif

  // (1) MainWindow — inverse of step 1 in InitGlobals.
    MainWindow.Close();

#if 0 // TODO(port): Subsystem 5 — Win32 ClipCursor (→ sapp_lock_mouse)
    ClipCursor(nullptr);
#endif
}

// ************************************************************************
// *               sokol_main - macOS/sokol_app Entry Point               *
// ************************************************************************
//
// sokol_app owns the window and the run loop. Our lifecycle is:
//   1. sokol_main()  -> return sapp_desc describing the window + callbacks
//   2. AppInit()     -> sapp has created the window + Metal context.
//                       Call InitGlobals() which brings up TDisplay (which
//                       itself calls sg_setup via sapp_sgcontext()),
//                       timers, imagery loader, rules, etc.
//   3. AppFrame()    -> one game tick per call. Currently drives the
//                       legacy screen dispatch by pumping one frame of
//                       the current TScreen.
//   4. AppEvent()    -> translate sapp_event -> TScreen input hooks.
//   5. AppCleanup()  -> ShutdownGlobals() and teardown.
//
// The pre-port WinMain / MessageLoop / RegisterClass scaffolding lives
// in attic-final:attic/src/revmain_win32_entry.cpp and attic-final:attic/src/mainwnd_win32.cpp.

#include <sokol_glue.h>
#include "sokol_imgui.h"
#include "imgui.h"

// Forward decls for the sokol callbacks (defined below sokol_main).
static void AppInit();
static void AppFrame();
void SetMaxRuntimeSeconds(double s);   // see definition above AppFrame
static void AppEvent(const sapp_event* ev);
static void AppCleanup();

// The screen we were told to start on. Latched in AppInit, consumed in
// AppFrame. When a screen ends (TScreen::ShowScreen logic formerly in
// the old WinMain loop) we advance to GetNextScreen().
static TScreen* BootScreen = nullptr;
static bool SystemInitialized = false;

// Saved argv for AppInit — sokol owns the run loop after sokol_main
// returns, so we stash the pointers for the later GetParameters call.
static int    g_argc = 0;
static char **g_argv = nullptr;

sapp_desc sokol_main(int argc, char* argv[])
{
    // Stand up logging before anything else so early failures are visible.
    rev_logging_init("revenant.log");

    g_argc = argc;
    g_argv = argv;

    // --retail-ab=<target>: dump a port function over a case file for the
    // retail A/B compare, then exit; no window, no engine (retailab.h).
    int abexit = 0;
    if (RetailAB::Run(argc, argv, abexit))
        std::exit(abexit);

    IsMMX = false;
    ApplyCommandLineResolution(argc, argv);

    // --headless: skip sokol_app's makeKeyAndOrderFront so no NSWindow
    // appears on the desktop. Parsed here (before sokol_app shows the
    // window) AND again later in AppInit (which sets the post-init hide
    // flag in case desc.hidden wasn't honored by a build that doesn't
    // include our sokol_app patch). Scan argv directly — argh hasn't run.
    // Also parse --max-runtime=N here: a wall-clock hard ceiling that
    // hard-exits the process N seconds after AppFrame first ticks.
    // This is a belt-and-suspenders safety net for agent runs — even if
    // the input-script's auto-exit fails or the engine deadlocks before
    // the script drains, the process eventually exits on its own.
    bool cli_headless = false;
    bool cli_lockstep = false;          // --control / --replay: the session is serviced from frames
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (!a) continue;
        while (*a == '-' || *a == '/') ++a;
        if (std::strcmp(a, "headless") == 0) { cli_headless = true; }
        else if (std::strncmp(a, "control=", 8) == 0 || std::strncmp(a, "replay=", 7) == 0) { cli_lockstep = true; }
        else if (std::strncmp(a, "max-runtime=", 12) == 0)
        {
            char* endp = nullptr;
            const long s = std::strtol(a + 12, &endp, 10);
            if (endp && *endp == '\0' && s > 0)
                SetMaxRuntimeSeconds(double(s));
        }
    }

    sapp_desc desc = {};
    desc.init_cb = AppInit;
    desc.frame_cb = AppFrame;
    desc.event_cb = AppEvent;
    desc.cleanup_cb = AppCleanup;
    desc.width = GameScreenWidth;
    desc.height = GameScreenHeight;
    desc.window_title = "Revenant";
    desc.high_dpi = false;
    desc.sample_count = 1;
    // Enable OS clipboard. Required for the editor's Cut/Copy/Paste
    // commands -- sokol_imgui's clipboard handlers route through
    // sapp_get/set_clipboard_string which silently no-op when this
    // flag is false. Bumped buf size beyond the 8KB default since
    // serialized object blocks can run large.
    desc.enable_clipboard = true;
    desc.clipboard_size   = 1 << 20;     // 1 MB
    // A controlled or replayed session reads its channel each frame; frames
    // from a common-mode timer keep it going while the window is clicked.
    desc.timer_frames = cli_lockstep;
    if (cli_headless)
    {
        // Our sokol_app patch: skip makeKeyAndOrderFront so the NSWindow
        // never appears on the desktop, and use the accessory activation
        // policy so the app has no Dock icon. The Metal context + frame
        // timer still run normally; framesnap reads the offscreen RT.
        desc.hidden = true;
        desc.no_dock_icon = true;
        // ...which, silent too, makes the app an App Nap target: keep the
        // frame timer running.
        HeadlessWindow::KeepAwake();
        // Suppress audio in headless mode — agent-driven test runs were
        // dumping music + SFX onto the user's speakers. audio::SetSilenced
        // must be called BEFORE audio::Init (which sound.cpp:406 does on
        // first sound use); short-circuits Init so no miniaudio engine
        // ever comes up.
        audio::SetSilenced(true);
    }
    // Windowed by default; Borderless/FullScreen come from INI/args and
    // take effect before the window opens only if set via argv. Anything
    // else stays at sokol defaults.
    Windowed = true;
    return desc;
}

static bool BootGame();

static void AppInit()
{
    // Equivalent to the top of the retired WinMain: set paths, load INI,
    // parse command line, initialize monitor info, language, window, and
    // the rest of the system.

    // Seeded from the clock as retail's WinMain (srand(time()) at
    // 0x0048662d); --seed reseeds in GetParameters.
    SeedRandom((uint32_t)time(nullptr));

    // Posix port of retail's WinMain GetProgramPaths probe — RunPath is
    // the executable directory, SavePath is the per-user writable
    // Application Support / XDG_DATA_HOME dir (created if missing).
    rev_resolve_program_paths(RunPath, SavePath, MAXPATHLEN);

    // A playtest session (--control, --record, --replay; playtest.h). Under
    // --control the game boots when the director sends `start`.
    if (!Playtest.Initialize(g_argc, g_argv, &BootGame))
    {
        sapp_request_quit();
        return;
    }
    if (Playtest.WaitsForStart())
    {
        if (HeadlessWindow::ParseArgs(g_argc, g_argv))
            HeadlessWindow::HideAllWindows();
        return;
    }
    BootGame();
}

// The rest of the retired WinMain: the INI and the command line, the data,
// the engine, and the first screen. At launch, or on a playtest `start`.
static bool BootGame()
{
    Playtest.BeforeIni();
    INISetPath(RunPath);

    GetINISettings();
    ReadOptions();   // retail's order (0x004865a0): INI, [Options], command line

    GetParameters(g_argc, g_argv);
    Playtest.ApplyStart();

    // Headless snap / filmstrip flags. Parsed once after GetParameters so
    // the args[] argh consumed are still around (we read argv ourselves).
    {
        FrameSnap::SConfig snapCfg;
        if (FrameSnap::ParseArgs(g_argc, g_argv, snapCfg))
        {
            FrameSnap::SetConfig(snapCfg);
            if (snapCfg.seed_set) SeedRandom(snapCfg.seed);
            log_info("[framesnap] active: %s frames=%d interval=%.3fs prefix='%s' out='%s'",
                     snapCfg.is_filmstrip ? "filmstrip" : "single-snap",
                     snapCfg.frames, snapCfg.interval_sec,
                     snapCfg.prefix.c_str(), snapCfg.out_path.c_str());
        }
        if (HeadlessWindow::ParseArgs(g_argc, g_argv))
        {
            log_info("[headless] --headless active; window will be hidden");
            // desc.hidden already kept the window off screen; this and the
            // per-frame call in AppFrame keep it that way.
            HeadlessWindow::HideAllWindows();
            // A hidden window has no pointer for the OS to draw: the game
            // draws its cursor into the frame, where snapshots see it.
            SetHardwareCursorEnabled(false);
        }
    }

    // Base resource archives — retail WinMain opened these explicitly before
    // anything that calls rev_fopen (FontTable->Initialize, LoadClasses...).
    if (!MountArchive("resources.rvr"))
        FatalError("Error in pack file RESOURCE.RVR", nullptr);
    if (!MountArchive("imagery.rvi"))
        FatalError("Error in pack file IMAGERY.RVI", nullptr);

    // Enumerate available modules (data/Modules/*) and mount the main one.
    // ModuleManager.Initialize parses each module.def into a TModule, then
    // resolves [Modules]/MainModule from the INI; SetCurModule does the
    // actual VFS mount. Per-module files (area.def, master.s, sector .DATs,
    // etc.) resolve through that mount thereafter.
    if (!ModuleManager.Initialize())
        FatalError("No playable modules found under data/Modules/", nullptr);
    if (!ModuleManager.SetCurModule(ModuleManager.MainIndex()))
        FatalError("Unable to mount main module", nullptr);
    if (StartupSceneModule[0] && !ModuleManager.SetCurModule(StartupSceneModule))
        FatalError("Unable to mount requested scene module", nullptr);

    if (!InitMonitor())
        FatalError("Invalid monitor selected", nullptr);

    // MainWindow's lifecycle now lives at the top of InitGlobals so every
    // singleton flows through the canonical caller; sokol_app already
    // owns the actual window so there is no construction-ordering issue.
    if (!InitGlobals())
    {
        sapp_request_quit();
        return false;
    }

    // Revisited tunables. Layered defaults, lowest priority first:
    //   1. SRevisitedSettings field initializers (already in place at static init)
    //   2. revisited_defaults.cpp baked globals (this call)
    //   3. Revenant.ini [Revisited] overlay     (gated on --revisited)
    //   4. debug-panel live tweaks              (later in session)
    // Apply runs unconditionally so even classic-retail boots get the
    // modernized engine's baseline sun/AO/shadow state pushed to Renderer.
    // MapRenderer doesn't exist until PlayScreen::Initialize -- that side
    // of Apply fires from there.
    GetBakedRevisitedDefaults(RevisitedSettings);
    LoadRevisitedSettings();
    ApplyRevisitedSettingsToRenderer(Renderer);

    if (StartupTestMode[0])
    {
        if (strcmp(StartupTestMode, "ui-cinematic") == 0)
        {
            log_info("[boot] routing to CinematicScreen");
            BootScreen = &CinematicScreen;
        }
        else if (strcmp(StartupTestMode, "ui-death") == 0)
        {
            log_info("[boot] routing to DeathScreen");
            BootScreen = &DeathScreen;
        }
        else if (strcmp(StartupTestMode, "ui-mainmenu") == 0)
        {
            log_info("[boot] routing to LogoScreen (title)");
            SBootOptions options;
            options.noIntro    = true;
            options.menuButton = TLogoScreen::ButtonFromName(StartupMenuButton);
            BootScreen = GameFlow.Boot(options);
        }
        else
        {
            log_info("[boot] routing to TestScreen, mode='%s'", StartupTestMode);
            BootScreen = &TestScreen;
        }
    }
    else
    {
        SBootOptions options;
        options.quickstart     = StartupQuickstart || StartupSavePath[0];
        options.quickstartSave = StartupQuickstartSave[0] ? StartupQuickstartSave : StartupSavePath;
        options.noIntro        = StartupNoIntro;
        options.menuButton     = TLogoScreen::ButtonFromName(StartupMenuButton);
        if (StartupMenuButton[0] && options.menuButton < 0)
            log_warn("[boot] --menu=%s: unknown title button "
                     "(newgame|loadgame|options|exit)", StartupMenuButton);
        if (StartupSectorId[0] &&
            std::sscanf(StartupSectorId, "%d_%d_%d", &options.devLevel,
                        &options.devSectorX, &options.devSectorY) != 3)
        {
            log_warn("[boot] bad --sector='%s', expected L_X_Y", StartupSectorId);
            options.devLevel = options.devSectorX = options.devSectorY = -1;
        }
        else if (!StartupSectorId[0] && StartupLevelId[0] &&
                 std::sscanf(StartupLevelId, "%d", &options.devLevel) != 1)
        {
            log_warn("[boot] bad --level='%s', expected L", StartupLevelId);
            options.devLevel = -1;
        }
        BootScreen = GameFlow.Boot(options);
    }

    TestModes::InputSimArm();   // no-op without --input-script

    SystemInitialized = true;
    Playtest.Started();
    return true;
}

// --max-runtime=N: wall-clock hard ceiling that std::_Exit(0)s the process
// N seconds after the first AppFrame tick. Belt-and-suspenders safety net
// for agent runs (paired with the input-script auto-exit). 0 = disabled.
// Parsed once in GetParameters; consumed by AppFrame.
static double g_max_runtime_sec = 0.0;
static std::chrono::steady_clock::time_point g_max_runtime_start;
static bool g_max_runtime_started = false;

void SetMaxRuntimeSeconds(double s)
{
    g_max_runtime_sec = s;
    g_max_runtime_started = false;
}

// A frame that runs no game: a playtest host waiting between steps keeps
// its window showing the last frame, under the director's caption.
static void PresentIdleFrame()
{
    if (!SystemInitialized || Closing || HeadlessWindow::IsActive() || !Display.IsActive())
        return;
    simgui_frame_desc_t fd = {};
    fd.width      = sapp_width();
    fd.height     = sapp_height();
    fd.delta_time = sapp_frame_duration();
    fd.dpi_scale  = sapp_dpi_scale();
    simgui_new_frame(&fd);
    Playtest.DrawOverlay();
    const uint64_t draws = RandomDraws();
    Display.Represent();
    if (RandomDraws() != draws)
        log_error("[playtest] presenting the last frame again drew game random numbers");
}

// One step of game time: --fixedstep, a lockstep playtest session and
// Shadow Smoke advance exactly one game tick on an integer count; otherwise
// a frame capture's --snapstep, otherwise the live clock (a replay's
// recorded frame times).
static void BeginTimeStep()
{
    const auto& snap_config = FrameSnap::GetConfig();
    if (FixedStep || Shadow.Active() || (Playtest.Active() && Playtest.Lockstep()))
        TTime::BeginFixedFrame();
    else
        TTime::BeginFrame(Playtest.FrameTime(FrameSnap::Active() && snap_config.fixed_step_sec > 0.0
                                             ? snap_config.fixed_step_sec : sapp_frame_duration()));
}

static void AppFrame()
{
    // Drive the legacy screen dispatch. The pre-port top-level loop was:
    //     while (NextScreen) NextScreen = TScreen::ShowScreen(NextScreen, 0);
    // TScreen::ShowScreen is blocking (runs that screen's TimerLoop),
    // Every screen (test + production) flows through the same path now that
    // TScreen::TimerTick is sokol-native. ShowScreen Initialize()s a screen
    // and hands back; AppFrame calls its TimerTick once per sokol frame;
    // when the screen sets `done`, we EndCurrentScreen and advance to
    // GetNextScreen (or quit if there isn't one).

    // --max-runtime hard ceiling: if a positive limit was passed, hard-
    // exit when wall-clock elapsed since first frame exceeds it. Belt-
    // and-suspenders with the input-script auto-exit -- catches engine
    // deadlocks, runaway loops, or crashes-pre-script-start that the
    // script-side path can't handle. Uses std::_Exit(0) (skips atexit /
    // static dtors) because sapp_request_quit() is best-effort and can
    // be swallowed by a hung screen TimerLoop -- the whole point of this
    // safety net is that it WORKS even when something is stuck. Mid-
    // capture PNGs may be lost, which is fine: max-runtime hitting means
    // the run was already broken. A recording gets its end line first.
    if (g_max_runtime_sec > 0.0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!g_max_runtime_started)
        {
            g_max_runtime_start = now;
            g_max_runtime_started = true;
        }
        const double elapsed_sec = std::chrono::duration<double>(now - g_max_runtime_start).count();
        if (elapsed_sec >= g_max_runtime_sec)
        {
            log_info("[max-runtime] hit %.1fs limit -- hard exit",
                     g_max_runtime_sec);
            Playtest.Shutdown();
            std::_Exit(0);
        }
    }

    // A playtest session decides whether a frame runs: under --control only
    // while a step has ticks left (docs/playtest/PLAN.md §6.2).
    if (Playtest.Active() && !Playtest.BeginFrame())
    {
        PresentIdleFrame();
        return;
    }

    if (!SystemInitialized || Closing)
        return;

    Display.UpdateCanvas(sapp_width(), sapp_height());

    // The frame's first time step (turbo's further ones begin in the
    // simulation loop below).
    const int32_t steps = Shadow.Active() ? Shadow.StepsThisFrame() : 1;
    BeginTimeStep();

    // Start the ImGui frame before Animate/draw so screens can build debug
    // panels from their normal per-frame code. simgui_render() is called
    // inside TDisplay::FlipPage on the swapchain pass.
    {
        // KEEP trickle enabled (ImGui 1.92 default). It splits ambiguous
        // input transitions across frames, which is what makes Mac
        // Cmd-modified shortcuts work (sokol's Cmd-held auto-keyup
        // fires KEY_DOWN+KEY_UP in the same NSEvent — without trickle,
        // Z appears to never be pressed). Trickle's per-event-type
        // rules already consolidate mouse-move spam, so high-Hz mice
        // don't actually pile up. See sokol issue #233 +
        // memory/project_imgui_mac_modifiers.md.
        simgui_frame_desc_t fd = {};
        fd.width       = sapp_width();
        fd.height      = sapp_height();
        fd.delta_time  = sapp_frame_duration();
        fd.dpi_scale   = sapp_dpi_scale();
        simgui_new_frame(&fd);
    }

    // Begin next queued screen if none is active. With nothing to run the app
    // is quitting; quit requests are processed asynchronously (frames can keep
    // arriving meanwhile), so close the ImGui frame opened above before
    // returning or the next NewFrame asserts.
    if (!CurrentScreen && BootScreen)
    {
        TScreen *next = BootScreen;
        BootScreen = nullptr;
        TScreen::ShowScreen(next, 0);
    }

    if (!CurrentScreen)
    {
        ImGui::EndFrame();
        sapp_request_quit();
        return;
    }

    // The cursor's corner picture starts over each frame (cursor.h).
    BeginCursorFrame();

    // The frame's simulation: one step, or under Shadow Smoke's turbo as
    // many as fit its budget, each a whole frame's worth -- the time step,
    // the input, the tick, the update list -- so input given during a step
    // acts on the next tick (docs/shadow/ARCHITECTURE.md §11). Only the
    // last step draws. A screen that is ending stops the steps.
    for (int32_t step = 0; step < steps; ++step)
    {
        if (step > 0)
        {
            if (CurrentScreen->ReadyToEnd() || Closing)
                break;
            BeginTimeStep();
        }

        // The frame's input, in order, before the tick (inputqueue.h): what
        // the hardware sent since the last frame, then scripted input
        // (--input-script), then a playtest session's (its step's events, or
        // its recording's).
        TestModes::InputSimTick(CurrentScreen);
        Playtest.BeforeDrain();
        InputQueue.Drain(CurrentScreen);

        // Tick / Draw split (see docs/FRAME_PIPELINE.md). Tick catches up
        // missed legacy 24Hz pulses (pure sim, no draw calls); DrawFrame
        // opens the Overlay2D pass on the backbuffer and runs the screen's
        // 2D draw work. Anything in the active screen's Pulse goes through
        // Tick; anything in its Animate goes through DrawFrame.
        CurrentScreen->Tick();
        TObjectComponent::RunUpdateList();
    }
    if (Shadow.Active())
        Shadow.StepsDone();
    CurrentScreen->DrawFrame();
    DebugUI::DrawFrame();
    Playtest.DrawOverlay();     // over the window only: frame captures leave it out

    // Present the frame: composite backbuffer onto the swapchain and commit
    // the Metal command buffer. Without this, sokol's cmd_buffer is never
    // released/nil'd and its dangling pointer crashes the next frame's
    // sg_begin_pass. Skip in headless mode (no Display.Initialize() yet
    // / already Close()d) — IsActive() guards the back-buffer dereference
    // path inside FlipPage.
    if (Display.IsActive())
        Display.FlipPage();

    // --headless: hide the OS window so snap runs don't pop up on screen
    // and steal focus. Called every frame (cheap; idempotent).
    if (HeadlessWindow::IsActive())
        HeadlessWindow::HideAllWindows();

    // Headless snap / filmstrip tick — reads the backbuffer (GPU readback),
    // accumulates frames, and on the Nth capture writes PNG(s) + quits.
    if (FrameSnap::Active())
        FrameSnap::TickAfterRender();

    Playtest.EndFrame();

    // A closing screen runs on until its fade-out reaches black.
    if (CurrentScreen->ReadyToEnd() || Closing)
    {
        TScreen *next = CurrentScreen->GetNextScreen();
        TScreen::EndCurrentScreen();
        GameFlow.ScreenEnded(next);
        BootScreen = next;
        if (!next && !Closing)
            sapp_request_quit();
    }
}

static void AppCleanup()
{
    // ShutdownGlobals() handles MainWindow.Close() as its very last step;
    // AppCleanup just drives that and then unmounts the resource archives.
    // The log lines bracket the teardown so a test run can confirm the
    // process went through it rather than a hard exit.
    log_info("[shutdown] begin");
    Playtest.Shutdown();        // a recording's end line, while the game is still up
    // REVSYNC: 0x004870ee -- retail saved [Options] once its main loop ended.
    SaveOptions();
    if (SystemInitialized)
        ShutdownGlobals();
    UnmountAll();
    log_info("[shutdown] complete");
    // A verified replay that diverged exits 1, a session that couldn't
    // start 2 (playtest.h).
    if (const int code = Playtest.ExitCode())
        std::exit(code);
    // A Shadow Smoke run: 1 a step failed, 2 a ledger violation (shadowrun.h).
    if (const int code = Shadow.ExitCode())
        std::exit(code);
}

// Translate an sapp_keycode into the Windows virtual-key code the screen and
// pane key handlers were written against, and the recordings name
// (docs/playtest/PROTOCOL.md §2). Letters and digits are their ASCII codes
// in both; punctuation and the keypad have VK codes of their own (passing
// sokol's through made ' read as Right and - as Insert). 0: no VK (the key
// is not delivered).
static int32_t SappKeyToVK(sapp_keycode key)
{
    if ((key >= SAPP_KEYCODE_A && key <= SAPP_KEYCODE_Z) ||
        (key >= SAPP_KEYCODE_0 && key <= SAPP_KEYCODE_9))
        return (int32_t)key;
    if (key >= SAPP_KEYCODE_F1 && key <= SAPP_KEYCODE_F24)
        return VK_F1 + (key - SAPP_KEYCODE_F1);
    if (key >= SAPP_KEYCODE_KP_0 && key <= SAPP_KEYCODE_KP_9)
        return VK_NUMPAD0 + (key - SAPP_KEYCODE_KP_0);
    switch (key)
    {
        case SAPP_KEYCODE_LEFT_CONTROL:
        case SAPP_KEYCODE_RIGHT_CONTROL: return VK_CONTROL;
        case SAPP_KEYCODE_LEFT_ALT:
        case SAPP_KEYCODE_RIGHT_ALT:     return VK_MENU;
        case SAPP_KEYCODE_LEFT_SHIFT:
        case SAPP_KEYCODE_RIGHT_SHIFT:   return VK_SHIFT;
        case SAPP_KEYCODE_LEFT_SUPER:    return VK_LWIN;
        case SAPP_KEYCODE_RIGHT_SUPER:   return VK_RWIN;
        case SAPP_KEYCODE_MENU:          return VK_APPS;
        case SAPP_KEYCODE_BACKSPACE:     return VK_BACK;
        case SAPP_KEYCODE_ENTER:
        case SAPP_KEYCODE_KP_ENTER:      return VK_RETURN;
        case SAPP_KEYCODE_TAB:           return VK_TAB;
        case SAPP_KEYCODE_ESCAPE:        return VK_ESCAPE;
        case SAPP_KEYCODE_SPACE:         return VK_SPACE;
        case SAPP_KEYCODE_LEFT:          return VK_LEFT;
        case SAPP_KEYCODE_RIGHT:         return VK_RIGHT;
        case SAPP_KEYCODE_UP:            return VK_UP;
        case SAPP_KEYCODE_DOWN:          return VK_DOWN;
        case SAPP_KEYCODE_INSERT:        return VK_INSERT;
        case SAPP_KEYCODE_DELETE:        return VK_DELETE;
        case SAPP_KEYCODE_HOME:          return VK_HOME;
        case SAPP_KEYCODE_END:           return VK_END;
        case SAPP_KEYCODE_PAGE_UP:       return VK_PRIOR;
        case SAPP_KEYCODE_PAGE_DOWN:     return VK_NEXT;
        case SAPP_KEYCODE_CAPS_LOCK:     return VK_CAPITAL;
        case SAPP_KEYCODE_SCROLL_LOCK:   return VK_SCROLL;
        case SAPP_KEYCODE_NUM_LOCK:      return VK_NUMLOCK;
        case SAPP_KEYCODE_PRINT_SCREEN:  return VK_SNAPSHOT;
        case SAPP_KEYCODE_PAUSE:         return VK_PAUSE;
        case SAPP_KEYCODE_KP_DECIMAL:    return VK_DECIMAL;
        case SAPP_KEYCODE_KP_DIVIDE:     return VK_DIVIDE;
        case SAPP_KEYCODE_KP_MULTIPLY:   return VK_MULTIPLY;
        case SAPP_KEYCODE_KP_SUBTRACT:   return VK_SUBTRACT;
        case SAPP_KEYCODE_KP_ADD:        return VK_ADD;
        // The US layout's OEM keys (VK_OEM_1 .. VK_OEM_7, VK_OEM_PLUS ..).
        case SAPP_KEYCODE_SEMICOLON:     return 0xBA;
        case SAPP_KEYCODE_EQUAL:         return 0xBB;
        case SAPP_KEYCODE_COMMA:         return 0xBC;
        case SAPP_KEYCODE_MINUS:         return 0xBD;
        case SAPP_KEYCODE_PERIOD:        return 0xBE;
        case SAPP_KEYCODE_SLASH:         return 0xBF;
        case SAPP_KEYCODE_GRAVE_ACCENT:  return 0xC0;
        case SAPP_KEYCODE_LEFT_BRACKET:  return 0xDB;
        case SAPP_KEYCODE_BACKSLASH:     return 0xDC;
        case SAPP_KEYCODE_RIGHT_BRACKET: return 0xDD;
        case SAPP_KEYCODE_APOSTROPHE:    return 0xDE;
        default:                         return 0;
    }
}

// sokol's events become the game's input. Window, focus and quit events are
// handled here; game input passes the ImGui and focus gates and joins the
// input queue, which the frame dispatches before its tick (AppFrame,
// inputqueue.h).
static void AppEvent(const sapp_event* ev)
{
    if (!ev) return;
    // macOS can deliver window events (focus on activation) before AppInit
    // has stood the display up: there's no ImGui context, nor a game, to
    // route them to yet.
    if (!Display.ImGuiReady())
        return;

    // ImGui sees every event. simgui_handle_event returns the OR of
    // WantCaptureKeyboard and WantCaptureMouse, which swallows mouse drags
    // whenever a slider has keyboard focus — so we split the flags and gate
    // each event class on the one that matters.
    simgui_handle_event(ev);
    ImGuiIO& io = ImGui::GetIO();
    const bool imgui_kbd   = io.WantCaptureKeyboard;
    const bool imgui_mouse = io.WantCaptureMouse;

    // A controlled or replayed session's input is its director's or its
    // recording's alone; the window still closes.
    if (Playtest.OwnsInput())
    {
        if (ev->type == SAPP_EVENTTYPE_QUIT_REQUESTED)
            Closing = true;
        return;
    }

    const SGameCanvas canvas{Display.Width(), Display.Height()};
    const int32_t mouseX = canvas.MouseX(ev->mouse_x, sapp_width());
    const int32_t mouseY = canvas.MouseY(ev->mouse_y, sapp_height());
    switch (ev->type)
    {
      case SAPP_EVENTTYPE_KEY_DOWN:
      case SAPP_EVENTTYPE_KEY_UP:
      {
        const bool down = ev->type == SAPP_EVENTTYPE_KEY_DOWN;

        // macOS Cmd+Q quits the app, matching the platform convention.
        // ImGui doesn't get to swallow this -- the user always wants it
        // to work even if a slider has keyboard focus.
        if (down && ev->key_code == SAPP_KEYCODE_Q && (ev->modifiers & SAPP_MODIFIER_SUPER))
        {
            sapp_request_quit();
            break;
        }

        const int32_t vk = SappKeyToVK(ev->key_code);
        if (down && vk == VK_F10)
        {
            DebugUI::SetVisible(!DebugUI::IsVisible());
            break;
        }

        // The modifiers always reach the game, so CtrlDown and the others
        // follow the keyboard while ImGui has it or the app is inactive.
        const bool modifier = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT;
        if (!vk || (!modifier && (!AppActive || imgui_kbd)))
            break;
        InputQueue.PushKey(vk, down);
        break;
      }

      case SAPP_EVENTTYPE_CHAR:
      {
        if (!AppActive) break;
        if (imgui_kbd) break;
        if (InputQueue.QueuedCtrl() && InputQueue.QueuedShift()) break;
        // The game reads WM_CHAR codes; macOS reports Backspace as DEL.
        const int32_t ch = ev->char_code == 0x7f ? 0x08 : (int32_t)ev->char_code;
        InputQueue.PushChar(ch);
        break;
      }

      // No MOUSE_ENTER / MOUSE_LEAVE handlers for cursor swapping. The
      // platform layer (src/platform/macosx/cursor.mm) installs its own
      // NSTrackingArea with NSTrackingActiveAlways + NSTrackingCursorUpdate
      // on the sokol contentView, so AppKit drives cursor swaps via
      // cursorUpdate: directly. That fires reliably across key-window
      // transitions, which sokol's MOUSE_LEAVE does not -- sokol's own
      // tracking area is NSTrackingActiveInKeyWindow and goes silent the
      // moment another window becomes key.

      case SAPP_EVENTTYPE_MOUSE_MOVE:
      {
        // While an --input-script owns the cursor, ignore hardware moves so the
        // script has exclusive control of the pointer + hover dispatch.
        if (TestModes::InputScriptActive()) break;
        if (!AppActive) break;
        const bool sector_test_drag =
            StartupTestMode[0] &&
            stricmp(StartupTestMode, "sector") == 0 &&
            (InputQueue.QueuedButtons() != 0);
        if (imgui_mouse && !sector_test_drag) break;
        // A frame can see dozens of moves (trackpads fire at 120–500 Hz);
        // the queue keeps the latest of a run of them.
        InputQueue.PushMove(mouseX, mouseY);
        break;
      }

      case SAPP_EVENTTYPE_MOUSE_DOWN:
      case SAPP_EVENTTYPE_MOUSE_UP:
      {
        if (TestModes::InputScriptActive()) break;   // script owns the mouse
        if (!AppActive) break;
        const bool down = ev->type == SAPP_EVENTTYPE_MOUSE_DOWN;
        // A release always goes through so an in-progress drag can clear even
        // if it lands over an ImGui panel; otherwise drag state latches.
        if (down && imgui_mouse) break;
        int32_t button = 0;
        switch (ev->mouse_button)
        {
          case SAPP_MOUSEBUTTON_LEFT:   button = SInputEvent::kButtonLeft;   break;
          case SAPP_MOUSEBUTTON_RIGHT:  button = SInputEvent::kButtonRight;  break;
          case SAPP_MOUSEBUTTON_MIDDLE: button = SInputEvent::kButtonMiddle; break;
          default: break;
        }
        if (button)
            InputQueue.PushButton(button, down, mouseX, mouseY);
        break;
      }

      case SAPP_EVENTTYPE_RESIZED:
      {
        // Canvas and render-target updates run at the next frame boundary.
        break;
      }

      case SAPP_EVENTTYPE_FOCUSED:
      case SAPP_EVENTTYPE_RESUMED:
      {
        AppActive = true;
        if (CurrentScreen)
            CurrentScreen->Redraw();
        break;
      }

      case SAPP_EVENTTYPE_UNFOCUSED:
      case SAPP_EVENTTYPE_SUSPENDED:
      case SAPP_EVENTTYPE_ICONIFIED:
      {
        AppActive = false;
        break;
      }

      case SAPP_EVENTTYPE_QUIT_REQUESTED:
      {
        Closing = true;
        break;
      }

      default:
        break;
    }
}

// Retired original WinMain body. Replaced by sokol_main + AppInit above.
// Kept here in #if 0 briefly so the diff to attic-final:attic/src/revmain_win32_entry.cpp
// is easy to eyeball; delete once the sokol path is verified at runtime.
#if 0
static void UnusedWinMainAnchor_()
{
#ifdef _DEBUG
   _CrtMemState s1;

   _CrtMemCheckpoint( &s1 );

    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_WNDW);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Preset capabilities flags
    IsMMX = HasMMX();

#if 0 // TODO(port): Subsystem 7 — MEMORYSTATUS snapshot; dropped, see header comment
    StartMemory.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&StartMemory);
#endif

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

    ::hInstance = hInstance;

    int32_t size = sizeof(SObjectInfo);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Set windows to return fail when file not found..
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Calls small heap alloc routine for requests < MAXSMALLHEAP
    _set_sbh_threshold( MAXSMALLHEAP );

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Randomize!!
    srand( (unsigned)time( nullptr ) );

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Get upper case command line
    strupr(lpCmdLine);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Gets location of program path and save path
    GetProgramPaths(lpCmdLine, RunPath, SavePath);
    _chdir(RunPath);
    INISetPath(RunPath);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // What version of Windows is this
    uint32_t ver  = GetVersion();
    uint8_t major = LOBYTE(ver);
    uint8_t minor = HIBYTE(ver);

    if (major <= 3) FatalError("This game requires Windows '95/NT 4.0 (or higher)!", nullptr);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Get INI file settings
    GetINISettings();

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Get Program Parameters
    GetParameters(lpCmdLine);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif
  // Init Pause Thread Mutex
    PauseMutex = CreateMutex(nullptr, false, nullptr);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Init monitor information
    if (!InitMonitor())
        FatalError("Invalid monitor selected", nullptr);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Language: read by GetINISettings ([Language] Language).

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Create the main window
    if (!MainWindow.Initialize(hInstance, hPrevInstance, lpCmdLine, nCmdShow))
        FatalError("Couldn't create main window", nullptr);

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Ok, now initialize the rest of the system
    if (!InitGlobals())
        return 0;

#ifdef _DEBUG
    if (!_CrtCheckMemory())
    {
        _CrtMemDumpAllObjectsSince(&s1);
        _RPT0(_CRT_ERROR, "Memory Error");
    }
#endif

  // Figure out first screen
    TScreen* NextScreen = &PlayScreen;

  // Do game screens
    while (NextScreen)
        NextScreen = TScreen::ShowScreen(NextScreen, 0);

  // Close the system down
    ShutdownGlobals();

    return 0;
}
#endif  // retired WinMain body

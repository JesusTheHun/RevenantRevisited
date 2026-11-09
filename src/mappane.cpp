// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                    mappane.cpp - Map pane object                      *
// *************************************************************************

#include "mappane.h"
#include "dialog.h"

#include "gamemap.h"
#include "graphics.h"
#include "mapmanager.h"
#include "maprenderer.h"
#include "stream.h"
#include "3dscene.h"
#include "bmsurface.h"
#include "multisurface.h"
#include "mosaicsurf.h"
#include "display.h"
#include "dls.h"
#include "editorstub.h"
#include "playscreen.h"
#include "inventory.h"
#include "player.h"
#include "spell.h"
#include "script.h"
#include "ctrlmap.h"
#include "cursor.h"
#include "multi.h"
#include "savegame.h"
#include "money.h"
#include "exit.h"
#include "textbar.h"
#include "uidragstate.h"
#include "spellpane.h"
#include "statpane.h"
#include "sound.h"
#include "logging.h"
#include "gameoptions.h"

#include <algorithm>
#include <array>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include <time.h>
#include <string>
#include <unordered_map>
#include <vector>

extern TEditStatusPane StatusBar;
extern TConsolePane Console;
extern TEditClassPane ClassPane;
extern PTBitmap PointerCursor;
extern TMulti* EditorData;

extern TObjectInstance* TakenObject;
extern TObjectInstance* DroppedObject;

#define GRIDZOFF    334

// NoSmoothScroll stuff

#define SCROLLEDGESIDES     32
#define SCROLLEDGETOP       32
#define SCROLLEDGEBOTTOM    32

#define CHARACTER_HEIGHT    32

#define MAPGRIDWIDTH        (MAPPANEWIDTH - (SCROLLEDGESIDES * 2))
#define MAPGRIDHEIGHT       (MAPPANEHEIGHT - SCROLLEDGETOP - SCROLLEDGEBOTTOM)

inline int32_t ScreenGrid(int32_t n, int32_t s)
{
    if (absval(n) < (s/2))
        return 0;

    if (n < 0)
        n -= s / 2;
    else
        n += s / 2;

    return (n / s);
}

// Fast mapindex → TObjectInstance* lookup. Holders store the integer
// mapindex and resolve through MapPane.GetInstance() each frame (see
// mappane.h). Constructed on first use regardless of global-init order, and
// never destroyed: objects deleted during static destruction (maps or
// players a FatalError/exit path didn't shut down) still unregister here,
// after a destructed function-local static would be gone. Same reasoning as
// TSafeObjectBase's registry (saferef.h). Kept in sync exclusively by
// TObjectInstance::SetMapIndex() and TObjectInstance::~TObjectInstance().
static std::unordered_map<int32_t, TObjectInstance*>& InstMap()
{
    static auto* m = new std::unordered_map<int32_t, TObjectInstance*>();
    return *m;
}

void TMapPane::RegisterInstance(TObjectInstance* oi, int32_t index)
{
    if (!oi || index < 0) return;
    InstMap()[index] = oi;
}

void TMapPane::UnregisterInstance(int32_t index)
{
    if (index < 0) return;
    InstMap().erase(index);
}

// Free-function mapindex lookup used by TSafeRef<T>. Lives here so
// object.h can forward-declare it without pulling in mappane's includes.
TObjectInstance* LookupMapIndex(int32_t index)
{
    if (index < 0) return nullptr;
    auto& m = InstMap();
    auto it = m.find(index);
    return (it != m.end()) ? it->second : nullptr;
}

// Generate a runtime-unique map index. Older data can contain duplicate
// persisted ids, so this must be monotonic/collision-checked rather than
// cycling through a small timestamp-derived range.
int32_t TMapPane::MakeIndex()
{
    static int32_t next = 0x10000000;
    for (;;)
    {
        if (next < 0x10000000)
            next = 0x10000000;
        const int32_t index = next++;
        if (LookupMapIndex(index) == nullptr)
            return index;
    }
}

// ****************
// * TMapIterator *
// ****************

// Initializes iterator with a range value (makes rect and calls Initialize)
TMapIterator::TMapIterator(const S3DPoint& pos, int32_t range, int32_t fl, int32_t objset)
{
    SRect r;
    r.left = pos.x - range; 
    r.top = pos.y - range;
    r.right = pos.x + range; 
    r.bottom = pos.y + range;
    Initialize(&r, fl, objset);
}

// Initializes iterator with an object using default LOCALRANGE value.  This is the 
// standard way to do searches in the map system for objects nearby other objects.
TMapIterator::TMapIterator(RTObjectInstance oi, int32_t fl, int32_t objset)
{
    S3DPoint pos;
    oi.GetPos(pos); 
    SRect r;
    r.left = pos.x - LOCALRANGE; 
    r.top = pos.y - LOCALRANGE;
    r.right = pos.x + LOCALRANGE; 
    r.bottom = pos.y + LOCALRANGE;
    Initialize(&r, fl | CHECK_MAPRECT, objset);
}

TMapIterator::TMapIterator(int32_t lvl, PSRect maprect, int32_t fl, int32_t objset)
    : level(lvl)
{
    Initialize(maprect, fl | CHECK_LOADED | (maprect ? CHECK_MAPRECT : 0), objset);
}

void TMapIterator::Initialize(PSRect sr, int32_t fl, int32_t os)
{
    if (sr)
    {
        if (!(fl & (CHECK_RECT | CHECK_MAPRECT | CHECK_SCRRECT | CHECK_SECTRECT)))
            fl |= CHECK_RECT;
        memcpy(&r, sr, sizeof(SRect));
    }
    else
    {
        memset(&r, 0, sizeof(SRect));
    }
    flags = fl;
    objset = os;
    sx = -1;        // Causes NextItem() to start at sx=0
    sy = 0;
    mapnum = 0;
    secnum = 0;
    index = 0;
    numitems = 0;
    sector = nullptr;
    parent = nullptr;
    invindex = 0;
    parent = item = nullptr;
    NextItem();
}


TObjectInstance* TMapIterator::NextItem()
{
    // OPTIONAL Inventory Iterator
    // ***************************

  // Check Inventory    
    if (flags & CHECK_NOINVENT)
        item = nullptr;
    else
    {
        if (item && item->NumInventoryItems() > 0)
        {
            parent = item;
            invindex = 0;
        }

        item = nullptr;

        while (parent && !item)
        {
            if (invindex < parent->NumInventoryItems())
                item = parent->GetInventory(invindex++);
            else
            {
                invindex = parent->InvIndex() + 1;
                parent = parent->GetOwner();
            }
        }
    }

    // Main Sector Iterator Loop
    // *************************

    while (!item)
    {
        index++;                    // Try to increment index

        // INFREQUENT Get Next Sector
        // **************************

        while (index >= numitems)   // If at end of array (or no array) get next sector
        {
            sector = (flags & CHECK_LOADED) ? NextLoadedSector() : NextWindowSector();
            if (!sector)
                return nullptr;
            numitems = sector->NumObjSetItems(objset);
            index = 0;
        }

        item = sector->GetObjSetInstance(objset, index);    // Get item

        // OPTIONAL: Checks
        // ****************

        while (item && (flags & (CHECK_MOVING | CHECK_INVIS | CHECK_SCRRECT | CHECK_LIGHT | CHECK_MAPRECT)))
        {
            if (flags & CHECK_MOVING)
            {
                if (item->GetFlags() & OF_MOVING)
                {
                    item = nullptr;
                    break;
                }
            }

            if (flags & CHECK_INVIS)
            {
                if (item->GetFlags() & OF_INVISIBLE)
                {
                    item = nullptr;
                    break;
                }
            }

            bool good, checked = false;
            SRect ir;

            if (flags & CHECK_SCRRECT)
            {
                checked = true;
                item->GetScreenRect(ir);
                good = r.Intersects(ir);
            }
            else if (flags & CHECK_MAPRECT)
            {
                checked = true;
                S3DPoint p;
                item->GetPos(p);
                good = r.In(SPoint(p.x, p.y));
            }

            if (flags & CHECK_LIGHT && item->GetFlags() & OF_LIGHT &&
                item->GetLightIntensity() != 0)
            {
                if (!checked || !good)
                {
                    checked = true;
                    item->GetLightRect(ir);
                    good = r.Intersects(ir);
                }
            }

            if (checked && !good)
            {
                item = nullptr;            // go to next item, screen rect doesn't intersect
                break;
            }

            break;      // all tests passed, exit loop
        }
    }

    return item;
}

// The next sector of the pane's 3x3 window that passes the rect checks.
TSector* TMapIterator::NextWindowSector()
{
    for (;;)
    {
        if (++sx >= SECTORWINDOWX)
        {
            sx = 0;
            sy++;
        }
        if (sy >= SECTORWINDOWY)
            return nullptr;

        TSector* candidate = MapPane.sectors[sx][sy];
        if (!candidate)
            continue;

        SRect sr;
        if (flags & CHECK_SECTRECT)
        {
            candidate->GetMaxScreenRect(sr);
            if (!r.Intersects(sr))
                continue;
        }
        else if (flags & CHECK_MAPRECT)
        {
            candidate->GetMaxMapRect(sr);
            if (!r.Intersects(sr))
                continue;
        }
        return candidate;
    }
}

// The next loaded sector of `level` (every level when negative) that meets
// the map rect, walking MapManager's cached maps.
TSector* TMapIterator::NextLoadedSector()
{
    for (; mapnum < MapManager.NumCached(); mapnum++, secnum = 0)
    {
        const TGameMap* map = MapManager.Cached(mapnum);
        if (!map || (level >= 0 && map->Level() != level))
            continue;

        const std::vector<TSector*>& list = map->Sectors();
        while (secnum < list.size())
        {
            TSector* candidate = list[secnum++];
            if (!candidate)
                continue;
            if (flags & CHECK_MAPRECT)
            {
                SRect sr;
                candidate->GetMaxMapRect(sr);
                if (!r.Intersects(sr))
                    continue;
            }
            sx = candidate->SectorX();
            sy = candidate->SectorY();
            return candidate;
        }
    }
    return nullptr;
}

void TMapIterator::Nuke()
{
    if (item)
    {
        MapPane.DeleteObject(item);
        item = nullptr;
    }
}

// ************************
// * Initialize and Close *
// ************************

bool TMapPane::Initialize()
{
  // Initialize per-map 3D scene state (light/animator lists, viewport)
    Scene3D.OnMapLoaded();

    TPane::Initialize(); // Initialize values and create background areas

    int32_t px = MAPPANEX = GetPosX();
    int32_t py = MAPPANEY = GetPosY();
    int32_t pw = MAPPANEWIDTH = GetWidth();
    int32_t ph = MAPPANEHEIGHT = GetHeight();

    oldposx = posx = oldposy = posy = 
    oldsectorx = oldsectory = sectorx = sectory =
    oldscrollx = scrollx = oldscrolly = scrolly = 0x80000000;
    oldlevel = level = 0;

    ClearWindow();

    SColor c;
    c.red = 255;
    c.green = 255;
    c.blue = 255;
    SetLightColor(0, c, 35);

  // Clear mouse click last key
    lastkey = -1;

  // Setup ambient stuff
    SetAmbientLight(10);
    SColor color = {(uint8_t)255, (uint8_t)255, (uint8_t)255};
    SetAmbientColor(color);

  // Initialize Dynamic Light
    dlight.intensity = 0;
    dlight.lightindex = -1;
    dlight.pos.x = dlight.pos.y = dlight.pos.z = 0;

  // Init cash
    TMoney::Initialize();

  // Init exits
    TExit::Initialize();

  // Target finder
    targetcallback = nullptr;
    abortcallback = nullptr;

  // Walkmap draw
    wmrevealx = 0;
    wmrevealy = 0;
    wmrevealsizex = (SECTORHEIGHT >> WALKMAPSHIFT);
    wmrevealsizey = (SECTORWIDTH >> WALKMAPSHIFT);

    dragmode = false;
    clicked = false;

    memset(&center, 0, sizeof(S3DPoint));

  // Make sure update thread is going  
    BeginUpdateThread();

    return true;
}

void TMapPane::Close()
{
  // Make sure update thread is finished
    EndUpdateThread();

  // Retail freed every loaded sector here (FreeAllSectors). The sectors
  // belong to TMapManager in the port; the pane only lets go of its window.
    ClearWindow();

  // Free imagery in imagery system
    TObjectImagery::FreeAllImagery();

  // Death to realtime 3D (per-map state)
    Scene3D.OnMapUnloaded();
 
  // Call base class close function
    TPane::Close(); // Free's background areas
}

// This function gets called when the pane is created, or resized, to set the pane's
// background buffer.  We also put our 3D viewport resize here too.
void TMapPane::CreateBackgroundBuffers()
{
  // Set global map size variables
    MAPPANEX = GetPosX();
    MAPPANEY = GetPosY();
    MAPPANEWIDTH = GetWidth();
    MAPPANEHEIGHT = GetHeight();
    SCROLLBUFWIDTH = MAPPANEWIDTH + UPDATEWIDTH * 2;
    SCROLLBUFHEIGHT = MAPPANEHEIGHT + UPDATEHEIGHT * 2;

  // Set 3D system viewport size
    Scene3D.SetSize(MAPPANEX, MAPPANEY, MAPPANEWIDTH, MAPPANEHEIGHT);

  // Do lit multi for map pane update surface for the update thread
    litmulti = new TMosaicSurface(
            SCROLLBUFWIDTH / MOSAICTILEX,
            SCROLLBUFHEIGHT / MOSAICTILEY,
            MOSAICTILEX,
            MOSAICTILEY,
            MOSAICSURF_SYSTEMMEM | MOSAICSURF_ZBUFFER | MOSAICSURF_ZSYSTEMMEM);
    litmulti->SetClipMode(CLIP_WRAP);

  // Make a clone of the litmulti for the main thread so it has its own origin/clip rect
  // which can be set independently even while the update thread is running.
    updatemulti = new TMosaicSurface(litmulti);
    updatemulti->SetClipMode(CLIP_WRAP);

  // Clone tiles and zbuffer from lit multi, and add 32 bit buffer tiles
//  if (!IsMMX)
//  {
        unlitmulti = new TMosaicSurface(litmulti,
            MOSAICSURF_BMSURFACE | MOSAICSURF_32BIT | MOSAICSURF_CLONEZBUFFER);
        unlitmulti->SetClipMode(CLIP_WRAP);
        zbufferiscloned = true;
//      lightmulti = nullptr;
//  }
//  else
//  {
//      unlitmulti = new TMosaicSurface(litmulti,
//          MOSAICSURF_BMSURFACE | MOSAICSURF_CLONEZBUFFER);
//      unlitmulti->SetClipMode(CLIP_WRAP);
//      zbufferiscloned = true;
//      lightmulti = new TMosaicSurface(lightmulti,
//          MOSAICSURF_BMSURFACE | MOSAICSURF_8BIT);
//  }


  // IMPORTANT NOTE:
  //
  // THE 'litmulti' SURFACE MUST ONLY BE USED IN THE UPDATE THREAD (i.e. DrawUnlit(),
  // DrawLights(), DrawLit(), DrawSelected(), etc.  Conversely, the updatemulti
  // MUST ****NOT**** be used in the update thread.  If these rules are not followed,
  // the program will not work correctly!!!

  // Old CPU-side background caching is not needed with the sokol GPU compositor;
  // UseBackgroundArea was removed from TDisplay during the sokol port.
  //  int32_t bgbuffer = Display.UseBackgroundArea(
  //      GetPosX(), GetPosY(), GetWidth(), GetHeight(), updatemulti);
  //  SetBackgroundBuffer(bgbuffer);

  // Start the update thread going
    BeginUpdateThread();
    Update(); // Cause whole pane to be redrawn
}

void TMapPane::FreeBackgroundBuffers()
{
  // End the update thread
    EndUpdateThread();
    
  // Free background scrolling area for this pane
  // FreeBackgroundArea was removed from TDisplay during the sokol port.
  //  Display.FreeBackgroundArea(GetBackgroundBuffer());
  //  ClearBackgroundBuffer();

  // Kill surfaces
    if (unlitmulti && litmulti && updatemulti)
    {
        delete unlitmulti;
        unlitmulti = nullptr;
        delete litmulti;
        litmulti = nullptr;
        delete updatemulti;
        updatemulti = nullptr;
    }
}

// ******************
// * Input Handling *
// ******************

void TMapPane::SnapWalkDisplay(int32_t mapindex)
{
    if (mapindex < 0)
        return;

    TObjectInstance* inst = GetInstance(mapindex);
    if (inst && inst->GetImagery())
    {
        S3DPoint pos;
        inst->GetPos(pos);
        pos.x >>= WALKMAPSHIFT;
        pos.y >>= WALKMAPSHIFT;
        pos.x %= SECTORWIDTH >> WALKMAPSHIFT;
        pos.y %= SECTORHEIGHT >> WALKMAPSHIFT;

        int32_t x, y, width, length;
        inst->GetFacingBoundBox(x, y, width, length);

        wmrevealx = pos.x - x;
        wmrevealy = pos.y - y;

        wmrevealsizex = wmrevealx + width;
        wmrevealsizey = wmrevealy + length;

        RedrawAll();
    }
}

void TMapPane::KeyPress(int32_t key, bool down)
{
    bool interuptforreals = true;

    if (down && Editor && StatusBar.EditWalkmap())
    {
        switch (key)
        {
          case VK_UP:
            if (CtrlDown)
                wmrevealy--;
            else
                wmrevealsizey--;

            RedrawAll();
            break;
          case VK_DOWN:
            if (CtrlDown)
                wmrevealy++;
            else
                wmrevealsizey++;

            RedrawAll();
            break;
          case VK_LEFT:
            if (CtrlDown)
                wmrevealx--;
            else
                wmrevealsizex--;

            RedrawAll();
            break;
          case VK_RIGHT:
            if (CtrlDown)
                wmrevealx++;
            else
                wmrevealsizex++;

            RedrawAll();
            break;
          case VK_HOME:
            if (CtrlDown)
            {
                wmrevealx = (center.x - ((sectorx + 1) << SECTORWSHIFT)) >> WALKMAPSHIFT;
                wmrevealy = (center.y - ((sectory + 1) << SECTORHSHIFT)) >> WALKMAPSHIFT;
            }
            else
            {
                wmrevealsizex = (center.x - ((sectorx + 1) << SECTORWSHIFT)) >> WALKMAPSHIFT;
                wmrevealsizey = (center.y - ((sectory + 1) << SECTORHSHIFT)) >> WALKMAPSHIFT;
            }

            RedrawAll();
            break;
          case VK_END:
            if (CtrlDown)
            {
                wmrevealx = 0;
                wmrevealy = 0;
                wmrevealsizex = (SECTORHEIGHT >> WALKMAPSHIFT);
                wmrevealsizey = (SECTORWIDTH >> WALKMAPSHIFT);
            }
            else
            {
                SnapWalkDisplay(StatusBar.GetSelectedObj());
            }
            RedrawAll();
            break;
        }
    }

    if (down && key == VK_ESCAPE)
        exit(0);
}

// REVSYNC: FindClickPos @ 0x0044e930 (the 1998 body). From the bottom of the
// view (plus the height of a 255-high walk cell) up to the pointer, the first
// walk cell whose floor, raised to its height, lies under the pointer; else
// the pointer's point at start's height. Map-screen coordinates come from the
// map view (retail's x + posx, y + posy).
void TMapPane::FindClickPos(int32_t x, int32_t y, S3DPoint& start, S3DPoint& target)
{
    int32_t mapx = 0, mapy = 0, vw = 0, vh = 0, bottomx = 0, bottomy = 0;
    if (mapview)
        mapview->GetViewportSize(vw, vh);
    PaneToMapScreen(x, y, mapx, mapy);
    PaneToMapScreen(x, vh - 1, bottomx, bottomy);
    bottomy += 1 + ((255 * 866) / 1000);

    S3DPoint top, bottom;
    ScreenToWorld(mapx, mapy, top);
    ScreenToWorld(mapx, bottomy, bottom);
    top &= (uint32_t)~(WALKMAPGRANULARITY - 1);
    bottom &= (uint32_t)~(WALKMAPGRANULARITY - 1);

    bool found = false;

    for ( ; bottom.y >= top.y && !found; bottom.x -= 16, bottom.y -= 16)
        for (int32_t i = 0; i < 3; i++)
        {
            S3DPoint oldbottom = bottom;
            if (i == 1)
                bottom.y -= 16;
            else if (i == 2)
                bottom.x -= 16;

            int32_t height = GetWalkHeight(bottom);

            S3DPoint walkpos;
            ScreenToWorld(mapx, mapy + ((height * 866) / 1000), walkpos);
            walkpos.x += 8;
            walkpos.y += 8;

            if (walkpos.x >> WALKMAPSHIFT == bottom.x >> WALKMAPSHIFT &&
                walkpos.y >> WALKMAPSHIFT == bottom.y >> WALKMAPSHIFT)
            {
                bottom.z = height;
                found = true;
                break;
            }

            bottom = oldbottom;
        }

    if (!found)
        ScreenToWorld(mapx, mapy, bottom, start.z);

    target = bottom;
}

// ************* Draw order change functions ***************

bool TMapPane::SwapDrawOrder(TObjectInstance* inst0, TObjectInstance* inst1)
{
    TMapIterator i, j;
    for (; i; i++)
        if (i == inst0)
            break;

    for (; j; j++)
        if (j == inst1)
            break;

    if (!i || !j || i.SectorX() != j.SectorX() || i.SectorY() != j.SectorY())
        return false;       // no dice, items are in different sectors

    int32_t pos0 = i.SectorIndex();
    int32_t pos1 = j.SectorIndex();

    TSector* sect = sectors[i.SectorX()][i.SectorY()];

    sect->RemoveObject(pos0);
    sect->RemoveObject(pos1);

    sect->SetObject(inst0, pos1);
    sect->SetObject(inst1, pos0);

    return true;
}

void TMapPane::PushToFront(TObjectInstance* inst)
{
    TMapIterator i;
    for (; i; i++)
        if (i == inst)
            break;

    if (!i)
        return;

    TObjectInstance* top = nullptr;
    for (TMapIterator j; j; j++)
        top = j;

    SwapDrawOrder(inst, top);
}

void TMapPane::PushToBack(TObjectInstance* inst)
{
    TMapIterator i;
    for (; i; i++)
        if (i == inst)
            break;

    if (!i)
        return;

    TMapIterator j;
    SwapDrawOrder(inst, j);
}

// ************** Movement control ******************

char *Directions[] = { "ne", "e", "se", "s", "sw", "w", "nw", "n", "ne" };

// Gets the world position (given character's z pos) that mouse is hovering over
void TMapPane::GetMouseMapPos(S3DPoint &p, int32_t zoffset)
{
    ScreenToMapPos(cursorx, cursory, p, zoffset);
}

// The world point under the screen pixel (x, y) at Locke's height plus
// `zoffset`: the pointer conversion the right-button walk uses
// (UpdateMouseMovement @ 0x0044ee00, + 50).
void TMapPane::ScreenToMapPos(int32_t x, int32_t y, S3DPoint &p, int32_t zoffset)
{
    S3DPoint curpos;
    Player->GetPos(curpos);

    // The map view sits at the screen's top left: the pointer is in its
    // pixels. Retail's x + posx has no bounds: past the view (over the HUD)
    // the point lies on beyond its edge.
    int32_t mapx = 0, mapy = 0;
    (void)PaneToMapScreen(x, y, mapx, mapy);
    ScreenToWorld(mapx, mapy, p, curpos.z + zoffset);
}

// Gets the angle of the mouse from the character's current position
int32_t TMapPane::GetMouseMapAngle()
{
    S3DPoint curpos, target;

    Player->GetPos(curpos);
    GetMouseMapPos(target);

    return ConvertToFacing(curpos, target);
}

// REVSYNC: UpdateMouseMovement @ 0x0044ee00. The point under the pointer at
// Locke's height + 50; within 16 of him (x and y) only the arrow (the held
// direction stays); otherwise the wedge for the angle's 45-degree sector, its
// shadow at (0, 43), and that direction held. Retail pressed the numpad keys
// through the screen; the port holds the joystick direction in the control
// map, the same direction flags UpdateMove reads.
void TMapPane::UpdateMouseMovement(int32_t x, int32_t y)
{
    if (!Player)
        return;

    S3DPoint curpos, target;

    Player->GetPos(curpos);
    GetMouseMapPos(target, 50);

    int32_t angle = ConvertToFacing(curpos, target);

    target -= curpos;
    if (absval(target.x) < 16 && absval(target.y) < 16)
    {
        SetMouseBitmap(GameData->Bitmap("cursor"));
        walking = true;
        return;
    }

    angle = (angle + 0x10) & 0xe0;

  // Get wedge cursor name
    char buf[32];
    snprintf(buf, sizeof(buf), "wedge-%s", Directions[angle >> 5]);
    SetMouseBitmap(GameData->Bitmap(buf));
    strcat(buf, "shadow");
    SetMouseShadow(GameData->Bitmap(buf), 0, 43);
    SetMouseCornerBitmap((PTBitmap)nullptr, true);

  // Get movement command
    static constexpr int32_t kMoveKeys[8] =
        { VK_JOYUPRIGHT, VK_JOYRIGHT, VK_JOYDOWNRIGHT, VK_JOYDOWN,
          VK_JOYDOWNLEFT, VK_JOYLEFT, VK_JOYUPLEFT, VK_JOYUP };
    HoldMoveKey(kMoveKeys[angle >> 5]);

    walking = true;
}

// The right-button walk's direction: `key` held in the control map (-1:
// none), the previous one let go.
void TMapPane::HoldMoveKey(int32_t key)
{
    if (key == lastkey)
        return;
    const uint32_t modes = PlayScreen.ControlModeMask();
    if (lastkey != -1)
        ControlMap.GetCommand(lastkey, false, modes);
    lastkey = key;
    if (lastkey != -1)
        ControlMap.GetCommand(lastkey, true, modes);
}

void TMapPane::MouseClick(int32_t button, int32_t x, int32_t y)
{
    if (Editor)
    {
        if (button == MB_LEFTDOWN && InPane(x, y))
        {
            if (dragmode)
            {
                StatusBar.StopMoving();
            }
            else if (StatusBar.EditWalkmap())
            {
                for (int32_t sy = SECTORWINDOWY - 1; sy >= 0; sy--)
                    for (int32_t sx = SECTORWINDOWX - 1; sx >= 0; sx--)
                        if (sectors[sx][sy])
                        {
                            for (int32_t gy = min(wmrevealsizey, (SECTORHEIGHT >> WALKMAPSHIFT)) - 1; gy >= wmrevealy; gy--)
                                for (int32_t gx = min(wmrevealsizex, (SECTORWIDTH >> WALKMAPSHIFT)) - 1; gx >= wmrevealx; gx--)
                                {
                                    S3DPoint pos, screenpos, wpos;
                                    pos.x = gx;
                                    pos.y = gy;
                                    wpos.x = ((sectorx + sx) << SECTORWSHIFT) + (gx << WALKMAPSHIFT);
                                    wpos.y = ((sectory + sy) << SECTORHSHIFT) + (gy << WALKMAPSHIFT);
                                    wpos.z = sectors[sx][sy]->ReturnWalkmap(pos.x, pos.y);

                                    int32_t scrx, scry;
                                    WorldToScreen(wpos, scrx, scry);

                                    // wacky formula to get diamond shape
                                    int32_t dx = (scrx - (x + posx)) / 2;
                                    int32_t dy = scry - (y + posy);
                                    if ((absval(dx) + absval(dy)) < 8)
                                    {
                                        // hang onto the data for dragging
                                        grabx = sectorx + sx;
                                        graby = sectory + sy;
                                        objx = gx;
                                        objy = gy;
                                        onobject = wpos.z;
                                        oldz = y + posy;

                                        SRect ir;
                                        ir.left = scrx - 20;
                                        ir.top = scry - 20;
                                        ir.right = scrx + 20;
                                        ir.bottom = scry + 20;
                                        AddBgUpdateRect(ir, BGDRAW_LIT);
                                        return;
                                    }
                                }
                        }

                grabx = graby = objx = objy = oldz = onobject = -1;
            }
            else
            {
                TObjectInstance* oi = OnObject(x, y);
                if (oi)
                {
                    S3DPoint pos;
                    oi->GetPos(pos);
                    WorldToScreen(pos, objx, objy);
                    grabx = posx - objx;
                    graby = posy - objy;
                    objx = x + grabx;
                    objy = y + graby;
                    oldz = pos.z;
                    StatusBar.Select(oi->GetMapIndex(), CtrlDown);
                }
            }
        }
        if (button == MB_MIDDLEDOWN && InPane(x, y))
        {
            TObjectInstance* oi = OnObject(x, y);
            if (oi)
                StatusBar.Select(oi->GetMapIndex());
            Console.Input("follow\n");
        }
        else if (button == MB_LEFTUP)
        {
            if (targetcallback)
            {
                S3DPoint pos;
                ScreenToWorld(x+posx-objx, y+posy-objy, pos, center.z);

                if (StatusBar.GridSnap())
                {
                    pos.x = pos.x & GRIDMASK;
                    pos.y = pos.y & GRIDMASK;
                    // z is kind of a special case, so just use the exact value
                    //pos.z = pos.z & GRIDMASK;
                }

                (*targetcallback)(pos);
                if (mode == MODE_MOVE)
                {
                    dragmode = false;
                    targetcallback = nullptr;
                    SetMouseBitmap(PointerCursor);
                }
            }
            else if (StatusBar.EditWalkmap())
            {
                if (objx >= 0 && objy >= 0)
                {
                    S3DPoint wpos, pos;
                    pos.x = objx;
                    pos.y = objy;
                    wpos.x = (grabx << SECTORWSHIFT) + (objx << WALKMAPSHIFT);
                    wpos.y = (graby << SECTORHSHIFT) + (objy << WALKMAPSHIFT);
                    wpos.z = sectors[grabx - sectorx][graby - sectory]->ReturnWalkmap(pos.x, pos.y);
                    int32_t scrx, scry;
                    WorldToScreen(wpos, scrx, scry);
                    SRect ir;
                    ir.left = scrx - 20;
                    ir.top = scry - 20;
                    ir.right = scrx + 20;
                    ir.bottom = scry + 20;
                    AddBgUpdateRect(ir);
                    grabx = graby = objx = objy = oldz = onobject = -1;
                }
            }
            else
                StatusBar.StopMoving();
        }
        else if (button == MB_RIGHTDOWN)
        {
            if (targetcallback)
            {
                if (abortcallback)
                    (*abortcallback)();

                targetcallback = nullptr;
                SetMouseBitmap(PointerCursor);
                //PlayScreen.ReleaseExclusivePane(&MapPane);
                dragmode = false;
            }
            else
            {
                if (mousebutton == MB_LEFTDOWN)
                    StatusBar.Undo();       // command was aborted, move everything back

                while (StatusBar.Deselect());
                dragmode = false;
            }
        }
    }
    else        // if not in the editor (normal gameplay)
        PlayMouseClick(button, x, y);
}

// REVSYNC: MouseClick @ 0x0044f140, outside the editor
// (docs/gameflow/forensics/MAP_INPUT.md §4). Retail first left a point over
// the side tabs' controls to them; the play screen's routing does that in
// the port (TPlayScreen::ClickRoute). It hands the map every release, as
// retail's screen did, wherever it lands: the right one ends the walk
// anywhere, a left one off the view only forgets the press. A right press
// with Ctrl held shows what is under the pointer on the stats pane instead of
// walking (0x0044f87d..0x0044f8e3); a left press on an item binds the stats
// pane to it, quietly (0x0044fb74).
void TMapPane::PlayMouseClick(int32_t button, int32_t x, int32_t y)
{
    if (playMouseClickSeam)
    {
        playMouseClickSeam(button, x, y);
        return;
    }
    Notify(N_CANCELCONTROL, Player);

    int32_t mapx = 0, mapy = 0;
    const bool inview = PaneToMapScreen(x, y, mapx, mapy);
    // Combat clicks attack unless the pointer shows something else to do.
    const bool attackcursor = hovercursor == CURSOR_NONE || hovercursor == CURSOR_SWORDS;

    switch (button)
    {
      case MB_RIGHTDOWN:
        if (!Player)
            return;
        if (CtrlDown)
        {
            TObjectInstance* const on = OnObject(x, y);
            switch (on ? on->ObjClass() : OBJCLASS_TILE)
            {
              case OBJCLASS_TILE: case OBJCLASS_EFFECT: case OBJCLASS_EXIT:
              case OBJCLASS_HELPER: case OBJCLASS_SHADOW:
                break;
              default:
                PlayScreen.ShowStatPane();
                StatPane.SetObject(on);
                break;
            }
            return;
        }
        UpdateMouseMovement(x, y);
        return;

      case MB_RIGHTUP:
        if (Player && walking)
        {
            SetMouseBitmap(GameData->Bitmap("cursor"));
            HoldMoveKey(-1);
            walking = false;
        }
        return;

      case MB_LEFTDOWN:
      {
        if (!Player || !inview)
            return;

        if (Player->IsCombat() && attackcursor)
        {
            // Another character Locke may fight becomes his target;
            // otherwise the press is a random swing.
            TObjectInstance* on = OnObject(x, y);
            if (on && on->ObjClass() == OBJCLASS_CHARACTER && on != Player->Fighting())
            {
                if (Player->IsEnemy((TCharacter*)on))
                    Player->SetFighting((TCharacter*)on);
                return;
            }
            Player->ButtonAttack(random(1, 3));
            return;
        }

        if (Player->IsBowMode() && attackcursor)
        {
            if (!Player->IsBowDrawn())
            {
                Player->DrawBow();
                Player->AimBow(GetMouseMapAngle());
            }
            clicked = true;
            return;
        }

        // The press only remembers what it was over (an item also goes to
        // the stats pane); the release acts.
        TObjectInstance* on = OnObject(x, y);
        onobject = on ? on->GetMapIndex() : -1;
        clicked = true;
        if (on && on->IsInventoryItem())
            StatPane.SetObject(on);
        return;
      }

      case MB_LEFTUP:
        if (Player && !Player->IsDead() && inview)
            PlayMouseRelease(x, y, attackcursor);
        clicked = false;
        return;
    }
}

// The left button's release (MAP_INPUT.md §4.3): on the object pressed, Use
// it, or pick it up if it is an item, when it is within 96 of Locke or is a
// character; a farther one is walked to. A release that wasn't a press on the
// map lets go of an item held from the inventory, the belt or the equipment:
// used on the object under it, else dropped there (INVENTORY_ACTIONS.md §11).
void TMapPane::PlayMouseRelease(int32_t x, int32_t y, bool attackcursor)
{
    if (Player->IsCombat() && attackcursor && clicked)
        return;
    if (Player->IsBowMode() && Player->IsBowDrawn() && clicked)
    {
        Player->ShootBow(GetMouseMapAngle());
        return;
    }

    // REVSYNC: 0x0044fce3 -- the item the inventory, the belt or the doll
    // holds (retail read their held slots; the port's grab).
    TObjectInstance* inst = clicked ? nullptr : UIDragState::HeldItem();

    TObjectInstance* on = OnObject(x, y, inst);
    const int32_t objindex = on ? on->GetMapIndex() : -1;
    if (on)
    {
        S3DPoint op, pp;
        on->GetPos(op);
        Player->GetPos(pp);
        log_info("[mapinput] release (%d, %d) on %s at (%d,%d,%d), Locke at (%d,%d,%d), distance %d, pressed on %d",
                 x, y, on->GetName(), op.x, op.y, op.z, pp.x, pp.y, pp.z, on->Distance(Player), onobject);
    }
    else
        log_info("[mapinput] release (%d, %d) on nothing, pressed on %d", x, y, onobject);

    if (clicked)
    {
        // Door clicks use U's forward reach, not the general pickup radius.
        if (on && on->ObjClass() == OBJCLASS_EXIT && onobject == objindex)
        {
            if (Player->FindUseTarget(on) == on)
                on->Use(Player, -1);
            else
                TextBar.Print("%s", DialogList.GetLine("USENOTHING"));
            return;
        }
        constexpr int32_t kReach = 96;      // retail: Distance < 0x61
        if (on && on->Distance(Player) > kReach && on->ObjClass() != OBJCLASS_CHARACTER)
        {
            WalkToward(x, y, on);
            return;
        }
        if (on && onobject == objindex)
        {
            if (!on->IsInventoryItem())
                on->Use(Player, -1);
            else if (on->ObjClass() != OBJCLASS_PLAYER && on->ObjClass() != OBJCLASS_CHARACTER)
            {
                // Gold or food may merge into a pile and be deleted
                const TSafeRef<TObjectInstance> taken(on);
                Player->Pickup(on);
                TakenObject = taken.Get();
            }
        }
        if (!on && onobject == -1 && clickwalk && !Player->IsCombat() && !Player->IsBowMode())
            ClickWalkTo(x, y);
        return;
    }

    // REVSYNC: 0x0044fff8..0x004500a6 -- a held item let go here: used on
    // the object under it (a character's script gets it, a container takes
    // it, ...), else dropped at the pointer.
    if (!inst)
        return;
    // A key or other carried item must not bypass that reach check.
    if (on && on->ObjClass() == OBJCLASS_EXIT && Player->FindUseTarget(on) != on)
    {
        TextBar.Print("%s", DialogList.GetLine("USENOTHING"));
        return;
    }
    if (on && on->Use(Player, inst->GetMapIndex()))
        return;
    DropHeldItem(inst, x, y);
}

// REVSYNC: 0x004500ab..0x0045046a -- a held item onto the floor: not one that
// can't change hands (no Value) nor ammunition, silently; a worn one comes
// off first. The floor under the pointer (FindClickPos), moved (64, 64) but
// for money; farther than 96 from Locke it lands on the line toward it, as
// far as walk checks from Locke at 0, 32 and 64 allow, at his height. Then
// "<name> dropped.", DroppedObject, and a fall from 64 above (a still toss)
// but for armor and money. No sound: the item's played when it was picked up.
// Deviation: retail's pile line used a PLURAL line no language ships ("3
// Bread[PLURAL] dropped."); the port uses "s" when it is missing.
void TMapPane::DropHeldItem(TObjectInstance* item, int32_t x, int32_t y)
{
    if (!item->CanTrade() || item->ObjClass() == OBJCLASS_AMMO)
        return;
    const int32_t worn = item->InventNum() - kInvSlotEquipFirst;
    if ((uint32_t)worn < NUM_EQ_SLOTS)
        Player->Equip(nullptr, worn);

    S3DPoint start, target;
    Player->GetPos(start);
    FindClickPos(x, y, start, target);
    if (item->ObjClass() != OBJCLASS_MONEY)
    {
        target.x += 64;
        target.y += 64;
    }
    constexpr int32_t kDropReach = 96;
    const double dx = target.x - start.x, dy = target.y - start.y;
    if (int32_t(sqrt(dx * dx + dy * dy)) > kDropReach)
    {
        const double angle = atan2(dy, dx);
        const float stepx = float(cos(angle) * 32.0);
        const float stepy = float(sin(angle) * 32.0);
        float px = float(start.x), py = float(start.y);
        for (int32_t step = 0; step < kDropReach / 32; ++step)
        {
            int32_t maxdelta = 0, height = 0;
            bool hole = false;
            GetWalkHeightRadius(S3DPoint(int32_t(px), int32_t(py), start.z), 32, maxdelta, height, hole);
            if (abs(maxdelta) > 32 || hole)
                break;
            px += stepx;
            py += stepy;
        }
        target = S3DPoint(int32_t(px), int32_t(py), start.z);
    }

    item->RemoveFromInventory();
    item->SetPos(target, Player->GetLevel());
    item->AddToMap();

    const std::string name = DialogList.DisplayName(item->GetName());
    const int32_t amount = item->Amount();
    char line[256];
    if (amount > 1)
    {
        if (DialogList.FindLine("FULLMULTDROP") >= 0)
            snprintf(line, sizeof(line), DialogList.GetLine("FULLMULTDROP"), amount, name.c_str());
        else if (DialogList.FindLine("FULLMULTDROPREV") >= 0)
            snprintf(line, sizeof(line), DialogList.GetLine("FULLMULTDROPREV"), name.c_str(), amount);
        else
            snprintf(line, sizeof(line), "%d %s%s %s", amount, name.c_str(),
                     DialogList.FindLine("PLURAL") >= 0 ? DialogList.GetLine("PLURAL") : "s",
                     DialogList.GetLine("BASEDROPEPED"));
    }
    else if (DialogList.FindLine("FULLSINGDROP") >= 0)
        snprintf(line, sizeof(line), DialogList.GetLine("FULLSINGDROP"), name.c_str());
    else
        snprintf(line, sizeof(line), "%s %s", name.c_str(), DialogList.GetLine("BASEDROPEPED"));
    TextBar.Print("%s", line);
    log_info("[mapinput] dropped %s at (%d,%d,%d)", item->GetName(), target.x, target.y, target.z);

    DroppedObject = item;

    if (item->NeedsAnimator())
        item->CreateAnimator();
    TObjectAnimator* const animator = item->GetAnimator();
    if (animator && stricmp(item->GetClassName(), "Armor") != 0 && item->ObjClass() != OBJCLASS_MONEY)
    {
        S3DPoint from = target;
        from.z += 64;
        animator->Toss(from, target, -1, true);
    }
}

// Click to walk (docs/playtest/PLAN.md §10), a patch both games carry for the
// playtest (retail's named build "clickwalk" patches this release path of
// MouseClick @ 0x0044f140; CLICKWALK.md §9): a press and release on open
// ground -- nothing under either, Locke's root neither combat nor the bow,
// as retail's gates read it -- sends Locke to the point under the release
// by the right-button walk's conversion (UpdateMouseMovement @ 0x0044ee00:
// the release's own x, y at Locke's height + 50), through Goto (0x004cedb0),
// which turns to any of 256 angles and stops on the point. The walk mode is
// the root's (the run key held: running).
void TMapPane::ClickWalkTo(int32_t x, int32_t y)
{
    S3DPoint target;
    ScreenToMapPos(x, y, target, 50);
    const bool going = Player->Goto(target.x, target.y, nullptr);
    log_info("[mapinput] click walk (%d, %d) to (%d,%d): %s", x, y, target.x, target.y,
             going ? "going" : "refused");
}

// Retail 0x0044fd98..0x0044ff40: an object out of reach. Locke walks to the
// floor point under the pointer less 64 toward him (two steps of 32 along the
// line) when the line from him is walkable every 32 units for (distance - 32)
// / 32 steps; else "too far". The walk carries the object, which its arrival
// picks up when it is an item (TCharacter::Goto); a door takes a second click.
// The walk test is GetWalkHeightRadius within 32 (0x0044fec1): a step of
// more than 32 between neighbouring cells, or a cell with no walkmap, ends it.
void TMapPane::WalkToward(int32_t x, int32_t y, TObjectInstance* on)
{
    S3DPoint start, target;
    Player->GetPos(start);
    FindClickPos(x, y, start, target);

    const double angle = atan2(double(target.y - start.y), double(target.x - start.x));
    const float stepx = float(cos(angle) * 32.0);
    const float stepy = float(sin(angle) * 32.0);
    target.x += int32_t(stepx * -2.0f);
    target.y += int32_t(stepy * -2.0f);

    const int32_t steps = (on->Distance(Player) - 32) / 32;
    float px = float(start.x), py = float(start.y);
    int32_t step = 0;
    for (; step < steps; ++step)
    {
        S3DPoint p(int32_t(px), int32_t(py), start.z);
        int32_t maxdelta = 0, height = 0;
        bool hole = false;
        GetWalkHeightRadius(p, 32, maxdelta, height, hole);
        if (maxdelta > 32 || hole)
            break;
        px += stepx;
        py += stepy;
    }

    if (step == steps)
        Player->Goto(target.x, target.y, on);
    else
        TextBar.Print(TTextBar::ELineType::Notice, "%s", DialogList.GetLine("ITEMTOFAR"));
}

#define MOUSESCROLLSPEED    8
#define MOUSEBOUNDARY       1

void TMapPane::MouseMove(int32_t button, int32_t x, int32_t y)
{
    if (Editor)
    {
        // mouse scrolling on screen edges
        mx = my = 0;

        if (cursorx < MOUSEBOUNDARY)
            mx = -MOUSESCROLLSPEED;
        else if (cursorx >= (Display.Width() - MOUSEBOUNDARY))
            mx = MOUSESCROLLSPEED;

        if (cursory < MOUSEBOUNDARY)
            my = -MOUSESCROLLSPEED;
        else if (cursory >= (Display.Height() - MOUSEBOUNDARY))
            my = MOUSESCROLLSPEED;

        if (InPane(x, y) &&
            ((button == MB_LEFTDOWN && !dragmode) || (dragmode && button != MB_LEFTDOWN)))
        {
            if (StatusBar.EditWalkmap() && !dragmode)
            {
                if (objx >= 0 && objy >= 0)
                {
                    S3DPoint pos;
                    pos.x = objx;
                    pos.y = objy;
                    pos.z = min(255, max(0, (((oldz - (y + posy)) * 1000) / 866) + onobject));
                    sectors[grabx - sectorx][graby - sectory]->SetWalkmap(pos.x, pos.y, pos.z);

                    SRect ir;
                    ir.left = x + GetScrollX() - 32;
                    ir.top = y + GetScrollY() - 32;
                    ir.right = ir.left + 63;
                    ir.bottom = ir.top + 63;
                    AddBgUpdateRect(ir);
                }
            }
            else
            {
                if (ShiftDown)
                {
                    TObjectInstance* inst = OnObject(x, y);
                    if (inst)
                        StatusBar.Select(inst->GetMapIndex(), true);
                }
                else
                {
                    TObjectInstance* oi = GetInstance(StatusBar.GetSelectedObj());
                    if (oi && !(oi->GetFlags() & OF_EDITORLOCK))
                    {
                        if (!(oi->GetFlags() & OF_SELDRAW))
                        {
                            // avoid accidental dragging
                            if (!dragmode &&
                                (absval((posx + x) - grabx) + absval((posy + y) - graby)) <= 3)
                                return;

                            StatusBar.StartMoving();
                        }
                        
                        S3DPoint oldpos;
                        oi->GetPos(oldpos);

                        MapPane.MoveObjScreen(StatusBar.GetSelectedObj(), x - objx, y - objy);

                        S3DPoint newpos;
                        oi->GetPos(newpos);

                        // handle z moving here, since it is relative to the object, not the map
                        if (!StatusBar.RestrictZ())
                        {
                            newpos.z = oldz - (y - (objy - graby));
                            if (StatusBar.GridSnap())
                                newpos.z &= GRIDMASK;
                            oi->SetPos(newpos);
                        }

                        // move other selected objects relative to this one
                        S3DPoint delta = newpos;
                        delta -= oldpos;

                        for (int32_t i = StatusBar.GetFirstObj(); i >= 0; i = StatusBar.GetNextObj())
                        {
                            TObjectInstance* inst = GetInstance(i);
                            if (!inst || inst == oi)
                                continue;

                            S3DPoint pos;
                            inst->GetPos(pos);
                            pos += delta;
                            inst->SetPos(pos);
                        }
                    }
                }
            }
        }
    }
    else        // REVSYNC: MouseMove @ 0x004504d0, outside the editor
    {
        if (button == MB_RIGHTDOWN && walking)
            UpdateMouseMovement(x, y);

      // Aim the bow if we have it drawn
        if (Player && Player->IsBowDrawn())
        {
            int32_t angle = GetMouseMapAngle();
            Player->AimBow(angle);
        }
    }
}

// utility function for dragging objects with the mouse
void TMapPane::MoveObjScreen(int32_t index, int32_t x, int32_t y)
{
    if (index < 0)
        return;

    TObjectInstance* oi = GetInstance(index);
    if (oi)
    {
        S3DPoint newpos;
        oi->GetPos(newpos);
/*
        TObjectInstance* overinst = GetInstance(OnObject(x, y));
        if (overinst)
        {
            S3DPoint opos;
            overinst->GetPos(opos);
            ScreenToWorld(x + posx, y + posy + opos.z, newpos);
            newpos.z = opos.z;
        }
        else*/
        {
            S3DPoint tmppos = newpos;
            ScreenToWorld(x + posx, y + posy, newpos, tmppos.z);

            if (StatusBar.GridSnap())
            {
                newpos.x = newpos.x & GRIDMASK;
                newpos.y = newpos.y & GRIDMASK;
            }
            if (StatusBar.RestrictX())
                newpos.x = tmppos.x;
            if (StatusBar.RestrictY())
                newpos.y = tmppos.y;
        }

        oi->SetPos(newpos);
        StatusBar.SetDirty(true);
    }
}

// *********************
// * Pane Manipulation *
// *********************

// Set Map Pos
void TMapPane::SetMapPos(S3DPoint& newpos)
{
    int32_t nposx, nposy;

  // Set new pane scroll pos
    WorldToScreen(newpos, nposx, nposy);
    SetScrollPos(nposx - (MAPPANEWIDTH/2), nposy - (MAPPANEHEIGHT/2));
    center = newpos;
}

// ********************
// * Object Functions *
// ********************

// Create object
int32_t TMapPane::NewObject(SObjectDef* def, int32_t imagery)
{
    if (newObjectSeam)
        return newObjectSeam(def, imagery);
    TObjectClass* oc = TObjectClass::GetClass(def->objclass);
    if (!oc)
    {
        fprintf(stderr, "MAPPANE: Bad class in NewObject");
        return -1;
    }
    TObjectInstance* oi = oc->NewObject(def, imagery);
    if (!oi)
    {
        fprintf(stderr, "MAPPANE: Unable to create obj in NewObject");
        return -1;
    }

    int32_t shadowindex = AddShadow(oi);

    oi->SetMapIndex(-1);
    int32_t index = AddObject(oi);

    // REVSYNC: the failure path of TMapPane::NewObject @ 0x00450e40 deletes
    // the shadow it made (once; the 1998 code deleted it a second time).
    if (index < 0)
    {
        if (shadowindex >= 0)
            DeleteObject(GetInstance(shadowindex));
        // REVSYNC-DIVERGENCE: retail leaked the object it couldn't place;
        // nothing else refers to it, so it is freed here.
        delete oi;
    }

    return index;
}

// Add object
int32_t TMapPane::AddObject(TObjectInstance* oi)
{
    if (addObjectSeam)
        return addObjectSeam(oi);
    if (oi->GetSector() != nullptr)    // Object is already in the map
    {
        fprintf(stderr, "MAPPANE: Object already in a sector in AddObject");
        return -1;
    }

    if (oi->GetLevel() != GetMapLevel())
    {
        fprintf(stderr, "MAPPANE: Invalid level in AddObject");
        return -1;
    }

    S3DPoint pos;
    oi->GetPos(pos);

    int32_t sx = (pos.x >> SECTORWSHIFT);
    int32_t sy = (pos.y >> SECTORHSHIFT);

    const TGameMap* active = MapManager.CurrentMap();
    const bool transient = active && active->IsTransient();
    if ((!transient && ((uint32_t)sx >= MAXSECTORX || (uint32_t)sy >= MAXSECTORY)) ||
        sx < sectorx || sx >= sectorx + SECTORWINDOWX ||
        sy < sectory || sy >= sectory + SECTORWINDOWY)
    {
        fprintf(stderr, "MAPPANE: Object not in current map area in AddObject");
        return -1;
    }

    TSector* sect = sectors[sx - sectorx][sy - sectory];
    if (!sect)
    {
        fprintf(stderr, "MAPPANE: Sector unavailable in AddObject");
        return -1;
    }

    LOCKSECTORS;       // Lock sector system so update won't access it
                       // (MAKE SURE UNLOCK IS ALWAYS CALLED.. THERE MUST BE NO RETURN 
                       // BETWEEN THESE TWO FUNCTIONS!!)

    sect->AddObject(oi);

    UNLOCKSECTORS;      // If this isn't called, system will lock up

    if (oi->GetMapIndex() < 0)
        oi->SetMapIndex(MakeIndex());

    if (oi->InventNum() < 0)
    {
        TransferWalkmap(oi);
        AddObjectUpdateRect(oi->GetMapIndex());
    }

    return oi->GetMapIndex();
}

// REVSYNC: TMapPane::RemoveObject @ 0x00451610 -- take an object out of the
// world (the main place objects are removed): its shadow, its animator, then
// out of its owner's inventory or out of its sector. Retail works from the
// object's own links, so this finds objects outside the sector window and
// in sectors no window holds (the 1998 version searched the window by
// mapindex). The object is not deleted.
TObjectInstance* TMapPane::RemoveObject(TObjectInstance* inst)
{
    if (!inst)
        return nullptr;

  // Notify that object is being deleted. Kept where the 1998 code had it;
  // retail notified from the object's detach (0x0046e630) instead.
    Notify(N_DELETINGOBJECT, inst);

    if (inst->GetShadow() >= 0)
        RemoveObject(GetInstance(inst->GetShadow()));

    if (inst->HasAnimator())
        inst->FreeAnimator();

    // Retail also sent a multiplayer "object removed" message (0x00584270)
    // for objects in the map; the port is single player.
    if (inst->IsInInventory() || inst->GetOwner())
        inst->RemoveFromInventory();
    else if (inst->GetSector())
        RemoveFromSector(inst);

    return inst;
}

// Removes and deletes an object
void TMapPane::DeleteObject(TObjectInstance* obj)
{
    RemoveObject(obj);
    delete obj;
}

// Causes object to be put into new OBJSET_xxx lists when flags change in game
void TMapPane::ObjectFlagsChanged(TObjectInstance* oi, uint32_t oldflags, uint32_t newflags)
{
    if (!oi->GetSector())
        return;

    oi->GetSector()->ObjectFlagsChanged(oi, oldflags, newflags);
}

TObjectInstance* TMapPane::RemoveFromSector(TObjectInstance* inst)
{
    TSector* sector = inst->GetSector();
    if (!sector)
        return inst;

    // extract from sector
    ExtractWalkmap(inst);

    LOCKSECTORS;        // Prevent update system from accessing sector stuff
                        // Group the update rect and the actual removal below so update system doesn't
                        // accidently redraw this object before it is deleted

    AddObjectUpdateRect(inst->GetMapIndex());
    sector->RemoveObject(inst);

    UNLOCKSECTORS;

    return inst;
}

// Add the shadow (if any) associated with an object
int32_t TMapPane::AddShadow(TObjectInstance* oi)
{
    extern TObjectClass ShadowClass;
    static int32_t s_shadow_log_count = 0;

    if (oi->ObjClass() == OBJCLASS_SHADOW)
    {
        oi->SetShadow(-1);
        return -1;
    }

    SObjectDef def;
    memset(&def, 0, sizeof(SObjectDef));

    char buf[80];
    sprintf(buf, "shadow_%s", oi->GetTypeName());

    def.objclass = OBJCLASS_SHADOW;
    int32_t objtype = ShadowClass.FindObjType(buf);
    if (s_shadow_log_count < 32)
    {
        log_info("[shadow] request class='%s' type='%s' shadow_type='%s' found=%d",
                 oi->GetClassName(), oi->GetTypeName(), buf, objtype);
        ++s_shadow_log_count;
    }
    if (objtype < 0)
        return -1;

    def.objtype = objtype;
    oi->GetPos(def.pos);

    int32_t index = NewObject(&def);
    if (index >= 0)
        oi->SetShadow(index);

    return index;
}

// REVSYNC: FindObject @ 0x00451d70 -- the occurance'th object of that name
// in the loaded sectors of any level (its iterator flags 0x2a0: loaded,
// active or not), not only the pane's 3x3 window. beginfighting and
// giveweapons look their objects up this way.
TObjectInstance* TMapPane::FindObject(const char *name, int32_t occurance, int32_t objset)
{
    int32_t found = 0;

    for (TMapIterator i(-1, nullptr, CHECK_NOINVENT, objset); i; i++)
    {
        const char *instname = i->GetName();

        if (!stricmp(name, instname) && ++found == occurance)
            return i;
    }

    return nullptr;
}

// Retail drops the spoken lines and any open choices when the camera stops
// following the player, and closes the play screen's drawer when it holds the
// shop (SetCameraObject 0x004538d0, SetCameraPos 0x00453940).
bool TMapPane::IsFollowingPlayer() const
{
    return (centeron.flags & CENTERON_OBJ) && Player && centeron.obj == Player;
}

// REVSYNC: the camera half of a teleport (TExit::Activate 0x0050d3a0, `pos`
// 0x00423d40): flag 8, which UpdateMapPos clears after jumping.
void TMapPane::SnapIfFollowing(const TObjectInstance* obj)
{
    if ((centeron.flags & CENTERON_OBJ) && centeron.obj == obj)
        centeron.flags |= CENTERON_SNAP;
}

void TMapPane::StopFollowingPlayer()
{
    DialogPane.ClearSpeech(false);
    if (PlayScreen.Drawer() == TPlayScreen::EDrawer::BuySell)
        PlayScreen.CloseDrawer();
}

void TMapPane::CenterOnObj(TObjectInstance* obj, uint32_t flags)
{
    if (IsFollowingPlayer() && obj != Player)
        StopFollowingPlayer();
    centeron.obj = obj;
    centeron.flags = (flags & ~CENTERON_POS) | CENTERON_OBJ;
}

void TMapPane::CenterOnPos(const S3DPoint& pos, int32_t level, uint32_t flags)
{
    if (IsFollowingPlayer())
        StopFollowingPlayer();
    centeron.pos = pos;
    centeron.level = level;
    centeron.flags = (flags & ~CENTERON_OBJ) | CENTERON_POS;
}

// REVSYNC: 0x00451de0 -- the nearest object of that name on `lvl` within
// sqrt(0x800000) (~2896 units) of frompos, from the level's loaded sectors
// (its iterator flags 0x4a0), not only the pane's 3x3 window: a script
// near the window's edge finds its waypoints (Hruthford's `goto HRUWAY01`
// with the camera elsewhere on the level). Only the sectors within that
// reach are walked. Names match exactly (ignoring case) unless `partial`
// asks for an abbreviation match; the 1998 source had the two branches the
// other way round, and retail's callers pass `partial` for the abbreviated
// forms (`get`, `select`, `swap`).
TObjectInstance* TMapPane::FindClosestObject(const char *name, const S3DPoint& frompos, int32_t lvl, bool partial, int32_t objset)
{
    constexpr int32_t kMaxSqrDist = 0x800000;
    constexpr int32_t kReach      = 2897;       // ceil(sqrt(kMaxSqrDist))
    SRect area;
    area.left   = frompos.x - kReach;
    area.top    = frompos.y - kReach;
    area.right  = frompos.x + kReach;
    area.bottom = frompos.y + kReach;

    TObjectInstance* closest = nullptr;
    int32_t closestdist = kMaxSqrDist;

    for (TMapIterator i(lvl, &area, CHECK_NOINVENT, objset); i; i++)
    {
        const char *instname = i->GetName();
        if (!instname)
            continue;

        const bool found = partial ? abbrevcmp(name, instname) > 0
                                   : stricmp(name, instname) == 0;
        if (found)
        {
            S3DPoint pos;
            i->GetPos(pos);

            if (SQRDIST(pos, frompos) < closestdist)
            {
                closest = i;
                closestdist = SQRDIST(pos, frompos);
            }
        }
    }

    return closest;
}

// REVSYNC: FindClosestObject @ 0x00451fe0 -- from the object's position and
// level, else the camera's.
TObjectInstance* TMapPane::FindClosestObject(const char *name, TObjectInstance* from, bool partial, int32_t objset)
{
    const S3DPoint frompos = from ? from->Pos() : center;
    return FindClosestObject(name, frompos, from ? from->GetLevel() : level, partial, objset);
}

namespace
{
// A square in 32 bits, wrapping as retail's imul does (sums of them wrap
// the same way in uint32_t).
constexpr uint32_t Square32(int32_t v)
{
    return static_cast<uint32_t>(v) * static_cast<uint32_t>(v);
}
}  // namespace

// REVSYNC: TMapPane::FindObjectsInRange @ 0x00452060 (MAP_QUERIES.md §2.1)
// -- an insertion sort by distance: a newcomer goes before the first entry
// not nearer than it, so between equals the later found comes first, and a
// full list drops its farthest. The distances and the ids move together;
// the 1998 code moved bytes of the one and never the other (with two in
// range, Use found "nothing to use" beside a chest).
int32_t TMapPane::FindObjectsInRange(const S3DPoint& pos, int32_t lvl, int32_t* array, int32_t width,
    int32_t height, int32_t objclass, int32_t maxnum, int32_t objset)
{
    if (findObjectsSeam)
        return findObjectsSeam(pos, array, width, height, objclass, maxnum, objset);
    if (maxnum < 1)
        return 0;
    if (lvl < 0)
        lvl = level;

    const bool sphere = height < 1;
    SRect rect;
    if (sphere)
    {
        rect.left = pos.x - width;
        rect.top = pos.y - width;
        rect.right = rect.left + 2 * width - 1;
        rect.bottom = rect.top + 2 * width - 1;
    }
    else
    {
        rect.left = pos.x - (width >> 1);
        rect.top = pos.y - (height >> 1);
        rect.right = rect.left + width - 1;
        rect.bottom = rect.top + height - 1;
    }
    const int32_t sqrwidth = static_cast<int32_t>(Square32(width));
    // Retail takes the radius off only in a search for characters or
    // players by class: (Radius * 0.4f) squared on the x87, truncated.
    const bool radii = objclass == OBJCLASS_CHARACTER || objclass == OBJCLASS_PLAYER;
    constexpr double kRadiusScale = 0.4f;

    // The distances found so far: on the stack up to 64, as retail's.
    std::array<int32_t, 64> local{};
    std::vector<int32_t> heap;
    int32_t* dist = local.data();
    if (maxnum > static_cast<int32_t>(local.size()))
    {
        heap.resize(static_cast<size_t>(maxnum));
        dist = heap.data();
    }
    int32_t found = 0;
    ForObjectsIn(lvl, rect, objset, [&](TObjectInstance* inst) {
        if (inst->IsInInventory() || (objclass != -1 && inst->ObjClass() != objclass))
            return;
        const S3DPoint& at = inst->Pos();
        const int32_t dx = abs(pos.x - at.x);
        const int32_t dy = abs(pos.y - at.y);
        int32_t d;
        if (sphere)
        {
            uint32_t r2 = 0;
            if (radii)
            {
                const double r = static_cast<TCharacter*>(inst)->Radius() * kRadiusScale;
                r2 = static_cast<uint32_t>(static_cast<int32_t>(r * r));
            }
            d = static_cast<int32_t>(Square32(dx) + Square32(dy) + Square32(abs(pos.z - at.z)) - r2);
            if (d >= sqrwidth)
                return;
        }
        else
        {
            if (dx > (width >> 1) || dy > (height >> 1))
                return;
            d = static_cast<int32_t>(Square32(dx) + Square32(dy));
        }

        int32_t i = 0;
        while (i < found && d > dist[i])
            ++i;
        if (i >= maxnum)
            return;
        const int32_t kept = (std::min)(maxnum - 1, found);
        for (int32_t k = kept; k > i; --k)
        {
            dist[k] = dist[k - 1];
            array[k] = array[k - 1];
        }
        array[i] = inst->GetMapIndex();
        dist[i] = d;
        if (found < maxnum)
            ++found;
    });
    return found;
}

// REVSYNC: 0x00452480 -- the first object of `objset` on `lvl` inside the
// cube, every face included, in the order the iterator hands them out. It
// walks that level's loaded sectors that meet the cube's map rect, and their
// objects inside that rect (iterator 0x0044cf80, flags 0x6e0: loaded
// sectors, active or not, of one level; the map rect; no inventory) -- not
// the pane's 3x3 window. A CUBE trigger asks on its owner's level
// (0x004927b0); the 1998 search walked the window, the player's level.
TObjectInstance* TMapPane::ObjectInCube(PS3DRect cube, int32_t lvl, int32_t objset)
{
    SRect area;
    area.left   = cube->beg.x;
    area.top    = cube->beg.y;
    area.right  = cube->end.x;
    area.bottom = cube->end.y;

    for (TMapIterator i(lvl, &area, CHECK_NOINVENT, objset); i; i++)
    {
        if (cube->In(i->Pos()))
            return i;
    }

    return nullptr;
}

// REVSYNC: OnObject @ 0x00452520 (docs/gameflow/forensics/MAP_INPUT.md §2).
// First the 3D scene: a mesh shown at retail's probe pixels around the point
// is the answer as it stands (Locke himself included). Else the map's
// objects: the screen rect must hold the point; tiles (outside the editor)
// and meshes are never answers here; the first object found stays unless a
// later one is always on top, or, while nothing always-on-top is held, shows
// its own pixel at the point (GetZ); outside the editor an object counts only
// with a cursor for `with`, or as a pickup. Retail drew the probe into the
// frame and z-found its software buffers; the port asks the map renderer what
// the last frame showed (TMapRenderer::Pick), and the point's map-screen
// position replaces retail's (x + posx, y + posy).
TObjectInstance* TMapPane::OnObject(int32_t screenx, int32_t screeny, TObjectInstance* with)
{
    return OnObject(screenx, screeny, with, EPickRead::Exact);
}

TObjectInstance* TMapPane::OnObject(int32_t screenx, int32_t screeny, TObjectInstance* with, EPickRead read)
{
    if (!mapview)
        return nullptr;

    // A deterministic run picks from the drawn frame, as retail did: what the
    // hover and a click see then follows the frames, not the GPU's timing.
    if (DeterministicRun())
        read = EPickRead::Drawn;

    TMapRenderer::SPick pick;
    if (mapview->Pick(screenx, screeny, pick, read) && pick.mesh)
        return pick.mesh;

    SPoint p;
    if (!PaneToMapScreen(screenx, screeny, p.x, p.y))
        return nullptr;

    TObjectInstance* on = nullptr;
    bool priority = false;
    for (TMapIterator i(nullptr, CHECK_NOINVENT); i; i++)
    {
        TObjectInstance* inst = i;
        if (!inst->OnObject(p))
            continue;
        if (!Editor && inst->ObjClass() == OBJCLASS_TILE)
            continue;
        if (const TObjectImagery* img = inst->GetImagery())
            if (img->GetHeader() && img->GetHeader()->imageryid == OBJIMAGE_MESH3D)
                continue;
        if (on && !inst->AlwaysOnTop() && (priority || !inst->GetZ(pick.frontmost)))
            continue;
        if (!Editor && !(!GetDragObj() && inst->IsInventoryItem()) &&
            inst->CursorType(with) == CURSOR_NONE)
            continue;

        on = inst;
        priority = inst->AlwaysOnTop();
    }

    return on;
}

// Retail's x + posx, y + posy: the scroll origin of the frame that was drawn,
// which in the port is the map renderer's camera.
bool TMapPane::PaneToMapScreen(int32_t x, int32_t y, int32_t& mapx, int32_t& mapy) const
{
    return mapview && mapview->ScreenToMapScreen(x, y, mapx, mapy);
}

// Resolve an instance by mapindex. O(1) via the registry; falls back to a
// TMapIterator scan on a miss (defensive — shouldn't happen if SetMapIndex
// and the dtor are the only mutation points, but keeps behavior identical
// to retail in case a code path sets mapindex directly and skips the
// registry). objset is preserved from retail's signature; when the hit
// path is taken it is ignored, since every in-tree caller passes
// OBJSET_ALL.
TObjectInstance* TMapPane::GetInstance(int32_t index, int32_t objset)
{
    if (index < 0)
        return nullptr;

    auto& m = InstMap();
    auto it = m.find(index);
    if (it != m.end())
        return it->second;

    TMapIterator i(nullptr, 0, objset);
    for ( ; i; i++)
        if (i->GetMapIndex() == index)
            return i;
    return nullptr;
}

// *********************
// * Walkmap Functions *
// *********************

void PosToWalkGrid(S3DPoint& pos, int32_t &x, int32_t &y)
{
    // because 0, 0 is actually the center of a walk grid, not the edge,
    // everything has to be offset by half the walk grid size

    x = (pos.x + (GRIDSIZE / 2)) >> WALKMAPSHIFT;
    y = (pos.y + (GRIDSIZE / 2)) >> WALKMAPSHIFT;
}

void WalkGridToPos(int32_t x, int32_t y, S3DPoint& pos)
{
    // because 0, 0 is actually the center of a walk grid, not the edge,
    // everything has to be offset by half the walk grid size

    pos.x = (x << WALKMAPSHIFT) - (GRIDSIZE / 2);
    pos.y = (y << WALKMAPSHIFT) - (GRIDSIZE / 2);
    pos.z = 0;
}

// General purpose handler for transfer, extract, clear etc
void TMapPane::WalkmapHandler(TObjectInstance* oi, int32_t mode)
{
    // Sector resolver: the walkmaps an object touches are in its own map --
    // its own sector (which may be one no map holds, e.g. a sector an editor
    // command loaded on its own) and the loaded sectors of its level. Not
    // the sector window or the current map: those hold another level's
    // sectors at the same (sx, sy) whenever the object isn't on the level
    // being played. (The window borrows from the current map, so for
    // objects there this is the same lookup.)
    TSector* const own = oi ? oi->GetSector() : nullptr;
    const TGameMap* const map = oi ? MapManager.GetCached(oi->GetLevel()) : nullptr;
    auto find_sector = [own, map](int32_t sx, int32_t sy) -> TSector* {
        if ((!map || !map->IsTransient()) &&
            ((uint32_t)sx >= MAXSECTORX || (uint32_t)sy >= MAXSECTORY))
            return nullptr;
        if (own && own->SectorX() == sx && own->SectorY() == sy)
            return own;
        return map ? map->FindSector(sx, sy) : nullptr;
    };

    // EXTRACT mode also wants to redraw the walkmap rect as part of the
    // clear. Wrap the resolver to capture the sector for the redraw
    // call, since TGameMap::StampWalkmap doesn't know about the
    // pane redraw chrome.
    if (mode == WALK_EXTRACT)
    {
        // Stamp the clear, then walk the affected sectors a second time
        // to redraw the rect. The redraw needs imagery bbox / pos in
        // walkmap-local coords; recompute here once after the stamp.
        TGameMap::StampWalkmap(oi, mode, find_sector);

        // RedrawWalkmapRect re-stamps the neighbours from the sector window,
        // which holds the current map's sectors only; redrawing them into
        // another level's sector would corrupt it.
        TObjectImagery* imagery = oi ? oi->GetImagery() : nullptr;
        if (imagery && map && map == windowmap.Get())
        {
            int32_t w, l, h;
            imagery->GetWorldBoundBox(oi->GetState(), w, l, h);
            int32_t rx = imagery->GetWorldRegX(oi->GetState());
            int32_t ry = imagery->GetWorldRegY(oi->GetState());
            if (oi->GetFace() != 0)
            {
                int32_t nx, ny, nsx, nsy;
                oi->GetFacingBoundBox(nx, ny, nsx, nsy);
                rx = nx; ry = ny; w = nsx; l = nsy;
            }
            S3DPoint pos; oi->GetPos(pos);
            const int32_t x = (pos.x >> WALKMAPSHIFT) - rx;
            const int32_t y = (pos.y >> WALKMAPSHIFT) - ry;
            const int32_t startsectx = (x << WALKMAPSHIFT) >> SECTORWSHIFT;
            const int32_t startsecty = (y << WALKMAPSHIFT) >> SECTORHSHIFT;
            const int32_t endsectx   = ((x + w - 1) << WALKMAPSHIFT) >> SECTORWSHIFT;
            const int32_t endsecty   = ((y + l - 1) << WALKMAPSHIFT) >> SECTORHSHIFT;
            for (int32_t sy = startsecty; sy <= endsecty; sy++)
            for (int32_t sx = startsectx; sx <= endsectx; sx++)
                if (TSector* sect = find_sector(sx, sy))
                    RedrawWalkmapRect(oi, x, y, w, l, sect);
        }
        return;
    }

    TGameMap::StampWalkmap(oi, mode, find_sector);
}

void TMapPane::RedrawWalkmapRect(TObjectInstance* oi, int32_t x, int32_t y, int32_t w, int32_t l, TSector* dsect)
{
    for (int32_t sx = 0; sx < SECTORWINDOWX; sx++)
        for (int32_t sy = 0; sy < SECTORWINDOWY; sy++)
        {
            TSector* sect = sectors[sx][sy];
            if (!sect)
                continue;

            for (TObjectIterator i(sect->ObjectArray()); i; i++)
            {
                TObjectInstance* inst = i.Item();

                if (!inst || oi == inst || inst->IsInInventory() || (inst->Flags() & OF_NOWALK))
                    continue;

                TObjectImagery* imagery = inst->GetImagery();
                if (!imagery)
                    continue;

                const uint8_t *walk = imagery->GetWalkMap(inst->GetState());
                if (!walk)
                    continue;

                int32_t iw, il, ih;
                imagery->GetWorldBoundBox(inst->GetState(), iw, il, ih);

                int32_t rx, ry;
                rx = imagery->GetWorldRegX(inst->GetState());
                ry = imagery->GetWorldRegY(inst->GetState());

                S3DPoint pos;
                inst->GetPos(pos);

                int32_t ix = (pos.x >> WALKMAPSHIFT) - rx;
                int32_t iy = (pos.y >> WALKMAPSHIFT) - ry;

                // clip this bad boy
                int32_t cx = ix - x, cy = iy - y, cw = iw, cl = il;
                int32_t ax = 0, ay = 0;

                if (cx < 0)
                {
                    cw += cx;
                    ax = -cx;
                    cx = 0;
                }

                if (cy < 0)
                {
                    cl += cy;
                    ay = -cy;
                    cy = 0;
                }

                if ((cx + cw) > w)
                    cw = w - cx;

                if ((cy + cl) > l)
                    cl = l - cy;

                if (cw <= 0 || cl <= 0)
                    continue;

                cx += x;
                cy += y;
                walk += (ay * iw) + ax;

                dsect->WalkmapHandler(WALK_TRANSFER, const_cast<uint8_t*>(walk), pos.z, cx, cy, cw, cl, iw);
            }
        }
}

// REVSYNC: GetWalkHeight @ 0x00452e10 -- the walk cell under pos: 0, 0 is
// the centre of a walk cell, not its edge, so offset by half a cell.
int32_t TMapPane::GetWalkHeight(const S3DPoint& pos, int32_t lvl)
{
    return GetWalkGridHeight(lvl, (pos.x + GRIDSIZE / 2) >> WALKMAPSHIFT, (pos.y + GRIDSIZE / 2) >> WALKMAPSHIFT);
}

// Retail looks a cell's sector up among every loaded one by (level, x, y)
// (0x00499e10); the port keeps a level's sectors in its cached map, the
// current level's also in the pane's window.
int32_t TMapPane::GetWalkGridHeight(int32_t lvl, int32_t x, int32_t y)
{
    if (walkGridSeam)
        return walkGridSeam(lvl, x, y);

    const int32_t world_sx = x >> (SECTORWSHIFT - WALKMAPSHIFT);
    const int32_t world_sy = y >> (SECTORHSHIFT - WALKMAPSHIFT);
    const int32_t wx = x & ((1 << (SECTORWSHIFT - WALKMAPSHIFT)) - 1);
    const int32_t wy = y & ((1 << (SECTORHSHIFT - WALKMAPSHIFT)) - 1);

    if (lvl == level)
    {
        const int32_t sx = world_sx - sectorx;
        const int32_t sy = world_sy - sectory;
        if (sx >= 0 && sx < SECTORWINDOWX && sy >= 0 && sy < SECTORWINDOWY && sectors[sx][sy])
            return sectors[sx][sy]->ReturnWalkmap(wx, wy);

        // MapManager-canonical fallback (see GetWalkHeight comment).
        if (TGameMap* m = MapManager.CurrentMap())
            if (TSector* sec = m->FindSector(world_sx, world_sy))
                return sec->ReturnWalkmap(wx, wy);
        return 0;
    }

    if (TGameMap* m = MapManager.GetCached(lvl))
        if (TSector* sec = m->FindSector(world_sx, world_sy))
            return sec->ReturnWalkmap(wx, wy);
    return 0;
}

bool TMapPane::WalkCellKnown(int32_t x, int32_t y)
{
    if (walkGridSeam)
        return true;
    const int32_t world_sx = x >> (SECTORWSHIFT - WALKMAPSHIFT);
    const int32_t world_sy = y >> (SECTORHSHIFT - WALKMAPSHIFT);
    const int32_t sx = world_sx - sectorx;
    const int32_t sy = world_sy - sectory;
    if (sx >= 0 && sx < SECTORWINDOWX && sy >= 0 && sy < SECTORWINDOWY && sectors[sx][sy])
        return true;
    const TGameMap* m = MapManager.CurrentMap();
    return m && m->IsLoaded();
}

// Returns the maximum walk height in the area bounded by 
// pos - width/2,height/2 to pos + with/2,height/2
int32_t TMapPane::GetWalkHeightArea(S3DPoint& pos, int32_t width, int32_t height)
{
    if (width == 0)
        return GetWalkHeight(pos);

    width /= 2;     // to make the calculations simpler
    height /= 2;

    // make sure they are aligned values
    width &= GRIDMASK;
    height &= GRIDMASK;

    S3DPoint p(0, 0, 0);
    int32_t highest = 0;

    for (p.y = (pos.y - height); p.y <= (pos.y + height); p.y += GRIDSIZE)
        for (p.x = (pos.x - width) ; p.x <= (pos.x + width); p.x += GRIDSIZE)
        {
            int32_t h = GetWalkHeight(p);

            if (h == 0)
                return 0;

            if (h > highest)
                highest = h;
        }

    return highest;
}

// Returns the minimum delta (fall), and maximum delta (rise) between any two walk grids 
// within the radius, and also the height at the current position.

// REVSYNC: GetWalkHeightRadius @ 0x004530a0 -- the height under pos; then,
// for a radius in (0, 0xf0), every walk cell in the box pos +- radius whose
// nearest point is within the radius (the distance table, or the squares
// past it): maxdelta is the largest height step to the cell west or north of
// it (when that one is in range too), hole is set by a cell of height 0 (no
// walkmap). A radius whose cells would reach 0x80 is cut to 0x6f. The cells
// are read from the loaded sectors of level lvl (FindSector 0x00499e10).
void TMapPane::GetWalkHeightRadius(const S3DPoint& pos, int32_t lvl, int32_t radius, int32_t& maxdelta,
    int32_t& height, bool& hole)
{
    maxdelta = 0;
    hole = false;
    height = GetWalkHeight(pos, lvl);
    if (radius <= 0 || radius >= 0xf0)
        return;
    if (((radius + GRIDSIZE - 1) & GRIDMASK) >= 0x80)
        radius = 0x6f;

    const int32_t x0 = (pos.x - radius) & GRIDMASK;
    const int32_t y0 = (pos.y - radius) & GRIDMASK;
    const int32_t x1 = (pos.x + radius + GRIDSIZE) & GRIDMASK;
    const int32_t y1 = (pos.y + radius + GRIDSIZE) & GRIDMASK;

    // Retail keeps the cells in one static array, eight to a column, which
    // the larger radii run past; each read still finds the cell this call
    // wrote, so a grid over the whole box gives the same answers.
    constexpr int32_t kCells = (2 * 0x70 + 2 * GRIDSIZE) / GRIDSIZE + 1;
    int32_t cells[kCells][kCells];     // [row][column]; -1 out of range
    int32_t row = 0;
    for (int32_t y = y0; y <= y1; y += GRIDSIZE, ++row)
    {
        int32_t col = 0;
        for (int32_t x = x0; x <= x1; x += GRIDSIZE, ++col)
        {
            // A cell spans its centre -8..+7: its point nearest pos.
            const int32_t dx = abs(std::clamp(pos.x, x - GRIDSIZE / 2, x + GRIDSIZE / 2 - 1) - pos.x);
            const int32_t dy = abs(std::clamp(pos.y, y - GRIDSIZE / 2, y + GRIDSIZE / 2 - 1) - pos.y);
            const bool inrange = (dx < 256 && dy < 256) ? DistTable[dx][dy] <= radius
                                                        : dx * dx + dy * dy <= radius * radius;
            if (!inrange)
            {
                cells[row][col] = -1;
                continue;
            }
            const int32_t h = GetWalkHeight(S3DPoint(x, y, 0), lvl);
            cells[row][col] = h;
            if (h == 0)
                hole = true;
            if (col > 0 && cells[row][col - 1] >= 0)
                maxdelta = (std::max)(maxdelta, abs(h - cells[row][col - 1]));
            if (row > 0 && cells[row - 1][col] >= 0)
                maxdelta = (std::max)(maxdelta, abs(h - cells[row - 1][col]));
        }
    }
}

void TMapPane::TransferAllWalkmaps()
{
    for (TMapIterator i(nullptr, CHECK_NOINVENT); i; i++)
        TransferWalkmap(i);
}

void TMapPane::ClearWalkmaps()
{
    for (int32_t sx = 0; sx < SECTORWINDOWX; sx++)
    {
        for (int32_t sy = 0; sy < SECTORWINDOWY; sy++)
        {
            TSector* sect = sectors[sx][sy];
            if (!sect)
                continue;

            sect->ClearWalkmap();
        }
    }
}

namespace
{
// 16.16 fixed point as retail's shl / sar: the wrap and the sign's shift are
// the x86's, not undefined behaviour.
constexpr int32_t ToFixed(int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) << 16); }
constexpr int32_t FromFixed(int32_t v) { return v >> 16; }

// One fixed-point step along a line `loop` cells long, nudged one unit toward
// its sign on all but the major axis (0x00453445..0x004534ed).
constexpr int32_t LineStep(int32_t delta, int32_t loop, bool major)
{
    return ToFixed(delta) / loop + (major ? 0 : (delta >= 0 ? 1 : -1));
}
}  // namespace

// REVSYNC: TMapPane::LineOfSight @ 0x004533d0 -- a DDA over the walk cells
// from `from`'s cell to `to`'s, both ends included, the line's height
// interpolated alongside (MAP_QUERIES.md §1.1). A cell blocks when its
// level's sector isn't loaded, it has no walkmap (height 0), or its walk
// height is above the line there.
bool TMapPane::LineOfSight(const S3DPoint& from, const S3DPoint& to, int32_t lvl, S3DPoint* blockedat,
    S3DPoint* laststep)
{
    if (lineOfSightSeam)
        return lineOfSightSeam(from, to);

    const int32_t sx = (from.x + GRIDSIZE / 2) >> WALKMAPSHIFT;
    const int32_t sy = (from.y + GRIDSIZE / 2) >> WALKMAPSHIFT;
    int32_t dx = ((to.x + GRIDSIZE / 2) >> WALKMAPSHIFT) - sx;
    int32_t dy = ((to.y + GRIDSIZE / 2) >> WALKMAPSHIFT) - sy;
    int32_t dz = to.z - from.z;

    const bool xmajor = abs(dx) >= abs(dy);         // ties go to x
    const int32_t loop = xmajor ? abs(dx) : abs(dy);
    if (loop > 0)
    {
        dx = LineStep(dx, loop, xmajor);
        dy = LineStep(dy, loop, !xmajor);
        dz = LineStep(dz, loop, false);
    }

    int32_t x = ToFixed(sx);
    int32_t y = ToFixed(sy);
    int32_t z = ToFixed(from.z);
    for (int32_t c = 0; c <= loop; ++c)
    {
        const int32_t h = GetWalkGridHeight(lvl, FromFixed(x), FromFixed(y));
        if (h == 0 || h > FromFixed(z))
        {
            if (blockedat)
            {
                WalkGridToPos(FromFixed(x), FromFixed(y), *blockedat);
                blockedat->z = FromFixed(z);
            }
            if (laststep)
            {
                laststep->x = FromFixed(x) - FromFixed(x - dx);
                laststep->y = FromFixed(y) - FromFixed(y - dy);
                laststep->z = FromFixed(z) - FromFixed(z - dz);
            }
            return false;
        }
        x += dx;
        y += dy;
        z += dz;
    }
    return true;
}

// ***************************
// * Dynamic Light Functions *
// ***************************

void TMapPane::DrawDLight()
{
    static int32_t oldintensity = 0;

    return;

    if (oldintensity != dlight.intensity && dlight.lightindex >= 0)
    {
        Scene3D.DeleteLight(dlight.lightindex);
        dlight.lightindex = -1;
    }
    oldintensity = dlight.intensity;

    if (dlight.intensity == 0)
        return;

    if (dlight.lightindex < 0)
    {
        dlight.lightindex = Scene3D.AddLight(dlight.pos, dlight.color, dlight.intensity, dlight.multiplier);
    }
    else
    {
        Scene3D.SetLightPosition(dlight.lightindex, dlight.pos);
    }

    S3DPoint spos;
    WorldToScreen(dlight.pos, spos);

//  if (NoNormals)
        DrawLightNoNormals(spos, dlight.color, dlight.intensity, &Display);
//  else
//      DrawLight(spos, dlight.color, dlight.intensity, &Display);

    PTBitmap bitmap = GameData->Bitmap("dlight");
    Display.Put(spos.x - bitmap->width / 2,
                 spos.y - bitmap->width / 2 - 20, 
                 bitmap, DM_ALPHALIGHTEN); //, nullptr, 31 - (dlight.intensity / 8));
}

// REVSYNC: 0x00453640. Retail adds the gamma ambient offset here, so every
// area ambient, the map's start value (map init 0x0044d5c0 inlines this
// with light 10) and the script's ambient command carry it. FadeAmbient
// (0x00453720) stores its target without it, as retail.
void TMapPane::SetAmbientLight(int32_t light, bool stopfade)
{
    light = (std::max)(0, light + GammaAmbientOffset(GammaLevel));

    if (stopfade)
        totambfadeframes = totambfadesteps = 0; // Stop fading

    if (totambfadeframes > 0)  // Doing fade? Then change target value
    {
        newambient = light;
    }
    else
    {
        if (ambient == light)
            return;
        ambient = light; 
        ambientchanged = true;  // Causes ambient to be reset in next call to DrawBackground()
    }
}   

void TMapPane::SetAmbientColor(SColor &color, bool stopfade)
{
    if (stopfade)
        totambfadeframes = totambfadesteps = 0; // Stop fading

    if (totambfadeframes > 0)  // Doing fade? Then change target value
    {
        newambcolor = color;
    }
    else
    {
        if (ambcolor.red == color.red &&
            ambcolor.green == color.green &&
            ambcolor.blue == color.blue)
                return;
        ambcolor = color;
        ambientchanged = true;  // Causes ambient to be reset in next call to DrawBackground()
    }
}

// Fades to the given ambient light in 'frame' frames in 'steps' discreet steps
// REVSYNC: FadeAmbient @ 0x00453720 -- the cross-fade between nearby areas
// (TArea::Enter 0x0041ba00: same level, within 0x400) and a load into the
// same room.
// REVSYNC-DIVERGENCE: retail stores the target as given, without the
// GammaLevel offset every other ambient setter adds (SetAmbientLight
// 0x00453640), so each such fade dropped the room's ambient by the offset
// (+10 at the default level 3: the Keep went from 14 to 4, an orange wash)
// until a day/night area set it again. Not reproduced: the target gets the
// offset too (LIGHTING_FIDELITY.md §2.5).
void TMapPane::FadeAmbient(int32_t light, SColor &color, int32_t frames, int32_t steps)
{
    light = (std::max)(0, light + GammaAmbientOffset(GammaLevel));

    if (GameSpeed == 5)
        steps = frames; // Do smooth ambient fading for fast computers

    if (light == ambient && 
        color.red == ambcolor.red && 
        color.green == ambcolor.green && 
        color.blue == ambcolor.blue)
            return;

    oldambient = ambient;
    oldambcolor = ambcolor;
    newambient = light;
    newambcolor = color;
    totambfadeframes = frames;
    totambfadesteps = steps;
    ambfadeframes = 0;
    ambfadesteps = 0;
}

// Called to actually set the new ambient values
void TMapPane::PulseFadeAmbient()
{
    if (totambfadeframes <= 0 || totambfadesteps <= 0)
        return;

    if (ambfadeframes != totambfadeframes * ambfadesteps / totambfadesteps)
    {
        ambfadeframes++;
        return;
    }

  // Interpolate between old and new ambient values
    ambient = oldambient * (totambfadesteps - ambfadesteps) / totambfadesteps + 
            newambient * ambfadesteps / totambfadesteps;
    ambcolor.red = (uint8_t)((int32_t)((int32_t)oldambcolor.red * (totambfadesteps - ambfadesteps) / totambfadesteps + 
            (int32_t)newambcolor.red * ambfadesteps / totambfadesteps));
    ambcolor.green = (uint8_t)((int32_t)((int32_t)oldambcolor.green * (totambfadesteps - ambfadesteps) / totambfadesteps + 
            (int32_t)newambcolor.green * ambfadesteps / totambfadesteps));
    ambcolor.blue = (uint8_t)((int32_t)((int32_t)oldambcolor.blue * (totambfadesteps - ambfadesteps) / totambfadesteps + 
            (int32_t)newambcolor.blue * ambfadesteps / totambfadesteps));
    ambientchanged = true;

  // Next fade step
    ambfadeframes++;
    ambfadesteps++;
    if (ambfadesteps > totambfadesteps)         // Done
        totambfadeframes = totambfadesteps = 0;
}

// ********************************
// * Background Drawing Functions *
// ********************************

void TMapPane::Update3DScenePos()
{
    S3DPoint source;
    S3DPoint target;
    S3DPoint scrn;

    target = center;

    // convert the position to have no z height for d3d - it makes no difference
    // for what you see on the screen, and seems to fix some problems with the d3d view
    if (target.z > 0)
    {
        int32_t x, y;
        WorldToScreen(target, x, y);
        ScreenToWorld(x, y, target);
    }


    WorldToScreen(target, scrn);

    Scene3D.SetCameraPos(target, (int32_t)(scrn.z * ZSCALE));
}

// UpdateMapPos - Checks the system movement params and sets the map pane position.

void TMapPane::UpdateMapPos()
{

    if (Editor)     // Set map position code for editor
    {               // --------------------------------
        if (SmoothScroll)
        {
            if (mx || my)
            {
                S3DPoint mpos;
                ScreenToWorld(mx, my, mpos);
                mpos += center;
                SetMapPos(mpos);
                StatusBar.SetDirty(true);
            }
        }
        else
        {
            int32_t sx, sy;
            WorldToScreen(center, sx, sy);
            int32_t x = ScreenGrid(sx, MAPGRIDWIDTH) * MAPGRIDWIDTH;
            int32_t y = ScreenGrid(sy - CHARACTER_HEIGHT, MAPGRIDHEIGHT) * MAPGRIDHEIGHT;

            if (x != sx || y != sy)
            {
                S3DPoint newpos;
                ScreenToWorld(x, y, newpos);
                SetMapPos(newpos);
                RedrawAll();
            }
        }
    }
    else
    {
        // REVSYNC: UpdateMapPos @ 0x004539d0, the game half. The camera moves
        // toward what it follows -- the centeron object or point, else where
        // it is -- scrolling smoothly, or jumping when it is told to snap (a
        // teleport), changes level, is ~1024 units away or isn't scrolling.
        // Grid scrolling is used only with smooth scrolling off while the
        // player has control (0x0065d0d0 is PlayScreen +0x5e0, the control
        // flag). Not ported: a map pane mode (+0x918 == 1) that ignored the
        // centeron target; its meaning is unknown.
        prevcenter = center;

        S3DPoint target = center;
        int32_t targetlevel = newlevel;
        if ((centeron.flags & CENTERON_OBJ) && centeron.obj)
        {
            target = centeron.obj->Pos();
            targetlevel = centeron.obj->GetLevel();
        }
        else if (centeron.flags & CENTERON_POS)
        {
            target = centeron.pos;
            targetlevel = centeron.level;
        }

        // Height changes under 9 units (steps, bobbing) don't move the camera.
        if (absval(lastcamz - target.z) < 9)
            target.z = lastcamz;

        const bool gridscroll = !SmoothScroll && PlayScreen.IsControlOn();
        bool jump = gridscroll || targetlevel != newlevel ||
                    dist(center.x, center.y, target.x, target.y) > 0x3ff ||
                    !(centeron.flags & CENTERON_SCROLL) || (centeron.flags & CENTERON_SNAP);

        S3DPoint next = target;
        if (!jump)
        {
            const int32_t factor = ScrollLock ? 6 : 10;
            const S3DPoint want((target.x - center.x) / factor,
                                (target.y - center.y) / factor,
                                (target.z - center.z) / factor);
            // Retail's test sums signed velocities, so a fast scroll up or
            // left never jumps.
            if (want.x + want.y < 0x100)
            {
                auto approach = [](int32_t v, int32_t goal) {
                    v += (goal > v) - (goal < v);
                    return std::clamp(v, -12, 12);
                };
                scrollvel.x = approach(scrollvel.x, want.x);
                scrollvel.y = approach(scrollvel.y, want.y);
                scrollvel.z = approach(scrollvel.z, want.z);
                next = center;
                next += scrollvel;
            }
            else
                jump = true;
        }
        if (jump)
            scrollvel = S3DPoint(0, 0, 0);
        lastcamz = target.z;

        if (gridscroll)
        {
            // The view moves a screen (less a 64-pixel overlap) at a time, to
            // the cell the target is in.
            auto cell = [](int32_t v, int32_t size) {
                if (absval(v) < size / 2)
                    return 0;
                return (v + (v < 0 ? -size / 2 : size / 2)) / size;
            };
            const int32_t cellw = MAPPANEWIDTH - 64;
            const int32_t cellh = MAPPANEHEIGHT - 64;
            int32_t sx, sy;
            WorldToScreen(target, sx, sy);
            S3DPoint snapped;
            ScreenToWorld(cell(sx, cellw) * cellw, cell(sy - 32, cellh) * cellh, snapped);
            if (absval(snapped.x - center.x) >= 5 || absval(snapped.y - center.y) >= 5)
            {
                SetMapPos(snapped);
                RedrawAll();
            }
        }
        else
            SetMapPos(next);

        SetMapLevel(targetlevel);
        centeron.flags &= ~CENTERON_SNAP;
    }
}

// The pulse function is pretty much the first function that's called in a screen refresh.
// You can move objects around, change the map pane position, and do anything else you
// want to before the map pane is scrolled or moved, the background is drawn, etc.
//
// The Pulse function should be where the object AI stuff is called so that it can be
// updated before the objects are displayed in the DrawBackground() and Animate() funcitons.

void TMapPane::Pulse()
{
  // Pulse fading ambient values
    PulseFadeAmbient(); 

  // REVSYNC: TMapPane::Pulse @ 0x00454390 -- objects pulse then move; their
  // animation frames advance at the end of the tick (TPlayScreen::Pulse,
  // retail's screen slot 0x24), not here.

  // Pulse objects (does object AI)
    PulseObjects();

  // Moves all objects
    MoveObjects();

  // Update map position (uses mouse scrolling for editor or 'centeron' state for game)
    UpdateMapPos();

  // Set new screen map position now that objects have been pulsed
    oldposx     = posx;
    posx        = GetScrollX();
    oldposy     = posy;
    posy        = GetScrollY();

  // Update 3D position
    Update3DScenePos();

  // Borrow the sectors around the player (retail streamed them in here:
  // UpdateSectors @ 0x00459220; TMapManager loads whole levels in the port)
    UpdateActiveWindow();

  // Leave Pulse with new map position completely set
}

void TMapPane::DrawBackground()
{
  // *********************** GLOBAL STATE CHANGES *************************
  
  // **** THIS IS WHERE ANY GLOBAL GAME IMAGERY CHANGES SHOULD BE MADE ****
  // **** FUNCTIONS WHICH CHANGE IMAGERY STATES (like ambient lighting,****
  // **** ETC. SHOULD FLAG THAT THEY NEED TO BE UPDATED, AND THIS      ****
  // **** FUNCTION SHOULD ACTUALLY DO THE UPDATING.  THIS PREVENTS A   ****
  // **** GOBAL STATE FROM BEING SET HALFWAY THROUGH A FRAME           ****
    
  // Causes ambient values to be changed at the beginning of a frame refresh
    if (ambientchanged)
    {
        ::SetAmbientLight(ambient);
        Scene3D.SetAmbientLight(ambient);
        ::SetAmbientColor(ambcolor);
        Scene3D.SetAmbientColor(ambcolor);
    
        ambientchanged = false;

        SRect r;
        r.left = GetScrollX() - 10000;
        r.top = GetScrollY() - 10000;
        r.right = GetScrollX() + 10000;
        r.bottom = GetScrollY() + 10000;
        AddBgUpdateRect(r, BGDRAW_AMBIENT);
    }

  // Update edges of screen
    int32_t updatex = GetScrollX() & UPDATEMASKX;
    int32_t updatey = GetScrollY() & UPDATEMASKY;
    UpdateEdges(updatex, updatey);

  // If redraw all, refresh the whole zbuffer
    if (IsDirty())
    {
        SRect r;
        r.left = posx;
        r.top = posy;
        r.right = r.left + GetWidth() - 1;
        r.bottom = r.top + GetHeight() - 1;
        Scene3D.RestoreZBuffer(r);
    }

    SetDirty(false);
}

void TMapPane::RedrawAll()
{
    SetDirty(true);
    scrollx += 1000000;
    scrolly += 1000000;
    ClearLights();

    // stuff to make the smoothscroll command work better
    if (!Editor && Player && !SmoothScroll)
    {
        S3DPoint pos, newpos;
        Player->GetPos(pos);

        int32_t x, y;
        WorldToScreen(pos, x, y);
        x = ScreenGrid(x, MAPGRIDWIDTH);
        y = ScreenGrid(y - CHARACTER_HEIGHT, MAPGRIDHEIGHT);
        x *= MAPGRIDWIDTH;
        y *= MAPGRIDHEIGHT;
        ScreenToWorld(x, y, newpos);

        if (absval(newpos.x - center.x) > 4 || absval(newpos.y - center.y) > 4)
            SetMapPos(newpos);
    }
}

void TMapPane::AddBgUpdateRect(SRect &r, int32_t bgdraw)
{
    if (IsDirty() || bgdraw >= BGDRAW_NONE)
        return; // No need since everything will be redrawn anyway

    if (numbgrects >= MAXMAPBGRECTS)
    {
        SetDirty(true);     // This pane is nasty bad (causes pane to redraw everything)
        numbgrects = 0;
        return;
    }

    bgrects[numbgrects].rect = r;
    bgrects[numbgrects].bgdraw = bgdraw;
    numbgrects++;
}

void TMapPane::AddObjectUpdateRect(int32_t index)
{
    TObjectInstance* inst = GetInstance(index);
    if (!inst)
        return;

    SRect ir;
    inst->GetScreenRect(ir);
    AddBgUpdateRect(ir);

    inst->RedrawLight();            // in case it's a light
}

void TMapPane::ReloadImagery()
{
    if (NoNormals == true)
        TObjectImagery::SetImageryPath(NONORMALPATH);
    else
        TObjectImagery::SetImageryPath(NORMALPATH);
    
    TObjectImagery::ReloadImagery();

    RedrawAll();
}

// ***************************************************************************************
// ***************************  Progressive Update System   ******************************
// ***************************************************************************************

static bool ThreadRunning, QuitThread, Updating, UpdateCancelled;
static HANDLE UpdateStartEvent, UpdateDoneEvent, PauseUpdateMutex;
static HANDLE SectorMutex, ObjectMutex;
static HANDLE UpdateThreadHandle;
static unsigned UpdateThreadId;

#define UPDATEWAITSAMPLES 5
static int32_t UpdateWaitSample, UpdateSleep;
static int32_t UpdateWaitFrames[UPDATEWAITSAMPLES];
static int32_t UpdateWaitTime[UPDATEWAITSAMPLES];

#define MAXQUEUERECTS (MAXMAPBGRECTS + 2)
static int32_t numqueuerects;
static SBgUpdateRect queuerects[MAXQUEUERECTS];

static char *PauseMutexFile, *SectorMutexFile, *ObjectMutexFile;
static int32_t PauseMutexLine, SectorMutexLine, ObjectMutexLine;

static TObjectArray UpdateObjs;

#if 0 // TODO(port): Subsystem 4 — threading (Win32 events/mutex/CreateThread → worker pool)

void TMapPane::BeginUpdateThread()
{
    if (ThreadRunning)
        return;

    UpdateWaitSample = 0;
    UpdateSleep = 0;

    UpdateStartEvent = CreateEvent(nullptr, true, false, nullptr);
    UpdateDoneEvent = CreateEvent(nullptr, true, false, nullptr);
    PauseUpdateMutex = CreateMutex(nullptr, false, nullptr);
    SectorMutex = CreateMutex(nullptr, false, nullptr);
    ObjectMutex = CreateMutex(nullptr, false, nullptr);

    QuitThread = false;
    Updating = false;
    UpdateCancelled = false;

    UpdateThreadHandle = (HANDLE)_beginthreadex(nullptr, 0, TMapPane::UpdateThread, nullptr, true,
        &UpdateThreadId);

    ThreadRunning = true;
}

void TMapPane::EndUpdateThread()
{
    if (!ThreadRunning)
        return;

    while (ReleaseMutex(PauseUpdateMutex));
    while (ReleaseMutex(SectorMutex));
    while (ReleaseMutex(ObjectMutex));

    CancelUpdate();
    QuitThread = true;
    PulseEvent(UpdateStartEvent);

    WaitForSingleObject(UpdateThreadHandle, INFINITE);
    
    ThreadRunning = false;
}

void ClearMutex(HANDLE hmutex)
{
#ifdef DEBUG
    char *err = "%s not released in file %s line %s";
    char buf[80];
    if (hmutex == PauseUpdateMutex && PauseMutexFile != nullptr)
    {
        sprintf(buf, err, "PauseUpdateMutex", PauseMutexFile, PauseMutexLine);
        ThreadError(buf);
    }
    else if (hmutex == SectorMutex && SectorMutexFile != nullptr)
    {
        sprintf(buf, err, "SectorMutex", SectorMutexFile, SectorMutexLine);
        ThreadError(buf);
    }
    else if (hmutex == ObjectMutex && ObjectMutexFile != nullptr)
    {
        sprintf(buf, err, "ObjectMutex", ObjectMutexFile, ObjectMutexLine);
        ThreadError(buf);
    }
#endif

    while (ReleaseMutex(hmutex));
}

int32_t TMapPane::GetUpdateSleep()
{
    return UpdateSleep;
}

void TMapPane::UpdateTimeSlice(bool draw)
{
    if (!ThreadRunning)
        return;
    
 // Check to see if queued update is finished yet
    if (isrecqueued)
    {
        if (UpdateDone())
        {
            scrollrect = queuedrect; // Set valid scroll area to queued rect if done
            isrecqueued = false;
            PutQueueRectsToDisplay();  // Causes display to show new rects
            ClearQueueRects();
        }
        else // If still waiting, allow time for update thread to run
        {
            if (UpdateSleep > 0)
            {
                int32_t start = GetTickCount();
                SetEvent(UpdateStartEvent);
                WaitForSingleObject(UpdateDoneEvent, UpdateSleep); // Not too int32_t
                ResetEvent(UpdateDoneEvent);
                int32_t end = GetTickCount();
                int32_t time = end - start;
//              if (time < UpdateSleep)
//                  Sleep(UpdateSleep - time);
                UpdateWaitTime[UpdateWaitSample] += time;
            }
        } 
    }

    UpdateWaitFrames[UpdateWaitSample]++;
    if (UpdateWaitFrames[UpdateWaitSample] > 24)
        AdjustTimeSlice(0);
}

void TMapPane::AdjustTimeSlice(int32_t milliseconds)
{
    if (!ThreadRunning)
        return;

    UpdateWaitTime[UpdateWaitSample] += milliseconds;

    UpdateSleep = 0;
    for (int32_t c = 0; c < UPDATEWAITSAMPLES; c++)
    {
        UpdateSleep = max(UpdateSleep, 
            (UpdateWaitTime[c] * 100 + 80) / (max(UpdateWaitFrames[c], 1) * 100));
    }

    if (UpdateSleep > 70)
        UpdateSleep = 70;

    UpdateWaitSample++;
    if (UpdateWaitSample >= UPDATEWAITSAMPLES)
        UpdateWaitSample = 0;

    UpdateWaitTime[UpdateWaitSample] = 0;
    UpdateWaitFrames[UpdateWaitSample] = 0;
}

void TMapPane::FlushUpdate(bool updatetimeslice)
{
    int32_t wait = WaitUpdate();    // Wait for previous queue to finish
    if (updatetimeslice)
        AdjustTimeSlice(wait);  // Adjust update wait value by how int32_t we waited here
    if (isrecqueued)
    {
        scrollrect = queuedrect;    // Update current refreshed rect
        isrecqueued = false;        // Set no rect as being queued
    }
    PutQueueRectsToDisplay();   // Causes display to show new rects
    ClearQueueRects();          // Make sure queue is empty
}

void TMapPane::UpdateEdges(int32_t updatex, int32_t updatey)
{
    if (!ThreadRunning)
        return;

    oldscrollx = scrollx;
    oldscrolly = scrolly;

    scrollx    = updatex;
    scrolly    = updatey;

  // Check to see if queued update is finished yet
    if (isrecqueued && UpdateDone())
        FlushUpdate();

  // Hey, do we update everything?
    bool redrawall = level != oldlevel || IsDirty();

  // If no scrolling occured, just draw bg rects and return
    if (scrollx == oldscrollx && 
        scrolly == oldscrolly && 
        !redrawall)
    {
        if (numbgrects > 0)
        {
            FlushUpdate();

          // Add new bg rects to fresh queue
            for (int32_t c = 0; c < numbgrects; c++)
            {
                SRect dr;
                if (!ClipRect(scrollrect, bgrects[c].rect, dr))
                    continue;
                QueueUpdateRect(dr, bgrects[c].bgdraw);
            }
            numbgrects = 0;

            BeginUpdate(); // Start updating
            FlushUpdate(); // Flush it right now
        }

        return;
    }

  // Screen is always within this rect
    SRect innerscrollrect = SRect(scrollx, scrolly, 
        scrollx + SCROLLBUFWIDTH - UPDATEWIDTH - 1, 
        scrolly + SCROLLBUFHEIGHT - UPDATEHEIGHT - 1);
    SRect outerscrollrect = innerscrollrect;

  // Redraw the whole scroll buffer
    if (redrawall || 
        (!outerscrollrect.Intersects(scrollrect) &&
         !outerscrollrect.Intersects(queuedrect)))
    {
        CancelUpdate(); // Cancel any current updating and clear queue rects

     // Draw whole screen with one update edge on each side
        innerscrollrect = SRect(scrollx, scrolly,
            scrollx + SCROLLBUFWIDTH - 1, scrolly + SCROLLBUFHEIGHT - 1);
        outerscrollrect = innerscrollrect;

        QueueUpdateRect(outerscrollrect, BGDRAW_UNLIT);

        queuedrect = outerscrollrect;
        isrecqueued = true;

        BeginUpdate();
        FlushUpdate();

        numbgrects = 0;

        return;
    }

  // Redraw just the edges
    else
    {
        FlushUpdate(true);      // true = Update timeslice values

      // Since we're drawing two update edges in front of us, and there are only 2 edges
      // extra in the scroll buffer, make sure we don't draw the right edge when scrolling
      // left, or the left edge when scrolling right, etc. etc.   If we draw all outside
      // edges, the drawing will wrap around.

      // Figure out our movment vector (What's our vector Victor,.  rodger Rodger.. etc. etc.)

        if (scrollx > oldscrollx)       // Moving right
        {
          // We've drawing two edges in front of us to the right, there are no edges to the left
            outerscrollrect.right += UPDATEWIDTH;
        }
        else if (scrollx < oldscrollx)  // Moving left
        {
          // We're drawing two edges in front of us to the left, there are no edges to the right
            outerscrollrect.left -= UPDATEWIDTH;
        }
        else
        {
            outerscrollrect.left = scrollrect.left;
            outerscrollrect.right = scrollrect.right;
        }

        if (scrolly > oldscrolly)       // Moving down
        {
          // We've drawing two edges in front of us to the bottom, there are no edges to the top
            outerscrollrect.bottom += UPDATEHEIGHT;
        }
        else if (scrolly < oldscrolly)  // Moving up
        {
          // We've drawing two edges in front of us to the top, there are no edges to the bottom
            outerscrollrect.top -= UPDATEHEIGHT;
        }
        else
        {
          // Just keep the edges we already have
            outerscrollrect.top = scrollrect.top;
            outerscrollrect.bottom = scrollrect.bottom;
        }

      // Subtract the current valid area from new area and draws invalid edges
        SRect edgerects[4];
        int32_t numedgerects;
        if (SubtractRect(outerscrollrect, scrollrect, edgerects, numedgerects))
        {
            for (int32_t c = 0; c < numedgerects; c++)
                QueueUpdateRect(edgerects[c], BGDRAW_UNLIT);
        }

      // Get part of valid scroll area that is within the new area
        ClipRect(scrollrect, outerscrollrect, scrollrect);

      // Cause update system to update background rects
        for (int32_t c = 0; c < numbgrects; c++)
        {
            SRect r = bgrects[c].rect;

          // Clip to not intersect edge rects which we're already drawing
            if (!ClipRect(r, scrollrect, r))
                continue;

            QueueUpdateRect(r, bgrects[c].bgdraw);
        }

        queuedrect = outerscrollrect;
        isrecqueued = true;

        BeginUpdate();

      // If inner scrolling rect not in current valid scroll rect, or bgrects, wait for update  
        if (numbgrects > 0 || !innerscrollrect.In(scrollrect))
            FlushUpdate(true);      // true = Adjust time slice

        numbgrects = 0;
    }

}

void TMapPane::BeginUpdate()
{
    if (!ThreadRunning)
        return;

    BEGIN_CRITICAL();

    Updating = true;
    UpdateCancelled = false;
    SetEvent(UpdateStartEvent);
    
    END_CRITICAL();
}

bool TMapPane::UpdateDone()
{
    return !Updating;
}

void TMapPane::CancelUpdate()
{
    if (!ThreadRunning)
        return;

    if (!Updating)
    {
        UpdateCancelled = false;
        return;
    }

    BEGIN_CRITICAL();

    Updating = false;
    UpdateCancelled = true;

    END_CRITICAL();

    FlushUpdate();
}

uint32_t TMapPane::WaitUpdate()
{
    if (!ThreadRunning)
        return 0;

    if (!Updating)
    {
        UpdateCancelled = false;
        return 0;
    }

    // Get start time in milliseconds
    uint32_t start = GetTickCount();

  // Release all mutexes just in case we're still blocking the loader
    ClearMutex(PauseUpdateMutex);
    ClearMutex(SectorMutex);
    ClearMutex(ObjectMutex);

    while (Updating)
    {
        SetEvent(UpdateStartEvent);
        WaitForSingleObject(UpdateDoneEvent, INFINITE);
        ResetEvent(UpdateDoneEvent);
    }

    UpdateCancelled = false;

    // Get end time in milliseconds
    uint32_t end = GetTickCount();

    return end - start; // Return elapsed time for wait
}

void TMapPane::ClearQueueRects()
{
    if (!ThreadRunning)
        return;

    if (Updating)
        WaitUpdate();   // Wait for update to finish before clearing queue

    BEGIN_CRITICAL();

    numqueuerects = 0;

    END_CRITICAL();
}

void TMapPane::QueueUpdateRect(RSRect rect, int32_t bgdraw)
{
    if (!ThreadRunning)
        return;

    if (Updating)
        FlushUpdate();

    if (numqueuerects >= MAXQUEUERECTS)  // This should never happen
        return;

//  SRect dr;
//  if (!ClipRect(scrollrect, rect, dr))
//      return;

    BEGIN_CRITICAL();

    queuerects[numqueuerects].rect = rect; //dr;
    queuerects[numqueuerects].bgdraw = bgdraw;
    numqueuerects++;
    
    END_CRITICAL();
}

void TMapPane::PutQueueRectsToDisplay()
{
    if (!ThreadRunning)
        return;

    if (Updating)
        WaitUpdate();  // Wait for update to finish before adding display rects

    for (int32_t c = 0; c < numqueuerects; c++)
    {
        Display.AddBackgroundUpdateRect(       
            GetBackgroundBuffer(),  
            queuerects[c].rect.x(), queuerects[c].rect.y(), queuerects[c].rect.w(), queuerects[c].rect.h(),
            UPDATE_BUFFERTOSCREEN | UPDATE_NEXTFRAME);
    }
}

unsigned _stdcall TMapPane::UpdateThread(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    MapPane.UpdateLoop();

  // Kill the events
    ClearMutex(PauseUpdateMutex); // Release if I own it just in case
    ClearMutex(SectorMutex);
    ClearMutex(ObjectMutex);

    CloseHandle(UpdateStartEvent);
    CloseHandle(UpdateDoneEvent);
    CloseHandle(PauseUpdateMutex);
    CloseHandle(SectorMutex);
    CloseHandle(ObjectMutex);

    _endthreadex( 0 );

    return 0;
}

void TMapPane::UpdateLoop()
{
    HANDLE pause[2];
    pause[0] = PauseUpdateMutex; 
    pause[1] = SectorMutex;

    for (;;)
    {
        if (!Updating)
            WaitForSingleObject(UpdateStartEvent, INFINITE);

        ResetEvent(UpdateStartEvent);

        if (QuitThread)
            break;

        if (!Updating)
        {
            ClearMutex(PauseUpdateMutex);
            ClearMutex(SectorMutex);
            ClearMutex(ObjectMutex);
            SetEvent(UpdateDoneEvent);
            continue;
        }

        BEGIN_CRITICAL();
        bool dodraw = numqueuerects > 0;
        END_CRITICAL();     

      // Draw update rect list  
        if (dodraw)
        {
            for (int32_t c = 0; ; c++)
            {
                BEGIN_CRITICAL();

                SRect r;
                int32_t bgdraw;
                bool done;

                if (c < numqueuerects)
                {
                    r = queuerects[c].rect;
                    bgdraw = queuerects[c].bgdraw;
                    done = false;
                }
                else
                    done = true;
                
                END_CRITICAL();

                if (done == true || UpdateCancelled)
                    break;

                WaitForMultipleObjects(2, pause, true, INFINITE);

              // Draw the update region 
                DrawUpdateRect(r, bgdraw);
                
                ReleaseMutex(PauseUpdateMutex);
                ReleaseMutex(SectorMutex);
                ReleaseMutex(ObjectMutex);

                ClearMutex(PauseUpdateMutex);
                ClearMutex(SectorMutex);
                ClearMutex(ObjectMutex);
            }
        }
        
        BEGIN_CRITICAL();

        if (UpdateCancelled)   // If cancelled, values were reset in cancel, don't change
        {                      // Now because we could have more new rects queued already
            UpdateCancelled = false;
        }
        else                   // If not cancelled, reset all values here
        {
            Updating = false;
            UpdateCancelled = false;
            SetEvent(UpdateDoneEvent);
        }

        END_CRITICAL();

        if (QuitThread)
            break;
    }

}

void TMapPane::PauseUpdate(char *file, int32_t line)
{
    if (!ThreadRunning)
        return;

    WaitSingleErr(PauseUpdateMutex);

    PauseMutexFile = file;
    PauseMutexLine = line;
}

void TMapPane::ResumeUpdate()
{
    if (!ThreadRunning)
        return;

    ReleaseMutex(PauseUpdateMutex);
    ClearMutex(PauseUpdateMutex);

    PauseMutexFile = nullptr;
    PauseMutexLine = 0;

}   

void TMapPane::LockSectors(char *file, int32_t line)
{
    if (!ThreadRunning)
        return;

    WaitSingleErr(SectorMutex);

    SectorMutexFile = file;
    SectorMutexLine = line;
}

void TMapPane::UnlockSectors()
{
    if (!ThreadRunning)
        return;

    ReleaseMutex(SectorMutex);
    ClearMutex(SectorMutex);

    SectorMutexFile = nullptr;
    SectorMutexLine = 0;
}

void TMapPane::LockObjects(char *file, int32_t line)
{
    if (!ThreadRunning)
        return;

    WaitSingleErr(ObjectMutex);

    ObjectMutexFile = file;
    ObjectMutexLine = line;
}

void TMapPane::UnlockObjects()
{
    if (!ThreadRunning)
        return;

    ReleaseMutex(ObjectMutex);
    ClearMutex(ObjectMutex);

    ObjectMutexFile = nullptr;
    ObjectMutexLine = 0;
}

#else // TODO(port): Subsystem 4 — threading stubs (synchronous fallback)

void TMapPane::BeginUpdateThread() {}
void TMapPane::EndUpdateThread() {}

static void ClearMutex(HANDLE /*hmutex*/) {}

int32_t TMapPane::GetUpdateSleep()
{
    return UpdateSleep;
}

void TMapPane::UpdateTimeSlice(bool /*draw*/) {}
void TMapPane::AdjustTimeSlice(int32_t /*milliseconds*/) {}

void TMapPane::FlushUpdate(bool /*updatetimeslice*/)
{
    if (isrecqueued)
    {
        scrollrect = queuedrect;
        isrecqueued = false;
    }
    PutQueueRectsToDisplay();
    ClearQueueRects();
}

void TMapPane::UpdateEdges(int32_t updatex, int32_t updatey)
{
    oldscrollx = scrollx;
    oldscrolly = scrolly;
    scrollx    = updatex;
    scrolly    = updatey;

    // Without the async update thread, just mark the whole scroll buffer
    // as valid and drop any pending background-rect state.
    SRect full = SRect(scrollx, scrolly,
        scrollx + SCROLLBUFWIDTH - 1, scrolly + SCROLLBUFHEIGHT - 1);
    scrollrect = full;
    queuedrect = full;
    isrecqueued = false;
    numbgrects = 0;
    numqueuerects = 0;
}

void TMapPane::BeginUpdate() {}

bool TMapPane::UpdateDone()
{
    return true;
}

void TMapPane::CancelUpdate() {}

uint32_t TMapPane::WaitUpdate()
{
    return 0;
}

void TMapPane::ClearQueueRects()
{
    numqueuerects = 0;
}

void TMapPane::QueueUpdateRect(RSRect rect, int32_t bgdraw)
{
    if (numqueuerects >= MAXQUEUERECTS)
        return;
    queuerects[numqueuerects].rect = rect;
    queuerects[numqueuerects].bgdraw = bgdraw;
    numqueuerects++;
}

void TMapPane::PutQueueRectsToDisplay()
{
    // TODO(port): wire up background-buffer blits once the display
    // pipeline gains AddBackgroundUpdateRect (or equivalent) again.
    for (int32_t c = 0; c < numqueuerects; c++)
    {
        Display.AddUpdateRect(
            queuerects[c].rect.x(), queuerects[c].rect.y(),
            queuerects[c].rect.w(), queuerects[c].rect.h(),
            UPDATE_BUFFERTOSCREEN | UPDATE_NEXTFRAME);
    }
}

unsigned TMapPane::UpdateThread(void *)
{
    return 0;
}

void TMapPane::UpdateLoop() {}

void TMapPane::PauseUpdate(const char * /*file*/, int32_t /*line*/) {}
void TMapPane::ResumeUpdate() {}
void TMapPane::LockSectors(const char * /*file*/, int32_t /*line*/) {}
void TMapPane::UnlockSectors() {}
void TMapPane::LockObjects(const char * /*file*/, int32_t /*line*/) {}
void TMapPane::UnlockObjects() {}

#endif // TODO(port): Subsystem 4 — threading

void TMapPane::DrawUpdateRect(RSRect r, int32_t bgdraw)
{
    bgdraw = bgdraw & (~(uint32_t)BGDRAW_REDRAW);

    GetUpdateObjs(r); // Get list of objects to draw

    SRect dr = r;

  // Leave origin at 0,0 (for wrap clipping) and set clip rect to current rect)
    unlitmulti->SetClipRect(dr.x(), dr.y(), dr.w(), dr.h());
    unlitmulti->SetClipMode(CLIP_WRAP);

  // Leave origin at 0,0 (for wrap clipping) and set clip rect to current rect)
    litmulti->SetClipRect(dr.x(), dr.y(), dr.w(), dr.h());
    litmulti->SetClipMode(CLIP_WRAP);

  // Draw the tile and lighting pipeline - unlit first, then lights, then lit
    if (bgdraw <= BGDRAW_UNLIT)
        DrawUnlitObjects(dr);
    if (QuitThread || UpdateCancelled)
        return;

    if (bgdraw <= BGDRAW_LIGHTS)
    {
        if (bgdraw > BGDRAW_UNLIT) // Clear lighting values
            DrawAmbientLight(unlitmulti, dr);
        DrawStaticLights(dr);
    }
    if (QuitThread || UpdateCancelled)
        return;

    if (bgdraw <= BGDRAW_AMBIENT)
        TransferUnlitToLit(dr);
    if (QuitThread || UpdateCancelled)
        return;

    if (bgdraw <= BGDRAW_LIT)
        DrawLitObjects(dr);
    if (QuitThread || UpdateCancelled)
        return;

    if (Editor)
        DrawSelectedObjects(dr);

    if (QuitThread || UpdateCancelled)
        return;
}

// The static light pass, as the renderer needs it: every light retail's
// DrawStaticLights would draw over screen rect r (the update rect), in
// GetUpdateObjs order (REVSYNC: GetUpdateObjs @ 0x0045f800 -- the sector
// window row by row, each sector's objects in order; a light is in when its
// light rect meets r), each given its light-table id as it is drawn
// (TObjectInstance::AcquireLightId, DrawLight 0x00471930). The order decides
// both which colours take the four ids and, per pixel, which light's colour
// shows: the last one drawn over it (docs/LIGHTING_FIDELITY.md §2.6).
//
// Retail gave an id back when its light's sector left the window
// (DeleteSector -> OffScreen); the port's map manager keeps sectors past
// the window, so a light holding an id outside the window gives it back here.
void TMapPane::GetDrawLights(SRect &r, std::vector<TObjectInstance*> &lights)
{
    auto in_window = [this](const TSector* sector) {
        for (int32_t y = 0; y < SECTORWINDOWY; ++y)
            for (int32_t x = 0; x < SECTORWINDOWX; ++x)
                if (sector && sectors[x][y] == sector)
                    return true;
        return false;
    };
    const auto left = std::remove_if(lightidholders.begin(), lightidholders.end(), [&](const TSafeRef<>& ref) {
        TObjectInstance* inst = ref.Get();
        if (!inst)
            return true;                    // deleted: its destructor gave the id back
        if (in_window(inst->GetSector()))
            return false;
        inst->ReleaseLightId();
        return true;
    });
    lightidholders.erase(left, lightidholders.end());

    lights.clear();
    for (TMapIterator i(&r, CHECK_RECT|CHECK_LIGHT|CHECK_MOVING|CHECK_INVIS|CHECK_NOINVENT); i; i++)
    {
        TObjectInstance* inst = i.Item();
        if (!inst->IsLight() || inst->GetLightIntensity() == 0)
            continue;
        if (inst->GetLightDef()->lightid == -1)
            lightidholders.push_back(TSafeRef<>(inst));
        inst->AcquireLightId();
        lights.push_back(inst);
    }
}

void TMapPane::GetUpdateObjs(SRect &r)
{
    UpdateObjs.Clear();

    for (TMapIterator i(&r, CHECK_RECT|CHECK_LIGHT|CHECK_MOVING|CHECK_INVIS|CHECK_NOINVENT); i; i++)
    {
        UpdateObjs.Add(i);

    // Check to see if update was canceled
        if (UpdateCancelled)
            return;
    }
}

void TMapPane::DrawUnlitObjects(SRect &r)
{
    if (ClearBeforeDraw || level != 0)
        unlitmulti->Box(r.x(), r.y(), r.w(), r.h(), 0, 0xFFFF, 0,
            DM_WRAPCLIP | DM_ZBUFFER | DM_NORMALS);
    else
        unlitmulti->Box(r.x(), r.y(), r.w(), r.h(), 0, 0xFFFF, 0,
            DM_WRAPCLIP | DM_ZBUFFER | DM_NORMALS | DM_NODRAW);

    for (TObjectIterator i(&UpdateObjs); i; i++)
    {
        WaitSingleErr(ObjectMutex);
        i.Item()->DrawUnlit(unlitmulti);
#if 0 // TODO(port): Subsystem 4 — threading
        ReleaseMutex(ObjectMutex);
#endif

    // Check to see if update was canceled
        if (UpdateCancelled)
            return;

    }
}

void TMapPane::DrawStaticLights(SRect &r)
{
    for (TObjectIterator i(&UpdateObjs); i; i++)
    {
        WaitSingleErr(ObjectMutex);
        i.Item()->DrawLight(unlitmulti, IsDirty());
#if 0 // TODO(port): Subsystem 4 — threading
        ReleaseMutex(ObjectMutex);
#endif

    // Check to see if update was canceled
        if (UpdateCancelled)
            return;
    }
}

void TMapPane::TransferUnlitToLit(SRect &r)
{
  // Transfer graphic/lighting data to lit buffer
    TransferAndLight32to16(litmulti, unlitmulti, r);

  // Transfer zbuffer data to lit buffer
    if (!zbufferiscloned)
        litmulti->Blit(r.x(), r.y(), unlitmulti, r.x(), r.y(), r.w(), r.h(), DM_NODRAW | DM_ZBUFFER | DM_WRAPCLIP);
}

void TMapPane::DrawLitObjects(SRect &r)
{
    if (Editor && StatusBar.EditWalkmap())
        DrawWalkMap(r);

    for (TObjectIterator i(&UpdateObjs); i; i++)
    {
        if (i.Item()->GetFlags() & OF_EDITOR && !Editor)
            continue;

        WaitSingleErr(ObjectMutex);
        i.Item()->DrawLit(litmulti);
#if 0 // TODO(port): Subsystem 4 — threading
        ReleaseMutex(ObjectMutex);
#endif

    // Check to see if update was canceled
        if (UpdateCancelled)
            return;
    }

    // debug stuff
//  if (ShowZBuffer)
//      litmulti->Blit(0, 0, litmulti->GetZBuffer()); 
//  if (ShowNormalBuffer)
//      litmulti->Blit(0, 0, litmulti->GetNormalBuffer()); 
}

void TMapPane::DrawWalkMap(SRect &r)
{
    SRect sr;
    int32_t sx, sy;

    // draw walkmap
    for (sx = 0; sx < SECTORWINDOWX; sx++)
    {
        for (sy = 0; sy < SECTORWINDOWY; sy++)
        {
            if (sectors[sx][sy])
            {
                sectors[sx][sy]->GetMaxScreenRect(sr);
                if (!r.Intersects(sr))
                    continue;

                for (int32_t y = wmrevealy; y < (SECTORHEIGHT >> WALKMAPSHIFT) && y < wmrevealsizey; y++)
                    for (int32_t x = wmrevealx; x < (SECTORWIDTH >> WALKMAPSHIFT) && x < wmrevealsizex; x++)
                    {
//                      if (x == objx && y == objy &&
//                          (sectorx+sx) == grabx && (sectory+sy) == graby)
//                          continue;           // dragging it

                        S3DPoint pos, screenpos;

                        pos.x = x;
                        pos.y = y;
                        pos.z = sectors[sx][sy]->ReturnWalkmap(pos.x, pos.y);

                        pos.x = ((sectorx + sx) << SECTORWSHIFT) + (x << WALKMAPSHIFT);
                        pos.y = ((sectory + sy) << SECTORHSHIFT) + (y << WALKMAPSHIFT);
                        WorldToScreen(pos, screenpos);
                        screenpos.y += 8;
                        screenpos.z -= GRIDZOFF;

                        SColor color;
                        color.red = max(30, 255 - 4*absval(pos.z - 20));
                        color.green = max(30, 255 - 4*absval(pos.z - 100));
                        color.blue = max(30, 255 - 4*absval(pos.z - 180));
                        if (pos.z > 0)
                            litmulti->ZPut(screenpos.x, screenpos.y, screenpos.z, EditorData->Bitmap("grid"), DM_TRANSPARENT | DM_WRAPCLIP | DM_ZBUFFER | DM_USEREG | DM_BACKGROUND);
                        litmulti->Put(screenpos.x, screenpos.y, EditorData->Bitmap("gridout"), DM_TRANSPARENT | DM_WRAPCLIP | DM_USEREG | DM_BACKGROUND | DM_CHANGECOLOR, &color);
                    }
            }
        }
    }
}

void TMapPane::DrawSelectedObjects(SRect &r)
{
    SRect sr;
    SColor color;
    color.red = 220; color.green = 20; color.blue = 50;

    for (int32_t i = StatusBar.GetFirstObj(); i >= 0; i = StatusBar.GetNextObj())
    {
        TObjectInstance* inst = GetInstance(i);
        if (!inst || inst->IsInInventory() || inst->GetFlags() & OF_SELDRAW)
            continue;

      // Check to see if update was canceled
        if (UpdateCancelled)
            return;

        inst->DrawSelected(litmulti);
    }
}

// ***************************************************************************************
// ******************************  End Of UPDATE SYSTEM  *********************************
// ***************************************************************************************

void TMapPane::AnimateSelectedObjects()
{
    SetClipRect();

    if (StatusBar.EditWalkmap())
    {
        if (objx >= 0 && objy >= 0)
        {
            S3DPoint pos, screenpos;
            pos.x = objx;
            pos.y = objy;
            pos.z = sectors[grabx - sectorx][graby - sectory]->ReturnWalkmap(pos.x, pos.y);
            pos.x = (grabx << SECTORWSHIFT) + (objx << WALKMAPSHIFT);
            pos.y = (graby << SECTORHSHIFT) + (objy << WALKMAPSHIFT);
            WorldToScreen(pos, screenpos);
            screenpos.x += MAPPANEX - PosX();
            screenpos.y += MAPPANEY - PosY();
            screenpos.y += 8;
            screenpos.z -= GRIDZOFF;

            SColor color;
            color.red = max(30, 255 - 4*absval(pos.z - 20));
            color.green = max(30, 255 - 4*absval(pos.z - 100));
            color.blue = max(30, 255 - 4*absval(pos.z - 180));
            Display.ZPut(screenpos.x, screenpos.y, screenpos.z, EditorData->Bitmap("grid"), DM_TRANSPARENT | DM_WRAPCLIP | DM_ZBUFFER | DM_USEREG);
            Display.Put(screenpos.x, screenpos.y, EditorData->Bitmap("gridout"), DM_TRANSPARENT | DM_WRAPCLIP | DM_USEREG | DM_CHANGECOLOR, &color);
        }
    }

    for (int32_t i = StatusBar.GetFirstObj(); i >= 0; i = StatusBar.GetNextObj())
    {
        TObjectInstance* inst = GetInstance(i);
        if (!inst || inst->IsInInventory() || !(inst->GetFlags() & OF_SELDRAW))
            continue;

        inst->DrawSelected(&Display);

        if (inst->GetShadow() >= 0)
        {
            TObjectInstance* s = GetInstance(inst->GetShadow());
            if (!s || s->IsInInventory() || !(s->GetFlags() & OF_SELDRAW))
                continue;

            s->DrawSelected(&Display);
        }
    }

    Display.ResetClipRect();
}

template <typename Visit>
void TMapPane::ForEachTickObject(int32_t objset, Visit&& visit)
{
    if (tickObjectsSeam)
    {
        for (TObjectInstance* inst : tickObjectsSeam(objset))
            visit(inst, nullptr);
        return;
    }
    for (TMapIterator i(nullptr, CHECK_NOINVENT, objset); i; i++)
        visit(i.Item(), &i);
}

// REVSYNC: TMapPane::NextFrameObjects @ 0x00457ef0 -- the animate set's
// NextFrame (slot 0x120), last in the tick (screen slot 0x24, 0x004902c0).
void TMapPane::NextFrameObjects()
{
    if (NoAnimateObjs)
        return;

    ForEachTickObject(OBJSET_ANIMATE, [](TObjectInstance* inst, TMapIterator*) { inst->NextFrame(); });
}

// REVSYNC: TMapPane::PulseObjects @ 0x004580f0 -- each object of the pulse
// set Pulses (slot 0x110), but a killed one (OF_KILL) is taken out of the
// map: not a player, which keeps its tick (0x0045815b).
void TMapPane::PulseObjects()
{
    if (NoPulseObjs)
        return;

    ForEachTickObject(OBJSET_PULSE, [](TObjectInstance* inst, TMapIterator* i) {
        if ((inst->GetFlags() & OF_KILL) && inst->ObjClass() != OBJCLASS_PLAYER)
        {
            if (i)
                i->Nuke();
            else
                removeObjectSeam(inst);
        }
        else
            inst->Pulse();
    });
}

// REVSYNC: TMapPane::MoveObjects @ 0x00458390 -- every object of the moving
// set Moves (slot 0x114, its bits kept at +0xbc for the next tick's
// UpdateAction), then every one takes the motion of the frame it is on
// (SetObjectMotion, slot 0x118).
void TMapPane::MoveObjects()
{
    if (NoPulseObjs)
        return;

    ForEachTickObject(OBJSET_MOVING, [](TObjectInstance* inst, TMapIterator*) { inst->SetMoveBits(inst->Move()); });

    // NOTE: after SetObjectMotion() is called, any calls to SetNextMove() or
    // SetMoveAngle() will OVERIDE the movement in the animation!
    ForEachTickObject(OBJSET_MOVING, [](TObjectInstance* inst, TMapIterator*) { inst->SetObjectMotion(); });
}

namespace {

// The animate pass's test (0x00458750): the object's screen rect or its
// animation rect meets r.
bool MeetsAnimateRect(TObjectInstance* oi, const SRect& r)
{
    SRect ir, ar;
    oi->GetScreenRect(ir);
    oi->GetAnimRect(ar);
    return r.Intersects(ir) || r.Intersects(ar);
}

} // namespace

// REVSYNC: TMapPane::AnimateObjects @ 0x00458750, its usual branch (+0x918
// clear). Retail anchored the rect at +0x88/+0x8c, the scroll origin the
// background update publishes (UpdateEdges: the scroll rounded down to the
// 64-pixel update grid), and grew the pane by 0xc0 on every side. The scroll
// is SetMapPos's: the view centre's screen point less half the pane, and the
// pane is the 640x480 screen (docs/LIGHTING_FIDELITY.md F14). The port's map
// pane has no size of its own (TMapRenderer draws the view), so the rect is
// built from the centre the last tick set, which is the view the next draw
// shows, and retail's pane size.
SRect TMapPane::AnimateRect() const
{
    constexpr int32_t kMargin = 0xc0;
    int32_t centrex = 0, centrey = 0;
    WorldToScreen(center, centrex, centrey);
    const int32_t originx = int32_t(uint32_t(centrex - WIDTH / 2) & UPDATEMASKX);
    const int32_t originy = int32_t(uint32_t(centrey - HEIGHT / 2) & UPDATEMASKY);
    return SRect(originx - kMargin, originy - kMargin,
                 originx + WIDTH + kMargin - 1, originy + HEIGHT + kMargin - 1);
}

// The pass reaches what its iterator hands out (flags 0x29, CHECK_SECTRECT |
// CHECK_INVIS | CHECK_NOINVENT, over the animate set): the visible animated
// objects of the window's sectors whose screen rect meets AnimateRect(). It
// is skipped while NoAnimateObjs (0x006680d8) is set.
std::optional<bool> TMapPane::AnimatePassOnScreen(TObjectInstance* oi)
{
    if (NoAnimateObjs || !oi->IsAnimated() || (oi->GetFlags() & OF_INVISIBLE))
        return std::nullopt;

    TSector* const sector = oi->GetSector();
    if (!sector)
        return std::nullopt;

    const SRect r = AnimateRect();
    for (const auto& column : sectors)
        for (TSector* windowsector : column)
        {
            if (windowsector != sector)
                continue;
            SRect sr;
            sector->GetMaxScreenRect(sr);
            if (!r.Intersects(sr))
                return std::nullopt;
            return MeetsAnimateRect(oi, r);
        }
    return std::nullopt;
}

void TMapPane::AnimateObjects(bool draw)
{
    if (NoAnimateObjs)
        return;

    SRect r = AnimateRect();     // Get area of screen where we should turn on animators

    Display.SetClipRect(MAPPANEX, MAPPANEY, GetWidth(), GetHeight());

    for (TMapIterator i(&r, CHECK_SECTRECT|CHECK_INVIS|CHECK_NOINVENT, OBJSET_ANIMATE); i; i++)
    {
        TObjectInstance* inst = i.Item();

        const bool intersects = MeetsAnimateRect(inst, r);

        if (intersects && !inst->HasAnimator())
            inst->OnScreen();

        else if (!intersects)
            inst->OffScreen();

        inst->Animate(draw);
    }

    Display.ResetClipRect();
}

// *****************
// * Sector Window *
// *****************

// The window borrows sectors from TMapManager's current map (see mappane.h).
// Loading, saving and freeing sectors is the manager's job: retail's
// FreeAllSectors / ReloadSectors / SaveAllSectors / DeleteSector and the
// streaming UpdateSectors are in attic-final:attic/src/mappane_sectors.cpp with the
// TMapManager operations that replace them.

// REVSYNC: UpdateSectors @ 0x00459220 -- the window is the sectors around
// the map's center (where the camera looks: normally the player, elsewhere
// during a scripted pan), on the map's level.
bool TMapPane::BindCommandMapWindow(int32_t maplevel, const S3DPoint& mapcenter)
{
    TGameMap* map = MapManager.CurrentMap();
    if (!map || map->Level() != maplevel) return false;
    // Diagnostics do not silently take over a live gameplay window.
    if (!command_window_borrowed)
        for (int32_t y = 0; y < SECTORWINDOWY; ++y)
            for (int32_t x = 0; x < SECTORWINDOWX; ++x)
                if (sectors[x][y]) return false;
    center = prevcenter = mapcenter;
    level = newlevel = maplevel;
    sectorx = (mapcenter.x >> SECTORWSHIFT) - SECTORWINDOWX / 2;
    sectory = (mapcenter.y >> SECTORHSHIFT) - SECTORWINDOWY / 2;
    BindWindow(map); // Use the same safe-reference and unload listener as gameplay.
    command_window_borrowed = true;
    return true;
}

void TMapPane::ReleaseCommandMapWindow()
{
    if (command_window_borrowed) ClearWindow();
}

void TMapPane::UpdateActiveWindow()
{
    TGameMap* map = MapManager.CurrentMap();
    if (!map) return;

    // The camera has moved to a level that isn't current yet (a teleport):
    // nothing to borrow until TGameSession::EnterLevel brings it in.
    if (map->Level() != newlevel)
    {
        if (windowmap.Get())
            ClearWindow();
        level = newlevel;
        return;
    }

    // Window origin = center's sector minus half the window, so that sector
    // is the center cell.
    const int32_t new_sectorx = (center.x >> SECTORWSHIFT) - SECTORWINDOWX / 2;
    const int32_t new_sectory = (center.y >> SECTORHSHIFT) - SECTORWINDOWY / 2;

    // A load replaces the map even when the center lands in the same sector
    // cell, so the map is part of the check.
    if (windowmap.Get() == map && level == newlevel &&
        sectorx == new_sectorx && sectory == new_sectory)
        return;

    level    = newlevel;
    sectorx  = new_sectorx;
    sectory  = new_sectory;
    BindWindow(map);
}

// Where the camera looks this frame: the previous tick's center, moved
// `fraction` of the way to this tick's (the center steps at 24 Hz). At the
// tick that is retail's view: TMapPane::Pulse (0x00454390) takes the pane's
// current scroll as the frame's origin (+0x78) before the scroll this tick's
// UpdateMapPos asked for comes in, so retail drew each tick one center behind.
S3DPoint TMapPane::CameraPos(double fraction) const
{
    const double f = std::clamp(fraction, 0.0, 1.0);
    return S3DPoint(prevcenter.x + (int32_t)std::lround((center.x - prevcenter.x) * f),
                    prevcenter.y + (int32_t)std::lround((center.y - prevcenter.y) * f),
                    prevcenter.z + (int32_t)std::lround((center.z - prevcenter.z) * f));
}

void TMapPane::BindWindow(TGameMap* map)
{
    if (windowmap.Get() != map)
    {
        ClearWindow();
        windowmap = map;
        // The map fires Unloaded before it frees its sectors (gamemap.h), so
        // the window never holds a sector the map has freed.
        windowlistener = map->AddListener([this](EGameMapEvent event, TGameMap*) {
            if (event == EGameMapEvent::Unloaded)
                ClearWindow();
        });
    }

    for (int32_t y = 0; y < SECTORWINDOWY; ++y)
        for (int32_t x = 0; x < SECTORWINDOWX; ++x)
            sectors[x][y] = map->FindSector(sectorx + x, sectory + y);
}

void TMapPane::ClearWindow()
{
    if (TGameMap* map = windowmap.Get(); map && windowlistener)
        map->RemoveListener(windowlistener);
    windowlistener = 0;
    windowmap.Clear();
    command_window_borrowed = false;

    for (int32_t y = 0; y < SECTORWINDOWY; ++y)
        for (int32_t x = 0; x < SECTORWINDOWX; ++x)
            sectors[x][y] = nullptr;
}

/*void TMapPane::GetPlayerFocus(S3DPoint& pos)
{
    if (!Player)
        return;

    if (ScrollLock)
        Player->GetPos(pos);
    else
    {
        // find the vector of facing (where's he's currently focusing his eyes)
        ConvertToVector(((PTCharacter)Player)->GetFacingTarget(), 96, pos);

        S3DPoint curpos;        // add in current position
        Player->GetPos(curpos);
        pos += curpos;
    }

    pos.x -= 42;            // account for character height on screen
    pos.y -= 42;
}
*/

// ***************************
// * Main Animation Function *
// ***************************

// REVSYNC: Animate @ 0x00454450. Retail drew the map here (the objects'
// animations, then T3DScene::DrawScene, which also picked under the
// pointer); TMapRenderer::RenderFrame draws it in the port, so what is left
// is the tail: the cursor for what is under the pointer. The type is worked
// out on every 8th screen frame (retail's frame counter, one per drawn tick:
// about three times a second) and kept (+0x12c); the cursor gets it every
// drawn frame, which retail cleared after drawing (0x0043a480) and the port
// clears as the next frame begins (BeginCursorFrame). Called by the game
// mode while the pointer is on the map.
// Not ported: the mark retail sets on a hovered item's animator (+0x35,
// purpose unconfirmed).
void TMapPane::Animate(bool draw)
{
    if (!draw || Editor || !mapview)
        return;

    // The map view sits at the screen's top left: the pointer is in its pixels.
    const int32_t x = cursorx;
    const int32_t y = cursory;
    int32_t mapx = 0, mapy = 0;
    if (!PaneToMapScreen(x, y, mapx, mapy))
        return;

    // Each frame copies back the ids around the pointer; the hover reads the
    // latest copy, taken where the pointer was a frame or two ago, rather
    // than wait on the GPU for this frame's (a click reads it exactly). A
    // deterministic run reads the drawn frame at once (OnObject), retail's
    // synchronous pick.
    mapview->SetPickPoint(x, y);

    const int32_t frame = CurrentScreen ? CurrentScreen->FrameCount() : 0;
    if ((frame & 7) == 1 && frame != hoverpickframe)
    {
        hoverpickframe = frame;
        hovercursor = CURSOR_NONE;
        TObjectInstance* on = OnObject(x, y, nullptr, EPickRead::Latest);
        if (on)
        {
            if (!GetDragObj() && on->IsInventoryItem() && !(on->GetFlags() & OF_INVISIBLE))
                hovercursor = CURSOR_HAND;          // can pick up while in the map pane
            else
                hovercursor = on->CursorType(GetDragObj());
        }
        // The pick, in the log when it changes (test runs read it).
        const int32_t onindex = on ? on->GetMapIndex() : -1;
        if (onindex != hoverindex)
        {
            hoverindex = onindex;
            log_info("[mapinput] over %s (%d, %d): cursor %d",
                     on ? on->GetName() : "nothing", x, y, hovercursor);
        }
    }

    SetMouseCornerBitmap(hovercursor);
}

// ***********************
// * Object Manipulation *
// ***********************

int32_t TMapPane::MoneyHandler(TObjectInstance* oi, int32_t amount)
{
    if (oi == nullptr)
        return 0;

    bool subtract = (amount > 0);

    int32_t total = 0;

    for (TInventoryIterator i(oi); i; i++)
    {
        if (i->ObjClass() == OBJCLASS_MONEY)
        {
            int32_t val = i->Amount() * i->Value();

            if (subtract)
            {
                if (amount < val)
                    i->SetAmount((val - amount) / i->Value());
                else    // extract the object, it's used up
                    DeleteObject( i);
            }

            total += val;
        }

        if (subtract && total >= amount)
            break;
    }

    for (TInventoryIterator j(oi); j; j++)
    {
        if (subtract && total >= amount)
            break;

        total += MoneyHandler(j, subtract ? amount - total : 0);
    }

    return total;
}

int32_t TMapPane::SubtractMoney(TObjectInstance* oi, int32_t amount)
{
    if (amount < 1 || GetTotalMoney(oi) < amount)
        return 0;

    return MoneyHandler(oi, amount);
}

int32_t TMapPane::GetTotalMoney(TObjectInstance* oi)
{
    return MoneyHandler(oi, 0);
}

// The loaded sector at (level, sx, sy): any sector of a level TMapManager
// holds (retail's loaded-sector list, 0x00499e10).
static TSector* LoadedSector(int32_t level, int32_t sx, int32_t sy)
{
    TGameMap* map = MapManager.GetCached(level);
    return map ? map->FindSector(sx, sy) : nullptr;
}

// REVSYNC: CheckPos @ 0x00459f50 -- before an object moves to newpos (which
// this may clamp), keep it in a loaded sector:
// - a move within its sector changes nothing;
// - into another loaded sector, it changes sectors;
// - a player whose destination isn't loaded (another level, not yet
//   brought in) leaves the map until TGameSession::EnterLevel puts it back;
// - any other object stays inside its sector.
// Returns the object's map index, 0 when it left the map, -1 when it can't
// be placed.
int32_t TMapPane::CheckPos(TObjectInstance* inst, S3DPoint& newpos, int32_t newlevel)
{
    if (inst->IsInInventory())
        return inst->GetMapIndex();

    // Only non-map objects (players) can change level.
    if (newlevel == -1 || !(inst->Flags() & OF_NONMAP))
        newlevel = inst->GetLevel();

    const TGameMap* movement_map = MapManager.GetCached(newlevel);
    if (!movement_map || !movement_map->IsTransient()) {
        newpos.x = std::clamp(newpos.x, 0, MAXMAPWIDTH);
        newpos.y = std::clamp(newpos.y, 0, MAXMAPHEIGHT);
    }

    if (!LoadedSector(newlevel, newpos.x >> SECTORWSHIFT, newpos.y >> SECTORHSHIFT))
    {
        if (inst->Flags() & OF_NONMAP)
        {
            TakeOutOfMap(inst);
            return 0;
        }

        // Retail clamps to its sector's span plus one: a move past the far
        // edge lands on the next sector's first unit, which then fails below.
        const S3DPoint cur = inst->Pos();
        const int32_t minx = (cur.x >> SECTORWSHIFT) << SECTORWSHIFT;
        const int32_t miny = (cur.y >> SECTORHSHIFT) << SECTORHSHIFT;
        newpos.x = std::clamp(newpos.x, minx, minx + SECTORWIDTH);
        newpos.y = std::clamp(newpos.y, miny, miny + SECTORHEIGHT);
    }

    TSector* from = inst->GetSector();
    TSector* to = LoadedSector(newlevel, newpos.x >> SECTORWSHIFT, newpos.y >> SECTORHSHIFT);
    if (!from || !to)
        return -1;
    if (from != to)
    {
        LOCKSECTORS;
        from->RemoveObject(inst);
        to->AddObject(inst);
        UNLOCKSECTORS;
    }
    return inst->GetMapIndex();
}

// The player half of CheckPos: out of its sector with its shadow and
// animator, but not deleted -- the camera, scripts and the player list keep
// their references (no N_DELETINGOBJECT, unlike RemoveObject).
void TMapPane::TakeOutOfMap(TObjectInstance* inst)
{
    if (inst->GetShadow() >= 0)
        RemoveObject(GetInstance(inst->GetShadow()));
    if (inst->HasAnimator())
        inst->FreeAnimator();
    if (inst->GetSector())
        RemoveFromSector(inst);
}

// Walkmap auto-generator for current sector
void TMapPane::CalculateWalkmap()
{
    TSector* sector = sectors[1][1];
    if (!sector)
        return;

    for (int32_t y = 0; y < (SECTORWIDTH >> WALKMAPSHIFT); y++)
        for (int32_t x = 0; x < (SECTORHEIGHT >> WALKMAPSHIFT); x++)
        {
            S3DPoint pos, screenpos;

            pos.x = ((sectorx+1) << SECTORWSHIFT) + (x << WALKMAPSHIFT);
            pos.y = ((sectory+1) << SECTORHSHIFT) + (y << WALKMAPSHIFT);
            pos.z = 0;
            WorldToScreen(pos, screenpos);

            if (screenpos.x < posx || screenpos.x >= (posx+GetWidth()) ||
                screenpos.y < posy || screenpos.y >= (posy+GetHeight()))
                continue;

            for (pos.z = 255; pos.z > 1; pos.z--)
            {
                pos.x = ((sectorx+1) << SECTORWSHIFT) + (x << WALKMAPSHIFT);
                pos.y = ((sectory+1) << SECTORHSHIFT) + (y << WALKMAPSHIFT);
                WorldToScreen(pos, screenpos);

                if (screenpos.x < posx || screenpos.x >= (posx+GetWidth()) ||
                    screenpos.y < posy || screenpos.y >= (posy+GetHeight()))
                    continue;

                updatemulti->SetClipRect(screenpos.x, screenpos.y, 1, 1);
                screenpos.y += 8;
                screenpos.z -= GRIDZOFF;

                if (!updatemulti->ZFind(screenpos.x, screenpos.y, screenpos.z, EditorData->Bitmap("grid"), DM_USEREG | DM_ZBUFFER))
                {
                    pos.x = x;
                    pos.y = y;
                    sector->SetWalkmap(pos.x, pos.y, pos.z);
                    break;
                }
            }
        }

    MapPane.RedrawAll();
}

void TMapPane::AdjustWalkmap(int32_t deltaz, bool absolute, bool nonzero)
{
    TSector* sect = sectors[1][1];
    if (!sect || !deltaz)
        return;

    S3DPoint pos;
    for (pos.y = min(wmrevealsizey, (SECTORHEIGHT >> WALKMAPSHIFT)) - 1; pos.y >= wmrevealy; pos.y--)
        for (pos.x = min(wmrevealsizex, (SECTORWIDTH >> WALKMAPSHIFT)) - 1; pos.x >= wmrevealx; pos.x--)
        {
            pos.z = sect->ReturnWalkmap(pos.x, pos.y);
            if (nonzero && pos.z == 0)
                continue;

            if (absolute)
                pos.z = 0;

            pos.z = min(255, max(0, pos.z + deltaz));
            sect->SetWalkmap(pos.x, pos.y, pos.z);
        }

    RedrawAll();
}


char *ModeCursorName[NUMMODES] = { "crosshar", "stamp", "marrow" };

// Handler for special editor modes
void TMapPane::SetMode(void (*tfunc)(S3DPoint), void (*afunc)(), int32_t newmode)
{
    if (newmode >= NUMMODES || newmode < 0)
        return;

    targetcallback = tfunc;
    abortcallback = afunc;
    mode = newmode;

    if (mode == MODE_CLONE || mode == MODE_MOVE)
    {
        dragmode = true;
        TObjectInstance* inst = GetInstance(StatusBar.GetSelectedObj());
        if (inst)
        {
            SRect r;
            inst->GetScreenRect(r);
            objx = 0;
            objy = 0; //-((r.bottom - r.top + 1) / 2);
        }
    }

    SetMouseBitmap(EditorData->Bitmap(ModeCursorName[mode]));

    //PlayScreen.SetExclusivePane(&MapPane);
}

// Notify objects of changes
void TMapPane::Notify(uint32_t notify, void *ptr)
{
  // If nothing has requested notify, just quit 
    if (!(notify & notifyflags))
        return;

    bool notifyanything = false;

    // Find objects to notify
    for (TMapIterator i(nullptr, CHECK_NONE, OBJSET_NOTIFY); i; i++)
    {
        if (i->GetFlags() & OF_NOTIFY)
        {
            i->Notify(notify, ptr);
            notifyanything = true;
        }
    }

    // Nothing wants to be notified anymore.. so set flags to 0
    if (!notifyanything)
        notifyflags = 0;
}

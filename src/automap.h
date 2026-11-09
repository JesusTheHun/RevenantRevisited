// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                    automap.h  - Automapper File                       *
// *************************************************************************

#pragma once

#include "renderer.h"
#include "screen.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class TMulti;
class TPlayer;
class TSurface;
struct SAutoMapRecord;

// REVSYNC: the explored area of one level (retail's visgrid, 0x2084 bytes):
// a bit per 128 x 128 map-screen px, a word of 4 x 4 bits per 512 x 512
// automap tile, 65 x 64 words. Columns -63..63, rows 0..63; a negative
// column folds onto rows 2..64 with its row moved 32 (0x00529300), so a
// negative and a positive column can share a word, as in retail.
class TAutoMapGrid
{
  public:
    static constexpr int32_t kWords = 65 * 64;

    explicit TAutoMapGrid(int32_t lev = 0) : level(lev) {}

    [[nodiscard]] int32_t Level() const { return level; }
    // Whether the map-screen point (x, y) is explored (0x00529300); a point
    // off the grid isn't.
    [[nodiscard]] bool Explored(int32_t x, int32_t y) const;
    // Marks it explored (0x005293f0).
    void Explore(int32_t x, int32_t y);

    // The grid as a save record holds it (0x005295e0): (0x8000 | count - 1,
    // value) for a run, (0, value) for a single word. Retail's decoder
    // (0x00529150) reads any such stream.
    [[nodiscard]] std::vector<int16_t> Encode() const;
    // From a record's stream; false unless it makes exactly kWords words
    // (retail "error decompressing level visgrid", 0x00529220).
    bool Decode(const std::vector<int16_t>& stream);

  private:
    static bool Address(int32_t x, int32_t y, int32_t& word, int32_t& bit);

    int32_t level = 0;
    std::array<uint16_t, kWords> words{};
};

// REVSYNC: TAutoMap @ 0x0065b4f0 (vtable 0x005a5658; docs/ui/HUD_REBUILD.md
// §6d). The side panel's automap, in its lower slot: the module's automap
// tiles around the camera, scaled by the zoom, the explored area clear and
// the rest under a fog, the characters near the player as markers, inside
// the ring of its frame. "+" and "-" zoom while held; dragging inside the
// ring scrolls the map, a right-click returns it to the camera.
//
// The map follows what the player explores (RecordTravels), into the main
// player's automap record (TPlayer::automap), which the game saves.
class TAutoMap final : public TPane
{
  public:
    static constexpr int32_t kWidth = 0xbc;
    static constexpr int32_t kHeight = 0xae;

    TAutoMap();
    ~TAutoMap() override;
    TAutoMap(const TAutoMap&) = delete;
    TAutoMap& operator=(const TAutoMap&) = delete;

    bool Initialize() override;     // 0x00529970
    void Close() override;          // 0x0052a490
    void Pulse() override;          // 0x0052a280: the zoom, the scroll, the tiles, the fog
    void Compose() override;        // 0x0052a5f0: the map, the markers and fog, the frame
    void Draw() override;
    void MouseClick(int32_t button, int32_t x, int32_t y) override;    // 0x0052c1a0
    void MouseMove(int32_t button, int32_t x, int32_t y) override;     // 0x0052c2c0

    // REVSYNC: 0x0052b9f0 -- the player's travels: the tiles to show next,
    // and the area around the player explored whenever the player moved.
    // The play screen calls it after its panes pulse.
    void RecordTravels();
    // REVSYNC: 0x0052c3d0 -- the main player became `player` (none: null):
    // the map shows that player's explored area, for the current module.
    void BindPlayer(TPlayer* player);
    // REVSYNC: 0x0052c5c0 -- the level shown folded into the bound player's
    // record, as the player's save does first (0x00529830).
    void FlushGrid();
    [[nodiscard]] bool IsBoundTo(const SAutoMapRecord& record) const { return travels == &record; }

    // The fog of war over the unexplored area, and the markers (the "fow"
    // command, 0x00425440). On from the start.
    [[nodiscard]] bool FogOfWar() const { return fogofwar; }
    void ToggleFogOfWar() { fogofwar = !fogofwar; SetDirty(true); }

    // The zoom in percent (100 to 300) and the held buttons, for the hosts.
    [[nodiscard]] int32_t Zoom() const { return zoom; }
    [[nodiscard]] bool ZoomInHeld() const { return zoomin; }
    [[nodiscard]] bool ZoomOutHeld() const { return zoomout; }
    void ScrollZoom(int32_t units);

  private:
    struct STile
    {
        TTextureHandle texture = kInvalidTexture;   // none: the file is missing
        bool kept = false;                          // carried over from the last ring
    };
    struct SStamp                                   // a tile to stamp into the mosaic
    {
        int32_t x = 0, y = 0;                       // its slot
        TTextureHandle texture = kInvalidTexture;   // none: black
    };

    [[nodiscard]] int32_t ToScreen(int32_t mosaic) const;
    [[nodiscard]] int32_t HalfSpan() const;
    void BindTravels(TPlayer& player);
    void LoadGrid(int32_t level);
    void StoreGrid();
    void RebuildTileCache();        // 0x0052aa70
    void StampPendingTiles();
    void BuildFog();                // 0x0052af00
    void ComposeBody(int32_t target_w, int32_t target_h);       // 0x0052a830
    void ComposeMarkers(int32_t target_w, int32_t target_h);    // 0x0052a620
    void ComposeFog(int32_t target_w, int32_t target_h);        // 0x0052b520
    void ComposeFrame(int32_t target_w, int32_t target_h);      // 0x0052bdb0

    std::unique_ptr<TMulti> archive;            // AutoMap.dat (retail DAT_0066f330)
    TBitmap* marker = nullptr;                  // 4 x 4 markers of 4 x 4: frame across, kind down
    TBitmap* frame = nullptr;                   // "Amap"
    TBitmap* plussel = nullptr;
    TBitmap* minussel = nullptr;
    std::unique_ptr<TSurface> mosaic;           // the tiles, a 5 x 5 torus (retail +0x68 / +0x6c)
    std::unique_ptr<TSurface> layer;            // the pane as last composed
    std::array<STile, 25> tiles{};              // [x * 5 + y] (retail +0xa8)
    std::vector<SStamp> pending;                // tiles to stamp into the mosaic (in Compose)
    bool mosaicfresh = true;                    // the mosaic is still to be cleared

    SAutoMapRecord* travels = nullptr;          // the player's record (retail +0x60)
    std::optional<TAutoMapGrid> grid;           // the level shown (retail +0x64)

    int32_t tilex = 0, tiley = 0;               // the ring's first tile (retail +0x110 / +0x118)
    int32_t prevtilex = 0, prevtiley = 0;       // at the last RecordTravels (+0x10c / +0x114)
    int32_t cachelevel = 0;                     // the level the ring holds (+0x12c)
    int32_t scrollx = 0, scrolly = 0;           // the window's top-left, mosaic px (+0x11c / +0x120)
    int32_t zoom = 100;                         // percent (+0x130)
    int32_t wheelRemainder = 0;
    int32_t playerx = 0, playery = 0;           // the player's map-screen point (+0x138 / +0x140)
    int32_t prevplayerx = 0, prevplayery = 0;   // at the last RecordTravels (+0x134 / +0x13c)
    bool zoomin = false;                        // "+" held (+0x9c)
    bool zoomout = false;                       // "-" held (+0xa0)
    bool freescroll = false;                    // dragged off the camera (+0xa4)
    bool fogofwar = true;                       // retail DAT_005d7a6c
    int32_t lastmousex = 0, lastmousey = 0;     // retail DAT_0066e2dc / DAT_0066e2e0
    int32_t pulses = 0;                         // retail DAT_0066f334
    int32_t markerframe = 0;                    // retail DAT_0066e148

    // The fog: 10 x 10 vertices and how undiscovered each is (retail
    // DAT_0066e6b0, DAT_0066e14c), and what they were built for.
    static constexpr int32_t kFogSide = 10;
    std::array<TRenderer::SColorVertex2D, kFogSide * kFogSide> fogvertices{};
    std::array<float, kFogSide * kFogSide> fog{};
    int32_t fogzoom = 0;                        // retail DAT_0066f338
    int32_t fogx = 0x7fffffff, fogy = 0x7fffffff;   // the snapped origin (DAT_005e32f0 / 4)
};

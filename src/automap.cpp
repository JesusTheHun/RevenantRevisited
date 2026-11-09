// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                 automap.cpp  - EXILE Automapper File                  *
// *************************************************************************
//
// Retail's automap pane (TAutoMap, d:\revenant\AutoMap.cpp). The 1998
// snapshot's pane -- a CPU merge of before/after map bitmaps -- is in
// attic-final:attic/src/automap.cpp; retail replaced it with this one.
//
// Retail drew the pane through T3DScene's overlay quads on a D3D7 device,
// which puts pixel centres on integer coordinates; the GPU puts them half a
// pixel in. The map's scaled quads and the fog's vertices move half a pixel
// right and down (kD3DPixelCentre) so the same pixels are covered and the
// same texels sampled. Quads drawn 1:1 cover and sample alike either way.
//
// *************************************************************************

#include "automap.h"

#include "bitmap.h"
#include "character.h"
#include "logging.h"
#include "mappane.h"
#include "module.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "revutils.h"
#include "sector.h"
#include "surface.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// REVSYNC: 0x0052a830 / 0x0052b520 -- the map's window, 144 x 144 at (20, 18)
// inside the frame.
constexpr int32_t kWindowLeft = 0x14;
constexpr int32_t kWindowTop = 0x12;
constexpr int32_t kWindowSize = 0x90;

// REVSYNC: 0x00529970 / 0x0052aa70 -- a 320 x 320 mosaic of 64 x 64 tiles, the
// 5 x 5 ring around the window's top-left tile, each in its slot of the torus.
constexpr int32_t kMosaicSize = 0x140;
constexpr int32_t kTileSize = 0x40;
constexpr int32_t kRing = 5;
constexpr int32_t kTileShift = 6;               // mosaic px -> tile
constexpr int32_t kMosaicShift = 3;             // map-screen px -> mosaic px

// REVSYNC: 0x0052a280 -- ten points a pulse while held, from 100 to 300.
constexpr int32_t kZoomMin = 100;
constexpr int32_t kZoomMax = 300;
constexpr int32_t kZoomStep = 10;

// REVSYNC: 0x0052c1a0 / 0x0052bdb0 -- "+" at (8, 3) and "-" at (144, 3), the
// size of PlusSel both (strictly inside); the ring about (90, 87), radius 69.
constexpr int32_t kPlusLeft = 8;
constexpr int32_t kMinusLeft = 0x90;
constexpr int32_t kButtonTop = 3;
constexpr int32_t kRingX = 0x5a;
constexpr int32_t kRingY = 0x57;
constexpr int32_t kRingRadiusSquared = 0x1299;

// REVSYNC: 0x0052a620 -- a marker is 4 x 4 of the Marker sheet: its frame
// across (a frame every five pulses, four frames), its kind down: the player,
// an enemy of the player, anyone else. Its top-left at the character's place,
// when that is inside (20..163, 18..161).
constexpr int32_t kMarkerSize = 4;
constexpr int32_t kMarkerFrames = 4;
constexpr int32_t kPulsesPerMarkerFrame = 5;
constexpr int32_t kMarkerPlayerRow = 0;
constexpr int32_t kMarkerEnemyRow = 8;
constexpr int32_t kMarkerOtherRow = 0xc;
constexpr int32_t kMarkerRight = 0xa4;
constexpr int32_t kMarkerBottom = 0xa2;
constexpr int32_t kNearRange = 0x200;

// REVSYNC: 0x0052b9f0 -- the player explores 512 map-screen px around, a
// 128 px cell at a time.
constexpr int32_t kExploreReach = 0x200;
constexpr int32_t kExploreStep = 0x80;

// REVSYNC: 0x0052c3d0 -- the record's module with none current.
constexpr const char* kNoModule = "Revenant";

constexpr float kD3DPixelCentre = 0.5f;

// v mod m in 0..m-1.
int32_t Wrap(int32_t v, int32_t m)
{
    const int32_t r = v % m;
    return r < 0 ? r + m : r;
}

// REVSYNC: 0x0052a830 -- retail's offset of the scroll into the mosaic: a
// negative multiple of the size gives the size, not 0 (the same picture).
int32_t MosaicOffset(int32_t v, int32_t m)
{
    return v % m + (v < 0 ? m : 0);
}

// REVSYNC: 0x0052b526 -- the fog's triangles: two a cell, clockwise on screen.
constexpr int32_t kFogCells = 9;
constexpr int32_t kFogIndices = kFogCells * kFogCells * 6;
constexpr std::array<uint16_t, kFogIndices> MakeFogIndices()
{
    std::array<uint16_t, kFogIndices> indices{};
    int32_t n = 0;
    for (int32_t row = 0; row < kFogCells; ++row)
        for (int32_t col = 0; col < kFogCells; ++col)
        {
            const uint16_t v = static_cast<uint16_t>(row * 10 + col);
            for (const uint16_t i : { v, uint16_t(v + 1), uint16_t(v + 10), uint16_t(v + 1), uint16_t(v + 11),
                                      uint16_t(v + 10) })
                indices[n++] = i;
        }
    return indices;
}
constexpr std::array<uint16_t, kFogIndices> kFogIndexList = MakeFogIndices();

uint64_t PathKey(const std::string& path)
{
    uint64_t hash = 1469598103934665603ull;     // FNV-1a
    for (const char c : path)
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return hash ? hash : 1;
}

// REVSYNC: TBitmap::LoadBMP @ 0x004a2ce0 as the automap uses it -- a 64 x 64
// 8-bit Windows bitmap, uncompressed, rows bottom up (or top down with a
// negative height), into RGBA8. Retail draws an 8-bit bitmap through its
// 15-bit palette, so a colour keeps 5 bits a channel.
bool ReadTile(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba)
{
    const auto u16 = [&](size_t at) { return uint32_t(file[at] | file[at + 1] << 8); };
    const auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    constexpr size_t kFileHeader = 14, kInfoHeader = 40;
    if (file.size() < kFileHeader + kInfoHeader || file[0] != 'B' || file[1] != 'M')
        return false;
    const size_t info = u32(kFileHeader);
    const int32_t width = int32_t(u32(kFileHeader + 4));
    const int32_t height = int32_t(u32(kFileHeader + 8));
    if (info < kInfoHeader || width != kTileSize || (height != kTileSize && height != -kTileSize)
        || u16(kFileHeader + 14) != 8 || u32(kFileHeader + 16) != 0)
        return false;
    const size_t colours = u32(kFileHeader + 32) ? u32(kFileHeader + 32) : 256;
    const size_t palette = kFileHeader + info;
    const size_t pixels = u32(10);
    const size_t stride = (size_t(width) + 3) & ~size_t(3);
    if (colours > 256 || palette + colours * 4 > file.size() || pixels + stride * kTileSize > file.size())
        return false;
    rgba.assign(size_t(kTileSize) * kTileSize * 4, 0);
    for (int32_t row = 0; row < kTileSize; ++row)
    {
        const uint8_t* src = &file[pixels + stride * size_t(height > 0 ? kTileSize - 1 - row : row)];
        uint8_t* dst = &rgba[size_t(row) * kTileSize * 4];
        for (int32_t col = 0; col < kTileSize; ++col, dst += 4)
        {
            const size_t index = src[col] < colours ? src[col] : 0;
            const uint8_t* bgr = &file[palette + index * 4];
            dst[0] = bgr[2] & 0xf8;
            dst[1] = bgr[1] & 0xf8;
            dst[2] = bgr[0] & 0xf8;
            dst[3] = 0xff;
        }
    }
    return true;
}

// REVSYNC: 0x0052aa70 -- the module's automap tile for (level, x, y),
// "<module>\automaps\<level>_<x>_<y>.bmp". None when the file is missing.
TTextureHandle LoadTile(int32_t level, int32_t x, int32_t y)
{
    char name[64];
    std::snprintf(name, sizeof(name), "automaps\\%d_%d_%d.bmp", level, x, y);
    const std::string path = ModuleManager.ModuleFilePath(name);
    if (path.empty() || !rev_file_exists(path.c_str()) || !Renderer)
        return kInvalidTexture;
    FILE* file = rev_fopen(path.c_str(), "rb");
    if (!file)
        return kInvalidTexture;
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(file);
    std::vector<uint8_t> rgba;
    if (!ReadTile(bytes, rgba))
    {
        log_warn("[automap] %s is not a %d x %d 8-bit bitmap", path.c_str(), kTileSize, kTileSize);
        return kInvalidTexture;
    }
    const TTextureHandle texture = Renderer->RegisterTextureAsset(PathKey(path), rgba.data(), rgba.size(), kTileSize,
                                                                  kTileSize, ERendererTextureFormat::RGBA8,
                                                                  rgba.size(), ERendererTextureFilter::Nearest);
    if (texture != kInvalidTexture)
        Renderer->AddTextureAssetRef(texture);
    return texture;
}

}  // namespace

// ****************
// * TAutoMapGrid *
// ****************

// REVSYNC: 0x00529300 / 0x005293f0 -- the word of the 512 px tile, the bit
// of the 128 px cell in it.
bool TAutoMapGrid::Address(int32_t x, int32_t y, int32_t& word, int32_t& bit)
{
    int32_t column = x >> 9;
    int32_t row = y >> 9;
    if (column > 0x3f || column < -0x3f || row < 0 || row > 0x3f)
        return false;
    if (column < 0)
    {
        column += 0x41;
        row = row <= 0x1f ? row + 0x20 : row - 0x20;
    }
    word = column * 64 + row;
    bit = (Wrap(y, 512) >> 7) * 4 + (Wrap(x, 512) >> 7);
    return true;
}

bool TAutoMapGrid::Explored(int32_t x, int32_t y) const
{
    int32_t word = 0, bit = 0;
    return Address(x, y, word, bit) && (words[word] >> bit) & 1;
}

void TAutoMapGrid::Explore(int32_t x, int32_t y)
{
    int32_t word = 0, bit = 0;
    if (Address(x, y, word, bit))
        words[word] = static_cast<uint16_t>(words[word] | (1u << bit));
}

std::vector<int16_t> TAutoMapGrid::Encode() const
{
    std::vector<int16_t> stream;
    for (size_t i = 0; i < words.size();)
    {
        size_t run = 1;
        while (i + run < words.size() && words[i + run] == words[i] && run < 0x8000)
            ++run;
        stream.push_back(static_cast<int16_t>(run > 1 ? 0x8000 | (run - 1) : 0));
        stream.push_back(static_cast<int16_t>(words[i]));
        i += run;
    }
    return stream;
}

// REVSYNC: 0x00529150 -- (control, value) pairs: with 0x8000 set the value
// repeats control & 0x7fff more times.
bool TAutoMapGrid::Decode(const std::vector<int16_t>& stream)
{
    size_t n = 0;
    for (size_t i = 0; i + 1 < stream.size(); i += 2)
    {
        const uint16_t control = static_cast<uint16_t>(stream[i]);
        const size_t count = control & 0x8000 ? (control & 0x7fffu) + 1 : 1;
        if (n + count > words.size())
            return false;
        std::fill_n(words.begin() + n, count, static_cast<uint16_t>(stream[i + 1]));
        n += count;
    }
    return n == words.size();
}

// ************
// * TAutoMap *
// ************

TAutoMap::TAutoMap() : TPane(0, 0, kWidth, kHeight) {}
TAutoMap::~TAutoMap() = default;

// REVSYNC: 0x00529970. Its host places it in the side panel's lower slot.
bool TAutoMap::Initialize()
{
    if (IsOpen())
        return true;
    archive.reset(TMulti::LoadMulti("AutoMap.dat"));
    marker = archive ? archive->Bitmap("Marker") : nullptr;
    frame = archive ? archive->Bitmap("Amap") : nullptr;
    plussel = archive ? archive->Bitmap("PlusSel") : nullptr;
    minussel = archive ? archive->Bitmap("MinusSel") : nullptr;
    if (!marker || !frame || !plussel || !minussel)
    {
        log_error("[automap] AutoMap.dat or its Marker / Amap / PlusSel / MinusSel is missing");
        archive.reset();
        return false;
    }
    if (!TPane::Initialize())
        return false;
    mosaic = std::make_unique<TSurface>(kMosaicSize, kMosaicSize, SG_PIXELFORMAT_RGBA8, SG_FILTER_NEAREST);
    mosaicfresh = true;
    pending.clear();
    tiles = {};
    travels = nullptr;
    grid.reset();
    tilex = tiley = prevtilex = prevtiley = 0;
    cachelevel = 0;
    scrollx = scrolly = 0;
    zoom = kZoomMin;
    wheelRemainder = 0;
    fogofwar = true;
    markerframe = 0;
    lastmousex = lastmousey = 0;
    zoomin = zoomout = freescroll = false;
    if (Player)
    {
        BindTravels(*Player);
        LoadGrid(MapPane.GetMapLevel());
        Player->GetScreenPos(playerx, playery);
        for (int32_t dx = -kExploreReach; dx <= kExploreReach; dx += kExploreStep)
            for (int32_t dy = -kExploreReach; dy <= kExploreReach; dy += kExploreStep)
                grid->Explore(playerx + dx, playery + dy);
    }
    SetDirty(true);
    return true;
}

void TAutoMap::Close()
{
    if (!IsOpen())
        return;
    TPane::Close();
    for (STile& tile : tiles)
        if (tile.texture != kInvalidTexture && Renderer)
            Renderer->ReleaseTextureAssetRef(tile.texture);
    tiles = {};
    pending.clear();
    mosaic.reset();
    layer.reset();
    grid.reset();
    travels = nullptr;
    marker = frame = plussel = minussel = nullptr;
    archive.reset();
}

// REVSYNC: 0x00529970 / 0x0052c3d0 -- the player's record, for the current
// module: another module's levels are dropped.
void TAutoMap::BindTravels(TPlayer& player)
{
    travels = &player.AutoMapRecord();
    const TModule* module = ModuleManager.Active();
    const std::string name = module ? module->dirname : kNoModule;
    if (travels->module != name)
    {
        travels->module = name;
        travels->levels.clear();
    }
}

// The record's level `level` (the last one, as retail reads them), else a
// level nothing of is explored.
void TAutoMap::LoadGrid(int32_t level)
{
    grid.emplace(level);
    if (!travels)
        return;
    for (const SAutoMapLevel& entry : travels->levels)
    {
        if (entry.level != level)
            continue;
        TAutoMapGrid loaded(level);
        if (loaded.Decode(entry.mask))
            grid = loaded;
        else
            log_warn("[automap] error decompressing level visgrid for level %d", level);
    }
}

// REVSYNC: 0x0052c5c0 -- the grid replaces the record's first entry for its
// level, at the end.
void TAutoMap::StoreGrid()
{
    if (!grid || !travels)
        return;
    std::vector<SAutoMapLevel>& levels = travels->levels;
    const auto same = std::find_if(levels.begin(), levels.end(),
                                   [&](const SAutoMapLevel& entry) { return entry.level == grid->Level(); });
    if (same != levels.end())
        levels.erase(same);
    levels.push_back({ grid->Level(), grid->Encode() });
}

void TAutoMap::FlushGrid()
{
    StoreGrid();
}

// REVSYNC: 0x0052c3d0 -- the grid is dropped unsaved and the new player's
// loaded.
void TAutoMap::BindPlayer(TPlayer* player)
{
    travels = nullptr;
    if (!player || !IsOpen())
        return;
    grid.reset();
    BindTravels(*player);
    LoadGrid(MapPane.GetMapLevel());
    SetDirty(true);
}

void TAutoMap::RecordTravels()
{
    if (!Player || !IsOpen())
        return;
    const int32_t level = MapPane.GetMapLevel();
    prevtilex = tilex;
    prevtiley = tiley;
    tilex = scrollx >> kTileShift;
    tiley = scrolly >> kTileShift;
    prevplayerx = playerx;
    prevplayery = playery;
    Player->GetScreenPos(playerx, playery);
    if (!travels)
        BindTravels(*Player);
    if (grid && grid->Level() != level)
    {
        StoreGrid();
        grid.reset();
    }
    if (!grid)
        LoadGrid(level);
    if (playerx == prevplayerx && playery == prevplayery)
        return;
    for (int32_t dx = -kExploreReach; dx <= kExploreReach; dx += kExploreStep)
        for (int32_t dy = -kExploreReach; dy <= kExploreReach; dy += kExploreStep)
            grid->Explore(playerx + dx, playery + dy);
}

// Mosaic px to window px at the zoom (retail's integer arithmetic).
int32_t TAutoMap::ToScreen(int32_t mosaicpx) const
{
    return mosaicpx * zoom * kWindowSize / ((kMosaicSize - kTileSize) * 100);
}

// Half the window, in mosaic px, at the zoom.
int32_t TAutoMap::HalfSpan() const
{
    return (kMosaicSize - kTileSize) * 100 * kWindowSize / (zoom * kWindowSize) / 2;
}

// REVSYNC: 0x0052a280 -- following the camera, the window centres on the map
// view's centre every pulse; dragged off it, it keeps its centre through a
// zoom. Then the tiles and the fog catch up.
void TAutoMap::Pulse()
{
    if (++pulses >= kPulsesPerMarkerFrame)
    {
        pulses = 0;
        markerframe = (markerframe + 1) % kMarkerFrames;
    }
    int32_t centrex = 0, centrey = 0;
    if (freescroll)
    {
        centrex = scrollx + HalfSpan();
        centrey = scrolly + HalfSpan();
    }
    const int32_t oldzoom = zoom;
    if (zoomin)
        zoom = (std::min)(zoom + kZoomStep, kZoomMax);
    else if (zoomout)
        zoom = (std::max)(zoom - kZoomStep, kZoomMin);
    if (!freescroll)
    {
        int32_t viewx = 0, viewy = 0;
        MapPane.GetViewCentre(viewx, viewy);
        scrollx = (viewx >> kMosaicShift) - HalfSpan();
        scrolly = (viewy >> kMosaicShift) - HalfSpan();
    }
    else if (zoom != oldzoom)
    {
        scrollx = centrex - HalfSpan();
        scrolly = centrey - HalfSpan();
    }
    RebuildTileCache();
    BuildFog();
    SetDirty(true);
}

// REVSYNC: 0x0052aa70 -- when the ring moved or the level changed: tiles still
// in the ring stay (a missing one isn't looked for again), the rest go, and
// the new ones are loaded and stamped -- a missing tile black.
void TAutoMap::RebuildTileCache()
{
    const int32_t level = MapPane.GetMapLevel();
    if (tilex == prevtilex && tiley == prevtiley && level == cachelevel)
        return;
    std::array<STile, kRing * kRing> kept{};
    for (int32_t i = 0; i < kRing; ++i)
        for (int32_t j = 0; j < kRing; ++j)
        {
            STile& tile = tiles[i * kRing + j];
            const int32_t ni = prevtilex - tilex + i;
            const int32_t nj = prevtiley - tiley + j;
            if (cachelevel == level && ni >= 0 && ni < kRing && nj >= 0 && nj < kRing)
                kept[ni * kRing + nj] = { tile.texture, true };
            else if (tile.texture != kInvalidTexture && Renderer)
                Renderer->ReleaseTextureAssetRef(tile.texture);
            tile = {};
        }
    tiles = kept;
    for (int32_t i = 0; i < kRing; ++i)
        for (int32_t j = 0; j < kRing; ++j)
        {
            STile& tile = tiles[i * kRing + j];
            if (tile.texture != kInvalidTexture || tile.kept)
                continue;
            const int32_t x = tilex + i, y = tiley + j;
            tile.texture = LoadTile(level, x, y);
            pending.push_back({ Wrap(x * kTileSize, kMosaicSize), Wrap(y * kTileSize, kMosaicSize), tile.texture });
        }
    cachelevel = level;
}

void TAutoMap::StampPendingTiles()
{
    if (!mosaicfresh && pending.empty())
        return;
    if (mosaicfresh)
        mosaic->StartPass(0.0f, 0.0f, 0.0f, 1.0f);
    else
        mosaic->ContinuePass();
    mosaicfresh = false;
    for (const SStamp& stamp : pending)
        if (stamp.texture != kInvalidTexture)
            Renderer->Composite(stamp.texture, stamp.x, stamp.y, kTileSize, kTileSize, kMosaicSize, kMosaicSize, 0, 0,
                                kTileSize, kTileSize, kTileSize, kTileSize);
        else
            Renderer->DrawSolidRectToTarget(stamp.x, stamp.y, kTileSize, kTileSize, kMosaicSize, kMosaicSize, 0, 0, 0);
    pending.clear();
    mosaic->EndPass();
}

// REVSYNC: 0x0052af00 -- a vertex every N 128 px cells (N to fit ten across
// the window), black, as opaque as the share of unexplored cells it samples:
// at N = 1 the cell to its lower right, at N = 2 the four meeting at it (N 3
// and 4 need a zoom under 100). Recounted when the zoom, the snapped origin
// or the player moved; placed every pulse.
void TAutoMap::BuildFog()
{
    const int32_t denom = (kMosaicSize - kTileSize) * 100;
    const int32_t span = (kMosaicSize - kTileSize) * 100 * kWindowSize / (zoom * kWindowSize);
    const int32_t cells = (((span * 8 + 0x78) >> 7) + 7) / 8;
    const int32_t step = cells << 7;
    const int32_t pitch = ((step >> kMosaicShift) * zoom * kWindowSize) / denom;
    const int32_t x8 = scrollx << kMosaicShift, y8 = scrolly << kMosaicShift;
    const int32_t modx = MosaicOffset(x8, step), mody = MosaicOffset(y8, step);
    const int32_t x0 = x8 - modx, y0 = y8 - mody;
    const bool recount = zoom != fogzoom || (std::llabs(int64_t(fogx) - x0) >> 7) >= cells
                      || (std::llabs(int64_t(fogy) - y0) >> 7) >= cells || playerx != prevplayerx
                      || playery != prevplayery;
    const int32_t offx = (((modx >> kMosaicShift) * zoom * kWindowSize) / denom);
    const int32_t offy = (((mody >> kMosaicShift) * zoom * kWindowSize) / denom);
    const auto unexplored = [this](int32_t x, int32_t y) { return grid && grid->Explored(x, y) ? 0 : 1; };
    for (int32_t row = 0; row < kFogSide; ++row)
    {
        const int32_t y = y0 + row * step, ym = y - 0x80;
        for (int32_t col = 0; col < kFogSide; ++col)
        {
            const int32_t x = x0 + col * step, xm = x - 0x80;
            float& f = fog[col * kFogSide + row];
            if (recount)
            {
                fogx = x0;
                fogy = y0;
                switch (cells)
                {
                case 2:
                    f = 0.25f * float(unexplored(xm, ym) + unexplored(x, y) + unexplored(xm, y) + unexplored(x, ym));
                    break;
                case 3:
                {
                    int32_t count = 0;
                    for (const int32_t sx : { xm, x, xm + 0x100 })
                        for (const int32_t sy : { ym, y, ym + 0x100 })
                            count += unexplored(sx, sy);
                    f = float(count) / 9.0f;
                    break;
                }
                case 4:
                {
                    int32_t count = 0;
                    for (const int32_t sx : { xm - 0x80, xm, x, xm + 0x100 })
                        for (const int32_t sy : { ym - 0x80, ym, y, ym + 0x100 })
                            count += unexplored(sx, sy);
                    f = float(count) / 16.0f;
                    break;
                }
                default:
                    f = float(unexplored(x, y));
                    break;
                }
            }
            TRenderer::SColorVertex2D& v = fogvertices[row * kFogSide + col];
            v.x = float(kWindowLeft - offx + col * pitch) + kD3DPixelCentre;
            v.y = float(kWindowTop - offy + row * pitch) + kD3DPixelCentre;
            v.r = v.g = v.b = 0;
            v.a = static_cast<uint8_t>(static_cast<int32_t>(f * 255.0f));
        }
    }
    fogzoom = zoom;
}

// REVSYNC: 0x0052a5f0 -- the map, then (with the fog of war) the markers and
// the fog over them, then the frame.
void TAutoMap::Compose()
{
    if (!IsDirty() || !Renderer || !mosaic)
        return;
    SetDirty(false);
    StampPendingTiles();
    if (!layer)
        layer = std::make_unique<TSurface>(kWidth, kHeight, SG_PIXELFORMAT_RGBA8);
    layer->StartPass(0.0f, 0.0f, 0.0f, 0.0f);
    ComposeBody(kWidth, kHeight);
    if (fogofwar)
    {
        ComposeMarkers(kWidth, kHeight);
        ComposeFog(kWidth, kHeight);
    }
    ComposeFrame(kWidth, kHeight);
    layer->EndPass();
}

void TAutoMap::Draw()
{
    if (layer && Renderer)
        Renderer->DrawSurface(layer.get(), GetPosX(), GetPosY());
}

// REVSYNC: 0x0052a830 -- the mosaic, scaled, four times over so the torus
// fills the window wherever the scroll is.
void TAutoMap::ComposeBody(int32_t target_w, int32_t target_h)
{
    const int32_t size = ToScreen(kMosaicSize);
    const int32_t left = kWindowLeft - ToScreen(MosaicOffset(scrollx, kMosaicSize));
    const int32_t top = kWindowTop - ToScreen(MosaicOffset(scrolly, kMosaicSize));
    Renderer->SetTargetClip(kWindowLeft, kWindowTop, kWindowSize, kWindowSize);
    for (const int32_t x : { left, left + size })
        for (const int32_t y : { top, top + size })
            Renderer->DrawSurfaceStretchedToTarget(mosaic.get(), float(x) + kD3DPixelCentre,
                                                   float(y) + kD3DPixelCentre, float(size), float(size), 0, 0,
                                                   kMosaicSize, kMosaicSize, target_w, target_h);
    Renderer->ResetTargetClip(target_w, target_h);
}

// REVSYNC: 0x0052a620 -- the living, visible characters near the player.
void TAutoMap::ComposeMarkers(int32_t target_w, int32_t target_h)
{
    if (!Player)
        return;
    S3DPoint pos;
    Player->GetPos(pos);
    Renderer->SetTargetClip(kWindowLeft, kWindowTop, kWindowSize, kWindowSize);
    for (TMapIterator i(pos, kNearRange, CHECK_NOINVENT | CHECK_MAPRECT, OBJSET_CHARACTER); i.Item(); i++)
    {
        TObjectInstance* object = i.Item();
        if (!object->IsCharacter() || static_cast<TCharacter*>(object)->IsDead()
            || (object->GetFlags() & OF_INVISIBLE))
            continue;
        int32_t sx = 0, sy = 0;
        object->GetScreenPos(sx, sy);
        const int32_t x = ToScreen((sx >> kMosaicShift) - scrollx) + kWindowLeft;
        const int32_t y = ToScreen((sy >> kMosaicShift) - scrolly) + kWindowTop;
        if (x < kWindowLeft || x >= kMarkerRight || y < kWindowTop || y >= kMarkerBottom)
            continue;
        const int32_t row = object == Player ? kMarkerPlayerRow
                          : static_cast<TCharacter*>(object)->IsEnemy(Player) ? kMarkerEnemyRow
                                                                              : kMarkerOtherRow;
        Renderer->DrawBitmapSubrectToTarget(marker, x, y, markerframe * kMarkerSize, row, kMarkerSize, kMarkerSize,
                                            target_w, target_h);
    }
    Renderer->ResetTargetClip(target_w, target_h);
}

// REVSYNC: 0x0052b520 -- the fog's triangles, blended by their alpha.
void TAutoMap::ComposeFog(int32_t target_w, int32_t target_h)
{
    Renderer->SetTargetClip(kWindowLeft, kWindowTop, kWindowSize, kWindowSize);
    Renderer->DrawColoredTrianglesToTarget(fogvertices.data(), static_cast<int32_t>(fogvertices.size()),
                                           kFogIndexList.data(), kFogIndices, target_w, target_h);
    Renderer->ResetTargetClip(target_w, target_h);
}

// REVSYNC: 0x0052bdb0 -- the frame; PlusSel and MinusSel while held.
void TAutoMap::ComposeFrame(int32_t target_w, int32_t target_h)
{
    Renderer->DrawBitmapToTarget(frame, 0, 0, target_w, target_h);
    if (zoomin)
        Renderer->DrawBitmapToTarget(plussel, kPlusLeft, kButtonTop, target_w, target_h);
    if (zoomout)
        Renderer->DrawBitmapToTarget(minussel, kMinusLeft, kButtonTop, target_w, target_h);
}

void TAutoMap::ScrollZoom(int32_t units)
{
    const int64_t accumulated = int64_t(wheelRemainder) + units;
    const int32_t centrex = scrollx + HalfSpan(), centrey = scrolly + HalfSpan();
    zoom = int32_t((std::clamp)(int64_t(zoom) + accumulated / 120 * kZoomStep,
                              int64_t(kZoomMin), int64_t(kZoomMax)));
    wheelRemainder = int32_t(accumulated % 120);
    if (freescroll)
    {
        scrollx = centrex - HalfSpan();
        scrolly = centrey - HalfSpan();
    }
    RebuildTileCache();
    SetDirty(true);
}

void TAutoMap::MouseClick(int32_t button, int32_t x, int32_t y)
{
    const auto on = [&](int32_t left) {
        return x > left && x < left + plussel->width && y > kButtonTop && y < kButtonTop + plussel->height;
    };
    const bool inring = (y - kRingY) * (y - kRingY) + (x - kRingX) * (x - kRingX) < kRingRadiusSquared;
    if (button == MB_LEFTDOWN)
    {
        if (on(kPlusLeft))
            zoomin = true;
        else if (on(kMinusLeft))
            zoomout = true;
        else if (inring)
        {
            lastmousex = x;
            lastmousey = y;
            freescroll = true;
        }
    }
    else if (button == MB_LEFTUP)
    {
        zoomin = zoomout = false;
        lastmousex = x;
        lastmousey = y;
    }
    else if (button == MB_RIGHTUP)
        freescroll = false;
    SetDirty(true);
}

// REVSYNC: 0x0052c2c0 -- dragging inside the ring moves the map a mosaic px
// a pointer px, whatever the zoom.
void TAutoMap::MouseMove(int32_t button, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= GetWidth() || y >= GetHeight() || button != MB_LEFTDOWN)
        return;
    if ((y - kRingY) * (y - kRingY) + (x - kRingX) * (x - kRingX) >= kRingRadiusSquared)
        return;
    scrollx += lastmousex - x;
    lastmousex = x;
    scrolly += lastmousey - y;
    lastmousey = y;
    SetDirty(true);
}

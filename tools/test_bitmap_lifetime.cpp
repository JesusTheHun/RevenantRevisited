// The generated fixture appends the real cache key, texture-cache methods,
// and TDefPane::ReleaseAssets. Doubles supply storage, decoding and GPU upload.
#include <gtest/gtest.h>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Buffer {
    void* value = nullptr;
    void* ptr() const { return value; }
};
struct TBitmap {
    int width = 1, height = 1, regx = 0, regy = 0, flags = 0, drawmode = 0, keycolor = 0;
    int datasize = 4, aliassize = 0, alphasize = 0, palettesize = 0;
    uint8_t* data8 = nullptr;
    Buffer alias, alpha, palette;
    std::array<uint8_t,4> color{255,0,0,255};
    bool decodable = true;
};
using PTBitmap = TBitmap*;
using TTextureHandle = uint32_t;
constexpr TTextureHandle kInvalidTexture = 0;
constexpr uint64_t kFnvOffset64 = 1469598103934665603ull, kFnvPrime64 = 1099511628211ull;
uint64_t UIBitmapAtlasKey(PTBitmap, EBitmapDecode);
enum class ERendererTextureFormat { RGBA8 };
enum class ERendererTextureFilter { Nearest };
inline void log_warn(const char*, ...) {}
inline bool DecodeBitmapToRGBA(TBitmap* bm, uint8_t* out, int, int, int, EBitmapDecode) {
    std::copy(bm->color.begin(), bm->color.end(), out);
    return bm->decodable;
}

class TRenderer {
public:
    struct SBitmapTexture { PTBitmap source; TTextureHandle texture; };
    std::unordered_map<uint64_t,SBitmapTexture> bitmap_texture_cache;
    std::unordered_map<uint64_t,TTextureHandle> texture_by_key;
    std::unordered_map<TTextureHandle,std::array<uint8_t,4>> uploaded;
    std::unordered_set<TTextureHandle> alive;
    TTextureHandle next = 1;
    void ReleaseBitmapTextures(PTBitmap);
    TTextureHandle BitmapAsTexture(PTBitmap, EBitmapDecode);
    TTextureHandle RegisterTextureAsset(uint64_t key, const void* data, size_t, int, int,
                                        ERendererTextureFormat, uint64_t, ERendererTextureFilter) {
        if (auto it = texture_by_key.find(key); it != texture_by_key.end()) return it->second;
        const auto id = next++;
        const auto* rgba = static_cast<const uint8_t*>(data);
        std::copy(rgba, rgba + 4, uploaded[id].begin());
        alive.insert(id);
        texture_by_key[key] = id;
        return id;
    }
    void DestroyDynamicTexture(TTextureHandle id) { alive.erase(id); }
};
inline TRenderer* Renderer = nullptr;
struct TSurface {};
struct TMulti {
    int32_t numoffsets = 1;
    TBitmap bitmap;
    TBitmap* Bitmap(int32_t) { return &bitmap; }
};
// Retain this allocation so the next archive is guaranteed to reuse exactly
// its bitmap address. Check eviction occurs before the source is released.
inline int retired = 0;
inline void RetireArchive(TMulti* archive) {
    for (const auto& [key, cached] : Renderer->bitmap_texture_cache)
        EXPECT_NE(cached.source, &archive->bitmap);
    ++retired;
}
class TDefPane {
public:
    TSurface* surface = nullptr;
    std::map<std::string,TMulti*> archives;
    std::vector<TMulti*> bitmapDats;
    std::vector<int> widgets, styles, defines;
    TBitmap* background = nullptr;
    bool open = false, finishing = false, endRequested = false, draggingBar = false;
    int defflags = 0, arrowBar = -1, arrowStep = 0, focusWidget = -1;
    void ReleaseAssets();
};

namespace {
class BitmapLifetime : public testing::Test {
protected:
    TRenderer renderer;
    void SetUp() override { Renderer = &renderer; retired = 0; }
    void TearDown() override { Renderer = nullptr; }
};

TEST_F(BitmapLifetime, ClosingPaneEvictsAllDecodeVariantsBeforeReusingStorage) {
    TMulti storage;
    TDefPane pane;
    pane.archives["menus.dat"] = &storage;
    TBitmap unrelated;
    const auto retained = renderer.BitmapAsTexture(&unrelated, EBitmapDecode::Pixels);
    const auto key = UIBitmapAtlasKey(&storage.bitmap, EBitmapDecode::Pixels);
    std::vector<TTextureHandle> old;
    for (auto decode : {EBitmapDecode::Pixels,EBitmapDecode::Unkeyed,EBitmapDecode::Alias,
                        EBitmapDecode::PixelsAndAlias,EBitmapDecode::MagentaKeyed,EBitmapDecode::Overlay4444})
        old.push_back(renderer.BitmapAsTexture(&storage.bitmap, decode));
    pane.ReleaseAssets();
    EXPECT_EQ(retired, 1);
    EXPECT_EQ(renderer.bitmap_texture_cache.size(), 1);
    for (auto id : old) EXPECT_EQ(renderer.alive.count(id), 0);
    EXPECT_EQ(renderer.alive.count(retained), 1);
    storage.bitmap.color = {0,0,255,255}; // same addresses and metadata; a new screen's pixels
    EXPECT_EQ(UIBitmapAtlasKey(&storage.bitmap,EBitmapDecode::Pixels), key);
    const auto fresh = renderer.BitmapAsTexture(&storage.bitmap,EBitmapDecode::Pixels);
    EXPECT_NE(fresh, old[0]);
    EXPECT_EQ(renderer.uploaded[fresh], storage.bitmap.color);
}

TEST_F(BitmapLifetime, FailedDecodeDoesNotPoisonANewArchiveAtTheSameAddress) {
    TMulti storage;
    TDefPane pane;
    pane.archives["old.dat"] = &storage;
    storage.bitmap.decodable = false;
    EXPECT_EQ(renderer.BitmapAsTexture(&storage.bitmap,EBitmapDecode::Pixels), kInvalidTexture);
    pane.ReleaseAssets();
    storage.bitmap.decodable = true;
    EXPECT_NE(renderer.BitmapAsTexture(&storage.bitmap,EBitmapDecode::Pixels), kInvalidTexture);
}

TEST_F(BitmapLifetime, RepeatedCloseAndUncachedBitmapsAreSafe) {
    TMulti storage;
    TDefPane pane;
    pane.archives["unused.dat"] = &storage;
    renderer.ReleaseBitmapTextures(&storage.bitmap);
    pane.ReleaseAssets();
    pane.ReleaseAssets();
    EXPECT_EQ(retired, 1);
    EXPECT_TRUE(renderer.bitmap_texture_cache.empty());
}
} // namespace

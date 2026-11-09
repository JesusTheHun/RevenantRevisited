// Only the reader's dependencies are doubled; its declarations and bodies
// are copied verbatim from src/ by scroll_reader_fixture.py.
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define _CLASSDEF(name) class name; using P##name = name*;
#ifdef _WIN32
#define stricmp _stricmp
#else
#include <strings.h>
#define stricmp strcasecmp
#endif
constexpr int WIDTH = 640, HEIGHT = 480, CONTROL_CLICKED = 1;
constexpr int VK_ESCAPE = 27, VK_PRIOR = 33, VK_NEXT = 34;
constexpr int VK_HOME = 36, VK_END = 35, VK_UP = 38, VK_DOWN = 40;
struct SColor { uint8_t red, green, blue; };
struct TBitmap { int width = 570, height = 374; };
using PTBitmap = TBitmap*;
class TMulti {
public:
    static inline bool missingArchive = false, missingPaper = false;
    static inline int live = 0;
    TBitmap paper, button;
    TMulti() { ++live; }
    ~TMulti() { --live; }
    static TMulti* LoadMulti(const char* name) {
        if (missingArchive) return nullptr;
        auto* data = new TMulti;
        if (std::string(name) == "book.dat") data->paper = {602, 426};
        return data;
    }
    TBitmap* FindBitmap(const char*) { return missingPaper ? nullptr : &paper; }
    TBitmap* Bitmap(const char*) { return &button; }
};
struct SFontAtlas {};
struct TGenericFont { SColor color{74, 40, 16}; int lextra = 2; };
class TFontTable {
public:
    bool missing = false;
    std::string requested;
    SFontAtlas atlas;
    TGenericFont font;
    const SFontAtlas* Atlas(const char* name) { requested = name; return missing ? nullptr : &atlas; }
    TGenericFont* FindFont(const char*) { return &font; }
};
inline TFontTable fonts;
inline TFontTable* FontTable = &fonts;
inline float TextLineHeight(const SFontAtlas*) { return 18; }
enum class ETextEncoding { Windows1252, Utf8 };
inline constexpr auto kGameTextEncoding = ETextEncoding::Windows1252;
inline float TextWidth(const SFontAtlas*, const char* text, ETextEncoding) { return std::strlen(text) * 10.0f; }
int32_t WrapTextLines(const SFontAtlas*, const char*, float, std::vector<std::string>&,
                      ETextEncoding = kGameTextEncoding);
enum class ETextAlign { Left, Center };
struct TextDraw { std::string text; int x, y, w, h; ETextAlign align; float r, g, b; };
inline std::vector<TextDraw> textDraws;
inline void DrawTextToTarget(const SFontAtlas*, const char* text, int x, int y, int w, int h,
                              ETextAlign align, float r, float g, float b, int, int) {
    textDraws.push_back({text, x, y, w, h, align, r, g, b});
}
struct TRenderer {
    int paperDraws = 0;
    void DrawBitmapToTarget(TBitmap*, int, int, int, int) { ++paperDraws; }
};
inline TRenderer renderer;
inline TRenderer* Renderer = &renderer;
struct TDisplay { int Width() const { return 1920; } int Height() const { return 1080; } };
inline TDisplay Display;
struct TButton {
    bool hidden = false;
    int key;
    explicit TButton(int k) : key(k) {}
    void Show() { hidden = false; }
    void Hide() { hidden = true; }
};
class TPane {
public:
    bool open = false, dirty = false;
    int x = 0, y = 0, width = 0, height = 0, ends = 0;
    virtual ~TPane() = default;
    virtual bool Initialize() { open = true; return true; }
    virtual void Close() { open = false; }
    bool IsOpen() const { return open; }
    void Resize(int a, int b, int w, int h) { x = a; y = b; width = w; height = h; }
    void PaneResized() {}
    void SetDirty(bool state) { dirty = state; }
    void EndModal(int) { ++ends; }
    virtual void KeyPress(int32_t, bool) {}
    virtual void OnCanvasResize(int32_t, int32_t) {}
    virtual void OnScreenEvent(int32_t, void*) {}
};
using PTPane = TPane*;
class TButtonPane : public TPane {
public:
    std::vector<std::unique_ptr<TButton>> buttons;
    TButtonPane(int x, int y, int w, int h) { Resize(x, y, w, h); }
    void Close() override { buttons.clear(); TPane::Close(); }
    void NewButton(const char*, int, int, int, int, int key, void(*)(), TBitmap*, TBitmap*) {
        buttons.push_back(std::make_unique<TButton>(key));
    }
    TButton* Button(int i) { return buttons.at(i).get(); }
    virtual void OnControl(TButton*, int32_t) {}
    void KeyPress(int32_t key, bool down) override {
        if (down) for (const auto& button : buttons)
            if (button->key == key && !button->hidden) OnControl(button.get(), CONTROL_CLICKED);
    }
    void Compose() { ComposeBackground(width, height); }
protected:
    virtual void ComposeBackground(int32_t, int32_t) {}
};
class TObjectInstance {};
class TScroll : public TObjectInstance {
public:
    std::string text, type = "Scroll";
    const char* GetText() const { return text.c_str(); }
    const char* GetTypeName() const { return type.c_str(); }
    bool Use(TObjectInstance*, int32_t with = -1);
};
using PTScroll = TScroll*;
struct TScreen {
    enum { MODAL_GAME = 15, MODAL_ANIMATE = 16, SCREENEVENT_CLOSING = 0x100 };
};
class TPlayScreen : public TScreen {
public:
    PTPane nextpane = nullptr, modal = nullptr;
    bool rejectPush = false;
    uint32_t flags = 0;
    int redraws = 0;
    std::function<void(int32_t)> done;
    void SetNextPane(PTPane pane) { nextpane = pane; }
    void OpenNextPane();
    bool PushModal(PTPane pane, uint32_t f, std::function<void(int32_t)> cb) {
        if (rejectPush) return false;
        EXPECT_TRUE(pane->IsOpen());
        modal = pane; flags = f; done = std::move(cb); return true;
    }
    void Redraw() { ++redraws; }
    void Finish() { modal = nullptr; if (done) { auto cb = std::move(done); done = {}; cb(0); } }
};
inline TPlayScreen PlayScreen;
inline void log_error(const char*) {}

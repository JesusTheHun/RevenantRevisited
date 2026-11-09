// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                      scroll.h - TScroll module                        *
// *************************************************************************

#pragma once

#include "revenant.h"
#include "multi.h"
#include "object.h"
#include "button.h"
#include "font.h"

#include <memory>
#include <string>
#include <vector>

_CLASSDEF(TScroll)
class TScroll final : public TObjectInstance
{
  public:
    TScroll(TObjectImagery* newim) : TObjectInstance(newim) { text = nullptr; }
    TScroll(SObjectDef* def, TObjectImagery* newim) : TObjectInstance(def, newim) { text = nullptr; }

    _NODEFAULTCONS(TScroll);

    char *GetText() { return text; }
    void SetText(char *newtext);

    virtual bool Use(TObjectInstance* user, int32_t with = -1);
        // Read scroll
    int32_t CursorType(TObjectInstance* inst = nullptr) override { if (inst) return CURSOR_NONE; return CURSOR_EYE; }
        // Indicate you can read the scroll

    virtual void Load(RTInputStream is, int32_t version, int32_t objversion);
    virtual void Save(RTOutputStream os);

    // Scroll stats
    STATFUNC(Value)

  private:
    char *text;             // Text written on the scroll
};

DEFINE_BUILDER("SCROLL", TScroll)

// ***************
// * TScrollPane *
// ***************

_CLASSDEF(TScrollPane)
class TScrollPane : public TButtonPane
{
  public:
    TScrollPane() : TScrollPane(false) { }

    bool Initialize() override;
    void Close() override;
    void KeyPress(int32_t key, bool down) override;
    void OnCanvasResize(int32_t width, int32_t height) override;
    void OnScreenEvent(int32_t code, void* param) override;
    void OnControl(TButton* button, int32_t message) override;

    void SetScroll(PTScroll scroll);
    void Scroll(int32_t delta);

  protected:
    explicit TScrollPane(bool isbook) : TButtonPane(0, 0, WIDTH, HEIGHT), book(isbook) { }
    void ComposeBackground(int32_t target_w, int32_t target_h) override;

  private:
    void LayoutText();
    void UpdateButtons();
    int32_t VisibleLines() const { return linesperpage * (book ? 2 : 1); }

    const bool book;
    std::unique_ptr<TMulti> scrolldata;
    PTBitmap paper = nullptr;
    const SFontAtlas* scrollfont = nullptr;
    SColor textcolor{255, 255, 255};
    std::string text;
    std::vector<std::string> lines;
    int32_t line = 0;
    int32_t lineheight = 1;
    int32_t linesperpage = 1;
};

_CLASSDEF(TBookPane)
class TBookPane final : public TScrollPane
{
  public:
    TBookPane() : TScrollPane(true) { }
};

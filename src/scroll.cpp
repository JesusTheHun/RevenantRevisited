// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                     scroll.cpp - TScroll module                       *
// *************************************************************************

#include "revenant.h"
#include "scroll.h"
#include "display.h"
#include "dialog.h"
#include "renderer.h"
#include "bitmap.h"
#include "font.h"
#include "fonttable.h"
#include "playscreen.h"

#include <algorithm>

REGISTER_BUILDER(TScroll)
TObjectClass ScrollClass("SCROLL", OBJCLASS_SCROLL, 0);

// Hard coded class stats
DEFSTAT(Scroll, Value, VAL, 0, 0, 0, 1000000)

TScrollPane ScrollPane;
TBookPane BookPane;

void TScroll::SetText(char *newtext)
{
    if (text)
        delete text;

    text = new char[strlen(newtext)+1];
    if (text)
        strcpy(text, newtext);
}

void TScroll::Load(RTInputStream is, int32_t version, int32_t objversion)
{
    TObjectInstance::Load(is, version, objversion);

    short len;
    is >> len;

    if (len < 1)
        text = nullptr;
    else
    {
        text = new char[len+1];

        if (text)
        {
            int32_t i;
            for (i = 0; i < len; i++)
                is >> text[i];

            text[i] = 0;
        }
    }
}

void TScroll::Save(RTOutputStream os)
{
    TObjectInstance::Save(os);

    short len = text ? strlen(text) : 0;
    os << len;

    if (len)
    {
        for (int32_t i = 0; i < len; i++)
            os << text[i];
    }
}

bool TScroll::Use(TObjectInstance* user, int32_t with)
{
    if (with < 0)
    {
        PTScrollPane pane = &ScrollPane;

        if (!stricmp(GetTypeName(), "Book"))
            pane = &BookPane;

        if (pane->IsOpen())
            return false;
        pane->SetScroll(this);
        PlayScreen.SetNextPane(pane);
        return true;
    }

    return false;
}

// ***************
// * TScrollPane *
// ***************

bool TScrollPane::Initialize()
{
    if (IsOpen())
        return true;

    OnCanvasResize(Display.Width(), Display.Height());
    TButtonPane::Initialize();
    scrolldata.reset(TMulti::LoadMulti(book ? "book.dat" : "scroll.dat"));
    const char* fontname = book ? "Book" : "Scroll";
    scrollfont = FontTable ? FontTable->Atlas(fontname) : nullptr;
    const TGenericFont* font = FontTable ? FontTable->FindFont(fontname) : nullptr;
    if (!scrolldata || !scrollfont || !font)
    {
        Close();
        return false;
    }

    paper = scrolldata->FindBitmap(book ? "book" : "scroll");
    if (!paper)
    {
        Close();
        return false;
    }
    textcolor = font->color;
    lineheight = std::max(1, static_cast<int32_t>(TextLineHeight(scrollfont)) + font->lextra);
    linesperpage = book ? 16 : std::max(1, 230 / lineheight);
    const int32_t x = (WIDTH - paper->width) / 2;
    const int32_t y = (HEIGHT - paper->height) / (book ? 3 : 4);
    NewButton("down", x + (book ? 440 : 452), y + (book ? 386 : 330), 28, book ? 22 : 31,
              VK_NEXT, nullptr, scrolldata->Bitmap("downdown"), scrolldata->Bitmap("downup"));
    NewButton("up", x + (book ? 472 : 484), y + (book ? 379 : 323), 28, book ? 23 : 31,
              VK_PRIOR, nullptr, scrolldata->Bitmap("updown"), scrolldata->Bitmap("upup"));
    NewButton("exit", x + (book ? 505 : 518), y + (book ? 377 : 321), 28, book ? 30 : 31,
              VK_ESCAPE, nullptr, scrolldata->Bitmap("exitdown"), scrolldata->Bitmap("exitup"));
    LayoutText();
    return true;
}

void TScrollPane::Close()
{
    // Buttons borrow archive bitmaps; release them before their archive.
    TButtonPane::Close();
    scrolldata.reset();
    paper = nullptr;
    scrollfont = nullptr;
    text.clear();
    lines.clear();
    line = 0;
}

void TScrollPane::OnCanvasResize(int32_t width, int32_t height)
{
    // Keep the retail layout centered, including its input hit rectangles.
    Resize(std::max(0, (width - WIDTH) / 2), std::max(0, (height - HEIGHT) / 2), WIDTH, HEIGHT);
    PaneResized();
}

void TScrollPane::OnScreenEvent(int32_t code, void* param)
{
    if (code == TScreen::SCREENEVENT_CLOSING)
        Close();
}

void TScrollPane::ComposeBackground(int32_t target_w, int32_t target_h)
{
    if (!paper || !scrollfont || !Renderer)
        return;
    const int32_t x = (WIDTH - paper->width) / 2;
    const int32_t y = (HEIGHT - paper->height) / (book ? 3 : 4);
    Renderer->DrawBitmapToTarget(paper, x, y, target_w, target_h);

    for (int32_t side = 0; side < (book ? 2 : 1); ++side)
    {
        if (book)
        {
            const std::string number = std::to_string(line / linesperpage + side + 1);
            DrawTextToTarget(scrollfont, number.c_str(), x + 180 + side * 232, y + 21 + side * 2,
                             0, lineheight, ETextAlign::Center, textcolor.red / 255.0f,
                             textcolor.green / 255.0f, textcolor.blue / 255.0f, target_w, target_h);
        }
        for (int32_t row = 0; row < linesperpage; ++row)
        {
            const int32_t index = line + side * linesperpage + row;
            if (index >= static_cast<int32_t>(lines.size()))
                break;
            DrawTextToTarget(scrollfont, lines[index].c_str(), x + (book ? 82 + side * 232 : 65),
                             y + (book ? 40 + side * 2 : 68) + row * lineheight,
                             book ? 216 : 430, lineheight, ETextAlign::Left,
                             textcolor.red / 255.0f, textcolor.green / 255.0f, textcolor.blue / 255.0f,
                             target_w, target_h);
        }
    }
}

void TScrollPane::OnControl(TButton* button, int32_t message)
{
    if (message != CONTROL_CLICKED)
        return;
    if (button == Button(0))
        Scroll(VisibleLines());
    else if (button == Button(1))
        Scroll(-VisibleLines());
    else if (button == Button(2))
        EndModal(0); // The completion closes the pane after input dispatch.
}

void TScrollPane::KeyPress(int32_t key, bool down)
{
    TButtonPane::KeyPress(key, down);
    if (!down)
        return;
    switch (key)
    {
    case VK_UP:   Scroll(-1); break;
    case VK_DOWN: Scroll(1); break;
    case VK_HOME: Scroll(-static_cast<int32_t>(lines.size())); break;
    case VK_END:  Scroll(static_cast<int32_t>(lines.size())); break;
    }
}

void TScrollPane::SetScroll(PTScroll scroll)
{
    // The object may be deleted by a subsequent load; the reader owns its text.
    text = scroll && scroll->GetText() ? scroll->GetText() : "";
    // Retail scrolls can store a short language-table tag instead of prose.
    if (!text.empty() && text.size() < 16)
    {
        const int32_t id = DialogList.FindLine(text.c_str());
        if (id >= 0)
            text = DialogList.GetLine(id);
    }
    LayoutText();
}

void TScrollPane::LayoutText()
{
    lines.clear();
    line = 0;
    if (scrollfont)
        WrapTextLines(scrollfont, text.c_str(), book ? 216.0f : 430.0f, lines);
    if (IsOpen() && paper)
        UpdateButtons();
    SetDirty(true);
}

void TScrollPane::UpdateButtons()
{
    if (line + VisibleLines() < static_cast<int32_t>(lines.size()))
        Button(0)->Show();
    else
        Button(0)->Hide();
    if (line > 0)
        Button(1)->Show();
    else
        Button(1)->Hide();
}

void TScrollPane::Scroll(int32_t delta)
{
    const int32_t count = static_cast<int32_t>(lines.size());
    // A book ends on its last spread; a scroll keeps its last page filled.
    const int32_t last = book ? std::max(0, (count - 1) / VisibleLines()) * VisibleLines()
                              : std::max(0, count - VisibleLines());
    const int32_t next = static_cast<int32_t>(std::clamp<int64_t>(int64_t(line) + delta, 0, last));
    if (next != line)
    {
        line = next;
        UpdateButtons();
        SetDirty(true);
    }
}

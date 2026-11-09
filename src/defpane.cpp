// *************************************************************************
// *                  Revenant Revisited (port) - 2026                     *
// *  defpane.cpp - DEF widget pane (TDefPane)                              *
// *************************************************************************
//
// Parses Revenant's `.def` UI layout files + the shared `widgets.def` style
// library and renders the resulting widget tree into a TSurface. See
// defpane.h for the architecture, docs/ui/forensics/DEF_ENGINE.md for how
// retail draws each widget.

#include "defpane.h"

#include "font.h"
#include "fonttable.h"
#include "logging.h"
#include "multi.h"
#include "renderer.h"
#include "revdefs.h"    // MB_LEFTDOWN / MB_LEFTUP
#include "revenant.h"   // ResourcePath
#include "revutils.h"   // rev_read_file
#include "sound.h"
#include "surface.h"
#include "time.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// =====================================================================
// Flag bits -- the #define values in widgets.def that the engine tests.
// The .def files' own #define lines fill the symbol table their FLAGS
// expressions are evaluated with.
// =====================================================================
namespace {

// Control flags (every widget).
constexpr uint32_t kCtrlHidden     = 0x00000002;
constexpr uint32_t kCtrlDisabled   = 0x00000004;
constexpr uint32_t kCtrlSelectable = 0x00000010;
constexpr uint32_t kCtrlDrawToBg   = 0x00000800;
constexpr uint32_t kCtrlClearBg    = 0x00001000;
// TEXT
constexpr uint32_t kTextFrame      = 0x00010000;
constexpr uint32_t kTextCenter     = 0x00040000;
constexpr uint32_t kTextRight      = 0x00080000;
constexpr uint32_t kTextTop        = 0x00100000;
constexpr uint32_t kTextBottom     = 0x00200000;
// BUTTON
constexpr uint32_t kBtnDown        = 0x00010000;
constexpr uint32_t kBtnToggle      = 0x00040000;
constexpr uint32_t kBtnRadio       = 0x00080000;
constexpr uint32_t kBtnDrawRadio   = 0x00200000;
constexpr uint32_t kBtnDrawToggle  = 0x00400000;
// SCROLLBAR
constexpr uint32_t kScrollVScroll  = 0x00010000;
constexpr uint32_t kScrollHScroll  = 0x00020000;
// LISTBOX
constexpr uint32_t kListVList      = 0x00010000;
constexpr uint32_t kListHList      = 0x00020000;
constexpr uint32_t kListNoScroll   = 0x00040000;
constexpr uint32_t kListUseFormat  = 0x00080000;
constexpr uint32_t kListViewOnly   = 0x00200000;
// EDIT
constexpr uint32_t kEditNoFrame    = 0x00010000;
constexpr uint32_t kEditNoEdit     = 0x00020000;
constexpr uint32_t kEditSpin       = 0x00040000;
constexpr uint32_t kEditCenter     = 0x00200000;
constexpr uint32_t kEditRight      = 0x00400000;

// REVSYNC: the DEF engine draws its sprites with DM_USEDEFAULT (0x80000000,
// cls_0x5b93c4.cpp:237), which takes the bitmap's own draw mode (1998
// TSurface::ParamDrawSetup). A DM_ALPHA sprite -- the "alpha" and "tex"
// widget packs, popupalpha's Background -- goes through the alpha blit
// (0x004b349d), which reads only its alpha track, never the key colour: its
// dark glass and edges are black pixels (the key, 0) with alpha. Other
// stored modes key only with DM_TRANSPARENT; without it the key colour draws
// like any other (optionsalpha's Background, mode 0, is a whole picture).
EBitmapDecode DecodeFor(const TBitmap* bm)
{
    return (bm && (bm->drawmode & DM_TRANSPARENT) && !(bm->drawmode & DM_ALPHA)) ? EBitmapDecode::Pixels
                                                                                 : EBitmapDecode::Unkeyed;
}

// REVSYNC: a sprite's draw mode as the engine resolves DM_USEDEFAULT
// (0x0042cd22, 0x00429b74): the bitmap's own without DM_USEREG and
// DM_BACKGROUND, with DM_NORESTORE.
uint32_t ResolvedMode(uint32_t mode, const TBitmap* bm)
{
    return mode == kDefDrawDefault && bm ? (bm->drawmode & ~0x20010u) | 0x20u : mode;
}

// The draw modes that make a widget restore its rect before drawing
// (DEF_ENGINE.md §1.3): a button's face, a scrollbar's, list's or edit's
// chrome.
constexpr uint32_t kButtonRestoreModes = DM_ALPHA | DM_TRANSPARENT;                   // 0x2100
constexpr uint32_t kChromeRestoreModes = 0x8000u | DM_ALPHA | 0x1000u | DM_TRANSPARENT; // 0xb100

// A style without a FONT draws in FONT.DEF's "Med" (font 7, every compose
// of the Options pane).
constexpr const char* kDefaultFont = "Med";

// DEF_FADE: the level runs 0..5, one step per pulse (TButtonPane::Initialize
// 0x00434e40 sets the target 5; 0x00435d70 steps it), and the pane is drawn
// with alpha level/5 (0x00436090). The port moves it with time at the pulse
// rate rather than in steps.
constexpr float kFadeSteps = 5.0f;

// REVSYNC: the EDIT's pulse @ 0x004329a0 -- while edited, a counter runs
// 0..47 a pulse (from 0 when editing begins, 0x00432a34) and the caret "|"
// shows while it is under 24 (0x0043276c). The port runs it on the clock.
constexpr int32_t kCaretPeriod = 48;
constexpr int32_t kCaretOn     = 24;

// Retail TDefButton plays "click1" when it fires (0x0042d390 for a key,
// 0x0042d4b0 for a mouse press on a toggle or a release inside a button):
// once, at full volume, not positioned (0x0049b990(id, 0x7f, 1, 0, 0x50, 700)).
constexpr const char* kClickSound = "click1";

// ----- low-level helpers -------------------------------------------------

// REVSYNC: FUN_004377c0 — widgets.def and the screen's .def are read from
// ResourcePath, loose file first. That is how the loose 1.22 copies beside
// resources.rvr (options.def with "No Combat Results", joingame.def,
// mpingame.def) override the pack's older ones.
std::string ReadResourceText(const char* name)
{
    const std::string path = std::string(ResourcePath) + name;
    std::vector<uint8_t> bytes;
    if (!rev_read_file(path.c_str(), bytes, EOpenOrder::LooseFirst))
        return {};
    return std::string(bytes.begin(), bytes.end());
}

bool ParseNumber(const std::string& t, int64_t& out)
{
    if (t.empty())
        return false;
    char* end = nullptr;
    const long v = strtol(t.c_str(), &end, 0);   // base 0: handles 0x.., decimal, sign
    if (end == t.c_str() || *end != '\0')
        return false;
    out = v;
    return true;
}

// Strip // line + /* */ block comments and join `\`-newline continuations,
// then split into logical lines, pulling `#define NAME VALUE` into `defines`.
void Preprocess(const std::string& raw,
                std::unordered_map<std::string, uint32_t>& defines,
                std::vector<std::string>& outLines)
{
    std::string clean;
    clean.reserve(raw.size());
    const size_t n = raw.size();
    for (size_t i = 0; i < n; )
    {
        const char c = raw[i];
        if (c == '/' && i + 1 < n && raw[i + 1] == '*')          // block comment
        {
            i += 2;
            while (i + 1 < n && !(raw[i] == '*' && raw[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < n && raw[i + 1] == '/')          // line comment
        {
            i += 2;
            while (i < n && raw[i] != '\n') ++i;
            continue;                                            // keep the newline
        }
        if (c == '"')                                            // string literal
        {
            clean += c; ++i;
            while (i < n && raw[i] != '"') { clean += raw[i]; ++i; }
            if (i < n) { clean += '"'; ++i; }
            continue;
        }
        if (c == '\\')                                           // line continuation
        {
            size_t j = i + 1;
            while (j < n && (raw[j] == ' ' || raw[j] == '\t' || raw[j] == '\r')) ++j;
            if (j < n && raw[j] == '\n') { clean += ' '; i = j + 1; continue; }
        }
        if (c == '\r') { ++i; continue; }
        clean += c; ++i;
    }

    size_t start = 0;
    while (start <= clean.size())
    {
        const size_t nl   = clean.find('\n', start);
        const size_t stop = (nl == std::string::npos) ? clean.size() : nl;
        std::string  line = clean.substr(start, stop - start);

        const size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos)
        {
            const std::string trimmed = line.substr(a);
            if (trimmed.rfind("#define", 0) == 0)
            {
                // #define NAME VALUE  -> symbol table
                size_t p = 7;
                auto skipws = [&] { while (p < trimmed.size() && std::isspace((unsigned char)trimmed[p])) ++p; };
                auto word   = [&] { const size_t s = p;
                                    while (p < trimmed.size() && !std::isspace((unsigned char)trimmed[p])) ++p;
                                    return trimmed.substr(s, p - s); };
                skipws(); const std::string name = word();
                skipws(); const std::string val  = word();
                int64_t v = 0;
                if (!name.empty() && ParseNumber(val, v))
                    defines[name] = static_cast<uint32_t>(v);
            }
            else
            {
                outLines.push_back(line);
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// Split a logical line into tokens. Quoted strings become one token (quotes
// stripped, `quoted` true); '|' is its own token.
void Tokenize(const std::string& line,
              std::vector<std::string>& toks, std::vector<bool>& quoted)
{
    const size_t n = line.size();
    for (size_t i = 0; i < n; )
    {
        const char c = line[i];
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '"')
        {
            ++i;
            std::string s;
            while (i < n && line[i] != '"') { s += line[i]; ++i; }
            if (i < n) ++i;
            toks.push_back(std::move(s)); quoted.push_back(true);
            continue;
        }
        if (c == '|') { toks.emplace_back("|"); quoted.push_back(false); ++i; continue; }
        std::string s;
        while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '|' && line[i] != '"')
            { s += line[i]; ++i; }
        toks.push_back(std::move(s)); quoted.push_back(false);
    }
}

EDefWidget WidgetTypeOf(const std::string& kw)
{
    if (kw == "BITMAP")    return EDefWidget::Bitmap;
    if (kw == "FRAME")     return EDefWidget::Frame;
    if (kw == "TEXT")      return EDefWidget::Text;
    if (kw == "BUTTON")    return EDefWidget::Button;
    if (kw == "SCROLLBAR") return EDefWidget::Scrollbar;
    if (kw == "LISTBOX")   return EDefWidget::Listbox;
    if (kw == "EDIT")      return EDefWidget::Edit;
    if (kw == "DROPLIST")  return EDefWidget::Droplist;
    return EDefWidget::Unknown;
}

const char* TypeName(EDefWidget t)
{
    switch (t)
    {
        case EDefWidget::Bitmap:    return "BITMAP";
        case EDefWidget::Frame:     return "FRAME";
        case EDefWidget::Text:      return "TEXT";
        case EDefWidget::Button:    return "BUTTON";
        case EDefWidget::Scrollbar: return "SCROLLBAR";
        case EDefWidget::Listbox:   return "LISTBOX";
        case EDefWidget::Edit:      return "EDIT";
        case EDefWidget::Droplist:  return "DROPLIST";
        default:                    return "?";
    }
}

// A style before anything is parsed into it. The TEXT style's text flags
// start as TEXT_LEFT | TEXT_SHADOW (the TEXT parser 0x0042b340, +0xd0 =
// 0x401); every other text format as TEXT_LEFT.
SDefStyle DefaultStyle(EDefWidget type)
{
    SDefStyle s;
    if (type == EDefWidget::Text)
        s.text.flags = TextFlag::Left | TextFlag::Shadow;
    return s;
}

// The arrows and the thumb along a scrollbar: the top-left of a piece from
// its placement.
int32_t PartX(const SDefScrollbar& bar, const SDefScrollPart& part)
{
    return bar.x + (part.relR ? bar.w - part.a : part.a);
}

int32_t PartY(const SDefScrollbar& bar, const SDefScrollPart& part)
{
    return bar.y + (part.relB ? bar.h - part.b : part.b);
}

bool HitBitmap(const TBitmap* bm, int32_t x, int32_t y, int32_t px, int32_t py)
{
    return bm && px >= x && py >= y && px < x + bm->width && py < y + bm->height;
}

}  // namespace

// =====================================================================
// Flag-expression evaluation: `<term> [| <term>]*`, term = #define symbol or
// number. Advances `i` past the consumed terms. Stops at the next keyword.
// =====================================================================
uint32_t TDefPane::EvalFlags(const std::vector<std::string>& toks, size_t& i) const
{
    uint32_t acc = 0;
    bool wantTerm = true;
    while (i < toks.size())
    {
        const std::string& tk = toks[i];
        if (tk == "|") { wantTerm = true; ++i; continue; }
        if (!wantTerm) break;                       // a non-'|' after a term ends the expr

        int64_t num = 0;
        if (ParseNumber(tk, num))
            acc |= static_cast<uint32_t>(num);
        else
        {
            const auto it = defines.find(tk);
            if (it != defines.end()) acc |= it->second;
            else break;                             // unknown identifier -> end of expr
        }
        wantTerm = false;
        ++i;
    }
    return acc;
}

// =====================================================================
// Style resolution: the widgets.def style for a widget's type and the
// variant its control flags pick (the constructors, DEF_ENGINE.md §3-§6).
// =====================================================================
const SDefStyle& TDefPane::ResolveStyle(EDefWidget type, uint32_t flags) const
{
    std::string variant;
    switch (type)
    {
        case EDefWidget::Button:      // by the draw bits (0x0042c6a0)
            variant = (flags & kBtnDrawRadio)  ? "RADIO"
                    : (flags & kBtnDrawToggle) ? "TOGGLE" : "NORMAL";
            break;
        case EDefWidget::Scrollbar:   // 0x0042df00
            variant = (flags & kScrollHScroll) ? "HSCROLL" : "VSCROLL";
            break;
        case EDefWidget::Listbox:     // 0x00430090: NOSCROLL takes a third, undefined style
            if (!(flags & kListNoScroll))
                variant = (flags & kListVList) ? "VLIST" : (flags & kListHList) ? "HLIST" : "";
            break;
        case EDefWidget::Edit:        // 0x00431ca0
            variant = (flags & kEditSpin) ? "SPIN" : "NORM";
            break;
        case EDefWidget::Droplist:
            variant = "VSCROLL";
            break;
        default:                      // FRAME / TEXT / BITMAP: one style
            break;
    }
    const auto it = styles.find(std::string(TypeName(type)) + "/" + variant);
    if (it != styles.end())
        return it->second;
    static const SDefStyle kText = DefaultStyle(EDefWidget::Text);
    static const SDefStyle kOther;
    return type == EDefWidget::Text ? kText : kOther;
}

// =====================================================================
// Attribute parsing: a style's keywords, from a STYLE line or the rest of a
// widget line (over the copy of its type's style). Retail parses each type
// with a fixed-order grammar (DEF_ENGINE.md §2-§6); the keywords here are
// the same, and what a keyword means depends on where it falls:
//  - FONT starts the text format; FLAGS, COLOR and DRAWMODE after it are the
//    text's (FLAGS replaces, it isn't OR'd in). DRAWMODE before it is the
//    chrome's.
//  - UPLABEL / DOWNLABEL start a button label; the FLAGS and COLOR after are
//    its. A DOWNLABEL starts as a copy of the UPLABEL; UPLABELRECT also sets
//    DOWNLABELRECT.
//  - RECT is an EDIT's outer rect before FONT and its text rect after; on
//    every other type the content / text inset.
//  - UP / DOWN are a BUTTON's faces; on a SCROLLBAR, a LISTBOX's SCROLLBAR
//    clause and an EDIT's SPIN they are arrows, with THUMB.
// =====================================================================
namespace {

class TStyleParser
{
  public:
    TStyleParser(const std::vector<std::string>& toks, const std::vector<bool>& quoted,
                 const std::unordered_map<std::string, uint32_t>& defines, EDefWidget type)
        : t(toks), q(quoted), defs(defines), type(type) {}

    void Apply(size_t start, SDefStyle& s)
    {
        enum class ECtx { Main, Up, Down } ctx = ECtx::Main;
        bool sawFont = false;
        auto format = [&]() -> SDefTextFormat& {
            return ctx == ECtx::Up ? s.uplabel : ctx == ECtx::Down ? s.downlabel : s.text;
        };

        for (i = start; i < t.size(); )
        {
            if (q[i]) { ++i; continue; }                 // a stray string
            const std::string& k = t[i++];

            if (k == "FONT")       { s.text.font = Str(); sawFont = true; ctx = ECtx::Main; }
            else if (k == "FLAGS") format().flags = Flags();
            else if (k == "COLOR") format().color = Color();
            else if (k == "DRAWMODE")
            {
                const uint32_t mode = Flags();
                if (ctx != ECtx::Main || sawFont) format().drawmode = mode;
                else                              s.drawmode = mode;
            }
            else if (k == "BGBITMAP" || k == "BITMAP") s.bgbitmap = Str();
            else if (k == "FRAME")    { s.frame = Insets(); s.framed = true; }
            else if (k == "MARGINS")  s.margins = Insets();
            else if (k == "NOCENTER") s.nocenter = true;
            else if (k == "RECT")     (type == EDefWidget::Edit && !sawFont ? s.outerRect : s.rect) = Insets();
            else if (k == "ITEM")     { s.itemw = Int(); s.itemh = Int(); }
            else if (k == "SELCOLOR") s.selcolor = Color();
            else if (k == "EDITCOLOR") s.editcolor = Color();
            else if (k == "UPLABEL")
            {
                s.uplabel = SDefTextFormat{};
                s.uplabel.font = Str();
                s.hasUpLabel = true;
                ctx = ECtx::Up;
            }
            else if (k == "DOWNLABEL")
            {
                s.downlabel = s.uplabel;
                s.downlabel.font = Str();
                s.hasDownLabel = true;
                ctx = ECtx::Down;
            }
            else if (k == "UPLABELRECT")   { s.uplabelrect = Insets(); s.downlabelrect = s.uplabelrect; ctx = ECtx::Main; }
            else if (k == "DOWNLABELRECT") { s.downlabelrect = Insets(); ctx = ECtx::Main; }
            else if (k == "SCROLLBAR")     // a LISTBOX's: SCROLLBAR <RELRB> a b c d
            {
                s.hasScrollbar = true;
                s.scrollRel = Keyword("RELRB");
                s.scrollRect = Insets();
            }
            else if ((k == "UP" || k == "DOWN") && type == EDefWidget::Button)
                (k == "UP" ? s.up : s.down) = Str();
            else if (k == "UP" || k == "DOWN")   // name <RELR> a <RELB> b
            {
                SDefScrollPart& part = k == "UP" ? s.scrollUp : s.scrollDown;
                part.bitmap = Str();
                part.relR = Keyword("RELR");
                part.a = Int();
                part.relB = Keyword("RELB");
                part.b = Int();
            }
            else if (k == "THUMB")               // name a b <RELBR> c
            {
                s.scrollThumb.bitmap = Str();
                s.scrollThumb.a = Int();
                s.scrollThumb.b = Int();
                s.scrollThumb.relR = Keyword("RELBR");
                s.scrollThumb.c = Int();
            }
            // Anything else -- variant names, SPIN, a widget's own attributes
            // (MAXLEN), the DROPLIST's keywords -- is skipped a token at a
            // time; its numbers fall through the same way.
        }
    }

  private:
    std::string Str() { return i < t.size() ? t[i++] : std::string(); }

    int32_t Int()
    {
        int64_t v = 0;
        if (i < t.size() && !q[i] && ParseNumber(t[i], v))
            ++i;
        return static_cast<int32_t>(v);
    }

    bool Keyword(const char* word)
    {
        if (i < t.size() && !q[i] && t[i] == word)
        {
            ++i;
            return true;
        }
        return false;
    }

    SDefInsets Insets()
    {
        SDefInsets r;
        r.l = Int(); r.t = Int(); r.r = Int(); r.b = Int();
        return r;
    }

    SDefColor Color()
    {
        SDefColor c;
        c.r = static_cast<uint8_t>(Int());
        c.g = static_cast<uint8_t>(Int());
        c.b = static_cast<uint8_t>(Int());
        return c;
    }

    uint32_t Flags()
    {
        uint32_t acc = 0;
        bool wantTerm = true;
        while (i < t.size())
        {
            if (t[i] == "|") { wantTerm = true; ++i; continue; }
            if (!wantTerm) break;
            int64_t v = 0;
            if (ParseNumber(t[i], v))
                acc |= static_cast<uint32_t>(v);
            else if (const auto it = defs.find(t[i]); it != defs.end())
                acc |= it->second;
            else
                break;
            wantTerm = false;
            ++i;
        }
        return acc;
    }

    const std::vector<std::string>& t;
    const std::vector<bool>& q;
    const std::unordered_map<std::string, uint32_t>& defs;
    EDefWidget type;
    size_t i = 0;
};

}  // namespace

// =====================================================================
// STYLE line: `STYLE <TYPE> [<VARIANT>] <attrs...>` -> styles["TYPE/VARIANT"].
// A STYLE parses from the defaults, so a screen's STYLE replaces the
// widgets.def one rather than merging into it (0x0042b2c0).
// =====================================================================
void TDefPane::ParseStyleLine(const std::vector<std::string>& toks,
                              const std::vector<bool>& quoted)
{
    if (toks.size() < 2) return;
    const std::string& typeName = toks[1];
    const EDefWidget   type     = WidgetTypeOf(typeName);
    if (type == EDefWidget::Unknown) return;

    // Optional variant keyword in toks[2].
    static const char* const kVariants[] = {
        "NORMAL", "TOGGLE", "RADIO", "VSCROLL", "HSCROLL",
        "VLIST", "HLIST", "NORM", "SPIN"
    };
    std::string variant;
    size_t      attrStart = 2;
    if (toks.size() > 2 && !quoted[2])
    {
        for (const char* v : kVariants)
            if (toks[2] == v) { variant = v; attrStart = 3; break; }
    }

    SDefStyle s = DefaultStyle(type);
    TStyleParser(toks, quoted, defines, type).Apply(attrStart, s);
    styles[std::string(typeName) + "/" + variant] = std::move(s);
}

// =====================================================================
// Widget lines inside a PANEL body [begin, end).
// =====================================================================
void TDefPane::ParseWidgetLines(const std::vector<std::vector<std::string>>& lines,
                                const std::vector<std::vector<bool>>& quoted,
                                size_t begin, size_t end)
{
    for (size_t idx = begin; idx < end; )
    {
        const auto& t = lines[idx];
        const auto& q = quoted[idx];
        const EDefWidget type = WidgetTypeOf(t[0]);
        if (type == EDefWidget::Unknown) { ++idx; continue; }   // BEGIN/END/FIELD/stray

        SDefWidget w;
        w.type = type;

        // REVSYNC: the base parse @ 0x0042a350 -- POS, NAME, FIELD, INDEX,
        // TEXT, STRINGREF, FLAGS, GROUP, HOTKEY, each optional, in that
        // order. That FLAGS is the control word; a FLAGS after the style's
        // FONT is the text's.
        size_t i = 1;
        bool sawFlags = false;
        const auto num = [&](size_t at) {
            int64_t v = 0;
            if (at < t.size()) ParseNumber(t[at], v);
            return static_cast<int32_t>(v);
        };
        const auto str = [&](size_t at) { return at < t.size() ? t[at] : std::string(); };
        while (i < t.size())
        {
            const std::string& k = t[i];
            if (q[i])                       { ++i; }
            else if (k == "POS")            { w.x = num(i + 1); w.y = num(i + 2); w.w = num(i + 3); w.h = num(i + 4); i += 5; }
            else if (k == "NAME")           { w.name  = str(i + 1); i += 2; }
            else if (k == "FIELD")          { w.field = str(i + 1); i += 2; }
            else if (k == "TEXT")           { w.text  = str(i + 1); i += 2; }
            else if (k == "INDEX")          { i += 2; }               // no in-scope screen reads it
            else if (k == "STRINGREF")      { i += 2; }               // nor uses one
            else if (k == "GROUP")          { w.group = num(i + 1); i += 2; }
            else if (k == "HOTKEY")         { w.hotkey = num(i + 1); i += 2; }
            else if (k == "FLAGS" && !sawFlags) { ++i; w.flags = EvalFlags(t, i); sawFlags = true; }
            else break;
        }

        // The type's style, the rest of the line parsed over the copy.
        w.style = ResolveStyle(type, w.flags);
        TStyleParser(t, q, defines, type).Apply(i, w.style);
        for (size_t m = i; m + 1 < t.size(); ++m)
            if (!q[m] && t[m] == "MAXLEN")
                w.maxlen = num(m + 1);

        // The constructors' adjustments.
        if (type == EDefWidget::Text)
        {
            // REVSYNC: 0x0042b5d0 -- the alignment control flags move the
            // style's text alignment; nothing else of the control word
            // reaches the text.
            uint32_t& f = w.style.text.flags;
            if (w.flags & kTextCenter)      f = (f & ~(TextFlag::Left | TextFlag::Right)) | TextFlag::Center;
            else if (w.flags & kTextRight)  f = (f & ~(TextFlag::Left | TextFlag::Center)) | TextFlag::Right;
            if (w.flags & kTextTop)         f = (f & ~(TextFlag::Bottom | TextFlag::VCenter)) | TextFlag::Top;
            else if (w.flags & kTextBottom) f = (f & ~(TextFlag::Top | TextFlag::VCenter)) | TextFlag::Bottom;
        }
        else if (type == EDefWidget::Button)
        {
            // 0x0042c6a0: a GROUP makes a radio button, and a radio button
            // toggles.
            if (w.group >= 0)       w.flags |= kBtnRadio;
            if (w.flags & kBtnRadio) w.flags |= kBtnToggle;
            w.selected = (w.flags & kBtnDown) != 0;
        }
        w.hidden   = (w.flags & kCtrlHidden) != 0;
        w.disabled = (w.flags & kCtrlDisabled) != 0;

        ++idx;

        // LISTBOX row format: an inner BEGIN ... END block (0x0042f700),
        // each item starting from the list's text format.
        if (type == EDefWidget::Listbox && idx < end && !lines[idx].empty()
            && lines[idx][0] == "BEGIN")
        {
            ++idx;
            while (idx < end && !lines[idx].empty() && lines[idx][0] != "END")
            {
                const auto& ft = lines[idx];
                const auto& fq = quoted[idx];
                // FIELD|TEXT x y w h <FONT f> <FLAGS e> <COLOR r g b> <DRAWMODE d> "string"
                if (ft[0] == "FIELD" || ft[0] == "TEXT")
                {
                    SDefFormatItem item;
                    item.kind = ft[0] == "FIELD" ? SDefFormatItem::EKind::Field : SDefFormatItem::EKind::Text;
                    item.format = w.style.text;
                    int64_t v = 0;
                    if (ft.size() > 1) { ParseNumber(ft[1], v); item.x = static_cast<int32_t>(v); }
                    if (ft.size() > 2) { ParseNumber(ft[2], v); item.y = static_cast<int32_t>(v); }
                    if (ft.size() > 3) { ParseNumber(ft[3], v); item.w = static_cast<int32_t>(v); }
                    if (ft.size() > 4) { ParseNumber(ft[4], v); item.h = static_cast<int32_t>(v); }
                    for (size_t k = 5; k < ft.size(); )
                    {
                        if (fq[k])                         { item.name = ft[k]; ++k; }
                        else if (ft[k] == "FONT")          { if (k + 1 < ft.size()) item.format.font = ft[k + 1]; k += 2; }
                        else if (ft[k] == "FLAGS")         { ++k; item.format.flags = EvalFlags(ft, k); }
                        else if (ft[k] == "DRAWMODE")      { ++k; item.format.drawmode = EvalFlags(ft, k); }
                        else if (ft[k] == "COLOR")
                        {
                            int64_t r = 0, g = 0, b = 0;
                            if (k + 1 < ft.size()) ParseNumber(ft[k + 1], r);
                            if (k + 2 < ft.size()) ParseNumber(ft[k + 2], g);
                            if (k + 3 < ft.size()) ParseNumber(ft[k + 3], b);
                            item.format.color = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                            k += 4;
                        }
                        else ++k;
                    }
                    w.format.push_back(std::move(item));
                }
                else
                {
                    // BITMAP items draw a bitmap field through the pane's
                    // bitmap getter (vtable +0xa0); no in-scope list has one.
                    log_warn("[defpane] list '%s': format item '%s' isn't supported", w.name.c_str(), ft[0].c_str());
                }
                ++idx;
            }
            if (idx < end) ++idx;                       // consume END
        }

        widgets.push_back(std::move(w));
    }
}

// =====================================================================
// File loading. widgets.def is loaded styles-only; the screen .def adds any
// STYLE overrides + the requested panel's widgets.
// =====================================================================
bool TDefPane::LoadDefFile(const char* name, bool collectPanels)
{
    const std::string raw = ReadResourceText(name);
    if (raw.empty())
    {
        log_error("[defpane] cannot read '%s'", name);
        return false;
    }

    std::vector<std::string> rawLines;
    Preprocess(raw, defines, rawLines);

    std::vector<std::vector<std::string>> toks;
    std::vector<std::vector<bool>>        quoted;
    toks.reserve(rawLines.size());
    quoted.reserve(rawLines.size());
    for (const auto& l : rawLines)
    {
        std::vector<std::string> t;
        std::vector<bool>        q;
        Tokenize(l, t, q);
        if (!t.empty()) { toks.push_back(std::move(t)); quoted.push_back(std::move(q)); }
    }

    for (size_t idx = 0; idx < toks.size(); )
    {
        const std::string& kw = toks[idx][0];
        if (kw == "STYLE")
        {
            ParseStyleLine(toks[idx], quoted[idx]);
            ++idx;
        }
        else if (kw == "PANEL")
        {
            const std::string pname = (toks[idx].size() > 1) ? toks[idx][1] : std::string();
            size_t b = idx + 1;
            if (b < toks.size() && toks[b][0] == "BEGIN")
            {
                // Find the matching END, honouring nested LISTBOX BEGIN/END.
                size_t  e     = b + 1;
                int32_t depth = 1;
                while (e < toks.size() && depth > 0)
                {
                    if      (toks[e][0] == "BEGIN") ++depth;
                    else if (toks[e][0] == "END")   { if (--depth == 0) break; }
                    ++e;
                }
                if (collectPanels && pname == targetPanel)
                    ParseWidgetLines(toks, quoted, b + 1, e);
                idx = (e < toks.size()) ? e + 1 : toks.size();
            }
            else ++idx;
        }
        else ++idx;
    }
    return true;
}

// The constructors' layout, now that the art is loaded.
void TDefPane::LayoutWidgets()
{
    for (SDefWidget& w : widgets)
    {
        const SDefStyle& st = w.style;
        if (w.type == EDefWidget::Scrollbar)
        {
            // REVSYNC: 0x0042e02a-0x0042e0bf -- with a BGBITMAP, a horizontal
            // bar is as tall as its art less the margins, a vertical one as
            // wide (the Options sliders: 21 - 4 = 17, whatever the .def says).
            if (const TBitmap* bm = st.bgbitmap.empty() ? nullptr : LookupBitmap(st.bgbitmap))
            {
                if (w.flags & kScrollHScroll)
                    w.h = bm->height - st.margins.t - st.margins.b;
                else
                    w.w = bm->width - st.margins.l - st.margins.r;
            }
            // The parts lay out along the bar by VSCROLL (0x0042e2e6).
            w.bar.x = w.x; w.bar.y = w.y; w.bar.w = w.w; w.bar.h = w.h;
            w.bar.vertical = (w.flags & kScrollVScroll) != 0;
        }
        else if (w.type == EDefWidget::Listbox && st.hasScrollbar)
        {
            // REVSYNC: 0x004304a0 -- the list's scrollbar from its SCROLLBAR
            // clause (a b c d): along a VLIST's right (RELRB: from the right),
            // inset b at the top and d at the bottom; an HLIST's the
            // transpose.
            const SDefInsets& r = st.scrollRect;
            SDefScrollbar& bar = w.bar;
            bar.vertical = IsVList(w);
            if (bar.vertical)
            {
                bar.x = w.x + (st.scrollRel ? w.w - r.l : r.l);
                bar.y = w.y + r.t;
                bar.w = w.x + (st.scrollRel ? w.w - r.r : r.r) - bar.x;
                bar.h = w.h - r.b - r.t;
            }
            else
            {
                bar.y = w.y + (st.scrollRel ? w.h - r.l : r.l);
                bar.x = w.x + r.t;
                bar.h = w.y + (st.scrollRel ? w.h - r.r : r.r) - bar.y;
                bar.w = w.w - r.b - r.t;
            }
            bar.minval = bar.maxval = bar.value = 0;
            // REVSYNC: 0x00430c50 -- the page is the items the window shows
            // while the value counts rows, so a track click moves the
            // Options list 16 rows.
            bar.page = ListGrid(w).Visible();
        }
    }
}

// =====================================================================
// Asset + font lookup
// =====================================================================
TBitmap* TDefPane::LookupBitmap(const std::string& entry)
{
    // Non-fatal name walk: TMulti::Object FatalErrors on a miss, which we can't
    // use when searching across several archives. Walk the name table directly
    // (numoffsets/names/offsets are public TMultiData fields).
    if (entry.empty())
        return nullptr;
    for (TMulti* dat : bitmapDats)
    {
        if (!dat) continue;
        for (int32_t c = 0; c < dat->numoffsets; ++c)
        {
            const char* p = reinterpret_cast<const char*>(static_cast<void*>(dat->names[c]));
            if (p && stricmp(p, entry.c_str()) == 0)
                return reinterpret_cast<TBitmap*>(static_cast<void*>(dat->offsets[c]));
        }
    }
    return nullptr;
}

// The DEF's font by its FONT.DEF name (TFontTable: a WINFONT draws in GDI's
// cell for it).
const SFontAtlas* TDefPane::FontFor(const std::string& name)
{
    if (!FontTable)
        return nullptr;
    if (const SFontAtlas* atlas = name.empty() ? nullptr : FontTable->Atlas(name.c_str()))
        return atlas;
    return FontTable->Atlas(kDefaultFont);
}

// =====================================================================
// Open / Close
// =====================================================================
TMulti* TDefPane::LoadDat(const std::string& name)
{
    TMulti* dat = TMulti::LoadMulti(const_cast<char*>(name.c_str()));
    if (!dat)
    {
        log_error("[defpane] %s missing", name.c_str());
        return nullptr;
    }
    archives[name] = dat;
    return dat;
}

bool TDefPane::Open(const char* defName, const char* panelName, uint32_t defFlags_,
                    int32_t x, int32_t y, int32_t w, int32_t h, const char* datBase)
{
    ReleaseAssets();
    if (IsOpen())
        TPane::Close();

    Resize(x, y, w, h);
    paneW = w; paneH = h;
    defflags    = defFlags_;
    targetPanel = panelName ? panelName : "";

    // 1) Styles + flag #defines from the shared widgets.def.
    if (!LoadDefFile("widgets.def", /*collectPanels*/ false))
        return false;

    // 2) The screen's own .def: any STYLE overrides + the requested panel.
    std::string screenFile = std::string(defName) + ".def";
    if (!LoadDefFile(screenFile.c_str(), /*collectPanels*/ true))
        return false;

    if (widgets.empty())
        log_warn("[defpane] panel '%s' in %s produced no widgets",
                 targetPanel.c_str(), screenFile.c_str());

    // 3) Assets. REVSYNC: 0x00435b20 (the screen's dat) / 0x00435150 (the
    //    shared widget pack): "alpha" without DEF_OVERLAY, else "tex".
    //    REVSYNC-DIVERGENCE: retail took "notex" instead of "tex" under the
    //    NOTEXOVERLAYS command line (DAT_006680c8), which the port doesn't have.
    const char* variant = (defflags & DEF_OVERLAY) ? "tex" : "alpha";
    if (TMulti* wdat = LoadDat(std::string("widgets") + variant + ".dat"))
        bitmapDats.push_back(wdat);
    bgDatName = datBase ? std::string(datBase) + variant + ".dat" : std::string();
    if (!bgDatName.empty())
        if (TMulti* sdat = LoadDat(bgDatName))
            bitmapDats.insert(bitmapDats.begin(), sdat);   // search screen dat first
    background = LookupBitmap("Background");

    // 4) BITMAP FIELD widgets: retail's default field getter (0x00436de0)
    //    looks the field's name up in the screen dat, then the widget pack
    //    (the in-game menu's "picture" is its dat's "Picture"). Panes with
    //    live pictures draw their own (DrawField).
    for (SDefWidget& wid : widgets)
        if (wid.type == EDefWidget::Bitmap && !wid.field.empty() && !wid.fieldBitmap)
            wid.fieldBitmap = LookupBitmap(wid.field);
    LayoutWidgets();

    // 5) The compose-to-target render surface (default RGBA8 format).
    surface = new TSurface(paneW, paneH);
    open    = true;
    TPane::Initialize();

    // DEF_FADE starts black-transparent and fades in (0x00434e40: level 0,
    // target 5).
    fadeStartTime = TTime::Time();
    fadeFromLevel = 0.0f;
    finishing     = false;
    endRequested  = false;
    log_info("[defpane] opened '%s' panel '%s' flags 0x%x %dx%d @(%d,%d): %zu widgets, bg=%s",
             defName, targetPanel.c_str(), defflags, paneW, paneH, x, y,
             widgets.size(), background ? "OK" : "MISS");

    // Retail control event 1, once the widgets exist.
    OnOpened();
    return true;
}

bool TDefPane::OpenChrome(int32_t x, int32_t y, int32_t w, int32_t h,
                          const char* datName, const char* backgroundEntry)
{
    ReleaseAssets();
    if (IsOpen())
        TPane::Close();

    Resize(x, y, w, h);
    paneW = w; paneH = h;
    targetPanel.clear();
    bgDatName = datName ? datName : "";

    TMulti* dat = bgDatName.empty() ? nullptr : LoadDat(bgDatName);
    if (!dat)
        return false;
    bitmapDats.push_back(dat);
    background = backgroundEntry ? LookupBitmap(backgroundEntry) : nullptr;
    if (backgroundEntry && !background)
        log_warn("[defpane] %s has no '%s'", bgDatName.c_str(), backgroundEntry);

    surface = new TSurface(paneW, paneH);
    open    = true;
    TPane::Initialize();
    log_info("[defpane] opened chrome '%s' %dx%d @(%d,%d), bg=%s",
             bgDatName.c_str(), paneW, paneH, x, y, background ? "OK" : "MISS");
    return true;
}

void TDefPane::AddText(const char* name, int32_t x, int32_t y, int32_t w, int32_t h,
                       const char* text, uint32_t flags, const SDefColor& color,
                       const char* font)
{
    SDefWidget wid;
    wid.type  = EDefWidget::Text;
    wid.name  = name ? name : "";
    wid.text  = text ? text : "";
    wid.x = x; wid.y = y; wid.w = w; wid.h = h;
    wid.style.text.flags = flags;
    wid.style.text.color = color;
    if (font)
        wid.style.text.font = font;
    widgets.push_back(std::move(wid));
    SetDirty(true);
}

bool TDefPane::AddSpriteButton(const char* name, const char* faceBase)
{
    char face[64];
    std::snprintf(face, sizeof(face), "%sU", faceBase);
    TBitmap* up = LookupBitmap(face);
    if (!up)
    {
        log_warn("[defpane] sprite button '%s': no face '%s'", name, face);
        return false;
    }
    SDefWidget w;
    w.type   = EDefWidget::Button;
    w.name   = name ? name : faceBase;
    w.faceUp = up;
    std::snprintf(face, sizeof(face), "%sD", faceBase);
    w.faceDown = LookupBitmap(face);
    std::snprintf(face, sizeof(face), "%sS", faceBase);
    w.faceHover = LookupBitmap(face);
    // Retail places the sprite at its registration point: screen top-left =
    // (-regx, -regy) (MainMenu_SPEC §4).
    w.x = -up->regx;
    w.y = -up->regy;
    w.w = up->width;
    w.h = up->height;
    widgets.push_back(std::move(w));
    SetDirty(true);
    return true;
}

void TDefPane::Close()
{
    ReleaseAssets();
    TPane::Close();
}

void TDefPane::ReleaseAssets()
{
    delete surface;
    surface = nullptr;
    for (auto& kv : archives)
    {
        if (!kv.second)
            continue;
        // These UI DAT archives own the bitmaps borrowed by the renderer.
        // Heap addresses can be reused by the next screen, so evict their
        // fallback textures before releasing the source resource blob.
        if (Renderer)
            for (int32_t i = 0; i < kv.second->numoffsets; ++i)
                Renderer->ReleaseBitmapTextures(kv.second->Bitmap(i));
        free(kv.second);     // TMulti is a malloc'd resource blob
    }
    archives.clear();
    bitmapDats.clear();
    widgets.clear();
    styles.clear();
    defines.clear();
    background = nullptr;
    open = false;
    defflags  = 0;
    finishing = false;
    endRequested = false;
    draggingBar = -1;
    arrowBar = -1;
    arrowStep = 0;
    focusWidget = -1;
}

TDefPane::~TDefPane() { ReleaseAssets(); }

// =====================================================================
// Data binding / lookup
// =====================================================================
SDefWidget* TDefPane::Find(const char* name)
{
    if (!name) return nullptr;
    for (SDefWidget& w : widgets)
        if (w.name == name) return &w;
    return nullptr;
}

void TDefPane::SetFieldBitmap(const char* field, TBitmap* bm)
{
    if (!field) return;
    for (SDefWidget& w : widgets)
        if (w.type == EDefWidget::Bitmap && w.field == field)
            w.fieldBitmap = bm;
}

// REVSYNC: 0x00430c50 -- the scrollbar runs over the rows (columns of an
// HLIST) the items fill past the window: max((count - visible + n - 1) / n,
// 0), n the items in a row (0x00430cc9-0x00430d12).
void TDefPane::SetListCount(const char* listName, int32_t count)
{
    SDefWidget* w = Find(listName);
    if (!w || w->type != EDefWidget::Listbox)
        return;
    w->count = (std::max)(0, count);
    if (w->selitem >= w->count)
        w->selitem = -1;
    const SListGrid grid = ListGrid(*w);
    const int32_t perRow = IsVList(*w) ? grid.cols : grid.rows;
    w->bar.minval = 0;
    w->bar.maxval = (std::max)(0, (w->count - grid.Visible() + perRow - 1) / perRow);
    ScrollList(*w, w->bar.value);
    SetDirty(true);
}

std::string TDefPane::FieldText(const std::string& field, int32_t index, SDefColor& colour)
{
    (void)field; (void)index; (void)colour;
    return {};
}

void TDefPane::SetText(const char* name, const std::string& text)
{
    if (SDefWidget* w = Find(name))
    {
        w->text = text;
        if (w->type == EDefWidget::Edit && w->maxlen > 0 &&
            static_cast<int32_t>(w->text.size()) > w->maxlen)
            w->text.resize(static_cast<size_t>(w->maxlen));
        SetDirty(true);
    }
}

void TDefPane::SetHotKey(const char* name, int32_t vk)
{
    if (SDefWidget* w = Find(name))
        w->hotkey = vk;
}

void TDefPane::SelectListItem(const char* listName, int32_t item)
{
    if (SDefWidget* w = Find(listName); w && w->type == EDefWidget::Listbox)
        SetSelection(*w, item);
}

void TDefPane::SetSliderRange(const char* name, int32_t minval, int32_t maxval)
{
    SDefWidget* w = Find(name);
    if (!w || w->type != EDefWidget::Scrollbar)
        return;
    w->bar.minval = minval;
    w->bar.maxval = (std::max)(minval, maxval);
    w->bar.value  = std::clamp(w->bar.value, w->bar.minval, w->bar.maxval);
    SetDirty(true);
}

void TDefPane::SetSliderValue(const char* name, int32_t value)
{
    if (SDefWidget* w = Find(name); w && w->type == EDefWidget::Scrollbar)
        ChangeBar(*w, value);
}

void TDefPane::SetSliderPage(const char* name, int32_t page)
{
    if (SDefWidget* w = Find(name); w && w->type == EDefWidget::Scrollbar)
        w->bar.page = page;
}

// A scrollbar's new value: a list scrolls; a SCROLLBAR raises its change.
void TDefPane::ChangeBar(SDefWidget& w, int32_t value)
{
    if (w.type == EDefWidget::Listbox)
    {
        ScrollList(w, value);
        return;
    }
    value = std::clamp(value, w.bar.minval, (std::max)(w.bar.minval, w.bar.maxval));
    if (value == w.bar.value)
        return;
    w.bar.value = value;
    SetDirty(true);
    OnSliderChanged(w);
}

// =====================================================================
// Lists
// =====================================================================
bool TDefPane::IsVList(const SDefWidget& w)
{
    return (w.flags & kListVList) || !(w.flags & kListHList);
}

// REVSYNC: the list layout @ 0x004308b0 -- the cells fill the rect inside
// RECT: as many whole ITEM-sized cells across and down as fit (an ITEM
// wider or taller than the room is cut to it).
TDefPane::SListGrid TDefPane::ListGrid(const SDefWidget& w)
{
    const SDefStyle& st = w.style;
    SListGrid g;
    g.cx = w.x + st.rect.l;
    g.cy = w.y + st.rect.t;
    const int32_t cw = w.w - st.rect.l - st.rect.r;
    const int32_t ch = w.h - st.rect.t - st.rect.b;
    g.itemw = (std::max)(1, (std::min)(cw, st.itemw));
    g.itemh = (std::max)(1, (std::min)(ch, st.itemh));
    g.cols  = (std::max)(1, cw / g.itemw);
    g.rows  = (std::max)(1, ch / g.itemh);
    return g;
}

// The first row shown (column of an HLIST): its scrollbar's value; the first
// item is that row's (0x00430b28-0x00430b41).
void TDefPane::ScrollList(SDefWidget& w, int32_t row)
{
    row = std::clamp(row, 0, (std::max)(0, w.bar.maxval));
    const SListGrid grid = ListGrid(w);
    const int32_t top = row * (IsVList(w) ? grid.cols : grid.rows);
    if (row == w.bar.value && top == w.top)
        return;
    w.bar.value = row;
    w.top = top;
    SetDirty(true);
    OnListScrolled(w);
}

// REVSYNC: list SetSelection @ 0x00430b80: an item out of range selects
// nothing; one outside the window scrolls it to the window's middle row; a
// change raises the list event 5000.
void TDefPane::SetSelection(SDefWidget& w, int32_t item)
{
    if (item < 0 || item >= w.count)
        item = -1;
    const int32_t previous = w.selitem;
    w.selitem = item;

    const SListGrid grid = ListGrid(w);
    if (item >= 0 && (item < w.top || item >= w.top + grid.Visible()))
    {
        const bool vlist = IsVList(w);
        const int32_t line = item / (vlist ? grid.cols : grid.rows);
        ScrollList(w, line - (vlist ? grid.rows : grid.cols) / 2);
    }
    SetDirty(true);

    if (item != previous)
        OnListSelect(w, item);
}

// REVSYNC: the list hit test @ 0x004310b0 -- the item in the cell under the
// pointer, -1 outside the cells or past the last item.
int32_t TDefPane::ListItemAt(const SDefWidget& w, int32_t lx, int32_t ly)
{
    const SListGrid g = ListGrid(w);
    if (lx < g.cx || ly < g.cy || lx >= g.cx + g.cols * g.itemw || ly >= g.cy + g.rows * g.itemh)
        return -1;
    const int32_t col = (lx - g.cx) / g.itemw;
    const int32_t row = (ly - g.cy) / g.itemh;
    const int32_t item = w.top + (IsVList(w) ? row * g.cols + col : col * g.rows + row);
    return item < w.count ? item : -1;
}

void TDefPane::ClickListItem(SDefWidget& w, int32_t lx, int32_t ly)
{
    if (const int32_t item = ListItemAt(w, lx, ly); item >= 0)
        SetSelection(w, item);
}

// =====================================================================
// Rendering
// =====================================================================

// REVSYNC: the frame tiler @ 0x00429ac0 -- the art's FRAME insets cut it
// into corners, edges and a centre; edges and centre are TILED (the last
// tile cut short), never stretched, over the rect grown by MARGINS (the art
// carries its drop shadow there); NOCENTER leaves the centre unpainted.
void TDefPane::DrawFrame(TBitmap* bm, const SDefStyle& style, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!bm || !surface)
        return;
    const SDefInsets& f = style.frame;
    const SDefInsets& m = style.margins;
    const int32_t X0 = x - m.l, Y0 = y - m.t;
    const int32_t W = w + m.l + m.r, H = h + m.t + m.b;
    const int32_t mw = bm->width - f.l - f.r;     // the centre's size in the art
    const int32_t mh = bm->height - f.t - f.b;
    const int32_t tw = surface->Width(), th = surface->Height();
    const EBitmapDecode decode = DecodeFor(bm);
    const auto put = [&](int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
        if (sw > 0 && sh > 0)
            Renderer->DrawBitmapSubrectToTarget(bm, dx, dy, sx, sy, sw, sh, tw, th, decode);
    };
    // One band of the frame: its left piece, the middle tiled across, its
    // right piece.
    const auto band = [&](int32_t dy, int32_t sy, int32_t sh, bool middle) {
        put(X0, dy, 0, sy, f.l, sh);
        if (middle && mw > 0)
            for (int32_t xx = X0 + f.l; xx < X0 + W - f.r; xx += mw)
                put(xx, dy, f.l, sy, (std::min)(mw, X0 + W - f.r - xx), sh);
        put(X0 + W - f.r, dy, f.l + mw, sy, f.r, sh);
    };
    if (f.t > 0)
        band(Y0, 0, f.t, true);
    if (mh > 0)
        for (int32_t yy = Y0 + f.t; yy < Y0 + H - f.b; yy += mh)
            band(yy, f.t, (std::min)(mh, Y0 + H - f.b - yy), !style.nocenter);
    if (f.b > 0)
        band(Y0 + H - f.b, f.t + mh, f.b, true);
}

// A style's chrome: tiled as a frame when the style has FRAME, else the art
// once at its size.
void TDefPane::DrawChrome(const SDefStyle& style, const std::string& bitmap,
                          int32_t x, int32_t y, int32_t w, int32_t h)
{
    TBitmap* bm = LookupBitmap(bitmap);
    if (!bm || !surface)
        return;
    if (style.framed)
        DrawFrame(bm, style, x, y, w, h);
    else
        Renderer->DrawBitmapToTarget(bm, x, y, surface->Width(), surface->Height(), DecodeFor(bm));
}

// REVSYNC: the widgets' background restore (DEF_ENGINE.md §1.3): the rect,
// grown by `m` (the style's MARGINS where the widget's art overhangs it),
// goes back to the pane's background surface (ParamBlit from +0x84; cleared
// where there's none) before the widget draws, erasing what earlier widgets
// drew there.
void TDefPane::RestoreBackground(const SDefInsets& m, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!surface)
        return;
    x -= m.l; y -= m.t; w += m.l + m.r; h += m.t + m.b;
    const int32_t tw = surface->Width(), th = surface->Height();
    Renderer->ClearRectToTarget(x, y, w, h, tw, th);
    if (!background)
        return;
    const int32_t x0 = (std::max)(x, 0), y0 = (std::max)(y, 0);
    const int32_t x1 = (std::min)(x + w, background->width), y1 = (std::min)(y + h, background->height);
    if (x1 > x0 && y1 > y0)
        Renderer->DrawBitmapSubrectToTarget(background, x0, y0, x0, y0, x1 - x0, y1 - y0, tw, th,
                                            DecodeFor(background));
}

// A scrollbar's, list's or edit's restore: when its chrome draws keyed or
// blended, or it has none (0x0042eb30, 0x00431160, 0x004323b0).
void TDefPane::RestoreUnderChrome(const SDefStyle& style, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const TBitmap* bm = LookupBitmap(style.bgbitmap);
    if (!bm || (ResolvedMode(style.drawmode, bm) & kChromeRestoreModes))
        RestoreBackground(style.margins, x, y, w, h);
}

// Retail's text compose (0x004be2b0) in a widget's text format.
void TDefPane::DrawString(const std::string& text, int32_t x, int32_t y, int32_t w,
                          int32_t h, const SDefTextFormat& format, const SDefColor& color)
{
    if (text.empty() || !surface)
        return;
    if (const SFontAtlas* atlas = FontFor(format.font))
        DrawTextComposed(atlas, text.c_str(), x, y, w, h, format.flags, color.r / 255.0f,
                         color.g / 255.0f, color.b / 255.0f, surface->Width(), surface->Height());
}

void TDefPane::DrawWidget(const SDefWidget& wid, int32_t index)
{
    const SDefStyle& st = wid.style;
    switch (wid.type)
    {
        case EDefWidget::Bitmap:
            // REVSYNC: 0x0042ad10 -- the art at its size, never stretched.
            if (wid.flags & kCtrlClearBg)
                RestoreBackground(st.margins, wid.x, wid.y, wid.w, wid.h);
            if (!wid.field.empty())
                DrawField(wid);
            else if (TBitmap* bm = LookupBitmap(st.bgbitmap); bm && surface)
                Renderer->DrawBitmapToTarget(bm, wid.x, wid.y, surface->Width(), surface->Height(), DecodeFor(bm));
            break;
        case EDefWidget::Frame:
            // REVSYNC: 0x0042b150 -- always through the frame tiler.
            if (wid.flags & kCtrlClearBg)
                RestoreBackground(st.margins, wid.x, wid.y, wid.w, wid.h);
            DrawFrame(LookupBitmap(st.bgbitmap), st, wid.x, wid.y, wid.w, wid.h);
            break;
        case EDefWidget::Text:
            DrawText(wid);
            break;
        case EDefWidget::Button:
            DrawButton(wid);
            break;
        case EDefWidget::Scrollbar:
            // REVSYNC: 0x0042eb30 -- the bar's art, then its parts.
            RestoreUnderChrome(st, wid.bar.x, wid.bar.y, wid.bar.w, wid.bar.h);
            DrawChrome(st, st.bgbitmap, wid.bar.x, wid.bar.y, wid.bar.w, wid.bar.h);
            DrawScrollbar(wid, index);
            break;
        case EDefWidget::Listbox:
            DrawListbox(wid, index);
            break;
        case EDefWidget::Edit:
            DrawEdit(wid);
            break;
        default:
            // DROPLIST: no in-scope screen has one.
            break;
    }
}

// REVSYNC: TEXT Draw @ 0x0042b700 -- the text in the rect inside RECT, in
// the style's format (alignment adjusted by the control flags when it was
// built); a FIELD's text from the pane's getter. TEXTFLAG_FRAME tiles the
// style's BGBITMAP under it.
void TDefPane::DrawText(const SDefWidget& w)
{
    const SDefStyle& st = w.style;
    // The rect grows by the MARGINS only with the frame (0x0042b725).
    if (w.flags & kCtrlClearBg)
        RestoreBackground((w.flags & kTextFrame) ? st.margins : SDefInsets{}, w.x, w.y, w.w, w.h);
    if (w.flags & kTextFrame)
        DrawFrame(LookupBitmap(st.bgbitmap), st, w.x, w.y, w.w, w.h);
    SDefColor colour = st.text.color;
    const std::string text = w.field.empty() ? w.text : FieldText(w.field, 0, colour);
    const SDefInsets& r = st.rect;
    DrawString(text, w.x + r.l, w.y + r.t, w.w - r.l - r.r, w.h - r.t - r.b, st.text, colour);
}

// REVSYNC: BUTTON Draw @ 0x0042cc30. Down (BTNFLAG_DOWN: a toggle on, a push
// button held) with a DOWN face takes that face and the down label, else
// the up ones; a state without a label draws none. The face is tiled when
// the style has FRAME (NORMAL), else drawn at its size (TOGGLE, RADIO). The
// label goes in the rect inside the state's label rect.
void TDefPane::DrawButton(const SDefWidget& w)
{
    if (!surface)
        return;
    const int32_t tw = surface->Width(), th = surface->Height();
    if (w.faceUp)
    {
        // Sprite button (title / death screens): U, D while pressed, S while
        // hovered (MainMenu_SPEC §6.6).
        TBitmap* face = w.faceUp;
        if (w.pressed && w.faceDown)       face = w.faceDown;
        else if (w.hovered && w.faceHover) face = w.faceHover;
        Renderer->DrawBitmapToTarget(face, w.x, w.y, tw, th, DecodeFor(face));
        return;
    }

    const SDefStyle& st = w.style;
    const bool down = (w.flags & kBtnToggle) ? w.selected : w.pressed;
    TBitmap* downFace = down ? LookupBitmap(st.down) : nullptr;
    const bool downState = downFace != nullptr;
    if (TBitmap* face = downState ? downFace : LookupBitmap(st.up))
    {
        if (ResolvedMode(st.drawmode, face) & kButtonRestoreModes)
            RestoreBackground(st.margins, w.x, w.y, w.w, w.h);
        if (st.framed)
            DrawFrame(face, st, w.x, w.y, w.w, w.h);
        else
            Renderer->DrawBitmapToTarget(face, w.x, w.y, tw, th, DecodeFor(face));
    }
    if (!(downState ? st.hasDownLabel : st.hasUpLabel))
        return;
    const SDefTextFormat& label = downState ? st.downlabel : st.uplabel;
    const SDefInsets& r = down ? st.downlabelrect : st.uplabelrect;
    DrawString(w.text, w.x + r.l, w.y + r.t, w.w - r.l - r.r, w.h - r.t - r.b, label, label.color);
}

// REVSYNC: LISTBOX Draw @ 0x00431410 -- the chrome (even with
// CTRLFLAG_CLEARBG), its scrollbar's parts, then the items in the window,
// each in its cell (DrawItem 0x00430db0): a VLIST fills the cells a row at
// a time, an HLIST a column at a time; the selected item's cell is filled
// with SELCOLOR; a USEFORMAT list draws its format items in the cell
// (FormatDraw 0x0042f570, each cut to the cell), another its FIELD's text.
void TDefPane::DrawListbox(const SDefWidget& w, int32_t index)
{
    if (!surface)
        return;
    const SDefStyle& st = w.style;
    RestoreUnderChrome(st, w.x, w.y, w.w, w.h);
    DrawChrome(st, st.bgbitmap, w.x, w.y, w.w, w.h);
    // Its scrollbar has no art of its own: retail restores the bar's rect
    // and redraws the list's chrome into it (0x0042ebf3), which leaves it
    // as it is.
    if (st.hasScrollbar)
        DrawScrollbar(w, index);

    const SListGrid g = ListGrid(w);
    const bool vlist = IsVList(w);
    const int32_t last = (std::min)(w.top + g.Visible(), w.count);
    for (int32_t i = w.top; i < last; ++i)
    {
        const int32_t k = i - w.top;
        const int32_t col = vlist ? k % g.cols : k / g.rows;
        const int32_t row = vlist ? k / g.cols : k % g.rows;
        const int32_t X = g.cx + col * g.itemw, Y = g.cy + row * g.itemh;
        if (i == w.selitem && !(w.flags & kListViewOnly))
            Renderer->DrawSolidRectToTarget(X, Y, g.itemw, g.itemh, surface->Width(), surface->Height(),
                                            st.selcolor.r, st.selcolor.g, st.selcolor.b);
        if (w.flags & kListUseFormat)
        {
            for (const SDefFormatItem& f : w.format)
            {
                SDefColor colour = f.format.color;
                const std::string text = f.kind == SDefFormatItem::EKind::Field ? FieldText(f.name, i, colour) : f.name;
                DrawString(text, X + f.x, Y + f.y, (std::min)(f.x + f.w, g.itemw) - f.x,
                           (std::min)(f.y + f.h, g.itemh) - f.y, f.format, colour);
            }
        }
        else if (!w.field.empty())
        {
            SDefColor colour = st.text.color;
            DrawString(FieldText(w.field, i, colour), X, Y, g.itemw, g.itemh, st.text, colour);
        }
    }
}

// REVSYNC: EDIT Draw @ 0x004328b0 -- the frame inside the outer RECT
// (unless EDITFLAG_NOFRAME or disabled), then the text inside both rects:
// EDITCOLOR while edited, with the caret "|" in the on half of its blink;
// EDITFLAG_CENTER / RIGHT move the alignment (0x004327fc).
void TDefPane::DrawEdit(const SDefWidget& w)
{
    const SDefStyle& st = w.style;
    const SDefInsets& o = st.outerRect;
    const SDefInsets& r = st.rect;
    RestoreUnderChrome(st, w.x, w.y, w.w, w.h);
    if (!(w.flags & kEditNoFrame) && !w.disabled)
        DrawChrome(st, st.bgbitmap, w.x + o.l, w.y + o.t, w.w - o.l - o.r, w.h - o.t - o.b);

    std::string text = w.text;
    if (w.focused)
    {
        const double pulses = (TTime::Time() - w.focusTime) * TTime::LegacyFramerate;
        if (static_cast<int64_t>(pulses) % kCaretPeriod < kCaretOn)
            text += '|';
    }
    SDefTextFormat format = st.text;
    if (w.flags & kEditCenter)     format.flags = (format.flags & ~(TextFlag::Left | TextFlag::Right)) | TextFlag::Center;
    else if (w.flags & kEditRight) format.flags = (format.flags & ~(TextFlag::Left | TextFlag::Center)) | TextFlag::Right;
    DrawString(text, w.x + o.l + r.l, w.y + o.t + r.t, w.w - o.l - o.r - r.l - r.r,
               w.h - o.t - o.b - r.t - r.b, format, w.focused ? st.editcolor : st.text.color);
}

// The BITMAP FIELD source (retail field getter 0x00436de0): the bitmap bound
// to the field, if any.
void TDefPane::DrawField(const SDefWidget& wid)
{
    if (wid.fieldBitmap && surface)
        Renderer->DrawBitmapToTarget(wid.fieldBitmap, wid.x, wid.y,
                                     surface->Width(), surface->Height(), DecodeFor(wid.fieldBitmap));
}

// REVSYNC: the scrollbar's parts (0x0042e7d0, 0x0042e980, 0x0042e620): each
// arrow, its "D" art while held, and the thumb at its size where the value
// puts it (when the range is at least 1).
void TDefPane::DrawScrollbar(const SDefWidget& w, int32_t index)
{
    if (!surface)
        return;
    const int32_t tw = surface->Width(), th = surface->Height();
    const SDefStyle& st = w.style;
    const SBarLayout bar = BarLayout(w);
    const auto put = [&](TBitmap* up, const SDefScrollPart& part, bool held, int32_t x, int32_t y) {
        TBitmap* bm = held ? LookupBitmap(part.bitmap + "D") : nullptr;
        if (!bm)
            bm = up;
        if (bm)
            Renderer->DrawBitmapToTarget(bm, x, y, tw, th, DecodeFor(bm));
    };
    put(bar.up, st.scrollUp, arrowBar == index && arrowStep < 0, bar.upX, bar.upY);
    put(bar.down, st.scrollDown, arrowBar == index && arrowStep > 0, bar.downX, bar.downY);
    if (bar.showThumb)
        put(bar.thumb, st.scrollThumb, draggingBar == index, bar.thumbX, bar.thumbY);
}

// REVSYNC: the scrollbar layout @ 0x0042e240 and SetValue @ 0x0042e440:
// the arrows where their placements put them; the thumb's track from a past
// the bar's start to b short of its end, the thumb at value / range of the
// way along it.
// REVSYNC-DIVERGENCE: retail places the thumb by value / (max - min), not
// (value - min) / (max - min); every DEF scrollbar's range starts at 0,
// where the two agree.
TDefPane::SBarLayout TDefPane::BarLayout(const SDefWidget& w)
{
    const SDefStyle& st = w.style;
    const SDefScrollbar& b = w.bar;
    SBarLayout bar;
    bar.up    = LookupBitmap(st.scrollUp.bitmap + "U");
    bar.down  = LookupBitmap(st.scrollDown.bitmap + "U");
    bar.thumb = LookupBitmap(st.scrollThumb.bitmap + "U");
    bar.upX   = PartX(b, st.scrollUp);
    bar.upY   = PartY(b, st.scrollUp);
    bar.downX = PartX(b, st.scrollDown);
    bar.downY = PartY(b, st.scrollDown);

    const SDefScrollPart& thumb = st.scrollThumb;
    const int32_t start = b.vertical ? b.y : b.x;
    const int32_t size  = b.vertical ? b.h : b.w;
    bar.trackStart = start + thumb.a;
    bar.trackEnd   = start + size - thumb.b;
    const int32_t range = b.maxval - b.minval;
    const int32_t along = range > 0 ? bar.trackStart + (bar.trackEnd - bar.trackStart) * (b.value - b.minval) / range
                                    : bar.trackStart;
    const int32_t across = b.vertical ? b.x + (thumb.relR ? b.w - thumb.c : thumb.c)
                                      : b.y + (thumb.relR ? b.h - thumb.c : thumb.c);
    bar.thumbX = b.vertical ? across : along;
    bar.thumbY = b.vertical ? along : across;
    bar.showThumb = range >= 1 && bar.thumb;
    return bar;
}

void TDefPane::Render()
{
    if (!open || !surface) return;

    surface->StartPass(0.0f, 0.0f, 0.0f, 0.0f);   // transparent (modal over frozen frame)
    Paint();
    surface->EndPass();
}

void TDefPane::Paint()
{
    PaintBackground();
    PaintWidgets();
}

void TDefPane::PaintBackground()
{
    if (background)
        Renderer->DrawBitmapToTarget(background, 0, 0, surface->Width(), surface->Height(),
                                     DecodeFor(background));
}

// REVSYNC: the button pane's draw, 0x00435de0 -- the widgets in order, the
// hidden ones not at all. CTRLFLAG_DRAWTOBG widgets are drawn into the
// pane's background (under every other), and the pane's topmost widget
// (+0xa8: the EDIT being edited) last.
void TDefPane::PaintWidgets()
{
    const SDefWidget* topmost = EditingWidget();
    for (int32_t pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < widgets.size(); ++i)
        {
            const SDefWidget& w = widgets[i];
            if (!w.hidden && &w != topmost && ((w.flags & kCtrlDrawToBg) != 0) == (pass == 0))
                DrawWidget(w, static_cast<int32_t>(i));
        }
    if (topmost && !topmost->hidden)
        DrawWidget(*topmost, static_cast<int32_t>(topmost - widgets.data()));
}

// =====================================================================
// Input
// =====================================================================

// REVSYNC: the scrollbar's press (OPTIONS.md §8). On an arrow it waits for
// the release, which steps by 1 (0x0042f249); on the thumb it drags
// (0x0042f2e0); elsewhere on the bar it pages toward the pointer, with a
// click (0x0042f16d-0x0042f1fa).
void TDefPane::PressBar(int32_t index, int32_t lx, int32_t ly)
{
    SDefWidget& w = widgets[index];
    const SBarLayout bar = BarLayout(w);
    if (HitBitmap(bar.up, bar.upX, bar.upY, lx, ly))
    {
        arrowBar = index;
        arrowStep = -1;
    }
    else if (HitBitmap(bar.down, bar.downX, bar.downY, lx, ly))
    {
        arrowBar = index;
        arrowStep = 1;
    }
    else if (bar.showThumb && HitBitmap(bar.thumb, bar.thumbX, bar.thumbY, lx, ly))
    {
        draggingBar = index;
        dragGrab = w.bar.vertical ? ly - bar.thumbY : lx - bar.thumbX;
    }
    else
    {
        const int32_t at = w.bar.vertical ? ly : lx;
        const int32_t thumb = w.bar.vertical ? bar.thumbY : bar.thumbX;
        PlayClick();
        ChangeBar(w, at < thumb ? w.bar.value - w.bar.page : w.bar.value + w.bar.page);
    }
    SetDirty(true);
}

// The held thumb follows the pointer, the place it was taken staying under
// it; the value is the nearest to where the thumb sits.
void TDefPane::DragBar(SDefWidget& w, int32_t lx, int32_t ly)
{
    const SBarLayout bar = BarLayout(w);
    const int32_t travel = bar.trackEnd - bar.trackStart;
    if (travel <= 0)
        return;
    const int32_t pos = std::clamp((w.bar.vertical ? ly : lx) - dragGrab, bar.trackStart, bar.trackEnd) - bar.trackStart;
    ChangeBar(w, w.bar.minval + (pos * (w.bar.maxval - w.bar.minval) + travel / 2) / travel);
}

void TDefPane::ReleaseBarArrow(int32_t lx, int32_t ly)
{
    if (arrowBar >= 0 && arrowBar < static_cast<int32_t>(widgets.size()))
    {
        SDefWidget& w = widgets[arrowBar];
        const SBarLayout bar = BarLayout(w);
        const bool onArrow = arrowStep < 0 ? HitBitmap(bar.up, bar.upX, bar.upY, lx, ly)
                                           : HitBitmap(bar.down, bar.downX, bar.downY, lx, ly);
        if (onArrow)
            ChangeBar(w, w.bar.value + arrowStep);
        SetDirty(true);
    }
    arrowBar = -1;
    arrowStep = 0;
}

SDefWidget* TDefPane::EditingWidget()
{
    for (SDefWidget& w : widgets)
        if (w.type == EDefWidget::Edit && w.focused)
            return &w;
    return nullptr;
}

// The widget a click goes to. REVSYNC: the base click 0x00436530 asks the
// widgets in DEF order (0x004365de..0x0043661f); the first that takes it.
int32_t TDefPane::WidgetAt(int32_t lx, int32_t ly) const
{
    for (size_t i = 0; i < widgets.size(); ++i)
        if (widgets[i].TakesClick(lx, ly))
            return static_cast<int32_t>(i);
    return -1;
}

bool TDefPane::InListBar(const SDefWidget& w, int32_t lx, int32_t ly)
{
    return w.type == EDefWidget::Listbox && w.style.hasScrollbar && lx >= w.bar.x && ly >= w.bar.y &&
           lx < w.bar.x + w.bar.w && ly < w.bar.y + w.bar.h;
}

void TDefPane::OnMouseDown(int32_t lx, int32_t ly)
{
    draggingBar = -1;
    arrowBar = -1;
    // REVSYNC: 0x00436530 -- a press outside the EDIT being edited ends its
    // editing.
    for (SDefWidget& w : widgets)
        if (w.focused && !w.Contains(lx, ly))
            w.focused = false;

    const int32_t index = WidgetAt(lx, ly);
    if (index < 0)
        return;
    SetDirty(true);
    SDefWidget& w = widgets[static_cast<size_t>(index)];
    // REVSYNC: 0x004369f0 -- the press offers the widget the focus, with
    // "frontend_move" unless it has it; only a CTRLFLAG_SELECTABLE one keeps
    // it, so a DEF screen's widgets sound on every press.
    if (!w.faceUp && index != focusWidget)
    {
        PLAY("frontend_move");
        if (w.flags & kCtrlSelectable)
            focusWidget = index;
    }

    switch (w.type)
    {
        case EDefWidget::Button:
            // REVSYNC: 0x0042d4b0 -- a toggle changes on the press, with the
            // click (a radio button only turns on) and its event (0xbb9 /
            // 0xbba); a push button goes down until the release.
            if (w.flags & kBtnToggle)
            {
                PlayClick();
                const bool on = (w.flags & kBtnRadio) ? true : !w.selected;
                if (on != w.selected)
                {
                    w.selected = on;
                    if (on && (w.flags & kBtnRadio))
                        for (SDefWidget& other : widgets)
                            if (&other != &w && other.type == EDefWidget::Button && other.group == w.group)
                                other.selected = false;
                    const std::string name = w.name;
                    Activate(name.c_str());
                }
            }
            else
                w.pressed = true;
            break;
        case EDefWidget::Edit:
            // REVSYNC: 0x004329f0 -- editing begins (the EDIT takes the
            // focus), the caret's blink from its start.
            if (!(w.flags & kEditNoEdit) && !w.focused)
            {
                w.focused = true;
                w.focusTime = TTime::Time();
                focusWidget = index;
            }
            break;
        case EDefWidget::Listbox:
            // REVSYNC: list MouseClick @ 0x00431500 -- its scrollbar first,
            // else the item under the pointer is selected.
            if (InListBar(w, lx, ly))
                PressBar(index, lx, ly);
            else if (!(w.flags & kListViewOnly))
                ClickListItem(w, lx, ly);
            break;
        case EDefWidget::Scrollbar:
            PressBar(index, lx, ly);
            break;
        default:
            break;
    }
}

// REVSYNC: DefWidget_DispatchInput @ 0x004361f0. The EDIT being edited takes
// the key (its char handler 0x00432f70 sees Backspace and Enter); otherwise
// the first button whose key it is fires (0x0042d160): its own key, Enter
// for "ok" / "yes", ESC for "cancel" / "no". Buttons fire on key-down
// (0x0042d390).
void TDefPane::OnKey(int32_t vk, bool down)
{
    if (SDefWidget* ed = EditingWidget())
    {
        if (!down)
            return;
        if (vk == VK_BACK)
        {
            if (!ed->text.empty())
                ed->text.pop_back();
        }
        else if (vk == VK_RETURN)
        {
            // Enter ends editing and tells the pane (event 6001).
            ed->focused = false;
            log_info("[defpane] '%s' entered: \"%s\"", ed->name.c_str(), ed->text.c_str());
        }
        SetDirty(true);
        return;
    }

    if (!down)
        return;
    auto isKeyOf = [vk](const SDefWidget& w) {
        if (w.hotkey != 0)
            return vk == w.hotkey;
        if (vk == VK_RETURN)
            return w.name == "ok" || w.name == "yes";
        if (vk == VK_ESCAPE)
            return w.name == "cancel" || w.name == "no";
        return false;
    };
    for (const SDefWidget& w : widgets)
    {
        if (w.type != EDefWidget::Button || w.hidden || w.disabled || !isKeyOf(w))
            continue;
        const std::string name = w.name;
        if (!w.faceUp)
            PlayClick();
        Activate(name.c_str());
        return;
    }
}

// The release goes to what holds the mouse -- a push button held, a
// scrollbar arrow -- else to the widget under the pointer.
// REVSYNC: 0x0042d4b0 (release) -- a push button held comes up, and fires
// with the click when the pointer is still on it.
const char* TDefPane::OnMouseUp(int32_t lx, int32_t ly)
{
    draggingBar = -1;
    SetDirty(true);
    bool held = false;
    const char* activated = nullptr;
    for (SDefWidget& w : widgets)
    {
        if (w.type != EDefWidget::Button || !w.pressed) continue;
        held = true;
        if (w.Contains(lx, ly))
        {
            activated = w.name.c_str();
            if (!w.faceUp)
                PlayClick();
        }
        w.pressed = false;
    }
    if (held)
        return activated;
    if (arrowBar >= 0)
    {
        ReleaseBarArrow(lx, ly);
        return nullptr;
    }

    // REVSYNC: list MouseClick @ 0x00431572 (release) -- "click1" anywhere
    // in the list, and on an item its selection and event 5002.
    const int32_t index = WidgetAt(lx, ly);
    if (index < 0)
        return nullptr;
    SDefWidget& list = widgets[static_cast<size_t>(index)];
    if (list.type == EDefWidget::Listbox && !InListBar(list, lx, ly))
    {
        PlayClick();
        if (const int32_t item = ListItemAt(list, lx, ly); item >= 0)
        {
            SetSelection(list, item);
            OnListClick(list, item);
        }
    }
    return nullptr;
}

void TDefPane::OnMouseMove(int32_t lx, int32_t ly)
{
    if (draggingBar >= 0 && draggingBar < static_cast<int32_t>(widgets.size()))
        DragBar(widgets[draggingBar], lx, ly);

    for (SDefWidget& w : widgets)
        w.hovered = (w.type == EDefWidget::Button && !w.hidden && !w.disabled && w.Contains(lx, ly));
}

void TDefPane::PlayClick() const
{
    const int32_t id = SoundPlayer.FindSound(kClickSound);
    if (id >= 0 && SoundPlayer.Mount(id))
        SoundPlayer.Play(id);
}

// =====================================================================
// TPane hooks
// =====================================================================

float TDefPane::FadeLevel() const
{
    const float moved = static_cast<float>((TTime::Time() - fadeStartTime) * TTime::LegacyFramerate);
    const float level = finishing ? fadeFromLevel - moved : fadeFromLevel + moved;
    return std::clamp(level, 0.0f, kFadeSteps);
}

bool TDefPane::FadedIn() const
{
    return open && !finishing && (!(defflags & DEF_FADE) || FadeLevel() >= kFadeSteps);
}

// REVSYNC: the DEF pane's close slot @ 0x00435010: a DEF_FADE pane sets its
// fade target to 0 and closes only when the pulse (0x00435d70) has stepped
// the level down to it.
void TDefPane::Finish(int32_t result)
{
    if (!open || finishing)
        return;
    finishResult = result;
    if (defflags & DEF_FADE)
    {
        fadeFromLevel = FadeLevel();
        fadeStartTime = TTime::Time();
        finishing     = true;
        if (fadeFromLevel > 0.0f)
            return;
    }
    RequestEnd();
}

// The screen removes an ended modal at the start of its next tick, so the
// pane is drawn once more after this: it stays finishing (faded out, level
// 0) until Close, rather than reading as a fade-in from where the fade-out
// began, which drew it opaque for that frame.
void TDefPane::RequestEnd()
{
    if (endRequested)
        return;
    endRequested = true;
    EndModal(finishResult);
}

void TDefPane::Pulse()
{
    if (finishing && FadeLevel() <= 0.0f)
        RequestEnd();
    // The caret blinks while an EDIT is edited.
    if (EditingWidget())
        SetDirty(true);
}

void TDefPane::Compose()
{
    Render();
}

// REVSYNC: 0x00436090 -- an overlay pane is drawn with alpha level/5 while it
// fades (DEF_FADE), opaque otherwise.
void TDefPane::Draw()
{
    if (!surface)
        return;
    if ((defflags & DEF_INGAME) == DEF_INGAME)
        Renderer->DrawSurfaceTinted(surface, GetPosX(), GetPosY(), 1.0f, 1.0f, 1.0f,
                                    FadeLevel() / kFadeSteps);
    else
        Renderer->DrawSurface(surface, GetPosX(), GetPosY());
}

void TDefPane::MouseClick(int32_t button, int32_t x, int32_t y)
{
    if (!open || finishing) return;
    if (button == MB_LEFTDOWN)
        OnMouseDown(x, y);
    else if (button == MB_LEFTUP)
        if (const char* name = OnMouseUp(x, y))
        {
            const std::string activated = name;
            Activate(activated.c_str());
        }
}

void TDefPane::MouseMove(int32_t button, int32_t x, int32_t y)
{
    (void)button;
    if (open && !finishing)
        OnMouseMove(x, y);
}

void TDefPane::KeyPress(int32_t key, bool down)
{
    if (open && !finishing)
        OnKey(key, down);
}

// REVSYNC: 0x00436460 -> the EDIT's char handler 0x00432f70: a printable
// character goes on the end of the text being edited, up to MAXLEN.
void TDefPane::CharPress(int32_t key, bool down)
{
    if (!open || finishing || !down)
        return;
    SDefWidget* ed = EditingWidget();
    if (!ed || key < 0x20 || key > 0x7e)
        return;
    if (ed->maxlen <= 0 || static_cast<int32_t>(ed->text.size()) < ed->maxlen)
    {
        ed->text.push_back(static_cast<char>(key));
        SetDirty(true);
    }
}

void TDefPane::Activate(const char* widgetName)
{
    int32_t buttonIndex = 0;
    for (const SDefWidget& w : widgets)
    {
        if (w.type != EDefWidget::Button) continue;
        ++buttonIndex;
        if (w.name == widgetName)
        {
            log_info("[defpane] '%s' activated (button %d)", w.name.c_str(), buttonIndex);
            OnActivate(w, buttonIndex);
            return;
        }
    }
}

void TDefPane::OnActivate(const SDefWidget& widget, int32_t buttonIndex)
{
    if (onActivate)
        onActivate(*this, widget, buttonIndex);
}

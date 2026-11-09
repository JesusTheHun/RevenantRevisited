// *************************************************************************
// *                  Revenant Revisited (port) - 2026                     *
// *   inputqueue.cpp - TInputQueue, the game's input in frame order       *
// *************************************************************************

#include "inputqueue.h"

#include "cursor.h"
#include "revenant.h"
#include "screen.h"

TInputQueue InputQueue;

SInputEvent SInputEvent::Key(int32_t vk, bool down)
{
    SInputEvent e;
    e.kind = EKind::Key;
    e.code = vk;
    e.down = down;
    return e;
}

SInputEvent SInputEvent::Char(int32_t code)
{
    SInputEvent e;
    e.kind = EKind::Char;
    e.code = code;
    return e;
}

SInputEvent SInputEvent::Wheel(int32_t units, int32_t x, int32_t y)
{
    SInputEvent e;
    e.kind = EKind::Wheel;
    e.code = units;
    e.x = x;
    e.y = y;
    return e;
}

SInputEvent SInputEvent::Pointer(int32_t x, int32_t y, int32_t buttons)
{
    SInputEvent e;
    e.kind    = EKind::Pointer;
    e.x       = x;
    e.y       = y;
    e.buttons = buttons & kButtonsAll;
    return e;
}

void TInputQueue::Track(SState& state, const SInputEvent& event)
{
    switch (event.kind)
    {
      case SInputEvent::EKind::Key:
        if (event.code == VK_CONTROL)
            state.ctrl = event.down;
        else if (event.code == VK_SHIFT)
            state.shift = event.down;
        break;
      case SInputEvent::EKind::Pointer:
        state.x       = event.x;
        state.y       = event.y;
        state.buttons = event.buttons;
        break;
      case SInputEvent::EKind::Char:
      case SInputEvent::EKind::Wheel:
        break;
    }
}

void TInputQueue::Push(const SInputEvent& event)
{
    // A move right after a move: only the latest position matters. (A move
    // after a press stays separate: it is a drag.)
    const bool move = event.kind == SInputEvent::EKind::Pointer && event.buttons == queued.buttons;
    if (move && tailIsMove)
        pending.back() = event;
    else
        pending.push_back(event);
    tailIsMove = move;
    Track(queued, event);
}

void TInputQueue::PushMove(int32_t x, int32_t y)
{
    Push(SInputEvent::Pointer(x, y, queued.buttons));
}

void TInputQueue::PushButton(int32_t button, bool down, int32_t x, int32_t y)
{
    const int32_t held = down ? (queued.buttons | button) : (queued.buttons & ~button);
    Push(SInputEvent::Pointer(x, y, held));
}

void TInputQueue::Drain(TScreen* screen)
{
    // Dispatching can queue more (a screen that synthesizes input); those
    // wait for the next frame, as they would have arrived after this drain.
    std::vector<SInputEvent> events;
    events.swap(pending);
    tailIsMove = false;
    for (const SInputEvent& event : events)
    {
        if (observer)
            observer(event);
        Dispatch(screen, event);
    }
}

void TInputQueue::Reset(int32_t x, int32_t y)
{
    pending.clear();
    tailIsMove = false;
    queued   = {};
    queued.x = x;
    queued.y = y;
    buttons  = 0;
    cursorx  = x;
    cursory  = y;
    mousebutton = 0;
    CtrlDown = ShiftDown = AltDown = false;
}

// The screen gets what the hardware handlers used to send it: a key with
// the modifier globals kept up to date, a character, a move (with the held
// buttons) when the pointer moved, then a click per button that changed.
void TInputQueue::Dispatch(TScreen* screen, const SInputEvent& event)
{
    switch (event.kind)
    {
      case SInputEvent::EKind::Key:
        if (event.code == VK_CONTROL)
            CtrlDown = event.down;
        else if (event.code == VK_MENU)
            AltDown = event.down;
        else if (event.code == VK_SHIFT)
            ShiftDown = event.down;
        if (screen)
            screen->KeyPress(event.code, event.down);
        return;

      case SInputEvent::EKind::Char:
        if (screen)
            screen->CharPress(event.code, true);
        return;

      case SInputEvent::EKind::Wheel:
        if (screen)
            screen->MouseWheel(event.code, event.x, event.y);
        return;

      case SInputEvent::EKind::Pointer:
        break;
    }

    if (event.x != cursorx || event.y != cursory)
    {
        cursorx = event.x;
        cursory = event.y;
        if (screen)
            screen->MouseMove(mousebutton, cursorx, cursory);
    }

    // The game's button codes. mousebutton keeps the hardware handler's
    // bookkeeping (MB_MIDDLEDOWN is 3, so a middle press reads as both).
    struct SButton { int32_t bit; int32_t downcode; int32_t upcode; };
    static constexpr SButton kButtons[] = {
        { SInputEvent::kButtonLeft,   MB_LEFTDOWN,   MB_LEFTUP   },
        { SInputEvent::kButtonRight,  MB_RIGHTDOWN,  MB_RIGHTUP  },
        { SInputEvent::kButtonMiddle, MB_MIDDLEDOWN, MB_MIDDLEUP },
    };
    const int32_t changed = event.buttons ^ buttons;
    buttons = event.buttons;
    for (const SButton& b : kButtons)
    {
        if (!(changed & b.bit))
            continue;
        const bool down = (event.buttons & b.bit) != 0;
        if (down)
            mousebutton |= b.downcode;
        else
            mousebutton &= ~b.downcode;
        if (screen)
            screen->MouseClick(down ? b.downcode : b.upcode, cursorx, cursory);
    }
}

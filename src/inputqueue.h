// *************************************************************************
// *                  Revenant Revisited (port) - 2026                     *
// *   inputqueue.h - TInputQueue, the game's input in frame order         *
// *************************************************************************
//
// Every input the game acts on goes through one queue: live hardware
// (AppEvent, after its ImGui and focus gates), the --input-script
// simulator, the playtest control channel and a replayed recording. The
// queue drains at one point of the frame, just before the screen's tick
// (AppFrame), and dispatches to the current screen in order. That drain is
// also where a recording is written, so live play and its replay dispatch
// the same events at the same tick. Design: docs/playtest/PLAN.md §6.1;
// the event vocabulary is the recording's (docs/playtest/PROTOCOL.md §2).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class TScreen;

// One input event as the game receives it: a Windows virtual key, a
// Windows-1252 character (WM_CHAR), or the whole pointer state -- position
// in 640x480 game-screen pixels and the held buttons. The dispatch derives
// moves, presses and releases from the change in pointer state.
struct SInputEvent
{
    enum class EKind : uint8_t { Key, Char, Pointer };

    static constexpr int32_t kButtonLeft   = 1 << 0;
    static constexpr int32_t kButtonRight  = 1 << 1;
    static constexpr int32_t kButtonMiddle = 1 << 2;
    static constexpr int32_t kButtonsAll   = kButtonLeft | kButtonRight | kButtonMiddle;

    EKind   kind    = EKind::Key;
    int32_t code    = 0;        // Key: VK code; Char: Windows-1252 code
    bool    down    = false;    // Key
    int32_t x       = 0;        // Pointer
    int32_t y       = 0;
    int32_t buttons = 0;        // Pointer: kButton* held

    [[nodiscard]] static SInputEvent Key(int32_t vk, bool down);
    [[nodiscard]] static SInputEvent Char(int32_t code);
    [[nodiscard]] static SInputEvent Pointer(int32_t x, int32_t y, int32_t buttons);
};

class TInputQueue
{
  public:
    // ---- producers ----
    // A pointer event that only moves replaces a move still waiting at the
    // end of the queue: a frame dispatches the latest position once (a
    // trackpad sends hundreds a second).
    void Push(const SInputEvent& event);
    void PushKey(int32_t vk, bool down) { Push(SInputEvent::Key(vk, down)); }
    void PushChar(int32_t code)         { Push(SInputEvent::Char(code)); }
    // The pointer moved to (x, y), its buttons as already queued.
    void PushMove(int32_t x, int32_t y);
    // `button` (a kButton* bit) went down or up at (x, y).
    void PushButton(int32_t button, bool down, int32_t x, int32_t y);

    // The state once everything queued has been dispatched, for producers
    // that build on it (a press keeps the buttons already held).
    [[nodiscard]] int32_t QueuedButtons() const { return queued.buttons; }
    [[nodiscard]] bool QueuedCtrl() const       { return queued.ctrl; }
    [[nodiscard]] bool QueuedShift() const      { return queued.shift; }

    // ---- the drain ----
    // Dispatches everything queued to `screen` (none: the state still
    // follows), in order. Each event reaches the observer as it dispatches.
    void Drain(TScreen* screen);

    // Called with each event as it dispatches: the recorder.
    using TObserver = std::function<void(const SInputEvent&)>;
    void SetObserver(TObserver observer) { this->observer = std::move(observer); }

    // A session's start: nothing queued, nothing held, the pointer at
    // (x, y) -- the protocol's starting state (PROTOCOL.md §2).
    void Reset(int32_t x, int32_t y);
    // Preserve held input when the game canvas changes size.
    void RescalePointer(int32_t oldWidth, int32_t oldHeight, int32_t newWidth, int32_t newHeight);

  private:
    struct SState
    {
        int32_t x       = 0;
        int32_t y       = 0;
        int32_t buttons = 0;
        bool    ctrl    = false;
        bool    shift   = false;
    };

    void Dispatch(TScreen* screen, const SInputEvent& event);
    static void Track(SState& state, const SInputEvent& event);

    std::vector<SInputEvent> pending;
    bool      tailIsMove = false;   // pending's last event only moved the pointer
    SState    queued;           // after everything pushed so far
    int32_t   buttons = 0;      // the buttons dispatched (cursorx/cursory: the position)
    TObserver observer;
};

extern TInputQueue InputQueue;

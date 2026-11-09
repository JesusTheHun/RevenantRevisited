"""Compile production function bodies with small asset-free fixtures."""
from pathlib import Path
import argparse
ROOT = Path(__file__).resolve().parents[1]
def function(file, signature):
    text = (ROOT / file).read_text(encoding='utf-8')
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
prelude = r'''

#include "inputqueue.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>
constexpr int VK_CONTROL=17, VK_MENU=18, VK_SHIFT=16;
constexpr int MB_LEFTDOWN=1, MB_RIGHTDOWN=2, MB_MIDDLEDOWN=3;
constexpr int MB_LEFTUP=4, MB_RIGHTUP=5, MB_MIDDLEUP=6;
int32_t cursorx=0,cursory=0,mousebutton=0;
bool CtrlDown=false,ShiftDown=false,AltDown=false;
class TScreen {
public:
    std::vector<std::string> events;
    void KeyPress(int key,bool down) { events.push_back("key:"+std::to_string(key)+(down?":down":":up")); }
    void CharPress(int,bool) {}
    void MouseMove(int,int,int) { events.push_back("move"); }
    void MouseClick(int button,int,int) { events.push_back("button:"+std::to_string(button)); }
    void MouseWheel(int units,int x,int y) {
        events.push_back("wheel:"+std::to_string(units)+":"+std::to_string(x)+":"+std::to_string(y));
    }
};
constexpr int kZoomMin=100,kZoomMax=300,kZoomStep=10;
class TAutoMap {
public:
    int zoom=100,wheelRemainder=0,scrollx=0,scrolly=0;
    bool freescroll=false;
    int HalfSpan() const { return 10000/zoom; }
    void RebuildTileCache() {}
    void SetDirty(bool) {}
    void ScrollZoom(int32_t units);
};

'''
prelude = prelude.replace('"inputqueue.h"', '"' + (ROOT / 'src/inputqueue.h').as_posix() + '"')
text = prelude + "\n"
queue = (ROOT / 'src/inputqueue.cpp').read_text(encoding='utf-8')
text += queue[queue.index('TInputQueue InputQueue;'):] + '\n'
text += function('src/automap.cpp', 'void TAutoMap::ScrollZoom(') + "\n"
text += r'''
TEST(GameInput, WheelKeepsOrderingCoordinatesAndHeldButtons) {
    TInputQueue queue; TScreen screen; queue.Reset(0,0);
    queue.PushButton(SInputEvent::kButtonRight,true,10,20);
    queue.PushKey('R',true);
    queue.Push(SInputEvent::Wheel(120,700,400));
    queue.PushKey('R',false);
    queue.PushButton(SInputEvent::kButtonRight,false,10,20);
    queue.Drain(&screen);
    const std::vector<std::string> expected={"move","button:2","key:82:down","wheel:120:700:400","key:82:up","button:5"};
    EXPECT_EQ(screen.events,expected);
    EXPECT_EQ(mousebutton,0);
    EXPECT_EQ(queue.QueuedButtons(),0);
}
TEST(GameInput, WheelZoomAccumulatesSmallDeltasAndClamps) {
    TAutoMap map;
    map.ScrollZoom(60); EXPECT_EQ(map.zoom,100);
    map.ScrollZoom(60); EXPECT_EQ(map.zoom,110);
    map.ScrollZoom(240); EXPECT_EQ(map.zoom,130);
    map.ScrollZoom(-120); EXPECT_EQ(map.zoom,120);
    map.ScrollZoom(12000); EXPECT_EQ(map.zoom,300);
    map.ScrollZoom(-12000); EXPECT_EQ(map.zoom,100);
}
TEST(GameInput, WheelZoomPreservesDraggedMapCentre) {
    TAutoMap map; map.freescroll=true; map.scrollx=50; map.scrolly=75;
    const int x=map.scrollx+map.HalfSpan(), y=map.scrolly+map.HalfSpan();
    map.ScrollZoom(120);
    EXPECT_EQ(map.scrollx+map.HalfSpan(),x);
    EXPECT_EQ(map.scrolly+map.HalfSpan(),y);
}
'''
args.output.write_text(text, encoding="utf-8")

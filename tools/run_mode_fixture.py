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
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>
enum ACTION { ACTION_ANIMATE, ACTION_MOVE, ACTION_COMBAT, ACTION_COMBATMOVE, ACTION_BOW, ACTION_BOWMOVE };
constexpr int RESNAMELEN=64;
char* StName(const char* root,const char* suffix) {
    static char name[RESNAMELEN];
    std::strcpy(name,root); std::strcat(name,suffix); return name;
}
struct TActionBlock {
    char name[RESNAMELEN]{};
    ACTION action;
    bool interrupt=false;
    int moveangle=0;
    TActionBlock(const char* n,ACTION a) : action(a) { std::strcpy(name,n); }
    TActionBlock(const TActionBlock& old,const char* n,ACTION a) : TActionBlock(old) {
        std::strcpy(name,n); action=a;
    }
    bool Is(ACTION a) const { return action==a; }
};
class TCharacter {
public:
    TActionBlock *root=nullptr,*doing=nullptr,*desired=nullptr;
    std::vector<std::unique_ptr<TActionBlock>> owned;
    std::set<std::string> animations;
    using Seam=bool(*)(TCharacter*);
    static inline Seam setRunModeSeam=nullptr;
    const char* GetCombatRoot() const { return "hand"; }
    const char* GetBowRoot() const { return "bow"; }
    bool HasActionAni(const char* n) const { return animations.count(n)>0; }
    bool IsRunMode() const { return root && (std::string(root->name)=="run" || std::string(root->name)=="handrun" || std::string(root->name)=="bowrun"); }
    bool IsMoving()
        { return doing && (doing->action == ACTION_MOVE ||
                           doing->action == ACTION_COMBATMOVE ||
                           doing->action == ACTION_BOWMOVE); }
    void SetRoot(TActionBlock* b) { owned.emplace_back(b); root=b; }
    void SetDesired(TActionBlock* b) { if(b!=root) owned.emplace_back(b); desired=b; }
    bool SetRunMode();
};

'''
text = prelude + "\n"
text += function('src/character.cpp', 'bool TCharacter::SetRunMode()') + "\n"
text += r'''
TEST(GameInput, RunCanBePressedBeforeOrDuringEveryMovingAction) {
    for(const auto mode : {ACTION_ANIMATE,ACTION_COMBAT,ACTION_BOW}) {
        for(bool alreadyMoving : {false,true}) {
            TCharacter player;
            const ACTION move=mode==ACTION_ANIMATE?ACTION_MOVE:mode==ACTION_COMBAT?ACTION_COMBATMOVE:ACTION_BOWMOVE;
            const std::string prefix=mode==ACTION_ANIMATE?"run":mode==ACTION_COMBAT?"handrun":"bowrun";
            player.animations={prefix,prefix+"f"};
            player.SetRoot(new TActionBlock("walk",mode));
            player.owned.emplace_back(new TActionBlock("walkf",alreadyMoving?move:mode));
            player.doing=player.owned.back().get(); player.doing->moveangle=96;
            ASSERT_TRUE(player.SetRunMode());
            EXPECT_STREQ(player.root->name,prefix.c_str());
            ASSERT_NE(player.desired,nullptr);
            EXPECT_STREQ(player.desired->name,(prefix+(alreadyMoving?"f":"")).c_str());
            if(alreadyMoving) {
                EXPECT_EQ(player.desired->action,move);
                EXPECT_EQ(player.desired->moveangle,96);
            }
        }
    }
}
'''
args.output.write_text(text, encoding="utf-8")

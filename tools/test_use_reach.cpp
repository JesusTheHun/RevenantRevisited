// Only dependencies are doubled. The generated fixture appends the shipped
// ObjectsAhead, FindUseTarget, TryUse and PlayMouseRelease method bodies.
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

enum { OBJCLASS_PLAYER, OBJCLASS_CHARACTER, OBJCLASS_CONTAINER, OBJCLASS_EXIT, OBJCLASS_ITEM };
constexpr int OF_INVISIBLE = 1;
struct S3DPoint {
    int x = 0, y = 0, z = 0;
    S3DPoint& operator+=(const S3DPoint& p) { x += p.x; y += p.y; z += p.z; return *this; }
};
inline void ConvertToVector(int angle, int length, S3DPoint& p) {
    const double a = angle * 6.283185307179586 / 256;
    p = {int(std::lround(std::cos(a)*length)),int(std::lround(std::sin(a)*length)),0};
}
class TObjectInstance {
public:
    int kind = OBJCLASS_EXIT, flags = 0, index = 1, uses = 0, usedWith = -1;
    S3DPoint pos;
    bool enemy = false, item = false;
    virtual ~TObjectInstance() = default;
    int ObjClass() const { return kind; }
    int GetFlags() const { return flags; }
    int GetMapIndex() const { return index; }
    void GetPos(S3DPoint& out) const { out = pos; }
    const char* GetName() const { return "door"; }
    const char* GetTypeName() const { return "Door1"; }
    int Distance(const TObjectInstance* other) const {
        const double x=pos.x-other->pos.x,y=pos.y-other->pos.y,z=pos.z-other->pos.z;
        return int(std::sqrt(x*x+y*y+z*z));
    }
    bool IsInventoryItem() const { return item; }
    bool Use(TObjectInstance*, int with) { ++uses; usedWith=with; return true; }
};
class TCharacter : public TObjectInstance {
public:
    int health = 100, face = 0, pickups = 0;
    bool combat = false, bow = false, drawn = false;
    int Health() const { return health; }
    int GetFace() const { return face; }
    bool IsEnemy(const TCharacter* target) const { return target->enemy; }
    bool IsCombat() const { return combat; }
    bool IsBowMode() const { return bow; }
    bool IsBowDrawn() const { return drawn; }
    void ShootBow(int) {}
    void Pickup(TObjectInstance*) { ++pickups; }
    TObjectInstance* FindUseTarget(TObjectInstance* target = nullptr);
    bool TryUse();
};
inline TCharacter* Player = nullptr;
struct TTextBar { int messages=0; void Print(const char*, ...) { ++messages; } };
inline TTextBar TextBar;
struct TDialogList {
    int FindLine(const char*) const { return -1; }
    const char* GetLine(const char* key) const { return key; }
};
inline TDialogList DialogList;
inline std::string ShownName(const char* name) { return name; }
inline void log_info(const char*, ...) {}
template<class T> struct TSafeRef {
    T* ptr;
    explicit TSafeRef(T* p) : ptr(p) {}
    T* Get() const { return ptr; }
};
namespace UIDragState {
inline TObjectInstance* held = nullptr;
inline TObjectInstance* HeldItem() { return held; }
}
inline TObjectInstance* TakenObject = nullptr;
class TMapPane {
public:
    std::vector<TObjectInstance*> objects;
    TObjectInstance* pointed = nullptr;
    bool clicked = true, clickwalk = false;
    int onobject = 1, walks = 0, drops = 0;
    int FindObjectsInRange(const S3DPoint& p, int32_t* out, int reach, int height, int kind, int limit) {
        EXPECT_EQ(reach,60); EXPECT_EQ(height,0); EXPECT_EQ(kind,-1); EXPECT_EQ(limit,10);
        std::vector<TObjectInstance*> found;
        for(auto* o:objects) {
            const int dx=p.x-o->pos.x,dy=p.y-o->pos.y,dz=p.z-o->pos.z;
            if(dx*dx+dy*dy+dz*dz < reach*reach) found.push_back(o);
        }
        std::stable_sort(found.begin(),found.end(),[&](auto* a,auto* b) {
            const auto distance=[&](auto* o) {const auto q=o->pos; return (q.x-p.x)*(q.x-p.x)+(q.y-p.y)*(q.y-p.y)+(q.z-p.z)*(q.z-p.z);};
            return distance(a)<distance(b);
        });
        const int n=std::min(limit,int(found.size()));
        for(int i=0;i<n;++i) out[i]=found[i]->index;
        return n;
    }
    TObjectInstance* GetInstance(int index) {
        for(auto* o:objects) if(o->index==index) return o;
        return nullptr;
    }
    TObjectInstance* OnObject(int,int,TObjectInstance*) { return pointed; }
    int GetMouseMapAngle() { return 0; }
    void WalkToward(int,int,TObjectInstance*) { ++walks; }
    void ClickWalkTo(int,int) { ++walks; }
    void DropHeldItem(TObjectInstance*,int,int) { ++drops; }
    void PlayMouseRelease(int32_t,int32_t,bool);
};
inline TMapPane MapPane;

namespace {
class UseReach : public testing::Test {
protected:
    TCharacter actor;
    TObjectInstance door;
    void SetUp() override {
        actor.kind=OBJCLASS_PLAYER; actor.index=0;
        Player=&actor; door.pos={60,0,0};
        MapPane={}; MapPane.objects={&actor,&door}; MapPane.pointed=&door;
        UIDragState::held=nullptr; TextBar.messages=0;
    }
};

TEST_F(UseReach, MouseAndKeyboardUseTheSameForwardReach) {
    for(const S3DPoint at: {S3DPoint{60,0,0},{110,0,0},{120,0,0},{0,80,0},{-1,0,0},{60,0,61}}) {
        SCOPED_TRACE(testing::Message()<<at.x<<","<<at.y<<","<<at.z);
        door.pos=at; door.uses=0;
        const bool keyboard=actor.TryUse();
        EXPECT_EQ(door.uses,keyboard?1:0);
        door.uses=0;
        MapPane.PlayMouseRelease(0,0,false);
        EXPECT_EQ(door.uses,keyboard?1:0);
        EXPECT_EQ(MapPane.walks,0); // a distant door is not activated remotely
    }
}

TEST_F(UseReach, ExplicitTargetDoesNotUseAnotherNearbyObject) {
    TObjectInstance chest;
    chest.kind=OBJCLASS_CONTAINER; chest.index=2; chest.pos={20,0,0};
    MapPane.objects.push_back(&chest);
    door.pos={130,0,0};
    EXPECT_EQ(actor.FindUseTarget(),&chest);
    EXPECT_EQ(actor.FindUseTarget(&door),nullptr);
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(chest.uses,0);
    EXPECT_EQ(door.uses,0);
}

TEST_F(UseReach, FacingAndVisibilityApplyEquallyToBothInputs) {
    actor.face=128;
    EXPECT_FALSE(actor.TryUse());
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,0);
    actor.face=0; door.flags=OF_INVISIBLE;
    EXPECT_FALSE(actor.TryUse());
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,0);
    door.flags=0; actor.health=0;
    EXPECT_FALSE(actor.TryUse());
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,0);
}

TEST_F(UseReach, DraggingAKeyCannotBypassDoorReach) {
    TObjectInstance key; key.index=42;
    UIDragState::held=&key; MapPane.clicked=false;
    door.pos={0,80,0};
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,0); EXPECT_EQ(MapPane.drops,0);
    door.pos={60,0,0};
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,1); EXPECT_EQ(door.usedWith,42);
}

TEST_F(UseReach, ReleaseOnADifferentObjectDoesNotUseTheDoor) {
    MapPane.onobject=99;
    MapPane.PlayMouseRelease(0,0,false);
    EXPECT_EQ(door.uses,0);
}
} // namespace

#include <gtest/gtest.h>
#include <memory>
#include <vector>

struct TObjectInstance {
    bool item = false;
    bool IsInventoryItem() const { return item; }
};
struct SPartSysTrack { int prototype = -1; };
struct T3DImagery { std::vector<SPartSysTrack> partsys_tracks; };
struct SPartSysControllers {
    struct Controller { SPartSysTrack track; };
    bool unsupported = false;
    std::vector<Controller> controllers;
};
struct T3DAnimator {
    TObjectInstance* inst = nullptr;
    T3DImagery* imagery = nullptr;
    std::unique_ptr<SPartSysControllers> partsys_controllers;
    T3DImagery* Get3DImagery() const { return imagery; }
    bool PartSysOwnsObject(int object) const;
};

class PartSysOwnership : public ::testing::Test {
protected:
    TObjectInstance item{true};
    T3DImagery imagery{{{2}, {7}}};
    T3DAnimator animator;
    void SetUp() override {
        animator.inst = &item;
        animator.imagery = &imagery;
        animator.partsys_controllers = std::make_unique<SPartSysControllers>();
        animator.partsys_controllers->unsupported = true;
    }
};

TEST_F(PartSysOwnership, UnsupportedItemRetainsAuthoredBaseMeshes) {
    EXPECT_FALSE(animator.PartSysOwnsObject(0));
    EXPECT_FALSE(animator.PartSysOwnsObject(3));
}
TEST_F(PartSysOwnership, UnsupportedItemHidesEveryAuthoredPrototype) {
    EXPECT_TRUE(animator.PartSysOwnsObject(2));
    EXPECT_TRUE(animator.PartSysOwnsObject(7));
}
TEST_F(PartSysOwnership, UnsupportedEffectsStillHideEntireOwner) {
    item.item = false;
    EXPECT_TRUE(animator.PartSysOwnsObject(0));
    EXPECT_TRUE(animator.PartSysOwnsObject(2));
}
TEST_F(PartSysOwnership, UnresolvedPrototypeKeepsItemHidden) {
    imagery.partsys_tracks.push_back({-1});
    EXPECT_TRUE(animator.PartSysOwnsObject(0));
}
TEST_F(PartSysOwnership, MissingOwnerOrImageryKeepsUnsupportedMeshHidden) {
    animator.inst = nullptr;
    EXPECT_TRUE(animator.PartSysOwnsObject(0));
    animator.inst = &item;
    animator.imagery = nullptr;
    EXPECT_TRUE(animator.PartSysOwnsObject(0));
}
TEST_F(PartSysOwnership, SupportedControllerOwnsOnlyItsLivePrototype) {
    animator.partsys_controllers->unsupported = false;
    animator.partsys_controllers->controllers.push_back({{7}});
    EXPECT_FALSE(animator.PartSysOwnsObject(0));
    EXPECT_FALSE(animator.PartSysOwnsObject(2));
    EXPECT_TRUE(animator.PartSysOwnsObject(7));
}
TEST_F(PartSysOwnership, OrdinaryMeshesHaveNoParticleOwner) {
    animator.partsys_controllers.reset();
    EXPECT_FALSE(animator.PartSysOwnsObject(0));
}

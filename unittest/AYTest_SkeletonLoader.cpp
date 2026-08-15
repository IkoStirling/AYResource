#include "AYResource.h"
#include "AYResource/assetsDefs/ISkeleton.h"
#include "AYResource/assetsImpl/Skeleton.h"
#include "AYResource/Loader/SkeletonLoader.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(SkeletonLoaderTests)

    TEST_CASE(CreateTestSkeleton) {
        Skeleton skeleton;
        skeleton.createTestSkeleton();

        CHECK(skeleton.getBoneCount() == 4);
        CHECK(skeleton.getRootBoneCount() == 1);
        CHECK(skeleton.isLoaded() == true);
    }

    TEST_CASE(FindBone) {
        Skeleton skeleton;
        skeleton.createTestSkeleton();

        CHECK(skeleton.findBone("Root") == 0);
        CHECK(skeleton.findBone("UpperArm") == 1);
        CHECK(skeleton.findBone("LowerArm") == 2);
        CHECK(skeleton.findBone("Hand") == 3);
        CHECK(skeleton.findBone("NonExistent") == -1);
    }

    TEST_CASE(BoneHierarchy) {
        Skeleton skeleton;
        skeleton.createTestSkeleton();

        // Root has no parent
        CHECK(skeleton.getParentBoneIndex(0) == -1);
        // UpperArm's parent is Root
        CHECK(skeleton.getParentBoneIndex(1) == 0);
        // LowerArm's parent is UpperArm
        CHECK(skeleton.getParentBoneIndex(2) == 1);
        // Hand's parent is LowerArm
        CHECK(skeleton.getParentBoneIndex(3) == 2);
    }

    TEST_CASE(SaveAndLoadBinary) {
        Skeleton original;
        original.createTestSkeleton();

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        Skeleton loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getBoneCount() == original.getBoneCount());
        CHECK(loaded.getRootBoneCount() == original.getRootBoneCount());
        CHECK(loaded.findBone("Root") == original.findBone("Root"));
        CHECK(loaded.findBone("UpperArm") == original.findBone("UpperArm"));
        CHECK(loaded.findBone("LowerArm") == original.findBone("LowerArm"));
        CHECK(loaded.findBone("Hand") == original.findBone("Hand"));
    }

    TEST_CASE(LoadNonExistentFile) {
        Skeleton skeleton;
        CHECK(skeleton.load("nonexistent.ayskel") == false);
        CHECK(skeleton.isLoaded() == false);
    }

    TEST_CASE(CanLoad) {
        SkeletonLoader loader;
        CHECK(loader.canLoad("skeleton.ayskel") == true);
        CHECK(loader.canLoad("path/to/bone.ayskel") == true);
        CHECK(loader.canLoad("test.aymesh") == false);
        CHECK(loader.canLoad("test.ayskelabc") == false);
    }

    TEST_CASE(GetResourceType) {
        SkeletonLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Skeleton") == 0);
    }

    TEST_CASE(LoadFromBinary) {
        Skeleton original;
        original.createTestSkeleton();

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        SkeletonLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
        CHECK(skeleton != nullptr);
        CHECK(skeleton->getBoneCount() == 4);
    }

TEST_SUITE_END

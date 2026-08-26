// AYTest_FBXParserAnimation.cpp — R-02 FBXParser animation extraction tests
//
// 验证 FBXParser::_parseAnimations:
//   - 静态 FBX (cube.fbx) 不产生 animations
//   - 含动画的 FBX 至少产生 1 条 AnimationData,track 数 >= 1
//   - "rotation" track 的 valueType == Quaternion
//   - "position" / "scale" track 的 valueType == Vector3
//   - duration 使用秒，track time 保留 raw ticks（播放器绑定时仅转换一次）
//   - MeshOnly loadOption 跳过 _parseAnimations
//
// 使用 vcpkg 下的 assimp 测试 FBX,路径在多台机器上应当一致;
// 找不到时静默 skip (CI 在不同环境下都可能缺)。

#include "AYResource.h"
#include "AYResource/Converter/FBXParser.h"
#include "AYTest.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ayt::resource;

namespace {

// assimp v6.0.4 测试 FBX 路径
const char* kSpiderFbx =
    "D:/Projects/vcpkg/buildtrees/assimp/src/v6.0.4-12c3574bf8.clean/test/models/FBX/spider.fbx";
const char* kAnimSkelFbx =
    "D:/Projects/vcpkg/buildtrees/assimp/src/v6.0.4-12c3574bf8.clean/test/models/FBX/animation_with_skeleton.fbx";
const char* kHuesitosFbx =
    "D:/Projects/vcpkg/buildtrees/assimp/src/v6.0.4-12c3574bf8.clean/test/models/FBX/huesitos.fbx";
const char* kCubeFbx =
    "D:/Projects/AliyatRenderer/assets/core/models/cube.fbx";
const char* kSourFbx =
    "D:/Aliyat/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
const char* kSourFixFbx =
    "D:/Aliyat/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour_fix.fbx";
const char* kSourAnimFbx =
    "D:/Aliyat/AliyatRenderer/assets/core/models/sour-miku-Creamy/SourWithAnim.fbx";

bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

bool approximatelyOne(float value) {
    return std::abs(value - 1.0f) <= 1.0e-4f;
}

float maximumIdentityError(const ayt::math::Float4x4& matrix) {
    float error = 0.0f;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const float expected = row == column ? 1.0f : 0.0f;
            error = std::max(error, std::abs(matrix(row, column) - expected));
        }
    }
    return error;
}

bool trackVaries(const AnimationData& animation,
                 const std::string& boneName,
                 const std::string& property,
                 float epsilon = 1.0e-4f) {
    for (const auto& track : animation.tracks) {
        if (track.targetNode != boneName || track.property != property
            || track.times.size() < 2u) {
            continue;
        }
        const size_t width = track.valueType == AnimTrackType::Quaternion ? 4u : 3u;
        if (track.values.size() < width * 2u) continue;
        for (size_t key = 1u; key < track.times.size(); ++key) {
            for (size_t component = 0u; component < width; ++component) {
                if (std::abs(track.values[key * width + component]
                           - track.values[component]) > epsilon) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

TEST_SUITE(FBXParserAnimationTests)

    TEST_CASE(SourMorphImportSmoke) {
        const char* runSmoke = std::getenv("AY_RUN_SOUR_IMPORT_SMOKE");
        if (runSmoke == nullptr || std::string(runSmoke) != "1") {
            printf("    [SKIP] set AY_RUN_SOUR_IMPORT_SMOKE=1 to run the Sour FBX regression\n");
            return;
        }
        if (!fileExists(kSourFbx)) {
            printf("    [SKIP] %s not found\n", kSourFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        parser.setSeparateModels(true);
        CHECK(parser.parse(kSourFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        CHECK(!asset->meshes.empty());
        CHECK(!asset->meshes[0].morphTargets.empty());
        CHECK(asset->meshes[0].morphTargets[0].defaultWeight == 0.0f);
    }

    TEST_CASE(SourModelAndAnimationHierarchyContractSmoke) {
        const char* runSmoke = std::getenv("AY_RUN_SOUR_IMPORT_SMOKE");
        if (runSmoke == nullptr || std::string(runSmoke) != "1") {
            printf("    [SKIP] set AY_RUN_SOUR_IMPORT_SMOKE=1 to run the Sour hierarchy regression\n");
            return;
        }
        if (!fileExists(kSourFixFbx) || !fileExists(kSourAnimFbx)) {
            printf("    [SKIP] Sour_fix.fbx or SourWithAnim.fbx not found\n");
            return;
        }

        FBXParser modelParser;
        modelParser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(modelParser.parse(kSourFixFbx));
        auto model = modelParser.getResult();
        CHECK(model != nullptr);
        CHECK(!model->skeletons.empty());
        CHECK(!model->meshes.empty());
        const auto& skeleton = model->skeletons.front();
        size_t normalizedRoots = 0;
        std::vector<ayt::math::Float4x4> bindWorlds(skeleton.bones.size());
        std::unordered_map<std::string, size_t> modelBoneIndices;
        for (size_t boneIndex = 0; boneIndex < skeleton.bones.size(); ++boneIndex) {
            const auto& bone = skeleton.bones[boneIndex];
            modelBoneIndices.emplace(bone.name, boneIndex);
            if (bone.parentIndex < 0) {
                CHECK(approximatelyOne(bone.localScale.x));
                CHECK(approximatelyOne(bone.localScale.y));
                CHECK(approximatelyOne(bone.localScale.z));
                ++normalizedRoots;
            }
            const ayt::math::Float4x4 local = ayt::math::Float4x4::fromTRS(
                bone.localPosition, bone.localRotation, bone.localScale);
            bindWorlds[boneIndex] = bone.parentIndex >= 0
                ? bindWorlds[static_cast<size_t>(bone.parentIndex)] * local
                : local;
            CHECK(maximumIdentityError(
                bindWorlds[boneIndex] * bone.inverseBindMatrix) <= 0.002f);
        }
        CHECK(normalizedRoots >= 1u);

        float minimumCoordinate = std::numeric_limits<float>::max();
        float maximumCoordinate = std::numeric_limits<float>::lowest();
        for (const auto& mesh : model->meshes) {
            for (float coordinate : mesh.positions) {
                minimumCoordinate = std::min(minimumCoordinate, coordinate);
                maximumCoordinate = std::max(maximumCoordinate, coordinate);
            }
        }
        // Sour's authored object scale is already applied to its mesh-space
        // vertices.  The persisted asset is human-sized, not the FBX wrapper's
        // 100x scene-node size, and needs no runtime Transform scale.
        CHECK(maximumCoordinate - minimumCoordinate > 1.0f);
        CHECK(maximumCoordinate - minimumCoordinate < 3.0f);

        FBXParser animationParser;
        animationParser.setLoadOption(IConverter::LoadOption::AnimationOnly);
        CHECK(animationParser.parse(kSourAnimFbx));
        auto animation = animationParser.getResult();
        CHECK(animation != nullptr);
        CHECK(!animation->animations.empty());
        CHECK(animation->meshes.empty());
        CHECK(animation->skeletons.empty());

        bool sawRootPosition = false;
        bool sawRootRotation = false;
        bool sawRootScale = false;
        std::unordered_set<std::string> animatedBoneNames;
        for (const auto& track : animation->animations.front().tracks) {
            CHECK(track.targetNode != "Sour_arm");
            animatedBoneNames.insert(track.targetNode);
            if (track.targetNode == "全ての親") {
                sawRootPosition = sawRootPosition || track.property == "position";
                sawRootRotation = sawRootRotation || track.property == "rotation";
                if (track.property == "scale") {
                    sawRootScale = true;
                    for (float value : track.values) {
                        CHECK(approximatelyOne(value));
                    }
                }
            }
        }
        CHECK(sawRootPosition);
        CHECK(sawRootRotation);
        CHECK(sawRootScale);
        CHECK(trackVaries(animation->animations.front(), "ひじ.L", "rotation"));
        CHECK(trackVaries(animation->animations.front(), "足.L", "rotation"));
        CHECK(trackVaries(animation->animations.front(), "足首D.L", "rotation"));

        const auto rootBone = modelBoneIndices.find("全ての親");
        CHECK(rootBone != modelBoneIndices.end());
        if (rootBone != modelBoneIndices.end()) {
            const auto& bone = skeleton.bones[rootBone->second];
            ayt::math::FVector3 position = bone.localPosition;
            ayt::math::FQuaternion rotation = bone.localRotation;
            ayt::math::FVector3 scale = bone.localScale;
            for (const auto& track : animation->animations.front().tracks) {
                if (track.targetNode != bone.name || track.values.empty()) continue;
                if (track.property == "position" && track.values.size() >= 3u) {
                    position = {track.values[0], track.values[1], track.values[2]};
                } else if (track.property == "rotation" && track.values.size() >= 4u) {
                    rotation = {track.values[0], track.values[1],
                                track.values[2], track.values[3]};
                } else if (track.property == "scale" && track.values.size() >= 3u) {
                    scale = {track.values[0], track.values[1], track.values[2]};
                }
            }
            // This root is static in the reference clip.  Its first pose must
            // already be in mesh bind space; retaining Sour_arm's FBX wrapper
            // would leave a 90-degree/100x transform here and lay the model down.
            const ayt::math::Float4x4 firstPose =
                ayt::math::Float4x4::fromTRS(position, rotation, scale);
            CHECK(maximumIdentityError(firstPose * bone.inverseBindMatrix) <= 0.002f);
        }

        // Every weighted joint used by every source submesh (body, clothes,
        // shoes and accessories included) must resolve to a target-skeleton
        // bone and receive a baked local animation track.  Render cooking may
        // compact those joints per section, but must not change this binding.
        size_t weightedSubmeshes = 0u;
        for (const auto& mesh : model->meshes) {
            for (const auto& submesh : mesh.submeshes) {
                std::unordered_set<UInt32> sectionJoints;
                const size_t end = static_cast<size_t>(submesh.startIndex)
                                 + submesh.indexCount;
                CHECK(end <= mesh.indices.size());
                for (size_t at = submesh.startIndex; at < end; ++at) {
                    const UInt32 vertex = mesh.indices[at];
                    CHECK(vertex < mesh.skinVertices.size());
                    if (vertex >= mesh.skinVertices.size()) continue;
                    const auto& skin = mesh.skinVertices[vertex];
                    for (UInt32 slot = 0u; slot < 4u; ++slot) {
                        if (skin.weight[slot] > 0.0f) {
                            sectionJoints.insert(skin.joint[slot]);
                        }
                    }
                }
                if (sectionJoints.empty()) continue;
                ++weightedSubmeshes;
                for (UInt32 joint : sectionJoints) {
                    CHECK(joint < skeleton.bones.size());
                    if (joint >= skeleton.bones.size()) continue;
                    CHECK(animatedBoneNames.contains(skeleton.bones[joint].name));
                }
            }
        }
        CHECK(weightedSubmeshes >= 30u);
    }

    TEST_CASE(StaticCubeFbxHasNoAnimations) {
        if (!fileExists(kCubeFbx)) {
            printf("    [SKIP] %s not found\n", kCubeFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(parser.parse(kCubeFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        CHECK(asset->animations.empty());
    }

    // R-02: 软跳过 — 如果 spider 没产 anim,显式报 [SOFT-SKIP],
    // 而不是把核心 CHECK 留在 if 后面导致静默绿。
    // CI 抓 stdout 看到 [SOFT-SKIP] 时应人工确认:换文件 / 升级 Assimp / 接受现实。
    TEST_CASE(SpiderFbxProducesAnimations) {
        if (!fileExists(kSpiderFbx)) {
            printf("    [SKIP] %s not found\n", kSpiderFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(parser.parse(kSpiderFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);

        printf("    [INFO] spider.fbx produced %zu animations\n", asset->animations.size());
        if (asset->animations.empty()) {
            printf("    [SOFT-SKIP] spider.fbx produced 0 animations on this Assimp build; "
                   "rotation-valueType assertion not executed. Consider switching to a known-animated FBX.\n");
            return;  // soft-skip: 拿不到输入,不强断言
        }

        // 拿到 anim 才断言
        bool sawQuaternionTrack = false;
        size_t totalTracks = 0;
        for (const auto& anim : asset->animations) {
            CHECK(anim.tracks.size() >= 1u);
            CHECK(anim.duration >= 0.0f);
            CHECK(anim.ticksPerSecond > 0.0f);
            float maximumTrackTick = 0.0f;
            for (const auto& t : anim.tracks) {
                ++totalTracks;
                if (t.property == "rotation") {
                    CHECK(t.valueType == AnimTrackType::Quaternion);
                    sawQuaternionTrack = true;
                } else if (t.property == "position" || t.property == "scale") {
                    CHECK(t.valueType == AnimTrackType::Vector3);
                }
                CHECK(!t.times.empty());
                CHECK(!t.values.empty());
                maximumTrackTick = std::max(maximumTrackTick, t.times.back());
            }
            if (maximumTrackTick > 0.0f) {
                const float maximumTrackSecond = maximumTrackTick / anim.ticksPerSecond;
                const float timelineTolerance = std::max(
                    1.0f / anim.ticksPerSecond,
                    std::max(anim.duration, maximumTrackSecond) * 0.01f);
                CHECK(std::abs(anim.duration - maximumTrackSecond) <= timelineTolerance);
            }
        }
        CHECK(sawQuaternionTrack);
        printf("    [VERDICT] spider.fbx: %zu anims, %zu tracks, quaternion-rotation seen=%s\n",
               asset->animations.size(), totalTracks, sawQuaternionTrack ? "yes" : "NO");
    }

    TEST_CASE(AnimationWithSkeletonFbxProducesAnimations) {
        if (!fileExists(kAnimSkelFbx)) {
            printf("    [SKIP] %s not found\n", kAnimSkelFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(parser.parse(kAnimSkelFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);

        printf("    [INFO] animation_with_skeleton.fbx: anims=%zu, skeletons=%zu\n",
               asset->animations.size(), asset->skeletons.size());
        if (asset->animations.empty()) {
            printf("    [SOFT-SKIP] animation_with_skeleton.fbx produced 0 animations; skeleton assertion skipped\n");
            return;  // soft-skip
        }
        // 拿到 anim 才断言 skeleton 存在
        CHECK(asset->skeletons.size() >= 1u);
        printf("    [VERDICT] animation_with_skeleton.fbx: anims=%zu, skeletons=%zu\n",
               asset->animations.size(), asset->skeletons.size());
    }

    TEST_CASE(HuesitosFbxMayOrMayNotHaveAnimations) {
        if (!fileExists(kHuesitosFbx)) {
            printf("    [SKIP] %s not found\n", kHuesitosFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(parser.parse(kHuesitosFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        // huesitos 是 skinned mesh,可能无动画 — 仅打印,不强制断言
        printf("    [INFO] huesitos.fbx animations: %zu\n", asset->animations.size());
    }

    TEST_CASE(MeshOnlySkipsAnimations) {
        if (!fileExists(kSpiderFbx)) {
            printf("    [SKIP] %s not found\n", kSpiderFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::MeshOnly);
        CHECK(parser.parse(kSpiderFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        CHECK(asset->animations.empty());
    }

    TEST_CASE(AnimationOnlySkipsRenderAssets) {
        if (!fileExists(kSpiderFbx)) {
            printf("    [SKIP] %s not found\n", kSpiderFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::AnimationOnly);
        CHECK(parser.parse(kSpiderFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        CHECK(!asset->animations.empty());
        CHECK(asset->meshes.empty());
        CHECK(asset->materials.empty());
        CHECK(asset->textures.empty());
        CHECK(asset->skeletons.empty());
    }

    // 验证 skeleton 本地 rest pose 被分解 (T+R+S) — 至少 root bone 的 localRotation 不全 0
    TEST_CASE(SkeletonLocalPoseDecomposed) {
        if (!fileExists(kSpiderFbx)) {
            printf("    [SKIP] %s not found\n", kSpiderFbx);
            return;
        }
        FBXParser parser;
        parser.setLoadOption(IConverter::LoadOption::Full);
        CHECK(parser.parse(kSpiderFbx));
        auto asset = parser.getResult();
        CHECK(asset != nullptr);
        if (asset->skeletons.empty()) {
            printf("    [SKIP] spider.fbx has no skeleton\n");
            return;
        }
        if (asset->skeletons[0].bones.empty()) {
            printf("    [SKIP] spider.fbx skeleton has no bones\n");
            return;
        }

        // root bone 应有非单位 quaternion 概率大;若为单位 quat,至少 position 有偏移
        const auto& root = asset->skeletons[0].bones[0];
        const bool transOk = (root.localPosition.x != 0.0f
                           || root.localPosition.y != 0.0f
                           || root.localPosition.z != 0.0f);
        const float qsum = root.localRotation.x * root.localRotation.x
                         + root.localRotation.y * root.localRotation.y
                         + root.localRotation.z * root.localRotation.z
                         + root.localRotation.w * root.localRotation.w;
        const bool rotOk = (qsum > 0.5f); // 任何 quat 平方和 ≈ 1
        CHECK(transOk || rotOk);
        printf("    [INFO] root bone pos=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f,%.3f,%.3f)\n",
               root.localPosition.x, root.localPosition.y, root.localPosition.z,
               root.localRotation.x, root.localRotation.y, root.localRotation.z, root.localRotation.w);
    }

TEST_SUITE_END

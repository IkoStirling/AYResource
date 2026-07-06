// AYTest_AnimationConverter.cpp — R-02 AnimationConverter unit tests
//
// 验证 AnimationConverter::convertAll 的:
//   1. valueType 透传到 .ayanm 二进制 (loadFromBinary 后 getTrackType 仍正确)
//   2. 文件名 sanitize (空名 / 保留字符 / 重复 take)
//   3. GUID hash 包含 valueType (类型变化会改变 GUID)
//
// 设计原则:每个 case 必须真实跑 convertAll → 文件落盘 → load 回来。
// 不能绕过 convertAll 手搓 Animation。

#include "AYResource.h"
#include "Converter/AnimationConverter.h"
#include "Loader/AnimationLoader.h"
#include "AYAnimation.h"
#include "IAYAnimation.h"
#include "AYTest.h"

#include <cstring>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

using namespace ayt::resource;

namespace {

// R-02 测试输出目录 — 不与其他 test 共享,避免互相污染
const char* kOutDir = "D:/Projects/AYRuntime/AYResource/test_output_animconv";

// 构建一条最小可用的 AnimationData
AnimationData makeAnimData(const std::string& name,
                           const std::string& bone,
                           AnimTrackType type,
                           const std::vector<float>& values) {
    AnimationData d;
    d.name = name;
    d.duration = 1.0f;
    d.ticksPerSecond = 30.0f;
    KeyframeTrack t;
    t.targetNode = bone;
    t.property = (type == AnimTrackType::Quaternion) ? "rotation" : "position";
    t.valueType = type;
    t.times = { 0.0f, 0.5f, 1.0f };
    t.values = values;
    d.tracks.push_back(std::move(t));
    return d;
}

// 读二进制文件 → buffer;失败时返回空 buffer
std::vector<UInt8> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<UInt8> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

} // namespace

TEST_SUITE(AnimationConverterTests)

    // valueType: 写盘 → 读回 → 类型仍是 Quaternion
    // 之前版本手搓 Animation 绕过了 convertAll;这次必须真过 convertAll
    TEST_CASE(ValueTypeRoundTrip_Quaternion) {
        AnimationConverter conv;
        conv.setOutputDir(kOutDir);

        std::vector<AnimationData> anims;
        anims.push_back(makeAnimData("run", "root",
                                     AnimTrackType::Quaternion,
                                     { 0,0,0,1, 0.1f,0,0,0.99f, 0,0,0,1 }));

        auto res = conv.convertAll(anims, "hero");
        CHECK(res.size() == 1u);

        // convertAll 应给出 animations/hero_run.ayanm
        const std::string vp = res[0].path;
        CHECK(vp.find("animations/hero_run.ayanm") != std::string::npos);

        // 真去盘上读
        std::string fullPath = std::string(kOutDir) + "/" + vp;
        auto bin = readFile(fullPath);
        CHECK(!bin.empty());

        // magic 'AYNM' = 0x4E4D5941 little-endian
        if (bin.size() >= 4) {
            UInt32 magic = *reinterpret_cast<UInt32*>(bin.data());
            CHECK(magic == 0x4E4D5941);
        }

        // load 回来,确认 valueType 真的是 Quaternion (而不是默认的 Vector3)
        AnimationLoader loader;
        auto loaded = std::dynamic_pointer_cast<Animation>(
            loader.loadFromBinary(bin.data(), bin.size()));
        CHECK(loaded != nullptr);
        CHECK(loaded->getTrackCount() == 1u);
        CHECK(loaded->getTrackType(0) == AnimTrackType::Quaternion);
    }

    TEST_CASE(ValueTypeRoundTrip_Vector3) {
        AnimationConverter conv;
        conv.setOutputDir(kOutDir);

        std::vector<AnimationData> anims;
        anims.push_back(makeAnimData("walk", "root",
                                     AnimTrackType::Vector3,
                                     { 0,0,0,  1,2,3,  4,5,6 }));

        auto res = conv.convertAll(anims, "hero");
        CHECK(res.size() == 1u);

        std::string fullPath = std::string(kOutDir) + "/" + res[0].path;
        auto bin = readFile(fullPath);
        CHECK(!bin.empty());

        AnimationLoader loader;
        auto loaded = std::dynamic_pointer_cast<Animation>(
            loader.loadFromBinary(bin.data(), bin.size()));
        CHECK(loaded != nullptr);
        CHECK(loaded->getTrackType(0) == AnimTrackType::Vector3);
        // 顺便:值不丢。raw float pointer 读第 2 key (index 3..5)
        const float* raw = loaded->getTrackValues(0);
        CHECK(raw != nullptr);
        CHECK(raw[3] == 1.0f);
        CHECK(raw[4] == 2.0f);
        CHECK(raw[5] == 3.0f);
    }

    // 文件名 sanitize + dedupe — 真写盘,确认文件存在、名字唯一
    // 输入 5 个 anim:空名×2、含保留字符×1、重复名×2
    TEST_CASE(FilenameSanitization) {
        AnimationConverter conv;
        conv.setOutputDir(kOutDir);

        std::vector<AnimationData> anims;

        AnimationData empty1; empty1.name = ""; empty1.duration = 1.0f;
        AnimationData empty2; empty2.name = ""; empty2.duration = 1.0f;
        anims.push_back(empty1);
        anims.push_back(empty2);

        AnimationData weird;
        weird.name = "take/with:bad*chars";
        weird.duration = 1.0f;
        anims.push_back(weird);

        AnimationData dup1; dup1.name = "run"; dup1.duration = 1.0f;
        AnimationData dup2; dup2.name = "run"; dup2.duration = 1.0f;
        anims.push_back(dup1);
        anims.push_back(dup2);

        auto res = conv.convertAll(anims, "hero");
        CHECK(res.size() == 5u);

        // 路径唯一
        for (size_t i = 0; i < res.size(); ++i) {
            for (size_t j = i + 1; j < res.size(); ++j) {
                CHECK(res[i].path != res[j].path);
            }
        }

        // 真写盘:5 个文件都应该存在,且 magic 对
        for (const auto& r : res) {
            std::string fullPath = std::string(kOutDir) + "/" + r.path;
            auto bin = readFile(fullPath);
            CHECK(!bin.empty());
            if (bin.size() >= 4) {
                UInt32 magic = *reinterpret_cast<UInt32*>(bin.data());
                CHECK(magic == 0x4E4D5941);
            }
        }

        // 保留字符被替换
        bool sawSanitized = false;
        for (const auto& r : res) {
            if (r.path.find("take_with_bad_chars") != std::string::npos) {
                sawSanitized = true;
            }
        }
        CHECK(sawSanitized);

        // 空名 fallback
        bool sawTake0 = false;
        bool sawTake1 = false;
        for (const auto& r : res) {
            if (r.path.find("hero_take_0") != std::string::npos) sawTake0 = true;
            if (r.path.find("hero_take_1") != std::string::npos) sawTake1 = true;
        }
        CHECK(sawTake0);
        CHECK(sawTake1);

        // 重复 dedupe:run 应出现一次,dup 出现一次
        bool sawRun = false;
        bool sawDup = false;
        for (const auto& r : res) {
            if (r.path.find("/hero_run.ayanm") != std::string::npos) sawRun = true;
            if (r.path.find("_dup2") != std::string::npos) sawDup = true;
        }
        CHECK(sawRun);
        CHECK(sawDup);
    }

    // valueType 影响 GUID — 同样 name/duration/ticksPerSecond,只改 valueType,
    // GUID 必须不同(因为 R-02 把 valueType 字节加进了 hash)
    TEST_CASE(ValueTypeAffectsGuid) {
        AnimationConverter conv;
        conv.setOutputDir(kOutDir);

        // Quaternion
        std::vector<AnimationData> animsQ;
        animsQ.push_back(makeAnimData("x", "n", AnimTrackType::Quaternion,
                                      { 0,0,0,1,  0,0,0,1 }));
        auto resQ = conv.convertAll(animsQ, "guidtest");

        // Vector3 — 故意用同样 name/ticks,让 GUID 差异只能来自 valueType
        std::vector<AnimationData> animsV;
        animsV.push_back(makeAnimData("x", "n", AnimTrackType::Vector3,
                                      { 0,0,0,  1,1,1 }));
        auto resV = conv.convertAll(animsV, "guidtest");

        CHECK(resQ.size() == 1u);
        CHECK(resV.size() == 1u);

        // GUID 必须不同
        CHECK(!(resQ[0].guid == resV[0].guid));
    }

TEST_SUITE_END

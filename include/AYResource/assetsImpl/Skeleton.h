#pragma once
#include "AYResource/assetsDefs/ISkeleton.h"
#include "AYResource/IResource.h"
#include "AYResource/IResourceLoader.h"
#include <AYMath/MathTypes.h>
#include <vector>
#include <unordered_map>
#include <string>

namespace ayt::resource
{

// ===== Skeleton — ISkeleton 实现类 =====
class Skeleton : public ISkeleton {
public:
    Skeleton();
    virtual ~Skeleton() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== ISkeleton =====
    size_t getBoneCount() const override { return _bones.size(); }
    const Bone* getBones() const override { return _bones.data(); }

    int findBone(const char* name) const override;
    int getParentBoneIndex(size_t boneIndex) const override;

    const ayt::math::Float4x4* getInverseBindMatrices() const override {
        return _inverseBindMatrices.data();
    }

    // ===== Local transforms =====
    const ayt::math::FVector3* getLocalPositions() const override { return _localPositions.data(); }
    const ayt::math::FQuaternion* getLocalRotations() const override { return _localRotations.data(); }
    const ayt::math::FVector3* getLocalScales() const override { return _localScales.data(); }

    size_t getRootBoneCount() const override { return _rootIndices.size(); }
    const int* getRootBoneIndices() const override { return _rootIndices.data(); }

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== Setters (for building skeleton) =====
    void setBoneCount(size_t count);
    void setBone(size_t index, const Bone& bone);
    void addBone(const Bone& bone);

    // ===== Create test data =====
    void createTestSkeleton();

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::vector<Bone> _bones;
    std::vector<ayt::math::Float4x4> _inverseBindMatrices;
    std::vector<ayt::math::FVector3> _localPositions;
    std::vector<ayt::math::FQuaternion> _localRotations;
    std::vector<ayt::math::FVector3> _localScales;
    std::vector<int> _rootIndices;
    std::unordered_map<std::string, size_t> _boneNameMap;
    std::string _path;
};

} // namespace ayt::resource
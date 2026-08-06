// AYSkeletonMask.h — P3.x 刀 1 (2026-08-06) concrete SkeletonMask asset.
//
// Concrete ISkeletonMask implementation. Lives in `ayt::resource`
// namespace alongside the other asset classes (Skeleton / Animation /
// Mesh / etc.). Supports both loader-driven path (`.aymask` file → load
// → loadFromBinary → bone entries populated) AND in-memory authoring
// path (SkeletonMask::create() + addEntry() — preserved from the P2.2
// temporary in-package fixture). The dual-mode design matches how
// Skeleton exposes both `createTestSkeleton()` and `loadFromBinary()`.

#pragma once

#include "IAYSkeletonMask.h"

#include <aymath/MathTypes.h>     // FGuid

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

class SkeletonMask final : public ISkeletonMask {
public:
    SkeletonMask();
    virtual ~SkeletonMask() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    std::size_t sizeInBytes() const override;
    // getPath / getType / isLoaded / setLoaded / reload / addTag / removeTag
    // / hasTag / getTags inherit from IResource.

    // ===== ISkeletonMask =====
    std::size_t getEntryCount() const override;
    const SkeletonMaskBone* getEntries() const override;
    std::size_t getAuthoredBoneCount() const override;
    bool  hasWildcard()    const override;
    float wildcardWeight() const override;
    const char* getDebugName() const override;
    const ayt::math::FGuid& getGuid() const override { return _guid; }

    // ===== Authoring API (preserved from P2.2 in-memory fixture) =====

    // Add (boneName, weight). Weights clamped to [0, 1]. Empty boneName
    // marks a WILDCARD entry whose weight applies to every bone not
    // named by another entry (named wins over wildcard). Duplicate
    // names overwrite the prior entry (last wins).
    void addEntry(const char* boneName, float weight);

    void setDebugName(const char* n);

    // ===== In-memory factory (preserved from P2.2 in-memory fixture) =====

    // Use this path for unit tests + procedural authoring. The loader
    // path (load / loadFromBinary) does NOT call this; it constructs
    // SkeletonMask directly via `std::make_shared<SkeletonMask>()`.
    static std::shared_ptr<SkeletonMask> create();

    // ===== GUID setter (used by loader after loadFromBinary) =====
    void setGuid(const ayt::math::FGuid& guid) { _guid = guid; }

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, std::size_t size);
    bool saveToBinary(std::vector<ayt::math::UInt8>& outData) const;

    // ===== Test data =====
    void createTestMask();

private:
    void clear();

    ayt::math::FGuid              _guid;
    std::vector<SkeletonMaskBone> _entries;
    bool                          _hasWildcard    = false;
    float                         _wildcardWeight = 0.0f;
    std::string                   _debugName      = "SkeletonMask";
};

} // namespace ayt::resource
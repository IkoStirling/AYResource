#pragma once

#include "AYResource/IntermediateAsset.h"
#include <cstddef>

namespace ayt::resource {

// Alpha evidence sampled only from texels covered by a material's actual
// submesh UV triangles.  Whole-image histograms are incorrect for atlases.
struct MaterialAlphaCoverage {
    std::size_t sampleCount = 0;
    std::size_t transparentCount = 0;
    std::size_t partialCount = 0;
    std::size_t opaqueCount = 0;
};

enum class MaterialAlphaEvidence {
    // An RGBA base-color alpha channel without an explicit source Blend
    // declaration is treated as coverage.  This preserves depth writes for
    // atlas-backed cloth/hair cards and only discards transparent texels.
    BaseColorAlpha,
    // A distinct opacity texture is authored opacity evidence.  Continuous
    // values may therefore select Blend when the coverage warrants it.
    DedicatedOpacity,
};

// Pure decision rule shared by FBX import and unit tests.  Empty or
// effectively opaque evidence stays Opaque; binary coverage is Mask.  A
// meaningful continuous-alpha population becomes Blend only for a dedicated
// opacity input.  BaseColor alpha stays depth-writing Mask unless the source
// material already supplied an explicit Blend declaration.
MaterialAlphaMode classifyMaterialAlphaCoverage(
    const MaterialAlphaCoverage& coverage,
    MaterialAlphaEvidence evidence = MaterialAlphaEvidence::BaseColorAlpha);

// Select the discard threshold for a depth-writing Mask. A texture whose
// actually referenced UV region never reaches transparent alpha is not a
// binary cutout: FBX/MMD exporters often preserve continuous alpha on an
// otherwise solid garment. A low threshold keeps that surface present while
// still rejecting numerical zero. Real cutouts keep the conventional 0.5.
float inferMaterialAlphaCutoff(
    const MaterialAlphaCoverage& coverage,
    MaterialAlphaMode mode) noexcept;

} // namespace ayt::resource

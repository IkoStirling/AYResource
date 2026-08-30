#include "AYResource.h"
#include "AYResource/Converter/FBXConverter.h"
#include "AYResource/Converter/FBXParser.h"
#include "AYResource/MaterialTextureContract.h"
#include "AYResource/MaterialSurfaceClassifier.h"
#include "AYResource/Converter/MeshConverter.h"
#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/Loader/MaterialLoader.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYResource/Loader/SkeletonLoader.h"
#include "AYTest.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/assetsImpl/Material.h"
#include "AYResource/assetsImpl/Texture.h"
#include "AYResource/assetsImpl/Skeleton.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <filesystem>

using namespace ayt::resource;
using namespace ayt::math;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

static std::string fbxTestOutputDir() {
    return ayt::test::testTmpPath("test_output");
}

static std::string fbxTestOutputStaticDir() {
    return ayt::test::testTmpPath("test_output_static");
}

TEST_SUITE(FBXConverterTests)

    TEST_CASE(MeshConverterReplacesSameSizeStaleMaterialIndices) {
        namespace fs = std::filesystem;
        const fs::path root = ayt::test::testTmpPath("mesh_same_size_replace");
        fs::create_directories(root);

        MeshData data;
        data.name = "semantic";
        data.attributeMask =
            1u << static_cast<UInt8>(MeshAttribute::Position);
        data.positions = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f};
        data.indices = {0, 1, 2, 1, 3, 2};
        data.submeshes = {{0, 3, 0, 0}, {3, 3, 0, 0}};
        data.materialSlots = {"materials/a.aymat", "materials/b.aymat"};

        MeshConverter converter;
        converter.setOutputDir(root.string());
        auto first = converter.convertAll({data}, "semantic");
        CHECK(first.size() == 1u);

        data.submeshes[1].materialIndex = 1;
        auto second = converter.convertAll({data}, "semantic");
        CHECK(second.size() == 1u);
        CHECK(first[0].size == second[0].size);

        Mesh loaded;
        CHECK(loaded.load((root / second[0].path).string()));
        CHECK(loaded.getSubmeshCount() == 2u);
        CHECK(loaded.getSubmeshes()[0].materialIndex == 0u);
        CHECK(loaded.getSubmeshes()[1].materialIndex == 1u);
    }

    TEST_CASE(ImportedMeshPreservesExplicitSubmeshMaterialSlots) {
        namespace fs = std::filesystem;
        const fs::path root = ayt::test::testTmpPath("fbx_submesh_slots");
        fs::create_directories(root);
        const fs::path objPath = root / "two_materials.obj";
        const fs::path mtlPath = root / "two_materials.mtl";
        {
            std::ofstream mtl(mtlPath);
            mtl << "newmtl Left\nKd 1 0 0\n"
                   "newmtl Right\nKd 0 1 0\n";
        }
        {
            std::ofstream obj(objPath);
            obj << "mtllib two_materials.mtl\n"
                   "o TwoMaterials\n"
                   "v -1 0 0\nv 0 0 0\nv -1 1 0\n"
                   "v 1 0 0\nv 1 1 0\n"
                   "vn 0 0 1\n"
                   "usemtl Left\nf 1//1 2//1 3//1\n"
                   "usemtl Right\nf 2//1 4//1 5//1\n";
        }

        FBXParser parser(objPath.string());
        parser.setLoadOption(IConverter::LoadOption::Full);
        parser.setAssetBaseName("two_materials");
        CHECK(parser.parse(objPath.string()));
        auto intermediate = parser.getResult();
        CHECK(intermediate != nullptr);
        CHECK(intermediate->meshes.size() == 1u);
        const MeshWindingAudit winding = auditCanonicalMeshWinding(
            intermediate->meshes[0].positions,
            intermediate->meshes[0].normals,
            intermediate->meshes[0].indices);
        CHECK(winding.comparedTriangleCount == 2u);
        CHECK(winding.mismatchedTriangleCount == 0u);
        CHECK(winding.invalidIndexTriangleCount == 0u);
        CHECK(intermediate->meshes[0].submeshes.size() == 2u);
        CHECK(intermediate->meshes[0].submeshes[0].sourceMaterialIndex
              < intermediate->materials.size());
        CHECK(intermediate->meshes[0].submeshes[1].sourceMaterialIndex
              < intermediate->materials.size());
        CHECK(intermediate->meshes[0].submeshes[0].sourceMaterialIndex
              != intermediate->meshes[0].submeshes[1].sourceMaterialIndex);

        FBXConverter converter(objPath.string());
        converter.setOutputDir(root.string());
        const ConversionResult result = converter.convert();

        bool checkedMesh = false;
        for (const auto& resource : result.resources) {
            if (resource.type != "Mesh") continue;
            Mesh mesh;
            CHECK(mesh.load((root / resource.path).string()));
            CHECK(mesh.getSubmeshCount() == 2u);
            CHECK(mesh.getMaterialSlotCount() == 2u);
            const IMesh::Submesh* submeshes = mesh.getSubmeshes();
            CHECK(submeshes != nullptr);
            CHECK(submeshes[0].materialIndex == 0u);
            CHECK(submeshes[1].materialIndex == 1u);
            CHECK(std::string(mesh.getMaterialSlot(0))
                  != std::string(mesh.getMaterialSlot(1)));
            checkedMesh = true;
        }
        CHECK(checkedMesh);
    }

    TEST_CASE(MaterialAlphaCoverageClassificationIsModelIndependent) {
        CHECK(classifyMaterialAlphaCoverage({}) == MaterialAlphaMode::Opaque);

        MaterialAlphaCoverage opaque;
        opaque.sampleCount = 1000;
        opaque.partialCount = 1;
        opaque.opaqueCount = 999;
        CHECK(classifyMaterialAlphaCoverage(opaque)
              == MaterialAlphaMode::Opaque);

        MaterialAlphaCoverage cutout;
        cutout.sampleCount = 1000;
        cutout.transparentCount = 400;
        cutout.partialCount = 20;
        cutout.opaqueCount = 580;
        CHECK(classifyMaterialAlphaCoverage(cutout)
              == MaterialAlphaMode::Mask);

        MaterialAlphaCoverage translucent;
        translucent.sampleCount = 1000;
        translucent.transparentCount = 100;
        translucent.partialCount = 350;
        translucent.opaqueCount = 550;
        CHECK(classifyMaterialAlphaCoverage(translucent)
              == MaterialAlphaMode::Mask);
        CHECK(classifyMaterialAlphaCoverage(
                  translucent, MaterialAlphaEvidence::DedicatedOpacity)
              == MaterialAlphaMode::Blend);

        MaterialAlphaCoverage continuousSolid;
        continuousSolid.sampleCount = 1000;
        continuousSolid.partialCount = 980;
        continuousSolid.opaqueCount = 20;
        CHECK(classifyMaterialAlphaCoverage(continuousSolid)
              == MaterialAlphaMode::Mask);
        CHECK(inferMaterialAlphaCutoff(
                  continuousSolid, MaterialAlphaMode::Mask) < 0.04f);
        CHECK(inferMaterialAlphaCutoff(cutout, MaterialAlphaMode::Mask)
              == 0.5f);
        CHECK(inferMaterialAlphaCutoff(
                  continuousSolid, MaterialAlphaMode::Opaque) == 0.5f);
    }

    TEST_CASE(ManualSourceCoordinatesBakeZUpRightHandedMeshIntoEngineSpace) {
        namespace fs = std::filesystem;
        const fs::path root = ayt::test::testTmpPath("fbx_manual_coordinates");
        fs::create_directories(root);
        const fs::path objPath = root / "z_up.obj";
        {
            std::ofstream obj(objPath);
            obj << "o ZUp\n"
                   "v 0 0 0\n"
                   "v 0 0 2\n"
                   "v 1 0 0\n"
                   "f 1 2 3\n";
        }

        SourceCoordinatePolicy coordinates;
        coordinates.mode = SourceCoordinateMode::Manual;
        coordinates.up = ImportAxis::PositiveZ;
        coordinates.forward = ImportAxis::NegativeY;
        coordinates.handedness = ImportHandedness::Right;
        coordinates.tag = "test-zup-rh-v1";

        FBXParser parser(objPath.string());
        parser.setSourceCoordinatePolicy(coordinates);
        CHECK(parser.parse(objPath.string()));
        const std::unique_ptr<IntermediateAsset> parsed = parser.getResult();
        CHECK(parsed != nullptr);
        CHECK(parsed && parsed->meshes.size() == 1u);
        if (parsed && parsed->meshes.size() == 1u) {
            const MeshData& parsedMesh = parsed->meshes.front();
            const MeshWindingAudit winding = auditCanonicalMeshWinding(
                parsedMesh.positions, parsedMesh.normals, parsedMesh.indices);
            CHECK(winding.comparedTriangleCount == 1u);
            CHECK(winding.mismatchedTriangleCount == 0u);
            CHECK(winding.invalidIndexTriangleCount == 0u);
        }

        FBXConverter converter(objPath.string());
        converter.setOutputDir(root.string());
        converter.setSourceCoordinatePolicy(coordinates);
        const ConversionResult result = converter.convert();
        CHECK(result.sourceCoordinateTag
              == sourceCoordinatePolicyCacheTag(coordinates));

        bool checkedMesh = false;
        for (const auto& resource : result.resources) {
            if (resource.type != "Mesh") continue;
            Mesh mesh;
            CHECK(mesh.load((root / resource.path).string()));
            FVector3 minBounds;
            FVector3 maxBounds;
            mesh.getBounds(minBounds, maxBounds);
            CHECK(std::abs(minBounds.y - 0.0f) < 0.0001f);
            CHECK(std::abs(maxBounds.y - 2.0f) < 0.0001f);
            CHECK(std::abs(maxBounds.z - minBounds.z) < 0.0001f);
            checkedMesh = true;
        }
        CHECK(checkedMesh);
    }

    TEST_CASE(AutoFbxCoordinatesResolveAxisWrapperWithoutItsUnitScale) {
        // Blender commonly leaves mesh/bind data in Z-up space and emits a
        // 100x, +90-degree FBX object wrapper.  Auto import must use only the
        // signed-axis basis: Assimp has already converted the payload units.
        const Float4x4 blenderFbxWrapper(
            100.0f, 0.0f,   0.0f,   0.0f,
            0.0f,   0.0f, 100.0f,   0.0f,
            0.0f,-100.0f,   0.0f,   0.0f,
            0.0f,   0.0f,   0.0f,   1.0f);
        SourceCoordinatePolicy requested;
        requested.mode = SourceCoordinateMode::Auto;
        requested.metersPerUnit = 0.0f;

        bool inferred = false;
        const SourceCoordinatePolicy resolved =
            detail::resolveFbxAutoCoordinatePolicy(
                requested, blenderFbxWrapper, &inferred);

        CHECK(inferred);
        CHECK(resolved.mode == SourceCoordinateMode::Manual);
        CHECK(resolved.up == ImportAxis::PositiveZ);
        CHECK(resolved.forward == ImportAxis::NegativeY);
        CHECK(resolved.handedness == ImportHandedness::Right);
        CHECK(std::abs(resolved.metersPerUnit - 1.0f) < 0.0001f);
    }

    TEST_CASE(AssimpMaterialSurfaceStateDoesNotTreatDefaultBlendAsTransparency) {
        namespace fs = std::filesystem;
        const fs::path root =
            ayt::test::testTmpPath("assimp_material_surface_contract");
        fs::create_directories(root);
        const fs::path objPath = root / "surface.obj";
        const fs::path mtlPath = root / "surface.mtl";
        {
            std::ofstream mtl(mtlPath);
            mtl << "newmtl Opaque\nKd 0.8 0.7 0.6\n"
                   "newmtl Faded\nKd 0.2 0.4 0.8\nd 0.35\n";
        }
        {
            std::ofstream obj(objPath);
            obj << "mtllib surface.mtl\n"
                   "o Surface\n"
                   "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                   "v 2 0 0\nv 3 0 0\nv 2 1 0\n"
                   "usemtl Opaque\nf 1 2 3\n"
                   "usemtl Faded\nf 4 5 6\n";
        }

        FBXParser parser(objPath.string());
        CHECK(parser.parse(objPath.string()));
        const auto asset = parser.getResult();
        CHECK(asset != nullptr);
        if (!asset) return;

        const auto findMaterial = [&](const char* name) -> const MaterialData* {
            for (const auto& material : asset->materials) {
                if (material.name == name) return &material;
            }
            return nullptr;
        };
        const MaterialData* opaque = findMaterial("Opaque");
        const MaterialData* faded = findMaterial("Faded");
        CHECK(opaque != nullptr);
        CHECK(faded != nullptr);
        if (!opaque || !faded) return;
        CHECK(opaque->alphaMode == MaterialAlphaMode::Opaque);
        CHECK(faded->alphaMode == MaterialAlphaMode::Blend);
        CHECK(faded->blendFunction == MaterialBlendFunction::StandardAlpha);
        CHECK_FALSE(faded->sourceProperties.empty());
        const auto opacity = std::find_if(
            faded->parameters.begin(), faded->parameters.end(),
            [](const Param& parameter) { return parameter.name == "opacity"; });
        CHECK(opacity != faded->parameters.end());
        if (opacity != faded->parameters.end()) {
            CHECK(std::abs(opacity->floatValue - 0.35f) < 0.001f);
        }
    }

    TEST_CASE(MaterialConverterPersistsImporterNeutralSourceMetadata) {
        namespace fs = std::filesystem;
        const fs::path root =
            ayt::test::testTmpPath("material_source_metadata");
        fs::create_directories(root);

        MaterialData data;
        data.name = "AdditiveMaterial";
        data.shader = "pbr.phoskia";
        data.alphaMode = MaterialAlphaMode::Blend;
        data.blendFunction = MaterialBlendFunction::Additive;
        data.shadingModel = MaterialShadingModel::Pbr;
        data.sourceAdapter = "fbxsdk-test-double";
        MaterialData::SourceProperty property;
        property.key = "Maya|vendor property";
        property.type = MaterialSourcePropertyType::Float;
        property.value = "0.25,0.5";
        data.sourceProperties.push_back(property);

        MaterialData::TextureSource texture;
        texture.parameterName = "baseColorTexture";
        texture.sourcePath = "source.png";
        texture.virtualPath = "textures/source_d.aytex";
        texture.usageSuffix = "_d";
        texture.sourceSemantic = 12;
        texture.sourceLayer = 2;
        texture.uvChannel = 1;
        texture.mapping = MaterialTextureMapping::Uv;
        texture.operation = MaterialTextureOperation::Add;
        texture.wrapU = MaterialTextureWrap::Clamp;
        texture.wrapV = MaterialTextureWrap::Mirror;
        texture.blendFactor = 0.75f;
        texture.hasUvTransform = true;
        texture.uvTranslation[0] = 0.1f;
        texture.uvTranslation[1] = 0.2f;
        texture.uvScale[0] = 2.0f;
        texture.uvScale[1] = 3.0f;
        texture.uvRotation = 0.4f;
        data.textureSources.push_back(texture);

        MaterialConverter converter;
        converter.setOutputDir(root.string());
        const auto converted = converter.convertAll({data}, "metadata");
        CHECK(converted.size() == 1u);
        if (converted.empty()) return;

        Material loaded;
        CHECK(loaded.load((root / converted.front().path).string()));
        CHECK(loaded.getInt("__ayBlendFunction") == 1);
        CHECK(loaded.getInt("__aySourceShadingModel")
              == static_cast<int>(MaterialShadingModel::Pbr));
        CHECK(std::string(loaded.getString("__aySourceProperties"))
              .find("Maya|vendor property") != std::string::npos);
        CHECK(std::string(loaded.getString("__aySourceProperties"))
              .find("fbxsdk-test-double") != std::string::npos);
        const FVector4 binding =
            loaded.getVector4("__ayTexture.baseColorTexture.binding");
        CHECK(binding.x == 12.0f);
        CHECK(binding.y == 2.0f);
        CHECK(binding.z == 1.0f);
        CHECK(std::abs(binding.w - 0.75f) < 0.001f);
        const FVector4 transform =
            loaded.getVector4("__ayTexture.baseColorTexture.uvTransform");
        CHECK(std::abs(transform.x - 0.1f) < 0.001f);
        CHECK(std::abs(transform.w - 3.0f) < 0.001f);
    }

TEST_SUITE_END

TEST_SUITE(UvContractTests)

    TEST_CASE(SourceUvOriginNormalizesVAndTangentHandedness) {
        namespace fs = std::filesystem;
        const fs::path root = ayt::test::testTmpPath("fbx_uv_origin_contract");
        fs::create_directories(root);
        const fs::path objPath = root / "triangle.obj";
        {
            std::ofstream obj(objPath);
            obj << "o UvOrigin\n"
                   "v 0 0 0\n"
                   "v 1 0 0\n"
                   "v 0 1 0\n"
                   "vt 0.10 0.20\n"
                   "vt 0.80 0.25\n"
                   "vt 0.20 0.90\n"
                   "vn 0 0 1\n"
                   "f 1/1/1 2/2/1 3/3/1\n";
        }

        SourceCoordinatePolicy topPolicy;
        topPolicy.uvOrigin = ImportUvOrigin::TopLeft;
        topPolicy.tag = "uv-origin-test";
        SourceCoordinatePolicy bottomPolicy = topPolicy;
        bottomPolicy.uvOrigin = ImportUvOrigin::BottomLeft;

        CHECK(sourceCoordinatePolicyCacheTag(topPolicy)
              != sourceCoordinatePolicyCacheTag(bottomPolicy));

        FBXParser topParser(objPath.string());
        topParser.setSourceCoordinatePolicy(topPolicy);
        const bool topParsed = topParser.parse(objPath.string());
        std::unique_ptr<IntermediateAsset> topAsset = topParser.getResult();

        FBXParser bottomParser(objPath.string());
        bottomParser.setSourceCoordinatePolicy(bottomPolicy);
        const bool bottomParsed = bottomParser.parse(objPath.string());
        std::unique_ptr<IntermediateAsset> bottomAsset = bottomParser.getResult();

        CHECK(topParsed);
        CHECK(bottomParsed);
        CHECK(topAsset != nullptr);
        CHECK(bottomAsset != nullptr);
        if (!topParsed || !bottomParsed || !topAsset || !bottomAsset) return;
        CHECK(topAsset->meshes.size() == 1u);
        CHECK(bottomAsset->meshes.size() == 1u);
        if (topAsset->meshes.size() != 1u || bottomAsset->meshes.size() != 1u) {
            return;
        }

        const MeshData& topMesh = topAsset->meshes.front();
        const MeshData& bottomMesh = bottomAsset->meshes.front();
        CHECK(topMesh.uvs.size() == bottomMesh.uvs.size());
        CHECK(topMesh.tangents.size() == bottomMesh.tangents.size());
        if (topMesh.uvs.size() != bottomMesh.uvs.size()
            || topMesh.tangents.size() != bottomMesh.tangents.size()) {
            return;
        }
        for (size_t vertex = 0; vertex < topMesh.uvs.size() / 2u; ++vertex) {
            CHECK(std::abs(topMesh.uvs[vertex * 2]
                           - bottomMesh.uvs[vertex * 2]) < 0.0001f);
            CHECK(std::abs((topMesh.uvs[vertex * 2 + 1]
                            + bottomMesh.uvs[vertex * 2 + 1]) - 1.0f)
                  < 0.0001f);
            CHECK(std::abs(topMesh.tangents[vertex * 4 + 3]
                           + bottomMesh.tangents[vertex * 4 + 3]) < 0.0001f);
        }

        // The merged import path owns a separate copy loop; keep it on the
        // same normalized contract instead of only testing node separation.
        FBXParser mergedParser(objPath.string());
        mergedParser.setSeparateModels(false);
        mergedParser.setSourceCoordinatePolicy(bottomPolicy);
        const bool mergedParsed = mergedParser.parse(objPath.string());
        std::unique_ptr<IntermediateAsset> mergedAsset = mergedParser.getResult();
        CHECK(mergedParsed);
        CHECK(mergedAsset != nullptr);
        if (!mergedParsed || !mergedAsset || mergedAsset->meshes.empty()) return;
        CHECK(mergedAsset->meshes.front().uvs == bottomMesh.uvs);
        CHECK(mergedAsset->meshes.front().tangents == bottomMesh.tangents);
    }

TEST_SUITE_END

TEST_SUITE(FBXMaterialConverterTests)

    TEST_CASE(ImportedMaterialUsesRuntimePbrShader) {
        namespace fs = std::filesystem;
        const fs::path root = ayt::test::testTmpPath("fbx_pbr_contract");
        fs::create_directories(root);
        const fs::path objPath = root / "triangle.obj";
        const fs::path mtlPath = root / "triangle.mtl";
        const fs::path texturePath = root / "shared.ppm";
        {
            std::ofstream texture(texturePath, std::ios::binary);
            texture << "P6\n1 1\n255\n";
            const char pixel[3] = {static_cast<char>(64),
                                   static_cast<char>(128),
                                   static_cast<char>(255)};
            texture.write(pixel, sizeof(pixel));
        }
        {
            std::ofstream mtl(mtlPath);
            mtl << "newmtl ImportedMaterial\n"
                   "Kd 0.8 0.4 0.2\n"
                   "Pm 0.7\n"
                   "Pr 0.3\n"
                   "map_Kd shared.ppm\n"
                   "map_d shared.ppm\n";
        }
        {
            std::ofstream obj(objPath);
            obj << "mtllib triangle.mtl\n"
                   "o Triangle\n"
                   "v 0 0 0\n"
                   "v 1 0 0\n"
                   "v 0 1 0\n"
                   "vt 0 0\n"
                   "vt 1 0\n"
                   "vt 0 1\n"
                   "vn 0 0 1\n"
                   "usemtl ImportedMaterial\n"
                   "f 1/1/1 2/2/1 3/3/1\n";
        }

        FBXConverter converter(objPath.string());
        converter.setOutputDir(root.string());
        converter.setCookTextures(false);
        MaterialImportPolicy policy;
        policy.tag = "unit-material-policy-v1";
        // Name-based rules survive source material reordering.
        policy.maskNames = "ImportedMaterial";
        policy.doubleSidedNames = "ImportedMaterial";
        converter.setMaterialImportPolicy(policy);
        const ConversionResult result = converter.convert();
        CHECK(result.materialPolicyTag == policy.tag);
        // The exact current tag is compiled into both conversion and cache
        // tests so every contract bump exercises automatic invalidation.
        CHECK(result.importerContractTag == kFbxImporterContractTag);

        const ConversionResult cached =
            ConversionResult::fromJson(result.toJson());
        CHECK(cached.importerContractTag == kFbxImporterContractTag);
        CHECK(cached.sourceCoordinateTag
              == sourceCoordinatePolicyCacheTag(SourceCoordinatePolicy{}));

        bool checkedMaterial = false;
        for (const auto& resource : result.resources) {
            if (resource.type != "Material") {
                continue;
            }
            Material material;
            CHECK(material.load((root / resource.path).string()));
            CHECK(std::string(material.getShader()) == "pbr.phoskia");
            if (std::string(material.getName()) != "ImportedMaterial") {
                continue;
            }
            CHECK(material.getAlphaMode() == MaterialAlphaMode::Mask);
            CHECK(material.getAlphaCutoff() == 0.5f);
            CHECK(material.isDoubleSided());
            CHECK_FALSE(material.hasParameter("__ayAlphaMode"));
            CHECK(material.hasParameter("baseColorTexture"));
            // Assimp reports map_Kd again as the opacity slot for this
            // source.  The runtime must consume the image alpha once, not
            // multiply its red channel into alpha a second time.
            CHECK_FALSE(material.hasParameter("opacityTexture"));
            CHECK(material.hasParameter("opacitySource"));
            CHECK(material.getFloat("opacitySource")
                  == materialOpacitySourceValue(
                      MaterialOpacitySource::BaseColorAlpha));
            bool emittedDeadOpacityTexture = false;
            for (const auto& converted : result.resources) {
                emittedDeadOpacityTexture = emittedDeadOpacityTexture
                    || converted.path.find("shared_o") != std::string::npos;
            }
            CHECK_FALSE(emittedDeadOpacityTexture);
            checkedMaterial = true;
        }
        CHECK(checkedMaterial);
    }

    TEST_CASE(DistinctOpacityTextureKeepsExplicitRedChannelContract) {
        namespace fs = std::filesystem;
        const fs::path root =
            ayt::test::testTmpPath("fbx_distinct_opacity_source_v10");
        fs::create_directories(root);
        const fs::path objPath = root / "triangle.obj";
        const fs::path mtlPath = root / "triangle.mtl";
        const fs::path basePath = root / "base.ppm";
        const fs::path opacityPath = root / "opacity.ppm";
        for (const fs::path& path : {basePath, opacityPath}) {
            std::ofstream texture(path, std::ios::binary);
            texture << "P6\n1 1\n255\n";
            const char pixel[3] = {static_cast<char>(255),
                                   static_cast<char>(255),
                                   static_cast<char>(255)};
            texture.write(pixel, sizeof(pixel));
        }
        {
            std::ofstream mtl(mtlPath);
            mtl << "newmtl Layered\n"
                   "Kd 1 1 1\n"
                   "map_Kd base.ppm\n"
                   "map_d opacity.ppm\n";
        }
        {
            std::ofstream obj(objPath);
            obj << "mtllib triangle.mtl\n"
                   "o Triangle\n"
                   "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                   "vt 0 0\nvt 1 0\nvt 0 1\n"
                   "vn 0 0 1\n"
                   "usemtl Layered\n"
                   "f 1/1/1 2/2/1 3/3/1\n";
        }

        FBXConverter converter(objPath.string());
        converter.setOutputDir(root.string());
        converter.setCookTextures(false);
        MaterialImportPolicy policy;
        policy.maskNames = "Layered";
        converter.setMaterialImportPolicy(policy);
        const ConversionResult result = converter.convert();

        bool checkedMaterial = false;
        bool emittedOpacityTexture = false;
        for (const auto& resource : result.resources) {
            emittedOpacityTexture = emittedOpacityTexture
                || resource.path.find("opacity_o") != std::string::npos;
            if (resource.type != "Material") continue;
            Material material;
            CHECK(material.load((root / resource.path).string()));
            if (std::string(material.getName()) != "Layered") continue;
            CHECK(material.hasParameter("baseColorTexture"));
            CHECK(material.hasParameter("opacityTexture"));
            CHECK(material.getFloat("opacitySource")
                  == materialOpacitySourceValue(
                      MaterialOpacitySource::TextureRed));
            checkedMaterial = true;
        }
        CHECK(checkedMaterial);
        CHECK(emittedOpacityTexture);
    }

// 注释掉 — 该 case 依赖磁盘上旧版 (Submesh 12 字节) .aymesh 文件
// 旧文件 SUBM chunk size = N × 12,新 loader 按 N × 16 读会越界。
// 应当先跑 FBXConverter 生成新版 .aymesh 再读,不在 unit test 里做。
/*
TEST_CASE(FileValide) {
    std::string fullPath = "D:/Projects/AYRuntime/AYResource/test_output/meshes/Sour_RootNode_Sour_mesh.aymesh";
    CHECK(fileExists(fullPath) == true);
    ...
}
*/

    // Suzanne 路径不在主测试机器上,默认 skip
    TEST_CASE(ConvertSuzanne) {
        std::string fbxPath = "D:/Projects/suzanne.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }

        FBXConverter converter(fbxPath);
        CHECK(converter.isValid() == true);

        // 设置输出目录
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 验证输出文件存在
        std::string outPath = fbxTestOutputDir() + "/" + result.resources[0].path;
        CHECK(fileExists(outPath) == true);

        // 验证资源信息
        CHECK(result.resources[0].type == "Mesh");
        CHECK(result.resources[0].size > 0);
    }

    TEST_CASE(ConvertCube) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/cube.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }

        FBXConverter converter(fbxPath);
        CHECK(converter.isValid() == true);

        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 验证输出文件
        std::string outPath = fbxTestOutputDir() + "/" + result.resources[0].path;
        CHECK(fileExists(outPath) == true);
    }

    // Sour Miku FBX 解析很慢,在 CI / 频繁跑测试时建议注释
    /*
    TEST_CASE(ConvertSourMikuFull) {

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 统计各类型资源数量
        size_t meshCount = 0;
        size_t materialCount = 0;
        size_t textureCount = 0;
        size_t skeletonCount = 0;
        for (const auto& res : result.resources) {
            if (res.type == "Mesh") meshCount++;
            else if (res.type == "Material") materialCount++;
            else if (res.type == "Texture") textureCount++;
            else if (res.type == "Skeleton") skeletonCount++;
        }

        printf("=== Conversion Result ===\n");
        printf("  total resources: %zu\n", result.resources.size());
        printf("  Mesh: %zu, Material: %zu, Texture: %zu, Skeleton: %zu\n", meshCount, materialCount, textureCount, skeletonCount);
        printf("  dependencies: %zu\n", result.dependencies.size());

        // 验证有 Mesh 资源
        CHECK(meshCount > 0);
        printf("  [OK] Mesh count: %zu\n", meshCount);

        // ===== 验证 Mesh =====
        for (size_t i = 0; i < result.resources.size(); i++) {
            const auto& res = result.resources[i];
            if (res.type != "Mesh") continue;

            printf("  Testing Mesh[%zu]: path=%s size=%lld\n", i, res.path.c_str(), (long long)res.size);

            // 验证文件存在
            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);
            printf("    [OK] File exists\n");

            // 读取二进制
            std::ifstream rawFile(fullPath, std::ios::binary);
            CHECK(rawFile.is_open() == true);
            rawFile.seekg(0, std::ios::end);
            size_t rawSize = rawFile.tellg();
            rawFile.seekg(0, std::ios::beg);

            std::vector<UInt8> fileData(rawSize);
            rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);

            // 验证 header
            UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
            UInt16 version = *reinterpret_cast<UInt16*>(fileData.data() + 4);
            CHECK(magic == 0x484D5941);  // 'AYMH'
            CHECK(version == 1);
            printf("    [OK] Magic=0x%08X, Version=%d\n", magic, version);

            // 加载并验证
            MeshLoader loader;
            auto mesh = std::dynamic_pointer_cast<Mesh>(
                loader.loadFromBinary(fileData.data(), fileData.size()));
            CHECK(mesh != nullptr);
            CHECK(mesh->getVertexCount() > 0);
            CHECK(mesh->getIndexCount() > 0);
            printf("    [OK] vertexCount=%u, indexCount=%u\n",
                   mesh->getVertexCount(), mesh->getIndexCount());

            // 验证 bounds
            CHECK(mesh->hasBounds() == true);
            FVector3 min, max;
            mesh->getBounds(min, max);
            CHECK(min.x <= max.x && min.y <= max.y && min.z <= max.z);
            printf("    [OK] Bounds valid\n");

            // 验证顶点数据
            CHECK(mesh->getVertexData() != nullptr);
            CHECK(mesh->getIndexData() != nullptr);

            // 验证 submesh 和 material slot
            printf("    [OK] submeshCount=%u, materialSlotCount=%u\n",
                   mesh->getSubmeshCount(), mesh->getMaterialSlotCount());
        }

        // ===== 验证 Material =====
        printf("  Testing Materials...\n");
        for (size_t i = 0; i < result.resources.size(); i++) {
            const auto& res = result.resources[i];
            if (res.type != "Material") continue;

            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);
            printf("    Material: %s (size=%lld) [OK]\n", res.path.c_str(), (long long)res.size);
        }
        CHECK(materialCount > 0);
        printf("  [OK] Material count: %zu\n", materialCount);

        // ===== 验证 Texture（如果有） =====
        if (textureCount > 0) {
            printf("  Testing Textures...\n");
            for (size_t i = 0; i < result.resources.size(); i++) {
                const auto& res = result.resources[i];
                if (res.type != "Texture") continue;

                std::string fullPath = fbxTestOutputDir() + "/" + res.path;
                CHECK(fileExists(fullPath) == true);
                printf("    Texture: %s (size=%lld) [OK]\n", res.path.c_str(), (long long)res.size);
            }
            printf("  [OK] Texture count: %zu\n", textureCount);
        } else {
            printf("  [INFO] No embedded textures in this FBX\n");
        }

        // ===== 验证 Skeleton（如果有） =====
        if (skeletonCount > 0) {
            printf("  Testing Skeletons...\n");
            for (size_t i = 0; i < result.resources.size(); i++) {
                const auto& res = result.resources[i];
                if (res.type != "Skeleton") continue;

                printf("  Testing Skeleton[%zu]: path=%s size=%lld\n", i, res.path.c_str(), (long long)res.size);

                std::string fullPath = fbxTestOutputDir() + "/" + res.path;
                CHECK(fileExists(fullPath) == true);
                printf("    [OK] File exists\n");

                // 读取二进制
                std::ifstream rawFile(fullPath, std::ios::binary);
                CHECK(rawFile.is_open() == true);
                rawFile.seekg(0, std::ios::end);
                size_t rawSize = rawFile.tellg();
                rawFile.seekg(0, std::ios::beg);

                std::vector<UInt8> fileData(rawSize);
                rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);

                // 验证 header
                UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
                CHECK(magic == 0x534B4C4E);  // 'SKLN'
                printf("    [OK] Magic=0x%08X\n", magic);

                // 加载并验证
                SkeletonLoader loader;
                auto skeleton = std::dynamic_pointer_cast<Skeleton>(
                    loader.loadFromBinary(fileData.data(), fileData.size()));
                CHECK(skeleton != nullptr);
                CHECK(skeleton->getBoneCount() > 0);
                printf("    [OK] boneCount=%zu\n", skeleton->getBoneCount());
            }
            printf("  [OK] Skeleton count: %zu\n", skeletonCount);
        } else {
            printf("  [INFO] No skeletons in this FBX\n");
        }

        // ===== 验证依赖关系 =====
        printf("  Testing Dependencies...\n");
        CHECK(result.dependencies.size() > 0);
        int dep_count = 0;
        for (const auto& dep : result.dependencies) {
			if (dep.from.empty() || dep.to.empty() ){
				dep_count++;
			}

        }
        CHECK(dep_count == 0);
    }
    */

    //TEST_CASE(ConvertSourMikuMergedVsSeparate) {
    //    // 注意：SourMiku 只有 1 个顶级节点，所以无论哪种模式都只输出 1 个 .aymesh
    //    // 分离模式只在多顶级节点的 FBX 下才有意义
    //    std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
    //    // 验证两种模式都能正常工作
    //    for (bool separate : {false, true}) {
    //        FBXConverter converter(fbxPath);
    //        converter.setLoadOption(IConverter::LoadOption::Full);
    //        converter.setSeparateModels(separate);
    //        converter.setOutputDir(fbxTestOutputDir() + "/" + std::string(separate ? "separate" : "merged"));
    //        ConversionResult result = converter.convert();
    //        size_t meshCount = 0;
    //        for (const auto& res : result.resources) {
    //            if (res.type == "Mesh") meshCount++;
    //        }
    //        printf("=== Mode: %s, Mesh count: %zu ===\n", separate ? "Separate" : "Merged", meshCount);
    //        CHECK(meshCount > 0);
    //    }
    //}

    TEST_CASE(FactoryCreate) {
        // 测试工厂方法
        auto fbxConv = IConverter::create("test.fbx");
        CHECK(fbxConv != nullptr);
        CHECK(strcmp(fbxConv->getSourceType(), "FBX") == 0);

        auto gltfConv = IConverter::create("test.gltf");
        CHECK(gltfConv != nullptr);
        CHECK(strcmp(gltfConv->getSourceType(), "glTF") == 0);

        auto glbConv = IConverter::create("test.glb");
        CHECK(glbConv != nullptr);
        CHECK(strcmp(glbConv->getSourceType(), "glTF") == 0);

        auto unknownConv = IConverter::create("test.obj");
        CHECK(unknownConv == nullptr);
    }

    // ===== R-02 新增测试 =====

    // Sour Miku FBX 解析很慢 — 注释掉,等性能优化后再启用
    // 验证 FBX 含动画时,resources 里出现 type == "Animation" 且 .ayanm 文件存在并以 'AYNM' 开头
    /*
    TEST_CASE(AnimationsAreEmitted) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        size_t animCount = 0;
        for (const auto& res : result.resources) {
            if (res.type != "Animation") continue;
            ++animCount;

            // 文件存在
            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);

            // magic == 'AYNM' (0x4E4D5941 little-endian)
            std::ifstream rawFile(fullPath, std::ios::binary);
            rawFile.seekg(0, std::ios::end);
            size_t rawSize = (size_t)rawFile.tellg();
            rawFile.seekg(0, std::ios::beg);
            std::vector<UInt8> fileData(rawSize);
            rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);
            CHECK(fileData.size() >= 8);
            UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
            CHECK(magic == 0x4E4D5941);
        }
        printf("    [INFO] Sour.fbx emitted %zu animations\n", animCount);
    }
    */

    // Sour Miku FBX 解析很慢 — 注释掉
    // 验证 .aydep.json 包含 mesh→skeleton 与 skeleton→animation 边
    /*
    TEST_CASE(DependenciesContainMeshToSkeleton) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();

        // 必须有 skeleton 才会写 mesh→skel 边
        bool hasSkeleton = false;
        bool hasAnim = false;
        for (const auto& res : result.resources) {
            if (res.type == "Skeleton") hasSkeleton = true;
            if (res.type == "Animation") hasAnim = true;
        }
        if (!hasSkeleton) {
            printf("    [SKIP] Sour.fbx has no skeleton\n");
            return;
        }

        bool sawMeshToSkel = false;
        bool sawSkelToAnim = false;
        for (const auto& dep : result.dependencies) {
            if (dep.from.find("meshes/") == 0 && dep.to.find("skeletons/") == 0) {
                sawMeshToSkel = true;
            }
            if (dep.from.find("skeletons/") == 0 && dep.to.find("animations/") == 0) {
                sawSkelToAnim = true;
            }
        }
        CHECK(sawMeshToSkel);
        if (hasAnim) {
            CHECK(sawSkelToAnim);
        }
    }
    */

    // 静态 FBX (cube.fbx 没有 anim/skel) 必须不产生 animation
    TEST_CASE(StaticFbxHasNoAnimations) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/cube.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputStaticDir());

        ConversionResult result = converter.convert();
        for (const auto& res : result.resources) {
            CHECK(res.type != "Animation");
        }
    }

TEST_SUITE_END

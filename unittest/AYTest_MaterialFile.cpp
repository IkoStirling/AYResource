#include "AYResource.h"
#include "AYResource/Loader/MaterialFile.h"
#include "AYTest.h"

#include <vector>
#include <cstring>
#include <string>

using namespace ayt::resource;

TEST_SUITE(MaterialFileTests)

TEST_CASE(texture_reference_rebase_preserves_existing_dimensional_type_and_refuses_invalid_targets)
{
    for(const auto type:{MaterialParamType::Texture2D,MaterialParamType::Texture3D,MaterialParamType::TextureCube}){
        Material original;original.initialise("Typed","pbr.phoskia");original.setTexture("customTexture","./before.txr");
        std::vector<UInt8> bytes;CHECK(original.saveToBinary(bytes));
        UInt32 nameLength=0,shaderLength=0;
        std::memcpy(&nameLength,bytes.data()+24,4);std::memcpy(&shaderLength,bytes.data()+28,4);
        const std::size_t tag=40+nameLength+shaderLength+4+std::string("customTexture").size();
        bytes[tag]=static_cast<UInt8>(type);
        Material material;CHECK(material.loadFromBinary(bytes.data(),bytes.size()));
        CHECK(material.setTextureReference("customTexture","textures/source/new.txr"));
        CHECK(material.getParameterType("customTexture")==type);
        CHECK(std::string(material.getTexture("customTexture"))=="textures/source/new.txr");
        CHECK_FALSE(material.setTextureReference("missing","no.txr"));
        CHECK_FALSE(material.setTextureReference("customTexture",std::string("bad\0suffix",10)));
        CHECK(std::string(material.getTexture("customTexture"))=="textures/source/new.txr");
        CHECK(material.getParameterCount()==1u);
        CHECK(material.saveToBinary(bytes));Material reopened;CHECK(reopened.loadFromBinary(bytes.data(),bytes.size()));
        CHECK(reopened.getParameterType("customTexture")==type);
        CHECK(std::string(reopened.getTexture("customTexture"))=="textures/source/new.txr");
    }
    Material scalar;scalar.setFloat("roughness",.25f);
    CHECK_FALSE(scalar.setTextureReference("roughness","texture.txr"));
    CHECK(scalar.getFloat("roughness")==.25f);
}


TEST_CASE(single_material_rejects_nul_c_string_identities_and_unbounded_lengths_without_mutation)
{
    Material original;original.initialise("Identity", "pbr.phoskia");original.setFloat("roughness",.5f);
    std::vector<UInt8> bytes;CHECK(original.saveToBinary(bytes));
    UInt32 nameLength=0,shaderLength=0;
    std::memcpy(&nameLength,bytes.data()+24,4);std::memcpy(&shaderLength,bytes.data()+28,4);
    const size_t payload=40+nameLength+shaderLength;
    Material retained;retained.initialise("Retain","retain.phoskia");retained.setFloat("sentinel",.75f);
    const auto refuses=[&](const std::vector<UInt8>& data){
        CHECK_FALSE(retained.loadFromBinary(data.data(),data.size()));
        CHECK(std::string(retained.getName())=="Retain");CHECK(retained.getFloat("sentinel")==.75f);
    };
    for(size_t offset:{size_t(40),size_t(40+nameLength),payload+4}){
        auto invalid=bytes;invalid[offset]=0;refuses(invalid);
    }
    const UInt32 excessive=UINT32_MAX;
    for(size_t offset:{size_t(24),size_t(28),size_t(32),payload}){
        auto invalid=bytes;std::memcpy(invalid.data()+offset,&excessive,4);refuses(invalid);
    }
    // A caller may hand us an unaligned file slice. Decoding uses bounded memcpy.
    std::vector<UInt8> unaligned{0};unaligned.insert(unaligned.end(),bytes.begin(),bytes.end());
    CHECK(retained.loadFromBinary(unaligned.data()+1,bytes.size()));
    CHECK(retained.getFloat("roughness")==.5f);
    auto invalid=original;invalid.setName(std::string("hidden\0tail",11));
    std::vector<UInt8> sentinel{7,8};CHECK_FALSE(invalid.saveToBinary(sentinel));CHECK(sentinel==std::vector<UInt8>({7,8}));
    invalid=original;invalid.setShader(std::string("shader\0tail",11));
    CHECK_FALSE(invalid.saveToBinary(sentinel));CHECK(sentinel==std::vector<UInt8>({7,8}));
}

TEST_CASE(single_material_matrix_and_unknown_authored_values_round_trip_losslessly)
{
    Material source;source.initialise("Matrix surface","pbr.phoskia");
    float matrix[16];for(int i=0;i<16;++i)matrix[i]=(i-7)*.125f;
    source.setMatrix("customTransform",matrix);source.setString("sourceLabel","retain this import metadata");
    source.setFloat("tailValue",.37f);source.setTexture("normalTexture","./textures/normal.png");
    std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));Material decoded;
    CHECK(decoded.loadFromBinary(bytes.data(),bytes.size()));CHECK(decoded.getParameterCount()==4);
    const auto* result=decoded.getMatrix("customTransform");CHECK(result!=nullptr);
    if(result)for(int i=0;i<16;++i)CHECK(result[i]==matrix[i]);
    CHECK(std::string(decoded.getString("sourceLabel"))=="retain this import metadata");
    CHECK(decoded.getFloat("tailValue")==.37f);CHECK(std::string(decoded.getTexture("normalTexture"))=="./textures/normal.png");
}

TEST_CASE(single_material_rejects_truncation_trailing_unknown_duplicate_and_count_errors_atomically)
{
    Material source;source.initialise("Strict","pbr.phoskia");source.setFloat("roughness",.6f);
    std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));
    Material decoded;decoded.initialise("Keep me","keep.phoskia");decoded.setFloat("original",.9f);
    for(size_t length=0;length<bytes.size();++length){
        CHECK_FALSE(decoded.loadFromBinary(bytes.data(),length));
        CHECK(std::string(decoded.getName())=="Keep me");CHECK(decoded.getFloat("original")==.9f);
    }
    auto broken=bytes;broken.push_back(0);CHECK_FALSE(decoded.loadFromBinary(broken.data(),broken.size()));
    // Packed single-material v2/v3 header layout is part of the file contract.
    UInt32 name=0,shader=0;std::memcpy(&name,bytes.data()+24,4);std::memcpy(&shader,bytes.data()+28,4);
    const size_t payload=40+name+shader;UInt32 parameterName=0;std::memcpy(&parameterName,bytes.data()+payload,4);
    broken=bytes;broken[payload+4+parameterName]=255;CHECK_FALSE(decoded.loadFromBinary(broken.data(),broken.size()));
    broken=bytes;broken[23]=2;CHECK_FALSE(decoded.loadFromBinary(broken.data(),broken.size()));
    broken=bytes;broken[23]=0;CHECK_FALSE(decoded.loadFromBinary(broken.data(),broken.size()));
    broken=bytes;broken.insert(broken.end(),bytes.begin()+payload,bytes.end());broken[23]=2;
    const UInt32 doubled=UInt32((bytes.size()-payload)*2);std::memcpy(broken.data()+32,&doubled,4);
    CHECK_FALSE(decoded.loadFromBinary(broken.data(),broken.size()));
    CHECK(std::string(decoded.getName())=="Keep me");CHECK(decoded.getParameterCount()==1);
    CHECK(decoded.loadFromBinary(bytes.data(),bytes.size()));CHECK(std::string(decoded.getName())=="Strict");
}

TEST_CASE(single_material_keeps_v1_v2_compatibility_and_refuses_unrepresentable_parameter_counts)
{
    Material source;source.initialise("Legacy","pbr.phoskia");source.setInt("__ayAlphaMode",1);
    source.setFloat("__ayAlphaCutoff",.34f);source.setBool("__ayDoubleSided",true);
    std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));Material decoded;
    auto v2=bytes;v2[4]=2;CHECK(decoded.loadFromBinary(v2.data(),v2.size()));
    auto v1=bytes;v1[4]=1;v1.erase(v1.begin()+36,v1.begin()+40);
    CHECK(decoded.loadFromBinary(v1.data(),v1.size()));CHECK(decoded.getAlphaMode()==MaterialAlphaMode::Mask);
    CHECK(decoded.getAlphaCutoff()==.34f);CHECK(decoded.isDoubleSided());
    Material large;for(int i=0;i<256;++i)large.setFloat(("value"+std::to_string(i)).c_str(),float(i));
    std::vector<UInt8> sentinel{1,2,3};CHECK_FALSE(large.saveToBinary(sentinel));CHECK(sentinel==std::vector<UInt8>({1,2,3}));
}

TEST_CASE(save_and_load_multi_material_file)
{
    auto matA = std::make_shared<Material>();
    matA->setName("MatA");
    matA->setShader("shaders/a.phoskia");
    matA->setSurfaceProperties(MaterialAlphaMode::Mask, 0.37f, true);
    matA->setFloat("metallic", 0.25f);

    auto matB = std::make_shared<Material>();
    matB->setName("MatB");
    matB->setShader("shaders/b.phoskia");
    matB->setSurfaceProperties(MaterialAlphaMode::Blend, 0.5f, false);
    const float colorB[] = {1.0f, 0.5f, 0.25f, 1.0f};
    matB->setFloat4("baseColor", colorB);

    MaterialFile file;
    file.addMaterial(matA);
    file.addMaterial(matB);

    std::vector<UInt8> bytes;
    CHECK(file.saveToBinary(bytes));
    CHECK(bytes.size() > 0);

    MaterialFile loaded;
    CHECK(loaded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(loaded.getMaterialCount() == 2);

    auto outA = loaded.getMaterial(0);
    auto outB = loaded.getMaterial(1);
    CHECK(outA != nullptr);
    CHECK(outB != nullptr);
    CHECK(outA->getName() == std::string("MatA"));
    CHECK(outB->getName() == std::string("MatB"));
    CHECK(outA->getShader() == std::string("shaders/a.phoskia"));
    CHECK(outB->getShader() == std::string("shaders/b.phoskia"));
    CHECK(outA->getAlphaMode() == MaterialAlphaMode::Mask);
    CHECK(outA->getAlphaCutoff() == 0.37f);
    CHECK(outA->isDoubleSided());
    CHECK(outB->getAlphaMode() == MaterialAlphaMode::Blend);
    CHECK_FALSE(outB->isDoubleSided());
    CHECK(outA->getFloat("metallic") == 0.25f);
}

TEST_CASE(save_and_load_single_material_surface_contract)
{
    Material original;
    original.setName("HairCards");
    original.setShader("shaders/pbr.phoskia");
    original.setSurfaceProperties(MaterialAlphaMode::Mask, 0.42f, true);
    original.setTexture("baseColorTexture", "textures/hair.png");

    std::vector<UInt8> bytes;
    CHECK(original.saveToBinary(bytes));

    Material loaded;
    CHECK(loaded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(loaded.getAlphaMode() == MaterialAlphaMode::Mask);
    CHECK(loaded.getAlphaCutoff() == 0.42f);
    CHECK(loaded.isDoubleSided());
    CHECK(loaded.getTexture("baseColorTexture") ==
          std::string("textures/hair.png"));
}

TEST_SUITE_END

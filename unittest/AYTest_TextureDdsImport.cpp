#include <AYTest.h>
#include <AYResource/ImportJob.h>
#include <AYResource/Loader/TextureLoader.h>
#include <AYResource/assetsImpl/Texture.h>
#include "../src/ImportDds.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
using namespace ayt::resource;
namespace {
namespace fs=std::filesystem;
void put(std::vector<UInt8>& bytes,std::size_t offset,std::uint32_t value){
    for(int byte=0;byte<4;++byte)bytes[offset+std::size_t(byte)]=UInt8((value>>(byte*8))&255);
}
std::vector<UInt8> dds(bool dx10=false,std::uint32_t format=98){
    std::vector<UInt8> bytes(dx10?180u:136u,0);
    put(bytes,0,detail::ddsFourCC('D','D','S',' '));put(bytes,4,124);
    put(bytes,8,0x81007);put(bytes,12,4);put(bytes,16,4);put(bytes,20,dx10?16:8);
    put(bytes,28,dx10?2:1);put(bytes,76,32);put(bytes,80,4);
    put(bytes,84,dx10?detail::ddsFourCC('D','X','1','0'):detail::ddsFourCC('D','X','T','1'));
    put(bytes,108,0x1000);
    if(dx10){put(bytes,128,format);put(bytes,132,3);put(bytes,140,1);}
    else {bytes[128]=0;bytes[129]=0xf8;} // BC1 block, color0 red, all indices zero.
    return bytes;
}
struct Fixture{
    fs::path root=fs::temp_directory_path()/("ay-dds-import-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Fixture(){std::error_code error;fs::remove_all(root,error);}
    void write(const std::vector<UInt8>& bytes){
        fs::create_directories(root);
        std::ofstream file(root/"Surface.dds",std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    }
    ImportOptions options()const{
        ImportOptions value;value.sourcePath=(root/"Surface.dds").string();
        value.outputDir=(root/"Imported").string();value.outputNamespace="dds-source";
        value.requireCharacterAssets=false;return value;
    }
    std::string read(const fs::path& path)const{
        std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
    }
};
}
TEST_SUITE(TextureDdsImportTests)
TEST_CASE(legacy_bc1_and_dx10_bc7_are_wrapped_as_loadable_engine_textures)
{
    for(bool dx10:{false,true}){
        Fixture fixture;const auto source=dds(dx10);fixture.write(source);
        const auto imported=importAsset(fixture.options());
        CHECK_MSG(imported.ok,imported.error.c_str());
        CHECK(imported.conversion.resources.size()==1u);
        if(!imported.ok || imported.conversion.resources.empty())continue;
        const auto& resource=imported.conversion.resources[0];
        TextureLoader loader;
        const auto texture=std::dynamic_pointer_cast<Texture>(loader.load((fixture.root/"Imported"/resource.path).string()));
        CHECK(texture!=nullptr);
        if(texture){
            CHECK(texture->getWidth()==4 && texture->getHeight()==4);
            CHECK(texture->getFormat()==(dx10?TextureFormat::BC7:TextureFormat::BC1));
            CHECK(texture->getMipmapCount()==(dx10?2u:1u));
            CHECK(texture->getImageDataSize()==(dx10?32u:8u));
            CHECK(texture->getImageDataBytes()[1]==(dx10?0u:0xf8u));
        }
        CHECK(importAsset(fixture.options()).usedCache);
    }
}
TEST_CASE(dds_decoder_refuses_every_truncated_prefix_and_unrepresentable_surfaces)
{
    const auto valid=dds(true);
    detail::DdsImportData data;data.width=123;
    std::string error;
    for(std::size_t length=0;length<valid.size();++length){
        const std::vector<UInt8> prefix(valid.begin(),valid.begin()+std::ptrdiff_t(length));
        CHECK_FALSE(detail::parseImportDds(prefix,data,error));
        CHECK(data.width==123);
    }
    for(int variant=0;variant<5;++variant){
        auto bad=valid;
        if(variant==0)put(bad,140,2);
        if(variant==1)put(bad,136,4);
        if(variant==2)put(bad,132,4);
        if(variant==3)put(bad,128,81); // Signed BC4 has no matching engine format.
        if(variant==4)put(bad,28,17);
        CHECK_FALSE(detail::parseImportDds(bad,data,error));
    }
}
TEST_CASE(bad_dds_reimport_preserves_previous_typed_payload_and_metadata)
{
    Fixture fixture;fixture.write(dds());
    auto options=fixture.options();
    const auto first=importAsset(options);
    CHECK_MSG(first.ok,first.error.c_str());
    if(!first.ok)return;
    const auto path=fixture.root/"Imported"/first.conversion.resources[0].path;
    const auto original=fixture.read(path),metadata=fixture.read(fs::u8path(first.depSidecarPath));
    auto bad=dds();bad.pop_back();fixture.write(bad);options.force=true;
    const auto failed=importAsset(options);
    CHECK_FALSE(failed.ok);
    CHECK(fixture.read(path)==original);
    CHECK(fixture.read(fs::u8path(first.depSidecarPath))==metadata);
}
TEST_CASE(sampling_settings_roundtrip_recipe_invalidate_cache_and_preserve_compressed_pixels)
{
    Fixture fixture;fixture.write(dds(true));
    auto options=fixture.options();
    const auto original=importAsset(options);CHECK(original.ok);
    options.overrideTextureSampler=true;
    options.textureSampler.minFilter=TextureFilter::Trilinear;
    options.textureSampler.magFilter=TextureFilter::Point;
    options.textureSampler.wrapU=TextureWrap::Mirror;
    options.textureSampler.wrapV=TextureWrap::Clamp;
    ImportOptions restored;std::string error;
    CHECK(parseImportRecipe(serializeImportRecipe(options),restored,&error));
    CHECK(restored.overrideTextureSampler && restored.keepTextureMipmaps);
    CHECK(restored.textureSampler.minFilter==TextureFilter::Trilinear);
    const auto changed=importAsset(restored);CHECK_MSG(changed.ok,changed.error.c_str());
    CHECK_FALSE(changed.usedCache);
    if(!changed.ok || changed.conversion.resources.empty())return;
    const auto path=fixture.root/"Imported"/changed.conversion.resources[0].path;
    Texture texture;CHECK(texture.load(path.string()));
    CHECK(texture.getMipmapCount()==2);CHECK(texture.getFormat()==TextureFormat::BC7);
    CHECK(texture.getImageDataSize()==32);
    CHECK(texture.getSampler().minFilter==TextureFilter::Trilinear);
    CHECK(texture.getSampler().magFilter==TextureFilter::Point);
    CHECK(texture.getSampler().wrapU==TextureWrap::Mirror && texture.getSampler().wrapV==TextureWrap::Clamp);
    CHECK(importAsset(restored).usedCache);
    restored.keepTextureMipmaps=false;
    const auto base=importAsset(restored);CHECK(base.ok);CHECK_FALSE(base.usedCache);
    CHECK(texture.load(path.string()));CHECK(texture.getMipmapCount()==1);
    CHECK(texture.getImageDataSize()==16);CHECK(importAsset(restored).usedCache);
    restored.textureSampler.minFilter=TextureFilter::Point;
    const auto nearest=importAsset(restored);CHECK(nearest.ok);CHECK_FALSE(nearest.usedCache);
    CHECK(texture.load(path.string()));CHECK(texture.getSampler().minFilter==TextureFilter::Point);
}

TEST_CASE(invalid_sampling_recipe_preserves_previous_options_and_last_published_texture)
{
    Fixture fixture;fixture.write(dds());
    auto options=fixture.options();options.overrideTextureSampler=true;
    const auto good=importAsset(options);CHECK(good.ok);
    if(!good.ok || good.conversion.resources.empty())return;
    const auto path=fixture.root/"Imported"/good.conversion.resources[0].path;
    const auto previous=fixture.read(path);
    for(int variant=0;variant<5;++variant){
        auto value=nlohmann::json::parse(serializeImportRecipe(options));
        auto& sampler=value["settings"]["textureSampling"];
        if(variant==0)sampler["min"]=5;
        if(variant==1)sampler["wrapV"]=-1;
        if(variant==2)sampler["anisotropy"]=0;
        if(variant==3)sampler["lodBias"]=nullptr;
        if(variant==4)sampler["mag"]=2.5;
        ImportOptions target;target.sourcePath="preserve";
        CHECK_FALSE(parseImportRecipe(value.dump(),target));CHECK(target.sourcePath=="preserve");
    }
    options.textureSampler.anisotropy=32;
    const auto bad=importAsset(options);CHECK_FALSE(bad.ok);
    CHECK(fixture.read(path)==previous);
}

TEST_CASE(raw_image_sampling_override_is_refused_without_replacing_previous_outputs)
{
    Fixture fixture;fixture.write(dds());
    const std::string hex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    const auto source=fixture.root/"Surface.png";
    { std::ofstream file(source,std::ios::binary);
      for(size_t i=0;i<hex.size();i+=2)file.put(char(std::stoi(hex.substr(i,2),nullptr,16))); }
    auto options=fixture.options();options.sourcePath=source.string();
    const auto raw=importAsset(options);CHECK_MSG(raw.ok,raw.error.c_str());
    if(!raw.ok || raw.conversion.resources.empty())return;
    const auto path=fixture.root/"Imported"/raw.conversion.resources[0].path;
    const auto previous=fixture.read(path);
    options.overrideTextureSampler=true;options.textureSampler.minFilter=TextureFilter::Trilinear;
    const auto refused=importAsset(options);CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("require cooked texture output")!=std::string::npos);
    CHECK(fixture.read(path)==previous);
    options.cookTextures=true;
    const auto cooked=importAsset(options);CHECK_MSG(cooked.ok,cooked.error.c_str());
    if(!cooked.ok || cooked.conversion.resources.empty())return;
    Texture texture;CHECK(texture.load((fixture.root/"Imported"/cooked.conversion.resources[0].path).string()));
    CHECK(texture.getSampler().minFilter==TextureFilter::Trilinear);
    CHECK(texture.getMipmapCount()==2);
    CHECK(fixture.read(path)==previous); // Output-mode changes retain old referenced paths.
}

TEST_CASE(batch_import_propagates_texture_sampling_settings)
{
    Fixture fixture;fixture.write(dds());
    const auto other=fixture.root/"Second.dds";
    { const auto bytes=dds();std::ofstream file(other,std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); }
    ImportBatchOptions options;
    options.sourcePaths={(fixture.root/"Surface.dds").string(),other.string()};
    options.outputDir=(fixture.root/"Imported").string();options.requireCharacterAssets=false;
    options.overrideTextureSampler=true;options.textureSampler.minFilter=TextureFilter::Point;
    options.textureSampler.wrapU=TextureWrap::Mirror;
    const auto imported=importAssetBatch(options);CHECK(imported.okCount==2 && imported.failCount==0);
    for(const auto& result:imported.results){
        CHECK(result.ok && !result.conversion.resources.empty());
        if(!result.ok || result.conversion.resources.empty())continue;
        Texture texture;CHECK(texture.load((fixture.root/"Imported"/result.conversion.resources[0].path).string()));
        CHECK(texture.getSampler().minFilter==TextureFilter::Point);
        CHECK(texture.getSampler().wrapU==TextureWrap::Mirror);
    }
}

TEST_SUITE_END

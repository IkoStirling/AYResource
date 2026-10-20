#include <AYTest.h>
#include <AYResource/ImportJob.h>
#include <AYResource/Loader/TextureLoader.h>
#include <AYResource/assetsImpl/Texture.h>
#include "../src/ImportDds.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
TEST_SUITE_END

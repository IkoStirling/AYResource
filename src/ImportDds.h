#pragma once
#include <AYResource/IntermediateAsset.h>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ayt::resource::detail {
struct DdsImportData {
    UInt32 width=0,height=0;
    TextureFormat format=TextureFormat::BC1;
    std::vector<UInt32> offsets,sizes;
    std::vector<UInt8> blocks;
};
constexpr std::uint32_t ddsFourCC(char a,char b,char c,char d)
{
    return std::uint32_t(static_cast<unsigned char>(a))
        | (std::uint32_t(static_cast<unsigned char>(b))<<8)
        | (std::uint32_t(static_cast<unsigned char>(c))<<16)
        | (std::uint32_t(static_cast<unsigned char>(d))<<24);
}
// DDS_HEADER / DDS_HEADER_DXT10 per Microsoft's DDS programming reference.
// Only one 2D UNORM BC texture is representable by the current Texture codec.
inline bool parseImportDds(const std::vector<UInt8>& bytes,DdsImportData& output,std::string& error)
{
    const auto reject=[&](const char* reason){error=reason;return false;};
    if(bytes.size()<128)return reject("truncated DDS header");
    const auto word=[&](std::size_t offset){
        return std::uint32_t(bytes[offset])|(std::uint32_t(bytes[offset+1])<<8)
            |(std::uint32_t(bytes[offset+2])<<16)|(std::uint32_t(bytes[offset+3])<<24);
    };
    if(word(0)!=ddsFourCC('D','D','S',' ') || word(4)!=124 || word(76)!=32)
        return reject("invalid DDS header identity/size");
    if(word(112)&(0xfe00u|0x200000u))return reject("DDS cube/volume textures are not a 2D import");
    if(word(24)>1)return reject("DDS depth is not a 2D surface");
    DdsImportData candidate;
    candidate.width=word(16);candidate.height=word(12);
    if(!candidate.width || !candidate.height)return reject("zero DDS dimensions");
    const auto count=word(28)?word(28):1u;
    if(count>16)return reject("DDS mip count exceeds the texture codec");
    std::size_t dataOffset=128;
    switch(word(84)){
    case ddsFourCC('D','X','T','1'):candidate.format=TextureFormat::BC1;break;
    case ddsFourCC('D','X','T','5'):candidate.format=TextureFormat::BC3;break;
    case ddsFourCC('A','T','I','1'):
    case ddsFourCC('B','C','4','U'):candidate.format=TextureFormat::BC4;break;
    case ddsFourCC('A','T','I','2'):
    case ddsFourCC('B','C','5','U'):candidate.format=TextureFormat::BC5;break;
    case ddsFourCC('D','X','1','0'):
        if(bytes.size()<148)return reject("truncated DDS DX10 header");
        if(word(132)!=3 || word(140)!=1 || (word(136)&4))
            return reject("DDS arrays/cubes/other dimensions are not a single 2D texture");
        dataOffset=148;
        switch(word(128)){
        case 71:case 72:candidate.format=TextureFormat::BC1;break;
        case 77:case 78:candidate.format=TextureFormat::BC3;break;
        case 80:candidate.format=TextureFormat::BC4;break;
        case 83:candidate.format=TextureFormat::BC5;break;
        case 98:case 99:candidate.format=TextureFormat::BC7;break;
        default:return reject("unsupported DDS DXGI format; expected UNORM BC1/3/4/5/7");
        }
        break;
    default:return reject("unsupported DDS FourCC; expected BC1/3/4/5 or DX10 BC7");
    }
    auto width=candidate.width,height=candidate.height;
    std::uint64_t total=0;
    for(UInt32 mip=0;mip<count;++mip){
        const auto blockBytes=candidate.format==TextureFormat::BC1 || candidate.format==TextureFormat::BC4 ? 8u:16u;
        const auto size=((std::uint64_t(width)+3)/4)*((std::uint64_t(height)+3)/4)*blockBytes;
        if(size>(std::numeric_limits<UInt32>::max)() || total+size>bytes.size()-dataOffset
            || total+size>(std::numeric_limits<UInt32>::max)())return reject("truncated or oversized DDS mip payload");
        candidate.offsets.push_back(UInt32(total));candidate.sizes.push_back(UInt32(size));total+=size;
        if(mip+1<count && width==1 && height==1)return reject("DDS has extra mips after 1x1");
        width=width>1?width/2:1;height=height>1?height/2:1;
    }
    if(total!=bytes.size()-dataOffset)return reject("DDS contains an unsupported extra surface or trailing payload");
    candidate.blocks.assign(bytes.begin()+static_cast<std::ptrdiff_t>(dataOffset),bytes.end());
    output=std::move(candidate);error.clear();return true;
}
}

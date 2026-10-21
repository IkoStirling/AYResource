#include <AYTest.h>
#include <AYResource/assetsImpl/Texture.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
using namespace ayt::resource;
void put32(std::vector<UInt8>& bytes,size_t offset,uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = static_cast<UInt8>(value>>(8*i));
}
std::vector<UInt8> legacy() {
    // Independent little-endian v1 fixture: 36-byte header, 1x1 RGBA8.
    std::vector<UInt8> bytes(40,0);
    put32(bytes,0,ITexture::MAGIC); bytes[4] = 1; bytes[23] = 1;
    put32(bytes,24,1); put32(bytes,28,1); put32(bytes,32,4);
    bytes[36] = 11; bytes[37] = 22; bytes[38] = 33; bytes[39] = 255;
    return bytes;
}
Texture authored() {
    Texture texture; texture.createSolidColor(4,4,64,128,192);
    TextureSampler sampler;
    sampler.minFilter = TextureFilter::Anisotropic; sampler.magFilter = TextureFilter::Point;
    sampler.wrapU = TextureWrap::Mirror; sampler.wrapV = TextureWrap::Clamp; sampler.wrapW = TextureWrap::Border;
    sampler.anisotropy = 8; sampler.sRGB = true; sampler.lodBias = -1.25f;
    texture.setSampler(sampler);
    return texture;
}
} // namespace

TEST_SUITE(TextureSamplingCodecTests)

TEST_CASE(v2_roundtrip_preserves_every_sampler_field_and_image_bytes) {
    const auto source = authored();
    std::vector<UInt8> bytes; CHECK(source.saveToBinary(bytes));
    CHECK(bytes.size() == 52+64); CHECK(bytes[4] == 2); CHECK(bytes[5] == 0);
    CHECK(bytes[36] == 4); CHECK(bytes[37] == 1);
    CHECK(bytes[38] == 2); CHECK(bytes[39] == 1); CHECK(bytes[40] == 3); CHECK(bytes[41] == 1);
    CHECK(bytes[42] == 0 && bytes[43] == 0);
    Texture loaded; CHECK(loaded.loadFromBinary(bytes.data(),bytes.size()));
    const auto& sampler = loaded.getSampler();
    CHECK(sampler.minFilter == TextureFilter::Anisotropic); CHECK(sampler.magFilter == TextureFilter::Point);
    CHECK(sampler.wrapU == TextureWrap::Mirror); CHECK(sampler.wrapV == TextureWrap::Clamp);
    CHECK(sampler.wrapW == TextureWrap::Border); CHECK(sampler.sRGB);
    CHECK(sampler.anisotropy == 8); CHECK(sampler.lodBias == -1.25f);
    CHECK(loaded.getMipmapSize(0) == 64);
    CHECK(std::equal(source.getMipmapData(),source.getMipmapData()+64,loaded.getMipmapData()));
    std::vector<UInt8> encodedAgain; CHECK(loaded.saveToBinary(encodedAgain)); CHECK(bytes == encodedAgain);
}

TEST_CASE(v1_load_uses_historical_defaults_and_never_inherits_prior_sampler) {
    auto texture = authored(); const auto bytes = legacy();
    CHECK(texture.loadFromBinary(bytes.data(),bytes.size()));
    CHECK(texture.getWidth() == 1 && texture.getHeight() == 1);
    CHECK(texture.getMipmapData()[0] == 11); CHECK(texture.getMipmapData()[2] == 33);
    const auto& sampler = texture.getSampler();
    CHECK(sampler.minFilter == TextureFilter::Bilinear && sampler.magFilter == TextureFilter::Bilinear);
    CHECK(sampler.wrapU == TextureWrap::Repeat && sampler.wrapV == TextureWrap::Repeat);
    CHECK(sampler.wrapW == TextureWrap::Repeat && sampler.anisotropy == 1);
    CHECK_FALSE(sampler.sRGB); CHECK(sampler.lodBias == 0);
}

TEST_CASE(every_truncated_prefix_is_refused_and_preserves_previous_image_and_sampler) {
    const auto source = authored(); std::vector<UInt8> bytes;
    CHECK(source.saveToBinary(bytes));
    for (size_t size = 0; size < bytes.size(); ++size) {
        auto texture = authored();
        CHECK_FALSE(texture.loadFromBinary(bytes.data(),size));
        CHECK(texture.getWidth() == 4 && texture.getMipmapData()[0] == 64);
        CHECK(texture.getSampler().anisotropy == 8 && texture.getSampler().sRGB);
        CHECK(texture.isLoaded());
    }
}

TEST_CASE(bad_headers_sampler_enums_flags_and_nonfinite_values_are_refused) {
    const auto source = authored(); std::vector<UInt8> good; CHECK(source.saveToBinary(good));
    for (int variant = 0; variant < 17; ++variant) {
        auto bad = good;
        switch (variant) {
        case 0: bad[0] ^= 1; break;
        case 1: bad[4] = 3; break;
        case 2: bad[22] = 255; break;
        case 3: bad[23] = 0; break;
        case 4: bad[23] = 4; break;
        case 5: put32(bad,24,0); break;
        case 6: put32(bad,24,UINT32_MAX); put32(bad,28,UINT32_MAX); break;
        case 7: put32(bad,32,UINT32_MAX); break;
        case 8: bad[36] = 5; break;
        case 9: bad[37] = 255; break;
        case 10: bad[38] = 4; break;
        case 11: bad[41] = 2; break;
        case 12: bad[42] = 1; break;
        case 13: put32(bad,44,0x7fc00000); break;
        case 14: put32(bad,48,0x7f800000); break;
        case 15: put32(bad,44,0); break;
        case 16: bad.push_back(0); break;
        }
        auto texture = authored();
        CHECK_FALSE(texture.loadFromBinary(bad.data(),bad.size()));
        CHECK(texture.getMipmapData()[0] == 64); CHECK(texture.getSampler().lodBias == -1.25f);
    }
}

TEST_CASE(invalid_public_mip_layout_is_bounded_and_failed_save_preserves_output) {
    for (int variant = 0; variant < 6; ++variant) {
        auto texture = authored();
        if (variant == 0) texture.setMipmapLayout({UINT32_MAX},{64});
        if (variant == 1) texture.setMipmapLayout({1},{64});
        if (variant == 2) texture.setMipmapLayout({0},{65});
        if (variant == 3) texture.setMipmapLayout({0},{});
        if (variant == 4) {auto sampler = texture.getSampler(); sampler.anisotropy = 32; texture.setSampler(sampler);}
        if (variant == 5) texture.setMipmapLayout({0},{64,4}); // Unmatched trailing table entry.
        if (variant < 4) CHECK(texture.getMipmapData() == nullptr);
        std::vector<UInt8> output{7,8,9};
        CHECK_FALSE(texture.saveToBinary(output)); CHECK(output == std::vector<UInt8>({7,8,9}));
    }
}

TEST_CASE(compute_size_refuses_unknown_formats_and_integer_overflow) {
    CHECK(Texture::computeMipSize(UINT32_MAX,UINT32_MAX,TextureFormat::RGBA8) == 0);
    CHECK(Texture::computeMipSize(UINT32_MAX,UINT32_MAX,TextureFormat::BC7) == 0);
    CHECK(Texture::computeMipSize(4,4,static_cast<TextureFormat>(255)) == 0);
    CHECK(Texture::computeMipSize(5,3,TextureFormat::BC1) == 16);
    CHECK(Texture::computeMipSize(1,1,TextureFormat::BC7) == 16);
}

TEST_CASE(helper_creation_rejects_overflow_and_zero_checker_size_without_allocation) {
    auto texture = authored();
    texture.createSolidColor(UINT32_MAX,UINT32_MAX,0,0,0);
    CHECK_FALSE(texture.isLoaded()); CHECK(texture.getMipmapData() == nullptr);
    texture.createCheckerboard(4,4,0);
    CHECK_FALSE(texture.isLoaded()); CHECK(texture.getMipmapData() == nullptr);
    texture.createSolidColor(4,4,1,2,3);
    CHECK(texture.isLoaded()); CHECK(texture.getMipmapSize() == 64);
    CHECK(texture.getSampler().minFilter == TextureFilter::Bilinear);
}

TEST_SUITE_END

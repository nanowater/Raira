// Tests font_image_parse() (src/fonts/font_image.cpp) with a real image built by tools/make_fonts_image.py and with damaged ones.
//   test <fonts.bin> <Regular.otf> <Bold.otf>
#include "fonts/font_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string & what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}

static std::vector<uint8_t> read_file(const char * path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void put_u32(std::vector<uint8_t> & v, size_t at, uint32_t x)
{
    for(int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i));
}

static uint32_t read_off0(const std::vector<uint8_t> & v)
{
    return (uint32_t)v[12] | ((uint32_t)v[13] << 8) | ((uint32_t)v[14] << 16) | ((uint32_t)v[15] << 24);
}

static bool parses(const std::vector<uint8_t> & img)
{
    FontImage f;
    return font_image_parse(img.data(), img.size(), f);
}

int main(int argc, char ** argv)
{
    if(argc != 4) {
        std::fprintf(stderr, "usage: %s fonts.bin Regular.otf Bold.otf\n", argv[0]);
        return 2;
    }
    const auto image = read_file(argv[1]);
    const auto reg   = read_file(argv[2]);
    const auto bold  = read_file(argv[3]);

    FontImage f;
    const bool good = font_image_parse(image.data(), image.size(), f);
    check(good, "the real image parses");
    if(good) {
        check(f.regular_size == reg.size() && std::memcmp(f.regular, reg.data(), reg.size()) == 0, "Regular is byte for byte the file");
        check(f.bold_size == bold.size() && std::memcmp(f.bold, bold.data(), bold.size()) == 0, "Bold is byte for byte the file");
        check(((f.regular - image.data()) % 16) == 0 && ((f.bold - image.data()) % 16) == 0, "both fonts start on a 16-byte boundary");
        check(f.regular_size > 3000000 && std::memcmp(f.regular, "OTTO", 4) == 0, "Regular is an OpenType/CFF font");
    }

    check(!font_image_parse(nullptr, 0, f), "null image");
    check(!parses({}), "empty image");
    check(!parses(std::vector<uint8_t>(image.begin(), image.begin() + 20)), "image shorter than its header");
    check(!parses(std::vector<uint8_t>(0x780000, 0xFF)), "erased flash (all 0xFF)");
    check(!parses(std::vector<uint8_t>(0x780000, 0x00)), "zeroed flash");

    // header layout: 0 magic, 4 version, 8 count, 12 offset0, 16 size0, 20 offset1, 24 size1
    const size_t OFF0 = 12, SIZE0 = 16, OFF1 = 20, SIZE1 = 24;
    auto bad = image;
    bad[0] = 'X';
    check(!parses(bad), "wrong magic");
    bad = image;
    put_u32(bad, 4, 2);
    check(!parses(bad), "unknown version");
    bad = image;
    put_u32(bad, 8, 3);
    check(!parses(bad), "unexpected font count");
    bad = image;
    put_u32(bad, OFF0, (uint32_t)image.size() + 1);
    check(!parses(bad), "entry 0 starts behind the image");
    bad = image;
    put_u32(bad, SIZE0, (uint32_t)image.size());
    check(!parses(bad), "entry 0 is longer than the image");
    bad = image;
    put_u32(bad, OFF1, 0xFFFFFFF0u);
    put_u32(bad, SIZE1, 0x20);
    check(!parses(bad), "entry 1: offset + size overflows 32 bits");
    bad = image;
    put_u32(bad, OFF0, 4);
    check(!parses(bad), "entry 0 overlaps the header");
    bad = image;
    put_u32(bad, SIZE1, 0);
    check(!parses(bad), "entry 1 has size 0");
    bad = image;
    put_u32(bad, SIZE1, (uint32_t)image.size());
    check(!parses(bad), "entry 1 is longer than the image");
    // the untouched copies must still parse, so that each failure above is caused by its own change
    bad = image;
    put_u32(bad, OFF0, read_off0(image));
    check(parses(bad), "rewriting a field with its own value changes nothing");
    check(!parses(std::vector<uint8_t>(image.begin(), image.end() - 100)), "image cut inside the Bold font");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

#include "fonts/font_image.h"

#include <cstring>

namespace {

const size_t HEADER_SIZE  = 12;
const size_t ENTRY_SIZE   = 8;
const uint32_t VERSION    = 1;
const uint32_t FONT_COUNT = 2;

uint32_t read_u32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* offset and size of entry `i` are inside the image (written so that the sums cannot overflow) */
bool entry_fits(const uint8_t * image, size_t image_size, uint32_t i, const uint8_t *& data, size_t & size)
{
    const uint8_t * e    = image + HEADER_SIZE + i * ENTRY_SIZE;
    const uint32_t off   = read_u32(e);
    const uint32_t len   = read_u32(e + 4);
    const size_t min_off = HEADER_SIZE + FONT_COUNT * ENTRY_SIZE; /* not inside the header */
    if(len == 0 || off < min_off || off > image_size || len > image_size - off) return false;
    data = image + off;
    size = len;
    return true;
}

} // namespace

bool font_image_parse(const uint8_t * image, size_t image_size, FontImage & out)
{
    if(!image || image_size < HEADER_SIZE + FONT_COUNT * ENTRY_SIZE) return false;
    if(std::memcmp(image, "MCFT", 4) != 0) return false;
    if(read_u32(image + 4) != VERSION || read_u32(image + 8) != FONT_COUNT) return false;

    FontImage f;
    if(!entry_fits(image, image_size, 0, f.regular, f.regular_size)) return false;
    if(!entry_fits(image, image_size, 1, f.bold, f.bold_size)) return false;
    out = f;
    return true;
}

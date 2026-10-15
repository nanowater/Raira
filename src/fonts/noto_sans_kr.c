/* Embeds the Noto Sans CJK KR subset (Latin, Hangul, kana, punctuation; SIL OFL 1.1)
 * so the UI needs no font file at runtime. NOTO_SANS_KR_PATH is set by CMake. */
#include <stddef.h>

__asm__(
    ".section .rodata\n"
    ".global noto_sans_kr_data\n"
    ".balign 16\n"
    "noto_sans_kr_data:\n"
    ".incbin \"" NOTO_SANS_KR_PATH "\"\n"
    ".global noto_sans_kr_data_end\n"
    "noto_sans_kr_data_end:\n"
    ".byte 0\n"
    ".previous\n");

extern const unsigned char noto_sans_kr_data[];
extern const unsigned char noto_sans_kr_data_end[];

const unsigned char * noto_sans_kr_get(size_t * size)
{
    *size = (size_t)(noto_sans_kr_data_end - noto_sans_kr_data);
    return noto_sans_kr_data;
}

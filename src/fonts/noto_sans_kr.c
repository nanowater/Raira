/* Embeds the Noto Sans CJK KR subsets (Regular and Bold; SIL OFL 1.1) so the UI needs no
 * font file at runtime. The *_PATH macros are set by CMake. */
#include <stddef.h>

#define EMBED_FILE(sym, path)                \
    __asm__(".section .rodata\n"             \
            ".global " #sym "_data\n"        \
            ".balign 16\n" #sym "_data:\n"   \
            ".incbin \"" path "\"\n"         \
            ".global " #sym "_data_end\n" #sym "_data_end:\n" \
            ".byte 0\n"                      \
            ".previous\n");                  \
    extern const unsigned char sym##_data[]; \
    extern const unsigned char sym##_data_end[]; \
    const unsigned char * sym##_get(size_t * size) \
    {                                        \
        *size = (size_t)(sym##_data_end - sym##_data); \
        return sym##_data;                   \
    }

EMBED_FILE(noto_sans_kr, NOTO_SANS_KR_PATH)
EMBED_FILE(noto_sans_kr_bold, NOTO_SANS_KR_BOLD_PATH)

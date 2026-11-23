#ifndef SEEDLESS_COMMANDS_H
#define SEEDLESS_COMMANDS_H
#include <stdbool.h>
#include <stdint.h>

enum { SCREEN_W = 256, SCREEN_H = 192, BIN_WIDTH = 32, BINS_PER_ROW = SCREEN_W / BIN_WIDTH,
       BIN_COUNT = BINS_PER_ROW * SCREEN_H };
typedef struct {
    uint32_t x_width, z, dz, fraction;
    uint32_t red, green, blue, coef;
    uint32_t uv, duv, texture_base, palette_base;
    uint32_t tex_param, attr, flags, reserved;
} compute_span;
_Static_assert(sizeof(compute_span) == 64, "std430 span layout");

bool compute_gl_init(void);
void compute_gl_close(void);
bool compute_gl_draw(const compute_span *spans, unsigned count,
                     const uint32_t *bins, unsigned bin_words, const uint32_t *arena, unsigned words, unsigned dirty_first, unsigned dirty_end,
                     const uint32_t *coefficients, unsigned coef_words, uint32_t clear, uint32_t depth, uint32_t alpha, bool effects, void *dst);
#endif

#ifndef SEEDLESS_COMPUTE_H
#define SEEDLESS_COMPUTE_H

#include <stdbool.h>
#include <stdint.h>
#include "gpu/gpu.h"

/* Called before the emulation thread starts and after its workers stop. */
void compute_start(bool verify);
void compute_stop(void);
bool compute_frame(gpu_t *gpu, gpu3d_t *scene, unsigned bank, uint8_t *dst);
void compute_check(const uint8_t *reference);

/* Native scan conversion calls these sinks only during command collection.
 * A failed collection always falls back before publishing any output. */
void compute_rows(const uint8_t *rows, unsigned y, unsigned count,
                  unsigned flags, const gpu3d_bank_vertex_t *first);
void compute_rect(unsigned x, unsigned y, unsigned w, unsigned h, int s, int t,
                  uint32_t depth, uint32_t color);
void gpu3d_compute_collect_edges(uint8_t *ctx, const gpu3d_polygon_t *polygon,
                                const gpu3d_bank_vertex_t *vertices,
                                uint32_t y0, uint32_t y1);
#endif

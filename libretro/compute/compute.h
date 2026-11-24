#ifndef SEEDLESS_COMPUTE_H
#define SEEDLESS_COMPUTE_H

#include <stdbool.h>
#include <stdint.h>
#include "gpu/gpu.h"

typedef struct compute_profile {
    uint64_t draws;
    uint64_t upload_ns;
    uint64_t submit_ns;
    uint64_t map_wait_ns;
    uint64_t copy_ns;
    uint64_t gpu_ns;
    uint64_t gpu_timed_draws;
    uint64_t gpu_disjoint_draws;
} compute_profile_t;

/* Called before the emulation thread starts and after its workers stop. */
void compute_start(bool verify);
/* Read after the emulation and raster threads are quiescent. */
bool compute_report(void);
void compute_stop(void);
void compute_profile_reset(void);
void compute_profile_read(compute_profile_t *profile);
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

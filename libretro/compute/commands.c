#include "compute.h"
#include "commands.h"
#include "core_internals.h"
#include "hires_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame dispatcher serializes submissions. All storage is private to the
 * backend, never in the savestate or in a band worker's published page. */
enum { MAX_SPANS = 32768, MAX_WORDS = 2*1024*1024, HASH_SIZE = 4096 };
typedef struct {
    const gpu3d_texture_entry_t *key;
    uint32_t texels, palette;
} texture_slot;

static struct {
    bool enabled, verify, ready, failed, compare;
    unsigned count, words, span_cap, arena_cap;
    unsigned coef_words, coef_cap, known_words, dirty_first, dirty_end;
    uint32_t *coefficients;
    uint32_t counts[BIN_COUNT];
    unsigned bin_cap;
    uint32_t *bins;
    compute_span material;
    compute_span *input;
    uint32_t *arena, *reference_candidate, *planes;
    texture_slot textures[HASH_SIZE];
    uint64_t rendered, fallback, compared;
} batch;

static void *grow(void *old, unsigned *capacity, unsigned needed,
                  unsigned limit, size_t size)
{
    if (needed <= *capacity) return old;
    if (needed > limit) { batch.failed = true; return old; }
    unsigned n = *capacity ? *capacity : 256;
    while (n < needed) n *= 2;
    void *p = realloc(old, n * size);
    if (!p) { batch.failed = true; return old; }
    *capacity = n;
    return p;
}

static unsigned append_words(const void *src, unsigned words)
{
    unsigned base = batch.words;
    batch.arena = grow(batch.arena, &batch.arena_cap, base + words,
                       MAX_WORDS, sizeof(uint32_t));
    if (!batch.failed) {
        /* The CPU texture cache may recycle a pointer on a palette/VRAM
         * change. Compare bytes at the atlas location, never trust identity
         * across frames. Accumulate changes even across abandoned batches. */
        if (base+words > batch.known_words ||
            memcmp(batch.arena+base, src, words*4u) != 0) {
            memcpy(batch.arena+base, src, words*4u);
            if (base < batch.dirty_first) batch.dirty_first = base;
            if (base+words > batch.dirty_end) batch.dirty_end = base+words;
            if (base+words > batch.known_words) batch.known_words = base+words;
        }
        batch.words += words;
    }
    return base;
}

static bool material(const gpu3d_polygon_t *p, unsigned disp)
{
    compute_span *s = &batch.material;
    memset(s, 0, sizeof(*s));
    s->attr = p->polygon_attr;
    s->tex_param = p->texture_param;
    s->texture_base = s->palette_base = UINT32_MAX;
    if ((s->attr & 48u) == 48u || !(s->attr & 0x1f0000u)) return false;
    if (disp & 2) s->flags |= 32;
    if (disp & 0x20) s->flags |= 64;
    if (!(p->vertex_count & 0x200)) return true;
    const gpu3d_texture_entry_t *t = p->texture;
    if (!t || !t->data || (t->palette_count && !t->palette) ||
        t->palette_count > 256 ||
        t->width != (8u << ((s->tex_param >> 20) & 7u)) ||
        t->height != (8u << ((s->tex_param >> 23) & 7u))) return false;
    unsigned index = ((uintptr_t)t >> 4) & (HASH_SIZE-1);
    while (batch.textures[index].key && batch.textures[index].key != t)
        index = (index+1) & (HASH_SIZE-1);
    texture_slot *slot = &batch.textures[index];
    if (!slot->key) {
        unsigned area = t->width * t->height;
        slot->key = t;
        slot->texels = append_words(t->data, t->palette_count ? area/4 : area);
        slot->palette = t->palette_count ? append_words(t->palette, t->palette_count) : UINT32_MAX;
    }
    s->texture_base = slot->texels;
    s->palette_base = slot->palette;
    return !batch.failed;
}

static compute_span *new_span(unsigned x, unsigned y, unsigned width)
{
    if (batch.failed || width == 0) return NULL;
    if (x >= SCREEN_W || width > SCREEN_W-x || y >= SCREEN_H) {
        batch.failed = true;
        return NULL;
    }
    batch.input = grow(batch.input, &batch.span_cap, batch.count+1, MAX_SPANS,
                       sizeof(compute_span));
    if (batch.failed) return NULL;
    compute_span *s = &batch.input[batch.count++];
    *s = batch.material;
    s->x_width = x | (width << 16);
    s->reserved = y; /* stable counting sort key */
    unsigned bin = y * BINS_PER_ROW;
    for (unsigned bx = x/BIN_WIDTH; bx <= (x+width-1)/BIN_WIDTH; bx++)
        batch.counts[bin+bx]++;
    return s;
}

void compute_rect(unsigned x, unsigned y, unsigned w, unsigned h, int u, int v,
                  uint32_t depth, uint32_t color)
{
    for (unsigned i = 0; i < h; i++) {
        compute_span *s = new_span(x, y+i, w);
        if (!s) return;
        s->flags |= 4;
        s->z = depth;
        s->red = (color & 255u) << 3;
        s->green = ((color >> 8) & 255u) << 3;
        s->blue = ((color >> 16) & 255u) << 3;
        s->uv = (uint16_t)u | ((uint32_t)(v+i) << 16);
    }
}

static uint32_t field(const uint8_t *row, unsigned n)
{
    uint32_t v;
    memcpy(&v, row + n * RECON_3D_ROW_ARRAY, 4);
    return v;
}

void compute_rows(const uint8_t *row, unsigned y, unsigned count, unsigned flags,
                  const gpu3d_bank_vertex_t *first)
{
    for (unsigned i = 0; i < count; i++, row += 4) {
        unsigned width = (uint16_t)field(row,9);
        if (!width) continue;
        uint32_t xb = field(row,8);
        compute_span *s = new_span((uint16_t)xb, y+i, width);
        if (!s) return;
        uint32_t rg = field(row,6), drg = field(row,7);
        s->red = (uint16_t)rg | (drg << 16);
        s->green = (rg >> 16) | (drg & 0xffff0000u);
        s->blue = (xb >> 16) | (field(row,9) & 0xffff0000u);
        if (flags & 4u) {
            s->red &= 65535u; s->green &= 65535u; s->blue &= 65535u;
        }
        if (batch.material.flags & 64u) s->reserved |= ((field(row,10) & 511u) << 8) |
                                 (((field(row,10) >> 16) & 511u) << 17);
        s->uv = field(row,4);
        s->duv = field(row,5);
        if (flags & 16u) {
            s->z = (flags & 8u) ? (uint32_t)first->w : (uint32_t)first->z << 9;
        } else if (flags & 8u) {
            s->flags |= 2;
            s->z = field(row,0);
            s->dz = field(row,1);
        } else {
            int32_t delta = (int32_t)field(row,3);
            int64_t step = (int64_t)delta * raster_reciprocal_30[width];
            if (delta < 0) step += 0x3fffffffu;
            s->z = field(row,2);
            s->dz = (uint32_t)(step >> 30);
            s->fraction = (uint32_t)step & 0x3fffffffu;
        }
        if (flags & 32u) {
            s->coef = raster_reciprocal_31[width];
        } else if (!(flags & 4u) || s->texture_base != UINT32_MAX || (s->flags & 2u)) {
            /* Match ARM reciprocal estimate and both refinement steps exactly.
             * GLES division rounding is implementation-dependent. */
            float numerator[SCREEN_W+16] = {0}, denominator[SCREEN_W+16] = {0};
            int16_t coef[SCREEN_W+16] = {0};
            gpu3d_raster_row_ramp_generate((int *)(uintptr_t)row, numerator, denominator, 1);
            gpu3d_raster_perspective_divide_fix15(coef, numerator, denominator, width);
            s->flags |= 1;
            unsigned words = (width+1)/2;
            batch.coefficients = grow(batch.coefficients, &batch.coef_cap,
                    batch.coef_words+words, MAX_WORDS/2, sizeof(uint32_t));
            if (batch.failed) return;
            s->coef = batch.coef_words;
            memcpy(batch.coefficients + batch.coef_words, coef, words*4);
            batch.coef_words += words;
        }
    }
}

void compute_start(bool verify)
{
    batch.enabled = true;
    batch.verify = verify;
}

void compute_stop(void)
{
    if (batch.ready) compute_gl_close();
    if (batch.rendered || batch.fallback)
        fprintf(stderr, "[SeedlessDS compute] %llu GPU frames, %llu CPU fallback, %llu compared\n",
            (unsigned long long)batch.rendered, (unsigned long long)batch.fallback,
            (unsigned long long)batch.compared);
    free(batch.input); free(batch.bins); free(batch.arena); free(batch.reference_candidate); free(batch.planes); free(batch.coefficients);
    memset(&batch, 0, sizeof(batch));
}

bool compute_frame(gpu_t *gpu, gpu3d_t *scene, unsigned bank, uint8_t *dst)
{
    if (!batch.enabled) return false;
    batch.compare = false;
    if (!dst || !scene->opaque[bank].count ||
        (gpu->raster.disp3dcnt & 0x4000u) || recon_scale_3d != 2 ||
        recon_block_header != GPU3D_BAND_HEADER_OFFSET)
        goto fallback;
    batch.count = batch.words = batch.coef_words = 0;
    batch.failed = false;
    memset(batch.counts, 0, sizeof(batch.counts));
    memset(batch.textures, 0, sizeof(batch.textures));
    const gpu3d_bank_vertex_t *v = scene->vertex_bank[bank].vertex;
    uint32_t toon[32];
    for (unsigned i = 0; i < 32; i++)
        toon[i] = scene->toon_table_expanded[0][i] |
                  ((uint32_t)scene->toon_table_expanded[1][i] << 8) |
                  ((uint32_t)scene->toon_table_expanded[2][i] << 16);
    append_words(toon, 32);
    for (unsigned b = 0; b < GPU3D_BANDS; b++) {
        const gpu3d_band_bucket_t *bucket = &gpu->bucket[0][b];
        for (unsigned j = 0; j < bucket->count; j++) {
            uint16_t id;
            memcpy(&id, bucket->list+j*2, 2);
            const gpu3d_polygon_t *p = &scene->opaque[bank].polygon[id];
            if (!material(p, gpu->raster.disp3dcnt)) goto fallback;
            gpu3d_compute_collect_edges(gpu->band_context[0], p, v, b*16, (b+1)*16);
            if (batch.failed) goto fallback;
        }
    }
    if (!batch.count) goto fallback;
    /* Sort small references, never duplicate the 64-byte span per bin.
     * Stable insertion preserves opaque/translucent order at equal depths. */
    unsigned total = BIN_COUNT + 1;
    for (unsigned i = 0; i < BIN_COUNT; i++) total += batch.counts[i];
    batch.bins = grow(batch.bins, &batch.bin_cap, total, 2*MAX_SPANS*BINS_PER_ROW,
                      sizeof(uint32_t));
    if (batch.failed) goto fallback;
    unsigned pos = BIN_COUNT + 1;
    for (unsigned i = 0; i < BIN_COUNT; i++) {
        batch.bins[i] = pos;
        pos += batch.counts[i];
        batch.counts[i] = batch.bins[i];
    }
    batch.bins[BIN_COUNT] = pos;
    for (unsigned i = 0; i < batch.count; i++) {
        const compute_span *s = &batch.input[i];
        unsigned x = (uint16_t)s->x_width, w = s->x_width >> 16;
        unsigned row = (s->reserved & 255u) * BINS_PER_ROW;
        for (unsigned bx = x/BIN_WIDTH; bx <= (x+w-1)/BIN_WIDTH; bx++)
            batch.bins[batch.counts[row+bx]++] = i;
    }
    if (!batch.ready) {
        if (!compute_gl_init()) { batch.enabled = false; goto fallback; }
        batch.ready = true;
    }
    if (batch.verify && !batch.reference_candidate)
        batch.reference_candidate = malloc(SCREEN_W * SCREEN_H * 4);
    if (batch.verify && !batch.reference_candidate) goto fallback;
    uint32_t pass;
    memcpy(&pass, (uint8_t *)gpu->vram.config + 1180, sizeof(pass));
    unsigned ctrl = gpu->raster.disp3dcnt;
    bool edges = (ctrl & 32u) && !pass;
    bool translucent = scene->translucent[bank].count != 0;
    bool effects = edges || (ctrl & 128u) || translucent;
    uint8_t *output = batch.verify ? (uint8_t *)batch.reference_candidate : dst;
    if (effects && !batch.planes) batch.planes = malloc(SCREEN_W*SCREEN_H*8);
    if (effects && !batch.planes) goto fallback;
    if (!compute_gl_draw(batch.input, batch.count, batch.bins, total, batch.arena, batch.words,
                         batch.dirty_first, batch.dirty_end,
                         batch.coefficients, batch.coef_words,
                         gpu->raster.clear_color_rgb, gpu->raster.clear_depth_word,
                         gpu->raster.alpha_test_ref, effects, effects ? (void *)batch.planes : output)) {
        batch.enabled = false;
        goto fallback;
    }
    batch.dirty_first = UINT32_MAX;
    batch.dirty_end = 0;
    /* The following GPU words can be transient coefficients, so only the
     * current texture prefix is known to survive the submission. */
    batch.known_words = batch.words;
    if (effects) {
        uint8_t *ctx = gpu->band_context[0];
        gpu3d_band_header_t *header = (gpu3d_band_header_t *)(ctx+GPU3D_BAND_HEADER_OFFSET);
        for (unsigned band = 0; band < GPU3D_BANDS; band++) {
            unsigned pos = band*4096;
            memcpy(ctx, batch.planes+pos, 4096*4);
            uint32_t *attrs = (uint32_t *)(ctx + GPU3D_BAND_PLANE_SIZE);
            uint32_t any = 0;
            for (unsigned i = 0; i < 4096; i++) {
                uint32_t attr = batch.planes[SCREEN_W*SCREEN_H + pos+i];
                any |= attr;
                attrs[i] = attr & 0x7fffffffu;
            }
            header->band_dirty = any >> 31;
            header->band_mark = UINT32_MAX;
            if (translucent) gpu3d_raster_native_translucent_band(ctx, band);
            gpu3d_raster_finish_native_band(ctx, output + pos*4, band, pass);
        }
        if (edges) {
            if (!(ctrl & 128u)) gpu3d_raster_band_flush_plain_to(gpu, output);
            else if (ctrl & 64u) gpu3d_raster_band_compose_set_alpha_to(gpu, output);
            else gpu3d_raster_band_flush_fogged_to(gpu, output);
        }
    }
    batch.rendered++;
    batch.compare = batch.verify;
    return !batch.verify;
fallback:
    batch.fallback++;
    return false;
}

void compute_check(const uint8_t *reference)
{
    if (!batch.compare) return;
    batch.compare = false;
    batch.compared++;
    for (unsigned i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t sw;
        memcpy(&sw, reference+i*4, 4);
        if (sw != batch.reference_candidate[i]) {
            fprintf(stderr, "[SeedlessDS compute] comparison %llu at (%u,%u): software %08x, GPU %08x; using software\n",
                (unsigned long long)batch.compared, i%SCREEN_W, i/SCREEN_W, sw, batch.reference_candidate[i]);
            batch.enabled = false;
            return;
        }
    }
}

/* Native 3D renderer differential test. Requires a working ES 3.1 context.
 * The texture fixtures start at the decoded texture-cache boundary. */
#include "compute.h"
#include "core_internals.h"
#include "hires_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t candidate[256*192], reference[256*192];
static uint32_t texels[256*256], palette[256];
static uint8_t indices[256*256];

enum { GOURAUD=1, VARY_Z=2, VARY_W=4, W_BUFFER=8, TEXTURE=16,
       INDEXED=32, DECAL=64, RECT=128, OVERLAP=256, EQUAL=512, TRANS=1024, BLEND=2048, WRITE_Z=4096,
       TOON=8192, HIGHLIGHT=16384, EDGES=32768, FOG=65536, FOG_ALPHA=131072, BORDER=262144 };

static double time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000.0 + t.tv_nsec / 1000.0;
}

static void draw_reference(gpu_t *gpu, unsigned flags)
{
    gpu3d_raster_band_draw_dispatch(gpu->band_context[0]);
    if (!(flags & EDGES)) return;
    if (!(flags & FOG)) gpu3d_raster_band_flush_plain(gpu);
    else if (flags & FOG_ALPHA) gpu3d_raster_band_compose_set_alpha(gpu);
    else gpu3d_raster_band_flush_fogged(gpu);
}

static bool run(gpu_t *gpu, const char *name, unsigned flags, unsigned repeats)
{
    gpu3d_t *scene = GPU3D_OF(gpu);
    gpu3d_polygon_t *polys = scene->opaque[0].polygon;
    gpu3d_bank_vertex_t *v = scene->vertex_bank[0].vertex;
    unsigned n = flags & RECT ? 4 : 3;
    unsigned polys_n = flags & OVERLAP ? 3 : 1;
    bool trans = (flags & TRANS) != 0;
    scene->opaque[0].count = trans ? 1 : polys_n;
    scene->translucent[0].count = trans ? polys_n-1 : 0;
    gpu->raster.disp3dcnt = (flags & HIGHLIGHT ? 2u : 0u) | (flags & BLEND ? 8u : 0u) |
                            (flags & EDGES ? 32u : 0u) | (flags & FOG ? 128u : 0u) |
                            (flags & FOG_ALPHA ? 64u : 0u);
    gpu->raster.frame_ready = 1;
    scene->swap_params_previous = flags & W_BUFFER ? 2 : 0;
    scene->vertex_bank[0].count = n*polys_n;
    gpu->raster.alpha_test_ref = 3;
    gpu3d_texture_entry_t texture = {
        .data = flags & INDEXED ? indices : (uint8_t *)texels,
        .palette = (uint8_t *)palette, .palette_count = flags & INDEXED ? 256 : 0,
        .width=256, .height=256, .format=flags & INDEXED ? 4 : 7
    };
    for (unsigned i=0; i<polys_n; i++) {
        unsigned x = (flags & BORDER ? 0 : 13) + i*7;
        unsigned y = (flags & BORDER ? 0 : 19) + i*3;
        gpu3d_bank_vertex_t *a = v+i*n;
        a[0] = (gpu3d_bank_vertex_t){.x=x,.y=y,.z=123-i,.w=4096,.color=0x271e - i*0x810,.s=16,.t=32};
        a[1]=a[2]=a[3]=a[0];
        if (flags & RECT) {
            a[1].x=a[2].x=x+177;
            a[2].y=a[3].y=y+122;
            a[1].s=a[2].s=a[0].s+177*16;
            a[2].t=a[3].t=a[0].t+122*16;
        } else {
            a[1].x=231; a[1].y=93; a[1].s=3900; a[1].t=300;
            a[2].x=105; a[2].y=flags & BORDER ? 192 : 173; a[2].s=(uint16_t)-900; a[2].t=4500;
        }
        if (flags & GOURAUD) { a[1].color=0x1532; a[2].color=0x6321; }
        if (flags & VARY_W) { a[1].w=7859; a[2].w=3084; }
        if (flags & VARY_Z) { a[1].z=900; a[2].z=60; }
        polys[i] = (gpu3d_polygon_t){
            .vertex_count=n, .first_vertex=i*n,
            .polygon_attr=0x1f0000 | (i<<24) | (flags & DECAL ? 16 : 0)
        };
        if (flags & TOON) polys[i].polygon_attr |= 32;
        if (flags & FOG) polys[i].polygon_attr |= 0x8000;
        if (trans && i) {
            polys[i].polygon_attr &= ~0x3f1f0000u;
            polys[i].polygon_attr |= 0x010d0000u;
            if (flags & WRITE_Z) polys[i].polygon_attr |= 1u<<11;
        }
        if ((flags & EQUAL) && i) {
            polys[i].polygon_attr |= 0x4000;
            for (unsigned j=0; j<n; j++) a[j].z=123;
        }
    }
    if (trans) memcpy(scene->translucent[0].polygon, polys+1, (polys_n-1)*sizeof(*polys));
    gpu3d_raster_band_bucket_classify(scene,gpu->bucket[1],&scene->translucent[0],v,1);
    gpu3d_raster_band_bucket_classify_by_depth(scene,gpu->bucket[0],&scene->opaque[0],v,0);
    if (flags & TEXTURE) {
        for (unsigned i=0; i<polys_n; i++) {
            gpu3d_polygon_t *p = trans && i ? &scene->translucent[0].polygon[i-1] : &polys[i];
            p->texture_param=(texture.format<<26)|(5u<<20)|(5u<<23)|(15u<<16);
            p->texture=&texture;
            p->vertex_count |= 0x200;
            if (flags & RECT)
                gpu3d_poly_detect_axis_rect_fastpath((uint8_t*)p,(uint8_t*)(v+i*n));
        }
    }
    if (!compute_frame(gpu,scene,0,(uint8_t*)candidate)) {
        fprintf(stderr,"%s could not dispatch\n",name);
        return false;
    }
    draw_reference(gpu, flags);
    unsigned bad=0;
    for (unsigned i=0; i<256*192; i++) {
        if (reference[i]==candidate[i]) continue;
        if (!bad) fprintf(stderr,"%s (%u,%u) CPU=%08x GPU=%08x\n",
                          name,i%256,i/256,reference[i],candidate[i]);
        bad++;
    }
    printf("%s: %u differing pixels\n",name,bad);
    if (bad || !repeats) return bad == 0;
    double cpu = 0, gl = 0;
    for (unsigned i = 0; i < repeats; i++) {
        double start = time_us();
        if (!compute_frame(gpu, scene, 0, (uint8_t *)candidate)) return false;
        gl += time_us() - start;
        start = time_us();
        draw_reference(gpu, flags);
        cpu += time_us() - start;
    }
    printf("  %u warm draws, mean microseconds: software %.1f, GLES %.1f\n",
           repeats, cpu/repeats, gl/repeats);
    return memcmp(reference, candidate, sizeof(reference)) == 0;
}

int main(int argc, char **argv)
{
    unsigned repeats = 0;
    if (argc == 3 && strcmp(argv[1], "--bench") == 0) {
        char *end;
        unsigned long n = strtoul(argv[2], &end, 10);
        if (*end == '\0' && n > 0 && n <= 100000) repeats = (unsigned)n;
    }
    if (argc != 1 && !repeats) {
        fprintf(stderr, "Usage: %s [--bench iterations]\n", argv[0]);
        return 2;
    }
    gpu_t *gpu=calloc(1,GPU_SIZE+GPU3D_SIZE);
    if (!gpu) return 1;
    static uint32_t config[512];
    gpu->vram.config = (void *)config;
    setvbuf(stdout,NULL,_IOLBF,0);
    gpu3d_t *scene=GPU3D_OF(gpu);
    scene->bank=1;
    recon_scale_3d_loaded=recon_native_loaded=1;
    gpu3d_band_header_t *hdr=GPU3D_BAND_HEADER(gpu->band_context[0]);
    hdr->gpu=gpu; hdr->gpu3d=scene; hdr->threads=1;
    gpu->raster.frame_front=(uint8_t*)reference;
    gpu->raster.clear_color_rgb=0x820a0307;
    gpu->raster.clear_depth_word=0xffffff;
    for (unsigned i=1;i<=512;i++) {
        raster_reciprocal_30[i]=(0x40000000u+i-1)/i;
        raster_reciprocal_31[i]=(0x80000000u+i-1)/i;
    }
    for (unsigned i=0;i<256;i++)
        palette[i]=((i%5 ? 31u : 0u)<<24)|((i&63)<<16)|((i/2&63)<<8)|(i/3&63);
    for (unsigned i=0;i<256*256;i++) {
        indices[i]=(uint8_t)(i*17+i/256);
        texels[i]=palette[indices[i]];
    }
    recon_arena_3d = (uint8_t *)gpu;
    for (unsigned i=0; i<32; i++) {
        for (unsigned c=0; c<3; c++) {
            scene->toon_table_expanded[c][i]=(i*3+c*9)&63;
            scene->edge_color_expanded[c][i&7]=(i*7+c*13)&63;
        }
        scene->fog_table[i]=30+i*2;
    }
    scene->fog_color=0x0d081322;
    compute_start(false);
    const struct { const char *name; unsigned flags; } cases[] = {
        {"flat triangle",0}, {"Gouraud",GOURAUD}, {"depth gradient",VARY_Z|OVERLAP},
        {"perspective colour",GOURAUD|VARY_W}, {"W depth",W_BUFFER|VARY_W|OVERLAP},
        {"equal depth",OVERLAP|EQUAL}, {"direct colour",TEXTURE},
        {"indexed affine",TEXTURE|INDEXED}, {"perspective decal",TEXTURE|DECAL|VARY_W},
        {"indexed rectangle",TEXTURE|INDEXED|RECT}, {"direct rectangle",TEXTURE|RECT},
        {"toon",TOON|GOURAUD}, {"highlight",TOON|HIGHLIGHT|GOURAUD},
        {"textured toon",TOON|TEXTURE|GOURAUD}, {"textured highlight",TOON|HIGHLIGHT|TEXTURE},
        {"translucent polygons",TRANS|OVERLAP|BLEND}, {"translucent depth write",TRANS|OVERLAP|WRITE_Z},
        {"translucent texture",TRANS|OVERLAP|BLEND|TEXTURE|INDEXED},
        {"translucent rectangles",TRANS|OVERLAP|RECT|TEXTURE|INDEXED|BLEND},
        {"edge marking",EDGES|OVERLAP},
        {"edges at screen border",EDGES|OVERLAP|BORDER},
        {"edges with textured rectangles",EDGES|OVERLAP|TEXTURE|RECT},
        {"edge marking with blend",EDGES|OVERLAP|TRANS|BLEND},
        {"colour fog",FOG|OVERLAP}, {"alpha fog",FOG|FOG_ALPHA|OVERLAP},
        {"edges and fog",EDGES|FOG|OVERLAP}, {"edges and alpha fog",EDGES|FOG|FOG_ALPHA|OVERLAP},
        {"mixed interpolation",TEXTURE|INDEXED|GOURAUD|VARY_W|VARY_Z|OVERLAP}
    };
    bool ok=true;
    for (unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++)
        ok=run(gpu,cases[i].name,cases[i].flags,repeats) && ok;
    compute_stop();
    free(gpu);
    return ok ? 0 : 1;
}

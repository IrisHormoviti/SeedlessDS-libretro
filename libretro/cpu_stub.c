/* Placeholder CPU / JIT symbols for non-AArch64 hosts.
 * Seedless-core currently only ships an AArch64 JIT; x86_64 cannot emulate. */

#include <stdint.h>
#include <stdio.h>
#include "seedlessds/cpu_backend.h"

static int stub_init(nds_t *m, arm_t *c)
{
    (void)m;
    (void)c;
    fprintf(stderr, "SeedlessDS-libretro: CPU JIT requires AArch64. "
                    "This host build cannot run games.\n");
    return -1;
}

static void stub_reset(arm_t *c) { (void)c; }
static uint32_t stub_run(arm_t *c, uint32_t cycles)
{
    (void)c;
    return cycles;
}
static void stub_invalidate(arm_t *c, uint32_t a, uint32_t n)
{
    (void)c;
    (void)a;
    (void)n;
}
static void stub_flush(arm_t *c) { (void)c; }
static void stub_irq(arm_t *c) { (void)c; }
static void stub_save(arm_t *c, state_stream_t *s)
{
    (void)c;
    (void)s;
}
static void stub_shutdown(arm_t *c) { (void)c; }

static const cpu_backend_t stub = {
    "stub-no-jit",
    stub_init,
    stub_reset,
    stub_run,
    stub_invalidate,
    stub_flush,
    stub_irq,
    stub_save,
    stub_shutdown,
};

const cpu_backend_t *nds_cpu_backend_interp(void)
{
    return &stub;
}

const cpu_backend_t *nds_cpu_backend_jit_arm64(void)
{
    return &stub;
}

const cpu_backend_t *nds_cpu_backend_default(void)
{
    return &stub;
}

/* Symbols normally provided by jit-arm64/cache.c + memhooks.S */

void jit_arena_protect_rwx(unsigned char *base)
{
    (void)base;
}

void jit_arena_regions_init(unsigned char *arena)
{
    (void)arena;
}

void *jit_cache_lookup_or_compile(void *ctx, uint32_t dir)
{
    (void)ctx;
    (void)dir;
    return NULL;
}

void jit_warm_cache_clear(void) {}
void jit_warm_cache_begin(unsigned char *machine) { (void)machine; }
void jit_warm_cache_preload(unsigned char *machine, int allow)
{
    (void)machine;
    (void)allow;
}
void jit_warm_cache_record(void *ctx, uint32_t pc)
{
    (void)ctx;
    (void)pc;
}
void jit_warm_cache_save(void) {}
void jit_native_cache_set_root(const char *path) { (void)path; }
void jit_native_cache_compile_begin(void *ctx, uint32_t pc)
{
    (void)ctx;
    (void)pc;
}
void jit_native_cache_compile_end(void *ctx, uint32_t pc, uint8_t *entry)
{
    (void)ctx;
    (void)pc;
    (void)entry;
}
void jit_native_cache_note_range(void *ctx, uint32_t address, uint32_t size)
{
    (void)ctx;
    (void)address;
    (void)size;
}
void jit_warm_cache_profile_read(uint64_t *loaded, uint64_t *signature_skips,
                                 uint64_t *already_compiled, uint64_t *candidates,
                                 uint64_t *compiled_blocks,
                                 uint64_t *native_blocks_restored)
{
    if (loaded) *loaded = 0;
    if (signature_skips) *signature_skips = 0;
    if (already_compiled) *already_compiled = 0;
    if (candidates) *candidates = 0;
    if (compiled_blocks) *compiled_blocks = 0;
    if (native_blocks_restored) *native_blocks_restored = 0;
}

void jit_profile_reset(void) {}

void jit_profile_read(uint64_t *blocks, uint64_t *analysis_us)
{
    if (blocks) *blocks = 0;
    if (analysis_us) *analysis_us = 0;
}

void jit_dispatch_profile_read(uint64_t *primary_misses, uint64_t *secondary_hits)
{
    if (primary_misses) *primary_misses = 0;
    if (secondary_hits) *secondary_hits = 0;
}

void jit_transfer_profile_read(uint64_t *ldm_calls, uint64_t *ldm_hits,
                               uint64_t *gxfifo_stm_calls, uint64_t *gxfifo_stm_hits)
{
    if (ldm_calls) *ldm_calls = 0;
    if (ldm_hits) *ldm_hits = 0;
    if (gxfifo_stm_calls) *gxfifo_stm_calls = 0;
    if (gxfifo_stm_hits) *gxfifo_stm_hits = 0;
}

void jit_cache_refresh_block_info(unsigned char *ctx)
{
    (void)ctx;
}

int32_t jit_cache_flush(uint8_t *ctx, uint32_t arg)
{
    (void)ctx;
    (void)arg;
    return 0;
}

uint32_t jit_watch_write8(unsigned char *machine, uint32_t dir)
{
    (void)machine;
    (void)dir;
    return 1;
}

uint32_t jit_watch_write16(unsigned char *machine, uint32_t dir)
{
    (void)machine;
    (void)dir;
    return 1;
}

uint32_t jit_watch_write32(unsigned char *machine, uint32_t dir)
{
    (void)machine;
    (void)dir;
    return 1;
}

/* memhooks.S tail-call into JIT dispatcher; unused without AArch64 JIT. */
void script_lua_register_dual_slot_tail(void *a0, uint64_t a1)
{
    (void)a0;
    (void)a1;
}

/* LZMA 7zCrc.c (x86) expects this from CpuArch.c, which we do not build. */
int CPU_Is_InOrder(void)
{
    return 1;
}

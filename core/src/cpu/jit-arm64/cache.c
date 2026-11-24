#include <stdint.h>
#include "jit_hooks.h"
#include "hires_runtime.h"
#include "blob_symbols.h"
#include "core/nds_state.h"
#define CPU(p) ((arm_t *)(p))
#define PT(p) (&CPU(p)->pagetable)
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include "core_internals.h"
#include "mem_access.h"
#include "seedlessds/platform.h"
#if defined(__linux__)
#include <sys/mman.h>
#endif

/* The primary way lives in arm_t for a single-load hot path. Keep the second
 * way outside arm_t because generated code and save-state layouts depend on
 * the structure offsets. One row per emulated CPU. */
uint64_t jit_block_lookup_secondary[2][ARM_BLOCK_LOOKUP]
    __attribute__((visibility("hidden"), aligned(64)));

static void *jit_block_lookup_cached(void *ctx, uint32_t pc);
static void flush_cache_range(const uint8_t *first, const uint8_t *last);
static uint64_t jit_native_mapping_signature(const nds_t *nds);
static uint64_t jit_native_hash_u64(uint64_t hash, uint64_t value);

#define JIT_WARM_CACHE_MAGIC 0x574a5344u
#define JIT_WARM_CACHE_VERSION 3u
#define JIT_WARM_CACHE_MAX_ENTRIES 256u
#define JIT_WARM_CACHE_PREWARM_ENTRIES JIT_WARM_CACHE_MAX_ENTRIES
#define JIT_WARM_CACHE_SIGNATURE_HALFWORDS 4u
#define JIT_NATIVE_CACHE_MAGIC 0x4e4a5344u
#define JIT_NATIVE_CACHE_VERSION 2u
#ifndef SEEDLESS_JIT_NATIVE_CACHE_ABI
#define SEEDLESS_JIT_NATIVE_CACHE_ABI UINT64_C(0)
#endif
#define JIT_NATIVE_CACHE_MAX_BLOCKS 0x8000u
#define JIT_NATIVE_CACHE_MAX_DEPENDENCIES 0x20000u
#define JIT_NATIVE_CACHE_MAX_BLOCK_DEPENDENCIES 16u
#define JIT_NATIVE_CACHE_COMPILE_DEPTH 64u

typedef struct jit_warm_cache_entry {
    uint32_t pc;
    uint32_t cpu;
    uint32_t signature;
    uint32_t samples;
} jit_warm_cache_entry_t;

typedef struct jit_warm_cache_header {
    uint32_t magic;
    uint32_t version;
    uint32_t game_code;
    uint32_t rom_size;
    uint32_t header_crc;
    uint32_t count;
    uint32_t entries_crc;
} jit_warm_cache_header_t;

/* Native blocks contain relative branches into the core and arena. Restore is
 * allowed only for this exact emitter ABI, ROM, and a complete set of guest
 * instruction signatures. Keep metadata separate from the arena so pointers
 * can be rebased and all guest/JIT invalidation maps can be restored. */
typedef struct jit_native_cache_header {
    uint32_t magic;
    uint32_t version;
    uint64_t abi;
    uint32_t game_code;
    uint32_t rom_size;
    uint32_t header_crc;
    uint32_t block_count;
    uint32_t dependency_count;
    uint32_t arena_size;
    uint32_t aux_size;
    uint32_t payload_crc;
    uint64_t mapping_signature;
} jit_native_cache_header_t;

typedef struct jit_native_cache_block {
    uint32_t pc;
    uint32_t dependency_first;
    uint16_t halfwords;
    uint8_t cpu;
    uint8_t dependency_count;
    uint64_t signature;
} jit_native_cache_block_t;

typedef struct jit_native_cache_dependency {
    uint32_t address;
    uint32_t size;
    uint64_t signature;
} jit_native_cache_dependency_t;

typedef struct jit_native_compile_frame {
    void *cpu;
    uint32_t pc;
    uint32_t dependency_count;
    struct {
        uint32_t address;
        uint32_t size;
    } dependencies[JIT_NATIVE_CACHE_MAX_BLOCK_DEPENDENCIES];
} jit_native_compile_frame_t;

typedef struct jit_native_cache_aux {
    uint32_t arm9_itcm_arm[ARM_ITCM_ARM_BLOCKS];
    uint32_t arm9_itcm_thumb[ARM_ITCM_THUMB_BLOCKS];
    uint32_t arm9_page_bits[0x1000];
    uint32_t arm9_group_bits[0x80];
    uint32_t arm7_page_bits[0x1000];
    uint32_t arm7_group_bits[0x80];
    uint32_t main_ram_page_bits[0x40];
    uint32_t itcm_page_bits;
    uint32_t shared_wram_page_bits;
    uint32_t arm7_wram_page_bits;
    uint32_t vram_page_bits[BUS_VRAM_BITMAP_WORDS];
    uint8_t main_ram_code_bits[0x40000];
    uint32_t itcm_code_bits[0x200];
    uint8_t shared_wram_code_bits[0x800];
    uint8_t arm7_wram_code_bits[0x1000];
    uint8_t vram_code_bits[0xa400];
} jit_native_cache_aux_t;

static jit_warm_cache_entry_t jit_warm_cache_entries[JIT_WARM_CACHE_MAX_ENTRIES];
static jit_warm_cache_entry_t jit_warm_cache_preload_entries[JIT_WARM_CACHE_PREWARM_ENTRIES];
static uint32_t jit_warm_cache_count;
static uint32_t jit_warm_cache_preload_count;
static uint32_t jit_warm_cache_game_code;
static uint32_t jit_warm_cache_rom_size;
static uint32_t jit_warm_cache_header_crc;
static uint8_t jit_warm_cache_enabled;
static char jit_warm_cache_path[0x410];
static char jit_native_cache_path[0x410];
static jit_native_cache_block_t jit_native_cache_blocks[JIT_NATIVE_CACHE_MAX_BLOCKS];
static jit_native_cache_dependency_t
    jit_native_cache_dependencies[JIT_NATIVE_CACHE_MAX_DEPENDENCIES];
static jit_native_compile_frame_t
    jit_native_compile_frames[JIT_NATIVE_CACHE_COMPILE_DEPTH];
static uint32_t jit_native_cache_block_count;
static uint32_t jit_native_cache_dependency_count;
static uint32_t jit_native_compile_depth;
static uint32_t jit_native_compile_overflow_depth;
static uint8_t jit_native_cache_complete;
static uint64_t jit_native_cache_initial_mapping_signature;
#if defined(SEEDLESS_JIT_PROFILE)
static uint64_t jit_warm_profile_loaded;
static uint64_t jit_warm_profile_signature_skips;
static uint64_t jit_warm_profile_already_compiled;
static uint64_t jit_warm_profile_candidates;
static uint64_t jit_warm_profile_compiled_blocks;
static uint64_t jit_warm_profile_native_blocks_restored;
static uint8_t jit_warm_profile_preloading;
#endif

void jit_warm_cache_clear(void)
{
    jit_warm_cache_enabled = 0;
    jit_warm_cache_count = 0;
    jit_warm_cache_preload_count = 0;
    jit_warm_cache_path[0] = 0;
    jit_native_cache_path[0] = 0;
    jit_native_cache_block_count = 0;
    jit_native_cache_dependency_count = 0;
    jit_native_compile_depth = 0;
    jit_native_compile_overflow_depth = 0;
    jit_native_cache_complete = 0;
    jit_native_cache_initial_mapping_signature = 0;
#if defined(SEEDLESS_JIT_PROFILE)
    jit_warm_profile_loaded = 0;
    jit_warm_profile_signature_skips = 0;
    jit_warm_profile_already_compiled = 0;
    jit_warm_profile_candidates = 0;
    jit_warm_profile_compiled_blocks = 0;
    jit_warm_profile_native_blocks_restored = 0;
    jit_warm_profile_preloading = 0;
#endif
}

static uint32_t jit_warm_cache_signature(void *ctx, uint32_t pc)
{
    pagetable_t *table = PT(ctx);
    uint32_t address = pc & ~1u;
    uint32_t signature = 2166136261u;
    for (uint32_t i = 0; i < JIT_WARM_CACHE_SIGNATURE_HALFWORDS; i++) {
        uint32_t at = address + i * 2u;
        uint64_t page = table->page[at >> PAGETABLE_PAGE_SHIFT];
        uint16_t halfword = (page == 0 || page == PAGETABLE_MISSING)
            ? (uint16_t)bus_read16((unsigned char *)table, at)
            : *(const uint16_t *)(uintptr_t)((page << 2) + at);
        signature = (signature ^ (uint8_t)halfword) * 16777619u;
        signature = (signature ^ (uint8_t)(halfword >> 8)) * 16777619u;
    }
    return signature;
}

static uint32_t jit_warm_cache_entries_crc(const jit_warm_cache_entry_t *entries,
                                           uint32_t count)
{
    uint32_t crc = 2166136261u;
    const uint8_t *bytes = (const uint8_t *)entries;
    for (uint32_t i = 0; i < count * (uint32_t)sizeof(*entries); i++)
        crc = (crc ^ bytes[i]) * 16777619u;
    return crc;
}

static uint32_t jit_native_cache_crc(const void *data, size_t size)
{
    uint32_t crc = 2166136261u;
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++)
        crc = (crc ^ bytes[i]) * 16777619u;
    return crc;
}

static uint64_t jit_native_guest_signature(void *ctx, uint32_t pc,
                                           uint16_t halfwords)
{
    pagetable_t *table = PT(ctx);
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t address = pc & ~1u;
    for (uint32_t i = 0; i < halfwords; i++) {
        uint32_t at = address + i * 2u;
        uint64_t page = table->page[at >> PAGETABLE_PAGE_SHIFT];
        uint64_t base = page << 2;
        uint16_t halfword = (page == 0 || page == PAGETABLE_MISSING)
            ? (uint16_t)bus_read16((unsigned char *)table, at)
            : *(const uint16_t *)(uintptr_t)(base + at);
        hash = (hash ^ (uint8_t)halfword) * UINT64_C(1099511628211);
        hash = (hash ^ (uint8_t)(halfword >> 8)) * UINT64_C(1099511628211);
    }
    return hash;
}

static int jit_native_dependency_supported(void *ctx, uint32_t address,
                                           uint32_t size)
{
    if (size == 0 || size > 0x10000u || address + size < address)
        return 0;
    pagetable_t *table = PT(ctx);
    uint32_t end = address + size - 1;
    if ((address >> 28) != 0) {
        uint32_t first_page = address >> PAGETABLE_PAGE_SHIFT;
        uint32_t last_page = end >> PAGETABLE_PAGE_SHIFT;
        for (uint32_t page = first_page; page <= last_page; page++)
            if (table->page[page] == 0 ||
                table->page[page] == PAGETABLE_MISSING)
                return 0;
        return 1;
    }
    for (uint32_t at = address;;) {
        uint32_t index = at >> BUS_REGION_SHIFT;
        if (index >= BUS_ARM9_REGIONS ||
            table->region[index].read_kind == BUS_REGION_HANDLED)
            return 0;
        if (table->region[index].read_kind == BUS_REGION_IGNORED) {
            uint32_t first_page = address >> PAGETABLE_PAGE_SHIFT;
            uint32_t last_page = end >> PAGETABLE_PAGE_SHIFT;
            for (uint32_t page = first_page; page <= last_page; page++)
                if (table->page[page] == 0 ||
                    table->page[page] == PAGETABLE_MISSING)
                    return 0;
            return 1;
        }
        uint32_t next = ((at >> BUS_REGION_SHIFT) + 1u) << BUS_REGION_SHIFT;
        if (next == 0 || next > end)
            break;
        at = next;
    }
    return 1;
}

static uint64_t jit_native_memory_signature(void *ctx, uint32_t address,
                                             uint32_t size)
{
    pagetable_t *table = PT(ctx);
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = jit_native_hash_u64(hash, address);
    hash = jit_native_hash_u64(hash, size);
    for (uint32_t i = 0; i < size; i++) {
        uint32_t at = address + i;
        uint64_t page = table->page[at >> PAGETABLE_PAGE_SHIFT];
        uint64_t base = page << 2;
        uint8_t value = (page == 0 || page == PAGETABLE_MISSING)
            ? (uint8_t)bus_read8((unsigned char *)table, at)
            : *(const uint8_t *)(uintptr_t)(base + at);
        hash = (hash ^ value) * UINT64_C(1099511628211);
    }
    return hash;
}

void jit_native_cache_compile_begin(void *ctx, uint32_t pc)
{
    if (!jit_warm_cache_enabled)
        return;
    if (jit_native_compile_overflow_depth != 0) {
        jit_native_compile_overflow_depth++;
        jit_native_cache_complete = 0;
        return;
    }
    if (jit_native_compile_depth == JIT_NATIVE_CACHE_COMPILE_DEPTH) {
        jit_native_compile_overflow_depth = 1;
        jit_native_cache_complete = 0;
        return;
    }
    jit_native_compile_frame_t *frame =
        &jit_native_compile_frames[jit_native_compile_depth++];
    frame->cpu = ctx;
    frame->pc = pc;
    frame->dependency_count = 0;
}

void jit_native_cache_note_range(void *ctx, uint32_t address, uint32_t size)
{
    if (!jit_warm_cache_enabled || jit_native_compile_overflow_depth != 0 ||
        jit_native_compile_depth == 0)
        return;
    jit_native_compile_frame_t *frame =
        &jit_native_compile_frames[jit_native_compile_depth - 1];
    if (frame->cpu != ctx)
        return;
    if (!jit_native_dependency_supported(ctx, address, size)) {
        jit_native_cache_complete = 0;
        return;
    }
    for (uint32_t i = 0; i < frame->dependency_count; i++) {
        if (frame->dependencies[i].address == address &&
            frame->dependencies[i].size == size)
            return;
    }
    if (frame->dependency_count == JIT_NATIVE_CACHE_MAX_BLOCK_DEPENDENCIES) {
        jit_native_cache_complete = 0;
        return;
    }
    frame->dependencies[frame->dependency_count].address = address;
    frame->dependencies[frame->dependency_count].size = size;
    frame->dependency_count++;
}

static void jit_native_cache_record(void *ctx, uint32_t pc, uint8_t *entry,
                                    const jit_native_compile_frame_t *frame)
{
    if (!jit_warm_cache_enabled || !ctx || !entry || !jit_native_cache_complete)
        return;

    jit_arena_t *arena = CPU(ctx)->jit_arena;
    uint8_t *base = (uint8_t *)arena;
    uintptr_t entry_address = (uintptr_t)entry;
    uintptr_t arena_address = (uintptr_t)base;
    if (entry_address < arena_address + sizeof(uint32_t) ||
        entry_address >= arena_address + sizeof(*arena)) {
        jit_native_cache_complete = 0;
        return;
    }
    uint32_t header_offset = rd32(entry - sizeof(uint32_t));
    if (header_offset > sizeof(*arena) - sizeof(jit_block_header_t)) {
        jit_native_cache_complete = 0;
        return;
    }
    const jit_block_header_t *header =
        (const jit_block_header_t *)(base + header_offset);
    uint16_t halfwords = header->flags & JIT_BLOCK_HEADER_HALFWORDS_MASK;
    if (!halfwords || (header->pc & ~1u) != (pc & ~1u)) {
        jit_native_cache_complete = 0;
        return;
    }
    if (!frame || frame->dependency_count == 0 ||
        jit_native_cache_dependency_count + frame->dependency_count >
            JIT_NATIVE_CACHE_MAX_DEPENDENCIES) {
        jit_native_cache_complete = 0;
        return;
    }

    uint8_t cpu = CPU(ctx)->is_arm9 != 0;
    uint32_t i = 0;
    for (; i < jit_native_cache_block_count; i++) {
        jit_native_cache_block_t *block = &jit_native_cache_blocks[i];
        if (block->pc == pc && block->cpu == cpu)
            break;
    }
    if (i == JIT_NATIVE_CACHE_MAX_BLOCKS) {
        jit_native_cache_complete = 0;
        return;
    }
    uint32_t first_dependency = jit_native_cache_dependency_count;
    for (uint32_t d = 0; d < frame->dependency_count; d++) {
        uint32_t address = frame->dependencies[d].address;
        uint32_t size = frame->dependencies[d].size;
        jit_native_cache_dependencies[jit_native_cache_dependency_count++] =
            (jit_native_cache_dependency_t){
                address, size, jit_native_memory_signature(ctx, address, size)
            };
    }
    if (i == jit_native_cache_block_count)
        jit_native_cache_block_count++;
    jit_native_cache_blocks[i] = (jit_native_cache_block_t){
        pc, first_dependency, halfwords, cpu,
        (uint8_t)frame->dependency_count,
        jit_native_guest_signature(ctx, pc, halfwords)
    };
}

void jit_native_cache_compile_end(void *ctx, uint32_t pc, uint8_t *entry)
{
    if (!jit_warm_cache_enabled)
        return;
    if (jit_native_compile_overflow_depth != 0) {
        jit_native_compile_overflow_depth--;
        return;
    }
    if (jit_native_compile_depth == 0) {
        jit_native_cache_complete = 0;
        return;
    }
    jit_native_compile_frame_t frame =
        jit_native_compile_frames[--jit_native_compile_depth];
    if (frame.cpu != ctx || frame.pc != pc) {
        jit_native_cache_complete = 0;
        return;
    }
    if (entry && jit_native_cache_complete)
        jit_native_cache_record(ctx, pc, entry, &frame);
}

static void jit_native_cache_capture_aux(nds_t *nds, jit_native_cache_aux_t *aux)
{
    bus_t *bus = &nds->bus;
    memcpy(aux->arm9_itcm_arm, nds->arm9.itcm_arm_blocks,
           sizeof aux->arm9_itcm_arm);
    memcpy(aux->arm9_itcm_thumb, nds->arm9.itcm_thumb_blocks,
           sizeof aux->arm9_itcm_thumb);
    memcpy(aux->arm9_page_bits, nds->arm9.pagetable.page_bits,
           sizeof aux->arm9_page_bits);
    memcpy(aux->arm9_group_bits, nds->arm9.pagetable.group_bits,
           sizeof aux->arm9_group_bits);
    memcpy(aux->arm7_page_bits, nds->arm7.pagetable.page_bits,
           sizeof aux->arm7_page_bits);
    memcpy(aux->arm7_group_bits, nds->arm7.pagetable.group_bits,
           sizeof aux->arm7_group_bits);
    memcpy(aux->main_ram_page_bits, bus->main_ram_page_bits,
           sizeof aux->main_ram_page_bits);
    aux->itcm_page_bits = bus->itcm_page_bits;
    aux->shared_wram_page_bits = bus->shared_wram_page_bits;
    aux->arm7_wram_page_bits = bus->arm7_wram_page_bits;
    memcpy(aux->vram_page_bits, bus->vram_page_bits,
           sizeof aux->vram_page_bits);
    memcpy(aux->main_ram_code_bits, bus->main_ram_code_bits,
           sizeof aux->main_ram_code_bits);
    memcpy(aux->itcm_code_bits, bus->itcm_code_bits,
           sizeof aux->itcm_code_bits);
    memcpy(aux->shared_wram_code_bits, bus->shared_wram_code_bits,
           sizeof aux->shared_wram_code_bits);
    memcpy(aux->arm7_wram_code_bits, bus->arm7_wram_code_bits,
           sizeof aux->arm7_wram_code_bits);
    memcpy(aux->vram_code_bits, bus->vram_code_bits,
           sizeof aux->vram_code_bits);
}

static void jit_native_cache_apply_aux(nds_t *nds,
                                       const jit_native_cache_aux_t *aux)
{
    bus_t *bus = &nds->bus;
    memcpy(nds->arm9.itcm_arm_blocks, aux->arm9_itcm_arm,
           sizeof aux->arm9_itcm_arm);
    memcpy(nds->arm9.itcm_thumb_blocks, aux->arm9_itcm_thumb,
           sizeof aux->arm9_itcm_thumb);
    memcpy(nds->arm9.pagetable.page_bits, aux->arm9_page_bits,
           sizeof aux->arm9_page_bits);
    memcpy(nds->arm9.pagetable.group_bits, aux->arm9_group_bits,
           sizeof aux->arm9_group_bits);
    memcpy(nds->arm7.pagetable.page_bits, aux->arm7_page_bits,
           sizeof aux->arm7_page_bits);
    memcpy(nds->arm7.pagetable.group_bits, aux->arm7_group_bits,
           sizeof aux->arm7_group_bits);
    memcpy(bus->main_ram_page_bits, aux->main_ram_page_bits,
           sizeof aux->main_ram_page_bits);
    bus->itcm_page_bits = aux->itcm_page_bits;
    bus->shared_wram_page_bits = aux->shared_wram_page_bits;
    bus->arm7_wram_page_bits = aux->arm7_wram_page_bits;
    memcpy(bus->vram_page_bits, aux->vram_page_bits,
           sizeof aux->vram_page_bits);
    memcpy(bus->main_ram_code_bits, aux->main_ram_code_bits,
           sizeof aux->main_ram_code_bits);
    memcpy(bus->itcm_code_bits, aux->itcm_code_bits,
           sizeof aux->itcm_code_bits);
    memcpy(bus->shared_wram_code_bits, aux->shared_wram_code_bits,
           sizeof aux->shared_wram_code_bits);
    memcpy(bus->arm7_wram_code_bits, aux->arm7_wram_code_bits,
           sizeof aux->arm7_wram_code_bits);
    memcpy(bus->vram_code_bits, aux->vram_code_bits,
           sizeof aux->vram_code_bits);
}

static int jit_native_cache_encode_pointer(const uint8_t *base,
                                           const uint8_t *pointer,
                                           uint8_t **encoded)
{
    uintptr_t start = (uintptr_t)base;
    uintptr_t value = (uintptr_t)pointer;
    if (value < start || value - start > sizeof(jit_arena_t))
        return 0;
    *encoded = (uint8_t *)(uintptr_t)(value - start);
    return 1;
}

static uint64_t jit_native_hash_u64(uint64_t hash, uint64_t value)
{
    for (unsigned i = 0; i < sizeof(value); i++) {
        hash = (hash ^ (uint8_t)value) * UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

static int jit_native_normalize_data_pointer(const nds_t *nds, uintptr_t pointer,
                                             uint64_t *normalized)
{
    const bus_t *bus = &nds->bus;
    const struct {
        uintptr_t base;
        size_t size;
        uint32_t tag;
    } ranges[] = {
        { (uintptr_t)nds, NDS_MACHINE_SIZE, 1 },
        { (uintptr_t)bus->shared_region, 0x4000000u, 2 },
        { (uintptr_t)bus->address_window, BUS_ADDRESS_WINDOW_BYTES, 3 },
        { (uintptr_t)bus->vram_region, 0x800000u, 4 },
        { (uintptr_t)bus->vram_window, 0x800000u, 5 }
    };
    if (pointer == 0) {
        *normalized = 0;
        return 1;
    }
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        if (!ranges[i].base || pointer < ranges[i].base ||
            pointer - ranges[i].base >= ranges[i].size)
            continue;
        *normalized = ((uint64_t)ranges[i].tag << 56) |
                      (uint64_t)(pointer - ranges[i].base);
        return 1;
    }
    return 0;
}

static int jit_native_hash_region_pointer(const nds_t *nds, uint64_t *hash,
                                          uintptr_t pointer, int is_code)
{
    uint64_t normalized;
    if (!pointer) {
        *hash = jit_native_hash_u64(*hash, 0);
        return 1;
    }
    if (is_code) {
        normalized = pointer - (uintptr_t)&jit_native_mapping_signature;
    } else if (!jit_native_normalize_data_pointer(nds, pointer, &normalized)) {
        return 0;
    }
    *hash = jit_native_hash_u64(*hash, normalized);
    return 1;
}

static uint64_t jit_native_mapping_signature(const nds_t *nds)
{
    const bus_t *bus = &nds->bus;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < BUS_REGIONS; i++) {
        const bus_region_t *region = &bus->region[i];
        hash = jit_native_hash_u64(hash, region->mask);
        hash = jit_native_hash_u64(hash, region->read_kind);
        hash = jit_native_hash_u64(hash, region->write_kind);
        if (!jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->read8, region->read_kind != BUS_REGION_DIRECT) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->read16, 1) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->read32, 1) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->write8, region->write_kind != BUS_REGION_DIRECT) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->write16, 1) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->write32, 1) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->page_bits, 0) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->code_bits, 0) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->bitmap_lookup[0], 1) ||
            !jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)region->bitmap_lookup[1], 1))
            return 0;
    }
    for (uint32_t i = 0; i < VRAM_BANKS; i++)
        if (!jit_native_hash_region_pointer(nds, &hash,
                (uintptr_t)bus->vram_bank[i], 0))
            return 0;
    for (uint32_t i = 0; i < 2; i++)
        for (uint32_t j = 0; j < 2; j++)
            if (!jit_native_hash_region_pointer(nds, &hash,
                    (uintptr_t)bus->wram_window[i][j], 0))
                return 0;
    hash = jit_native_hash_u64(hash, bus->dtcm_start);
    hash = jit_native_hash_u64(hash, bus->dtcm_end);
    hash = jit_native_hash_u64(hash, bus->itcm_size);
    hash = jit_native_hash_u64(hash, nds->arm9.cp15->dtcm_below_64mb);
    return hash ? hash : 1;
}

static int jit_native_cache_try_restore(nds_t *nds)
{
    if (!jit_warm_cache_enabled || !jit_native_cache_path[0] ||
        nds->runtime.jit_enabled == 0 || nds->bus.slot2_rom ||
        nds->bus.slot2_page_bits || nds->bus.slot2_code_bits ||
        !jit_native_cache_initial_mapping_signature)
        return 0;

    size_t (*read_file)(void *, size_t, size_t, void *) =
        (size_t (*)(void *, size_t, size_t, void *))sym_libc_fread;
    int (*close_file)(void *) = (int (*)(void *))sym_libc_fclose;
    void *file = nds_platform_default()->files.open(
        nds_platform_default()->user, jit_native_cache_path, "rb");
    if (!file)
        return 0;

    jit_native_cache_header_t header;
    int ok = 0;
    if (read_file(&header, sizeof header, 1, file) != 1 ||
        header.magic != JIT_NATIVE_CACHE_MAGIC ||
        header.version != JIT_NATIVE_CACHE_VERSION ||
        header.abi != SEEDLESS_JIT_NATIVE_CACHE_ABI ||
        header.game_code != jit_warm_cache_game_code ||
        header.rom_size != jit_warm_cache_rom_size ||
        header.header_crc != jit_warm_cache_header_crc ||
        header.mapping_signature != jit_native_cache_initial_mapping_signature ||
        header.arena_size != sizeof(jit_arena_t) ||
        header.aux_size != sizeof(jit_native_cache_aux_t) ||
        header.block_count == 0 ||
        header.block_count > JIT_NATIVE_CACHE_MAX_BLOCKS ||
        header.dependency_count == 0 ||
        header.dependency_count > JIT_NATIVE_CACHE_MAX_DEPENDENCIES)
        goto done;

    size_t blocks_size = (size_t)header.block_count * sizeof(jit_native_cache_block_t);
    size_t dependencies_size = (size_t)header.dependency_count *
                               sizeof(jit_native_cache_dependency_t);
    size_t payload_size = sizeof(jit_arena_t) + sizeof(jit_native_cache_aux_t) +
                          blocks_size + dependencies_size;
    if (payload_size != header.arena_size + header.aux_size + blocks_size +
                        dependencies_size)
        goto done;

    typedef void *(*fn_malloc)(size_t);
    typedef void (*fn_free)(void *);
    fn_malloc allocate = (fn_malloc)sym_libc_malloc;
    fn_free release = (fn_free)sym_libc_free;
    uint8_t *payload = (uint8_t *)allocate(payload_size);
    if (!payload)
        goto done;
    if (read_file(payload, 1, payload_size, file) != payload_size ||
        jit_native_cache_crc(payload, payload_size) != header.payload_crc) {
        release(payload);
        goto done;
    }

    jit_arena_t *saved = (jit_arena_t *)payload;
    jit_native_cache_aux_t *aux =
        (jit_native_cache_aux_t *)(payload + sizeof(*saved));
    jit_native_cache_block_t *blocks =
        (jit_native_cache_block_t *)((uint8_t *)aux + sizeof(*aux));
    jit_native_cache_dependency_t *dependencies =
        (jit_native_cache_dependency_t *)((uint8_t *)blocks + blocks_size);

    /* Validate source bytes and all saved offsets before changing live state.
     * One mismatch rejects the complete snapshot and leaves normal JIT intact. */
    for (uint32_t i = 0; i < header.block_count; i++) {
        const jit_native_cache_block_t *block = &blocks[i];
        arm_t *cpu;
        if (block->cpu > 1 || block->halfwords == 0 ||
            block->halfwords > JIT_BLOCK_HEADER_HALFWORDS_MASK) {
            release(payload);
            goto done;
        }
        cpu = block->cpu ? &nds->arm9 : &nds->arm7;
        if (jit_native_guest_signature(cpu, block->pc, block->halfwords) !=
            block->signature) {
            release(payload);
            goto done;
        }
        if (block->dependency_count == 0 ||
            block->dependency_count > JIT_NATIVE_CACHE_MAX_BLOCK_DEPENDENCIES ||
            block->dependency_first > header.dependency_count ||
            block->dependency_count > header.dependency_count -
                                      block->dependency_first) {
            release(payload);
            goto done;
        }
        for (uint32_t d = 0; d < block->dependency_count; d++) {
            const jit_native_cache_dependency_t *dependency =
                &dependencies[block->dependency_first + d];
            if (!jit_native_dependency_supported(cpu, dependency->address,
                                                  dependency->size) ||
                jit_native_memory_signature(cpu, dependency->address,
                                             dependency->size) !=
                    dependency->signature) {
                release(payload);
                goto done;
            }
        }
    }

    for (uint32_t i = 0; i < JIT_REGIONS; i++) {
        uintptr_t code = (uintptr_t)saved->region[i].code;
        uintptr_t table = (uintptr_t)saved->region[i].table;
        if (code > sizeof(*saved) || table > sizeof(*saved)) {
            release(payload);
            goto done;
        }
    }
    if ((uintptr_t)saved->record_cursor > sizeof(*saved) ||
        saved->itcm_variant_count > JIT_ITCM_VARIANTS) {
        release(payload);
        goto done;
    }
    for (uint32_t i = 0; i < saved->itcm_variant_count; i++) {
        uintptr_t block = (uintptr_t)saved->itcm_variant[i].block;
        if (block > sizeof(*saved)) {
            release(payload);
            goto done;
        }
    }

    jit_arena_t *live = &nds->jit_arena;
    memcpy(live, saved, sizeof(*live));
    uint8_t *base = (uint8_t *)live;
    for (uint32_t i = 0; i < JIT_REGIONS; i++) {
        live->region[i].code = base + (uintptr_t)live->region[i].code;
        live->region[i].table = base + (uintptr_t)live->region[i].table;
    }
    live->record_cursor = live->records;
    memset(live->records, 0, sizeof live->records);
    memset(live->pending_branch, 0, sizeof live->pending_branch);
    live->pending_branch_count = 0;
    live->block_pc_count = 0;
    for (uint32_t i = 0; i < live->itcm_variant_count; i++) {
        uintptr_t offset = (uintptr_t)live->itcm_variant[i].block;
        live->itcm_variant[i].block = offset ? base + offset : NULL;
    }

    jit_native_cache_apply_aux(nds, aux);
    memcpy(jit_native_cache_blocks, blocks, blocks_size);
    jit_native_cache_block_count = header.block_count;
    memcpy(jit_native_cache_dependencies, dependencies, dependencies_size);
    jit_native_cache_dependency_count = header.dependency_count;
    for (uint32_t i = 0; i < header.block_count; i++) {
        const jit_native_cache_block_t *block = &blocks[i];
        arm_t *cpu = block->cpu ? &nds->arm9 : &nds->arm7;
        for (uint32_t d = 0; d < block->dependency_count; d++) {
            const jit_native_cache_dependency_t *dependency =
                &dependencies[block->dependency_first + d];
            bus_range_mark_written((unsigned char *)cpu, dependency->address,
                                   dependency->size);
        }
    }
#if defined(SEEDLESS_JIT_PROFILE)
    jit_warm_profile_native_blocks_restored = header.block_count;
#endif
    memset(nds->arm9.block_lookup, 0, sizeof nds->arm9.block_lookup);
    memset(nds->arm7.block_lookup, 0, sizeof nds->arm7.block_lookup);
    memset(jit_block_lookup_secondary, 0, sizeof jit_block_lookup_secondary);

    flush_cache_range(live->code_main_ram, live->region[JIT_REGION_MAIN_RAM].code);
    flush_cache_range(live->code_main_ram + JIT_CODE_MAIN_RAM_SIZE,
                      live->region[JIT_REGION_MAIN_RAM].table);
    flush_cache_range(live->code_other, live->region[JIT_REGION_OTHER].code);
    flush_cache_range(live->code_other + JIT_CODE_OTHER_SIZE,
                      live->region[JIT_REGION_OTHER].table);
    flush_cache_range(live->code_itcm, live->region[JIT_REGION_ITCM].code);
    flush_cache_range(live->code_itcm + JIT_CODE_ITCM_SIZE,
                      live->region[JIT_REGION_ITCM].table);

    nds->arm9.jit_block =
        (uint8_t *)jit_cache_lookup_or_compile(&nds->arm9, nds->arm9.pc) + 8;
    nds->arm7.jit_block =
        (uint8_t *)jit_cache_lookup_or_compile(&nds->arm7, nds->arm7.pc) + 8;
    nds->arm9.jit_swi_entry = (uint8_t *)jit_cache_lookup_or_compile(
        &nds->arm9, nds->arm9.cp15->exception_vector_base + 8);
    nds->arm9.jit_irq_entry = (uint8_t *)jit_cache_lookup_or_compile(
        &nds->arm9, nds->arm9.cp15->exception_vector_base + 0x18);
    nds->arm7.jit_swi_entry =
        (uint8_t *)jit_cache_lookup_or_compile(&nds->arm7, 8);
    nds->arm7.jit_irq_entry =
        (uint8_t *)jit_cache_lookup_or_compile(&nds->arm7, 0x18);
    ok = 1;
    release(payload);
done:
    close_file(file);
    return ok;
}

static void jit_native_cache_save(nds_t *nds)
{
    if (!jit_warm_cache_enabled || !jit_native_cache_path[0] ||
        !jit_native_cache_complete || jit_native_cache_block_count == 0 ||
        jit_native_cache_dependency_count == 0 ||
        nds->bus.slot2_rom || nds->bus.slot2_page_bits || nds->bus.slot2_code_bits)
        return;
    /* Invalidation bitmaps are indexed through the live bus mapping. Persist
     * them only when that mapping still matches the post-reset layout. */
    if (!jit_native_cache_initial_mapping_signature ||
        jit_native_mapping_signature(nds) !=
        jit_native_cache_initial_mapping_signature)
        return;

    size_t blocks_size = (size_t)jit_native_cache_block_count *
                         sizeof(jit_native_cache_block_t);
    size_t dependencies_size = (size_t)jit_native_cache_dependency_count *
                               sizeof(jit_native_cache_dependency_t);
    size_t payload_size = sizeof(jit_arena_t) + sizeof(jit_native_cache_aux_t) +
                          blocks_size + dependencies_size;
    typedef void *(*fn_malloc)(size_t);
    typedef void (*fn_free)(void *);
    fn_malloc allocate = (fn_malloc)sym_libc_malloc;
    fn_free release = (fn_free)sym_libc_free;
    uint8_t *payload = (uint8_t *)allocate(payload_size);
    if (!payload)
        return;

    jit_arena_t *snapshot = (jit_arena_t *)payload;
    memcpy(snapshot, &nds->jit_arena, sizeof(*snapshot));
    uint8_t *base = (uint8_t *)&nds->jit_arena;
    for (uint32_t i = 0; i < JIT_REGIONS; i++) {
        if (!jit_native_cache_encode_pointer(base, snapshot->region[i].code,
                                             &snapshot->region[i].code) ||
            !jit_native_cache_encode_pointer(base, snapshot->region[i].table,
                                             &snapshot->region[i].table)) {
            release(payload);
            return;
        }
    }
    if (!jit_native_cache_encode_pointer(base, snapshot->record_cursor,
                                         &snapshot->record_cursor)) {
        release(payload);
        return;
    }
    memset(snapshot->records, 0, sizeof snapshot->records);
    memset(snapshot->pending_branch, 0, sizeof snapshot->pending_branch);
    snapshot->pending_branch_count = 0;
    snapshot->block_pc_count = 0;
    memset(snapshot->block_pc, 0, sizeof snapshot->block_pc);
    for (uint32_t i = 0; i < snapshot->itcm_variant_count; i++) {
        uint8_t *block = snapshot->itcm_variant[i].block;
        if (block && !jit_native_cache_encode_pointer(
                         base, block, &snapshot->itcm_variant[i].block)) {
            release(payload);
            return;
        }
    }

    jit_native_cache_aux_t *aux =
        (jit_native_cache_aux_t *)(payload + sizeof(*snapshot));
    jit_native_cache_capture_aux(nds, aux);
    memcpy((uint8_t *)aux + sizeof(*aux), jit_native_cache_blocks, blocks_size);
    memcpy((uint8_t *)aux + sizeof(*aux) + blocks_size,
           jit_native_cache_dependencies, dependencies_size);

    jit_native_cache_header_t header = {
        JIT_NATIVE_CACHE_MAGIC, JIT_NATIVE_CACHE_VERSION,
        SEEDLESS_JIT_NATIVE_CACHE_ABI,
        jit_warm_cache_game_code, jit_warm_cache_rom_size,
        jit_warm_cache_header_crc, jit_native_cache_block_count,
        jit_native_cache_dependency_count,
        sizeof(jit_arena_t), sizeof(jit_native_cache_aux_t),
        jit_native_cache_crc(payload, payload_size),
        jit_native_cache_initial_mapping_signature
    };
    void *file = nds_platform_default()->files.open(
        nds_platform_default()->user, jit_native_cache_path, "wb");
    if (file) {
        size_t (*write_file)(const void *, size_t, size_t, void *) =
            (size_t (*)(const void *, size_t, size_t, void *))sym_libc_fwrite;
        int (*close_file)(void *) = (int (*)(void *))sym_libc_fclose;
        if (write_file(&header, sizeof header, 1, file) == 1)
            (void)write_file(payload, 1, payload_size, file);
        close_file(file);
    }
    release(payload);
}

void jit_warm_cache_begin(unsigned char *machine)
{
    arm_t *cpu = CPU(machine);
    nds_t *nds = cpu->machine;
    rom_image_t *rom = nds->cart.rom;
    jit_warm_cache_header_t header;
    size_t (*read_file)(void *, size_t, size_t, void *);
    int (*close_file)(void *);

    jit_warm_cache_clear();
    if (!rom || !rom->data || rom->size < CART_HEADER_SIZE)
        return;

    /* cart_boot_from_header assigns cart.game_code during reset, after this
     * cache is opened. Read the same header field directly here. */
    jit_warm_cache_game_code = rd32(rom->data + 12);
    jit_warm_cache_rom_size = rom->size;
    jit_warm_cache_header_crc = util_crc32_buffer_padded(rom->data, CART_HEADER_SIZE);
    int path_length = snprintf(jit_warm_cache_path, sizeof jit_warm_cache_path,
                               "%s/jit_warm_%08x_%08x_%08x.bin", nds->cache_dir,
                               jit_warm_cache_game_code, jit_warm_cache_rom_size,
                               jit_warm_cache_header_crc);
    if (path_length < 0 || (size_t)path_length >= sizeof jit_warm_cache_path) {
        jit_warm_cache_path[0] = 0;
        return;
    }
    path_length = snprintf(jit_native_cache_path, sizeof jit_native_cache_path,
                           "%s.native", jit_warm_cache_path);
    if (path_length < 0 || (size_t)path_length >= sizeof jit_native_cache_path) {
        jit_native_cache_path[0] = 0;
    }
    jit_warm_cache_enabled = 1;
    jit_native_cache_complete = jit_native_cache_path[0] != 0;

    read_file = (size_t (*)(void *, size_t, size_t, void *))sym_libc_fread;
    close_file = (int (*)(void *))sym_libc_fclose;
    void *file = nds_platform_default()->files.open(
        nds_platform_default()->user, jit_warm_cache_path, "rb");
    if (!file)
        return;

    if (read_file(&header, sizeof header, 1, file) == 1 &&
        header.magic == JIT_WARM_CACHE_MAGIC &&
        header.version == JIT_WARM_CACHE_VERSION &&
        header.game_code == jit_warm_cache_game_code &&
        header.rom_size == jit_warm_cache_rom_size &&
        header.header_crc == jit_warm_cache_header_crc &&
        header.count <= JIT_WARM_CACHE_MAX_ENTRIES &&
        read_file(jit_warm_cache_entries, sizeof(jit_warm_cache_entry_t),
                  header.count, file) == header.count &&
        jit_warm_cache_entries_crc(jit_warm_cache_entries, header.count) ==
            header.entries_crc) {
        uint32_t valid_count = 0;
        for (uint32_t i = 0; i < header.count; i++) {
            jit_warm_cache_entry_t entry = jit_warm_cache_entries[i];
            if (entry.cpu > 1)
                continue;
            jit_warm_cache_entries[valid_count++] = entry;
        }
        jit_warm_cache_count = valid_count;
        jit_warm_cache_preload_count = valid_count;
        if (jit_warm_cache_preload_count > JIT_WARM_CACHE_PREWARM_ENTRIES)
            jit_warm_cache_preload_count = JIT_WARM_CACHE_PREWARM_ENTRIES;
        for (uint32_t i = 0; i < jit_warm_cache_preload_count; i++)
            jit_warm_cache_preload_entries[i] =
                jit_warm_cache_entries[valid_count - i - 1];
        /* The persisted list is oldest-to-newest for LRU tie-breaking. Rank
         * repeated frame-boundary samples first, keeping newest entries first
         * when two blocks have equal sample counts. */
        for (uint32_t i = 1; i < jit_warm_cache_preload_count; i++) {
            jit_warm_cache_entry_t entry = jit_warm_cache_preload_entries[i];
            uint32_t j = i;
            while (j != 0 &&
                   jit_warm_cache_preload_entries[j - 1].samples < entry.samples) {
                jit_warm_cache_preload_entries[j] =
                    jit_warm_cache_preload_entries[j - 1];
                j--;
            }
            jit_warm_cache_preload_entries[j] = entry;
        }
    }
    close_file(file);
}

static void jit_warm_cache_seed_entry(arm_t *cpu, uint32_t load_address,
                                      uint32_t load_size)
{
    uint32_t pc = cpu->pc;
    uint32_t entry = pc & ~1u;
    if (!load_size || entry < load_address ||
        (uint64_t)entry >= (uint64_t)load_address + load_size)
        return;

    if (jit_block_lookup_cached(cpu, pc)) {
#if defined(SEEDLESS_JIT_PROFILE)
        jit_warm_profile_already_compiled++;
#endif
        return;
    }
#if defined(SEEDLESS_JIT_PROFILE)
    jit_warm_profile_candidates++;
#endif
    (void)jit_cache_lookup_or_compile(cpu, pc);
}

void jit_warm_cache_preload(unsigned char *machine, int allow)
{
    nds_t *nds = (nds_t *)machine;
    /* 1: boot preload plus direct-ROM entry seeds; 2: restored-state manifest only. */
    if (allow == 0 || nds->runtime.jit_enabled == 0)
        return;

    if (allow == 1)
        jit_native_cache_initial_mapping_signature =
            jit_native_mapping_signature(nds);

    /* Restore only on a regular ROM boot. Savestate restoration has different
     * RAM and mapping contents; it keeps the existing signature-checked,
     * on-demand warm-precompile path below. */
    if (allow == 1 && jit_native_cache_try_restore(nds))
        return;

#if defined(SEEDLESS_JIT_PROFILE)
    jit_warm_profile_loaded = jit_warm_cache_preload_count;
    jit_warm_profile_preloading = 1;
#endif
    if (jit_warm_cache_enabled) {
        for (uint32_t i = 0; i < jit_warm_cache_preload_count; i++) {
            jit_warm_cache_entry_t *entry = &jit_warm_cache_preload_entries[i];
            arm_t *cpu = entry->cpu ? &nds->arm9 : &nds->arm7;
            if (entry->signature == 0 ||
                jit_warm_cache_signature(cpu, entry->pc) != entry->signature) {
#if defined(SEEDLESS_JIT_PROFILE)
                jit_warm_profile_signature_skips++;
#endif
                continue;
            }
            if (jit_block_lookup_cached(cpu, entry->pc) != NULL) {
#if defined(SEEDLESS_JIT_PROFILE)
                jit_warm_profile_already_compiled++;
#endif
                continue;
            }
#if defined(SEEDLESS_JIT_PROFILE)
            jit_warm_profile_candidates++;
#endif
            (void)jit_cache_lookup_or_compile(cpu, entry->pc);
        }
    }

    /* Header boot exposes both executable ranges before the first scheduler
     * slice. Seed their entry blocks now; the existing JIT links reachable
     * direct branches, while overlays and modified code retain runtime JIT. */
    if (allow == 1 && !nds->config.boot_from_firmware &&
        nds->cart.rom && nds->cart.rom->data &&
        nds->cart.rom->size >= CART_HEADER_SIZE) {
        const uint8_t *header = nds->cart.rom->data;
        jit_warm_cache_seed_entry(&nds->arm9, rd32(header + 40), rd32(header + 44));
        jit_warm_cache_seed_entry(&nds->arm7, rd32(header + 56), rd32(header + 60));
    }
#if defined(SEEDLESS_JIT_PROFILE)
    jit_warm_profile_preloading = 0;
#endif
}

void jit_warm_cache_profile_read(uint64_t *loaded, uint64_t *signature_skips,
                                 uint64_t *already_compiled, uint64_t *candidates,
                                 uint64_t *compiled_blocks,
                                 uint64_t *native_blocks_restored)
{
#if defined(SEEDLESS_JIT_PROFILE)
    if (loaded) *loaded = jit_warm_profile_loaded;
    if (signature_skips) *signature_skips = jit_warm_profile_signature_skips;
    if (already_compiled) *already_compiled = jit_warm_profile_already_compiled;
    if (candidates) *candidates = jit_warm_profile_candidates;
    if (compiled_blocks) *compiled_blocks = jit_warm_profile_compiled_blocks;
    if (native_blocks_restored)
        *native_blocks_restored = jit_warm_profile_native_blocks_restored;
#else
    if (loaded) *loaded = 0;
    if (signature_skips) *signature_skips = 0;
    if (already_compiled) *already_compiled = 0;
    if (candidates) *candidates = 0;
    if (compiled_blocks) *compiled_blocks = 0;
    if (native_blocks_restored) *native_blocks_restored = 0;
#endif
}

void jit_warm_cache_record(void *ctx, uint32_t pc)
{
    if (!jit_warm_cache_enabled)
        return;

    uint32_t signature = jit_warm_cache_signature(ctx, pc);
    if (!signature)
        return;

    uint32_t cpu = CPU(ctx)->is_arm9 != 0;
    uint32_t index = jit_warm_cache_count;
    for (uint32_t i = 0; i < jit_warm_cache_count; i++) {
        if (jit_warm_cache_entries[i].pc == pc &&
            jit_warm_cache_entries[i].cpu == cpu) {
            index = i;
            break;
        }
    }
    if (index < jit_warm_cache_count) {
        uint32_t samples = jit_warm_cache_entries[index].signature == signature
            ? jit_warm_cache_entries[index].samples : 0;
        memmove(&jit_warm_cache_entries[index],
                &jit_warm_cache_entries[index + 1],
                (jit_warm_cache_count - index - 1) * sizeof(jit_warm_cache_entries[0]));
        jit_warm_cache_count--;
        jit_warm_cache_entries[jit_warm_cache_count++] =
            (jit_warm_cache_entry_t){ pc, cpu, signature, samples };
        return;
    }

    if (jit_warm_cache_count == JIT_WARM_CACHE_MAX_ENTRIES) {
        /* Evict the least sampled entry; ties evict the oldest one. */
        uint32_t evict = 0;
        for (uint32_t i = 1; i < jit_warm_cache_count; i++) {
            if (jit_warm_cache_entries[i].samples <
                jit_warm_cache_entries[evict].samples)
                evict = i;
        }
        memmove(&jit_warm_cache_entries[evict],
                &jit_warm_cache_entries[evict + 1],
                (jit_warm_cache_count - evict - 1) * sizeof(jit_warm_cache_entries[0]));
        jit_warm_cache_count--;
    }
    jit_warm_cache_entries[jit_warm_cache_count++] =
        (jit_warm_cache_entry_t){ pc, cpu, signature, 0 };
}

void jit_warm_cache_sample(void *ctx, uint32_t pc)
{
    /* The frame-boundary PC is already executing guest code. Record it without
     * asking jit_block_lookup() to compile a miss as a side effect. */
    if (!jit_warm_cache_enabled || !ctx || !CPU(ctx)->machine ||
        CPU(ctx)->machine->runtime.jit_enabled == 0)
        return;

    uint32_t cpu = CPU(ctx)->is_arm9 != 0;
    uint32_t index = jit_warm_cache_count;
    for (uint32_t i = 0; i < jit_warm_cache_count; i++) {
        if (jit_warm_cache_entries[i].pc == pc &&
            jit_warm_cache_entries[i].cpu == cpu) {
            index = i;
            break;
        }
    }
    if (index == jit_warm_cache_count) {
        jit_warm_cache_record(ctx, pc);
        for (uint32_t i = 0; i < jit_warm_cache_count; i++) {
            if (jit_warm_cache_entries[i].pc == pc &&
                jit_warm_cache_entries[i].cpu == cpu) {
                index = i;
                break;
            }
        }
    }
    if (index < jit_warm_cache_count &&
        jit_warm_cache_entries[index].samples != UINT32_MAX)
        jit_warm_cache_entries[index].samples++;
}

void jit_warm_cache_save(void)
{
    if (!jit_warm_cache_enabled || !jit_warm_cache_path[0])
        return;

    size_t (*write_file)(const void *, size_t, size_t, void *);
    int (*close_file)(void *);
    write_file = (size_t (*)(const void *, size_t, size_t, void *))sym_libc_fwrite;
    close_file = (int (*)(void *))sym_libc_fclose;
    void *file = nds_platform_default()->files.open(
        nds_platform_default()->user, jit_warm_cache_path, "wb");
    if (!file)
        return;

    jit_warm_cache_header_t header = {
        JIT_WARM_CACHE_MAGIC, JIT_WARM_CACHE_VERSION,
        jit_warm_cache_game_code, jit_warm_cache_rom_size,
        jit_warm_cache_header_crc, jit_warm_cache_count,
        jit_warm_cache_entries_crc(jit_warm_cache_entries, jit_warm_cache_count)
    };
    if (write_file(&header, sizeof header, 1, file) == 1)
        (void)write_file(jit_warm_cache_entries, sizeof(jit_warm_cache_entry_t),
                         jit_warm_cache_count, file);
    close_file(file);
    jit_native_cache_save(&nds_machine);
}

void jit_block_lookup_invalidate(void *ctx, uint32_t dir)
{
    arm_t *cpu = CPU(ctx);
    uint32_t slot = (dir >> 2) & (ARM_BLOCK_LOOKUP - 1u);
    cpu->block_lookup[slot] = 0;
    jit_block_lookup_secondary[cpu->is_arm9 != 0][slot] = 0;
}





uint32_t jit_watch_write8(unsigned char *machine, uint32_t dir) {
    uint32_t (*delegate)(unsigned char *, uint32_t) = jit_watch_write_notify;

    unsigned char *map = (unsigned char *)PT(machine)->bus;

    if ((dir >> 25) == 0) {
        uint32_t i = (dir >> 6) & 0x1ff;
        uint32_t word = ((const bus_t *)map)->itcm_code_bits[i];
        if ((word & (1u << ((dir >> 1) & 31))) == 0)
            return 0;
        return delegate(machine, dir & 0xfffffffcu);
    }

    {
        unsigned char *regions = (unsigned char *)PT(machine)->region;
        unsigned char *entry = regions + (uint64_t)(dir >> 23) * 96;
        uint32_t pair = dir & 0xfffffffeu;
        uint32_t *(*resolver)(void *, void *, uint32_t) =
            *(uint32_t *(**)(void *, void *, uint32_t))(entry + 80);

        uint32_t *word = resolver(map, entry, pair);
        if (word == 0)
            return 0;
        if ((*word & (1u << ((dir >> 1) & 31))) == 0)
            return 0;
        if ((pair >> 24) > 2)
            return 1;

        uint32_t j = (dir >> 7) & 0x7fff;
        uint32_t *slot = &CPU(machine)->jit_arena->main_ram_written_words[j];
        *slot = *slot | (1u << ((dir >> 2) & 31));
        return 1;
    }
}
typedef void *(*Fn341e0Block)(void *, uint32_t);





uint32_t jit_watch_write_notify(unsigned char *machine, uint32_t dir)
{

    jit_arena_t *arena = CPU(machine)->jit_arena;
    uint32_t idx = (dir >> 2) & 0x1fffu;
    unsigned char *pcont = &arena->itcm_arm_hits[idx];
    uint8_t mark = *pcont;
    uint32_t low_bits = (uint32_t)(mark & 0x7f);
    uint32_t next = (low_bits < 0xfu) ? low_bits + 1u : low_bits;

    if ((mark & JIT_ITCM_HAS_VARIANTS) != 0) {
        uint32_t busy = arena->itcm_variant_count;
        uint32_t key = rd32(PT(machine)->bus->itcm + (uint64_t)idx * 4u);
        jit_itcm_variant_t *ent = arena->itcm_variant;
        uint64_t i = 0;
        int found = 0;
        int full = 0;

        if (busy != 0) {

            int look_dir = (ent->word == key);
            for (;;) {
                if (look_dir && ent->pc == dir) {
                    found = 1;
                    break;
                }
                i++;
                ent++;
                if (i >= (uint64_t)busy) {
                    full = ((uint32_t)i > 0xfu);
                    break;
                }
                look_dir = (ent->word == key);
            }
        }

        if (found) {

            arena->itcm_arm_blocks[idx] = (uint32_t)(ent->block - (uint8_t *)arena);
            jit_block_lookup_invalidate(machine, dir);
            return 0;
        }

        if (!full) {

            jit_itcm_variant_t *e = &arena->itcm_variant[i];
            Fn341e0Block compile = jit_cache_lookup_or_compile;
            void *block;

            e->word = key;
            e->pc = dir;
            arena->itcm_arm_blocks[idx] = 0;
            jit_block_lookup_invalidate(machine, dir);

            block = compile(machine, dir);

            e->block = (uint8_t *)block;
            arena->itcm_variant_count = (uint32_t)i + 1u;
            return 0;
        }

    }

    *pcont = (uint8_t)(next | (uint32_t)(mark & 0x80u));
    return 1;
}

uint32_t jit_watch_write16(unsigned char *machine, uint32_t dir) {
    uint32_t (*delegate)(unsigned char *, uint32_t) = jit_watch_write_notify;

    unsigned char *map = (unsigned char *)PT(machine)->bus;

    if ((dir >> 25) == 0) {
        uint32_t i = (dir >> 6) & 0x1ff;
        uint32_t word = ((const bus_t *)map)->itcm_code_bits[i];
        if ((word & (1u << ((dir >> 1) & 31))) == 0)
            return 0;
        return delegate(machine, dir & 0xfffffffcu);
    }

    {
        unsigned char *regions = (unsigned char *)PT(machine)->region;
        unsigned char *entry = regions + (uint64_t)(dir >> 23) * 96;
        uint32_t *(*resolver)(void *, void *, uint32_t) =
            *(uint32_t *(**)(void *, void *, uint32_t))(entry + 80);

        uint32_t *word = resolver(map, entry, dir);
        if (word == 0)
            return 0;
        if ((*word & (1u << ((dir >> 1) & 31))) == 0)
            return 0;
        if ((dir >> 24) > 2)
            return 1;

        uint32_t j = (dir >> 7) & 0x7fff;
        uint32_t *slot = &CPU(machine)->jit_arena->main_ram_written_words[j];
        *slot = *slot | (1u << ((dir >> 2) & 31));
        return 1;
    }
}

uint32_t jit_watch_write32(unsigned char *machine, uint32_t dir) {
    uint32_t (*delegate)(unsigned char *, uint32_t) = jit_watch_write_notify;

    unsigned char *map = (unsigned char *)PT(machine)->bus;

    if ((dir >> 25) == 0) {
        uint32_t i = (dir >> 6) & 0x1ff;
        uint32_t word = ((const bus_t *)map)->itcm_code_bits[i];
        if ((word & (3u << ((dir >> 1) & 31))) == 0)
            return 0;
        return delegate(machine, dir);
    }

    {
        unsigned char *regions = (unsigned char *)PT(machine)->region;
        unsigned char *entry = regions + (uint64_t)(dir >> 23) * 96;
        uint32_t *(*resolver)(void *, void *, uint32_t) =
            *(uint32_t *(**)(void *, void *, uint32_t))(entry + 80);

        uint32_t *word = resolver(map, entry, dir);
        if (word == 0)
            return 0;
        if ((*word & (3u << ((dir >> 1) & 31))) == 0)
            return 0;
        if ((dir >> 24) > 2)
            return 1;

        uint32_t j = (dir >> 7) & 0x7fff;
        uint32_t *slot = &CPU(machine)->jit_arena->main_ram_written_words[j];
        *slot = *slot | (1u << ((dir >> 2) & 31));
        return 1;
    }
}

static uint32_t *locate(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

uint32_t jit_watch_words_hit_1(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 3;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = locate(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = locate(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = locate(t, end);
    if (p2 == 0)
        return low;
    return (((*p2 & m_end) != 0) ? 1 : 0) | low;
}

static uint32_t *locate_5(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

uint32_t jit_watch_words_hit_2(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 7;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = locate_5(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = locate_5(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = locate_5(t, end);
    if (p2 == 0)
        return low;
    return (((*p2 & m_end) != 0) ? 1 : 0) | low;
}

static uint32_t *resolve(unsigned char *b, uint32_t a) {
    unsigned char *table = *(unsigned char **)(b + 8);
    if ((a >> 25) == 0)
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];

    unsigned char *desc = *(unsigned char **)b + (uint64_t)(a >> 23) * 96;
    return ((uint32_t *(*)(void *, void *, uint32_t))(*(void **)(desc + 80)))(
               table, desc, a);
}

int jit_watch_words_hit_3(unsigned char *machine, uint32_t dir) {
    unsigned char *b = (unsigned char *)&PT(machine)->region;

    uint32_t end = dir + 0xb;
    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = resolve(b, dir);
        if (p == 0) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    uint32_t first_row;
    uint32_t *p = resolve(b, dir);
    first_row = (p == 0) ? 0 : ((*p & m1) != 0);

    uint32_t *q = resolve(b, end);
    if (q == 0) return (int)first_row;
    return (int)(((*q & m2) != 0) | first_row);
}

static uint32_t *code_bits_word(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

uint32_t jit_watch_words_hit_4(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 15;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = code_bits_word(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = code_bits_word(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = code_bits_word(t, end);
    if (p2 == 0)
        return low;
    return (((*p2 & m_end) != 0) ? 1 : 0) | low;
}
#define STRIDE     96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve_t)(void *, void *, uint32_t);



static uint32_t *resolve_8(uint8_t *ctx, uint32_t d)
{
    uint8_t *table = (uint8_t *)rd_ptr(ctx + 8);

    if ((d >> 25) == 0) {
        uint32_t idx = (d >> 6) & 0x1ff;
        return &((bus_t *)table)->itcm_code_bits[idx];
    }

    uint8_t *table_desc = (uint8_t *)rd_ptr(ctx);
    uint8_t *desc = table_desc + (uint64_t)(d >> 23) * STRIDE;
    fn_resolve_t fn = (fn_resolve_t)rd_ptr(desc + HANDLER_OFF);
    return fn(table, desc, d);
}

uint32_t jit_watch_words_hit_5(unsigned char *param_1, uint32_t param_2)
{

    uint8_t *ctx = (uint8_t *)&PT(param_1)->region;

    uint32_t dir = param_2;
    uint32_t end = dir + 0x13;

    uint32_t m_low = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m_high = ~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = resolve_8(ctx, dir);
        if (p == 0)
            return 0;
        return ((m_low & m_high) & rd32(p)) != 0;
    }

    uint32_t bVar5 = 0;
    uint32_t *p1 = resolve_8(ctx, dir);
    if (p1 != 0)
        bVar5 = (rd32(p1) & m_low) != 0;

    uint32_t *p2 = resolve_8(ctx, end);
    if (p2 == 0)
        return bVar5;

    return ((rd32(p2) & m_high) != 0) | bVar5;
}
#undef STRIDE
#undef HANDLER_OFF
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_get)(void *, void *, uint32_t);


static uint32_t *resolve_9(uint8_t *base, uint32_t d) {
    if ((d >> 25) == 0) {
        uint8_t *table = (uint8_t *)rd_ptr(base + 8);
        return &((bus_t *)table)->itcm_code_bits[(d >> 6) & 0x1ff];
    }
    uint8_t *regions = (uint8_t *)rd_ptr(base);
    uint8_t *ent = regions + (size_t)(d >> 23) * ENTRY_STRIDE;
    fn_get m = (fn_get)rd_ptr(ent + HANDLER_OFF);
    return m(rd_ptr(base + 8), ent, d);
}

int jit_watch_words_hit_6(uint8_t *ctx, uint32_t dir) {

    uint32_t end = dir + 0x17;

    uint32_t m_start = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xfffffffeu << ((end >> 1) & 31));

    uint8_t *base = (uint8_t *)&PT(ctx)->region;

    if (((end ^ dir) & 0xffffffffu) < 0x40u) {

        uint32_t *p = resolve_9(base, dir);
        if (!p) return 0;
        return (*p & (m_start & m_end)) != 0;
    }

    int low;
    uint32_t *p1 = resolve_9(base, dir);
    if (!p1) {
        low = 0;
    } else {
        low = (*p1 & m_start) != 0;
    }

    uint32_t *p2 = resolve_9(base, end);
    if (!p2) return low;

    return ((*p2 & m_end) != 0) | low;
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF

static uint32_t *locate_10(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

uint32_t jit_watch_words_hit_7(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 0x1b;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = locate_10(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = locate_10(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = locate_10(t, end);
    if (p2 == 0)
        return low;
    return (((*p2 & m_end) != 0) ? 1 : 0) | low;
}
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve)(void *, void *, uint32_t);


static uint32_t *resolve_11(unsigned char *b, uint32_t a) {
    unsigned char *table = (unsigned char *)rd_ptr(b + 8);

    if ((a >> 25) == 0) {
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];
    }

    unsigned char *objs = (unsigned char *)rd_ptr(b);
    unsigned char *desc = objs + (uint64_t)(a >> 23) * ENTRY_STRIDE;
    fn_resolve m = (fn_resolve)rd_ptr(desc + HANDLER_OFF);
    return m(table, desc, a);
}

int jit_watch_words_hit_8(unsigned char *ctx, uint32_t dir) {

    unsigned char *b = (uint8_t *)&PT(ctx)->region;

    uint32_t end = dir + 0x1f;

    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40u) {

        uint32_t *p = resolve_11(b, dir);
        if (p == NULL) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_11(b, dir);
    if (p1 == NULL) {
        partial = 0;
    } else {
        partial = (*p1 & m1) != 0;
    }

    uint32_t *p2 = resolve_11(b, end);
    if (p2 == NULL) return partial;

    return (int)(((*p2 & m2) != 0) | (unsigned)partial);
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF

static uint32_t *locate_12(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

int jit_watch_words_hit_9(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 0x23;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = locate_12(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = locate_12(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = locate_12(t, end);
    if (p2 == 0)
        return (int)low;
    return (int)((((*p2 & m_end) != 0) ? 1u : 0u) | low);
}
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve_13)(void *, void *, uint32_t);


static uint32_t *resolve_13(unsigned char *b, uint32_t a) {
    unsigned char *table = (unsigned char *)rd_ptr(b + 8);

    if ((a >> 25) == 0) {
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];
    }

    unsigned char *objs = (unsigned char *)rd_ptr(b);
    unsigned char *desc = objs + (uint64_t)(a >> 23) * ENTRY_STRIDE;
    fn_resolve_13 m = (fn_resolve_13)rd_ptr(desc + HANDLER_OFF);
    return m(table, desc, a);
}

int jit_watch_words_hit_10(unsigned char *ctx, uint32_t dir) {

    unsigned char *b = (uint8_t *)&PT(ctx)->region;

    uint32_t end = dir + 0x27;

    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40u) {

        uint32_t *p = resolve_13(b, dir);
        if (p == NULL) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_13(b, dir);
    if (p1 == NULL) {
        partial = 0;
    } else {
        partial = (*p1 & m1) != 0;
    }

    uint32_t *p2 = resolve_13(b, end);
    if (p2 == NULL) return partial;

    return (int)(((*p2 & m2) != 0) | (unsigned)partial);
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve_14)(void *, void *, uint32_t);


static uint32_t *resolve_14(unsigned char *b, uint32_t a) {
    unsigned char *table = (unsigned char *)rd_ptr(b + 8);

    if ((a >> 25) == 0) {
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];
    }

    unsigned char *objs = (unsigned char *)rd_ptr(b);
    unsigned char *desc = objs + (uint64_t)(a >> 23) * ENTRY_STRIDE;
    fn_resolve_14 m = (fn_resolve_14)rd_ptr(desc + HANDLER_OFF);
    return m(table, desc, a);
}

int jit_watch_words_hit_11(unsigned char *ctx, uint32_t dir) {

    unsigned char *b = (uint8_t *)&PT(ctx)->region;

    uint32_t end = dir + 0x2b;

    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40u) {

        uint32_t *p = resolve_14(b, dir);
        if (p == NULL) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_14(b, dir);
    if (p1 == NULL) {
        partial = 0;
    } else {
        partial = (*p1 & m1) != 0;
    }

    uint32_t *p2 = resolve_14(b, end);
    if (p2 == NULL) return partial;

    return (int)(((*p2 & m2) != 0) | (unsigned)partial);
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF

static uint32_t *locate_15(unsigned char *t, uint32_t dir) {
    unsigned char *map = *(unsigned char **)(t + 8);
    if ((dir >> 25) == 0)
        return &((bus_t *)map)->itcm_code_bits[(dir >> 6) & 0x1ff];

    unsigned char *regions = *(unsigned char **)t;
    unsigned char *e = regions + (uint64_t)(dir >> 23) * 96;
    uint32_t *(*resolver)(void *, void *, uint32_t) =
        *(uint32_t *(**)(void *, void *, uint32_t))(e + 80);
    return resolver(map, e, dir);
}

uint32_t jit_watch_words_hit_12(unsigned char *machine, uint32_t dir) {
    unsigned char *t = (unsigned char *)&PT(machine)->region;
    uint32_t end = dir + 0x2f;

    uint32_t m_start = 0xFFFFFFFFu << ((dir >> 1) & 31);
    uint32_t m_end = ~(0xFFFFFFFEu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40) {
        uint32_t *p = locate_15(t, dir);
        if (p == 0)
            return 0;
        return ((m_start & m_end) & *p) != 0 ? 1 : 0;
    }

    uint32_t low = 0;
    uint32_t *p1 = locate_15(t, dir);
    if (p1 != 0)
        low = ((*p1 & m_start) != 0) ? 1 : 0;

    uint32_t *p2 = locate_15(t, end);
    if (p2 == 0)
        return low;
    return (((*p2 & m_end) != 0) ? 1 : 0) | low;
}
#define ENTRY_STRIDE   96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_get_16)(void *, void *, uint32_t);


static uint32_t *resolve_16(uint8_t *base, uint32_t d) {
    if ((d >> 25) == 0) {
        uint8_t *table = (uint8_t *)rd_ptr(base + 8);
        return &((bus_t *)table)->itcm_code_bits[(d >> 6) & 0x1ff];
    }
    uint8_t *objs = (uint8_t *)rd_ptr(base);
    uint8_t *ent = objs + (size_t)(d >> 23) * ENTRY_STRIDE;
    fn_get_16 m = (fn_get_16)rd_ptr(ent + HANDLER_OFF);
    return m(rd_ptr(base + 8), ent, d);
}

int jit_watch_words_hit_13(uint8_t *ctx, uint32_t dir) {

    uint32_t high = dir + 0x33;

    uint32_t m_low = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m_high = ~(0xfffffffeu << ((high >> 1) & 31));

    uint8_t *base = (uint8_t *)&PT(ctx)->region;

    if (((high ^ dir) & 0xffffffffu) < 0x40u) {

        uint32_t *p = resolve_16(base, dir);
        if (!p) return 0;
        return (*p & (m_low & m_high)) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_16(base, dir);
    if (!p1) {
        partial = 0;
    } else {
        partial = (*p1 & m_low) != 0;
    }

    uint32_t *p2 = resolve_16(base, high);
    if (!p2) return partial;

    return ((*p2 & m_high) != 0) | partial;
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve_17)(void *, void *, uint32_t);


static uint32_t *resolve_17(unsigned char *b, uint32_t a) {
    unsigned char *table = (unsigned char *)rd_ptr(b + 8);

    if ((a >> 25) == 0) {
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];
    }

    unsigned char *objs = (unsigned char *)rd_ptr(b);
    unsigned char *desc = objs + (uint64_t)(a >> 23) * ENTRY_STRIDE;
    fn_resolve_17 m = (fn_resolve_17)rd_ptr(desc + HANDLER_OFF);
    return m(table, desc, a);
}

int jit_watch_words_hit_14(unsigned char *ctx, uint32_t dir) {

    unsigned char *b = (uint8_t *)&PT(ctx)->region;

    uint32_t end = dir + 0x37;

    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40u) {

        uint32_t *p = resolve_17(b, dir);
        if (p == NULL) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_17(b, dir);
    if (p1 == NULL) {
        partial = 0;
    } else {
        partial = (*p1 & m1) != 0;
    }

    uint32_t *p2 = resolve_17(b, end);
    if (p2 == NULL) return partial;

    return (int)(((*p2 & m2) != 0) | (unsigned)partial);
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#define ENTRY_STRIDE   96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_get_18)(void *, void *, uint32_t);


static uint32_t *resolve_18(uint8_t *base, uint32_t d) {
    if ((d >> 25) == 0) {
        uint8_t *table = (uint8_t *)rd_ptr(base + 8);
        return &((bus_t *)table)->itcm_code_bits[(d >> 6) & 0x1ff];
    }
    uint8_t *objs = (uint8_t *)rd_ptr(base);
    uint8_t *ent = objs + (size_t)(d >> 23) * ENTRY_STRIDE;
    fn_get_18 m = (fn_get_18)rd_ptr(ent + HANDLER_OFF);
    return m(rd_ptr(base + 8), ent, d);
}

int jit_watch_words_hit_15(uint8_t *ctx, uint32_t dir) {

    uint32_t high = dir + 0x3b;

    uint32_t m_low = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m_high = ~(0xfffffffeu << ((high >> 1) & 31));

    uint8_t *base = (uint8_t *)&PT(ctx)->region;

    if (((high ^ dir) & 0xffffffffu) < 0x40u) {

        uint32_t *p = resolve_18(base, dir);
        if (!p) return 0;
        return (*p & (m_low & m_high)) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_18(base, dir);
    if (!p1) {
        partial = 0;
    } else {
        partial = (*p1 & m_low) != 0;
    }

    uint32_t *p2 = resolve_18(base, high);
    if (!p2) return partial;

    return ((*p2 & m_high) != 0) | partial;
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#define ENTRY_STRIDE    96
#define HANDLER_OFF 80
typedef uint32_t *(*fn_resolve_19)(void *, void *, uint32_t);


static uint32_t *resolve_19(unsigned char *b, uint32_t a) {
    unsigned char *table = (unsigned char *)rd_ptr(b + 8);

    if ((a >> 25) == 0) {
        return &((bus_t *)table)->itcm_code_bits[(a >> 6) & 0x1ff];
    }

    unsigned char *objs = (unsigned char *)rd_ptr(b);
    unsigned char *desc = objs + (uint64_t)(a >> 23) * ENTRY_STRIDE;
    fn_resolve_19 m = (fn_resolve_19)rd_ptr(desc + HANDLER_OFF);
    return m(table, desc, a);
}

int jit_watch_words_hit_16(unsigned char *ctx, uint32_t dir) {

    unsigned char *b = (uint8_t *)&PT(ctx)->region;

    uint32_t end = dir + 0x3f;

    uint32_t m1 = 0xffffffffu << ((dir >> 1) & 31);
    uint32_t m2 = (uint32_t)~(0xfffffffeu << ((end >> 1) & 31));

    if ((end ^ dir) < 0x40u) {

        uint32_t *p = resolve_19(b, dir);
        if (p == NULL) return 0;
        return ((m1 & m2) & *p) != 0;
    }

    int partial;
    uint32_t *p1 = resolve_19(b, dir);
    if (p1 == NULL) {
        partial = 0;
    } else {
        partial = (*p1 & m1) != 0;
    }

    uint32_t *p2 = resolve_19(b, end);
    if (p2 == NULL) return partial;

    return (int)(((*p2 & m2) != 0) | (unsigned)partial);
}
#undef ENTRY_STRIDE
#undef HANDLER_OFF





static uint64_t jit_cache_line_sizes[2] = { 0xffffu, 0xffffu };
static uint64_t jit_profile_blocks;
static uint64_t jit_profile_analysis_us;
#if defined(SEEDLESS_JIT_PROFILE)
static uint64_t jit_profile_ldm_batch_calls;
static uint64_t jit_profile_ldm_batch_hits;
static uint64_t jit_profile_gxfifo_stm_calls;
static uint64_t jit_profile_gxfifo_stm_hits;
uint64_t jit_dispatch_profile_counters[2][8]
    __attribute__((visibility("hidden"), aligned(64)));

void jit_dispatch_profile_read(uint64_t *primary_misses, uint64_t *secondary_hits)
{
    if (primary_misses)
        *primary_misses = __atomic_load_n(&jit_dispatch_profile_counters[0][0], __ATOMIC_RELAXED)
                        + __atomic_load_n(&jit_dispatch_profile_counters[1][0], __ATOMIC_RELAXED);
    if (secondary_hits)
        *secondary_hits = __atomic_load_n(&jit_dispatch_profile_counters[0][1], __ATOMIC_RELAXED)
                        + __atomic_load_n(&jit_dispatch_profile_counters[1][1], __ATOMIC_RELAXED);
}
#endif

void jit_transfer_profile_read(uint64_t *ldm_calls, uint64_t *ldm_hits,
                               uint64_t *gxfifo_stm_calls, uint64_t *gxfifo_stm_hits)
{
#if defined(SEEDLESS_JIT_PROFILE)
    if (ldm_calls) *ldm_calls = __atomic_load_n(&jit_profile_ldm_batch_calls, __ATOMIC_RELAXED);
    if (ldm_hits) *ldm_hits = __atomic_load_n(&jit_profile_ldm_batch_hits, __ATOMIC_RELAXED);
    if (gxfifo_stm_calls) *gxfifo_stm_calls = __atomic_load_n(&jit_profile_gxfifo_stm_calls, __ATOMIC_RELAXED);
    if (gxfifo_stm_hits) *gxfifo_stm_hits = __atomic_load_n(&jit_profile_gxfifo_stm_hits, __ATOMIC_RELAXED);
#else
    if (ldm_calls) *ldm_calls = 0;
    if (ldm_hits) *ldm_hits = 0;
    if (gxfifo_stm_calls) *gxfifo_stm_calls = 0;
    if (gxfifo_stm_hits) *gxfifo_stm_hits = 0;
#endif
}

void jit_profile_reset(void)
{
    __atomic_store_n(&jit_profile_blocks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&jit_profile_analysis_us, 0, __ATOMIC_RELAXED);
#if defined(SEEDLESS_JIT_PROFILE)
    memset(jit_dispatch_profile_counters, 0, sizeof jit_dispatch_profile_counters);
    __atomic_store_n(&jit_profile_ldm_batch_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&jit_profile_ldm_batch_hits, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&jit_profile_gxfifo_stm_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&jit_profile_gxfifo_stm_hits, 0, __ATOMIC_RELAXED);
    jit_warm_profile_loaded = 0;
    jit_warm_profile_signature_skips = 0;
    jit_warm_profile_already_compiled = 0;
    jit_warm_profile_candidates = 0;
    jit_warm_profile_compiled_blocks = 0;
    jit_warm_profile_preloading = 0;
#endif
}

void jit_profile_read(uint64_t *blocks, uint64_t *analysis_us)
{
    if (blocks)
        *blocks = __atomic_load_n(&jit_profile_blocks, __ATOMIC_RELAXED);
    if (analysis_us)
        *analysis_us = __atomic_load_n(&jit_profile_analysis_us, __ATOMIC_RELAXED);
}

static void flush_cache_range(const uint8_t *first, const uint8_t *last)
{
    uint64_t start = (uint64_t)(uintptr_t)first;
    uint64_t end = (uint64_t)(uintptr_t)last;
    if (start == end) return;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint8_t *g = (uint8_t *)jit_cache_line_sizes;
    uint64_t li = rd64(g), ld = rd64(g + 8);
    uint64_t ci = 4ull << (ctr & 0xf);
    uint64_t cd = 4ull << ((ctr >> 16) & 0xf);
    if (li >= ci) li = ci;
    if (ld >= cd) ld = cd;
    wr64(g, li); wr64(g + 8, ld);

    for (uint64_t p = start & ~(ld - 1); p < end; p += ld)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t p = start & ~(li - 1); p < end; p += li)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

}

void *jit_cache_lookup_or_compile(void *ctx, uint32_t dir)
{
    uint8_t *c = (uint8_t *)ctx;
    jit_arena_t *ar = CPU(c)->jit_arena;
    uint8_t *b = (uint8_t *)ar;
    uint32_t i13 = (dir >> 2) & 0x3ff;
    uint32_t i21 = (dir >> 2) & 0x1fff;
    uint32_t i20 = (dir >> 1) & 0x3fff;
    uint32_t pair = dir & 1;
    jit_hash_entry_t *ent = 0;
    uint32_t off = 0;

#define LOOKUP_RETURN(p, o) do {                                    \
        uint64_t old_target_ = CPU(c)->block_lookup[i13];            \
        if ((uint32_t)old_target_ != dir)                            \
            jit_block_lookup_secondary[CPU(c)->is_arm9 != 0][i13] = old_target_; \
        CPU(c)->block_lookup[i13] = ((uint64_t)(o) << 32) | dir; \
        return (void *)(p);                               \
    } while (0)

    if ((dir >> 25) == 0 && ((arm_t *)c)->is_arm9 == 1) {

        uint8_t *t = (uint8_t *)(pair ? CPU(c)->itcm_thumb_blocks : CPU(c)->itcm_arm_blocks);
        off = rd32(t + (size_t)(pair ? i20 : i21) * 4);
        if (off) LOOKUP_RETURN(b + off, off);
    } else {
        uint32_t mask = ((dir & 0xff000000u) == 0x2000000u) ? 0x7fffu : 0x1fffu;
        jit_hash_entry_t *t = ((dir & 0xff000000u) == 0x2000000u) ? ar->main_ram_blocks : ar->other_blocks;
        ent = &t[(dir >> 2) & mask];
        if (ent->pc == dir)     { uint32_t o_ = ent->offset; LOOKUP_RETURN(b + o_, o_); }
        off = ent->offset_2;
        if (ent->pc_2 == dir) LOOKUP_RETURN(b + off, off);
        if (off) {

            uint8_t *n = b + rd32(b + off - 4);
            for (;;) {
                uint32_t nx = rd32(n);
                if (nx == 0) break;
                n = b + nx;
                if (rd32(n + 4) == dir) { uint32_t o_ = rd32(n + 8); LOOKUP_RETURN(b + o_, o_); }
            }
        }
    }

    jit_region_t *region = ar->region;
    uint8_t *v0 = region[JIT_REGION_MAIN_RAM].code, *s0 = region[JIT_REGION_MAIN_RAM].table;
    uint8_t *v1 = region[JIT_REGION_OTHER].code,    *s1 = region[JIT_REGION_OTHER].table;
    uint8_t *v2 = region[JIT_REGION_ITCM].code,     *s2 = region[JIT_REGION_ITCM].table;

    ar->record_cursor = ar->records;
    ar->pending_branch_count = 0;
    ar->block_pc_count = 0;

#if defined(SEEDLESS_JIT_PROFILE)
    uint64_t analysis_start, analysis_end;
    time_now_microseconds(&analysis_start);
#endif
    uint8_t *updated = (uint8_t *)jit_block_analyze(ctx, dir & ~1u, pair);
#if defined(SEEDLESS_JIT_PROFILE)
    time_now_microseconds(&analysis_end);
    if (updated)
        __atomic_add_fetch(&jit_profile_blocks, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&jit_profile_analysis_us, analysis_end - analysis_start,
                       __ATOMIC_RELAXED);
#endif
    uint32_t noff = (uint32_t)(updated - b);

    if (updated) {
#if defined(SEEDLESS_JIT_PROFILE)
        if (jit_warm_profile_preloading)
            jit_warm_profile_compiled_blocks++;
#endif
        jit_warm_cache_record(ctx, dir);
        if (ent) {
            if (ent->offset == 0) {
                ent->pc = dir; ent->offset = noff;
            } else if (ent->offset_2 == 0) {
                ent->pc_2 = dir; ent->offset_2 = noff;
            } else {

                uint32_t p = rd32(b + ent->offset_2 - 4);
                for (;;) {
                    uint32_t v = rd32(b + p);
                    if (v == 0) break;
                    p = v;
                }
                wr32(b + p, rd32(updated - 4));
            }
        } else if (pair) {
            CPU(c)->itcm_thumb_blocks[i20] = noff;
        } else {
            CPU(c)->itcm_arm_blocks[i21] = noff;
            uint8_t *a = &ar->itcm_arm_hits[i21];
            if (*a >= JIT_ITCM_HOT_BLOCK) *a = (uint8_t)(*a | JIT_ITCM_HAS_VARIANTS);
        }
    }

    jit_link_pending_branches(ctx);

    flush_cache_range(v0, region[JIT_REGION_MAIN_RAM].code);
    flush_cache_range(region[JIT_REGION_MAIN_RAM].table, s0);
    flush_cache_range(v1, region[JIT_REGION_OTHER].code);
    flush_cache_range(region[JIT_REGION_OTHER].table, s1);
    flush_cache_range(v2, region[JIT_REGION_ITCM].code);
    flush_cache_range(region[JIT_REGION_ITCM].table, s2);

    LOOKUP_RETURN(updated, noff);
}

/* A slow ARM LDM may contain several reads from directly mapped memory, for
 * example after its first word reached a page through the bus helper. Keep
 * the normal path for translated/handled regions, TCM overlays, and ranges
 * crossing an 8 MiB bus region. For a contiguous direct range, populate the
 * page-table descriptors once per page, then perform the words directly. */
int jit_ldm_main_ram_words(void *ctx, uint32_t address, uint32_t count,
                           uint32_t *destination)
{
#if defined(SEEDLESS_JIT_PROFILE)
    __atomic_add_fetch(&jit_profile_ldm_batch_calls, 1, __ATOMIC_RELAXED);
#endif
    arm_t *cpu = (arm_t *)ctx;
    if (!cpu)
        return 0;
    bus_t *bus = cpu->bus;
    pagetable_t *table = &cpu->pagetable;
    if (!bus || !table->region || !destination || count < 2 || count > 16 ||
        (address & 3u) || (address >> 28) != 0)
        return 0;

    uint32_t last = address + count * 4u - 1u;
    if (last < address || (last >> 28) != 0 ||
        (address >> BUS_REGION_SHIFT) != (last >> BUS_REGION_SHIFT))
        return 0;

    if (cpu->is_arm9 && bus->dtcm_end > bus->dtcm_start &&
        (uint64_t)address < bus->dtcm_end && (uint64_t)last + 1u > bus->dtcm_start)
        return 0;

    /* The ARM9 ITCM overlays the low address range outside bus regions. */
    if (cpu->is_arm9 && address < bus->itcm_size)
        return 0;

    const bus_region_t *entry = &table->region[address >> BUS_REGION_SHIFT];
    if (entry->read_kind != BUS_REGION_DIRECT || !entry->read_memory)
        return 0;

    /* A normal bus read refreshes the descriptor even when it contains the
     * nonzero slow-access sentinel. Repeat it once per touched 2 KiB page;
     * all words in that page share this region's direct mapping. */
    uint32_t page = address & ~((1u << PAGETABLE_PAGE_SHIFT) - 1u);
    for (;;) {
        (void)bus_read32_slow((uint8_t *)table, page);
        if (page >= (last & ~((1u << PAGETABLE_PAGE_SHIFT) - 1u)))
            break;
        page += 1u << PAGETABLE_PAGE_SHIFT;
    }

    for (uint32_t i = 0; i < count; i++)
        destination[i] = rd32(entry->read_memory + ((address + i * 4u) & entry->mask));
#if defined(SEEDLESS_JIT_PROFILE)
    __atomic_add_fetch(&jit_profile_ldm_batch_hits, 1, __ATOMIC_RELAXED);
#endif
    return 1;
}

/* ARM STM bursts into the GX FIFO otherwise dispatch every word through the
 * slow I/O bus path. Reuse the FIFO's DMA batch decoder when this burst cannot
 * trigger its command-ring flush; boundary cases retain per-word semantics. */
int jit_stm_gxfifo_words(void *ctx, uint32_t address, uint32_t count,
                         const uint32_t *source)
{
#if defined(SEEDLESS_JIT_PROFILE)
    __atomic_add_fetch(&jit_profile_gxfifo_stm_calls, 1, __ATOMIC_RELAXED);
#endif
    arm_t *cpu = (arm_t *)ctx;
    if (!cpu || !cpu->is_arm9 || !cpu->bus || !source || count < 2 || count > 16 ||
        (address & 3u) || address < 0x04000400u)
        return 0;

    uint32_t last = address + count * 4u - 4u;
    if (last < address || last > 0x0400043cu)
        return 0;

    bus_t *bus = cpu->bus;
    gpu3d_t *gpu3d = bus->gpu3d;
    if (!gpu3d || !gpu3d->command_pending_cursor || !gpu3d->param_pending_cursor)
        return 0;

    uint32_t used = (uint32_t)(gpu3d->command_pending_cursor - gpu3d->command_ring);
    if (used + count * 4u >= 0x400u)
        return 0;

    gpu3d_gxfifo_decode_commands(gpu3d, (uint32_t *)source, count);
    uint8_t *mirror = (uint8_t *)bus->io_mirror;
    for (uint32_t i = 0; i < count; i++)
        wr32(mirror + ((address + i * 4u) & 0x7fffu), rd32(source + i));
#if defined(SEEDLESS_JIT_PROFILE)
    __atomic_add_fetch(&jit_profile_gxfifo_stm_hits, 1, __ATOMIC_RELAXED);
#endif
    return 1;
}




/* Return an already-compiled block without generating code on a miss. Warm
 * cache bookkeeping needs a true lookup: compiling here would skip the normal
 * branch-linking and AArch64 instruction-cache publication path. The analyzer
 * still uses jit_block_lookup() below when it deliberately compiles a branch
 * target as part of an outer compilation. */
static void *jit_block_lookup_cached(void *param_1, uint32_t param_2)
{

    unsigned char *ctx = (unsigned char *)param_1;
    uint32_t key = param_2;

    jit_arena_t *ar = CPU(ctx)->jit_arena;
    uint64_t base = (uint64_t)(uintptr_t)ar;
    unsigned char *pbase = (unsigned char *)base;

    jit_hash_entry_t *entry = NULL;

    if ((key >> 25) == 0 && CPU(ctx)->is_arm9 == 1) {
        int odd = (key & 1u) != 0;
        unsigned char *table =
            (unsigned char *)(odd ? CPU(ctx)->itcm_thumb_blocks : CPU(ctx)->itcm_arm_blocks);
        uint32_t shift = odd ? 1u : 2u;
        uint32_t mask  = odd ? 0x3fffu : 0x1fffu;
        uint32_t idx   = (key >> shift) & mask;
        uint32_t val   = rd32(table + (uint64_t)idx * 4u);
        if (val != 0) {
            return (void *)(base + val);
        }
        entry = NULL;
    } else {

        if ((key & 0xff000000u) == 0x02000000u) {
            entry = &ar->main_ram_blocks[(key >> 2) & 0x7fffu];
        } else {
            entry = &ar->other_blocks[(key >> 2) & 0x1fffu];
        }

        uint32_t w0 = entry->pc;
        if (w0 == key) {
            return (void *)(base + entry->offset);
        }

        uint32_t addr2   = entry->pc_2;
        uint32_t target2 = entry->offset_2;
        if (addr2 == key) {
            return (void *)(base + target2);
        }

        if (target2 != 0) {
            unsigned char *code2 = pbase + target2;
            uint32_t h = rd32(code2 - 4);

            unsigned char *p = pbase + h;
            for (;;) {
                uint32_t v = rd32(p);
                if (v == 0) {
                    break;
                }
                unsigned char *cand = pbase + v;
                if (rd32(cand + 4) == key) {
                    return (void *)(base + rd32(cand + 8));
                }
                p = cand;
            }
        }
    }

    return NULL;
}

void *jit_block_lookup(void *param_1, uint32_t param_2)
{
    unsigned char *ctx = (unsigned char *)param_1;
    uint32_t key = param_2;

    void *cached = jit_block_lookup_cached(ctx, key);
    if (cached != NULL)
        return cached;

    jit_arena_t *ar = CPU(ctx)->jit_arena;
    uint64_t base = (uint64_t)(uintptr_t)ar;
    unsigned char *pbase = (unsigned char *)base;
    jit_hash_entry_t *entry = NULL;
    if (!((key >> 25) == 0 && CPU(ctx)->is_arm9 == 1)) {
        if ((key & 0xff000000u) == 0x02000000u)
            entry = &ar->main_ram_blocks[(key >> 2) & 0x7fffu];
        else
            entry = &ar->other_blocks[(key >> 2) & 0x1fffu];
    }

    uint64_t updated = (uint64_t)(uintptr_t)jit_block_analyze(ctx, key & 0xfffffffeu, key & 1u);
    if (updated == 0) {
        return NULL;
    }
    uint32_t offset_updated = (uint32_t)(updated - base);

    if (entry == NULL) {

        if ((key & 1u) == 0) {
            uint32_t idx = (key >> 2) & 0x1fffu;
            unsigned char *table = (unsigned char *)CPU(ctx)->itcm_arm_blocks;
            wr32(table + (uint64_t)idx * 4u, offset_updated);

            unsigned char *flag = &ar->itcm_arm_hits[idx];
            uint8_t b = *flag;
            if (b >= JIT_ITCM_HOT_BLOCK) {
                *flag = (uint8_t)(b | JIT_ITCM_HAS_VARIANTS);
            }
        } else {
            uint32_t idx = (key >> 1) & 0x3fffu;
            unsigned char *table = (unsigned char *)CPU(ctx)->itcm_thumb_blocks;
            wr32(table + (uint64_t)idx * 4u, offset_updated);
        }
    } else {
        uint32_t t1 = entry->offset;
        if (t1 == 0) {
            entry->pc = key;
            entry->offset = offset_updated;
        } else {
            uint32_t t2 = entry->offset_2;
            if (t2 == 0) {
                entry->pc_2 = key;
                entry->offset_2 = offset_updated;
            } else {
                unsigned char *code2 = pbase + t2;
                uint32_t w = rd32(code2 - 4);
                for (;;) {
                    unsigned char *slot = pbase + w;
                    uint32_t next = rd32(slot);
                    if (next == 0) break;
                    w = next;
                }
                uint32_t self_updated = rd32((unsigned char *)(uintptr_t)updated - 4);
                wr32(pbase + w, self_updated);
            }
        }
    }
    return (void *)(uintptr_t)updated;
}

void jit_cache_refresh_block_info(unsigned char *ctx) {

    unsigned char *desc = CPU(ctx)->jit_block;
    if (desc == NULL) return;

    unsigned char *table = (unsigned char *)CPU(ctx)->jit_arena;

    uint32_t off;
    memcpy(&off, desc - 12, sizeof off);

    unsigned char *addr = table + off;

    uint32_t val;
    memcpy(&val, addr + 4, sizeof val);

    CPU(ctx)->pc = val;
}
#define SHORT_BLOCK 0x18
#define LONG_BLOCK 0x80
#define CHAIN     8
#define LIVE_MASK       0x3fffffffffffffffULL
typedef void *(*fn_memset)(void *, int, uint64_t);
extern void *jit_arena_regions_pair0_23(unsigned char *base) __asm__("jit_arena_regions_pair0");
extern void jit_arena_regions_pair2_23(unsigned char *ctx) __asm__("jit_arena_regions_pair2");
extern void *jit_arena_regions_pair1_23(unsigned char *base) __asm__("jit_arena_regions_pair1");





static void mark_live(uint32_t mask, uint8_t *table, const uint8_t *sub) {
    for (; mask; mask >>= 1, table += 0x100, sub += 4) {
        if (!(mask & 1)) continue;
        uint32_t m2 = rd32(sub);
        for (uint64_t o = 0; m2; m2 >>= 1, o += 8) {
            if (!(m2 & 1)) continue;
            int64_t v = (int64_t)rd64(table + o);
            if (v < 0) wr64(table + o, (uint64_t)v & LIVE_MASK);
        }
    }
}

static uint8_t *bus_code_bits_after_main_ram(bus_t *bus, uint32_t j) {
    if (j == 0) return bus->shared_wram_code_bits;
    if (j == 1) return bus->arm7_wram_code_bits;
    return bus->vram_code_bits + (size_t)(j - 2) * 0x1000;
}

static void clear_rows(uint32_t mask, uint8_t *rows) {
    for (; mask; mask >>= 1, rows += 128)
        if (mask & 1) memset(rows, 0, 128);
}

static void sweep_groups(uint8_t *c) {
    for (uint32_t k = 0; k < SHORT_BLOCK; k++) {
        uint32_t m = PT(c)->group_bits[k];
        if (m) mark_live(m, (uint8_t *)&PT(c)->page[(size_t)k * PAGETABLE_GROUP_PAGES],
                      (uint8_t *)&PT(c)->page_bits[(size_t)k * PAGETABLE_GROUP_BIT_WORDS]);
    }
}

static void sweep_chain(uint8_t *c) {
    for (uint32_t j = 0; j < CHAIN; j++) {
        uint32_t m = PT(c)->group_bits[PAGETABLE_GROUPS_BELOW_MAIN_RAM + j];
        if (m) mark_live(m, (uint8_t *)&PT(c)->page[(size_t)j * PAGETABLE_GROUP_PAGES],
                      (uint8_t *)&PT(c)->page_bits[(size_t)j * PAGETABLE_GROUP_BIT_WORDS]);
    }
}

int32_t jit_cache_flush(uint8_t *ctx, uint32_t arg) {

    uint8_t *mem  = (uint8_t *)CPU(ctx)->bus;
    uint8_t *other = (uint8_t *)CPU(ctx)->partner;
    uint32_t f = arg;

    ((fn_memset)sym_libc_memset)(CPU(ctx)->block_lookup, 0,
                                  sizeof CPU(ctx)->block_lookup);
    ((fn_memset)sym_libc_memset)(CPU(other)->block_lookup, 0,
                                  sizeof CPU(other)->block_lookup);
    ((fn_memset)sym_libc_memset)(jit_block_lookup_secondary, 0,
                                  sizeof jit_block_lookup_secondary);

    int short_form = 0;
    if (((f >> 24) & 0xffu) == 2) short_form = 1;
    else if (f <= 0x2000000u && CPU(ctx)->is_arm9 == 1) short_form = 1;

    if (!short_form) {

        uint32_t pr = CPU(ctx)->is_arm9;
        ((fn_memset)sym_libc_memset)(
            CPU(ctx)->jit_arena->other_blocks, 0, sizeof CPU(ctx)->jit_arena->other_blocks);
        uint8_t *primary = (pr == 1) ? ctx : other;
        uint8_t *per = (pr == 1) ? other : ctx;

        for (uint32_t k = SHORT_BLOCK, b = 0; k < LONG_BLOCK; k++, b++) {
            uint32_t m = PT(ctx)->group_bits[k];
            if (m) mark_live(m, (uint8_t *)&PT(ctx)->page[(size_t)b * PAGETABLE_GROUP_PAGES],
                          (uint8_t *)&PT(ctx)->page_bits[(size_t)b * PAGETABLE_GROUP_BIT_WORDS]);
        }
        for (uint32_t k = SHORT_BLOCK, b = 0; k < LONG_BLOCK; k++, b++) {
            uint32_t m = PT(other)->group_bits[k];
            if (m) mark_live(m, (uint8_t *)&PT(other)->page[(size_t)b * PAGETABLE_GROUP_PAGES],
                          (uint8_t *)&PT(other)->page_bits[(size_t)b * PAGETABLE_GROUP_BIT_WORDS]);
        }

        uint32_t *tail_page_bits = &((bus_t *)mem)->shared_wram_page_bits;
        for (uint32_t j = 0; j < 2 + BUS_VRAM_BITMAP_WORDS; j++) {
            uint32_t m = tail_page_bits[j];
            tail_page_bits[j] = 0;
            if (m) clear_rows(m, bus_code_bits_after_main_ram((bus_t *)mem, j));
        }

        if (((bus_t *)mem)->slot2_rom != NULL) {
            uint8_t *rows = ((bus_t *)mem)->slot2_code_bits;
            uint8_t *mask  = ((bus_t *)mem)->slot2_page_bits;
            for (uint32_t k = 0; k < 0x200; k++) {
                uint32_t m = rd32(mask + (size_t)k * 4);
                wr32(mask + (size_t)k * 4, 0);
                if (m) clear_rows(m, rows + (size_t)k * 0x1000);
            }
        }

        jit_arena_regions_pair2((unsigned char *)CPU(ctx)->jit_arena);

        uint8_t *bl = CPU(other)->jit_block;
        if (bl) {
            uint32_t pc = CPU(other)->pc;
            if ((pc >> 24) != 2
                && (pc > 0x2000000u || CPU(other)->is_arm9 != 1)) {
                uint8_t *a = (uint8_t *)CPU(other)->jit_arena;
                uint32_t d = rd32(bl - 12);
                uint32_t n = rd32(a + d + 4);
                CPU(other)->pc = n;
                uint8_t *r = jit_cache_lookup_or_compile(other, n);
                CPU(other)->jit_block = r + 8;
            }
        }

        CPU(per)->jit_swi_entry = (uint8_t *)jit_cache_lookup_or_compile(per, 8);
        CPU(per)->jit_irq_entry = (uint8_t *)jit_cache_lookup_or_compile(per, 0x18);
        uint32_t c = CPU(primary)->cp15->exception_vector_base;
        CPU(primary)->jit_swi_entry = (uint8_t *)jit_cache_lookup_or_compile(primary, c + 8);
        c = CPU(primary)->cp15->exception_vector_base;
        CPU(primary)->jit_irq_entry = (uint8_t *)jit_cache_lookup_or_compile(primary, c + 0x18);

        if (f != 0xffffffffu) return 1;
    }

    {
        int is_one = (CPU(ctx)->is_arm9 == 1);
        uint8_t *q = is_one ? ctx : other;
        uint8_t *m2 = (uint8_t *)CPU(q)->bus;
        jit_arena_t *ar = CPU(q)->jit_arena;

        ((fn_memset)sym_libc_memset)(((bus_t *)m2)->itcm_code_bits, 0, 0x800);
        for (uint32_t i = 0; i < 0x2000; i++) {
            wr32(&CPU(q)->itcm_arm_blocks[i], 0);
            ar->itcm_arm_hits[i] = (uint8_t)(ar->itcm_arm_hits[i] & (uint8_t)~JIT_ITCM_HAS_VARIANTS);
        }
        ((fn_memset)sym_libc_memset)(CPU(q)->itcm_thumb_blocks, 0, 0x10000);
        ar->itcm_variant_count = 0;
        jit_arena_regions_pair1((unsigned char *)ar);

        if (is_one && (f >> 25) == 0)
            return (CPU(ctx)->pc >> 25) == 0;
    }

    ((fn_memset)sym_libc_memset)(
        CPU(ctx)->jit_arena->main_ram_blocks, 0, sizeof CPU(ctx)->jit_arena->main_ram_blocks);

    if (CPU(ctx)->is_arm9 == 1) {
        sweep_groups(ctx);
        sweep_chain(other);
    } else {
        sweep_chain(ctx);
        sweep_groups(other);
    }

    for (uint32_t k = 0; k < 0x40; k++) {
        uint32_t m = ((bus_t *)mem)->main_ram_page_bits[k];
        ((bus_t *)mem)->main_ram_page_bits[k] = 0;
        if (m) clear_rows(m, ((bus_t *)mem)->main_ram_code_bits + (size_t)k * 0x1000);
    }
    {
        uint32_t m = ((bus_t *)mem)->itcm_page_bits;
        ((bus_t *)mem)->itcm_page_bits = 0;
        if (m) clear_rows(m, (uint8_t *)((bus_t *)mem)->itcm_code_bits);
    }

    jit_arena_regions_pair0((unsigned char *)CPU(ctx)->jit_arena);

    {
        uint8_t *bl = CPU(other)->jit_block;
        if (bl) {
            uint32_t pc = CPU(other)->pc;
            int needed = ((pc >> 24) == 2);
            if (!needed && (pc >> 25) == 0 && CPU(other)->is_arm9 == 1) needed = 1;
            if (needed) {
                uint8_t *a = (uint8_t *)CPU(other)->jit_arena;
                uint32_t d = rd32(bl - 12);
                uint32_t n = rd32(a + d + 4);
                CPU(other)->pc = n;
                uint8_t *r = jit_cache_lookup_or_compile(other, n);
                CPU(other)->jit_block = r + 8;
            }
        }
    }
    return 1;
}
#undef SHORT_BLOCK
#undef LONG_BLOCK
#undef CHAIN
#undef LIVE_MASK
#define IMM26    0x03ffffffu




void jit_link_pending_branches(void *ctx)
{
    jit_arena_t *ar = CPU(ctx)->jit_arena;
    uint32_t n = ar->pending_branch_count;
    if (n == 0) return;

    jit_pending_branch_t *p = ar->pending_branch;
    for (;;) {

        uint8_t *dest = jit_cache_lookup_or_compile(ctx, p->target_pc);
        if (dest) {

            uint8_t *ins = p->instruction;
            uint32_t v   = rd32(ins);
            uint32_t d   = (uint32_t)(dest - ins);
            wr32(ins, (v & ~IMM26) | ((d >> 2) & IMM26));
        }
        if (--n == 0) return;
        p++;
    }
}
#undef IMM26
#define MARK 0x36f8004cu


uint32_t jit_host_to_ds_addr(void *pc, void *base)
{
    const uint8_t *p = (const uint8_t *)pc + 4;
    const uint8_t *b = (const uint8_t *)base;

    do { p -= 4; } while (rd32(p) != MARK);

    uint32_t shift = rd32(p - 4);
    uint64_t n = (uint64_t)((const uint8_t *)pc - p) >> 2;

    const uint8_t *tab = b + shift;
    const uint8_t *e   = b + rd32(tab + 20);
    uint32_t w;
    do { w = rd32(e); e += 4; } while ((w >> 16) != (uint32_t)n);

    return rd32(tab + 4) + (w & 0xffffu);
}
#undef MARK

void *jit_arena_regions_pair0(unsigned char *base) {
    jit_arena_t *ar = (jit_arena_t *)base;
    ar->region[JIT_REGION_MAIN_RAM].code = ar->code_main_ram;
    ar->region[JIT_REGION_MAIN_RAM].table = ar->code_main_ram + sizeof ar->code_main_ram;
    return base;
}

void jit_arena_regions_pair2(unsigned char *ctx) {
    jit_arena_t *ar = (jit_arena_t *)ctx;
    unsigned char *a = ar->code_other;
    unsigned char *b = ar->code_other + sizeof ar->code_other;
    memcpy(&ar->region[JIT_REGION_OTHER].code, &a, 8);
    memcpy(&ar->region[JIT_REGION_OTHER].table, &b, 8);
}

void *jit_arena_regions_pair1(unsigned char *base) {
    jit_arena_t *ar = (jit_arena_t *)base;
    ar->region[JIT_REGION_ITCM].code = ar->code_itcm;
    ar->region[JIT_REGION_ITCM].table = ar->code_itcm + sizeof ar->code_itcm;
    return base;
}
static int (*core_mprotect)(void *, size_t, int);

void jit_arena_protect_rwx(unsigned char *base) {
    if (!core_mprotect)
        core_mprotect = (int (*)(void *, size_t, int))sym_libc_mprotect;

    jit_arena_t *arena = (jit_arena_t *)base;

#if defined(__linux__) && defined(MADV_HUGEPAGE)
    /* The AArch64 JIT walks a large, instruction-dense code arena. Encourage
     * Linux THP for the contiguous code buffers to reduce iTLB pressure on
     * small cores such as Cortex-A55. This is only an advice hint: kernels
     * without THP support or available huge pages keep the regular mapping. */
    (void)madvise(arena->code_main_ram,
                  JIT_CODE_MAIN_RAM_SIZE + JIT_CODE_ITCM_SIZE + JIT_CODE_OTHER_SIZE,
                  MADV_HUGEPAGE);
#endif

    ((volatile uint64_t *)arena->unmapped_2)[0] = 0;
    ((volatile uint64_t *)arena->unmapped_2)[1] = 0;

    core_mprotect(arena->code_main_ram, JIT_CODE_MAIN_RAM_SIZE, 7);
    core_mprotect(arena->code_itcm, JIT_CODE_ITCM_SIZE, 7);
}

void jit_arena_regions_init(unsigned char *arena) {
    jit_arena_t *ar = (jit_arena_t *)arena;
    ar->region[JIT_REGION_MAIN_RAM].code = ar->code_main_ram;
    ar->region[JIT_REGION_MAIN_RAM].table = ar->code_main_ram + sizeof ar->code_main_ram;
    ar->region[JIT_REGION_ITCM].code = ar->code_itcm;
    ar->region[JIT_REGION_ITCM].table = ar->code_itcm + sizeof ar->code_itcm;
    ar->region[JIT_REGION_OTHER].code = ar->code_other;
    ar->region[JIT_REGION_OTHER].table = ar->code_other + sizeof ar->code_other;
}
#undef STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef ENTRY_STRIDE
#undef HANDLER_OFF
#undef SHORT_BLOCK
#undef LONG_BLOCK
#undef CHAIN
#undef LIVE_MASK
#undef IMM26
#undef MARK

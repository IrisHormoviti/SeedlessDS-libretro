/* Single-process libretro frontend for repeatable ROM capture. Each invocation
 * boots one ROM and samples the core only at scheduler pause boundaries. */
#include <libretro.h>
#include "seedlessds/control.h"
#include "compute.h"
#include "core_internals.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *root_dir, *renderer;
static unsigned joypad;

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = root_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;
        var->value = NULL;
        if (!strcmp(var->key, "seedlessds_renderer")) var->value = renderer;
        if (!strcmp(var->key, "seedlessds_rtc_system_time")) var->value = "disabled";
        return var->value != NULL;
    }
    default:
        return false;
    }
}

static void poll_input(void) {}
static int16_t read_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0 && id < 16
        ? (joypad >> id) & 1u : 0;
}

static void discard_video(const void *pixels, unsigned w, unsigned h, size_t pitch)
{ (void)pixels; (void)w; (void)h; (void)pitch; }

static size_t discard_audio(const int16_t *samples, size_t frames)
{ (void)samples; return frames; }

static bool wait_until_paused(uint64_t target, nds_control_status_t *status)
{
    const struct timespec wait = { .tv_nsec = 5000000 };
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        nds_control_status(status);
        if (status->paused && status->frames == target) return true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (status->error || status->frames > target || now.tv_sec - started.tv_sec >= 120) {
            fprintf(stderr, "Replay timed out or stopped at frame %"PRIu64" while waiting for %"PRIu64"\n",
                    status->frames, target);
            return false;
        }
        nanosleep(&wait, NULL);
    }
}

static bool capture_at(int fd, uint64_t target)
{
    nds_control_status_t status;
    if (!wait_until_paused(target, &status)) return false;
    if (!nds_control_snapshot(fd, 0)) {
        fprintf(stderr, "Snapshot failed at scheduler frame %"PRIu64"\n", target);
        return false;
    }
    fprintf(stderr, "Snapshot at scheduler frame %"PRIu64", output frame %"PRIu64"\n",
            status.frames, status.output_frames);
    return true;
}

static double elapsed(struct timespec begin, struct timespec end)
{
    return (double)(end.tv_sec - begin.tv_sec) +
        (double)(end.tv_nsec - begin.tv_nsec) / 1e9;
}

static void init_frontend(retro_video_refresh_t video, unsigned compose_threads)
{
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(discard_audio);
    retro_set_input_poll(poll_input);
    retro_set_input_state(read_input);
    retro_init();
    nds_control_begin(946684800, 1, compose_threads);
}

static struct timespec first_video;
static bool received_video;

static void time_first_video(const void *pixels, unsigned w, unsigned h, size_t pitch)
{
    (void)w; (void)h; (void)pitch;
    if (pixels && !received_video) {
        clock_gettime(CLOCK_MONOTONIC, &first_video);
        received_video = true;
    }
}

static bool benchmark(const struct retro_game_info *game, uint64_t warmup, uint64_t frames)
{
    nds_control_status_t status;
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    if (!nds_control_run_to(1)) return false;
#if defined(SEEDLESS_JIT_PROFILE)
    jit_profile_reset();
#endif
    if (!retro_load_game(game) || !wait_until_paused(1, &status)) return false;
    retro_run();
    if (!received_video) {
        fprintf(stderr, "Core did not deliver video at the first scheduler frame\n");
        return false;
    }
    fprintf(stderr, "ROM load to first replay video callback: %.2f ms\n",
            elapsed(begin, first_video) * 1000);
#if defined(SEEDLESS_JIT_PROFILE)
    uint64_t boot_blocks, boot_analysis_us;
    jit_profile_read(&boot_blocks, &boot_analysis_us);
    fprintf(stderr, "Cold-start JIT analysis: %"PRIu64" blocks, %"PRIu64
            " us total (%.2f us/block)\n", boot_blocks, boot_analysis_us,
            boot_blocks ? (double)boot_analysis_us / (double)boot_blocks : 0.0);
#endif

    if (warmup && (!nds_control_run_to(1 + warmup) ||
                   !wait_until_paused(1 + warmup, &status))) return false;
#if defined(SEEDLESS_JIT_PROFILE)
    uint64_t warmup_blocks, warmup_analysis_us;
    jit_profile_read(&warmup_blocks, &warmup_analysis_us);
    fprintf(stderr, "JIT analysis after warmup: %"PRIu64" blocks, %"PRIu64
            " us total\n", warmup_blocks - boot_blocks,
            warmup_analysis_us - boot_analysis_us);
#endif
#if defined(SEEDLESS_COMPUTE_PROFILE)
    compute_profile_reset();
#endif
    uint64_t output_start = status.output_frames;
    uint64_t dispatch_start = status.dispatches;
    uint64_t target = status.frames + frames;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    if (!nds_control_run_to(target) || !wait_until_paused(target, &status)) return false;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds = elapsed(begin, end);
    fprintf(stderr, "Scheduler throughput: %"PRIu64" frames in %.3f s (%.2f emulated frames/s)\n",
            frames, seconds, frames / seconds);
    fprintf(stderr, "Core output frames: %"PRIu64"; 3D dispatches: %"PRIu64"\n",
            status.output_frames - output_start, status.dispatches - dispatch_start);
#if defined(SEEDLESS_COMPUTE_PROFILE)
    compute_profile_t gpu;
    compute_profile_read(&gpu);
    if (gpu.draws) {
        double scale = 1.0 / (double)gpu.draws / 1000.0;
        fprintf(stderr,
                "GLES per-draw CPU time: upload %.2f us, dispatch submit %.2f us, "
                "map wait %.2f us, copy/unmap %.2f us (%"PRIu64" draws)\n",
                gpu.upload_ns * scale, gpu.submit_ns * scale,
                gpu.map_wait_ns * scale, gpu.copy_ns * scale, gpu.draws);
    } else {
        fprintf(stderr, "GLES profiling: no compute draws in measured interval\n");
    }
#endif
    return true;
}

int main(int argc, char **argv)
{
    bool bench = argc > 1 && !strcmp(argv[1], "--bench");
    if (bench && (argc != 7 || (strcmp(argv[4], "software") && strcmp(argv[4], "compute")))) {
        fprintf(stderr, "Usage: %s --bench ROM SYSTEM_DIR software|compute WARMUP_FRAMES MEASURE_FRAMES\n", argv[0]);
        return 2;
    }
    if (bench) {
        char *end;
        errno = 0;
        uint64_t warmup = strtoull(argv[5], &end, 10);
        if (end == argv[5] || *end || errno) return 2;
        errno = 0;
        uint64_t frames = strtoull(argv[6], &end, 10);
        if (end == argv[6] || *end || errno || frames == 0 || frames > UINT32_MAX || warmup > UINT32_MAX)
            return 2;
        root_dir = argv[3];
        renderer = argv[4];
        char *rom_path = realpath(argv[2], NULL);
        if (!rom_path) { perror(argv[2]); return 1; }
        init_frontend(time_first_video, ~0u);
        nds_control_set_unpaced(1);
        struct retro_game_info game = { .path = rom_path };
        bool ok = benchmark(&game, warmup, frames);
        nds_control_pause(0);
        retro_deinit();
        nds_control_end();
        free(rom_path);
        return ok ? 0 : 1;
    }
    if (argc < 6 ||
        (strcmp(argv[3], "software") && strcmp(argv[3], "compute") && strcmp(argv[3], "verify"))) {
        fprintf(stderr, "Usage: %s ROM SYSTEM_DIR software|compute|verify NEW_CAPTURE FRAME[:JOY_MASK]...\n", argv[0]);
        return 2;
    }
    root_dir = argv[2];
    renderer = argv[3];
    uint64_t previous = 0;
    for (int i = 5; i < argc; i++) {
        char *end;
        errno = 0;
        uint64_t frame = strtoull(argv[i], &end, 10);
        if (end == argv[i] || errno || frame <= previous || frame > UINT32_MAX) return 2;
        if (*end == ':') {
            const char *start = end+1;
            unsigned long mask = strtoul(start, &end, 0);
            if (end == start || mask > UINT16_MAX || errno) return 2;
        }
        if (*end) return 2;
        if (i == 5 && strchr(argv[i], ':')) {
            fprintf(stderr, "Start with a neutral checkpoint before changing input\n");
            return 2;
        }
        previous = frame;
    }
    char *rom_path = realpath(argv[1], NULL);
    if (!rom_path) { perror(argv[1]); return 1; }
    int fd = open(argv[4], O_WRONLY|O_CREAT|O_EXCL, 0644);
    if (fd < 0) {
        perror(argv[4]);
        free(rom_path);
        return 1;
    }
    init_frontend(discard_video, 0);

    struct retro_game_info game = { .path = rom_path };
    uint64_t first = strtoull(argv[5], NULL, 10);
    bool ok = nds_control_run_to(first) && retro_load_game(&game) && capture_at(fd, first);
    for (int i = 6; ok && i < argc; i++) {
        char *end;
        uint64_t frame = strtoull(argv[i], &end, 10);
        if (*end == ':') {
            joypad = (unsigned)strtoul(end+1, NULL, 0);
            retro_run(); /* Applies the input while the scheduler is paused. */
        }
        ok = nds_control_run_to(frame) && capture_at(fd, frame);
    }
    if (ok && !strcmp(renderer, "verify") && !compute_report()) {
        fprintf(stderr, "Verification found a mismatch or compared zero 3D frames\n");
        ok = false;
    }
    nds_control_pause(0);
    retro_deinit();
    nds_control_end();
    close(fd);
    free(rom_path);
    return ok ? 0 : 1;
}

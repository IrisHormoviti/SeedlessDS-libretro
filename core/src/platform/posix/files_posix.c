#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/data_paths.h"

static const char *root(void)
{
    const char *r = getenv("SEEDLESS_ROOT");
    return (r && *r) ? r : ".";
}

static void resolve(char *dest, size_t size, const char *path)
{
    const char *f = path ? path : "";
    const size_t n = sizeof(DATA_SYSTEM_DIR "/") - 1;
    if (!strncmp(f, DATA_SYSTEM_DIR "/", n)) f += n;
    if (f[0] == '/') snprintf(dest, size, "%s", f);
    else             snprintf(dest, size, "%s/%s", root(), f);
}

static int open_flags_for_mode(const char *m)
{
    if (!m || !*m) return O_RDONLY;
    int plus = strchr(m, '+') != NULL;
    switch (m[0]) {
        case 'w': return plus ? (O_RDWR | O_CREAT | O_TRUNC)  : (O_WRONLY | O_CREAT | O_TRUNC);
        case 'a': return plus ? (O_RDWR | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_APPEND);
        case 'r':
        default:  return plus ? O_RDWR : O_RDONLY;
    }
}

static void create_parents(const char *path)
{
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    char *bar = strrchr(t, '/');
    if (!bar) return;
    *bar = 0;
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}

void files_descriptor_free(void **obj)
{
    free(obj[0]);
    free(obj[2]);
    free(obj);
}

void *files_jni_call_static_method_build_result(const char *arg0, const char *arg1)
{
    char complete[1024];
    const char *mode = arg1 ? arg1 : "rb";
    resolve(complete, sizeof complete, arg0);

    int flags = open_flags_for_mode(mode);
    if (flags & O_CREAT) create_parents(complete);

    int fd = open(complete, flags, 0644);
    if (fd < 0) return 0;

    unsigned char *r = (unsigned char *)malloc(24);
    if (!r) { close(fd); return 0; }
    *(void **)(r + 0)     = strdup(complete);
    *(uint32_t *)(r + 8)  = (uint32_t)fd;
    *(void **)(r + 16)    = strdup(complete);
    return r;
}

uint32_t files_jni_env_invoke_and_release(const char *arg0, const char *arg1)
{
    /* The DraStic state writer finishes by renaming the temporary file
     * through the Java-style files shim. The POSIX platform left this as a
     * no-op, so <rom>_0.dss was never created and libretro savestates could
     * not complete. Actually rename it; resolve() keeps absolute paths and
     * handles the DATA_* prefixes used by other callers. */
    char src[1024];
    char dst[1024];
    resolve(src, sizeof src, arg0);
    resolve(dst, sizeof dst, arg1);
    return rename(src, dst) == 0 ? 0 : 1;
}

uint32_t files_jni_env_invoke_alternate(void *arg)
{
    (void)arg;
    return 0;
}

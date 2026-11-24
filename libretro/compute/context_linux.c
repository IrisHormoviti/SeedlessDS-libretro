#include "context.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#ifdef SEEDLESS_TEST_EGL
#include <stdlib.h>
#endif
#include <unistd.h>
#include <string.h>

/* Give EGL a distinct GBM device so eglTerminate cannot tear down the
 * frontend's display. Use render nodes, which don't require DRM master. */
struct gbm_device;
static struct {
    void *lib;
    struct gbm_device *gbm;
    void (*destroy)(struct gbm_device *);
    int fd;
    EGLDisplay display;
    EGLContext context;
} device = { .fd = -1 };

bool compute_context_bind(void)
{
    return eglMakeCurrent(device.display, EGL_NO_SURFACE, EGL_NO_SURFACE, device.context);
}

void compute_context_release(void)
{
    eglMakeCurrent(device.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void close_node(void)
{
    if (device.context) eglDestroyContext(device.display, device.context);
    if (device.display) eglTerminate(device.display);
    if (device.gbm) device.destroy(device.gbm);
    if (device.fd >= 0) close(device.fd);
    device.context = EGL_NO_CONTEXT;
    device.display = EGL_NO_DISPLAY;
    device.gbm = NULL;
    device.fd = -1;
}

void compute_context_close(void)
{
    close_node();
    if (device.lib) dlclose(device.lib);
    device.lib = NULL;
}

static bool open_display(void)
{
    const EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, 0, EGL_NONE
    };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLConfig config;
    EGLint n;
    if (!device.display || !eglInitialize(device.display, NULL, NULL) ||
        !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(device.display, config_attrs, &config, 1, &n) || !n)
        return false;
    device.context = eglCreateContext(device.display, config, EGL_NO_CONTEXT, context_attrs);
    return device.context && compute_context_bind();
}

bool compute_context_open(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
#ifdef SEEDLESS_TEST_EGL
    if (platform && getenv("SEEDLESS_TEST_SURFACELESS")) {
        device.display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (open_display()) {
            fprintf(stderr, "[SeedlessDS compute] standalone surfaceless EGL test\n");
            return true;
        }
        goto fail;
    }
#endif
    device.lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!device.lib || !platform) goto fail;
    struct gbm_device *(*create)(int) = dlsym(device.lib, "gbm_create_device");
    device.destroy = dlsym(device.lib, "gbm_device_destroy");
    if (!create || !device.destroy) goto fail;

    glob_t nodes = {0};
    if (glob("/dev/dri/renderD*", 0, NULL, &nodes) != 0) { globfree(&nodes); goto fail; }
    for (size_t i = 0; i < nodes.gl_pathc; i++) {
        device.fd = open(nodes.gl_pathv[i], O_RDWR | O_CLOEXEC);
        if (device.fd < 0) continue;
        device.gbm = create(device.fd);
        if (device.gbm)
            device.display = platform(EGL_PLATFORM_GBM_KHR, device.gbm, NULL);
        if (open_display()) {
            fprintf(stderr, "[SeedlessDS compute] EGL on %s\n", nodes.gl_pathv[i]);
            globfree(&nodes);
            return true;
        }
        close_node();
    }
    globfree(&nodes);
fail:
    fprintf(stderr, "[SeedlessDS compute] could not open a GBM render node and surfaceless EGL context\n");
    compute_context_close();
    return false;
}

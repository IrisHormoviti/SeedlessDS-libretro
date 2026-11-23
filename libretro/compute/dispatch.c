#include "commands.h"
#include "context.h"
#include "raster_shader.h"
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <stdio.h>
#include <string.h>

/* The legacy libretro target links gl_stubs.c with -Bsymbolic. Resolve all
 * entry points from this EGL implementation, including ES 2.0 functions. */
#define FUNCTIONS(X) \
    X(CREATESHADER, CreateShader) X(SHADERSOURCE, ShaderSource) \
    X(COMPILESHADER, CompileShader) X(GETSHADERIV, GetShaderiv) \
    X(GETSHADERINFOLOG, GetShaderInfoLog) X(DELETESHADER, DeleteShader) \
    X(CREATEPROGRAM, CreateProgram) X(ATTACHSHADER, AttachShader) \
    X(LINKPROGRAM, LinkProgram) X(GETPROGRAMIV, GetProgramiv) \
    X(GETPROGRAMINFOLOG, GetProgramInfoLog) X(DELETEPROGRAM, DeleteProgram) \
    X(GENBUFFERS, GenBuffers) X(DELETEBUFFERS, DeleteBuffers) \
    X(BINDBUFFER, BindBuffer) X(BINDBUFFERBASE, BindBufferBase) \
    X(BUFFERDATA, BufferData) X(BUFFERSUBDATA, BufferSubData) \
    X(USEPROGRAM, UseProgram) X(GETUNIFORMLOCATION, GetUniformLocation) \
    X(UNIFORM1UI, Uniform1ui) X(DISPATCHCOMPUTE, DispatchCompute) \
    X(MEMORYBARRIER, MemoryBarrier) X(MAPBUFFERRANGE, MapBufferRange) \
    X(UNMAPBUFFER, UnmapBuffer) X(GETERROR, GetError) X(GETSTRING, GetString)
#define MEMBER(type,name) PFNGL##type##PROC name;
static struct { FUNCTIONS(MEMBER) } gl;
static GLuint program, buffers[4];
static size_t capacity[4];
static GLint clear_loc, depth_loc, alpha_loc, effects_loc, coef_loc;

static bool init_resources(void)
{
#define LOAD(type,name) gl.name = (PFNGL##type##PROC)eglGetProcAddress("gl" #name); if (!gl.name) return false;
    FUNCTIONS(LOAD)
    fprintf(stderr, "[SeedlessDS compute] %s, %s\n", gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION));
    GLuint shader = gl.CreateShader(GL_COMPUTE_SHADER);
    if (!shader) return false;
    const char *source = raster_shader;
    gl.ShaderSource(shader, 1, &source, NULL);
    gl.CompileShader(shader);
    GLint ok = 0;
    char log[2048] = {0};
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[SeedlessDS compute] shader compilation failed: %s\n", log);
        gl.DeleteShader(shader);
        return false;
    }
    program = gl.CreateProgram();
    gl.AttachShader(program, shader);
    gl.LinkProgram(program);
    gl.DeleteShader(shader);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        gl.GetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "[SeedlessDS compute] program linkage failed: %s\n", log);
        return false;
    }
    clear_loc = gl.GetUniformLocation(program, "clearColor");
    depth_loc = gl.GetUniformLocation(program, "clearDepth");
    alpha_loc = gl.GetUniformLocation(program, "alphaRef");
    effects_loc = gl.GetUniformLocation(program, "effects");
    coef_loc = gl.GetUniformLocation(program, "coefBase");
    gl.GenBuffers(4, buffers);
    for (unsigned i = 0; i < 4; i++)
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[3]);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, SCREEN_W*SCREEN_H*8, NULL, GL_STREAM_READ);
    gl.UseProgram(program);
    return gl.GetError() == GL_NO_ERROR;
}

bool compute_gl_init(void)
{
    if (!compute_context_open()) return false;
    bool ok = init_resources();
    compute_context_release();
    if (!ok) compute_context_close();
    return ok;
}

void compute_gl_close(void)
{
    /* Destroying the private context owns and frees all of its GL objects. */
    compute_context_close();
    memset(capacity, 0, sizeof(capacity));
}

static bool reserve_buffer(unsigned index, size_t bytes)
{
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[index]);
    if (bytes > capacity[index] || !capacity[index]) {
        size_t n = 256;
        while (n < bytes) n *= 2;
        gl.BufferData(GL_SHADER_STORAGE_BUFFER, n, NULL, GL_STREAM_DRAW);
        capacity[index] = n;
        return true;
    }
    return false;
}

static void upload(unsigned index, const void *src, size_t bytes)
{
    reserve_buffer(index, bytes);
    if (bytes) gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, bytes, src);
}

bool compute_gl_draw(const compute_span *spans, unsigned count, const uint32_t *bins, unsigned bin_words,
                     const uint32_t *arena, unsigned words, unsigned dirty_first, unsigned dirty_end,
                     const uint32_t *coefficients, unsigned coef_words, uint32_t clear,
                     uint32_t depth, uint32_t alpha, bool effects, void *dst)
{
    if (!compute_context_bind()) return false;
    upload(0, spans, count*sizeof(*spans));
    upload(1, bins, bin_words*sizeof(*bins));
    if (reserve_buffer(2, (words+coef_words)*sizeof(*arena))) {
        dirty_first = 0;
        dirty_end = words;
    }
    if (dirty_end > words) dirty_end = words;
    if (dirty_first < dirty_end)
        gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, dirty_first*4, (dirty_end-dirty_first)*4,
                         arena+dirty_first);
    if (coef_words)
        gl.BufferSubData(GL_SHADER_STORAGE_BUFFER, words*4, coef_words*4, coefficients);
    gl.Uniform1ui(coef_loc, words);
    gl.Uniform1ui(clear_loc, clear);
    gl.Uniform1ui(depth_loc, depth);
    gl.Uniform1ui(alpha_loc, alpha);
    gl.Uniform1ui(effects_loc, effects);
    bool ok = gl.GetError() == GL_NO_ERROR;
    if (ok) {
        gl.DispatchCompute(SCREEN_W/8, SCREEN_H/8, 1);
        gl.MemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[3]);
        size_t bytes = SCREEN_W*SCREEN_H*(effects ? 8 : 4);
        const void *p = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        ok = p != NULL;
        if (p) {
            memcpy(dst, p, bytes);
            ok = gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER) == GL_TRUE;
        }
    }
    GLenum error = gl.GetError();
    compute_context_release();
    if (!ok || error) fprintf(stderr, "[SeedlessDS compute] dispatch/readback failed (GL %#x)\n", error);
    return ok && !error;
}

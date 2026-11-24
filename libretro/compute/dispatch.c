#include "commands.h"
#include "context.h"
#include "raster_shader.h"
#include "edges_shader.h"
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
    X(UNIFORM1UI, Uniform1ui) X(UNIFORM1UIV, Uniform1uiv) X(DISPATCHCOMPUTE, DispatchCompute) \
    X(MEMORYBARRIER, MemoryBarrier) X(MAPBUFFERRANGE, MapBufferRange) \
    X(UNMAPBUFFER, UnmapBuffer) X(GETERROR, GetError) X(GETSTRING, GetString)
#define MEMBER(type,name) PFNGL##type##PROC name;
static struct { FUNCTIONS(MEMBER) } gl;
typedef struct {
    GLuint id;
    GLint clear, depth, alpha, effects, coef;
} raster_program;
static raster_program raster[2];
static GLuint buffers[5];
static GLuint edge_program;
static GLint edge_depth, edge_palette;
static size_t capacity[4];

static GLuint compile_program(const char *body, bool textured)
{
    GLuint shader = gl.CreateShader(GL_COMPUTE_SHADER);
    if (!shader) return 0;
    char config[192];
    snprintf(config, sizeof(config),
             "#version 310 es\n#define TEXTURED %u\n#define WORKGROUP_X %u\n"
             "#define WORKGROUP_Y %u\n#define BIN_WIDTH %u\n#define BINS_PER_ROW %u\n"
             "#define BIN_BASE %u\n",
             (unsigned)textured, (unsigned)WORKGROUP_X, (unsigned)WORKGROUP_Y,
             (unsigned)BIN_WIDTH, (unsigned)BINS_PER_ROW, (unsigned)(BIN_COUNT+1));
    const char *source[] = { config, body };
    gl.ShaderSource(shader, 2, source, NULL);
    gl.CompileShader(shader);
    GLint ok = 0;
    char log[2048] = {0};
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[SeedlessDS compute] shader compilation failed: %s\n", log);
        gl.DeleteShader(shader);
        return 0;
    }
    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, shader);
    gl.LinkProgram(program);
    gl.DeleteShader(shader);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        gl.GetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "[SeedlessDS compute] program linkage failed: %s\n", log);
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

static bool init_program(raster_program *out, bool textured)
{
    GLuint program = compile_program(raster_shader, textured);
    if (!program) return false;
    *out = (raster_program){
        program, gl.GetUniformLocation(program, "clearColor"),
        gl.GetUniformLocation(program, "clearDepth"),
        gl.GetUniformLocation(program, "alphaRef"),
        gl.GetUniformLocation(program, "effects"), gl.GetUniformLocation(program, "coefBase")
    };
    return gl.GetError() == GL_NO_ERROR;
}

static bool init_edges(void)
{
    edge_program = compile_program(edges_shader, false);
    if (!edge_program) return false;
    edge_depth = gl.GetUniformLocation(edge_program, "clearDepth");
    edge_palette = gl.GetUniformLocation(edge_program, "edgeColors[0]");
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[4]);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, SCREEN_W*SCREEN_H*4, NULL, GL_STREAM_READ);
    return gl.GetError() == GL_NO_ERROR;
}

static bool init_resources(void)
{
#define LOAD(type,name) gl.name = (PFNGL##type##PROC)eglGetProcAddress("gl" #name); if (!gl.name) return false;
    FUNCTIONS(LOAD)
    fprintf(stderr, "[SeedlessDS compute] %s, %s\n", gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION));
    gl.GenBuffers(5, buffers);
    for (unsigned i = 0; i < 4; i++)
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[3]);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, SCREEN_W*SCREEN_H*8, NULL, GL_STREAM_READ);
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
    memset(raster, 0, sizeof(raster));
    edge_program = 0;
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
                     uint32_t depth, uint32_t alpha, bool effects, bool textured, const uint32_t *edge_colors, void *dst)
{
    if (!compute_context_bind()) return false;
    raster_program *program = &raster[textured];
    if (!program->id && !init_program(program, textured)) {
        compute_context_release();
        return false;
    }
    if (edge_colors && !edge_program && !init_edges()) {
        compute_context_release();
        return false;
    }
    gl.UseProgram(program->id);
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
    gl.Uniform1ui(program->coef, words);
    gl.Uniform1ui(program->clear, clear);
    gl.Uniform1ui(program->depth, depth);
    gl.Uniform1ui(program->alpha, alpha);
    gl.Uniform1ui(program->effects, effects || edge_colors);
    bool ok = gl.GetError() == GL_NO_ERROR;
    if (ok) {
        gl.DispatchCompute(SCREEN_W/WORKGROUP_X, SCREEN_H/WORKGROUP_Y, 1);
        if (edge_colors) {
            gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            gl.UseProgram(edge_program);
            gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[3]);
            gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[4]);
            gl.Uniform1ui(edge_depth, depth);
            gl.Uniform1uiv(edge_palette, 8, edge_colors);
            gl.DispatchCompute(SCREEN_W/WORKGROUP_X, SCREEN_H/WORKGROUP_Y, 1);
            gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
            gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[1]);
        }
        gl.MemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[edge_colors ? 4 : 3]);
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

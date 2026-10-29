#include "renpy_vita.h"
#include <stdlib.h>
#include "psp2/kernel/processmgr.h"
#include "psp2/kernel/threadmgr.h"
#include "psp2/sysmodule.h"
#include "psp2/avplayer.h"
#include "psp2/kernel/sysmem.h"
#include "psp2/gxm.h"
#include "psp2/audioout.h"
#include <pygame_sdl2/pygame_sdl2.h>
#include <libswscale/swscale.h>
#include <GLES2/gl2.h>
#include "vita_pixmap.h"

#define FRAMEBUFFER_ALIGNMENT 0x40000

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))
#endif

#define SCE_AVPLAYER_STATE_READY 2

int RPVITA_exit_process(int res) {
    sceKernelExitProcess(res);
}

/* Video Player */

static void convert_nv12_to_rgba(uint8_t *, uint8_t *, int, int);
static void *mem_alloc(void *p, uint32_t alignment, uint32_t size);
static void mem_free(void *p, void* pMemory);
static void *gpu_alloc(void* p, uint32_t alignment, uint32_t size);
static void gpu_free(void* p, void* pMemory);
static void event_callback(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData);

enum {
  PLAYER_INACTIVE,
  PLAYER_ACTIVE,
  PLAYER_STOP,
};

/*
 * One per movie, passed to its threads and to AvPlayer's event callback, so
 * each works on its own player. When a movie ends Ren'Py starts the next one
 * at once, and AvPlayer crashes (in its controller thread) if a second player
 * is set up while the first is still open, so video_start waits for the old
 * one to be closed. (Never freed; a few bytes per movie.)
 */
typedef struct {
    SceAvPlayerHandle handle;
    volatile int threads_started;  /* set at READY */
    volatile int audio_running;
    volatile int closed;
} MoviePlayer;

static MoviePlayer *current_player = NULL;
SceAvPlayerHandle movie_player;
int player_state = PLAYER_INACTIVE;
static SDL_mutex *frame_mutex;
static int audio_port = -1;

/*
 * Zero-copy video: AvPlayer decodes into GPU-shared pixmap memory (frame
 * pool, handed out by gpu_alloc), and video_read_texture converts the
 * current NV12 frame to RGB with a shader into one texture, all on the GPU.
 * Off unless video_set_zero_copy(1); otherwise frames are converted on the
 * CPU and returned as surfaces (read_video).
 */
/* AvPlayer asks for all its output frames in one allocation (three
 * 960x544 NV12 frames: 2.25 MiB), besides 1 MiB-aligned decoder memory,
 * which isn't served from the pool. */
#define FRAME_POOL_SIZE 2
#define FRAME_POOL_TEXELS_X 512
#define FRAME_POOL_TEXELS_Y 1536
#define FRAME_POOL_MAX_ALIGNMENT (256 * 1024)

static VitaPixmap frame_pool[FRAME_POOL_SIZE];
static int frame_pool_used[FRAME_POOL_SIZE];
static int frame_pool_state = 0;  /* 0 not made yet, 1 ready, -1 failed */
static int zero_copy_wanted = 0;
static int zero_copy = 0;         /* this movie decodes into the pool */
static char zero_copy_msg[256] = "";
static uint64_t last_zero_copy_ts = (uint64_t) -1;
static int zero_copy_frames = 0;
static int zero_copy_misses = 0;

#define ALLOC_LOG_SIZE 16
static struct {
    uint32_t alignment;
    uint32_t size;
    void *ptr;
    int pool;
} alloc_log[ALLOC_LOG_SIZE];
static int alloc_log_count = 0;

void movie_audio_init() {
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (audio_port < 0) {
        printf("Error opening audio port: %d\n", audio_port);
        // TODO: error handling
        return;
    }
    // TODO: close
}

void RPVITA_video_init() {
    import_pygame_sdl2();
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    // hold it until game end
    frame_mutex = SDL_CreateMutex();
    movie_audio_init();
}

#define MAX_WIDTH 960
#define MAX_HEIGHT 544
#define NEON_ALIGN 0x80
static uint8_t _video_buffer[MAX_WIDTH * MAX_HEIGHT * 4 + NEON_ALIGN];
#define video_buffer ((uint8_t *) ALIGN((uintptr_t) _video_buffer, NEON_ALIGN))
static SceAvPlayerFrameInfo video_frame_info;
static int received_frames = 0;
static uint64_t lastFrameTs = 0;
static uint8_t frame_buffer[2][MAX_WIDTH * MAX_HEIGHT * 4];

void RPVITA_periodic() {
}

static int player_current(MoviePlayer *mp) {
    return mp == current_player && player_state == PLAYER_ACTIVE;
}

// Closes a movie's player; resets the shared state if it's still the
// current movie.
static void close_player(MoviePlayer *mp) {
    // The audio thread may be inside sceAvPlayerGetAudioData.
    for (int i = 0; i < 100 && mp->audio_running; i++) {
        sceKernelDelayThread(10000);
    }

    sceAvPlayerStop(mp->handle);
    sceAvPlayerClose(mp->handle);
    mp->closed = 1;

    SDL_LockMutex(frame_mutex);
    if (mp == current_player) {
        player_state = PLAYER_INACTIVE;
        received_frames = 0;
    }
    SDL_UnlockMutex(frame_mutex);
}

int fetch_frame_thread(void *data) {
    MoviePlayer *mp = (MoviePlayer *) data;

    // Fetch video frame
    while (player_current(mp)) {
        if (sceAvPlayerIsActive(mp->handle)) {
            if (sceAvPlayerGetVideoData(mp->handle, &video_frame_info)) {
                int width = video_frame_info.details.video.width;
                int height = video_frame_info.details.video.height;

                if (lastFrameTs == 0 || video_frame_info.timeStamp != lastFrameTs) {
                    // Receive new frame

                    // Convert pixel format
                    convert_nv12_to_rgba(video_frame_info.pData, video_buffer, width, height);

                    SDL_LockMutex(frame_mutex);
                    memcpy(frame_buffer[0], video_buffer, width * height * 4);
                    received_frames += 1;
                    SDL_UnlockMutex(frame_mutex);
                }
            } else {
                // No new frame yet: wait rather than spin (this thread used
                // to poll AvPlayer in a tight loop).
                sceKernelDelayThread(2000);
            }
        } else {
            break;
        }
    }

    close_player(mp);
    printf("Video thread exit\n");
    return 0;
}

// Zero-copy movies: frames are fetched on the main thread
// (video_read_texture); this only notices the end of the movie and closes
// the player, as fetch_frame_thread does.
int monitor_thread(void *data) {
    MoviePlayer *mp = (MoviePlayer *) data;

    while (player_current(mp) && sceAvPlayerIsActive(mp->handle)) {
        sceKernelDelayThread(10000);
    }

    close_player(mp);
    return 0;
}

PyObject *RPVITA_video_read_video() {
    SDL_Surface *surf = NULL;

    SDL_LockMutex(frame_mutex);
    if (received_frames > 0) {
        if (received_frames > 1) {
            printf("WARN: Dropped %d frames\n", received_frames - 1);
        }

        int width = video_frame_info.details.video.width;
        int height = video_frame_info.details.video.height;

        memcpy(frame_buffer[1], frame_buffer[0], width * height * 4);

        // Create SDL surface
        surf = SDL_CreateRGBSurfaceFrom(frame_buffer[1], width, height, 32, width * 4,
                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);

        received_frames = 0;
    }
    SDL_UnlockMutex(frame_mutex);

    // Make return
    if (surf) {
        return PySurface_New(surf);
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}


int movie_audio_thread(void *data) {
    MoviePlayer *mp = (MoviePlayer *) data;
    SceAvPlayerFrameInfo frame;
    memset(&frame, 0, sizeof(SceAvPlayerFrameInfo));

    uint8_t *noSound = (uint8_t *) memalign(0x20, 4096 * 4);
    memset(noSound, 0, 4096 * 4);

    while (player_current(mp) && sceAvPlayerIsActive(mp->handle)) {
        if (sceAvPlayerGetAudioData(mp->handle, &frame)) {
            sceAudioOutSetConfig(audio_port, 1024, frame.details.audio.sampleRate,
                    frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            sceAudioOutOutput(audio_port, frame.pData);
        } else {
            sceAudioOutOutput(audio_port, noSound);
        }
    }

    mp->audio_running = 0;
    printf("Audio thread exit\n");
    return 0;
}

void RPVITA_video_start(const char *file) {
    SceAvPlayerInitData playerInit;
    memset(&playerInit, 0, sizeof(SceAvPlayerInitData));

    // Stop the previous movie and wait until its player is closed.
    MoviePlayer *old = current_player;

    SDL_LockMutex(frame_mutex);
    current_player = NULL;
    player_state = PLAYER_STOP;
    SDL_UnlockMutex(frame_mutex);

    if (old && !old->closed) {
        if (!old->threads_started) {
            close_player(old);
        } else {
            for (int i = 0; i < 300 && !old->closed; i++) {
                sceKernelDelayThread(10000);
            }
        }
    }

    if (zero_copy_wanted && frame_pool_state == 0) {
        frame_pool_state = 1;
        for (int i = 0; i < FRAME_POOL_SIZE; i++) {
            if (vita_pixmap_create(FRAME_POOL_TEXELS_X, FRAME_POOL_TEXELS_Y, 1, &frame_pool[i], zero_copy_msg, sizeof(zero_copy_msg))) {
                frame_pool_state = -1;
                break;
            }
        }
    }

    zero_copy = zero_copy_wanted && frame_pool_state == 1;
    last_zero_copy_ts = (uint64_t) -1;

    playerInit.memoryReplacement.allocate = mem_alloc;
    playerInit.memoryReplacement.deallocate = mem_free;
    playerInit.memoryReplacement.allocateTexture = gpu_alloc;
    playerInit.memoryReplacement.deallocateTexture = gpu_free;
    playerInit.eventReplacement.eventCallback = event_callback;

    MoviePlayer *mp = (MoviePlayer *) calloc(1, sizeof(MoviePlayer));
    playerInit.eventReplacement.objectPointer = mp;

    playerInit.basePriority = 0xA0;
    playerInit.numOutputVideoFrameBuffers = zero_copy ? 3 : 2;
    playerInit.autoStart = 0;

    mp->handle = sceAvPlayerInit(&playerInit);

    SDL_LockMutex(frame_mutex);
    current_player = mp;
    movie_player = mp->handle;
    player_state = PLAYER_ACTIVE;
    SDL_UnlockMutex(frame_mutex);

    sceAvPlayerAddSource(mp->handle, file);
}

void RPVITA_video_stop() {
    player_state = PLAYER_STOP;
}

int RPVITA_video_get_playing() {
    return player_state == PLAYER_ACTIVE && sceAvPlayerIsActive(movie_player);
}

static void event_callback(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData) {
    MoviePlayer *mp = (MoviePlayer *) p;
    printf("Player Event: %d\n", argEventId);

    if (argEventId == SCE_AVPLAYER_STATE_READY) {
        SceAvPlayerStreamInfo info;
        memset(&info, 0, sizeof(SceAvPlayerStreamInfo));

        int n = sceAvPlayerStreamCount(mp->handle);
        printf("Stream count: %d\n", n);

        for (int i = 0; i < n; i++) {
            sceAvPlayerGetStreamInfo(mp->handle, i, &info);

            if (info.type == SCE_AVPLAYER_AUDIO) {
                printf("Video: audio stream: freq=%d, channels=%d\n",
                        info.details.audio.sampleRate, info.details.audio.channelCount);

                // Enable
                printf("Enable audio stream\n");
                sceAvPlayerEnableStream(mp->handle, i);
            } else if (info.type == SCE_AVPLAYER_VIDEO) {
                printf("Video: video stream: width=%d, height=%d, lang=%d\n",
                        info.details.video.width, info.details.video.height, info.details.video.languageCode);

                // Enable
                printf("Enable video stream\n");
                sceAvPlayerEnableStream(mp->handle, i);
            }
        }

        printf("Video ts: %ld\n", sceAvPlayerCurrentTime(mp->handle));
        printf("Start video\n");
        sceAvPlayerStart(mp->handle);
        mp->threads_started = 1;
        mp->audio_running = 1;
        if (zero_copy) {
            SDL_CreateThread(monitor_thread, "Video monitor", mp);
        } else {
            SDL_CreateThread(fetch_frame_thread, "Video fetch frame", mp);
        }
        SDL_CreateThread(movie_audio_thread, "Movie audio thread", mp);
    }
}

/* Utils function */

struct SwsCache {
    struct SwsContext *sws_ctx;
    int width;
    int height;
} sws_cache = { NULL, 0, 0 };

static void convert_nv12_to_rgba(uint8_t *nv12_data, uint8_t *rgba_data, int width, int height) {
    struct SwsContext *sws_ctx = NULL;

    // Get sws context from cache
    if (sws_cache.sws_ctx && sws_cache.width == width && sws_cache.height == height) {
        sws_ctx = sws_cache.sws_ctx;
    } else {
        // Create new sws context and cache it
        if (sws_cache.sws_ctx) {
            sws_freeContext(sws_cache.sws_ctx);
        }
        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_NV12, width, height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            // TODO error handling
            return;
        }
        sws_cache.sws_ctx = sws_ctx;
        sws_cache.width = width;
        sws_cache.height = height;
    }

    uint8_t *src_slices[3] = { nv12_data,
                               nv12_data + width * height };
    int src_strides[3] = { width, width };

    uint8_t *dst_slices[1] = { rgba_data };
    int dst_strides[1] = { width * 4 };

    sws_scale(sws_ctx, src_slices, src_strides, 0, height, dst_slices, dst_strides);
}

static void *mem_alloc(void *p, uint32_t alignment, uint32_t size) {
	return memalign(alignment, size);
}

static void mem_free(void *p, void* pMemory) {
	free(pMemory);
}

static void *gpu_alloc(void *p, uint32_t alignment, uint32_t size) {
    void *res = NULL;

    if (zero_copy) {
        for (int i = 0; i < FRAME_POOL_SIZE; i++) {
            if (!frame_pool_used[i] && size <= frame_pool[i].size && alignment <= FRAME_POOL_MAX_ALIGNMENT
                    && ((uintptr_t) frame_pool[i].base % alignment) == 0) {
                frame_pool_used[i] = 1;
                if (alloc_log_count < ALLOC_LOG_SIZE) {
                    alloc_log[alloc_log_count].alignment = alignment;
                    alloc_log[alloc_log_count].size = size;
                    alloc_log[alloc_log_count].ptr = frame_pool[i].base;
                    alloc_log[alloc_log_count].pool = i;
                    alloc_log_count++;
                }
                return frame_pool[i].base;
            }
        }
    }

    if (alignment < FRAMEBUFFER_ALIGNMENT)
        alignment = FRAMEBUFFER_ALIGNMENT;

    size = ALIGN(size, alignment);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(SceKernelAllocMemBlockOpt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    SceUID memblock = sceKernelAllocMemBlock("Video Memblock", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    sceKernelGetMemBlockBase(memblock, &res);
    sceGxmMapMemory(res, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));

    if (alloc_log_count < ALLOC_LOG_SIZE) {
        alloc_log[alloc_log_count].alignment = alignment;
        alloc_log[alloc_log_count].size = size;
        alloc_log[alloc_log_count].ptr = res;
        alloc_log[alloc_log_count].pool = -1;
        alloc_log_count++;
    }

    return res;
}

static void gpu_free(void *p, void *ptr) {
    for (int i = 0; i < FRAME_POOL_SIZE; i++) {
        if (frame_pool_state == 1 && ptr == frame_pool[i].base) {
            frame_pool_used[i] = 0;
            return;
        }
    }

    SceUID memblock = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(memblock);
}


/* Zero-copy conversion ---------------------------------------------------- */

static const char *zc_vertex_shader =
    "attribute vec2 a_position;\n"
    "void main() { gl_Position = vec4(a_position, 0.0, 1.0); }\n";

/* Reads NV12 bytes out of a pool pixmap (4 bytes per texel, B G R A order
 * in memory) and converts them to RGB (BT.601, limited range). Output pixel
 * (x, y) is video pixel (x, y), so texture row 0 is the top of the frame. */
static const char *zc_fragment_shader =
    "precision highp float;\n"
    "uniform sampler2D u_pool;\n"
    "uniform vec2 u_pool_size;\n"
    "uniform float u_y_offset;\n"
    "uniform float u_uv_offset;\n"
    "uniform float u_stride;\n"
    "float byte_at(float address) {\n"
    "    float texel = floor(address / 4.0);\n"
    "    float component = address - texel * 4.0;\n"
    "    float tx = mod(texel, u_pool_size.x);\n"
    "    float ty = floor(texel / u_pool_size.x);\n"
    "    vec4 t = texture2D(u_pool, (vec2(tx, ty) + 0.5) / u_pool_size);\n"
    "    return dot(t, vec4(equal(vec4(component), vec4(2.0, 1.0, 0.0, 3.0))));\n"
    "}\n"
    "void main() {\n"
    "    vec2 p = floor(gl_FragCoord.xy);\n"
    "    float y = (byte_at(u_y_offset + p.y * u_stride + p.x) - 0.0625) * 1.164;\n"
    "    float uv = u_uv_offset + floor(p.y / 2.0) * u_stride + floor(p.x / 2.0) * 2.0;\n"
    "    float u = byte_at(uv) - 0.5;\n"
    "    float v = byte_at(uv + 1.0) - 0.5;\n"
    "    gl_FragColor = vec4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);\n"
    "}\n";

static GLuint zc_program = 0;
static GLint zc_u_pool, zc_u_pool_size, zc_u_y_offset, zc_u_uv_offset, zc_u_stride;
static GLuint zc_fbo = 0;
static GLuint zc_target = 0;
static int zc_target_width = 0;
static int zc_target_height = 0;

static GLuint zc_compile(GLenum kind, const char *source) {
    GLuint shader = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[200] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        snprintf(zero_copy_msg, sizeof(zero_copy_msg), "shader compile failed: %s", log);
        return 0;
    }
    return shader;
}

static int zc_setup(int width, int height) {
    GLint ok = 0;

    if (!zc_program) {
        GLuint vs = zc_compile(GL_VERTEX_SHADER, zc_vertex_shader);
        GLuint fs = zc_compile(GL_FRAGMENT_SHADER, zc_fragment_shader);
        if (!vs || !fs) {
            return -1;
        }
        zc_program = glCreateProgram();
        glAttachShader(zc_program, vs);
        glAttachShader(zc_program, fs);
        glBindAttribLocation(zc_program, 0, "a_position");
        glLinkProgram(zc_program);
        glGetProgramiv(zc_program, GL_LINK_STATUS, &ok);
        if (!ok) {
            snprintf(zero_copy_msg, sizeof(zero_copy_msg), "shader link failed");
            glDeleteProgram(zc_program);
            zc_program = 0;
            return -1;
        }
        zc_u_pool = glGetUniformLocation(zc_program, "u_pool");
        zc_u_pool_size = glGetUniformLocation(zc_program, "u_pool_size");
        zc_u_y_offset = glGetUniformLocation(zc_program, "u_y_offset");
        zc_u_uv_offset = glGetUniformLocation(zc_program, "u_uv_offset");
        zc_u_stride = glGetUniformLocation(zc_program, "u_stride");
        glGenFramebuffers(1, &zc_fbo);
    }

    if (zc_target_width != width || zc_target_height != height) {
        GLint old_framebuffer = 0;

        if (zc_target) {
            glDeleteTextures(1, &zc_target);
        }
        glGenTextures(1, &zc_target);
        glBindTexture(GL_TEXTURE_2D, zc_target);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, zc_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, zc_target, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            snprintf(zero_copy_msg, sizeof(zero_copy_msg), "framebuffer status 0x%x", status);
            return -1;
        }

        zc_target_width = width;
        zc_target_height = height;
    }

    return 0;
}

/* Converts the frame at data (inside pool pixmap block) into zc_target,
 * leaving the GL state as it found it. */
static void zc_convert(VitaPixmap *block, uint8_t *data, int width, int height) {
    static const GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };

    GLint program, framebuffer, viewport[4], array_buffer, active_texture, texture;
    GLint attrib_enabled, attrib_size, attrib_type, attrib_normalized, attrib_stride, attrib_buffer;
    GLvoid *attrib_pointer;
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib_enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attrib_normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attrib_buffer);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib_pointer);

    glBindFramebuffer(GL_FRAMEBUFFER, zc_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(zc_program);
    glBindTexture(GL_TEXTURE_2D, block->texture);
    glUniform1i(zc_u_pool, 0);
    glUniform2f(zc_u_pool_size, (float) block->width, (float) block->height);
    glUniform1f(zc_u_y_offset, (float) (data - block->base));
    glUniform1f(zc_u_uv_offset, (float) (data - block->base + width * height));
    glUniform1f(zc_u_stride, (float) width);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_ARRAY_BUFFER, attrib_buffer);
    glVertexAttribPointer(0, attrib_size, attrib_type, attrib_normalized, attrib_stride, attrib_pointer);
    if (!attrib_enabled) {
        glDisableVertexAttribArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active_texture);
    glUseProgram(program);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

void RPVITA_video_set_zero_copy(int enabled) {
    zero_copy_wanted = enabled;
}

int RPVITA_video_zero_copy() {
    return zero_copy && player_state == PLAYER_ACTIVE;
}

/*
 * Zero-copy movies: if a new frame is due, converts it and returns
 * (texture, width, height); the same GL texture is reused for every frame.
 * Returns None when there's no new frame (or zero-copy isn't in use).
 */
PyObject *RPVITA_video_read_texture() {
    SceAvPlayerFrameInfo info;

    if (!zero_copy || player_state != PLAYER_ACTIVE || !sceAvPlayerIsActive(movie_player)) {
        Py_RETURN_NONE;
    }

    if (!sceAvPlayerGetVideoData(movie_player, &info) || info.timeStamp == last_zero_copy_ts) {
        Py_RETURN_NONE;
    }

    int width = info.details.video.width;
    int height = info.details.video.height;

    VitaPixmap *block = NULL;
    for (int i = 0; i < FRAME_POOL_SIZE; i++) {
        if (info.pData >= frame_pool[i].base && info.pData + width * height * 3 / 2 <= frame_pool[i].base + frame_pool[i].size) {
            block = &frame_pool[i];
        }
    }

    if (!block) {
        zero_copy_misses++;
        Py_RETURN_NONE;
    }

    if (zc_setup(width, height)) {
        Py_RETURN_NONE;
    }

    zc_convert(block, info.pData, width, height);
    last_zero_copy_ts = info.timeStamp;
    zero_copy_frames++;

    return Py_BuildValue("(iii)", (int) zc_target, width, height);
}

/* Diagnostics: zero-copy state and AvPlayer's frame allocations. */
PyObject *RPVITA_video_debug() {
    PyObject *allocs = PyList_New(0);
    for (int i = 0; i < alloc_log_count; i++) {
        PyObject *t = Py_BuildValue("(IIki)", alloc_log[i].alignment, alloc_log[i].size, (unsigned long) alloc_log[i].ptr, alloc_log[i].pool);
        PyList_Append(allocs, t);
        Py_DECREF(t);
    }
    return Py_BuildValue("{s:i,s:i,s:i,s:i,s:s,s:N}",
        "pool_state", frame_pool_state, "zero_copy", zero_copy,
        "frames", zero_copy_frames, "misses", zero_copy_misses,
        "msg", zero_copy_msg, "allocs", allocs);
}

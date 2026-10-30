/* The Vita port's own movie player: up to HW_MAX movies at once.
 *
 * Sony's AvPlayer can only have one player open (a second crashes in its
 * shared demuxer), so the port could show only one movie at a time; on PC
 * each Movie has its own channel. This plays H.264/AAC MP4 files with the
 * hardware H.264 decoder directly (SceVideodec / sceAvcdec), which runs two
 * streams at once when its library is set up the way wiliwili's FFmpeg
 * decoder does it (see the renpy-8 branch hwvideo-experiment for the
 * probes). FFmpeg demuxes the files and decodes the AAC audio.
 *
 * Per player: a demux thread reads packets; video access units go through
 * h264_mp4toannexb to the decoder, which writes NV12 pictures into a ring of
 * HW_RING slots in GPU-shared pixmaps (vita_pixmap.c), with rows HW_PITCH
 * bytes apart so that a picture row is a texture row; audio is decoded,
 * converted to 48 kHz stereo S16 and
 * queued for an audio thread that plays it on its own audio port. Playback
 * starts once the first picture and some audio are ready; the clock is the
 * sound that's playing (the wall clock for a movie without sound), and the
 * main thread (hwvideo_read_texture) shows the newest picture
 * that is due, converting it into the player's own RGBA texture with a
 * shader (hw_convert). Looping seeks back to the start and keeps decoding, with
 * timestamps offset by the loop length, so a loop has no gap; audio is
 * placed by its timestamps, which keeps it in step across loops.
 *
 * Only movies up to HW_MAX_WIDTH x HW_MAX_HEIGHT with at most HW_REFS
 * reference frames (vita_video.py's encodes: 960x540, level 3.1) are
 * supported; hwvideo_open fails for others.
 */

#include "renpy_vita.h"
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <psp2/audioout.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <GLES2/gl2.h>
#include "vita_pixmap.h"

#define HW_MAX 2
#define HW_RING 8                     /* at most; as many as fit the pool block */
/* Pictures stay reserved for this many newer pictures after being shown:
   the GPU runs a frame or two behind, and a slot handed back to the decoder
   at once was overwritten while the GPU still converted it (blocks of the
   next frame showing, most visibly in 60 fps movies). */
#define HW_KEEP 2
#define HW_MAX_WIDTH 960
#define HW_MAX_HEIGHT 544
#define HW_REFS 8
#define HW_RATE 48000
#define HW_GRAIN 1024                 /* audio frames per output call */
#define HW_PCM_FRAMES (HW_RATE * 2)   /* queued audio, 2 s */
#define HW_TS 90000                   /* decoder timestamp units */

/* Picture layout in the ring: rows HW_PITCH bytes apart (one row of a
   HW_PITCH / 4 texel wide pixmap), luma rows then the interleaved chroma
   rows; HW_SLOTS_PER_PIXMAP pictures per pixmap, HW_RING / that pixmaps per
   player. */
#define HW_PITCH 1024
#define HW_SLOT_ROWS (HW_MAX_HEIGHT * 3 / 2)
#define HW_SLOTS_PER_PIXMAP 4
#define HW_PIXMAPS (HW_RING / HW_SLOTS_PER_PIXMAP)

/* Not in VitaSDK's headers; as declared in wiliwili's FFmpeg patch
   (scripts/psv/ffmpeg/ffmpeg.patch, libavcodec/vitadec_video.c). */
typedef struct {
    SceAvcdecBuf memBuf;
    SceUID memBufUid;
    SceUIntVAddr vaContext;
    SceUInt32 contextSize;
} HwVideodecCtrl;

int sceVideodecInitLibraryWithUnmapMemInternal(SceVideodecType type, HwVideodecCtrl *ctrl, SceVideodecQueryInitInfo *query);
void sceVideodecQueryMemSizeInternal(SceVideodecType type, SceVideodecQueryInitInfo *query, SceUInt32 *size);
void sceVideodecSetConfigInternal(SceVideodecType type, SceInt32 cfg);
void sceAvcdecSetDecodeMode(SceVideodecType type, SceInt32 mode);
int sceAvcdecQueryDecoderMemSizeInternal(SceVideodecType type, const SceAvcdecQueryDecoderInfo *query, SceAvcdecDecoderInfo *info);
int sceAvcdecCreateDecoderInternal(SceVideodecType type, SceAvcdecCtrl *decoder, const SceAvcdecQueryDecoderInfo *query);
int sceAvcdecDecodeStop(SceAvcdecCtrl *ctrl, SceAvcdecArrayPicture *pictures);
SceUID sceCodecEngineOpenUnmapMemBlock(void *ptr, SceSize size);
SceInt32 sceCodecEngineCloseUnmapMemBlock(SceUID uid);
SceUIntVAddr sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceUInt32 size, SceUInt32 align);
SceInt32 sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, SceUIntVAddr p);

enum { SLOT_FREE, SLOT_READY, SLOT_SHOWN };

typedef struct {
    int used;
    int generation;
    int index;
    SDL_mutex *lock;               /* slots, shown frame, close */

    char path[512];
    int loop;
    volatile int stop;
    volatile int demux_done;
    volatile int audio_done;
    volatile int input_done;       /* end of a non-looping movie reached */
    volatile int failed;
    char msg[160];

    AVFormatContext *fmt;
    AVBSFContext *bsf;
    AVCodecContext *actx;
    SwrContext *swr;
    AVFrame *aframe;
    int vstream, astream;
    int width, height;             /* display size */
    int coded_w, coded_h;          /* multiples of 16 */
    int64_t loop_offset;           /* HW_TS units added to this pass */
    int64_t pass_end;              /* largest pts + frame length this pass */
    int64_t frame_len;

    SceAvcdecCtrl ctrl;
    SceUID frame_block;
    int decoder_ok;

    int slots;                     /* ring slots in use */
    VitaPixmap *slot_pixmap[HW_RING];
    int slot_row[HW_RING];         /* first row of the picture in its pixmap */
    uint8_t *slot[HW_RING];
    int slot_state[HW_RING];
    int64_t slot_pts[HW_RING];
    int shown;
    int recent[HW_KEEP];           /* previously shown, still reserved */
    int64_t shown_pts;

    volatile SceUInt64 t0;         /* clock start, 0 until started */
    volatile int64_t audio_pos;    /* frames handed to the audio port */
    volatile SceUInt64 audio_time; /* when the last block was handed over */
    int port;
    int volume;                    /* 0..32768 */
    int volume_set;
    int16_t *pcm;                  /* stereo frames, ring */
    volatile int64_t pcm_read, pcm_write;   /* frame counters */

    int pictures, shown_count, dropped, decode_errors, underruns;
} HwPlayer;

static HwPlayer players[HW_MAX];
static int lib_state = 0;          /* 0 not tried, 1 ready, < 0 failed */
static char lib_msg[160] = "";
static SceUID lib_block = -1, lib_unmap = -1;
static SceUIntVAddr lib_vaddr = 0;
static SDL_mutex *open_lock = NULL;

/* GL objects, made on the main thread (hwvideo_prepare): per player index
   its ring pixmaps and the texture its pictures are converted into. */
static VitaPixmap pixmaps[HW_MAX][HW_PIXMAPS];
static GLuint targets[HW_MAX], target_fbos[HW_MAX];
static int targets_state = 0;      /* 0 not made, 1 ready, -1 failed */
static GLuint program;
static GLint u_tex, u_size, u_tex_size, u_y_row, u_uv_row;

/* NV12 to RGB (BT.601, limited range). Output pixel (x, y) is picture pixel
   (x, y), so texture row 0 is the top of the picture. With the ring layout,
   picture row y is texture row u_y_row + y and its chroma row
   u_uv_row + y / 2, and texel x / 4 of either holds bytes x..x+3 (B G R A
   order: byte 0 is .b): both reads use coordinates interpolated from the
   vertex shader, which the GPU does much faster than reads at addresses
   computed per pixel (renpy_vita.c's shader for AvPlayer's frames). */
static const char *vertex_shader =
    "attribute vec2 a_position;\n"
    "uniform vec2 u_size;\n"
    "uniform vec2 u_tex_size;\n"
    "uniform float u_y_row;\n"
    "uniform float u_uv_row;\n"
    "varying highp vec2 v_y;\n"
    "varying highp vec2 v_uv;\n"
    "varying highp float v_x;\n"
    "void main() {\n"
    "    vec2 p = (a_position * 0.5 + 0.5) * u_size;\n"
    "    v_x = p.x;\n"
    "    v_y = vec2(p.x / (u_tex_size.x * 4.0), (u_y_row + p.y) / u_tex_size.y);\n"
    "    v_uv = vec2(p.x / (u_tex_size.x * 4.0), (u_uv_row + p.y * 0.5) / u_tex_size.y);\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_shader =
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "varying highp vec2 v_y;\n"
    "varying highp vec2 v_uv;\n"
    "varying highp float v_x;\n"
    "void main() {\n"
    "    vec4 ty = texture2D(u_tex, v_y);\n"
    "    vec4 tc = texture2D(u_tex, v_uv);\n"
    "    float k = floor(mod(v_x, 4.0));\n"
    "    float y = (dot(ty, vec4(equal(vec4(k), vec4(2.0, 1.0, 0.0, 3.0)))) - 0.0625) * 1.164;\n"
    "    float h = step(1.5, k);\n"
    "    float u = mix(tc.b, tc.r, h) - 0.5;\n"
    "    float v = mix(tc.g, tc.a, h) - 0.5;\n"
    "    gl_FragColor = vec4(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u, 1.0);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *source) {
    GLuint shader = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[120] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        snprintf(lib_msg, sizeof(lib_msg), "shader: %s", log);
        return 0;
    }
    return shader;
}

static int gl_setup(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_shader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_shader);
    GLint ok = 0;
    if (!vs || !fs) {
        return -1;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        snprintf(lib_msg, sizeof(lib_msg), "shader link failed");
        return -1;
    }
    u_tex = glGetUniformLocation(program, "u_tex");
    u_size = glGetUniformLocation(program, "u_size");
    u_tex_size = glGetUniformLocation(program, "u_tex_size");
    u_y_row = glGetUniformLocation(program, "u_y_row");
    u_uv_row = glGetUniformLocation(program, "u_uv_row");

    for (int i = 0; i < HW_MAX; i++) {
        for (int j = 0; j < HW_PIXMAPS; j++) {
            if (vita_pixmap_create(HW_PITCH / 4, HW_SLOT_ROWS * HW_SLOTS_PER_PIXMAP, 1, &pixmaps[i][j], lib_msg, sizeof(lib_msg))) {
                return -1;
            }
        }

        GLint old_framebuffer = 0;
        glGenTextures(1, &targets[i]);
        glBindTexture(GL_TEXTURE_2D, targets[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HW_MAX_WIDTH, HW_MAX_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &target_fbos[i]);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[i], 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            snprintf(lib_msg, sizeof(lib_msg), "framebuffer status 0x%x", status);
            return -1;
        }
    }
    return 0;
}

/* Converts a ring picture into the player's texture, leaving the GL state
   as it found it. */
static void hw_convert(VitaPixmap *pm, int row, int coded_h, int width, int height, GLuint fbo) {
    static const GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };

    GLint old_program, framebuffer, viewport[4], array_buffer, active_texture, texture;
    GLint attrib_enabled, attrib_size, attrib_type, attrib_normalized, attrib_stride, attrib_buffer;
    GLvoid *attrib_pointer;
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
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

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, pm->texture);
    glUniform1i(u_tex, 0);
    glUniform2f(u_size, (float) width, (float) height);
    glUniform2f(u_tex_size, (float) pm->width, (float) pm->height);
    glUniform1f(u_y_row, (float) row);
    glUniform1f(u_uv_row, (float) (row + coded_h));
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
    glUseProgram(old_program);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

static SceUID cdram_alloc(uint32_t size, uint32_t align, void **ptr) {
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = align;
    size = (size + 0x3FFFF) & ~0x3FFFFu;
    SceUID uid = sceKernelAllocMemBlock("hwvideo", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    if (uid >= 0 && sceKernelGetMemBlockBase(uid, ptr) < 0) {
        sceKernelFreeMemBlock(uid);
        return -1;
    }
    return uid;
}

/* Sets up the decoder library for HW_MAX streams, once. */
static int lib_init(void) {
    if (lib_state) {
        return lib_state;
    }

    lib_state = -1;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC) < 0) {
        snprintf(lib_msg, sizeof(lib_msg), "can't load the AVC decoder module");
        return lib_state;
    }

    SceVideodecQueryInitInfo query;
    memset(&query, 0, sizeof(query));
    query.hwAvc.size = sizeof(query.hwAvc);
    query.hwAvc.horizontal = HW_MAX_WIDTH;
    query.hwAvc.vertical = HW_MAX_HEIGHT;
    query.hwAvc.numOfRefFrames = HW_REFS;
    query.hwAvc.numOfStreams = HW_MAX;

    SceUInt32 size = 0;
    sceVideodecSetConfigInternal(SCE_VIDEODEC_TYPE_HW_AVCDEC, 2);
    sceAvcdecSetDecodeMode(SCE_VIDEODEC_TYPE_HW_AVCDEC, 0x80);
    sceVideodecQueryMemSizeInternal(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &size);
    if (!size) {
        snprintf(lib_msg, sizeof(lib_msg), "no library memory size");
        return lib_state;
    }

    /* For two streams, the allocation from unmapped memory only succeeded
       with a block 1 MiB larger than the size and 1 MiB alignment. */
    void *mem = NULL;
    uint32_t block_size = ((size + 0xFFFFF) & ~0xFFFFFu) + 0x100000;
    lib_block = cdram_alloc(block_size, 0x100000, &mem);
    if (lib_block < 0) {
        snprintf(lib_msg, sizeof(lib_msg), "no CDRAM for the library (%u bytes)", (unsigned) block_size);
        return lib_state;
    }
    lib_unmap = sceCodecEngineOpenUnmapMemBlock(mem, block_size);
    if (lib_unmap <= 0) {
        snprintf(lib_msg, sizeof(lib_msg), "OpenUnmapMemBlock 0x%x", lib_unmap);
        return lib_state;
    }
    lib_vaddr = sceCodecEngineAllocMemoryFromUnmapMemBlock(lib_unmap, size, 0x100000);
    if (!lib_vaddr) {
        snprintf(lib_msg, sizeof(lib_msg), "AllocMemoryFromUnmapMemBlock failed");
        return lib_state;
    }

    HwVideodecCtrl ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.vaContext = lib_vaddr;
    ctrl.contextSize = size;
    int rc = sceVideodecInitLibraryWithUnmapMemInternal(SCE_VIDEODEC_TYPE_HW_AVCDEC, &ctrl, &query);
    if (rc != 0) {
        snprintf(lib_msg, sizeof(lib_msg), "InitLibrary 0x%x", rc);
        return lib_state;
    }

    lib_state = 1;
    return lib_state;
}

/* GL thread, at each interaction: the shader, ring pixmaps and conversion
   targets, once. */
int RPVITA_hwvideo_prepare(void) {
    if (targets_state == 0 && vita_pixmap_gl_thread()) {
        targets_state = gl_setup() ? -1 : 1;
    }
    return targets_state;
}

static int64_t to_ts(int64_t v, AVRational tb) {
    return av_rescale_q(v, tb, (AVRational) { 1, HW_TS });
}

static void set_ts(SceVideodecTimeStamp *ts, int64_t v) {
    ts->upper = (uint32_t) ((uint64_t) v >> 32);
    ts->lower = (uint32_t) v;
}

/* Waits for a free ring slot; -1 if the player is stopping. */
static int free_slot(HwPlayer *p) {
    while (!p->stop) {
        SDL_LockMutex(p->lock);
        for (int i = 0; i < p->slots; i++) {
            if (p->slot_state[i] == SLOT_FREE) {
                SDL_UnlockMutex(p->lock);
                return i;
            }
        }
        SDL_UnlockMutex(p->lock);
        sceKernelDelayThread(2000);
    }
    return -1;
}

/* Starts the clock once a picture and some audio are ready. */
static void maybe_start(HwPlayer *p) {
    if (p->t0) {
        return;
    }
    int audio_ready = p->astream < 0 || p->pcm_write >= HW_GRAIN * 4 || p->input_done;
    if (p->pictures > 0 && audio_ready) {
        p->t0 = sceKernelGetProcessTimeWide();
    }
}

/* Decodes one access unit (au NULL: drains the decoder at the end). */
static int decode_au(HwPlayer *p, AVPacket *pkt) {
    SceAvcdecAu au;
    memset(&au, 0, sizeof(au));
    if (pkt) {
        AVRational tb = p->fmt->streams[p->vstream]->time_base;
        int64_t pts = pkt->pts == AV_NOPTS_VALUE ? 0 : to_ts(pkt->pts, tb) + p->loop_offset;
        set_ts(&au.pts, pts);
        if (pkt->dts == AV_NOPTS_VALUE) {
            au.dts.upper = au.dts.lower = 0xFFFFFFFF;
        } else {
            set_ts(&au.dts, to_ts(pkt->dts, tb) + p->loop_offset);
        }
        au.es.pBuf = pkt->data;
        au.es.size = pkt->size;

        int64_t end = pts - p->loop_offset + p->frame_len;
        if (end > p->pass_end) {
            p->pass_end = end;
        }
    }

    for (;;) {
        int s = free_slot(p);
        if (s < 0) {
            return -1;
        }

        SceAvcdecPicture pic;
        SceAvcdecPicture *pics[1] = { &pic };
        SceAvcdecArrayPicture array;
        memset(&pic, 0, sizeof(pic));
        pic.size = sizeof(pic);
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
        pic.frame.framePitch = HW_PITCH;
        pic.frame.frameWidth = p->coded_w;
        pic.frame.frameHeight = p->coded_h;
        pic.frame.pPicture[0] = p->slot[s];
        memset(&array, 0, sizeof(array));
        array.numOfElm = 1;
        array.pPicture = pics;

        int rc = pkt ? sceAvcdecDecode(&p->ctrl, &au, &array) : sceAvcdecDecodeStop(&p->ctrl, &array);

        if (array.numOfOutput) {
            int64_t pts = ((int64_t) pic.info.pts.upper << 32) | pic.info.pts.lower;
            SDL_LockMutex(p->lock);
            p->slot_pts[s] = pts;
            p->slot_state[s] = SLOT_READY;
            SDL_UnlockMutex(p->lock);
            p->pictures++;
            maybe_start(p);
        }

        if (rc == (int) SCE_AVCDEC_ERROR_ES_BUFFER_FULL) {
            if (!array.numOfOutput) {
                sceKernelDelayThread(1000);
            }
            continue;
        }
        if (rc < 0) {
            p->decode_errors++;
            if (p->decode_errors == 1) {
                snprintf(p->msg, sizeof(p->msg), "decode 0x%x", rc);
            }
            /* A stream the decoder can't handle (e.g. more reference frames
               than HW_REFS) would otherwise "play" forever with nothing. */
            if (p->decode_errors > 30 && p->pictures == 0) {
                p->failed = 1;
                p->stop = 1;
            }
            return 0;
        }
        /* Draining: call again while pictures come out. */
        if (!pkt && array.numOfOutput) {
            continue;
        }
        return 0;
    }
}

/* Queues decoded audio, placed by its timestamp (pts, in seconds of the
   whole playback): a gap is filled with silence, an overlap dropped, so
   audio stays with the video across loops. */
static void queue_audio(HwPlayer *p, uint8_t **data, int frames, double pts) {
    int64_t expected = (int64_t) (pts * HW_RATE);
    int64_t delta = expected - p->pcm_write;
    int16_t *src = (int16_t *) data[0];

    if (delta > HW_PCM_FRAMES / 2) {
        delta = HW_PCM_FRAMES / 2;
    }
    if (delta < -HW_RATE / 100) {
        int skip = (int) -delta;
        if (skip >= frames) {
            return;
        }
        src += skip * 2;
        frames -= skip;
    }

    while (!p->stop) {
        int64_t room = HW_PCM_FRAMES - (p->pcm_write - p->pcm_read);
        int64_t pad = delta > HW_RATE / 100 ? delta : 0;
        if (room >= frames + pad) {
            for (int64_t i = 0; i < pad; i++) {
                int64_t w = (p->pcm_write + i) % HW_PCM_FRAMES;
                p->pcm[w * 2] = p->pcm[w * 2 + 1] = 0;
            }
            for (int i = 0; i < frames; i++) {
                int64_t w = (p->pcm_write + pad + i) % HW_PCM_FRAMES;
                p->pcm[w * 2] = src[i * 2];
                p->pcm[w * 2 + 1] = src[i * 2 + 1];
            }
            p->pcm_write += pad + frames;
            return;
        }
        maybe_start(p);
        sceKernelDelayThread(5000);
    }
}

/* pkt NULL: drains the decoder (at a loop or the end). */
static void decode_audio(HwPlayer *p, AVPacket *pkt) {
    if (avcodec_send_packet(p->actx, pkt) < 0) {
        return;
    }
    while (avcodec_receive_frame(p->actx, p->aframe) == 0) {
        AVFrame *f = p->aframe;
        uint8_t buf[8192 * 4];
        uint8_t *out[1] = { buf };
        int max = sizeof(buf) / 4;
        int n = swr_convert(p->swr, out, max, (const uint8_t **) f->extended_data, f->nb_samples);
        if (n > 0 && f->pts != AV_NOPTS_VALUE) {
            AVRational tb = p->fmt->streams[p->astream]->time_base;
            double pts = (double) (to_ts(f->pts, tb) + p->loop_offset) / HW_TS;
            queue_audio(p, out, n, pts);
        }
        av_frame_unref(f);
    }
}

static int demux_thread(void *data) {
    HwPlayer *p = (HwPlayer *) data;
    AVPacket *pkt = av_packet_alloc();

    /* The ring pixmaps are made on the main thread at an interaction. */
    while (!p->stop && targets_state == 0) {
        sceKernelDelayThread(10000);
    }
    if (targets_state != 1) {
        p->failed = 1;
        snprintf(p->msg, sizeof(p->msg), "no ring: %s", lib_msg);
        p->stop = 1;
    } else {
        p->slots = HW_RING;
        for (int i = 0; i < HW_RING; i++) {
            p->slot_pixmap[i] = &pixmaps[p->index][i / HW_SLOTS_PER_PIXMAP];
            p->slot_row[i] = (i % HW_SLOTS_PER_PIXMAP) * HW_SLOT_ROWS;
            p->slot[i] = p->slot_pixmap[i]->base + p->slot_row[i] * HW_PITCH;
        }
    }

    while (!p->stop) {
        int rc = av_read_frame(p->fmt, pkt);

        if (rc < 0) {
            if (p->loop && p->pass_end > 0) {
                /* Loop: back to the start, keep decoding, timestamps go on.
                   The audio decoder still holds the end of this pass, which
                   must be queued with this pass's offset. */
                if (p->actx) {
                    decode_audio(p, NULL);
                    avcodec_flush_buffers(p->actx);
                }
                p->loop_offset += p->pass_end;
                p->pass_end = 0;
                av_seek_frame(p->fmt, p->vstream, 0, AVSEEK_FLAG_BACKWARD);
                av_bsf_flush(p->bsf);
                continue;
            }
            decode_au(p, NULL);
            if (p->actx) {
                decode_audio(p, NULL);
            }
            p->input_done = 1;
            maybe_start(p);
            break;
        }

        if (pkt->stream_index == p->vstream) {
            if (av_bsf_send_packet(p->bsf, pkt) == 0) {
                while (!p->stop && av_bsf_receive_packet(p->bsf, pkt) == 0) {
                    decode_au(p, pkt);
                    av_packet_unref(pkt);
                }
            }
        } else if (pkt->stream_index == p->astream) {
            decode_audio(p, pkt);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    p->demux_done = 1;
    return 0;
}

static int audio_thread(void *data) {
    HwPlayer *p = (HwPlayer *) data;
    static int16_t silence[HW_GRAIN * 2];
    int16_t *buf = (int16_t *) malloc(HW_GRAIN * 4);

    while (!p->stop && !p->t0) {
        sceKernelDelayThread(2000);
    }

    while (!p->stop) {
        if (p->volume_set != p->volume) {
            int vol[2] = { p->volume, p->volume };
            sceAudioOutSetVolume(p->port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
            p->volume_set = p->volume;
        }

        int64_t avail = p->pcm_write - p->pcm_read;
        if (avail >= HW_GRAIN) {
            for (int i = 0; i < HW_GRAIN; i++) {
                int64_t r = (p->pcm_read + i) % HW_PCM_FRAMES;
                buf[i * 2] = p->pcm[r * 2];
                buf[i * 2 + 1] = p->pcm[r * 2 + 1];
            }
            p->pcm_read += HW_GRAIN;
            sceAudioOutOutput(p->port, buf);
            p->audio_time = sceKernelGetProcessTimeWide();
            p->audio_pos = p->pcm_read;
        } else if (p->input_done && p->demux_done) {
            break;
        } else {
            p->underruns++;
            sceAudioOutOutput(p->port, silence);
        }
    }

    free(buf);
    p->audio_done = 1;
    return 0;
}

static void release(HwPlayer *p) {
    if (p->decoder_ok) {
        sceAvcdecDeleteDecoder(&p->ctrl);
        p->decoder_ok = 0;
    }
    if (p->frame_block >= 0) {
        sceKernelFreeMemBlock(p->frame_block);
        p->frame_block = -1;
    }
    if (p->port >= 0) {
        sceAudioOutReleasePort(p->port);
        p->port = -1;
    }
    if (p->bsf) av_bsf_free(&p->bsf);
    if (p->swr) swr_free(&p->swr);
    if (p->actx) avcodec_free_context(&p->actx);
    if (p->aframe) av_frame_free(&p->aframe);
    if (p->fmt) avformat_close_input(&p->fmt);
    free(p->pcm);
    p->pcm = NULL;
}

static int setup(HwPlayer *p) {
    char url[540];
    snprintf(url, sizeof(url), "file:%s", p->path);
    if (avformat_open_input(&p->fmt, url, NULL, NULL) < 0) {
        snprintf(p->msg, sizeof(p->msg), "can't open %s", p->path);
        return -1;
    }
    avformat_find_stream_info(p->fmt, NULL);

    p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (p->vstream < 0 || p->fmt->streams[p->vstream]->codecpar->codec_id != AV_CODEC_ID_H264) {
        snprintf(p->msg, sizeof(p->msg), "no H.264 video");
        return -1;
    }
    AVStream *vs = p->fmt->streams[p->vstream];
    p->width = vs->codecpar->width;
    p->height = vs->codecpar->height;
    p->coded_w = (p->width + 15) & ~15;
    p->coded_h = (p->height + 15) & ~15;
    if (p->coded_w > HW_MAX_WIDTH || p->coded_h > HW_MAX_HEIGHT) {
        snprintf(p->msg, sizeof(p->msg), "%dx%d is too large", p->width, p->height);
        return -1;
    }
    AVRational rate = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    p->frame_len = rate.num ? av_rescale_q(1, av_inv_q(rate), (AVRational) { 1, HW_TS }) : HW_TS / 30;

    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter || av_bsf_alloc(filter, &p->bsf) < 0) {
        snprintf(p->msg, sizeof(p->msg), "no h264_mp4toannexb");
        return -1;
    }
    avcodec_parameters_copy(p->bsf->par_in, vs->codecpar);
    p->bsf->time_base_in = vs->time_base;
    if (av_bsf_init(p->bsf) < 0) {
        snprintf(p->msg, sizeof(p->msg), "h264_mp4toannexb init failed");
        return -1;
    }

    p->astream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (p->astream >= 0) {
        AVCodecParameters *par = p->fmt->streams[p->astream]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        p->actx = codec ? avcodec_alloc_context3(codec) : NULL;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (p->actx) {
            p->actx->pkt_timebase = p->fmt->streams[p->astream]->time_base;
        }
        if (!p->actx || avcodec_parameters_to_context(p->actx, par) < 0 || avcodec_open2(p->actx, codec, NULL) < 0
                || swr_alloc_set_opts2(&p->swr, &stereo, AV_SAMPLE_FMT_S16, HW_RATE,
                                       &p->actx->ch_layout, p->actx->sample_fmt, p->actx->sample_rate, 0, NULL) < 0
                || swr_init(p->swr) < 0) {
            /* Play without sound rather than not at all. */
            if (p->actx) avcodec_free_context(&p->actx);
            if (p->swr) swr_free(&p->swr);
            p->astream = -1;
        } else {
            p->aframe = av_frame_alloc();
        }
    }

    p->pcm = (int16_t *) calloc(HW_PCM_FRAMES, 4);
    p->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, HW_GRAIN, HW_RATE, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (p->port < 0) {
        snprintf(p->msg, sizeof(p->msg), "audio port 0x%x", p->port);
        return -1;
    }

    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo info;
    memset(&q, 0, sizeof(q));
    memset(&info, 0, sizeof(info));
    q.horizontal = p->coded_w;
    q.vertical = p->coded_h;
    q.numOfRefFrames = HW_REFS;
    int rc = sceAvcdecQueryDecoderMemSizeInternal(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &info);
    if (rc < 0) {
        snprintf(p->msg, sizeof(p->msg), "QueryDecoderMemSize 0x%x", rc);
        return -1;
    }
    void *mem = NULL;
    p->frame_block = cdram_alloc(info.frameMemSize, 0x100000, &mem);
    if (p->frame_block < 0) {
        snprintf(p->msg, sizeof(p->msg), "no CDRAM for the decoder (%u bytes)", (unsigned) info.frameMemSize);
        return -1;
    }
    memset(&p->ctrl, 0, sizeof(p->ctrl));
    p->ctrl.frameBuf.pBuf = mem;
    p->ctrl.frameBuf.size = info.frameMemSize;
    rc = sceAvcdecCreateDecoderInternal(SCE_VIDEODEC_TYPE_HW_AVCDEC, &p->ctrl, &q);
    if (rc < 0) {
        snprintf(p->msg, sizeof(p->msg), "CreateDecoder 0x%x", rc);
        return -1;
    }
    p->decoder_ok = 1;
    return 0;
}

static HwPlayer *lookup(int handle) {
    int i = handle & 0xF;
    if (handle <= 0 || i >= HW_MAX) {
        return NULL;
    }
    HwPlayer *p = &players[i];
    if (!p->used || p->generation != (handle >> 4)) {
        return NULL;
    }
    return p;
}

/* Whether the hardware player can be used (the decoder library initialises). */
int RPVITA_hwvideo_available(void) {
    if (!open_lock) {
        open_lock = SDL_CreateMutex();
    }
    SDL_LockMutex(open_lock);
    int rc = lib_init();
    SDL_UnlockMutex(open_lock);
    return rc == 1;
}

/* Starts playing a file; returns a handle (> 0), or 0 if it can't be played
   (then hwvideo_message says why). */
int RPVITA_hwvideo_open(const char *path, int loop) {
    if (!RPVITA_hwvideo_available()) {
        return 0;
    }

    SDL_LockMutex(open_lock);
    HwPlayer *p = NULL;
    for (int i = 0; i < HW_MAX; i++) {
        if (!players[i].used) {
            p = &players[i];
            p->index = i;
            break;
        }
    }
    if (!p) {
        snprintf(lib_msg, sizeof(lib_msg), "all %d players busy", HW_MAX);
        SDL_UnlockMutex(open_lock);
        return 0;
    }

    SDL_mutex *lock = p->lock ? p->lock : SDL_CreateMutex();
    int generation = p->generation + 1;
    int index = p->index;
    memset(p, 0, sizeof(*p));
    p->lock = lock;
    p->generation = generation;
    p->index = index;
    p->frame_block = -1;
    p->port = -1;
    p->volume = 32768;
    p->volume_set = -1;
    p->shown = -1;
    for (int i = 0; i < HW_KEEP; i++) {
        p->recent[i] = -1;
    }
    p->loop = loop;
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->used = 1;
    SDL_UnlockMutex(open_lock);

    if (setup(p) < 0) {
        snprintf(lib_msg, sizeof(lib_msg), "%s", p->msg);
        release(p);
        p->used = 0;
        return 0;
    }

    SDL_CreateThread(demux_thread, "hwvideo demux", p);
    SDL_CreateThread(audio_thread, "hwvideo audio", p);
    return index | (generation << 4);
}

/* Stops a player and frees it; waits for its threads (briefly). */
void RPVITA_hwvideo_close(int handle) {
    HwPlayer *p = lookup(handle);
    if (!p) {
        return;
    }
    p->stop = 1;
    for (int i = 0; i < 300 && !(p->demux_done && p->audio_done); i++) {
        sceKernelDelayThread(5000);
    }
    SDL_LockMutex(p->lock);
    release(p);
    p->used = 0;
    SDL_UnlockMutex(p->lock);
}

/* True until a non-looping movie has shown its last picture and played its
   sound (or the player failed). */
int RPVITA_hwvideo_playing(int handle) {
    HwPlayer *p = lookup(handle);
    if (!p || p->failed) {
        return 0;
    }
    if (!p->input_done) {
        return 1;
    }
    SDL_LockMutex(p->lock);
    int ready = 0;
    for (int i = 0; i < p->slots; i++) {
        ready += p->slot_state[i] == SLOT_READY;
    }
    SDL_UnlockMutex(p->lock);
    return ready > 0 || !p->audio_done;
}

void RPVITA_hwvideo_set_volume(int handle, double volume) {
    HwPlayer *p = lookup(handle);
    if (p) {
        if (volume < 0) volume = 0;
        if (volume > 1) volume = 1;
        p->volume = (int) (volume * 32768);
    }
}

/* The playback position in HW_TS units. With sound, it's the audio
   that's playing: sceAudioOutOutput returns when the block before the one
   just handed over has been played, so the block handed over last starts
   about then; time since then is added, up to one block, so the clock
   stops when the sound does (an underrun, the app suspended) and video
   waits rather than racing ahead. Without sound, the wall clock. */
static int64_t clock_now(HwPlayer *p) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (p->astream < 0) {
        return (int64_t) (now - p->t0) * HW_TS / 1000000;
    }
    if (!p->audio_time) {
        return 0;
    }
    int64_t since = (int64_t) (now - p->audio_time);
    int64_t grain_us = (int64_t) HW_GRAIN * 1000000 / HW_RATE;
    if (since > grain_us) {
        since = grain_us;
    }
    return (p->audio_pos - HW_GRAIN) * HW_TS / HW_RATE + since * HW_TS / 1000000;
}

/* Main thread: if a new picture is due, converts it into the player's
   texture and returns (texture, width, height, texture width, texture
   height); else None. */
PyObject *RPVITA_hwvideo_read_texture(int handle) {
    HwPlayer *p = lookup(handle);
    if (!p || !p->t0 || targets_state != 1 || !p->slots) {
        Py_RETURN_NONE;
    }

    int64_t now = clock_now(p);
    int pick = -1;

    SDL_LockMutex(p->lock);
    for (int i = 0; i < p->slots; i++) {
        if (p->slot_state[i] == SLOT_READY && p->slot_pts[i] <= now && (pick < 0 || p->slot_pts[i] > p->slot_pts[pick])) {
            pick = i;
        }
    }
    if (pick >= 0) {
        for (int i = 0; i < p->slots; i++) {
            if (i != pick && p->slot_state[i] == SLOT_READY && p->slot_pts[i] < p->slot_pts[pick]) {
                p->slot_state[i] = SLOT_FREE;
                p->dropped++;
            }
        }
        if (p->shown >= 0) {
            if (p->recent[HW_KEEP - 1] >= 0) {
                p->slot_state[p->recent[HW_KEEP - 1]] = SLOT_FREE;
            }
            for (int i = HW_KEEP - 1; i > 0; i--) {
                p->recent[i] = p->recent[i - 1];
            }
            p->recent[0] = p->shown;
        }
        p->slot_state[pick] = SLOT_SHOWN;
        p->shown = pick;
        p->shown_pts = p->slot_pts[pick];
        p->shown_count++;
    }
    SDL_UnlockMutex(p->lock);

    if (pick < 0) {
        Py_RETURN_NONE;
    }

    hw_convert(p->slot_pixmap[pick], p->slot_row[pick], p->coded_h, p->width, p->height, target_fbos[p->index]);

    return Py_BuildValue("(iiiii)", (int) targets[p->index], p->width, p->height, HW_MAX_WIDTH, HW_MAX_HEIGHT);
}

/* Diagnostics for one player (or the library, for an unknown handle). */
PyObject *RPVITA_hwvideo_debug(int handle) {
    HwPlayer *p = lookup(handle);
    if (!p) {
        return Py_BuildValue("{s:i,s:i,s:s}", "lib", lib_state, "targets", targets_state, "msg", lib_msg);
    }
    double clock = p->t0 ? clock_now(p) / (double) HW_TS : -1.0;
    return Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:d,s:d,s:L,s:L,s:s,s:i,s:i}",
        "index", p->index, "pictures", p->pictures, "shown", p->shown_count, "dropped", p->dropped,
        "decode_errors", p->decode_errors, "underruns", p->underruns, "input_done", p->input_done,
        "demux_done", p->demux_done, "audio_done", p->audio_done, "clock", clock,
        "shown_pts", p->shown_pts / (double) HW_TS, "pcm_read", (long long) p->pcm_read,
        "pcm_write", (long long) p->pcm_write, "msg", p->msg, "width", p->width, "height", p->height);
}

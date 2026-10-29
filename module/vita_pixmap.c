/*
 * GPU-shared textures on the Vita (PVR_PSP2 driver): a texture whose pixels
 * live in memory the CPU writes directly, so updating it needs no
 * glTexImage2D upload (~40 MB/s on this driver).
 *
 * The driver's window system supports EGL images of native pixmaps
 * (Psp2NativePixmap, EGL_NATIVE_PIXMAP_KHR): it allocates the pixmap's memory
 * and maps it to the GPU with the device data and memory context the caller
 * supplies (see init_services).
 */

#include <Python.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/kernel/sysmem.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "vita_pixmap.h"

/* Services types, as laid out in PVR_PSP2's services.h. */
typedef struct {
    void *psConnection;
    void *hDevCookie;
} VP_DEV_DATA;

void *sceKernelGetTLSAddr(int key);

/*
 * PVRSRVConnect from the app fails (PVRSRV_ERROR_INVALID_PARAMS) once EGL is
 * connected, so the pixmap uses EGL's own device data and memory context (the
 * ones GLES textures are mapped with). Found by disassembling libIMGEGL.suprx
 * (PVR_PSP2 v3.9): its per-thread data is in TLS slot 255, with a pointer to
 * its global data at +48; the services context is at global + 0x3f8:
 * connection at +0, PVRSRV_DEV_DATA at +4, device memory context at +0xc4.
 */
#define EGL_TLS_KEY 255
#define EGL_TLS_GLOBAL 48
#define EGL_GLOBAL_SYS_CONTEXT 0x3f8
#define EGL_SYS_DEV_DATA 4
#define EGL_SYS_DEV_MEM_CONTEXT 0xc4

static VP_DEV_DATA *dev_data = NULL;
static void *dev_mem_context = NULL;

static PFNEGLCREATEIMAGEKHRPROC create_image = NULL;

static int init_services(char *msg, size_t len) {
    void **slot;
    char *tls;
    char *global;
    char *sys;

    if (dev_data) {
        return 0;
    }

    slot = (void **) sceKernelGetTLSAddr(EGL_TLS_KEY);
    if (!slot || !*slot) {
        snprintf(msg, len, "no EGL thread data (call from the GL thread)");
        return -1;
    }

    tls = (char *) *slot;
    global = *(char **) (tls + EGL_TLS_GLOBAL);
    if (!global) {
        snprintf(msg, len, "no EGL global data");
        return -1;
    }

    sys = global + EGL_GLOBAL_SYS_CONTEXT;

    if (!*(void **) sys || ((VP_DEV_DATA *) (sys + EGL_SYS_DEV_DATA))->psConnection != *(void **) sys) {
        snprintf(msg, len, "EGL services context doesn't look right: connection %p, dev data connection %p",
            *(void **) sys, ((VP_DEV_DATA *) (sys + EGL_SYS_DEV_DATA))->psConnection);
        return -1;
    }

    create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    if (!create_image) {
        snprintf(msg, len, "no eglCreateImageKHR");
        return -1;
    }

    dev_data = (VP_DEV_DATA *) (sys + EGL_SYS_DEV_DATA);
    dev_mem_context = *(void **) (sys + EGL_SYS_DEV_MEM_CONTEXT);

    return 0;
}

int vita_pixmap_create(int width, int height, int nearest, VitaPixmap *out, char *msg, size_t len) {
    Psp2NativePixmap *pixmap;
    EGLImageKHR image;
    GLuint texture = 0;
    GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
    GLenum error;

    if (init_services(msg, len)) {
        return -1;
    }

    /* Freed never: EGL keeps the pointer as the image's drawable. */
    pixmap = (Psp2NativePixmap *) calloc(1, sizeof(Psp2NativePixmap));
    pixmap->type = PSP2_DRAWABLE_TYPE_PIXMAP;
    pixmap->sizeX = width;
    pixmap->sizeY = height;
    pixmap->memType = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    pixmap->psDevData = dev_data;
    pixmap->hDevMemContext = (unsigned int) dev_mem_context;

    image = create_image(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer) pixmap, NULL);

    if (image == EGL_NO_IMAGE_KHR) {
        snprintf(msg, len, "eglCreateImageKHR failed: 0x%x", eglGetError());
        free(pixmap);
        return -1;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES) image);

    error = glGetError();
    if (error) {
        snprintf(msg, len, "glEGLImageTargetTexture2DOES error 0x%x", error);
        return -1;
    }

    out->base = (unsigned char *) pixmap->memBase;
    /* As WSEGL_CreatePixmapDrawable sizes CDRAM pixmaps. */
    out->size = ((unsigned int) width * height * 4 + 256 * 1024 - 1) & ~(256 * 1024 - 1);
    out->texture = texture;
    out->width = width;
    out->height = height;

    return 0;
}

/*
 * Test helper: creates a width x height pixmap texture. Returns (texture,
 * address of the pixels, message); texture is 0 on failure.
 */
PyObject *RPVITA_pixmap_texture(int width, int height) {
    char msg[256] = "ok";
    VitaPixmap pixmap;

    if (vita_pixmap_create(width, height, 0, &pixmap, msg, sizeof(msg))) {
        return Py_BuildValue("(iks)", 0, 0UL, msg);
    }

    return Py_BuildValue("(iks)", (int) pixmap.texture, (unsigned long) pixmap.base, msg);
}

/* Test pattern: bands of red, green, blue and white, shifted by phase. */
void RPVITA_pixmap_fill(unsigned long address, int width, int height, int phase) {
    unsigned char *p = (unsigned char *) address;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int band = (((x + phase) * 4) / width) % 4;
            p[0] = (band == 0 || band == 3) ? 255 : 0;
            p[1] = (band == 1 || band == 3) ? 255 : 0;
            p[2] = (band == 2 || band == 3) ? 255 : 0;
            p[3] = 255;
            p += 4;
        }
    }
}

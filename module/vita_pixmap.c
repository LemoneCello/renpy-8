/*
 * GPU-shared textures on the Vita (PVR_PSP2 driver): a texture whose pixels
 * live in memory the CPU writes directly, so updating it needs no
 * glTexImage2D upload (~40 MB/s on this driver).
 *
 * The driver's window system supports EGL images of native pixmaps
 * (Psp2NativePixmap, EGL_NATIVE_PIXMAP_KHR): it allocates the pixmap's memory
 * and maps it to the GPU with the device data and memory context the caller
 * supplies. Those come from the GPU services library (SceGpuEs4User, stub
 * generated from PVR_PSP2's SceGpuEs4User.emd), on a connection of our own.
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

/* Services types, as laid out in PVR_PSP2's services.h. */
typedef struct {
    void *psConnection;
    void *hDevCookie;
} VP_DEV_DATA;

typedef struct {
    int eDeviceType;
    int eDeviceClass;
    unsigned int ui32DeviceIndex;
    char *pszPDumpDevName;
    char *pszPDumpRegName;
} VP_DEVICE_IDENTIFIER;

typedef struct {
    unsigned char data[24];
} VP_HEAP_INFO;

#define VP_MAX_DEVICES 16
#define VP_MAX_CLIENT_HEAPS 32
#define VP_DEVICE_CLASS_3D 0
#define VP_DEVICE_TYPE_UNKNOWN 0

int PVRSRVConnect(void **ppsConnection, unsigned int flags);
int PVRSRVEnumerateDevices(void *psConnection, unsigned int *num, VP_DEVICE_IDENTIFIER *ids);
int PVRSRVAcquireDeviceData(void *psConnection, unsigned int index, VP_DEV_DATA *dev, int type);
int PVRSRVCreateDeviceMemContext(VP_DEV_DATA *dev, void **ctx, unsigned int *heap_count, VP_HEAP_INFO *heaps);

static VP_DEV_DATA dev_data;
static void *dev_mem_context = NULL;
static int services_ready = 0;

static PFNEGLCREATEIMAGEKHRPROC create_image = NULL;

static int init_services(char *msg, size_t len) {
    void *connection = NULL;
    unsigned int count = 0;
    unsigned int heaps = 0;
    VP_DEVICE_IDENTIFIER ids[VP_MAX_DEVICES];
    VP_HEAP_INFO heap_info[VP_MAX_CLIENT_HEAPS];
    int rv;

    if (services_ready) {
        return 0;
    }

    rv = PVRSRVConnect(&connection, 0);
    if (rv) {
        snprintf(msg, len, "PVRSRVConnect failed: %d", rv);
        return -1;
    }

    rv = PVRSRVEnumerateDevices(connection, &count, ids);
    if (rv) {
        snprintf(msg, len, "PVRSRVEnumerateDevices failed: %d", rv);
        return -1;
    }

    for (unsigned int i = 0; i < count; i++) {
        if (ids[i].eDeviceClass == VP_DEVICE_CLASS_3D) {
            rv = PVRSRVAcquireDeviceData(connection, ids[i].ui32DeviceIndex, &dev_data, VP_DEVICE_TYPE_UNKNOWN);
            if (rv) {
                snprintf(msg, len, "PVRSRVAcquireDeviceData failed: %d", rv);
                return -1;
            }
            break;
        }
    }

    if (!dev_data.psConnection) {
        snprintf(msg, len, "no 3D device among %u devices", count);
        return -1;
    }

    rv = PVRSRVCreateDeviceMemContext(&dev_data, &dev_mem_context, &heaps, heap_info);
    if (rv) {
        snprintf(msg, len, "PVRSRVCreateDeviceMemContext failed: %d", rv);
        return -1;
    }

    create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    if (!create_image) {
        snprintf(msg, len, "no eglCreateImageKHR");
        return -1;
    }

    services_ready = 1;
    return 0;
}

/*
 * Creates a width x height 32-bit pixmap, an EGL image of it and a GL
 * texture bound to that image. Returns (texture, address of the pixels,
 * message); texture is 0 on failure.
 */
PyObject *RPVITA_pixmap_texture(int width, int height) {
    char msg[256] = "ok";
    Psp2NativePixmap *pixmap;
    EGLImageKHR image;
    GLuint texture = 0;
    GLenum error;

    if (init_services(msg, sizeof(msg))) {
        return Py_BuildValue("(iks)", 0, 0UL, msg);
    }

    pixmap = (Psp2NativePixmap *) calloc(1, sizeof(Psp2NativePixmap));
    pixmap->type = PSP2_DRAWABLE_TYPE_PIXMAP;
    pixmap->sizeX = width;
    pixmap->sizeY = height;
    pixmap->memType = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    pixmap->psDevData = &dev_data;
    pixmap->hDevMemContext = (unsigned int) dev_mem_context;

    image = create_image(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer) pixmap, NULL);

    if (image == EGL_NO_IMAGE_KHR) {
        snprintf(msg, sizeof(msg), "eglCreateImageKHR failed: 0x%x", eglGetError());
        free(pixmap);
        return Py_BuildValue("(iks)", 0, 0UL, msg);
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES) image);

    error = glGetError();
    if (error) {
        snprintf(msg, sizeof(msg), "glEGLImageTargetTexture2DOES error 0x%x (memBase %p)", error, pixmap->memBase);
    }

    return Py_BuildValue("(iks)", (int) texture, (unsigned long) pixmap->memBase, msg);
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

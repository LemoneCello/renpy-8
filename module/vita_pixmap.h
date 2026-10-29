#ifndef VITA_PIXMAP_H
#define VITA_PIXMAP_H

#include <stddef.h>
#include <GLES2/gl2.h>

/* A GL texture whose pixels (32 bits each, B G R A in memory) live at base,
 * where the CPU and other hardware can write them without an upload. */
typedef struct {
    unsigned char *base;
    unsigned int size;
    GLuint texture;
    int width;
    int height;
} VitaPixmap;

/* True on a thread with EGL state (the GL thread). */
int vita_pixmap_gl_thread(void);

/* Creates one; call on the GL thread. Returns 0, or -1 with a message. */
int vita_pixmap_create(int width, int height, int nearest, VitaPixmap *out, char *msg, size_t len);

#endif

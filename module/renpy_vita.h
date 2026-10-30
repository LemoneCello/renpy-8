#include <Python.h>
#include <SDL.h>

int RPVITA_exit_process(int res);
long long RPVITA_thread_cpu_us();

/* Video Player */

void RPVITA_video_init();
PyObject *RPVITA_video_read_video();
void RPVITA_video_start(const char *file);
void RPVITA_video_stop();
int RPVITA_video_get_playing();
void RPVITA_periodic();
void RPVITA_video_set_zero_copy(int enabled);
void RPVITA_video_set_looping(int enabled);
int RPVITA_video_prepare();
int RPVITA_video_zero_copy();
PyObject *RPVITA_video_read_texture();
PyObject *RPVITA_video_debug();

/* GPU-shared textures (vita_pixmap.c) */

PyObject *RPVITA_pixmap_texture(int width, int height);
void RPVITA_pixmap_fill(unsigned long address, int width, int height, int phase);

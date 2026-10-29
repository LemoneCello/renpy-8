cdef extern from "renpy_vita.h":
    int RPVITA_exit_process(int res)
    void RPVITA_video_init()
    object RPVITA_video_read_video()
    void RPVITA_video_start(char *file)
    void RPVITA_video_stop()
    int RPVITA_video_get_playing()
    void RPVITA_periodic()
    object RPVITA_pixmap_texture(int width, int height)
    void RPVITA_pixmap_fill(unsigned long address, int width, int height, int phase)

def exit_process(res):
    RPVITA_exit_process(res)

def video_init():
    RPVITA_video_init()

def video_read_video():
    return RPVITA_video_read_video()

def video_start(file):
    RPVITA_video_start(file)

def video_stop():
    RPVITA_video_stop()

def video_get_playing():
    return RPVITA_video_get_playing()

def periodic():
    RPVITA_periodic()

def pixmap_texture(width, height):
    """
    Creates a texture whose pixels the CPU can write directly (no upload).
    Returns (GL texture number, address of the pixels, message); the
    texture number is 0 on failure.
    """

    return RPVITA_pixmap_texture(width, height)

def pixmap_fill(address, width, height, phase):
    RPVITA_pixmap_fill(address, width, height, phase)

cdef extern from "renpy_vita.h":
    int RPVITA_exit_process(int res)
    void RPVITA_video_init()
    object RPVITA_video_read_video()
    void RPVITA_video_start(char *file)
    void RPVITA_video_stop()
    int RPVITA_video_get_playing()
    void RPVITA_periodic()
    void RPVITA_video_set_zero_copy(int enabled)
    int RPVITA_video_prepare()
    int RPVITA_video_zero_copy()
    object RPVITA_video_read_texture()
    object RPVITA_video_debug()
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

def video_set_zero_copy(enabled):
    """
    If true, movies started afterwards decode into GPU-shared memory and are
    read with video_read_texture instead of video_read_video.
    """

    RPVITA_video_set_zero_copy(1 if enabled else 0)

def video_prepare():
    """
    Call on the main (GL) thread: makes the zero-copy frame pool if
    video_set_zero_copy(True) was called. Returns 1 (ready), 0 or -1.
    """

    return RPVITA_video_prepare()

def video_zero_copy():
    return RPVITA_video_zero_copy()

def video_read_texture():
    """
    For zero-copy movies: (GL texture, width, height) when a new frame has
    been converted into the texture, else None.
    """

    return RPVITA_video_read_texture()

def video_debug():
    return RPVITA_video_debug()

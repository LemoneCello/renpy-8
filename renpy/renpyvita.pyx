cdef extern from "renpy_vita.h":
    int RPVITA_exit_process(int res)
    long long RPVITA_thread_cpu_us()
    int RPVITA_hwvideo_available()
    int RPVITA_hwvideo_prepare()
    int RPVITA_hwvideo_open(const char *path, int loop) nogil
    void RPVITA_hwvideo_close(int handle) nogil
    int RPVITA_hwvideo_playing(int handle)
    void RPVITA_hwvideo_set_volume(int handle, double volume)
    object RPVITA_hwvideo_read_texture(int handle)
    object RPVITA_hwvideo_debug(int handle)
    void RPVITA_video_init()
    object RPVITA_video_read_video()
    void RPVITA_video_start(char *file)
    void RPVITA_video_stop()
    int RPVITA_video_get_playing()
    void RPVITA_periodic()
    void RPVITA_video_set_zero_copy(int enabled)
    void RPVITA_video_set_looping(int enabled)
    int RPVITA_video_prepare()
    int RPVITA_video_zero_copy()
    object RPVITA_video_read_texture()
    object RPVITA_video_debug()
    object RPVITA_pixmap_texture(int width, int height)
    void RPVITA_pixmap_fill(unsigned long address, int width, int height, int phase)

def exit_process(res):
    RPVITA_exit_process(res)

def thread_cpu_us():
    return RPVITA_thread_cpu_us()

# The port's own movie player (module/vita_hwvideo.c): several at once.

def hwvideo_available():
    return RPVITA_hwvideo_available()

def hwvideo_prepare():
    return RPVITA_hwvideo_prepare()

def hwvideo_open(path, loop):
    cdef bytes b = path.encode("utf-8") if isinstance(path, str) else path
    cdef const char *c = b
    cdef int rv
    cdef int l = 1 if loop else 0
    with nogil:
        rv = RPVITA_hwvideo_open(c, l)
    return rv

def hwvideo_close(int handle):
    with nogil:
        RPVITA_hwvideo_close(handle)

def hwvideo_playing(int handle):
    return bool(RPVITA_hwvideo_playing(handle))

def hwvideo_set_volume(int handle, double volume):
    RPVITA_hwvideo_set_volume(handle, volume)

def hwvideo_read_texture(int handle):
    return RPVITA_hwvideo_read_texture(handle)

def hwvideo_debug(int handle=0):
    return RPVITA_hwvideo_debug(handle)

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

def video_set_looping(enabled):
    """
    If true, movies started afterwards loop by themselves until stopped.
    """

    RPVITA_video_set_looping(1 if enabled else 0)

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

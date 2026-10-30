import renpy
import renpy.renpyvita as renpyvita
from renpy.audio.audio import MusicContext
import sys
import time


# Whether the port's own player (module/vita_hwvideo.c) is used: it plays
# up to two movies at once, each on its own channel as on PC, where Sony's
# AvPlayer (one at a time, on the shared "movie" channel) couldn't. None
# until checked.
hw = None


def hw_available():
    """
    True if movies play through the port's own player. Checked once; the
    decoder library is set up then.
    """

    global hw

    if hw is None:
        hw = bool(renpy.config.vita_hw_video and renpy.config.vita_zero_copy_video and renpyvita.hwvideo_available())

        if not hw and renpy.config.vita_hw_video:
            print("Vita hardware video player not available:", renpyvita.hwvideo_debug(0))

    return hw


def prepare():
    """
    Called on the main thread at each interaction: makes the GPU-shared
    memory zero-copy movies decode into, once, as that needs the GL thread
    (channels start movies from the periodic audio thread).
    """

    if not renpy.config.vita_zero_copy_video:
        return

    # The port's own player has its own pixmaps and shader; AvPlayer's frame
    # pool is only made when AvPlayer plays the movies.
    if hw_available():
        renpyvita.hwvideo_prepare()
    else:
        renpyvita.video_set_zero_copy(True)
        renpyvita.video_prepare()


class VitaVideoChannel(object):

    def __init__(self, name, file_prefix="", file_suffix="", default_loop=None):

        # A list of queued filenames.
        self.queue = [ ]

        # The filename that's currently playing.
        self.filename = None

        # The name assigned to this channel. This is used to look up
        # information about the channel in the MusicContext object.
        self.name = name

        # The name of the mixer this channel uses. Set below, as there's
        # no good default.
        self.mixer = None

        # The time the music in this channel was last changed.
        self.last_changed = 0

        # The callback that is called if the queue becomes empty.
        self.callback = None

        # Ignored.
        self.synchro_start = False
        self.wait_stop = False
        self.loop = [ ]

        # A prefix and suffix that are used to create the full filenames.
        self.file_prefix = file_prefix
        self.file_suffix = file_suffix

        # The port's own player for the playing file (0: none).
        self.handle = 0

        # When to try again after the player couldn't start a movie.
        self.retry_time = 0

        # The volume set by set_volume, and the one last given to the player.
        self.chan_volume = 1.0
        self.actual_volume = None

        if default_loop is None:
            # By default, should we loop the music?
            self.default_loop = True
            # Was this set explicitly?
            self.default_loop_set = False

        else:
            self.default_loop = default_loop
            self.default_loop_set = True

    def get_context(self):
        """
        Returns the MusicContext corresponding to this channel, taken from
        the context object. Allocates a MusicContext if none exists.
        """

        mcd = renpy.game.context().music

        rv = mcd.get(self.name)
        if rv is None:
            rv = mcd[self.name] = MusicContext()

        return rv

    context = property(get_context)

    def copy_context(self):
        """
        Copies the MusicContext associated with this channel, updates the
        ExecutionContext to point to the copy, and returns the copy.
        """

        mcd = renpy.game.context().music

        ctx = self.get_context().copy()
        mcd[self.name] = ctx
        return ctx

    def start(self):
        """
        Starts playing the first video in the queue.
        """

        if not self.queue:
            return

        name = self.queue.pop(0)
        filename = sys.argv[0] + "/game/" + name

        real_filename = filename.encode('utf-8')

        print("Playing", filename, real_filename)

        if hw_available():
            # A loop of one file loops in the player, without a gap.
            loop = not self.queue and self.loop == [ name ]
            self.close()
            self.handle = renpyvita.hwvideo_open(filename, loop)
            self.filename = name
            self.actual_volume = None
            self.update_volume()

            if not self.handle:
                # E.g. both players busy: try again in a second, not at
                # every periodic pass.
                print("Can't play", filename, renpyvita.hwvideo_debug(0))
                self.retry_time = time.time() + 1.0
                self.queue.insert(0, name)

            return

        renpyvita.video_set_zero_copy(renpy.config.vita_zero_copy_video)

        # A loop of one file loops in the player, without a gap (as Ren'Py's
        # own channels do); other loops are queued again in periodic().
        renpyvita.video_set_looping(not self.queue and self.loop == [ name ])
        renpyvita.video_start(real_filename)

    def close(self):
        if self.handle:
            renpyvita.hwvideo_close(self.handle)
            self.handle = 0

    def stop(self):
        if hw_available():
            self.close()
        else:
            renpyvita.video_stop()

        self.filename = None

    def get_playing(self):
        if hw_available():
            return bool(self.handle) and renpyvita.hwvideo_playing(self.handle)

        return renpyvita.video_get_playing()

    def update_volume(self):
        if not self.handle:
            return

        prefs = renpy.game.preferences
        vol = self.chan_volume * prefs.volumes.get(self.mixer, 1.0) * prefs.volumes.get("main", 1.0)

        if prefs.mute.get(self.mixer, False):
            vol = 0.0

        if vol != self.actual_volume:
            renpyvita.hwvideo_set_volume(self.handle, vol)
            self.actual_volume = vol

    def periodic(self):

        # This should be set from something that checks to see if our
        # mixer is muted.
        force_stop = self.context.force_stop

        if force_stop:
            self.dequeue()
            self.stop()
            return

        renpyvita.periodic()

        self.update_volume()

        if self.get_playing():
            return

        # A finished movie's player is closed before the next starts.
        self.close()

        if time.time() < self.retry_time:
            return

        if not self.queue and self.loop:
            self.queue.extend(self.loop)

        if self.queue:
            self.start()

    def dequeue(self, even_tight=False):
        """
        Clears the queued music, except for a first item that has
        not been started.
        """

        if self.get_playing():
            self.queue = [ ]
        else:
            self.queue = self.queue[:1]

        self.loop = [ ]

    def interact(self):
        """
        Called (mostly) once per interaction.
        """

        self.periodic()

    def fadeout(self, secs):
        """
        Causes the playing music to be faded out for the given number
        of seconds. Also clears any queued music.
        """

        self.stop()
        self.queue = [ ]
        self.loop = [ ]

    def enqueue(self, filenames, loop=True, synchro_start=False, fadein=0, tight=None, loop_only=False, relative_volume=1.0):
        if not loop_only:
            self.queue.extend(filenames)

        # As Ren'Py's Channel.enqueue: what to play again when the queue runs out.
        self.loop = list(filenames) if loop else [ ]

    def set_volume(self, volume):
        self.chan_volume = volume

    def get_pos(self):
        pass

    def set_pan(self, pan, delay):
        pass

    def set_secondary_volume(self, volume, delay):
        pass

    def pause(self):
        # TODO
        pass

    def unpause(self):
        # TODO
        pass

    def reload(self):
        return

    def read_video(self):
        return renpyvita.video_read_video()

    # The GL texture zero-copy frames are converted into, wrapped once.
    texture = None
    texture_key = None

    def zero_copy(self):
        """
        True if the playing movie decodes into GPU-shared memory, so its
        frames come from read_video_texture rather than read_video.
        """

        if hw_available():
            return bool(self.handle)

        return renpyvita.video_zero_copy()

    def read_video_texture(self):
        """
        Returns a TextureGrid when a new frame of a zero-copy movie has been
        converted, else None. Every frame goes into the same GL texture.
        """

        if hw_available():
            r = renpyvita.hwvideo_read_texture(self.handle) if self.handle else None
        else:
            r = renpyvita.video_read_texture()

        if r is None:
            return None

        if r != self.texture_key:
            import renpy.gl.gltexture
            self.texture = renpy.gl.gltexture.texture_grid_from_number(*r)
            self.texture_key = r

        return self.texture

    def video_ready(self):
        return 1

#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The Flipper's sound on this PC's, live: what QEMU's wav backend writes to
a FIFO (the launcher's `run --audio pc`), played through PipeWire's pw-play
as it comes. Standard library only.

    fo_sound.py FD QEMU_PID

FD: the FIFO, open (the launcher's, so that QEMU's open of it never waits);
its first 44 bytes are the WAV header, the rest 16-bit stereo at 48 kHz.
The FIFO is always drained: what the player cannot take at once is dropped,
so QEMU never waits on it. It ends when QEMU does. FO_SOUND_VOLUME (0-1)
sets the player's volume (default 1).
"""
import fcntl, os, select, subprocess, sys, time

fd, qemu = int(sys.argv[1]), int(sys.argv[2])
volume = os.environ.get("FO_SOUND_VOLUME", "1")


def alive():
    try:
        os.kill(qemu, 0)
        return True
    except OSError:
        return False


player = subprocess.Popen(["pw-play", "-a", "--rate", "48000", "--channels",
                           "2", "--format", "s16", "--volume", volume, "-"],
                          stdin=subprocess.PIPE)
fcntl.fcntl(player.stdin, fcntl.F_SETFL,
            fcntl.fcntl(player.stdin, fcntl.F_GETFL) | os.O_NONBLOCK)
header, played, dropped, told = 44, 0, 0, time.time()
carry = b""                             # (a frame's first bytes)
while alive():
    if not select.select([fd], [], [], 0.5)[0]:
        continue
    data = os.read(fd, 1 << 16)
    if header:
        skip = min(header, len(data))
        header -= skip
        data = data[skip:]
    data = carry + data
    whole = len(data) - len(data) % 4   # (whole frames: 2 channels, s16)
    data, carry = data[:whole], data[whole:]
    for i in range(0, len(data), 4096):
        piece = data[i:i + 4096]        # (up to PIPE_BUF: all of it or none)
        try:
            n = os.write(player.stdin.fileno(), piece)
        except (BlockingIOError, BrokenPipeError):
            n = 0
        played += n
        dropped += len(piece) - n
    if time.time() - told > 5:
        print(f"fo_sound: {played} bytes played, {dropped} dropped",
              flush=True)
        told = time.time()
print(f"fo_sound: QEMU ended; {played} bytes played, {dropped} dropped",
      flush=True)
try:
    player.stdin.close()
except OSError:
    pass
player.wait(5)

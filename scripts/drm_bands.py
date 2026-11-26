#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Four bands on the HDMI monitor, in a pixel format of one's choice: red,
green, blue and 50 % grey, side by side, held a while. It runs on Flipper OS
(as root, with no other DRM master: the Minimal profile), and is the Linux
check's look at the VOP2's formats (sent in over the console).

    python3 drm_bands.py FOURCC SECONDS

FOURCC: XR24, AR24, AB24, XB24 (8 bits a colour), AR30, XR30, AB30, XB30 (10),
RG24, BG24 (24-bit), RG16. With the DRM's own ioctls: the HDMI connector and
its CRTC, a dumb buffer filled, ADDFB2, SETCRTC. Standard library only.
"""
import ctypes, fcntl, mmap, os, struct, sys, time

def iowr(nr, size):
    return (3 << 30) | (size << 16) | (0x64 << 8) | nr

def ioctl(fd, nr, fmt, *vals):
    buf = bytearray(struct.pack(fmt, *vals))
    fcntl.ioctl(fd, iowr(nr, len(buf)), buf)
    return struct.unpack(fmt, buf)

fourcc, secs = sys.argv[1], float(sys.argv[2])
fd = os.open("/dev/dri/card1", os.O_RDWR)
RES = "<4Q8I"
r = ioctl(fd, 0xA0, RES, *([0] * 12))
ncrtc, nconn = r[5], r[6]
cb, nb = (ctypes.c_uint32 * ncrtc)(), (ctypes.c_uint32 * nconn)()
r = ioctl(fd, 0xA0, RES, 0, ctypes.addressof(cb), ctypes.addressof(nb), 0,
          0, ncrtc, nconn, 0, 0, 0, 0, 0)
CONN = "<4Q12I"
MODE = "<I10H3I32s"
msize = struct.calcsize(MODE)
for cid in nb:
    c = ioctl(fd, 0xA7, CONN, 0, 0, 0, 0, 0, 0, 0, 0, cid, 0, 0, 0, 0, 0, 0, 0)
    nmodes, nenc = c[4], c[6]
    modes = (ctypes.c_uint8 * (msize * max(nmodes, 1)))()
    encs = (ctypes.c_uint32 * max(nenc, 1))()
    c = ioctl(fd, 0xA7, CONN, ctypes.addressof(encs), ctypes.addressof(modes),
              0, 0, nmodes, 0, nenc, 0, cid, 0, 0, 0, 0, 0, 0, 0)
    if c[11] == 1 and c[9] == 11 and nmodes:       # connected, HDMI-A
        mode = bytes(modes[:msize])
        enc = ioctl(fd, 0xA6, "<5I", encs[0], 0, 0, 0, 0)
        crtc = enc[2] or cb[(enc[3] & -enc[3]).bit_length() - 1]
        break
else:
    sys.exit("no HDMI monitor")
w, h = struct.unpack_from("<I2H", mode)[1], struct.unpack_from("<I10H", mode)[6]
bpp = {"RG16": 16, "BG24": 24, "RG24": 24}.get(fourcc, 32)
d = ioctl(fd, 0xB2, "<6IQ", h, w, bpp, 0, 0, 0, 0)
handle, pitch, size = d[4], d[5], d[6]
off = ioctl(fd, 0xB3, "<IIQ", handle, 0, 0)[2]
m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
              offset=off)
bands = [(1.0, 0, 0), (0, 1.0, 0), (0, 0, 1.0), (0.5, 0.5, 0.5)]
def pixel(rgb):
    r, g, b = rgb
    if fourcc in ("AR30", "XR30"):
        return struct.pack("<I", 3 << 30 | round(r * 1023) << 20 |
                           round(g * 1023) << 10 | round(b * 1023))
    if fourcc in ("AB30", "XB30"):
        return struct.pack("<I", 3 << 30 | round(b * 1023) << 20 |
                           round(g * 1023) << 10 | round(r * 1023))
    if fourcc == "RG16":
        return struct.pack("<H", round(r * 31) << 11 | round(g * 63) << 5 |
                           round(b * 31))
    if fourcc == "RG24":
        return bytes([round(b * 255), round(g * 255), round(r * 255)])
    if fourcc == "BG24":
        return bytes([round(r * 255), round(g * 255), round(b * 255)])
    if fourcc in ("AB24", "XB24"):
        return bytes([round(r * 255), round(g * 255), round(b * 255), 255])
    return bytes([round(b * 255), round(g * 255), round(r * 255), 255])
row = b"".join(pixel(bands[x * 4 // w]) for x in range(w))
for y in range(h):
    m[y * pitch:y * pitch + len(row)] = row
code = struct.unpack("<I", fourcc.encode())[0]
fb = ioctl(fd, 0xB8, "<5I4I4I4I4xQQQQ", 0, w, h, code, 0, handle, 0, 0, 0,
           pitch, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)[0]
cid_arr = (ctypes.c_uint32 * 1)(cid)
ioctl(fd, 0xA2, "<QIIIIIII" + MODE[1:], ctypes.addressof(cid_arr), 1, crtc,
      fb, 0, 0, 0, 1, *struct.unpack(MODE, mode))
print(f"shown: {fourcc} {w}x{h} fb {fb} crtc {crtc}", flush=True)
time.sleep(secs)

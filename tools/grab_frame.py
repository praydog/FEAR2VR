"""Save what the game is ACTUALLY rendering, from the shared frame mapping.

Every claim about the viewmodel in this project has so far been a claim about a socket transform or
a counter -- never about a pixel. `AGENTS.md` records that the engine's own desktop capture returns
black, so "take a screenshot" had no answer and the question went unasked.

It has one now: the VR pipeline already publishes the finished frame into `Local\\fear2vr_frame` for
the 64-bit host to upload. That buffer is the image on the headset. Reading it from a third process
costs the game nothing and is the only evidence in this project that a rendered thing moved.

    python tools/grab_frame.py out.bmp

Layout mirrors shared/xr/SharedFrame.hpp. The offsets are restated here rather than shared, because
this is a debugging tool that must keep working when the mod does not -- but the magic and the
asserted sizes are checked, so a schema change shows up as a refusal rather than a garbled picture.
"""

import ctypes
import ctypes.wintypes as wt
import struct
import sys

FILE_MAP_READ = 0x0004

# shared/xr/SharedFrame.hpp
MAGIC = 0x32524546  # 'FER2'
HEADER_BYTES = 128
HOST_STATE_BYTES = 64
HANDS_STATE_BYTES = 256
PAYLOAD_OFFSET = HEADER_BYTES + HOST_STATE_BYTES + HANDS_STATE_BYTES
MAX_BYTES = 2560 * 1440 * 4
SLOTS = 3
NAME = "Local\\fear2vr_frame"

k32 = ctypes.windll.kernel32
k32.OpenFileMappingW.restype = wt.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def read_frame():
    h = k32.OpenFileMappingW(FILE_MAP_READ, False, NAME)
    if not h:
        raise SystemExit("mapping '%s' not open -- is the mod injected and publishing?" % NAME)

    total = PAYLOAD_OFFSET + SLOTS * MAX_BYTES
    base = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, total)
    if not base:
        raise SystemExit("MapViewOfFile failed: %d" % ctypes.get_last_error())

    hdr = ctypes.string_at(base, HEADER_BYTES)
    magic, version = struct.unpack_from("<II", hdr, 16)
    if magic != MAGIC:
        raise SystemExit("bad magic 0x%08X -- wrong mapping or a schema change" % magic)

    seq, layout = struct.unpack_from("<II", hdr, 24)
    width, height, pitch, nbytes = struct.unpack_from("<IIII", hdr, 32)
    bgra, pid, frames = struct.unpack_from("<III", hdr, 48)
    slot = struct.unpack_from("<I", hdr, 60)[0]

    if width == 0 or height == 0 or nbytes == 0:
        raise SystemExit("header reports no frame yet (%dx%d)" % (width, height))

    off = PAYLOAD_OFFSET + (slot % SLOTS) * MAX_BYTES
    payload = ctypes.string_at(base + off, min(nbytes, MAX_BYTES))

    return {
        "seq": seq, "layout": layout, "width": width, "height": height,
        "pitch": pitch, "bytes": nbytes, "bgra": bgra, "pid": pid,
        "frames": frames, "slot": slot, "pixels": payload,
    }


def write_bmp(path, f):
    w, h, pitch = f["width"], f["height"], f["pitch"]
    row_bytes = w * 4
    # BMP rows run bottom-up, and the writer's pitch is NOT width * 4 -- publishing it wrong is how
    # an image comes out sheared rather than obviously broken.
    rows = []
    for y in range(h - 1, -1, -1):
        start = y * pitch
        rows.append(f["pixels"][start:start + row_bytes])
    body = b"".join(rows)

    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(body), 2835, 2835, 0, 0)
    header = struct.pack("<2sIHHI", b"BM", 14 + len(info) + len(body), 0, 0, 14 + len(info))
    with open(path, "wb") as fh:
        fh.write(header + info + body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "frame.bmp"
    f = read_frame()
    write_bmp(out, f)
    print("%s  %dx%d pitch=%d layout=%d slot=%d seq=%d frames=%d pid=%d"
          % (out, f["width"], f["height"], f["pitch"], f["layout"], f["slot"], f["seq"],
             f["frames"], f["pid"]))


if __name__ == "__main__":
    main()

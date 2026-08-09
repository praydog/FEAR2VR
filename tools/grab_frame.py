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

# shared/xr/SharedFrame.hpp -- restated, not shared, because this tool must keep working when the
# mod does not. MapViewOfFile's offset argument has to land on a 64 KiB allocation-granularity
# boundary (kViewGranularity in the C++ header), which is why PAYLOAD_OFFSET and the per-slot
# strides below are rounded up rather than packed tight against the header sizes -- get this wrong
# and the payload read comes from the wrong slot instead of refusing outright.
MAGIC = 0x32524546  # 'FER2'
# Must match kSharedFrameVersion, and it is in the object NAME as well as the header -- see NAME.
VERSION = 6
HEADER_BYTES = 128
HOST_STATE_BYTES = 64
HANDS_STATE_BYTES = 256
HAPTICS_STATE_BYTES = 576  # xr::HapticsState -- 64-byte head + 16 x 32-byte ring slots
UI_HEADER_BYTES = 64
VIEW_GRANULARITY = 64 * 1024


def _align_up(value, granularity):
    return (value + granularity - 1) // granularity * granularity


# Native per-eye back buffer capacity -- 4320x2224 rounded up to a height of 2240 for headroom,
# same as kFrameCapacityWidth/Height. The UI layer is HALF that in each dimension: UICapture reads
# the HUD off the same back buffer at half its size, same as kUiMaxBytes.
FRAME_CAPACITY_WIDTH = 4320
FRAME_CAPACITY_HEIGHT = 2240
MAX_BYTES = FRAME_CAPACITY_WIDTH * FRAME_CAPACITY_HEIGHT * 4
UI_MAX_BYTES = (FRAME_CAPACITY_WIDTH // 2) * (FRAME_CAPACITY_HEIGHT // 2) * 4
SLOTS = 3
UI_SLOTS = 2

# HAPTICS_STATE_BYTES is in this sum even though it does not currently move PAYLOAD_OFFSET: the
# control block is far below the 64 KiB granularity either way, so the rounding absorbs it. Stating
# it anyway keeps the formula a mirror of the C++ one rather than a value that happens to agree,
# which is what stops it silently diverging the first time a block does cross the boundary.
PAYLOAD_OFFSET = _align_up(
    HEADER_BYTES + HOST_STATE_BYTES + HANDS_STATE_BYTES + HAPTICS_STATE_BYTES + UI_HEADER_BYTES,
    VIEW_GRANULARITY)
# The STRIDE between slots, not a slot's own byte count -- kSlotStride in the C++ header. Padded up
# so every slot boundary is still a valid MapViewOfFile offset, which is what the mod and the host
# actually need; this tool maps the whole section in one view and only cares about getting the same
# addresses they'd compute.
SLOT_STRIDE = _align_up(MAX_BYTES, VIEW_GRANULARITY)
UI_PAYLOAD_OFFSET = _align_up(PAYLOAD_OFFSET + SLOTS * SLOT_STRIDE, VIEW_GRANULARITY)
UI_SLOT_STRIDE = _align_up(UI_MAX_BYTES, VIEW_GRANULARITY)
TOTAL_BYTES = UI_PAYLOAD_OFFSET + UI_SLOTS * UI_SLOT_STRIDE  # kSharedFrameTotalBytes: frame AND UI slots

# VERSION-SUFFIXED, matching xr::kSharedFrameName. A bare name let a reloaded mod re-stamp the
# section a connected reader was already using; the version now lives in the object name so
# mismatched builds cannot meet at all.
NAME = "Local\\fear2vr_frame_v%u" % VERSION

k32 = ctypes.windll.kernel32
k32.OpenFileMappingW.restype = wt.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def read_frame():
    h = k32.OpenFileMappingW(FILE_MAP_READ, False, NAME)
    if not h:
        raise SystemExit("mapping '%s' not open -- is the mod injected and publishing?" % NAME)

    # The WHOLE section, including the UI slots -- kSharedFrameTotalBytes, not just the header and
    # frame slots. The mod and the 64-bit host split this into one view per slot because a 32-bit
    # process can struggle to find ~130 MB contiguous; this tool has no such constraint (a 64-bit
    # python.exe maps it in one call) and no reason to duplicate that machinery.
    total = TOTAL_BYTES
    base = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, total)
    if not base:
        raise SystemExit("MapViewOfFile failed: %d" % ctypes.get_last_error())

    hdr = ctypes.string_at(base, HEADER_BYTES)
    magic, version = struct.unpack_from("<II", hdr, 16)
    if magic != MAGIC:
        raise SystemExit("bad magic 0x%08X -- wrong mapping or a schema change" % magic)
    # ASSERTED, not merely unpacked. The name carries the version now, so reaching a mismatched
    # section should be impossible -- which is exactly why a silent mismatch here would be baffling
    # rather than obvious. Restated offsets are only correct for the version they were written for.
    if version != VERSION:
        raise SystemExit("shared frame is version %u, this tool understands %u" % (version, VERSION))

    seq, layout = struct.unpack_from("<II", hdr, 24)
    width, height, pitch, nbytes = struct.unpack_from("<IIII", hdr, 32)
    bgra, pid, frames = struct.unpack_from("<III", hdr, 48)
    slot = struct.unpack_from("<I", hdr, 60)[0]

    if width == 0 or height == 0 or nbytes == 0:
        raise SystemExit("header reports no frame yet (%dx%d)" % (width, height))

    off = PAYLOAD_OFFSET + (slot % SLOTS) * SLOT_STRIDE
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

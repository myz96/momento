"""Frame extraction from our own MJPEG AVI files, no codecs.

The firmware writes plain RIFF chunks: every video frame is a `00dc`
chunk whose payload is a complete JPEG. We scan for those chunks and
verify each payload starts with the JPEG SOI marker, which also rejects
the false `00dc` hits inside the idx1 index. The `avih` header gives the
frame period, so each frame gets a real timestamp.
"""

import mmap
import struct
from dataclasses import dataclass

DEFAULT_FPS = 15.0
JPEG_SOI = b"\xff\xd8"


@dataclass
class FrameRef:
    t: float  # seconds from the start
    offset: int  # payload offset in the file
    size: int  # payload size in bytes


def _read_fps(m) -> float:
    # dwMicroSecPerFrame is the first field of the avih payload:
    # 'avih' + u32 chunk size + u32 microseconds-per-frame.
    pos = m.find(b"avih")
    if pos < 0 or pos + 12 > len(m):
        return DEFAULT_FPS
    (us_per_frame,) = struct.unpack_from("<I", m, pos + 8)
    if us_per_frame <= 0 or us_per_frame > 10_000_000:
        return DEFAULT_FPS
    return 1_000_000 / us_per_frame


def scan_frames(path: str) -> tuple[float, list[FrameRef]]:
    """Returns (fps, frames) for the AVI file at path."""
    with open(path, "rb") as f:
        try:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:  # empty file
            return DEFAULT_FPS, []
        with m:
            fps = _read_fps(m)
            frames: list[FrameRef] = []
            pos = 0
            while True:
                hit = m.find(b"00dc", pos)
                if hit < 0 or hit + 8 > len(m):
                    break
                (size,) = struct.unpack_from("<I", m, hit + 4)
                start = hit + 8
                if (
                    0 < size <= len(m) - start
                    and m[start : start + 2] == JPEG_SOI
                ):
                    frames.append(
                        FrameRef(t=len(frames) / fps, offset=start, size=size)
                    )
                    pos = start + size
                else:
                    pos = hit + 4
            return fps, frames


def pick_frames(
    frames: list[FrameRef], fps: float, every_s: float, max_frames: int
) -> list[FrameRef]:
    """Picks one frame per every_s seconds, capped at max_frames.

    When the cap bites, the picks spread evenly across the whole clip
    instead of covering only the start.
    """
    if not frames:
        return []
    step = max(1, round(every_s * fps))
    picks = frames[::step]
    if len(picks) > max_frames:
        if max_frames == 1:
            return [frames[0]]
        n = len(frames)
        picks = [frames[round(k * (n - 1) / (max_frames - 1))] for k in range(max_frames)]
    return picks


def read_frame(path: str, ref: FrameRef) -> bytes:
    with open(path, "rb") as f:
        f.seek(ref.offset)
        return f.read(ref.size)

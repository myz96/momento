import struct

import pytest
from httpx import ASGITransport, AsyncClient

from momento_backend.main import app

FAKE_TRANSCRIPT_TEXT = "fake transcript about the battery"

R2_VARS = [
    "MOMENTO_R2_ACCOUNT_ID",
    "MOMENTO_R2_ACCESS_KEY_ID",
    "MOMENTO_R2_SECRET_ACCESS_KEY",
    "MOMENTO_R2_BUCKET",
    "MOMENTO_API_KEY",
]


@pytest.fixture(autouse=True)
def _isolate_from_real_r2(monkeypatch):
    """Tests must never touch the real bucket, even with a filled .env
    (main.py loads it at import time)."""
    for var in R2_VARS:
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setenv("MOMENTO_TZ", "UTC")


@pytest.fixture(autouse=True)
def _fake_transcriber(monkeypatch):
    """No test may ever load real whisper weights."""
    monkeypatch.setenv("MOMENTO_FAKE_TRANSCRIPT", FAKE_TRANSCRIPT_TEXT)


@pytest.fixture
async def env(tmp_path, monkeypatch):
    media_dir = tmp_path / "media"
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(media_dir))
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac, media_dir


@pytest.fixture
async def client(env) -> AsyncClient:
    return env[0]


def tiny_jpeg(tag: int) -> bytes:
    return b"\xff\xd8" + bytes([tag]) * 10 + b"\xff\xd9"


def synthetic_avi(n_frames: int, us_per_frame: int = 66_666) -> bytes:
    buf = b"RIFF\x00\x00\x00\x00AVI "
    buf += b"avih" + struct.pack("<I", 56) + struct.pack("<I", us_per_frame)
    buf += b"\x00" * 52
    for i in range(n_frames):
        jpg = tiny_jpeg(i % 250)
        buf += b"00dc" + struct.pack("<I", len(jpg)) + jpg
    # A fake idx1 entry: a 00dc id whose "payload" is not a JPEG. The
    # scanner must skip it instead of counting a bogus frame.
    buf += b"idx100dc" + struct.pack("<I", 16) + b"\x10\x00\x00\x00" * 4
    return buf

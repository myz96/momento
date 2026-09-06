"""Tests for the AI layer: transcripts, frames, notes, and search."""

import asyncio
import struct

import pytest
from httpx import ASGITransport, AsyncClient

from momento_backend.main import app

FAKE_TEXT = "fake transcript about the battery"


@pytest.fixture
async def env(tmp_path, monkeypatch):
    media_dir = tmp_path / "media"
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(media_dir))
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac, media_dir


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


async def wait_for_transcript(client: AsyncClient, name: str, tries: int = 50):
    for _ in range(tries):
        response = await client.get(f"/media/{name}/transcript")
        if response.status_code == 200:
            return response.json()
        assert response.status_code == 202
        await asyncio.sleep(0.1)
    raise AssertionError(f"transcript for {name} never arrived")


# --- meta store and mtime --------------------------------------------------


async def test_meta_objects_stay_out_of_the_media_list(env) -> None:
    client, media_dir = env
    await client.post("/media", files={"file": ("PHOTO_001.JPG", b"x", "image/jpeg")})
    (media_dir / "_meta" / "notes").mkdir(parents=True)
    (media_dir / "_meta" / "notes" / "PHOTO_001.JPG.json").write_text("{}")
    response = await client.get("/media")
    assert [e["name"] for e in response.json()] == ["PHOTO_001.JPG"]


async def test_uploaded_mtime_wins_over_upload_time(env) -> None:
    client, _ = env
    response = await client.post(
        "/media",
        files={"file": ("PHOTO_002.JPG", b"x", "image/jpeg")},
        data={"mtime": "1757000000"},
    )
    assert response.status_code == 201
    response = await client.get("/media")
    (entry,) = response.json()
    assert entry["mtime"] == 1757000000


# --- transcripts -----------------------------------------------------------


async def test_upload_triggers_eager_transcription(env) -> None:
    client, _ = env
    await client.post("/media", files={"file": ("AUD_001.WAV", b"RIFFx", "audio/wav")})
    record = await wait_for_transcript(client, "AUD_001.WAV")
    assert record["text"] == FAKE_TEXT
    assert record["name"] == "AUD_001.WAV"


async def test_transcript_request_queues_missing_file(env) -> None:
    client, media_dir = env
    media_dir.mkdir(parents=True, exist_ok=True)
    (media_dir / "AUD_002.WAV").write_bytes(b"RIFFy")
    first = await client.get("/media/AUD_002.WAV/transcript")
    assert first.status_code in (200, 202)  # 202 unless the queue outraced us
    record = await wait_for_transcript(client, "AUD_002.WAV")
    assert record["text"] == FAKE_TEXT


async def test_transcript_rejects_non_audio_and_missing(env) -> None:
    client, _ = env
    assert (await client.get("/media/PHOTO_001.JPG/transcript")).status_code == 400
    assert (await client.get("/media/AUD_404.WAV/transcript")).status_code == 404


async def test_backfill_reports_queued_and_done(env) -> None:
    client, media_dir = env
    await client.post("/media", files={"file": ("AUD_003.WAV", b"RIFFa", "audio/wav")})
    await wait_for_transcript(client, "AUD_003.WAV")
    (media_dir / "AUD_004.WAV").write_bytes(b"RIFFb")
    response = await client.post("/transcripts/backfill")
    counts = response.json()
    assert counts["done"] == 1
    assert counts["queued"] == 1
    await wait_for_transcript(client, "AUD_004.WAV")


# --- frames ----------------------------------------------------------------


async def test_frames_extraction_and_fetch(env) -> None:
    client, _ = env
    avi = synthetic_avi(90)  # ~6 s at 15 fps
    await client.post("/media", files={"file": ("VID_001.AVI", avi, "video/x-msvideo")})
    response = await client.get("/media/VID_001.AVI/frames")
    assert response.status_code == 200
    index = response.json()
    assert index["total_frames"] == 90
    assert index["fps"] == pytest.approx(15.0, abs=0.1)
    # every=4 s at 15 fps -> frames 0 and 60
    assert [f["t"] for f in index["frames"]] == [0.0, 4.0]

    frame = await client.get("/media/VID_001.AVI/frames/0")
    assert frame.status_code == 200
    assert frame.content == tiny_jpeg(0)
    assert frame.headers["content-type"] == "image/jpeg"
    assert (await client.get("/media/VID_001.AVI/frames/9")).status_code == 404


async def test_frames_cap_spreads_across_the_clip(env) -> None:
    client, _ = env
    avi = synthetic_avi(90)
    await client.post("/media", files={"file": ("VID_002.AVI", avi, "video/x-msvideo")})
    response = await client.get("/media/VID_002.AVI/frames?every=0.1&max=8")
    index = response.json()
    assert len(index["frames"]) == 8
    assert index["frames"][0]["t"] == 0.0
    assert index["frames"][-1]["t"] == pytest.approx(89 / 15, abs=0.1)


async def test_frames_cap_of_one_returns_the_first_frame(env) -> None:
    client, _ = env
    await client.post(
        "/media", files={"file": ("VID_003.AVI", synthetic_avi(90), "video/x-msvideo")}
    )
    response = await client.get("/media/VID_003.AVI/frames?every=0.1&max=1")
    assert response.status_code == 200
    assert [f["t"] for f in response.json()["frames"]] == [0.0]


async def test_frames_reject_non_video(env) -> None:
    client, _ = env
    await client.post("/media", files={"file": ("AUD_005.WAV", b"RIFFc", "audio/wav")})
    assert (await client.get("/media/AUD_005.WAV/frames")).status_code == 400


# --- notes, catalog, search ------------------------------------------------


async def test_note_lifecycle_and_catalog_join(env) -> None:
    client, _ = env
    await client.post("/media", files={"file": ("PHOTO_010.JPG", b"x", "image/jpeg")})
    await client.post("/media", files={"file": ("AUD_010.WAV", b"RIFFd", "audio/wav")})
    await wait_for_transcript(client, "AUD_010.WAV")

    response = await client.put(
        "/catalog/PHOTO_010.JPG", json={"note": "Zach solders the charge board"}
    )
    assert response.status_code == 200

    catalog = {r["name"]: r for r in (await client.get("/catalog")).json()}
    assert catalog["PHOTO_010.JPG"]["note"] == "Zach solders the charge board"
    assert catalog["PHOTO_010.JPG"]["kind"] == "photo"
    assert catalog["AUD_010.WAV"]["has_transcript"] is True
    assert catalog["AUD_010.WAV"]["note"] is None

    record = (await client.get("/catalog/AUD_010.WAV")).json()
    assert record["transcript"] == FAKE_TEXT

    assert (await client.delete("/catalog/PHOTO_010.JPG")).status_code == 200
    assert (await client.delete("/catalog/PHOTO_010.JPG")).status_code == 404


async def test_note_requires_existing_file(env) -> None:
    client, _ = env
    response = await client.put("/catalog/PHOTO_404.JPG", json={"note": "ghost"})
    assert response.status_code == 404


async def test_search_covers_names_notes_and_transcripts(env) -> None:
    client, _ = env
    await client.post("/media", files={"file": ("PHOTO_020.JPG", b"x", "image/jpeg")})
    await client.post("/media", files={"file": ("AUD_020.WAV", b"RIFFe", "audio/wav")})
    await wait_for_transcript(client, "AUD_020.WAV")
    await client.put("/catalog/PHOTO_020.JPG", json={"note": "Sunset over the HARBOUR"})

    hits = (await client.get("/search", params={"q": "harbour"})).json()
    assert [h["name"] for h in hits] == ["PHOTO_020.JPG"]
    assert hits[0]["matches"][0]["source"] == "note"

    hits = (await client.get("/search", params={"q": "battery"})).json()
    assert [h["name"] for h in hits] == ["AUD_020.WAV"]
    assert any(m["source"] == "transcript" for m in hits[0]["matches"])

    hits = (await client.get("/search", params={"q": "photo_020"})).json()
    assert [h["name"] for h in hits] == ["PHOTO_020.JPG"]

    # Every token must match somewhere; a half-hit is no hit.
    hits = (await client.get("/search", params={"q": "sunset battery"})).json()
    assert hits == []

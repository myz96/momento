import pytest
from conftest import synthetic_avi, tiny_jpeg

from momento_backend import mcp_server


@pytest.fixture
def media_dir(tmp_path, monkeypatch):
    media = tmp_path / "media"
    media.mkdir(parents=True)
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(media))
    return media


def test_note_search_and_listing_roundtrip(media_dir) -> None:
    (media_dir / "PHOTO_001.JPG").write_bytes(tiny_jpeg(1))
    assert "note saved" in mcp_server.save_note(
        "PHOTO_001.JPG", "Sunset over the harbour"
    )
    assert "PHOTO_001.JPG" in mcp_server.search_moments("harbour")
    assert "no matches" in mcp_server.search_moments("submarine")
    listing = mcp_server.list_moments(kind="photo")
    assert "PHOTO_001.JPG" in listing
    assert mcp_server.list_moments(kind="clip") == "[]"
    record = mcp_server.get_moment("PHOTO_001.JPG")
    assert "Sunset over the harbour" in record


def test_images_flow_through(media_dir) -> None:
    (media_dir / "PHOTO_002.JPG").write_bytes(tiny_jpeg(2))
    (media_dir / "VID_001.AVI").write_bytes(synthetic_avi(45))
    photo = mcp_server.view_photo("PHOTO_002.JPG")
    assert photo.data == tiny_jpeg(2)
    result = mcp_server.view_clip_frames("VID_001.AVI")
    assert "3.0s clip" in result[0]
    assert result[1].data == tiny_jpeg(0)


def test_errors_are_value_errors(media_dir) -> None:
    with pytest.raises(ValueError, match="No such file"):
        mcp_server.get_moment("PHOTO_404.JPG")
    with pytest.raises(ValueError, match="Invalid file name"):
        mcp_server.view_photo("../evil.jpg")
    (media_dir / "AUD_001.WAV").write_bytes(b"RIFFx")
    with pytest.raises(ValueError):
        mcp_server.view_photo("AUD_001.WAV")


def test_transcript_tool_queues_then_serves(media_dir) -> None:
    import time

    (media_dir / "AUD_002.WAV").write_bytes(b"RIFFy")
    first = mcp_server.get_transcript("AUD_002.WAV")
    if "processing" in first:
        for _ in range(50):
            time.sleep(0.1)
            first = mcp_server.get_transcript("AUD_002.WAV")
            if "processing" not in first:
                break
    assert "battery" in first

import pytest
from httpx import ASGITransport, AsyncClient

from momento_backend.main import app

PAYLOAD = b"0123456789abcdef"


@pytest.fixture
async def client(tmp_path, monkeypatch) -> AsyncClient:
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(tmp_path / "media"))
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        await ac.post(
            "/media", files={"file": ("AUD_001.WAV", PAYLOAD, "audio/wav")}
        )
        yield ac


@pytest.mark.asyncio
async def test_full_download_advertises_ranges(client: AsyncClient) -> None:
    response = await client.get("/media/AUD_001.WAV")
    assert response.status_code == 200
    assert response.headers["accept-ranges"] == "bytes"
    assert response.headers["content-length"] == str(len(PAYLOAD))
    assert response.content == PAYLOAD


@pytest.mark.asyncio
async def test_bounded_range(client: AsyncClient) -> None:
    response = await client.get(
        "/media/AUD_001.WAV", headers={"Range": "bytes=4-7"}
    )
    assert response.status_code == 206
    assert response.content == b"4567"
    assert response.headers["content-range"] == f"bytes 4-7/{len(PAYLOAD)}"
    assert response.headers["content-length"] == "4"


@pytest.mark.asyncio
async def test_open_ended_range(client: AsyncClient) -> None:
    response = await client.get(
        "/media/AUD_001.WAV", headers={"Range": "bytes=10-"}
    )
    assert response.status_code == 206
    assert response.content == PAYLOAD[10:]


@pytest.mark.asyncio
async def test_webkit_probe_range(client: AsyncClient) -> None:
    # WebKit opens media with exactly this probe.
    response = await client.get(
        "/media/AUD_001.WAV", headers={"Range": "bytes=0-1"}
    )
    assert response.status_code == 206
    assert response.content == b"01"


@pytest.mark.asyncio
async def test_suffix_range(client: AsyncClient) -> None:
    response = await client.get(
        "/media/AUD_001.WAV", headers={"Range": "bytes=-4"}
    )
    assert response.status_code == 206
    assert response.content == b"cdef"


@pytest.mark.asyncio
async def test_out_of_bounds_range_is_416(client: AsyncClient) -> None:
    response = await client.get(
        "/media/AUD_001.WAV", headers={"Range": "bytes=99-"}
    )
    assert response.status_code == 416

import pytest
from httpx import ASGITransport, AsyncClient

from momento_backend.main import app

KEY = "test-api-key-123"


@pytest.fixture
async def client(tmp_path, monkeypatch) -> AsyncClient:
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(tmp_path / "media"))
    monkeypatch.setenv("MOMENTO_API_KEY", KEY)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest.mark.asyncio
async def test_media_rejects_missing_key(client: AsyncClient) -> None:
    assert (await client.get("/media")).status_code == 401
    assert (await client.get("/media/PHOTO_001.JPG")).status_code == 401
    response = await client.post(
        "/media", files={"file": ("PHOTO_001.JPG", b"x", "image/jpeg")}
    )
    assert response.status_code == 401


@pytest.mark.asyncio
async def test_media_rejects_wrong_key(client: AsyncClient) -> None:
    response = await client.get(
        "/media", headers={"Authorization": "Bearer nope"}
    )
    assert response.status_code == 401


@pytest.mark.asyncio
async def test_media_accepts_bearer_key(client: AsyncClient) -> None:
    headers = {"Authorization": f"Bearer {KEY}"}
    payload = b"fake jpeg bytes"
    response = await client.post(
        "/media",
        files={"file": ("PHOTO_001.JPG", payload, "image/jpeg")},
        headers=headers,
    )
    assert response.status_code == 201
    response = await client.get("/media", headers=headers)
    assert response.status_code == 200


@pytest.mark.asyncio
async def test_download_accepts_query_key(client: AsyncClient) -> None:
    headers = {"Authorization": f"Bearer {KEY}"}
    await client.post(
        "/media",
        files={"file": ("PHOTO_002.JPG", b"pixels", "image/jpeg")},
        headers=headers,
    )
    response = await client.get(f"/media/PHOTO_002.JPG?key={KEY}")
    assert response.status_code == 200
    assert response.content == b"pixels"


@pytest.mark.asyncio
async def test_health_stays_open(client: AsyncClient) -> None:
    assert (await client.get("/health")).status_code == 200

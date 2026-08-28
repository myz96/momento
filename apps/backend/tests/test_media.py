import pytest
from httpx import ASGITransport, AsyncClient

from momento_backend.main import app


@pytest.fixture
async def client(tmp_path, monkeypatch) -> AsyncClient:
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(tmp_path / "media"))
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


@pytest.mark.asyncio
async def test_upload_then_list_then_download(client: AsyncClient) -> None:
    payload = b"fake jpeg bytes"
    response = await client.post(
        "/media", files={"file": ("PHOTO_001.JPG", payload, "image/jpeg")}
    )
    assert response.status_code == 201
    assert response.json() == {"name": "PHOTO_001.JPG", "size": len(payload)}

    response = await client.get("/media")
    assert response.status_code == 200
    assert response.json() == [{"name": "PHOTO_001.JPG", "size": len(payload)}]

    response = await client.get("/media/PHOTO_001.JPG")
    assert response.status_code == 200
    assert response.content == payload


@pytest.mark.asyncio
async def test_list_is_empty_at_start(client: AsyncClient) -> None:
    response = await client.get("/media")
    assert response.status_code == 200
    assert response.json() == []


@pytest.mark.asyncio
async def test_upload_rejects_bad_names(client: AsyncClient) -> None:
    for bad in ["../evil.jpg", ".hidden.jpg", "notes.txt"]:
        response = await client.post(
            "/media", files={"file": (bad, b"x", "application/octet-stream")}
        )
        assert response.status_code == 400, bad


@pytest.mark.asyncio
async def test_download_missing_file_is_404(client: AsyncClient) -> None:
    response = await client.get("/media/PHOTO_404.JPG")
    assert response.status_code == 404


@pytest.mark.asyncio
async def test_download_rejects_traversal(client: AsyncClient) -> None:
    # The router refuses the encoded slash (404) or safe_name rejects the
    # decoded name (400). Both keep the path inside the storage dir.
    response = await client.get("/media/..%2Fsecrets.jpg")
    assert response.status_code in (400, 404)

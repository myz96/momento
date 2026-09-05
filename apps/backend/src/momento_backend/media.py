"""Media routes: uploads from the companion app.

The storage backend comes from the environment (see storage.py): local
disk by default, Cloudflare R2 when the MOMENTO_R2_* variables are set.
"""

from pathlib import Path

from fastapi import APIRouter, HTTPException, UploadFile
from fastapi.responses import StreamingResponse
from starlette.concurrency import run_in_threadpool

from momento_backend.storage import MediaStorage, storage_from_env

router = APIRouter(prefix="/media", tags=["media"])

ALLOWED_SUFFIXES = {".jpg", ".jpeg", ".wav", ".avi"}

CONTENT_TYPES = {
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".wav": "audio/wav",
    ".avi": "video/x-msvideo",
}


def get_storage() -> MediaStorage:
    # Resolved per request so tests can swap MOMENTO_MEDIA_DIR; the boto3
    # client behind R2Storage is cheap to construct.
    return storage_from_env(ALLOWED_SUFFIXES)


def safe_name(name: str) -> str:
    """Rejects names that could escape the storage namespace."""
    if (
        not name
        or name != Path(name).name
        or name.startswith(".")
        or Path(name).suffix.lower() not in ALLOWED_SUFFIXES
    ):
        raise HTTPException(status_code=400, detail=f"Invalid file name: {name!r}")
    return name


@router.post("", status_code=201)
async def upload_media(file: UploadFile) -> dict[str, int | str]:
    name = safe_name(file.filename or "")
    storage = get_storage()
    size = await run_in_threadpool(storage.save, name, file.file)
    return {"name": name, "size": size}


@router.get("")
async def list_media() -> list[dict[str, int | str]]:
    storage = get_storage()
    entries = await run_in_threadpool(storage.list)
    return [{"name": e.name, "size": e.size} for e in entries]


@router.get("/{name}")
async def download_media(name: str) -> StreamingResponse:
    name = safe_name(name)
    storage = get_storage()
    if not await run_in_threadpool(storage.exists, name):
        raise HTTPException(status_code=404, detail=f"No such file: {name}")
    media_type = CONTENT_TYPES.get(
        Path(name).suffix.lower(), "application/octet-stream"
    )
    return StreamingResponse(storage.stream(name), media_type=media_type)

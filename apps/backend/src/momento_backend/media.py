"""Media routes: uploads from the companion app.

The storage backend comes from the environment (see storage.py): local
disk by default, Cloudflare R2 when the MOMENTO_R2_* variables are set.
"""

from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Request, UploadFile
from fastapi.responses import StreamingResponse
from starlette.concurrency import run_in_threadpool

from momento_backend.auth import require_key
from momento_backend.storage import MediaStorage, storage_from_env

router = APIRouter(
    prefix="/media", tags=["media"], dependencies=[Depends(require_key)]
)

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


def parse_range(header: str, size: int) -> tuple[int, int] | None:
    """Parses a single-range 'bytes=' header; None means serve the whole
    file. WebKit media players require 206 responses to play audio/video."""
    if not header.startswith("bytes="):
        return None
    spec = header[6:].split(",")[0].strip()
    start_s, _, end_s = spec.partition("-")
    try:
        if start_s == "":
            suffix = int(end_s)
            if suffix <= 0:
                raise HTTPException(status_code=416, detail="Bad range")
            return max(0, size - suffix), size - 1
        start = int(start_s)
        end = int(end_s) if end_s else size - 1
    except ValueError:
        return None
    end = min(end, size - 1)
    if start > end or start >= size:
        raise HTTPException(status_code=416, detail="Range out of bounds")
    return start, end


@router.get("/{name}")
async def download_media(name: str, request: Request) -> StreamingResponse:
    name = safe_name(name)
    storage = get_storage()
    size = await run_in_threadpool(storage.size, name)
    if size is None:
        raise HTTPException(status_code=404, detail=f"No such file: {name}")
    media_type = CONTENT_TYPES.get(
        Path(name).suffix.lower(), "application/octet-stream"
    )

    span = None
    range_header = request.headers.get("range")
    if range_header and size > 0:
        span = parse_range(range_header, size)
    if span is not None:
        start, end = span
        return StreamingResponse(
            storage.stream(name, start, end),
            status_code=206,
            media_type=media_type,
            headers={
                "Content-Range": f"bytes {start}-{end}/{size}",
                "Content-Length": str(end - start + 1),
                "Accept-Ranges": "bytes",
            },
        )
    return StreamingResponse(
        storage.stream(name),
        media_type=media_type,
        headers={"Content-Length": str(size), "Accept-Ranges": "bytes"},
    )

"""Media storage: uploads from the companion app land on disk.

The storage directory comes from MOMENTO_MEDIA_DIR (default ./data/media).
S3 or another object store can replace the disk later behind the same
routes.
"""

import os
import uuid
from pathlib import Path

from fastapi import APIRouter, HTTPException, UploadFile
from fastapi.responses import FileResponse

router = APIRouter(prefix="/media", tags=["media"])

ALLOWED_SUFFIXES = {".jpg", ".jpeg", ".wav", ".avi"}
CHUNK_BYTES = 1024 * 1024


def media_dir() -> Path:
    path = Path(os.environ.get("MOMENTO_MEDIA_DIR", "data/media"))
    path.mkdir(parents=True, exist_ok=True)
    return path


def safe_name(name: str) -> str:
    """Rejects names that could escape the storage directory."""
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
    target = media_dir() / name
    # Stage then rename: an interrupted upload never leaves a truncated
    # file under the final name, so the client's size check stays honest.
    # The stage name is unique per request so concurrent uploads of the
    # same name cannot interleave.
    stage = target.with_name(f"{target.name}.{uuid.uuid4().hex}.part")
    size = 0
    try:
        with stage.open("wb") as out:
            while chunk := await file.read(CHUNK_BYTES):
                out.write(chunk)
                size += len(chunk)
    except Exception:
        stage.unlink(missing_ok=True)
        raise
    stage.replace(target)
    return {"name": name, "size": size}


@router.get("")
async def list_media() -> list[dict[str, int | str]]:
    entries = [
        {"name": p.name, "size": p.stat().st_size}
        for p in media_dir().iterdir()
        if p.is_file() and p.suffix.lower() in ALLOWED_SUFFIXES
    ]
    entries.sort(key=lambda e: e["name"])
    return entries


@router.get("/{name}")
async def download_media(name: str) -> FileResponse:
    target = media_dir() / safe_name(name)
    if not target.is_file():
        raise HTTPException(status_code=404, detail=f"No such file: {name}")
    return FileResponse(target)

"""The AI layer: transcripts, frames, notes, and search.

These routes turn stored media into text an agent can work with. The
split of labor: this backend does mechanical extraction (audio → text,
bytes → frames) and stores knowledge (notes); the reading agent does the
understanding and writes down what it learned. See the momento CLI and
the .claude/skills/momento skill for the client side.
"""

import datetime
import tempfile
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Query
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from momento_backend import avi_frames, meta, transcribe
from momento_backend.auth import require_key
from momento_backend.media import get_storage, safe_name
from momento_backend.storage import MediaStorage

router = APIRouter(tags=["catalog"], dependencies=[Depends(require_key)])

KINDS = {".jpg": "photo", ".jpeg": "photo", ".wav": "audio", ".avi": "clip"}


def kind_of(name: str) -> str:
    return KINDS.get(Path(name).suffix.lower(), "unknown")


def _require_file(storage: MediaStorage, name: str) -> int:
    size = storage.size(name)
    if size is None:
        raise HTTPException(status_code=404, detail=f"No such file: {name}")
    return size


# --- transcripts -----------------------------------------------------------


@router.get("/media/{name}/transcript")
async def get_transcript(name: str) -> JSONResponse:
    name = safe_name(name)
    if Path(name).suffix.lower() != ".wav":
        raise HTTPException(
            status_code=400, detail="Transcripts exist for .wav files only"
        )
    storage = get_storage()
    await run_in_threadpool(_require_file, storage, name)
    record = await run_in_threadpool(transcribe.get_transcript, storage, name)
    if record is not None:
        return JSONResponse(record)
    transcribe.queue_transcription(storage, name)
    return JSONResponse({"name": name, "status": "processing"}, status_code=202)


@router.post("/transcripts/backfill")
async def backfill_transcripts() -> dict[str, int]:
    storage = get_storage()
    entries = await run_in_threadpool(storage.list)
    done_keys = set(
        await run_in_threadpool(storage.list_meta, meta.TRANSCRIPTS_PREFIX)
    )
    queued = 0
    done = 0
    for e in entries:
        if Path(e.name).suffix.lower() != ".wav":
            continue
        if meta.transcript_key(e.name) in done_keys:
            done += 1
        elif transcribe.queue_transcription(storage, e.name):
            queued += 1
    return {"queued": queued, "done": done}


# --- frames ----------------------------------------------------------------


def _extract_frames(
    storage: MediaStorage, name: str, every_s: float, max_frames: int
) -> dict:
    with tempfile.NamedTemporaryFile(suffix=".avi") as tmp:
        for chunk in storage.stream(name):
            tmp.write(chunk)
        tmp.flush()
        fps, frames = avi_frames.scan_frames(tmp.name)
        picks = avi_frames.pick_frames(frames, fps, every_s, max_frames)
        entries = []
        for i, ref in enumerate(picks):
            storage.write_meta(
                meta.frame_key(name, i), avi_frames.read_frame(tmp.name, ref)
            )
            entries.append({"i": i, "t": round(ref.t, 2), "size": ref.size})
    index = {
        "name": name,
        "every": every_s,
        "max": max_frames,
        "fps": round(fps, 2),
        "total_frames": len(frames),
        "duration_s": round(len(frames) / fps, 1) if frames else 0.0,
        "frames": entries,
    }
    meta.write_json(storage, meta.frames_index_key(name), index)
    return index


@router.get("/media/{name}/frames")
async def get_frames(
    name: str,
    every: float = Query(4.0, gt=0, le=60),
    max_frames: int = Query(8, alias="max", ge=1, le=32),
) -> dict:
    name = safe_name(name)
    if Path(name).suffix.lower() != ".avi":
        raise HTTPException(status_code=400, detail="Frames exist for .avi files only")
    storage = get_storage()
    await run_in_threadpool(_require_file, storage, name)
    index = await run_in_threadpool(
        meta.read_json, storage, meta.frames_index_key(name)
    )
    if index is not None and index.get("every") == every and index.get("max") == max_frames:
        return index
    return await run_in_threadpool(_extract_frames, storage, name, every, max_frames)


@router.get("/media/{name}/frames/{i}")
async def get_frame(name: str, i: int) -> Response:
    name = safe_name(name)
    storage = get_storage()
    index = await run_in_threadpool(
        meta.read_json, storage, meta.frames_index_key(name)
    )
    if index is None:
        raise HTTPException(
            status_code=404, detail="No frames extracted; GET /media/{name}/frames first"
        )
    if not 0 <= i < len(index.get("frames", [])):
        raise HTTPException(status_code=404, detail=f"No frame {i}")
    data = await run_in_threadpool(storage.read_meta, meta.frame_key(name, i))
    if data is None:
        raise HTTPException(status_code=404, detail=f"No frame {i}")
    return Response(content=data, media_type="image/jpeg")


# --- notes and the catalog -------------------------------------------------


class NoteIn(BaseModel):
    note: str = Field(min_length=1, max_length=20_000)


@router.put("/catalog/{name}")
async def put_note(name: str, body: NoteIn) -> dict:
    name = safe_name(name)
    storage = get_storage()
    await run_in_threadpool(_require_file, storage, name)
    record = {
        "name": name,
        "note": body.note,
        "updated_at": datetime.datetime.now(datetime.UTC).isoformat(
            timespec="seconds"
        ),
    }
    await run_in_threadpool(meta.write_json, storage, meta.note_key(name), record)
    return record


@router.delete("/catalog/{name}")
async def delete_note(name: str) -> dict:
    name = safe_name(name)
    storage = get_storage()
    deleted = await run_in_threadpool(storage.delete_meta, meta.note_key(name))
    if not deleted:
        raise HTTPException(status_code=404, detail=f"No note for {name}")
    return {"name": name, "deleted": True}


def _build_catalog(storage: MediaStorage) -> list[dict]:
    entries = storage.list()
    mtimes = meta.load_mtimes(storage)
    noted = set(storage.list_meta(meta.NOTES_PREFIX))
    transcribed = set(storage.list_meta(meta.TRANSCRIPTS_PREFIX))
    framed = set(storage.list_meta(meta.FRAMES_PREFIX))
    records = []
    for e in entries:
        record = {
            "name": e.name,
            "kind": kind_of(e.name),
            "size": e.size,
            "mtime": mtimes.get(e.name, e.mtime),
            "note": None,
            "has_transcript": meta.transcript_key(e.name) in transcribed,
            "has_frames": meta.frames_index_key(e.name) in framed,
        }
        if meta.note_key(e.name) in noted:
            note = meta.read_json(storage, meta.note_key(e.name))
            if note:
                record["note"] = note.get("note")
        records.append(record)
    return records


@router.get("/catalog")
async def get_catalog() -> list[dict]:
    storage = get_storage()
    return await run_in_threadpool(_build_catalog, storage)


@router.get("/catalog/{name}")
async def get_record(name: str) -> dict:
    name = safe_name(name)
    storage = get_storage()
    size = await run_in_threadpool(_require_file, storage, name)
    mtimes = await run_in_threadpool(meta.load_mtimes, storage)
    note = await run_in_threadpool(meta.read_json, storage, meta.note_key(name))
    transcript = await run_in_threadpool(transcribe.get_transcript, storage, name)
    frames = await run_in_threadpool(
        meta.read_json, storage, meta.frames_index_key(name)
    )
    return {
        "name": name,
        "kind": kind_of(name),
        "size": size,
        "mtime": mtimes.get(name),
        "note": note.get("note") if note else None,
        "transcript": transcript.get("text") if transcript else None,
        "frames": frames,
    }


# --- search ----------------------------------------------------------------


def _snippet(text: str, token: str, radius: int = 60) -> str:
    lower = text.lower()
    pos = lower.find(token)
    if pos < 0:
        return ""
    start = max(0, pos - radius)
    end = min(len(text), pos + len(token) + radius)
    clip = " ".join(text[start:end].split())
    prefix = "…" if start > 0 else ""
    suffix = "…" if end < len(text) else ""
    return f"{prefix}{clip}{suffix}"


def _search(storage: MediaStorage, query: str) -> list[dict]:
    tokens = [t for t in query.lower().split() if t]
    if not tokens:
        return []
    noted = set(storage.list_meta(meta.NOTES_PREFIX))
    transcribed = set(storage.list_meta(meta.TRANSCRIPTS_PREFIX))
    mtimes = meta.load_mtimes(storage)
    results = []
    for e in storage.list():
        sources = {"name": e.name}
        if meta.note_key(e.name) in noted:
            note = meta.read_json(storage, meta.note_key(e.name))
            if note and note.get("note"):
                sources["note"] = note["note"]
        if meta.transcript_key(e.name) in transcribed:
            transcript = meta.read_json(storage, meta.transcript_key(e.name))
            if transcript and transcript.get("text"):
                sources["transcript"] = transcript["text"]
        combined = " ".join(sources.values()).lower()
        if not all(t in combined for t in tokens):
            continue
        matches = []
        for source, text in sources.items():
            for token in tokens:
                snippet = _snippet(text, token)
                if snippet:
                    matches.append({"source": source, "snippet": snippet})
                    break
        results.append(
            {
                "name": e.name,
                "kind": kind_of(e.name),
                "mtime": mtimes.get(e.name, e.mtime),
                "matches": matches,
            }
        )
    return results


@router.get("/search")
async def search(q: str = Query(min_length=1, max_length=500)) -> list[dict]:
    storage = get_storage()
    return await run_in_threadpool(_search, storage, q)

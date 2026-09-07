"""The AI layer: transcripts, frames, notes, and search.

These routes turn stored media into text an agent can work with. The
split of labor: this backend does mechanical extraction (audio → text,
bytes → frames) and stores knowledge (notes); the reading agent does
the understanding and writes down what it learned.

The module has two halves: a service layer of public functions that
take (storage, plain params) and raise ValueError — shared by the HTTP
routes below and the MCP tools in mcp_server.py — and the routes
themselves, which stay thin.
"""

import datetime
import re
import tempfile
import threading
import zoneinfo
from concurrent.futures import ThreadPoolExecutor
from os import environ
from pathlib import Path

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from momento_backend import avi_frames, meta, transcribe
from momento_backend.media import KINDS, get_storage, require_file, safe_name
from momento_backend.storage import MediaStorage

router = APIRouter(tags=["catalog"])

MAX_NOTE_CHARS = 20_000

# Meta objects are small and independent; reading them one by one costs
# an R2 round trip each, so list and search fetch them concurrently.
_reader_pool = ThreadPoolExecutor(max_workers=8, thread_name_prefix="meta-read")

# One extraction per clip at a time, so overlapping requests with
# different parameters cannot interleave frame writes with the index.
_frame_locks: dict[str, threading.Lock] = {}
_frame_locks_guard = threading.Lock()


def kind_of(name: str) -> str:
    return KINDS.get(Path(name).suffix.lower(), "unknown")


def home_tz() -> datetime.tzinfo:
    """Days group in the household timezone (MOMENTO_TZ), not UTC, so
    "what did I do Saturday" means Michael's Saturday on every client."""
    name = environ.get("MOMENTO_TZ", "UTC")
    try:
        return zoneinfo.ZoneInfo(name)
    except (zoneinfo.ZoneInfoNotFoundError, ValueError):
        return datetime.UTC


def _local_time(mtime: int | None) -> datetime.datetime | None:
    """None for absent AND for garbage values (e.g. milliseconds stored
    as seconds) — one bad stored mtime must not 500 the whole catalog."""
    if not mtime:
        return None
    try:
        return datetime.datetime.fromtimestamp(mtime, tz=home_tz())
    except (ValueError, OverflowError, OSError):
        return None


def day_of(mtime: int | None) -> str | None:
    local = _local_time(mtime)
    return local.strftime("%Y-%m-%d") if local else None


def captured_iso(mtime: int | None) -> str | None:
    local = _local_time(mtime)
    return local.isoformat(timespec="minutes") if local else None


def _read_field(storage: MediaStorage, key: str, field: str) -> str | None:
    doc = meta.read_json(storage, key)
    return doc.get(field) if doc else None


def build_catalog(
    storage: MediaStorage, day: str | None = None, kind: str | None = None
) -> list[dict]:
    entries = storage.list()
    mtimes = meta.load_mtimes(storage)
    noted = set(storage.list_meta(meta.NOTES_PREFIX))
    transcribed = set(storage.list_meta(meta.TRANSCRIPTS_PREFIX))
    framed = set(storage.list_meta(meta.FRAMES_PREFIX))
    records = []
    for e in entries:
        # The recorded capture time wins; the storage timestamp (upload
        # time) is the fallback for files uploaded before mtime support.
        mtime = mtimes.get(e.name, e.mtime)
        record = {
            "name": e.name,
            "kind": kind_of(e.name),
            "size": e.size,
            "mtime": mtime,
            "captured": captured_iso(mtime),
            "note": None,
            "has_transcript": meta.transcript_key(e.name) in transcribed,
            "has_frames": meta.frames_index_key(e.name) in framed,
        }
        if kind and record["kind"] != kind:
            continue
        if day and day_of(mtime) != day:
            continue
        records.append(record)
    with_notes = [r for r in records if meta.note_key(r["name"]) in noted]
    futures = {
        r["name"]: _reader_pool.submit(
            _read_field, storage, meta.note_key(r["name"]), "note"
        )
        for r in with_notes
    }
    for r in with_notes:
        r["note"] = futures[r["name"]].result()
    records.sort(key=lambda r: (r["mtime"] or 0, r["name"]))
    return records


def write_note(storage: MediaStorage, name: str, note: str) -> dict:
    note = note.strip()
    if not (1 <= len(note) <= MAX_NOTE_CHARS):
        raise ValueError(f"A note needs 1 to {MAX_NOTE_CHARS} characters")
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    record = {
        "name": name,
        "note": note,
        "updated_at": datetime.datetime.now(datetime.UTC).isoformat(
            timespec="seconds"
        ),
    }
    meta.write_json(storage, meta.note_key(name), record)
    return record


def load_record(storage: MediaStorage, name: str) -> dict | None:
    entry = next((e for e in storage.list() if e.name == name), None)
    if entry is None:
        return None
    mtimes = meta.load_mtimes(storage)
    mtime = mtimes.get(name, entry.mtime)
    note = meta.read_json(storage, meta.note_key(name))
    transcript = transcribe.get_transcript(storage, name, entry.size)
    frames = meta.read_json(storage, meta.frames_index_key(name))
    return {
        "name": name,
        "kind": kind_of(name),
        "size": entry.size,
        "mtime": mtime,
        "captured": captured_iso(mtime),
        "note": note.get("note") if note else None,
        "transcript": transcript.get("text") if transcript else None,
        "frames": frames,
    }


# A real pair's epoch-ms prefixes are stamped in the same sync run,
# seconds apart. A candidate further away belongs to another session —
# a clip whose true pair is gone must get "no pair", never another
# recording's words.
PAIR_WINDOW_MS = 10 * 60 * 1000


def resolve_audio_name(storage: MediaStorage, name: str) -> str:
    """Maps a clip to its paired audio file (…VID_012.AVI → …AUD_012.WAV).

    The device restarts its index after every sync, so several sessions
    can each hold an AUD_012. The pair is the candidate whose epoch-ms
    prefix sits closest to the clip's, within PAIR_WINDOW_MS. Returns
    the input name unchanged when there is no pair."""
    stem = Path(name).stem.upper()
    if Path(name).suffix.lower() != ".avi" or "VID_" not in stem:
        return name
    wanted = f"AUD_{stem.split('VID_', 1)[1]}.WAV"
    candidates = [e.name for e in storage.list() if e.name.upper().endswith(wanted)]
    if not candidates:
        return name

    def prefix_num(n: str) -> int | None:
        m = re.match(r"(\d+)", n)
        return int(m.group(1)) if m else None

    vid_prefix = prefix_num(name)
    if vid_prefix is not None:
        numbered = [
            (abs(p - vid_prefix), c)
            for c in candidates
            if (p := prefix_num(c)) is not None
        ]
        if numbered:
            distance, best = min(numbered)
            return best if distance <= PAIR_WINDOW_MS else name
        return name
    return candidates[0]


def get_or_queue_transcript(storage: MediaStorage, name: str) -> tuple[str, dict | None]:
    """The one transcript entry point for every client: validates the
    kind, resolves a clip to its paired audio, returns (resolved_name,
    record) — record None means a job was queued and the caller should
    retry. Raises ValueError for anything that can never transcribe."""
    if Path(name).suffix.lower() not in (".wav", ".avi"):
        raise ValueError("Transcripts exist for .wav and .avi files only")
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    resolved = resolve_audio_name(storage, name)
    if resolved == name and name.lower().endswith(".avi"):
        raise ValueError(f"No paired audio for {name}")
    if storage.size(resolved) is None:
        raise ValueError(f"No such file: {resolved}")
    record = transcribe.get_transcript(storage, resolved)
    if record is None:
        transcribe.queue_transcription(storage, resolved)
    return resolved, record


def _frame_lock(name: str) -> threading.Lock:
    with _frame_locks_guard:
        return _frame_locks.setdefault(name, threading.Lock())


def clamp_frame_params(every_s: float, max_frames: int) -> tuple[float, int]:
    return min(max(every_s, 0.2), 60.0), min(max(max_frames, 1), 32)


def _index_current(index: dict | None, every_s: float, max_frames: int, size) -> bool:
    return (
        index is not None
        and index.get("every") == every_s
        and index.get("max") == max_frames
        and index.get("size") == size
    )


def get_or_extract_frames(
    storage: MediaStorage, name: str, every_s: float, max_frames: int
) -> dict:
    every_s, max_frames = clamp_frame_params(every_s, max_frames)
    size = storage.size(name)
    if size is None:
        raise ValueError(f"No such file: {name}")
    index = meta.read_json(storage, meta.frames_index_key(name))
    if _index_current(index, every_s, max_frames, size):
        return index
    with _frame_lock(name):
        index = meta.read_json(storage, meta.frames_index_key(name))
        if _index_current(index, every_s, max_frames, size):
            return index
        return _extract_frames(storage, name, every_s, max_frames, size)


def _extract_frames(
    storage: MediaStorage, name: str, every_s: float, max_frames: int, size: int
) -> dict:
    # Drop the old index before touching any frame object: a crash
    # mid-extraction must leave "no frames yet" (re-extract on the next
    # request), never an index whose timestamps describe other bytes.
    storage.delete_meta(meta.frames_index_key(name))
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
        "size": size,
        "every": every_s,
        "max": max_frames,
        "fps": round(fps, 2),
        "total_frames": len(frames),
        "duration_s": round(len(frames) / fps, 1) if frames else 0.0,
        "frames": entries,
    }
    meta.write_json(storage, meta.frames_index_key(name), index)
    return index


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


def search_catalog(storage: MediaStorage, query: str) -> list[dict]:
    tokens = query.lower().split()
    if not tokens:
        return []
    noted = set(storage.list_meta(meta.NOTES_PREFIX))
    transcribed = set(storage.list_meta(meta.TRANSCRIPTS_PREFIX))
    mtimes = meta.load_mtimes(storage)
    entries = storage.list()
    futures = {}
    for e in entries:
        if meta.note_key(e.name) in noted:
            futures[("note", e.name)] = _reader_pool.submit(
                _read_field, storage, meta.note_key(e.name), "note"
            )
        if meta.transcript_key(e.name) in transcribed:
            # Through get_transcript, not a raw read: a transcript whose
            # size fingerprint no longer matches the file must not keep
            # matching searches after a replacement upload.
            futures[("transcript", e.name)] = _reader_pool.submit(
                lambda n=e.name, s=e.size: (
                    transcribe.get_transcript(storage, n, s) or {}
                ).get("text")
            )
    results = []
    for e in entries:
        sources = {"name": e.name}
        for source in ("note", "transcript"):
            future = futures.get((source, e.name))
            text = future.result() if future else None
            if text:
                sources[source] = text
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
        mtime = mtimes.get(e.name, e.mtime)
        results.append(
            {
                "name": e.name,
                "kind": kind_of(e.name),
                "mtime": mtime,
                "captured": captured_iso(mtime),
                "matches": matches,
            }
        )
    return results


# --- routes ----------------------------------------------------------------


@router.get("/media/{name}/transcript")
async def get_transcript(name: str) -> JSONResponse:
    name = safe_name(name)
    storage = get_storage()
    try:
        resolved, record = await run_in_threadpool(
            get_or_queue_transcript, storage, name
        )
    except ValueError as e:
        status = 404 if "No " in str(e) else 400
        raise HTTPException(status_code=status, detail=str(e)) from None
    if record is not None:
        return JSONResponse(record)
    return JSONResponse({"name": resolved, "status": "processing"}, status_code=202)


@router.post("/transcripts/backfill")
async def backfill_transcripts() -> dict[str, int]:
    storage = get_storage()
    entries = await run_in_threadpool(storage.list)
    counts = {"queued": 0, "done": 0, "in_progress": 0}
    for e in entries:
        if Path(e.name).suffix.lower() != ".wav":
            continue
        record = await run_in_threadpool(
            transcribe.get_transcript, storage, e.name, e.size
        )
        if record is not None:
            counts["done"] += 1
        elif transcribe.queue_transcription(storage, e.name):
            counts["queued"] += 1
        else:
            counts["in_progress"] += 1
    return counts


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
    await run_in_threadpool(require_file, storage, name)
    return await run_in_threadpool(
        get_or_extract_frames, storage, name, every, max_frames
    )


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


class NoteIn(BaseModel):
    note: str


@router.put("/catalog/{name}")
async def put_note(name: str, body: NoteIn) -> dict:
    name = safe_name(name)
    storage = get_storage()
    try:
        return await run_in_threadpool(write_note, storage, name, body.note)
    except ValueError as e:
        status = 404 if "No such file" in str(e) else 400
        raise HTTPException(status_code=status, detail=str(e)) from None


@router.delete("/catalog/{name}")
async def delete_note(name: str) -> dict:
    name = safe_name(name)
    storage = get_storage()
    deleted = await run_in_threadpool(storage.delete_meta, meta.note_key(name))
    if not deleted:
        raise HTTPException(status_code=404, detail=f"No note for {name}")
    return {"name": name, "deleted": True}


@router.get("/catalog")
async def get_catalog(
    day: str | None = Query(None, pattern=r"^\d{4}-\d{2}-\d{2}$"),
    kind: str | None = Query(None, pattern=r"^(photo|audio|clip)$"),
) -> list[dict]:
    storage = get_storage()
    return await run_in_threadpool(build_catalog, storage, day, kind)


@router.get("/catalog/{name}")
async def get_record(name: str) -> dict:
    name = safe_name(name)
    storage = get_storage()
    record = await run_in_threadpool(load_record, storage, name)
    if record is None:
        raise HTTPException(status_code=404, detail=f"No such file: {name}")
    return record


@router.get("/search")
async def search(q: str = Query(min_length=1, max_length=500)) -> list[dict]:
    storage = get_storage()
    return await run_in_threadpool(search_catalog, storage, q)

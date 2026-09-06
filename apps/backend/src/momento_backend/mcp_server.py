"""MCP endpoint: the momento CLI verbs for remote agents.

claude.ai, phones, and other MCP clients cannot run the local CLI, so
the same six verbs are exposed as MCP tools on the deployed backend.
The rule holds here too: no logic lives in this layer — every tool is a
thin wrapper over the catalog module. Auth is the same API key, applied
by an ASGI gate in main.py before requests reach this app.
"""

import datetime
import json
import tempfile

from fastapi import HTTPException
from mcp.server import MCPServer
from mcp.server.mcpserver.utilities.types import Image

from momento_backend import catalog, meta, transcribe
from momento_backend.media import get_storage, safe_name

INSTRUCTIONS = """\
Momento is a wearable camera; this server holds its photos, voice
memos, and video clips. Protocol: search_moments first. On a miss,
list_moments near the relevant day and inspect the candidates —
view_photo and view_clip_frames show you the pixels, get_transcript
gives you the words. Then ALWAYS save_note what you learned, even when
the file was not the answer: notes make the next search instant.
Note style: first sentence says who and what; then names, objects,
topics, rough duration, setting.
"""

mcp = MCPServer("momento", instructions=INSTRUCTIONS)


def _valid(name: str) -> str:
    try:
        return safe_name(name)
    except HTTPException as e:
        raise ValueError(e.detail) from None


def _iso(mtime: int | None) -> str | None:
    if not mtime:
        return None
    return datetime.datetime.fromtimestamp(mtime, tz=datetime.UTC).isoformat(
        timespec="minutes"
    )


def _with_captured(record: dict) -> dict:
    record["captured"] = _iso(record.get("mtime"))
    return record


@mcp.tool()
def list_moments(day: str | None = None, kind: str | None = None) -> str:
    """Lists every file with capture time, kind, note, and enrichment
    flags. day filters to one UTC date (YYYY-MM-DD); kind is photo,
    audio, or clip. Files without a note are uninspected."""
    storage = get_storage()
    records = [_with_captured(r) for r in catalog._build_catalog(storage)]
    if kind:
        records = [r for r in records if r["kind"] == kind]
    if day:
        records = [r for r in records if (r["captured"] or "").startswith(day)]
    return json.dumps(records, indent=1)


@mcp.tool()
def search_moments(query: str) -> str:
    """Finds files whose name, note, or transcript matches every word
    in the query (case-insensitive). Returns matches with snippets."""
    storage = get_storage()
    hits = [_with_captured(h) for h in catalog._search(storage, query)]
    if not hits:
        return "no matches — list_moments near the right date and inspect files"
    return json.dumps(hits, indent=1)


@mcp.tool()
def get_moment(name: str) -> str:
    """One file in full: metadata, note, transcript text, and the frame
    index when frames were extracted."""
    name = _valid(name)
    storage = get_storage()
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    mtimes = meta.load_mtimes(storage)
    note = meta.read_json(storage, meta.note_key(name))
    transcript = transcribe.get_transcript(storage, name)
    frames = meta.read_json(storage, meta.frames_index_key(name))
    return json.dumps(
        {
            "name": name,
            "kind": catalog.kind_of(name),
            "captured": _iso(mtimes.get(name)),
            "note": note.get("note") if note else None,
            "transcript": transcript.get("text") if transcript else None,
            "frames": frames,
        },
        indent=1,
    )


@mcp.tool()
def save_note(name: str, note: str) -> str:
    """Saves what you learned about one file. Do this after every
    inspection — notes are the search index. First sentence: who and
    what. Then detail: names, objects, topics, duration, setting."""
    name = _valid(name)
    storage = get_storage()
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
    return f"note saved for {name}"


@mcp.tool()
def get_transcript(name: str) -> str:
    """The words spoken in a voice memo (.wav). For a clip's audio,
    pass the matching AUD_ file from list_moments (VID_012 pairs with
    AUD_012). Queues transcription when missing — retry in a minute."""
    name = _valid(name)
    if not name.lower().endswith(".wav"):
        raise ValueError("Transcripts exist for .wav files only")
    storage = get_storage()
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    record = transcribe.get_transcript(storage, name)
    if record is not None:
        return record["text"] or "(silence — the transcript is empty)"
    transcribe.queue_transcription(storage, name)
    return "processing — call get_transcript again in about a minute"


@mcp.tool()
def view_photo(name: str) -> Image:
    """Returns the photo itself so you can look at it."""
    name = _valid(name)
    if catalog.kind_of(name) != "photo":
        raise ValueError("view_photo takes .jpg files; clips use view_clip_frames")
    storage = get_storage()
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    with tempfile.SpooledTemporaryFile() as tmp:
        for chunk in storage.stream(name):
            tmp.write(chunk)
        tmp.seek(0)
        return Image(data=tmp.read(), format="jpeg")


@mcp.tool()
def view_clip_frames(name: str, every: float = 4.0, max_frames: int = 8) -> list:
    """Returns frames sampled from a clip (default one per 4 s, cap 8)
    so you can watch it. Frames arrive in time order; the first text
    line carries their timestamps."""
    name = _valid(name)
    if catalog.kind_of(name) != "clip":
        raise ValueError("view_clip_frames takes .avi files; photos use view_photo")
    storage = get_storage()
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")
    every = min(max(every, 0.2), 60.0)
    max_frames = min(max(max_frames, 1), 16)
    index = meta.read_json(storage, meta.frames_index_key(name))
    if index is None or index.get("every") != every or index.get("max") != max_frames:
        index = catalog._extract_frames(storage, name, every, max_frames)
    times = ", ".join(f"{f['t']}s" for f in index["frames"])
    result: list = [f"{name}: {index['duration_s']}s clip, frames at {times}"]
    for f in index["frames"]:
        data = storage.read_meta(meta.frame_key(name, f["i"]))
        if data:
            result.append(Image(data=data, format="jpeg"))
    return result

"""MCP endpoint: the momento CLI verbs for remote agents.

claude.ai, phones, and other MCP clients cannot run the local CLI, so
the same verbs are exposed as MCP tools on the deployed backend. The
rule holds here: no logic lives in this layer — every tool is a thin
wrapper over the catalog service functions. Auth is the same API key,
applied by the KeyGate in main.py before requests reach this app.
"""

import json

from fastapi import HTTPException
from mcp.server import MCPServer
from mcp.server.mcpserver.utilities.types import Image

from momento_backend import catalog, meta
from momento_backend.media import get_storage, safe_name
from momento_backend.storage import MediaStorage

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


def _require(storage: MediaStorage, name: str) -> None:
    if storage.size(name) is None:
        raise ValueError(f"No such file: {name}")


@mcp.tool()
def list_moments(day: str | None = None, kind: str | None = None) -> str:
    """Lists every file with capture time, kind, note, and enrichment
    flags, oldest first. day filters to one home-timezone date
    (YYYY-MM-DD); kind is photo, audio, or clip. Files without a note
    are uninspected."""
    return json.dumps(catalog.build_catalog(get_storage(), day, kind), indent=1)


@mcp.tool()
def search_moments(query: str) -> str:
    """Finds files whose name, note, or transcript matches every word
    in the query (case-insensitive). Returns matches with snippets."""
    hits = catalog.search_catalog(get_storage(), query)
    if not hits:
        return "no matches — list_moments near the right date and inspect files"
    return json.dumps(hits, indent=1)


@mcp.tool()
def get_moment(name: str) -> str:
    """One file in full: metadata, note, transcript text, and the frame
    index when frames were extracted."""
    name = _valid(name)
    record = catalog.load_record(get_storage(), name)
    if record is None:
        raise ValueError(f"No such file: {name}")
    return json.dumps(record, indent=1)


@mcp.tool()
def save_note(name: str, note: str) -> str:
    """Saves what you learned about one file. Do this after every
    inspection — notes are the search index. First sentence: who and
    what. Then detail: names, objects, topics, duration, setting."""
    name = _valid(name)
    catalog.write_note(get_storage(), name, note)
    return f"note saved for {name}"


@mcp.tool()
def get_transcript(name: str) -> str:
    """The words spoken in a voice memo (.wav) or a clip's paired audio
    (pass the .avi name; the pair resolves server-side). Queues
    transcription when missing — retry in a minute."""
    name = _valid(name)
    resolved, record = catalog.get_or_queue_transcript(get_storage(), name)
    if record is None:
        return "processing — call get_transcript again in about a minute"
    text = record["text"] or "(silence — the transcript is empty)"
    return text if resolved == name else f"[{resolved}] {text}"


@mcp.tool()
def view_photo(name: str) -> Image:
    """Returns the photo itself so you can look at it."""
    name = _valid(name)
    storage = get_storage()
    if catalog.kind_of(name) != "photo":
        raise ValueError("view_photo takes .jpg files; clips use view_clip_frames")
    _require(storage, name)
    return Image(data=b"".join(storage.stream(name)), format="jpeg")


@mcp.tool()
def view_clip_frames(name: str, every: float = 4.0, max_frames: int = 8) -> list:
    """Returns frames sampled from a clip (default one per 4 s, cap 8)
    so you can watch it. Frames arrive in time order; the first text
    line carries their timestamps."""
    name = _valid(name)
    storage = get_storage()
    if catalog.kind_of(name) != "clip":
        raise ValueError("view_clip_frames takes .avi files; photos use view_photo")
    index = catalog.get_or_extract_frames(storage, name, every, max_frames)
    times = ", ".join(f"{f['t']}s" for f in index["frames"])
    result: list = [f"{name}: {index['duration_s']}s clip, frames at {times}"]
    for f in index["frames"]:
        data = storage.read_meta(meta.frame_key(name, f["i"]))
        if data:
            result.append(Image(data=data, format="jpeg"))
    return result

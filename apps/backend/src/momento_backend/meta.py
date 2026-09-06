"""Small JSON objects stored next to the media under _meta/.

Capture times live in one map (_meta/mtimes.json). Notes and transcripts
live one object per media file, so concurrent writers never race on a
shared document.
"""

import json
import threading

from momento_backend.storage import MediaStorage

MTIMES_KEY = "_meta/mtimes.json"
NOTES_PREFIX = "_meta/notes/"
TRANSCRIPTS_PREFIX = "_meta/transcripts/"
FRAMES_PREFIX = "_meta/frames/"

# The app uploads sequentially, but two requests can still overlap; one
# process-wide lock keeps the read-modify-write on mtimes.json safe.
_mtimes_lock = threading.Lock()


def read_json(storage: MediaStorage, key: str) -> dict | None:
    raw = storage.read_meta(key)
    if raw is None:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return None


def write_json(storage: MediaStorage, key: str, value: dict) -> None:
    storage.write_meta(key, json.dumps(value, sort_keys=True).encode())


def load_mtimes(storage: MediaStorage) -> dict[str, int]:
    data = read_json(storage, MTIMES_KEY) or {}
    return {k: v for k, v in data.items() if isinstance(v, int)}


def record_mtime(storage: MediaStorage, name: str, mtime: int) -> None:
    with _mtimes_lock:
        data = load_mtimes(storage)
        data[name] = mtime
        write_json(storage, MTIMES_KEY, data)


def note_key(name: str) -> str:
    return f"{NOTES_PREFIX}{name}.json"


def transcript_key(name: str) -> str:
    return f"{TRANSCRIPTS_PREFIX}{name}.json"


def frames_index_key(name: str) -> str:
    return f"{FRAMES_PREFIX}{name}/index.json"


def frame_key(name: str, index: int) -> str:
    return f"{FRAMES_PREFIX}{name}/{index}.jpg"

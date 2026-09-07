"""Voice transcription with faster-whisper, cached under _meta/.

Transcription is the one enrichment an agent cannot do itself, and it is
free on our own CPU, so it runs eagerly: every uploaded .wav gets queued.
A single worker thread serializes jobs (the model is the memory hog).
Jobs are idempotent — if the machine stops mid-job, the transcript stays
missing and the next request queues it again.

MOMENTO_FAKE_TRANSCRIPT short-circuits the model for tests, so no test
ever downloads or loads real weights.
"""

import datetime
import logging
import os
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor

from momento_backend import meta
from momento_backend.storage import MediaStorage

log = logging.getLogger("momento.transcribe")

_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="transcribe")
_inflight: set[str] = set()
_inflight_lock = threading.Lock()

# Only the single executor thread touches the model, so no lock.
_model = None


def _model_name() -> str:
    return os.environ.get("MOMENTO_WHISPER_MODEL", "base.en")


def _get_model():
    global _model
    if _model is None:
        from faster_whisper import WhisperModel

        _model = WhisperModel(_model_name(), device="cpu", compute_type="int8")
    return _model


def _transcribe_file(wav_path: str) -> dict:
    fake = os.environ.get("MOMENTO_FAKE_TRANSCRIPT")
    if fake is not None:
        return {"text": fake, "language": "en", "duration_s": 0.0, "model": "fake"}
    model = _get_model()
    segments, info = model.transcribe(wav_path)
    text = " ".join(s.text.strip() for s in segments).strip()
    return {
        "text": text,
        "language": info.language,
        "duration_s": round(info.duration, 1),
        "model": _model_name(),
    }


def get_transcript(
    storage: MediaStorage, name: str, size: int | None = None
) -> dict | None:
    """Returns the cached transcript only when it matches the file's
    current bytes (by size) — a healed or replaced upload must never
    keep serving the old file's words. Pass size when the caller
    already knows it, to spare a storage round trip."""
    record = meta.read_json(storage, meta.transcript_key(name))
    if record is None:
        return None
    if size is None:
        size = storage.size(name)
    if record.get("size") != size:
        return None
    return record


def queue_transcription(storage: MediaStorage, name: str) -> bool:
    """Queues one job; returns False when the same file is already queued."""
    with _inflight_lock:
        if name in _inflight:
            return False
        _inflight.add(name)
    _executor.submit(_job, storage, name)
    return True


def _job(storage: MediaStorage, name: str) -> None:
    try:
        if get_transcript(storage, name) is not None:
            return
        size = 0
        with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
            for chunk in storage.stream(name):
                tmp.write(chunk)
                size += len(chunk)
            tmp.flush()
            result = _transcribe_file(tmp.name)
        record = {
            "name": name,
            "size": size,
            "created_at": datetime.datetime.now(datetime.UTC).isoformat(
                timespec="seconds"
            ),
            **result,
        }
        meta.write_json(storage, meta.transcript_key(name), record)
        log.info("transcribed %s (%s chars)", name, len(record["text"]))
    except Exception:
        log.exception("transcription failed for %s", name)
    finally:
        with _inflight_lock:
            _inflight.discard(name)

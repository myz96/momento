# Momento Backend (FastAPI)

Python 3.12 + FastAPI backend. Receives media backups from the companion
app and stores them on disk. Runs locally for now; a cloud host and object
storage come later behind the same routes.

## Commands

```bash
just backend-dev    # uv run uvicorn --app-dir src momento_backend.main:app --reload
just backend-test   # uv run pytest
just backend-lint   # uv run ruff check .
```

## API

| Route | Method | Purpose |
|-------|--------|---------|
| `/health` | GET | liveness check |
| `/media` | POST | multipart upload; optional `mtime` form field records the capture time |
| `/media` | GET | list stored files `[{"name","size","mtime"}]` |
| `/media/{name}` | GET | download one file (Range supported) |
| `/media/{name}/transcript` | GET | cached whisper transcript; 202 + queue when missing |
| `/media/{name}/frames` | GET | MJPEG frame index (`every`, `max` params); extracts on first call |
| `/media/{name}/frames/{i}` | GET | one extracted frame as JPEG |
| `/transcripts/backfill` | POST | queue transcripts for every memo missing one |
| `/catalog` | GET | every file joined with kind, mtime, note, enrichment flags |
| `/catalog/{name}` | GET/PUT/DELETE | one record / save a note / remove a note |
| `/search` | GET | `?q=` keyword search over names, notes, transcripts |

Only `.jpg`, `.jpeg`, `.wav`, `.avi` names are accepted. `safe_name`
rejects anything that could escape the storage directory.

## AI layer

The catalog routes exist for agents (see `.claude/skills/momento/` and
the `momento` CLI, `src/momento_backend/cli.py`). The split of labor:
the backend does mechanical extraction and stores knowledge; the
reading agent does the understanding.

- **Transcripts are eager** because they are free: every uploaded `.wav`
  queues a faster-whisper job (`transcribe.py`, base.en int8, one worker
  thread, model baked into the Docker image). Jobs are idempotent — a
  machine stop mid-job just leaves the transcript missing and the next
  request re-queues it.
- **Everything else is lazy**: frames extract on first request
  (`avi_frames.py` scans our own `00dc` chunks, no codecs); image
  understanding happens in the calling agent's own vision; the agent
  writes what it learned back as a note (`PUT /catalog/{name}`).
- Derived data lives under the `_meta/` prefix next to the media
  (`meta.py`); anything with a "/" in its key never appears in the
  media list.
- v2 (planned): mount an MCP endpoint on this same app so claude.ai and
  phones can connect. Keep logic out of the CLI so that stays a one-day
  job.

## Storage

Two backends behind one interface (`storage.py`), picked from the
environment (`.env` is loaded at startup; see `.env.example`):

- **DiskStorage** (default): files land in `MOMENTO_MEDIA_DIR`
  (default `data/media`, gitignored). Uploads stage to a unique `.part`
  name and rename into place, so an interrupted upload never leaves a
  truncated file under a final name.
- **R2Storage**: activates when all four `MOMENTO_R2_*` variables are
  set (account id, access key id, secret, bucket). S3-compatible via
  boto3; puts are atomic, so no staging is needed.

The app skips uploads only when the listed name AND size match, so
repeated backups are cheap and a truncated copy heals itself.

## Layout

- `src/momento_backend/main.py` — app factory and health routes
- `src/momento_backend/media.py` — media router and disk storage
- `tests/` — pytest with httpx ASGI transport

## Related

- Root monorepo: `../../CLAUDE.md`
- App upload flow: `apps/mobile/src-tauri/src/lib.rs` (`backup_to_cloud`)

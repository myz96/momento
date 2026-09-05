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
| `/media` | POST | multipart upload of one media file |
| `/media` | GET | list stored files `[{"name","size"}]` |
| `/media/{name}` | GET | download one file |

Only `.jpg`, `.jpeg`, `.wav`, `.avi` names are accepted. `safe_name`
rejects anything that could escape the storage directory.

## Storage

Files land in `MOMENTO_MEDIA_DIR` (default `data/media`, gitignored).
Uploads stage to a unique `.part` name and rename into place, so an
interrupted upload never leaves a truncated file under a final name.
The app skips uploads only when the listed name AND size match, so
repeated backups are cheap and a truncated copy heals itself.

## Layout

- `src/momento_backend/main.py` — app factory and health routes
- `src/momento_backend/media.py` — media router and disk storage
- `tests/` — pytest with httpx ASGI transport

## Related

- Root monorepo: `../../CLAUDE.md`
- App upload flow: `apps/mobile/src-tauri/src/lib.rs` (`backup_to_cloud`)

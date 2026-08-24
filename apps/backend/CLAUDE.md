# Momento Backend (Python)

Python FastAPI backend for cloud sync, user accounts, and media processing.

Planned media processing capabilities (in ideation):
- Audio transcription (Whisper or similar)
- Image tagging / scene classification
- Daily / weekly timeline summaries
- Pattern detection across captured moments
- AI-assisted question-answering over your lifelog

## Status

**Not yet scaffolded.** This directory is a placeholder. Follow the setup instructions below when ready to start development.

## Tech Stack

- **Language**: Python 3.12
- **Framework**: FastAPI
- **Package Manager**: uv
- **Test Runner**: pytest
- **Linter**: ruff
- **Type Checker**: mypy (optional)

## Development Commands

After scaffolding:

```bash
# Run development server
uv run uvicorn momento_backend.main:app --reload

# Run tests
uv run pytest

# Lint
uv run ruff check .
uv run ruff format .

# Type check (if using mypy)
uv run mypy src/
```

Or use the justfile from repo root:
```bash
just backend-dev
just backend-test
just backend-lint
```

## FastAPI Project Structure (Recommended)

```
src/momento_backend/
├── __init__.py
├── main.py              # App factory, route registration
├── config.py            # Settings via pydantic-settings
├── routers/
│   ├── __init__.py
│   ├── auth.py          # User auth endpoints
│   ├── devices.py       # Device registration/pairing
│   └── media.py         # Media sync endpoints
├── models/
│   ├── __init__.py
│   └── user.py          # Pydantic models / SQLAlchemy schemas
├── services/
│   ├── __init__.py
│   └── sync.py          # Business logic
└── db/
    ├── __init__.py
    └── session.py       # Database session management
```

## pyproject.toml Template

```toml
[project]
name = "momento-backend"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = [
    "fastapi",
    "uvicorn[standard]",
]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.ruff]
line-length = 88
target-version = "py312"

[tool.ruff.lint]
select = ["E", "F", "I", "UP", "B"]

[tool.pytest.ini_options]
asyncio_mode = "auto"
```

## Minimal FastAPI App

`src/momento_backend/main.py`:

```python
from fastapi import FastAPI

app = FastAPI(title="Momento Backend")

@app.get("/health")
async def health():
    return {"status": "ok"}
```

## Python Coding Conventions

- Use type hints on all function signatures
- Prefer Pydantic models for request/response validation
- Use async/await for all I/O operations
- Run `ruff format .` before commits
- Run `ruff check .` and fix all warnings
- Tests: one test file per router/module, use `pytest-asyncio`

## Integration with Firmware

The backend will receive media via the mobile app, not directly from the device. Flow: device → app → backend.

When direct device upload is added later, it will likely use:
- **HTTP POST**: photo/audio upload after device connects to Wi-Fi
- **WebSocket**: real-time sync status updates

Protocol details will be defined in `packages/` (shared contracts).

## Integration with Mobile App

The backend will expose:
- REST API for user accounts, device management, media browsing
- WebSocket for real-time notifications

API contracts will be defined in `packages/` (OpenAPI spec).

## Environment Variables

Expected `.env` file:
```
DATABASE_URL=postgresql://...
SECRET_KEY=...
ALLOWED_ORIGINS=http://localhost:3000
```

Use `pydantic-settings` for config:
```python
from pydantic_settings import BaseSettings

class Settings(BaseSettings):
    database_url: str
    secret_key: str
    allowed_origins: list[str] = ["http://localhost:3000"]

    class Config:
        env_file = ".env"
```

## Related

- Root monorepo: `../../CLAUDE.md`
- FastAPI docs: https://fastapi.tiangolo.com/
- uv docs: https://docs.astral.sh/uv/

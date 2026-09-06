FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

# ctranslate2 (faster-whisper) links against OpenMP.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libgomp1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
ENV HF_HOME=/app/hf-cache
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev --no-install-project

# Bake the whisper model into the image: cold starts never download it,
# and a broken model fails the build instead of the first transcript.
RUN uv run --no-sync python -c \
    "from faster_whisper import WhisperModel; WhisperModel('base.en', device='cpu', compute_type='int8')"

COPY src ./src

EXPOSE 8000
CMD ["uv", "run", "--no-sync", "uvicorn", "--app-dir", "src", \
     "momento_backend.main:app", "--host", "0.0.0.0", "--port", "8000"]

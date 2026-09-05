FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

WORKDIR /app
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev --no-install-project

COPY src ./src

EXPOSE 8000
CMD ["uv", "run", "--no-sync", "uvicorn", "--app-dir", "src", \
     "momento_backend.main:app", "--host", "0.0.0.0", "--port", "8000"]

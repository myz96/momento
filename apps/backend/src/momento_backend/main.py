from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

load_dotenv()

from momento_backend.media import router as media_router

app = FastAPI(title="Momento Backend")
# The app's webview fetches media cross-origin (tauri://localhost).
# Auth is the API key, not cookies, so a wildcard origin is safe.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "POST", "DELETE"],
    allow_headers=["Authorization", "Range"],
)
app.include_router(media_router)


@app.get("/")
async def root() -> dict[str, str]:
    return {"message": "hello world"}


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}

from dotenv import load_dotenv
from fastapi import FastAPI

load_dotenv()

from momento_backend.media import router as media_router

app = FastAPI(title="Momento Backend")
app.include_router(media_router)


@app.get("/")
async def root() -> dict[str, str]:
    return {"message": "hello world"}


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}

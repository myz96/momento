from contextlib import asynccontextmanager

from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from mcp.server.transport_security import TransportSecuritySettings

load_dotenv()

from momento_backend.auth import KeyGate
from momento_backend.catalog import router as catalog_router
from momento_backend.mcp_server import mcp
from momento_backend.media import router as media_router

# Stateless + JSON responses: every request stands alone, which suits
# Fly auto-stop machines (no session survives a machine stop anyway).
mcp_app = mcp.streamable_http_app(
    streamable_http_path="/mcp",
    json_response=True,
    stateless_http=True,
    transport_security=TransportSecuritySettings(
        enable_dns_rebinding_protection=True,
        allowed_hosts=["momento-backend.fly.dev", "localhost:*", "127.0.0.1:*", "test"],
    ),
)


@asynccontextmanager
async def lifespan(_: FastAPI):
    # The MCP session manager must run for the app's lifetime or /mcp
    # returns 500s.
    async with mcp.session_manager.run():
        yield


api = FastAPI(title="Momento Backend", lifespan=lifespan)
# The app's webview fetches media cross-origin (tauri://localhost).
# Auth is the API key, not cookies, so a wildcard origin is safe.
api.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "POST", "PUT", "DELETE"],
    allow_headers=["Authorization", "Range", "Content-Type"],
)
# The catalog router registers /media/{name}/transcript and /frames;
# they never collide with /media/{name} because a path parameter does
# not match across "/".
api.include_router(catalog_router)
api.include_router(media_router)


@api.get("/")
async def root() -> dict[str, str]:
    return {"message": "hello world"}


@api.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}


class _Dispatch:
    """Serves /mcp from the MCP ASGI app directly — no mount, so no 307
    redirect that some MCP clients refuse to follow. Everything else,
    including the lifespan, goes to the FastAPI app."""

    def __init__(self, main_asgi, mcp_asgi):
        self.main = main_asgi
        self.mcp = mcp_asgi

    async def __call__(self, scope, receive, send) -> None:
        if scope["type"] == "http" and scope["path"].rstrip("/") == "/mcp":
            await self.mcp(scope, receive, send)
            return
        await self.main(scope, receive, send)


# Remote agents (claude.ai, phones) connect to /mcp; same key, ASGI-level.
app = _Dispatch(api, KeyGate(mcp_app))

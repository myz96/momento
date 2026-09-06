"""API-key check for the media routes.

With MOMENTO_API_KEY unset (local development), everything is open.
With it set (any deployed backend), a request must carry the key in the
Authorization header — or, for media downloads only, in a `key` query
parameter, because browser <img>/<audio> tags cannot send headers.
"""

import os
import secrets
import urllib.parse

from fastapi import HTTPException, Request


def _key_ok(auth_header: str, query_string: str) -> bool:
    expected = os.environ.get("MOMENTO_API_KEY")
    if not expected:
        return True
    supplied = ""
    if auth_header.lower().startswith("bearer "):
        supplied = auth_header[7:].strip()
    else:
        params = urllib.parse.parse_qs(query_string)
        if params.get("key"):
            supplied = params["key"][0]
    return secrets.compare_digest(supplied, expected)


def require_key(request: Request) -> None:
    if not _key_ok(
        request.headers.get("authorization", ""),
        request.url.query,
    ):
        raise HTTPException(status_code=401, detail="Missing or wrong API key")


class KeyGate:
    """The same API-key check as require_key, as an ASGI wrapper.

    Mounted sub-apps (the MCP endpoint) bypass FastAPI dependencies, so
    the gate runs at the ASGI layer instead."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send) -> None:
        if scope["type"] == "http":
            headers = {k: v for k, v in scope.get("headers") or []}
            auth = headers.get(b"authorization", b"").decode("latin-1")
            query = scope.get("query_string", b"").decode("latin-1")
            if not _key_ok(auth, query):
                from fastapi.responses import JSONResponse

                response = JSONResponse(
                    {"detail": "Missing or wrong API key"}, status_code=401
                )
                await response(scope, receive, send)
                return
        await self.app(scope, receive, send)

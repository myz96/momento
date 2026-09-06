"""API-key check for the whole app.

With MOMENTO_API_KEY unset (local development), everything is open.
With it set, every request must carry the key — in the Authorization
header, or in a `key` query parameter (browser <img>/<audio> tags and
MCP connector URLs cannot send headers). One ASGI gate wraps the whole
composed app, so a new route is closed by default; only "/" and
"/health" stay open, and OPTIONS passes for CORS preflights.
"""

import os
import secrets
import urllib.parse

from fastapi.responses import JSONResponse

OPEN_PATHS = {"/", "/health"}


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
    try:
        return secrets.compare_digest(supplied, expected)
    except TypeError:
        # compare_digest rejects non-ASCII str input; a garbage key is
        # a wrong key, not a server error.
        return False


class KeyGate:
    """The API-key check as an ASGI wrapper around the composed app."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send) -> None:
        if (
            scope["type"] == "http"
            and scope["path"] not in OPEN_PATHS
            and scope["method"] != "OPTIONS"
        ):
            headers = dict(scope.get("headers") or [])
            auth = headers.get(b"authorization", b"").decode("latin-1")
            query = scope.get("query_string", b"").decode("latin-1")
            if not _key_ok(auth, query):
                response = JSONResponse(
                    {"detail": "Missing or wrong API key"}, status_code=401
                )
                await response(scope, receive, send)
                return
        await self.app(scope, receive, send)

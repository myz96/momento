"""API-key check for the media routes.

With MOMENTO_API_KEY unset (local development), everything is open.
With it set (any deployed backend), a request must carry the key in the
Authorization header — or, for media downloads only, in a `key` query
parameter, because browser <img>/<audio> tags cannot send headers.
"""

import os
import secrets

from fastapi import HTTPException, Request


def require_key(request: Request) -> None:
    expected = os.environ.get("MOMENTO_API_KEY")
    if not expected:
        return

    supplied = ""
    auth = request.headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        supplied = auth[7:].strip()
    elif "key" in request.query_params:
        supplied = request.query_params["key"]

    if not secrets.compare_digest(supplied, expected):
        raise HTTPException(status_code=401, detail="Missing or wrong API key")

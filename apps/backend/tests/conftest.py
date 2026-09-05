import pytest

R2_VARS = [
    "MOMENTO_R2_ACCOUNT_ID",
    "MOMENTO_R2_ACCESS_KEY_ID",
    "MOMENTO_R2_SECRET_ACCESS_KEY",
    "MOMENTO_R2_BUCKET",
    "MOMENTO_API_KEY",
]


@pytest.fixture(autouse=True)
def _isolate_from_real_r2(monkeypatch):
    """Tests must never touch the real bucket, even with a filled .env
    (main.py loads it at import time)."""
    for var in R2_VARS:
        monkeypatch.delenv(var, raising=False)

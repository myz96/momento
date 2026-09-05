from momento_backend.storage import DiskStorage, R2Storage, storage_from_env

SUFFIXES = {".jpg", ".wav"}

R2_VARS = {
    "MOMENTO_R2_ACCOUNT_ID": "acct123",
    "MOMENTO_R2_ACCESS_KEY_ID": "key",
    "MOMENTO_R2_SECRET_ACCESS_KEY": "secret",
    "MOMENTO_R2_BUCKET": "momento-media",
}


def test_disk_storage_is_the_default(monkeypatch, tmp_path):
    for var in R2_VARS:
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(tmp_path))
    storage = storage_from_env(SUFFIXES)
    assert isinstance(storage, DiskStorage)
    assert storage.root == tmp_path


def test_r2_storage_when_fully_configured(monkeypatch):
    for var, value in R2_VARS.items():
        monkeypatch.setenv(var, value)
    storage = storage_from_env(SUFFIXES)
    assert isinstance(storage, R2Storage)
    assert storage.bucket == "momento-media"
    assert "acct123.r2.cloudflarestorage.com" in storage.client.meta.endpoint_url


def test_partial_r2_config_falls_back_to_disk(monkeypatch, tmp_path):
    for var, value in R2_VARS.items():
        monkeypatch.setenv(var, value)
    monkeypatch.delenv("MOMENTO_R2_SECRET_ACCESS_KEY")
    monkeypatch.setenv("MOMENTO_MEDIA_DIR", str(tmp_path))
    assert isinstance(storage_from_env(SUFFIXES), DiskStorage)

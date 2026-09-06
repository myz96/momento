"""Media storage backends behind one interface.

DiskStorage is the default (local development). R2Storage activates when
all MOMENTO_R2_* variables are set — same routes, same JSON, no app
changes. R2 puts are atomic, so only the disk backend needs .part
staging.
"""

import os
import uuid
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Protocol

CHUNK_BYTES = 1024 * 1024

# Derived data (transcripts, notes, extracted frames) lives under this
# prefix, next to the media it describes. The media routes never list or
# serve it: keys with "/" are filtered from list() and rejected by
# safe_name.
META_PREFIX = "_meta/"


def _check_meta_key(key: str) -> str:
    if not key.startswith(META_PREFIX) or ".." in key:
        raise ValueError(f"Not a meta key: {key!r}")
    return key


@dataclass
class StoredFile:
    name: str
    size: int
    mtime: int | None = None


class MediaStorage(Protocol):
    def save(self, name: str, src: BinaryIO) -> int: ...

    def list(self) -> list[StoredFile]: ...

    def size(self, name: str) -> int | None: ...

    def stream(self, name: str, start: int = 0, end: int | None = None) -> Iterator[bytes]: ...

    def read_meta(self, key: str) -> bytes | None: ...

    def write_meta(self, key: str, data: bytes) -> None: ...

    def delete_meta(self, key: str) -> bool: ...

    def list_meta(self, prefix: str) -> "list[str]": ...


class DiskStorage:
    def __init__(self, root: Path, allowed_suffixes: set[str]):
        self.root = root
        self.allowed_suffixes = allowed_suffixes

    def _dir(self) -> Path:
        self.root.mkdir(parents=True, exist_ok=True)
        return self.root

    def save(self, name: str, src: BinaryIO) -> int:
        target = self._dir() / name
        # Stage then rename: an interrupted upload never leaves a
        # truncated file under the final name.
        stage = target.with_name(f"{target.name}.{uuid.uuid4().hex}.part")
        size = 0
        try:
            with stage.open("wb") as out:
                while chunk := src.read(CHUNK_BYTES):
                    out.write(chunk)
                    size += len(chunk)
        except Exception:
            stage.unlink(missing_ok=True)
            raise
        stage.replace(target)
        return size

    def list(self) -> list[StoredFile]:
        entries = []
        for p in self._dir().iterdir():
            if not p.is_file() or p.suffix.lower() not in self.allowed_suffixes:
                continue
            st = p.stat()
            entries.append(
                StoredFile(name=p.name, size=st.st_size, mtime=int(st.st_mtime))
            )
        entries.sort(key=lambda e: e.name)
        return entries

    def size(self, name: str) -> int | None:
        target = self._dir() / name
        return target.stat().st_size if target.is_file() else None

    def stream(
        self, name: str, start: int = 0, end: int | None = None
    ) -> Iterator[bytes]:
        with (self._dir() / name).open("rb") as f:
            f.seek(start)
            remaining = None if end is None else end - start + 1
            while True:
                want = CHUNK_BYTES if remaining is None else min(CHUNK_BYTES, remaining)
                if want <= 0:
                    return
                chunk = f.read(want)
                if not chunk:
                    return
                if remaining is not None:
                    remaining -= len(chunk)
                yield chunk

    def read_meta(self, key: str) -> bytes | None:
        path = self._dir() / _check_meta_key(key)
        try:
            return path.read_bytes()
        except FileNotFoundError:
            return None

    def write_meta(self, key: str, data: bytes) -> None:
        path = self._dir() / _check_meta_key(key)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Same stage-then-rename pattern as save(): a crash never leaves a
        # truncated meta object under the final name.
        stage = path.with_name(f"{path.name}.{uuid.uuid4().hex}.part")
        try:
            stage.write_bytes(data)
            stage.replace(path)
        except Exception:
            stage.unlink(missing_ok=True)
            raise

    def delete_meta(self, key: str) -> bool:
        path = self._dir() / _check_meta_key(key)
        try:
            path.unlink()
            return True
        except FileNotFoundError:
            return False

    def list_meta(self, prefix: str) -> "list[str]":
        base = self._dir() / _check_meta_key(prefix)
        if not base.is_dir():
            return []
        keys = [
            str(p.relative_to(self._dir()).as_posix())
            for p in base.rglob("*")
            if p.is_file() and not p.name.endswith(".part")
        ]
        keys.sort()
        return keys


def _is_missing(error) -> bool:
    """True for a not-found ClientError; config and permission failures
    must surface, not read as an absent file."""
    code = error.response.get("Error", {}).get("Code", "")
    return code in ("404", "NoSuchKey", "NotFound")


class R2Storage:
    def __init__(
        self,
        account_id: str,
        access_key_id: str,
        secret_access_key: str,
        bucket: str,
        allowed_suffixes: set[str],
    ):
        import boto3

        self.bucket = bucket
        self.allowed_suffixes = allowed_suffixes
        self.client = boto3.client(
            "s3",
            endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
            aws_access_key_id=access_key_id,
            aws_secret_access_key=secret_access_key,
            region_name="auto",
        )

    def _objects(self, **kwargs):
        paginator = self.client.get_paginator("list_objects_v2")
        for page in paginator.paginate(Bucket=self.bucket, **kwargs):
            yield from page.get("Contents", [])

    def save(self, name: str, src: BinaryIO) -> int:
        self.client.upload_fileobj(src, self.bucket, name)
        head = self.client.head_object(Bucket=self.bucket, Key=name)
        return head["ContentLength"]

    def list(self) -> list[StoredFile]:
        entries: list[StoredFile] = []
        for obj in self._objects():
            # Media files sit at the bucket root; anything with a "/"
            # is derived data under _meta/ and never a media file.
            if "/" in obj["Key"]:
                continue
            if Path(obj["Key"]).suffix.lower() not in self.allowed_suffixes:
                continue
            entries.append(
                StoredFile(
                    name=obj["Key"],
                    size=obj["Size"],
                    mtime=int(obj["LastModified"].timestamp()),
                )
            )
        entries.sort(key=lambda e: e.name)
        return entries

    def size(self, name: str) -> int | None:
        try:
            head = self.client.head_object(Bucket=self.bucket, Key=name)
            return head["ContentLength"]
        except self.client.exceptions.ClientError as e:
            if _is_missing(e):
                return None
            raise

    def stream(
        self, name: str, start: int = 0, end: int | None = None
    ) -> Iterator[bytes]:
        kwargs = {"Bucket": self.bucket, "Key": name}
        if start or end is not None:
            kwargs["Range"] = f"bytes={start}-{'' if end is None else end}"
        body = self.client.get_object(**kwargs)["Body"]
        while chunk := body.read(CHUNK_BYTES):
            yield chunk

    def read_meta(self, key: str) -> bytes | None:
        try:
            body = self.client.get_object(
                Bucket=self.bucket, Key=_check_meta_key(key)
            )["Body"]
            return body.read()
        except self.client.exceptions.ClientError as e:
            if _is_missing(e):
                return None
            raise

    def write_meta(self, key: str, data: bytes) -> None:
        self.client.put_object(
            Bucket=self.bucket, Key=_check_meta_key(key), Body=data
        )

    def delete_meta(self, key: str) -> bool:
        key = _check_meta_key(key)
        try:
            self.client.head_object(Bucket=self.bucket, Key=key)
        except self.client.exceptions.ClientError as e:
            if _is_missing(e):
                return False
            raise
        self.client.delete_object(Bucket=self.bucket, Key=key)
        return True

    def list_meta(self, prefix: str) -> "list[str]":
        return sorted(
            obj["Key"] for obj in self._objects(Prefix=_check_meta_key(prefix))
        )


def storage_from_env(allowed_suffixes: set[str]) -> MediaStorage:
    account_id = os.environ.get("MOMENTO_R2_ACCOUNT_ID")
    access_key = os.environ.get("MOMENTO_R2_ACCESS_KEY_ID")
    secret = os.environ.get("MOMENTO_R2_SECRET_ACCESS_KEY")
    bucket = os.environ.get("MOMENTO_R2_BUCKET")
    if account_id and access_key and secret and bucket:
        return R2Storage(account_id, access_key, secret, bucket, allowed_suffixes)
    root = Path(os.environ.get("MOMENTO_MEDIA_DIR", "data/media"))
    return DiskStorage(root, allowed_suffixes)

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


@dataclass
class StoredFile:
    name: str
    size: int


class MediaStorage(Protocol):
    def save(self, name: str, src: BinaryIO) -> int: ...

    def list(self) -> list[StoredFile]: ...

    def size(self, name: str) -> int | None: ...

    def stream(self, name: str, start: int = 0, end: int | None = None) -> Iterator[bytes]: ...


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
        entries = [
            StoredFile(name=p.name, size=p.stat().st_size)
            for p in self._dir().iterdir()
            if p.is_file() and p.suffix.lower() in self.allowed_suffixes
        ]
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


class R2Storage:
    def __init__(
        self,
        account_id: str,
        access_key_id: str,
        secret_access_key: str,
        bucket: str,
    ):
        import boto3

        self.bucket = bucket
        self.client = boto3.client(
            "s3",
            endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
            aws_access_key_id=access_key_id,
            aws_secret_access_key=secret_access_key,
            region_name="auto",
        )

    def save(self, name: str, src: BinaryIO) -> int:
        self.client.upload_fileobj(src, self.bucket, name)
        head = self.client.head_object(Bucket=self.bucket, Key=name)
        return head["ContentLength"]

    def list(self) -> list[StoredFile]:
        entries: list[StoredFile] = []
        paginator = self.client.get_paginator("list_objects_v2")
        for page in paginator.paginate(Bucket=self.bucket):
            for obj in page.get("Contents", []):
                entries.append(StoredFile(name=obj["Key"], size=obj["Size"]))
        entries.sort(key=lambda e: e.name)
        return entries

    def size(self, name: str) -> int | None:
        try:
            head = self.client.head_object(Bucket=self.bucket, Key=name)
            return head["ContentLength"]
        except self.client.exceptions.ClientError:
            return None

    def stream(
        self, name: str, start: int = 0, end: int | None = None
    ) -> Iterator[bytes]:
        kwargs = {"Bucket": self.bucket, "Key": name}
        if start or end is not None:
            kwargs["Range"] = f"bytes={start}-{'' if end is None else end}"
        body = self.client.get_object(**kwargs)["Body"]
        while chunk := body.read(CHUNK_BYTES):
            yield chunk


def storage_from_env(allowed_suffixes: set[str]) -> MediaStorage:
    account_id = os.environ.get("MOMENTO_R2_ACCOUNT_ID")
    access_key = os.environ.get("MOMENTO_R2_ACCESS_KEY_ID")
    secret = os.environ.get("MOMENTO_R2_SECRET_ACCESS_KEY")
    bucket = os.environ.get("MOMENTO_R2_BUCKET")
    if account_id and access_key and secret and bucket:
        return R2Storage(account_id, access_key, secret, bucket)
    root = Path(os.environ.get("MOMENTO_MEDIA_DIR", "data/media"))
    return DiskStorage(root, allowed_suffixes)

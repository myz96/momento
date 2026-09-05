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

    def exists(self, name: str) -> bool: ...

    def stream(self, name: str) -> Iterator[bytes]: ...


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

    def exists(self, name: str) -> bool:
        return (self._dir() / name).is_file()

    def stream(self, name: str) -> Iterator[bytes]:
        with (self._dir() / name).open("rb") as f:
            while chunk := f.read(CHUNK_BYTES):
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

    def exists(self, name: str) -> bool:
        try:
            self.client.head_object(Bucket=self.bucket, Key=name)
            return True
        except self.client.exceptions.ClientError:
            return False

    def stream(self, name: str) -> Iterator[bytes]:
        body = self.client.get_object(Bucket=self.bucket, Key=name)["Body"]
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

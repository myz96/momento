"""momento — an agent-friendly CLI for the Momento media library.

A thin client of the deployed backend: it formats and forwards, the
backend decides. Media downloads land in ~/.cache/momento and every
command prints absolute paths, so an agent can Read what it fetched.
The usage protocol lives in .claude/skills/momento/SKILL.md.
"""

import argparse
import datetime
import json
import os
import sys
import time
from pathlib import Path

import httpx
from dotenv import load_dotenv

DEFAULT_BACKEND = "https://momento-backend.fly.dev"
CACHE_DIR = Path.home() / ".cache" / "momento"
TRANSCRIPT_WAIT_S = 600
POLL_S = 2.0


def _load_config() -> tuple[str, str]:
    # The backend's own .env carries the key on this machine; a local
    # .env (cwd) wins when present.
    load_dotenv()
    load_dotenv(Path(__file__).parents[2] / ".env")
    backend = os.environ.get("MOMENTO_BACKEND_URL", DEFAULT_BACKEND).rstrip("/")
    key = os.environ.get("MOMENTO_API_KEY", "")
    if not key:
        sys.exit("momento: MOMENTO_API_KEY is not set (env or apps/backend/.env)")
    return backend, key


def _client() -> httpx.Client:
    backend, key = _load_config()
    return httpx.Client(
        base_url=backend,
        headers={"Authorization": f"Bearer {key}"},
        timeout=httpx.Timeout(300.0, connect=20.0),
        follow_redirects=True,
    )


def _fail(response: httpx.Response) -> None:
    try:
        detail = response.json().get("detail", response.text)
    except ValueError:
        detail = response.text
    sys.exit(f"momento: {response.status_code} {detail}")


def _fmt_time(mtime: int | None) -> str:
    if not mtime:
        return "unknown-time      "
    local = datetime.datetime.fromtimestamp(mtime, tz=datetime.UTC).astimezone()
    return local.strftime("%Y-%m-%d %H:%M ")


def _fmt_size(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f}MB"
    return f"{size / 1024:.0f}KB"


def _day_of(mtime: int | None) -> str | None:
    if not mtime:
        return None
    local = datetime.datetime.fromtimestamp(mtime, tz=datetime.UTC).astimezone()
    return local.strftime("%Y-%m-%d")


def _audio_name(client: httpx.Client, name: str) -> str:
    """Maps a clip to its paired audio file (…VID_012.AVI -> …AUD_012.WAV).

    Synced names carry an epoch-ms prefix, so the pair is found by its
    index in the live file list, not by string surgery on the name.
    """
    stem = Path(name).stem.upper()
    if Path(name).suffix.lower() != ".avi" or "VID_" not in stem:
        return name
    index = stem.split("VID_", 1)[1]
    response = client.get("/media")
    if response.status_code != 200:
        return name
    wanted = f"AUD_{index}.WAV"
    for entry in response.json():
        if entry["name"].upper().endswith(wanted):
            return entry["name"]
    return name


def cmd_ls(args: argparse.Namespace) -> None:
    with _client() as client:
        response = client.get("/catalog")
        if response.status_code != 200:
            _fail(response)
        records = response.json()
    if args.kind:
        records = [r for r in records if r["kind"] == args.kind]
    if args.day:
        records = [r for r in records if _day_of(r["mtime"]) == args.day]
    records.sort(key=lambda r: (r["mtime"] or 0, r["name"]))
    if args.json:
        print(json.dumps(records, indent=2))
        return
    if not records:
        print("no files match")
        return
    for r in records:
        flags = []
        if r["note"]:
            flags.append("note")
        if r["has_transcript"]:
            flags.append("transcript")
        flag_s = f" [{','.join(flags)}]" if flags else ""
        print(
            f"{_fmt_time(r['mtime'])}{r['name']:<18} {r['kind']:<6}"
            f" {_fmt_size(r['size']):>8}{flag_s}"
        )


def _download(client: httpx.Client, url: str, target: Path) -> Path:
    target.parent.mkdir(parents=True, exist_ok=True)
    with client.stream("GET", url) as response:
        if response.status_code != 200:
            response.read()
            _fail(response)
        with target.open("wb") as out:
            for chunk in response.iter_bytes():
                out.write(chunk)
    return target


def cmd_get(args: argparse.Namespace) -> None:
    with _client() as client:
        path = _download(client, f"/media/{args.name}", CACHE_DIR / args.name)
    print(path)


def cmd_frames(args: argparse.Namespace) -> None:
    with _client() as client:
        response = client.get(
            f"/media/{args.name}/frames",
            params={"every": args.every, "max": args.max},
        )
        if response.status_code != 200:
            _fail(response)
        index = response.json()
        print(
            f"# {args.name}: {index['duration_s']}s, {index['total_frames']} frames,"
            f" showing {len(index['frames'])}",
            file=sys.stderr,
        )
        for f in index["frames"]:
            target = CACHE_DIR / "frames" / args.name / f"t{f['t']:07.2f}.jpg"
            _download(client, f"/media/{args.name}/frames/{f['i']}", target)
            print(target)


def cmd_transcript(args: argparse.Namespace) -> None:
    deadline = time.monotonic() + (0 if args.no_wait else TRANSCRIPT_WAIT_S)
    with _client() as client:
        name = _audio_name(client, args.name)
        if name != args.name:
            print(f"# clip audio lives in {name}", file=sys.stderr)
        while True:
            response = client.get(f"/media/{name}/transcript")
            if response.status_code == 200:
                record = response.json()
                print(record["text"] or "(silence — the transcript is empty)")
                return
            if response.status_code != 202:
                _fail(response)
            if time.monotonic() >= deadline:
                sys.exit(
                    "momento: transcript is still processing; retry in a minute"
                )
            print("# processing…", file=sys.stderr)
            time.sleep(POLL_S)


def cmd_note(args: argparse.Namespace) -> None:
    with _client() as client:
        if args.delete:
            response = client.delete(f"/catalog/{args.name}")
            if response.status_code != 200:
                _fail(response)
            print(f"note deleted for {args.name}")
            return
        text = " ".join(args.text).strip()
        if not text:
            sys.exit("momento: a note needs text (or pass --delete)")
        response = client.put(f"/catalog/{args.name}", json={"note": text})
        if response.status_code != 200:
            _fail(response)
        print(f"note saved for {args.name}")


def cmd_show(args: argparse.Namespace) -> None:
    with _client() as client:
        response = client.get(f"/catalog/{args.name}")
        if response.status_code != 200:
            _fail(response)
        record = response.json()
    if args.json:
        print(json.dumps(record, indent=2))
        return
    print(f"name:       {record['name']} ({record['kind']})")
    print(f"captured:   {_fmt_time(record['mtime']).strip() or 'unknown'}")
    print(f"size:       {_fmt_size(record['size'])}")
    print(f"note:       {record['note'] or '(none — add one after you look)'}")
    if record["transcript"] is not None:
        print(f"transcript: {record['transcript']}")
    if record["frames"]:
        print(f"frames:     {len(record['frames']['frames'])} extracted")


def cmd_search(args: argparse.Namespace) -> None:
    query = " ".join(args.query)
    with _client() as client:
        response = client.get("/search", params={"q": query})
        if response.status_code != 200:
            _fail(response)
        hits = response.json()
    if args.json:
        print(json.dumps(hits, indent=2))
        return
    if not hits:
        print("no matches — try `momento ls` and inspect files near the right date")
        return
    for hit in hits:
        print(f"{hit['name']} ({hit['kind']}, {_fmt_time(hit['mtime']).strip()})")
        for m in hit["matches"]:
            print(f"  {m['source']}: {m['snippet']}")


def cmd_backfill(_: argparse.Namespace) -> None:
    with _client() as client:
        response = client.post("/transcripts/backfill")
        if response.status_code != 200:
            _fail(response)
        counts = response.json()
    print(f"queued {counts['queued']}, already done {counts['done']}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="momento",
        description=(
            "Talk to the Momento media library. Files download to"
            f" {CACHE_DIR} and commands print absolute paths, so you can"
            " open or Read them directly. Protocol: search first; on a"
            " miss, ls near the right date, get/frames/transcript to"
            " inspect, then write a note so the next search is cheap."
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ls", help="list files with dates, kinds, and enrichment flags")
    p.add_argument("--day", help="filter to one local day, YYYY-MM-DD")
    p.add_argument("--kind", choices=["photo", "audio", "clip"])
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("get", help="download one file, print its local path")
    p.add_argument("name")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("frames", help="extract clip frames, print their local paths")
    p.add_argument("name")
    p.add_argument("--every", type=float, default=4.0, help="seconds between frames")
    p.add_argument("--max", type=int, default=8, help="frame count cap")
    p.set_defaults(func=cmd_frames)

    p = sub.add_parser(
        "transcript", help="print a memo transcript (accepts VID_* for clip audio)"
    )
    p.add_argument("name")
    p.add_argument("--no-wait", action="store_true", help="do not poll while processing")
    p.set_defaults(func=cmd_transcript)

    p = sub.add_parser("note", help="save what you learned about one file")
    p.add_argument("name")
    p.add_argument("text", nargs="*", help="the note text")
    p.add_argument("--delete", action="store_true", help="remove the note instead")
    p.set_defaults(func=cmd_note)

    p = sub.add_parser("show", help="one file: metadata, note, transcript, frames")
    p.add_argument("name")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("search", help="find files by note, transcript, or name")
    p.add_argument("query", nargs="+")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_search)

    p = sub.add_parser("backfill", help="queue transcripts for every memo missing one")
    p.set_defaults(func=cmd_backfill)

    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        args.func(args)
    except httpx.HTTPError as e:
        sys.exit(f"momento: backend not reachable: {e}")


if __name__ == "__main__":
    main()

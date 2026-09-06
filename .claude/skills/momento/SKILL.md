---
name: momento
description: Search and inspect Michael's Momento media library (photos, voice memos, video clips from the wearable camera). Use when asked about captured moments, days, memos, or "what did I do/say/see" questions.
---

# Momento media library

Momento is a wearable camera. Its photos, voice memos, and video clips
live in the cloud. The `momento` CLI gives you the files and their text
records. You bring the understanding: look at images with your own
vision, read transcripts, then write down what you learned.

Run every command from the repo root as:

```bash
uv run --project apps/backend momento <command>
```

## The protocol

1. **Search first.** `momento search <words>` covers names, notes, and
   memo transcripts. A hit usually answers the question.
2. **On a miss, go to the timeline.** Most questions anchor to a day.
   `momento ls` shows every file with its capture time; `--day
   YYYY-MM-DD` narrows it. Files without a `[note]` flag are
   uninspected — those are where unknown answers hide.
3. **Inspect the candidates.**
   - Photo: `momento get PHOTO_012.JPG` prints a local path. Read the
     image with your vision.
   - Voice memo: `momento transcript AUD_012.WAV` prints the text.
   - Clip: `momento frames VID_012.AVI` prints paths of frames sampled
     every 4 s (cap 8). Read them in order — they tell the story.
     `momento transcript VID_012.AVI` adds the clip's audio.
4. **Write a note for anything you inspected.** This is the important
   step: notes make the next search instant and free. Do it even when
   the file was not the answer.

   ```bash
   momento note VID_012.AVI "Zach and Michael assemble the charge board at the kitchen table. Zach explains the battery lead soldering. ~40s, indoors, evening."
   ```

   Note style: first sentence says who and what. Then useful detail:
   names, objects, topics, rough duration, setting. Plain text — the
   note is a search target, so include the words a future question
   would use.

## Command reference

| Command | Does |
|---|---|
| `momento ls [--day D] [--kind photo\|audio\|clip] [--json]` | timeline with capture times and `[note,transcript]` flags |
| `momento search <words>` | all words must match; sources: name, note, transcript |
| `momento get NAME` | download, print local path |
| `momento frames NAME [--every S] [--max N]` | sample clip frames, print local paths |
| `momento transcript NAME` | memo text; accepts `VID_*` and maps to the paired `AUD_*`; polls while processing |
| `momento note NAME <text...>` / `--delete` | save or remove your note |
| `momento show NAME [--json]` | one file: metadata, note, transcript, frames |
| `momento backfill` | queue transcripts for every memo missing one |

## Facts worth knowing

- Capture times are real when the device had internet (SNTP). A file
  with `unknown-time` fell back to its upload time.
- Clips pair with audio by index: `VID_012.AVI` ↔ `AUD_012.WAV`.
- Only backed-up files are visible here. Files still on the phone
  appear after the app's next cloud backup.
- The first request after idle wakes the Fly machine; allow a few
  seconds.
- Config: `MOMENTO_BACKEND_URL` + `MOMENTO_API_KEY`, auto-loaded from
  `apps/backend/.env`.

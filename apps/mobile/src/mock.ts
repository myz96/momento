/* Browser harness for design work: fakes the Tauri IPC layer so the
 * frontend runs in a plain browser with representative data. Loaded only
 * when VITE_MOCK is set (pnpm dev:mock); never part of a real build. */

type MockFile = {
  name: string;
  path: string;
  size: number;
  kind: string;
  modified_ms: number;
  location: "local" | "cloud";
  thumb: string | null;
};

const DAY = 24 * 60 * 60 * 1000;
const now = Date.now();

function photo(n: number, ms: number, cloud = true): MockFile {
  return {
    name: `PHOTO_00${n}.JPG`,
    path: cloud ? "" : `/mock/t${n}.jpg`,
    size: 60_000 + n * 3211,
    kind: "photo",
    modified_ms: ms,
    location: cloud ? "cloud" : "local",
    thumb: cloud ? `/mock/t${n}.jpg` : null,
  };
}

function audio(n: number, ms: number): MockFile {
  return {
    name: `AUD_00${n}.WAV`,
    path: "/mock/silence.wav",
    size: 150_000 + n * 7013,
    kind: "audio",
    modified_ms: ms,
    location: "local",
    thumb: null,
  };
}

function video(n: number, ms: number, thumbIdx: number): MockFile {
  return {
    name: `VID_00${n}.AVI`,
    path: "/mock/clip.avi",
    size: 900_000 + n * 55_555,
    kind: "video",
    modified_ms: ms,
    location: "local",
    thumb: `/mock/t${thumbIdx}.jpg`,
  };
}

const MEDIA: MockFile[] = [
  photo(1, now - 2 * 3600_000),
  audio(1, now - 3 * 3600_000),
  photo(2, now - 5 * 3600_000),
  video(1, now - 6 * 3600_000, 3),
  photo(4, now - DAY - 2 * 3600_000),
  photo(5, now - DAY - 3 * 3600_000),
  audio(2, now - DAY - 4 * 3600_000),
  video(2, now - DAY - 5 * 3600_000, 6),
  photo(7, now - 8 * DAY - 2 * 3600_000),
  audio(3, now - 8 * DAY - 3 * 3600_000),
  video(3, now - 8 * DAY - 5 * 3600_000, 8),
];

async function invoke(cmd: string, args?: Record<string, unknown>) {
  switch (cmd) {
    case "list_local_media":
      return MEDIA;
    case "device_info":
      return { device: "momento", files: 4, total_bytes: 2_400_000 };
    case "sync_device":
      return [];
    case "backup_to_cloud":
      return { uploaded: 0, already_backed_up: 8, freed: 0, freed_bytes: 0 };
    case "provision_wifi":
      return { state: "sta", ip: "172.20.10.9", ssid: String(args?.ssid ?? "") };
    case "open_media":
      return null;
    default:
      throw new Error(`mock: unknown command ${cmd}`);
  }
}

(window as any).__TAURI_INTERNALS__ = {
  invoke,
  transformCallback: (cb: (r: unknown) => void) => cb,
  convertFileSrc: (p: string) => p,
};

export {};

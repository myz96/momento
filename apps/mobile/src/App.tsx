import { useCallback, useEffect, useRef, useState } from "react";
import { Channel, convertFileSrc, invoke } from "@tauri-apps/api/core";
import "./App.css";

type DeviceInfo = { device: string; files: number; total_bytes: number };
type SyncProgress = { file: string; index: number; total: number };
type WifiStatus = { state: string; ip: string; ssid: string };
type BackupReport = {
  uploaded: number;
  already_backed_up: number;
  freed: number;
  freed_bytes: number;
};
type LocalFile = {
  name: string;
  path: string;
  size: number;
  kind: string;
  modified_ms: number;
  location: "local" | "cloud";
  thumb: string | null;
};

type LinkState = "unknown" | "ok" | "down" | "busy";

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

function formatTime(ms: number): string {
  if (!ms) return "";
  return new Date(ms).toLocaleTimeString(undefined, {
    hour: "numeric",
    minute: "2-digit",
  });
}

function dayLabel(ms: number): string {
  if (!ms) return "Undated";
  const d = new Date(ms);
  const today = new Date();
  const yesterday = new Date(today);
  yesterday.setDate(today.getDate() - 1);
  if (d.toDateString() === today.toDateString()) return "Today";
  if (d.toDateString() === yesterday.toDateString()) return "Yesterday";
  return d.toLocaleDateString(undefined, {
    weekday: "long",
    day: "numeric",
    month: "long",
    year: d.getFullYear() === today.getFullYear() ? undefined : "numeric",
  });
}

function groupByDay(files: LocalFile[]): [string, LocalFile[]][] {
  const groups: [string, LocalFile[]][] = [];
  for (const f of files) {
    const label = dayLabel(f.modified_ms);
    const last = groups[groups.length - 1];
    if (last && last[0] === label) {
      last[1].push(f);
    } else {
      groups.push([label, [f]]);
    }
  }
  return groups;
}

function usePersistedState(
  key: string,
  initial: string,
): [string, (v: string) => void] {
  const [value, setValue] = useState(() => localStorage.getItem(key) ?? initial);
  const set = (v: string) => {
    setValue(v);
    localStorage.setItem(key, v);
  };
  return [value, set];
}

function StatusPill({
  label,
  state,
  detail,
}: {
  label: string;
  state: LinkState;
  detail: string;
}) {
  const text = state === "busy" ? "working" : detail;
  return (
    <span
      className={`pill pill-${state}`}
      title={state === "unknown" ? `${label}: not checked yet` : `${label}: ${text}`}
    >
      <i className="pill-dot" />
      {label}
      {state !== "unknown" && <span className="pill-detail">{text}</span>}
    </span>
  );
}

function App() {
  const [base, setBase] = usePersistedState("deviceUrl", "http://momento.local");
  const [status, setStatus] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [media, setMedia] = useState<LocalFile[]>([]);
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [provStatus, setProvStatus] = useState("");
  const [provError, setProvError] = useState("");
  const [provBusy, setProvBusy] = useState(false);
  const [backendUrl, setBackendUrl] = usePersistedState(
    "backendUrl",
    "http://localhost:8000",
  );
  const [cloudStatus, setCloudStatus] = useState("");
  const [cloudError, setCloudError] = useState("");
  const [cloudBusy, setCloudBusy] = useState(false);
  const [freeSpaceStr, setFreeSpaceStr] = usePersistedState("freeSpace", "on");
  const [autoBackupStr, setAutoBackupStr] = usePersistedState("autoBackup", "on");
  const [cloudKey, setCloudKey] = usePersistedState("cloudKey", "");
  const [cloudReadyStr, setCloudReadyStr] = usePersistedState("cloudReady", "off");
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [viewer, setViewer] = useState<{ file: LocalFile; src: string } | null>(
    null,
  );
  const [deviceState, setDeviceState] = useState<LinkState>("unknown");
  const [cloudState, setCloudState] = useState<LinkState>("unknown");
  const freeSpace = freeSpaceStr === "on";
  const autoBackup = autoBackupStr === "on";
  // Auto-backup stays quiet until the cloud is actually configured: a
  // fresh install must not greet its user with a failure banner.
  const cloudReady =
    cloudReadyStr === "on" || backendUrl !== "http://localhost:8000";

  function closeSettings() {
    setSettingsOpen(false);
    setProvStatus("");
    setProvError("");
  }

  const refreshMedia = useCallback(async () => {
    try {
      setMedia(await invoke<LocalFile[]>("list_local_media"));
    } catch (e) {
      setError(String(e));
    }
  }, []);

  useEffect(() => {
    refreshMedia();
  }, [refreshMedia]);

  async function checkDevice() {
    setBusy(true);
    setError("");
    setDeviceState("busy");
    setStatus("Looking for the device…");
    try {
      const i = await invoke<DeviceInfo>("device_info", { base });
      setDeviceState("ok");
      setStatus(
        i.files === 0
          ? "Device connected. Its storage is empty."
          : `Device connected: ${i.files} new capture(s) waiting (${formatBytes(i.total_bytes)}).`,
      );
    } catch (e) {
      console.error(e);
      setDeviceState("down");
      setStatus("");
      setError(
        "Could not find the device. Hold its CAM button for 1.5 seconds " +
          "(the light blinks), and check the address in Settings.",
      );
    } finally {
      setBusy(false);
    }
  }

  async function syncNow() {
    setBusy(true);
    setError("");
    setDeviceState("busy");
    setStatus("Starting sync…");
    const onProgress = new Channel<SyncProgress>();
    onProgress.onmessage = (p) => {
      setStatus(`Bringing in ${p.index} of ${p.total}…`);
    };
    try {
      const synced = await invoke<LocalFile[]>("sync_device", {
        base,
        onProgress,
      });
      setDeviceState("ok");
      setStatus(
        synced.length === 0
          ? "Nothing new on the device."
          : `${synced.length} new capture(s) safely in your library. The device is clear.`,
      );
      await refreshMedia();
    } catch (e) {
      console.error(e);
      setDeviceState("down");
      const raw = String(e);
      setError(
        raw.includes("not reachable")
          ? "Could not find the device. Hold its CAM button for 1.5 seconds " +
              "(the light blinks) and try again — first time? Open Settings " +
              "and send it your Wi-Fi."
          : raw,
      );
      setStatus("");
      await refreshMedia();
    } finally {
      setBusy(false);
    }
  }

  async function provisionWifi() {
    setProvBusy(true);
    setProvError("");
    setProvStatus("Looking for the device over Bluetooth…");
    try {
      const result = await invoke<WifiStatus>("provision_wifi", {
        ssid: wifiSsid,
        password: wifiPass,
      });
      setProvStatus(
        `Done — the device joined ${result.ssid}. Its address (http://${result.ip}) is saved below; the Sync button now works over your network.`,
      );
      setBase(`http://${result.ip}`);
    } catch (e) {
      setProvStatus("");
      setProvError(String(e));
    } finally {
      setProvBusy(false);
    }
  }

  const cloudBusyRef = useRef(false);
  const autoFailedRef = useRef(false);

  const backupToCloud = useCallback(
    async (silent: boolean) => {
      if (cloudBusyRef.current) return;
      cloudBusyRef.current = true;
      setCloudBusy(true);
      setCloudError("");
      setCloudState("busy");
      if (!silent) setCloudStatus("Checking what the cloud already has…");
      const onProgress = new Channel<SyncProgress>();
      onProgress.onmessage = (p) => {
        if (!silent) {
          setCloudStatus(`Backing up ${p.index} of ${p.total}…`);
        }
      };
      try {
        const report = await invoke<BackupReport>("backup_to_cloud", {
          backend: backendUrl,
          freeSpace,
          apiKey: cloudKey,
          onProgress,
        });
        setCloudState("ok");
        setCloudReadyStr("on");
        const parts = [
          report.uploaded === 0
            ? "Everything is backed up."
            : `Backed up ${report.uploaded} capture(s).`,
        ];
        if (report.freed > 0) {
          parts.push(
            `Freed ${formatBytes(report.freed_bytes)} on this device.`,
          );
        }
        if (!silent || report.uploaded > 0 || report.freed > 0) {
          setCloudStatus(parts.join(" "));
        } else if (autoFailedRef.current) {
          setCloudStatus("Auto-backup is working again. Everything is backed up.");
        }
        autoFailedRef.current = false;
        await refreshMedia();
      } catch (e) {
        setCloudState("down");
        if (!silent) {
          setCloudStatus("");
          setCloudError(String(e));
        } else {
          autoFailedRef.current = true;
          setCloudStatus(
            "Cloud backup is paused — the cloud is unreachable. Retrying every 5 minutes.",
          );
        }
      } finally {
        cloudBusyRef.current = false;
        setCloudBusy(false);
      }
    },
    [backendUrl, freeSpace, cloudKey, refreshMedia, setCloudReadyStr],
  );

  const backupRef = useRef(backupToCloud);
  backupRef.current = backupToCloud;

  useEffect(() => {
    if (!autoBackup || !cloudReady) return;
    const kickoff = setTimeout(() => backupRef.current(true), 3000);
    const timer = setInterval(() => backupRef.current(true), 5 * 60 * 1000);
    return () => {
      clearTimeout(kickoff);
      clearInterval(timer);
    };
  }, [autoBackup, cloudReady]);

  // Success messages fade on their own; errors stay until the next action.
  useEffect(() => {
    if (!status) return;
    const t = setTimeout(() => setStatus(""), 8000);
    return () => clearTimeout(t);
  }, [status]);
  useEffect(() => {
    if (!cloudStatus || autoFailedRef.current) return;
    const t = setTimeout(() => setCloudStatus(""), 8000);
    return () => clearTimeout(t);
  }, [cloudStatus]);

  const notice = error || cloudError || status || cloudStatus;
  const noticeKind = error || cloudError ? "bad" : "ok";

  return (
    <div className="shell">
      <header className="bar">
        <span className="brand">Momento</span>
        <div className="pills">
          <StatusPill
            label="Device"
            state={deviceState}
            detail={deviceState === "ok" ? "connected" : "not found"}
          />
          <StatusPill
            label="Cloud"
            state={cloudState}
            detail={cloudState === "ok" ? "backed up" : "unreachable"}
          />
        </div>
        <div className="bar-actions">
          <button className="btn primary" onClick={syncNow} disabled={busy}>
            {busy ? "Syncing…" : "Sync"}
          </button>
          <button
            className="btn quiet"
            aria-label="Settings"
            title="Settings"
            onClick={() => setSettingsOpen(true)}
          >
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
              <circle cx="12" cy="12" r="3" />
              <path d="M19.4 15a1.7 1.7 0 0 0 .34 1.87l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-1.87-.34 1.7 1.7 0 0 0-1 1.55V21a2 2 0 1 1-4 0v-.09a1.7 1.7 0 0 0-1-1.55 1.7 1.7 0 0 0-1.87.34l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.7 1.7 0 0 0 .34-1.87 1.7 1.7 0 0 0-1.55-1H3a2 2 0 1 1 0-4h.09a1.7 1.7 0 0 0 1.55-1 1.7 1.7 0 0 0-.34-1.87l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.7 1.7 0 0 0 1.87.34h.01a1.7 1.7 0 0 0 1-1.55V3a2 2 0 1 1 4 0v.09a1.7 1.7 0 0 0 1 1.55 1.7 1.7 0 0 0 1.87-.34l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.7 1.7 0 0 0-.34 1.87v.01a1.7 1.7 0 0 0 1.55 1H21a2 2 0 1 1 0 4h-.09a1.7 1.7 0 0 0-1.55 1z" />
            </svg>
          </button>
        </div>
      </header>

      <div className={`notice notice-${noticeKind}`} role="status">
        {notice}
      </div>

      <main className="stage">
        {media.length === 0 ? (
          <div className="empty">
            <p className="empty-title">No moments yet</p>
            <p className="empty-body">
              Wear the device through your day — the front button takes a
              photo, the side button starts and stops a recording. Hold the
              front button for 1.5 seconds to start a sync, then press{" "}
              <strong>Sync</strong> here.
            </p>
            <p className="empty-body">
              First time? Send the device your Wi-Fi once, so syncing works
              over your own network.
            </p>
            <button className="btn" onClick={() => setSettingsOpen(true)}>
              Set up the device
            </button>
          </div>
        ) : (
          groupByDay(media).map(([label, files]) => {
            const visual = files.filter((m) => m.kind !== "audio");
            const voices = files.filter((m) => m.kind === "audio");
            return (
              <section className="day" key={label}>
                <h2 className="day-head">{label}</h2>
                {visual.length > 0 && (
                  <div className="tiles">
                    {visual.map((m) => {
                      const inCloud = m.location === "cloud";
                      const full = inCloud
                        ? `${backendUrl}/media/${m.name}${
                            cloudKey
                              ? `?key=${encodeURIComponent(cloudKey)}`
                              : ""
                          }`
                        : convertFileSrc(m.path);
                      const img =
                        m.kind === "video"
                          ? m.thumb
                            ? convertFileSrc(m.thumb)
                            : null // fresh clip: no preview until offload
                          : inCloud && m.thumb
                            ? convertFileSrc(m.thumb)
                            : full;
                      return (
                        <button
                          className="tile"
                          key={m.name}
                          onClick={() => setViewer({ file: m, src: full })}
                          title={`${m.kind === "video" ? "Clip" : "Photo"} · ${formatTime(m.modified_ms)} · ${formatBytes(m.size)}${inCloud ? " · in the cloud" : ""}`}
                        >
                          {img && <img src={img} alt="" loading="lazy" />}
                          {m.kind === "video" && (
                            <span className="tile-play" aria-hidden="true">
                              <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5.5v13l11-6.5z" /></svg>
                            </span>
                          )}
                          <span className="tile-meta">
                            {formatTime(m.modified_ms)}
                            {inCloud && <CloudMark />}
                          </span>
                        </button>
                      );
                    })}
                  </div>
                )}
                {voices.map((m) => {
                  const inCloud = m.location === "cloud";
                  const src = inCloud
                    ? `${backendUrl}/media/${m.name}${
                        cloudKey ? `?key=${encodeURIComponent(cloudKey)}` : ""
                      }`
                    : convertFileSrc(m.path);
                  return (
                    <VoiceRow
                      key={m.name}
                      src={src}
                      time={formatTime(m.modified_ms)}
                      size={formatBytes(m.size)}
                      inCloud={inCloud}
                    />
                  );
                })}
              </section>
            );
          })
        )}
      </main>

      {viewer && (
        <div className="viewer-scrim" onClick={() => setViewer(null)}>
          <div className="viewer" onClick={(e) => e.stopPropagation()}>
            <header className="viewer-head">
              <span className="viewer-title">
                {viewer.file.kind === "video" ? "Clip" : "Photo"} ·{" "}
                {dayLabel(viewer.file.modified_ms)},{" "}
                {formatTime(viewer.file.modified_ms)}
                {viewer.file.location === "cloud" && <CloudMark />}
              </span>
              <span className="viewer-actions">
                <button
                  className="btn quiet"
                  onClick={() =>
                    invoke("open_media", {
                      path:
                        viewer.file.location === "cloud"
                          ? viewer.src
                          : viewer.file.path,
                    })
                  }
                >
                  Save original
                </button>
                <button className="btn quiet" onClick={() => setViewer(null)}>
                  Close
                </button>
              </span>
            </header>
            {viewer.file.kind === "video" ? (
              <ClipPlayer src={viewer.src} />
            ) : (
              <img className="viewer-photo" src={viewer.src} alt="" />
            )}
          </div>
        </div>
      )}

      {settingsOpen && (
        <div className="sheet-scrim" onClick={closeSettings}>
          <div
            className="sheet"
            role="dialog"
            aria-label="Settings"
            onClick={(e) => e.stopPropagation()}
          >
            <header className="sheet-head">
              <h2>Settings</h2>
              <button className="btn quiet" onClick={closeSettings}>
                Done
              </button>
            </header>

            <section className="set-group">
              <h3>Device Wi-Fi, set up once</h3>
              <p className="set-hint">
                Hold the camera button on the device for 1.5 seconds — the
                light blinks — then send it your Wi-Fi over Bluetooth. Use a
                2.4 GHz network (for a phone hotspot, turn on Maximize
                Compatibility). From then on the device joins by itself.
              </p>
              <div className="set-row">
                <input
                  value={wifiSsid}
                  onChange={(e) => setWifiSsid(e.currentTarget.value)}
                  placeholder="Wi-Fi name"
                  disabled={provBusy}
                />
                <input
                  type="password"
                  value={wifiPass}
                  onChange={(e) => setWifiPass(e.currentTarget.value)}
                  placeholder="Wi-Fi password"
                  disabled={provBusy}
                />
                <button
                  className="btn"
                  onClick={provisionWifi}
                  disabled={provBusy || !wifiSsid.trim()}
                >
                  {provBusy ? "Sending…" : "Send over Bluetooth"}
                </button>
              </div>
              {provStatus && <p className="set-status">{provStatus}</p>}
              {provError && <p className="set-error">{provError}</p>}
            </section>

            <section className="set-group">
              <h3>Device address</h3>
              <p className="set-hint">
                Wi-Fi setup fills this in for you. On a home network{" "}
                <code>http://momento.local</code> also works. Without any
                setup, join the device's own <strong>Momento</strong> network
                (password <code>momento123</code>) and use{" "}
                <code>http://192.168.4.1</code>.
              </p>
              <div className="set-row">
                <input
                  value={base}
                  onChange={(e) => setBase(e.currentTarget.value)}
                  placeholder="http://momento.local"
                  disabled={busy}
                />
                <button className="btn" onClick={checkDevice} disabled={busy}>
                  Check
                </button>
              </div>
              {status && <p className="set-status">{status}</p>}
              {error && <p className="set-error">{error}</p>}
            </section>

            <section className="set-group">
              <h3>Cloud backup</h3>
              <div className="set-row">
                <input
                  value={backendUrl}
                  onChange={(e) => setBackendUrl(e.currentTarget.value)}
                  placeholder="https://momento-backend.fly.dev"
                  disabled={cloudBusy}
                />
                <input
                  type="password"
                  value={cloudKey}
                  onChange={(e) => setCloudKey(e.currentTarget.value)}
                  placeholder="API key"
                  disabled={cloudBusy}
                />
                <button
                  className="btn"
                  onClick={() => backupToCloud(false)}
                  disabled={cloudBusy}
                >
                  {cloudBusy ? "Backing up…" : "Back up now"}
                </button>
              </div>
              <label className="set-check">
                <input
                  type="checkbox"
                  checked={freeSpace}
                  onChange={(e) =>
                    setFreeSpaceStr(e.currentTarget.checked ? "on" : "off")
                  }
                  disabled={cloudBusy}
                />
                <span>
                  Free up space after backup — keep small previews here,
                  stream originals from the cloud
                </span>
              </label>
              <label className="set-check">
                <input
                  type="checkbox"
                  checked={autoBackup}
                  onChange={(e) =>
                    setAutoBackupStr(e.currentTarget.checked ? "on" : "off")
                  }
                />
                <span>Back up automatically whenever the cloud is reachable</span>
              </label>
              {cloudStatus && <p className="set-status">{cloudStatus}</p>}
              {cloudError && <p className="set-error">{cloudError}</p>}
            </section>
          </div>
        </div>
      )}
    </div>
  );
}

/* Plays a device clip inside the app. The clips are MJPEG AVI — a
 * stream of JPEG frames — so the player extracts the frames and
 * animates them; no external player and no video codec needed. */
function ClipPlayer({ src }: { src: string }) {
  const [frames, setFrames] = useState<string[]>([]);
  const [fps, setFps] = useState(15);
  const [idx, setIdx] = useState(0);
  const [playing, setPlaying] = useState(true);
  const [state, setState] = useState<"loading" | "ready" | "error">("loading");

  useEffect(() => {
    let dead = false;
    const urls: string[] = [];
    (async () => {
      try {
        const r = await fetch(src);
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const buf = new Uint8Array(await r.arrayBuffer());
        const dv = new DataView(buf.buffer);
        let usec = 66666;
        for (let i = 0; i < Math.min(buf.length - 12, 256); i++) {
          if (buf[i] === 0x61 && buf[i + 1] === 0x76 && buf[i + 2] === 0x69 && buf[i + 3] === 0x68) {
            usec = dv.getUint32(i + 8, true) || usec;
            break;
          }
        }
        for (let i = 0; i < buf.length - 8; ) {
          if (buf[i] === 0x69 && buf[i + 1] === 0x64 && buf[i + 2] === 0x78 && buf[i + 3] === 0x31) {
            break; // idx1: past the last frame
          }
          if (buf[i] === 0x30 && buf[i + 1] === 0x30 && buf[i + 2] === 0x64 && buf[i + 3] === 0x63) {
            const len = dv.getUint32(i + 4, true);
            const start = i + 8;
            if (len > 0 && start + len <= buf.length && buf[start] === 0xff && buf[start + 1] === 0xd8) {
              urls.push(
                URL.createObjectURL(
                  new Blob([buf.subarray(start, start + len)], { type: "image/jpeg" }),
                ),
              );
              i = start + len + (len & 1);
              continue;
            }
          }
          i++;
        }
        if (dead) return;
        if (urls.length === 0) throw new Error("no frames found");
        setFrames(urls);
        setFps(Math.min(30, Math.max(1, Math.round(1_000_000 / usec))));
        setState("ready");
      } catch (e) {
        console.error(e);
        if (!dead) setState("error");
      }
    })();
    return () => {
      dead = true;
      urls.forEach((u) => URL.revokeObjectURL(u));
    };
  }, [src]);

  useEffect(() => {
    if (!playing || state !== "ready") return;
    const t = setInterval(
      () => setIdx((i) => (i + 1) % frames.length),
      1000 / fps,
    );
    return () => clearInterval(t);
  }, [playing, state, frames.length, fps]);

  if (state === "loading") return <p className="viewer-note">Loading clip…</p>;
  if (state === "error")
    return <p className="viewer-note">This clip could not be loaded. Check the cloud connection and try again.</p>;
  return (
    <div className="clip">
      <img
        src={frames[idx]}
        alt=""
        onClick={() => setPlaying((p) => !p)}
      />
      <div className="clip-controls">
        <button className="btn quiet" onClick={() => setPlaying((p) => !p)}>
          {playing ? "Pause" : "Play"}
        </button>
        <input
          type="range"
          min={0}
          max={frames.length - 1}
          value={idx}
          onChange={(e) => {
            setPlaying(false);
            setIdx(Number(e.currentTarget.value));
          }}
        />
        <span className="clip-time">
          {(idx / fps).toFixed(1)}s / {(frames.length / fps).toFixed(1)}s
        </span>
      </div>
    </div>
  );
}

/* A voice row whose player retries after a failed load — the personal
 * cloud sleeps when idle and the first request can catch it waking. */
function VoiceRow({
  src,
  time,
  size,
  inCloud,
}: {
  src: string;
  time: string;
  size: string;
  inCloud: boolean;
}) {
  const [attempt, setAttempt] = useState(0);
  return (
    <div className="voice">
      <span className="voice-label">Voice memo</span>
      <audio
        key={attempt}
        controls
        preload="metadata"
        src={src}
        onError={() => {
          if (attempt < 2) {
            setTimeout(() => setAttempt((a) => a + 1), 1500 * (attempt + 1));
          }
        }}
      />
      <span className="voice-spacer" />
      <span className="voice-meta">
        {time}
        <span className="voice-size"> · {size}</span>
        {inCloud && <CloudMark />}
      </span>
    </div>
  );
}

function CloudMark() {
  return (
    <svg
      className="cloudmark"
      width="12"
      height="12"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2.2"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-label="in the cloud"
    >
      <path d="M17.5 19a4.5 4.5 0 0 0 .42-8.98 6 6 0 0 0-11.7 1.4A4 4 0 0 0 7 19z" />
    </svg>
  );
}

export default App;

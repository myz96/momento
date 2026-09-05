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

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDate(ms: number): string {
  if (!ms) return "";
  return new Date(ms).toLocaleString();
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

function displayName(name: string): string {
  // Local files carry a "<epoch-ms>_" prefix; hide it in the UI.
  return name.replace(/^\d+_/, "");
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
  const freeSpace = freeSpaceStr === "on";
  const autoBackup = autoBackupStr === "on";

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
    setStatus("Contacting the device…");
    try {
      const i = await invoke<DeviceInfo>("device_info", { base });
      setStatus(
        `Device found: ${i.files} file(s), ${formatBytes(i.total_bytes)}.`,
      );
    } catch (e) {
      setStatus("");
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }

  async function syncNow() {
    setBusy(true);
    setError("");
    setStatus("Starting sync…");
    const onProgress = new Channel<SyncProgress>();
    onProgress.onmessage = (p) => {
      setStatus(`Syncing ${p.index}/${p.total} — ${p.file}`);
    };
    try {
      const synced = await invoke<LocalFile[]>("sync_device", {
        base,
        onProgress,
      });
      setStatus(
        synced.length === 0
          ? "The device has no files."
          : `Synced ${synced.length} file(s). The device storage is clear.`,
      );
      await refreshMedia();
    } catch (e) {
      setError(String(e));
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
        `Device joined ${result.ssid} at ${result.ip}. Sync now works over your home network.`,
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
      if (!silent) setCloudStatus("Checking what the cloud already has…");
      const onProgress = new Channel<SyncProgress>();
      onProgress.onmessage = (p) => {
        // Auto runs stay quiet until they have a result to report.
        if (!silent) {
          setCloudStatus(
            `Uploading ${p.index}/${p.total} — ${displayName(p.file)}`,
          );
        }
      };
      try {
        const report = await invoke<BackupReport>("backup_to_cloud", {
          backend: backendUrl,
          freeSpace,
          apiKey: cloudKey,
          onProgress,
        });
        const parts = [
          report.uploaded === 0
            ? "Nothing new to upload."
            : `Uploaded ${report.uploaded} file(s).`,
        ];
        if (report.freed > 0) {
          parts.push(
            `Freed ${formatBytes(report.freed_bytes)} on this device (${report.freed} file(s) now stream from the cloud).`,
          );
        }
        if (!silent || report.uploaded > 0 || report.freed > 0) {
          setCloudStatus(parts.join(" "));
        } else if (autoFailedRef.current) {
          // A quiet success after a failure clears the stale warning.
          setCloudStatus("Auto-backup is working again. Everything is backed up.");
        }
        autoFailedRef.current = false;
        await refreshMedia();
      } catch (e) {
        if (!silent) {
          setCloudStatus("");
          setCloudError(String(e));
        } else {
          // Quiet, but never invisible: a broken auto-backup must surface.
          autoFailedRef.current = true;
          setCloudStatus("Auto-backup failed. Retrying every 5 minutes.");
        }
      } finally {
        cloudBusyRef.current = false;
        setCloudBusy(false);
      }
    },
    [backendUrl, freeSpace, cloudKey, refreshMedia],
  );

  // The ref pattern keeps the timers stable: typing in the URL field must
  // not fire a backup against a half-typed address.
  const backupRef = useRef(backupToCloud);
  backupRef.current = backupToCloud;

  useEffect(() => {
    if (!autoBackup) return;
    const kickoff = setTimeout(() => backupRef.current(true), 3000);
    const timer = setInterval(() => backupRef.current(true), 5 * 60 * 1000);
    return () => {
      clearTimeout(kickoff);
      clearInterval(timer);
    };
  }, [autoBackup]);

  return (
    <main className="container">
      <header>
        <h1>Momento</h1>
        <p className="subtitle">Your captured moments, synced from the device.</p>
      </header>

      <section className="device-panel">
        <h2>Device Wi-Fi setup (one time)</h2>
        <p className="hint">
          Hold the CAM button on the device for 1.5 s (the LED blinks), then
          send it your home Wi-Fi over Bluetooth. After this, the device joins
          your network and your Mac keeps its internet.
        </p>
        <div className="row">
          <input
            value={wifiSsid}
            onChange={(e) => setWifiSsid(e.currentTarget.value)}
            placeholder="Home Wi-Fi name"
            disabled={provBusy}
          />
          <input
            type="password"
            value={wifiPass}
            onChange={(e) => setWifiPass(e.currentTarget.value)}
            placeholder="Wi-Fi password"
            disabled={provBusy}
          />
          <button onClick={provisionWifi} disabled={provBusy}>
            {provBusy ? "Sending…" : "Send over Bluetooth"}
          </button>
        </div>
        {provStatus && <p className="status">{provStatus}</p>}
        {provError && <p className="error">{provError}</p>}
      </section>

      <section className="device-panel">
        <h2>Device</h2>
        <p className="hint">
          Hold the CAM button on the device for 1.5 s. Set up once above and
          use <code>http://momento.local</code>. Without setup, join the{" "}
          <strong>Momento</strong> network (password <code>momento123</code>)
          and use <code>http://192.168.4.1</code>.
        </p>
        <div className="row">
          <input
            value={base}
            onChange={(e) => setBase(e.currentTarget.value)}
            placeholder="http://192.168.4.1"
            disabled={busy}
          />
          <button onClick={checkDevice} disabled={busy}>
            Check device
          </button>
          <button className="primary" onClick={syncNow} disabled={busy}>
            Sync now
          </button>
        </div>
        {status && <p className="status">{status}</p>}
        {error && <p className="error">{error}</p>}
      </section>

      <section className="device-panel">
        <h2>Cloud backup</h2>
        <p className="hint">
          Uploads the library to the Momento backend. Files already in the
          cloud are skipped.
        </p>
        <div className="row">
          <input
            value={backendUrl}
            onChange={(e) => setBackendUrl(e.currentTarget.value)}
            placeholder="http://localhost:8000"
            disabled={cloudBusy}
          />
          <input
            type="password"
            value={cloudKey}
            onChange={(e) => setCloudKey(e.currentTarget.value)}
            placeholder="API key (empty for local)"
            disabled={cloudBusy}
          />
          <button
            className="primary"
            onClick={() => backupToCloud(false)}
            disabled={cloudBusy}
          >
            {cloudBusy ? "Uploading…" : "Back up now"}
          </button>
        </div>
        <label className="checkline">
          <input
            type="checkbox"
            checked={freeSpace}
            onChange={(e) => setFreeSpaceStr(e.currentTarget.checked ? "on" : "off")}
            disabled={cloudBusy}
          />
          Free up space after backup (keep thumbnails, stream originals)
        </label>
        <label className="checkline">
          <input
            type="checkbox"
            checked={autoBackup}
            onChange={(e) => setAutoBackupStr(e.currentTarget.checked ? "on" : "off")}
          />
          Back up automatically when the cloud is reachable
        </label>
        {cloudStatus && <p className="status">{cloudStatus}</p>}
        {cloudError && <p className="error">{cloudError}</p>}
      </section>

      <section>
        <div className="gallery-head">
          <h2>Library</h2>
          <span className="count">{media.length} item(s)</span>
        </div>
        {media.length === 0 ? (
          <p className="hint">No media yet. Sync the device to fill this list.</p>
        ) : (
          groupByDay(media).map(([label, files]) => (
            <div key={label}>
              <h3 className="day-head">{label}</h3>
              <div className="gallery">
                {files.map((m) => {
                  const inCloud = m.location === "cloud";
                  const src = inCloud
                    ? `${backendUrl}/media/${m.name}${
                        cloudKey ? `?key=${encodeURIComponent(cloudKey)}` : ""
                      }`
                    : convertFileSrc(m.path);
                  const thumbSrc = inCloud && m.thumb ? convertFileSrc(m.thumb) : src;
                  return (
                    <div className="card" key={m.name}>
                      {m.kind === "photo" && (
                        <img src={thumbSrc} alt={displayName(m.name)} />
                      )}
                      {m.kind === "audio" && (
                        <div className="audio-box">
                          <span className="badge">Voice</span>
                          <audio controls src={src} />
                        </div>
                      )}
                      {m.kind === "video" && (
                        <div className="video-box">
                          {inCloud && m.thumb ? (
                            <img src={thumbSrc} alt={displayName(m.name)} />
                          ) : (
                            <span className="badge">Clip</span>
                          )}
                          <button
                            onClick={() =>
                              invoke("open_media", {
                                path: inCloud ? src : m.path,
                              })
                            }
                          >
                            Open in player
                          </button>
                        </div>
                      )}
                      <div className="meta">
                        <span className="name">
                          {displayName(m.name)}
                          {inCloud && <span className="cloud-badge">☁︎</span>}
                        </span>
                        <span className="sub">
                          {formatBytes(m.size)} · {formatDate(m.modified_ms)}
                        </span>
                      </div>
                    </div>
                  );
                })}
              </div>
            </div>
          ))
        )}
      </section>
    </main>
  );
}

export default App;

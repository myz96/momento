import { useCallback, useEffect, useState } from "react";
import { Channel, convertFileSrc, invoke } from "@tauri-apps/api/core";
import "./App.css";

type DeviceInfo = { device: string; files: number; total_bytes: number };
type SyncProgress = { file: string; index: number; total: number };
type WifiStatus = { state: string; ip: string; ssid: string };
type BackupReport = { uploaded: number; already_backed_up: number };
type LocalFile = {
  name: string;
  path: string;
  size: number;
  kind: string;
  modified_ms: number;
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

function displayName(name: string): string {
  // Local files carry a "<epoch-ms>_" prefix; hide it in the UI.
  return name.replace(/^\d+_/, "");
}

function App() {
  const [base, setBase] = useState("http://momento.local");
  const [status, setStatus] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [media, setMedia] = useState<LocalFile[]>([]);
  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [provStatus, setProvStatus] = useState("");
  const [provError, setProvError] = useState("");
  const [provBusy, setProvBusy] = useState(false);
  const [backendUrl, setBackendUrl] = useState("http://localhost:8000");
  const [cloudStatus, setCloudStatus] = useState("");
  const [cloudError, setCloudError] = useState("");
  const [cloudBusy, setCloudBusy] = useState(false);

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

  async function backupToCloud() {
    setCloudBusy(true);
    setCloudError("");
    setCloudStatus("Checking what the cloud already has…");
    const onProgress = new Channel<SyncProgress>();
    onProgress.onmessage = (p) => {
      setCloudStatus(`Uploading ${p.index}/${p.total} — ${displayName(p.file)}`);
    };
    try {
      const report = await invoke<BackupReport>("backup_to_cloud", {
        backend: backendUrl,
        onProgress,
      });
      setCloudStatus(
        report.uploaded === 0
          ? `Nothing new to upload. ${report.already_backed_up} file(s) already backed up.`
          : `Uploaded ${report.uploaded} file(s). ${report.already_backed_up} were already backed up.`,
      );
    } catch (e) {
      setCloudStatus("");
      setCloudError(String(e));
    } finally {
      setCloudBusy(false);
    }
  }

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
          <button className="primary" onClick={backupToCloud} disabled={cloudBusy}>
            {cloudBusy ? "Uploading…" : "Back up now"}
          </button>
        </div>
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
          <div className="gallery">
            {media.map((m) => (
              <div className="card" key={m.path}>
                {m.kind === "photo" && (
                  <img src={convertFileSrc(m.path)} alt={displayName(m.name)} />
                )}
                {m.kind === "audio" && (
                  <div className="audio-box">
                    <span className="badge">Voice</span>
                    <audio controls src={convertFileSrc(m.path)} />
                  </div>
                )}
                {m.kind === "video" && (
                  <div className="video-box">
                    <span className="badge">Clip</span>
                    <button onClick={() => invoke("open_media", { path: m.path })}>
                      Open in player
                    </button>
                  </div>
                )}
                <div className="meta">
                  <span className="name">{displayName(m.name)}</span>
                  <span className="sub">
                    {formatBytes(m.size)} · {formatDate(m.modified_ms)}
                  </span>
                </div>
              </div>
            ))}
          </div>
        )}
      </section>
    </main>
  );
}

export default App;

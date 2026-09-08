# Flash session runbook

Everything below is code-complete and builds green. The device still
runs the pre-streaming firmware. This session flashes the new build and
verifies each pending feature. Budget: about an hour.

## 0. Prep (2 min)

The build is already done (`build/momento.bin`). If you changed code:

```bash
source ~/.espressif/tools/activate_idf_v6.1-beta1.sh   # never pipe this
cd apps/firmware && idf.py build
```

Open the Mac app (`/Applications/momento-app.app`) — you need it for
the BLE and sync checks.

## 1. Flash (5 min)

The board wedges esptool when sync-mode Wi-Fi is active. Use the
replug trick:

1. Unplug the device.
2. Arm the watcher, then plug in — it flashes ~2 s after the port
   appears:

```bash
cd apps/firmware
while [ ! -e /dev/tty.usbmodem* ]; do sleep 0.5; done; sleep 2; \
  idf.py -p $(ls /dev/tty.usbmodem*) flash monitor
```

3. If esptool still reports "No serial data received": unplug, replug,
   re-run. Never open the port with `cat` or pyserial between tries.

Keep the monitor open for the whole session — every check below prints
log lines.

## 2. Recorder checks (15 min)

| Check | Steps | Pass when |
|---|---|---|
| Long recording | Press REC. Wait 60+ s. Press REC. | Monitor shows one clip, no stop at 30 s or 60 s |
| **Rotation (new)** | Press REC. Wait past 5 min. Press REC. | Monitor shows "Clip cap reached; rolling into a new clip"; SD holds VID_N and VID_N+1 with matching AUD pairs |
| Crash safety | Press REC. Wait 30 s. Pull the USB cable. Replug. | The clip exists and plays up to ~4 s before the pull |

Tip for rotation: a 5-minute wait is slow. For a fast check, set
`MAX_RECORD_US` to `20LL * 1000 * 1000` (20 s), build, flash, verify the
roll, then restore the value and flash the final build.

## 3. Clock and dates (10 min)

1. Hold CAM 1.5 s → sync mode (LED blinks). The device joins the home
   network (station retry runs until it lands).
2. Monitor shows the SNTP line when the clock sets.
3. Take a photo and a short memo AFTER the clock sets.
4. Sync in the Mac app → the gallery groups them under today with a
   real time.
5. Cross-check the cloud: `uv run --project apps/backend momento ls`
   shows the same capture times after a backup.

Hotspot reminders (if you test on the iPhone hotspot): 2.4 GHz only —
Maximize Compatibility ON; keep the hotspot settings screen open while
the device joins; mDNS does not resolve — use the device IP
(172.20.10.2–14).

## 4. BLE and hands-free sync (15 min)

| Check | Steps | Pass when |
|---|---|---|
| BLE advertise from boot | Power the device, do NOT hold CAM | The app's device pill finds it |
| BLE wake | Device idle (not in sync mode). Press Sync in the app | App wakes the device over BLE (0x02), reads the IP, syncs, then exits sync mode (0x03); no button holds |
| Auto-sync | Enable the auto-sync toggle in settings. Wait 3 min | A sync runs by itself, then chains into a backup |
| Provisioning copy | Open Wi-Fi setup, send credentials | Status reaches "sta" within ~45 s; "ap" along the way is not an error |

## 5. Phone session (separate, with the iPhone)

1. `cd apps/mobile && pnpm tauri ios dev 'MZ's iPhone'` (curly
   apostrophe; phone unlocked; Local Network permission → force-quit and
   reopen once).
2. Verify the new design on the phone.
3. Start a sync, background the app → the BackgroundGuard must keep it
   running (task #14).
4. After a backup from the phone, `momento ls` shows real capture times
   for the new files.

## 6. Wrap up

- Task list: #16, #18, #19, #31 complete when their checks pass; #14
  after the phone check.
- Any failure: keep the monitor output — it is the debug artifact.

## Australia hardware session (~2026-09-22, with Zach)

Order matters: wire and test EVERYTHING on the breadboard first;
desolder the header pins only after both checks pass. The breadboard is
the debug harness — keep it until nothing needs probing.

1. **Battery.** Solder the LiPo to the XIAO's battery pads (underside
   of the board — separate from the header pins; charger is built in,
   charges over the existing USB port). Check: device boots on battery
   alone; record a clip; check runtime.
2. **Haptics.** Wire the DRV2605L breakout: VIN→3V3, GND→GND,
   SDA→GPIO5, SCL→GPIO6, motor to the driver's motor terminals. The
   driver is already in the firmware (`haptics.c`, ERM library A,
   Adafruit-breakout defaults). Check: boot log says "DRV2605L ready";
   CAM press = one click; REC press = double click at start AND stop.
   No chip wired = clean no-op ("haptics off") — nothing breaks.
   LRA motor instead of ERM? Change REG_FEEDBACK bit 7 and the library
   in haptics.c.
3. Only now: desolder the header pins (flush-cut the spacer, pull pins
   one at a time with the iron on the pad), wire buttons/LED/DRV
   point-to-point, and print the v0 case (magnet ring on the
   phone-facing wall, magnets away from the camera and mic).

- Deep-sleep firmware comes after the battery proves itself.

# Smart Button V2 — ESP32-C3, Supabase, GitHub OTA

A standalone ESP32-C3 Super Mini device, with direct Wi-Fi (no hub), MFRC522 RFID, push button, RGB LED, battery/plug diagnostics, offline event buffering, Supabase HTTPS ingest, and automatic versioned OTA firmware releases from GitHub.

**Firmware:** `2.0.0` (first release). **Target:** ESP32-C3 only. **Important:** The code is provided as a release candidate; a physical-device compile / end-to-end OTA test still needs to be performed. Do not deploy unattended until tested.

## Structure

```
src/main.cpp                      PlatformIO firmware source
src/SB2Types.h                   Shared types (avoids Arduino auto-prototype errors)
src/TLSRoots.h                   Public TLS CA root certificates (NO secrets)
arduino/Smart_Button_V2/         Arduino IDE sketch + required headers
platformio.ini                   ESP32-C3 board, OTA partition scheme, libraries
.github/workflows/build-ota.yml Build + publish binary and manifest to GitHub
build/esp32c3/                   Automatically populated OTA release binaries
version.txt                      The currently built version
 tools/write_manifest.py         Creates SHA256 manifest and copies firmware.bin
 tools/sync_arduino.py           Keep PlatformIO and Arduino IDE files identical
```

## 1. Confirm Supabase is ready (NO OLD API)

Use the **new Smart Button V2 Supabase project**, with the SQL schema from the previous starter package. It must contain:

- `public.sb2_devices` (a device `BTN-V2-001` and a stored hash of its unique token)
- `public.sb2_events`
- `public.sb2_ingest_event(...)`, with `EXECUTE` granted to the `anon` role
- `public.sb2_dashboard_users`, for dashboard accounts

The firmware sends:

```
POST https://YOUR_PROJECT_REF.supabase.co/rest/v1/rpc/sb2_ingest_event
apikey: sb_publishable_...
Content-Type: application/json

{
  "p_device_code": "BTN-V2-001",
  "p_device_token": "UNIQUE_DEVICE_TOKEN",
  "p_event_type": "TEST_BUTTON_CLICK",
  "p_event_uuid": "d5a5ae48-3a23-42e9-afd2-a3ae1aa1f923",
  "p_metadata": {
    "firmware": "2.0.0",
    "battery_mv": 3850,
    "plugged": false
  }
}
```

Your *real* device token is generated once by the registration SQL in your Supabase SQL Editor and **must never be committed to GitHub**. If you lost it, rotate it using the SQL from `02_register_device.sql`. The dashboard login uses Supabase Auth email/password, NOT these device credentials. Never put a service-role/secret key, database password, or dashboard password into this firmware.

> Only an API response with HTTP `2xx` and JSON `"ok":true` removes an event from the flash-backed queue. Events retain their UUID across retries and reboot. See limitations below.

## 2. Actual verified hardware pinout

| Hardware | ESP32-C3 pin | Configuration |
|---|---:|---|
| Metal push button | GPIO2 | Active HIGH, physical external 10k pull-down |
| Plug detect | GPIO0 | Digital HIGH=plugged (confirm physical wiring) |
| Battery voltage | GPIO1 | ADC through 10k:10k divider, multiplier x2 |
| RGB LED red | GPIO21 | HIGH = ON |
| RGB LED green | GPIO20 | HIGH = ON |
| RGB LED blue | GPIO3 | HIGH = ON |
| MFRC522 SCK | GPIO4 | SPI |
| MFRC522 MISO | GPIO5 | SPI |
| MFRC522 MOSI | GPIO6 | SPI |
| MFRC522 SDA/SS | GPIO7 | SPI select |
| MFRC522 RST | GPIO10 | Reset |

Use a stable **3.3V supply for MFRC522**, not 5V. The ADC can measure the voltage of the net connected through the divider, but is not automatically proof of an individual cell's state of charge. The linear percentage estimate (3.0–4.2V) is intentionally labeled approximate; verify with a multimeter. GPIO2 is a strapping pin; keep its resistor circuit compatible with boot.

## 3. Flash the FIRST firmware manually from Arduino IDE

1. Install **Espressif ESP32 boards** through Boards Manager (Arduino ESP32 core 3.x recommended). Install library **MFRC522** (Miguel Balboa) and **ArduinoJson version 6** (not version 7, as this sketch uses the v6 API).
2. Open `arduino/Smart_Button_V2/Smart_Button_V2.ino`. Keep **all three files** together: `.ino`, `SB2Types.h`, `TLSRoots.h`.
3. Set **Tools → Board → ESP32C3 Dev Module**. Set **USB CDC On Boot: Enabled**, **Flash Size: 4MB**.
4. Under Partition Scheme select **Minimal SPIFFS (1.9MB APP with OTA)** or an equivalent 4MB dual-OTA-app layout. **Do not use Huge APP / No OTA.**
5. The bundled first-boot hotspot password is for development only. **Change it to a unique per-device password in the setup page**, stored in NVS and preserved across OTA updates. Optionally replace `SETUP_AP_PASSWORD_DEFAULT` in the source before your first manual flash.
6. Change `OTA_REPO_OWNER` / `OTA_REPO_NAME` if your GitHub repository differs from the defaults. Leave the `FW_VERSION` at `2.0.0` for the first release.
7. Upload once over USB-C. Open Serial Monitor at **115200 baud**.

> PlatformIO alternative: install PlatformIO, then run `pio run -e esp32c3 -t upload` and `pio device monitor -b 115200` from the project root. `platformio.ini` configures the correct board and OTA partition layout.

## 4. Provision credentials through the local hotspot

On first start, incomplete configuration, or a 3-second hold:

- **Wi-Fi SSID:** `SmartButtonV2-Setup`
- **AP password:** `changeMeV2-2026` only on first boot, or your previously saved per-device hotspot password (the default is for development only)
- **Setup page:** http://192.168.4.1
- **AP TX power:** 8.5 dBm, applied after `softAP()` (previously verified with this ESP32-C3 Super Mini)

In the setup page, enter:

1. `Device Code`: `BTN-V2-001` (must match `sb2_devices.device_code`)
2. Wi-Fi SSID: select from the scan dropdown or type a hidden network manually; provide Wi-Fi password
3. Supabase Project URL: `https://YOUR_PROJECT_REF.supabase.co` (from Supabase Connect or Dashboard Settings)
4. Supabase Publishable Key: `sb_publishable_...` (from **Settings → API Keys**)
5. Unique Device Token: the 64-character token from `02_register_device.sql`
6. Set a **new, unique Setup Hotspot Password** (8–63 characters). This is saved in NVS and survives OTA updates.
7. Keep **TEST mode** checked initially
8. Click **Save and Restart**

The password and device token are persisted in ESP32 NVS (Preferences) and intentionally **not printed** in the Serial Monitor. For real devices, physical flash access can expose unencrypted NVS: protect the device and consider flash encryption in production.

If an earlier experiment left pending events in the old firmware's `v2_events` namespace, this firmware never reads that namespace; it uses the new `sb2queue` namespace. **Do not erase flash** if you need to recover old events from a previous test.

## 5. Event behavior and verification

The device performs the following:

- Power-on → queue `TEST_DEVICE_ON` once per boot
- RFID card → green LED, UID printed, queue `TEST_RFID_SCAN` with `p_badge_uid`
- Short button release → blue LED, queue `TEST_BUTTON_CLICK`
- Button hold **3 seconds** → open setup AP; **no click/count event**
- Every second → print voltage, approximate battery percentage, charger-plug state, queue size, Wi-Fi status
- Wi-Fi disconnected → slow red pulse and queue events to NVS (capacity **24 events**)
- Wi-Fi reconnect → drain queued events, retry same UUID on failure
- When internet and clock are ready → check GitHub OTA after first successful sends and approximately every hour
- Serial Monitor: type `u` and Send to request an immediate OTA check (once pending queue is drained), or `p` to request the setup AP

Production events (`DEVICE_ON`, `BUTTON_CLICK`, `RFID_SCAN`) are enabled only when **TEST mode is unchecked** in the setup page. Any previously stored test events keep their original test classification.

Test SQL:

```sql
select event_uuid, device_code, event_type, badge_uid, event_value,
       metadata, received_at
from public.sb2_events
where device_code = 'BTN-V2-001'
order by received_at desc
limit 30;
```

Expected Serial Monitor after provisioning:

```
[RFID] Version=0x82
[WIFI] Connected IP=192.168.x.x RSSI=-50 channel=6
[TIME] UTC timestamp ...; HTTPS ready
[API] TEST_DEVICE_ON seq=1 HTTP=200 accepted=1 response={"ok":true,...}
```

The dashboard currently derives "online" from **recent events**. It may show an idle, healthy device offline after 90 seconds; this firmware deliberately does not invent additional `HEARTBEAT` event types unsupported by the existing schema. A proper heartbeat event/function and dashboard change can be added separately.

## 6. Create the GitHub repository to host OTA builds

**IMPORTANT: The repository must actually be created on GitHub; having the ZIP locally does not publish it.**

1. Go to https://github.com/new.
2. Create repository **`smart_button_v2_ota`** under GitHub user **`ismailoviic`** (or adjust the firmware's `OTA_REPO_OWNER` and `OTA_REPO_NAME`). Choose **Public** for GitHub raw downloads without personal-access tokens. Keep the repo limited to firmware source and binaries; NEVER upload your device tokens.
3. Upload the entire *contents* of this package, retaining `.github/workflows/build-ota.yml` and the root `platformio.ini`. You can use GitHub Desktop, or in a terminal:

   ```bash
   cd Smart_Button_V2_GitHub_OTA
   git init -b main
   git add .
   git commit -m "Initial ESP32-C3 Smart Button V2 firmware"
   git remote add origin https://github.com/ismailoviic/smart_button_v2_ota.git
   git push -u origin main
   ```

4. Open **GitHub → Actions → Build and publish ESP32-C3 OTA firmware**. The workflow installs PlatformIO, compiles **ESP32-C3** firmware, writes the SHA-256 manifest, and commits both files to `build/esp32c3/`.
5. Check **Settings → Actions → General → Workflow permissions** and ensure Actions may write repository contents (subject to organization policy). The workflow has `permissions: contents: write`. If automatic push is blocked by branch protection, download the compiled artifact from Actions and upload `firmware.bin`, `manifest.json`, and `version.txt` manually to main.
6. Verify these URLs are now publicly reachable:

   ```text
   https://raw.githubusercontent.com/ismailoviic/smart_button_v2_ota/main/build/esp32c3/manifest.json
   https://raw.githubusercontent.com/ismailoviic/smart_button_v2_ota/main/build/esp32c3/firmware.bin
   ```

The generated `manifest.json` has the exact fields the device expects:

```json
{
  "version": "2.0.0",
  "target": "esp32c3",
  "size": 123456,
  "sha256": "<64 lowercase hexadecimal characters generated from the actual binary>"
}
```

*The sample size/hash are placeholders; **GitHub Actions generates the real manifest** from the compiled firmware.*

## 7. How to release an OTA update

1. Change `#define FW_VERSION "2.0.0"` to `"2.0.1"` in **`src/main.cpp`** (plus your code change).
2. Run `python tools/sync_arduino.py` to update the Arduino IDE copy. **OR** if you edited the Arduino `.ino` instead, run `python tools/sync_arduino.py --from-arduino`.
3. Commit and push the changed files to GitHub main.
4. Wait for GitHub Actions to finish successfully. Check `build/esp32c3/manifest.json` now shows `2.0.1` with a matching SHA256 digest of the generated `.bin`.
5. Keep the device powered and connected. It checks OTA about once an hour, **or** send `u` from Serial Monitor and let the pending event queue drain first.
6. Watch Serial Monitor: `[OTA] Upgrading 2.0.0 -> 2.0.1`, progress logs, checksum validation, restart. After restart the printed firmware version should be `2.0.1`.

**GitHub Actions does not need your Wi-Fi passwords or Supabase tokens.** The OTA binary has only code and public CA certificates; secrets remain in per-device NVS and survive normal OTA upgrades.

## 8. Boundaries / commissioning checks

- **Build verification:** No ESP32 Arduino compiler/toolchain was available in the preparation environment. GitHub Actions is included specifically to perform the actual build check. Correct any compiler/version incompatibilities revealed by that first build *before* deploying to devices. The source has been structurally checked and follows Arduino core 2.x/3.x-compatible APIs where feasible.
- **TLS trust:** Uses real certificate validation with a small bundle of widely used public CA roots (`ISRG Root X1`, `DigiCert Global Root G2`, `GTS Root R1` and `GTS Root R4`). If Supabase or GitHub changes certificate chain to an unsupported root, update `TLSRoots.h` from a trusted CA source and release a tested firmware. It never calls `setInsecure()`.
- **OTA integrity:** The SHA-256 digest is checked before activating a firmware image. SHA-256 alone is not a digital signature; authenticity relies on valid HTTPS plus GitHub repository security. Enable GitHub 2FA and branch protection for production.
- **Internet access / time:** NTP access is required for normal certificate verification. If the factory firewall blocks NTP, `[TIME] Waiting ...` continues and HTTPS uploads will not start until the clock syncs.
- **Flash wear:** Each queued event persists the full 24-entry queue to NVS. This is fine as a pilot but should be redesigned with a wear-aware append log for high-volume industrial counting.
- **Queue limits:** Queue holds 24 events. If full, the device logs `[QUEUE] FULL`, flashes red, and does not invent/drop a count silently. An extended outage can still lose new production events after queue fills.
- **Event time:** Supabase `received_at` is server-receipt time. For buffered offline events the real action time is not yet reconstructed; metadata includes the boot ID and uptime. Do not use server receipt time as precise occurrence time during outages.
- **Battery percentage:** A linear approximation and may not match real Li-ion state of charge. Verify the analog measurement and BMS divider with a multimeter.
- **Client access:** Change the local AP password in the portal on first setup; it is persisted in NVS, so OTA cannot revert to the packaged default. Rotate Supabase device tokens if exposed. The default AP credential is only for laboratory setup.
- **Testing:** Start with TEST events, confirm `[API] ... HTTP=200 accepted=1`, check Supabase SQL, then the Vercel dashboard. Test live events only after correct filtering, access and device configuration are confirmed.

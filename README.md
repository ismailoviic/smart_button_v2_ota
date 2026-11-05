# Smart Button V2 — ESP32-C3 Manual GitHub OTA (Arduino IDE only)

Firmware `2.0.0`, proof-of-concept for **ESP32-C3 Super Mini**. One `.ino` source file, one `version.txt`, and one manually exported application binary `build/esp32c3/firmware.bin`. **No Python, PlatformIO, GitHub Actions, or other build scripts required.**

## 1. Configure the sketch

Edit **only the constants at the top** of `Smart_Button_V2/Smart_Button_V2.ino`:

- `FW_VERSION` — start `2.0.0`, increment on every firmware release.
- `DEVICE_CODE` — `BTN-V2-001` (must be the `device_code` registered in your fresh Supabase `sb2_devices`).
- `WIFI_SSID` and `WIFI_PASSWORD` — 2.4 GHz Wi-Fi details.
- `SUPABASE_URL` — the full new project root `https://YOUR_PROJECT_REF.supabase.co`, **not** the old HTTP backend.
- `SUPABASE_KEY` — **publishable** key `sb_publishable_...`.
- `SUPABASE_DEVICE_TOKEN` — the unique 64-character token returned when you registered the button (if lost, rotate it in Supabase and use the new token).
- `GITHUB_USERNAME`, `GITHUB_REPO`, and `GITHUB_BRANCH` — must match the *public* repository with `version.txt` and compiled binary.
- `TEST_MODE_DEFAULT` — true for `TEST_` events, false for real `DEVICE_ON`, `BUTTON_CLICK`, `RFID_SCAN`.

**Security:** A publicly accessible GitHub repository must **not** contain real Wi-Fi passwords or device tokens in its `.ino` source. Prefer leaving the placeholder constants in the public source and entering live values once through the firmware's setup hotspot (stored in NVS). Also note that a public compiled `.bin` can reveal embedded literal credentials: keep real secrets out of the *firmware binary* by using portal provisioning. The source-code placeholders are convenient for a private local prototype, not a secure production model. Publishable keys are safe for clients, but unique device tokens are secrets. Do not put Supabase service-role / `sb_secret_` keys on devices.

Important: existing values in ESP32 NVS take precedence over source defaults. Change them using the setup portal, or explicitly erase NVS if it is safe to discard old configuration; do **not** wipe flash when there are unrecovered events.

## 2. Arduino IDE settings

1. Install `esp32 by Espressif Systems` under Boards Manager, and `MFRC522` under Library Manager (SPI / WiFi / HTTPClient / Update / Preferences are included with ESP32 core).
2. Open `Smart_Button_V2/Smart_Button_V2.ino`.
3. Board: **ESP32C3 Dev Module**. USB CDC On Boot: **Enabled**. Flash size: **4 MB** (for typical C3 Super Mini board). Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA)** or another 4MB **dual OTA** partition layout. A no-OTA/huge-app layout cannot install remote firmware.
4. Upload **once using USB**. Serial Monitor 115200 baud.

Your working hardware pinout is built in: BAT GPIO1 (10k/10k divider), PLUG GPIO0 digital, button GPIO2 active HIGH, LEDs R21/G20/B3, MFRC522 SCK4/MISO5/MOSI6/SS7/RST10. If your wiring changes, modify these constants.

On boot, the red/green/blue LED sequence runs; battery voltage prints each second; RFID gives a green flash, short button clicks blink red twice, and a **3-second hold** enters the setup hotspot. The serial command `p` also opens setup. The `u` command requests an immediate OTA check when connected.

If placeholders are still present (or you hold the button), connect to Wi-Fi `SmartButtonV2-Setup`, password initially `changeMeV2-2026`, open `http://192.168.4.1`, choose a scanned network, and enter the device's Supabase details. **Use a unique setup AP password for deployments** by changing `SETUP_AP_PASSWORD` locally before compiling. The setup portal currently does not provide a form field to change the AP password.

## 3. First GitHub publication

Create a **public** GitHub repository. Its paths must match exactly:

```text
smart_button_v2_ota/
  Smart_Button_V2/Smart_Button_V2.ino
  version.txt                       (text: 2.0.0)
  build/esp32c3/firmware.bin        (Arduino-exported APPLICATION binary)
  README.md
```

After editing/updating code in Arduino IDE:

1. Select **Sketch → Export Compiled Binary**.
2. Locate the resulting **application** binary (`Smart_Button_V2.ino.bin` or similar) in the sketch directory or its `build/` subfolder, depending on Arduino-ESP32 core version.
3. **Do not choose** `.merged.bin`, `.bootloader.bin`, or `.partitions.bin`. Those are not the OTA application image.
4. Rename/copy the application binary to **`build/esp32c3/firmware.bin`** in your GitHub repository working directory.
5. Make sure `version.txt` contains `2.0.0` (one plain-text line) and the code's `FW_VERSION` matches it.
6. Push `Smart_Button_V2.ino`, `firmware.bin`, and `version.txt` to branch `main` (or your configured branch).
7. Verify these public URLs open successfully (replace owner/repo as needed):

```text
https://raw.githubusercontent.com/YOUR_GITHUB_USERNAME/smart_button_v2_ota/main/version.txt
https://raw.githubusercontent.com/YOUR_GITHUB_USERNAME/smart_button_v2_ota/main/build/esp32c3/firmware.bin
```

## 4. Each subsequent OTA update (the entire workflow)

1. Edit `Smart_Button_V2.ino` in Arduino IDE.
2. Increase **`#define FW_VERSION "2.0.0"` → `"2.0.1"`** (or higher).
3. **Sketch → Export Compiled Binary**, using the **same board and partition settings as the installed firmware**.
4. Replace `build/esp32c3/firmware.bin` in your GitHub repo with the new application `.ino.bin`.
5. Update `version.txt` to **`2.0.1`**. Ideally, upload the new binary **before** changing version.txt, so the device never sees a new version paired with an old binary.
6. Push the changes to the configured public branch.
7. Power on/restart the ESP32-C3: once Wi-Fi and NTP are ready, it checks `version.txt`, downloads `firmware.bin` over verified HTTPS when newer, installs with `Update`, then reboots. It also checks approximately every hour, or immediately when you enter `u` in Serial Monitor.
8. Verify the new boot line: `SMART BUTTON V2 2.0.1 / ESP32-C3`.

**An OTA success cannot be confirmed until you test it on your board.** GitHub CDN caching can delay seeing a just-pushed `version.txt`. Verify actual contents of the public URL if an update does not appear. Never publish a version change without matching application binary.

## 5. Supabase compatibility (new project only)

The sketch POSTS HTTPS JSON directly to:

```text
https://YOUR_PROJECT_REF.supabase.co/rest/v1/rpc/sb2_ingest_event
```

Request headers: `apikey: sb_publishable_...`, `Content-Type: application/json`. Body uses `p_device_code`, `p_device_token`, `p_event_type`, `p_badge_uid`, `p_event_uuid`, `p_metadata`, matching your `01_schema.sql`. Events get distinct UUIDs and are retained in a flash-backed queue of 24 pending events; they are removed after confirmed `HTTP 2xx` **and** JSON `ok:true`. Retries preserve UUID for server-side deduplication. The initial boot sends one `TEST_DEVICE_ON` event, short clicks send `TEST_BUTTON_CLICK`, scans send `TEST_RFID_SCAN`. This is the established new schema, **not** the former hub API.

In Supabase SQL Editor:

```sql
select received_at, device_code, event_type, badge_uid, metadata
from public.sb2_events
where device_code = 'BTN-V2-001'
order by received_at desc
limit 30;
```

## 6. Boundaries

- The sketch has been structurally reviewed here, **but an actual Arduino-ESP32 compile and physical device test could not be run**. Start with TEST mode and perform a real USB build before assuming it is production ready.
- TLS verifies connections against bundled public roots, which may need maintenance if GitHub or Supabase certificate chains change. The manual version+binary workflow does **not** implement a separate firmware signature/checksum. For commercial deployment use a signed firmware update process.
- Device credentials embedded in source or binaries are recoverable; storing tokens in ordinary NVS is also not tamperproof. The firmware and queue are for prototype use; avoid exposing real manufacturing data until authentication/physical access is hardened.
- The app slot must be large enough for the complete application binary; `Update.begin()` reports an error if the OTA partition is too small.
- The battery percentage is a rough linear estimate. Verify actual battery voltage and charge detection with a meter.
- An extended outage can fill the 24-event queue. Events after it fills are explicitly reported as not stored, not silently counted.
- A server receipt timestamp is not necessarily the same as the physical action time when events are buffered offline.
- If a TLS or Wi-Fi operation fails, Serial Monitor reports an HTTP/status error; the old firmware continues running.

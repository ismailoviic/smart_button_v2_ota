# Smart Button V2 — ESP32-C3

Production button for the OMS Textile ERP. Each button joins the factory Wi-Fi
and posts its events **directly to the ERP on the local network** (plain HTTP,
no hub, no cloud). Firmware updates come from this GitHub repository.

## Events

`POST http://<ERP address>/api/v1/iot/ingest`, JSON:

| `event_type` | When | Queued / resent |
|---|---|---|
| `DEVICE_ON`  | every boot | yes |
| `PIECE_DONE` | short press | yes |
| `BADGE_SCAN` | RFID card read (`badge_uid`) | yes |
| `HEARTBEAT`  | every 30 s while the queue is empty | no |

Other fields: `device_id` (device code, the button's name in the ERP), `mac`,
`firmware_version`, `event_id` (UUID, kept across resends; the ERP takes each
one once), `battery_mv`, `battery_pct`, `uptime_seconds`, `rssi`.

Queued events are stored in flash (24 max) and removed once the ERP answers
2xx. A 4xx other than 408/429 drops the event (a resend would be refused the
same way and would block the queue); network errors and 5xx are retried every
7 s. The ERP shows a button offline when it has not been heard from in 60 s.

## Setup (per button)

1. On the ERP server PC, from `oms-textile-backend`:
   `powershell -ExecutionPolicy Bypass -File scripts\button-address.ps1`
   It prints the address to type (e.g. `http://192.168.1.2:8000`) and checks a
   button can reach it; `-Fix` in an Administrator PowerShell repairs the WSL
   port forwarding and the firewall rule.
2. Hold the button **3 s** (or first boot without settings): it opens the
   hotspot `SmartButtonV2-Setup` (password `12345678`). Join it and open
   `http://192.168.4.1`.
3. Enter the Wi-Fi (2.4 GHz, same network as the ERP), the ERP address, and the
   device code (default `BTN-` + the end of the MAC, shown on the page).
   Save: the button restarts and connects.
4. In the ERP, **IoT Management → Device Registry**: the button appears after
   its first event; assign it to a chain, and set the chain's active model.

Settings live in NVS and survive firmware updates.

## LEDs

Power-on: red then blue. Press: two red flashes. Badge: blue. Setup hotspot:
blue. Error (queue full, event refused): red 1.2 s. Not on Wi-Fi: short red
blink every 3 s.

## Build and release (Arduino IDE)

Board **ESP32C3 Dev Module**, ESP32 core **3.3.12**, USB CDC On Boot
**Enabled**, Flash 4 MB, Partition scheme **Default 4MB with spiffs** (two
1.25 MB OTA slots). Library: **MFRC522**.

1. Bump `FW_VERSION` in `Smart_Button_V2/Smart_Button_V2.ino`.
2. **Sketch → Export Compiled Binary**; it lands in
   `Smart_Button_V2/build/esp32.esp32.esp32c3/` — `Smart_Button_V2.ino.bin` is
   the image the buttons download.
3. Put the same version in `version.txt`. Commit the binary **with or before**
   `version.txt`: a new version paired with an old binary makes every button
   download and reinstall it forever.
4. Push to `main`. Buttons check `version.txt` at boot and every hour (serial
   command `u` checks now).

First flash of a new board: over USB from the IDE.

## Limits

- The ERP ingest endpoint is unauthenticated: fine on the factory LAN, not
  to be exposed to the internet.
- Firmware updates are not signed; anyone who can push here can update every
  button.
- The whole queue is rewritten to flash on each event; on very busy stations
  this wears the flash over time.

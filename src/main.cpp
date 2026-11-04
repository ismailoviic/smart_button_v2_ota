/*
  SMART BUTTON V2 - ESP32-C3 SUPER MINI
  Wi-Fi -> Supabase HTTPS RPC; RFID; button; battery/plug telemetry;
  flash-backed event queue; GitHub manifest OTA with TLS + SHA256.

  Arduino IDE: ESP32C3 Dev Module, USB CDC On Boot enabled,
  4MB flash, Minimal SPIFFS (1.9MB APP with OTA) partition scheme.

  Device credentials are provisioned through the local setup AP and NVS.
  NEVER commit a device token, Wi-Fi password, or Supabase secret key.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <time.h>
#include <ctype.h>
#include "TLSRoots.h"
#include "SB2Types.h"

#ifndef CONFIG_IDF_TARGET_ESP32C3
#error "This firmware targets ESP32-C3. Select ESP32C3 Dev Module."
#endif

#define FW_VERSION "2.0.0"
#define DEVICE_DEFAULT "BTN-V2-001"
#define SETUP_AP_PASSWORD_DEFAULT "changeMeV2-2026" // onboarding only; change in setup page
#define OTA_REPO_OWNER "ismailoviic" // Replace if your GitHub account differs
#define OTA_REPO_NAME  "smart_button_v2_ota"
#define OTA_BRANCH     "main"

// Verified with the user's ESP32-C3 Super Mini hardware test.
#define PIN_BUTTON       2  // ACTIVE HIGH; external 10k pull-down
#define PIN_PLUG_DETECT  0  // HIGH when plugged, as tested
#define PIN_BAT_ADC      1  // 10k:10k voltage divider
#define PIN_LED_R       21
#define PIN_LED_G       20
#define PIN_LED_B        3
#define PIN_SPI_SCK      4
#define PIN_SPI_MISO     5
#define PIN_SPI_MOSI     6
#define PIN_RFID_SS      7
#define PIN_RFID_RST    10

#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_CAL_MULT 1.0f
#define WIFI_AP_TX_POWER  WIFI_POWER_8_5dBm
#define WIFI_STA_TX_POWER WIFI_POWER_8_5dBm

static const uint32_t BUTTON_DEBOUNCE_MS = 45;
static const uint32_t PORTAL_HOLD_MS = 3000;
static const uint32_t TELEMETRY_INTERVAL_MS = 1000;
static const uint32_t CONNECT_TIMEOUT_MS = 18000;
static const uint32_t POST_RETRY_MS = 6500;
static const uint32_t OTA_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const uint32_t QUEUE_MAGIC = 0x53324232UL;


Preferences settings;
WebServer webServer(80);
DNSServer dns;
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
SemaphoreHandle_t queueMutex = nullptr;
QueueImage queued = {};

String deviceCode = DEVICE_DEFAULT;
String wifiSSID, wifiPassword, supabaseUrl, publishableKey, deviceToken;
String setupApPassword = SETUP_AP_PASSWORD_DEFAULT;
bool testMode = true;
volatile bool portalMode = false;
volatile bool portalRequested = false;
volatile bool otaRequested = false;
volatile int lastWiFiReason = 0;
volatile int lastHttpCode = 0;
uint32_t bootId = 0;
uint32_t connectStartedMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastButtonEdgeMs = 0;
uint32_t buttonPressedAt = 0;
uint32_t ledEffectUntilMs = 0;
uint32_t otaLastCheckMs = 0;
uint32_t nextPostMs = 0;
uint8_t wifiAttempts = 0;
uint8_t ledEffect = 0; // 1 green, 2 blue, 3 red, 4 cyan
bool buttonRaw = false, buttonStable = false, longHoldUsed = false;
bool clockConfigured = false;
bool onlineLogged = false;

void openSetupPortal();
void loadSettings();
void networkWorker(void *arg);
void scanRFID();
void checkButton();
void updateLED();
void triggerLED(uint8_t color, uint32_t duration);
void queueEvent(uint8_t kind, const String &uid = "");
void startStation();
void checkOTA();

static String htmlEscape(String v) {
  v.replace("&", "&amp;"); v.replace("<", "&lt;");
  v.replace(">", "&gt;"); v.replace("\"", "&quot;");
  v.replace("'", "&#39;"); return v;
}

void rgb(bool red, bool green, bool blue) {
  digitalWrite(PIN_LED_R, red ? HIGH : LOW);
  digitalWrite(PIN_LED_G, green ? HIGH : LOW);
  digitalWrite(PIN_LED_B, blue ? HIGH : LOW);
}
void triggerLED(uint8_t color, uint32_t duration) {
  ledEffect = color;
  ledEffectUntilMs = millis() + duration;
}
void updateLED() {
  uint32_t now = millis();
  if (ledEffect && (int32_t)(ledEffectUntilMs - now) > 0) {
    if (ledEffect == 1) rgb(false, true, false);
    else if (ledEffect == 2) rgb(false, false, true);
    else if (ledEffect == 3) rgb(true, false, false);
    else rgb(false, true, true);
    return;
  }
  ledEffect = 0;
  bool redPulse = !portalMode && WiFi.status() != WL_CONNECTED && (now % 3000 < 120);
  rgb(redPulse, false, false);
}

uint16_t readBatteryMv() {
  uint32_t sum = 0;
  for (uint8_t i=0; i<8; ++i) {
    sum += analogReadMilliVolts(PIN_BAT_ADC);
  }
  float mv = (sum / 8.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL_MULT;
  return (uint16_t)constrain((int)lroundf(mv), 0, 65535);
}
uint8_t batteryPercent(uint16_t mv) {
  if (mv >= 4200) return 100;
  if (mv <= 3000) return 0;
  return (uint8_t)((mv - 3000UL) * 100UL / 1200UL);
}
bool isPlugged() { return digitalRead(PIN_PLUG_DETECT) == HIGH; }

void newUUID(char output[37]) {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(output, 37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
    b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

bool persistQueueLocked() {
  Preferences p;
  if (!p.begin("sb2queue", false)) return false;
  size_t written = p.putBytes("events", &queued, sizeof(queued));
  p.end();
  return written == sizeof(queued);
}
void loadQueue() {
  queued.magic = QUEUE_MAGIC;
  Preferences p;
  if (p.begin("sb2queue", true)) {
    if (p.getBytesLength("events") == sizeof(queued)) {
      QueueImage tmp = {};
      if (p.getBytes("events", &tmp, sizeof(tmp)) == sizeof(tmp) &&
          tmp.magic == QUEUE_MAGIC && tmp.count <= QUEUE_CAPACITY) {
        queued = tmp;
      }
    }
    p.end();
  }
  Serial.printf("[QUEUE] Restored %u events, seq=%lu\n",
    queued.count, (unsigned long)queued.nextSequence);
}
uint16_t pendingEvents() {
  uint16_t count = 0;
  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    count = queued.count;
    xSemaphoreGive(queueMutex);
  }
  return count;
}
void queueEvent(uint8_t kind, const String &uid) {
  StoredEvent ev = {};
  newUUID(ev.uuid);
  uid.substring(0, sizeof(ev.badgeUid)-1).toCharArray(ev.badgeUid, sizeof(ev.badgeUid));
  ev.bootId = bootId;
  ev.uptimeSeconds = millis() / 1000UL;
  ev.batteryMv = readBatteryMv();
  ev.plugged = isPlugged();
  ev.kind = kind;
  ev.testMode = testMode ? 1 : 0;

  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(750)) != pdTRUE) {
    Serial.println("[QUEUE] ERROR: unable to lock queue");
    triggerLED(3, 800);
    return;
  }
  if (queued.count >= QUEUE_CAPACITY) {
    Serial.println("[QUEUE] FULL: event not stored; resolve connectivity");
    triggerLED(3, 1200);
    xSemaphoreGive(queueMutex);
    return;
  }
  ev.sequence = ++queued.nextSequence;
  queued.events[queued.count++] = ev;
  if (!persistQueueLocked()) {
    --queued.count;
    --queued.nextSequence;
    Serial.println("[QUEUE] ERROR: event could not be persisted");
    triggerLED(3, 1200);
  } else {
    Serial.printf("[EVENT] Queued kind=%u seq=%lu test=%d pending=%u\n",
      ev.kind, (unsigned long)ev.sequence, ev.testMode, queued.count);
  }
  xSemaphoreGive(queueMutex);
}

void loadSettings() {
  if (!settings.begin("sb2cfg", true)) {
    Serial.println("[CFG] Could not read NVS");
    return;
  }
  deviceCode = settings.getString("device", DEVICE_DEFAULT);
  wifiSSID = settings.getString("ssid", "");
  wifiPassword = settings.getString("pass", "");
  supabaseUrl = settings.getString("url", "");
  publishableKey = settings.getString("pub", "");
  deviceToken = settings.getString("token", "");
  setupApPassword = settings.getString("ap_pass", SETUP_AP_PASSWORD_DEFAULT);
  testMode = settings.getBool("test", true);
  settings.end();
  supabaseUrl.trim();
  while (supabaseUrl.endsWith("/")) supabaseUrl.remove(supabaseUrl.length()-1);
  Serial.printf("[CFG] Device=%s, SSID=%s, project-configured=%d, token-configured=%d, TEST=%d\n",
    deviceCode.c_str(), wifiSSID.c_str(), supabaseUrl.length()>0,
    deviceToken.length()>0, testMode);
}

const char* eventName(const StoredEvent &ev) {
  static const char* testNames[] = {"TEST_DEVICE_ON", "TEST_BUTTON_CLICK", "TEST_RFID_SCAN"};
  static const char* liveNames[] = {"DEVICE_ON", "BUTTON_CLICK", "RFID_SCAN"};
  if (ev.kind > 2) return "INVALID";
  return ev.testMode ? testNames[ev.kind] : liveNames[ev.kind];
}

bool canSend() {
  return WiFi.status() == WL_CONNECTED && supabaseUrl.startsWith("https://") &&
         publishableKey.length() > 10 && deviceToken.length() >= 32 &&
         time(nullptr) > 1700000000;
}

// The actual device secret is NEVER logged. Only an API success response is logged.
bool postEvent(const StoredEvent &ev) {
  if (!canSend()) return false;
  DynamicJsonDocument doc(1536);
  doc["p_device_code"] = deviceCode;
  doc["p_device_token"] = deviceToken;
  doc["p_event_type"] = eventName(ev);
  doc["p_event_uuid"] = ev.uuid;
  if (strlen(ev.badgeUid)) doc["p_badge_uid"] = ev.badgeUid;
  JsonObject m = doc.createNestedObject("p_metadata");
  m["firmware"] = FW_VERSION;
  m["boot_id"] = ev.bootId;
  m["sequence"] = ev.sequence;
  m["uptime_seconds"] = ev.uptimeSeconds;
  m["battery_mv"] = ev.batteryMv;
  m["battery_pct_approx"] = batteryPercent(ev.batteryMv);
  m["plugged"] = (bool)ev.plugged;
  m["wifi_rssi_dbm"] = WiFi.RSSI();
  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setCACert(SB2_CA_BUNDLE);
  HTTPClient http;
  String endpoint = supabaseUrl + "/rest/v1/rpc/sb2_ingest_event";
  if (!http.begin(client, endpoint)) {
    Serial.println("[API] HTTP begin failed");
    return false;
  }
  http.setConnectTimeout(7000);
  http.setTimeout(12000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", publishableKey);
  int code = http.POST(body);
  String response = http.getString();
  http.end();
  lastHttpCode = code;
  if (response.length() > 230) response = response.substring(0, 230);
  bool accepted = false;
  if (code >= 200 && code < 300) {
    DynamicJsonDocument reply(512);
    if (deserializeJson(reply, response) == DeserializationError::Ok) {
      accepted = reply["ok"] | false;
    }
  }
  Serial.printf("[API] %s seq=%lu HTTP=%d accepted=%d response=%s\n",
    eventName(ev), (unsigned long)ev.sequence, code, accepted, response.c_str());
  return accepted;
}

int drainOneEvent() {
  StoredEvent ev = {};
  bool have = false;
  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    if (queued.count) { ev = queued.events[0]; have = true; }
    xSemaphoreGive(queueMutex);
  }
  if (!have) return 0;
  if (!postEvent(ev)) return -1;
  if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(750)) == pdTRUE) {
    if (queued.count && strcmp(queued.events[0].uuid, ev.uuid) == 0) {
      // Only remove AFTER the RPC returned ok=true. UUID deduplicates retries.
      memmove(&queued.events[0], &queued.events[1],
        (queued.count-1) * sizeof(StoredEvent));
      --queued.count;
      if (!persistQueueLocked()) Serial.println("[QUEUE] ACK persisted unsuccessfully; duplicate retry possible");
    }
    xSemaphoreGive(queueMutex);
  }
  return 1;
}

static String manifestURL() {
  return String("https://raw.githubusercontent.com/") + OTA_REPO_OWNER + "/" +
         OTA_REPO_NAME + "/" + OTA_BRANCH + "/build/esp32c3/manifest.json";
}
static String firmwareURL() {
  return String("https://raw.githubusercontent.com/") + OTA_REPO_OWNER + "/" +
         OTA_REPO_NAME + "/" + OTA_BRANCH + "/build/esp32c3/firmware.bin";
}
int compareVersion(const String &remote, const char* current) {
  unsigned int a=0,b=0,c=0,x=0,y=0,z=0;
  if (sscanf(remote.c_str(), "%u.%u.%u", &a,&b,&c) != 3 ||
      sscanf(current, "%u.%u.%u", &x,&y,&z) != 3) return -2;
  if (a!=x) return a>x ? 1 : -1;
  if (b!=y) return b>y ? 1 : -1;
  if (c!=z) return c>z ? 1 : -1;
  return 0;
}
bool isSha256Hex(const String &s) {
  if (s.length()!=64) return false;
  for (size_t i=0;i<s.length();i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

// Security: firmware and manifest are TLS verified against public CA roots;
// binary digest must match manifest before Update.end() can activate image.
bool installFirmware(const String &expectedSha, uint32_t expectedSize) {
  WiFiClientSecure client;
  client.setCACert(SB2_CA_BUNDLE);
  HTTPClient http;
  if (!http.begin(client, firmwareURL())) return false;
  http.setConnectTimeout(9000);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] firmware GET status=%d\n", code); http.end(); return false;
  }
  int contentLength = http.getSize();
  if (contentLength < 32768 || contentLength > 1900000 ||
      (expectedSize && (uint32_t)contentLength != expectedSize)) {
    Serial.printf("[OTA] invalid firmware content length %d\n", contentLength);
    http.end(); return false;
  }
  if (!Update.begin((size_t)contentLength, U_FLASH)) {
    Serial.println("[OTA] Update.begin failed (check OTA partition scheme)");
    Update.printError(Serial); http.end(); return false;
  }
  mbedtls_md_context_t shaContext;
  mbedtls_md_init(&shaContext);
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!mdInfo || mbedtls_md_setup(&shaContext, mdInfo, 0) != 0 ||
      mbedtls_md_starts(&shaContext) != 0) {
    Serial.println("[OTA] SHA256 initialization failed");
    mbedtls_md_free(&shaContext); Update.abort(); http.end(); return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t received = 0;
  uint32_t lastBytesMs = millis();
  bool failed = false;
  triggerLED(4, 30000);
  while (received < (uint32_t)contentLength) {
    if (stream->available()) {
      size_t want = min((size_t)stream->available(), sizeof(buffer));
      want = min(want, (size_t)contentLength - received);
      int n = stream->readBytes(buffer, want);
      if (n <= 0 || Update.write(buffer, (size_t)n) != (size_t)n ||
          mbedtls_md_update(&shaContext, buffer, (size_t)n) != 0) {
        failed = true; break;
      }
      received += (uint32_t)n;
      lastBytesMs = millis();
      if ((received % (64*1024)) < (uint32_t)n)
        Serial.printf("[OTA] received %lu / %d bytes\n", (unsigned long)received, contentLength);
    } else {
      if (millis() - lastBytesMs > 30000 || WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] stream timeout/disconnect"); failed = true; break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  unsigned char digest[32] = {};
  if (mbedtls_md_finish(&shaContext, digest) != 0) failed = true;
  mbedtls_md_free(&shaContext);
  char hexDigest[65];
  for (size_t i=0;i<32;i++) snprintf(hexDigest+2*i, 3, "%02x", digest[i]);
  hexDigest[64] = 0;
  bool shaMatches = expectedSha.equalsIgnoreCase(String(hexDigest));
  if (failed || received != (uint32_t)contentLength || !shaMatches) {
    Serial.printf("[OTA] REJECTED: bytes=%lu expected=%d checksum_valid=%d\n",
      (unsigned long)received, contentLength, shaMatches);
    Update.abort(); http.end(); triggerLED(3, 1000); return false;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    Serial.println("[OTA] Update.end failed");
    Update.printError(Serial); http.end(); triggerLED(3, 1000); return false;
  }
  http.end();
  Serial.println("[OTA] Successful validated update. Restarting...");
  Serial.flush();
  ESP.restart();
  return true;
}

void checkOTA() {
  if (!canSend() || portalMode || pendingEvents()) return;
  Serial.printf("[OTA] Checking GitHub manifest: %s\n", manifestURL().c_str());
  WiFiClientSecure client;
  client.setCACert(SB2_CA_BUNDLE);
  HTTPClient http;
  if (!http.begin(client, manifestURL())) { Serial.println("[OTA] manifest HTTP begin failed"); return; }
  http.setConnectTimeout(7000);
  http.setTimeout(12000);
  http.addHeader("Cache-Control", "no-cache");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] manifest status=%d (check repo URL and release workflow)\n", code);
    http.end(); return;
  }
  String data = http.getString();
  http.end();
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, data) != DeserializationError::Ok) {
    Serial.println("[OTA] invalid manifest JSON"); return;
  }
  String target = doc["target"] | "";
  String version = doc["version"] | "";
  String sha = doc["sha256"] | "";
  uint32_t size = doc["size"] | 0UL;
  if (target != "esp32c3" || !isSha256Hex(sha) || size < 32768 || size > 1900000) {
    Serial.println("[OTA] invalid target / checksum / size"); return;
  }
  int cmp = compareVersion(version, FW_VERSION);
  if (cmp == -2) { Serial.println("[OTA] invalid version"); return; }
  if (cmp <= 0) {
    Serial.printf("[OTA] Up to date: local=%s remote=%s\n", FW_VERSION, version.c_str());
    return;
  }
  Serial.printf("[OTA] Upgrading %s -> %s\n", FW_VERSION, version.c_str());
  installFirmware(sha, size);
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Obtained IP: "); Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      lastWiFiReason = info.wifi_sta_disconnected.reason;
      Serial.printf("[WIFI] Disconnected reason=%d\n", (int)lastWiFiReason);
      break;
    default: break;
  }
}
void startStation() {
  if (wifiSSID.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false); // Do not overlap connect/reconnect calls.
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  bool ok = WiFi.setTxPower(WIFI_STA_TX_POWER);
  connectStartedMs = millis();
  ++wifiAttempts;
  Serial.printf("[WIFI] Connect attempt %u SSID=%s TX8.5=%d\n",
    wifiAttempts, wifiSSID.c_str(), ok);
}

void networkWorker(void*) {
  bool firstOnline = false;
  bool firstOtaChecked = false;
  while (true) {
    if (portalRequested && !portalMode) {
      portalRequested = false;
      openSetupPortal();
    }
    if (portalMode || wifiSSID.isEmpty()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      onlineLogged = false;
      firstOnline = false;
      if (millis() - connectStartedMs > CONNECT_TIMEOUT_MS) {
        if (wifiAttempts < 3) {
          WiFi.disconnect(false, false);
          vTaskDelay(pdMS_TO_TICKS(150));
          startStation();
        } else {
          Serial.println("[WIFI] Failed after 3 attempts; opening setup portal");
          portalRequested = true;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    wifiAttempts = 0;
    if (!onlineLogged) {
      onlineLogged = true;
      Serial.printf("[WIFI] Connected IP=%s RSSI=%d channel=%d\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    }
    if (!clockConfigured) {
      configTime(0, 0, "time.google.com", "pool.ntp.org");
      clockConfigured = true;
      Serial.println("[TIME] Waiting for NTP so HTTPS certificates can be verified");
    }
    if (time(nullptr) <= 1700000000) {
      vTaskDelay(pdMS_TO_TICKS(400));
      continue;
    }
    if (!firstOnline) {
      firstOnline = true;
      Serial.printf("[TIME] UTC timestamp %lld; HTTPS ready\n", (long long)time(nullptr));
    }
    if (pendingEvents() && (int32_t)(millis() - nextPostMs) >= 0) {
      int result = drainOneEvent();
      nextPostMs = millis() + (result > 0 ? 150UL : POST_RETRY_MS);
    }
    if (otaRequested || (!firstOtaChecked && millis() > 20000) ||
        (firstOtaChecked && millis() - otaLastCheckMs >= OTA_INTERVAL_MS)) {
      otaRequested = false;
      if (!pendingEvents()) {
        firstOtaChecked = true;
        otaLastCheckMs = millis();
        checkOTA();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void scanRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  char uid[24] = {};
  for (byte i=0; i<rfid.uid.size; ++i) {
    char one[3];
    snprintf(one, sizeof(one), "%02X", rfid.uid.uidByte[i]);
    if (strlen(uid)+2 < sizeof(uid)) strcat(uid, one);
  }
  Serial.printf("[RFID] UID=%s\n", uid);
  triggerLED(1, 500);
  queueEvent(2, String(uid));
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void checkButton() {
  uint32_t now = millis();
  bool raw = digitalRead(PIN_BUTTON) == HIGH; // external pull-down
  if (raw != buttonRaw) {
    buttonRaw = raw;
    lastButtonEdgeMs = now;
  }
  if (now - lastButtonEdgeMs >= BUTTON_DEBOUNCE_MS && raw != buttonStable) {
    buttonStable = raw;
    if (raw) {
      buttonPressedAt = now;
      longHoldUsed = false;
    } else if (!longHoldUsed && now - buttonPressedAt < PORTAL_HOLD_MS) {
      Serial.println("[BUTTON] Short click");
      triggerLED(2, 350);
      queueEvent(1);
    }
  }
  if (buttonStable && !longHoldUsed && now - buttonPressedAt >= PORTAL_HOLD_MS) {
    longHoldUsed = true;
    Serial.println("[BUTTON] Long hold: entering Wi-Fi setup, no count event");
    portalRequested = true;
    triggerLED(4, 800);
  }
}

String wifiScanJson() {
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "{\"status\":\"scanning\"}";
  if (n < 0) return "{\"status\":\"failed\"}";
  DynamicJsonDocument doc(4096);
  doc["status"] = "ready";
  JsonArray arr = doc.createNestedArray("networks");
  String seen[24];
  int used=0;
  for (int i=0; i<n && used<24; ++i) {
    String ssid=WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool duplicate=false;
    for (int j=0;j<used;j++) if(seen[j]==ssid) duplicate=true;
    if (duplicate) continue;
    seen[used++]=ssid;
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = ssid;
    obj["rssi"] = WiFi.RSSI(i);
    obj["open"] = WiFi.encryptionType(i)==WIFI_AUTH_OPEN;
  }
  String json;
  serializeJson(doc,json);
  return json;
}

void handlePortalRoot() {
  String s;
  s.reserve(7500);
  s += R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Smart Button V2 Setup</title><style>
  body{background:#0b1220;color:#edf2ff;font:15px system-ui;padding:18px}
  main{max-width:500px;margin:25px auto;background:#182337;border:1px solid #35455b;padding:24px;border-radius:16px}
  h2{margin-top:0} label{display:block;margin:13px 0 5px;font-weight:600}
  input,select,button{width:100%;padding:11px;box-sizing:border-box;border-radius:8px;font:inherit}
  input,select{background:#0b1220;color:white;border:1px solid #475569}
  button{margin-top:16px;background:#4dd9b2;border:0;font-weight:bold;cursor:pointer;color:#10251f}
  small{color:#afbed3} .secondary{background:#34445d;color:white}
  </style></head><body><main><h2>Smart Button V2 Setup</h2>
  <p>Firmware: )HTML";
  s += FW_VERSION;
  s += " | Pending events: " + String(pendingEvents());
  s += "<br>Device: " + htmlEscape(deviceCode) + "</p>";
  s += R"HTML(<form method="post" action="/save">
  <label>Device Code</label><input name="device" maxlength="40" required value=")HTML";
  s += htmlEscape(deviceCode);
  s += "\">";
  s += R"HTML(<label>Available 2.4 GHz Wi-Fi networks</label><select id="nets"><option>Scanning...</option></select>
  <button type="button" class="secondary" onclick="scan()">Refresh networks</button>
  <label>Wi-Fi SSID (editable for hidden networks)</label><input name="ssid" id="ssid" maxlength="32" required value=")HTML";
  s += htmlEscape(wifiSSID);
  s += "\">";
  s += R"HTML(<label>Wi-Fi Password</label><input name="pass" type="password" placeholder="Leave blank to keep existing password">
  <label>Supabase Project URL</label><input name="url" required value=")HTML";
  s += htmlEscape(supabaseUrl);
  s += "\">";
  s += R"HTML(<label>Supabase Publishable Key</label><input name="pub" type="password" autocomplete="off" placeholder="Leave blank to keep existing key">
  <label>Unique Device Token</label><input name="token" type="password" autocomplete="off" placeholder="Leave blank to keep existing token">
  <label>Setup Hotspot Password (change default after first boot)</label><input name="ap_pass" type="password" autocomplete="new-password" placeholder="Leave blank to keep existing AP password">
  <label><input style="width:auto" type="checkbox" name="test" )HTML";
  if (testMode) s += "checked ";
  s += R"HTML(> TEST mode (recommended until validated)</label>
  <button type="submit">Save and Restart</button></form>
  <p><small>Only 2.4 GHz Wi-Fi is supported. The device token is not shown here. Keep this setup hotspot private.</small></p>
  <script>
  async function scan(){
    const select=document.getElementById('nets');
    select.replaceChildren(new Option('Scanning...', ''));
    try{
      await fetch('/scan/start',{method:'POST'});
      for(let i=0;i<24;i++){
        await new Promise(r=>setTimeout(r,550));
        let data=await (await fetch('/scan/result')).json();
        if(data.status==='ready'){
          select.replaceChildren(new Option('Choose network',''));
          for(const n of data.networks) select.add(new Option(n.ssid+' ('+n.rssi+' dBm)'+(n.open?' open':' secured'), n.ssid));
          select.onchange=()=>{if(select.value)document.getElementById('ssid').value=select.value};
          return;
        }
      }
      select.replaceChildren(new Option('Scan timed out; enter SSID manually',''));
    }catch(e){select.replaceChildren(new Option('Scan unavailable; enter SSID manually',''));}
  }
  scan();
  </script></main></body></html>)HTML";
  webServer.send(200,"text/html",s);
}
void handlePortalSave() {
  String newDevice=webServer.arg("device"); newDevice.trim();
  String newSSID=webServer.arg("ssid"); newSSID.trim();
  String newURL=webServer.arg("url"); newURL.trim();
  String newPub=webServer.arg("pub");
  String newToken=webServer.arg("token");
  String newPassword=webServer.arg("pass");
  String newApPass=webServer.arg("ap_pass");
  while (newURL.endsWith("/")) newURL.remove(newURL.length()-1);
  if (newDevice.length()<3 || newDevice.length()>40 || newSSID.isEmpty() ||
      !newURL.startsWith("https://") || newURL.indexOf(".supabase.co")<0 ||
      (newPub.isEmpty() && publishableKey.isEmpty()) ||
      (newToken.isEmpty() && deviceToken.isEmpty())) {
    webServer.send(400,"text/plain","Invalid settings: device, SSID, Supabase URL, publishable key and device token required.");
    return;
  }
  if (newApPass.length() && (newApPass.length()<8 || newApPass.length()>63)) {
    webServer.send(400,"text/plain","Setup AP password must be 8-63 characters.");
    return;
  }
  if (newDevice!=deviceCode && !newToken.isEmpty() && pendingEvents()>1) {
    webServer.send(409,"text/plain","Pending events exist. Upload them before changing device identity.");
    return;
  }
  if (newDevice!=deviceCode && newToken.isEmpty()) {
    webServer.send(400,"text/plain","Provide a new device token when changing device code.");
    return;
  }
  if (newSSID!=wifiSSID && newPassword.isEmpty()) {
    // New network with empty password is valid only for an open network.
    Serial.println("[CFG] New SSID with empty password (open network assumed)");
  }
  Preferences p;
  if (!p.begin("sb2cfg",false)) { webServer.send(500,"text/plain","NVS unavailable"); return; }
  p.putString("device",newDevice);
  p.putString("ssid",newSSID);
  if (!newPassword.isEmpty() || newSSID!=wifiSSID) p.putString("pass",newPassword);
  p.putString("url",newURL);
  if (!newPub.isEmpty()) p.putString("pub",newPub);
  if (!newToken.isEmpty()) p.putString("token",newToken);
  if (!newApPass.isEmpty()) p.putString("ap_pass",newApPass);
  p.putBool("test", webServer.hasArg("test"));
  p.end();
  webServer.send(200,"text/html","<h2>Saved. Device restarting.</h2><p>Reconnect to factory Wi-Fi and check Serial Monitor.</p>");
  Serial.println("[CFG] Saved; restarting (no secrets printed)");
  delay(500);
  ESP.restart();
}
void openSetupPortal() {
  if (portalMode) return;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  bool ok=WiFi.softAP("SmartButtonV2-Setup",setupApPassword.c_str(),1);
  // Proven ESP32-C3 Super Mini SSID visibility workaround.
  WiFi.setTxPower(WIFI_AP_TX_POWER);
  Serial.printf("[AP] started=%d SSID=SmartButtonV2-Setup IP=%s TX=8.5 dBm\n",
    ok,WiFi.softAPIP().toString().c_str());
  if (!ok) { Serial.println("[AP] FAILED"); return; }
  dns.start(53,"*",IPAddress(192,168,4,1));
  webServer.on("/",HTTP_GET,handlePortalRoot);
  webServer.on("/save",HTTP_POST,handlePortalSave);
  webServer.on("/scan/start",HTTP_POST,[]() {
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true,false);
    }
    webServer.send(200,"application/json","{\"status\":\"scanning\"}");
  });
  webServer.on("/scan/result",HTTP_GET,[](){
    webServer.sendHeader("Cache-Control","no-store");
    webServer.send(200,"application/json",wifiScanJson());
  });
  webServer.onNotFound([](){
    webServer.sendHeader("Location","http://192.168.4.1/",true);
    webServer.send(302,"text/plain","");
  });
  webServer.begin();
  portalMode=true;
  triggerLED(4,650);
}

void setup() {
  Serial.begin(115200);
  delay(1200); // Only for USB CDC startup.
  Serial.printf("\nSMART BUTTON V2 %s / ESP32-C3\n",FW_VERSION);
  Serial.println("[BOARD] LED R21 G20 B3; BAT GPIO1; PLUG GPIO0; BUTTON GPIO2 active HIGH");
  Serial.println("[BOARD] MFRC522 SPI SCK4 MISO5 MOSI6 SS7 RST10");
  pinMode(PIN_LED_R,OUTPUT); pinMode(PIN_LED_G,OUTPUT); pinMode(PIN_LED_B,OUTPUT);
  rgb(false,false,false);
  pinMode(PIN_BUTTON,INPUT); // physical 10k pull-down
  pinMode(PIN_PLUG_DETECT,INPUT);
  pinMode(PIN_BAT_ADC,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC,ADC_11db);
  buttonRaw=buttonStable=(digitalRead(PIN_BUTTON)==HIGH);
  buttonPressedAt=millis();
  SPI.begin(PIN_SPI_SCK,PIN_SPI_MISO,PIN_SPI_MOSI,PIN_RFID_SS);
  rfid.PCD_Init();
  delay(45);
  Serial.printf("[RFID] Version=0x%02X\n",rfid.PCD_ReadRegister(MFRC522::VersionReg));
  queueMutex=xSemaphoreCreateMutex();
  if (!queueMutex) {Serial.println("[FATAL] no mutex");while(true){delay(1000);}}
  bootId=esp_random();
  loadQueue();
  loadSettings();
  queueEvent(0); // Device ON (test or production) exactly once per boot
  WiFi.onEvent(onWiFiEvent);
  if (wifiSSID.isEmpty() || supabaseUrl.isEmpty() ||
      publishableKey.isEmpty() || deviceToken.isEmpty()) {
    openSetupPortal();
  } else {
    startStation();
  }
  if (xTaskCreate(networkWorker,"sb2-network",16384,nullptr,1,nullptr)!=pdPASS)
    Serial.println("[FATAL] network worker failed");
  Serial.println("[SERIAL] Send 'u' to check GitHub OTA now; 'p' to open setup portal");
}

void loop() {
  updateLED();
  if (portalMode) {
    dns.processNextRequest();
    webServer.handleClient();
  } else {
    checkButton();
    scanRFID();
  }
  if (millis()-lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs=millis();
    uint16_t mv=readBatteryMv();
    Serial.printf("[BATTERY] %.3f V, ~%u%%, plugged=%s; pending=%u; Wi-Fi=%d\n",
      mv/1000.0f,batteryPercent(mv),isPlugged()?"YES":"NO",
      pendingEvents(),(int)WiFi.status());
  }
  while (Serial.available()) {
    char cmd=(char)Serial.read();
    if (cmd=='p' || cmd=='P') portalRequested=true;
    if (cmd=='u' || cmd=='U') otaRequested=true;
  }
  vTaskDelay(pdMS_TO_TICKS(5));
}

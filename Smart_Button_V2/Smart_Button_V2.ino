/*
  SMART BUTTON V2 — ESP32-C3 Super Mini
  Direct Supabase HTTPS events + GitHub version.txt / firmware.bin OTA.
  Arduino IDE only: Sketch > Export Compiled Binary; copy application .bin to GitHub.
  Target board: ESP32C3 Dev Module; USB CDC On Boot: Enabled;
  Partition scheme: 4 MB / dual OTA app slots (e.g. Minimal SPIFFS).
  Prototype: edit the placeholder constants below, or configure via setup AP.
  Do not publish Wi-Fi passwords or device tokens in a public GitHub repository.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <SPI.h>
#include <MFRC522.h>
#include <esp_system.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifndef CONFIG_IDF_TARGET_ESP32C3
#error "Select ESP32C3 Dev Module, not ESP32 Dev Module."
#endif

// ==================== EDIT THESE ====================
#define FW_VERSION          "2.0.12"
#define DEVICE_CODE         "BTN-V2-001"       // Must match sb2_devices in Supabase
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
#define SUPABASE_URL        "https://bvddjxonnunymwhuzitt.supabase.co"  // No trailing /
#define SUPABASE_KEY        "sb_publishable_aN5yqTjexArUtTB5evTQsA_vV38-Aa-"
#define SUPABASE_DEVICE_TOKEN "6d8ad66e1d4248198f4f66ca4b7e0cecca5b9f9393894acda7388f17449d2883"
#define GITHUB_USERNAME     "ismailoviic"
#define GITHUB_REPO         "smart_button_v2_ota"
#define GITHUB_BRANCH       "main"
#define SETUP_AP_PASSWORD   "12345678" // Prototype only; change before deployment
#define TEST_MODE_DEFAULT   true
// ====================================================

// Pins confirmed by the WORKING hardware-test sketch.
#define PIN_BAT_ADC      1    // Voltage divider 10k + 10k
#define PIN_PLUG         0    // Digital: HIGH means plugged, if verified
#define PIN_BUTTON       2    // Active HIGH, external 10k pull-down
#define PIN_LED_R       21   // Verified RED output; green channel disconnected/unused
#define PIN_LED_B        3   // Verified BLUE output
#define PIN_SPI_SCK      4
#define PIN_SPI_MISO     5
#define PIN_SPI_MOSI     6
#define PIN_RFID_SS      7
#define PIN_RFID_RST    10
#define STA_TX_POWER WIFI_POWER_8_5dBm   // This setting connected to 'isma' in testing
#define AP_TX_POWER  WIFI_POWER_8_5dBm   // C3 hotspot visibility workaround

static const uint32_t WIFI_RETRY_MS = 20000UL;
static const uint32_t POST_RETRY_MS = 7000UL;
static const uint32_t OTA_CHECK_MS = 3600000UL; // boot and every hour
static const uint32_t BUTTON_HOLD_MS = 3000UL;
static const uint32_t BUTTON_DEBOUNCE_MS = 45UL;
static const size_t QUEUE_LIMIT = 24;
static const uint32_t QUEUE_MAGIC = 0x53423234UL;

// Public TLS certificate roots (not credentials), supporting common Supabase/GitHub chains.
// Public CA root certificates (not secrets). Regenerate if server trust changes.

static const char SB2_TLS_ROOTS[] = R"SB2_CA(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)SB2_CA";


struct ButtonEvent {
  char uuid[37];
  char uid[24];
  uint32_t boot_id;
  uint32_t sequence;
  uint32_t uptime_s;
  uint16_t battery_mv;
  uint8_t plugged;
  uint8_t kind;    // 0=DEVICE_ON, 1=BUTTON_CLICK, 2=RFID_SCAN
  uint8_t is_test;
};
struct QueueStore {
  uint32_t magic;
  uint32_t sequence;
  uint16_t count;
  ButtonEvent entries[QUEUE_LIMIT];
};

QueueStore queueStore = {};
SemaphoreHandle_t queueLock = nullptr;
WebServer web(80);
DNSServer dns;
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
String cfgDevice = DEVICE_CODE, cfgSsid = WIFI_SSID, cfgPass = WIFI_PASSWORD;
String cfgUrl = SUPABASE_URL, cfgKey = SUPABASE_KEY, cfgToken = SUPABASE_DEVICE_TOKEN;
bool testMode = TEST_MODE_DEFAULT;
volatile bool portalMode = false, wantPortal = false, wantOTA = false;
bool buttonLastRaw = false, buttonStable = false, longHold = false;
uint32_t buttonEdgeMs = 0, buttonDownMs = 0, bootMs = 0;
uint32_t ledUntilMs = 0, lastTelemetry = 0, lastWifiAttempt = 0, lastOtaCheck = 0, nextPost = 0;
uint8_t ledEffect = 0; // 1 blue RFID, 2 red double blink button, 3 blue setup, 4 red error
uint32_t bootId = 0;
bool wasOnline = false, clockStarted = false, checkedOTAOnConnect = false;

// All custom event types are used only within function bodies to prevent
// Arduino IDE auto-prototype errors involving structs defined in .ino files.

String escapeJson(const String &value) {
  String out; out.reserve(value.length() + 8);
  for (size_t i=0; i<value.length(); ++i) {
    char c=value[i];
    if (c=='\\' || c=='\"') { out+='\\'; out+=c; }
    else if (c=='\n') out+="\\n";
    else if (c=='\r') out+="\\r";
    else if (c=='\t') out+="\\t";
    else if ((uint8_t)c < 32) out+='?';
    else out+=c;
  }
  return out;
}
String escapeHtml(String value) {
  value.replace("&","&amp;"); value.replace("<","&lt;");
  value.replace(">","&gt;"); value.replace("\"","&quot;");
  value.replace("'","&#39;"); return value;
}
// Green LED channel is intentionally unused due to reported hardware failure.
void rgb(bool red, bool blue) {
  digitalWrite(PIN_LED_R,red?HIGH:LOW);
  digitalWrite(PIN_LED_B,blue?HIGH:LOW);
}
void ledFlash(uint8_t effect, uint32_t ms) {
  ledEffect=effect; ledUntilMs=millis()+ms;
}
void updateLED() {
  uint32_t now=millis();
  if (now-bootMs<600) { // Power-on LED check: RED then BLUE only
    uint32_t step=(now-bootMs)/300;
    rgb(step==0,step==1); return;
  }
  if (ledEffect && (int32_t)(ledUntilMs-now)>0) {
    if (ledEffect==1) rgb(false,true); // RFID: blue
    else if (ledEffect==2) { // Button: two short red flashes
      uint32_t remaining=ledUntilMs-now;
      uint32_t elapsed=600-remaining;
      rgb((elapsed%300)<140,false);
    }
    else if (ledEffect==3) rgb(false,true); // Setup portal: blue
    else rgb(true,false); // Error: red
    return;
  }
  ledEffect=0;
  bool offline=WiFi.status()!=WL_CONNECTED;
  rgb(offline && (now%3000)<120,false);
}
uint16_t readBatteryMv() {
  uint32_t sum=0;
  for (int n=0;n<8;n++) sum+=analogReadMilliVolts(PIN_BAT_ADC);
  // Two equal resistors: voltage at GPIO1 = half the cell voltage.
  return (uint16_t)constrain((int)((sum/8.0f)*2.0f),0,65535);
}
int batteryPercent(uint16_t mv) {
  if (mv<=3000) return 0;
  if (mv>=4200) return 100;
  return (int)((mv-3000UL)*100UL/1200UL); // Approximation only
}
bool isPlugged() { return digitalRead(PIN_PLUG)==HIGH; }
void makeUUID(char out[37]) {
  uint8_t b[16]; esp_fill_random(b,sizeof(b));
  b[6]=(b[6]&0x0f)|0x40; b[8]=(b[8]&0x3f)|0x80;
  snprintf(out,37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}
void loadConfig() {
  Preferences p;
  if(p.begin("sb2cfg",true)) {
    cfgDevice=p.getString("device",DEVICE_CODE);
    cfgSsid=p.getString("ssid",WIFI_SSID);
    cfgPass=p.getString("pass",WIFI_PASSWORD);
    cfgUrl=p.getString("url",SUPABASE_URL);
    cfgKey=p.getString("pub",SUPABASE_KEY);
    cfgToken=p.getString("token",SUPABASE_DEVICE_TOKEN);
    testMode=p.getBool("test",TEST_MODE_DEFAULT);
    p.end();
  }
  cfgUrl.trim();
  while(cfgUrl.endsWith("/")) cfgUrl.remove(cfgUrl.length()-1);
  Serial.printf("[CFG] Device=%s SSID=%s test=%d tokenPresent=%d\n",
                cfgDevice.c_str(),cfgSsid.c_str(),testMode,cfgToken.length()>=32);
  Serial.println("[CFG] Saved setup-portal settings override source-code placeholders.");
}
void loadQueue() {
  queueStore.magic=QUEUE_MAGIC;
  Preferences p;
  if (p.begin("sb2qmanual",true)) {
    if(p.getBytesLength("events")==sizeof(queueStore)) {
      QueueStore stored={};
      if(p.getBytes("events",&stored,sizeof(stored))==sizeof(stored) &&
         stored.magic==QUEUE_MAGIC && stored.count<=QUEUE_LIMIT) queueStore=stored;
    }
    p.end();
  }
  Serial.printf("[QUEUE] Restored %u pending events\n",queueStore.count);
}
bool persistQueue() {
  Preferences p;
  if(!p.begin("sb2qmanual",false)) return false;
  size_t written=p.putBytes("events",&queueStore,sizeof(queueStore));
  p.end(); return written==sizeof(queueStore);
}
uint16_t pendingCount() {
  uint16_t n=0;
  if(xSemaphoreTake(queueLock,pdMS_TO_TICKS(500))==pdTRUE) {
    n=queueStore.count; xSemaphoreGive(queueLock);
  }
  return n;
}
void enqueueEvent(uint8_t kind,const String &uid="") {
  if(xSemaphoreTake(queueLock,pdMS_TO_TICKS(500))!=pdTRUE) return;
  if(queueStore.count>=QUEUE_LIMIT) {
    Serial.println("[QUEUE] FULL — event was NOT saved"); ledFlash(4,1200);
    xSemaphoreGive(queueLock); return;
  }
  ButtonEvent e={};
  makeUUID(e.uuid);
  uid.substring(0,sizeof(e.uid)-1).toCharArray(e.uid,sizeof(e.uid));
  e.kind=kind; e.is_test=testMode;
  e.boot_id=bootId; e.uptime_s=millis()/1000;
  e.battery_mv=readBatteryMv(); e.plugged=isPlugged();
  e.sequence=++queueStore.sequence;
  queueStore.entries[queueStore.count++]=e;
  if(!persistQueue()) {
    --queueStore.count; --queueStore.sequence;
    Serial.println("[QUEUE] Flash save failed"); ledFlash(4,1200);
  } else Serial.printf("[EVENT] Queued type=%u test=%d seq=%lu pending=%u\n",
                       kind,e.is_test,(unsigned long)e.sequence,queueStore.count);
  xSemaphoreGive(queueLock);
}
String eventName(uint8_t kind,bool test) {
  const char *namesTest[]={"TEST_DEVICE_ON","TEST_BUTTON_CLICK","TEST_RFID_SCAN"};
  const char *namesLive[]={"DEVICE_ON","BUTTON_CLICK","RFID_SCAN"};
  if(kind>2) return "INVALID";
  return test ? namesTest[kind] : namesLive[kind];
}
String eventJson() {
  // Called by the network worker with queueLock held. Do not expose token in logs.
  if(queueStore.count==0) return "";
  const ButtonEvent &e=queueStore.entries[0];
  String json="{";
  json+="\"p_device_code\":\""+escapeJson(cfgDevice)+"\",";
  json+="\"p_device_token\":\""+escapeJson(cfgToken)+"\",";
  json+="\"p_event_type\":\""+eventName(e.kind,e.is_test)+"\",";
  json+="\"p_event_uuid\":\""+String(e.uuid)+"\",";
  if(e.uid[0]) json+="\"p_badge_uid\":\""+String(e.uid)+"\",";
  else json+="\"p_badge_uid\":null,";
  json+="\"p_metadata\":{";
  json+="\"firmware\":\""+String(FW_VERSION)+"\",";
  json+="\"firmware_version\":\""+String(FW_VERSION)+"\",";
  json+="\"boot_id\":"+String(e.boot_id)+",";
  json+="\"sequence\":"+String(e.sequence)+",";
  json+="\"uptime_s\":"+String(e.uptime_s)+",";
  json+="\"battery_mv\":"+String(e.battery_mv)+",";
  json+="\"battery_pct_approx\":"+String(batteryPercent(e.battery_mv))+",";
  json+="\"plugged\":"+String(e.plugged ? "true" : "false")+",";
  json+="\"wifi_rssi_dbm\":"+String(WiFi.RSSI());
  json+="}}";
  return json;
}
bool clockValid() { return time(nullptr)>1700000000; }
bool sendFirstEvent() {
  if(!clockValid() || WiFi.status()!=WL_CONNECTED) return false;
  String body, uid;
  if(xSemaphoreTake(queueLock,pdMS_TO_TICKS(500))!=pdTRUE) return false;
  if(!queueStore.count) { xSemaphoreGive(queueLock); return true; }
  body=eventJson(); uid=queueStore.entries[0].uuid;
  String name=eventName(queueStore.entries[0].kind,queueStore.entries[0].is_test);
  xSemaphoreGive(queueLock);

  WiFiClientSecure tls; tls.setCACert(SB2_TLS_ROOTS);
  HTTPClient http;
  String endpoint=cfgUrl+"/rest/v1/rpc/sb2_ingest_event";
  if(!http.begin(tls,endpoint)) { Serial.println("[API] begin failed"); return false; }
  http.setConnectTimeout(7000); http.setTimeout(12000);
  http.addHeader("Content-Type","application/json");
  http.addHeader("apikey",cfgKey);
  int status=http.POST(body);
  String answer=http.getString();
  http.end();
  // PostgreSQL RPC returns {"ok":true,...}. Require both HTTP 2xx and ok=true.
  String normalized=answer;
  normalized.replace(" ",""); normalized.replace("\n",""); normalized.replace("\r","");
  bool accepted=(status>=200 && status<300 && normalized.indexOf("\"ok\":true")>=0);
  if(answer.length()>180) answer=answer.substring(0,180);
  Serial.printf("[API] %s HTTP=%d accepted=%d response=%s\n",name.c_str(),status,accepted,answer.c_str());
  if(!accepted) return false;
  if(xSemaphoreTake(queueLock,pdMS_TO_TICKS(1000))==pdTRUE) {
    if(queueStore.count && uid==queueStore.entries[0].uuid) {
      // Drop event only after confirmed server acceptance; UUID handles retries.
      memmove(queueStore.entries,queueStore.entries+1,
              (queueStore.count-1)*sizeof(ButtonEvent));
      queueStore.count--;
      if(!persistQueue()) Serial.println("[QUEUE] Could not persist ACK; possible replay");
    }
    xSemaphoreGive(queueLock);
  }
  return true;
}

String githubBase() {
  return String("https://raw.githubusercontent.com/")+GITHUB_USERNAME+"/"+
         GITHUB_REPO+"/"+GITHUB_BRANCH+"/";
}
int versionCompare(String remote) {
  unsigned int a=0,b=0,c=0,x=0,y=0,z=0; char extra=0;
  if(sscanf(remote.c_str(),"%u.%u.%u%c",&a,&b,&c,&extra)!=3 ||
     sscanf(FW_VERSION,"%u.%u.%u%c",&x,&y,&z,&extra)!=3) return -2;
  if(a!=x) return a>x ? 1:-1;
  if(b!=y) return b>y ? 1:-1;
  if(c!=z) return c>z ? 1:-1;
  return 0;
}
bool downloadAndInstallFirmware() {
String url = githubBase() +
  "Smart_Button_V2/build/esp32.esp32.esp32c3/Smart_Button_V2.ino.bin";
  WiFiClientSecure tls; tls.setCACert(SB2_TLS_ROOTS);
  HTTPClient http;
  if(!http.begin(tls,url)) return false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(8000); http.setTimeout(25000);
  int status=http.GET();
  int size=http.getSize();
  Serial.printf("[OTA] firmware status=%d size=%d\n",status,size);
  if(status!=HTTP_CODE_OK || size<32768) { http.end(); return false; }
  if(!Update.begin((size_t)size,U_FLASH)) {
    Serial.println("[OTA] Update.begin failed. Check dual-OTA partition scheme.");
    Update.printError(Serial); http.end(); return false;
  }
  auto *stream=http.getStreamPtr();
  if(!stream) { Serial.println("[OTA] No firmware response stream"); Update.abort(); http.end(); return false; }
  uint8_t buffer[1024];
  int received=0; uint32_t lastData=millis(); bool failed=false;
  while(received<size) {
    int n=stream->available();
    if(n>0) {
      size_t want=min((size_t)n,sizeof(buffer));
      want=min(want,(size_t)(size-received));
      int got=stream->readBytes(buffer,want);
      if(got<=0 || Update.write(buffer,got)!=(size_t)got) { failed=true; break; }
      received+=got; lastData=millis();
      if((received%65536)<got) Serial.printf("[OTA] %d/%d bytes\n",received,size);
    } else {
      if(millis()-lastData>25000 || WiFi.status()!=WL_CONNECTED) { failed=true; break; }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if(failed || received!=size || !Update.end() || !Update.isFinished()) {
    Serial.printf("[OTA] FAILED. Received %d/%d\n",received,size);
    Update.printError(Serial); Update.abort(); http.end(); return false;
  }
  http.end();
  Serial.println("[OTA] Update successful, restarting...");
  Serial.flush(); ESP.restart();
  return true;
}
void checkGitHubOTA() {
  if(!clockValid() || WiFi.status()!=WL_CONNECTED) return;
  if(String(GITHUB_USERNAME).startsWith("YOUR_")) {
    Serial.println("[OTA] Set GITHUB_USERNAME before using OTA."); return;
  }
  String url=githubBase()+"version.txt";
  Serial.printf("[OTA] Checking %s\n",url.c_str());
  WiFiClientSecure tls; tls.setCACert(SB2_TLS_ROOTS);
  HTTPClient http;
  if(!http.begin(tls,url)) { Serial.println("[OTA] version begin failed"); return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(8000); http.setTimeout(12000);
  http.addHeader("Cache-Control","no-cache");
  int status=http.GET(); String remote=(status==200) ? http.getString() : "";
  http.end(); remote.trim();
  if(status!=200) { Serial.printf("[OTA] version HTTP=%d\n",status); return; }
  int cmp=versionCompare(remote);
  if(cmp==-2) { Serial.printf("[OTA] Invalid version.txt: %s\n",remote.c_str()); return; }
  Serial.printf("[OTA] Local %s / GitHub %s\n",FW_VERSION,remote.c_str());
  if(cmp>0) {
    Serial.println("[OTA] Newer version found; downloading exported application binary...");
    if(!downloadAndInstallFirmware()) Serial.println("[OTA] Installation failed; continuing current firmware");
  } else Serial.println("[OTA] Already up to date");
}
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    Serial.printf("[WIFI] Disconnected, reason=%u\n",info.wifi_sta_disconnected.reason);
}
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false); // Prevent overlapping connection attempts.
  WiFi.begin(cfgSsid.c_str(),cfgPass.c_str());
  bool ok=WiFi.setTxPower(STA_TX_POWER);
  lastWifiAttempt=millis();
  Serial.printf("[WIFI] Connecting SSID=%s, TX power=8.5 dBm (%d)\n",cfgSsid.c_str(),ok);
}

String scanJson() {
  int count=WiFi.scanComplete();
  if(count==WIFI_SCAN_RUNNING) return "{\"status\":\"scanning\"}";
  if(count<0) return "{\"status\":\"unavailable\"}";
  String json="{\"status\":\"ready\",\"networks\":[";
  bool first=true;
  for(int i=0;i<count && i<35;i++) {
    String ssid=WiFi.SSID(i);
    if(!ssid.length()) continue;
    if(!first) json+=",";
    first=false;
    json+="{\"name\":\""+escapeJson(ssid)+"\",\"rssi\":"+String(WiFi.RSSI(i))+"}";
  }
  json+="]}";
  return json;
}
void portalHome() {
  String html=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><style>
  body{background:#0c1424;color:#e6f1ff;font:16px system-ui;padding:20px}
  main{max-width:500px;margin:20px auto;background:#19253a;padding:23px;border-radius:15px}
  input,select,button{display:block;width:100%;box-sizing:border-box;padding:11px;margin:7px 0 16px;border-radius:7px;font:inherit}
  input,select{background:#0c1424;color:white;border:1px solid #53657e}
  button{background:#49dcb1;border:0;font-weight:bold;color:#15251f}
  label{display:block;margin-top:8px}small{color:#bac5d3}</style></head><body><main><h2>Smart Button V2 Setup</h2>
  <p>Use a 2.4 GHz network. Credentials are stored on the ESP32-C3.</p><form action="/save" method="post">
  <label>Device code</label><input name="device" value=")HTML";
  html+=escapeHtml(cfgDevice);
  html+=R"HTML(" required><label>Detected Wi-Fi networks</label><select id="networks"><option>Scanning...</option></select>
  <label>SSID (editable)</label><input id="ssid" name="ssid" required value=")HTML";
  html+=escapeHtml(cfgSsid);
  html+=R"HTML("><label>Wi-Fi Password (blank keeps saved)</label><input name="password" type="password">
  <label>Supabase project URL</label><input name="url" required value=")HTML";
  html+=escapeHtml(cfgUrl);
  html+=R"HTML("><label>Supabase publishable key (blank keeps saved)</label><input name="key" type="password">
  <label>Device token (blank keeps saved)</label><input name="token" type="password">
  <label><input style="display:inline;width:auto" type="checkbox" name="test" )HTML";
  if(testMode) html+="checked ";
  html+=R"HTML(> Test mode</label><button type="submit">Save & Restart</button></form>
  <script>async function scan(){const s=document.getElementById('networks');try{
  await fetch('/scan',{method:'POST'});for(let i=0;i<22;i++){
  await new Promise(r=>setTimeout(r,600));const d=await(await fetch('/results')).json();
  if(d.status==='ready'){s.replaceChildren(new Option('Select a network',''));
  for(const n of d.networks)s.add(new Option(n.name+' ('+n.rssi+' dBm)',n.name));
  s.onchange=()=>{if(s.value)document.getElementById('ssid').value=s.value};return;}}
  s.replaceChildren(new Option('Scan timed out — enter SSID manually',''));
  }catch(e){s.replaceChildren(new Option('Enter SSID manually',''))}}scan();</script>
  <small>If changing the device code, update the Supabase registration token accordingly.</small>
  </main></body></html>)HTML";
  web.send(200,"text/html",html);
}
void savePortal() {
  String device=web.arg("device"), ssid=web.arg("ssid"), url=web.arg("url");
  String key=web.arg("key"), token=web.arg("token"), pass=web.arg("password");
  device.trim(); ssid.trim(); url.trim();
  if(ssid.isEmpty() || device.isEmpty() || !url.startsWith("https://")) {
    web.send(400,"text/plain","Invalid device/SSID/URL"); return;
  }
  if(device!=cfgDevice && pendingCount()>0) {
    web.send(409,"text/plain","Drain queued events before changing device identity"); return;
  }
  Preferences p;
  if(!p.begin("sb2cfg",false)) { web.send(500,"text/plain","NVS unavailable");return; }
  p.putString("device",device);
  p.putString("ssid",ssid);
  if(pass.length() || ssid!=cfgSsid) p.putString("pass",pass);
  p.putString("url",url);
  if(key.length()) p.putString("pub",key);
  if(token.length()) p.putString("token",token);
  p.putBool("test",web.hasArg("test"));
  p.end();
  web.send(200,"text/html","<h2>Saved. Device restarting.</h2>");
  delay(450); ESP.restart();
}
void startPortal() {
  if(portalMode) return;
  WiFi.disconnect(false,false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  bool ok=WiFi.softAP("SmartButtonV2-Setup",SETUP_AP_PASSWORD,1);
  WiFi.setTxPower(AP_TX_POWER); // Set after SoftAP for ESP32-C3 visibility.
  portalMode=ok;
  Serial.printf("[PORTAL] started=%d SSID=SmartButtonV2-Setup URL=http://192.168.4.1\n",ok);
  if(!ok) return;
  ledFlash(3,700);
  dns.start(53,"*",IPAddress(192,168,4,1));
  web.on("/",HTTP_GET,portalHome);
  web.on("/save",HTTP_POST,savePortal);
  web.on("/scan",HTTP_POST,[](){
    if(WiFi.scanComplete()!=WIFI_SCAN_RUNNING) {
      WiFi.scanDelete(); WiFi.scanNetworks(true,false);
    }
    web.send(200,"application/json","{\"status\":\"scanning\"}");
  });
  web.on("/results",HTTP_GET,[](){web.send(200,"application/json",scanJson());});
  web.onNotFound([](){web.sendHeader("Location","http://192.168.4.1/");web.send(302,"text/plain","");});
  web.begin();
}
void checkButton() {
  uint32_t now=millis();
  bool down=digitalRead(PIN_BUTTON)==HIGH;
  if(down!=buttonLastRaw) {buttonLastRaw=down;buttonEdgeMs=now;}
  if(down!=buttonStable && now-buttonEdgeMs>=BUTTON_DEBOUNCE_MS) {
    buttonStable=down;
    if(down) { buttonDownMs=now; longHold=false; }
    else if(!longHold) { Serial.println("[BUTTON] Click"); ledFlash(2,600); enqueueEvent(1); }
  }
  if(buttonStable && !longHold && now-buttonDownMs>=BUTTON_HOLD_MS) {
    longHold=true; wantPortal=true;
    Serial.println("[BUTTON] Hold 3 seconds -> setup hotspot");
  }
}
void checkRFID() {
  if(!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  char uid[24]={0};
  for(byte n=0;n<rfid.uid.size;n++) {
    char part[3]; snprintf(part,sizeof(part),"%02X",rfid.uid.uidByte[n]);
    if(strlen(uid)+2<sizeof(uid)) strcat(uid,part);
  }
  Serial.printf("[RFID] UID=%s\n",uid);
  ledFlash(1,450); enqueueEvent(2,uid);
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
}
void networkWorker(void*) {
  for(;;) {
    if(wantPortal && !portalMode) {wantPortal=false;startPortal();}
    if(portalMode) {vTaskDelay(pdMS_TO_TICKS(150));continue;}
    if(WiFi.status()!=WL_CONNECTED) {
      wasOnline=false;checkedOTAOnConnect=false;
      if(millis()-lastWifiAttempt>=WIFI_RETRY_MS) {
        WiFi.disconnect(false,false);
        vTaskDelay(pdMS_TO_TICKS(100)); startWiFi();
      }
      vTaskDelay(pdMS_TO_TICKS(200));continue;
    }
    if(!wasOnline) {
      wasOnline=true;
      Serial.printf("[WIFI] Connected: %s channel=%d RSSI=%d\n",
        WiFi.localIP().toString().c_str(),WiFi.channel(),WiFi.RSSI());
    }
    if(!clockStarted) {
      configTime(0,0,"pool.ntp.org","time.google.com");
      clockStarted=true;
      Serial.println("[TIME] Synchronizing clock for verified HTTPS...");
    }
    if(!clockValid()) {vTaskDelay(pdMS_TO_TICKS(400));continue;}
    if(!checkedOTAOnConnect || wantOTA || millis()-lastOtaCheck>=OTA_CHECK_MS) {
      checkedOTAOnConnect=true;wantOTA=false;lastOtaCheck=millis();
      checkGitHubOTA(); // Immediately after Wi-Fi+time, before first API POST.
    }
    if(pendingCount()>0 && (int32_t)(millis()-nextPost)>=0) {
      bool ok=sendFirstEvent();
      nextPost=millis()+(ok?150UL:POST_RETRY_MS);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
void setup() {
  Serial.begin(115200);
  delay(900); // USB CDC enumeration only
  Serial.printf("\n=== SMART BUTTON V2 %s / ESP32-C3 ===\n",FW_VERSION);
  pinMode(PIN_LED_R,OUTPUT); pinMode(PIN_LED_B,OUTPUT);
  rgb(false,false);
  pinMode(PIN_BUTTON,INPUT); pinMode(PIN_PLUG,INPUT); pinMode(PIN_BAT_ADC,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC,ADC_11db);
  bootMs=millis();
  buttonLastRaw=buttonStable=(digitalRead(PIN_BUTTON)==HIGH);
  SPI.begin(PIN_SPI_SCK,PIN_SPI_MISO,PIN_SPI_MOSI,PIN_RFID_SS);
  rfid.PCD_Init();
  Serial.printf("[RFID] MFRC522 version=0x%02X\n",rfid.PCD_ReadRegister(MFRC522::VersionReg));
  queueLock=xSemaphoreCreateMutex();
  if(!queueLock) {Serial.println("[FATAL] Could not create queue mutex");while(true)delay(1000);}
  bootId=esp_random();
  loadQueue();loadConfig();
  WiFi.onEvent(onWiFiEvent);
  enqueueEvent(0); // DEVICE_ON test/production event, stored before Wi-Fi/OTA.
  if(cfgSsid.startsWith("YOUR_") || cfgUrl.indexOf("YOUR_PROJECT_REF")>=0 ||
     cfgKey.indexOf("YOUR_PUBLISHABLE_KEY")>=0 || cfgToken.startsWith("YOUR_")) {
    Serial.println("[SETUP] Fill placeholder credentials or configure through hotspot.");
    startPortal();
  } else startWiFi();
  if(xTaskCreate(networkWorker,"sb2-net",12288,nullptr,1,nullptr)!=pdPASS)
    Serial.println("[FATAL] Network task could not start");
  Serial.println("[HELP] Send 'u' to check GitHub OTA now, 'p' for Wi-Fi setup portal");
}
void loop() {
  updateLED();
  if(portalMode) { dns.processNextRequest();web.handleClient(); }
  else {checkButton();checkRFID();}
  if(millis()-lastTelemetry>=1000) {
    lastTelemetry=millis(); uint16_t mv=readBatteryMv();
    Serial.printf("[BATTERY] %.3f V ~%d%% plugged=%s queue=%u Wi-Fi=%d\n",
      mv/1000.0f,batteryPercent(mv),isPlugged()?"YES":"NO",pendingCount(),(int)WiFi.status());
  }
  while(Serial.available()) {
    char c=(char)Serial.read();
    if(c=='u' || c=='U') wantOTA=true;
    if(c=='p' || c=='P') wantPortal=true;
  }
  vTaskDelay(pdMS_TO_TICKS(5));
}

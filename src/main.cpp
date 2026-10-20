#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

// ---- 液晶 (ST7789 240x320、TFT-VUMeterTester-240-320 で動作確認した設定を S3 のピンに置き換え) ----
// GND = GND, VCC = 3.3V, SDO(MISO) は未接続。S3 の SPI2 (FSPI) の標準ピンに合わせてある
#define TFT_SCK  12 // SCL
#define TFT_MOSI 11 // SDA
#define TFT_MISO -1
#define TFT_CS   10
#define TFT_DC   9
#define TFT_RST  8
#define TFT_BL   7

#define TFT_ROTATION 1 // 横置き 320x240。上下が逆なら 3

// ---- MQTT (仕様は WorkingTimer の MQTT_TOPICS.md が唯一の正) ----
#define MQTT_HOST      "192.168.11.32"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "infopanel" // 後ろに MAC の末尾 4 桁を付けて使う (例 infopanel-A1B2)。WorkingTimer と ID が重ならないように

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *panel = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false /* IPS: 色反転していたため false */, 240, 320);
// 画面全体のキャンバス (320x240x2 = 150KB、PSRAM に取る) に描いてから一度に送る
Arduino_Canvas *cv = new Arduino_Canvas(320, 240, panel);

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

// ---- PC の状態 (info/pc/<PC>/status・comfy) ----
struct PcInfo
{
  const char *name;
  char status[10]; // online / offline / sleep / lost、未受信は空
  char comfy[10];  // off / idle / run / error / down / stalled、未受信は空
  int step, steps, q;
};
PcInfo pcs[] = {{"PC1"}, {"PC2"}, {"PC3"}};

const char *MQTT_SUBS[] = {"info/pc/+/status", "info/pc/+/comfy"};

// ---- 色 ----
uint16_t c565(uint32_t rgb) { return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F); }
const uint32_t C_BG = 0x000000, C_TEXT = 0xE0E0E0, C_DIM = 0x606060, C_CYAN = 0x00C8FF,
               C_GREEN = 0x30D060, C_YELLOW = 0xF0C020, C_RED = 0xF04040;

// 文字は画面の内側 (x, y >= 0) にだけ書く。Arduino_GFX の文字描画は上端・左端のはみ出しを切り取らず、
// キャンバスの外のメモリを壊すため (WorkingTimer で再起動ループの原因になった)
void text(int x, int y, int size, uint32_t rgb, const char *s)
{
  if (x < 0 || y < 0)
    return;
  cv->setTextSize(size);
  cv->setTextColor(c565(rgb));
  cv->setCursor(x, y);
  cv->print(s);
}

PcInfo *findPc(const char *name, size_t len)
{
  for (PcInfo &pc : pcs)
    if (strlen(pc.name) == len && strncmp(pc.name, name, len) == 0)
      return &pc;
  return nullptr;
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  // info/pc/<PC>/<kind>
  const char *p = topic + strlen("info/pc/");
  const char *slash = strchr(p, '/');
  if (strncmp(topic, "info/pc/", 8) != 0 || !slash)
    return;
  PcInfo *pc = findPc(p, slash - p);
  if (!pc)
    return;
  const char *kind = slash + 1;
  if (strcmp(kind, "status") == 0)
  {
    size_t n = min((size_t)length, sizeof(pc->status) - 1);
    memcpy(pc->status, payload, n);
    pc->status[n] = 0;
  }
  else if (strcmp(kind, "comfy") == 0)
  {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length))
      return;
    strlcpy(pc->comfy, doc["st"] | "", sizeof(pc->comfy));
    pc->step = doc["step"] | -1;
    pc->steps = doc["steps"] | -1;
    pc->q = doc["q"] | -1;
  }
}

// つながるまで 5 秒から 30 秒まで間隔を延ばしながら試す
void mqttUpdate()
{
  static uint32_t lastTry = 0, retryMs = 0;
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (!mqtt.connected())
  {
    if (retryMs && millis() - lastTry < retryMs)
      return;
    lastTry = millis();
    static char clientId[24] = "";
    if (!clientId[0])
    {
      uint64_t mac = ESP.getEfuseMac(); // 下位バイトから MAC の 1 バイト目
      snprintf(clientId, sizeof(clientId), MQTT_CLIENT_ID "-%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    }
    Serial.printf("mqtt: connecting to %s:%d as %s ...\n", MQTT_HOST, MQTT_PORT, clientId);
    if (!mqtt.connect(clientId))
    {
      retryMs = retryMs ? min(retryMs * 2, (uint32_t)30000) : 5000;
      Serial.printf("mqtt: connect failed (state %d), retry in %lus\n", mqtt.state(), (unsigned long)(retryMs / 1000));
      return;
    }
    retryMs = 5000;
    Serial.println("mqtt: connected");
    for (const char *t : MQTT_SUBS)
      Serial.printf("mqtt: subscribe %s %s\n", t, mqtt.subscribe(t) ? "ok" : "FAILED");
  }
  mqtt.loop();
}

uint32_t statusRGB(const char *st)
{
  if (!st[0])
    return C_DIM;
  if (!strcmp(st, "online") || !strcmp(st, "idle") || !strcmp(st, "run"))
    return C_GREEN;
  if (!strcmp(st, "lost") || !strcmp(st, "error") || !strcmp(st, "down") || !strcmp(st, "stalled"))
    return C_RED;
  if (!strcmp(st, "sleep"))
    return C_YELLOW;
  return C_DIM; // offline / off
}

// 仮の 1 ページ: 時計・日付・PC の状態・接続情報
void renderFrame()
{
  cv->fillScreen(c565(C_BG));
  char buf[48];

  // 上段: タイトルと接続状態
  text(8, 6, 2, C_CYAN, "INFO PANEL");
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  text(236, 10, 1, wifiOk ? C_GREEN : C_DIM, "WIFI");
  text(272, 10, 1, mqtt.connected() ? C_GREEN : C_DIM, "MQTT");
  cv->drawFastHLine(0, 28, 320, c565(C_DIM));

  // 時計
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  bool synced = t.tm_year >= (2024 - 1900);
  if (synced)
  {
    strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    text(40, 42, 5, C_TEXT, buf); // 1 文字 30x40、8 文字で幅 240
    static const char *WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, WDAY[t.tm_wday]);
    text(76, 90, 2, C_DIM, buf);
  }
  else
    text(64, 56, 2, C_DIM, wifiOk ? "WAITING FOR NTP" : "CONNECTING WIFI");
  cv->drawFastHLine(0, 116, 320, c565(C_DIM));

  // PC の状態
  int y = 126;
  for (PcInfo &pc : pcs)
  {
    text(8, y, 2, C_TEXT, pc.name);
    text(56, y, 2, statusRGB(pc.status), pc.status[0] ? pc.status : "--");
    const char *cs = pc.comfy[0] ? pc.comfy : "--";
    text(160, y, 2, statusRGB(pc.comfy), cs);
    if (!strcmp(pc.comfy, "run") && pc.steps > 0)
    {
      snprintf(buf, sizeof(buf), "%d/%d", pc.step, pc.steps);
      text(208, y, 2, C_TEXT, buf);
    }
    y += 24;
  }
  cv->drawFastHLine(0, 200, 320, c565(C_DIM));

  // 下段: IP・電波・メモリ
  if (wifiOk)
    snprintf(buf, sizeof(buf), "%s  %ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    snprintf(buf, sizeof(buf), "WIFI: %s", WIFI_SSID);
  text(8, 210, 1, C_DIM, buf);
  snprintf(buf, sizeof(buf), "HEAP %uK  PSRAM %uK", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
  text(8, 224, 1, C_DIM, buf);

  cv->flush();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("psram: %s, size %u\n", psramFound() ? "found" : "NOT FOUND", ESP.getPsramSize());

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  if (!cv->begin(40000000))
    Serial.println("canvas begin failed!");

  mqtt.setBufferSize(1024);
  IPAddress broker;
  broker.fromString(MQTT_HOST);
  mqtt.setServer(broker, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2); // CONNACK などを待つ上限 (秒)
  mqttNet.setTimeout(1);    // TCP 接続を待つ上限 (秒)

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime("JST-9", "ntp.nict.jp", "time.google.com", "pool.ntp.org");

  Serial.printf("heap: free %u, psram free %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop()
{
  mqttUpdate();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 100)
  {
    lastDraw = millis();
    renderFrame();
  }
}

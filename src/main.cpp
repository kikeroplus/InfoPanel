#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdarg.h>
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

#define TFT_ROTATION 3 // 横置き 320x240。上下が逆なら 1

// ---- スイッチ (片側 GND、押すと LOW) とパッシブブザー (PWM で鳴らす) ----
#define SW1_PIN    13
#define SW2_PIN    14
#define SW3_PIN    1 // SW3・SW4 は将来の拡張用 (まだ役割なし)
#define SW4_PIN    2
#define BUZZER_PIN 6

// ブザーとバックライトの PWM。ブザーは LEDC チャンネル 0 (タイマー 0)、バックライトは別のタイマーになる 4
#define BZ_CH 0
#define BL_CH 4

// ---- MQTT (仕様は WorkingTimer の MQTT_TOPICS.md が唯一の正) ----
#define MQTT_HOST      "192.168.11.32"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "infopanel" // 後ろに MAC の末尾 4 桁を付けて使う (例 infopanel-A1B2)。WorkingTimer と ID が重ならないように

#define SCREEN_W 320
#define SCREEN_H 240

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *panel = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false /* IPS: 色反転していたため false */, 240, 320);
// 画面全体のキャンバス (320x240x2 = 150KB、PSRAM に取る) に描いてから一度に送る
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
char clientId[24] = "";

const char *MQTT_SUBS[] = {"info/pc/+/status", "info/pc/+/sys", "info/pc/+/comfy", "doorbell/call/+"};

// ---- 色 (暗い紺の地に、シアンを差し色にする) ----
uint16_t c565(uint32_t rgb) { return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F); }
const uint32_t C_BG = 0x04070C, C_PANEL = 0x0A111C, C_LINE = 0x1C3448, C_SEGOFF = 0x0F1C28,
               C_DIM = 0x5A7084, C_TEXT = 0xD8E6F0, C_CYAN = 0x00D0FF,
               C_GREEN = 0x30E080, C_YELLOW = 0xF0C020, C_RED = 0xFF4050;

// a と b を t (0〜1) で混ぜる
uint32_t mix(uint32_t a, uint32_t b, float t)
{
  t = constrain(t, 0.0f, 1.0f);
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8)
  {
    int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
    out |= (uint32_t)(ca + (cb - ca) * t) << sh;
  }
  return out;
}

// ---- PC の情報 (info/pc/<PC>/status・sys・comfy) ----
struct PcInfo
{
  const char *name;
  const char *nick; // 利用者が付けた愛称 (名前の横に控えめに出す)
  uint32_t rgb;     // グラフやレーダーでこの PC を表す色
  const char *hw;   // CPU / GPU の短い名前
  const char *ip;
  char status[10];  // online / offline / sleep / lost、未受信は空
  bool statusBase;  // 最初の値 (retain) を受け取った。以後の変化だけ知らせる
  int cpu, cpuT, ram, gpu, gpuT;
  float vram, vramTot;
  uint32_t sysAt;   // sys を最後に受け取った millis。0 は未受信
  char comfy[10];   // off / idle / run / error / down / stalled、未受信は空
  bool comfyBase;
  int q, step, steps, el;
  char node[26], err[42];
  // 手元で計算するもの
  float sPerIt;     // 1 ステップにかかる秒 (実行中のノード)。不明は -1
  int rateStep, rateSteps;
  uint32_t rateAt;
  int jobs;         // 今日終わったジョブの数
  uint32_t runMs;   // 今日 run だった時間
};
PcInfo pcs[] = {
    {"PC1", "CONDOR", 0x00D0FF, "ULTRA5 / 5060TI", "192.168.55.4"},
    {"PC2", "FALCON", 0xFF4FD8, "ULTRA7 / 5060TI", "192.168.55.2"},
    {"PC3", "KIWI", 0xFFB020, "13400 / 5060", "192.168.55.3"},
};
const int PC_N = sizeof(pcs) / sizeof(pcs[0]);

void pcReset(PcInfo &pc)
{
  pc.status[0] = pc.comfy[0] = pc.node[0] = pc.err[0] = 0;
  pc.statusBase = pc.comfyBase = false;
  pc.cpu = pc.cpuT = pc.ram = pc.gpu = pc.gpuT = -1;
  pc.vram = pc.vramTot = -1;
  pc.sysAt = 0;
  pc.q = pc.step = pc.steps = pc.el = -1;
  pc.sPerIt = -1;
  pc.rateStep = pc.rateSteps = -1;
  pc.rateAt = 0;
  pc.jobs = 0;
  pc.runMs = 0;
}

// sys は 5 秒ごとに来る。15 秒来なければ古いとみなして表示しない
bool sysFresh(const PcInfo &pc) { return pc.sysAt && millis() - pc.sysAt < 15000; }
bool isRun(const PcInfo &pc) { return !strcmp(pc.comfy, "run"); }

// ---- 履歴 (5 秒ごと、12 分ぶん) ----
#define HIST_N  144
#define HIST_MS 5000
int8_t histGpu[PC_N][HIST_N], histGpuT[PC_N][HIST_N], histRssi[HIST_N];
int histHead = 0; // 次に書く位置 (= いちばん古い値の位置)

void sampleHistory()
{
  for (int i = 0; i < PC_N; i++)
  {
    bool f = sysFresh(pcs[i]);
    histGpu[i][histHead] = f ? pcs[i].gpu : -1;
    histGpuT[i][histHead] = f ? min(pcs[i].gpuT, 127) : -1;
  }
  histRssi[histHead] = WiFi.status() == WL_CONNECTED ? -WiFi.RSSI() : -1; // 正の数で持つ
  histHead = (histHead + 1) % HIST_N;
}

// ---- 時刻 ----
bool timeSynced(struct tm &t)
{
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t.tm_year >= (2024 - 1900);
}

void nowHHMM(char *buf)
{
  struct tm t;
  if (timeSynced(t))
    strftime(buf, 6, "%H:%M", &t);
  else
    strcpy(buf, "--:--");
}

// 秒を 3m12s / 1h23m の形に
void fmtDur(char *buf, size_t n, uint32_t sec)
{
  if (sec < 3600)
    snprintf(buf, n, "%um%02us", (unsigned)(sec / 60), (unsigned)(sec % 60));
  else
    snprintf(buf, n, "%uh%02um", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60));
}

// ---- 出来事の記録 (新しいものが [0]) ----
struct Event
{
  char at[6];
  int pc; // -1 は PC 以外
  uint32_t rgb;
  char msg[40];
};
#define EVENT_N 17
Event events[EVENT_N];
int eventN = 0;
uint32_t eventAt = 0; // 最後に記録した millis (画面の「新着」表示用)

void addEvent(int pc, uint32_t rgb, const char *fmt, ...)
{
  memmove(events + 1, events, (EVENT_N - 1) * sizeof(Event));
  Event &e = events[0];
  nowHHMM(e.at);
  e.pc = pc;
  e.rgb = rgb;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  eventN = min(eventN + 1, EVENT_N);
  eventAt = millis();
  Serial.printf("event: %s %s\n", pc >= 0 ? pcs[pc].name : "-", e.msg);
}

// ---- ブザー: 音の並びを loop() の中で順に鳴らす (待たない) ----
struct Note
{
  uint16_t freq, ms; // freq 0 は休み
};
const Note SND_CLICK[] = {{3200, 12}};
const Note SND_BOOT[] = {{1047, 60}, {1568, 60}, {2093, 100}};
const Note SND_DONE[] = {{1568, 90}, {0, 30}, {2093, 160}};
const Note SND_ALERT[] = {{2600, 150}, {0, 80}, {2600, 150}, {0, 80}, {2600, 150}};
const Note SND_CALL[] = {{1760, 220}, {1397, 380}};
const Note SND_VOL[] = {{1200, 40}, {0, 30}, {800, 60}};
#define PLAY(s) play(s, sizeof(s) / sizeof(s[0]))

// 音量はデューティ比 (10 ビット、512 が 50% で最大) で変える。OFF は鳴らさない
const uint16_t VOL_DUTY[] = {512, 80, 20, 0};
const char *VOL_NAMES[] = {"HIGH", "MID", "LOW", "OFF"};
#define VOL_OFF 3
int volLevel = 0;
bool muted() { return volLevel == VOL_OFF; }

const Note *seq = nullptr;
int seqN = 0, seqI = 0;
uint32_t seqAt = 0;

void startNote()
{
  if (seq[seqI].freq)
  {
    ledcWriteTone(BZ_CH, seq[seqI].freq);
    ledcWrite(BZ_CH, VOL_DUTY[volLevel]);
  }
  else
    ledcWrite(BZ_CH, 0);
  seqAt = millis();
}

void play(const Note *s, int n, bool force = false)
{
  if (muted() && !force)
    return;
  seq = s;
  seqN = n;
  seqI = 0;
  startNote();
}

void soundUpdate()
{
  if (!seq || millis() - seqAt < seq[seqI].ms)
    return;
  if (++seqI < seqN)
    startNote();
  else
  {
    ledcWrite(BZ_CH, 0);
    seq = nullptr;
  }
}

// ---- スイッチ: 同じ状態が 25ms 続いたら確定 (チャタリング除去)。0.7 秒押し続けると長押し ----
enum BtnEv
{
  EV_NONE,
  EV_SHORT, // 離したとき (長押しにならなかった)
  EV_LONG,  // 押し続けて 0.7 秒たったとき
};
struct Button
{
  uint8_t pin;
  bool down, raw, longFired;
  uint32_t changedAt, downAt;
};
Button sw1 = {SW1_PIN}, sw2 = {SW2_PIN}, sw3 = {SW3_PIN}, sw4 = {SW4_PIN};

BtnEv buttonUpdate(Button &b)
{
  uint32_t now = millis();
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw)
  {
    b.raw = raw;
    b.changedAt = now;
  }
  else if (raw != b.down && now - b.changedAt >= 25)
  {
    b.down = raw;
    if (raw)
    {
      b.downAt = now;
      b.longFired = false;
    }
    else if (!b.longFired)
      return EV_SHORT;
  }
  if (b.down && !b.longFired && now - b.downAt >= 700)
  {
    b.longFired = true;
    return EV_LONG;
  }
  return EV_NONE;
}

// ---- バックライト ----
const uint8_t BL_LEVELS[] = {255, 90, 20};
const char *BL_NAMES[] = {"HIGH", "MID", "LOW"};
int blLevel = 0;
void applyBacklight() { ledcWrite(BL_CH, BL_LEVELS[blLevel]); }

// ---- アラート (画面中央に重ねて出す。新しいものが来たら置き換え、古いものは出来事の記録に残る) ----
enum AlertKind
{
  AK_LOST,  // PC の予期せぬ切断
  AK_COMFY, // ComfyUI の error / down / stalled
  AK_CALL,  // 呼び出しベル
};
struct Alert
{
  bool active;
  int pc;
  AlertKind kind;
  uint32_t rgb;
  char title[24], detail[44], at[6];
  uint32_t shownAt;
} alert;

void raiseAlert(int pc, AlertKind kind, uint32_t rgb, const char *title, const char *detail)
{
  alert.active = true;
  alert.pc = pc;
  alert.kind = kind;
  alert.rgb = rgb;
  strlcpy(alert.title, title, sizeof(alert.title));
  strlcpy(alert.detail, detail, sizeof(alert.detail));
  nowHHMM(alert.at);
  alert.shownAt = millis();
  if (kind == AK_CALL)
    PLAY(SND_CALL);
  else
    PLAY(SND_ALERT);
}

// 正常に戻ったら、その PC のアラートを閉じる
void retractAlert(int pc, AlertKind kind)
{
  if (alert.active && alert.pc == pc && alert.kind == kind)
    alert.active = false;
}

// ---- 画面下に数秒だけ出す通知 ----
char toastMsg[40] = "";
uint32_t toastRgb = 0, toastUntil = 0;

void toast(uint32_t rgb, const char *msg)
{
  strlcpy(toastMsg, msg, sizeof(toastMsg));
  toastRgb = rgb;
  toastUntil = millis() + 4000;
}

// ---- MQTT ----
uint32_t msgTotal = 0, msgPerMin = 0;

int findPc(const char *name, size_t len)
{
  for (int i = 0; i < PC_N; i++)
    if (strlen(pcs[i].name) == len && strncmp(pcs[i].name, name, len) == 0)
      return i;
  return -1;
}

void upper(char *dst, const char *src, size_t n)
{
  size_t i = 0;
  for (; src[i] && i < n - 1; i++)
    dst[i] = toupper((unsigned char)src[i]);
  dst[i] = 0;
}

void jobDone(int i, int el)
{
  PcInfo &pc = pcs[i];
  pc.jobs++;
  char dur[12] = "", msg[40];
  if (el >= 0)
    fmtDur(dur, sizeof(dur), el);
  addEvent(i, C_GREEN, "JOB DONE %s", dur);
  snprintf(msg, sizeof(msg), "%s JOB DONE %s", pc.name, dur);
  toast(pc.rgb, msg);
  PLAY(SND_DONE);
}

void handleStatus(int i, const byte *payload, unsigned int length)
{
  PcInfo &pc = pcs[i];
  char s[10];
  size_t n = min((size_t)length, sizeof(s) - 1);
  memcpy(s, payload, n);
  s[n] = 0;
  if (strcmp(s, pc.status) != 0 && pc.statusBase)
  {
    // 予期せぬ切断 (online → lost) だけアラートにする。offline・sleep は記録だけ
    if (!strcmp(pc.status, "online") && !strcmp(s, "lost"))
    {
      raiseAlert(i, AK_LOST, C_RED, (String(pc.name) + " LOST").c_str(), "UNEXPECTED DISCONNECT");
      addEvent(i, C_RED, "LOST");
    }
    else
    {
      char up[10];
      upper(up, s[0] ? s : "no data", sizeof(up));
      addEvent(i, !strcmp(s, "online") ? C_GREEN : !strcmp(s, "sleep") ? C_YELLOW : C_DIM, "%s", up);
    }
  }
  strcpy(pc.status, s);
  pc.statusBase = true;
  if (!strcmp(s, "online"))
    retractAlert(i, AK_LOST);
}

void handleSys(int i, const byte *payload, unsigned int length)
{
  JsonDocument doc;
  if (deserializeJson(doc, payload, length))
    return;
  PcInfo &pc = pcs[i];
  pc.cpu = doc["cpu"] | -1;
  pc.cpuT = doc["cpu_t"] | -1;
  pc.ram = doc["ram"] | -1;
  pc.gpu = doc["gpu"] | -1;
  pc.gpuT = doc["gpu_t"] | -1;
  pc.vram = doc["vram"] | -1.0f;
  pc.vramTot = doc["vram_tot"] | -1.0f;
  pc.sysAt = millis();
}

void handleComfy(int i, const byte *payload, unsigned int length)
{
  PcInfo &pc = pcs[i];
  char st[10] = "";
  int el = -1, step = -1, steps = -1;
  JsonDocument doc;
  if (length > 0) // 空は retain の削除 (データなしに戻す)
  {
    if (deserializeJson(doc, payload, length))
      return;
    strlcpy(st, doc["st"] | "", sizeof(st));
    el = doc["el"] | -1;
    step = doc["step"] | -1;
    steps = doc["steps"] | -1;
  }

  if (pc.comfyBase)
  {
    bool wasRun = isRun(pc), nowRun = !strcmp(st, "run");
    if (wasRun && !strcmp(st, "idle"))
      jobDone(i, pc.el);
    else if (wasRun && nowRun && el >= 0 && pc.el >= 0 && el + 2 < pc.el)
      jobDone(i, pc.el); // 待ち行列の次のジョブに移った (経過秒が戻った)
    if (!wasRun && nowRun)
      addEvent(i, C_CYAN, "JOB START");

    bool bad = !strcmp(st, "error") || !strcmp(st, "down") || !strcmp(st, "stalled");
    if (bad && strcmp(st, pc.comfy) != 0)
    {
      char title[24], detail[44], up[10];
      upper(up, st, sizeof(up));
      snprintf(title, sizeof(title), "%s %s", pc.name, up);
      if (!strcmp(st, "error"))
        snprintf(detail, sizeof(detail), "%s %s", (const char *)(doc["node"] | ""), (const char *)(doc["err"] | ""));
      else if (!strcmp(st, "down"))
        strcpy(detail, "COMFYUI NOT RESPONDING");
      else
        snprintf(detail, sizeof(detail), "%s NO PROGRESS", (const char *)(doc["node"] | ""));
      raiseAlert(i, AK_COMFY, C_RED, title, detail);
      addEvent(i, C_RED, "COMFY %s", up);
    }
  }

  if (!strcmp(st, "idle") || !strcmp(st, "run"))
    retractAlert(i, AK_COMFY);

  // ステップの進み具合から 1 ステップの秒数を出す (同じノードの間だけ)
  uint32_t now = millis();
  if (!strcmp(st, "run") && steps > 0 && step >= 0)
  {
    if (steps == pc.rateSteps && pc.rateStep >= 0 && step > pc.rateStep)
    {
      float s = (now - pc.rateAt) / 1000.0f / (step - pc.rateStep);
      pc.sPerIt = pc.sPerIt < 0 ? s : pc.sPerIt * 0.6f + s * 0.4f;
      pc.rateStep = step;
      pc.rateAt = now;
    }
    else if (steps != pc.rateSteps || step < pc.rateStep)
    {
      pc.sPerIt = -1;
      pc.rateStep = step;
      pc.rateSteps = steps;
      pc.rateAt = now;
    }
  }
  else
  {
    pc.sPerIt = -1;
    pc.rateStep = pc.rateSteps = -1;
  }

  strcpy(pc.comfy, st);
  pc.el = el;
  pc.step = step;
  pc.steps = steps;
  pc.q = length > 0 ? (doc["q"] | -1) : -1;
  strlcpy(pc.node, length > 0 ? (const char *)(doc["node"] | "") : "", sizeof(pc.node));
  strlcpy(pc.err, length > 0 ? (const char *)(doc["err"] | "") : "", sizeof(pc.err));
  pc.comfyBase = true;
}

void handleDoorbell(const char *dev, const byte *payload, unsigned int length)
{
  char p[8];
  size_t n = min((size_t)length, sizeof(p) - 1);
  memcpy(p, payload, n);
  p[n] = 0;
  char up[16];
  upper(up, dev, sizeof(up));
  if (!strcmp(p, "ON"))
  {
    raiseAlert(-1, AK_CALL, C_YELLOW, "CALL", up);
    addEvent(-1, C_YELLOW, "CALL %s", up);
  }
  else if (!strcmp(p, "OFF"))
    retractAlert(-1, AK_CALL);
  else if (!strcmp(p, "TIMER"))
    addEvent(-1, C_CYAN, "TIMER %s", up);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  msgTotal++;
  if (strncmp(topic, "doorbell/call/", 14) == 0)
  {
    handleDoorbell(topic + 14, payload, length);
    return;
  }
  // info/pc/<PC>/<kind>
  if (strncmp(topic, "info/pc/", 8) != 0)
    return;
  const char *p = topic + 8;
  const char *slash = strchr(p, '/');
  if (!slash)
    return;
  int i = findPc(p, slash - p);
  if (i < 0)
    return;
  const char *kind = slash + 1;
  if (!strcmp(kind, "status"))
    handleStatus(i, payload, length);
  else if (!strcmp(kind, "sys"))
    handleSys(i, payload, length);
  else if (!strcmp(kind, "comfy"))
    handleComfy(i, payload, length);
}

// つながるまで 5 秒から 30 秒まで間隔を延ばしながら試す
void mqttUpdate()
{
  static uint32_t lastTry = 0, retryMs = 0;
  static bool wasConnected = false;
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (!mqtt.connected())
  {
    if (wasConnected)
    {
      wasConnected = false;
      addEvent(-1, C_RED, "MQTT DISCONNECTED");
    }
    if (retryMs && millis() - lastTry < retryMs)
      return;
    lastTry = millis();
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
    wasConnected = true;
    addEvent(-1, C_GREEN, "MQTT CONNECTED");
    for (const char *t : MQTT_SUBS)
      Serial.printf("mqtt: subscribe %s %s\n", t, mqtt.subscribe(t) ? "ok" : "FAILED");
  }
  mqtt.loop();
}

// ---- 描画の部品 ----
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

int textW(int size, const char *s) { return strlen(s) * 6 * size; }
void textR(int xr, int y, int size, uint32_t rgb, const char *s) { text(xr - textW(size, s), y, size, rgb, s); }
void textC(int xc, int y, int size, uint32_t rgb, const char *s) { text(xc - textW(size, s) / 2, y, size, rgb, s); }

// 角に差し色のかぎを付けた枠
void frame(int x, int y, int w, int h, const char *label = nullptr, uint32_t accent = C_CYAN)
{
  cv->fillRect(x, y, w, h, c565(C_PANEL));
  cv->drawRect(x, y, w, h, c565(C_LINE));
  uint16_t a = c565(accent);
  const int k = 6;
  cv->drawFastHLine(x, y, k, a);
  cv->drawFastVLine(x, y, k, a);
  cv->drawFastHLine(x + w - k, y, k, a);
  cv->drawFastVLine(x + w - 1, y, k, a);
  cv->drawFastHLine(x, y + h - 1, k, a);
  cv->drawFastVLine(x, y + h - k, k, a);
  cv->drawFastHLine(x + w - k, y + h - 1, k, a);
  cv->drawFastVLine(x + w - 1, y + h - k, k, a);
  if (label)
    textR(x + w - 6, y + 4, 1, C_DIM, label);
}

// 区切りのある横棒 (1 区切り 4px + すき間 1px)
void segBar(int x, int y, int w, int h, float pct, uint32_t rgb)
{
  int n = w / 5;
  int lit = pct < 0 ? 0 : (int)(n * min(pct, 100.0f) / 100.0f + 0.5f);
  for (int i = 0; i < n; i++)
    cv->fillRect(x + i * 5, y, 4, h, c565(i < lit ? rgb : C_SEGOFF));
}

// 負荷・温度の色: warn 以上で黄、crit 以上で赤
// 愛称の色: その PC の色を灰色に寄せて控えめにする
uint32_t nickRGB(const PcInfo &pc) { return mix(pc.rgb, C_DIM, 0.6f); }

uint32_t levelRGB(int v, int warn, int crit, uint32_t normal = C_CYAN)
{
  if (v < 0)
    return C_DIM;
  return v >= crit ? C_RED : v >= warn ? C_YELLOW : normal;
}

uint32_t statusRGB(const char *st)
{
  if (!st[0])
    return C_DIM;
  if (!strcmp(st, "online") || !strcmp(st, "idle"))
    return C_GREEN;
  if (!strcmp(st, "run"))
    return C_CYAN;
  if (!strcmp(st, "lost") || !strcmp(st, "error") || !strcmp(st, "down") || !strcmp(st, "stalled"))
    return C_RED;
  if (!strcmp(st, "sleep"))
    return C_YELLOW;
  return C_DIM; // offline / off
}

// 7 セグの 1 区画。中心線から離れた行ほど両端を短くして、先を尖らせる
void segH(int x0, int x1, int yc, int t, uint16_t c)
{
  for (int r = 0; r < t; r++)
  {
    int in = abs(2 * r - (t - 1)) / 2;
    cv->drawFastHLine(x0 + in, yc - t / 2 + r, x1 - x0 + 1 - 2 * in, c);
  }
}
void segV(int xc, int y0, int y1, int t, uint16_t c)
{
  for (int r = 0; r < t; r++)
  {
    int in = abs(2 * r - (t - 1)) / 2;
    cv->drawFastVLine(xc - t / 2 + r, y0 + in, y1 - y0 + 1 - 2 * in, c);
  }
}

// 7 セグメントの数字 (細い線で先を尖らせる)。消えている区画も薄く描く
void seg7(int x, int y, int w, int h, int t, int d, uint32_t on)
{
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  uint8_t m = d >= 0 && d <= 9 ? SEG[d] : 0;
  uint16_t con = c565(on), coff = c565(C_SEGOFF);
  // 各区画の中心線と、区画どうしのすき間
  int xl = x + t / 2, xr = x + w - 1 - t / 2;
  int yt = y + t / 2, ym = y + h / 2, yb = y + h - 1 - t / 2;
  const int g = 2;
  segH(xl + g, xr - g, yt, t, m & 0x01 ? con : coff); // a
  segV(xr, yt + g, ym - g, t, m & 0x02 ? con : coff); // b
  segV(xr, ym + g, yb - g, t, m & 0x04 ? con : coff); // c
  segH(xl + g, xr - g, yb, t, m & 0x08 ? con : coff); // d
  segV(xl, ym + g, yb - g, t, m & 0x10 ? con : coff); // e
  segV(xl, yt + g, ym - g, t, m & 0x20 ? con : coff); // f
  segH(xl + g, xr - g, ym, t, m & 0x40 ? con : coff); // g
}

// ---- ページ ----
enum Page
{
  PG_OVERVIEW,
  PG_SYSTEMS,
  PG_RENDER,
  PG_TRENDS,
  PG_SCOPE,
  PG_EVENTS,
  PG_DEVICE,
  PG_GRAY, // 液晶の焼き付き (残像) を抜くための全面グレー。上の帯も出さない
  PG_N
};
const char *PAGE_NAMES[PG_N] = {"OVERVIEW", "SYSTEMS", "RENDER", "TRENDS", "SCOPE", "EVENTS", "DEVICE", "GRAY"};
int page = PG_OVERVIEW;

// 上の帯: ページ番号と名前、ページの位置、接続、時刻
void drawHeader()
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d", page + 1);
  text(8, 8, 1, C_CYAN, buf);
  text(26, 4, 2, C_TEXT, PAGE_NAMES[page]);
  int tw = 26 + textW(2, PAGE_NAMES[page]) + 6;

  for (int i = 0; i < PG_N; i++)
    cv->fillRect(168 + i * 8, i == page ? 8 : 9, 5, i == page ? 5 : 3, c565(i == page ? C_CYAN : C_LINE));

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  uint32_t link = wifiOk && mqtt.connected() ? C_GREEN : wifiOk ? C_YELLOW : C_RED;
  cv->fillRect(238, 8, 5, 5, c565(link));
  if (muted())
    text(248, 7, 1, C_RED, "MUTE");
  nowHHMM(buf);
  textR(314, 7, 1, C_TEXT, buf);

  cv->drawFastHLine(0, 22, SCREEN_W, c565(C_LINE));
  cv->fillRect(0, 21, tw, 2, c565(C_CYAN));
}

// 01 OVERVIEW: 大きな時計と 3 台の要約
void drawOverview()
{
  char buf[48];
  struct tm t;
  bool synced = timeSynced(t);

  // 時計 HH:MM (7 セグ) と秒
  int hh = synced ? t.tm_hour : -1, mm = synced ? t.tm_min : -1, ss = synced ? t.tm_sec : -1;
  const int Y = 30, DW = 34, DH = 58, T = 4;
  seg7(8, Y, DW, DH, T, hh < 0 ? -1 : hh / 10, C_TEXT);
  seg7(50, Y, DW, DH, T, hh < 0 ? -1 : hh % 10, C_TEXT);
  bool colon = !synced || millis() % 1000 < 500;
  uint16_t cc = c565(colon ? C_CYAN : C_SEGOFF);
  cv->fillRect(93, Y + 17, 4, 4, cc);
  cv->fillRect(93, Y + 37, 4, 4, cc);
  seg7(106, Y, DW, DH, T, mm < 0 ? -1 : mm / 10, C_TEXT);
  seg7(148, Y, DW, DH, T, mm < 0 ? -1 : mm % 10, C_TEXT);
  seg7(192, Y + 30, 16, 28, 2, ss < 0 ? -1 : ss / 10, C_CYAN);
  seg7(212, Y + 30, 16, 28, 2, ss < 0 ? -1 : ss % 10, C_CYAN);

  // 日付・曜日・起動からの時間
  if (synced)
  {
    static const char *WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    snprintf(buf, sizeof(buf), "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    text(248, 32, 2, C_TEXT, buf);
    text(248, 52, 2, t.tm_wday == 0 ? C_RED : t.tm_wday == 6 ? C_CYAN : C_DIM, WDAY[t.tm_wday]);
    snprintf(buf, sizeof(buf), "%d", t.tm_year + 1900);
    text(248, 72, 1, C_DIM, buf);
  }
  else
  {
    text(248, 36, 1, C_DIM, WiFi.status() == WL_CONNECTED ? "SYNC NTP" : "WIFI...");
  }
  char up[12];
  fmtDur(up, sizeof(up), millis() / 1000);
  snprintf(buf, sizeof(buf), "UP %s", up);
  text(248, 82, 1, C_DIM, buf);

  // 3 台のカード
  for (int i = 0; i < PC_N; i++)
  {
    PcInfo &pc = pcs[i];
    int x = 6 + i * 104, y = 98, w = 100, h = 102;
    frame(x, y, w, h, nullptr, pc.rgb);
    text(x + 6, y + 6, 2, pc.rgb, pc.name);
    text(x + 44, y + 13, 1, nickRGB(pc), pc.nick);
    uint32_t sc = statusRGB(pc.status);
    cv->fillCircle(x + w - 12, y + 13, 4, c565(sc));
    if (!strcmp(pc.status, "online"))
      cv->drawCircle(x + w - 12, y + 13, 5 + (millis() / 150) % 4, c565(mix(C_PANEL, sc, 0.5f)));

    // ComfyUI の状態と進み具合
    char up[10];
    upper(up, pc.comfy[0] ? pc.comfy : "--", sizeof(up));
    text(x + 6, y + 28, 1, statusRGB(pc.comfy), up);
    if (isRun(pc) && pc.steps > 0)
    {
      snprintf(buf, sizeof(buf), "%d/%d", pc.step, pc.steps);
      textR(x + w - 6, y + 28, 1, C_TEXT, buf);
    }
    else if (pc.q > 0)
    {
      snprintf(buf, sizeof(buf), "Q%d", pc.q);
      textR(x + w - 6, y + 28, 1, C_TEXT, buf);
    }
    float pct = isRun(pc) && pc.steps > 0 ? 100.0f * pc.step / pc.steps : 0;
    segBar(x + 6, y + 40, 90, 6, pct, statusRGB(pc.comfy));

    // 負荷
    bool f = sysFresh(pc);
    struct
    {
      const char *label;
      int v, temp;
    } rows[] = {{"GPU", pc.gpu, pc.gpuT}, {"CPU", pc.cpu, pc.cpuT}, {"RAM", pc.ram, -1}};
    for (int r = 0; r < 3; r++)
    {
      int ry = y + 54 + r * 12;
      text(x + 6, ry, 1, C_DIM, rows[r].label);
      if (!f || rows[r].v < 0)
      {
        textR(x + w - 6, ry, 1, C_DIM, "--");
        continue;
      }
      if (rows[r].temp >= 0)
        snprintf(buf, sizeof(buf), "%3d%% %2dC", rows[r].v, rows[r].temp);
      else
        snprintf(buf, sizeof(buf), "%3d%%", rows[r].v);
      uint32_t c = rows[r].temp >= 0 ? levelRGB(rows[r].temp, 75, 85, C_TEXT) : levelRGB(rows[r].v, 80, 92, C_TEXT);
      textR(x + w - 6, ry, 1, c, buf);
    }
    text(x + 6, y + 90, 1, C_DIM, "VRAM");
    if (f && pc.vram >= 0 && pc.vramTot > 0)
      snprintf(buf, sizeof(buf), "%.1f/%.0fG", pc.vram, pc.vramTot);
    else
      strcpy(buf, "--");
    textR(x + w - 6, y + 90, 1, f ? C_TEXT : C_DIM, buf);
  }

  // 下段: 最新の出来事と今日のまとめ
  cv->drawFastHLine(0, 206, SCREEN_W, c565(C_LINE));
  if (eventN)
  {
    const Event &e = events[0];
    text(8, 212, 1, C_DIM, e.at);
    int x = 44;
    if (e.pc >= 0)
    {
      text(x, 212, 1, pcs[e.pc].rgb, pcs[e.pc].name);
      x += 24;
    }
    text(x, 212, 1, millis() - eventAt < 10000 ? e.rgb : C_TEXT, e.msg);
  }
  int active = 0, jobs = 0, gsum = 0, gn = 0;
  for (PcInfo &pc : pcs)
  {
    active += isRun(pc);
    jobs += pc.jobs;
    if (sysFresh(pc) && pc.gpu >= 0)
    {
      gsum += pc.gpu;
      gn++;
    }
  }
  snprintf(buf, sizeof(buf), "RENDER %d/%d   JOBS TODAY %d", active, PC_N, jobs);
  text(8, 226, 1, C_DIM, buf);
  if (gn)
  {
    snprintf(buf, sizeof(buf), "GPU AVG %d%%", gsum / gn);
    textR(314, 226, 1, C_DIM, buf);
  }
}

// 02 SYSTEMS: 3 台の負荷と温度
void drawSystems()
{
  char buf[32];
  for (int i = 0; i < PC_N; i++)
  {
    PcInfo &pc = pcs[i];
    int x = 6, y = 28 + i * 70, w = 308, h = 66;
    frame(x, y, w, h, nullptr, pc.rgb);
    text(x + 8, y + 6, 2, pc.rgb, pc.name);
    text(x + 46, y + 13, 1, nickRGB(pc), pc.nick);
    char up[10];
    upper(up, pc.status[0] ? pc.status : "--", sizeof(up));
    text(x + 8, y + 26, 1, statusRGB(pc.status), up);
    text(x + 8, y + 40, 1, C_DIM, pc.hw);
    text(x + 8, y + 52, 1, C_DIM, pc.ip + 7); // 192.168. を省く

    bool f = sysFresh(pc);
    struct
    {
      const char *label;
      float pct;
      uint32_t rgb;
    } rows[] = {
        {"CPU", (float)pc.cpu, levelRGB(pc.cpu, 80, 95)},
        {"GPU", (float)pc.gpu, levelRGB(pc.gpu, 101, 101)},
        {"RAM", (float)pc.ram, levelRGB(pc.ram, 80, 92)},
        {"VRAM", pc.vramTot > 0 ? 100 * pc.vram / pc.vramTot : -1, levelRGB(pc.vramTot > 0 ? 100 * pc.vram / pc.vramTot : -1, 85, 95)},
    };
    for (int r = 0; r < 4; r++)
    {
      int ry = y + 7 + r * 14;
      text(x + 104, ry, 1, C_DIM, rows[r].label);
      segBar(x + 134, ry, 100, 7, f ? rows[r].pct : -1, rows[r].rgb);
      if (!f || rows[r].pct < 0)
      {
        textR(x + w - 8, ry, 1, C_DIM, "--");
        continue;
      }
      uint32_t c = C_TEXT;
      if (r == 0 || r == 1)
      {
        int temp = r == 0 ? pc.cpuT : pc.gpuT;
        if (temp >= 0)
          snprintf(buf, sizeof(buf), "%3d%% %2dC", (int)rows[r].pct, temp);
        else
          snprintf(buf, sizeof(buf), "%3d%%", (int)rows[r].pct);
        c = levelRGB(temp, 75, 85, C_TEXT);
      }
      else if (r == 2)
        snprintf(buf, sizeof(buf), "%3d%%", (int)rows[r].pct);
      else
        snprintf(buf, sizeof(buf), "%.1f/%.0fG", pc.vram, pc.vramTot);
      textR(x + w - 8, ry, 1, c, buf);
    }
  }
}

// 03 RENDER: ComfyUI の詳しい状態
void drawRender()
{
  char buf[48];
  for (int i = 0; i < PC_N; i++)
  {
    PcInfo &pc = pcs[i];
    int x = 6, y = 28 + i * 70, w = 308, h = 66;
    uint32_t sc = statusRGB(pc.comfy);
    frame(x, y, w, h, nullptr, pc.comfy[0] ? sc : C_LINE);
    text(x + 8, y + 6, 2, pc.rgb, pc.name);
    text(x + 46, y + 13, 1, nickRGB(pc), pc.nick);

    // 状態の札 (塗りつぶしに暗い文字)
    char up[10];
    upper(up, pc.comfy[0] ? pc.comfy : "no data", sizeof(up));
    int bw = textW(1, up) + 8;
    const int bx = x + 90;
    cv->fillRect(bx, y + 6, bw, 12, c565(sc));
    text(bx + 4, y + 8, 1, C_BG, up);
    // 実行中・エラーのノード名
    if (pc.node[0] && (isRun(pc) || !strcmp(pc.comfy, "error") || !strcmp(pc.comfy, "stalled")))
    {
      int nx = bx + bw + 6, room = (x + w - 8 - 30 - nx) / 6; // 右端の Q の分を空ける
      snprintf(buf, sizeof(buf), "%.*s", constrain(room, 0, 24), pc.node);
      text(nx, y + 8, 1, C_DIM, buf);
    }
    if (pc.q >= 0)
    {
      snprintf(buf, sizeof(buf), "Q%d", pc.q);
      textR(x + w - 8, y + 8, 1, pc.q > 0 ? C_TEXT : C_DIM, buf);
    }

    // 進み具合
    float pct = isRun(pc) && pc.steps > 0 ? 100.0f * pc.step / pc.steps : 0;
    segBar(x + 8, y + 26, 240, 9, pct, isRun(pc) ? pc.rgb : C_DIM);
    if (isRun(pc) && pc.steps > 0)
    {
      snprintf(buf, sizeof(buf), "%d/%d", pc.step, pc.steps);
      textR(x + w - 8, y + 27, 1, C_TEXT, buf);
    }

    // 3 行目: 経過・速さ・残り、またはエラーの内容
    buf[0] = 0;
    uint32_t c3 = C_TEXT;
    if (isRun(pc))
    {
      char el[12] = "--", rate[12] = "--", eta[12] = "--";
      if (pc.el >= 0)
        snprintf(el, sizeof(el), "%d:%02d", pc.el / 60, pc.el % 60);
      if (pc.sPerIt > 0)
      {
        if (pc.sPerIt >= 1)
          snprintf(rate, sizeof(rate), "%.2fs/it", pc.sPerIt);
        else
          snprintf(rate, sizeof(rate), "%.2fit/s", 1 / pc.sPerIt);
        int remain = (int)((pc.steps - pc.step) * pc.sPerIt);
        snprintf(eta, sizeof(eta), "%d:%02d", remain / 60, remain % 60);
      }
      snprintf(buf, sizeof(buf), "EL %s  %s  NODE ETA %s", el, rate, eta);
    }
    else if (!strcmp(pc.comfy, "error"))
    {
      snprintf(buf, sizeof(buf), "%.40s", pc.err[0] ? pc.err : "ERROR");
      c3 = C_RED;
    }
    else if (!strcmp(pc.comfy, "down"))
    {
      strcpy(buf, "COMFYUI NOT RESPONDING");
      c3 = C_RED;
    }
    else if (!strcmp(pc.comfy, "stalled"))
    {
      snprintf(buf, sizeof(buf), "NO PROGRESS  EL %d:%02d", max(pc.el, 0) / 60, max(pc.el, 0) % 60);
      c3 = C_RED;
    }
    else if (!strcmp(pc.comfy, "idle"))
    {
      strcpy(buf, "READY");
      c3 = C_DIM;
    }
    else if (!strcmp(pc.comfy, "off"))
    {
      strcpy(buf, "COMFYUI NOT RUNNING");
      c3 = C_DIM;
    }
    text(x + 8, y + 41, 1, c3, buf);

    // 4 行目: 今日の成績
    char run[12];
    fmtDur(run, sizeof(run), pc.runMs / 1000);
    snprintf(buf, sizeof(buf), "TODAY  %d JOBS  RUN %s", pc.jobs, run);
    text(x + 8, y + 53, 1, C_DIM, buf);
  }
}

// 折れ線グラフ (3 台を重ねる)。古い値が左
void drawChart(int x, int y, int w, int h, int8_t hist[][HIST_N], int lo, int hi, const char *label, const char *unit)
{
  frame(x, y, w, h, label);
  int gx = x + 28, gy = y + 16, gw = w - 36, gh = h - 26;
  char buf[8];
  for (int k = 0; k <= 4; k++)
  {
    int ly = gy + gh - gh * k / 4;
    for (int px = gx; px < gx + gw; px += 4)
      cv->drawPixel(px, ly, c565(C_LINE));
    if (k % 2 == 0)
    {
      snprintf(buf, sizeof(buf), "%d", lo + (hi - lo) * k / 4);
      textR(gx - 4, ly - 3, 1, C_DIM, buf);
    }
  }
  text(x + 6, y + 4, 1, C_DIM, unit);
  for (int i = 0; i < PC_N; i++)
  {
    uint16_t c = c565(pcs[i].rgb);
    int px0 = -1, py0 = -1;
    for (int k = 0; k < HIST_N; k++)
    {
      int v = hist[i][(histHead + k) % HIST_N];
      if (v < 0)
      {
        px0 = -1;
        continue;
      }
      int px = gx + gw * k / (HIST_N - 1);
      int py = gy + gh - gh * (constrain(v, lo, hi) - lo) / (hi - lo);
      if (px0 >= 0)
        cv->drawLine(px0, py0, px, py, c);
      px0 = px;
      py0 = py;
    }
    // 最新の値を右端に
    int v = hist[i][(histHead + HIST_N - 1) % HIST_N];
    if (v >= 0)
    {
      int py = gy + gh - gh * (constrain(v, lo, hi) - lo) / (hi - lo);
      cv->fillCircle(gx + gw, py, 2, c);
    }
  }
}

// 04 TRENDS: GPU 使用率と温度の 12 分間
void drawTrends()
{
  drawChart(6, 28, 308, 102, histGpu, 0, 100, "GPU LOAD  12 MIN", "%");
  drawChart(6, 134, 308, 102, histGpuT, 30, 90, "GPU TEMP  12 MIN", "C");
  // 凡例
  for (int i = 0; i < PC_N; i++)
  {
    cv->fillRect(40 + i * 40, 32, 8, 3, c565(pcs[i].rgb));
    text(50 + i * 40, 30, 1, pcs[i].rgb, pcs[i].name);
  }
}

// 05 SCOPE: レーダー。中心から遠いほど GPU が忙しい
void drawScope()
{
  const int CX = 114, CY = 132, R = 100;
  const float TAU = 6.2831853f;
  uint16_t line = c565(C_LINE);
  for (int k = 1; k <= 4; k++)
    cv->drawCircle(CX, CY, R * k / 4, line);
  cv->drawFastHLine(CX - R, CY, 2 * R, line);
  cv->drawFastVLine(CX, CY - R, 2 * R, line);
  for (int k = 0; k < 36; k++) // 10 度ごとの目盛り
  {
    float a = TAU * k / 36;
    int l = k % 3 == 0 ? 6 : 3;
    cv->drawLine(CX + cosf(a) * (R - l), CY + sinf(a) * (R - l), CX + cosf(a) * R, CY + sinf(a) * R, line);
  }

  // 掃引線と、後ろに残る薄い光
  float sweep = TAU * (millis() % 4000) / 4000.0f;
  for (int k = 9; k >= 0; k--)
  {
    float a = sweep - k * 0.05f;
    uint32_t c = mix(C_BG, C_CYAN, k == 0 ? 1.0f : 0.5f * (1 - k / 10.0f));
    cv->drawLine(CX, CY, CX + cosf(a) * R, CY + sinf(a) * R, c565(c));
  }

  // 3 台の光点 (120 度ずつ)
  for (int i = 0; i < PC_N; i++)
  {
    PcInfo &pc = pcs[i];
    float a = TAU * (0.75f + i / 3.0f);
    while (a >= TAU)
      a -= TAU;
    bool f = sysFresh(pc) && pc.gpu >= 0;
    float r = f ? 10 + (R - 18) * pc.gpu / 100.0f : 10;
    int bx = CX + cosf(a) * r, by = CY + sinf(a) * r;
    float since = sweep - a; // 掃引線が通ってからの角度
    if (since < 0)
      since += TAU;
    float glow = 1 - since / TAU;
    if (f)
    {
      cv->fillCircle(bx, by, 4, c565(mix(C_BG, pc.rgb, 0.3f + 0.7f * glow)));
      if (glow > 0.85f)
        cv->drawCircle(bx, by, 4 + (1 - glow) * 40, c565(mix(C_BG, pc.rgb, glow)));
    }
    else
      cv->drawCircle(bx, by, 4, c565(C_DIM));
    int lx = CX + cosf(a) * (R + 0) - 9, ly = CY + sinf(a) * (R - 12) - 4;
    text(constrain(lx, 2, 300), constrain(ly, 26, 230), 1, f ? pc.rgb : C_DIM, pc.name);
  }

  // 右の数値
  char buf[24];
  int x = 226;
  frame(x, 28, 88, 208, "GPU");
  int active = 0, gsum = 0, gn = 0;
  for (int i = 0; i < PC_N; i++)
  {
    PcInfo &pc = pcs[i];
    int y = 42 + i * 44;
    text(x + 8, y, 1, pc.rgb, pc.name);
    textR(x + 80, y, 1, nickRGB(pc), pc.nick);
    bool f = sysFresh(pc) && pc.gpu >= 0;
    if (f)
    {
      snprintf(buf, sizeof(buf), "%d%%", pc.gpu);
      gsum += pc.gpu;
      gn++;
    }
    else
      strcpy(buf, "--");
    textR(x + 80, y + 10, 2, f ? C_TEXT : C_DIM, buf);
    if (f && pc.gpuT >= 0)
    {
      snprintf(buf, sizeof(buf), "%dC", pc.gpuT);
      text(x + 8, y + 18, 1, levelRGB(pc.gpuT, 75, 85, C_DIM), buf);
    }
    active += isRun(pc);
  }
  cv->drawFastHLine(x + 6, 172, 76, line);
  text(x + 8, 180, 1, C_DIM, "AVG");
  snprintf(buf, sizeof(buf), gn ? "%d%%" : "--", gn ? gsum / gn : 0);
  textR(x + 80, 178, 2, C_CYAN, buf);
  text(x + 8, 204, 1, C_DIM, "RENDER");
  snprintf(buf, sizeof(buf), "%d/%d", active, PC_N);
  textR(x + 80, 202, 2, active ? C_CYAN : C_DIM, buf);
}

// 06 EVENTS: 出来事の記録
void drawEvents()
{
  if (!eventN)
  {
    textC(SCREEN_W / 2, 120, 1, C_DIM, "NO EVENTS YET");
    return;
  }
  for (int k = 0; k < eventN; k++)
  {
    const Event &e = events[k];
    int y = 30 + k * 12;
    if (k == 0 && millis() - eventAt < 10000)
      cv->fillRect(0, y - 2, SCREEN_W, 12, c565(C_PANEL));
    text(8, y, 1, C_DIM, e.at);
    cv->fillRect(42, y, 2, 7, c565(e.rgb));
    text(50, y, 1, e.pc >= 0 ? pcs[e.pc].rgb : C_DIM, e.pc >= 0 ? pcs[e.pc].name : "SYS");
    text(80, y, 1, k == 0 ? e.rgb : mix(e.rgb, C_DIM, 0.4f), e.msg);
  }
}

// 07 DEVICE: この端末の状態とスイッチの使い方
void drawDevice()
{
  char buf[48];
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  frame(6, 28, 152, 100, "NETWORK");
  text(14, 40, 1, C_DIM, "SSID");
  snprintf(buf, sizeof(buf), "%.14s", WIFI_SSID);
  textR(150, 40, 1, C_TEXT, buf);
  text(14, 52, 1, C_DIM, "IP");
  textR(150, 52, 1, wifiOk ? C_TEXT : C_DIM, wifiOk ? WiFi.localIP().toString().c_str() : "--");
  text(14, 64, 1, C_DIM, "RSSI");
  int rssi = wifiOk ? WiFi.RSSI() : 0;
  int bars = !wifiOk ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int b = 0; b < 4; b++)
    cv->fillRect(48 + b * 5, 71 - (b + 1) * 2, 3, (b + 1) * 2, c565(b < bars ? C_CYAN : C_SEGOFF));
  snprintf(buf, sizeof(buf), wifiOk ? "%ddBm" : "--", rssi);
  textR(150, 64, 1, C_TEXT, buf);
  // 電波の強さの 12 分間 (-30〜-90dBm)
  int gx = 14, gy = 80, gw = 136, gh = 40;
  cv->drawRect(gx, gy, gw, gh, c565(C_LINE));
  int px0 = -1, py0 = -1;
  for (int k = 0; k < HIST_N; k++)
  {
    int v = histRssi[(histHead + k) % HIST_N];
    if (v < 0)
    {
      px0 = -1;
      continue;
    }
    int px = gx + 1 + (gw - 3) * k / (HIST_N - 1);
    int py = gy + 1 + (gh - 3) * (constrain(v, 30, 90) - 30) / 60;
    if (px0 >= 0)
      cv->drawLine(px0, py0, px, py, c565(C_CYAN));
    px0 = px;
    py0 = py;
  }

  frame(162, 28, 152, 100, "MQTT");
  text(170, 40, 1, C_DIM, "BROKER");
  textR(306, 40, 1, C_TEXT, MQTT_HOST);
  text(170, 52, 1, C_DIM, "STATE");
  textR(306, 52, 1, mqtt.connected() ? C_GREEN : C_RED, mqtt.connected() ? "CONNECTED" : "OFFLINE");
  text(170, 64, 1, C_DIM, "ID");
  textR(306, 64, 1, C_TEXT, clientId[0] ? clientId : "--");
  text(170, 76, 1, C_DIM, "MSGS");
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)msgTotal);
  textR(306, 76, 1, C_TEXT, buf);
  text(170, 88, 1, C_DIM, "PER MIN");
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)msgPerMin);
  textR(306, 88, 1, C_TEXT, buf);

  frame(6, 132, 152, 104, "SYSTEM");
  char up[12];
  fmtDur(up, sizeof(up), millis() / 1000);
  struct
  {
    const char *label;
    String value;
  } sys[] = {
      {"UPTIME", up},
      {"HEAP", String(ESP.getFreeHeap() / 1024) + "K"},
      {"PSRAM", String(ESP.getFreePsram() / 1024) + "K"},
      {"CPU", String(ESP.getCpuFreqMHz()) + "MHZ"},
      {"BUILD", __DATE__},
  };
  for (int r = 0; r < 5; r++)
  {
    text(14, 144 + r * 12, 1, C_DIM, sys[r].label);
    textR(150, 144 + r * 12, 1, C_TEXT, sys[r].value.c_str());
  }

  frame(162, 132, 152, 104, "CONTROLS");
  const char *help[][2] = {
      {"SW1", "NEXT PAGE"},
      {"SW2", "PREV PAGE"},
      {"SW1 HOLD", "BRIGHTNESS"},
      {"SW2 HOLD", "VOLUME"},
  };
  for (int r = 0; r < 4; r++)
  {
    text(170, 144 + r * 12, 1, C_CYAN, help[r][0]);
    textR(306, 144 + r * 12, 1, C_TEXT, help[r][1]);
  }
  // 押している間だけ光る (配線の確認用)
  const Button *sws[] = {&sw1, &sw2, &sw3, &sw4};
  for (int k = 0; k < 4; k++)
  {
    char name[4] = {'S', 'W', (char)('1' + k), 0};
    text(170 + k * 34, 192, 1, sws[k]->down ? C_GREEN : C_DIM, name);
  }
  text(170, 204, 1, C_DIM, "LIGHT");
  textR(306, 204, 1, C_TEXT, BL_NAMES[blLevel]);
  text(170, 216, 1, C_DIM, "SOUND");
  textR(306, 216, 1, muted() ? C_RED : C_GREEN, VOL_NAMES[volLevel]);
}

// アラートを画面中央に重ねる。枠が点滅する
void drawAlert()
{
  int x = 18, y = 62, w = 284, h = 116;
  bool blink = (millis() - alert.shownAt) % 800 < 400;
  cv->fillRect(x, y, w, h, c565(mix(C_BG, alert.rgb, 0.12f)));
  uint16_t b = c565(blink ? alert.rgb : mix(C_BG, alert.rgb, 0.4f));
  cv->drawRect(x, y, w, h, b);
  cv->drawRect(x + 1, y + 1, w - 2, h - 2, b);
  // 斜めの縞 (警告らしく)
  for (int k = 0; k < w - 4; k += 12)
    cv->fillTriangle(x + 2 + k, y + 2, x + 8 + k, y + 2, x + 2 + k, y + 8, c565(mix(C_BG, alert.rgb, 0.5f)));
  textC(SCREEN_W / 2, y + 22, 3, alert.rgb, alert.title);
  textC(SCREEN_W / 2, y + 58, 1, C_TEXT, alert.detail);
  char buf[24];
  snprintf(buf, sizeof(buf), "AT %s", alert.at);
  textC(SCREEN_W / 2, y + 74, 2, C_TEXT, buf);
  textC(SCREEN_W / 2, y + h - 14, 1, C_DIM, "PRESS ANY SWITCH");
}

void drawToast()
{
  int y = 212, h = 24;
  cv->fillRect(0, y, SCREEN_W, h, c565(C_PANEL));
  cv->drawFastHLine(0, y, SCREEN_W, c565(toastRgb));
  cv->fillRect(0, y, 4, h, c565(toastRgb));
  text(12, y + 8, 1, C_TEXT, toastMsg);
}

// 残像抜きの色。AUTO はグレー → 白 → 黒を 10 秒ずつ繰り返す
const char *FILL_NAMES[] = {"GRAY", "WHITE", "BLACK", "AUTO"};
const uint32_t FILL_RGB[] = {0x808080, 0xFFFFFF, 0x000000};
const int FILL_N = sizeof(FILL_NAMES) / sizeof(FILL_NAMES[0]);
int fillMode = 0;
uint32_t fillChangedAt = 0;

void renderFrame()
{
  if (page == PG_GRAY)
  {
    cv->fillScreen(c565(fillMode < 3 ? FILL_RGB[fillMode] : FILL_RGB[millis() / 10000 % 3]));
    // 切り替えた直後の 2 秒だけ名前を出す
    if (fillChangedAt && millis() - fillChangedAt < 2000)
    {
      cv->fillRect(110, 106, 100, 28, c565(C_BG));
      textC(SCREEN_W / 2, 112, 2, C_CYAN, FILL_NAMES[fillMode]);
    }
    if (alert.active)
      drawAlert();
    cv->flush();
    return;
  }
  cv->fillScreen(c565(C_BG));
  drawHeader();
  switch (page)
  {
  case PG_OVERVIEW: drawOverview(); break;
  case PG_SYSTEMS: drawSystems(); break;
  case PG_RENDER: drawRender(); break;
  case PG_TRENDS: drawTrends(); break;
  case PG_SCOPE: drawScope(); break;
  case PG_EVENTS: drawEvents(); break;
  case PG_DEVICE: drawDevice(); break;
  }
  if (millis() < toastUntil && !alert.active)
    drawToast();
  if (alert.active)
    drawAlert();
  cv->flush();
}

// ---- スイッチの操作 ----
void handleButtons()
{
  BtnEv e1 = buttonUpdate(sw1), e2 = buttonUpdate(sw2), e3 = buttonUpdate(sw3), e4 = buttonUpdate(sw4);
  if (e1 == EV_NONE && e2 == EV_NONE && e3 == EV_NONE && e4 == EV_NONE)
    return;
  // アラート中はどれかを押すと閉じるだけ
  if (alert.active)
  {
    if (e1 == EV_SHORT || e2 == EV_SHORT || e3 == EV_SHORT || e4 == EV_SHORT)
    {
      alert.active = false;
      PLAY(SND_CLICK);
    }
    return;
  }
  if (e1 == EV_SHORT)
    page = (page + 1) % PG_N;
  if (e2 == EV_SHORT)
    page = (page + PG_N - 1) % PG_N;
  if (e1 == EV_SHORT || e2 == EV_SHORT)
    PLAY(SND_CLICK);
  if (e1 == EV_LONG && page == PG_GRAY)
  {
    fillMode = (fillMode + 1) % FILL_N;
    fillChangedAt = millis();
    PLAY(SND_CLICK);
  }
  else if (e1 == EV_LONG)
  {
    blLevel = (blLevel + 1) % (sizeof(BL_LEVELS) / sizeof(BL_LEVELS[0]));
    applyBacklight();
    toast(C_CYAN, (String("BRIGHTNESS ") + BL_NAMES[blLevel]).c_str());
    PLAY(SND_CLICK);
  }
  if (e2 == EV_LONG)
  {
    volLevel = (volLevel + 1) % (sizeof(VOL_DUTY) / sizeof(VOL_DUTY[0]));
    toast(muted() ? C_RED : C_GREEN, (String("SOUND ") + VOL_NAMES[volLevel]).c_str());
    if (muted())
    {
      ledcWrite(BZ_CH, 0);
      seq = nullptr;
    }
    else
      PLAY(SND_VOL); // 新しい音量で試し鳴らし
  }
  // SW3・SW4 はまだ役割がないので、押されたことだけ知らせる
  BtnEv ex[] = {e3, e4};
  for (int k = 0; k < 2; k++)
    if (ex[k] != EV_NONE)
    {
      char msg[32];
      snprintf(msg, sizeof(msg), "SW%d %s (NOT ASSIGNED)", 3 + k, ex[k] == EV_LONG ? "HOLD" : "PRESS");
      Serial.printf("sw%d: %s\n", 3 + k, ex[k] == EV_LONG ? "hold" : "press");
      toast(C_DIM, msg);
      PLAY(SND_CLICK);
    }
}

// 1 秒ごと: 今日の稼働時間を足す、日付が変わったら今日の成績を消す、1 分ごとの受信数
void tick()
{
  static uint32_t last = 0, minuteAt = 0, minuteMsgs = 0;
  static int day = -1;
  uint32_t now = millis();
  if (now - last < 1000)
    return;
  uint32_t dt = last ? now - last : 0;
  last = now;
  for (PcInfo &pc : pcs)
    if (isRun(pc))
      pc.runMs += dt;

  struct tm t;
  if (timeSynced(t))
  {
    if (day >= 0 && t.tm_yday != day)
    {
      for (PcInfo &pc : pcs)
      {
        pc.jobs = 0;
        pc.runMs = 0;
      }
      addEvent(-1, C_DIM, "NEW DAY");
    }
    day = t.tm_yday;
  }

  if (now - minuteAt >= 60000)
  {
    msgPerMin = msgTotal - minuteMsgs;
    minuteMsgs = msgTotal;
    minuteAt = now;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("psram: %s, size %u\n", psramFound() ? "found" : "NOT FOUND", ESP.getPsramSize());

  ledcSetup(BZ_CH, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BZ_CH);
  ledcWrite(BZ_CH, 0);
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(TFT_BL, BL_CH);
  applyBacklight();
  if (!cv->begin(40000000))
    Serial.println("canvas begin failed!");

  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  pinMode(SW3_PIN, INPUT_PULLUP);
  pinMode(SW4_PIN, INPUT_PULLUP);

  for (PcInfo &pc : pcs)
    pcReset(pc);
  memset(histGpu, -1, sizeof(histGpu));
  memset(histGpuT, -1, sizeof(histGpuT));
  memset(histRssi, -1, sizeof(histRssi));

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

  addEvent(-1, C_CYAN, "BOOT");
  PLAY(SND_BOOT);
  Serial.printf("heap: free %u, psram free %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop()
{
  mqttUpdate();
  handleButtons();
  soundUpdate();
  tick();

  static uint32_t lastSample = 0;
  if (millis() - lastSample >= HIST_MS)
  {
    lastSample = millis();
    sampleHistory();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 66) // 約 15 フレーム/秒 (レーダーを滑らかに)
  {
    lastDraw = millis();
    renderFrame();
  }
}

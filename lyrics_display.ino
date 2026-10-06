/*
  ESP32 + SSD1306 OLED synced lyrics display

  The phone plays the song and fetches the lyrics. This ESP32 serves the page,
  receives line-by-line updates over a WebSocket, and animates them on the OLED.

  Libraries (Library Manager, or GitHub zip for the ESP32Async ones):
    - U8g2
    - ESPAsyncWebServer (ESP32Async)
    - AsyncTCP (ESP32Async)

  Wiring (default I2C): OLED VCC->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22

  Protocol (text over WebSocket, phone -> ESP32):
    T|title                      song title shown at the top
    L|idx|elapsedMs|durMs|text   a lyric line is active (empty text = instrumental)
    P                            paused
    S                            stopped
*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <U8g2lib.h>
#include "index_html.h"

// ---------------- CONFIG ----------------
const char* WIFI_SSID = "hello";      // phone hotspot (2.4 GHz)
const char* WIFI_PASS = "123456789";
const char* AP_SSID   = "LyricsESP";              // fallback if hotspot not found
const char* AP_PASS   = "lyrics1234";

// SSD1306 128x64 I2C. For an SH1106 board use U8G2_SH1106_128X64_NONAME_F_HW_I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

#define FONT_BIG    u8g2_font_6x12_tr
#define FONT_SMALL  u8g2_font_5x8_tr
#define FONT_TITLE  u8g2_font_5x7_tr

// Must stay above the first function: the Arduino IDE auto-generates function
// prototypes there, and they need to know this type.
struct Wrapped {
  String l[4];
  uint8_t n = 0;
  bool small = false;
};

// ---------------- NETWORK ----------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
String ipStr = "";
bool apMode = false;

// WebSocket callbacks run in another task, so messages go through a small
// ring buffer and are handled in loop().
#define QN   8
#define QLEN 224
char msgQ[QN][QLEN];
volatile uint8_t qHead = 0, qTail = 0;
portMUX_TYPE qMux = portMUX_INITIALIZER_UNLOCKED;

void qPush(const char* s, size_t n) {
  portENTER_CRITICAL(&qMux);
  uint8_t next = (qHead + 1) % QN;
  if (next != qTail) {
    size_t m = n < QLEN - 1 ? n : QLEN - 1;
    memcpy(msgQ[qHead], s, m);
    msgQ[qHead][m] = 0;
    qHead = next;
  }
  portEXIT_CRITICAL(&qMux);
}

bool qPop(char* out) {
  bool ok = false;
  portENTER_CRITICAL(&qMux);
  if (qTail != qHead) {
    memcpy(out, msgQ[qTail], QLEN);
    qTail = (qTail + 1) % QN;
    ok = true;
  }
  portEXIT_CRITICAL(&qMux);
  return ok;
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type,
               void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
      qPush((const char*)data, len);
  } else if (type == WS_EVT_DISCONNECT) {
    qPush("S", 1);
  }
}

// ---------------- LYRIC STATE ----------------
enum Mode { IDLE, PLAYING, PAUSED };
Mode mode = IDLE;
String title = "";

Wrapped curW, prevW;
bool curBlank = true, prevBlank = false;
int curIdx = -2;
uint32_t lineAnchor = 0, lineDur = 1000, pausedEl = 0, slideStart = 0;

// Word-wrap into at most maxRows rows. Returns false if it did not fit.
bool wrapWith(const String& s, const uint8_t* font, int maxRows, Wrapped& out) {
  u8g2.setFont(font);
  out.n = 0;
  for (auto& x : out.l) x = "";
  String line = "";
  int i = 0, len = s.length();
  while (i < len) {
    int j = s.indexOf(' ', i);
    if (j < 0) j = len;
    String word = s.substring(i, j);
    i = j + 1;
    if (!word.length()) continue;
    String test = line.length() ? line + " " + word : word;
    if (u8g2.getStrWidth(test.c_str()) <= 126) {
      line = test;
    } else {
      if (line.length()) {
        if (out.n >= maxRows) return false;
        out.l[out.n++] = line;
      }
      line = word;
      while (u8g2.getStrWidth(line.c_str()) > 126) {  // very long word: hard split
        int k = line.length() - 1;
        while (k > 1 && u8g2.getStrWidth(line.substring(0, k).c_str()) > 126) k--;
        if (out.n >= maxRows) return false;
        out.l[out.n++] = line.substring(0, k);
        line = line.substring(k);
      }
    }
  }
  if (line.length()) {
    if (out.n >= maxRows) return false;
    out.l[out.n++] = line;
  }
  return true;
}

void wrapLine(const String& s, Wrapped& out) {
  out.small = false;
  if (wrapWith(s, FONT_BIG, 3, out)) return;
  out.small = true;
  wrapWith(s, FONT_SMALL, 4, out);  // if still too long, the tail is cut
}

void handleMsg(char* m) {
  char c = m[0];
  if (c == 'T' && m[1] == '|') {
    title = String(m + 2);
  } else if (c == 'P') {
    if (mode == PLAYING) {
      pausedEl = millis() - lineAnchor;
      mode = PAUSED;
    }
  } else if (c == 'S') {
    mode = IDLE;
    curIdx = -2;
  } else if (c == 'L' && m[1] == '|') {
    char* f[4];
    f[0] = m + 2;
    for (int k = 1; k < 4; k++) {
      char* q = strchr(f[k - 1], '|');
      if (!q) return;
      *q = 0;
      f[k] = q + 1;
    }
    int idx = atoi(f[0]);
    uint32_t el = strtoul(f[1], nullptr, 10);
    uint32_t dur = strtoul(f[2], nullptr, 10);
    const char* text = f[3];

    bool fresh = (mode == IDLE);
    if (idx != curIdx || fresh) {          // new line -> start slide animation
      if (fresh) { prevW = Wrapped(); prevBlank = false; }
      else       { prevW = curW; prevBlank = curBlank; }
      curBlank = (text[0] == 0);
      if (!curBlank) wrapLine(String(text), curW);
      curIdx = idx;
      slideStart = millis();
    }
    lineAnchor = millis() - el;            // same line again = just re-sync the clock
    lineDur = dur ? dur : 1;
    mode = PLAYING;
  }
}

// ---------------- DRAWING ----------------
float eq[6] = {1, 1, 1, 1, 1, 1}, eqT[6] = {1, 1, 1, 1, 1, 1};
uint32_t eqNext = 0;

void updateEq() {
  uint32_t now = millis();
  if (now > eqNext) {
    eqNext = now + 90;
    for (int i = 0; i < 6; i++) eqT[i] = (mode == PLAYING) ? random(2, 12) : 1;
  }
  for (int i = 0; i < 6; i++) eq[i] += (eqT[i] - eq[i]) * 0.45f;
}

void drawEq(int x0) {
  for (int i = 0; i < 6; i++) {
    int h = (int)eq[i];
    if (h < 1) h = 1;
    u8g2.drawBox(x0 + i * 4, 11 - h, 3, h);
  }
}

void drawTitle() {
  u8g2.setFont(FONT_TITLE);
  u8g2.setClipWindow(0, 0, 100, 11);
  int tw = u8g2.getStrWidth(title.c_str());
  if (tw <= 100) {
    u8g2.drawStr(0, 2, title.c_str());
  } else {                                  // marquee
    int span = tw + 24;
    int off = (millis() / 35) % span;
    u8g2.drawStr(-off, 2, title.c_str());
    u8g2.drawStr(span - off, 2, title.c_str());
  }
  u8g2.setMaxClipWindow();
}

void drawWrapped(const Wrapped& w, int dy) {
  u8g2.setFont(w.small ? FONT_SMALL : FONT_BIG);
  int rh = w.small ? 10 : 13;
  int top = 14 + (44 - w.n * rh) / 2 + dy;
  for (int i = 0; i < w.n; i++) {
    int tw = u8g2.getStrWidth(w.l[i].c_str());
    u8g2.drawStr((128 - tw) / 2, top + i * rh, w.l[i].c_str());
  }
}

void drawDots(int dy) {                      // instrumental / no lyrics
  for (int k = 0; k < 3; k++) {
    int y = 36 + (int)(sinf(millis() / 180.0f + k * 0.9f) * 5) + dy;
    u8g2.drawDisc(54 + k * 10, y, 2);
  }
}

void drawBlock(bool blank, const Wrapped& w, int dy) {
  if (blank) drawDots(dy);
  else drawWrapped(w, dy);
}

void drawIdle() {
  u8g2.setFont(FONT_BIG);
  const char* a = "ESP32 LYRICS";
  u8g2.drawStr((128 - u8g2.getStrWidth(a)) / 2, 3, a);
  u8g2.setFont(FONT_TITLE);
  String url = "http://" + ipStr;
  u8g2.drawStr((128 - u8g2.getStrWidth(url.c_str())) / 2, 20, url.c_str());
  const char* b = apMode ? "WiFi: LyricsESP" : "Open this on your phone";
  u8g2.drawStr((128 - u8g2.getStrWidth(b)) / 2, 31, b);
  for (int i = 0; i < 8; i++) {              // idle wave
    int h = 3 + (int)((sinf(millis() / 300.0f + i * 0.8f) + 1) * 5);
    u8g2.drawBox(30 + i * 8, 63 - h, 5, h);
  }
}

void render() {
  u8g2.clearBuffer();
  updateEq();
  if (mode == IDLE) {
    drawIdle();
  } else {
    drawTitle();
    drawEq(104);
    u8g2.drawHLine(0, 12, 128);

    float t = (millis() - slideStart) / 280.0f;
    if (t > 1) t = 1;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease-out
    u8g2.setClipWindow(0, 14, 128, 59);
    if (t < 1) drawBlock(prevBlank, prevW, -(int)(e * 30));  // old line exits up
    drawBlock(curBlank, curW, (int)((1 - e) * 30));          // new line enters from below
    u8g2.setMaxClipWindow();

    // progress through the current line
    uint32_t el = (mode == PLAYING) ? millis() - lineAnchor : pausedEl;
    float f = (float)el / (float)lineDur;
    if (f > 1) f = 1;
    for (int x = 0; x < 128; x += 4) u8g2.drawPixel(x, 62);
    u8g2.drawBox(0, 61, (int)(f * 128), 3);
  }
  u8g2.sendBuffer();
}

void showMsg(const char* s) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_BIG);
  u8g2.drawStr(0, 24, s);
  u8g2.sendBuffer();
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  Serial.begin(115200);
  // Wire.begin(SDA, SCL);   // uncomment and edit if your OLED is on other pins
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setFontPosTop();

  showMsg("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    ipStr = WiFi.localIP().toString();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ipStr = WiFi.softAPIP().toString();
    apMode = true;
  }
  WiFi.setSleep(false);                      // lower latency
  Serial.println("Page at http://" + ipStr);
  MDNS.begin("lyrics");                      // http://lyrics.local on some phones

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/html", INDEX_HTML);
  });
  server.begin();
}

void loop() {
  char buf[QLEN];
  while (qPop(buf)) handleMsg(buf);

  static uint32_t last = 0;
  if (millis() - last >= 30) {
    last = millis();
    render();
  }
  ws.cleanupClients();
}

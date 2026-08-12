/*
 * GLM Token Monitor — ESP32-2432S028 (CYD) v14
 * Clean grid layout, consistent spacing, pixel-perfect
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>

#define BG    0x0210
#define CARD  0x2104
#define CARD2 0x3186
#define PLT   0x18E2
#define ACC   0x5DFF
#define GRN   0x4F1F
#define RED   0xF820
#define YEL   0xFF20
#define TXT   0xFFFF
#define DIM   0x9492
#define DIM2  0x4228
#define PUR   0x932F
#define TEAL  0x4DFF
#define GOLD  0xD4A0
#define PINK  0xF812

TFT_eSPI tft = TFT_eSPI();
// ====== RGB LED (3 separate LEDs, active LOW: HIGH=off) ======
// IO4=Red, IO16=Green, IO17=Blue
void ledOff() {
  pinMode(4, OUTPUT);
  pinMode(16, OUTPUT);
  pinMode(17, OUTPUT);
  digitalWrite(4, HIGH);   // Red off
  digitalWrite(16, HIGH);  // Green off
  digitalWrite(17, HIGH);  // Blue off
}
#define W 320
#define H 240

const char* KEY  = "0cb8538cbb1f457389ea5a5939c5f031.C5MOCFbwk5Rr2SoI";
const char* HOST = "api.z.ai";

// ====== State ======
int     tokPct = 0;
unsigned long tokResetMs = 0;
int     mcpPct = 0, mcpRemaining = 0, mcpUsage = 0;
unsigned long mcpResetMs = 0;
char planLevel[16] = "";
unsigned long lastFetch = 0;

#define NHOURS 24
unsigned long hourTokens[NHOURS];
int hourCount = 0;
unsigned long totalTok24 = 0, totalCalls24 = 0;
int peakHourIdx = 0;
unsigned long peakHourTok = 0;
unsigned long avgPerHour = 0;
unsigned long currentHourTok = 0;

#define NMODELS 8
char modelName[NMODELS][20];
unsigned long modelTokens[NMODELS];
int modelCount = 0;

bool chartDirty = true;
String ipAddr = "";

// ====== Helpers ======
uint8_t c565_r(uint16_t c) { return (c >> 8) & 0xF8; }
uint8_t c565_g(uint16_t c) { return ((c >> 3) & 0xFC); }
uint8_t c565_b(uint16_t c) { return (c << 3) & 0xF8; }

void gradientH(int y, int w, uint16_t c1, uint16_t c2, float t) {
  uint8_t r = (uint8_t)(c565_r(c1) * (1-t) + c565_r(c2) * t);
  uint8_t g = (uint8_t)(c565_g(c1) * (1-t) + c565_g(c2) * t);
  uint8_t b = (uint8_t)(c565_b(c1) * (1-t) + c565_b(c2) * t);
  tft.drawFastHLine(0, y, w, tft.color565(r, g, b));
}

String fmtTok(unsigned long v) {
  char b[16];
  if (v >= 1000000UL) sprintf(b, "%.1fM", v / 1000000.0f);
  else if (v >= 1000UL) sprintf(b, "%.1fK", v / 1000.0f);
  else sprintf(b, "%lu", v);
  return String(b);
}

String fmtReset(unsigned long resetMs) {
  if (resetMs == 0) return "--";
  time_t now = time(nullptr);
  long diff = (long)(resetMs / 1000 - now);
  char b[16];
  if (diff <= 0) { strcpy(b, "now"); return String(b); }
  int dd = diff / 86400;
  int hh = (diff % 86400) / 3600;
  int mm = (diff % 3600) / 60;
  if (dd > 0) sprintf(b, "%dd%dh", dd, hh);
  else sprintf(b, "%dh%dm", hh, mm);
  return String(b);
}

// ====== Grid Layout ======
// 3 rows, consistent margins
//
// ROW 0: Header (y=0-22)     height=22
//   gap 2px
// ROW 1: Quota card (y=24-82)  height=58
//   gap 2px
// ROW 2: Chart card (y=84-180) height=96
//   gap 2px
// ROW 3: Footer (y=182-239)   height=58
//
// Columns: 4px margin L/R, card width = 312
// Footer: 4 columns, 78px each, 4px gap = 320

// Chart plot inside card
#define PX 8
#define PY 108
#define PW 304
#define PH 68
#define PB (PY + PH)  // 176

void drawStatic() {
  tft.fillScreen(BG);

  // === Header bar ===
  for (int y = 0; y < 22; y++)
    gradientH(y, W, CARD2, CARD, (float)y / 22.0f);
  tft.drawFastHLine(0, 22, W, ACC);

  // === Quota card ===
  tft.fillRoundRect(4, 24, W - 8, 70, 3, CARD);
  tft.drawRoundRect(4, 24, W - 8, 70, 3, DIM2);
  tft.drawFastVLine(W / 2, 28, 62, DIM2);  // divider

  // === Chart card ===
  tft.fillRoundRect(4, 96, W - 8, 86, 3, CARD);
  tft.drawRoundRect(4, 96, W - 8, 86, 3, DIM2);

  // === Footer ===
  for (int y = 0; y < 58; y++)
    gradientH(182 + y, W, CARD, BG, (float)y / 58.0f);
  tft.drawFastHLine(0, 182, W, ACC);

  // Footer vertical dividers (4 columns @ 80px)
  for (int dx : {80, 160, 240}) {
    tft.drawFastVLine(dx, 184, 52, DIM2);
  }
}

// ====== Fetch ======
String httpsGet(const char* path) {
  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(12000);
  if (!c.connect(HOST, 443)) return "";
  c.print(String("GET ") + path + " HTTP/1.1\r\n"
    "Host: " + String(HOST) + "\r\n"
    "Authorization: Bearer " + String(KEY) + "\r\n"
    "Connection: close\r\n\r\n");
  String body = "";
  bool he = false;
  unsigned long to = millis() + 15000;
  while (c.connected() || c.available()) {
    if (millis() > to) break;
    if (!he) {
      String l = c.readStringUntil('\n');
      if (l == "\r" || l.length() == 0) he = true;
      continue;
    }
    if (c.available()) body += (char)c.read();
  }
  c.stop();
  int b = body.indexOf('{');
  return (b < 0) ? "" : body.substring(b);
}

void fetchQuota() {
  String j = httpsGet("/api/monitor/usage/quota/limit");
  if (!j.length()) return;
  JsonDocument d;
  if (deserializeJson(d, j)) return;
  if (d["code"].as<int>() != 200) return;
  const char* lvl = d["data"]["level"] | "";
  strncpy(planLevel, lvl, 15);
  if (!planLevel[0]) strcpy(planLevel, "?");
  for (JsonVariant l : d["data"]["limits"].as<JsonArray>()) {
    const char* type = l["type"] | "";
    if (strcmp(type, "TOKENS_LIMIT") == 0) {
      tokPct = l["percentage"] | 0;
      tokResetMs = l["nextResetTime"] | 0UL;
    } else if (strcmp(type, "TIME_LIMIT") == 0) {
      mcpPct = l["percentage"] | 0;
      mcpRemaining = l["remaining"] | 0;
      mcpUsage = l["usage"] | 0;
      mcpResetMs = l["nextResetTime"] | 0UL;
    }
  }
}

void fetchUsage() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long t0 = millis();
  while (time(nullptr) < 1700000000) {
    if (millis() - t0 > 8000) return;
    delay(500);
  }
  time_t now = time(nullptr);
  time_t st = now - 86400;
  char ss[24], es[24];
  strftime(ss, 24, "%Y-%m-%d%%20%H:%M:%S", gmtime(&st));
  strftime(es, 24, "%Y-%m-%d%%20%H:%M:%S", gmtime(&now));
  String path = "/api/monitor/usage/model-usage?startTime=" + String(ss) + "&endTime=" + String(es);
  String j = httpsGet(path.c_str());
  if (!j.length()) return;
  JsonDocument d;
  if (deserializeJson(d, j)) return;
  if (d["code"].as<int>() != 200) return;
  JsonObject data = d["data"].as<JsonObject>();
  JsonArray xt = data["x_time"].as<JsonArray>();
  JsonArray tu = data["tokensUsage"].as<JsonArray>();
  JsonArray cc = data["modelCallCount"].as<JsonArray>();
  int total = xt.size();
  if (!total) return;
  int si = max(0, total - NHOURS);
  hourCount = total - si;
  totalTok24 = 0;
  totalCalls24 = 0;
  peakHourTok = 0;
  peakHourIdx = 0;
  for (int i = 0; i < hourCount; i++) {
    int idx = si + i;
    hourTokens[i] = (idx < (int)tu.size()) ? tu[idx].as<unsigned long>() : 0;
    totalTok24 += hourTokens[i];
    totalCalls24 += (idx < (int)cc.size()) ? cc[idx].as<unsigned long>() : 0;
    if (hourTokens[i] > peakHourTok) { peakHourTok = hourTokens[i]; peakHourIdx = i; }
  }
  currentHourTok = (hourCount > 0) ? hourTokens[hourCount - 1] : 0;
  avgPerHour = (hourCount > 0) ? totalTok24 / hourCount : 0;

  JsonArray ml = data["modelDataList"].as<JsonArray>();
  modelCount = min((int)ml.size(), NMODELS);
  for (int m = 0; m < modelCount; m++) {
    strncpy(modelName[m], ml[m]["modelName"] | "?", 19);
    modelName[m][19] = 0;
    JsonArray tt = ml[m]["tokensUsage"].as<JsonArray>();
    unsigned long sum = 0;
    for (int i = si; i < total && i < (int)tt.size(); i++)
      sum += tt[i].as<unsigned long>();
    modelTokens[m] = sum;
  }
  for (int i = 0; i < modelCount - 1; i++)
    for (int j = i + 1; j < modelCount; j++)
      if (modelTokens[j] > modelTokens[i]) {
        unsigned long t = modelTokens[i]; modelTokens[i] = modelTokens[j]; modelTokens[j] = t;
        char n[20]; strcpy(n, modelName[i]); strcpy(modelName[i], modelName[j]); strcpy(modelName[j], n);
      }
}

// ====== Animations ======
void animSplash() {
  tft.fillScreen(BG);
  for (int r = 8; r <= 50; r += 4) {
    tft.drawCircle(W / 2, 70, r, ACC);
    delay(12);
  }
  delay(80);
  tft.fillCircle(W / 2, 70, 14, ACC);
  tft.fillCircle(W / 2, 70, 10, BG);
  tft.fillCircle(W / 2, 70, 6, ACC);
  delay(150);

  const char* title = "z.ai / GLM";
  tft.setTextColor(ACC);
  tft.setTextDatum(TC_DATUM);
  int tw = tft.textWidth(title, 4);
  int tx = W / 2 - tw / 2;
  for (int i = 0; i < (int)strlen(title); i++) {
    char buf[2] = {title[i], 0};
    int cw = tft.textWidth(buf, 4);
    tft.drawString(buf, tx, 105, 4);
    tx += cw;
    delay(30);
  }
  tft.setTextColor(DIM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Token Monitor", W / 2, 140, 2);
  delay(200);
}

void animLoading(const char* msg, int y) {
  tft.fillRect(0, y - 12, W, 24, BG);
  tft.setTextColor(DIM);
  tft.setTextDatum(TC_DATUM);
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, y - 12, W, 24, BG);
    String s = String(msg);
    for (int d = 0; d <= i; d++) s += ".";
    tft.drawString(s, W / 2, y, 2);
    delay(350);
  }
}

// ====== Dynamic draws ======
void drawHeader() {
  tft.fillRect(0, 0, 140, 22, CARD2);
  tft.setTextColor(TXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("z.ai / GLM", 8, 11, 4);

  // Plan badge
  char b[16];
  sprintf(b, "%s", planLevel);
  int pw = tft.textWidth(b, 2) + 10;
  tft.fillRoundRect(148, 3, pw, 16, 8, GOLD);
  tft.setTextColor(BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(b, 148 + pw / 2, 11, 2);

  // WiFi bars at top-right edge
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  int bars = (rssi >= -50) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : (rssi >= -85) ? 1 : 0;
  int wx = W - 24;
  for (int bi = 0; bi < 4; bi++) {
    int bh = 3 + bi * 3;
    tft.fillRect(wx + bi * 4, 19 - bh, 3, bh, (bi < bars) ? GRN : DIM2);
  }

  // IP next to bars
  if (ipAddr.length()) {
    tft.setTextColor(DIM);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(ipAddr, W - 44, 11, 1);
  }
}

void drawBarWidget(int bx, int by, int bw, int bh, int pct, uint16_t color) {
  tft.fillRoundRect(bx, by, bw, bh, bh / 2, PLT);
  tft.drawRoundRect(bx, by, bw, bh, bh / 2, DIM2);
  int fw = (int)((float)bw * pct / 100.0f);
  if (fw > 0) {
    if (fw < bh) fw = bh;
    tft.fillRoundRect(bx, by, fw, bh, bh / 2, color);
    tft.drawFastHLine(bx + 2, by + 1, fw - 4, TXT);
  }
}

void updateQuota() {
  char b[20];
  uint16_t tc = (tokPct < 60) ? GRN : (tokPct < 85) ? YEL : RED;
  uint16_t mc = (mcpPct < 60) ? TEAL : (mcpPct < 85) ? YEL : RED;

  // LEFT: 5H TOKENS
  tft.fillRect(8, 28, 148, 50, CARD);
  tft.setTextColor(DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("5H TOKENS", 12, 30, 1);

  drawBarWidget(12, 42, 136, 20, tokPct, tc);

  sprintf(b, "%d%%", tokPct);
  tft.setTextColor(TXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(b, 16, 52, 2);

  // 5h: countdown + time
  String rs = fmtReset(tokResetMs);
  tft.setTextColor(YEL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("reset " + rs, 148, 52, 1);
  // Reset clock time
  if (tokResetMs > 0) {
    time_t rt = tokResetMs / 1000;
    char tb[12];
    strftime(tb, sizeof(tb), "@%H:%M", gmtime(&rt));
    tft.setTextColor(DIM);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(tb, 12, 66, 1);
  }

  // Right: MCP
  tft.fillRect(162, 28, 148, 50, CARD);
  tft.setTextColor(DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("MCP TOOLS", 166, 30, 1);

  drawBarWidget(166, 42, 136, 20, mcpPct, mc);

  sprintf(b, "%d%%", mcpPct);
  tft.setTextColor(TXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(b, 170, 52, 2);

  sprintf(b, "%d/%d", mcpUsage - mcpRemaining, mcpUsage);
  tft.setTextColor(TEAL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(b, 308, 52, 1);

  // MCP countdown + reset time
  String mrs = fmtReset(mcpResetMs);
  tft.setTextColor(YEL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(mrs, 308, 66, 1);
  if (mcpResetMs > 0) {
    time_t mrt = mcpResetMs / 1000;
    char mtb[16];
    strftime(mtb, sizeof(mtb), "@%m-%d %H:%M", gmtime(&mrt));
    tft.setTextColor(DIM);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(mtb, 166, 66, 1);
  }
}

void drawChart(bool animate) {
  // Title
  tft.fillRect(8, 98, 304, 10, CARD);
  tft.fillCircle(10, 103, 2, ACC);
  tft.setTextColor(DIM);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("24H TOKENS / HOUR", 16, 104, 1);

  char b[48];
  sprintf(b, "peak %s  avg %s", fmtTok(peakHourTok).c_str(), fmtTok(avgPerHour).c_str());
  tft.setTextColor(PINK);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(b, W - 12, 104, 1);

  // Plot area
  tft.fillRect(PX, PY, PW, PH, PLT);

  if (!hourCount) {
    tft.setTextColor(DIM);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Loading...", PX + PW / 2, PY + PH / 2, 2);
    chartDirty = false;
    return;
  }

  unsigned long mx = max(peakHourTok, 1UL);

  // Grid
  for (int g = 1; g <= 3; g++) {
    int gy = PB - (PH - 2) * g / 4;
    for (int x = PX; x < PX + PW; x += 6)
      tft.drawFastHLine(x, gy, 3, DIM2);
  }

  // Avg line
  if (avgPerHour > 0) {
    int ay = PB - (int)((float)avgPerHour * (float)(PH - 2) / (float)mx);
    for (int x = PX; x < PX + PW; x += 5)
      tft.drawFastHLine(x, ay, 2, TEAL);
  }
  tft.drawFastHLine(PX, PB, PW, DIM2);

  // Bars
  int n = hourCount;
  int bw = (PW - n) / n;
  if (bw < 2) bw = 2;
  if (bw > 10) bw = 10;
  int tw = n * bw + (n - 1);
  int sx = PX + (PW - tw) / 2;

  for (int i = 0; i < n; i++) {
    int bh = (int)((float)hourTokens[i] * (float)(PH - 2) / (float)mx);
    if (hourTokens[i] > 0 && bh < 2) bh = 2;
    int x = sx + i * (bw + 1);

    uint16_t c;
    if (i == n - 1) c = ACC;        // latest = cyan
    else if (i == peakHourIdx) c = GOLD;  // peak = gold
    else c = PUR;                    // others = purple

    if (animate) {
      for (int s = 1; s <= 6; s++) {
        int h = (int)((float)bh * (float)s / 6.0f);
        if (h > 0) tft.fillRect(x, PB - h, bw, h, c);
        delay(4);
      }
    } else {
      if (bh > 0) tft.fillRect(x, PB - bh, bw, bh, c);
    }
  }

  // Time axis
  tft.setTextColor(DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("-24h", PX, PB + 1, 1);
  tft.drawString("-12h", PX + PW / 2 - 10, PB + 1, 1);
  tft.setTextColor(ACC);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("now", PX + PW, PB + 1, 1);

  chartDirty = false;
}

void updateFooter() {
  char b[40];

  // Clear with gradient
  for (int y = 0; y < 58; y++)
    gradientH(182 + y, W, CARD, BG, (float)y / 58.0f);
  tft.drawFastHLine(0, 182, W, ACC);
  tft.drawFastVLine(80, 184, 52, DIM2);
  tft.drawFastVLine(160, 184, 52, DIM2);
  tft.drawFastVLine(240, 184, 52, DIM2);

  // Col 1: TOKENS (x=4-80)
  tft.setTextColor(DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("24H TOKENS", 8, 186, 1);
  tft.setTextColor(ACC);
  tft.drawString(fmtTok(totalTok24), 8, 200, 2);
  tft.setTextColor(DIM);
  tft.drawString("now: " + fmtTok(currentHourTok), 8, 218, 1);

  // Col 2: CALLS (x=82-160)
  tft.drawString("CALLS", 84, 186, 1);
  sprintf(b, "%lu", totalCalls24);
  tft.setTextColor(TXT);
  tft.drawString(b, 84, 200, 2);

  // Col 3: MODELS (x=162-240)
  tft.setTextColor(DIM);
  tft.drawString("MODELS", 164, 186, 1);
  for (int i = 0; i < min(modelCount, 2); i++) {
    char sn[10];
    strncpy(sn, modelName[i], 9);
    sn[9] = 0;
    int yy = 200 + i * 14;
    tft.setTextColor(i == 0 ? TXT : DIM);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(sn, 164, yy, 1);
    tft.setTextColor(i == 0 ? ACC : PUR);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(fmtTok(modelTokens[i]), 236, yy, 1);
  }

  // Col 4: RESETS (x=242-316)
  tft.setTextColor(DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("RESETS", 244, 186, 1);
  tft.setTextColor(YEL);
  tft.drawString("5H " + fmtReset(tokResetMs), 244, 200, 1);
  tft.setTextColor(TEAL);
  tft.drawString("MCP " + fmtReset(mcpResetMs), 244, 214, 1);
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(200);

  ledOff();

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);

  animSplash();

  animLoading("Connecting WiFi", 175);
  WiFi.begin("Xiaomi_1CD9_2.4ghz", "TT!@#67235520");
  WiFi.setSleep(false);
  unsigned long ws = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - ws > 20000) {
      WiFi.disconnect();
      delay(1000);
      WiFi.begin("Xiaomi_1CD9_2.4ghz", "Tt!@#67235520");
      ws = millis();
    }
  }
  ipAddr = WiFi.localIP().toString();

  // Draw dashboard skeleton first, then fetch in background
  drawStatic();
  drawHeader();
  drawChart(false);  // shows "Loading..."
  updateFooter();

  tft.setTextColor(DIM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Fetching data...", W / 2, 200, 1);

  fetchQuota();
  updateQuota();

  fetchUsage();

  // Now draw real data
  updateQuota();
  drawChart(true);
  updateFooter();

  lastFetch = millis();
}

void loop() {
  static unsigned long lastFooter = 0;
  static unsigned long lastPulse = 0;

  if (millis() - lastFetch > 300000 || lastFetch == 0) {
    fetchQuota();
    fetchUsage();
    drawHeader();
    updateQuota();
    chartDirty = true;
    lastFetch = millis();
  }

  if (chartDirty) drawChart(false);

  if (millis() - lastFooter > 10000) {
    updateFooter();
    updateQuota();
    lastFooter = millis();
  }

  // Pulse latest bar
  if (millis() - lastPulse > 1500 && hourCount > 0) {
    unsigned long mx = max(peakHourTok, 1UL);
    int n = hourCount;
    int bw = (PW - n) / n;
    if (bw < 2) bw = 2;
    if (bw > 10) bw = 10;
    int tw = n * bw + (n - 1);
    int sx = PX + (PW - tw) / 2;
    int bh = (int)((float)hourTokens[n - 1] * (float)(PH - 2) / (float)mx);
    if (hourTokens[n - 1] > 0 && bh < 2) bh = 2;
    int x = sx + (n - 1) * (bw + 1);
    static bool pulseOn = false;
    tft.drawRect(x - 1, PB - bh - 1, bw + 2, bh + 2, pulseOn ? ACC : TXT);
    pulseOn = !pulseOn;
    lastPulse = millis();
  }

  delay(200);
}

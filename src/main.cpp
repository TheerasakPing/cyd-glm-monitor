/*
 * GLM Token Monitor — ESP32-2432S028 (CYD) v29
 * Grafana dashboard style — panels, headers, metrics
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFiManager.h>

#define W 320
#define H 240

// Grafana-style colors
#define BG     0x0000  // pure black
#define PANEL  0x1082  // panel bg dark blue-gray
#define PANELH 0x18C4  // panel header slightly lighter
#define BORDER 0x2104  // border
#define ACCENT 0x07FF  // cyan accent
#define GREEN  0x07E0  // bright green
#define YELLOW 0xFFE0
#define RED    0xF800
#define ORANGE 0xFD20
#define PURPLE 0xF81F  // magenta
#define BLUE   0x07DF  // blue
#define WHITE  0xFFFF
#define GRAY   0x8410
#define DGRAY  0x4208

TFT_eSPI tft = TFT_eSPI();
WebServer ota(3232);
Preferences prefs;
WiFiManager wm;

// API key — loaded from flash, configurable via AP portal
char apiKey[64] = "0cb8538cbb1f457389ea5a5939c5f031.C5MOCFbwk5Rr2SoI";
const char* HOST = "api.z.ai";

int tokPct=0, mcpPct=0, mcpRem=0, mcpUse=0;
uint64_t tokReset=0, mcpReset=0;
char plan[16]="";
#define NH 24
unsigned long hTok[NH]; int hCnt=0;
unsigned long tot24=0, call24=0, peakTok=0, avgH=0, nowTok=0;
int peakIdx=0;
#define NM 8
char mName[NM][20]; unsigned long mTok[NM]; int mCnt=0;
String ip="";
unsigned long lastF=0;
bool dirty=true;

String fT(unsigned long v){char b[16];if(v>=1000000UL)sprintf(b,"%.1fM",v/1000000.0);else if(v>=1000UL)sprintf(b,"%.1fK",v/1000.0);else sprintf(b,"%lu",v);return String(b);}
String fR(uint64_t ms){if(!ms)return"--";long d=(long)((long long)(ms/1000)-(long long)time(nullptr));if(d<=0)return"now";char b[16];int dd=d/86400,hh=(d%86400)/3600,mm=(d%3600)/60;if(dd>0)sprintf(b,"%dd%dh",dd,hh);else sprintf(b,"%dh%dm",hh,mm);return String(b);}

// Draw text on specific bg color
void P(const String& s, int x, int y, uint16_t c, int f, uint16_t bg=BG) {
  tft.setTextColor(c, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(s, x, y, f);
}
void PR(const String& s, int x, int y, uint16_t c, int f, uint16_t bg=BG) {
  tft.setTextColor(c, bg);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(s, x, y, f);
}

// Grafana panel: header bar + body
void panel(int x, int y, int w, int h, const char* title, uint16_t accent=ACCENT) {
  // Body bg
  tft.fillRoundRect(x, y, w, h, 2, PANEL);
  // Header bar
  tft.fillRoundRect(x, y, w, 14, 2, PANELH);
  tft.drawFastHLine(x, y+13, w, BORDER);
  // Title
  P(title, x+4, y+3, WHITE, 1, PANELH);
  // Accent dot
  tft.fillCircle(x+w-6, y+6, 2, accent);
  // Border
  tft.drawRoundRect(x, y, w, h, 2, BORDER);
}

// API
String httpsGet(const char* path) {
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(12000);
  if(!c.connect(HOST,443)) return "";
  c.print(String("GET ")+path+" HTTP/1.1\r\nHost: "+HOST+"\r\nAuthorization: Bearer "+String(apiKey)+"\r\nConnection: close\r\n\r\n");
  String b=""; bool he=false; unsigned long to=millis()+15000;
  while(c.connected()||c.available()){if(millis()>to)break;if(!he){String l=c.readStringUntil('\n');if(l=="\r"||!l.length())he=true;continue;}if(c.available())b+=(char)c.read();}
  c.stop(); int i=b.indexOf('{'); return i<0?"":b.substring(i);
}

void fetchQuota() {
  String j=httpsGet("/api/monitor/usage/quota/limit");
  if(!j.length())return;
  JsonDocument d; if(deserializeJson(d,j))return;
  if(d["code"].as<int>()!=200)return;
  const char*l=d["data"]["level"]|""; strncpy(plan,l,15);
  if(!plan[0])strcpy(plan,"?");
  for(JsonVariant v:d["data"]["limits"].as<JsonArray>()){
    const char*t=v["type"]|"";
    if(!strcmp(t,"TOKENS_LIMIT")){tokPct=v["percentage"]|0;tokReset=(uint64_t)(v["nextResetTime"].as<unsigned long long>());}
    else if(!strcmp(t,"TIME_LIMIT")){mcpPct=v["percentage"]|0;mcpRem=v["remaining"]|0;mcpUse=v["usage"]|0;mcpReset=(uint64_t)(v["nextResetTime"].as<unsigned long long>());}
  }
}

// Touch on VSPI — minimal, just detect press
#define T_CLK 25
#define T_MISO 39
#define T_MOSI 32
#define T_CS 33

SPIClass touchSPI(VSPI);

void touchInit() {
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
}

bool touchPressed() {
  touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(T_CS, LOW);
  touchSPI.transfer(0xB0);  // Z1
  uint16_t hi = touchSPI.transfer(0x00);
  uint16_t lo = touchSPI.transfer(0x00);
  digitalWrite(T_CS, HIGH);
  touchSPI.endTransaction();
  return (((hi << 8) | lo) >> 3) > 200;
}
#define NDAYS 30
unsigned long dayTokens[NDAYS];
int dayCount = 0;
unsigned long monthTotal = 0;
unsigned long monthPeak = 0;
int monthPeakDay = 0;

int currentPage = 0;  // 0=main, 1=monthly
unsigned long lastPageSwitch = 0;
bool pageDrawn[2] = {false, false};

// Fetch 30-day daily totals — API returns daily when range > 2 days
void fetchMonthly() {
  configTime(0,0,"pool.ntp.org","time.google.com");
  unsigned long t0=millis();
  while(time(nullptr)<1700000000){if(millis()-t0>8000)return;delay(500);}
  
  time_t now = time(nullptr);
  monthTotal = 0; monthPeak = 0; monthPeakDay = 0;
  dayCount = 0;
  for(int i=0; i<NDAYS; i++) dayTokens[i] = 0;

  // Single request for 30 days — API returns daily granularity
  time_t startT = now - (30 * 86400);
  char ss[24], es[24];
  strftime(ss, 24, "%Y-%m-%d%%20%H:%M:%S", gmtime(&startT));
  strftime(es, 24, "%Y-%m-%d%%20%H:%M:%S", gmtime(&now));

  String path = "/api/monitor/usage/model-usage?startTime="+String(ss)+"&endTime="+String(es);
  String j = httpsGet(path.c_str());
  if(!j.length()) { Serial.println("Monthly: empty response"); return; }

  JsonDocument d;
  if(deserializeJson(d, j)) { Serial.println("Monthly: parse error"); return; }
  if(d["code"].as<int>() != 200) { Serial.println("Monthly: bad code"); return; }

  JsonObject data = d["data"].as<JsonObject>();
  JsonArray xt = data["x_time"].as<JsonArray>();
  JsonArray tu = data["tokensUsage"].as<JsonArray>();
  int total = xt.size();
  Serial.printf("Monthly: %d entries\n", total);
  if(!total) return;

  // API returns 1 entry per day for ranges > 2 days
  dayCount = min(total, NDAYS);
  for(int i=0; i<dayCount; i++) {
    dayTokens[i] = tu[i].as<unsigned long>();
    monthTotal += dayTokens[i];
    if(dayTokens[i] > monthPeak) { monthPeak = dayTokens[i]; monthPeakDay = i; }
  }

  Serial.printf("Monthly: %d days, total %s, peak %s\n", dayCount, fT(monthTotal).c_str(), fT(monthPeak).c_str());
}

void drawMonthlyPage() {
  char b[80];
  tft.fillScreen(BG);

  // Top bar
  tft.fillRect(0, 0, W, 14, PANELH);
  tft.drawFastHLine(0, 14, W, BORDER);
  P("z.ai / GLM — 30 Day Usage", 4, 3, WHITE, 1, PANELH);
  PR("page 2/2", W-4, 3, GRAY, 1, PANELH);

  // Stats panel
  panel(0, 16, W, 30, "Monthly Summary", PURPLE);
  sprintf(b, "Total: %s tok  Peak: %s  Avg: %s/d",
    fT(monthTotal).c_str(), fT(monthPeak).c_str(),
    dayCount ? fT(monthTotal/dayCount).c_str() : "--");
  P(b, 6, 32, ACCENT, 1, PANEL);

  // Chart panel
  panel(0, 48, W, 140, "Daily Token Usage (30 days)", BLUE);

  // Chart area
  int cx=16, cy=66, cw=288, chh=100, cb=cy+chh;
  tft.fillRect(cx, cy, cw, chh, BG);

  if(!dayCount) {
    P("no data", cx+cw/2-20, cy+chh/2, GRAY, 1, BG);
  } else {
    unsigned long mx = max(monthPeak, 1UL);

    // Grid
    for(int g=1; g<=3; g++) {
      int gy = cb - chh*g/4;
      for(int x=cx; x<cx+cw; x+=4) tft.drawPixel(x, gy, DGRAY);
    }
    tft.drawFastHLine(cx, cb, cw, BORDER);

    // Bars
    int n = dayCount;
    int bw = (cw-n)/n; if(bw<2) bw=2; if(bw>10) bw=10;
    int tw = n*bw+(n-1);
    int sx = cx+(cw-tw)/2;

    for(int i=0; i<n; i++) {
      int bh = (int)((float)dayTokens[i]*(float)(chh-2)/(float)mx);
      if(dayTokens[i]>0 && bh<2) bh=2;
      int x = sx+i*(bw+1);
      uint16_t c;
      if(i==n-1) c = ACCENT;           // today
      else if(i==monthPeakDay) c = WHITE; // peak
      else c = PURPLE;

      if(bh>0) tft.fillRect(x, cb-bh, bw, bh, c);
      // top highlight
      if(bh>2) tft.drawFastHLine(x, cb-bh, bw, WHITE);
    }

    // Axis
    P("-30d", cx, cb+4, GRAY, 1, PANEL);
    P("-15d", cx+cw/2-10, cb+4, GRAY, 1, PANEL);
    PR("today", cx+cw, cb+4, ACCENT, 1, PANEL);

    // Peak label
    sprintf(b, "peak %s", fT(monthPeak).c_str());
    PR(b, cx+cw, cy+2, GRAY, 1, BG);
  }

  // Footer
  panel(0, 190, W, 48, "Info", ORANGE);
  if(mCnt>0) {
    char sn[10]; strncpy(sn, mName[0], 9); sn[9]=0;
    sprintf(b, "Top: %s (%s tokens)", sn, fT(mTok[0]).c_str());
    P(b, 6, 206, PURPLE, 1, PANEL);
  }
  PR("OTA "+ip+":3232", W-6, 206, DGRAY, 1, PANEL);
  P("tap to switch", 6, 222, GRAY, 1, PANEL);

  pageDrawn[1] = true;
}

void drawAll();  // forward declaration

void drawMainPage() {
  drawAll();
  pageDrawn[0] = true;
}

void fetchUsage() {
  configTime(0,0,"pool.ntp.org","time.google.com");
  unsigned long t0=millis();
  while(time(nullptr)<1700000000){if(millis()-t0>8000)return;delay(500);}
  time_t now=time(nullptr),st=now-86400;
  char ss[24],es[24];
  strftime(ss,24,"%Y-%m-%d%%20%H:%M:%S",gmtime(&st));
  strftime(es,24,"%Y-%m-%d%%20%H:%M:%S",gmtime(&now));
  String j=httpsGet(("/api/monitor/usage/model-usage?startTime="+String(ss)+"&endTime="+String(es)).c_str());
  if(!j.length())return;
  JsonDocument d; if(deserializeJson(d,j))return;
  if(d["code"].as<int>()!=200)return;
  JsonObject data=d["data"].as<JsonObject>();
  JsonArray xt=data["x_time"].as<JsonArray>();
  JsonArray tu=data["tokensUsage"].as<JsonArray>();
  JsonArray cc=data["modelCallCount"].as<JsonArray>();
  int total=xt.size();if(!total)return;
  int si=max(0,total-NH);
  hCnt=total-si;tot24=0;call24=0;peakTok=0;peakIdx=0;
  for(int i=0;i<hCnt;i++){int idx=si+i;hTok[i]=(idx<(int)tu.size())?tu[idx].as<unsigned long>():0;tot24+=hTok[i];call24+=(idx<(int)cc.size())?cc[idx].as<unsigned long>():0;if(hTok[i]>peakTok){peakTok=hTok[i];peakIdx=i;}}
  nowTok=hCnt?hTok[hCnt-1]:0;avgH=hCnt?tot24/hCnt:0;
  JsonArray ml=data["modelDataList"].as<JsonArray>();
  mCnt=min((int)ml.size(),NM);
  for(int m=0;m<mCnt;m++){strncpy(mName[m],ml[m]["modelName"]|"?",19);mName[m][19]=0;JsonArray tt=ml[m]["tokensUsage"].as<JsonArray>();unsigned long s=0;for(int i=si;i<total&&i<(int)tt.size();i++)s+=tt[i].as<unsigned long>();mTok[m]=s;}
  for(int i=0;i<mCnt-1;i++)for(int j=i+1;j<mCnt;j++)if(mTok[j]>mTok[i]){unsigned long t=mTok[i];mTok[i]=mTok[j];mTok[j]=t;char n[20];strcpy(n,mName[i]);strcpy(mName[i],mName[j]);strcpy(mName[j],n);}
}

void handleRoot(){ota.send(200,"text/html","<h2>CYD GLM Monitor</h2>"
  "<h3>OTA</h3><form method=POST action=/upload enctype=multipart/form-data><input type=file name=update><input type=submit value='Upload Firmware'></form>"
  "<h3>Settings</h3><form method=POST action=/savekey>"
  "<label>Z.ai API Key:</label><br><input name=apikey value='"+String(apiKey)+"' size=50><br><input type=submit value=Save>"
  "</form><br><a href=/shot>Screenshot</a> | <a href=/reset>Reset WiFi</a>");}

void handleSaveKey(){
  if(ota.hasArg("apikey")){
    String k = ota.arg("apikey");
    if(k.length()>0){
      strncpy(apiKey, k.c_str(), 63); apiKey[63]=0;
      prefs.putString("apikey", apiKey);
      ota.send(200,"text/html","<h2>Saved!</h2><a href=/>Back</a>");
    } else ota.send(400,"text/plain","empty key");
  } else ota.send(400,"text/plain","missing apikey");
}

void handleReset(){
  wm.resetSettings();
  ota.send(200,"text/html","<h2>WiFi reset!</h2>Rebooting...");
  delay(1000);
  ESP.restart();
}
void handleShot(){
  const int cw=8,ch=16,cols=40,rows=15;
  String out="<pre style='font:6px monospace;line-height:1'>";
  for(int r=0;r<rows;r++){for(int c=0;c<cols;c++){uint16_t px=tft.readPixel(c*cw+cw/2,r*ch+ch/2);uint8_t rr=((px>>11)&0x1F)*255/31;uint8_t gg=((px>>5)&0x3F)*255/63;uint8_t bb=(px&0x1F)*255/31;char hex[8];sprintf(hex,"#%02X%02X%02X",rr,gg,bb);out+="<span style='background:"+String(hex)+"'>&nbsp;&nbsp;</span>";}out+="\n";}
  out+="</pre>";ota.send(200,"text/html",out);
}
void handleUpload(){HTTPUpload&u=ota.upload();if(u.status==UPLOAD_FILE_START){if(!Update.begin(0,U_FLASH))Update.printError(Serial);}else if(u.status==UPLOAD_FILE_WRITE){if(Update.write(u.buf,u.currentSize)!=u.currentSize)Update.printError(Serial);}else if(u.status==UPLOAD_FILE_END){if(Update.end(true)){delay(500);ESP.restart();}else Update.printError(Serial);}}
void ledOff(){pinMode(4,OUTPUT);pinMode(16,OUTPUT);pinMode(17,OUTPUT);digitalWrite(4,HIGH);digitalWrite(16,HIGH);digitalWrite(17,HIGH);}

// Grafana layout (320x240):
// Top bar: y=0-14 (dashboard title)
// Row 1: y=16-80 — two panels side by side (5H quota, MCP quota)
// Row 2: y=82-170 — chart panel (full width)
// Row 3: y=172-238 — stats panel (full width)

#define PX 16
#define PY 96
#define PW 288
#define PH 64
#define PB (PY+PH)

void splash() {
  tft.fillScreen(BG);
  tft.fillRoundRect(60, 50, 200, 80, 4, PANEL);
  tft.drawRoundRect(60, 50, 200, 80, 4, BORDER);
  tft.fillRoundRect(60, 50, 200, 16, 2, PANELH);
  P("z.ai / GLM", 66, 53, WHITE, 1, PANELH);
  P("Grafana Dashboard", 90, 80, ACCENT, 2, PANEL);
  P("loading...", 110, 104, GRAY, 1, PANEL);
  delay(400);
}

void drawTopBar() {
  tft.fillRect(0, 0, W, 14, PANELH);
  tft.drawFastHLine(0, 14, W, BORDER);
  P("z.ai / GLM", 4, 3, WHITE, 1, PANELH);
  PR("OTA "+ip+":3232", W-4, 3, ACCENT, 1, PANELH);
}

void drawQuotaPanels() {
  char b[32];

  // LEFT: 5H
  panel(0, 16, 156, 64, "5H Token Quota", GREEN);
  uint16_t tc = (tokPct<60)?GREEN:(tokPct<85)?YELLOW:RED;
  // big % on right
  sprintf(b, "%d%%", tokPct);
  PR(b, 152, 30, tc, 4, PANEL);
  // gauge bar
  int bx=6, by=52, bw=144, bh=8;
  tft.drawRoundRect(bx, by, bw, bh, 2, BORDER);
  int fw = (int)((float)bw * tokPct / 100.0);
  if(fw>0) { if(fw<2)fw=2; tft.fillRoundRect(bx, by, fw, bh, 2, tc); }
  // reset countdown — always show something
  if(tokReset > 0) {
    time_t now = time(nullptr);
    if(now >= 1700000000) {
      sprintf(b, "reset %s", fR(tokReset).c_str());
    } else {
      // NTP not synced — show raw
      sprintf(b, "reset @%lu", tokReset/1000);
    }
    P(b, 6, 64, YELLOW, 1, PANEL);
    Serial.printf("5H reset: %s (tokReset=%llu)\n", b, tokReset);
  } else {
    P("reset --", 6, 64, GRAY, 1, PANEL);
    Serial.println("5H: no reset data");
  }

  // RIGHT: MCP
  panel(164, 16, 156, 64, "MCP Tools", ACCENT);
  uint16_t mc = (mcpPct<60)?ACCENT:(mcpPct<85)?YELLOW:RED;
  sprintf(b, "%d%%", mcpPct);
  PR(b, 316, 30, mc, 4, PANEL);
  // gauge bar
  tft.drawRoundRect(170, 52, 144, 8, 2, BORDER);
  int mfw = (int)(144.0*mcpPct/100.0);
  if(mfw>0) { if(mfw<2)mfw=2; tft.fillRoundRect(170, 52, mfw, 8, 2, mc); }
  sprintf(b, "%d/%d", mcpUse-mcpRem, mcpUse);
  P(b, 170, 64, ACCENT, 1, PANEL);
  if(mcpReset) {
    sprintf(b, "%s", fR(mcpReset).c_str());
    PR(b, 318, 64, YELLOW, 1, PANEL);
  }
}

void drawChartPanel(bool anim) {
  char b[80];
  panel(0, 82, W, 88, "Token Usage (24h)", PURPLE);

  // Stats in header right
  sprintf(b, "%s tok  %lu calls", fT(tot24).c_str(), call24);
  PR(b, W-10, 85, GRAY, 1, PANELH);

  // Clear plot area
  tft.fillRect(PX, PY, PW, PH, BG);

  if(!hCnt) { P("no data", PX+PW/2-20, PY+PH/2, GRAY, 1, BG); dirty=false; return; }

  unsigned long mx = max(peakTok, 1UL);

  // Grid lines (Grafana style — faint horizontal)
  for(int g=1; g<=3; g++) {
    int gy = PB - PH*g/4;
    for(int x=PX; x<PX+PW; x+=4) tft.drawPixel(x, gy, DGRAY);
  }
  tft.drawFastHLine(PX, PB, PW, BORDER);

  // Bars — Grafana style: area chart look with varying colors
  int n = hCnt;
  int bw = (PW-n)/n; if(bw<2) bw=2; if(bw>8) bw=8;
  int tw = n*bw+(n-1);
  int sx = PX+(PW-tw)/2;

  for(int i=0; i<n; i++) {
    int bh = (int)((float)hTok[i]*(float)(PH-2)/(float)mx);
    if(hTok[i]>0 && bh<2) bh=2;
    int x = sx+i*(bw+1);

    uint16_t c;
    if(i==n-1) c = ACCENT;        // latest = cyan
    else if(i==peakIdx) c = WHITE; // peak = white
    else c = PURPLE;               // normal = purple

    if(anim) {
      for(int s=1; s<=6; s++) {
        int hh = (int)((float)bh*(float)s/6.0);
        if(hh>0) tft.fillRect(x, PB-hh, bw, hh, c);
        delay(2);
      }
    } else {
      if(bh>0) tft.fillRect(x, PB-bh, bw, bh, c);
    }
  }

  // Avg line (dashed)
  if(avgH>0) {
    int ay = PB-(int)((float)avgH*(float)(PH-2)/(float)mx);
    for(int x=PX; x<PX+PW; x+=5) tft.drawFastHLine(x, ay, 2, GREEN);
  }

  // Peak label
  sprintf(b, "peak %s  avg %s", fT(peakTok).c_str(), fT(avgH).c_str());
  PR(b, PX+PW, PY+2, GRAY, 1, BG);

  // Axis labels
  P("-24h", PX, PB+4, GRAY, 1, PANEL);
  PR("now", PX+PW, PB+4, ACCENT, 1, PANEL);

  dirty = false;
}

void drawStatsPanel() {
  char b[40];
  panel(0, 172, W, 66, "Statistics", BLUE);
  bool ok = (time(nullptr) >= 1700000000);

  int y = 190;

  // 4 metric columns
  P("CURRENT", 6, y, GRAY, 1, PANEL);
  P(fT(nowTok), 6, y+12, ACCENT, 2, PANEL);

  P("TOTAL 24H", 80, y, GRAY, 1, PANEL);
  P(fT(tot24), 80, y+12, WHITE, 2, PANEL);

  P("CALLS", 160, y, GRAY, 1, PANEL);
  sprintf(b, "%lu", call24);
  P(b, 160, y+12, ORANGE, 2, PANEL);

  // Top model
  P("TOP MODEL", 230, y, GRAY, 1, PANEL);
  if(mCnt>0) {
    char sn[10]; strncpy(sn,mName[0],9); sn[9]=0;
    P(sn, 230, y+12, PURPLE, 1, PANEL);
    sprintf(b, "%s tok", fT(mTok[0]).c_str());
    P(b, 230, y+24, PURPLE, 1, PANEL);
  }

  // Status line
  if(ok) {
    sprintf(b, "5H reset: %s", fR(tokReset).c_str());
    P(b, 6, 224, YELLOW, 1, PANEL);
    sprintf(b, "MCP reset: %s", fR(mcpReset).c_str());
    P(b, 130, 224, YELLOW, 1, PANEL);
  }
  PR("v29", W-6, 224, DGRAY, 1, PANEL);

  // Live indicator
  static bool blink=false; blink=!blink;
  if(blink) tft.fillCircle(290, 194, 2, GREEN);
}

void drawAll() {
  tft.fillScreen(BG);
  drawTopBar();
  drawQuotaPanels();
  drawChartPanel(false);
  drawStatsPanel();
}

void setup() {
  Serial.begin(115200); delay(200);
  ledOff();
  ledcSetup(0,1000,8); ledcAttachPin(21,0); ledcWrite(0,38); // 15%
  tft.init(); tft.setRotation(1);

  splash();

  tft.fillScreen(BG);
  P("connecting...", 20, 100, ACCENT, 2);

  // Load saved API key
  prefs.begin("glm", false);
  String savedKey = prefs.getString("apikey", "");
  if(savedKey.length() > 0) {
    strncpy(apiKey, savedKey.c_str(), 63);
    apiKey[63] = 0;
  }

  // WiFiManager with custom API key parameter
  WiFiManagerParameter keyParam("apikey", "Z.ai API Key", apiKey, 64);
  wm.addParameter(&keyParam);

  // Show portal message on TFT
  tft.fillScreen(BG);
  tft.setTextColor(ACCENT, BG); tft.setTextDatum(TC_DATUM);
  tft.drawString("WiFi Setup", W/2, 70, 2);
  tft.setTextColor(GRAY, BG); tft.drawString("Connect to:", W/2, 95, 1);
  tft.setTextColor(ACCENT, BG); tft.drawString("CYD-GLM-Setup", W/2, 110, 2);
  tft.setTextColor(YELLOW, BG); tft.drawString("192.168.4.1", W/2, 130, 1);

  // Auto-connect, 5 min timeout → retry
  bool res = wm.autoConnect("CYD-GLM-Setup");

  if(!res) {
    tft.setTextColor(RED, BG); tft.drawString("WiFi failed - retry...", W/2, 150, 1);
    delay(2000);
    ESP.restart();
  }

  // Save API key if changed
  String newKey = keyParam.getValue();
  if(newKey.length() > 0 && strcmp(newKey.c_str(), apiKey) != 0) {
    strncpy(apiKey, newKey.c_str(), 63);
    apiKey[63] = 0;
    prefs.putString("apikey", apiKey);
  }

  ip = WiFi.localIP().toString();
  Serial.println("WiFi: " + ip);
  Serial.println("API key: " + String(apiKey));

  ota.on("/",HTTP_GET,handleRoot);
  ota.on("/savekey",HTTP_POST,handleSaveKey);
  ota.on("/reset",HTTP_GET,handleReset);
  ota.on("/shot",HTTP_GET,handleShot);
  ota.on("/upload",HTTP_POST,[](){ota.send(200,"text/plain","OK");},handleUpload);
  ota.begin();
  touchInit();

  drawMainPage();

  fetchQuota();
  drawQuotaPanels();
  drawStatsPanel();

  fetchUsage();
  drawChartPanel(true);
  drawStatsPanel();

  // Pre-fetch monthly data during startup so page 2 is ready
  P("loading monthly...", 6, 224, GRAY, 1, PANEL);
  fetchMonthly();

  lastF=millis();
  lastPageSwitch=millis();
}

void loop() {
  static unsigned long lastUI=0, lastPulse=0, lastMonthly=0;

  ota.handleClient();

  // Touch to switch pages
  static unsigned long lastTouch = 0;
  if(touchPressed() && millis()-lastTouch > 1000) {
    currentPage = (currentPage + 1) % 2;
    pageDrawn[0] = false;
    pageDrawn[1] = false;
    lastTouch = millis();
    delay(200); // debounce
  }

  // Draw current page if needed
  if(currentPage == 0) {
    if(!pageDrawn[0]) drawMainPage();
    // Update data
    if(millis()-lastF>60000||lastF==0){
      fetchQuota(); fetchUsage();
      drawQuotaPanels(); dirty=true;
      lastF=millis();
    }
    if(dirty) drawChartPanel(false);
    if(millis()-lastUI>30000){
      drawStatsPanel(); drawQuotaPanels();
      lastUI=millis();
    }
    // Refresh monthly every 10 min during page 0
    static unsigned long lastMonthly = 0;
    if(millis()-lastMonthly > 600000 || (lastMonthly==0 && dayCount>0)) {
      lastMonthly = millis();
      if(dayCount > 0) fetchMonthly();  // silent refresh
    }
    // Pulse latest bar
    if(millis()-lastPulse>1500&&hCnt>0){
      unsigned long mx=max(peakTok,1UL);
      int n=hCnt,bw=(PW-n)/n;if(bw<2)bw=2;if(bw>8)bw=8;
      int tw=n*bw+(n-1),sx=PX+(PW-tw)/2;
      int bh=(int)((float)hTok[n-1]*(float)(PH-2)/(float)mx);
      if(hTok[n-1]>0&&bh<2)bh=2;
      int x=sx+(n-1)*(bw+1);
      static bool on=false;
      tft.drawRect(x-1,PB-bh-1,bw+2,bh+2,on?ACCENT:WHITE);
      on=!on; lastPulse=millis();
    }
  } else {
    // Page 2: monthly — just draw, don't fetch (fetched during page 0 idle)
    if(!pageDrawn[1]) drawMonthlyPage();
  }

  delay(10);
}

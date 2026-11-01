/*
 * ============================================================================
 *  Cardputer ADV - Solo BTC "Lottery" Miner
 * ----------------------------------------------------------------------------
 *  Gerekli kutuphaneler (Arduino Library Manager):
 *    - M5Cardputer   (M5Stack, son surum; M5Unified + M5GFX otomatik gelir)
 *    - ArduinoJson   (Benoit Blanchon, 7.x)
 *  Kart: Boards Manager -> "M5Stack" -> "M5Cardputer"
 *  Ayar: USB CDC On Boot = Enabled, Partition = Default (8MB uyumlu)
 *
 *  Tuslar (normal mod):
 *    Tab veya /   : sonraki sayfa        ,  : onceki sayfa
 *    P veya BtnG0 : duraklat / devam     - = : ekran parlakligi
 *    ; (yukari)  . (asagi)  : ayarlarda gezin      Enter : sec / duzenle
 *  Duzenleme modunda:  yaz, Del = sil, Enter = kaydet, ` (Esc) = iptal
 * ============================================================================
 */
#include <M5Cardputer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <vector>
#include "miner_types.h"

static void checkHash(const uint8_t* h, uint32_t nonce, const Job& job);

// ------------------------------- VARSAYILANLAR ------------------------------
#define DEFAULT_ADDR   "1KJKzc3YknKSPyjybgx6GzkvKZjo9c7rWV"
#define DEFAULT_POOL   "public-pool.io"
#define DEFAULT_PORT   21496
#define BACKUP_POOL    "solo.ckpool.org"
#define BACKUP_PORT    3333
#define DEFAULT_WORKER "cardputer"
#define FW_NAME        "BTC SOLO MINER"
static const double DIFF1 = 2.6959535291011309e67;

// --------------------------------- AYARLAR ----------------------------------
struct Cfg {
  String ssid, pass, host, addr, worker;
  uint16_t port;
  uint8_t bright;
  uint16_t dimSec;
} cfg;
Preferences prefs;
SemaphoreHandle_t cfgMtx;

void loadCfg() {
  prefs.begin("miner", true);
  cfg.ssid   = prefs.getString("ssid", "");
  cfg.pass   = prefs.getString("pass", "");
  cfg.host   = prefs.getString("host", DEFAULT_POOL);
  cfg.port   = prefs.getUShort("port", DEFAULT_PORT);
  cfg.addr   = prefs.getString("addr", DEFAULT_ADDR);
  cfg.worker = prefs.getString("wrk", DEFAULT_WORKER);
  cfg.bright = prefs.getUChar("br", 110);
  cfg.dimSec = prefs.getUShort("dim", 60);
  prefs.end();
}
void saveCfg() {
  prefs.begin("miner", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.host);
  prefs.putUShort("port", cfg.port);
  prefs.putString("addr", cfg.addr);
  prefs.putString("wrk", cfg.worker);
  prefs.putUChar("br", cfg.bright);
  prefs.putUShort("dim", cfg.dimSec);
  prefs.end();
}

// ------------------------------ PAYLASILAN DURUM ----------------------------
// (Job ve Share struct'lari miner_types.h icinde tanimlidir)

volatile uint32_t g_h32 = 0;          // hash sayaci (tasarsa sorun degil)
volatile uint32_t g_jobSeq = 0;
volatile bool g_paused = false, g_mining = false, g_needEn2 = false;
volatile bool g_wifi = false, g_reconf = false, g_blockFound = false;
volatile int  g_poolState = 0;        // 0 kapali, 1 baglaniyor, 2 online, 3 auth hatasi
volatile uint32_t g_shA = 0, g_shR = 0, g_jobs = 0;
volatile double g_best = 0, g_poolDiff = 0.001, g_netDiff = 0;
Job g_job;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t shareQ;
bool g_usingBackup = false;

// --------------------------------- YARDIMCI ---------------------------------
static inline void dsha(const uint8_t* in, size_t n, uint8_t* out) {
  uint8_t t[32];
  mbedtls_sha256(in, n, t, 0);
  mbedtls_sha256(t, 32, out, 0);
}
static int hv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
static size_t hex2bin(const char* h, uint8_t* o, size_t mx) {
  size_t n = strlen(h) / 2;
  if (n > mx) n = mx;
  for (size_t i = 0; i < n; i++) o[i] = (hv(h[2 * i]) << 4) | hv(h[2 * i + 1]);
  return n;
}
static void bin2hex(const uint8_t* b, size_t n, char* o) {
  const char* d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { o[2 * i] = d[b[i] >> 4]; o[2 * i + 1] = d[b[i] & 15]; }
  o[2 * n] = 0;
}
String fmtNum(double v) {
  const char* u[] = {"", "K", "M", "G", "T", "P", "E"};
  int i = 0;
  while (v >= 1000 && i < 6) { v /= 1000; i++; }
  char b[20];
  snprintf(b, sizeof(b), v >= 100 ? "%.0f%s" : (v >= 10 ? "%.1f%s" : "%.2f%s"), v, u[i]);
  return String(b);
}
String fmtDiff(double v) {
  if (v < 1) { char b[16]; snprintf(b, sizeof(b), "%.4f", v); return String(b); }
  return fmtNum(v);
}
String fmtUptime(uint32_t s) {
  char b[20];
  snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  return String(b);
}
String trunc(const String& s, int n) {
  if ((int)s.length() <= n) return s;
  return s.substring(0, n - 1) + "~";
}

// ------------------------------- STRATUM / JOB ------------------------------
WiFiClient cl;
String s_en1;
int s_en2size = 4;
bool s_subscribed = false, s_authorized = false;
uint32_t en2ctr = 0, submitId = 100;
struct Notify {
  String id, prev, c1, c2, ver, nbits, ntime;
  std::vector<String> branch;
  bool valid = false;
} nf;

void buildJob() {
  if (!nf.valid || !s_subscribed) return;
  static uint8_t cb[1024];
  size_t n = 0;
  n += hex2bin(nf.c1.c_str(), cb + n, sizeof(cb) - n);
  n += hex2bin(s_en1.c_str(), cb + n, sizeof(cb) - n);
  uint8_t en2b[8] = {0};
  for (int i = 0; i < 4 && i < s_en2size; i++) en2b[i] = (en2ctr >> (8 * i)) & 0xFF;
  if (n + s_en2size < sizeof(cb)) { memcpy(cb + n, en2b, s_en2size); n += s_en2size; }
  n += hex2bin(nf.c2.c_str(), cb + n, sizeof(cb) - n);

  uint8_t root[32];
  dsha(cb, n, root);
  for (auto& b : nf.branch) {
    uint8_t cat[64];
    memcpy(cat, root, 32);
    hex2bin(b.c_str(), cat + 32, 32);
    dsha(cat, 64, root);
  }

  Job j;
  memset(&j, 0, sizeof(j));
  uint8_t t[32];
  hex2bin(nf.ver.c_str(), t, 4);
  for (int i = 0; i < 4; i++) j.hdr[i] = t[3 - i];
  hex2bin(nf.prev.c_str(), t, 32);
  for (int w = 0; w < 8; w++)
    for (int i = 0; i < 4; i++) j.hdr[4 + w * 4 + i] = t[w * 4 + 3 - i];
  memcpy(j.hdr + 36, root, 32);
  hex2bin(nf.ntime.c_str(), t, 4);
  for (int i = 0; i < 4; i++) j.hdr[68 + i] = t[3 - i];
  hex2bin(nf.nbits.c_str(), t, 4);
  for (int i = 0; i < 4; i++) j.hdr[72 + i] = t[3 - i];

  uint32_t nb = strtoul(nf.nbits.c_str(), NULL, 16);
  int ex = nb >> 24;
  double tgt = (double)(nb & 0xFFFFFF) * pow(256.0, ex - 3);
  j.netDiff = tgt > 0 ? DIFF1 / tgt : 0;
  g_netDiff = j.netDiff;

  strncpy(j.id, nf.id.c_str(), sizeof(j.id) - 1);
  strncpy(j.ntime, nf.ntime.c_str(), sizeof(j.ntime) - 1);
  bin2hex(en2b, s_en2size, j.en2);

  portENTER_CRITICAL(&mux);
  memcpy(&g_job, &j, sizeof(Job));
  g_jobSeq++;
  portEXIT_CRITICAL(&mux);
}

void handleLine(const String& line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  const char* m = doc["method"] | "";
  if (!strcmp(m, "mining.notify")) {
    JsonArray p = doc["params"].as<JsonArray>();
    if (p.size() < 8) return;
    nf.id = p[0].as<String>();
    nf.prev = p[1].as<String>();
    nf.c1 = p[2].as<String>();
    nf.c2 = p[3].as<String>();
    nf.branch.clear();
    for (JsonVariant b : p[4].as<JsonArray>()) nf.branch.push_back(b.as<String>());
    nf.ver = p[5].as<String>();
    nf.nbits = p[6].as<String>();
    nf.ntime = p[7].as<String>();
    nf.valid = true;
    en2ctr = 0;
    g_jobs++;
    buildJob();
  } else if (!strcmp(m, "mining.set_difficulty")) {
    double d = doc["params"][0].as<double>();
    if (d > 0) g_poolDiff = d;
  } else {
    int id = doc["id"] | 0;
    if (id == 1) {
      JsonArray r = doc["result"].as<JsonArray>();
      if (!r.isNull() && r.size() >= 3) {
        s_en1 = r[1].as<String>();
        s_en2size = r[2] | 4;
        if (s_en2size < 1) s_en2size = 1;
        if (s_en2size > 8) s_en2size = 8;
        s_subscribed = true;
        buildJob();
      }
    } else if (id == 2) {
      s_authorized = doc["result"] | false;
      g_poolState = s_authorized ? 2 : 3;
    } else if (id >= 100) {
      if (doc["result"] | false) g_shA++; else g_shR++;
    }
  }
  g_mining = s_subscribed && s_authorized && nf.valid;
}

void sendLine(const char* s) { cl.print(s); cl.print("\n"); }

void stratumLoop() {
  static uint32_t retryAt = 0, lastRx = 0;
  static String line;
  static int fails = 0;

  if (g_reconf) {
    g_reconf = false;
    cl.stop();
    retryAt = 0; fails = 0;
  }
  if (!g_wifi) {
    if (cl.connected()) cl.stop();
    g_mining = false; g_poolState = 0;
    return;
  }
  if (!cl.connected()) {
    g_mining = false;
    if (g_poolState != 3) g_poolState = 1;
    if (millis() < retryAt) return;
    String host, addr, wrk;
    uint16_t port;
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    host = g_usingBackup ? String(BACKUP_POOL) : cfg.host;
    port = g_usingBackup ? BACKUP_PORT : cfg.port;
    addr = cfg.addr; wrk = cfg.worker;
    xSemaphoreGive(cfgMtx);

    s_subscribed = false; s_authorized = false; nf.valid = false;
    xQueueReset(shareQ);
    if (cl.connect(host.c_str(), port, 5000)) {
      cl.setNoDelay(true);
      line = ""; lastRx = millis(); fails = 0;
      g_poolState = 1;
      sendLine("{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"CardputerADV/1.0\"]}");
      char buf[320];
      snprintf(buf, sizeof(buf), "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s.%s\",\"x\"]}", addr.c_str(), wrk.c_str());
      sendLine(buf);
      sendLine("{\"id\":3,\"method\":\"mining.suggest_difficulty\",\"params\":[0.0005]}");
    } else {
      fails++;
      if (fails >= 3) { g_usingBackup = !g_usingBackup; fails = 0; }
      retryAt = millis() + 4000;
    }
    return;
  }

  while (cl.available()) {
    char c = cl.read();
    lastRx = millis();
    if (c == '\n') { handleLine(line); line = ""; }
    else if (line.length() < 8192) line += c;
  }
  if (millis() - lastRx > 240000) { cl.stop(); return; }   // pool sessiz -> yeniden baglan

  Share s;
  while (xQueueReceive(shareQ, &s, 0) == pdTRUE) {
    String w;
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    w = cfg.addr + "." + cfg.worker;
    xSemaphoreGive(cfgMtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08x\"]}",
             (unsigned long)submitId++, w.c_str(), s.id, s.en2, s.ntime, (unsigned int)s.nonce);
    sendLine(buf);
  }
  if (g_needEn2) { g_needEn2 = false; en2ctr++; buildJob(); }
}

void wifiMaintain() {
  static uint32_t lastTry = 0, downSince = 0;
  static bool wasUp = false;
  if (g_reconf) { WiFi.disconnect(); lastTry = 0; }
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasUp) { WiFi.setSleep(false); wasUp = true; }
    downSince = 0; g_wifi = true;
    return;
  }
  wasUp = false; g_wifi = false;
  if (!downSince) downSince = millis();
  String ssid, pass;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  ssid = cfg.ssid; pass = cfg.pass;
  xSemaphoreGive(cfgMtx);
  if (ssid.length() == 0) return;
  if (lastTry == 0 || millis() - lastTry > 15000) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  if (millis() - downSince > 600000) ESP.restart();        // 10 dk internet yok -> yeniden baslat
}

void netTask(void*) {
  uint32_t lastHash = 0, lastProg = millis();
  for (;;) {
    wifiMaintain();
    stratumLoop();
    if (g_mining && !g_paused) {
      uint32_t h = g_h32;
      if (h != lastHash) { lastHash = h; lastProg = millis(); }
      else if (millis() - lastProg > 120000) ESP.restart();  // madenci kilitlendi
    } else lastProg = millis();
    if (ESP.getFreeHeap() < 20000) ESP.restart();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------- MADENCI ---------------------------------
static void checkHash(const uint8_t* h, uint32_t nonce, const Job& job) {
  double v = 0;
  for (int i = 31; i >= 0; i--) v = v * 256.0 + h[i];
  if (v < 1) v = 1;
  double d = DIFF1 / v;
  if (d > g_best) g_best = d;
  if (job.netDiff > 0 && d >= job.netDiff) g_blockFound = true;
  if (d >= g_poolDiff) {
    Share s;
    memset(&s, 0, sizeof(s));
    strncpy(s.id, job.id, sizeof(s.id) - 1);
    strncpy(s.en2, job.en2, sizeof(s.en2) - 1);
    strncpy(s.ntime, job.ntime, sizeof(s.ntime) - 1);
    s.nonce = nonce;
    xQueueSend(shareQ, &s, 0);
  }
}

void loop() {   // core 1: sadece hash
  static uint32_t seq = 0, nonce = 0, startNonce = 0;
  static Job job;
  static mbedtls_sha256_context mid, ctx;
  static bool init = false, have = false, done = false;
  if (!init) { mbedtls_sha256_init(&mid); mbedtls_sha256_init(&ctx); init = true; }

  if (g_paused || !g_mining) { vTaskDelay(pdMS_TO_TICKS(50)); return; }
  if (seq != g_jobSeq) {
    portENTER_CRITICAL(&mux);
    memcpy(&job, &g_job, sizeof(Job));
    seq = g_jobSeq;
    portEXIT_CRITICAL(&mux);
    mbedtls_sha256_starts(&mid, 0);
    mbedtls_sha256_update(&mid, job.hdr, 64);
    nonce = esp_random(); startNonce = nonce;
    have = true; done = false;
  }
  if (!have || done) { vTaskDelay(pdMS_TO_TICKS(20)); return; }

  uint8_t tail[16], h1[32], h2[32];
  memcpy(tail, job.hdr + 64, 12);
  uint32_t cnt = 0;
  while (cnt < 4096) {
    memcpy(tail + 12, &nonce, 4);
    mbedtls_sha256_clone(&ctx, &mid);
    mbedtls_sha256_update(&ctx, tail, 16);
    mbedtls_sha256_finish(&ctx, h1);
    mbedtls_sha256(h1, 32, h2, 0);
    if (h2[31] == 0 && h2[30] == 0) checkHash(h2, nonce, job);
    nonce++; cnt++;
    if (nonce == startNonce) { done = true; g_needEn2 = true; break; }
  }
  g_h32 += cnt;
  static uint32_t yieldCnt = 0;
  yieldCnt += cnt;
  if (yieldCnt >= 65536) { yieldCnt = 0; vTaskDelay(1); }    // watchdog'a nefes
}

// ------------------------------------ UI ------------------------------------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t C_BG = rgb(10, 10, 14), C_PANEL = rgb(32, 32, 42), C_OR = rgb(247, 147, 26);
const uint16_t C_GRN = rgb(60, 220, 110), C_RED = rgb(240, 70, 70), C_YEL = rgb(250, 200, 50);
const uint16_t C_GREY = rgb(140, 140, 150), C_WHITE = rgb(255, 255, 255), C_BLK = rgb(0, 0, 0);

M5Canvas cv(&M5Cardputer.Display);
int page = 0, selIdx = 0;
bool editing = false, resetArmed = false;
String ebuf;
uint32_t lastKey = 0;
double g_rate = 0;
uint64_t g_total = 0;
float hist[60];
int battPct = -1;

#define NSET 10
const char* SET_LABEL[NSET] = {"WiFi SSID", "WiFi Pass", "Pool host", "Pool port", "BTC addr",
                               "Worker", "Brightness", "Dim after", "Apply+Reboot", "Factory rst"};
const uint8_t BR_STEPS[] = {20, 60, 110, 160, 210, 255};
const uint16_t DIM_STEPS[] = {0, 15, 30, 60, 120, 300};

String settingValue(int i) {
  switch (i) {
    case 0: return cfg.ssid.length() ? cfg.ssid : "<bos>";
    case 1: return cfg.pass.length() ? String("********") : "<bos>";
    case 2: return cfg.host;
    case 3: return String(cfg.port);
    case 4: return cfg.addr.length() > 22 ? cfg.addr.substring(0, 10) + ".." + cfg.addr.substring(cfg.addr.length() - 8) : cfg.addr;
    case 5: return cfg.worker;
    case 6: return String(cfg.bright);
    case 7: return cfg.dimSec ? String(cfg.dimSec) + " sn" : String("kapali");
    case 8: return "Enter = yeniden baslat";
    case 9: return resetArmed ? "TEKRAR Enter = SIL" : "Enter = varsayilan";
  }
  return "";
}
String settingEditStart(int i) {
  switch (i) {
    case 0: return cfg.ssid; case 1: return cfg.pass; case 2: return cfg.host;
    case 3: return String(cfg.port); case 4: return cfg.addr; case 5: return cfg.worker;
  }
  return "";
}

void commitEdit() {
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  String t = ebuf; t.trim();
  switch (selIdx) {
    case 0: cfg.ssid = ebuf; break;
    case 1: cfg.pass = ebuf; break;
    case 2: if (t.length()) cfg.host = t; break;
    case 3: { int p = t.toInt(); if (p > 0 && p < 65536) cfg.port = p; } break;
    case 4: if (t.length() > 10) cfg.addr = t; break;
    case 5: if (t.length()) cfg.worker = t; break;
  }
  saveCfg();
  xSemaphoreGive(cfgMtx);
  editing = false;
  g_usingBackup = false;
  g_reconf = true;
}

void activateSetting() {
  if (selIdx <= 5) { editing = true; ebuf = settingEditStart(selIdx); return; }
  if (selIdx == 6) {
    int k = 0;
    for (int i = 0; i < 6; i++) if (BR_STEPS[i] <= cfg.bright) k = i;
    cfg.bright = BR_STEPS[(k + 1) % 6];
    M5Cardputer.Display.setBrightness(cfg.bright);
    saveCfg();
  } else if (selIdx == 7) {
    int k = 0;
    for (int i = 0; i < 6; i++) if (DIM_STEPS[i] == cfg.dimSec) k = i;
    cfg.dimSec = DIM_STEPS[(k + 1) % 6];
    saveCfg();
  } else if (selIdx == 8) {
    saveCfg(); delay(200); ESP.restart();
  } else if (selIdx == 9) {
    if (!resetArmed) { resetArmed = true; return; }
    prefs.begin("miner", false); prefs.clear(); prefs.end();
    delay(200); ESP.restart();
  }
}

void handleKeys(Keyboard_Class::KeysState& st) {
  lastKey = millis();
  if (editing) {
    if (st.enter) { commitEdit(); return; }
    if (st.del) { if (ebuf.length()) ebuf.remove(ebuf.length() - 1); return; }
    for (char c : st.word) {
      if (c == '`') { editing = false; return; }
      if (ebuf.length() < 90) ebuf += c;
    }
    return;
  }
  if (st.tab) page = (page + 1) % 3;
  if (st.enter && page == 2) activateSetting();
  for (char c : st.word) {
    switch (c) {
      case '/': page = (page + 1) % 3; break;
      case ',': page = (page + 2) % 3; break;
      case 'p': case 'P': g_paused = !g_paused; break;
      case '-': cfg.bright = max(10, (int)cfg.bright - 25); M5Cardputer.Display.setBrightness(cfg.bright); saveCfg(); break;
      case '=': cfg.bright = min(255, (int)cfg.bright + 25); M5Cardputer.Display.setBrightness(cfg.bright); saveCfg(); break;
      case ';': if (page == 2) { selIdx = (selIdx + NSET - 1) % NSET; resetArmed = false; } break;
      case '.': if (page == 2) { selIdx = (selIdx + 1) % NSET; resetArmed = false; } break;
    }
  }
}

void drawHeader(const char* title) {
  cv.fillRect(0, 0, 240, 16, g_blockFound ? C_GRN : C_OR);
  cv.setTextSize(1);
  cv.setTextColor(C_BLK);
  cv.setCursor(5, 4);
  cv.print(title);
  if (battPct >= 0) {
    char b[8]; snprintf(b, sizeof(b), "%d%%", battPct);
    cv.setCursor(168, 4); cv.print(b);
  }
  int rssi = WiFi.RSSI();
  int bars = !g_wifi ? 0 : (rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0);
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    cv.fillRect(206 + i * 5, 14 - h, 3, h, i < bars ? C_BLK : rgb(190, 110, 20));
  }
  if (!g_wifi) { cv.setCursor(226, 4); cv.print("x"); }
}

void kv(int x, int y, const char* k, const String& v, uint16_t col = C_WHITE) {
  cv.setTextColor(C_GREY, C_BG); cv.setCursor(x, y); cv.print(k);
  cv.setTextColor(col, C_BG); cv.setCursor(x + 48, y); cv.print(v);
}

const char* stateStr(uint16_t& col) {
  if (g_paused) { col = C_YEL; return "PAUSED"; }
  if (cfg.ssid.length() == 0) { col = C_RED; return "SET WIFI (page 3)"; }
  if (!g_wifi) { col = C_RED; return "WIFI CONNECTING"; }
  if (g_poolState == 3) { col = C_RED; return "AUTH FAILED"; }
  if (!g_mining) { col = C_OR; return "POOL CONNECTING"; }
  col = C_GRN; return "MINING";
}

void drawDash() {
  cv.fillSprite(C_BG);
  drawHeader(g_blockFound ? "*** BLOCK FOUND ***" : FW_NAME);
  String hr = fmtNum(g_rate) + "H/s";
  cv.setTextSize(3);
  cv.setTextColor(g_rate > 0 ? C_WHITE : C_GREY, C_BG);
  cv.setCursor(4, 21);
  cv.print(hr);
  cv.setTextSize(1);
  uint16_t col; const char* st = stateStr(col);
  cv.fillCircle(8, 52, 3, col);
  cv.setTextColor(col, C_BG); cv.setCursor(16, 49); cv.print(st);
  cv.setTextColor(C_GREY, C_BG);
  cv.setCursor(112, 49); cv.print(trunc(g_usingBackup ? String(BACKUP_POOL) : cfg.host, 21));

  kv(4, 62, "Shares", String((unsigned long)g_shA) + "/" + String((unsigned long)g_shR), g_shR ? C_YEL : C_GRN);
  kv(4, 74, "Best", fmtDiff(g_best), C_OR);
  kv(4, 86, "PoolDif", fmtDiff(g_poolDiff));
  kv(124, 62, "Hashes", fmtNum((double)g_total));
  kv(124, 74, "Uptime", fmtUptime(millis() / 1000));
  kv(124, 86, "Temp", String((int)temperatureRead()) + "C", temperatureRead() > 75 ? C_RED : C_WHITE);

  float mx = 1;
  for (int i = 0; i < 60; i++) if (hist[i] > mx) mx = hist[i];
  for (int i = 0; i < 60; i++) {
    int h = (int)(hist[i] / mx * 20);
    if (h > 0) cv.fillRect(i * 4, 120 - h, 3, h, C_OR);
    else cv.drawFastHLine(i * 4, 119, 3, C_PANEL);
  }
  cv.setTextColor(C_GREY, C_BG); cv.setCursor(2, 125);
  cv.print("TAB page  P pause  -/= light");
}

void drawNet() {
  cv.fillSprite(C_BG);
  drawHeader("NETWORK / POOL");
  int y = 21;
  auto row = [&](const char* k, const String& v, uint16_t c = C_WHITE) {
    cv.setTextColor(C_GREY, C_BG); cv.setCursor(4, y); cv.print(k);
    cv.setTextColor(c, C_BG); cv.setCursor(64, y); cv.print(v); y += 12;
  };
  row("SSID", trunc(cfg.ssid.length() ? cfg.ssid : "<yok>", 28));
  row("Link", g_wifi ? "BAGLI  " + String(WiFi.RSSI()) + "dBm" : "KOPUK", g_wifi ? C_GRN : C_RED);
  row("IP", g_wifi ? WiFi.localIP().toString() : "-");
  row("Pool", trunc((g_usingBackup ? String(BACKUP_POOL) : cfg.host) + ":" + String(g_usingBackup ? BACKUP_PORT : cfg.port), 28));
  const char* ps[] = {"KAPALI", "BAGLANIYOR", "ONLINE", "AUTH HATASI"};
  row("Durum", ps[g_poolState], g_poolState == 2 ? C_GRN : (g_poolState == 3 ? C_RED : C_OR));
  row("Jobs", String((unsigned long)g_jobs) + "   NetDiff " + fmtNum(g_netDiff));
  row("Adres", trunc(cfg.addr, 28), C_OR);
  row("Sistem", String(ESP.getFreeHeap() / 1024) + "KB bos  " + String(getCpuFrequencyMhz()) + "MHz");
  cv.setTextColor(C_GREY, C_BG); cv.setCursor(2, 125);
  cv.print("TAB page  P pause");
}

void drawSettings() {
  cv.fillSprite(C_BG);
  drawHeader("SETTINGS");
  int first = selIdx < 7 ? 0 : selIdx - 6;
  for (int r = 0; r < 7; r++) {
    int i = first + r;
    if (i >= NSET) break;
    int y = 20 + r * 12;
    bool sel = (i == selIdx);
    if (sel) cv.fillRect(0, y - 2, 240, 12, C_PANEL);
    cv.setTextColor(sel ? C_OR : C_GREY, sel ? C_PANEL : C_BG);
    cv.setCursor(3, y); cv.print(sel ? ">" : " ");
    cv.setCursor(11, y); cv.print(SET_LABEL[i]);
    cv.setTextColor(C_WHITE, sel ? C_PANEL : C_BG);
    cv.setCursor(88, y); cv.print(trunc(settingValue(i), 25));
  }
  cv.setTextColor(C_GREY, C_BG); cv.setCursor(2, 125);
  cv.print(";/. gez  Enter sec  TAB page");
  if (editing) {
    cv.fillRect(0, 40, 240, 70, C_PANEL);
    cv.drawRect(0, 40, 240, 70, C_OR);
    cv.setTextColor(C_OR, C_PANEL); cv.setCursor(6, 45);
    cv.print("Duzenle: "); cv.print(SET_LABEL[selIdx]);
    cv.setTextColor(C_WHITE, C_PANEL);
    String show = ebuf + ((millis() / 400) % 2 ? "_" : " ");
    int len = show.length();
    for (int line = 0; line < 3; line++) {
      int a = line * 38;
      if (a >= len) break;
      cv.setCursor(6, 60 + line * 12);
      cv.print(show.substring(a, min(len, a + 38)));
    }
    cv.setTextColor(C_GREY, C_PANEL); cv.setCursor(6, 98);
    cv.print("Enter=kaydet Del=sil `=iptal");
  }
}

void uiTask(void*) {
  cv.setColorDepth(16);
  cv.createSprite(240, 135);
  uint32_t lastSec = 0, lastH = g_h32;
  int curBr = -1;
  memset(hist, 0, sizeof(hist));
  lastKey = millis();
  if (cfg.ssid.length() == 0) page = 2;
  for (;;) {
    M5Cardputer.update();
    if (M5Cardputer.BtnA.wasPressed()) { g_paused = !g_paused; lastKey = millis(); }
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      auto st = M5Cardputer.Keyboard.keysState();
      handleKeys(st);
    }
    if (millis() - lastSec >= 1000) {
      lastSec = millis();
      uint32_t h = g_h32;
      uint32_t d = h - lastH; lastH = h;
      g_total += d;
      double inst = (g_mining && !g_paused) ? d : 0;
      g_rate = (g_rate == 0) ? inst : g_rate * 0.8 + inst * 0.2;
      if (inst == 0) g_rate = 0;
      memmove(hist, hist + 1, sizeof(float) * 59);
      hist[59] = (float)g_rate;
      battPct = M5Cardputer.Power.getBatteryLevel();
      if (battPct > 100) battPct = 100;
    }
    // ekran karartma
    int want = cfg.bright;
    if (cfg.dimSec && !editing && millis() - lastKey > (uint32_t)cfg.dimSec * 1000) want = 6;
    if (want != curBr) { M5Cardputer.Display.setBrightness(want); curBr = want; }

    if (page == 0) drawDash();
    else if (page == 1) drawNet();
    else drawSettings();
    cv.pushSprite(0, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

// ---------------------------------- SETUP -----------------------------------
void setup() {
  auto mcfg = M5.config();
  M5Cardputer.begin(mcfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(C_BG);
  Serial.begin(115200);
  loadCfg();
  M5Cardputer.Display.setBrightness(cfg.bright);
  M5Cardputer.Display.setTextColor(C_OR, C_BG);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(18, 40);
  M5Cardputer.Display.print("BTC SOLO MINER");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(C_GREY, C_BG);
  M5Cardputer.Display.setCursor(18, 70);
  M5Cardputer.Display.print("Cardputer ADV  |  ESP32-S3");
  delay(900);

  cfgMtx = xSemaphoreCreateMutex();
  shareQ = xQueueCreate(8, sizeof(Share));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("cardputer-miner");
  WiFi.setSleep(false);

  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(uiTask, "ui", 10240, NULL, 1, NULL, 0);
}
